#!/bin/bash
# ============================================================================
# build_android.sh —— 把 Leno VM 编成 Android 的 libleno.so 并打成一个可安装 APK
# ----------------------------------------------------------------------------
# 为什么手工打包（不用 Gradle）：这里只需要 aapt2 + javac + d8 + apksigner 四步，
#   每一步都能单独看产物；Gradle/AGP 会额外拖 1GB 依赖和一整套版本矩阵，
#   而本仓库要验证的只是"VM 能不能在 Android 上跑起来"，不是工程脚手架。
#
# 用法:
#   bash android/build_android.sh                # 编译 VM + 打包 APK
#   bash android/build_android.sh --so-only      # 只出 .so（移植调试时用，最快）
#   ANDROID_NDK_HOME=... ANDROID_HOME=... bash android/build_android.sh
# ============================================================================
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ANDROID_DIR="$ROOT/android"
APP="$ANDROID_DIR/app"
OUT="$ANDROID_DIR/build"
mkdir -p "$OUT"

DEFAULT_TOOLCHAIN="/c/android-toolchain"
NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
    for c in "$DEFAULT_TOOLCHAIN"/android-ndk-*; do [ -d "$c" ] && NDK="$c"; done
fi
SDK="${ANDROID_HOME:-$DEFAULT_TOOLCHAIN/sdk}"
JDK="${JAVA_HOME:-}"
if [ -z "$JDK" ]; then
    for c in "$DEFAULT_TOOLCHAIN"/jdk-*; do [ -d "$c" ] && JDK="$c"; done
fi
BT="$SDK/build-tools/34.0.0"
API=21

[ -d "$NDK" ] || { echo "找不到 NDK（设 ANDROID_NDK_HOME）"; exit 1; }
[ -d "$BT" ]  || { echo "找不到 build-tools/34.0.0（设 ANDROID_HOME）"; exit 1; }

# 目标 ABI 表。arm64-v8a 是唯一要发布的；x86_64 只用于在 x86 主机模拟器上验证
# （本机没有 ARM 设备时，只有它能真跑起来 —— 见文末 README 的说明）。
# FFI 实现按**调用约定**选，不按平台名选：
#   aarch64 → AAPCS64  (leno_ffi_arm64.c)
#   x86_64  → System V AMD64 (leno_ffi_linux.c)，Android x86_64 与 Linux 同一套
ABIS="${ABIS:-arm64-v8a x86_64}"

abi_triple() {
    case "$1" in
        arm64-v8a) echo aarch64-linux-android ;;
        x86_64)    echo x86_64-linux-android ;;
        *)         echo "" ;;
    esac
}

abi_ffi() {
    case "$1" in
        arm64-v8a*) echo src/module/ffi/leno_ffi_arm64.c ;;
        *)          echo src/module/ffi/leno_ffi_linux.c ;;
    esac
}

NDKBIN="$NDK/toolchains/llvm/prebuilt/windows-x86_64/bin"
[ -d "$NDKBIN" ] || NDKBIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"

echo "== 1/6 交叉编译 libleno.so (API $API, $ABIS) =="
# 源清单与桌面构建**共用同一份**（sources_core.txt / sources_vm.txt）——
# 这是本仓库的既有约定：新增源文件只改清单，不改各平台脚本。
SRC=$(cd "$ROOT" && cat sources_core.txt sources_vm.txt \
      | tr -d '\r' | grep -v '^[[:space:]]*$' | tr '\\' '/')

for ABI in $ABIS; do
    TRI="$(abi_triple "$ABI")"
    [ -n "$TRI" ] || { echo "未知 ABI: $ABI"; exit 1; }
    CC="$NDKBIN/${TRI}${API}-clang"
    [ -x "$CC" ] || { echo "找不到 NDK clang: $CC"; exit 1; }
    mkdir -p "$OUT/$ABI"
    SO="$OUT/$ABI/libleno.so"

    # shellcheck disable=SC2086
    "$CC" -shared -fPIC -O2 -std=c99 \
        -DLENO_VM_ONLY \
        -Isrc -Isrc/include \
        $SRC \
        "$(abi_ffi "$ABI")" \
        "$APP/jni/leno_jni.c" \
        -o "$SO" -lm -ldl -llog

    # 压缩调试符号：不 strip 的 .so 会大 5~10 倍
    "$NDKBIN/llvm-strip" --strip-unneeded "$SO" 2>/dev/null || true
    ls -lh "$SO" | awk -v a="$ABI" '{print "   ->", a, $5}'
done

if [ "${1:-}" = "--so-only" ]; then echo "只出 .so，结束"; exit 0; fi

