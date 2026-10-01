/* Leno FFI - ARM64 (AArch64) 实现
 * AAPCS64 (ARM Architecture Procedure Call Standard) 调用约定
 *
 * AAPCS64 ABI 规则:
 *   - 整数/指针参数: X0, X1, ... X7（按出现顺序）
 *   - 浮点参数: V0, V1, ... V7（**与整数各自独立计数**，按出现顺序）
 *     例: foo(int, double, int) → X0=int1, V0=double1, X1=int2
 *   - 返回值: X0 (整数/指针), V0 (浮点；float 在低 32 位)
 *   - 无需影子空间；SP 在发起 call 处必须 16 字节对齐
 *   - 超出寄存器的参数按**原始位置顺序**压栈（[SP+0] = 第 1 个溢出参数）
 *
 * 与 System V AMD64 的对比:
 *   - 整数寄存器: 8 个 (X0-X7) vs 6 个 (RDI-R9) → 多 2 个寄存器参数
 *   - 浮点寄存器: 8 个 (V0-V7) vs 8 个 (XMM0-XMM7) → 相同
 *   - 无影子空间 → 相同
 *   - 寄存器分配模型完全相同：整数/浮点**独立计数** ✓
 *     （⚠ 与 Win64 不同：那边是"第 i 个参数占第 i 号槽"的位置配对模型）
 *
 * 调用策略（与 Windows/Linux 侧同构，见 leno_ffi_win64.c 的文件头 ✓）:
 *   1. 纯整数/指针参数 → call_pure_int（C 函数指针，编译器摆寄存器/压栈 ✓）
 *   2. 纯浮点参数（≤8）**且返回也是浮点** → call_pure_double
 *   3. 其余（任何混合、浮点 >8、全浮点但整数返回、f32 返回）→ 通用汇编桩 leno_aapcs_call
 *
 * ★★ 2026-10-01：**废弃"按类型组合枚举函数指针 + 全 int64 回退"的老做法** ★★
 *   理由与另外两个平台完全相同（枚举补不全，组合数随参数个数指数增长），而且它还：
 *     · 某些 f32 组合**未枚举** ⇒ 老代码静默 `return 0`（函数根本没被调用 ✗）
 *     · f32 与 f64 混排会误配到 double 分支（那些分支只判 is_double、不要求其余槽非浮点）
 *       ⇒ 被调方拿到错值 ✗
 *     · 回退路径把浮点当 int64 走整数寄存器/栈，而被调方在 **V** 寄存器里等它 ⇒ 必错 ✗
 *       老代码自己在回退时还 `fprintf` 一句"浮点可能无法正确传递"——一份自己都不相信的实现 ✗
 *   三平台同源的两个既有缺陷（在 Windows 侧定位并修好，本文件一并收口）：
 *     · f32 返回值：前端 `ffi.c` 读 `result.f`，后端写 `result.d`（同一联合体）⇒ 恒为 0 ✗
 *     · "全浮点参数 + 整数返回"用 `double (*)(...)` 指针调用 ⇒ 从 V0 取值（真值在 X0）✗
 *   ⇒ 现在两者都由桩统一收口（"取低 4 字节 + 写 .f"只在 call_generic 里实现一次 ✓）
 *
 * ⚠⚠ **本文件尚未在真实 arm64 硬件上验证过** ⚠⚠
 *   开发机只有 x86-64（且无 AArch64 交叉工具链）⇒ 这里的汇编只经过逐行核对，
 *   **没有**经过汇编器与实机运行。
 *   首次在 arm64 上使用时，请务必先跑：
 *       build/leno assert/test_ffi_abi.leno        （38 例 ABI 边界 + 压力，会现场编译探针并逐项校验）
 *   如遇异常，按这三处依次排查：
 *     ① 调用帧偏移量：AapcsCallFrame 的字段偏移与汇编里的立即数必须一致
 *        （下面有 offsetof 的**编译期断言**顶着 ✓，改结构体就会编译失败 ✓）
 *     ② 压栈实参的**顺序**（倒序写、[SP+0] 必须是第 1 个溢出参数）与 **16 字节对齐**
 *        （本实现是"一次 sub 分配 (nstk + nstk&1) * 8 字节"，恒为 16 的倍数 ✓）
 *     ③ 栈参数只在寄存器用尽后才出现：整数看 X0-X7、浮点看 V0-V7，**两个序列独立计数** ✓
 */
