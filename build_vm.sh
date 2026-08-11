#!/bin/bash
set -e

echo "Building LenoLang VM Runtime (no compiler)..."

mkdir -p build

# 通用源文件清单：以 build_vm.bat（Windows 权威、可工作清单）为准。
# 注意：不包含各平台专属的 ffi 实现文件（win64/linux），在下方按平台追加。
# 历史上 build_vm.sh 曾引用过 src/object/object_{draw,window,event,image,font}.c 与
# src/module/guis/guis_*.c，但这些文件/目录已不存在，故不再列出。
SOURCES=""
SOURCES="$SOURCES src/vm_main.c"
SOURCES="$SOURCES src/error.c"
SOURCES="$SOURCES src/scope.c"
SOURCES="$SOURCES src/gc.c"
SOURCES="$SOURCES src/value.c"
SOURCES="$SOURCES src/string_table.c"
SOURCES="$SOURCES src/object/method_table.c"
SOURCES="$SOURCES src/object/object_string.c"
SOURCES="$SOURCES src/object/object_array.c"
SOURCES="$SOURCES src/object/object_dict.c"
SOURCES="$SOURCES src/object/object_number.c"
SOURCES="$SOURCES src/object/object_file.c"
SOURCES="$SOURCES src/object/object_socket.c"
SOURCES="$SOURCES src/object/object_struct.c"
SOURCES="$SOURCES src/object/object_face.c"
SOURCES="$SOURCES src/object/object_cstruct.c"
SOURCES="$SOURCES src/object/object_thread.c"
SOURCES="$SOURCES src/bound_method.c"
SOURCES="$SOURCES src/coroutine.c"
SOURCES="$SOURCES src/vm/vm.c"
SOURCES="$SOURCES src/type.c"
SOURCES="$SOURCES src/native.c"
SOURCES="$SOURCES src/bigint.c"
SOURCES="$SOURCES src/module_loader.c"
SOURCES="$SOURCES src/module_dispatch.c"
SOURCES="$SOURCES src/module_symbol_table/module_symbol_table.c"
SOURCES="$SOURCES src/module.c"
SOURCES="$SOURCES src/module/io/io.c"
SOURCES="$SOURCES src/module/types/types.c"
SOURCES="$SOURCES src/module/times/times.c"
SOURCES="$SOURCES src/module/arrays/arrays.c"
SOURCES="$SOURCES src/module/strings/strings.c"
SOURCES="$SOURCES src/module/maths/maths.c"
SOURCES="$SOURCES src/module/dicts/dicts.c"
SOURCES="$SOURCES src/module/structs/structs.c"
SOURCES="$SOURCES src/module/cstructs/cstructs.c"
SOURCES="$SOURCES src/module/rands/rands.c"
SOURCES="$SOURCES src/module/files/files.c"
SOURCES="$SOURCES src/module/asyncs/asyncs.c"
SOURCES="$SOURCES src/module/dirs/dirs.c"
SOURCES="$SOURCES src/module/jsons/jsons.c"
SOURCES="$SOURCES src/module/sockets/sockets.c"
SOURCES="$SOURCES src/module/ffi/ffi.c"
SOURCES="$SOURCES src/module/threads/threads.c"
SOURCES="$SOURCES src/module/assert/assert.c"
SOURCES="$SOURCES src/module/sys/sys.c"
SOURCES="$SOURCES src/module/regexs/regexs.c"
SOURCES="$SOURCES src/platform/platform_thread.c"
SOURCES="$SOURCES src/serialize/serialize.c"

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
  # leno_ffi_linux.c 用 #ifndef _WIN32 包裹，macOS（类 Unix）同样适用
  SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  LIBS="$LIBS -lpthread -ldl"
else
  SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  LIBS="$LIBS -lpthread -ldl"
  CFLAGS="$CFLAGS -D_GNU_SOURCE"
fi

CC=${CC:-gcc}

# debug.c 仅包含反汇编函数，VM 运行时不需要，通过 LENO_VM_ONLY 条件编译排除
$CC $CFLAGS -o build/leno_vm$EXE $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 -DLENO_VM_ONLY $LIBS

echo "VM build successful"
echo ""
echo "Usage: build/leno_vm$EXE <file.lenb>"
echo "Self:  build/leno_vm$EXE --self"