echo "== 1.5/6 编译 assets/*.leno -> *.lenb（桌面编译器） =="
# 为什么不复用目录里现成的 .lenb：**陈旧字节码是静默错误**。
#   实测踩过：assets 里躺着一份几天前的 .lenb，源码早改了，APK 装上去跑的还是旧逻辑，
#   而屏幕上只会看到"结果不对"，没有任何"你编译的是旧文件"的提示。
# 因此只要有桌面编译器就**每次重编**；没有才退回用现成的（并明确提示）。
HOST_LENO="$ROOT/build/leno.exe"
[ -x "$HOST_LENO" ] || HOST_LENO="$ROOT/build/leno"
if [ -x "$HOST_LENO" ]; then
    ( cd "$APP/assets" && rm -rf .lenocache && for src in *.leno; do
        [ -f "$src" ] || continue
        "$HOST_LENO" -c "$src" > "$OUT/lenoc.log" 2>&1 || {
            cat "$OUT/lenoc.log"; echo "编译 $src 失败"; exit 1; }
        # 警告（例如"内置模块被同名文件遮蔽"）**必须显式打出来**，不能吞进日志
        grep -E "warning:|\[错误\]" "$OUT/lenoc.log" | sed "s|^|   |" || true
    done )
    ls "$APP/assets"/*.lenb | sed 's|.*/|   -> |'
else
    echo "   （没找到桌面编译器 build/leno*，直接使用 assets 里现有的 .lenb）"
fi

echo "== 2/6 生成 R.java + 资源链接 =="
rm -rf "$OUT/gen" "$OUT/base.apk"
mkdir -p "$OUT/gen"
"$BT/aapt2" link \
    -I "$SDK/platforms/android-34/android.jar" \
    -A "$APP/assets" \
    --manifest "$APP/AndroidManifest.xml" \
    --java "$OUT/gen" \
    --min-sdk-version $API --target-sdk-version 34 \
    --auto-add-overlay \
    -o "$OUT/base.apk"

echo "== 2.5/6 修正 assets 条目名 =="
# aapt2 在 Windows 上递归打 assets 子目录时，条目名用的是**系统分隔符**：
#     assets/lmusic\images\about.png        （unzip -l 能直接看到反斜杠）
# 而 AssetManager 按 '/' 切路径查 zip ⇒ list("lmusic/images") 为空，
# open() 抛 FileNotFoundException。平铺 assets 没分隔符，所以以前没暴露。
# 改名不改长度（'\' → '/' 都是 1 字节）⇒ 偏移/CRC/压缩数据全不动，见 assetpaths.leno。
LENO_TP="$ROOT/build/leno.exe"
[ -x "$LENO_TP" ] || LENO_TP="$ROOT/build/leno"
if [ -x "$LENO_TP" ]; then
    "$LENO_TP" "$ANDROID_DIR/tools/assetpaths.leno" "$OUT/base.apk"
else
    echo "   ! 没有桌面 leno，跳过（带子目录的 assets 在设备上会读不到）"
fi

echo "== 3/6 编译 Java =="
rm -rf "$OUT/classes" && mkdir -p "$OUT/classes"
# SDL3 的 Java 宿主类（org.libsdl.app.*）必须打进 APK —— 见 SdlActivity.java 顶部注释：
#   libSDL3.so 的 JNI_OnLoad 会 FindClass("org/libsdl/app/SDLActivity") 并 RegisterNatives，
#   宿主里没有这个类就直接崩（ART abort）。源码就在模块里，不额外下载。
SDL_JAVA="$ROOT/build/leno_module/LenoSDL3/src/sdl3/android-project/app/src/main/java"
[ -d "$SDL_JAVA" ] || { echo "找不到 SDL3 的 Java 宿主源码: $SDL_JAVA"; exit 1; }
# 注意：**不能**用 -bootclasspath android.jar —— android.jar 里的
# java.lang.invoke.LambdaMetafactory 是空壳（缺 metafactory 方法签名），
# lambda 一编译就报 "Unable to find method metafactory"。放 -classpath 上，
# java.* 走 JDK 自己的、android.* 走 android.jar，d8 负责把 lambda desugar 掉。
"$JDK/bin/javac" -J-Duser.language=en -J-Duser.country=US \
    -source 8 -target 8 -nowarn -encoding UTF-8 \
    -classpath "$SDK/platforms/android-34/android.jar" \
    -d "$OUT/classes" \
    $(find "$APP/java" "$OUT/gen" "$SDL_JAVA" -name '*.java') > "$OUT/javac.log" 2>&1 || {
        cat "$OUT/javac.log"; echo "javac 失败"; exit 1; }
echo "   java 源文件: $(find "$APP/java" "$SDL_JAVA" -name '*.java' | wc -l) 个"

echo "== 4/6 dex（d8） =="
rm -rf "$OUT/dex" && mkdir -p "$OUT/dex"
"$JDK/bin/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 \
    --lib "$SDK/platforms/android-34/android.jar" \
    --min-api $API \
    --output "$OUT/dex" \
    $(find "$OUT/classes" -name '*.class')

