//===--- CrossTranslationUnit.cpp - -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the CrossTranslationUnit interface.
//
//===----------------------------------------------------------------------===//
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/Decl.h"
#include "clang/AST/ParentMapContext.h"
#include "clang/Basic/DiagnosticDriver.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CrossTU/CrossTUDiagnostic.h"
#include "clang/Driver/CreateASTUnitFromArgs.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/StaticAnalyzer/Core/AnalyzerOptions.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/IOSandbox.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <tuple>
#ifdef CLANG_ENABLE_CTU_SQLITE
#include <sqlite3.h>
#endif

namespace clang {
namespace cross_tu {


// One connection per analysis process, pinned to an immutable snapshot file.
// Prepared point queries use bounded SQLite pages, without a global symbol map.
struct SQLiteCTUIndex {
#ifdef CLANG_ENABLE_CTU_SQLITE
  sqlite3 *DB = nullptr;
  sqlite3_stmt *Query = nullptr;
  std::string Path;
  ~SQLiteCTUIndex() {
    sqlite3_finalize(Query);
    if (DB) sqlite3_close(DB);
  }
  llvm::Error open(StringRef File) {
    Path = File.str();
    if (sqlite3_open_v2(Path.c_str(), &DB, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        nullptr) != SQLITE_OK)
      return llvm::make_error<IndexError>(index_error_code::invalid_index_format, Path);
    // No mmap or per-symbol cache. SQLite's page cache is limited to 8 MiB.
    if (sqlite3_exec(DB, "PRAGMA query_only=ON; PRAGMA mmap_size=0; "
                        "PRAGMA cache_size=-8192; PRAGMA trusted_schema=OFF;",
                     nullptr, nullptr, nullptr) != SQLITE_OK)
      return llvm::make_error<IndexError>(index_error_code::invalid_index_format, Path);
    sqlite3_stmt *Meta = nullptr;
    if (sqlite3_prepare_v2(DB, "SELECT version, namespace FROM metadata", -1,
                           &Meta, nullptr) != SQLITE_OK)
      return llvm::make_error<IndexError>(index_error_code::invalid_index_format, Path);
    // Index schema v2 adds the candidate source path used for
    // caller-proximity duplicate resolution.
    bool Valid = sqlite3_step(Meta) == SQLITE_ROW && sqlite3_column_int(Meta, 0) == 2 &&
                 sqlite3_column_type(Meta, 1) == SQLITE_TEXT;
    std::string Namespace;
    if (Valid) Namespace = reinterpret_cast<const char *>(sqlite3_column_text(Meta, 1));
    Valid = Valid && sqlite3_step(Meta) == SQLITE_DONE;
    sqlite3_finalize(Meta);
    if (!Valid || sqlite3_prepare_v2(DB,
        "SELECT ast, source FROM definitions WHERE namespace=?1 AND usr=?2 ORDER BY source",
        -1, &Query, nullptr) != SQLITE_OK)
      return llvm::make_error<IndexError>(index_error_code::invalid_index_format, Path);
    sqlite3_bind_text(Query, 1, Namespace.c_str(), -1, SQLITE_TRANSIENT);
    return llvm::Error::success();
  }
  // Duplicate definitions resolve by caller proximity: upward directory
  // hops from the calling TU's directory to the lowest common ancestor with
  // each candidate's directory. The SQL ORDER BY source makes equal-distance
  // ties resolve deterministically to the smallest source path. Uncomparable
  // paths (empty / mapped) share the worst distance and also fall back to
  // the lexical order.
  static unsigned dirDistance(StringRef CallerDir, StringRef CandidateDir) {
    if (CallerDir.empty() || CandidateDir.empty())
      return std::numeric_limits<unsigned>::max() / 2;
    SmallVector<StringRef, 32> A, B;
    for (auto I = llvm::sys::path::begin(CallerDir),
              E = llvm::sys::path::end(CallerDir);
         I != E; ++I)
      A.push_back(*I);
    for (auto I = llvm::sys::path::begin(CandidateDir),
              E = llvm::sys::path::end(CandidateDir);
         I != E; ++I)
      B.push_back(*I);
    size_t LCA = 0, Max = std::min(A.size(), B.size());
    while (LCA < Max && A[LCA] == B[LCA]) ++LCA;
    return unsigned(A.size() - LCA);
  }
  // codeXek DU01: a definition that rode along with another import never
  // Shared per-caller selection over a USR's candidate rows: best (smallest)
  // directory distance from the caller's directory; ties and uncomparable
  // distances resolve lexically because the query orders by the source path.
  // Returns the candidate count (0 on index error) and, when count >= 1, the
  // chosen source path, the winning distance and the chosen AST file.
  size_t selectChosen(StringRef USR, StringRef CallerSource,
                      std::string &Chosen, unsigned &Best,
                      std::string &ChosenAST) {
    sqlite3_reset(Query);
    sqlite3_bind_text(Query, 2, USR.data(), USR.size(), SQLITE_TRANSIENT);
    StringRef CallerDir = llvm::sys::path::parent_path(CallerSource);
    // sqlite3_column_text buffers live only until the next step: copy.
    Chosen.clear();
    ChosenAST.clear();
    Best = 0;
    size_t Rows = 0;
    while (true) {
      int RC = sqlite3_step(Query);
      if (RC == SQLITE_DONE) break;
      if (RC != SQLITE_ROW || sqlite3_column_type(Query, 1) != SQLITE_TEXT)
        return 0; // index error
      ++Rows;
      std::string Source = reinterpret_cast<const char *>(sqlite3_column_text(Query, 1));
      std::string AST = sqlite3_column_type(Query, 0) == SQLITE_TEXT
                            ? reinterpret_cast<const char *>(sqlite3_column_text(Query, 0))
                            : std::string();
      unsigned D = dirDistance(CallerDir, llvm::sys::path::parent_path(Source));
      if (Rows == 1 || D < Best) { Best = D; Chosen = std::move(Source); ChosenAST = std::move(AST); }
    }
    return Rows;
  }
  // codeXek RF01-R: import-time candidate gate — the quiet variant used by
  // the ASTImporter body veto. A foreign body may enter the analyzed TU only
  // if it belongs to the candidate this caller's resolution chooses;
  // single-candidate and non-indexed USRs and index errors never gate. A
  // veto is visible as a distinct "suppressed" event (the product records
  // it per task; unlike "degraded" it does not weaken absence evidence,
  // because the chosen body remains loadable on demand).
  bool mayImportBody(StringRef USR, StringRef CallerSource,
                     StringRef DefinitionFile) {
    std::string Chosen, ChosenAST;
    unsigned Best = 0;
    size_t Rows = selectChosen(USR, CallerSource, Chosen, Best, ChosenAST);
    if (Rows < 2 || Chosen == DefinitionFile.str())
      return true;
    llvm::errs() << "codexek-ctu: suppressed usr=" << USR
                 << " caller=" << CallerSource
                 << " rejected=" << DefinitionFile
                 << " chosen=" << Chosen << " candidates=" << Rows << "\n";
    return false;
  }
  // passed the per-caller resolution. Before such a body inlines, the caller
  // verifies it is the candidate the policy would choose: run the same
  // distance/lexical selection and compare the chosen source with the
  // definition's own file. Non-indexed and single-candidate USRs are always
  // chosen; index errors do not gate (reported elsewhere).
  bool isChosenCandidate(StringRef USR, StringRef CallerSource,
                          StringRef DefinitionFile) {
    std::string Chosen, ChosenAST;
    unsigned Best = 0;
    size_t Rows = selectChosen(USR, CallerSource, Chosen, Best, ChosenAST);
    if (Rows == 0)
      return true; // index error: do not gate
    if (Rows < 2) return true;
    bool Verdict = Chosen == DefinitionFile.str();
    // codeXek RF01/RF03: a rejected passenger body degrades conservatively
    // (the To-context cannot hold two bodies of one function, so the chosen
    // candidate cannot be substituted in) — the degradation is recorded
    // explicitly instead of being presented as a successful resolution.
    // With the RF01-R import gate in place this should be unreachable for
    // passengers; it stays as a use-site safety net.
    llvm::errs() << (Verdict ? "codexek-ctu: kept" : "codexek-ctu: degraded")
                 << " usr=" << USR << " caller=" << CallerSource
                 << " imported=" << DefinitionFile << " chosen=" << Chosen
                 << " distance=" << Best << " candidates=" << Rows << "\n";
    return Verdict;
  }
  // codeXek dependency events: every cross-TU query outcome is reported so
  // the product can do dependency-driven incremental analysis (design
  // part IV §2) — "imported" carries the AST file whose definitions this
  // task actually consumed; "missing" is deduplicated per process because
  // unresolved USRs are re-queried at every call site, and a later
  // appearance of one must re-analyze tasks that conservatively evaluated
  // the call.
  llvm::StringSet<> ReportedMisses;
  llvm::Expected<std::string> lookup(StringRef USR, StringRef CallerSource) {
    std::string Chosen, AST;
    unsigned Best = 0;
    size_t Rows = selectChosen(USR, CallerSource, Chosen, Best, AST);
    if (Rows == 0) {
      if (ReportedMisses.insert(USR).second)
        llvm::errs() << "codexek-ctu: missing usr=" << USR
                     << " caller=" << CallerSource << "\n";
      return llvm::make_error<IndexError>(index_error_code::missing_definition);
    }
    if (Rows >= 2)
      // codeXek RF03: structured per-query resolution event; the product
      // keeps these per task so every duplicate choice is traceable.
      llvm::errs() << "codexek-ctu: resolved usr=" << USR
                   << " caller=" << CallerSource << " chosen=" << Chosen
                   << " distance=" << Best << " candidates=" << Rows << "\n";
    llvm::errs() << "codexek-ctu: imported usr=" << USR
                 << " caller=" << CallerSource << " ast=" << AST << "\n";
    return AST;
  }
#endif
};

namespace {

#define DEBUG_TYPE "CrossTranslationUnit"
STATISTIC(NumGetCTUCalled, "The # of getCTUDefinition function called");
STATISTIC(
    NumNotInOtherTU,
    "The # of getCTUDefinition called but the function is not in any other TU");
STATISTIC(NumGetCTUSuccess,
          "The # of getCTUDefinition successfully returned the "
          "requested function's body");
STATISTIC(NumUnsupportedNodeFound, "The # of imports when the ASTImporter "
                                   "encountered an unsupported AST Node");
STATISTIC(NumNameConflicts, "The # of imports when the ASTImporter "
                            "encountered an ODR error");
STATISTIC(NumTripleMismatch, "The # of triple mismatches");
STATISTIC(NumLangMismatch, "The # of language mismatches");
STATISTIC(NumLangDialectMismatch, "The # of language dialect mismatches");
STATISTIC(NumASTLoadThresholdReached,
          "The # of ASTs not loaded because of threshold");

// Same as Triple's equality operator, but we check a field only if that is
// known in both instances.
bool hasEqualKnownFields(const llvm::Triple &Lhs, const llvm::Triple &Rhs) {
  using llvm::Triple;
  if (Lhs.getArch() != Triple::UnknownArch &&
      Rhs.getArch() != Triple::UnknownArch && Lhs.getArch() != Rhs.getArch())
    return false;
  if (Lhs.getSubArch() != Triple::NoSubArch &&
      Rhs.getSubArch() != Triple::NoSubArch &&
      Lhs.getSubArch() != Rhs.getSubArch())
    return false;
  if (Lhs.getVendor() != Triple::UnknownVendor &&
      Rhs.getVendor() != Triple::UnknownVendor &&
      Lhs.getVendor() != Rhs.getVendor())
    return false;
  if (!Lhs.isOSUnknown() && !Rhs.isOSUnknown() &&
      Lhs.getOS() != Rhs.getOS())
    return false;
  if (Lhs.getEnvironment() != Triple::UnknownEnvironment &&
      Rhs.getEnvironment() != Triple::UnknownEnvironment &&
      Lhs.getEnvironment() != Rhs.getEnvironment())
    return false;
  if (Lhs.getObjectFormat() != Triple::UnknownObjectFormat &&
      Rhs.getObjectFormat() != Triple::UnknownObjectFormat &&
      Lhs.getObjectFormat() != Rhs.getObjectFormat())
    return false;
  return true;
}

// FIXME: This class is will be removed after the transition to llvm::Error.
class IndexErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "clang.index"; }

