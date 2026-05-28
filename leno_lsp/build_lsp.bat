@echo off
setlocal enabledelayedexpansion

echo Building Leno LSP Server with LenoC compiler...

if not exist build mkdir build

rem Delete old executable if exists
if exist build\leno_lsp.exe del /F build\leno_lsp.exe 2>nul

rem LSP source files
set LSP_SOURCES=
set LSP_SOURCES=!LSP_SOURCES! lsp_server.c
set LSP_SOURCES=!LSP_SOURCES! lsp_protocol.c
set LSP_SOURCES=!LSP_SOURCES! lsp_document.c
set LSP_SOURCES=!LSP_SOURCES! lsp_diagnostic.c
set LSP_SOURCES=!LSP_SOURCES! lsp_complete.c
set LSP_SOURCES=!LSP_SOURCES! lsp_hover.c
set LSP_SOURCES=!LSP_SOURCES! lsp_definition.c
set LSP_SOURCES=!LSP_SOURCES! json.c
set LSP_SOURCES=!LSP_SOURCES! leno_compiler_lib.c

rem LenoC source files
set LENO_SOURCES=
set LENO_SOURCES=!LENO_SOURCES! ../src/error.c
set LENO_SOURCES=!LENO_SOURCES! ../src/lexer.c
set LENO_SOURCES=!LENO_SOURCES! ../src/ast.c
set LENO_SOURCES=!LENO_SOURCES! ../src/scope.c
set LENO_SOURCES=!LENO_SOURCES! ../src/type.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_utils.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_expr.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_stmt_control.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_stmt_other.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_func.c
set LENO_SOURCES=!LENO_SOURCES! ../src/parser/parser_module.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_upvalue.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_type.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_visit.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_visit_method.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_visit_ast.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_visit_func.c
set LENO_SOURCES=!LENO_SOURCES! ../src/semantic/semantic_type_utils.c
set LENO_SOURCES=!LENO_SOURCES! ../src/gc.c
set LENO_SOURCES=!LENO_SOURCES! ../src/value.c
set LENO_SOURCES=!LENO_SOURCES! ../src/string_table.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_string.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_array.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_dict.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_number.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_file.c
set LENO_SOURCES=!LENO_SOURCES! ../src/object/object_struct.c
set LENO_SOURCES=!LENO_SOURCES! ../src/bound_method.c
set LENO_SOURCES=!LENO_SOURCES! ../src/coroutine.c
set LENO_SOURCES=!LENO_SOURCES! ../src/vm/vm.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_emit.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_expr.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_stmt.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_func.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_import.c
set LENO_SOURCES=!LENO_SOURCES! ../src/codegen/codegen_utils.c
set LENO_SOURCES=!LENO_SOURCES! ../src/debug.c
set LENO_SOURCES=!LENO_SOURCES! ../src/native.c
set LENO_SOURCES=!LENO_SOURCES! ../src/bigint.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module_loader.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module_symbol_table.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/io/io.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/types/types.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/times/times.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/arrays/arrays.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/strings/strings.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/maths/maths.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/dicts/dicts.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/structs/structs.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/rands/rands.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/files/files.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/asyncs/asyncs.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/dirs/dirs.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/jsons/jsons.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/sockets/sockets.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/ffi/ffi.c
set LENO_SOURCES=!LENO_SOURCES! ../src/module/ffi/leno_ffi_win64.c

gcc -o build\leno_lsp.exe !LSP_SOURCES! !LENO_SOURCES! -I../src -Wall -Wextra -std=c99 -O2 -lm -lws2_32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Build successful: build\leno_lsp.exe
