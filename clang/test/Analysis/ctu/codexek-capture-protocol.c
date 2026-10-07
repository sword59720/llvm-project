// codeXek N03 dependency-capture protocol v1: structured single-line JSON
// events on stderr, emitted ONLY when a SQLite index is attempted — the
// product's sole configuration. This test pins:
//  - the full event chain of a successful import: capture-begin (index
//    identity), imported (consumed AST), ast-load ok, and capture-end from
//    REAL consumer teardown carrying the EMITTED-event tally
//  - a missing-definition query reported once (per-process dedup)
//  - silence outside the SQLite path: text-index and no-CTU analyses keep
//    upstream stderr byte-identical
// A killed or crashed engine never reaches teardown, so the absence of
// capture-end means an incomplete capture (verified by the product's
// integration scenarios; killing is not lit-reproducible).
//
// RUN: rm -rf %t && mkdir -p %t/ctudir
// RUN: %clang_cc1 -emit-pch -o %t/ctudir/import.c.ast %S/Inputs/import.c
// RUN: %python %S/Inputs/mkctu.py %S/Inputs/import.c.externalDefMap.ast-dump.txt %t/ctudir/import.c.ast %t/ctudir/index.db
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection \
// RUN:   -analyzer-config eagerly-assume=false \
// RUN:   -analyzer-config experimental-enable-naive-ctu-analysis=true \
// RUN:   -analyzer-config ctu-dir=%t/ctudir -analyzer-config ctu-index-name=index.db \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=SQL
//
// Text index: same analysis shape, no protocol output at all.
// RUN: cp %S/Inputs/import.c.externalDefMap.ast-dump.txt %t/ctudir/externalDefMap.txt
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection \
// RUN:   -analyzer-config eagerly-assume=false \
// RUN:   -analyzer-config experimental-enable-naive-ctu-analysis=true \
// RUN:   -analyzer-config ctu-dir=%t/ctudir \
// RUN:   %s 2>&1 | FileCheck %s --check-prefix=SILENT
//
// No CTU: nothing to capture, nothing emitted.
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection \
// RUN:   -analyzer-config eagerly-assume=false %s 2>&1 \
// RUN:   | FileCheck %s --check-prefix=SILENT
//
// SILENT-NOT: codexek-ctu
//
// S1-R02: a CONFIGURED .db index emits an explicit zero-counter end marker
// even when zero queries ever opened it — the consumer must be able to
// tell "no cross-TU use" apart from "protocol output lost".
// RUN: echo 'int local_only(void){return 1;}' > %t/nocall.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core \
// RUN:   -analyzer-config experimental-enable-naive-ctu-analysis=true \
// RUN:   -analyzer-config ctu-dir=%t/ctudir -analyzer-config ctu-index-name=unused.db \
// RUN:   %t/nocall.c 2>&1 | FileCheck %s --check-prefix=DBZERO
//
// DBZERO: codexek-ctu: {"ast_errors":0,"ast_hits":0,"ast_loads":0,"body_gates":{{[0-9]+}},"import_errors":0,"index_errors":0,"index_loaded":false,"kind":"capture-end","query_errors":0,"v":1}
// DBZERO-NOT: codexek-ctu
//
// SQL: codexek-ctu: {"index":"{{.+}}index.db","kind":"capture-begin","namespace":"lit","v":1}
// SQL: codexek-ctu: {"ast":"{{.*}}import.c.ast","caller":"{{.*}}","kind":"imported","usr":"{{.+}}","v":1}
// SQL: codexek-ctu: {"ast":"{{.*}}import.c.ast","kind":"ast-load","result":"ok","v":1}
// SQL: codexek-ctu: {"caller":"{{.*}}","kind":"missing","usr":"c:@F@test_missing_definition","v":1}
// SQL: codexek-ctu: {"ast_errors":0,"ast_hits":{{[0-9]+}},"ast_loads":1,"body_gates":{{[0-9]+}},"degraded":0,"import_errors":0,"imported":1,"index_errors":0,"index_loaded":true,"kept":0,"kind":"capture-end","missing":1,"queries":{{[0-9]+}},"query_errors":0,"resolved":0,"suppressed":0,"v":1}
// SQL-NOT: codexek-ctu

void clang_analyzer_eval(int);

int testStaticImplicit(void);
int test_missing_definition(void);
int func(void) {
  int ret = testStaticImplicit();
  clang_analyzer_eval(ret == 4); // expected-warning{{TRUE}} ctu
                                 // expected-warning@-1{{UNKNOWN}} stu
  (void)test_missing_definition(); // ctu-miss: reported once by the protocol
  return testStaticImplicit();
}
