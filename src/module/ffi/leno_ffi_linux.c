/* Leno FFI - Linux x64 实现
 * System V AMD64 ABI 调用约定
 *
 * System V AMD64 ABI 规则:
 *   - 整数/指针参数: RDI, RSI, RDX, RCX, R8, R9（按参数位置依次分配）
 *   - 浮点参数: XMM0-XMM7（按参数位置依次分配）
 *   - 整数/浮点寄存器独立计数，按参数位置混合分配
 *     例: foo(int, double, int) → RDI=int1, XMM0=double1, RSI=int2
 *   - 返回值: RAX (整数/指针), XMM0 (浮点)
 *   - 无需影子空间
 *   - 超过 6 个寄存器参数通过栈传递
 *
 * 核心策略与 Windows 版相同，见 leno_ffi_win64.c 的注释。
 * 两个文件逻辑一致，仅函数名和条件编译不同。
 */

#include "leno_ffi.h"
#include <string.h>

#ifndef _WIN32

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
 *  2. 纯浮点参数的函数指针类型（0~6 个参数）
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

/* ===== 辅助函数 ===== */
static int is_float_type(FFIType t) {
    return t == FFI_TYPE_DOUBLE || t == FFI_TYPE_FLOAT;
}

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

/* ===== 纯整数分发 ===== */
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

/* ===== 纯浮点分发 ===== */
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

/* ===== 混合参数分发（返回 int） ===== */
static int64_t call_mixed_int_ret(void* func, const FFIArg* args, int total) {
    if (total == 2) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        if (!d0 && d1)  return ((f_i_d)func)(args[0].value.i, args[1].value.d);
        if (d0 && !d1)  return ((f_d_i)func)(args[0].value.d, args[1].value.i);
        return 0;
    }
    if (total == 3) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        int d2 = is_float_type(args[2].type);
        if (!d0 && !d1 && d2)  return ((f_i_i_d)func)(args[0].value.i, args[1].value.i, args[2].value.d);
        if (!d0 && d1 && !d2)  return ((f_i_d_i)func)(args[0].value.i, args[1].value.d, args[2].value.i);
        if (d0 && !d1 && !d2)  return ((f_d_i_i)func)(args[0].value.d, args[1].value.i, args[2].value.i);
        if (!d0 && d1 && d2)   return ((f_i_d_d)func)(args[0].value.i, args[1].value.d, args[2].value.d);
        if (d0 && !d1 && d2)   return ((f_d_i_d)func)(args[0].value.d, args[1].value.i, args[2].value.d);
        if (d0 && d1 && !d2)   return ((f_d_d_i)func)(args[0].value.d, args[1].value.d, args[2].value.i);
        return 0;
    }
    if (total == 4) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        int d2 = is_float_type(args[2].type);
        int d3 = is_float_type(args[3].type);
        if (!d0 && !d1 && !d2 && d3)  return ((f_i_i_i_d)func)(args[0].value.i, args[1].value.i, args[2].value.i, args[3].value.d);
        if (!d0 && !d1 && d2 && !d3)  return ((f_i_i_d_i)func)(args[0].value.i, args[1].value.i, args[2].value.d, args[3].value.i);
        if (!d0 && d1 && !d2 && !d3)  return ((f_i_d_i_i)func)(args[0].value.i, args[1].value.d, args[2].value.i, args[3].value.i);
        if (d0 && !d1 && !d2 && !d3)  return ((f_d_i_i_i)func)(args[0].value.d, args[1].value.i, args[2].value.i, args[3].value.i);
        if (!d0 && d1 && d2 && !d3)   return ((f_i_d_d_i)func)(args[0].value.i, args[1].value.d, args[2].value.d, args[3].value.i);
        if (d0 && !d1 && d2 && !d3)   return ((f_d_i_d_i)func)(args[0].value.d, args[1].value.i, args[2].value.d, args[3].value.i);
        if (d0 && d1 && !d2 && !d3)   return ((f_d_d_i_i)func)(args[0].value.d, args[1].value.d, args[2].value.i, args[3].value.i);
        return 0;
    }
    return 0;
}

/* ===== 混合参数分发（返回 double） ===== */
static double call_mixed_double_ret(void* func, const FFIArg* args, int total) {
    if (total == 2) {
        int d0 = is_float_type(args[0].type);
        int d1 = is_float_type(args[1].type);
        if (!d0 && d1)  return ((f_i_d_r)func)(args[0].value.i, args[1].value.d);
        if (d0 && !d1)  return ((f_d_i_r)func)(args[0].value.d, args[1].value.i);
        if (d0 && d1)   return ((f_d_d_r)func)(args[0].value.d, args[1].value.d);
        return 0;
    }
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

/* ===== 核心调用实现 ===== */
FFIValue ffi_call_sysv(void* func, const FFISignature* sig, const FFIArg* args) {
    FFIValue result = {0};
    int total = sig->nargs > FFI_MAX_ARGS ? FFI_MAX_ARGS : sig->nargs;

    int dcount = 0, icount = 0;
    for (int i = 0; i < total; i++) {
        if (is_float_type(args[i].type))
            dcount++;
        else
            icount++;
    }

    /* 路径 1: 纯整数/指针参数 */
    if (dcount == 0) {
        int64_t iargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++) {
            if (args[i].type == FFI_TYPE_POINTER)
                iargs[i] = (int64_t)(intptr_t)args[i].value.p;
            else
                iargs[i] = args[i].value.i;
        }
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
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

    /* 路径 2: 纯浮点参数 */
    if (icount == 0) {
        double dargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++)
            dargs[i] = args[i].value.d;
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            result.d = call_pure_double(func, dargs, total);
        } else {
            result.i = (int64_t)call_pure_double(func, dargs, total);
        }
        return result;
    }

    /* 路径 3: 混合参数 ≤4 个 → 精确分发 */
    if (total <= 4) {
        if (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT) {
            result.d = call_mixed_double_ret(func, args, total);
        } else {
            result.i = call_mixed_int_ret(func, args, total);
        }
        return result;
    }

    /* 路径 4: 混合参数回退策略 */
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

/* 通用调用入口 - Linux 平台转发到 ffi_call_sysv */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_sysv(func, sig, args);
}

#endif /* !_WIN32 */
