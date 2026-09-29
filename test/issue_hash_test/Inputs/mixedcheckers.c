// Same line, different checkers (error recovery keeps the path alive):
// all four findings must get pairwise-distinct v2 fingerprints.
int both(int c) {
  int *q = 0;
  return c ? 1 / 0 : *q;
}
