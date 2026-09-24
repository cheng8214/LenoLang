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
REM Source lists are SHARED with build.bat and build.sh (single source of truth
REM -- this is the P9 fix):
REM   sources_core.txt  / sources_compiler.txt / sources_vm.txt
REM Do NOT add a source file directly to this script: edit the list files.
REM NOTE: the parser (src\parser\*.c, listed in sources_compiler.txt) is
REM deliberately NOT part of this build -- this build is "no compiler".
REM module_symbol_table.c still *compiles* the source-scan chain
REM (inc\sym_table_scan.inc -> inc\scan\*.inc -> scan_enum.inc), which is the
REM only thing that calls parser_eval_const_expr_text(); that path is unreachable
REM at runtime (its sole external caller is the semantic analyser,
REM src\semantic\visitinc\visit_module.inc), so module_symbol_table.c provides a
REM VM-only stub for it (-DLENO_VM_ONLY). See the comment above that stub.
if not exist build mkdir build

set SOURCES=
for /f "usebackq delims=" %%f in ("sources_core.txt") do set SOURCES=!SOURCES! %%f
for /f "usebackq delims=" %%f in ("sources_vm.txt") do set SOURCES=!SOURCES! %%f
REM platform-specific FFI (Windows)
set SOURCES=!SOURCES! src\module\ffi\leno_ffi_win64.c

REM ---------------------------------------------------------------------------
REM Application icon: leno_icon.ico via resources\leno.rc (windres).
REM Search order: resources\leno_icon.ico, then build\leno_icon.ico.
REM Both VM binaries carry it, so every -p packed exe (they prepend one of these)
REM inherits the same icon. A missing .ico is NOT an error: we print a line and
REM build without an icon.
REM ---------------------------------------------------------------------------
set ICON_ICO=
if exist resources\leno_icon.ico set ICON_ICO=resources\leno_icon.ico
if not defined ICON_ICO if exist build\leno_icon.ico set ICON_ICO=build\leno_icon.ico
set ICON_OBJ=
if defined ICON_ICO (
    windres resources\leno.rc -O coff -I resources -I build -o build\leno_icon.o
    if !ERRORLEVEL! neq 0 (
        echo Icon resource build failed
        exit /b 1
    )
    set ICON_OBJ=build\leno_icon.o
    echo Icon: !ICON_ICO!
) else (
    echo Icon: leno_icon.ico not found - building without an icon
)

REM 1. Console build (leno_vm.exe, for command-line debugging)
REM    -s: strip the symbol table and debug info so function / variable / type
REM    names are not exposed. Drop -s here if you need to debug the VM itself.
gcc -o build\leno_vm.exe !SOURCES! !ICON_OBJ! -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo VM build failed
    exit /b 1
)

REM 2. Windowed build (leno_vm_gui.exe, for packaging GUI apps: no console window
REM    on startup; scripts can still get one via _console(true) / AllocConsole)
gcc -o build\leno_vm_gui.exe !SOURCES! !ICON_OBJ! -Isrc -Wall -Wextra -std=c99 -O2 -s -DLENO_VM_ONLY -lm -municode -lws2_32 -mwindows

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