  std::string message(int Condition) const override {
    switch (static_cast<index_error_code>(Condition)) {
    case index_error_code::success:
      // There should not be a success error. Jump to unreachable directly.
      // Add this case to make the compiler stop complaining.
      break;
    case index_error_code::unspecified:
      return "An unknown error has occurred.";
    case index_error_code::missing_index_file:
      return "The index file is missing.";
    case index_error_code::invalid_index_format:
      return "Invalid index file format.";
    case index_error_code::multiple_definitions:
      return "Multiple definitions in the index file.";
    case index_error_code::missing_definition:
      return "Missing definition from the index file.";
    case index_error_code::failed_import:
      return "Failed to import the definition.";
    case index_error_code::failed_to_get_external_ast:
      return "Failed to load external AST source.";
    case index_error_code::failed_to_generate_usr:
      return "Failed to generate USR.";
    case index_error_code::triple_mismatch:
      return "Triple mismatch";
    case index_error_code::lang_mismatch:
      return "Language mismatch";
    case index_error_code::lang_dialect_mismatch:
      return "Language dialect mismatch";
    case index_error_code::load_threshold_reached:
      return "Load threshold reached";
    case index_error_code::invocation_list_ambiguous:
      return "Invocation list file contains multiple references to the same "
             "source file.";
    case index_error_code::invocation_list_file_not_found:
      return "Invocation list file is not found.";
    case index_error_code::invocation_list_empty:
      return "Invocation list file is empty.";
    case index_error_code::invocation_list_wrong_format:
      return "Invocation list file is in wrong format.";
    case index_error_code::invocation_list_lookup_unsuccessful:
      return "Invocation list file does not contain the requested source file.";
    }
    llvm_unreachable("Unrecognized index_error_code.");
  }
};

static llvm::ManagedStatic<IndexErrorCategory> Category;
} // end anonymous namespace

