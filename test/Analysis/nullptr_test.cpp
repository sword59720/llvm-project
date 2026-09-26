void test() {
  int *p = nullptr;
  int x = *p;       // 错误 1：空指针解引用

  int *q = nullptr;
  *q = 1;           // 错误 2：希望继续发现
}
