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
gcc -o build\lenoreg.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -O2 -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)


echo Build successful
echo.
echo Usage: build\lenoreg.exe ^<file.leno^>
echo Tests:  build\lenoreg.exe assert\run_tests.leno build\lenoreg.exe assert
