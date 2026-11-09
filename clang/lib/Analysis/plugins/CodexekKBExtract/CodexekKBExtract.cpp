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
//     or a single `return <pointer param>;` with an unchanged pointer type.
//     where <condition> is one of
//       <integer param> == <int literal> | != | !<param> | bare <param>
//     and every reachable path ends in a return.
//
//   Everything else => coverage "unsupported" with a reason. The extractor
//   never emits a half-proven contract (the analogue of patch semantics:
//   a merge-style "partial extraction" would silently change analyzer
//   behavior; unsupported entries keep the original analysis).
//
// RET entry predicates require integer parameters. A direct pointer return
// uses an unconditional RETARG record.
//
// Output (path passed via -plugin-arg-cx-kb-extract <path>):
//   {"schema":1, "file":"<source path>", "entries":[
//     {"usr","name","signature","linkage","line",
//      "coverage":"complete"|"unsupported","reason",
//      "records":[<JSON objects with the encodeRecord field names>]}]}

#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/Type.h"
#include "clang/Analysis/CodexekKBInline.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/UnifiedSymbolResolution/USRGeneration.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"

#include <optional>
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

const VarDecl *integerGuardParam(const Expr *E, const FunctionDecl &FD) {
  const auto *Param = paramRef(E, FD);
  if (!Param || !Param->getType()->isIntegerType() ||
      Param->getType().isVolatileQualified())
    return nullptr;
  return Param;
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
    const auto *Ref = integerGuardParam(BO->getLHS(), FD);
    if (!Ref || !Lit) {
      Ref = integerGuardParam(BO->getRHS(), FD);
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
    llvm::APSInt Literal(Lit->getValue(), Lit->getType()->isUnsignedIntegerType());
    auto Value = Literal.tryExtValue();
    auto Type = Ref->getType();
    unsigned Width = FD.getASTContext().getIntWidth(Type);
    bool Fits = Value && (Type->isUnsignedIntegerType()
                              ? *Value >= 0 && Literal.isIntN(Width)
                              : llvm::APInt(64, *Value).isSignedIntN(Width));
    if (!Fits) {
      why = "comparison constant outside parameter or KB integer range";
      return false;
    }
    C.value = *Value;
    return true;
  }
  if (const auto *UO = dyn_cast<UnaryOperator>(Cond)) {
    if (UO->getOpcode() != UO_LNot) {
      why = "unrecognized unary condition";
      return false;
    }
    const auto *Ref = integerGuardParam(UO->getSubExpr(), FD);
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
  if (const auto *Ref = integerGuardParam(Cond, FD)) {
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

// Null and the address of a strong, non-thread-local object with static
// storage can be represented as pointer nullness facts.
bool recognizeReturnValue(const Expr *E, ASTContext &Ctx,
                          bool &ReturnsNonNull) {
  if (E->isNullPointerConstant(Ctx, Expr::NPC_ValueDependentIsNotNull)) {
    ReturnsNonNull = false;
    return true;
  }
  const auto *Op = dyn_cast<UnaryOperator>(E->IgnoreParenImpCasts());
  if (!Op || Op->getOpcode() != UO_AddrOf)
    return false;
  const auto *Ref = dyn_cast<DeclRefExpr>(Op->getSubExpr()->IgnoreParens());
  const auto *Object = Ref ? dyn_cast<VarDecl>(Ref->getDecl()) : nullptr;
  if (!Object || !Object->hasGlobalStorage() || Object->isWeak() ||
      Object->getTLSKind() != VarDecl::TLS_None)
    return false;
  ReturnsNonNull = true;
  return true;
}

struct Exit {
  std::optional<Condition> condition;
  unsigned returnArgument = 0;
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

  BodyWalker(const FunctionDecl &FD, ASTContext &Ctx, Entry &E)
      : FD(FD), Ctx(Ctx), E(E) {}

  bool hasFailed() const { return failed; }
  const std::string &failureReason() const { return why; }

  /* Analyzes the body. Flow::Terminates means every path provably ends in a
   * recognized constant return (E.exits is the complete contract, with each
   * exit carrying its accumulated path condition). Flow::FallsThrough means
   * some path leaves the function without a return — not full coverage. */
  Flow walkBody(const Stmt *Body) {
    if (const auto *CS = dyn_cast<CompoundStmt>(Body)) {
      if (CS->size() == 1)
        if (const auto *RS = dyn_cast<ReturnStmt>(*CS->body_begin()))
          if (emitAliasReturn(RS))
            return Flow::Terminates;
      return walkSequence(CS);
    }
    fail("body is not a statement sequence");
    return Flow::FallsThrough;
  }

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

  bool emitAliasReturn(const ReturnStmt *RS) {
    const auto *Value = RS->getRetValue();
    const auto *Param = Value ? paramRef(Value, FD) : nullptr;
    if (!Param || Param->getType().isVolatileQualified() ||
        !Ctx.hasSameType(Param->getType(), FD.getReturnType()))
      return false;
    for (unsigned I = 0; I < FD.getNumParams(); ++I)
      if (FD.getParamDecl(I) == Param) {
        Exit X;
        X.returnArgument = I + 1;
        X.line = Ctx.getSourceManager().getPresumedLoc(RS->getReturnLoc()).getLine();
        E.exits.push_back(X);
        return true;
      }
    return false;
  }

  Flow emitReturn(const ReturnStmt *RS,
                  std::optional<Condition> condition) {
    Exit exit;
    exit.line =
        Ctx.getSourceManager().getPresumedLoc(RS->getReturnLoc()).getLine();
    if (!RS->getRetValue()) {
      fail("void return in pointer-returning function");
      return Flow::Unsupported;
    }
    if (!recognizeReturnValue(RS->getRetValue(), Ctx, exit.returnsNonNull)) {
      fail("return is neither null nor the address of a static object");
      return Flow::Unsupported;
    }
    exit.condition = condition;
    E.exits.push_back(exit);
    return Flow::Terminates;
  }

  Flow walkSequence(const CompoundStmt *CS) {
    std::optional<Condition> remaining;
    for (const Stmt *Child : CS->body()) {
      if (const auto *IS = dyn_cast<IfStmt>(Child)) {
        // A second guard would need the conjunction with the first guard's
        // negation. RET records can express only one entry predicate.
        if (remaining) {
          fail("multiple guards require a compound entry predicate");
          return Flow::Unsupported;
        }
        if (IS->getInit() || IS->getConditionVariable()) {
          fail("if with init/condition variable");
          return Flow::Unsupported;
        }
        Condition C;
        if (!recognizeCondition(IS->getCond(), FD, C, why)) {
          fail("unextractable condition: " + why);
          return Flow::Unsupported;
        }
        const Stmt *Then = IS->getThen();
        const ReturnStmt *RS = dyn_cast<ReturnStmt>(Then);
        if (!RS && isa_and_nonnull<CompoundStmt>(Then)) {
          const auto *CS2 = cast<CompoundStmt>(Then);
          if (CS2->size() == 1) RS = dyn_cast<ReturnStmt>(*CS2->body().begin());
        }
        if (!RS) {
          fail("if body is not a single return statement");
          return Flow::Unsupported;
        }
        if (emitReturn(RS, C) == Flow::Unsupported)
          return Flow::Unsupported;

        C.equal = !C.equal;
        if (const Stmt *Else = IS->getElse()) {
          const auto *ER = dyn_cast<ReturnStmt>(Else);
          if (!ER) {
            fail("if else body is not a single return statement");
            return Flow::Unsupported;
          }
          return emitReturn(ER, C);
        }
        remaining = C;
        continue;
      }
      Flow childFlow = walkStatement(Child, remaining);
      if (childFlow == Flow::Unsupported) return Flow::Unsupported;
      if (childFlow == Flow::Terminates) return Flow::Terminates;
    }
    return Flow::FallsThrough;
  }

  Flow walkStatement(const Stmt *S, std::optional<Condition> condition) {
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
    case Stmt::ReturnStmtClass:
      return emitReturn(cast<ReturnStmt>(S), condition);
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

bool supportedCppForwardingFunction(const FunctionDecl &FD) {
  if (isa<CXXMethodDecl>(FD) || !FD.getIdentifier() ||
      FD.getTemplatedKind() != FunctionDecl::TK_NonTemplate)
    return false;
  const auto *Context = FD.getDeclContext()->getRedeclContext();
  if (!Context->isFileContext())
    return false;
  for (const auto *Declaration : Context->lookup(FD.getDeclName()))
    if (Declaration->getCanonicalDecl() != FD.getCanonicalDecl())
      return false;
  auto isValue = [](QualType T) {
    return !T.isVolatileQualified() &&
           (T->isIntegerType() || T->isPointerType());
  };
  if (!FD.getReturnType()->isVoidType() && !isValue(FD.getReturnType()))
    return false;
  // Even an unused by-value object parameter can run a destructor.
  return llvm::all_of(FD.parameters(), [&](const ParmVarDecl *P) {
    return isValue(P->getType());
  });
}

// Capture direct forwarding independently of the configured KB. Prepare can
// recompose the record after a model edit using the cached extraction.
std::optional<llvm::json::Object> forwardingCall(const FunctionDecl &FD,
                                                ASTContext &Ctx) {
  if (!FD.isExternallyVisible() || FD.isWeak() || FD.hasAttr<AsmLabelAttr>() ||
      FD.isNoReturn() || !FD.getType()->isFunctionProtoType() ||
      (Ctx.getLangOpts().CPlusPlus && !supportedCppForwardingFunction(FD)))
    return std::nullopt;
  const auto *Body = dyn_cast<CompoundStmt>(FD.getBody());
  if (!Body || Body->size() != 1)
    return std::nullopt;
  const Stmt *Statement = *Body->body_begin();
  const Expr *Expression = dyn_cast<Expr>(Statement);
  if (const auto *Return = dyn_cast<ReturnStmt>(Statement))
    Expression = Return->getRetValue();
  else if (!FD.getReturnType()->isVoidType())
    return std::nullopt;
  const auto *Call =
      Expression ? dyn_cast<CallExpr>(Expression->IgnoreParens()) : nullptr;
  const auto *Callee = Call ? Call->getDirectCallee() : nullptr;
  if (!Callee || Callee->hasBody() || !Callee->isExternallyVisible() ||
      Callee->isWeak() || Callee->isVariadic() || Callee->isNoReturn() ||
      !Callee->getType()->isFunctionProtoType() || Callee->getBuiltinID() ||
      Callee->hasAttr<AsmLabelAttr>() ||
      (Ctx.getLangOpts().CPlusPlus && !supportedCppForwardingFunction(*Callee)) ||
      Call->getNumArgs() != Callee->getNumParams() ||
      !Ctx.hasSameType(FD.getReturnType(), Callee->getReturnType()))
    return std::nullopt;
  auto USR = generateUSR(*Callee);
  if (USR.empty())
    return std::nullopt;
  llvm::SmallDenseSet<const VarDecl *, 8> Used;
  llvm::json::Array Arguments;
  for (unsigned I = 0; I < Call->getNumArgs(); ++I) {
    const auto *Param = paramRef(Call->getArg(I), FD);
    if (!Param || Param->getType().isVolatileQualified() ||
        !Ctx.hasSameType(Param->getType(), Callee->getParamDecl(I)->getType()) ||
        !Used.insert(Param).second)
      return std::nullopt;
    const auto *Parm = cast<ParmVarDecl>(Param);
    Arguments.push_back(static_cast<int64_t>(Parm->getFunctionScopeIndex()) + 1);
  }
  return llvm::json::Object{{"callee_usr", std::move(USR)},
                            {"callee", Callee->getQualifiedNameAsString()},
                            {"signature", canonicalSignature(*Callee, Ctx)},
                            {"arguments", std::move(Arguments)}};
}

class KBExtractConsumer : public ASTConsumer {
  std::string OutPath;
  std::string SourcePath;

  void extractDefinitions(const DeclContext &Context, ASTContext &Ctx,
                          llvm::json::Array &Entries) {
    auto &SM = Ctx.getSourceManager();
    for (Decl *D : Context.decls()) {
      if (isa<NamespaceDecl, LinkageSpecDecl>(D)) {
        extractDefinitions(*cast<DeclContext>(D), Ctx, Entries);
        continue;
      }
      const auto *FD = dyn_cast<FunctionDecl>(D);
      if (!FD) continue;
      if (!FD->isThisDeclarationADefinition() || !FD->hasBody()) continue;
      auto Location = SM.getPresumedLoc(FD->getLocation());
      Entry E;
      E.usr = generateUSR(*FD);
      E.name = FD->getQualifiedNameAsString();
      E.signature = canonicalSignature(*FD, Ctx);
      E.line = Location.getLine();
      E.linkage = FD->isExternallyVisible() ? "external" : "internal";
      llvm::json::Object JE{{"usr", E.usr}, {"name", E.name},
                            {"signature", E.signature}, {"linkage", E.linkage},
                            {"file", Location.isValid() ? Location.getFilename() : SourcePath},
                            {"line", static_cast<int64_t>(E.line)}};
      if (!FD->isVariadic())
        if (auto Forward = forwardingCall(*FD, Ctx)) {
          JE["coverage"] = "unsupported";
          JE["reason"] = "forwarded call needs a configured contract";
          JE["forward"] = std::move(*Forward);
          Entries.push_back(std::move(JE));
          continue;
        }
      if (FD->isVariadic() || isa<CXXMethodDecl>(FD) ||
          FD->getTemplatedKind() != FunctionDecl::TK_NonTemplate ||
          !FD->getReturnType()->isPointerType()) {
        JE["coverage"] = "unsupported";
        JE["reason"] = FD->getReturnType()->isPointerType()
                           ? "C++ method, template or variadic"
                           : "non-pointer return";
        Entries.push_back(std::move(JE));
        continue;
      }

      if (Ctx.getLangOpts().CPlusPlus &&
          (!Context.isTranslationUnit() || !supportedCppForwardingFunction(*FD))) {
        JE["coverage"] = "unsupported";
        JE["reason"] = "C++ declaration outside the supported leaf-function subset";
        Entries.push_back(std::move(JE));
        continue;
      }

      BodyWalker Walker(*FD, Ctx, E);
      auto flow = Walker.walkBody(FD->getBody());

      if (Walker.hasFailed() || flow != BodyWalker::Flow::Terminates) {
        JE["coverage"] = "unsupported";
        JE["reason"] = Walker.hasFailed()
                           ? Walker.failureReason()
                           : "fall-through exit without return";
        Entries.push_back(std::move(JE));
        continue;
      }
      JE["coverage"] = "complete";
      if (auto Leaf = codexek::exactInlineLeaf(*FD, Ctx))
        JE["inline_key"] = std::move(Leaf->Key);
      llvm::json::Array Records;
      for (const auto &X : E.exits) {
        llvm::json::Object Rec;
        Rec["schema"] = 1;
        Rec["function"] = E.name;
        Rec["signature"] = E.signature;
        Rec["unoverloaded"] = false; /* canonical signature present */
        Rec["kind"] = X.returnArgument ? "RETARG" : "RET";
        Rec["file"] = SourcePath;
        Rec["line"] = static_cast<int64_t>(X.line);
        Rec["entry"] = predAlways();
        if (const auto &C = X.condition)
          Rec["entry"] = C->equal ? predEq(C->param, C->value)
                                 : predNe(C->param, C->value);
        if (X.returnArgument) {
          Rec["target"] = predReturn();
          Rec["source"] = predOperand(X.returnArgument);
        } else {
          Rec["success"] = X.returnsNonNull ? retSuccessNonNull()
                                            : retSuccessEq(0);
        }
        Records.push_back(std::move(Rec));
      }
      JE["records"] = std::move(Records);
      Entries.push_back(std::move(JE));
    }
  }

  void HandleTranslationUnit(ASTContext &Ctx) override {
    llvm::json::Array Entries;
    extractDefinitions(*Ctx.getTranslationUnitDecl(), Ctx, Entries);
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
