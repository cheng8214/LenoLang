#!/bin/bash
# ============================================================================
# build_android.sh —— 交叉编译 quirc 为 Android 共享库
# ----------------------------------------------------------------------------
# 产物（每个 ABI 一份，与 android/build_android.sh 的取库路径对齐）：
#     lib/android/arm64-v8a/libquirc.so
#     lib/android/x86_64/libquirc.so
#
# 为什么文件名是 `libquirc.so`（带 lib 前缀，和桌面的 `quirc.so` 不同）：
#   Android 的安装器只把 `<name>/lib/lib<name>.so` 这种**带 lib 前缀**的条目解成
#   可 dlopen 的库；不带前缀的会被当成普通文件留在 APK 里，dlopen 必失败。
#
# 用法：
#   ./build_android.sh                    # 两个 ABI 都编
#   ABIS="arm64-v8a" ./build_android.sh   # 只编一个
#   API=24 ./build_android.sh             # 抬 minSdk（默认 21，与 APK 的 minSdk 一致）
#   ANDROID_NDK_HOME=/path/to/ndk ./build_android.sh
#
# quirc 是纯 C、零外部依赖（只用 libm 的 sqrt）⇒ 不需要 vendored 依赖或 CMake 工程，
#   直接用 NDK 的 clang 包装器编两次即可。这也是它比 SDL3 / curl 容易得多的原因。
# ============================================================================
set -euo pipefail
cd "$(dirname "$0")"

DEFAULT_TOOLCHAIN="/c/android-toolchain"
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
    for c in "$DEFAULT_TOOLCHAIN"/android-ndk-*; do [ -d "$c" ] && NDK="$c"; done
fi
[ -n "${NDK:-}" ] && [ -d "$NDK" ] || { echo "找不到 NDK（设 ANDROID_NDK_HOME）"; exit 1; }

# 主机三元组：NDK 的 prebuilt 目录名随主机变，别写死 windows-x86_64
HOST_TAG=""
for h in windows-x86_64 linux-x86_64 darwin-x86_64; do
    if [ -d "$NDK/toolchains/llvm/prebuilt/$h" ]; then HOST_TAG="$h"; break; fi
done
[ -n "$HOST_TAG" ] || { echo "NDK 里找不到 prebuilt 目录: $NDK/toolchains/llvm/prebuilt/"; exit 1; }
BIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"

API="${API:-21}"
ABIS="${ABIS:-arm64-v8a x86_64}"

triple() {
    case "$1" in
        arm64-v8a) echo aarch64-linux-android ;;
        x86_64)    echo x86_64-linux-android ;;
        *)         echo "" ;;
    esac
}

# 目标文件放临时目录：**不要**放在 src/ —— 那是源码目录，留 .o 会被 git 看见
OBJ="$(mktemp -d)"
trap 'rm -rf "$OBJ"' EXIT

for ABI in $ABIS; do
    T="$(triple "$ABI")"
    [ -n "$T" ] || { echo "不支持的 ABI: $ABI"; exit 1; }
    CC="$BIN/${T}${API}-clang"
    [ -x "$CC" ] || [ -f "$CC.cmd" ] || { echo "找不到编译器: $CC"; exit 1; }

    OUT="../lib/android/$ABI"
    mkdir -p "$OUT"
    echo "== $ABI  ($T$API) =="

    # vendored quirc 源码：不加 -Wall（上游本身不是零告警，不为消告警去 fork 它）
    for f in quirc decode identify version_db; do
        "$CC" -c -fPIC -O2 "$f.c" -o "$OBJ/$f.o"
    done

    # 我们自己的包装层：严格告警 + 只导出 LQ_EXPORT 标注的符号
    "$CC" -c -fPIC -O2 -Wall -Wextra -fvisibility=hidden leno_qr.c -o "$OBJ/leno_qr.o"

    # -lm：quirc 的定位换算用 sqrt，Android 上 libm 是独立库（不像 glibc 那样并进 libc）
    "$CC" -shared -o "$OUT/libquirc.so" "$OBJ"/*.o -lm

    # 剥符号：quirc 的版本数据库占体积，strip 后约 60KB/ABI
    "$BIN/llvm-strip" --strip-unneeded "$OUT/libquirc.so" 2>/dev/null || true

    printf "   -> %s  %s bytes\n" "$OUT/libquirc.so" "$(wc -c < "$OUT/libquirc.so")"
done

echo "完成。映射关系："
echo "   lib/android/<abi>/libquirc.so  ->  APK 的 lib/<abi>/libquirc.so"
echo "   运行期由 qr_native.leno 的 lib_name() 按 _os()/abi_dir() 选中，"
echo "   命中不了就退回裸名 libquirc.so（APK 里就是这个位置）。"
