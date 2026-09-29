// RUN: rm -rf %t && mkdir -p %t
//
// End-to-end test of the codexek/issueHash/v2 SARIF partial fingerprint on
// the single-file (.sarif) output path. This file is the base input; the
// Inputs/ variants each change exactly one aspect of the defect and the
// assertions below encode the acceptance matrix:
//
//   drift / reindent / comment / undefmsg / paramname  -> v2 unchanged
//   varrename / fnrename / paramtype                    -> v2 changed
//   sameline (two defects on one line)                  -> v2 distinct
//   mixedcheckers (four rules on one line)              -> v2 distinct
//
// The v2 values below are the MD5s of the spec formula
//   checker + "$" + signature + "$" + relative-column + "$" + line
// (the base defect hashes "core.NullDereference$int deref(int)$6$return*p;")
// computed independently of the engine. The v1 values were captured from
// the engine before the v2 patch and pin v1 byte-for-byte; they also
// document v1's own sensitivities (reindentation, inline comment and the
// message change all alter v1).

// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/base.sarif %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/drift.sarif %S/Inputs/drift.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/reindent.sarif %S/Inputs/reindent.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/comment.sarif %S/Inputs/comment.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/varrename.sarif %S/Inputs/varrename.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/undefmsg.sarif %S/Inputs/undefmsg.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/paramname.sarif %S/Inputs/paramname.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/fnrename.sarif %S/Inputs/fnrename.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/paramtype.sarif %S/Inputs/paramtype.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/sameline.sarif %S/Inputs/sameline.c
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/mixedcheckers.sarif %S/Inputs/mixedcheckers.c

// v1 anchors: every result still carries the pre-patch v1 value.
// RUN: grep -q '"clang/issueHash/v1": "7076770f27b09888ab3069418deeb23f"' %t/base.sarif
// RUN: grep -q '"clang/issueHash/v1": "7076770f27b09888ab3069418deeb23f"' %t/drift.sarif
// RUN: grep -q '"clang/issueHash/v1": "7076770f27b09888ab3069418deeb23f"' %t/paramname.sarif
// RUN: grep -q '"clang/issueHash/v1": "74a48d798c7e8545e2bf94fe370b817f"' %t/reindent.sarif
// RUN: grep -q '"clang/issueHash/v1": "18c8c12a7e702e54f6a8a6b684997706"' %t/comment.sarif
// RUN: grep -q '"clang/issueHash/v1": "555db70dada7b41fb478a25fbec04fc8"' %t/undefmsg.sarif
// RUN: grep -q '"clang/issueHash/v1": "65cbea370d42c260d71bdcf15ab13ef0"' %t/varrename.sarif
// RUN: grep -q '"clang/issueHash/v1": "edccc39398adcc2c7ca6dbadee8c56d4"' %t/fnrename.sarif
// RUN: grep -q '"clang/issueHash/v1": "26bc917e9406ea311e3da157405b3965"' %t/paramtype.sarif
// RUN: grep -q '"clang/issueHash/v1": "4a6e5f71d6cb4264a50bac40209b3790"' %t/sameline.sarif
// RUN: grep -q '"clang/issueHash/v1": "522d01c279a94b720b594a4d3d9b666f"' %t/sameline.sarif
// RUN: grep -q '"clang/issueHash/v1": "3cc6ed3ab7fac02529fb82443875117f"' %t/mixedcheckers.sarif
// RUN: grep -q '"clang/issueHash/v1": "5951843339e756fd1b600ca54ea61f8d"' %t/mixedcheckers.sarif
// RUN: grep -q '"clang/issueHash/v1": "6fee1b8ea54e89e506e51f4cc31278dc"' %t/mixedcheckers.sarif
// RUN: grep -q '"clang/issueHash/v1": "ac4da177877a42244120c6d31b3381c0"' %t/mixedcheckers.sarif

