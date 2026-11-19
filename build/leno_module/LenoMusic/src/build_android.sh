#!/usr/bin/env bash
# ============================================================================
# LenoMusic —— 把 miniaudio 交叉编译到 Android（arm64-v8a + x86_64）
#
# 产物：../lib/android/<abi>/libminiaudio.so
#   —— 与桌面侧 miniaudio.dll / miniaudio.so 是同一个 C 文件（miniaudio_dll.c）
#      编出来的，只是换了编译器和目标平台，没有平台专属代码分支。
#
# miniaudio 的 Android 后端：
#   · 播放/采集  → AAudio（API 26+）优先，退回 OpenSL ES（API 21+）
#   · 两者都是**运行时 dlopen** 系统库，所以链接阶段不需要 -lOpenSLES / -lAAudio，
#     只留 -lm -llog 即可（miniaudio 内部用 __android_log_print 打诊断）。
#
# 用法：
#   ./build_android.sh              # 双 ABI
#   ./build_android.sh x86_64       # 单 ABI
#   ANDROID_NDK_HOME=/path ./build_android.sh
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-/c/android-toolchain/android-ndk-r27c}"
API="${ANDROID_API:-21}"
ABIS="${1:-arm64-v8a x86_64}"

# NDK 的 clang 包装器按 <triple><api>-clang 命名
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
    Darwin)                HOST_TAG=darwin-x86_64 ;;
    *)                     HOST_TAG=linux-x86_64 ;;
esac
TOOLBIN="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
STRIP="$TOOLBIN/llvm-strip"
[ -x "$STRIP" ] || { echo "[x] NDK 无效：$NDK" >&2; exit 1; }

for ABI in $ABIS; do
    case "$ABI" in
        arm64-v8a)   TRIPLE=aarch64-linux-android ;;
        x86_64)      TRIPLE=x86_64-linux-android ;;
        armeabi-v7a) TRIPLE=armv7a-linux-androideabi ;;
        *) echo "[x] 不支持的 ABI: $ABI" >&2; exit 1 ;;
    esac
    CC="$TOOLBIN/${TRIPLE}${API}-clang"
    [ -x "$CC" ] || { echo "[x] 找不到编译器 $CC" >&2; exit 1; }

    OUTDIR="$HERE/../lib/android/$ABI"
    mkdir -p "$OUTDIR"
    echo "=== $ABI ($TRIPLE, API $API) ==="

    # -fvisibility=hidden + 显式 DLLEXPORT 已在 miniaudio_dll.c 里标好，
    #   所以这里不加 -fvisibility，免得把包装层导出的符号也藏掉。
    "$CC" -shared -fPIC -O2 -DNDEBUG \
        -o "$OUTDIR/libminiaudio.so" "$HERE/miniaudio_dll.c" \
        -lm -llog

    "$STRIP" --strip-unneeded "$OUTDIR/libminiaudio.so"
    printf "   + lib/android/%s/libminiaudio.so  %s bytes\n" "$ABI" "$(wc -c < "$OUTDIR/libminiaudio.so")"
done

echo
echo "完成。运行期由 lib/music_core.leno 的 rel_path() 选 android/<abi>/libminiaudio.so，"
echo "打包后（库被复制到 exe/lib 同目录）退回裸名 libminiaudio.so。"
