//===- NullDereferenceRecovery.h - Null access recovery ------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_NULLDEREFERENCERECOVERY_H
#define LLVM_CLANG_STATICANALYZER_CORE_PATHSENSITIVE_NULLDEREFERENCERECOVERY_H

#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramState.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/ProgramStateTrait.h"

namespace clang::ento {

/// Set by the null dereference checker after reporting a recoverable load or
/// store. Consumed by ExprEngine when evaluating that memory access. Suppressed
/// reports and null reference bindings do not request memory access recovery.
struct PendingNullDereferenceRecovery {};

template <>
struct ProgramStateTrait<PendingNullDereferenceRecovery>
    : ProgramStatePartialTrait<bool> {
  static void *GDMIndex() {
    static int Index;
    return &Index;
  }
};

} // namespace clang::ento

#endif
