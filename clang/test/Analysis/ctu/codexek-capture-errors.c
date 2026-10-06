// codeXek dependency-capture protocol v1 — ERROR paths (20261006 review
// CR01/CR02). A zero-exit analysis is not proof of a complete capture:
//  - CR02: a FAILED index query (corrupt page, invalid row) must surface as
//    a "query-error" event with a capture-end query_errors tally — it must
//    never masquerade as a plain "missing" (only SQLITE_DONE with zero rows
//    is a genuine miss; that control lives in codexek-capture-protocol.c).
//  - CR01: a definition import that fails AFTER a successful index hit and
//    AST load (ASTImporter name conflict / unsupported construct, or one of
//    the triple/language gates) must surface as an "import-failed" event
//    with an import_errors tally — the result used less information than
//    its dependency set claims.
// The product refuses fine reuse of captures whose end marker reports
// either error count above zero.
//
// RUN: rm -rf %t && mkdir -p %t/ctudir
// RUN: %clang_cc1 -emit-pch -o %t/ctudir/import.c.ast %S/Inputs/import.c
// RUN: %python %S/Inputs/mkctu.py --blob-source %S/Inputs/import.c.externalDefMap.ast-dump.txt %t/ctudir/import.c.ast %t/ctudir/blob.db
// RUN: %clang_analyze_cc1 -analyzer-checker=core \
// RUN:   -analyzer-config experimental-enable-naive-ctu-analysis=true \
// RUN:   -analyzer-config ctu-dir=%t/ctudir -analyzer-config ctu-index-name=blob.db \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=BLOB
//
// BLOB: codexek-ctu: {"kind":"query-error"
// BLOB-SAME: "usr":"c:@F@testStaticImplicit"
// BLOB: codexek-ctu: {"ast_errors":0,"ast_hits":0,"ast_loads":0,"degraded":0,"import_errors":0,"imported":0,"index_errors":0,"index_loaded":true,"kept":0,"kind":"capture-end","missing":0,"queries":{{[0-9]+}},"query_errors":1,"resolved":0,"suppressed":0,"v":1}
// BLOB-NOT: codexek-ctu
//
// Real import failure: the upstream test-import-failure fixture fails the
// ASTImporter with a name conflict after a successful index hit + AST load.
// RUN: %clang_cc1 -triple x86_64-pc-linux-gnu -std=c++17 -emit-pch \
// RUN:   -o %t/ctudir/test-import-failure-import.cpp.ast %S/Inputs/test-import-failure-import.cpp
// RUN: %python %S/Inputs/mkctu.py %S/Inputs/test-import-failure-import.cpp.externalDefMap.ast-dump.txt %t/ctudir/test-import-failure-import.cpp.ast %t/ctudir/import-failure.db
// RUN: %clang_analyze_cc1 -triple x86_64-pc-linux-gnu -std=c++17 \
// RUN:   -analyzer-checker=core \
// RUN:   -analyzer-config experimental-enable-naive-ctu-analysis=true \
// RUN:   -analyzer-config ctu-dir=%t/ctudir -analyzer-config ctu-index-name=import-failure.db \
// RUN:   %S/test-import-failure.cpp 2>&1 | FileCheck %s --check-prefix=IMPFAIL
//
// IMPFAIL: codexek-ctu: {"kind":"import-failed"
// IMPFAIL-SAME: "reason":"name-conflict"
// IMPFAIL: codexek-ctu: {"ast_errors":0,"ast_hits":{{[0-9]+}},"ast_loads":1,"degraded":0,"import_errors":1,"imported":{{[0-9]+}},"index_errors":0,"index_loaded":true,"kept":0,"kind":"capture-end","missing":0,"queries":{{[0-9]+}},"query_errors":0,"resolved":0,"suppressed":0,"v":1}
// IMPFAIL-NOT: codexek-ctu

int testStaticImplicit(void);
int func(void) {
  return testStaticImplicit();
}
