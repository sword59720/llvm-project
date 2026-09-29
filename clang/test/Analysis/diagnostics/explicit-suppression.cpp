// codeXek fork: checker-error-recover defaults to true in this fork; the
// unexpected cascade diagnostics after a recovered error are asserted in
// test/checker_recovery_test/, while these upstream expectations pin the
// upstream sink semantics, so the analysis invocations pin the upstream
// default explicitly.
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -analyzer-config suppress-c++-stdlib=false,checker-error-recover=false -verify %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -analyzer-config suppress-c++-stdlib=true,checker-error-recover=false -DSUPPRESSED=1 -verify %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core,debug.ExprInspection -analyzer-config checker-error-recover=false -DSUPPRESSED=1 -verify %s

#ifdef SUPPRESSED
// expected-no-diagnostics
#endif

#include "../Inputs/system-header-simulator-cxx.h"

void clang_analyzer_eval(bool);

class C {
  // The virtual function is to make C not trivially copy assignable so that we call the
  // variant of std::copy() that does not defer to memmove().
  virtual int f();
};

void testCopyNull(C *I, C *E) {
  std::copy(I, E, (C *)0);
#ifndef SUPPRESSED
  // expected-warning@#system_header_simulator_cxx_std_copy_impl_loop {{Called C++ object pointer is null}}
#endif
}
