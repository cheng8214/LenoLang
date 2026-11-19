#!/usr/bin/env bash
# ============================================================================
# LenoSDL3 —— 把 SDL3 / SDL3_image / SDL3_ttf 交叉编译到 Android（arm64-v8a + x86_64）
#
# 产物：../lib/android/<abi>/libSDL3.so、libSDL3_image.so、libSDL3_ttf.so
#   —— 这三个 .so 与 libleno.so 同目录分发，`ffi.load("libSDL3.so")` 走裸名命中。
#
# 依赖源码**全部随模块入库**（见 src/README.md 的版本清单），所以这个脚本
# 不需要联网、不需要 git submodule，拿到模块就能重编。
#
# 用法：
#   ./build_android.sh              # 双 ABI 全量
#   ./build_android.sh x86_64       # 只编一个 ABI
#   ANDROID_NDK_HOME=/path/to/ndk ./build_android.sh
#
# 环境变量：
#   ANDROID_NDK_HOME   NDK 根目录（默认 /c/android-toolchain/android-ndk-r27c）
#   ANDROID_API        目标 API（默认 21 —— SDL3 的下限，也是本包 minSdk）
#   JOBS               并行度（默认 8）
#   CMAKE               cmake 可执行文件
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-/c/android-toolchain/android-ndk-r27c}"
API="${ANDROID_API:-21}"
JOBS="${JOBS:-8}"

# ---- 工具定位 -------------------------------------------------------------
# ⚠ cmake 在 Windows 上通常装在 "C:\Program Files\CMake\bin"，**路径含空格和括号**。
#   一旦把它交给 make 去调用（`$(CMAKE_COMMAND)` 展开后不带引号），
#   mingw32-make 会通过 sh 执行 → `syntax error near unexpected token '('`。
#   所以下面所有对 cmake / make 的调用都走**数组 + 引号**，不用裸变量拼接。
if [ -n "${CMAKE:-}" ]; then
    CMAKE_BIN="$CMAKE"
elif command -v cmake >/dev/null 2>&1; then
    CMAKE_BIN="$(command -v cmake)"
elif [ -x "/c/Program Files/CMake/bin/cmake.exe" ]; then
    CMAKE_BIN="/c/Program Files/CMake/bin/cmake.exe"
else
    echo "[x] 找不到 cmake" >&2
    exit 1
fi

TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN_FILE" ] || { echo "[x] NDK 无效：$NDK" >&2; exit 1; }

# 生成器：优先 ninja（快），没有就退回 GNU make。
# make 也必须是**不含空格的路径**，理由同上 —— 优先用 /c/android-toolchain/bin/make.exe
# （由本仓库的安装脚本从 mingw64 复制过去），否则临时复制一份。
GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GEN_ARGS=(-G Ninja)
    echo "   generator: Ninja ($(command -v ninja))"
else
    MAKE_BIN=""
    for cand in /c/android-toolchain/bin/make.exe /usr/bin/make /usr/local/bin/make; do
        [ -x "$cand" ] && { MAKE_BIN="$cand"; break; }
    done
    if [ -z "$MAKE_BIN" ]; then
        for cand in "/c/Program Files (x86)/mingw64/bin/mingw32-make.exe" \
                    "/c/Program Files/mingw64/bin/mingw32-make.exe"; do
            if [ -x "$cand" ]; then
                mkdir -p /c/android-toolchain/bin
                cp -f "$cand" /c/android-toolchain/bin/make.exe
                cp -f "$(dirname "$cand")/libwinpthread-1.dll" /c/android-toolchain/bin/ 2>/dev/null || true
                MAKE_BIN=/c/android-toolchain/bin/make.exe
                break
            fi
        done
    fi
    [ -n "$MAKE_BIN" ] || { echo "[x] 找不到 ninja 或 make" >&2; exit 1; }
    GEN_ARGS=(-G "Unix Makefiles" "-DCMAKE_MAKE_PROGRAM=$MAKE_BIN")
    echo "   generator: Unix Makefiles ($MAKE_BIN)"
fi

STRIP="$(dirname "$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)")/llvm-strip"
[ -x "$STRIP" ] || { echo "[x] 找不到 llvm-strip" >&2; exit 1; }

echo "   cmake  : $CMAKE_BIN"
echo "   ndk    : $NDK"
echo "   api    : $API"
echo

# ---- 公共 CMake 参数 -----------------------------------------------------
common_args() {
    local abi="$1" builddir="$2"
    printf '%s\n' \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON
}

