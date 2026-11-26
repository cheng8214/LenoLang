@echo off
setlocal enabledelayedexpansion

REM ---------------------------------------------------------------------------
REM build_dev.bat -- same sources as build.bat, but the output is
REM   build\leno_dev.exe
REM Why: build\leno.exe is often in use by the user (running tests / benchmarks).
REM   Linking over a file that is being executed fails on Windows, so the
REM   development loop uses a separate binary name.
REM NOTE: keep this file ASCII-only (see the comment in build.bat).
REM ---------------------------------------------------------------------------

echo Building LenoLang Compiler (dev: leno_dev.exe)...

if not exist build mkdir build

set SOURCES=
for /f "usebackq delims=" %%f in ("sources_core.txt") do set SOURCES=!SOURCES! %%f
for /f "usebackq delims=" %%f in ("sources_compiler.txt") do set SOURCES=!SOURCES! %%f
set SOURCES=!SOURCES! src\module\ffi\leno_ffi_win64.c

set ICON_ICO=
if exist resources\leno_icon.ico set ICON_ICO=resources\leno_icon.ico
if not defined ICON_ICO if exist build\leno_icon.ico set ICON_ICO=build\leno_icon.ico
set ICON_OBJ=
if defined ICON_ICO (
    if not exist build\leno_icon.o (
        windres resources\leno.rc -O coff -I resources -I build -o build\leno_icon.o
        if !ERRORLEVEL! neq 0 (
            echo Icon resource build failed
            exit /b 1
        )
    )
    set ICON_OBJ=build\leno_icon.o
)

gcc -o build\leno_dev.exe !SOURCES! !ICON_OBJ! -Isrc -Wall -Wextra -std=c99 -O2 -lm -municode -lws2_32

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Build successful
echo Usage: build\leno_dev.exe ^<file.leno^>
