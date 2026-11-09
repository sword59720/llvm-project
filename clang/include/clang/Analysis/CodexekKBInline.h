//===- CodexekKBInline.h - Exact leaf summary eligibility -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM-exception.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_ANALYSIS_CODEXECKBINLINE_H
#define LLVM_CLANG_ANALYSIS_CODEXECKBINLINE_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Stmt.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/SHA256.h"
#include <optional>

namespace clang::codexek {

struct InlineLeaf {
  std::string Key;
  // Zero denotes a null pointer. Otherwise this is a one-based argument.
  unsigned Argument;
};

inline std::optional<InlineLeaf> exactInlineLeaf(const FunctionDecl &FD,
                                                const ASTContext &Ctx) {
  if (Ctx.getLangOpts().CPlusPlus || !FD.isExternallyVisible() || FD.isWeak() ||
      FD.hasAttrs() || FD.isVariadic() || FD.getBuiltinID() ||
      !FD.getType()->isFunctionProtoType() ||
      !FD.getReturnType()->isPointerType())
    return std::nullopt;
  for (const auto *Param : FD.parameters()) {
    auto Type = Param->getType();
    if (Type.isVolatileQualified() ||
        (!Type->isPointerType() && !Type->isIntegerType()))
      return std::nullopt;
  }
  const auto *Body = dyn_cast_or_null<CompoundStmt>(FD.getBody());
  if (!Body || Body->size() != 1)
    return std::nullopt;
  const auto *Return = dyn_cast<ReturnStmt>(*Body->body_begin());
  if (!Return || !Return->getRetValue())
    return std::nullopt;
  const auto *Cast =
      dyn_cast<ImplicitCastExpr>(Return->getRetValue()->IgnoreParens());
  if (!Cast)
    return std::nullopt;
  const Expr *Operand = Cast->getSubExpr()->IgnoreParens();
  unsigned Argument = 0;
  if (Cast->getCastKind() == CK_NullToPointer) {
    const auto *Literal = dyn_cast<IntegerLiteral>(Operand);
    if (!Literal || !Literal->getValue().isZero())
      return std::nullopt;
  } else if (Cast->getCastKind() == CK_LValueToRValue) {
    const auto *Ref = dyn_cast<DeclRefExpr>(Operand);
    const auto *Param = Ref ? dyn_cast<ParmVarDecl>(Ref->getDecl()) : nullptr;
    if (!Param || !Ctx.hasSameType(Param->getType(), FD.getReturnType()))
      return std::nullopt;
    unsigned Index = Param->getFunctionScopeIndex();
    if (Index >= FD.getNumParams() || FD.getParamDecl(Index) != Param)
      return std::nullopt;
    Argument = Index + 1;
  } else {
    return std::nullopt;
  }

  // Match the actual selected definition, including its physical source
  // location. Recheck the resolved return expression in the imported AST,
  // so an identical spelling under different macros cannot change the effect.
  const auto &SM = Ctx.getSourceManager();
  SourceLocation Begin = FD.getBeginLoc(), End = FD.getEndLoc();
  if (!Begin.isFileID() || !End.isFileID() || !Operand->getExprLoc().isFileID())
    return std::nullopt;
  StringRef Path = SM.getFilename(Begin);
  if (Path.empty())
    return std::nullopt;
  bool Invalid = false;
  StringRef Text = Lexer::getSourceText(CharSourceRange::getTokenRange(Begin, End),
                                       SM, Ctx.getLangOpts(), &Invalid);
  if (Invalid || Text.empty())
    return std::nullopt;
  llvm::SHA256 Hash;
  Hash.update(Path);
  Hash.update("\n");
  Hash.update(std::to_string(SM.getFileOffset(Begin)));
  Hash.update("\n");
  Hash.update(Text);
  Hash.update("\n");
  Hash.update(std::to_string(Argument));
  return InlineLeaf{llvm::toHex(Hash.final(), /*LowerCase=*/true), Argument};
}

} // namespace clang::codexek

#endif
