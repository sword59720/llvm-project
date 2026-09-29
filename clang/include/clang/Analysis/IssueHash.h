//===---------- IssueHash.h - Generate identification hashes ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_ANALYSIS_ISSUEHASH_H
#define LLVM_CLANG_ANALYSIS_ISSUEHASH_H

#include "llvm/ADT/SmallString.h"

namespace clang {
class Decl;
class FullSourceLoc;
class LangOptions;

/// Returns an opaque identifier for a diagnostic.
///
/// This opaque identifier is intended to be stable even when the source code
/// is changed. It allows to track diagnostics in the long term, for example,
/// find which diagnostics are "new", maintain a database of suppressed
/// diagnostics etc.
///
/// We may introduce more variants of issue hashes in the future
/// but older variants will still be available for compatibility.
///
/// This hash is based on the following information:
///   - Name of the checker that emitted the diagnostic.
///   - Warning message.
///   - Name of the enclosing declaration.
///   - Contents of the line of code with the issue, excluding whitespace.
///   - Column number (but not the line number! - which makes it stable).
llvm::SmallString<32> getIssueHash(const FullSourceLoc &IssueLoc,
                                   llvm::StringRef CheckerName,
                                   llvm::StringRef WarningMessage,
                                   const Decl *IssueDecl,
                                   const LangOptions &LangOpts);

/// Get the unhashed string representation of the V1 issue hash.
/// When hashed, it becomes the actual issue hash. Useful for testing.
/// See GetIssueHashV1() for more information.
std::string getIssueString(const FullSourceLoc &IssueLoc,
                           llvm::StringRef CheckerName,
                           llvm::StringRef WarningMessage,
                           const Decl *IssueDecl, const LangOptions &LangOpts);

/// codeXek V2 issue hash (exported as the "codexek/issueHash/v2" SARIF
/// partial fingerprint, in parallel to the V1 hash).
///
/// Compared to V1, the absolute column number is replaced by the byte offset
/// of the issue's starting token inside the concatenation of the line's
/// tokens, and the warning message is dropped entirely. The hash therefore
/// survives reindentation, alignment changes and inline comment edits, and
/// stays stable when the engine's message wording changes.
///
/// Based on:
///   - Name of the checker that emitted the diagnostic.
///   - Signature of the enclosing declaration (same as V1).
///   - Byte offset of the issue's starting token within the line's
///     concatenated tokens (token-based, comments do not take up space).
///   - Contents of the line of code with the issue, with all whitespace
///     removed (same token walk as V1's NormalizeLine, single line).
llvm::SmallString<32> getIssueHashV2(const FullSourceLoc &IssueLoc,
                                     llvm::StringRef CheckerName,
                                     const Decl *IssueDecl,
                                     const LangOptions &LangOpts);

/// Get the unhashed string representation of the V2 issue hash.
/// Useful for testing.
std::string getIssueStringV2(const FullSourceLoc &IssueLoc,
                             llvm::StringRef CheckerName,
                             const Decl *IssueDecl,
                             const LangOptions &LangOpts);
} // namespace clang

#endif
