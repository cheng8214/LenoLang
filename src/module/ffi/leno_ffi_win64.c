/* Leno FFI - Windows x64 实现
 * Microsoft x64 调用约定
 *
 * Microsoft x64 ABI 规则:
 *   - **前 4 个参数按位置固定配对寄存器槽**（这是 Win64 与 SysV 最大的不同 ⚠）：
 *       第 1 个 → RCX 或 XMM0      第 3 个 → R8  或 XMM2
 *       第 2 个 → RDX 或 XMM1      第 4 个 → R9  或 XMM3
 *     即"第 i 个参数用第 i 号槽"，**浮点与整数不各自计数**（SysV/AAPCS64 才是各自计数 ✓）
 *     例: foo(int, double, int, double) → RCX=int1, XMM1=double2, R8=int3, XMM3=double4
 *   - 返回值: RAX (整数/指针), XMM0 (浮点)
 *   - 调用者须在栈上预留 32 字节**影子空间**（home space），紧随返回地址
 *   - 超过 4 个整数 / 4 个浮点的参数按**原始位置顺序**压栈
 *   - 变参（printf 类）另有"浮点实参要同时写进对应 GPR 槽"的 shadow copy 要求：
 *     本实现**未支持**（Leno 的 clib 声明里没有变参语法，用不到 ✓）
 *
 * 调用策略（与 Linux/macOS 侧同构，见 leno_ffi_linux.c ✓）:
 *   1. 纯整数/指针参数 → call_pure_int（C 函数指针，编译器摆寄存器/压栈 ✓）
 *   2. 纯浮点参数（≤6）→ call_pure_double
 *   3. 其余（**任何**整数+浮点混合）→ 通用汇编调用桩 leno_win64_call
 *
 * ★★ 2026-10-01：**废弃"按类型组合枚举函数指针"的老做法** ★★
 *   老做法为 2/3/4 参数的 f32/f64 组合各写一个 typedef + if 分支（~250 行），
 *   超出覆盖面就落到"把 double 位模式当 int64 塞通用寄存器"的回退，
 *   并由上层 ffi.c 用「>4 参数且前 4 位含浮点就抛错」兜底。枚举法根本补不全
 *   （组合数随参数个数指数增长），实测三种坏法（本机 gcc 造 DLL 当被调方实录 ✓）：
 *     · (i32, f32, i32, f32)         → 枚举表里**没有"2 个浮点"**这档 ⇒
 *                                      落到 return 0 ⇒ **函数一次都没被调用**（静默返回 0 ✗✗）
 *     · (f32, f64, i32, i32)         → 误配到 double 分支（那些分支只判 is_double、
 *                                      不要求其余槽非浮点）⇒ 被调方拿到错值（1.5 变 0 ✗）
 *     · (i32, f32, i32, f32, i32)    → 上层拦截直接抛「超过 Win64 精确分发上限」✗
 *   根因不是"信息不够"：clib 声明在编译期就写明了每个形参类型，运行期以
 *   `FFISignature.arg_types[]` + `FFIArg.type` 一路传到后端（leno_ffi.h ✓）——
 *   信息一直是够的，缺的是"拿到类型后自己按 ABI 摆寄存器"。
 *   ⇒ 现在与 Linux 侧同款：**拿类型表精确分类**（Win64 是"第 i 个参数占第 i 号槽"，
 *     由位置决定用 RCX/RDX/R8/R9 还是 XMM0-3；第 5 个起按原始位置压栈）⇒ 任意组合
 *     都正确 ✓（同时 ffi.c 里那段"超过上限就抛"的拦截已删除 ✓）
 */
#include "leno_ffi.h"
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#ifdef _WIN32

/* ========================================================================
 *  1. 调用帧：把"已分类好的参数 + 出参"一次交给汇编桩
 *     （把 7 个参数塞进汇编的寄存器/栈太啰嗦，Win64 只剩 4 个参数寄存器 ⇒
 *       统一用**一个结构体指针**，桩里只用 RCX 一个入参 ✓）
 *     ⚠ 偏移量必须与下面汇编里的立即数一致：用 offsetof 做**编译期断言**兜住 ✓
 * ======================================================================== */
typedef struct {
    void*     fn;      /*  0: 被调函数 */
    uint64_t  gp[4];   /*  8: 整数/指针 → RCX, RDX, R8, R9 */
    uint64_t  fp[4];   /* 40: 浮点原始位模式 → XMM0-XMM3（f32 在低 4 字节 ✓） */
    uint64_t* stk;     /* 72: 放不下的参数，按**原始位置顺序** */
    int64_t   nstk;    /* 80: stk 里的个数 */
    uint64_t  ret_i;   /* 88: 返回 RAX */
    double    ret_d;   /* 96: 返回 XMM0 的低 8 字节 */
} Win64CallFrame;