// v2 anchors: every result carries the formula value alongside v1.
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/base.sarif
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/drift.sarif
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/reindent.sarif
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/comment.sarif
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/undefmsg.sarif
// RUN: grep -q '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"' %t/paramname.sarif
// RUN: grep -q '"codexek/issueHash/v2": "f9781e8025e56f766aab16f1c1a15965"' %t/varrename.sarif
// RUN: grep -q '"codexek/issueHash/v2": "3461c9084fd7701d44a486b97c69bbd8"' %t/fnrename.sarif
// RUN: grep -q '"codexek/issueHash/v2": "66fe312d8a54df1f27d9c4422e54e2f4"' %t/paramtype.sarif
// RUN: grep -q '"codexek/issueHash/v2": "1658847b2ae9e0745b78c67d73f83920"' %t/sameline.sarif
// RUN: grep -q '"codexek/issueHash/v2": "90fe4e2604015bdca8d3a3509daf4e59"' %t/sameline.sarif
// RUN: grep -q '"codexek/issueHash/v2": "5738bcd63320c8506172128c5a35962f"' %t/mixedcheckers.sarif
// RUN: grep -q '"codexek/issueHash/v2": "5f9f35ebcc3eb4e465198b2e16279926"' %t/mixedcheckers.sarif
// RUN: grep -q '"codexek/issueHash/v2": "e3737061550303e251a91654d2f4cd20"' %t/mixedcheckers.sarif
// RUN: grep -q '"codexek/issueHash/v2": "3e61b8e95a3fb55243f354afddb14662"' %t/mixedcheckers.sarif

// Exactly one v2 value across the six immunity variants (line drift,
// re-indentation, inline comment, message change, parameter rename).
// RUN: grep -oh '"codexek/issueHash/v2": "[0-9a-f]*"' %t/base.sarif %t/drift.sarif %t/reindent.sarif %t/comment.sarif %t/undefmsg.sarif %t/paramname.sarif | sort -u > %t/stable.v2
// RUN: printf '"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"\n' > %t/stable.expected
// RUN: diff %t/stable.expected %t/stable.v2

// Four distinct v2 values across base / token-content / function-name /
// parameter-type variants.
// RUN: grep -oh '"codexek/issueHash/v2": "[0-9a-f]*"' %t/base.sarif %t/varrename.sarif %t/fnrename.sarif %t/paramtype.sarif | sort -u > %t/changed.v2
// RUN: printf '"codexek/issueHash/v2": "3461c9084fd7701d44a486b97c69bbd8"\n"codexek/issueHash/v2": "66fe312d8a54df1f27d9c4422e54e2f4"\n"codexek/issueHash/v2": "a433c69be3b0aa73978a877617171a5c"\n"codexek/issueHash/v2": "f9781e8025e56f766aab16f1c1a15965"\n' > %t/changed.expected
// RUN: diff %t/changed.expected %t/changed.v2

// Two defects on one line: exactly two distinct v2 values, separated by the
// token-relative column (offsets 8 and 11 of "returnc?*a:*b;").
// RUN: grep -oh '"codexek/issueHash/v2": "[0-9a-f]*"' %t/sameline.sarif | sort -u > %t/sameline.v2
// RUN: printf '"codexek/issueHash/v2": "1658847b2ae9e0745b78c67d73f83920"\n"codexek/issueHash/v2": "90fe4e2604015bdca8d3a3509daf4e59"\n' > %t/sameline.expected
// RUN: diff %t/sameline.expected %t/sameline.v2

// Four different rules firing on the same line: four pairwise-distinct v2
// values (the checker name is a fingerprint component).
// RUN: grep -oh '"codexek/issueHash/v2": "[0-9a-f]*"' %t/mixedcheckers.sarif | sort -u > %t/mixed.v2
// RUN: printf '"codexek/issueHash/v2": "3e61b8e95a3fb55243f354afddb14662"\n"codexek/issueHash/v2": "5738bcd63320c8506172128c5a35962f"\n"codexek/issueHash/v2": "5f9f35ebcc3eb4e465198b2e16279926"\n"codexek/issueHash/v2": "e3737061550303e251a91654d2f4cd20"\n' > %t/mixed.expected
// RUN: diff %t/mixed.expected %t/mixed.v2

// The base defect itself: one null dereference of 'p'.
int deref(int unused) {
  int *p = 0;
  return *p;
}
