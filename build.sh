#!/bin/bash
set -e

echo "Building LenoLang Compiler..."

mkdir -p build

# 通用源文件清单：以 build.bat（Windows 权威、可工作清单）为准。
# 注意：不包含各平台专属的 ffi 实现文件（win64/linux），在下方按平台追加。
# 历史上 build.sh 曾引用过 src/object/object_{draw,window,event,image,font}.c 与
# src/module/guis/guis_*.c，但这些文件/目录已不存在，故不再列出。
SOURCES=""
SOURCES="$SOURCES src/main.c"
SOURCES="$SOURCES src/error.c"
SOURCES="$SOURCES src/lexer.c"
SOURCES="$SOURCES src/ast.c"
SOURCES="$SOURCES src/scope.c"
SOURCES="$SOURCES src/parser/parser.c"
SOURCES="$SOURCES src/parser/parser_utils.c"
SOURCES="$SOURCES src/parser/parser_expr.c"
SOURCES="$SOURCES src/parser/parser_stmt_control.c"
SOURCES="$SOURCES src/parser/parser_stmt_other.c"
SOURCES="$SOURCES src/parser/parser_func.c"
SOURCES="$SOURCES src/parser/parser_module.c"
SOURCES="$SOURCES src/semantic/semantic.c"
SOURCES="$SOURCES src/semantic/semantic_upvalue.c"
SOURCES="$SOURCES src/semantic/semantic_type.c"
SOURCES="$SOURCES src/semantic/semantic_visit.c"
SOURCES="$SOURCES src/semantic/semantic_visit_method.c"
SOURCES="$SOURCES src/semantic/semantic_visit_ast.c"
SOURCES="$SOURCES src/semantic/semantic_visit_func.c"
SOURCES="$SOURCES src/semantic/semantic_type_utils.c"
SOURCES="$SOURCES src/optimize/optimize.c"
SOURCES="$SOURCES src/gc.c"
SOURCES="$SOURCES src/value.c"
SOURCES="$SOURCES src/string_table.c"
SOURCES="$SOURCES src/object/object_string.c"
SOURCES="$SOURCES src/object/method_table.c"
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
SOURCES="$SOURCES src/codegen/codegen.c"
SOURCES="$SOURCES src/codegen/codegen_emit.c"
SOURCES="$SOURCES src/codegen/codegen_expr.c"
SOURCES="$SOURCES src/codegen/codegen_stmt.c"
SOURCES="$SOURCES src/codegen/codegen_func.c"
SOURCES="$SOURCES src/codegen/codegen_import.c"
SOURCES="$SOURCES src/codegen/codegen_utils.c"
SOURCES="$SOURCES src/debug.c"
SOURCES="$SOURCES src/type.c"
SOURCES="$SOURCES src/native.c"
SOURCES="$SOURCES src/bigint.c"
SOURCES="$SOURCES src/module_loader.c"
SOURCES="$SOURCES src/module_dispatch.c"
SOURCES="$SOURCES src/module_compiler.c"
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
SOURCES="$SOURCES src/package/package_platform.c"
SOURCES="$SOURCES src/package/package_toml.c"
SOURCES="$SOURCES src/package/package_init.c"
SOURCES="$SOURCES src/package/package_resolve.c"
SOURCES="$SOURCES src/package/package_install.c"

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

$CC $CFLAGS -o build/leno$EXE $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 $LIBS

echo "Build successful"
echo ""
echo "Usage: build/leno$EXE <file.leno>"
echo "Tests:  build/leno$EXE assert/run_tests.leno build/leno$EXE assert"