#define W64_OFF_FN     0
#define W64_OFF_GP     8
#define W64_OFF_FP    40
#define W64_OFF_STK   72
#define W64_OFF_NSTK  80
#define W64_OFF_RETI  88
#define W64_OFF_RETD  96

typedef char w64_layout_fn   [(offsetof(Win64CallFrame, fn)    == W64_OFF_FN)   ? 1 : -1];
typedef char w64_layout_gp   [(offsetof(Win64CallFrame, gp)    == W64_OFF_GP)   ? 1 : -1];
typedef char w64_layout_fp   [(offsetof(Win64CallFrame, fp)    == W64_OFF_FP)   ? 1 : -1];
typedef char w64_layout_stk  [(offsetof(Win64CallFrame, stk)   == W64_OFF_STK)  ? 1 : -1];
typedef char w64_layout_nstk [(offsetof(Win64CallFrame, nstk)  == W64_OFF_NSTK) ? 1 : -1];
typedef char w64_layout_reti [(offsetof(Win64CallFrame, ret_i) == W64_OFF_RETI) ? 1 : -1];
typedef char w64_layout_retd [(offsetof(Win64CallFrame, ret_d) == W64_OFF_RETD) ? 1 : -1];

/* ========================================================================
 *  2. 通用 Win64 调用桩（汇编）
 *
 *  C 原型:  void leno_win64_call(Win64CallFrame* f);      // 入参在 RCX
 *
 *  要点：
 *    · 入口帧：push rbp / push rbx（fn）/ push r12（帧指针）⇒ 之后 rsp ≡ 0 (mod 16)
 *    · **影子空间**：调目标前 sub rsp, 32；压栈实参在这之后压 ⇒ 影子空间正好
 *      紧贴返回地址（被调方 4 个寄存器实参的 home 槽 ✓），stk[0] 在其正上方
 *      即被调方看到的第 1 个栈参数 ✓
 *    · 压栈参数**倒序**压入；调用前必须 16 字节对齐：本桩在影子空间之后 rsp ≡ 0，
 *      故压栈个数应为**偶数** ⇒ nstk 为奇数时补压一个 0 ✓
 *    · 返回后先存结果（RAX / XMM0），再用 rbp 恢复 rsp（丢弃压栈实参），最后弹回 ✓
 *    · rbx / r12 是 callee-saved，已 push/pop ✓
 * ======================================================================== */
extern void leno_win64_call(Win64CallFrame* f);

__asm__(
    ".text\n"
    ".globl leno_win64_call\n"
    "leno_win64_call:\n"
    "    pushq %rbp\n"
    "    movq  %rsp, %rbp\n"
    "    pushq %rbx\n"                                  /* 被调函数地址 */
    "    pushq %r12\n"                                  /* 调用帧指针 */
    "    movq  %rcx, %r12\n"
    "    movq  0(%r12), %rbx\n"                        /* fn */
    "    movq  80(%r12), %rax\n"                       /* nstk */
    "    testq %rax, %rax\n"
    "    jz    3f\n"
    "    testb $1, %al\n"
    "    jz    1f\n"                                    /* nstk 为偶数 ⇒ 不用补位 */
    "    pushq $0\n"                                    /* nstk 为奇数 ⇒ 补 8 字节保持 16 对齐 */
    "1:\n"
    "    movq  72(%r12), %r11\n"                       /* stk */
    "2:\n"
    "    decq  %rax\n"
    "    pushq (%r11,%rax,8)\n"
    "    testq %rax, %rax\n"
    "    jnz   2b\n"
    "3:\n"
    /* ⚠ 影子空间必须**在压栈实参之后**再 sub：ABI 要求那 32 字节紧贴返回地址
     *   （被调方视角 = [rsp+8, rsp+40)），第 5 个参数紧随其后（= [rsp+40]）。
     *   若先 sub 再压实参，实参就会插在返回地址与影子空间之间 ⇒ 第 5+ 个参数整体错位 ✗
     *   （实测踩到：5 参数时前 4 个全对、第 5 个是垃圾值 ✓） */
    "    subq  $32, %rsp\n"
    /* ---- 整数/指针 → RCX,RDX,R8,R9 ---- */
    "    movq  8(%r12),  %rcx\n"
    "    movq  16(%r12), %rdx\n"
    "    movq  24(%r12), %r8\n"
    "    movq  32(%r12), %r9\n"
    /* ---- 浮点 → XMM0-XMM3（f32 的位模式也在低 4 字节，movsd 一并加载没问题）---- */
    "    movsd 40(%r12), %xmm0\n"
    "    movsd 48(%r12), %xmm1\n"
    "    movsd 56(%r12), %xmm2\n"
    "    movsd 64(%r12), %xmm3\n"
    "    callq *%rbx\n"
    "    movq  %rax, 88(%r12)\n"                       /* ret_i = RAX */
    "    movsd %xmm0, 96(%r12)\n"                      /* ret_d = XMM0 */
    "    leaq  -16(%rbp), %rsp\n"                      /* 丢弃压栈实参，回到保存寄存器处 */
    "    popq  %r12\n"
    "    popq  %rbx\n"
    "    popq  %rbp\n"
    "    retq\n"
);

