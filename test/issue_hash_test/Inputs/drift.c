// Lines added above the defect must not change the fingerprint.
int pad_one(void) { return 1; }
int pad_two(void) { return 2; }
int deref(int unused) {
  int *p = 0;
  return *p;
}
