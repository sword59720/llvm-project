//===---------- IssueHash.cpp - Generate identification hashes --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Analysis/IssueHash.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MD5.h"

#include <optional>
#include <sstream>
#include <string>

using namespace clang;

// Get a string representation of the parts of the signature that can be
// overloaded on.
static std::string GetSignature(const FunctionDecl *Target) {
  if (!Target)
    return "";
  std::string Signature;

  // When a flow sensitive bug happens in templated code we should not generate
  // distinct hash value for every instantiation. Use the signature from the
  // primary template.
  if (const FunctionDecl *InstantiatedFrom =
          Target->getTemplateInstantiationPattern())
    Target = InstantiatedFrom;

  if (!isa<CXXConstructorDecl>(Target) && !isa<CXXDestructorDecl>(Target) &&
      !isa<CXXConversionDecl>(Target))
    Signature.append(Target->getReturnType().getAsString()).append(" ");
  Signature.append(Target->getQualifiedNameAsString()).append("(");

  for (int i = 0, paramsCount = Target->getNumParams(); i < paramsCount; ++i) {
    if (i)
      Signature.append(", ");
    Signature.append(Target->getParamDecl(i)->getType().getAsString());
  }

  if (Target->isVariadic())
    Signature.append(", ...");
  Signature.append(")");

  const auto *TargetT =
      llvm::dyn_cast_or_null<FunctionType>(Target->getType().getTypePtr());

  if (!TargetT || !isa<CXXMethodDecl>(Target))
    return Signature;

  if (TargetT->isConst())
    Signature.append(" const");
  if (TargetT->isVolatile())
    Signature.append(" volatile");
  if (TargetT->isRestrict())
    Signature.append(" restrict");

  if (const auto *TargetPT =
          dyn_cast_or_null<FunctionProtoType>(Target->getType().getTypePtr())) {
    switch (TargetPT->getRefQualifier()) {
    case RQ_LValue:
      Signature.append(" &");
      break;
    case RQ_RValue:
      Signature.append(" &&");
      break;
    default:
      break;
    }
  }

  return Signature;
}

static std::string GetEnclosingDeclContextSignature(const Decl *EnclosingDecl) {
  if (const auto *ND = dyn_cast_or_null<NamedDecl>(EnclosingDecl)) {
    if (const auto *FD = dyn_cast<FunctionDecl>(ND))
      // To distinguish overloads we need to use the signature.
      return GetSignature(FD);
    return ND->getQualifiedNameAsString();
  }
  return "";
}

static StringRef GetNthLineOfFile(std::optional<llvm::MemoryBufferRef> Buffer,
                                  int Line) {
  if (!Buffer)
    return "";

  llvm::line_iterator LI(*Buffer, false);
  for (; !LI.is_at_eof() && LI.line_number() != Line; ++LI)
    ;

  return *LI;
}

static std::string NormalizeLine(const SourceManager &SM, const FullSourceLoc &L,
                                 const LangOptions &LangOpts) {
  static StringRef Whitespaces = " \t\n";

  StringRef Str = GetNthLineOfFile(SM.getBufferOrNone(L.getFileID(), L),
                                   L.getExpansionLineNumber());
  StringRef::size_type col = Str.find_first_not_of(Whitespaces);
  if (col == StringRef::npos)
    col = 1; // The line only contains whitespace.
  else
    col++;
  SourceLocation StartOfLine =
      SM.translateLineCol(SM.getFileID(L), L.getExpansionLineNumber(), col);
  std::optional<llvm::MemoryBufferRef> Buffer =
      SM.getBufferOrNone(SM.getFileID(StartOfLine), StartOfLine);
  if (!Buffer)
    return {};

  const char *BufferPos = SM.getCharacterData(StartOfLine);

  Token Token;
  Lexer Lexer(SM.getLocForStartOfFile(SM.getFileID(StartOfLine)), LangOpts,
              Buffer->getBufferStart(), BufferPos, Buffer->getBufferEnd());

  size_t NextStart = 0;
  std::ostringstream LineBuff;
  while (!Lexer.LexFromRawLexer(Token) && NextStart < 2) {
    if (Token.isAtStartOfLine() && NextStart++ > 0)
      continue;
    LineBuff << std::string(SM.getCharacterData(Token.getLocation()),
                            Token.getLength());
  }

  return LineBuff.str();
}

static llvm::SmallString<32> GetMD5HashOfContent(StringRef Content) {
  llvm::MD5 Hash;
  llvm::MD5::MD5Result MD5Res;
  SmallString<32> Res;

  Hash.update(Content);
  Hash.final(MD5Res);
  llvm::MD5::stringifyResult(MD5Res, Res);

  return Res;
}

