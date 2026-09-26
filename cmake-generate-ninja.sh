#!/bin/zsh
#rm -rf build-ninja
cmake -S llvm -B build-ninja -G Ninja \
-DCMAKE_OSX_ARCHITECTURES=arm64 \
-DCMAKE_BUILD_TYPE=Release \
-DLLVM_ENABLE_PROJECTS="clang;clang-tools-extra" \
-DLLVM_TARGETS_TO_BUILD="AArch64" \
-DLLVM_PARALLEL_COMPILE_JOBS=14 \
-DLLVM_PARALLEL_LINK_JOBS=2
#ninja -C build-ninja clang

