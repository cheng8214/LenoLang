/* Leno FFI - Windows x64 实现
 * Microsoft x64 调用约定
 *
 * Microsoft x64 ABI 规则:
 *   - 整数/指针参数: RCX, RDX, R8, R9（按参数位置依次分配）
 *   - 浮点参数: XMM0, XMM1, XMM2, XMM3（按参数位置依次分配）
 *   - 整数/浮点寄存器独立计数，按参数位置混合分配
 *     例: foo(int, double, int) → RCX=int1, XMM0=double1, RDX=int2
 *   - 返回值: RAX (整数/指针), XMM0 (浮点)
 *   - 调用者需在栈上预留 32 字节影子空间
 *   - 超过 4 个寄存器参数通过栈传递
 *
 * 核心策略:
 *   利用 C 编译器自动按 ABI 分配寄存器。
 *   根据参数列表的类型组合，选择对应的函数指针类型来调用。
 *   混合参数时，函数签名中 int/double 的顺序与原始参数一致，
 *   编译器会自动将 int 放入整数寄存器、double 放入浮点寄存器。
 *
 *   对于超出预定义类型组合的情况，回退到 "全 int64" 调用，
 *   将 double 参数的位模式当作 int64 传递（通过 memcpy 转换），
 *   接收端再还原为 double。这在 x64 上是安全的，因为:
 *   - 整数寄存器和浮点寄存器独立
 *   - XMM 寄存器的低 64 位存储 double，高 64 位无关
 *   - 将 double 位模式放入整数寄存器传给 XMM 不会被修改
 *
 *   但此回退策略仅适用于前 4 个参数（寄存器传参范围），
 *   更复杂的场景需要汇编动态生成调用序列（未来扩展）。
 */

#include "leno_ffi.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32

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
 *  2. 纯浮点参数的函数指针类型（0~12 个参数）
 * ======================================================================== */
typedef double (*fd0)(void);
typedef double (*fd1)(double);
typedef double (*fd2)(double, double);
typedef double (*fd3)(double, double, double);
typedef double (*fd4)(double, double, double, double);
typedef double (*fd5)(double, double, double, double, double);
typedef double (*fd6)(double, double, double, double, double, double);

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
typedef int64_t (*f_i_i_d_d)(int64_t, int64_t, double, double);
typedef int64_t (*f_i_d_i_d)(int64_t, double, int64_t, double);
typedef int64_t (*f_d_i_i_d)(double, int64_t, int64_t, double);

/* double 返回值版本 — 4 参数混合 */
typedef double (*f_i_i_i_d_r)(int64_t, int64_t, int64_t, double);
typedef double (*f_i_i_d_i_r)(int64_t, int64_t, double, int64_t);
typedef double (*f_i_d_i_i_r)(int64_t, double, int64_t, int64_t);
typedef double (*f_d_i_i_i_r)(double, int64_t, int64_t, int64_t);
typedef double (*f_i_i_d_d_r)(int64_t, int64_t, double, double);
typedef double (*f_i_d_i_d_r)(int64_t, double, int64_t, double);
typedef double (*f_i_d_d_i_r)(int64_t, double, double, int64_t);
typedef double (*f_d_i_i_d_r)(double, int64_t, int64_t, double);
typedef double (*f_d_i_d_i_r)(double, int64_t, double, int64_t);
typedef double (*f_d_d_i_i_r)(double, double, int64_t, int64_t);

/* float 版本 — 4 参数混合 */
typedef int64_t (*f_i_i_i_F)(int64_t, int64_t, int64_t, float);
typedef int64_t (*f_i_i_F_i)(int64_t, int64_t, float, int64_t);
typedef int64_t (*f_i_F_i_i)(int64_t, float, int64_t, int64_t);
typedef int64_t (*f_F_i_i_i)(float, int64_t, int64_t, int64_t);
typedef int64_t (*f_i_F_F_F)(int64_t, float, float, float);
typedef int64_t (*f_F_F_F_i)(float, float, float, int64_t);

