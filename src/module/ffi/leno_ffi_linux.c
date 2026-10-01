/* Leno FFI - Linux/macOS x64 实现
 * System V AMD64 ABI 调用约定
 *
 * System V AMD64 ABI 规则:
 *   - 整数/指针参数: RDI, RSI, RDX, RCX, R8, R9（与浮点**各自独立**计数，按参数位置分配）
 *   - 浮点参数: XMM0-XMM7（同上）
 *     例: foo(int, double, int) → RDI=int1, XMM0=double1, RSI=int2
 *   - 返回值: RAX (整数/指针), XMM0 (浮点)
 *   - 无需影子空间；整数超过 6 个 / 浮点超过 8 个的参数，按**原始位置顺序**压栈
 *
 * 调用策略：
 *   1. 纯整数/指针参数 → call_pure_int（1~12 个，走预定义函数指针）
 *   2. 纯浮点参数（≤6）→ call_pure_double
 *   3. 其余（**任何**整数+浮点混合）→ 通用汇编调用桩 leno_sysv_call
 *
 * 为什么需要通用桩（2026-09-30，修 PVZ 段错误）：
 *   早先混合参数靠"按类型组合枚举函数指针"覆盖（2/3/4 参数若干组合 + 5 参数 f32 特例），
 *   超出覆盖面的调用落到"整型回退"——把 double 的位模式当 int64 塞进**通用寄存器**，
 *   而被调函数按 ABI 期望它在 **XMM** 里 ⇒ 参数整体错位。
 *   实测触发点：`SDL_RenderTextureRotated(renderer, texture, src, dst, f64 angle, center, flip)`
 *   —— 7 个参数含 1 个 f64，angle 变成垃圾值，随后解引用出错、植物大战僵尸段错误 ✗。
 *   枚举法对参数个数是指数爆炸，永远补不全；故改为按 SysV 分类规则**精确分发**：
 *   整数按序进 RDI-R9、浮点按序进 XMM0-7、放不下的按原始顺序压栈，任意组合都正确 ✓
 */

#include "leno_ffi.h"
#include <string.h>
#include <stdio.h>

#if !defined(_WIN32) && !defined(__arm64__) && !defined(__aarch64__)

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
 *  2. 纯浮点参数的函数指针类型（0~6 个参数，XMM0-XMM5）
 * ======================================================================== */
typedef double (*fd0)(void);
typedef double (*fd1)(double);
typedef double (*fd2)(double, double);
typedef double (*fd3)(double, double, double);
typedef double (*fd4)(double, double, double, double);
typedef double (*fd5)(double, double, double, double, double);
typedef double (*fd6)(double, double, double, double, double, double);

/* ========================================================================
 *  3. 辅助函数：判断参数类型是否为浮点
 * ======================================================================== */
static int is_float_type(FFIType t) {
    return t == FFI_TYPE_DOUBLE || t == FFI_TYPE_FLOAT;
}

/* ========================================================================
 *  4. 核心调用实现：纯整数/指针参数分发
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
 *  5. 核心调用实现：纯浮点参数分发
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
 *  6. 通用 SysV 调用桩（汇编）
 *
 *  C 原型:
 *    void leno_sysv_call(void* fn,
 *                        const uint64_t* gp,   // 已分类：整数/指针，最多 6 个
 *                        const uint64_t* fp,   // 已分类：浮点原始位模式，固定 8 槽
 *                        const uint64_t* stk,  // 放不下的参数，按**原始位置顺序**
 *                        int64_t nstk,         // stk 里的个数
 *                        int64_t nfp,          // 实际用了几个 XMM（写 AL，变参函数需要）
 *                        SysvCallRet* out);    // 出参：rax + xmm0 原始 8 字节
 *
 *  要点：
 *    · 第 7 个参数 out 由 C 编译器放在栈上 ⇒ 从 16(%rbp) 取（压过 rbp 后）
 *    · 压栈参数**倒序**压入，使 stk[0] 落在最靠近返回地址处（= 被调函数看到的第 1 个栈参数）
 *    · 调用前必须保持 rsp 16 字节对齐：本桩在 rbp 之后共压 3 个寄存器，
 *      故 (3 + 1) + nstk + pad 需为奇数 ⇒ nstk 为偶数时补压一个 0
 *    · 返回后先把结果写进出参，再用 rbp 恢复 rsp（丢弃压栈的实参），最后弹回
 * ======================================================================== */

/* 汇编符号名：Mach-O 需要前导下划线，ELF 不需要 */
#ifdef __APPLE__
#  define LENO_SYSV_SYM(x)  "_" x
#  define LENO_SYSV_TYPE(x) ""
#  define LENO_SYSV_SIZE(x) ""
#else
#  define LENO_SYSV_SYM(x)  x
#  define LENO_SYSV_TYPE(x) ".type " x ", @function\n"
#  define LENO_SYSV_SIZE(x) ".size " x ", .-" x "\n"
#endif

