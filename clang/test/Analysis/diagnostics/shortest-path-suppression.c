// codeXek fork: checker-error-recover defaults to true in this fork; the
// unexpected cascade diagnostics after a recovered error are asserted in
// test/checker_recovery_test/, while these upstream expectations pin the
// upstream sink semantics, so the analysis invocations pin the upstream
// default explicitly.
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-config suppress-null-return-paths=true,checker-error-recover=false -analyzer-output=text -verify %s
// expected-no-diagnostics

int *returnNull(void) { return 0; }
int coin(void);

// Use a float parameter to ensure that the value is unknown. This will create
// a cycle in the generated ExplodedGraph.
void testCycle(float i) {
  int *x = returnNull();
  int y; 
  while (i > 0) {
    x = returnNull();
    y = 2;
    i -= 1;
  }
  *x = 1; // no-warning
  y += 1;
}
