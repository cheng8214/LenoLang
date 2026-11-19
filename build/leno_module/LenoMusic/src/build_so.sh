#!/bin/bash
# 编译 miniaudio 为 POSIX 共享库 —— build_dll.bat 的对应物（那边产出 miniaudio.dll）。
# 产物落到 lib/linux/miniaudio.so（2026-11-19 起库文件按平台分目录），
# 由 lib/music_core.leno 的 find_dll() 按 _os() 选中。
set -e

cd "$(dirname "$0")"

echo "Building miniaudio.so..."
gcc -shared -fPIC -O2 -o miniaudio.so miniaudio_dll.c -lm -lpthread -ldl

echo "Copying to lib/linux/..."
mkdir -p ../lib/linux
cp -f miniaudio.so ../lib/linux/miniaudio.so

echo "Done"