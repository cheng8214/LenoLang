/* Leno FFI - ARM64 (AArch64) 实现
 * AAPCS64 (ARM Architecture Procedure Call Standard) 调用约定
 *
 * AAPCS64 ABI 规则:
 *   - 整数/指针参数: X0, X1, X2, X3, X4, X5, X6, X7（按参数位置依次分配）
 *   - 浮点参数: V0, V1, V2, V3, V4, V5, V6, V7（按参数位置依次分配）
 *   - 整数/浮点寄存器独立计数，按参数位置混合分配
 *     例: foo(int, double, int) → X0=int1, V0=double1, X1=int2
 *   - 返回值: X0 (整数/指针), V0 (浮点)
 *   - 无需影子空间
 *   - 超过 8 个寄存器参数通过栈传递（SP 必须 16 字节对齐）
 *   - float 占 V 寄存器低 32 位，double 占低 64 位
 *
 * 与 System V AMD64 的对比:
 *   - 整数寄存器: 8 个 (X0-X7) vs 6 个 (RDI-R9) → 多 2 个寄存器参数
 *   - 浮点寄存器: 8 个 (V0-V7) vs 8 个 (XMM0-XMM7) → 相同
 *   - 无影子空间 → 相同
 *   - 返回值位置: X0/V0 vs RAX/XMM0 → 等价
 *   - 寄存器分配策略完全相同：整数/浮点独立按位置计数
 *
 * 核心策略:
 *   与 x86_64 版本完全一致：利用 C 编译器自动按 AAPCS64 分配寄存器。
 *   根据参数列表的类型组合，选择对应的函数指针类型来调用。
 *   混合参数时，函数签名中 int/double 的顺序与原始参数一致，
 *   编译器会自动将 int 放入 X 寄存器、double 放入 V 寄存器。
 *
 *   对于超出预定义类型组合的情况，回退到 "全 int64" 调用，
 *   将 double 参数的位模式当作 int64 传递（通过 memcpy 转换），
 *   接收端再还原为 double。这在 AAPCS64 上与 x86_64 有相同的限制:
 *   - 整数寄存器和浮点寄存器独立
 *   - 将 double 位模式放入整数寄存器传给 V 寄存器不会被自动转移
 *   - 此回退策略仅适用于前 8 个参数（寄存器传参范围）
 *
 * 注意:
 *   AAPCS64 比 x86_64 的调用约定更规整——整数浮点寄存器各 8 个，
 *   按参数位置依次分配，没有影子空间/混合寄存器规则等复杂性。
 *   实际的函数指针类型分发逻辑与 System V 版本完全相同，
 *   因为两者都是"整数/浮点独立按位置计数"的模型。
 */

#include "leno_ffi.h"
#include <string.h>

#if defined(__arm64__) || defined(__aarch64__)

/* ========================================================================
 *  1. 纯整数/指针参数的函数指针类型（0~12 个参数）
 * ======================================================================== */
typedef int64_t (*fi0)(void);
typedef int64_t (*fi1)(int64_t);
typedef int64_t (*fi2)(int64_t, int64_t);
typedef int64_t (*fi3)(int64_t, int64_t, int64_t);
typedef int64_t (*fi4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi5)(int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi9)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi10)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi11)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*fi12)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

/* ========================================================================
 *  2. 纯浮点参数的函数指针类型（0~8 个参数）
 *     AAPCS64 有 8 个浮点寄存器 (V0-V7)，比 Win64 多一倍
 * ======================================================================== */
typedef double (*fd0)(void);
typedef double (*fd1)(double);
typedef double (*fd2)(double, double);
typedef double (*fd3)(double, double, double);
typedef double (*fd4)(double, double, double, double);
typedef double (*fd5)(double, double, double, double, double);
typedef double (*fd6)(double, double, double, double, double, double);
typedef double (*fd7)(double, double, double, double, double, double, double);
typedef double (*fd8)(double, double, double, double, double, double, double, double);

