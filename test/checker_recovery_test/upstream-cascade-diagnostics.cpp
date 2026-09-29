// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,cplusplus,debug.ExprInspection -analyzer-config checker-error-recover=false,suppress-null-return-paths=true -verify=expected,default %s
// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,cplusplus,debug.ExprInspection -analyzer-config checker-error-recover=true,suppress-null-return-paths=true -verify=expected,recover %s
//
// Recovery cascade expectations distilled from the upstream
// clang/test/Analysis/diagnostics/ tests whose analysis invocations now pin
// checker-error-recover=false (ED01-ED09). Each upstream test keeps its
// upstream sink-semantics expectations; this file asserts that with
// recovery enabled the same scenarios produce the follow-on diagnostics,
// so the cascades are intentional rather than accidental.

void clang_analyzer_warnIfReached();

// ED01 (dtors.cpp): after a recovered use-after-release the path continues
// and the next access of the released memory reports again.
struct S {
  void foo();
};

void use_after_release_continues(S *p) {
  p->foo();
  delete p;
  p->foo(); // expected-warning{{Use of memory after it is released}}
  p->foo(); // recover-warning{{Use of memory after it is released}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

// ED02 (explicit-suppression.cpp): a null dereference that binds a
// reference recovers; returning that reference reports a null reference.
struct C {};

C &null_reference_through_recovery() {
  C *p = nullptr;
  C &r = *p; // expected-warning{{Dereference of null pointer}}
  return r; // recover-warning{{Returning null reference}}
}

// ED03/ED04/ED07/ED09 family: a recovered division by zero yields an
// undefined result; propagating it to the return value reports undefined
// garbage returned to the caller.
int division_zero_return_undefined(int zero) {
  if (zero)
    return 0;
  return 1 / zero; // expected-warning{{Division by zero}}
                   // recover-warning@-1{{The result of the '/' expression is undefined}}
                   // recover-warning@-2{{Undefined or garbage value returned to caller}}
}

// ED08 (shortest-path-suppression.c): the null store through a
// null-return-path-suppressed pointer stays unreported (the suppression
// holds), but recovery skips the store and continues, exposing the
// genuinely uninitialized loop-skipped local downstream.
int *returnNull(void) { return 0; }

void suppressed_store_skips_and_continues(float i) {
  int *x = returnNull();
  int y;
  while (i > 0) {
    x = returnNull();
    y = 2;
    i -= 1;
  }
  *x = 1; // no-warning: suppressed null-return-path report
  y += 1; // recover-warning{{The left expression of the compound assignment uses uninitialized memory}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}
