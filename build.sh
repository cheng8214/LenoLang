#!/bin/bash
set -e

echo "Building LenoLang Compiler..."

mkdir -p build

# ---------------------------------------------------------------------------
# 源清单与 Windows 脚本**共用同一份**（单一事实来源；P9 的根治）：
#   sources_core.txt      —— 核心运行时（VM + 编译器都用）
#   sources_compiler.txt  —— 编译器专属（lexer/parser/semantic/codegen/package）
#   sources_vm.txt        —— VM 专属入口（vm_main.c；与 src/main.c 互斥）
# ⚠ **不要再往本文件里逐个加源文件**：改清单文件，否则两边会漂移 ——
#   历史上正是这样漏掉 `src/debug.c`（`opcode_name()` 的唯一定义点）与
#   `parser_eval_const_expr_text` 的，VM-only 构建直接链不过 ✗。
#   清单里写的是 Windows 风格的 `\` ⇒ 这里换成 `/`；顺带去掉 CR（清单可能 CRLF）。
# ---------------------------------------------------------------------------
read_list() {
  tr -d '\r' < "$1" | grep -v '^[[:space:]]*$' | tr '\\' '/'
}

SOURCES=""
for f in $(read_list sources_core.txt) $(read_list sources_compiler.txt); do
  SOURCES="$SOURCES $f"
done

# 平台检测
OS="$(uname -s 2>/dev/null || echo unknown)"
case "$OS" in
  MINGW*|MSYS*|CYGWIN*)  PLATFORM=windows ;;
  Darwin*)               PLATFORM=macos   ;;
  *)                     PLATFORM=linux   ;;
esac

EXE=""
LIBS="-lm"
if [ "$PLATFORM" = "windows" ]; then
  SOURCES="$SOURCES src/module/ffi/leno_ffi_win64.c"
  LIBS="$LIBS -municode -lws2_32"
  EXE=".exe"
elif [ "$PLATFORM" = "macos" ]; then
  # 检测架构：arm64 (AAPCS64) vs x86_64 (System V AMD64)
  ARCH="$(uname -m 2>/dev/null || echo x86_64)"
  if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
    SOURCES="$SOURCES src/module/ffi/leno_ffi_arm64.c"
  else
    SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  fi
  LIBS="$LIBS -lpthread -ldl"
else
  # Linux: 检测架构
  ARCH="$(uname -m 2>/dev/null || echo x86_64)"
  if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    SOURCES="$SOURCES src/module/ffi/leno_ffi_arm64.c"
  else
    SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  fi
  LIBS="$LIBS -lpthread -ldl"
  CFLAGS="$CFLAGS -D_GNU_SOURCE"
fi

CC=${CC:-gcc}

$CC $CFLAGS -o build/lenoreg$EXE $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 $LIBS

echo "Build successful"
echo ""
echo "Usage: build/lenoreg$EXE <file.leno>"
echo "Tests:  build/lenoreg$EXE assert/run_tests.leno build/lenoreg$EXE assert"
