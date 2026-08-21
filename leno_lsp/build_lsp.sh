#!/bin/bash
set -e

echo "Building Leno LSP Server with LenoC compiler..."

mkdir -p build

# Delete old executable if exists
if [ -f build/leno_lsp ]; then
    rm -f build/leno_lsp
fi

# LSP source files
LSP_SOURCES=""
LSP_SOURCES="$LSP_SOURCES lsp_server.c"
LSP_SOURCES="$LSP_SOURCES lsp_protocol.c"
LSP_SOURCES="$LSP_SOURCES lsp_document.c"
LSP_SOURCES="$LSP_SOURCES lsp_diagnostic.c"
LSP_SOURCES="$LSP_SOURCES lsp_document_symbol.c"
LSP_SOURCES="$LSP_SOURCES lsp_signature.c"
LSP_SOURCES="$LSP_SOURCES lsp_references.c"
LSP_SOURCES="$LSP_SOURCES lsp_folding.c"
LSP_SOURCES="$LSP_SOURCES lsp_complete.c"
LSP_SOURCES="$LSP_SOURCES lsp_hover.c"
LSP_SOURCES="$LSP_SOURCES lsp_definition.c"
LSP_SOURCES="$LSP_SOURCES json.c"
LSP_SOURCES="$LSP_SOURCES leno_compiler_lib.c"

# LenoC source files
LENO_SOURCES=""
LENO_SOURCES="$LENO_SOURCES ../src/error.c"
LENO_SOURCES="$LENO_SOURCES ../src/lexer.c"
LENO_SOURCES="$LENO_SOURCES ../src/ast.c"
LENO_SOURCES="$LENO_SOURCES ../src/scope.c"
LENO_SOURCES="$LENO_SOURCES ../src/type.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_utils.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_expr.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_stmt_control.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_stmt_other.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_func.c"
LENO_SOURCES="$LENO_SOURCES ../src/parser/parser_module.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_upvalue.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_type.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_visit.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_visit_method.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_visit_ast.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_visit_func.c"
LENO_SOURCES="$LENO_SOURCES ../src/semantic/semantic_type_utils.c"
LENO_SOURCES="$LENO_SOURCES ../src/gc.c"
LENO_SOURCES="$LENO_SOURCES ../src/value.c"
LENO_SOURCES="$LENO_SOURCES ../src/string_table.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_string.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_array.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_dict.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_number.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_file.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_struct.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_cstruct.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_face.c"
LENO_SOURCES="$LENO_SOURCES ../src/object/object_thread.c"
LENO_SOURCES="$LENO_SOURCES ../src/bound_method.c"
LENO_SOURCES="$LENO_SOURCES ../src/coroutine.c"
LENO_SOURCES="$LENO_SOURCES ../src/vm/vm.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_emit.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_expr.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_stmt.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_func.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_import.c"
LENO_SOURCES="$LENO_SOURCES ../src/codegen/codegen_utils.c"
LENO_SOURCES="$LENO_SOURCES ../src/debug.c"
LENO_SOURCES="$LENO_SOURCES ../src/native.c"
LENO_SOURCES="$LENO_SOURCES ../src/bigint.c"
LENO_SOURCES="$LENO_SOURCES ../src/module_loader.c"
LENO_SOURCES="$LENO_SOURCES ../src/module_symbol_table/module_symbol_table.c"
LENO_SOURCES="$LENO_SOURCES ../src/module.c"
LENO_SOURCES="$LENO_SOURCES ../src/module_dispatch.c"
LENO_SOURCES="$LENO_SOURCES ../src/module_compiler.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/io/io.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/types/types.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/times/times.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/arrays/arrays.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/strings/strings.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/maths/maths.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/dicts/dicts.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/structs/structs.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/rands/rands.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/files/files.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/asyncs/asyncs.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/dirs/dirs.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/jsons/jsons.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/sockets/sockets.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/ffi.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/cstructs/cstructs.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/threads/threads.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/assert/assert.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/sys/sys.c"
LENO_SOURCES="$LENO_SOURCES ../src/module/regexs/regexs.c"
LENO_SOURCES="$LENO_SOURCES ../src/platform/platform_thread.c"
LENO_SOURCES="$LENO_SOURCES ../src/package/package_platform.c"
LENO_SOURCES="$LENO_SOURCES ../src/package/package_toml.c"
LENO_SOURCES="$LENO_SOURCES ../src/package/package_init.c"
LENO_SOURCES="$LENO_SOURCES ../src/package/package_resolve.c"
LENO_SOURCES="$LENO_SOURCES ../src/package/package_install.c"

# Platform-specific libraries and FFI implementation
LIBS="-lm -lpthread -ldl"

# 检测平台和架构，选择对应的 FFI 实现文件
OS="$(uname -s 2>/dev/null || echo unknown)"
ARCH="$(uname -m 2>/dev/null || echo x86_64)"
case "$OS" in
  Darwin*)
    if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_arm64.c"
    else
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_linux.c"
    fi
    ;;
  *)
    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_arm64.c"
    else
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_linux.c"
    fi
    ;;
esac

gcc -o build/leno_lsp $LSP_SOURCES $LENO_SOURCES -I../src -Wall -Wextra -std=c99 -O2 $LIBS

echo "Build successful: build/leno_lsp"