# 统一构建函数：$1=源码目录 $2=构建目录 $3=ABI $4..=额外 cmake 参数
build_one() {
    local srcdir="$1" builddir="$2" abi="$3"; shift 3
    local -a extra=("$@")
    local -a cflags=()

    # 去掉调试信息：SDL3 的 .so 未 strip 时 11MB，strip 后约 3MB（ttf 18MB→4MB）
    cflags+=("-DCMAKE_C_FLAGS_RELEASE=-O2 -DNDEBUG -g0")
    cflags+=("-DCMAKE_CXX_FLAGS_RELEASE=-O2 -DNDEBUG -g0")

    mkdir -p "$builddir"
    local -a common
    mapfile -t common < <(common_args "$abi" "$builddir")

    # ⚠ 用数组展开 + 引号：路径里有空格（"Program Files"）也不会被 sh 拆坏
    "$CMAKE_BIN" -S "$srcdir" -B "$builddir" "${GEN_ARGS[@]}" \
        "${common[@]}" "${cflags[@]}" "${extra[@]}" > "$builddir/configure.log" 2>&1 \
        || { echo "[x] configure 失败：$srcdir（见 $builddir/configure.log）" >&2; tail -25 "$builddir/configure.log" >&2; exit 1; }

    if [ "${GEN_ARGS[0]}" = "-G" ] && [ "${GEN_ARGS[1]}" = "Ninja" ]; then
        ninja -C "$builddir" -j "$JOBS" > "$builddir/build.log" 2>&1 \
            || { echo "[x] build 失败：$srcdir（见 $builddir/build.log）" >&2; tail -25 "$builddir/build.log" >&2; exit 1; }
    else
        "$MAKE_BIN" -C "$builddir" -j "$JOBS" > "$builddir/build.log" 2>&1 \
            || { echo "[x] build 失败：$srcdir（见 $builddir/build.log）" >&2; tail -25 "$builddir/build.log" >&2; exit 1; }
    fi
}

# ---- 主流程 --------------------------------------------------------------
ABIS="${1:-arm64-v8a x86_64}"
FOUND_OUT=""
for ABI in $ABIS; do
    echo "==================== $ABI ===================="
    BUILDS="$HERE/build/$ABI"
    LIBOUT="$HERE/../lib/android/$ABI"
    mkdir -p "$LIBOUT"

    # --- 1) SDL3 ---------------------------------------------------------
    echo "[1/3] SDL3"
    build_one "$HERE/sdl3" "$BUILDS/sdl3" "$ABI" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL=OFF \
        -DSDL_UNIX_CONSOLE_BUILD=OFF

    # --- 2) SDL3_image ---------------------------------------------------
    # 依赖全部走 vendored（external/ 随模块入库），故**不需要系统里装 libpng 等**。
    # 关掉 AVIF/JXL/TIFF/SVG：它们的依赖（aom、dav1d、libjxl、libtiff）会让
    # .so 体积翻几倍，而 Leno 这边用不到。
    echo "[2/3] SDL3_image"
    build_one "$HERE/sdl3_image" "$BUILDS/sdl3_image" "$ABI" \
        -DSDLIMAGE_VENDORED=ON \
        -DSDLIMAGE_AVIF=OFF -DSDLIMAGE_JXL=OFF -DSDLIMAGE_TIF=OFF -DSDLIMAGE_SVG=OFF \
        -DSDLIMAGE_SAMPLES=OFF -DSDLIMAGE_TESTS=OFF \
        -DSDL3_DIR="$BUILDS/sdl3" -DCMAKE_PREFIX_PATH="$BUILDS/sdl3"

    # --- 3) SDL3_ttf -----------------------------------------------------
    echo "[3/3] SDL3_ttf"
    build_one "$HERE/sdl3_ttf" "$BUILDS/sdl3_ttf" "$ABI" \
        -DSDLTTF_VENDORED=ON -DSDLTTF_PLUTOSVG=OFF -DSDLTTF_SAMPLES=OFF \
        -DSDL3_DIR="$BUILDS/sdl3" -DCMAKE_PREFIX_PATH="$BUILDS/sdl3"

    # --- 4) strip 并落到模块 lib/android/<abi>/ --------------------------
    for so in libSDL3.so libSDL3_image.so libSDL3_ttf.so; do
        src=""
        for cand in "$BUILDS/sdl3/$so" "$BUILDS/sdl3_image/$so" "$BUILDS/sdl3_ttf/$so"; do
            [ -f "$cand" ] && { src="$cand"; break; }
        done
        [ -n "$src" ] || { echo "[x] 没找到 $so" >&2; exit 1; }
        "$STRIP" --strip-unneeded "$src" -o "$LIBOUT/$so"
        printf "   + lib/android/%s/%-18s %8s bytes\n" "$ABI" "$so" "$(wc -c < "$LIBOUT/$so")"
    done
    FOUND_OUT="$LIBOUT"
done

echo
echo "完成。产物目录："
echo "  $FOUND_OUT"
echo
echo "提醒：这些 .so 与 libleno.so 一起由 android/build_android.sh 打进 APK 的 lib/<abi>/，"
echo "      运行时 \`ffi.load(\"libSDL3.so\")\` 靠裸文件名命中（见模块 lib/README.md）。"
