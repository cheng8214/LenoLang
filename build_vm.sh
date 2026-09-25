#!/bin/bash
set -e

echo "Building LenoLang VM Runtime (no compiler)..."

mkdir -p build

# ---------------------------------------------------------------------------
# 源清单与 build.sh / build_vm.bat **共用同一份**（单一事实来源）：
#   sources_core.txt  —— 核心运行时（VM 与编译器共用）
#   sources_vm.txt    —— VM 专属入口（vm_main.c；与 src/main.c 互斥）
# 注意：不包含各平台专属的 ffi 实现文件（win64/linux/arm64），在下方按平台追加。
# ⚠ 不要再往本文件里逐个加源文件：改清单文件，否则各构建路径必然漂移 ——
#   本文件此前抄的是一份**过期的手写清单**，漏了 src/dce.c，导致 VM-only 构建
#   链接失败（undefined reference to `dce_active` / `dce_enabled` /
#   `dce_set_active` / `dce_func_is_live` / `dce_note_module_cached` —— 引用方是
#   module_loader.c 与 serialize.c）；另外还漏了 src/debug.c 与
#   src/platform/platform_path.c。改用共享清单后与 build_vm.bat 完全一致 ✓。
#   （历史上还曾引用过 src/object/object_{draw,window,event,image,font}.c 与
#     src/module/guis/guis_*.c，这些文件/目录已不存在。）
# ---------------------------------------------------------------------------
read_list() {
  tr -d '\r' < "$1" | grep -v '^[[:space:]]*$' | tr '\\' '/'
}

SOURCES=""
for f in $(read_list sources_core.txt) $(read_list sources_vm.txt); do
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

# debug.c 仅包含反汇编函数，VM 运行时不需要，通过 LENO_VM_ONLY 条件编译排除
# -s: 剥离符号表和调试信息，避免暴露函数名/变量名/类型结构
#      如需调试 VM 本身，去掉 -s 重新 build_vm.sh 即可
$CC $CFLAGS -o build/leno_vm$EXE $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY $LIBS

echo "VM build successful: build/leno_vm$EXE"

# Windows: 额外构建无控制台版 leno_vm_gui.exe（-mwindows，GUI 打包用）
if [ "$PLATFORM" = "windows" ]; then
  $CC $CFLAGS -o build/leno_vm_gui.exe $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY $LIBS -mwindows
  if [ $? -ne 0 ]; then
    echo "VM GUI build failed"
    exit 1
  fi
  echo "VM GUI build successful: build/leno_vm_gui.exe (no console)"
fi

echo ""
echo "Usage: build/leno_vm$EXE <file.lenb>"
echo "Self:  build/leno_vm$EXE --self"