/* float 版本 — 5 参数（1 ptr + 4 f32，SDLRenderLine 等） */
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
        /* double×2 */ if (!argIsDbl(a,0)&&argIsDbl(a,1)&&argIsDbl(a,2)) return ((f_i_d_d)func)(arg_i(a,0),arg_d(a,1),arg_d(a,2));
        /* double×2 */ if (argIsDbl(a,0)&&!argIsDbl(a,1)&&argIsDbl(a,2)) return ((f_d_i_d)func)(arg_d(a,0),arg_i(a,1),arg_d(a,2));
        /* double×2 */ if (argIsDbl(a,0)&&argIsDbl(a,1)&&!argIsDbl(a,2)) return ((f_d_d_i)func)(arg_d(a,0),arg_d(a,1),arg_i(a,2));
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
        /* double×2 */ if (!argIsDbl(a,0)&&!argIsDbl(a,1)&&argIsDbl(a,2)&&argIsDbl(a,3)) return ((f_i_i_d_d)func)(arg_i(a,0),arg_i(a,1),arg_d(a,2),arg_d(a,3));
        /* double×2 */ if (!argIsDbl(a,0)&&argIsDbl(a,1)&&!argIsDbl(a,2)&&argIsDbl(a,3)) return ((f_i_d_i_d)func)(arg_i(a,0),arg_d(a,1),arg_i(a,2),arg_d(a,3));
        /* double×2 */ if (!argIsDbl(a,0)&&argIsDbl(a,1)&&argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_i_d_d_i)func)(arg_i(a,0),arg_d(a,1),arg_d(a,2),arg_i(a,3));
        /* double×2 */ if (argIsDbl(a,0)&&!argIsDbl(a,1)&&!argIsDbl(a,2)&&argIsDbl(a,3)) return ((f_d_i_i_d)func)(arg_d(a,0),arg_i(a,1),arg_i(a,2),arg_d(a,3));
        /* double×2 */ if (argIsDbl(a,0)&&!argIsDbl(a,1)&&argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_d_i_d_i)func)(arg_d(a,0),arg_i(a,1),arg_d(a,2),arg_i(a,3));
        /* double×2 */ if (argIsDbl(a,0)&&argIsDbl(a,1)&&!argIsDbl(a,2)&&!argIsDbl(a,3)) return ((f_d_d_i_i)func)(arg_d(a,0),arg_d(a,1),arg_i(a,2),arg_i(a,3));
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
    /* 2 参数混合，返回 double（区分 f32 和 f64） */
    if (total == 2) {
        /* f32 版本 */
        if (!argIsF32(args,0) && argIsF32(args,1))  return ((f_i_F_r)func)(args[0].value.i, args[1].value.f);
        if (argIsF32(args,0) && !argIsF32(args,1))  return ((f_F_i_r)func)(args[0].value.f, args[1].value.i);
        if (argIsF32(args,0) && argIsF32(args,1))   return ((f_F_F_r)func)(args[0].value.f, args[1].value.f);
        /* f64 版本 */
        int d0 = is_double_type(args[0].type);
        int d1 = is_double_type(args[1].type);
        if (!d0 && d1)  return ((f_i_d_r)func)(args[0].value.i, args[1].value.d);
        if (d0 && !d1)  return ((f_d_i_r)func)(args[0].value.d, args[1].value.i);
        if (d0 && d1)   return ((f_d_d_r)func)(args[0].value.d, args[1].value.d);
        return 0;
    }
    /* 3 参数混合，返回 double（仅 f64，f32 3参数返回 double 极少见，走回退） */
    if (total == 3) {
        int d0 = is_double_type(args[0].type);
        int d1 = is_double_type(args[1].type);
        int d2 = is_double_type(args[2].type);
        if (!d0 && !d1 && d2)  return ((f_i_i_d_r)func)(args[0].value.i, args[1].value.i, args[2].value.d);
        if (!d0 && d1 && !d2)  return ((f_i_d_i_r)func)(args[0].value.i, args[1].value.d, args[2].value.i);
        if (d0 && !d1 && !d2)  return ((f_d_i_i_r)func)(args[0].value.d, args[1].value.i, args[2].value.i);
        if (!d0 && d1 && d2)   return ((f_i_d_d_r)func)(args[0].value.i, args[1].value.d, args[2].value.d);
        if (d0 && !d1 && d2)   return ((f_d_i_d_r)func)(args[0].value.d, args[1].value.i, args[2].value.d);
        if (d0 && d1 && !d2)   return ((f_d_d_i_r)func)(args[0].value.d, args[1].value.d, args[2].value.i);
        return 0;
    }
    /* 4 参数混合，返回 double */
    if (total == 4) {
        int d0 = is_double_type(args[0].type);
        int d1 = is_double_type(args[1].type);
        int d2 = is_double_type(args[2].type);
        int d3 = is_double_type(args[3].type);
        /* 1 double */
        if (!d0 && !d1 && !d2 && d3) return ((f_i_i_i_d_r)func)(args[0].value.i, args[1].value.i, args[2].value.i, args[3].value.d);
        if (!d0 && !d1 && d2 && !d3) return ((f_i_i_d_i_r)func)(args[0].value.i, args[1].value.i, args[2].value.d, args[3].value.i);
        if (!d0 && d1 && !d2 && !d3) return ((f_i_d_i_i_r)func)(args[0].value.i, args[1].value.d, args[2].value.i, args[3].value.i);
        if (d0 && !d1 && !d2 && !d3) return ((f_d_i_i_i_r)func)(args[0].value.d, args[1].value.i, args[2].value.i, args[3].value.i);
        /* 2 doubles */
        if (!d0 && !d1 && d2 && d3) return ((f_i_i_d_d_r)func)(args[0].value.i, args[1].value.i, args[2].value.d, args[3].value.d);
        if (!d0 && d1 && !d2 && d3) return ((f_i_d_i_d_r)func)(args[0].value.i, args[1].value.d, args[2].value.i, args[3].value.d);
        if (!d0 && d1 && d2 && !d3) return ((f_i_d_d_i_r)func)(args[0].value.i, args[1].value.d, args[2].value.d, args[3].value.i);
        if (d0 && !d1 && !d2 && d3) return ((f_d_i_i_d_r)func)(args[0].value.d, args[1].value.i, args[2].value.i, args[3].value.d);
        if (d0 && !d1 && d2 && !d3) return ((f_d_i_d_i_r)func)(args[0].value.d, args[1].value.i, args[2].value.d, args[3].value.i);
        if (d0 && d1 && !d2 && !d3) return ((f_d_d_i_i_r)func)(args[0].value.d, args[1].value.d, args[2].value.i, args[3].value.i);
        return 0;
    }
    return 0;
}

