// An inline comment is not a token: the token-relative column of the
// dereference and the line content are unchanged; v1 column shifts.
int deref(int unused) {
  int *p = 0;
  return /* inline note */ *p;
}