#include "leno_ffi.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#if defined(__arm64__) || defined(__aarch64__)

/* ========================================================================
 *  1. 调用帧：把"已分类好的参数 + 出参"一次交给汇编桩
 *     （AAPCS64 有 8 个参数寄存器，足够直接传；但为与 Win64 侧同构、
 *       也为了少一处寄存器/栈混排的地方，统一用**一个结构体指针** ✓）
 *     ⚠ 偏移量必须与下面汇编里的立即数一致：用 offsetof 做**编译期断言**兜住 ✓
 * ======================================================================== */
typedef struct {
    void*     fn;      /*   0: 被调函数 */
    uint64_t  gp[8];   /*   8: 整数/指针 → X0-X7 */
    uint64_t  fp[8];   /*  72: 浮点原始位模式 → V0-V7（f32 在低 4 字节 ✓） */
    uint64_t* stk;     /* 136: 放不下的参数，按**原始位置顺序** */
    int64_t   nstk;    /* 144: stk 里的个数 */
    uint64_t  ret_i;   /* 152: 返回 X0 */
    double    ret_d;   /* 160: 返回 V0 的低 8 字节 */
} AapcsCallFrame;

#define AAPCS_OFF_FN     0
#define AAPCS_OFF_GP     8
#define AAPCS_OFF_FP    72
#define AAPCS_OFF_STK  136
#define AAPCS_OFF_NSTK 144
#define AAPCS_OFF_RETI 152
#define AAPCS_OFF_RETD 160

typedef char aapcs_layout_fn   [(offsetof(AapcsCallFrame, fn)    == AAPCS_OFF_FN)   ? 1 : -1];
typedef char aapcs_layout_gp   [(offsetof(AapcsCallFrame, gp)    == AAPCS_OFF_GP)   ? 1 : -1];
typedef char aapcs_layout_fp   [(offsetof(AapcsCallFrame, fp)    == AAPCS_OFF_FP)   ? 1 : -1];
typedef char aapcs_layout_stk  [(offsetof(AapcsCallFrame, stk)   == AAPCS_OFF_STK)  ? 1 : -1];
typedef char aapcs_layout_nstk [(offsetof(AapcsCallFrame, nstk)  == AAPCS_OFF_NSTK) ? 1 : -1];
typedef char aapcs_layout_reti [(offsetof(AapcsCallFrame, ret_i) == AAPCS_OFF_RETI) ? 1 : -1];
typedef char aapcs_layout_retd [(offsetof(AapcsCallFrame, ret_d) == AAPCS_OFF_RETD) ? 1 : -1];

/* ========================================================================
 *  2. 通用 AAPCS64 调用桩（汇编）
 *
 *  C 原型:  void leno_aapcs_call(AapcsCallFrame* f);      // 入参在 X0
 *
 *  要点：
 *    · 帧：stp x29,x30 → 再存 x19-x24（都 callee-saved）⇒ sp 始终 16 对齐 ✓
 *    · 栈参数：**一次** `sub sp, sp, (nstk + (nstk&1)) * 8`（补一格保证 16 对齐），
 *      然后把 stk[i] 写到 [sp + i*8] —— 即 [SP+0] 就是第 1 个溢出参数 ✓
 *      （AAPCS64 无 push 指令；一次分配比逐个 stp 更省心，也天然满足对齐 ✓）
 *    · 参数：ldp 一次装两个（x0-x7 / d0-d7）✓
 *    · 返回后先存 X0 / V0，再用 x29 恢复 sp（丢弃栈参数），最后弹回 ✓
 *    · 变参：AAPCS64 没有 SysV 那种"用 AL 报告向量寄存器个数"的约定 ⇒ 无需额外处理 ✓
 * ======================================================================== */

/* 汇编符号名：Mach-O 需要前导下划线，ELF 不需要 */
#ifdef __APPLE__
#  define LENO_AAPCS_SYM(x)  "_" x
#else
#  define LENO_AAPCS_SYM(x)  x
#endif

extern void leno_aapcs_call(AapcsCallFrame* f);