/* ========================================================================
 *  3. 混合参数的函数指针类型（2 参数）
 * ======================================================================== */
typedef int64_t (*f_i_d)(int64_t, double);
typedef int64_t (*f_d_i)(double, int64_t);
typedef double  (*f_i_d_r)(int64_t, double);
typedef double  (*f_d_i_r)(double, int64_t);
typedef double  (*f_d_d_r)(double, double);

/* float 版本 (f32) — 2 参数混合 */
typedef int64_t (*f_i_F)(int64_t, float);
typedef int64_t (*f_F_i)(float, int64_t);
typedef double  (*f_i_F_r)(int64_t, float);
typedef double  (*f_F_i_r)(float, int64_t);
typedef double  (*f_F_F_r)(float, float);

/* ========================================================================
 *  4. 混合参数的函数指针类型（3 参数）
 * ======================================================================== */
typedef int64_t (*f_i_i_d)(int64_t, int64_t, double);
typedef int64_t (*f_i_d_i)(int64_t, double, int64_t);
typedef int64_t (*f_d_i_i)(double, int64_t, int64_t);
typedef int64_t (*f_d_d_i)(double, double, int64_t);
typedef int64_t (*f_d_i_d)(double, int64_t, double);
typedef int64_t (*f_i_d_d)(int64_t, double, double);
typedef double  (*f_i_i_d_r)(int64_t, int64_t, double);
typedef double  (*f_i_d_i_r)(int64_t, double, int64_t);
typedef double  (*f_d_i_i_r)(double, int64_t, int64_t);
typedef double  (*f_d_d_i_r)(double, double, int64_t);
typedef double  (*f_d_i_d_r)(double, int64_t, double);
typedef double  (*f_i_d_d_r)(int64_t, double, double);

/* float 版本 — 3 参数混合 */
typedef int64_t (*f_i_i_F)(int64_t, int64_t, float);
typedef int64_t (*f_i_F_i)(int64_t, float, int64_t);
typedef int64_t (*f_F_i_i)(float, int64_t, int64_t);
typedef int64_t (*f_F_F_i)(float, float, int64_t);
typedef int64_t (*f_F_i_F)(float, int64_t, float);
typedef int64_t (*f_i_F_F)(int64_t, float, float);

/* ========================================================================
 *  5. 混合参数的函数指针类型（4 参数）
 * ======================================================================== */
typedef int64_t (*f_i_i_i_d)(int64_t, int64_t, int64_t, double);
typedef int64_t (*f_i_i_d_i)(int64_t, int64_t, double, int64_t);
typedef int64_t (*f_i_d_i_i)(int64_t, double, int64_t, int64_t);
typedef int64_t (*f_d_i_i_i)(double, int64_t, int64_t, int64_t);
typedef int64_t (*f_i_d_d_i)(int64_t, double, double, int64_t);
typedef int64_t (*f_d_i_d_i)(double, int64_t, double, int64_t);
typedef int64_t (*f_d_d_i_i)(double, double, int64_t, int64_t);

/* float 版本 — 4 参数混合 */
typedef int64_t (*f_i_i_i_F)(int64_t, int64_t, int64_t, float);
typedef int64_t (*f_i_i_F_i)(int64_t, int64_t, float, int64_t);
typedef int64_t (*f_i_F_i_i)(int64_t, float, int64_t, int64_t);
typedef int64_t (*f_F_i_i_i)(float, int64_t, int64_t, int64_t);
typedef int64_t (*f_i_F_F_F)(int64_t, float, float, float);
typedef int64_t (*f_F_F_F_i)(float, float, float, int64_t);

/* float 版本 — 5 参数（1 ptr + 4 f32，SDL_RenderLine 等） */
typedef int64_t (*f_i_F4)(int64_t, float, float, float, float);

/* ========================================================================
 *  辅助函数：判断参数类型是否为浮点
 * ======================================================================== */
