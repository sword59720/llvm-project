// cx-kb-extract: codexek KB behavior extractor (KB-6).
//
// Runs as an -add-plugin AST action alongside the main action during the
// CTU prepare step (clang ... -emit-ast), enumerating function definitions
// and extracting provable behavior records in the codexek KB record shape
// (field names mirror the product's encodeRecord in engine/src/kb.cpp).
//
// v1 scope — deliberately conservative; every accepted function must be a
// full-coverage, all-exits-provable contract:
//
//   Accepted function shape:
//     pointer-returning plain C function, non-variadic, non-template,
//     whose body is a linear sequence of:
//       - constant-initialized local declarations,
//       - `if (<condition>) return <constant-or-&global>;`   (no else),
//       - `if (<condition>) return V1; else return V2;`,
//       - `return <constant-or-&global>;`                    (the tail),
//     where <condition> is one of
//       <integer param> == <int literal> | != | !<param> | bare <param>
//     and every reachable path ends in a return.
//
//   Everything else => coverage "unsupported" with a reason. The extractor
//   never emits a half-proven contract (the analogue of patch semantics:
//   a merge-style "partial extraction" would silently change analyzer
//   behavior; unsupported entries keep the original analysis).
//
// Condition language: <integer param> ==/!= <integer literal>, !<param>,
// bare <param>. Pointer guards are NOT extracted (the product RET binding
// requires an integer entry operand). Pointer parameters may appear in
// return statements only as provable non-null facts (return &global).
//
// Output (path passed via -plugin-arg-cx-kb-extract -out=<path>):
//   {"schema":1, "file":"<source path>", "entries":[
//     {"usr","name","signature","linkage","line",
//      "coverage":"complete"|"unsupported","reason",
//      "records":[<JSON objects with the encodeRecord field names>]}]}

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"

#include <string>
#include <vector>

using namespace clang;

namespace {

// --- JSON helpers; field names mirror the product encodeRecord -------------

llvm::json::Value predOperand(unsigned index) {
  return llvm::json::Object{
      {"slot", "argument"}, {"index", static_cast<int64_t>(index)}};
}

llvm::json::Value predReturn() { return llvm::json::Object{{"slot", "return"}}; }

llvm::json::Value predAlways() { return llvm::json::Object{{"always", true}}; }

llvm::json::Value predEq(unsigned param, int64_t value) {
  return llvm::json::Object{{"operand", predOperand(param)},
                            {"op", "eq"},
                            {"value", value}};
}

llvm::json::Value predNe(unsigned param, int64_t value) {
  return llvm::json::Object{{"operand", predOperand(param)},
                            {"op", "ne"},
                            {"value", value}};
}

llvm::json::Value retSuccessEq(int64_t value) {
  return llvm::json::Object{{"operand", predReturn()},
                            {"op", "eq"},
                            {"value", value}};
}

llvm::json::Value retSuccessNonNull() {
  return llvm::json::Object{{"operand", predReturn()},
                            {"op", "ne"},
                            {"value", 0}};
}

// --- record language recognition -------------------------------------------

struct Condition {
  unsigned param = 0; // 1-based
  bool equal = true;
  int64_t value = 0;
};

const VarDecl *paramRef(const Expr *E, const FunctionDecl &FD) {
  E = E->IgnoreParenImpCasts();
  const auto *DRE = dyn_cast<DeclRefExpr>(E);
  if (!DRE) return nullptr;
  const auto *VD = dyn_cast<VarDecl>(DRE->getDecl());
  if (!VD) return nullptr;
  for (unsigned i = 0, n = FD.getNumParams(); i < n; ++i)
    if (FD.getParamDecl(i) == VD) return VD;
  return nullptr;
}

// Recognize <param> ==/!= <literal> (either orientation), !<param>, or bare
// <param>. Returns false when the shape is outside the record language.
bool recognizeCondition(const Expr *Cond, const FunctionDecl &FD,
                        Condition &C, std::string &why) {
  Cond = Cond->IgnoreParenImpCasts();
  if (const auto *BO = dyn_cast<BinaryOperator>(Cond)) {
    if (!BO->isEqualityOp()) {
      why = "relational or compound condition";
      return false;
    }
    const auto *Lit =
        dyn_cast<IntegerLiteral>(BO->getRHS()->IgnoreParenImpCasts());
    const auto *Ref = paramRef(BO->getLHS(), FD);
    if (!Ref || !Lit) {
      Ref = paramRef(BO->getRHS(), FD);
      Lit = dyn_cast<IntegerLiteral>(BO->getLHS()->IgnoreParenImpCasts());
    }
    if (!Ref || !Lit) {
      why = "condition is not <param> ==/!= <literal>";
      return false;
    }
    for (unsigned i = 0, n = FD.getNumParams(); i < n; ++i)
      if (FD.getParamDecl(i) == Ref) {
        C.param = i + 1;
        break;
      }
    C.equal = BO->getOpcode() == BO_EQ;
    C.value = Lit->getValue().getSExtValue();
    return true;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() != UO_LNot) {
      why = "unrecognized unary condition";
      return false;
    }
    const auto *Ref = paramRef(UO->getSubExpr(), FD);
    if (!Ref) {
      why = "condition !<expr> with expr not a parameter";
      return false;
    }
    for (unsigned i = 0, n = FD.getNumParams(); i < n; ++i)
      if (FD.getParamDecl(i) == Ref) {
        C.param = i + 1;
        break;
      }
    C.equal = true; // !p means p == 0
    C.value = 0;
    return true;
  }
  if (const auto *Ref = paramRef(Cond, FD)) {
    for (unsigned i = 0, n = FD.getNumParams(); i < n; ++i)
      if (FD.getParamDecl(i) == Ref) {
        C.param = i + 1;
        break;
      }
    C.equal = false; // bare p means p != 0
    C.value = 0;
    return true;
  }
  why = "unrecognized condition shape";
  return false;
}

