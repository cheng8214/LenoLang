@echo off
setlocal enabledelayedexpansion

echo Building Leno LSP Server with LenoC compiler...

if not exist build mkdir build

rem Delete old executable if exists
if exist build\leno_lsp.exe del /F build\leno_lsp.exe 2>nul
if exist build\leno_lsp_old.exe del /F build\leno_lsp_old.exe 2>nul

rem === LSP source files ===
set LSP_SOURCES=
set LSP_SOURCES=!LSP_SOURCES! lsp_server.c
set LSP_SOURCES=!LSP_SOURCES! lsp_protocol.c
set LSP_SOURCES=!LSP_SOURCES! lsp_document.c
set LSP_SOURCES=!LSP_SOURCES! lsp_complete.c
set LSP_SOURCES=!LSP_SOURCES! lsp_hover.c
set LSP_SOURCES=!LSP_SOURCES! lsp_definition.c
set LSP_SOURCES=!LSP_SOURCES! lsp_diagnostic.c
set LSP_SOURCES=!LSP_SOURCES! lsp_document_symbol.c
set LSP_SOURCES=!LSP_SOURCES! lsp_signature.c
set LSP_SOURCES=!LSP_SOURCES! lsp_references.c
set LSP_SOURCES=!LSP_SOURCES! lsp_folding.c
set LSP_SOURCES=!LSP_SOURCES! json.c
set LSP_SOURCES=!LSP_SOURCES! leno_compiler_lib.c

rem === New modular completion engine ===
set LSP_SOURCES=!LSP_SOURCES! comp_set.c
set LSP_SOURCES=!LSP_SOURCES! comp_context.c
set LSP_SOURCES=!LSP_SOURCES! comp_import.c
set LSP_SOURCES=!LSP_SOURCES! comp_keywords.c
set LSP_SOURCES=!LSP_SOURCES! comp_symbols.c

rem === LenoC source files ===
rem Source lists are SHARED with build.bat / build_vm.bat / build.sh (single source
rem of truth). Do NOT add a source file here: edit the list files, or the builds
rem drift apart -- which is exactly how this script broke: it still listed the
rem deleted src\codegen\codegen_inline.c, and was missing src\dce.c,
rem src\optimize\optimize.c, src\platform\platform_path.c and
rem src\package\package_icon.c.
rem   ..\sources_core.txt      - core runtime
rem   ..\sources_compiler.txt  - compiler-only (lexer/parser/semantic/codegen/...)
rem src\main.c is EXCLUDED: the LSP entry point is main() in lsp_server.c.
set LENO_SOURCES=
for /f "usebackq delims=" %%f in ("..\sources_core.txt") do set LENO_SOURCES=!LENO_SOURCES! ..\%%f
for /f "usebackq delims=" %%f in ("..\sources_compiler.txt") do if /i not "%%f"=="src\main.c" set LENO_SOURCES=!LENO_SOURCES! ..\%%f

rem Platform-specific FFI (Windows)
set LENO_SOURCES=!LENO_SOURCES! ..\src\module\ffi\leno_ffi_win64.c

rem Build to temp file first, then rename (handles file lock from running LSP)
if exist build\leno_lsp_new.exe del /F build\leno_lsp_new.exe 2>nul
gcc -o build\leno_lsp_new.exe !LSP_SOURCES! !LENO_SOURCES! -I../src -Wall -Wextra -std=c99 -O2 -lm -lws2_32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

rem Replace old executable (handles file lock from running LSP)
if exist build\leno_lsp.exe ren build\leno_lsp.exe leno_lsp_old.exe 2>nul
ren build\leno_lsp_new.exe leno_lsp.exe 2>nul
if exist build\leno_lsp_old.exe del /F build\leno_lsp_old.exe 2>nul

echo Build successful: build\leno_lsp.exe
