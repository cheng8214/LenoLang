#!/bin/sh
# 编译 quirc 为 POSIX 共享库 —— build_dll.bat 的对应物（那边产出 quirc.dll）。
# 产物 lib/quirc.so；macOS 同样产 .so（clang 的 -shared 等价于 -dynamiclib，
# 与 LenoSDL3 的 mac = "lib/SDL3.so" 同一口径，dlopen 能直接加载 ✓）。
#
# 用 cc 而不是写死 gcc：macOS 没有 gcc（只有 clang 提供的 gcc 兼容入口），
# 而 cc 在 Linux/macOS 上一定存在。需要指定编译器时 `CC=clang ./build_so.sh`。
set -e

cd "$(dirname "$0")"
CC=${CC:-cc}

echo "Building quirc.so with $CC ..."

# vendored quirc：不加 -Wall（上游本身不是零告警，不为消告警去 fork 它）
$CC -c -fPIC -O2 quirc.c decode.c identify.c version_db.c

# 我们自己的包装层：严格告警 + 只导出 LQ_EXPORT 标注的符号
$CC -c -fPIC -O2 -Wall -Wextra -fvisibility=hidden leno_qr.c

$CC -shared -o ../lib/quirc.so quirc.o decode.o identify.o version_db.o leno_qr.o

rm -f *.o

echo "Done: lib/quirc.so"