// Return value: integer literal (via casts, optional unary minus),
// address-of object (non-null fact), or null pointer literal.
bool recognizeReturnValue(const Expr *E, int64_t &value,
                          bool &returnsNonNull, std::string &why) {
  E = E->IgnoreParenImpCasts();
  if (const auto *Lit = dyn_cast<IntegerLiteral>(E)) {
    value = Lit->getValue().getSExtValue();
    returnsNonNull = false;
    return true;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
    if (UO->getOpcode() == UO_Minus) {
      if (const auto *Lit = dyn_cast<IntegerLiteral>(
              UO->getSubExpr()->IgnoreParenImpCasts())) {
        value = -Lit->getValue().getSExtValue();
        returnsNonNull = false;
        return true;
      }
    }
    if (UO->getOpcode() == UO_AddrOf) {
      // &object => provable non-null; the object's identity is the
      // caller-side analyzer's responsibility.
      returnsNonNull = true;
      value = 0;
      return true;
    }
  }
  if (isa<CXXNullPtrLiteralExpr>(E)) {
    value = 0;
    returnsNonNull = false;
    return true;
  }
  why = "non-constant return value";
  return false;
}

struct Exit {
  bool hasCondition = false;
  Condition condition;
  int64_t returnValue = 0;
  bool returnsNonNull = false;
  unsigned line = 0;
};

struct Entry {
  std::string usr;
  std::string name;
  std::string signature;
  std::string linkage = "external";
  unsigned line = 0;
  std::string coverage;
  std::string reason;
  std::vector<Exit> exits;
};

// Linear-sequence body walker. Returns "terminates" for statements that end
// every path they govern (a return). Any other shape => unsupported.
class BodyWalker {
public:
  enum class Flow { Terminates, FallsThrough, Unsupported };

private:
  const FunctionDecl &FD;
  ASTContext &Ctx;
  Entry &E;
  bool failed = false;
  std::string why;

  void fail(const std::string &reason) {
    if (!failed) {
      failed = true;
      why = reason;
    }
  }

public:
  BodyWalker(const FunctionDecl &FD, ASTContext &Ctx, Entry &E)
      : FD(FD), Ctx(Ctx), E(E) {}

  bool hasFailed() const { return failed; }
  const std::string &failureReason() const { return why; }

  /* Analyzes the body. Flow::Terminates means every path provably ends in a
   * recognized constant return (E.exits is the complete contract);
   * Flow::FallsThrough means some path leaves the function without a return
   * (not full coverage) - combined with failed=false this is still valid
   * partial content, but v1 requires full coverage, so the caller treats it
   * as unsupported with the fall-through reason. */
  Flow walkBody(const Stmt *Body) {
    if (const auto *CS = dyn_cast<CompoundStmt>(Body))
      return walkSequence(CS);
    fail("body is not a statement sequence");
    return Flow::FallsThrough;
  }

private:
  Flow walkSequence(const CompoundStmt *CS) {
    Flow flow = Flow::FallsThrough;
    for (const Stmt *Child : CS->body()) {
      Flow childFlow = walkStatement(Child);
      if (childFlow == Flow::Unsupported) return Flow::Unsupported;
      if (childFlow == Flow::Terminates) return Flow::Terminates;
      flow = Flow::FallsThrough;
    }
    return flow;
  }