// Tokenizes the issue line with the exact same walk NormalizeLine performs
// and additionally determines, within that single walk, the byte offset of
// the issue's starting token inside the concatenation of the line's tokens.
// The V2 hash must take the relative column and the line content from one
// shared tokenization: two independent computations could reintroduce
// sensitivity to whitespace alignment.
//
// Keep this walk in lockstep with NormalizeLine: same start (first
// non-whitespace character of the issue's expansion line) and same stopping
// rule (first token that starts a new line).
static std::pair<std::string, size_t>
GetNormalizedLineAndIssueColumnOffset(const SourceManager &SM,
                                      const FullSourceLoc &L,
                                      const LangOptions &LangOpts) {
  static StringRef Whitespaces = " \t\n";

  StringRef Str = GetNthLineOfFile(SM.getBufferOrNone(L.getFileID(), L),
                                   L.getExpansionLineNumber());
  StringRef::size_type col = Str.find_first_not_of(Whitespaces);
  if (col == StringRef::npos)
    col = 1; // The line only contains whitespace.
  else
    col++;
  SourceLocation StartOfLine =
      SM.translateLineCol(SM.getFileID(L), L.getExpansionLineNumber(), col);
  std::optional<llvm::MemoryBufferRef> Buffer =
      SM.getBufferOrNone(SM.getFileID(StartOfLine), StartOfLine);
  if (!Buffer)
    return {};

  const char *BufferPos = SM.getCharacterData(StartOfLine);
  // The issue position shares the line's buffer, so raw pointer comparison
  // against token extents is valid. Same FileID as the line by construction.
  const char *IssuePos = SM.getCharacterData(L);

  Token Token;
  Lexer Lexer(SM.getLocForStartOfFile(SM.getFileID(StartOfLine)), LangOpts,
              Buffer->getBufferStart(), BufferPos, Buffer->getBufferEnd());

  size_t NextStart = 0;
  size_t ColumnOffset = 0;
  size_t IssueColumnOffset = 0;
  bool FoundIssueToken = false;
  std::ostringstream LineBuff;
  while (!Lexer.LexFromRawLexer(Token) && NextStart < 2) {
    if (Token.isAtStartOfLine() && NextStart++ > 0)
      continue;
    const char *TokStart = SM.getCharacterData(Token.getLocation());
    if (!FoundIssueToken && TokStart + Token.getLength() > IssuePos) {
      // First token that is not entirely before the issue location: this is
      // the token the issue starts at (covers the issue pointing at a token
      // start, into a token, or at whitespace preceding it).
      IssueColumnOffset = ColumnOffset;
      FoundIssueToken = true;
    }
    LineBuff << std::string(TokStart, Token.getLength());
    ColumnOffset += Token.getLength();
  }
  // The issue location lies past every token of the line (e.g. inside a
  // trailing comment): attribute it to the end of the concatenation.
  if (!FoundIssueToken)
    IssueColumnOffset = ColumnOffset;

  return {LineBuff.str(), IssueColumnOffset};
}

std::string clang::getIssueString(const FullSourceLoc &IssueLoc,
                                  StringRef CheckerName,
                                  StringRef WarningMessage,
                                  const Decl *IssueDecl,
                                  const LangOptions &LangOpts) {
  static StringRef Delimiter = "$";

  return (llvm::Twine(CheckerName) + Delimiter +
          GetEnclosingDeclContextSignature(IssueDecl) + Delimiter +
          Twine(IssueLoc.getExpansionColumnNumber()) + Delimiter +
          NormalizeLine(IssueLoc.getManager(), IssueLoc, LangOpts) +
          Delimiter + WarningMessage)
      .str();
}

SmallString<32> clang::getIssueHash(const FullSourceLoc &IssueLoc,
                                    StringRef CheckerName,
                                    StringRef WarningMessage,
                                    const Decl *IssueDecl,
                                    const LangOptions &LangOpts) {

  return GetMD5HashOfContent(getIssueString(
      IssueLoc, CheckerName, WarningMessage, IssueDecl, LangOpts));
}

std::string clang::getIssueStringV2(const FullSourceLoc &IssueLoc,
                                    StringRef CheckerName,
                                    const Decl *IssueDecl,
                                    const LangOptions &LangOpts) {
  static StringRef Delimiter = "$";

  auto [Line, ColumnOffset] = GetNormalizedLineAndIssueColumnOffset(
      IssueLoc.getManager(), IssueLoc, LangOpts);

  return (llvm::Twine(CheckerName) + Delimiter +
          GetEnclosingDeclContextSignature(IssueDecl) + Delimiter +
          llvm::Twine(ColumnOffset) + Delimiter + Line)
      .str();
}

SmallString<32> clang::getIssueHashV2(const FullSourceLoc &IssueLoc,
                                      StringRef CheckerName,
                                      const Decl *IssueDecl,
                                      const LangOptions &LangOpts) {

  return GetMD5HashOfContent(
      getIssueStringV2(IssueLoc, CheckerName, IssueDecl, LangOpts));
}