/* ========================================================================
 *  3. 纯整数/指针参数的函数指针类型（0~12 个参数）
 *     —— 这条路径交给 C 编译器按 ABI 分配（最常见，且不依赖上面的汇编 ✓）
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
 *  4. 纯浮点参数的函数指针类型（0~6 个参数；第 5、6 个由编译器压栈 ✓）
 * ======================================================================== */
typedef double (*fd0)(void);
typedef double (*fd1)(double);
typedef double (*fd2)(double, double);
typedef double (*fd3)(double, double, double);
typedef double (*fd4)(double, double, double, double);
typedef double (*fd5)(double, double, double, double, double);
typedef double (*fd6)(double, double, double, double, double, double);

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
        default: return 0;
    }
}

/* ========================================================================
 *  8. 通用分发：把参数按 Win64 规则分类后交给 leno_win64_call
 *
 *  ★ 分类规则 = **按位置占槽**（不是"整数/浮点各自计数"⚠ 那是 SysV/AAPCS64 的模型）：
 *      第 i 个参数（i 从 0 起）：i < 4 ⇒ 占第 i 号槽（浮点写 fp[i]、整数写 gp[i]）；
 *                                i >= 4 ⇒ 落进 stk[]，保持**原始位置顺序** ✓
 *    ⚠ 实测踩过：一开始按"各自计数"分类（浮点顺序进 XMM0、整数顺序进 RCX…），
 *      调 `(int,double,int,double)` 时被调方收到 `a=1 b=4.5 c=0 d=0` ✗ ——
 *      因为它读的是**第 2 号槽 XMM1**，而我们只填了 XMM0/XMM1 的前两个浮点 ⇒ 整体错位 ✓
 * ======================================================================== */
static FFIValue call_generic(void* func, const FFISignature* sig, const FFIArg* args, int total) {
    Win64CallFrame f;
    uint64_t stk[FFI_MAX_ARGS];
    int nstk = 0;

    memset(&f, 0, sizeof(f));
    memset(stk, 0, sizeof(stk));
    f.fn = func;

    for (int i = 0; i < total; i++) {
        FFIType t = args[i].type;
        int is_f = is_float_type(t);
        uint64_t v = 0;

        if (is_f) {
            if (t == FFI_TYPE_FLOAT) {
                /* f32：位模式放低 4 字节 ⇒ 桩里 movsd 加载后低 32 位仍是这个 float ✓ */
                float fv = args[i].value.f;
                memcpy(&v, &fv, sizeof(float));
            } else {
                memcpy(&v, &args[i].value.d, sizeof(double));
            }
        } else {
            v = (t == FFI_TYPE_POINTER)
              ? (uint64_t)(intptr_t)args[i].value.p
              : (uint64_t)args[i].value.i;
        }

        if (i < 4) {
            if (is_f) f.fp[i] = v; else f.gp[i] = v;
        } else {
            stk[nstk++] = v;
        }
    }

    f.stk = stk;
    f.nstk = (int64_t)nstk;
    leno_win64_call(&f);

    FFIValue result;
    result.i = 0;   /* 先清零：FFIValue 是**联合体**，写 .f 后高 4 字节也必须是确定的 ✓ */
    if (sig->ret_type == FFI_TYPE_DOUBLE) {
        result.d = f.ret_d;
    } else if (sig->ret_type == FFI_TYPE_FLOAT) {
        /* f32 返回值是 XMM0 的**低 4 字节**，不能按 double 解释。
         * ⚠ 必须写 `.f`（不是 `.d`）：前端 `ffi.c` 读的是 `result.f`
         *   （`case TYPE_F32: return val_float((double)result.f);` ✓）——
         *   两边是同一个联合体的不同成员，**读写约定必须成对**。
         *   实测：写 `.d` 时前端读 `.f` 拿到的是 double 的低 4 字节
         *   （7.0 = 0x401C000000000000 ⇒ 低 4 字节全 0）⇒ f32 返回值恒为 0 ✗ */
        float v;
        memcpy(&v, &f.ret_d, sizeof(float));
        result.f = v;
    } else {
        result.i = (int64_t)f.ret_i;
    }
    return result;
}