echo "== 5/6 组装 APK（dex + .so + assets） =="
# assets 已由 aapt2 -A 放进 base.apk；这里补 dex 与 native 库。
# .so 用 STORED（不压缩）：extractNativeLibs=true 时安装器直接把它落到 /data/app/.../lib/
cp "$OUT/base.apk" "$OUT/unsigned.apk"
# 注入器是 Leno 自己写的（android/tools/apkadd.leno）——打包链路不再依赖 python：
#   已有条目按**原始字节**搬运（不解压、不重压）⇒ 17MB 的包 0.4 秒，
#   且 aapt2 的压缩产物原样保留（不膨胀）。
LENO="$ROOT/build/leno.exe"
[ -x "$LENO" ] || { echo "找不到 $LENO —— 先跑 bash build.sh 生成 leno.exe"; exit 1; }
"$LENO" "$ANDROID_DIR/tools/apkadd.leno" "$OUT/unsigned.apk" deflate \
    "$OUT/dex/classes.dex" classes.dex
for ABI in $ABIS; do
    "$LENO" "$ANDROID_DIR/tools/apkadd.leno" "$OUT/unsigned.apk" stored \
        "$OUT/$ABI/libleno.so" "lib/$ABI/libleno.so"

    # 第三方 native 库随包打入。**源的优先级**：
    #   1) 各模块自带的按 ABI 目录：build/leno_module/<Mod>/lib/android/<abi>/*.so
    #      —— 这是**唯一真实源**：库和用它的 .leno 模块放在一起，
    #         谁拿到那个模块谁就能自己打 APK，不必再向仓库另要一份二进制。
    #         现有：LenoSqlite（libsqlite3.so）、LenoQR（libquirc.so）
    #         （2026-11-19 起这里从"只认 LenoSqlite"改成**遍历所有模块**，
    #           否则新增一个带 Android 产物的模块就得回来改这个脚本）
    #   2) 兜底：app/jniLibs/<abi>/*.so（旧布局；现在这里是构建产物，已 gitignore）
    # 为什么必须自带：Android 只允许 dlopen public.libraries.txt 里的系统库，
    #   libsqlite.so 不在白名单（实测报 "not accessible for the namespace
    #   classloader-namespace"）⇒ 想让 Leno 用这些库只能自己带一份。
    # 注意 .so 必须 STORED（不压缩）+ 名字带 lib 前缀，否则安装器不会解成可加载的库。
    PKG_DIRS=""
    for mdir in "$ROOT"/build/leno_module/*/lib/android/"$ABI"; do
        [ -d "$mdir" ] && PKG_DIRS="$PKG_DIRS $mdir"
    done
    [ -n "$PKG_DIRS" ] || { [ -d "$APP/jniLibs/$ABI" ] && PKG_DIRS="$APP/jniLibs/$ABI"; }
    if [ -n "$PKG_DIRS" ]; then
        PACKED=""
        for PKG_LIBS in $PKG_DIRS; do
            for so in "$PKG_LIBS"/*.so; do
                [ -f "$so" ] || continue
                NAME="$(basename "$so")"
                # 两个模块带同名库时只打第一份（apkadd.leno 遇到重复条目会直接报错退出）
                case " $PACKED " in *" $NAME "*) echo "   ~ 跳过重复的 $NAME（来自 $(echo "$PKG_LIBS" | sed "s|$ROOT/||")）"; continue;; esac
                PACKED="$PACKED $NAME"

                ST="$OUT/$ABI/$NAME"
                if [ -x "$NDKBIN/llvm-strip" ]; then
                    "$NDKBIN/llvm-strip" --strip-unneeded "$so" -o "$ST" 2>/dev/null || cp "$so" "$ST"
                else
                    cp "$so" "$ST"
                fi
                "$LENO" "$ANDROID_DIR/tools/apkadd.leno" "$OUT/unsigned.apk" stored \
                    "$ST" "lib/$ABI/$NAME"
                echo "   + lib/$ABI/$NAME  $(wc -c < "$ST") bytes  ← $(echo "$PKG_LIBS" | sed "s|$ROOT/||")"
            done
        done
    else
        echo "   ! $ABI 没有可打包的第三方 .so（既无模块 lib/android/$ABI/，也无 app/jniLibs/$ABI/）"
    fi
done

echo "== 6/6 zipalign + 签名 =="
KS="$OUT/debug.keystore"
if [ ! -f "$KS" ]; then
    "$JDK/bin/keytool" -genkeypair -keystore "$KS" -storepass android -keypass android \
        -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$JDK/bin/java" -jar "$BT/lib/apksigner.jar" sign \
    --ks "$KS" --ks-pass pass:android --key-pass pass:android \
    --v1-signing-enabled true --v2-signing-enabled true \
    --out "$OUT/leno-demo.apk" "$OUT/aligned.apk"

"$JDK/bin/java" -jar "$BT/lib/apksigner.jar" verify --print-certs "$OUT/leno-demo.apk" | head -4
ls -lh "$OUT/leno-demo.apk" | awk '{print "APK:", $9, $5}'
echo "完成: $OUT/leno-demo.apk"
