#!/bin/bash
# 编译 quirc 为 POSIX 共享库 —— build_dll.bat 的对应物（那边产出 quirc.dll）。
# 产物 lib/quirc.so；macOS 同样产 .so（clang 的 -shared 等价于 -dynamiclib，
# 与 LenoSDL3 的 mac = "lib/SDL3.so" 同一口径，dlopen 能直接加载 ✓）。
set -e

cd "$(dirname "$0")"

echo "Building quirc.so..."

# vendored quirc：不加 -Wall（上游本身不是零告警，不为消告警去 fork 它）
gcc -c -fPIC -O2 quirc.c decode.c identify.c version_db.c

# 我们自己的包装层：严格告警 + 只导出 LQ_EXPORT 标注的符号
gcc -c -fPIC -O2 -Wall -Wextra -fvisibility=hidden leno_qr.c

# 产物直接落 lib/（src/ 只放源码，不留构建产物）
gcc -shared -o ../lib/quirc.so quirc.o decode.o identify.o version_db.o leno_qr.o

rm -f *.o

echo "Done: lib/quirc.so"
