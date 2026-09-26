#!/bin/zsh
# checker-error-recover 功能测试脚本。
# 用法: ./test/test.sh
# 可用 BUILD_DIR=<构建目录> 指定其他构建目录(默认 build-ninja)。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build-ninja}"
TEST_DIR="$ROOT/test/checker_recovery_test"
CLANG="$BUILD_DIR/bin/clang"

echo "=== 1. lit 验证测试 (error-recovery.cpp / null-dereference-recovery.cpp) ==="
# 期望输出内嵌在文件注释里(expected-warning / recover-warning), PASS 即全部匹配。
# 加 -a 可显示每个测试实际执行的命令和 clang 的完整输出。
"$BUILD_DIR/bin/llvm-lit" "$TEST_DIR"

echo
echo "=== 2. null-dereference-recovery.cpp 全文件文本报告 (恢复开启) ==="
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core,debug.ExprInspection \
  -analyzer-config checker-error-recover=true \
  -analyzer-output=text "$TEST_DIR/null-dereference-recovery.cpp"

echo
echo "=== 3. error-recovery.cpp 全文件文本报告 (恢复开启) ==="
# 同一路径上混合错误连续报告: 除零 -> 未初始化值 -> 空指针解引用 -> double free 等
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core,unix.Malloc,debug.ExprInspection \
  -analyzer-config checker-error-recover=true \
  -analyzer-output=text "$TEST_DIR/error-recovery.cpp"

echo
echo "=== 4. error-recovery.cpp 全文件文本报告 (恢复关闭, 对比) ==="
# 恢复关闭时错误节点是 sink, 每条路径只报告第一个错误
"$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
  -analyzer-checker=core,unix.Malloc,debug.ExprInspection \
  -analyzer-config checker-error-recover=false \
  -analyzer-output=text "$TEST_DIR/error-recovery.cpp"

echo
echo "=== 5. 生成 plist 报告到 Output 目录 ==="
# Output/ 是 lit 的临时目录(%t/%T 展开于此), lit 发现测试时会跳过, 适合存放生成产物
mkdir -p "$TEST_DIR/Output"
for f in error-recovery null-dereference-recovery; do
  "$CLANG" -cc1 -analyze -setup-static-analyzer -std=c++11 \
    -analyzer-checker=core,unix.Malloc,debug.ExprInspection \
    -analyzer-config checker-error-recover=true \
    -analyzer-output=plist -o "$TEST_DIR/Output/$f.plist" "$TEST_DIR/$f.cpp"
  echo "已写入 $TEST_DIR/Output/$f.plist"
done