__asm__(
    ".text\n"
    ".globl " LENO_AAPCS_SYM("leno_aapcs_call") "\n"
    LENO_AAPCS_SYM("leno_aapcs_call") ":\n"
    "    stp x29, x30, [sp, #-16]!\n"
    "    mov x29, sp\n"
    "    stp x19, x20, [sp, #-16]!\n"
    "    stp x21, x22, [sp, #-16]!\n"
    "    stp x23, x24, [sp, #-16]!\n"
    "    mov x19, x0\n"                                  /* 调用帧 */
    "    ldr x20, [x19, #0]\n"                           /* fn */
    "    ldr x21, [x19, #144]\n"                         /* nstk */
    "    cbz x21, 3f\n"
    "    and x24, x21, #1\n"                             /* nstk 为奇数 ⇒ 补一格 */
    "    add x24, x24, x21\n"
    "    lsl x24, x24, #3\n"                             /* 字节数（恒为 16 的倍数 ✓）*/
    "    sub sp, sp, x24\n"
    "    mov x0, sp\n"                                   /* 目标游标：⚠ 不能直接写
                                                         *   `str x24, [sp, x22, lsl #3]` ——
                                                         *   AArch64 **不允许 SP 作为寄存器偏移
                                                         *   寻址的基址**（SP 只能配立即数偏移）
                                                         *   会直接汇编失败 ✗。此刻 x0 空闲
                                                         *   （调用帧已在 x19 ✓），借它当基址 ✓ */
    "    ldr x23, [x19, #136]\n"                         /* stk 源 */
    "    mov x22, xzr\n"
    "1:\n"
    "    ldr x24, [x23, x22, lsl #3]\n"
    "    str x24, [x0, x22, lsl #3]\n"                   /* stk[i] → [sp + i*8] ✓ */
    "    add x22, x22, #1\n"
    "    cmp x22, x21\n"
    "    b.lt 1b\n"
    "3:\n"
    /* ---- 整数/指针 → X0-X7 ---- */
    "    ldp x0, x1, [x19, #8]\n"
    "    ldp x2, x3, [x19, #24]\n"
    "    ldp x4, x5, [x19, #40]\n"
    "    ldp x6, x7, [x19, #56]\n"
    /* ---- 浮点 → V0-V7（f32 的位模式也在低 4 字节，ldr d 一并加载没问题）---- */
    "    ldp d0, d1, [x19, #72]\n"
    "    ldp d2, d3, [x19, #88]\n"
    "    ldp d4, d5, [x19, #104]\n"
    "    ldp d6, d7, [x19, #120]\n"
    "    blr x20\n"
    "    str x0, [x19, #152]\n"                          /* ret_i = X0 */
    "    str d0, [x19, #160]\n"                          /* ret_d = V0 低 8 字节 */
    "    mov sp, x29\n"                                  /* 丢弃栈参数，回到保存寄存器处 */
    "    ldp x19, x20, [sp], #16\n"
    "    ldp x21, x22, [sp], #16\n"
    "    ldp x23, x24, [sp], #16\n"
    "    ldp x29, x30, [sp], #16\n"
    "    ret\n"
);

/* ========================================================================
 *  3. 纯整数/指针参数的函数指针类型（0~12 个参数）
 *     —— 这条路径交给 C 编译器按 ABI 分配（X0-X7 + 栈 ✓）
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
 *  4. 纯浮点参数的函数指针类型（0~8 个参数；第 9 个起由编译器压栈 ✓）
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
 *  5. 辅助函数
 * ======================================================================== */
static int is_float_type(FFIType t) {
    return t == FFI_TYPE_DOUBLE || t == FFI_TYPE_FLOAT;
}

/* ========================================================================
 *  6. 核心调用实现：纯整数/指针参数分发
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
 *  7. 核心调用实现：纯浮点参数分发
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
 *  8. 通用分发：把参数按 AAPCS64 规则分类后交给 leno_aapcs_call
 *
 *  分类规则（**与 SysV 同一套模型**，只是寄存器个数不同：8/8 vs 6/8）：
 *    · 整数/指针按出现顺序进 X0-X7；浮点按出现顺序进 V0-V7
 *      —— **两个序列独立计数** ✓（这就是 foo(int,double,int) → X0,V0,X1 的由来）
 *    · 各自的寄存器用完后，落进 stk[]，且**保持原始位置顺序** ✓
 *   ⚠ 别照抄 Win64 的"位置配槽"模型：那是 MS x64 独有的 ✓
 * ======================================================================== */