  Flow walkStatement(const Stmt *S) {
    switch (S->getStmtClass()) {
    case Stmt::NullStmtClass:
    case Stmt::DeclStmtClass: {
      if (const auto *DS = dyn_cast<DeclStmt>(S))
        for (const auto *D : DS->decls())
          if (const auto *VD = dyn_cast<VarDecl>(D))
            if (VD->hasInit() &&
                !isa<IntegerLiteral>(VD->getInit()->IgnoreParenImpCasts())) {
              fail("local declaration with non-constant initializer");
              return Flow::Unsupported;
            }
      return Flow::FallsThrough; // inert
    }
    case Stmt::ReturnStmtClass: {
      const auto *RS = cast<ReturnStmt>(S);
      Exit exit;
      exit.line =
          Ctx.getSourceManager().getPresumedLoc(RS->getReturnLoc()).getLine();
      if (!RS->getRetValue()) {
        fail("void return in pointer-returning function");
        return Flow::Unsupported;
      }
      if (!recognizeReturnValue(RS->getRetValue(), exit.returnValue,
                                exit.returnsNonNull, why)) {
        fail("unextractable return value: " + why);
        return Flow::Unsupported;
      }
      E.exits.push_back(exit);
      return Flow::Terminates;
    }
    case Stmt::IfStmtClass: {
      const auto *IS = cast<IfStmt>(S);
      if (IS->getInit() || IS->getConditionVariable()) {
        fail("if with init/condition variable");
        return Flow::Unsupported;
      }
      Condition C;
      if (!recognizeCondition(IS->getCond(), FD, C, why)) {
        fail("unextractable condition: " + why);
        return Flow::Unsupported;
      }
      // The then-branch must be a single return statement (or a compound
      // containing exactly one return and nothing else).
      const Stmt *Then = IS->getThen();
      const ReturnStmt *RS = dyn_cast<ReturnStmt>(Then);
      if (!RS && isa_and_nonnull<CompoundStmt>(Then)) {
        const auto *CS = cast<CompoundStmt>(Then);
        if (CS->size() == 1) RS = dyn_cast<ReturnStmt>(*CS->body().begin());
      }
      if (!RS) {
        fail("if body is not a single return statement");
        return Flow::Unsupported;
      }
      Exit exit;
      exit.line =
          Ctx.getSourceManager().getPresumedLoc(RS->getReturnLoc()).getLine();
      if (!RS->getRetValue()) {
        fail("void return in pointer-returning function");
        return Flow::Unsupported;
      }
      if (!recognizeReturnValue(RS->getRetValue(), exit.returnValue,
                                exit.returnsNonNull, why)) {
        fail("unextractable return value: " + why);
        return Flow::Unsupported;
      }
      exit.hasCondition = true;
      exit.condition = C;
      E.exits.push_back(exit);

      if (!IS->getElse()) return Flow::FallsThrough; // not-taken path continues

      // The else-branch must itself terminate (single return, or another
      // if-return sequence).
      const Stmt *Else = IS->getElse();
      if (const auto *ElseRet = dyn_cast<ReturnStmt>(Else)) {
        Exit tail;
        tail.line = Ctx.getSourceManager()
                        .getPresumedLoc(ElseRet->getReturnLoc())
                        .getLine();
        if (!ElseRet->getRetValue()) {
          fail("void return in pointer-returning function");
          return Flow::Unsupported;
        }
        if (!recognizeReturnValue(ElseRet->getRetValue(), tail.returnValue,
                                  tail.returnsNonNull, why)) {
          fail("unextractable return value: " + why);
          return Flow::Unsupported;
        }
        E.exits.push_back(tail);
        return Flow::Terminates; // both branches terminate
      }
      if (const auto *ElseCS = dyn_cast<CompoundStmt>(Else)) {
        if (walkSequence(ElseCS) == Flow::Terminates) return Flow::Terminates;
        if (failed) return Flow::Unsupported;
        return Flow::FallsThrough; // else falls through: not fully covered
      }
      if (walkStatement(Else) == Flow::Terminates) return Flow::Terminates;
      return Flow::FallsThrough;
    }
    default: {
      if (const auto *ExprS = dyn_cast<Expr>(S)) {
        const Expr *Stripped = ExprS->IgnoreParenImpCasts();
        if (isa<CallExpr>(Stripped)) {
          fail("contains a call");
          return Flow::Unsupported;
        }
        if (const auto *BO = dyn_cast<BinaryOperator>(Stripped))
          if (BO->isAssignmentOp()) {
            fail("contains an assignment");
            return Flow::Unsupported;
          }
      }
      fail("unsupported statement");
      return Flow::Unsupported;
    }
    }
  }
};

std::string canonicalSignature(const FunctionDecl &FD, ASTContext &Ctx) {
  PrintingPolicy Policy(Ctx.getLangOpts());
  Policy.SuppressTagKeyword = true;
  Policy.Bool = true;
  return FD.getType().getCanonicalType().getAsString(Policy);
}

std::string generateUSR(const FunctionDecl &FD) {
  SmallString<128> Buf;
  if (!index::generateUSRForDecl(&FD, Buf)) return Buf.str().str();
  return {};
}

