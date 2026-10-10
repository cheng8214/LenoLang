#!/bin/bash
set -e

echo "Building LenoLang Compiler..."

mkdir -p build

# ---------------------------------------------------------------------------
# 源清单与 Windows 脚本**共用同一份**（单一事实来源；P9 的根治）：
#   sources_core.txt      —— 核心运行时（VM + 编译器都用）
#   sources_compiler.txt  —— 编译器专属（lexer/parser/semantic/codegen/package）
#   sources_vm.txt        —— VM 专属入口（vm_main.c；与 src/main.c 互斥）
# ⚠ **不要再往本文件里逐个加源文件**：改清单文件，否则两边会漂移 ——
#   历史上正是这样漏掉 `src/debug.c`（`opcode_name()` 的唯一定义点）与
#   `parser_eval_const_expr_text` 的，VM-only 构建直接链不过 ✗。
#   清单里写的是 Windows 风格的 `\` ⇒ 这里换成 `/`；顺带去掉 CR（清单可能 CRLF）。
# ---------------------------------------------------------------------------
read_list() {
  tr -d '\r' < "$1" | grep -v '^[[:space:]]*$' | tr '\\' '/'
}

SOURCES=""
for f in $(read_list sources_core.txt) $(read_list sources_compiler.txt); do
  SOURCES="$SOURCES $f"
done

# 平台检测
OS="$(uname -s 2>/dev/null || echo unknown)"
case "$OS" in
  MINGW*|MSYS*|CYGWIN*)  PLATFORM=windows ;;
  Darwin*)               PLATFORM=macos   ;;
  *)                     PLATFORM=linux   ;;
esac

EXE=""
LIBS="-lm"
if [ "$PLATFORM" = "windows" ]; then
  SOURCES="$SOURCES src/module/ffi/leno_ffi_win64.c"
  LIBS="$LIBS -municode -lws2_32"
  EXE=".exe"
elif [ "$PLATFORM" = "macos" ]; then
  # 检测架构：arm64 (AAPCS64) vs x86_64 (System V AMD64)
  ARCH="$(uname -m 2>/dev/null || echo x86_64)"
  if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
    SOURCES="$SOURCES src/module/ffi/leno_ffi_arm64.c"
  else
    SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  fi
  # macOS 与 Linux 在链接库上有两点不同（CI 上 macOS 任务就是逐个撞出来的）：
  #   ① 没有 libdl（dlopen/dlsym 属于 libSystem）⇒ 不能带 -ldl
  #      否则：ld: library not found for -ldl
  #   ② iconv / iconv_close 属**独立的 libiconv** ⇒ 必须带 -liconv
  #      否则（object_cstruct.c 的 utf16<->utf8 会引用它们）：
  #        Undefined symbols for architecture arm64: "_iconv", "_iconv_close"
  # glibc 把 iconv 内置在 libc 里，所以 Linux 侧这两条都不需要。
  LIBS="$LIBS -lpthread -liconv"
else
  # Linux: 检测架构
  ARCH="$(uname -m 2>/dev/null || echo x86_64)"
  if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
    SOURCES="$SOURCES src/module/ffi/leno_ffi_arm64.c"
  else
    SOURCES="$SOURCES src/module/ffi/leno_ffi_linux.c"
  fi
  LIBS="$LIBS -lpthread -ldl"
  CFLAGS="$CFLAGS -D_GNU_SOURCE"
fi

CC=${CC:-gcc}

# ---------------------------------------------------------------------------
# Windows 专属：把 leno_icon.ico（经 resources/leno.rc）编进 exe。
# 查找顺序：resources/leno_icon.ico → build/leno_icon.ico（与 .bat 保持一致）。
# 图标落在 PE 资源段里，而 -p 打包产物是"prepend 这个二进制 + 追加尾部数据"
# ⇒ 同一个 ico 自动覆盖所有打包产物。缺 .ico 不算错误（打印一行、照常构建）。
# Linux/macOS 不做：那边图标不是 PE 资源（macOS 用 .icns、Linux 靠 .desktop）。
# ---------------------------------------------------------------------------
ICON_ICO=""
if [ -f resources/leno_icon.ico ]; then
  ICON_ICO="resources/leno_icon.ico"