/// Returns a human-readable language/dialect description for diagnostics.
/// Checks flags from highest to lowest standard since they are cumulative
/// (e.g. CPlusPlus20 implies CPlusPlus17).
/// This does not cover all possible languages (e.g. Obj-C or flavors of C),
/// because CTU currently does not differentiate between them.
static std::string getLangDescription(const LangOptions &LO) {
  if (!LO.CPlusPlus)
    return "non-C++";
  if (LO.CPlusPlus29)
    return "C++29";
  if (LO.CPlusPlus26)
    return "C++26";
  if (LO.CPlusPlus23)
    return "C++23";
  if (LO.CPlusPlus20)
    return "C++20";
  if (LO.CPlusPlus17)
    return "C++17";
  if (LO.CPlusPlus14)
    return "C++14";
  if (LO.CPlusPlus11)
    return "C++11";
  return "C++98";
}

char IndexError::ID;

void IndexError::log(raw_ostream &OS) const {
  OS << Category->message(static_cast<int>(Code)) << '\n';
}

std::error_code IndexError::convertToErrorCode() const {
  return std::error_code(static_cast<int>(Code), *Category);
}

/// Parse one line of the input CTU index file.
///
/// @param[in]  LineRef     The input CTU index item in format
///                         "<USR-Length>:<USR> <File-Path>".
/// @param[out] LookupName  The lookup name in format "<USR-Length>:<USR>".
/// @param[out] FilePath    The file path "<File-Path>".
static bool parseCrossTUIndexItem(StringRef LineRef, StringRef &LookupName,
                                  StringRef &FilePath) {
  // `LineRef` is "<USR-Length>:<USR> <File-Path>" now.

  size_t USRLength = 0;
  if (LineRef.consumeInteger(10, USRLength))
    return false;
  assert(USRLength && "USRLength should be greater than zero.");

  if (!LineRef.consume_front(":"))
    return false;

  // `LineRef` is now just "<USR> <File-Path>".

  // Check LookupName length out of bound and incorrect delimiter.
  if (USRLength >= LineRef.size() || ' ' != LineRef[USRLength])
    return false;

  LookupName = LineRef.substr(0, USRLength);
  FilePath = LineRef.substr(USRLength + 1);
  return true;
}

llvm::Expected<llvm::StringMap<std::string>>
parseCrossTUIndex(StringRef IndexPath) {
  std::ifstream ExternalMapFile{std::string(IndexPath)};
  if (!ExternalMapFile)
    return llvm::make_error<IndexError>(index_error_code::missing_index_file,
                                        IndexPath.str());

  llvm::StringMap<std::string> Result;
  std::string Line;
  unsigned LineNo = 1;
  while (std::getline(ExternalMapFile, Line)) {
    // Split lookup name and file path
    StringRef LookupName, FilePathInIndex;
    if (!parseCrossTUIndexItem(Line, LookupName, FilePathInIndex))
      return llvm::make_error<IndexError>(
          index_error_code::invalid_index_format, IndexPath.str(), LineNo);

    // Store paths with posix-style directory separator.
    SmallString<32> FilePath(FilePathInIndex);
    llvm::sys::path::native(FilePath, llvm::sys::path::Style::posix);

    bool InsertionOccurred;
    std::tie(std::ignore, InsertionOccurred) =
        Result.try_emplace(LookupName, FilePath.begin(), FilePath.end());
    if (!InsertionOccurred)
      return llvm::make_error<IndexError>(
          index_error_code::multiple_definitions, IndexPath.str(), LineNo);

    ++LineNo;
  }
  return Result;
}

std::string
createCrossTUIndexString(const llvm::StringMap<std::string> &Index) {
  std::ostringstream Result;
  for (const auto &E : Index)
    Result << E.getKey().size() << ':' << E.getKey().str() << ' '
           << E.getValue() << '\n';
  return Result.str();
}

bool shouldImport(const VarDecl *VD, const ASTContext &ACtx) {
  CanQualType CT = ACtx.getCanonicalType(VD->getType());
  return CT.isConstQualified() && VD->getType().isTrivialType(ACtx);
}

static bool hasBodyOrInit(const FunctionDecl *D, const FunctionDecl *&DefD) {
  return D->hasBody(DefD);
}
static bool hasBodyOrInit(const VarDecl *D, const VarDecl *&DefD) {
  return D->getAnyInitializer(DefD);
}
template <typename T> [[maybe_unused]] static bool hasBodyOrInit(const T *D) {
  const T *Unused;
  return hasBodyOrInit(D, Unused);
}

CrossTranslationUnitContext::CrossTranslationUnitContext(CompilerInstance &CI)
    : Context(CI.getASTContext()), ASTStorage(CI) {
  if (CI.getAnalyzerOpts().ShouldEmitErrorsOnInvalidConfigValue &&
      !CI.getAnalyzerOpts().CTUDir.empty()) {
    auto S = CI.getVirtualFileSystem().status(CI.getAnalyzerOpts().CTUDir);
    if (!S || S->getType() != llvm::sys::fs::file_type::directory_file)
      CI.getDiagnostics().Report(diag::err_analyzer_config_invalid_input)
          << "ctu-dir"
          << "a filename";
  }
}

