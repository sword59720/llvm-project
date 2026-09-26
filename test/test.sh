#!/bin/zsh
# 自定义静态分析器测试脚本。
# 用法: ./test/test.sh
# 可用 BUILD_DIR=<构建目录> 指定其他构建目录(默认 build-ninja)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ninja}"
TEST_DIR="$ROOT/test/Analysis"
CLANG="$BUILD_DIR/bin/clang"

echo "=== 1. lit 验证测试 (error-recovery.cpp / null-dereference-recovery.cpp) ==="
# 期望输出内嵌在文件注释里(expected-warning / recover-warning), PASS 即全部匹配。
# 加 -a 可显示每个测试实际执行的命令和 clang 的完整输出。
"$BUILD_DIR/bin/llvm-lit" "$TEST_DIR"

echo
echo "=== 2. playground: nullptr_test.cpp 文本报告 ==="
"$CLANG" -cc1 -analyze -setup-static-analyzer \
  -analyzer-checker=core -analyzer-config checker-error-recover=true \
  -analyzer-output=text "$TEST_DIR/nullptr_test.cpp"

echo
echo "=== 3. playground: 重新生成 plist 输出 ==="
"$CLANG" -cc1 -analyze -setup-static-analyzer \
  -analyzer-checker=core -analyzer-config checker-error-recover=true \
  -analyzer-output=plist -o "$TEST_DIR/nullptr_test.plist" "$TEST_DIR/nullptr_test.cpp"
echo "已写入 $TEST_DIR/nullptr_test.plist"

echo
echo "=== 4. lit 测试文件的路径报告演示 (two_null_accesses) ==="
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core -analyzer-config checker-error-recover=true \
  -analyze-function="two_null_accesses()" -analyzer-output=text \
  "$TEST_DIR/null-dereference-recovery.cpp"

echo
echo "=== 5. error-recovery.cpp: 混合错误恢复路径报告 (mixed_errors) ==="
# 除零 -> 未初始化值 -> 空指针解引用, 同一路径连续报告并继续探索
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core,unix.Malloc,debug.ExprInspection \
  -analyzer-config checker-error-recover=true \
  -analyze-function="mixed_errors(int)" -analyzer-output=text \
  "$TEST_DIR/error-recovery.cpp"

echo
echo "=== 6. error-recovery.cpp: double free 后继续发现空指针解引用 ==="
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core,unix.Malloc,debug.ExprInspection \
  -analyzer-config checker-error-recover=true \
  -analyze-function="double_free_then_null_access()" -analyzer-output=text \
  "$TEST_DIR/error-recovery.cpp"
