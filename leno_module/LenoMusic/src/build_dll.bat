@echo off
echo Building miniaudio.dll...

gcc -shared -o miniaudio.dll miniaudio_dll.c -O2 -Wall -lwinmm -lole32 -lksuser -lm
if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Copying to lib\ and native\...
copy /Y miniaudio.dll ..\lib\miniaudio.dll >nul
copy /Y miniaudio.dll ..\native\miniaudio.dll >nul
echo Done
