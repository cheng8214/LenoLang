#!/bin/bash
set -e

echo "Building Leno LSP Server with LenoC compiler..."

mkdir -p build

# Delete old executable if exists
if [ -f build/leno_lsp ]; then
    rm -f build/leno_lsp
fi

# LSP source files
LSP_SOURCES=""
LSP_SOURCES="$LSP_SOURCES lsp_server.c"
LSP_SOURCES="$LSP_SOURCES lsp_protocol.c"
LSP_SOURCES="$LSP_SOURCES lsp_document.c"
LSP_SOURCES="$LSP_SOURCES lsp_diagnostic.c"
LSP_SOURCES="$LSP_SOURCES lsp_document_symbol.c"
LSP_SOURCES="$LSP_SOURCES lsp_signature.c"
LSP_SOURCES="$LSP_SOURCES lsp_references.c"
LSP_SOURCES="$LSP_SOURCES lsp_folding.c"
LSP_SOURCES="$LSP_SOURCES lsp_complete.c"
LSP_SOURCES="$LSP_SOURCES lsp_hover.c"
LSP_SOURCES="$LSP_SOURCES lsp_definition.c"
LSP_SOURCES="$LSP_SOURCES json.c"
LSP_SOURCES="$LSP_SOURCES leno_compiler_lib.c"

# 模块化补全引擎
LSP_SOURCES="$LSP_SOURCES comp_set.c"
LSP_SOURCES="$LSP_SOURCES comp_context.c"
LSP_SOURCES="$LSP_SOURCES comp_import.c"
LSP_SOURCES="$LSP_SOURCES comp_keywords.c"
LSP_SOURCES="$LSP_SOURCES comp_symbols.c"

# LenoC source files
# ---------------------------------------------------------------------------
# 源清单与 build.sh / build.bat / build_vm.* **共用同一份**（单一事实来源）：
#   sources_core.txt      —— 核心运行时
#   sources_compiler.txt  —— 编译器专属（lexer/parser/semantic/codegen/package）
# ⚠ 不要再往本文件里逐个加源文件：改清单文件，否则必然漂移 ——
#   本文件此前抄的是一份**过期的手写清单**：既列着已不存在的
#   src/codegen/codegen_inline.c（gcc: No such file or directory），
#   又漏了 src/dce.c、src/optimize/optimize.c、src/platform/platform_path.c、
#   src/package/package_icon.c。
#   src/main.c 必须排除：LSP 的入口是 lsp_server.c 里的 main()，两处 main 互斥。
# ---------------------------------------------------------------------------
read_list() {
  tr -d '\r' < "$1" | grep -v '^[[:space:]]*$' | tr '\\' '/'
}

LENO_SOURCES=""
for f in $(read_list ../sources_core.txt) $(read_list ../sources_compiler.txt); do
  if [ "$f" = "src/main.c" ]; then
    continue
  fi
  LENO_SOURCES="$LENO_SOURCES ../$f"
done

# Platform-specific libraries and FFI implementation
# macOS 不提供 libdl（dlopen/dlsym 在 libSystem 里），带 -ldl 会链接失败：
#   ld: library not found for -ldl
# Linux 保持原样（glibc 的 libdl）。⚠ 这一处 CI 未覆盖（build.yml 只构建编译器与 VM），
# 所以只改 Darwin 分支、Linux 分支一字不动，避免引入未验证的影响。
case "$(uname -s 2>/dev/null)" in
  # Darwin：无 libdl（不能 -ldl），且 iconv 需显式链接（-liconv）
  Darwin*) LIBS="-lm -lpthread -liconv" ;;
  *)       LIBS="-lm -lpthread -ldl" ;;
esac

# 检测平台和架构，选择对应的 FFI 实现文件
OS="$(uname -s 2>/dev/null || echo unknown)"
ARCH="$(uname -m 2>/dev/null || echo x86_64)"
case "$OS" in
  Darwin*)
    if [ "$ARCH" = "arm64" ] || [ "$ARCH" = "aarch64" ]; then
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_arm64.c"
    else
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_linux.c"
    fi
    ;;
  *)
    if [ "$ARCH" = "aarch64" ] || [ "$ARCH" = "arm64" ]; then
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_arm64.c"
    else
      LENO_SOURCES="$LENO_SOURCES ../src/module/ffi/leno_ffi_linux.c"
    fi
    ;;
esac

# Linux 需要 _GNU_SOURCE —— 与 build.sh / build_vm.sh 同口径（那两处在 Linux 分支里加）。
#   缺了它，glibc 不声明一批 POSIX 名字（useconds_t / ssize_t / CLOCK_MONOTONIC /
#   fd_set / DT_DIR / S_IFDIR / MAP_ANONYMOUS …）⇒ 实测直接 31 个 error、构建失败 ✗
#   Darwin 不加（它走 _DARWIN_C_SOURCE 那一套，与既有分支保持一致）。
case "$(uname -s 2>/dev/null)" in
  Darwin*) : ;;
  *)       CFLAGS="$CFLAGS -D_GNU_SOURCE" ;;
esac

gcc -o build/leno_lsp $LSP_SOURCES $LENO_SOURCES -I../src -Wall -Wextra -std=c99 -O2 $CFLAGS $LIBS

echo "Build successful: build/leno_lsp"
