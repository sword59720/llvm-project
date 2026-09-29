// Re-indentation and in-line alignment spaces must not change the
// fingerprint; v1 (absolute column) is expected to change.
int deref(int unused) {
  int *p = 0;
          return     *   p   ;
}
