@echo off
setlocal
REM Build quirc.dll for Windows x64 (MinGW). ASCII only -- see build.bat's note.
REM Output: ..\lib\win\quirc.dll  (per-platform dirs since 2026-11-19)
REM Consumer: lib\qr_native.leno loads it via ffi.load("win/quirc.dll")

echo Building quirc.dll...

REM vendored quirc sources: do NOT add -Wall here (upstream is not warning-free,
REM and we must not fork it just to silence cosmetic warnings)
gcc -c -O2 quirc.c decode.c identify.c version_db.c
if %ERRORLEVEL% neq 0 (
    echo [fail] quirc sources
    exit /b 1
)

REM our wrapper: strict warnings (this is the code we own)
gcc -c -O2 -Wall -Wextra leno_qr.c
if %ERRORLEVEL% neq 0 (
    echo [fail] leno_qr.c
    exit /b 1
)

REM -static-libgcc: keep the DLL self-contained (no libgcc_s_seh-1.dll dependency,
REM which would otherwise have to be shipped next to the module)
REM output goes straight to ..\lib\win\ : src\ is source-only (no build artifact there)
if not exist ..\lib\win mkdir ..\lib\win
gcc -shared -static-libgcc -o ..\lib\win\quirc.dll quirc.o decode.o identify.o version_db.o leno_qr.o
if %ERRORLEVEL% neq 0 (
    echo [fail] link
    exit /b 1
)

del /Q *.o >nul 2>nul
echo Done: lib\win\quirc.dll
exit /b 0