CrossTranslationUnitContext::~CrossTranslationUnitContext() {}

std::optional<std::string>
CrossTranslationUnitContext::getLookupName(const Decl *D) {
  SmallString<128> DeclUSR;
  bool Ret = index::generateUSRForDecl(D, DeclUSR);
  if (Ret)
    return {};
  return std::string(DeclUSR);
}

/// Recursively visits the decls of a DeclContext, and returns one with the
/// given USR.
template <typename T>
const T *
CrossTranslationUnitContext::findDefInDeclContext(const DeclContext *DC,
                                                  StringRef LookupName) {
  assert(DC && "Declaration Context must not be null");
  for (const Decl *D : DC->decls()) {
    const auto *SubDC = dyn_cast<DeclContext>(D);
    if (SubDC)
      if (const auto *ND = findDefInDeclContext<T>(SubDC, LookupName))
        return ND;

    const auto *ND = dyn_cast<T>(D);
    const T *ResultDecl;
    if (!ND || !hasBodyOrInit(ND, ResultDecl))
      continue;
    std::optional<std::string> ResultLookupName = getLookupName(ResultDecl);
    if (!ResultLookupName || *ResultLookupName != LookupName)
      continue;
    return ResultDecl;
  }
  return nullptr;
}

template <typename T>
llvm::Expected<const T *> CrossTranslationUnitContext::getCrossTUDefinitionImpl(
    const T *D, StringRef CrossTUDir, StringRef IndexName,
    bool DisplayCTUProgress) {
  assert(D && "D is missing, bad call to this function!");
  assert(!hasBodyOrInit(D) &&
         "D has a body or init in current translation unit!");
  ++NumGetCTUCalled;
  const std::optional<std::string> LookupName = getLookupName(D);
  if (!LookupName)
    return llvm::make_error<IndexError>(
        index_error_code::failed_to_generate_usr);
  llvm::SmallString<256> CallerSource;
  if (auto MainFile = Context.getSourceManager().getFileEntryRefForID(
          Context.getSourceManager().getMainFileID()))
    CallerSource = MainFile->getName();
  llvm::Expected<ASTUnit *> ASTUnitOrError =
      loadExternalAST(*LookupName, CrossTUDir, IndexName, DisplayCTUProgress,
                      CallerSource);
  if (!ASTUnitOrError)
    return ASTUnitOrError.takeError();
  ASTUnit *Unit = *ASTUnitOrError;
  assert(&Unit->getFileManager() ==
         &Unit->getASTContext().getSourceManager().getFileManager());

  const llvm::Triple &TripleTo = Context.getTargetInfo().getTriple();
  const llvm::Triple &TripleFrom =
      Unit->getASTContext().getTargetInfo().getTriple();
  // The imported AST had been generated for a different target.
  // Some parts of the triple in the loaded ASTContext can be unknown while the
  // very same parts in the target ASTContext are known. Thus we check for the
  // known parts only.
  if (!hasEqualKnownFields(TripleTo, TripleFrom)) {
    // TODO: Pass the SourceLocation of the CallExpression for more precise
    // diagnostics.
    ++NumTripleMismatch;
    return llvm::make_error<IndexError>(index_error_code::triple_mismatch,
                                        std::string(Unit->getMainFileName()),
                                        TripleTo.str(), TripleFrom.str());
  }

  const auto &LangTo = Context.getLangOpts();
  const auto &LangFrom = Unit->getASTContext().getLangOpts();

  // FIXME: Currenty we do not support CTU across C++ and C and across
  // different dialects of C++.
  if (LangTo.CPlusPlus != LangFrom.CPlusPlus) {
    ++NumLangMismatch;
    return llvm::make_error<IndexError>(
        index_error_code::lang_mismatch, std::string(Unit->getMainFileName()),
        getLangDescription(LangTo), getLangDescription(LangFrom));
  }

  // If CPP dialects are different then return with error.
  //
  // Consider this STL code:
  //   template<typename _Alloc>
  //     struct __alloc_traits
  //   #if __cplusplus >= 201103L
  //     : std::allocator_traits<_Alloc>
  //   #endif
  //     { // ...
  //     };
  // This class template would create ODR errors during merging the two units,
  // since in one translation unit the class template has a base class, however
  // in the other unit it has none.
  if (LangTo.CPlusPlus11 != LangFrom.CPlusPlus11 ||
      LangTo.CPlusPlus14 != LangFrom.CPlusPlus14 ||
      LangTo.CPlusPlus17 != LangFrom.CPlusPlus17 ||
      LangTo.CPlusPlus20 != LangFrom.CPlusPlus20) {
    ++NumLangDialectMismatch;
    return llvm::make_error<IndexError>(index_error_code::lang_dialect_mismatch,
                                        std::string(Unit->getMainFileName()),
                                        getLangDescription(LangTo),
                                        getLangDescription(LangFrom));
  }

  TranslationUnitDecl *TU = Unit->getASTContext().getTranslationUnitDecl();
  if (const T *ResultDecl = findDefInDeclContext<T>(TU, *LookupName))
    return importDefinition(ResultDecl, Unit);
  return llvm::make_error<IndexError>(index_error_code::failed_import);
}

llvm::Expected<const FunctionDecl *>
CrossTranslationUnitContext::getCrossTUDefinition(const FunctionDecl *FD,
                                                  StringRef CrossTUDir,
                                                  StringRef IndexName,
                                                  bool DisplayCTUProgress) {
  return getCrossTUDefinitionImpl(FD, CrossTUDir, IndexName,
                                  DisplayCTUProgress);
}

llvm::Expected<const VarDecl *>
CrossTranslationUnitContext::getCrossTUDefinition(const VarDecl *VD,
                                                  StringRef CrossTUDir,
                                                  StringRef IndexName,
                                                  bool DisplayCTUProgress) {
  return getCrossTUDefinitionImpl(VD, CrossTUDir, IndexName,
                                  DisplayCTUProgress);
}

