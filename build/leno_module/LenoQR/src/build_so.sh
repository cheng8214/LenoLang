#!/bin/sh
# 编译 quirc 为 POSIX 共享库 —— build_dll.bat 的对应物（那边产出 quirc.dll）。
# 产物落在 lib/linux/quirc.so（2026-11-19 起库文件按平台分目录）。
# macOS 同样产 .so（clang 的 -shared 等价于 -dynamiclib），但当前只往 linux/ 放；
#   要在 macOS 分发时改用 `OUT_DIR=../lib/mac` 再跑一次即可。
#
# 用 cc 而不是写死 gcc：macOS 没有 gcc（只有 clang 提供的 gcc 兼容入口），
# 而 cc 在 Linux/macOS 上一定存在。需要指定编译器时 `CC=clang ./build_so.sh`。
set -e

cd "$(dirname "$0")"
CC=${CC:-cc}
OUT_DIR=${OUT_DIR:-../lib/linux}

echo "Building quirc.so with $CC ..."

# vendored quirc：不加 -Wall（上游本身不是零告警，不为消告警去 fork 它）
$CC -c -fPIC -O2 quirc.c decode.c identify.c version_db.c

# 我们自己的包装层：严格告警 + 只导出 LQ_EXPORT 标注的符号
$CC -c -fPIC -O2 -Wall -Wextra -fvisibility=hidden leno_qr.c

mkdir -p "$OUT_DIR"
$CC -shared -o "$OUT_DIR/quirc.so" quirc.o decode.o identify.o version_db.o leno_qr.o

rm -f *.o

echo "Done: $OUT_DIR/quirc.so"
