@echo off
setlocal enabledelayedexpansion

echo Building LenoLang Compiler...

if not exist build mkdir build

REM ---------------------------------------------------------------------------
REM NOTE: keep this file ASCII-only.
REM Batch files are parsed with the *active console code page*; a UTF-8 comment
REM in a GBK console gets mis-decoded and its fragments are executed as commands
REM (seen 2026-09-18: 'ebug.c' is not recognized as an internal command ...).
REM Chinese explanations belong in the docs / in the source comments instead.
REM ---------------------------------------------------------------------------
REM Source lists are SHARED with build_vm.bat and build.sh (single source of
REM truth -- this is the P9 fix):
REM   sources_core.txt      - core runtime (used by BOTH builds)
REM   sources_compiler.txt  - compiler-only (lexer/parser/semantic/codegen/package)
REM   sources_vm.txt        - VM-only entry point (vm_main.c; mutually exclusive
REM                           with src\main.c, so it must never land here)
REM Do NOT add a source file directly to this script: edit the list files, or the
REM two builds drift apart -- that is exactly how "undefined reference to
REM opcode_name" / "undefined reference to parser_eval_const_expr_text" happened.
set SOURCES=
for /f "usebackq delims=" %%f in ("sources_core.txt") do set SOURCES=!SOURCES! %%f
for /f "usebackq delims=" %%f in ("sources_compiler.txt") do set SOURCES=!SOURCES! %%f
REM platform-specific FFI (Windows)
set SOURCES=!SOURCES! src\module\ffi\leno_ffi_win64.c

REM ---------------------------------------------------------------------------
REM Application icon: leno_icon.ico via resources\leno.rc (windres).
REM Search order: resources\leno_icon.ico, then build\leno_icon.ico.
REM The icon lands in the PE resource section, and every -p packed exe prepends
REM this binary, so one .ico covers all outputs. A missing .ico is NOT an error:
REM we print a line and build without an icon.
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

gcc -o build\leno.exe !SOURCES! !ICON_OBJ! -Isrc -Wall -Wextra -std=c99 -O2 -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)


echo Build successful
echo.
echo Usage: build\leno.exe ^<file.leno^>
echo Tests:  build\leno.exe assert\run_tests.leno build\leno.exe assert