void CrossTranslationUnitContext::emitCrossTUDiagnostics(const IndexError &IE,
                                                         SourceLocation Loc) {
  switch (IE.getCode()) {
  case index_error_code::missing_index_file:
  case index_error_code::invocation_list_file_not_found:
    // If the external def-map refers to source files, you must provide an
    // invocation list file. Otherwise, CTU does not work at all, so you should
    // check your build and analysis configuration.
    Context.getDiagnostics().Report(Loc, diag::err_ctu_error_opening)
        << IE.getFileName();
    return;

  case index_error_code::invalid_index_format:
    Context.getDiagnostics().Report(Loc, diag::err_extdefmap_parsing)
        << IE.getFileName() << IE.getLineNum();
    return;

  case index_error_code::multiple_definitions:
    Context.getDiagnostics().Report(Loc, diag::warn_multiple_def_index)
        << IE.getLineNum();
    return;

  case index_error_code::triple_mismatch:
    Context.getDiagnostics().Report(Loc, diag::warn_ctu_incompat_triple)
        << IE.getFileName() << IE.getConfigToName() << IE.getConfigFromName();
    return;

  case index_error_code::missing_definition:
    // Ignore missing definitions because it is very common to have some symbols
    // defined outside of the analysis scope: they may be defined in 3-rd party
    // and standard libraries, generated code, and files excluded from the
    // analysis.
    // Even ignoring it with Ignored diagnostic might generate too much traffic.
    return;

  case index_error_code::failed_import:
  case index_error_code::unspecified:
    // Not clear what happened exactly, but the outcome is a missing definition
    // This is not a big deal, and is expected since ASTImporter is incomplete.
    Context.getDiagnostics().Report(Loc, diag::warn_ctu_import_failure)
        << Category->message(static_cast<int>(IE.getCode()));
    return;

  case index_error_code::failed_to_generate_usr:
    // This is unlikely, so it is worth looking into, hence an error.
  case index_error_code::failed_to_get_external_ast:
    // This is suspicious, since the external AST is mentioned in the external
    // defmap, so it should exist.
    Context.getDiagnostics().Report(Loc, diag::err_ctu_import_failure)
        << Category->message(static_cast<int>(IE.getCode()));
    return;

  case index_error_code::load_threshold_reached:
    // This is expected. It is still useful to be aware of, but it is normal
    // operation. Emit the remark only once to avoid noise.
    if (!HasEmittedLoadThresholdRemark) {
      HasEmittedLoadThresholdRemark = true;
      Context.getDiagnostics().Report(
          Loc, diag::remark_ctu_import_threshold_reached);
    }
    return;

  case index_error_code::lang_mismatch:
  case index_error_code::lang_dialect_mismatch:
    // Similar to target triple mismatch.
    Context.getDiagnostics().Report(Loc, diag::warn_ctu_incompat_lang)
        << IE.getFileName() << IE.getConfigToName() << IE.getConfigFromName();
    return;

  case index_error_code::invocation_list_wrong_format:
  case index_error_code::invocation_list_empty:
    // Without parsable invocation list, CTU cannot function.
    Context.getDiagnostics().Report(Loc, diag::err_invlist_parsing)
        << IE.getFileName() << IE.getLineNum();
    return;

  case index_error_code::invocation_list_ambiguous:
    // For automatically generated invocation lists, it is common to list
    // multiple invocations, if a file is compiled in multiple contexts. No need
    // to block CTU because of this.
    Context.getDiagnostics().Report(Loc, diag::warn_multiple_entries_invlist)
        << IE.getFileName();
    return;

  case index_error_code::invocation_list_lookup_unsuccessful:
    // Some files might be missing in the invocation list. It is sad but not
    // fatal, and CTU can take advantage of the definitions in files with known
    // invocations.
    Context.getDiagnostics().Report(Loc, diag::warn_invlist_missing_file)
        << IE.getFileName();
    return;

  case index_error_code::success:
    llvm_unreachable("Success is not an error.");
    return;
  }
  llvm_unreachable("Unrecognized index_error_code.");
}

CrossTranslationUnitContext::ASTUnitStorage::ASTUnitStorage(
    CompilerInstance &CI)
    : Loader(CI, CI.getAnalyzerOpts().CTUDir,
             CI.getAnalyzerOpts().CTUInvocationList),
      LoadGuard(CI.getASTContext().getLangOpts().CPlusPlus
                    ? CI.getAnalyzerOpts().CTUImportCppThreshold
                    : CI.getAnalyzerOpts().CTUImportThreshold) {}

llvm::Expected<ASTUnit *>
CrossTranslationUnitContext::ASTUnitStorage::getASTUnitForFile(
    StringRef FileName, bool DisplayCTUProgress) {
  // Try the cache first.
  auto ASTCacheEntry = FileASTUnitMap.find(FileName);
  if (ASTCacheEntry == FileASTUnitMap.end()) {

    // Do not load if the limit is reached.
    if (!LoadGuard) {
      ++NumASTLoadThresholdReached;
      return llvm::make_error<IndexError>(
          index_error_code::load_threshold_reached);
    }

    auto LoadAttempt = Loader.load(FileName);

    if (!LoadAttempt)
      return LoadAttempt.takeError();

    std::unique_ptr<ASTUnit> LoadedUnit = std::move(LoadAttempt.get());

    // Need the raw pointer and the unique_ptr as well.
    ASTUnit *Unit = LoadedUnit.get();

    // Update the cache.
    FileASTUnitMap[FileName] = std::move(LoadedUnit);

    LoadGuard.indicateLoadSuccess();

    if (DisplayCTUProgress)
      llvm::errs() << "CTU loaded AST file: " << FileName << "\n";

    return Unit;

  } else {
    // Found in the cache.
    return ASTCacheEntry->second.get();
  }
}

llvm::Expected<ASTUnit *>
CrossTranslationUnitContext::ASTUnitStorage::getASTUnitForFunction(
    StringRef FunctionName, StringRef CrossTUDir, StringRef IndexName,
    bool DisplayCTUProgress, StringRef CallerSource) {
  // Try the cache first.
  auto ASTCacheEntry = NameASTUnitMap.find(FunctionName);
  if (ASTCacheEntry == NameASTUnitMap.end()) {
    // Load the ASTUnit from the pre-dumped AST file specified by ASTFileName.

    auto ASTFile = getFileForFunction(FunctionName, CrossTUDir, IndexName, CallerSource);
    if (!ASTFile) return ASTFile.takeError();

    // Search in the index for the filename where the definition of FunctionName
    // resides.
    if (llvm::Expected<ASTUnit *> FoundForFile =
            getASTUnitForFile(*ASTFile, DisplayCTUProgress)) {

      // Update the cache.
      NameASTUnitMap[FunctionName] = *FoundForFile;
      return *FoundForFile;

    } else {
      return FoundForFile.takeError();
    }
  } else {
    // Found in the cache.
    return ASTCacheEntry->second;
  }
}