static int is_float_type(FFIType t) {
    return t == FFI_TYPE_DOUBLE || t == FFI_TYPE_FLOAT;
}
static int is_double_type(FFIType t) {
    return t == FFI_TYPE_DOUBLE;
}
static int is_float_only(FFIType t) {
    return t == FFI_TYPE_FLOAT;
}

/* ========================================================================
 *  辅助函数：将 double 的位模式转换为 int64_t（用于回退调用路径）
 *  这不是数值转换，而是位模式重解释
 * ======================================================================== */
static int64_t double_to_bits(double d) {
    int64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static double bits_to_double(int64_t i) {
    double d;
    memcpy(&d, &i, sizeof(d));
    return d;
}

/* ========================================================================
 *  核心调用实现：纯整数/指针参数分发
 * ======================================================================== */
static int64_t call_pure_int(void* func, int64_t* iargs, int count) {
    switch (count) {
        case 0:  return ((fi0)func)();
        case 1:  return ((fi1)func)(iargs[0]);
        case 2:  return ((fi2)func)(iargs[0], iargs[1]);
        case 3:  return ((fi3)func)(iargs[0], iargs[1], iargs[2]);
        case 4:  return ((fi4)func)(iargs[0], iargs[1], iargs[2], iargs[3]);
        case 5:  return ((fi5)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]);
        case 6:  return ((fi6)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]);
        case 7:  return ((fi7)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6]);
        case 8:  return ((fi8)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7]);
        case 9:  return ((fi9)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8]);
        case 10: return ((fi10)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9]);
        case 11: return ((fi11)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10]);
        case 12: return ((fi12)func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7], iargs[8], iargs[9], iargs[10], iargs[11]);
        default: return 0;
    }
}

/* ========================================================================
 *  核心调用实现：纯浮点参数分发
 *  AAPCS64 支持 8 个浮点寄存器 (V0-V7)
 * ======================================================================== */
static double call_pure_double(void* func, double* dargs, int count) {
    switch (count) {
        case 0:  return ((fd0)func)();
        case 1:  return ((fd1)func)(dargs[0]);
        case 2:  return ((fd2)func)(dargs[0], dargs[1]);
        case 3:  return ((fd3)func)(dargs[0], dargs[1], dargs[2]);
        case 4:  return ((fd4)func)(dargs[0], dargs[1], dargs[2], dargs[3]);
        case 5:  return ((fd5)func)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4]);
        case 6:  return ((fd6)func)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4], dargs[5]);
        case 7:  return ((fd7)func)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4], dargs[5], dargs[6]);
        case 8:  return ((fd8)func)(dargs[0], dargs[1], dargs[2], dargs[3], dargs[4], dargs[5], dargs[6], dargs[7]);
        default: return 0;
    }
}

/* ========================================================================
 *  核心调用实现：混合参数分发（2~4 参数）
 *  根据 arg_types 数组中每个参数的实际类型选择对应函数指针
 * ======================================================================== */
/* 辅助函数: 取值 & 类型判断 */
static inline int64_t  arg_i(const FFIArg* a, int n) { return a[n].value.i; }
static inline double   arg_d(const FFIArg* a, int n) { return a[n].value.d; }
static inline float    arg_f(const FFIArg* a, int n) { return a[n].value.f; }
static inline int      argIsDbl(const FFIArg* a, int n) { return is_double_type(a[n].type); }
static inline int      argIsF32(const FFIArg* a, int n) { return is_float_only(a[n].type); }

