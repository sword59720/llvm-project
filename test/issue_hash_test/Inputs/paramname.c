// Parameter names are not part of the signature (types only): the
// fingerprint must not change.
int deref(int renamed) {
  int *p = 0;
  return *p;
}