llvm::Expected<std::string>
CrossTranslationUnitContext::ASTUnitStorage::getFileForFunction(
    StringRef FunctionName, StringRef CrossTUDir, StringRef IndexName,
    StringRef CallerSource) {
  if (llvm::Error IndexLoadError = ensureCTUIndexLoaded(CrossTUDir, IndexName))
    return std::move(IndexLoadError);
#ifdef CLANG_ENABLE_CTU_SQLITE
  if (DiskIndex) return DiskIndex->lookup(FunctionName, CallerSource);
#endif
  auto It = NameFileMap.find(FunctionName);
  if (It == NameFileMap.end()) {
    ++NumNotInOtherTU;
    return llvm::make_error<IndexError>(index_error_code::missing_definition);
  }
  return It->second;
}

bool CrossTranslationUnitContext::ASTUnitStorage::isChosenCandidate(
    StringRef FunctionName, StringRef CallerSource,
    StringRef DefinitionFile) const {
#ifdef CLANG_ENABLE_CTU_SQLITE
  if (DiskIndex)
    return DiskIndex->isChosenCandidate(FunctionName, CallerSource,
                                        DefinitionFile);
#endif
  (void)FunctionName; (void)CallerSource; (void)DefinitionFile;
  return true; // text index is single-row per USR: no duplicates to resolve
}

bool CrossTranslationUnitContext::ASTUnitStorage::mayImportBody(
    StringRef FunctionName, StringRef CallerSource,
    StringRef DefinitionFile) const {
#ifdef CLANG_ENABLE_CTU_SQLITE
  if (DiskIndex)
    return DiskIndex->mayImportBody(FunctionName, CallerSource, DefinitionFile);
#endif
  (void)FunctionName; (void)CallerSource; (void)DefinitionFile;
  return true; // text index is single-row per USR: no duplicates to resolve
}

llvm::Error CrossTranslationUnitContext::ASTUnitStorage::ensureCTUIndexLoaded(
    StringRef CrossTUDir, StringRef IndexName) {
  // Dont initialize if the map is filled.
  if (IndexLoaded)
    return llvm::Error::success();

  // Get the absolute path to the index file.
  SmallString<256> IndexFile = CrossTUDir;
  if (llvm::sys::path::is_absolute(IndexName))
    IndexFile = IndexName;
  else
    llvm::sys::path::append(IndexFile, IndexName);

#ifdef CLANG_ENABLE_CTU_SQLITE
  if (StringRef(IndexFile).ends_with(".db")) {
    auto Index = std::make_shared<SQLiteCTUIndex>();
    if (auto Error = Index->open(IndexFile)) return Error;
    DiskIndex = std::move(Index);
    IndexLoaded = true;
    return llvm::Error::success();
  }
#endif

  if (auto IndexMapping = parseCrossTUIndex(IndexFile)) {
    // Initialize member map.
    NameFileMap = std::move(*IndexMapping);
    IndexLoaded = true;
    return llvm::Error::success();
  } else {
    // Error while parsing CrossTU index file.
    return IndexMapping.takeError();
  };
}

llvm::Expected<ASTUnit *> CrossTranslationUnitContext::loadExternalAST(
    StringRef LookupName, StringRef CrossTUDir, StringRef IndexName,
    bool DisplayCTUProgress, StringRef CallerSource) {
  // FIXME: The current implementation only supports loading decls with
  //        a lookup name from a single translation unit. If multiple
  //        translation units contains decls with the same lookup name an
  //        error will be returned.

  // Try to get the value from the heavily cached storage.
  llvm::Expected<ASTUnit *> Unit = ASTStorage.getASTUnitForFunction(
      LookupName, CrossTUDir, IndexName, DisplayCTUProgress, CallerSource);

  if (!Unit)
    return Unit.takeError();

  // Check whether the backing pointer of the Expected is a nullptr.
  if (!*Unit)
    return llvm::make_error<IndexError>(
        index_error_code::failed_to_get_external_ast);

  return Unit;
}

CrossTranslationUnitContext::ASTLoader::ASTLoader(
    CompilerInstance &CI, StringRef CTUDir, StringRef InvocationListFilePath)
    : CI(CI), CTUDir(CTUDir), InvocationListFilePath(InvocationListFilePath) {}

CrossTranslationUnitContext::LoadResultTy
CrossTranslationUnitContext::ASTLoader::load(StringRef Identifier) {
  llvm::SmallString<256> Path;
  if (llvm::sys::path::is_absolute(Identifier, PathStyle)) {
    Path = Identifier;
  } else {
    Path = CTUDir;
    llvm::sys::path::append(Path, PathStyle, Identifier);
  }

  // The path is stored in the InvocationList member in posix style. To
  // successfully lookup an entry based on filepath, it must be converted.
  llvm::sys::path::native(Path, PathStyle);

  // Normalize by removing relative path components.
  llvm::sys::path::remove_dots(Path, /*remove_dot_dot*/ true, PathStyle);

  if (Path.ends_with(".ast"))
    return loadFromDump(Path);
  else
    return loadFromSource(Path);
}

CrossTranslationUnitContext::LoadResultTy
CrossTranslationUnitContext::ASTLoader::loadFromDump(StringRef ASTDumpPath) {
  auto DiagOpts = std::make_shared<DiagnosticOptions>();
  TextDiagnosticPrinter *DiagClient =
      new TextDiagnosticPrinter(llvm::errs(), *DiagOpts);
  auto Diags = llvm::makeIntrusiveRefCnt<DiagnosticsEngine>(
      DiagnosticIDs::create(), *DiagOpts, DiagClient);
  return ASTUnit::LoadFromASTFile(
      ASTDumpPath, CI.getPCHContainerOperations()->getRawReader(),
      ASTUnit::LoadEverything, CI.getVirtualFileSystemPtr(), DiagOpts, Diags,
      CI.getFileSystemOpts(), CI.getHeaderSearchOpts());
}

