// Two defects on one line (guarded ternary branches so each dereference
// gets its own path): the token-relative column keeps them apart.
int pick(int c) {
  int *a = 0;
  int *b = 0;
  return c ? *a : *b;
}