typedef struct {
    uint64_t i;   /* RAX —— 整数/指针返回值 */
    double   d;   /* XMM0 的原始 8 字节 —— 浮点返回值 */
} SysvCallRet;

extern void leno_sysv_call(void* fn, const uint64_t* gp, const uint64_t* fp,
                           const uint64_t* stk, int64_t nstk, int64_t nfp,
                           SysvCallRet* out);

__asm__(
    ".text\n"
    ".globl " LENO_SYSV_SYM("leno_sysv_call") "\n"
    LENO_SYSV_TYPE("leno_sysv_call")
    LENO_SYSV_SYM("leno_sysv_call") ":\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    pushq %rbx\n"                                  /* fn 要在参数寄存器被占用后仍可用 */
    "    pushq %r12\n"                                  /* out 出参指针 */
    "    pushq %r13\n"                                  /* nfp */
    "    movq  %rdi, %rbx\n"
    "    movq  %rsi, %r10\n"                            /* gp */
    "    movq  %rdx, %r11\n"                            /* fp */
    "    movq  %r9,  %r13\n"                            /* nfp */
    "    movq  16(%rbp), %r12\n"                        /* out（第 7 个参数在栈上） */
    /* ---- 压栈参数（含对齐补位）---- */
    "    testb $1, %r8b\n"
    "    jnz   1f\n"                                    /* nstk 为奇数 ⇒ 不用补位 */
    "    pushq $0\n"                                    /* nstk 为偶数 ⇒ 补 8 字节保持 16 对齐 */
    "1:\n"
    "    testq %r8, %r8\n"
    "    jz    3f\n"
    "2:\n"
    "    decq  %r8\n"
    "    pushq (%rcx,%r8,8)\n"
    "    testq %r8, %r8\n"
    "    jnz   2b\n"
    "3:\n"
    /* ---- 整数/指针 → RDI,RSI,RDX,RCX,R8,R9 ---- */
    "    movq  0(%r10),  %rdi\n"
    "    movq  8(%r10),  %rsi\n"
    "    movq  16(%r10), %rdx\n"
    "    movq  24(%r10), %rcx\n"
    "    movq  32(%r10), %r8\n"
    "    movq  40(%r10), %r9\n"
    /* ---- 浮点 → XMM0-XMM7（f32 的位模式也在低 4 字节，movsd 一并加载没问题）---- */
    "    movsd 0(%r11),  %xmm0\n"
    "    movsd 8(%r11),  %xmm1\n"
    "    movsd 16(%r11), %xmm2\n"
    "    movsd 24(%r11), %xmm3\n"
    "    movsd 32(%r11), %xmm4\n"
    "    movsd 40(%r11), %xmm5\n"
    "    movsd 48(%r11), %xmm6\n"
    "    movsd 56(%r11), %xmm7\n"
    "    movl  %r13d, %eax\n"                            /* AL = 使用的向量寄存器数（变参约定） */
    "    call  *%rbx\n"
    "    movq  %rax, 0(%r12)\n"
    "    movsd %xmm0, 8(%r12)\n"
    "    leaq  -24(%rbp), %rsp\n"                        /* 丢弃压栈实参，回到保存寄存器处 */
    "    popq  %r13\n"
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    ret\n"
    LENO_SYSV_SIZE("leno_sysv_call")
);

/* ========================================================================
 *  7. 通用分发：把参数按 SysV 规则分类后交给 leno_sysv_call
 * ======================================================================== */
static FFIValue call_generic(void* func, const FFISignature* sig, const FFIArg* args, int total) {
    uint64_t gp[6]   = {0};   /* 整数/指针（无则填 0，桩会无条件加载） */
    uint64_t fp[8]   = {0};   /* 浮点原始位模式（同上，固定 8 槽，桩无条件加载） */
    uint64_t stk[FFI_MAX_ARGS] = {0};
    int ngp = 0, nfp = 0, nstk = 0;

    for (int i = 0; i < total; i++) {
        FFIType t = args[i].type;
        if (is_float_type(t)) {
            uint64_t bits = 0;
            if (t == FFI_TYPE_FLOAT) {
                /* f32：位模式放低 4 字节 ⇒ 桩里 movsd 加载后低 32 位仍是这个 float ✓ */
                float f = args[i].value.f;
                memcpy(&bits, &f, sizeof(float));
            } else {
                memcpy(&bits, &args[i].value.d, sizeof(double));
            }
            if (nfp < 8) fp[nfp++] = bits; else stk[nstk++] = bits;
        } else {
            uint64_t v = (t == FFI_TYPE_POINTER)
                       ? (uint64_t)(intptr_t)args[i].value.p
                       : (uint64_t)args[i].value.i;
            if (ngp < 6) gp[ngp++] = v; else stk[nstk++] = v;
        }
    }

    SysvCallRet ret;
    ret.i = 0;
    ret.d = 0.0;
    leno_sysv_call(func, gp, fp, stk, nstk, nfp, &ret);

    FFIValue result = {0};
    if (sig->ret_type == FFI_TYPE_DOUBLE) {
        result.d = ret.d;
    } else if (sig->ret_type == FFI_TYPE_FLOAT) {
        /* f32 返回值是 XMM0 的**低 4 字节**，不能按 double 解释。
         * ⚠ 必须写 `.f`（不是 `.d`）：前端 `ffi.c` 读的是 `result.f`
         *   （`case TYPE_F32: return val_float((double)result.f);` ✓）——
         *   两边是同一个联合体的不同成员，**读写约定必须成对**。
         *   实测（2026-10-01）：写 `.d` 时前端读 `.f` 拿到的是 double 的低 4 字节
         *   （7.0 = 0x401C000000000000 ⇒ 低 4 字节全 0）⇒ f32 返回值**恒为 0** ✗ */
        float f;
        memcpy(&f, &ret.d, sizeof(float));
        result.f = f;
    } else {
        result.i = (int64_t)ret.i;
    }
    return result;
}