elif [ -f build/leno_icon.ico ]; then
  ICON_ICO="build/leno_icon.ico"
fi
ICON_OBJ=""
if [ "$PLATFORM" = "windows" ] && [ -n "$ICON_ICO" ]; then
  if ! windres resources/leno.rc -O coff -I resources -I build -o build/leno_icon.o; then
    echo "Icon resource build failed"
    exit 1
  fi
  ICON_OBJ="build/leno_icon.o"
  echo "Icon: $ICON_ICO"
fi

$CC $CFLAGS -o build/leno$EXE $SOURCES $ICON_OBJ -Isrc -Wall -Wextra -std=c99 -O2 $LIBS

# ---------------------------------------------------------------------------
# Linux 桌面图标（只有 Linux 走这一步）
# ---------------------------------------------------------------------------
# 为什么 Windows 那边一行就够、Linux 得另起一段：Windows 的图标是 **PE 资源段**
#   （上面 windres 那段）；而 **ELF 没有"内嵌图标"这个概念** ⇒ Linux 桌面是从
#   `.desktop` 文件（`Icon=` 指向 PNG/SVG）+ 图标主题里取图标的。此前非 Windows
#   分支一个字节都不处理 ⇒ 编译出来的 leno 在 Linux 上**任何地方都不会显示图标** ✗
#   （与 Windows 的 leno.exe 不对称 —— 这就是"为啥 Linux 上没图标"的答案）。
#
# 做法：把 build/leno_icon.ico 里的 PNG **抽出来**再注册：
#   该 .ico 是 tools/png_to_ico.leno 产出的 **PNG 容器**（22 字节 ICO 头 + 原 PNG 数据，
#   见 resources/leno.rc 的说明）⇒ 直接跳过前 22 字节就是那张 PNG ✓
#   然后装到用户目录（~/.local/share）⇒ 应用菜单 / 启动器里就有图标了 ✓
# ⚠ 只影响「应用菜单 / 启动器」：文件管理器里**单个 ELF 文件**依旧不会有自定义图标
#   （Linux 没有这个机制，不是没做）。
# ⚠ 抽不出 PNG（比如 .ico 里是 DIB 编码的条目）⇒ 打印一行并跳过，**绝不影响构建** ✓
if [ "$PLATFORM" = "linux" ]; then
  ICON_SRC=""
  if [ -f resources/leno_icon.ico ]; then
    ICON_SRC="resources/leno_icon.ico"
  elif [ -f build/leno_icon.ico ]; then
    ICON_SRC="build/leno_icon.ico"
  fi
  if [ -n "$ICON_SRC" ]; then
    # 校验"偏移 22 起是 PNG 签名"（89 50 4E 47 0D 0A 1A 0A）
    ICON_SIG=$(tail -c +23 "$ICON_SRC" | od -An -tx1 -N8 | tr -d ' \n')
    if [ "$ICON_SIG" = "89504e470d0a1a0a" ]; then
      ICON_DIR="$HOME/.local/share/icons/hicolor/48x48/apps"
      ICON_DESKTOP="$HOME/.local/share/applications"
      if mkdir -p "$ICON_DIR" "$ICON_DESKTOP" 2>/dev/null; then
        tail -c +23 "$ICON_SRC" > "$ICON_DIR/leno.png"
        cat > "$ICON_DESKTOP/leno.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=LenoLang
Comment=Leno language compiler
Exec=$(pwd)/build/leno %f
Icon=leno
Terminal=true
Categories=Development;
DESKTOP
        echo "Icon: 桌面图标已安装 → $ICON_DESKTOP/leno.desktop（PNG: $ICON_DIR/leno.png）"
      else
        echo "Icon: 装不进 $ICON_DESKTOP（只读或 HOME 异常），跳过桌面图标"
      fi
    else
      echo "Icon: $ICON_SRC 不是 PNG 容器，跳过桌面图标（Linux 只认 PNG/SVG）"
    fi
  fi
fi

echo "Build successful"
echo ""
echo "Usage: build/leno$EXE <file.leno>"
echo "Tests:  build/leno$EXE assert/run_tests.leno build/leno$EXE assert"
