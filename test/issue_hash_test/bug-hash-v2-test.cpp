// RUN: %clang_analyze_cc1 -std=c++11 -analyzer-checker=core,debug.ExprInspection %s -verify
//
// Exact-string test for codexek/issueHash/v2 (mirrors upstream
// bug_hash_test.cpp for v1): clang_analyzer_hashDumpV2 reports the unhashed
// v2 string  checker $ enclosing signature $ token-relative column $ line ,
// where the relative column is the byte offset of the argument's starting
// token inside the concatenation of the line's tokens. The dump line
//   clang_analyzer_hashDumpV2(5);
// tokenizes to  clang_analyzer_hashDumpV2 ( 5 ) ;  so the argument token sits
// at offset 26 (25-char identifier plus '('). Unlike v1 there is no warning
// message component and no absolute column.

constexpr int clang_analyzer_hashDumpV2(int) { return 5; }

void function(int) {
  clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void function(int)$26$clang_analyzer_hashDumpV2(5);}}
}

namespace {
void variadicParam(int, ...) {
  clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void (anonymous namespace)::variadicParam(int, ...)$26$clang_analyzer_hashDumpV2(5);}}
}
} // namespace

constexpr int f() {
  return clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$int f()$32$returnclang_analyzer_hashDumpV2(5);}}
}

namespace AA {
class X {
  X() {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$AA::X::X()$26$clang_analyzer_hashDumpV2(5);}}
  }

  static void static_method() {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void AA::X::static_method()$26$clang_analyzer_hashDumpV2(5);}}
    variadicParam(5);
  }

  void method() && {
    struct Y {
      inline void method() const & {
        clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void AA::X::method()::Y::method() const &$26$clang_analyzer_hashDumpV2(5);}}
      }
    };

    Y y;
    y.method();

    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void AA::X::method() &&$26$clang_analyzer_hashDumpV2(5);}}
  }

  void OutOfLine();

  X &operator=(int) {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$X & AA::X::operator=(int)$26$clang_analyzer_hashDumpV2(5);}}
    return *this;
  }

  operator int() {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$AA::X::operator int()$26$clang_analyzer_hashDumpV2(5);}}
    return 0;
  }

  explicit operator float() {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$AA::X::operator float()$26$clang_analyzer_hashDumpV2(5);}}
    return 0;
  }
};
} // namespace AA

void AA::X::OutOfLine() {
  clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void AA::X::OutOfLine()$26$clang_analyzer_hashDumpV2(5);}}
}

void testLambda() {
  []() {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void testLambda()::(lambda)::operator()() const$26$clang_analyzer_hashDumpV2(5);}}
  }();
}

// Two defects on one line must differ by the token-relative column: the
// argument tokens sit at offsets 26 and 55 of the concatenated line.
void sameLineTwoDefects() {
  clang_analyzer_hashDumpV2(1); clang_analyzer_hashDumpV2(2); // expected-warning {{debug.ExprInspection$void sameLineTwoDefects()$26$clang_analyzer_hashDumpV2(1);clang_analyzer_hashDumpV2(2);}} expected-warning {{debug.ExprInspection$void sameLineTwoDefects()$55$clang_analyzer_hashDumpV2(1);clang_analyzer_hashDumpV2(2);}}
}

// Whitespace-only edits keep both the relative column and the line content:
// leading indentation, alignment spaces around tokens and parens, and inline
// comments (not tokens) leave the v2 string untouched.
void whitespaceInsensitivity() {
        clang_analyzer_hashDumpV2(5);
  clang_analyzer_hashDumpV2(   5  ); /* trailing note */
  clang_analyzer_hashDumpV2 ( 5 ) ;
  // expected-warning@-3 {{debug.ExprInspection$void whitespaceInsensitivity()$26$clang_analyzer_hashDumpV2(5);}}
  // expected-warning@-3 {{debug.ExprInspection$void whitespaceInsensitivity()$26$clang_analyzer_hashDumpV2(5);}}
  // expected-warning@-3 {{debug.ExprInspection$void whitespaceInsensitivity()$26$clang_analyzer_hashDumpV2(5);}}
}

// A multi-token argument: the issue token is the argument's first token.
void argumentExpression() {
  clang_analyzer_hashDumpV2(1 + 2); // expected-warning {{debug.ExprInspection$void argumentExpression()$26$clang_analyzer_hashDumpV2(1+2);}}
}

// Changing the token content of the line changes the v2 string.
void contentChange() {
  clang_analyzer_hashDumpV2(6); // expected-warning {{debug.ExprInspection$void contentChange()$26$clang_analyzer_hashDumpV2(6);}}
}

// Templates hash in their primary-template form: instantiations of the same
// template share the identity, explicit specializations do not.
template <typename T>
void f(T) {
  clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void f(T)$26$clang_analyzer_hashDumpV2(5);}}
}

template <typename T>
struct TX {
  void f(T) {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void TX::f(T)$26$clang_analyzer_hashDumpV2(5);}}
  }
};

template <>
void f<long>(long) {
  clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void f(long)$26$clang_analyzer_hashDumpV2(5);}}
}

template <>
struct TX<long> {
  void f(long) {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void TX<long>::f(long)$26$clang_analyzer_hashDumpV2(5);}}
  }
};

template <typename T>
struct TTX {
  template<typename S>
  void f(T, S) {
    clang_analyzer_hashDumpV2(5); // expected-warning {{debug.ExprInspection$void TTX::f(T, S)$26$clang_analyzer_hashDumpV2(5);}}
  }
};

void g() {
  // TX<int> and TX<double> are instantiated from the same code with the same
  // source locations; the v2 hash must not split their identity (no template
  // arguments in the signature). Explicit specializations keep their own.
  TX<int> x;
  TX<double> y;
  TX<long> xl;
  x.f(1);
  xl.f(1);
  f(5);
  f(3.0);
  y.f(2);
  TTX<int> z;
  z.f<int>(5, 5);
  f(5l);
}
