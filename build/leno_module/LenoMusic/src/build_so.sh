#!/bin/bash
# 编译 miniaudio 为 POSIX 共享库 —— build_dll.bat 的对应物（那边产出 miniaudio.dll）。
# 产物 lib/miniaudio.so 由 lib/music_core.leno 的 find_dll() 按 _os() 选中。
set -e

cd "$(dirname "$0")"

echo "Building miniaudio.so..."
gcc -shared -fPIC -O2 -o miniaudio.so miniaudio_dll.c -lm -lpthread -ldl

echo "Copying to lib/..."
cp -f miniaudio.so ../lib/miniaudio.so

echo "Done"