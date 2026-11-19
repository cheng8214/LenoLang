#!/usr/bin/env bash
# ============================================================================
# LenoWeb —— 把 curl（+ mbedTLS）交叉编译到 Android（arm64-v8a + x86_64）
#
# 产物：../lib/android/<abi>/libcurl.so
#
# 为什么自己编 TLS：Android 没有给 app 用的系统 TLS 库 —— 系统自带的
#   `libssl.so`（BoringSSL）**不在** `public.libraries.txt` 白名单里，
#   dlopen 会被 namespace 拒（SQLite 那边踩过同一个坑）。所以 curl 的 TLS
#   后端必须静态链进来。这里选 mbedTLS：体积小、CMake 好交叉编，
#   而且和随包的 Windows `libcurl-x64.dll` 用的是同一个后端（mbedTLS）。
#
# 证书从哪来：不随包塞 cacert.pem（会过期）。运行时由 .leno 侧把
#   /system/etc/security/cacerts/*.0 拼成一个 PEM bundle，再设 CURLOPT_CAINFO。
#   见 lib/web_curl_core.leno 的 ca_bundle()。
#
# 用法：
#   ./build_android.sh              # 双 ABI
#   ./build_android.sh x86_64
#   ANDROID_NDK_HOME=/path ./build_android.sh
# ============================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
NDK="${ANDROID_NDK_HOME:-/c/android-toolchain/android-ndk-r27c}"
API="${ANDROID_API:-21}"
JOBS="${JOBS:-8}"
ABIS="${1:-arm64-v8a x86_64}"

if [ -n "${CMAKE:-}" ]; then CMAKE_BIN="$CMAKE"
elif command -v cmake >/dev/null 2>&1; then CMAKE_BIN="$(command -v cmake)"
elif [ -x "/c/Program Files/CMake/bin/cmake.exe" ]; then CMAKE_BIN="/c/Program Files/CMake/bin/cmake.exe"
else echo "[x] 找不到 cmake" >&2; exit 1; fi

TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
[ -f "$TOOLCHAIN_FILE" ] || { echo "[x] NDK 无效：$NDK" >&2; exit 1; }

GEN_ARGS=()
if command -v ninja >/dev/null 2>&1; then
    GEN_ARGS=(-G Ninja); RUNNER=(ninja)
else
    MAKE_BIN=""
    for cand in /c/android-toolchain/bin/make.exe /usr/bin/make /usr/local/bin/make; do
        [ -x "$cand" ] && { MAKE_BIN="$cand"; break; }
    done
    [ -n "$MAKE_BIN" ] || { echo "[x] 找不到 ninja 或 make" >&2; exit 1; }
    GEN_ARGS=(-G "Unix Makefiles" "-DCMAKE_MAKE_PROGRAM=$MAKE_BIN"); RUNNER=("$MAKE_BIN")
fi

