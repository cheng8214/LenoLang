@echo off
setlocal enabledelayedexpansion

echo Building LenoLang VM Runtime (no compiler)...

REM ---------------------------------------------------------------------------
REM NOTE: keep this file ASCII-only.
REM Batch files are parsed with the *active console code page*; a UTF-8 comment
REM in a GBK console gets mis-decoded and its fragments are executed as commands
REM (seen 2026-09-18: 'ebug.c' is not recognized as an internal command ...).
REM Explanations in Chinese belong in the docs / in the source comments instead.
REM ---------------------------------------------------------------------------

if not exist build mkdir build

set SOURCES=
set SOURCES=!SOURCES! src\vm_main.c
set SOURCES=!SOURCES! src\error.c
set SOURCES=!SOURCES! src\scope.c
set SOURCES=!SOURCES! src\gc.c
set SOURCES=!SOURCES! src\value.c
set SOURCES=!SOURCES! src\string_table.c
set SOURCES=!SOURCES! src\object\method_table.c
set SOURCES=!SOURCES! src\object\object_string.c
set SOURCES=!SOURCES! src\object\object_array.c
set SOURCES=!SOURCES! src\object\object_dict.c
set SOURCES=!SOURCES! src\object\object_number.c
set SOURCES=!SOURCES! src\object\object_file.c
set SOURCES=!SOURCES! src\object\object_struct.c
set SOURCES=!SOURCES! src\object\object_face.c
set SOURCES=!SOURCES! src\object\object_cstruct.c
set SOURCES=!SOURCES! src\object\object_thread.c
set SOURCES=!SOURCES! src\object\object_socket.c
set SOURCES=!SOURCES! src\bound_method.c
set SOURCES=!SOURCES! src\coroutine.c
set SOURCES=!SOURCES! src\vm\vm.c
REM debug.c provides opcode_name(), which the JIT rejection histogram uses
REM (jit_scan.c x5, jit.c x3). The old comment here claimed debug.c was
REM "excluded via LENO_VM_ONLY", but that guard never existed in debug.c, so
REM omitting it broke this build with "undefined reference to `opcode_name'".
REM Fixed 2026-09-18. The disassembler in debug.c is unreferenced in the VM;
REM linking it in is harmless (-s strips the symbols).
set SOURCES=!SOURCES! src\debug.c
set SOURCES=!SOURCES! src\type.c
set SOURCES=!SOURCES! src\native.c
set SOURCES=!SOURCES! src\bigint.c
set SOURCES=!SOURCES! src\module_loader.c
set SOURCES=!SOURCES! src\module_dispatch.c
set SOURCES=!SOURCES! src\module_symbol_table\module_symbol_table.c
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
set SOURCES=!SOURCES! src\platform\platform_thread.c
set SOURCES=!SOURCES! src\serialize\serialize.c
REM NOTE: the parser (src\parser\*.c) is deliberately NOT here -- this build is
REM "no compiler". module_symbol_table.c still *compiles* the source-scan chain
REM (inc\sym_table_scan.inc -> inc\scan\*.inc -> scan_enum.inc), which is the
REM only thing that calls parser_eval_const_expr_text(); that path is unreachable
REM at runtime (its sole external caller is the semantic analyser,
REM src\semantic\visitinc\visit_module.inc), so module_symbol_table.c provides a
REM VM-only stub for it. See the comment above that stub.
REM JIT is part of the VM runtime: OP_CALL / back-edges call jit_try_hot_*,
REM gc calls jit_in_frame(); omitting these files breaks the link
REM (keep in sync with build.bat)
set SOURCES=!SOURCES! src\jit\jit_callout.c
set SOURCES=!SOURCES! src\jit\jit_scan.c
set SOURCES=!SOURCES! src\jit\jit.c
REM JIT backend selected by CPU architecture (arm64 reserved:
REM implement src\jit\backend\arm64.c and it is picked up automatically)
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
  set SOURCES=!SOURCES! src\jit\backend\arm64.c
) else (
  set SOURCES=!SOURCES! src\jit\backend\x86_64.c
)

REM 1. Console build (leno_vm.exe, for command-line debugging)
REM    -s: strip the symbol table and debug info so function / variable / type
REM    names are not exposed. Drop -s here if you need to debug the VM itself.
gcc -o build\leno_vm.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo VM build failed
    exit /b 1
)

REM 2. Windowed build (leno_vm_gui.exe, for packaging GUI apps: no console window
REM    on startup; scripts can still get one via _console(true) / AllocConsole)
gcc -o build\leno_vm_gui.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY -lm -municode -lws2_32 -mwindows

if %ERRORLEVEL% neq 0 (
    echo VM GUI build failed
    exit /b 1
)

echo VM build successful
echo   leno_vm.exe     - console version (debug/cli)
echo   leno_vm_gui.exe - GUI version (no console, for packaging)
echo.
echo Usage: build\leno_vm.exe ^<file.lenb^>
echo Self:  build\leno_vm.exe --self