/// Load the AST from a source-file, which is supposed to be located inside the
/// YAML formatted invocation list file under the filesystem path specified by
/// \p InvocationList. The invocation list should contain absolute paths.
/// \p SourceFilePath is the absolute path of the source file that contains the
/// function definition the analysis is looking for. The Index is built by the
/// \p clang-extdef-mapping tool, which is also supposed to be generating
/// absolute paths.
///
/// Proper diagnostic emission requires absolute paths, so even if a future
/// change introduces the handling of relative paths, this must be taken into
/// consideration.
CrossTranslationUnitContext::LoadResultTy
CrossTranslationUnitContext::ASTLoader::loadFromSource(
    StringRef SourceFilePath) {

  if (llvm::Error InitError = lazyInitInvocationList())
    return std::move(InitError);
  assert(InvocationList);

  auto Invocation = InvocationList->find(SourceFilePath);
  if (Invocation == InvocationList->end())
    return llvm::make_error<IndexError>(
        index_error_code::invocation_list_lookup_unsuccessful,
        SourceFilePath.str());

  const InvocationListTy::mapped_type &InvocationCommand = Invocation->second;

  SmallVector<const char *, 32> CommandLineArgs(InvocationCommand.size());
  std::transform(InvocationCommand.begin(), InvocationCommand.end(),
                 CommandLineArgs.begin(),
                 [](auto &&CmdPart) { return CmdPart.c_str(); });

  auto DiagOpts = std::make_shared<DiagnosticOptions>(CI.getDiagnosticOpts());
  auto *DiagClient = new ForwardingDiagnosticConsumer{CI.getDiagnosticClient()};
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID{
      CI.getDiagnostics().getDiagnosticIDs()};
  auto Diags = llvm::makeIntrusiveRefCnt<DiagnosticsEngine>(DiagID, *DiagOpts,
                                                            DiagClient);

  // This runs the driver which isn't expected to be free of sandbox violations.
  auto BypassSandbox = llvm::sys::sandbox::scopedDisable();
  return CreateASTUnitFromCommandLine(
      CommandLineArgs.begin(), (CommandLineArgs.end()),
      CI.getPCHContainerOperations(), DiagOpts, Diags,
      CI.getHeaderSearchOpts().ResourceDir);
}

llvm::Expected<InvocationListTy>
parseInvocationList(StringRef FileContent, llvm::sys::path::Style PathStyle,
                    StringRef FilePath) {
  InvocationListTy InvocationList;

  /// LLVM YAML parser is used to extract information from invocation list file.
  llvm::SourceMgr SM;
  llvm::yaml::Stream InvocationFile(FileContent, SM);

  auto GetLine = [&SM](const llvm::yaml::Node *N) -> int {
    return N ? SM.FindLineNumber(N->getSourceRange().Start) : 0;
  };
  auto WrongFormatError = [&](const llvm::yaml::Node *N) {
    return llvm::make_error<IndexError>(
        index_error_code::invocation_list_wrong_format, FilePath.str(),
        GetLine(N));
  };

  /// Only the first document is processed.
  llvm::yaml::document_iterator FirstInvocationFile = InvocationFile.begin();

  /// There has to be at least one document available.
  if (FirstInvocationFile == InvocationFile.end())
    return llvm::make_error<IndexError>(
        index_error_code::invocation_list_empty);

  llvm::yaml::Node *DocumentRoot = FirstInvocationFile->getRoot();
  if (!DocumentRoot)
    return llvm::make_error<IndexError>(
        index_error_code::invocation_list_wrong_format);

  /// According to the format specified the document must be a mapping, where
  /// the keys are paths to source files, and values are sequences of invocation
  /// parts.
  auto *Mappings = dyn_cast<llvm::yaml::MappingNode>(DocumentRoot);
  if (!Mappings)
    return WrongFormatError(DocumentRoot);

  for (auto &NextMapping : *Mappings) {
    /// The keys should be strings, which represent a source-file path.
    auto *Key =
        dyn_cast_if_present<llvm::yaml::ScalarNode>(NextMapping.getKey());
    if (!Key)
      return WrongFormatError(NextMapping.getKey());

    SmallString<32> ValueStorage;
    StringRef SourcePath = Key->getValue(ValueStorage);

    // Store paths with PathStyle directory separator.
    SmallString<32> NativeSourcePath(SourcePath);
    llvm::sys::path::native(NativeSourcePath, PathStyle);

    StringRef InvocationKey = NativeSourcePath;

    if (InvocationList.contains(InvocationKey))
      return llvm::make_error<IndexError>(
          index_error_code::invocation_list_ambiguous, InvocationKey.str());

    /// The values should be sequences of strings, each representing a part of
    /// the invocation.
    auto *Args =
        dyn_cast_if_present<llvm::yaml::SequenceNode>(NextMapping.getValue());
    if (!Args)
      return WrongFormatError(NextMapping.getValue());

    for (auto &Arg : *Args) {
      auto *CmdString = dyn_cast<llvm::yaml::ScalarNode>(&Arg);
      if (!CmdString)
        return WrongFormatError(&Arg);
      /// Every conversion starts with an empty working storage, as it is not
      /// clear if this is a requirement of the YAML parser.
      ValueStorage.clear();
      InvocationList[InvocationKey].emplace_back(
          CmdString->getValue(ValueStorage));
    }

    if (InvocationList[InvocationKey].empty())
      return WrongFormatError(Key);
  }

  return InvocationList;
}

llvm::Error CrossTranslationUnitContext::ASTLoader::lazyInitInvocationList() {
  /// Lazily initialize the invocation list member used for on-demand parsing.
  if (InvocationList)
    return llvm::Error::success();
  if (PreviousError)
    return llvm::make_error<IndexError>(*PreviousError);

  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileContent =
      CI.getVirtualFileSystem().getBufferForFile(InvocationListFilePath);
  if (!FileContent) {
    PreviousError = IndexError(index_error_code::invocation_list_file_not_found,
                               InvocationListFilePath.str());
    return llvm::make_error<IndexError>(*PreviousError);
  }
  std::unique_ptr<llvm::MemoryBuffer> ContentBuffer = std::move(*FileContent);
  assert(ContentBuffer && "If no error was produced after loading, the pointer "
                          "should not be nullptr.");

  llvm::Expected<InvocationListTy> ExpectedInvocationList = parseInvocationList(
      ContentBuffer->getBuffer(), PathStyle, InvocationListFilePath);

  if (!ExpectedInvocationList) {
    llvm::handleAllErrors(
        ExpectedInvocationList.takeError(),
        [this](const IndexError &E) { this->PreviousError = E; });
    return llvm::make_error<IndexError>(*PreviousError);
  }

  InvocationList = *ExpectedInvocationList;

  return llvm::Error::success();
}