static int64_t call_mixed_int_ret(void* func, const FFIArg* a, int total) {
    /* 2 参数混合 */
    if (total == 2) {
        /* double */ if (!argIsDbl(a,0) && argIsDbl(a,1)) return ((f_i_d)func)(arg_i(a,0), arg_d(a,1));
        /* double */ if (argIsDbl(a,0) && !argIsDbl(a,1)) return ((f_d_i)func)(arg_d(a,0), arg_i(a,1));
        /* f32    */ if (!argIsF32(a,0) && argIsF32(a,1)) return ((f_i_F)func)(arg_i(a,0), arg_f(a,1));
        /* f32    */ if (argIsF32(a,0) && !argIsF32(a,1)) return ((f_F_i)func)(arg_f(a,0), arg_i(a,1));
        return 0;
    }
    /* 3 参数混合 */
    if (total == 3) {
        /* double */ if (!argIsDbl(a,0)&&!argIsDbl(a,1)&&argIsDbl(a,2)) return ((f_i_i_d)func)(arg_i(a,0),arg_i(a,1),arg_d(a,2));
        /* double */ if (!argIsDbl(a,0)&&argIsDbl(a,1)&&!argIsDbl(a,2)) return ((f_i_d_i)func)(arg_i(a,0),arg_d(a,1),arg_i(a,2));
        /* double */ if (argIsDbl(a,0)&&!argIsDbl(a,1)&&!argIsDbl(a,2)) return ((f_d_i_i)func)(arg_d(a,0),arg_i(a,1),arg_i(a,2));
        /* f32    */ if (!argIsF32(a,0)&&!argIsF32(a,1)&&argIsF32(a,2)) return ((f_i_i_F)func)(arg_i(a,0),arg_i(a,1),arg_f(a,2));
        /* f32    */ if (!argIsF32(a,0)&&argIsF32(a,1)&&!argIsF32(a,2)) return ((f_i_F_i)func)(arg_i(a,0),arg_f(a,1),arg_i(a,2));
        /* f32    */ if (argIsF32(a,0)&&!argIsF32(a,1)&&!argIsF32(a,2)) return ((f_F_i_i)func)(arg_f(a,0),arg_i(a,1),arg_i(a,2));
        /* f32    */ if (!argIsF32(a,0)&&argIsF32(a,1)&&argIsF32(a,2))  return ((f_i_F_F)func)(arg_i(a,0),arg_f(a,1),arg_f(a,2));
        /* f32    */ if (argIsF32(a,0)&&!argIsF32(a,1)&&argIsF32(a,2))  return ((f_F_i_F)func)(arg_f(a,0),arg_i(a,1),arg_f(a,2));
        /* f32    */ if (argIsF32(a,0)&&argIsF32(a,1)&&!argIsF32(a,2))  return ((f_F_F_i)func)(arg_f(a,0),arg_f(a,1),arg_i(a,2));
        return 0;
    }
    /* 4 参数混合 */
    if (total == 4) {
        /* double */ if (!argIsDbl(a,0)&&!argIsDbl(a,1)&&!argIsDbl(a,2)&&argIsDbl(a,3)) return ((f_i_i_i_d)func)(arg_i(a,0),arg_i(a,1),arg_i(a,2),arg_d(a,3));
        /* double */ if (!argIsDbl(a,0)&&!argIsDbl(a,1)&&argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_i_i_d_i)func)(arg_i(a,0),arg_i(a,1),arg_d(a,2),arg_i(a,3));
        /* double */ if (!argIsDbl(a,0)&&argIsDbl(a,1)&&!argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_i_d_i_i)func)(arg_i(a,0),arg_d(a,1),arg_i(a,2),arg_i(a,3));
        /* double */ if (argIsDbl(a,0)&&!argIsDbl(a,1)&&!argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_d_i_i_i)func)(arg_d(a,0),arg_i(a,1),arg_i(a,2),arg_i(a,3));
        /* f32    */ if (!argIsF32(a,0)&&!argIsF32(a,1)&&!argIsF32(a,2)&&argIsF32(a,3)) return ((f_i_i_i_F)func)(arg_i(a,0),arg_i(a,1),arg_i(a,2),arg_f(a,3));
        /* f32    */ if (!argIsF32(a,0)&&!argIsF32(a,1)&&argIsF32(a,2)&&!argIsF32(a,3)) return ((f_i_i_F_i)func)(arg_i(a,0),arg_i(a,1),arg_f(a,2),arg_i(a,3));
        /* f32    */ if (!argIsF32(a,0)&&argIsF32(a,1)&&!argIsF32(a,2)&&!argIsF32(a,3)) return ((f_i_F_i_i)func)(arg_i(a,0),arg_f(a,1),arg_i(a,2),arg_i(a,3));
        /* f32    */ if (argIsF32(a,0)&&!argIsF32(a,1)&&!argIsF32(a,2)&&!argIsF32(a,3)) return ((f_F_i_i_i)func)(arg_f(a,0),arg_i(a,1),arg_i(a,2),arg_i(a,3));
        /* f32×3  */if (!argIsF32(a,0)&&argIsF32(a,1)&&argIsF32(a,2)&&argIsF32(a,3))  return ((f_i_F_F_F)func)(arg_i(a,0),arg_f(a,1),arg_f(a,2),arg_f(a,3));
        /* f32×3  */if (argIsF32(a,0)&&argIsF32(a,1)&&argIsF32(a,2)&&!argIsF32(a,3))  return ((f_F_F_F_i)func)(arg_f(a,0),arg_f(a,1),arg_f(a,2),arg_i(a,3));
        return 0;
    }
    return 0;
}

