// Same checker, same signature, same defect-line tokens as the base file,
// but a different bug type message (undefined vs null pointer). v2 has no
// message component and must stay identical; v1 changes.
int deref(int unused) {
  int *p;
  return *p;
}