/* ========================================================================
 *  9. ffi_call_win64 - Windows x64 调用约定的核心实现
 * ======================================================================== */
FFIValue ffi_call_win64(void* func, const FFISignature* sig, const FFIArg* args) {
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

    /* ★ f32 返回一律走通用桩（不进下面两条 C 函数指针路径）：
     *   C 函数指针按 double 读 XMM0 会把高 4 字节的垃圾一起读进来，而 f32 只需低 4 字节；
     *   而"取低 4 字节 + 写成 `.f`"这套约定只在桩里实现一次（见 call_generic 的收尾 ✓）
     *   ⇒ 少一处要同步的读写约定，少一类"某条路径忘了转"的坑 ✓
     *   （实测：走 C 路径时 f32 返回值恒为 0 ✗ —— 前端读 `.f`，写的是 `.d` ✓） */
    if (sig->ret_type == FFI_TYPE_FLOAT) {
        return call_generic(func, sig, args, total);
    }

    /* ===== 路径 1: 纯整数/指针参数 ===== */
    if (dcount == 0) {
        /* ★ 但"整数参数 + 浮点返回"这一支的老写法只枚举了 0~4 个参数，5 个以上
         *   **静默返回 0** ✗ ⇒ 参数多于 4 个时交给通用桩（桩按 RAX/XMM0 双写结果 ✓） */
        if (!(ret_is_float && total > 4)) {
            int64_t iargs[FFI_MAX_ARGS];
            for (int i = 0; i < total; i++) {
                if (args[i].type == FFI_TYPE_POINTER)
                    iargs[i] = (int64_t)(intptr_t)args[i].value.p;
                else
                    iargs[i] = args[i].value.i;
            }
            if (ret_is_float) {
                switch (total) {
                    case 0: result.d = ((double (*)(void))func)(); break;
                    case 1: result.d = ((double (*)(int64_t))func)(iargs[0]); break;
                    case 2: result.d = ((double (*)(int64_t, int64_t))func)(iargs[0], iargs[1]); break;
                    case 3: result.d = ((double (*)(int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2]); break;
                    case 4: result.d = ((double (*)(int64_t, int64_t, int64_t, int64_t))func)(iargs[0], iargs[1], iargs[2], iargs[3]); break;
                    default: break;
                }
            } else {
                result.i = call_pure_int(func, iargs, total);
            }
            return result;
        }
        return call_generic(func, sig, args, total);
    }

    /* ===== 路径 2: 纯浮点参数（≤6；5、6 个由编译器压栈 ✓）**且返回也是浮点** =====
     * ★ 2026-10-01 修正：老条件只要求"参数全浮点"，于是
     *   `int64_t f(double, double, ...)`（全浮点参数 + **整数返回**）也被塞进这条路径，
     *   而本条用的是 `double (*)(double, ...)` 函数指针 ⇒ **从 XMM0 取返回值**、
     *   再 `(int64_t)` 转一下 —— 可真正的返回值在 **RAX** ⇒ 拿到的是残留浮点的垃圾值 ✗
     *   实测症状（极难查）：`i64 f(f64×6)` 稳定返回 0 或 6 这种"无意义但确定"的值，
     *   **给被调方加一行 fprintf 就换个值**（因为它改变了 XMM0 里残留的是什么）✓
     *   ⇒ 要求 `ret_is_float`：返回整数时交给通用桩（桩按 ret_type 决定读 RAX 还是 XMM0 ✓）
     *   ⚠ Linux/arm64 侧是同一份写法（`result.i = (int64_t)call_pure_double(...)`）⇒ 同款缺陷 ✓ */
    if (icount == 0 && total <= 6 && ret_is_float) {
        double dargs[FFI_MAX_ARGS];
        for (int i = 0; i < total; i++)
            dargs[i] = args[i].value.d;
        result.d = call_pure_double(func, dargs, total);
        return result;
    }

    /* ===== 路径 3: 其余（任何整数+浮点混合，或纯浮点 >6）→ 通用桩 ===== */
    return call_generic(func, sig, args, total);
}

/* 通用调用入口 - Windows 平台转发到 ffi_call_win64 */
FFIValue ffi_call(void* func, const FFISignature* sig, const FFIArg* args) {
    return ffi_call_win64(func, sig, args);
}

#endif /* _WIN32 */
