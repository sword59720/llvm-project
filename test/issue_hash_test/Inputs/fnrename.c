// Renaming the enclosing function changes the signature: fingerprint
// must change.
int deref_renamed(int unused) {
  int *p = 0;
  return *p;
}
