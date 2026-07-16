#!/bin/bash
set -e

echo "Building LenoLang VM Runtime (no compiler)..."

mkdir -p build

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
SOURCES="$SOURCES src/object/object_struct.c"
SOURCES="$SOURCES src/object/object_face.c"
SOURCES="$SOURCES src/object/object_cstruct.c"
SOURCES="$SOURCES src/object/object_thread.c"
SOURCES="$SOURCES src/object/object_event.c"
SOURCES="$SOURCES src/object/object_draw.c"
SOURCES="$SOURCES src/object/object_window.c"
SOURCES="$SOURCES src/object/object_font.c"
SOURCES="$SOURCES src/object/object_image.c"
SOURCES="$SOURCES src/bound_method.c"
SOURCES="$SOURCES src/coroutine.c"
SOURCES="$SOURCES src/vm/vm.c"
SOURCES="$SOURCES src/debug.c"
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
SOURCES="$SOURCES src/module/guis/guis.c"
SOURCES="$SOURCES src/module/guis/guis_constants.c"
SOURCES="$SOURCES src/module/guis/guis_draw.c"
SOURCES="$SOURCES src/module/guis/guis_window.c"
SOURCES="$SOURCES src/module/guis/guis_event.c"
SOURCES="$SOURCES src/module/guis/guis_font.c"
SOURCES="$SOURCES src/module/guis/guis_image.c"
SOURCES="$SOURCES src/module/guis/leno_guis_log.c"
SOURCES="$SOURCES src/serialize/serialize.c"

# Platform-specific sources
if [[ "$OSTYPE" == "darwin"* ]]; then
    SOURCES="$SOURCES src/module/guis/leno_guis_macos.c"
    SOURCES="$SOURCES src/platform/platform_thread.c"
elif [[ "$OSTYPE" == "linux"* ]]; then
    SOURCES="$SOURCES src/module/guis/leno_guis_linux.c"
    SOURCES="$SOURCES src/platform/platform_thread.c"
fi

# Platform-specific libraries
LIBS="-lm -lpthread -ldl"
if [[ "$OSTYPE" == "darwin"* ]]; then
    LIBS="$LIBS -framework AppKit"
else
    LIBS="$LIBS -lX11"
fi

gcc -o build/leno_vm $SOURCES -Isrc -Wall -Wextra -std=c99 -O2 -s $LIBS -DLENO_VM_ONLY

echo "VM build successful"
echo ""
echo "Usage: build/leno_vm <file.lenb>"
echo "Self:  build/leno_vm --self"