STRIP="$(dirname "$(ls "$NDK"/toolchains/llvm/prebuilt/*/bin/llvm-strip | head -1)")/llvm-strip"

# mbedTLS 的 GEN_FILES=ON 要用 Perl（generate_errors.pl）和 Python（生成 ssl_debug_helpers）。
# Git for Windows 自带 perl，但**不在 PATH 里** ⇒ 显式找出来补进去，否则
# find_package(Perl REQUIRED) 会直接失败。
if ! command -v perl >/dev/null 2>&1; then
    for cand in "/c/Program Files/Git/usr/bin" "/c/Program Files/Git/mingw64/bin"; do
        if [ -x "$cand/perl.exe" ]; then
            PATH="$cand:$PATH"; export PATH
            echo "   perl : $cand/perl.exe（临时加入 PATH）"
            break
        fi
    done
fi
command -v perl >/dev/null 2>&1 || { echo "[!] 找不到 perl —— mbedTLS 的 GEN_FILES 会失败" >&2; }

# mbedTLS 3.6 的源码树（不是 release 压缩包）**不含生成文件**，必须 GEN_FILES=ON 现场生成：
#   error.c / version_features.c        → Perl
#   ssl_debug_helpers_generated.c       → Python
#   psa_crypto_driver_wrappers.{h,c}    → Python + jsonschema + jinja2
# 少一个就只会在 build 阶段报 "No module named ..."，很难猜。
if command -v python >/dev/null 2>&1; then
    if ! python -c "import jsonschema, jinja2" >/dev/null 2>&1; then
        echo "[!] 缺 python 包 jsonschema/jinja2（mbedTLS 生成 PSA 包装层要用）"
        echo "    装法：python -m pip install jsonschema jinja2"
        exit 1
    fi
    # ★ 把 CMake 的解释器**钉死**在刚检查过的这一个上。
    #   不钉的话 CMake 会自己去 PATH 里挑，可能挑到另一个没装 jsonschema 的 Python
    #   （实测：脚本用的是 3.13.12，CMake 挑到 Program Files 下的 3.13.2 ⇒ build 阶段才报错）。
    PY_EXE="$(python -c 'import sys; print(sys.executable)')"
    echo "   python: $PY_EXE"
fi

echo "   cmake: $CMAKE_BIN"
echo "   ndk  : $NDK"
echo "   api  : $API"
echo

run_build() {   # $1 = build dir
    local bd="$1"
    if [ "${GEN_ARGS[1]}" = "Ninja" ]; then
        ninja -C "$bd" -j "$JOBS" > "$bd/build.log" 2>&1 || { tail -30 "$bd/build.log" >&2; return 1; }
    else
        "$MAKE_BIN" -C "$bd" -j "$JOBS" > "$bd/build.log" 2>&1 || { tail -30 "$bd/build.log" >&2; return 1; }
    fi
}

for ABI in $ABIS; do
    echo "==================== $ABI ===================="
    BUILDS="$HERE/build/$ABI"
    LIBOUT="$HERE/../lib/android/$ABI"
    mkdir -p "$LIBOUT"

    # --- 1) mbedTLS（静态库，供 curl 静态链入）---------------------------
    echo "[1/2] mbedTLS (static)"
    mkdir -p "$BUILDS/mbedtls"
    "$CMAKE_BIN" -S "$HERE/mbedtls" -B "$BUILDS/mbedtls" "${GEN_ARGS[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
        -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF -DMBEDTLS_FATAL_WARNINGS=OFF \
        -DUSE_SHARED_MBEDTLS_LIBRARY=OFF -DUSE_STATIC_MBEDTLS_LIBRARY=ON \
        -DGEN_FILES=ON \
        ${PY_EXE:+-DPython3_EXECUTABLE=$PY_EXE} \
        > "$BUILDS/mbedtls/configure.log" 2>&1 \
        || { echo "[x] mbedtls configure 失败" >&2; tail -20 "$BUILDS/mbedtls/configure.log" >&2; exit 1; }
    run_build "$BUILDS/mbedtls" || { echo "[x] mbedtls build 失败" >&2; exit 1; }

    MBEDLIB="$BUILDS/mbedtls/library"
    ls "$MBEDLIB"/libmbedtls.a "$MBEDLIB"/libmbedx509.a "$MBEDLIB"/libmbedcrypto.a >/dev/null \
        || { echo "[x] mbedtls 静态库缺失" >&2; exit 1; }

    # --- 2) curl（shared，TLS 静态链入）----------------------------------
    echo "[2/2] curl (shared, mbedTLS inside)"
    mkdir -p "$BUILDS/curl"
    "$CMAKE_BIN" -S "$HERE/curl" -B "$BUILDS/curl" "${GEN_ARGS[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN_FILE" -DANDROID_ABI="$ABI" \
        -DANDROID_PLATFORM="android-$API" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG -g0" \
        -DBUILD_SHARED_LIBS=ON -DBUILD_CURL_EXE=OFF -DBUILD_EXAMPLES=OFF \
        -DBUILD_TESTING=OFF -DUSE_MANUAL=OFF \
        -DCURL_USE_MBEDTLS=ON -DCURL_USE_OPENSSL=OFF \
        -DMBEDTLS_INCLUDE_DIR="$HERE/mbedtls/include" \
        -DMBEDTLS_LIBRARY="$MBEDLIB/libmbedtls.a" \
        -DMBEDX509_LIBRARY="$MBEDLIB/libmbedx509.a" \
        -DMBEDCRYPTO_LIBRARY="$MBEDLIB/libmbedcrypto.a" \
        -DCURL_USE_LIBPSL=OFF -DUSE_NGHTTP2=OFF -DCURL_USE_LIBSSH2=OFF \
        -DCURL_ZLIB=OFF -DCURL_BROTLI=OFF -DCURL_ZSTD=OFF \
        -DCURL_DISABLE_LDAP=ON -DCURL_DISABLE_LDAPS=ON \
        -DCURL_DISABLE_SMB=ON -DCURL_DISABLE_TELNET=ON -DCURL_DISABLE_GOPHER=ON \
        -DENABLE_THREADED_RESOLVER=ON \
        > "$BUILDS/curl/configure.log" 2>&1 \
        || { echo "[x] curl configure 失败" >&2; tail -25 "$BUILDS/curl/configure.log" >&2; exit 1; }
    run_build "$BUILDS/curl" || { echo "[x] curl build 失败" >&2; exit 1; }

    # 产物路径：`$BUILDS/curl/lib/libcurl.so`（curl 把库放在多一层 lib/ 里）。
    # ⚠ 别写成 `SRC_SO="$(ls ...)"`：set -e 下**命令替换失败会让整个脚本静默退出**，
    #   表现是只打印到 "[2/2]" 就没了、退出码还是 0 —— 极难定位。所以先赋值路径再判 -f。
    SRC_SO="$BUILDS/curl/lib/libcurl.so"
    if [ ! -f "$SRC_SO" ]; then
        SRC_SO="$(find "$BUILDS/curl" -maxdepth 2 -name 'libcurl.so' 2>/dev/null | head -1 || true)"
    fi
    [ -n "${SRC_SO:-}" ] && [ -f "$SRC_SO" ] || { echo "[x] 没找到 libcurl.so（预期 $BUILDS/curl/lib/）" >&2; exit 1; }
    "$STRIP" --strip-unneeded "$SRC_SO" -o "$LIBOUT/libcurl.so"
    printf "   + lib/android/%s/libcurl.so  %s bytes\n" "$ABI" "$(wc -c < "$LIBOUT/libcurl.so")"
done

echo
echo "完成。注意 TLS 证书：不随包，运行时由 lib/web_curl_core.leno 从"
echo "      /system/etc/security/cacerts 拼 PEM bundle 后设 CURLOPT_CAINFO。"
