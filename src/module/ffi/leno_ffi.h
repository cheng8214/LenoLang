/* Leno FFI - 简化外部函数接口
 * 灵感来源于 LuaJIT FFI 和 Python ctypes
 * 支持: Windows x64, Linux x64, macOS x64, macOS arm64, Linux arm64
 *
 * 类型: void, int, double, pointer
 * 最大参数: 12
 *
 * 核心设计:
 *   前端（ffi.c）把实参按**类型**打包成 FFIArg 列表（每个实参都带 FFIType 标签 ✓），
 *   后端（leno_ffi_{win64,linux,arm64}.c）据此按各平台 ABI 分配寄存器与栈参数。
 *   ⇒ 类型信息一直是充分的：clib 声明在**编译期**就写明了每个形参类型，
 *     运行期一路传到后端（见下面的 FFISignature/FFIArg ✓）。
 *
 * 平台调用约定:
 *   - Windows x64:    Microsoft x64 (RCX/RDX/R8/R9 + XMM0-XMM3, 32B 影子空间)
 *   - Linux/macOS x64: System V AMD64 (RDI-R9 + XMM0-XMM7, 无影子空间)
 *   - ARM64 (任意 OS): AAPCS64 (X0-X7 + V0-V7, 无影子空间)
 *
 * 各平台的实现策略（2026-10-01 起）:
 *   · Windows x64 / Linux·macOS x64：**按 ABI 精确分类 + 汇编调用桩**
 *     （纯整数 / 纯浮点仍走 C 函数指针；**任何混合组合**交给桩 ⇒ 任意组合都正确 ✓
 *       见 leno_ffi_win64.c / leno_ffi_linux.c 的文件头 ✓）
 *   · ARM64：仍是"按类型组合枚举函数指针 + 全 int64 回退"的老做法 ⇒
 *     ⚠ 覆盖不全、部分组合会静默失效（详见 leno_ffi_arm64.c；待办：迁到 AAPCS64 桩）
 *
 * ⚠ 关于老回退"把 double 位模式当 int64 传"（现仅 arm64 侧仍有其影响）：
 *   它**并不是**"在寄存器传参范围内都安全"——恰恰相反：
 *     · 浮点实参落在**浮点寄存器**里时走整数寄存器交付 ⇒ 被调方读到陈旧值 ⇒ **必错** ✗
 *     · 只有浮点实参**溢出到栈上**时，位模式与 ABI 的栈布局一致 ⇒ 才安全 ✓
 *   （本文件此前把这条说反了。Linux 侧的 PVZ 段错误、Windows 侧 `(i32,f32,i32,f32)`
 *     静默空调用，根因都是它 ✓）
 *   与 LuaJIT FFI 的对比: LuaJIT 用 JIT 动态生成调用序列，天然没有这个问题；
 *   本仓库的汇编调用桩是同一思路的静态版本 ✓
 */

#ifndef LENO_FFI_H
#define LENO_FFI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 最大参数数量 ===== */
#define FFI_MAX_ARGS 12  /* Win64: 4 reg + 8 stack; SysV: 6 reg + 6 stack; AAPCS64: 8 reg + 4 stack */

/* ===== FFI 参数类型枚举 ===== */
typedef enum {
    FFI_TYPE_VOID    = 0,  /* 无返回值 */
    FFI_TYPE_INT     = 1,  /* 整数类型 (int64_t) */
    FFI_TYPE_DOUBLE  = 2,  /* 双精度浮点类型 */
    FFI_TYPE_POINTER = 3,  /* 指针类型 */
    FFI_TYPE_UINT8   = 4,  /* 无符号 8 位整数 */
    FFI_TYPE_INT8    = 5,  /* 有符号 8 位整数 */
    FFI_TYPE_UINT16  = 6,  /* 无符号 16 位整数 */
    FFI_TYPE_INT16   = 7,  /* 有符号 16 位整数 */
    FFI_TYPE_UINT32  = 8,  /* 无符号 32 位整数 */
    FFI_TYPE_INT32   = 9,  /* 有符号 32 位整数 */
    FFI_TYPE_FLOAT   = 10, /* 单精度浮点类型 */
    FFI_TYPE_BOOL    = 11, /* 布尔类型（0 或非零） */
} FFIType;

/* ===== FFI 参数值联合体 ===== */
/* 每个参数只能取其中一种类型，大小取最大成员 */
typedef union {
    int64_t  i;    /* 整数值 / 指针值（强转后） */
    double   d;    /* 浮点值 */
    void*    p;    /* 指针值 */
    uint8_t  u8;   /* 无符号 8 位 */
    int8_t   i8;   /* 有符号 8 位 */
    uint16_t u16;  /* 无符号 16 位 */
    int16_t  i16;  /* 有符号 16 位 */
    uint32_t u32;  /* 无符号 32 位 */
    float    f;    /* 单精度浮点 */
} FFIValue;

/* ===== FFI 参数描述符 ===== */
typedef struct {
    FFIType  type;   /* 参数类型 */
    FFIValue value;  /* 参数值 */
    int      owned;  /* 是否拥有指针内存（str16 自动转换时为 1，调用后需释放） */
} FFIArg;

/* ===== FFI 函数签名 ===== */
typedef struct {
    FFIType ret_type;                  /* 返回值类型 */
    int     nargs;                     /* 参数个数 */
    FFIType arg_types[FFI_MAX_ARGS];   /* 每个参数的类型 */
} FFISignature;

/* ===== 通用 FFI 调用入口 ===== */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args);

/* ===== 平台特定实现 ===== */
#ifdef _WIN32
FFIValue ffi_call_win64(void* func, const FFISignature* sig, const FFIArg* args);
#elif defined(__arm64__) || defined(__aarch64__)
FFIValue ffi_call_aapcs(void* func, const FFISignature* sig, const FFIArg* args);
#else
FFIValue ffi_call_sysv(void* func, const FFISignature* sig, const FFIArg* args);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LENO_FFI_H */
