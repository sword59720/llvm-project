// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,unix.Malloc,debug.ExprInspection -verify=expected,recover %s
// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,unix.Malloc,debug.ExprInspection -analyzer-config checker-error-recover=false -verify=expected,default %s
// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,unix.Malloc,debug.ExprInspection -analyzer-config checker-error-recover=true -verify=expected,recover %s

void clang_analyzer_warnIfReached();
extern "C" void *malloc(__SIZE_TYPE__);
extern "C" void free(void *);

// A single option controls errors from multiple checker families along a path.
void mixed_errors(int zero) {
  if (zero)
    return;
  (void)(42 / zero); // expected-warning{{Division by zero}}
                    // recover-warning@-1{{The result of the '/' expression is undefined}}

  int uninitialized;
  int value = uninitialized; // recover-warning{{Assigned value is uninitialized}}
  (void)value;

  int *p = nullptr;
  *p = 1; // recover-warning{{Dereference of null pointer}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

void repeated_division(int zero) {
  if (zero)
    return;
  (void)(1 / zero); // expected-warning{{Division by zero}}
                   // recover-warning@-1{{The result of the '/' expression is undefined}}
  (void)(2 / zero); // recover-warning{{Division by zero}}
                   // recover-warning@-1{{The result of the '/' expression is undefined}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

void double_free_then_null_access() {
  void *allocation = malloc(4);
  if (!allocation)
    return;
  free(allocation);
  free(allocation); // expected-warning{{Attempt to release already released memory}}
  int *p = nullptr;
  *p = 1; // recover-warning{{Dereference of null pointer}}
  clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

void undefined_condition() {
  bool condition;
  if (condition) // expected-warning{{Branch condition evaluates to a garbage value}}
    clang_analyzer_warnIfReached(); // recover-warning{{REACHABLE}}
}

// Recovery must not revive paths terminated for reasons other than reports.
void stop() __attribute__((noreturn));
void nonreturning_call() {
  stop();
  clang_analyzer_warnIfReached();
}

void infeasible_path(int value) {
  __builtin_assume(value == 0);
  if (value != 0)
    clang_analyzer_warnIfReached();
}