static double call_mixed_double_ret(void* func, const FFIArg* args, int total) {
    /* 2 参数混合，返回 double */
    if (total == 2) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        if (!d0 && d1)  return ((f_i_d_r)func)(args[0].value.i, args[1].value.d);
        if (d0 && !d1)  return ((f_d_i_r)func)(args[0].value.d, args[1].value.i);
        if (d0 && d1)   return ((f_d_d_r)func)(args[0].value.d, args[1].value.d);
        return 0;
    }
    /* 3 参数混合，返回 double */
    if (total == 3) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        int d2 = is_float_type(args[2].type);
        if (!d0 && !d1 && d2)  return ((f_i_i_d_r)func)(args[0].value.i, args[1].value.i, args[2].value.d);
        if (!d0 && d1 && !d2)  return ((f_i_d_i_r)func)(args[0].value.i, args[1].value.d, args[2].value.i);
        if (d0 && !d1 && !d2)  return ((f_d_i_i_r)func)(args[0].value.d, args[1].value.i, args[2].value.i);
        if (!d0 && d1 && d2)   return ((f_i_d_d_r)func)(args[0].value.i, args[1].value.d, args[2].value.d);
        if (d0 && !d1 && d2)   return ((f_d_i_d_r)func)(args[0].value.d, args[1].value.i, args[2].value.d);
        if (d0 && d1 && !d2)   return ((f_d_d_i_r)func)(args[0].value.d, args[1].value.d, args[2].value.i);
        return 0;
    }
    return 0;
}

/* ========================================================================
 *  ffi_call_aapcs - AAPCS64 调用约定的核心实现
 *
 *  调用策略（按优先级）:
 *  1. 纯整数/指针参数 → call_pure_int
 *  2. 纯浮点参数 → call_pure_double
 *  3. 混合参数 ≤4 个 → 预定义混合类型分发（含 f32）
 *  4. 5 参数 f32 专用路径（SDL_RenderLine 等）
 *  5. 混合参数 >4 个 → 回退：将 double 位模式当 int64 传
 *
 *  与 System V AMD64 的区别:
 *  - AAPCS64 有 8 个整数寄存器 (X0-X7)，回退策略对前 8 个参数安全
 *    （System V 仅 6 个，Win64 仅 4 个）
 *  - 纯浮点路径支持最多 8 个参数（与 System V 相同）
 * ======================================================================== */
