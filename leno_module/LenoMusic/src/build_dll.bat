@echo off

echo Building miniaudio.dll...

if not exist ..\build mkdir ..\build

gcc -shared -o ..\build\miniaudio.dll miniaudio_dll.c -O2 -Wall -lwinmm -lole32 -lksuser -lm

if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Build successful
