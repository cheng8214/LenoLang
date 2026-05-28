/* Leno FFI - 简化外部函数接口
 * 灵感来源于 LuaJIT FFI 和 Python ctypes
 * 支持: Windows x64, Linux x64
 *
 * 类型: void, int, double, pointer
 * 最大参数: 12 (6 寄存器 + 6 栈参数)
 *
 * 核心设计:
 *   参数按原始位置混合打包为统一类型列表 (int64_t/double)，
 *   利用 C 编译器自动按 x64 ABI 规则分配寄存器，
 *   避免手动处理混合寄存器分配的复杂性。
 *
 * 与 LuaJIT FFI 的对比:
 *   LuaJIT 使用 JIT 编译器动态生成调用序列，可处理任意参数组合。
 *   本实现使用预定义的函数指针类型分发，覆盖常见组合，
 *   对未覆盖的组合使用通用汇编调用路径（未来可扩展）。
 */

#ifndef LENO_FFI_H
#define LENO_FFI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 最大参数数量 ===== */
#define FFI_MAX_ARGS 12  /* 6 寄存器 + 6 栈参数（x64 ABI） */

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
#else
FFIValue ffi_call_sysv(void* func, const FFISignature* sig, const FFIArg* args);
#endif

#ifdef __cplusplus
}
#endif

#endif /* LENO_FFI_H */
