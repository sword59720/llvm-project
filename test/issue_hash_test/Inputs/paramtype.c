// Changing a parameter type changes the signature: fingerprint must
// change.
int deref(long unused) {
  int *p = 0;
  return *p;
}