static FFIValue call_generic(void* func, const FFISignature* sig, const FFIArg* args, int total) {
    AapcsCallFrame f;
    uint64_t stk[FFI_MAX_ARGS];
    int ngp = 0, nfp = 0, nstk = 0;

    memset(&f, 0, sizeof(f));
    memset(stk, 0, sizeof(stk));
    f.fn = func;

    for (int i = 0; i < total; i++) {
        FFIType t = args[i].type;
        if (is_float_type(t)) {
            uint64_t bits = 0;
            if (t == FFI_TYPE_FLOAT) {
                /* f32：位模式放低 4 字节 ⇒ 桩里 ldr d 加载后低 32 位仍是这个 float ✓ */
                float v = args[i].value.f;
                memcpy(&bits, &v, sizeof(float));
            } else {
                memcpy(&bits, &args[i].value.d, sizeof(double));
            }
            if (nfp < 8) f.fp[nfp++] = bits; else stk[nstk++] = bits;
        } else {
            uint64_t v = (t == FFI_TYPE_POINTER)
                       ? (uint64_t)(intptr_t)args[i].value.p
                       : (uint64_t)args[i].value.i;
            if (ngp < 8) f.gp[ngp++] = v; else stk[nstk++] = v;
        }
    }

    f.stk = stk;
    f.nstk = (int64_t)nstk;
    leno_aapcs_call(&f);

    FFIValue result;
    result.i = 0;   /* 先清零：FFIValue 是**联合体**，写 .f 后高 4 字节也必须是确定的 ✓ */
    if (sig->ret_type == FFI_TYPE_DOUBLE) {
        result.d = f.ret_d;
    } else if (sig->ret_type == FFI_TYPE_FLOAT) {
        /* f32 返回值在 V0 的**低 4 字节**，不能按 double 解释。
         * ⚠ 必须写 `.f`（不是 `.d`）：前端 `ffi.c` 读的是 `result.f`
         *   （`case TYPE_F32: return val_float((double)result.f);` ✓）——
         *   两边是同一个联合体的不同成员，**读写约定必须成对**。
         *   写 `.d` 时前端读到的是 double 的低 4 字节（7.0 = 0x401C000000000000
         *   ⇒ 低 4 字节全 0）⇒ f32 返回值**恒为 0** ✗ */
        float v;
        memcpy(&v, &f.ret_d, sizeof(float));
        result.f = v;
    } else {
        result.i = (int64_t)f.ret_i;
    }
    return result;
}

/* ========================================================================
 *  9. ffi_call_aapcs - AAPCS64 调用约定的核心实现
 * ======================================================================== */
FFIValue ffi_call_aapcs(void* func, const FFISignature* sig, const FFIArg* args) {
    FFIValue result;
    result.i = 0;
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

    /* ★ f32 返回一律走通用桩：C 函数指针按 double 读 V0 会把高 4 字节的垃圾一起读进来，
     *   而 f32 只需低 4 字节；"取低 4 字节 + 写成 .f" 只在桩里实现一次 ✓ */
    if (sig->ret_type == FFI_TYPE_FLOAT) {
        return call_generic(func, sig, args, total);
    }

    /* ===== 路径 1: 纯整数/指针参数 ===== */
    if (dcount == 0) {
        /* ⚠ "整数参数 + 浮点返回"这一支的老 switch 只枚举了 0~4 个参数，5 个以上
         *   **静默返回 0** ✗ ⇒ 参数多于 4 个时交给通用桩 ✓ */
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

    /* ===== 路径 2: 纯浮点参数（≤8）**且返回也是浮点** =====
     *   ★ `ret_is_float` 不能省：否则 `int64_t f(double, ...)`（全浮点参数 + **整数返回**）
     *   会走这条 `double (*)(...)` 路径 ⇒ **从 V0 取返回值**（真值在 X0）⇒ 拿到残留浮点垃圾 ✗ */
    if (icount == 0 && total <= 8 && ret_is_float) {
        double dargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++)
            dargs[i] = args[i].value.d;
        result.d = call_pure_double(func, dargs, total);
        return result;
    }

    /* ===== 路径 3: 其余 → 通用桩
     *   （任何整数+浮点混合，或纯浮点 >8，或"全浮点参数但整数返回"✓）===== */
    return call_generic(func, sig, args, total);
}

/* 通用调用入口 - ARM64 平台转发到 ffi_call_aapcs */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_aapcs(func, sig, args);
}

#endif /* __arm64__ || __aarch64__ */
