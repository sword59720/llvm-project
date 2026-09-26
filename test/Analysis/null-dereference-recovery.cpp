// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,debug.ExprInspection -analyzer-config checker-error-recover=false -verify=expected,default %s
// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,debug.ExprInspection -analyzer-config checker-error-recover=true,eagerly-assume=false -verify=expected,recover %s
// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core -analyzer-config checker-error-recover=true -analyze-function="two_null_accesses()" -analyzer-output=text %s 2>&1 | FileCheck %s --check-prefix=PATH

void clang_analyzer_eval(bool);
void clang_analyzer_warnIfReached();

void two_null_accesses() {
  int *p = nullptr;
  int x = *p; // expected-warning{{Dereference of null pointer}}
  int *q = nullptr;
  *q = 1; // recover-warning{{Dereference of null pointer}}
}

// PATH: warning: Dereference of null pointer (loaded from variable 'p')
// PATH: warning: Dereference of null pointer (loaded from variable 'q')
// PATH: note: Analysis continued after this null pointer dereference using an unknown value for a load or skipping a store
// PATH: note: Dereference of null pointer (loaded from variable 'q')

void stores_preserve_other_memory() {
  int value = 42;
  int *p = nullptr;
  *p = 1; // expected-warning{{Dereference of null pointer}}
  *p = 2; // recover-warning{{Dereference of null pointer}}
  clang_analyzer_eval(value == 42); // recover-warning{{TRUE}}
  value = 7;
  clang_analyzer_eval(value == 7); // recover-warning{{TRUE}}
}

void loads_produce_unknown_values() {
  int *p = nullptr;
  int x = *p; // expected-warning{{Dereference of null pointer}}
  clang_analyzer_eval(x == 0); // recover-warning{{UNKNOWN}}
  int y = *p; // recover-warning{{Dereference of null pointer}}
  clang_analyzer_eval(y == 0); // recover-warning{{UNKNOWN}}
  // The recovery marker must not affect a subsequent valid load.
  int value = 42;
  int *q = &value;
  clang_analyzer_eval(*q == 42); // recover-warning{{TRUE}}
}

void constrained_null_pointer(int *p) {
  if (p)
    return;
  int x = *p; // expected-warning{{Dereference of null pointer}}
  *p = 1; // recover-warning{{Dereference of null pointer}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

void both_branches_after_recovery(bool condition) {
  int *p = nullptr;
  *p = 1; // expected-warning{{Dereference of null pointer}}
  int *q = nullptr;
  if (condition)
    *q = 2; // recover-warning{{Dereference of null pointer}}
  else
    *q = 3; // recover-warning{{Dereference of null pointer}}
}

// A non-fatal error node alone cannot recover an undefined store destination.
void undefined_store_still_stops_in_engine() {
  int *p;
  *p = 1; // expected-warning{{Dereference of undefined pointer value}}
  clang_analyzer_warnIfReached();
}

void null_reference_binding_recovers() {
  int *p = nullptr;
  int &r = *p; // expected-warning{{Dereference of null pointer}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

void normal_accesses(int *p) {
  if (!p)
    return;
  *p = 42;
  clang_analyzer_eval(*p == 42); // expected-warning{{TRUE}}
}