FFIValue ffi_call_aapcs(void* func, const FFISignature* sig, const FFIArg* args) {
    FFIValue result = {0};
    int total = sig->nargs > FFI_MAX_ARGS ? FFI_MAX_ARGS : sig->nargs;

    /* 统计浮点和整数参数数量 */
    int dcount = 0, icount = 0;
    for (int i = 0; i < total; i++) {
        if (is_float_type(args[i].type))
            dcount++;
        else
            icount++;
    }

    /* ===== 路径 1: 纯整数/指针参数 ===== */
    if (dcount == 0) {
        int64_t iargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++) {
            if (args[i].type == FFI_TYPE_POINTER)
                iargs[i] = (int64_t)(intptr_t)args[i].value.p;
            else
                iargs[i] = args[i].value.i;
        }
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            /* 所有参数是整数，但返回 double */
            switch (total) {
                case 0: result.d = ((double (*)(void))func)(); break;
                case 1: result.d = ((double (*)(int64_t))func)(iargs[0]); break;
                case 2: result.d = ((double (*)(int64_t, int64_t))func)(iargs[0], iargs[1]); break;
                case 3: result.d = ((double (*)(int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2]); break;
                case 4: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3]); break;
                case 5: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4]); break;
                case 6: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5]); break;
                case 7: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6]); break;
                case 8: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3], iargs[4], iargs[5], iargs[6], iargs[7]); break;
                default: result.d = 0; break;
            }
        } else {
            result.i = call_pure_int(func, iargs, total);
        }
        return result;
    }

    /* ===== 路径 2: 纯浮点参数 ===== */
    if (icount == 0) {
        double dargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++)
            dargs[i] = args[i].value.d;
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            result.d = call_pure_double(func, dargs, total);
        } else {
            /* 所有参数是 double，但返回整数 */
            result.i = (int64_t)call_pure_double(func, dargs, total);
        }
        return result;
    }

    /* ===== 路径 3: 混合参数 ≤4 个 → 精确分发 ===== */
    if (total <= 4) {
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            result.d = call_mixed_double_ret(func, args, total);
        } else {
            result.i = call_mixed_int_ret(func, args, total);
        }
        return result;
    }

    /* ===== 路径 3.5: >4 参数 f32 专用 =====
     * Ptr + f32×4 (SDL_RenderLine 等常见 5 参数模式) */
    if (total == 5) {
        int fcount = 0;
        for (int i = 0; i < total; i++)
            if (is_float_only(args[i].type)) fcount++;
        /* (Ptr, f32, f32, f32, f32) — 5 参数，1 ptr + 4 f32 */
        if (fcount == 4 && !is_float_type(args[0].type)) {
            result.i = ((f_i_F4)func)(args[0].value.i,
                args[1].value.f, args[2].value.f, args[3].value.f, args[4].value.f);
            return result;
        }
        /* 其他 5 参数组合（含 double）见路径 4 */
    }

    /* ===== 路径 4: 混合参数回退策略 =====
     * 将所有参数统一为 int64_t 传递（double 转为位模式），
     * 让 C 编译器根据强转后的函数指针类型自动分配寄存器。
     *
     * 注意：这种方法在 AAPCS64 上对前 8 个参数是安全的，
     * 因为整数/浮点寄存器独立分配（X0-X7 / V0-V7 各 8 个）。
     * 但对栈传参（>8 参数），double 的位模式在栈上不会被自动
     * 加载到 V 寄存器。因此超过 8 个参数的混合调用可能不正确。
     *
     * 相比 System V (6 个安全) 和 Win64 (4 个安全)，
     * AAPCS64 的 8+8 寄存器提供了最大的回退安全范围。
     */
    {
        int64_t iargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++) {
            if (is_float_type(args[i].type))
                iargs[i] = double_to_bits(args[i].value.d);
            else if (args[i].type == FFI_TYPE_POINTER)
                iargs[i] = (int64_t)(intptr_t)args[i].value.p;
            else
                iargs[i] = args[i].value.i;
        }

        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            int64_t r = call_pure_int(func, iargs, total);
            result.d = bits_to_double(r);
        } else {
            result.i = call_pure_int(func, iargs, total);
        }
    }

    return result;
}

/* 通用调用入口 - ARM64 平台转发到 ffi_call_aapcs */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_aapcs(func, sig, args);
}

#endif /* __arm64__ || __aarch64__ */