/* ========================================================================
 *  ffi_call_win64 - Windows x64 调用约定的核心实现
 *
 *  调用策略（按优先级）:
 *  1. 纯整数/指针参数 → call_pure_int
 *  2. 纯浮点参数 → call_pure_double
 *  3. 混合参数 ≤4 个 → 预定义混合类型分发
 *  4. 混合参数 >4 个 → 回退：将 double 位模式当 int64 传，
 *     依赖 ABI 的整数/浮点寄存器独立性（仅适用于寄存器传参范围）
 * ======================================================================== */
FFIValue ffi_call_win64(void* func, const FFISignature* sig, const FFIArg* args) {
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
     * 此路径在以下条件下安全（上层 ffi.c 已做拦截）：
     *   - 前 4 个参数不含浮点（走整数寄存器 RCX/RDX/R8/R9，正确）
     *   - 第 5+ 个参数中的浮点走栈（8 字节对齐，位模式一致，被调函数从栈读取到 XMM，正确）
     *
     * 注意：float(f32) 参数在栈上会被当作 8 字节传递（int64 容器），
     * 被调函数从栈读取时会取低 32 位，这在 Win64 ABI 下是正确的
     * （栈上参数按 8 字节对齐，f32 占低 4 字节）。
     */
    {
        int64_t iargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++) {
            if (args[i].type == FFI_TYPE_DOUBLE)
                iargs[i] = double_to_bits(args[i].value.d);
            else if (args[i].type == FFI_TYPE_FLOAT) {
                /* f32: Win64 ABI 栈上参数按 8 字节容器传递，f32 占低 32 位
                 * 将 float 位模式放入 int64 的低 32 位（高 32 位清零）
                 * 被函数从栈读取低 32 位作为 float */
                float fval = args[i].value.f;
                int32_t fbits;
                memcpy(&fbits, &fval, sizeof(fbits));
                iargs[i] = (int64_t)(uint32_t)fbits;
            }
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

/* 通用调用入口 - Windows 平台转发到 ffi_call_win64 */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_win64(func, sig, args);
}

#endif /* _WIN32 */
