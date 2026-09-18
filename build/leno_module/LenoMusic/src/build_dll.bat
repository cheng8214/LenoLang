@echo off
echo Building miniaudio.dll...

gcc -shared -o miniaudio.dll miniaudio_dll.c -O2 -Wall -lwinmm -lole32 -lksuser -lm -DWINVER=0x0601 -D_WIN32_WINNT=0x0601
if %ERRORLEVEL% neq 0 (
    echo Build failed
    exit /b 1
)

echo Copying to lib\...
copy /Y miniaudio.dll ..\lib\miniaudio.dll >nul
echo Done