/* ========================================================================
 *  8. ffi_call_sysv - System V AMD64 调用约定的核心实现
 * ======================================================================== */
FFIValue ffi_call_sysv(void* func, const FFISignature* sig, const FFIArg* args) {
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

    int ret_is_float = (sig->ret_type == FFI_TYPE_DOUBLE || sig->ret_type == FFI_TYPE_FLOAT);

    /* ★ f32 返回一律走通用桩（2026-10-01，与 Windows 侧同口径）：
     *   C 函数指针按 `double` 读 XMM0 会把高 4 字节的垃圾一起读进来，而 f32 只需低 4 字节；
     *   且"取低 4 字节 + 写成 `.f`"这套约定只在桩里实现一次（见 call_generic 的收尾 ✓）
     *   ⇒ 少一处要同步的读写约定，少一类"某条路径忘了转"的坑 ✓
     *   （实测症状：f32 返回值**恒为 0** —— 前端读 `.f`，后端写的却是 `.d` ✓） */
    if (sig->ret_type == FFI_TYPE_FLOAT) {
        return call_generic(func, sig, args, total);
    }

    /* ===== 路径 1: 纯整数/指针参数 ===== */
    if (dcount == 0) {
        /* ⚠ "整数参数 + 浮点返回"这一支老写法只枚举了 0~4 个参数，5 个以上**静默返回 0** ✗
         *   ⇒ 参数多于 4 个时交给通用桩（桩按 ret_type 决定读 RAX 还是 XMM0 ✓） */
        if (!(ret_is_float && total > 4)) {
            int64_t iargs[FFI_MAX_ARGS];
            for (int i = 0; i < total; i++) {
                if (args[i].type == FFI_TYPE_POINTER)
                    iargs[i] = (int64_t)(intptr_t)args[i].value.p;
                else
                    iargs[i] = args[i].value.i;
            }
            if (ret_is_float) {
                /* 所有参数是整数，但返回浮点 */
                switch (total) {
                    case 0: result.d = ((double (*)(void))func)(); break;
                    case 1: result.d = ((double (*)(int64_t))func)(iargs[0]); break;
                    case 2: result.d = ((double (*)(int64_t, int64_t))func)(iargs[0], iargs[1]); break;
                    case 3: result.d = ((double (*)(int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2]); break;
                    case 4: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3]); break;
                    default: break;   /* 不可达：total > 4 已在上面转给通用桩 ✓ */
                }
            } else {
                result.i = call_pure_int(func, iargs, total);
            }
            return result;
        }
        return call_generic(func, sig, args, total);
    }

    /* ===== 路径 2: 纯浮点参数（≤6，全在 XMM0-XMM5）**且返回也是浮点** =====
     *   ★ 2026-10-01：必须加 `ret_is_float`。老条件只要求"参数全浮点"，于是
     *   `int64_t f(double, double, ...)`（全浮点参数 + **整数返回**）也走这条，
     *   而本条用的是 `double (*)(double, ...)` 函数指针 ⇒ **从 XMM0 取返回值**，
     *   可真正的返回值在 **RAX** ⇒ 拿到的是残留浮点的垃圾值 ✗
     *   实测症状（极难查）：`i64 f(f64×6)` 稳定返回 0 / 6 这类"无意义但确定"的值，
     *   给被调方加一行 fprintf 就换个值（它改变了 XMM0 里残留的是什么）✓ */
    if (icount == 0 && total <= 6 && ret_is_float) {
        double dargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++)
            dargs[i] = args[i].value.d;
        result.d = call_pure_double(func, dargs, total);
        return result;
    }

    /* ===== 路径 3: 其余 → 通用桩
     *   （任何整数+浮点混合，或纯浮点 >6，或"全浮点参数但整数返回"✓）===== */
    return call_generic(func, sig, args, total);
}

/* 通用调用入口 - Linux 平台转发到 ffi_call_sysv */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_sysv(func, sig, args);
}

#endif /* !_WIN32 && !__arm64__ && !__aarch64__ */