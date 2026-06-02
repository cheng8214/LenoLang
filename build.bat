@echo off
setlocal enabledelayedexpansion

echo Building LenoLang Compiler...

if not exist build mkdir build

set SOURCES=
set SOURCES=!SOURCES! src\main.c
set SOURCES=!SOURCES! src\error.c
set SOURCES=!SOURCES! src\lexer.c
set SOURCES=!SOURCES! src\ast.c
set SOURCES=!SOURCES! src\scope.c
set SOURCES=!SOURCES! src\parser\parser.c
set SOURCES=!SOURCES! src\parser\parser_utils.c
set SOURCES=!SOURCES! src\parser\parser_expr.c
set SOURCES=!SOURCES! src\parser\parser_stmt_control.c
set SOURCES=!SOURCES! src\parser\parser_stmt_other.c
set SOURCES=!SOURCES! src\parser\parser_func.c
set SOURCES=!SOURCES! src\parser\parser_module.c
set SOURCES=!SOURCES! src\semantic\semantic.c
set SOURCES=!SOURCES! src\semantic\semantic_upvalue.c
set SOURCES=!SOURCES! src\semantic\semantic_type.c
set SOURCES=!SOURCES! src\semantic\semantic_visit.c
set SOURCES=!SOURCES! src\semantic\semantic_visit_method.c
set SOURCES=!SOURCES! src\semantic\semantic_visit_ast.c
set SOURCES=!SOURCES! src\semantic\semantic_visit_func.c
set SOURCES=!SOURCES! src\semantic\semantic_type_utils.c
set SOURCES=!SOURCES! src\optimize\optimize.c
set SOURCES=!SOURCES! src\gc.c
set SOURCES=!SOURCES! src\value.c
set SOURCES=!SOURCES! src\string_table.c
set SOURCES=!SOURCES! src\object\object_string.c
set SOURCES=!SOURCES! src\object\object_array.c
set SOURCES=!SOURCES! src\object\object_dict.c
set SOURCES=!SOURCES! src\object\object_number.c
set SOURCES=!SOURCES! src\object\object_file.c
set SOURCES=!SOURCES! src\object\object_draw.c
set SOURCES=!SOURCES! src\object\object_window.c
set SOURCES=!SOURCES! src\object\object_event.c
set SOURCES=!SOURCES! src\object\object_image.c
set SOURCES=!SOURCES! src\object\object_font.c
set SOURCES=!SOURCES! src\object\object_struct.c
set SOURCES=!SOURCES! src\object\object_face.c
set SOURCES=!SOURCES! src\object\object_cstruct.c
set SOURCES=!SOURCES! src\object\object_thread.c
set SOURCES=!SOURCES! src\bound_method.c
set SOURCES=!SOURCES! src\coroutine.c
set SOURCES=!SOURCES! src\vm\vm.c
set SOURCES=!SOURCES! src\codegen\codegen.c
set SOURCES=!SOURCES! src\codegen\codegen_emit.c
set SOURCES=!SOURCES! src\codegen\codegen_expr.c
set SOURCES=!SOURCES! src\codegen\codegen_stmt.c
set SOURCES=!SOURCES! src\codegen\codegen_func.c
set SOURCES=!SOURCES! src\codegen\codegen_import.c
set SOURCES=!SOURCES! src\codegen\codegen_utils.c
set SOURCES=!SOURCES! src\debug.c
set SOURCES=!SOURCES! src\type.c
set SOURCES=!SOURCES! src\native.c
set SOURCES=!SOURCES! src\bigint.c
set SOURCES=!SOURCES! src\module_loader.c
set SOURCES=!SOURCES! src\module_dispatch.c
set SOURCES=!SOURCES! src\module_compiler.c
set SOURCES=!SOURCES! src\module_symbol_table.c
set SOURCES=!SOURCES! src\module.c
set SOURCES=!SOURCES! src\module\io\io.c
set SOURCES=!SOURCES! src\module\types\types.c
set SOURCES=!SOURCES! src\module\times\times.c
set SOURCES=!SOURCES! src\module\arrays\arrays.c
set SOURCES=!SOURCES! src\module\strings\strings.c
set SOURCES=!SOURCES! src\module\maths\maths.c
set SOURCES=!SOURCES! src\module\dicts\dicts.c
set SOURCES=!SOURCES! src\module\structs\structs.c
set SOURCES=!SOURCES! src\module\cstructs\cstructs.c
set SOURCES=!SOURCES! src\module\rands\rands.c
set SOURCES=!SOURCES! src\module\files\files.c
set SOURCES=!SOURCES! src\module\asyncs\asyncs.c
set SOURCES=!SOURCES! src\module\dirs\dirs.c
set SOURCES=!SOURCES! src\module\jsons\jsons.c
set SOURCES=!SOURCES! src\module\sockets\sockets.c
set SOURCES=!SOURCES! src\module\ffi\ffi.c
set SOURCES=!SOURCES! src\module\ffi\leno_ffi_win64.c
set SOURCES=!SOURCES! src\module\threads\threads.c
set SOURCES=!SOURCES! src\module\assert\assert.c
set SOURCES=!SOURCES! src\module\sys\sys.c
set SOURCES=!SOURCES! src\module\regexs\regexs.c
set SOURCES=!SOURCES! src\module\guis\guis.c
set SOURCES=!SOURCES! src\module\guis\guis_draw.c
set SOURCES=!SOURCES! src\module\guis\guis_window.c
set SOURCES=!SOURCES! src\module\guis\guis_style.c
set SOURCES=!SOURCES! src\module\guis\guis_event.c
set SOURCES=!SOURCES! src\module\guis\guis_image.c
set SOURCES=!SOURCES! src\module\guis\guis_font.c
set SOURCES=!SOURCES! src\module\guis\leno_guis_log.c
set SOURCES=!SOURCES! src\module\guis\leno_guis_win32.c
set SOURCES=!SOURCES! src\platform\platform_thread.c
set SOURCES=!SOURCES! src\serialize\serialize.c

gcc -o build\leno.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -O2 -lm -municode -lws2_32 -lgdi32 -luser32 -lcomdlg32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Building test runner...
if not exist build mkdir build
gcc -o build\test_runner.exe test\test_runner.c -std=c99 -O2

if %ERRORLEVEL% neq 0 (
    echo Test runner build failed
    exit /b 1
)

echo Build successful
echo.
echo Usage: build\leno.exe ^<file.leno^>
echo Tests:  build\test_runner.exe build\leno.exe test
