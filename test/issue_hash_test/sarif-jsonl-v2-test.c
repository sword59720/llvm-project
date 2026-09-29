// RUN: rm -rf %t && mkdir -p %t
//
// The fork's streaming path (output file ending in .jsonl: one SARIF
// document per line) must carry the codexek/issueHash/v2 partial
// fingerprint exactly like the single-file path: every record has v1 and
// v2 side by side, and the v2 values are identical to the single-file
// output for the same input. This input has two guarded null dereferences
// on one line, so the streaming file has two records with distinct v2
// values (token offsets 8 and 11 of "returnc?*a:*b;").

// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/stream.jsonl %s
// RUN: %clang_analyze_cc1 -analyzer-checker=core -analyzer-output=sarif -o %t/single.sarif %s

// One JSON document per record, two records.
// RUN: printf '2\n' > %t/two.lines
// RUN: grep -c '' %t/stream.jsonl | diff %t/two.lines -

// Both fingerprints on every record (compact JSON: no space after ':').
// RUN: sed -n 's/.*"clang\/issueHash\/v1":"\([0-9a-f]*\)".*/\1/p' %t/stream.jsonl | sort > %t/stream.v1
// RUN: printf '4a6e5f71d6cb4264a50bac40209b3790\n522d01c279a94b720b594a4d3d9b666f\n' > %t/stream.v1.expected
// RUN: diff %t/stream.v1.expected %t/stream.v1
// RUN: sed -n 's/.*"codexek\/issueHash\/v2":"\([0-9a-f]*\)".*/\1/p' %t/stream.jsonl | sort > %t/stream.v2
// RUN: printf '1658847b2ae9e0745b78c67d73f83920\n90fe4e2604015bdca8d3a3509daf4e59\n' > %t/stream.v2.expected
// RUN: diff %t/stream.v2.expected %t/stream.v2

// The v2 values are the same as the single-file path's.
// RUN: sed -n 's/.*"codexek\/issueHash\/v2": "\([0-9a-f]*\)".*/\1/p' %t/single.sarif | sort > %t/single.v2
// RUN: diff %t/stream.v2 %t/single.v2

int pick(int c) {
  int *a = 0;
  int *b = 0;
  return c ? *a : *b;
}