class KBExtractConsumer : public ASTConsumer {
  std::string OutPath;
  std::string SourcePath;

  void HandleTranslationUnit(ASTContext &Ctx) override {
    llvm::json::Array Entries;
    auto &SM = Ctx.getSourceManager();
    for (Decl *D : Ctx.getTranslationUnitDecl()->decls()) {
      const auto *FD = dyn_cast<FunctionDecl>(D);
      if (!FD) continue;
      unsigned line =
          SM.getPresumedLoc(FD->getLocation()).getLine();
      if (!FD->isThisDeclarationADefinition() || !FD->hasBody()) continue;
      if (FD->isVariadic() || isa<CXXMethodDecl>(FD) ||
          FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate ||
          !FD->getReturnType()->isPointerType()) {
        llvm::json::Object JE;
        JE["usr"] = generateUSR(*FD);
        JE["name"] = FD->getNameAsString();
        JE["coverage"] = "unsupported";
        JE["reason"] = FD->getReturnType()->isPointerType()
                           ? "C++ method, template or variadic"
                           : "non-pointer return";
        JE["line"] = static_cast<int64_t>(line);
        Entries.push_back(std::move(JE));
        continue;
      }

      Entry E;
      E.usr = generateUSR(*FD);
      E.name = FD->getNameAsString();
      E.signature = canonicalSignature(*FD, Ctx);
      E.line = line;
      E.linkage = FD->isExternallyVisible() ? "external" : "internal";

      BodyWalker Walker(*FD, Ctx, E);
      auto flow = Walker.walkBody(FD->getBody());

      llvm::json::Object JE;
      JE["usr"] = E.usr;
      JE["name"] = E.name;
      JE["signature"] = E.signature;
      JE["linkage"] = E.linkage;
      JE["line"] = static_cast<int64_t>(E.line);
      if (Walker.hasFailed() || flow != BodyWalker::Flow::Terminates) {
        JE["coverage"] = "unsupported";
        JE["reason"] = Walker.hasFailed()
                           ? Walker.failureReason()
                           : "fall-through exit without return";
        Entries.push_back(std::move(JE));
        continue;
      }
      JE["coverage"] = "complete";
      llvm::json::Array Records;
      for (const auto &X : E.exits) {
        llvm::json::Object Rec;
        Rec["schema"] = 1;
        Rec["function"] = E.name;
        Rec["signature"] = E.signature;
        Rec["unoverloaded"] = true;
        Rec["kind"] = "RET";
        Rec["file"] = SourcePath;
        Rec["line"] = static_cast<int64_t>(X.line);
        Rec["entry"] = X.hasCondition
                           ? (X.condition.equal ? predEq(X.condition.param,
                                                        X.condition.value)
                                                : predNe(X.condition.param,
                                                         X.condition.value))
                           : predAlways();
        Rec["success"] = X.returnsNonNull ? retSuccessNonNull()
                                          : retSuccessEq(X.returnValue);
        Records.push_back(std::move(Rec));
      }
      JE["records"] = std::move(Records);
      Entries.push_back(std::move(JE));
    }

    llvm::json::Object Root;
    Root["schema"] = 1;
    Root["file"] = SourcePath;
    Root["entries"] = std::move(Entries);
    std::error_code EC;
    llvm::raw_fd_ostream Out(OutPath, EC, llvm::sys::fs::OF_Text);
    if (EC) {
      llvm::errs() << "cx-kb-extract: cannot write " << OutPath << "\n";
      return;
    }
    Out << llvm::formatv("{0}", llvm::json::Value(std::move(Root)));
    Out.flush();
  }

public:
  KBExtractConsumer(StringRef InFile, StringRef Out)
      : OutPath(Out.str()), SourcePath(InFile.str()) {}
};

class KBExtractAction : public PluginASTAction {
  std::string OutPath;
  std::string SourcePath;

protected:
  std::unique_ptr<ASTConsumer>
  CreateASTConsumer(CompilerInstance &CI, StringRef InFile) override {
    SourcePath = InFile.str();
    return std::make_unique<KBExtractConsumer>(InFile, OutPath);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string> &arg) override {
    /* Positional output path (a "-out=..." value would be swallowed by the
     * clang option parser). Extra args ignored for forward compatibility. */
    if (!arg.empty()) OutPath = arg[0];
    if (OutPath.empty()) {
      llvm::errs() << "cx-kb-extract: missing output path argument\n";
      return false;
    }
    return true;
  }

  ActionType getActionType() override { return AddAfterMainAction; }
};

} // namespace

static FrontendPluginRegistry::Add<KBExtractAction>
    RegisterKBExtract("cx-kb-extract", "codexek KB behavior extractor (KB-6)");