template <typename T>
llvm::Expected<const T *>
CrossTranslationUnitContext::importDefinitionImpl(const T *D, ASTUnit *Unit) {
  assert(hasBodyOrInit(D) && "Decls to be imported should have body or init.");

  assert(&D->getASTContext() == &Unit->getASTContext() &&
         "ASTContext of Decl and the unit should match.");
  ASTImporter &Importer = getOrCreateASTImporter(Unit);

  auto ToDeclOrError = Importer.Import(D);
  if (!ToDeclOrError) {
    handleAllErrors(ToDeclOrError.takeError(), [&](const ASTImportError &IE) {
      switch (IE.Error) {
      case ASTImportError::NameConflict:
        ++NumNameConflicts;
        break;
      case ASTImportError::UnsupportedConstruct:
        ++NumUnsupportedNodeFound;
        break;
      case ASTImportError::Unknown:
        llvm_unreachable("Unknown import error happened.");
        break;
      }
    });
    return llvm::make_error<IndexError>(index_error_code::failed_import);
  }
  auto *ToDecl = cast<T>(*ToDeclOrError);
  assert(hasBodyOrInit(ToDecl) && "Imported Decl should have body or init.");
  ++NumGetCTUSuccess;

  // Parent map is invalidated after changing the AST.
  ToDecl->getASTContext().getParentMapContext().clear();

  return ToDecl;
}

llvm::Expected<const FunctionDecl *>
CrossTranslationUnitContext::importDefinition(const FunctionDecl *FD,
                                              ASTUnit *Unit) {
  return importDefinitionImpl(FD, Unit);
}

llvm::Expected<const VarDecl *>
CrossTranslationUnitContext::importDefinition(const VarDecl *VD,
                                              ASTUnit *Unit) {
  return importDefinitionImpl(VD, Unit);
}

void CrossTranslationUnitContext::lazyInitImporterSharedSt(
    TranslationUnitDecl *ToTU) {
  if (!ImporterSharedSt)
    ImporterSharedSt = std::make_shared<ASTImporterSharedState>(*ToTU);
}

ASTImporter &
CrossTranslationUnitContext::getOrCreateASTImporter(ASTUnit *Unit) {
  ASTContext &From = Unit->getASTContext();

  auto I = ASTUnitImporterMap.find(From.getTranslationUnitDecl());
  if (I != ASTUnitImporterMap.end())
    return *I->second;
  lazyInitImporterSharedSt(Context.getTranslationUnitDecl());
  ASTImporter *NewImporter = new ASTImporter(
      Context, Context.getSourceManager().getFileManager(), From,
      From.getSourceManager().getFileManager(), false, ImporterSharedSt);
  // codeXek RF01-R: candidate selection constrains every body import from
  // this foreign unit — explicit queries and incidental passengers alike.
  NewImporter->setShouldImportFunctionBody(
      [this](const FunctionDecl *FD) { return shouldImportForeignBody(FD); });
  ASTUnitImporterMap[From.getTranslationUnitDecl()].reset(NewImporter);
  return *NewImporter;
}

std::optional<clang::MacroExpansionContext>
CrossTranslationUnitContext::getMacroExpansionContextForSourceLocation(
    const clang::SourceLocation &ToLoc) const {
  // FIXME: Implement: Record such a context for every imported ASTUnit; lookup.
  return std::nullopt;
}

// codeXek DU01: verify that an imported definition (explicitly queried or
// transitively pulled in by another import) is the duplicate-resolution
// winner for THIS caller TU. Bodies of other candidates must not inline —
// their use would make diagnostics depend on import order.
bool CrossTranslationUnitContext::isChosenCandidate(const Decl *D) const {
  const SourceManager &SM = Context.getSourceManager();
  const std::optional<std::string> LookupName = getLookupName(D);
  if (!LookupName)
    return true;
  StringRef CallerSource;
  if (auto MainFile = SM.getFileEntryRefForID(SM.getMainFileID()))
    CallerSource = MainFile->getName();
  // RF01-M: macro-generated definitions carry the macro-expansion location;
  // resolve the spelling through it so the comparison uses the candidate's
  // actual file. A location that still yields no file cannot be attributed
  // to any candidate — never gate on it.
  StringRef DefinitionFile = SM.getFilename(SM.getExpansionLoc(D->getLocation()));
  if (DefinitionFile.empty())
    return true;
  return ASTStorage.isChosenCandidate(*LookupName, CallerSource,
                                      DefinitionFile);
}

// codeXek RF01-R: unified candidate selection constrains imports. Every
// foreign function body — the one this TU explicitly queries and any that
// rides along with another import — may enter the analyzed AST only if it
// belongs to the candidate this caller's duplicate resolution chooses.
// A non-chosen body is imported as a declaration only (merged into the
// redeclaration chain, parameters and type intact), so it cannot occupy
// the chosen definition's place; the chosen body loads on demand through
// the normal getCrossTUDefinition path. Diagnostics therefore never depend
// on import order. Single-candidate and non-indexed functions never gate;
// index errors never gate (they are reported through the lookup path).
bool CrossTranslationUnitContext::shouldImportForeignBody(
    const FunctionDecl *FromFD) {
  const std::optional<std::string> LookupName = getLookupName(FromFD);
  if (!LookupName)
    return true;
  const SourceManager &FromSM = FromFD->getASTContext().getSourceManager();
  // RF01-M: same expansion-location rule on the import gate — a
  // macro-generated definition must be attributed to its expansion file,
  // and an unattributable location never gates.
  StringRef DefinitionFile =
      FromSM.getFilename(FromSM.getExpansionLoc(FromFD->getLocation()));
  if (DefinitionFile.empty())
    return true;
  StringRef CallerSource;
  if (auto MainFile = Context.getSourceManager().getFileEntryRefForID(
          Context.getSourceManager().getMainFileID()))
    CallerSource = MainFile->getName();
  return ASTStorage.mayImportBody(*LookupName, CallerSource, DefinitionFile);
}

bool CrossTranslationUnitContext::isImportedAsNew(const Decl *ToDecl) const {
  if (!ImporterSharedSt)
    return false;
  return ImporterSharedSt->isNewDecl(const_cast<Decl *>(ToDecl));
}

bool CrossTranslationUnitContext::hasError(const Decl *ToDecl) const {
  if (!ImporterSharedSt)
    return false;
  return static_cast<bool>(
      ImporterSharedSt->getImportDeclErrorIfAny(const_cast<Decl *>(ToDecl)));
}

} // namespace cross_tu
} // namespace clang
