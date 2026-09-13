/*
 * x86_64.c - x86_64 JIT backend (codegen skeleton)
 *
 * Layout (target-specific, NOT portable):
 *   - x86_64_emit.h     register IDs (JIT_RAX..JIT_R14) + instruction encoders
 *   - x86_inc/ops_*.inc switch(op) cases, grouped by opcode family; these are
 *                       #included INSIDE compile_loop's switch so they can use
 *                       the function-local macros (TOS_*, EMIT_*) and locals
 *   - this file         file header + includes + codegen helpers + the
 *                       compile_loop skeleton (prologue / locals / macros /
 *                       while(1) / switch frame / exit / bailout / patch)
 *
 * What stays portable (shared by all backends, incl. a future arm64.c):
 *   - jit.h       public API
 *   - jit_mem.h   executable memory (has Win/POSIX branches)
 *   - jit_priv.h  shared internals + CodeBuf + off_map/patch helpers
 *   - jit.c       compile driver + public API
 *   - jit_scan.c  hot-loop scanning / inlining analysis
 *   - jit_callout.c runtime C helpers
 *
 * To add arm64: create backend/arm64.c (+ backend/arm64_emit.h, backend/x86_inc/...)
 * with the same public symbol `int compile_loop(CodegenCtx* ctx)` and register
 * it in build.sh / build.bat.
 */

#include "../jit.h"
#include "../jit_mem.h"
#include "../../include/leno_error.h"
#include "../../include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../jit_priv.h"

#include "x86_64_emit.h"

/*
 * Scratch area offset: scratch[i] is at [RBP - 8*(i+1)]
 * Returns the displacement for emit_mov_reg_mem8/32
 * (offmap_add/offmap_lookup/patch_add live in jit_priv.h - platform-neutral)
 */
/* Sentinel marking code positions unreachable via fall-through
 * (after OP_JUMP / OP_RETURN).  The vstack_restore block only
 * restores vstack from patch records when this sentinel is active,
 * preventing false restores at fall-through merge points. */
#define VSTACK_UNREACHABLE  (-99999)

/* Epilogue: mov rsp, rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret
 * (cannot use LEAVE because we pushed R12/R13/R14/RBX after RBP) */
#define EMIT_EPILOGUE() do { \
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP); \
    emit_pop_reg(cb, JIT_R14); \
    emit_pop_reg(cb, JIT_R13); \
    emit_pop_reg(cb, JIT_R12); \
    emit_pop_reg(cb, JIT_RBX); \
    emit_pop_rbp(cb); \
    emit_ret(cb); \
} while(0)

static int scratch_disp(int scratch_idx) {
    return -8 * (scratch_idx + 1);
}

/* ---- ffi 定宽内存读写：可内联方法表（消费方见 x86_inc/ops_return.inc）----
 * 表里每个条目是 ffi 的一个「ptr + offset 定宽访存」方法。内联只做三件与条目
 * 相关的事：CHECK_BOUNDS 的访问宽度、load/store 的宽度与扩展方式、写路径的
 * 截断宽度。其余前置检查（int48 偏移、NaN-boxed 对象、OBJ_FFI_POINTER、
 * !NULL/!freed、owned 边界）对所有条目完全同构；任一检查不过就 bailout 交
 * 解释器，所以报错文本与语义与解释器逐字一致。
 *
 *   size     : 访存字节数（1/2/4），同时是 CHECK_BOUNDS 的 access_size
 *   sign_ext : 读路径是否符号扩展（写路径忽略）；read_byte/read_uint16/
 *              read_uint 为 0（零扩展，对应 memcpy 到无符号窄类型）
 *   is_write : 0 = 读（2 实参，结果 raw int48 压栈）
 *              1 = 写（3 实参，结果 NULL_VAL）
 *
 * 有意不内联（理由见 docs/JIT实现与调试记录.md §2.5）：
 *   read_int64/read_uint64/write_int64/write_uint64 —— 值域越过 Value 的 int48
 *     表示：uint64 超 INT32_MAX 直接返回 bigint 对象，read_int64 走 val_int_safe
 *     超 int48 也要转对象，机器码里无法复刻分配；
 *   read_float/double、write_float/double —— 需要构造/拆 NaN-boxed float Value；
 *   read_ptr/read_at/read_string/offset —— 返回对象（要分配 val_obj）；
 *   read_bool —— 返回 bool Value，非 raw int48；
 *   copy4 —— 双指针，两套对象检查，无逐像素调用点。 */
typedef struct {
    const char* name;
    unsigned char size;
    unsigned char sign_ext;
    unsigned char is_write;
} FfiInlineSpec;

static const FfiInlineSpec ffi_inline_specs[] = {
    { "read_byte",    1, 0, 0 },
    { "read_int8",    1, 1, 0 },
    { "read_int16",   2, 1, 0 },
    { "read_uint16",  2, 0, 0 },
    { "read_int",     4, 1, 0 },
    { "read_uint",    4, 0, 0 },
    { "write_byte",   1, 0, 1 },
    { "write_int8",   1, 0, 1 },
    { "write_int16",  2, 0, 1 },
    { "write_uint16", 2, 0, 1 },
    { "write_int",    4, 0, 1 },
    { "write_uint",   4, 0, 1 },
};

static const FfiInlineSpec* ffi_inline_lookup(const char* method) {
    int i;
    int n = (int)(sizeof(ffi_inline_specs) / sizeof(ffi_inline_specs[0]));
    for (i = 0; i < n; i++) {
        if (strcmp(ffi_inline_specs[i].name, method) == 0) return &ffi_inline_specs[i];
    }
    return NULL;
}

/* ---- Main codegen function ---- */
int compile_loop(CodegenCtx* ctx) {
    const ScanResult* sr = ctx->sr;
    CodeBuf* cb = &ctx->cb;
    int n = sr->num_locals;

    /* ---- Inline context (switchable local_map and chunk) ----
     * cur_local_map starts as the caller's local_map; when we enter
     * an inlined callee, we switch to the callee's local_map.
     * cur_chunk similarly switches for constant access. */
    const int* cur_local_map = sr->local_map;
    Chunk* cur_chunk = ctx->chunk;

    /* Inline frame stack: saved when entering an inlined callee,
     * restored when the callee body ends. */
    typedef struct {
        const uint8_t* ip;
        const uint8_t* end;
        const int* local_map;
        Chunk* chunk;
        int bc_off;
        int vstack;
        int tos_live;
        int callee_ret_count;
        int callee_arg_count;
    } InlineFrame;
    InlineFrame inline_frames[4];
    int inline_depth = 0;

    /* Jump patch list for OP_RETURN inside inlined callees.
     * Each OP_RETURN emits a jmp that needs to be patched to inline_end.
     * 上限按「单个被内联函数体的 return 条数」计（内联退出时清零）；
     * 越界即拒绝编译（见 ops_return.inc 的检查），不允许写越界。 */
    #define JIT_INLINE_RET_MAX 64
    int inline_ret_patches[JIT_INLINE_RET_MAX];
    int inline_ret_patch_cnt = 0;

    /*
     * int48 overflow check: bail out to VM (which handles BigInt promotion)
     * if RAX doesn't fit in signed 48-bit range [-2^47, 2^47-1].
     *
     * sar r8, 47 yields 0 for valid positive, -1 for valid negative.
     * inc r8 maps 0→1, -1→0 (both ≤ 1 unsigned), anything else → > 1.
     * ja bailout catches the "anything else" case.
     *
     * site param: bytecode offset of the check, written to the global
     * jit_bailout_site BEFORE the conditional jump so the bailout debug
     * log can pinpoint which instruction overflowed.
     */
      #define EMIT_BAILOUT_SITE_WRITE(site) do { \
         emit_push_reg(cb, JIT_R9);  /* push r9 — save globals ptr */ \
         emit_mov_reg_imm64(cb, JIT_R9, (uint64_t)(uintptr_t)&jit_bailout_site); \
         emit_byte(cb, 0x41); emit_byte(cb, 0xC7); emit_byte(cb, 0x01); \
         emit_uint32(cb, (uint32_t)(int32_t)(site)); /* mov dword [r9], imm32 */ \
         emit_pop_reg(cb, JIT_R9);   /* pop r9 — restore globals ptr */ \
      } while(0)

     #define EMIT_INT48_CHECK(site) do { \
        emit_mov_rr(cb, JIT_R8, JIT_RAX);  \
        emit_sar_imm(cb, JIT_R8, 47);     \
        emit_inc_reg(cb, JIT_R8);          \
        emit_cmp_reg_imm8(cb, JIT_R8, 1);  \
        EMIT_BAILOUT_SITE_WRITE(site);     \
        int _p = emit_jcc(cb, 0x87);      \
        patch_add(ctx, _p, -1, 0);          \
     } while(0)

     /* 48-bit truncation for SHL: VM 语义为 int64 左移后 val_int() 截断为
      * 48 位有符号（不提升 BigInt）。shl rax,16 把 bit47 移到 bit63，
      * sar rax,16 再算术右移回 → 等价于截断低 48 位并保留符号。 */
     #define EMIT_INT48_TRUNCATE() do { \
        emit_shl_imm(cb, JIT_RAX, 16);   \
        emit_sar_imm(cb, JIT_RAX, 16);   \
     } while(0)

    /* int64 overflow check for MUL: bail out if OF flag set */
    #define EMIT_INT64_OVF_CHECK(site) do { \
        EMIT_BAILOUT_SITE_WRITE(site);     \
        int _p = emit_jcc(cb, 0x80);      \
        patch_add(ctx, _p, -1, 0);        \
    } while(0)

    /* ---- TOS (top-of-stack) register cache ----
     * tos_live=1: RAX holds the TOS value; memory stack has vstack-1 elements.
     * tos_live=0: all vstack elements are on the memory stack.
     * Rule: at all jump targets and loop start, tos_live must be 0. */

    /* Spill live TOS to memory stack (frees RAX for computation) */
    #define TOS_SPILL() do { \
        if (tos_live) { \
            emit_push_reg(cb, JIT_RAX); \
            tos_live = 0; \
        } \
    } while(0)

    /* Mark RAX as holding new TOS (after producing a result in RAX) */
    #define TOS_PRODUCE() do { tos_live = 1; } while(0)

    /* Move result from RDX to RAX, then mark as TOS (for IDIV remainder) */
    #define TOS_PRODUCE_FROM_RDX() do { \
        emit_mov_rr(cb, JIT_RAX, JIT_RDX); \
        tos_live = 1; \
    } while(0)

    /* Consume TOS into RAX (result in RAX, TOS removed) */
    #define TOS_CONSUME_RAX() do { \
        if (!tos_live) { \
            emit_pop_reg(cb, JIT_RAX); \
        } \
        tos_live = 0; \
    } while(0)

    /* Consume TOS into reg (reg gets TOS value, TOS removed) */
    #define TOS_CONSUME_TO(reg) do { \
        if (tos_live) { \
            if ((reg) != JIT_RAX) emit_mov_rr(cb, (reg), JIT_RAX); \
            tos_live = 0; \
        } else { \
            emit_pop_reg(cb, (reg)); \
        } \
    } while(0)

    /* Peek TOS into reg (TOS not consumed) */
    #define TOS_PEEK_TO(reg) do { \
        if (tos_live) { \
            if ((reg) != JIT_RAX) emit_mov_rr(cb, (reg), JIT_RAX); \
        } else { \
            emit_mov_reg_mem8(cb, (reg), JIT_RSP, 0); \
        } \
    } while(0)

    /* Discard TOS (remove without using) */
    #define TOS_DISCARD() do { \
        if (tos_live) { \
            tos_live = 0; \
        } else { \
            emit_byte(cb, 0x48); \
            emit_byte(cb, 0x83); \
            emit_byte(cb, 0xC4); \
            emit_byte(cb, 8); \
        } \
    } while(0)

/* ---- Callout helpers ---- */
    /* Convert RAX from virtual-stack raw to NaN-boxed Value (in-place).
     * Uses R8 as scratch, R10/R11 as constants. */
    #define EMIT_RAW_TO_VALUE() do { \
        emit_mov_rr(cb, JIT_R8, JIT_RAX); \
        emit_sar_imm(cb, JIT_R8, 47); \
        emit_inc_reg(cb, JIT_R8); \
        emit_cmp_reg_imm8(cb, JIT_R8, 1); \
        emit_byte(cb, 0x77); /* ja .boxed */ \
        int _rp = cb->len; \
        emit_byte(cb, 0x00); \
        emit_and_rr(cb, JIT_RAX, JIT_R10); \
        emit_or_rr(cb, JIT_RAX, JIT_R11); \
        cb->buf[_rp] = (uint8_t)(cb->len - (_rp + 1)); \
    } while(0)

    /* Convert RAX from NaN-boxed Value to virtual-stack raw (in-place).
     * Uses R8 as scratch, R10 as PAYLOAD_MASK. */
    #define EMIT_VALUE_TO_RAW() do { \
        emit_mov_rr(cb, JIT_R8, JIT_RAX); \
        { \
            int _b = (JIT_R8 >> 3) & 1; \
            emit_byte(cb, rex(1, 0, 0, _b)); \
            emit_byte(cb, 0xC1); \
            emit_byte(cb, modrm(3, 5, JIT_R8 & 7)); \
            emit_byte(cb, 48); \
        } \
        { \
            int _b = (JIT_R8 >> 3) & 1; \
            emit_byte(cb, rex(1, 0, 0, _b)); \
            emit_byte(cb, 0x81); \
            emit_byte(cb, modrm(3, 7, JIT_R8 & 7)); \
            emit_uint32(cb, 0x0000FFFB); \
        } \
        emit_byte(cb, 0x75); /* jne .not_int */ \
        int _vp = cb->len; \
        emit_byte(cb, 0x00); \
        emit_and_rr(cb, JIT_RAX, JIT_R10); \
        emit_shl_imm(cb, JIT_RAX, 16); \
        emit_sar_imm(cb, JIT_RAX, 16); \
        cb->buf[_vp] = (uint8_t)(cb->len - (_vp + 1)); \
    } while(0)

    /* ---- 裸 0/1（setcc 产物）→ VM 的 bool 表示 TRUE_VAL / FALSE_VAL ----
     * 解释器的每个比较 opcode 压的都是 val_bool(...)（TRUE_VAL/FALSE_VAL），
     * 而 JIT 的比较 opcode 只 setcc 出裸 0/1。这两者的位模式差别很大，且裸 0/1
     * 在 JIT 内部是**歧义**的（0x1 同时是 int48 的 1）：
     *   - 存全局 / 返回 / 传实参时会被「int48 重装箱」路径变成 int 1（bool 变 int）；
     *   - 写回 locals 时若该 slot 的入口值不是 int48（如声明为 bool 的局部量，
     *     入口值 FALSE_VAL），写回走「原样裸存」分支，0x1 直接落进 locals，
     *     解释器把它读成次正规 double 4.9e-324（`is bool`→false、`_int()`→0、
     *     JSON 里写成 4.9e-324）。
     * 所以比较结果必须在**产生处**就做成 VM 的表示，而不是在消费处猜。
     *
     * FALSE_VAL = QNAN|SIGN_BIT|TAG_FALSE = 0xFFF9000000000000，
     * TRUE_VAL  = QNAN|SIGN_BIT|TAG_TRUE  = 0xFFFA000000000000，
     * 两者相差 TAG_TRUE − TAG_FALSE = 1<<48 —— **必须用 ADD，不能用 OR**：
     *   TAG_FALSE = 0x0001_0000_0000_0000 的**第 48 位本身就是 1**，
     *   所以 `FALSE_VAL | (1<<48) == FALSE_VAL`（OR 是空操作），
     *   只有 `FALSE_VAL + (1<<48)` 才进位到第 49 位得到 TRUE_VAL
     *   （0xFFF9 + 0x0001 = 0xFFFA，差的是进位不是某一位）。
     * 实测教训：写成 OR 时比较结果恒为 FALSE_VAL，于是所有 while 循环
     * 第一轮就退出（`for` 循环的条件由 FOR_LOOP 自己比较，不受影响），
     * 而机器码反汇编看起来「完全正确」（`or rax, rdx`），极易被误判成
     * codegen/CPU 问题 —— 见 docs §8.24。
     * 输入 RAX（0/1，已 movzx，高 32 位为 0），用 RDX 当临时
     * —— 各比较 case 的 RDX 在结果产出时都已死亡。
     * 4 条指令、无分支（setcc 之后 EFLAGS 不再需要）。 */
    #define EMIT_RAW01_TO_BOOLVAL() do { \
        emit_mov_rr(cb, JIT_RDX, JIT_RAX); \
        emit_shl_imm(cb, JIT_RDX, 48); \
        emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)FALSE_VAL); \
        emit_add_rr(cb, JIT_RAX, JIT_RDX); \
    } while(0)

    /* ---- 裸数值 → double 规范化（通用算术/比较的 float 慢路径用）----
     * JIT 虚拟栈上的值只有两种数值形态：
     *   - int48：裸 int64 位模式（sar 47 + inc ≤ 1 可判定）
     *   - float：裸 IEEE754 double 位模式
     * 此外还有 NaN-boxed 的 Value（对象/null/bool，高 13 位全 1，即 >= 0xFFF8...）。
     * 注意：负的裸 double（如 0xBFEF...）小于 0xFFF8...，只有 NaN 位模式才会
     * 与 NaN-boxing 撞车，而 val_float() 已把这种 double 归一化为 QNAN。
     *
     *   dst_xmm   : 目标 XMM 号
     *   src_reg   : 源 GPR
     *   scratch   : 临时 GPR
     *   tagged_var: 整型左值；写入「该操作数是 NaN-boxed」的 rel32 补丁偏移，
     *               调用方随后用 patch_rel32(cb, tagged_var, cb->len) 落到
     *               concat / bailout 标签。
     *
     * int48 走 CVTSI2SD 提升，裸 double 直接 MOVQ 搬位，NaN-boxed 跳走。 */
    #define EMIT_NUM_TO_XMM(dst_xmm, src_reg, scratch_reg, tagged_var) do {      \
        emit_mov_rr(cb, (scratch_reg), (src_reg));                               \
        emit_sar_imm(cb, (scratch_reg), 47);                                     \
        emit_inc_reg(cb, (scratch_reg));                                         \
        emit_cmp_reg_imm8(cb, (scratch_reg), 1);                                 \
        int _int_p = emit_jcc(cb, 0x87);          /* ja .not_int48 */            \
        emit_cvtsi2sd(cb, (dst_xmm), (src_reg));                                 \
        int _done_p = emit_jmp(cb);                                              \
        patch_rel32(cb, _int_p, cb->len);         /* .not_int48: */              \
        emit_mov_reg_imm64(cb, (scratch_reg), 0xFFF8000000000000ULL);            \
        emit_cmp_rr(cb, (src_reg), (scratch_reg));                               \
        (tagged_var) = emit_jcc(cb, 0x83);        /* jae (unsigned) → tagged */  \
        emit_byte(cb, 0x66);                                                     \
        emit_byte(cb, rex(1, 0, 0, ((src_reg) >> 3) & 1));                       \
        emit_byte(cb, 0x0F);                                                     \
        emit_byte(cb, 0x6E);                      /* MOVQ xmm, r64 */            \
        emit_byte(cb, modrm(3, (dst_xmm) & 7, (src_reg) & 7));                   \
        patch_rel32(cb, _done_p, cb->len);                                       \
    } while(0)

    /* 非溢出类 bailout 的 site 汇报：与 int48 溢出检查共用 jit_bailout_site
     * 全局量，但用负值区间区分原因，便于 [JIT-DEBUG] 日志定位：
     *   site >= 0          : 溢出/截断类检查，值 = 触发指令的 bc_off
     *                        （内联帧含 0x10000 * depth 基址）
     *   site ∈ [-999, -1]  : JIT 序言（-1 = 进入自增溢出、-2 = step == 0、-3 = step 为 float）
     *   site <= -1000      : 其它原因（NaN 比较、callout 失败、类型不支持等），
     *                        对应 bc_off = -1000 - site
     * 每个 bailout 守卫都在跳转前写一次 site，因此 site 永远反映真正失败的那条指令。 */
    /* 序言 bailout 的 site 取值（jit_print_stats 会翻译成可读原因）：
     *   -1 = 进入自增 int48 溢出，-2 = step == 0，-3 = step 是 float */
    #define EMIT_BAILOUT_SITE_NONOVF(off) EMIT_BAILOUT_SITE_WRITE(-1000 - (int)(off))

    /* ---- 类型化浮点运算（OP_*_FLOAT / OP_*_FLOAT 比较）的操作数取值 ----
     * 这些 opcode 名字里带 FLOAT，但**操作数不保证已经是 float**：编译器只在
     * 静态类型确定时才补 OP_CAST_FLOAT，像 `acc + o.get("k", 0.0)`（float 局部量
     * 加动态类型调用结果）会把 int48 直接喂进来。解释器一律经 val_as_num_ex /
     * val_as_num 提升，所以 JIT 也必须按三态取操作数，否则 1 的 int48 位模式
     * 会当成 1e-323 的次正规 double 参与 SSE 运算（加了个「0」）。
     *   1) int48            → CVTSI2SD 提升
     *   2) 裸 double        → MOVQ 直接搬位
     *   3) NaN-boxed        → bailout 交解释器（解释器对 BigInt 转 double、
     *                         对其余非数值按 0.0；对象种类 JIT 无从区分，
     *                         一律回退，语义与性能都最稳）
     * 展开后 xmm0 = a（左操作数）、xmm1 = b（右操作数）；tagged_a/tagged_b
     * 是两条「操作数是 NaN-boxed」的 rel32 跳转，必须紧接着调用
     * EMIT_FLOAT_TAGGED_BAILOUT() 让它们落到 bailout 桩上。 */
    #define EMIT_FLOAT_ARGS2(tagged_a, tagged_b) do { \
        TOS_CONSUME_TO(JIT_RAX);                          /* b（右） */ \
        TOS_CONSUME_TO(JIT_RDX);                          /* a（左） */ \
        EMIT_NUM_TO_XMM(0, JIT_RDX, JIT_R8, tagged_a);    /* xmm0 = a */ \
        EMIT_NUM_TO_XMM(1, JIT_RAX, JIT_R8, tagged_b);    /* xmm1 = b */ \
    } while(0)

    /* 浮点结果（xmm0）写回 RAX，作为新的 TOS */
    #define EMIT_MOVQ_RAX_XMM0() do { \
        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); \
        emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0)); \
    } while(0)

    /* 把 EMIT_FLOAT_ARGS2 挂起的两条 tagged 跳转落到 bailout 桩：
     * 非溢出类 bailout，site 用负值编码，日志可定位到具体 bc_off。 */
    #define EMIT_FLOAT_TAGGED_BAILOUT(tagged_a, tagged_b) do { \
        patch_rel32(cb, (tagged_a), cb->len); \
        patch_rel32(cb, (tagged_b), cb->len); \
        EMIT_BAILOUT_SITE_NONOVF(bc_off); \
        { int _p = emit_jmp(cb); patch_add(ctx, _p, -1, 0); } \
    } while(0)

    /* Callout argument registers per target ABI:
     *   Windows x64:    arg1..arg4 = RCX/RDX/R8/R9, 5th+ go on the stack
     *                   (first stack arg at [RSP+32] after 32B shadow)
     *   System V AMD64: arg1..arg6 = RDI/RSI/RDX/RCX/R8/R9, no shadow space
     * JIT_ARGn maps an argument position to the platform's integer arg
     * register, so callout argument loading sequences are written once and
     * work on both ABIs. */
    #ifdef _WIN32
    #define JIT_ARG1 JIT_RCX
    #define JIT_ARG2 JIT_RDX
    #define JIT_ARG3 JIT_R8
    #define JIT_ARG4 JIT_R9
    #else
    #define JIT_ARG1 JIT_RDI
    #define JIT_ARG2 JIT_RSI
    #define JIT_ARG3 JIT_RDX
    #define JIT_ARG4 JIT_RCX
    #define JIT_ARG5 JIT_R8
    #endif

    /* Save volatile state, align RSP to 16 bytes (required by both ABIs),
     * then allocate the per-ABI call frame. Windows x64 needs 32-byte shadow
     * space + 16 bytes for stack arguments (e.g. MODULE_CALL's 5th arg at
     * [RSP+32]); without the extra 16, [RSP+32] would overlap vstack_top[0].
     * System V has no shadow space and no stack argument area, so only the
     * 16-byte alignment is needed. */
    #ifdef _WIN32
    #define EMIT_CALLOUT_ALLOC() do { \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xEC); emit_byte(cb, 0x30); \
    } while(0)
    #else
    #define EMIT_CALLOUT_ALLOC() do { } while(0)
    #endif

    #define EMIT_CALLOUT_BEGIN() do { \
        emit_mov_rr(cb, JIT_R12, JIT_RSP); \
        emit_mov_rr(cb, JIT_R13, JIT_RCX); \
        emit_mov_rr(cb, JIT_R14, JIT_R9); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xE4); emit_byte(cb, 0xF0); \
        EMIT_CALLOUT_ALLOC(); \
    } while(0)

    /* Restore state, reload R10/R11 */
    #define EMIT_CALLOUT_END() do { \
        emit_mov_rr(cb, JIT_RSP, JIT_R12); \
        emit_mov_rr(cb, JIT_RCX, JIT_R13); \
        emit_mov_rr(cb, JIT_R9, JIT_R14); \
        emit_mov_reg_imm64(cb, JIT_R10, JIT_PAYLOAD_MSK); \
        emit_mov_reg_imm64(cb, JIT_R11, JIT_INT_TAG); \
    } while(0)

    /* Emit: mov rax, imm64; call rax */
    #define EMIT_CALL(fn) do { \
        emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)(uintptr_t)(fn)); \
        emit_byte(cb, 0xFF); emit_byte(cb, modrm(3, 2, JIT_RAX & 7)); \
    } while(0)

    /* Store/load temp slots via RBP */
    #define EMIT_STORE_TMP(disp, reg) do { \
        if ((disp) >= -128 && (disp) <= 127) \
            emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)(disp), (reg)); \
        else \
            emit_mov_mem32_reg(cb, JIT_RBP, (disp), (reg)); \
    } while(0)

    #define EMIT_LOAD_TMP(reg, disp) do { \
        if ((disp) >= -128 && (disp) <= 127) \
            emit_mov_reg_mem8(cb, (reg), JIT_RBP, (int8_t)(disp)); \
        else \
            emit_mov_reg_mem32(cb, (reg), JIT_RBP, (disp)); \
    } while(0)

    /* Callout temp slot offsets (3 slots below virtual stack area).
     * n includes only caller locals; inline callee locals use slots
     * n..n+inline_extra_locals-1 which are below the vstack area. */
    int total_locals = n + sr->inline_extra_locals;
    int tmp1_disp = -8 * (total_locals + sr->max_vstack + 1);
    int tmp2_disp = -8 * (total_locals + sr->max_vstack + 2);
    int tmp3_disp = -8 * (total_locals + sr->max_vstack + 3);

    /* ---- Function prologue ---- */
    emit_push_rbp(cb);                        /* push rbp          */
    emit_push_reg(cb, JIT_RBX);              /* push rbx (type bitmap, callee-saved) */
    emit_push_reg(cb, JIT_R12);              /* push r12 (callout: saved RSP) */
    emit_push_reg(cb, JIT_R13);              /* push r13 (callout: saved RCX=locals) */
    emit_push_reg(cb, JIT_R14);              /* push r14 (callout: saved R9=globals) */
    emit_mov_rbp_rsp(cb);                     /* mov rbp, rsp      */

    /* ---- Entry ABI shim (System V AMD64) -----------------------------
     * The JIT function is entered through a plain C call:
     *     fn(locals, globals)          (jit.c / jit_callout.c thin bridge)
     * Everything below this point assumes the parameters sit in the Windows
     * x64 registers: RCX = locals (read as [rcx + slot*8]), RDX = globals
     * (copied into R9).  On System V (Linux/macOS x86-64) a C call delivers
     * arg1/arg2 in RDI/RSI instead, so alias them into the Win64 slots
     * first; otherwise the prologue would dereference garbage.
     * Safe here: nothing has been emitted before the pushes above (only
     * RBP/RBX/R12-R14 are touched), so RDI/RSI still hold the arguments.
     * JIT_ARG1/JIT_ARG2 (defined below) already handle callout arguments;
     * this shim covers the *entry* convention, which is the only place that
     * is not reached through those macros. */
#ifndef _WIN32
    emit_mov_rr(cb, JIT_RCX, JIT_RDI);        /* RCX = locals  (arg1) */
    emit_mov_rr(cb, JIT_RDX, JIT_RSI);        /* RDX = globals (arg2) */
#endif

    /* Allocate: scratch area (total_locals*8) + max_vstack*8 + callout temps (3*8), rounded to 16 */
    int frame_sz = total_locals * 8 + sr->max_vstack * 8 + 16 + 24;
    frame_sz = (frame_sz + 15) & ~15;        /* align to 16 */
    if (frame_sz <= 127) {
        emit_sub_rsp_imm8(cb, (uint8_t)frame_sz);
    } else {
        emit_sub_rsp_imm32(cb, frame_sz);
    }

    /* ---- Load constants into R10/R11 ---- */
    /* R10 = JIT_PAYLOAD_MASK */
    emit_mov_reg_imm64(cb, JIT_R10, JIT_PAYLOAD_MSK);
    /* R11 = JIT_INT_TAG */
    emit_mov_reg_imm64(cb, JIT_R11, JIT_INT_TAG);
    /* R9 = globals pointer (from RDX = second arg) */
    emit_mov_rr(cb, JIT_R9, JIT_RDX);
    /* RBX = type bitmap: bit i = 1 if local i is float, 0 if int.
     * Cleared to 0 (all-int) by default; float path sets bits via BTS. */
    emit_xor_rr(cb, JIT_RBX, JIT_RBX);

    /* ---- Type guards + extraction (int OR float) ---- */
    /* RCX = locals pointer (first arg, preserved) */
    /* RBX = type bitmap: bit i = 1 if local i is float, 0 if int */
    for (int i = 0; i < n; i++) {
        int slot = sr->local_slots[i];
        int disp = scratch_disp(i);
        /* Load value: mov rax, [rcx + slot*8] */
        {
            int sd = slot * 8;
            if (sd >= -128 && sd <= 127)
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RCX, (int8_t)sd);
            else
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RCX, sd);
        }
        /* Get top 16 bits: mov r8, rax; shr r8, 48 */
        emit_mov_rr(cb, JIT_R8, JIT_RAX);
        {
            int b = (JIT_R8 >> 3) & 1;
            emit_byte(cb, rex(1, 0, 0, b));
            emit_byte(cb, 0xC1);
            emit_byte(cb, modrm(3, 5, JIT_R8 & 7));  /* /5 = SHR */
            emit_byte(cb, 48);
        }
        /* Check int: cmp r8, 0xFFFB */
        {
            int b = (JIT_R8 >> 3) & 1;
            emit_byte(cb, rex(1, 0, 0, b));
            emit_byte(cb, 0x81);              /* CMP r/m64, imm32 */
            emit_byte(cb, modrm(3, 7, JIT_R8 & 7));  /* /7 = CMP */
            emit_uint32(cb, 0x0000FFFB);      /* INT_TAG >> 48 */
        }
        /* je .is_int (rel8 placeholder) */
        emit_byte(cb, 0x74);
        int isint_patch = cb->len;
        emit_byte(cb, 0x00);

        /* Non-int path: float OR object/null/bool.
         * All non-int values are stored as raw NaN-boxed bits and
         * marked via RBX bitmap so the write-back stores them as-is.
         * No bailout for non-numeric locals (needed for callout support). */

        /* Float path: RAX still has original raw double bits */
        /* Set type bit: BTS RBX, i  (48 0F BA EB imm8) */
        emit_byte(cb, 0x48);
        emit_byte(cb, 0x0F);
        emit_byte(cb, 0xBA);
        emit_byte(cb, 0xEB);  /* ModRM(11, 5, 3) = BTS RBX, imm8 */
        emit_byte(cb, (uint8_t)i);
        /* Store raw double bits: mov [rbp+disp], rax */
        if (disp >= -128 && disp <= 127) {
            emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
        } else {
            emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
        }
        /* jmp .next (rel8 placeholder) */
        emit_byte(cb, 0xEB);
        int next_patch = cb->len;
        emit_byte(cb, 0x00);

        /* .is_int: patch je to here */
        cb->buf[isint_patch] = (uint8_t)(cb->len - (isint_patch + 1));

        /* Int path: extract int48 (RAX still holds original value) */
        emit_and_rr(cb, JIT_RAX, JIT_R10);     /* AND PAYLOAD_MASK */
        emit_shl_imm(cb, JIT_RAX, 16);         /* sign-extend */
        emit_sar_imm(cb, JIT_RAX, 16);
        /* Store to scratch: mov [rbp + disp], rax */
        if (disp >= -128 && disp <= 127) {
            emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
        } else {
            emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
        }

        /* .next: patch jmp to here */
        cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
    }

    /* ---- Step check (for OP_FOR_LOOP) ----
     * 支持正/负步长（倒序循环）：方向在入口处按 step 符号分流，不在此 bailout。
     * 仅两种情况仍回退解释器：
     *   site -3: step 是 float（类型位图为 1）——JIT 的 FOR_LOOP 走 int48 快路径，
     *            无法对 double 位模式做方向/比较判断；
     *   site -2: step == 0 —— VM 语义为“不进循环”，交给解释器处理。 */
    if (sr->back_edge_type == 2) {
        int step_scratch = cur_local_map[sr->for_step_slot];
        if (step_scratch < 0) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] codegen FAIL: FOR_LOOP step_scratch<0\n");
            return 0;  /* shouldn't happen */
        }
        int disp = scratch_disp(step_scratch);

        /* 类型位图：bit(step_scratch) == 1 表示该 local 是 float → bailout */
        emit_byte(cb, 0x48);
        emit_byte(cb, 0x0F);
        emit_byte(cb, 0xBA);
        emit_byte(cb, 0xE3);  /* ModRM(11, 4, 3) = BT RBX, imm8 */
        emit_byte(cb, (uint8_t)(step_scratch & 0xFF));
        EMIT_BAILOUT_SITE_WRITE(-3);
        int float_patch = emit_jcc(cb, 0x82);  /* JC → bailout */
        patch_add(ctx, float_patch, -1, 0);

        /* Load step: mov rax, [rbp + disp] */
        if (disp >= -128 && disp <= 127) {
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
        } else {
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
        }
        /* step == 0 → bailout (site -2) */
        emit_test_rr(cb, JIT_RAX, JIT_RAX);
        EMIT_BAILOUT_SITE_WRITE(-2);
        int zero_patch = emit_jcc(cb, 0x84);  /* JE → bailout */
        patch_add(ctx, zero_patch, -1, 0);
    }

    /* ---- FOR_LOOP first-iteration increment + check ---- */
    /* The JIT is triggered at OP_FOR_LOOP BEFORE the interpreter does the increment.
     * So the JIT must do the increment itself on the first iteration to avoid
     * re-running the body with the same loop_var value the interpreter already used.
     *
     * Nesting support: when an inner for-loop (OP_FOR_PREP) is nested inside
     * the JIT body, its initial-condition check also emits a Jcc that must be
     * patched to the exit point.  We keep a small stack of these patches.
     */
    int for_loop_entry_patches[8];
    int for_loop_patch_cnt = 0;
    if (sr->back_edge_type == 2) {
        int si_lv = cur_local_map[sr->for_loop_var_slot];
        int si_st = cur_local_map[sr->for_step_slot];
        int si_en = cur_local_map[sr->for_end_slot];
        int d_lv = scratch_disp(si_lv);
        int d_st = scratch_disp(si_st);
        int d_en = scratch_disp(si_en);

        /* loop_var += step，再与 end 比较；退出跳转由调用处按方向选择
         * （正向用 JG/JGE，反向用 JL/JLE），同一段比较代码两种方向复用。 */
        #define EMIT_FOR_ENTRY_STEP() do { \
            /* mov rax, [rbp+d_lv] (loop_var) */ \
            if (d_lv >= -128 && d_lv <= 127) \
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv); \
            else \
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv); \
            /* mov rdx, [rbp+d_st] (step) */ \
            if (d_st >= -128 && d_st <= 127) \
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st); \
            else \
                emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st); \
            /* add rax, rdx (loop_var += step) */ \
            emit_add_rr(cb, JIT_RAX, JIT_RDX); \
            /* -1: this check is the loop-entry increment, bc_off not yet in scope */ \
            EMIT_INT48_CHECK(-1); \
            /* mov [rbp+d_lv], rax (store back) */ \
            if (d_lv >= -128 && d_lv <= 127) \
                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX); \
            else \
                emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX); \
            /* mov rdx, [rbp+d_en] (end) */ \
            if (d_en >= -128 && d_en <= 127) \
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en); \
            else \
                emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en); \
            /* cmp rax, rdx */ \
            emit_cmp_rr(cb, JIT_RAX, JIT_RDX); \
        } while(0)

        /* 读取 step 符号：step < 0 走反向入口（支持倒序 for 循环） */
        if (d_st >= -128 && d_st <= 127)
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_st);
        else
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_st);
        emit_test_rr(cb, JIT_RAX, JIT_RAX);
        int neg_step_patch = emit_jcc(cb, 0x88);  /* JS -> negative entry */

        /* ---- 正向入口 (step > 0) ---- */
        EMIT_FOR_ENTRY_STEP();
        /* If condition NOT met, jump to exit (loop is done) */
        /* step > 0, exclusive: JGE (loop_var >= end) → exit */
        /* step > 0, inclusive: JG  (loop_var > end) → exit  */
        uint8_t cc_exit = sr->for_inclusive ? 0x8F /*JG*/ : 0x8D /*JGE*/;
        for_loop_entry_patches[for_loop_patch_cnt++] = emit_jcc(cb, cc_exit);
        /* Will be patched to exit_mc later */

        int entry_done_patch = emit_jmp(cb);      /* 正向入口结束，跳过反向块 */

        /* ---- 反向入口 (step < 0)：退出条件与正向相反 ---- */
        patch_rel32(cb, neg_step_patch, cb->len);
        EMIT_FOR_ENTRY_STEP();
        uint8_t cc_exit_neg = sr->for_inclusive ? 0x8C /*JL*/ : 0x8E /*JLE*/;
        if (for_loop_patch_cnt < 8)
            for_loop_entry_patches[for_loop_patch_cnt++] = emit_jcc(cb, cc_exit_neg);
        else
            emit_jcc(cb, cc_exit_neg);
        patch_rel32(cb, entry_done_patch, cb->len);

        #undef EMIT_FOR_ENTRY_STEP
    }

    /* ---- Loop body start ---- */
    ctx->loop_start_mc = cb->len;

    /* ---- Generate loop body code ---- */
    const uint8_t* ip = ctx->body_start;
    const uint8_t* end = ctx->body_start + sr->body_size;
    int bc_off = 0;
    int vstack = 0;
    int tos_live = 0;  /* TOS register cache: 1=RAX holds TOS, 0=all on memory stack */

    /* Helper: check if current bc_off matches an inline site */
    #define FIND_INLINE_SITE(off) \
        ({ int _idx = -1; \
           for (int _i = 0; _i < sr->inline_count; _i++) { \
               if (sr->inline_sites[_i].bc_off == (off)) { _idx = _i; break; } \
           } _idx; })

    while (1) {
        /* Check if we've reached the end of an inlined callee body */
        if (ip >= end) {
            if (inline_depth > 0) {
                /* Reached end of inlined callee bytecode.
                 * This is the inline_end point. Patch all OP_RETURN
                 * jumps to here. */
                int end_mc = cb->len;
                for (int _i = 0; _i < inline_ret_patch_cnt; _i++) {
                    patch_rel32(cb, inline_ret_patches[_i], end_mc);
                }
                inline_ret_patch_cnt = 0;  /* reset for nested inline */

                /* Restore caller context */
                inline_depth--;
                InlineFrame* f = &inline_frames[inline_depth];
                /* The return values are on the vstack.
                 * Callee's vstack had ret_count items.
                 * Caller's vstack should be: saved_vstack - arg_count + ret_count */
                vstack = f->vstack - f->callee_arg_count + f->callee_ret_count;
                tos_live = 0;  /* TOS not cached after inline */
                bc_off = f->bc_off;
                ip = f->ip;
                end = f->end;
                cur_local_map = f->local_map;
                cur_chunk = f->chunk;
                continue;
            }
            break;  /* normal end of loop body */
        }

        uint8_t op = *ip;
        int size = opcode_size(ip);
        offmap_add(ctx, bc_off, cb->len);

        /* Restore vstack at forward jump targets — but ONLY when the
         * current code position is truly unreachable via fall-through.
         *
         * After an unconditional OP_JUMP or OP_RETURN, the linear scan
         * marks vstack as VSTACK_UNREACHABLE.  When the scan later reaches
         * a bc_off that is a forward-jump target, it restores vstack from
         * the patch record (which captured the correct vstack at the jump
         * source).  This fixes the double-counting of OP_POP in mutually
         * exclusive truthy/falsey paths (e.g. if/else, if/continue) without
         * corrupting fall-through merge points (e.g. if-without-else).
         *
         * tos_live is also reset to 0: all forward jumps TOS_SPILL
         * before emitting the jump, so jump targets always arrive
         * with tos_live=0. */
        if (vstack == VSTACK_UNREACHABLE) {
            int found = 0;
            for (int _pi = 0; _pi < ctx->patch_count; _pi++) {
                if (ctx->patches[_pi].target_bc == bc_off) {
                    vstack = ctx->patches[_pi].vstack;
                    tos_live = 0;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                /* Dead code with no jump target — use safe default */
                vstack = 0;
                tos_live = 0;
            }
        }

        switch (op) {
            #include "x86_inc/ops_stack.inc"
            #include "x86_inc/ops_globals.inc"
            #include "x86_inc/ops_arith.inc"
            #include "x86_inc/ops_float.inc"
            #include "x86_inc/ops_incdec.inc"
            #include "x86_inc/ops_index.inc"
            #include "x86_inc/ops_fcmp.inc"
            #include "x86_inc/ops_imm.inc"
            #include "x86_inc/ops_local.inc"
            #include "x86_inc/ops_icmp.inc"
            #include "x86_inc/ops_jump.inc"
            #include "x86_inc/ops_loop.inc"
            #include "x86_inc/ops_callout.inc"
            #include "x86_inc/ops_return.inc"
            #include "x86_inc/ops_misc.inc"
        }

        ip += size;
        bc_off += size;
    }

    /* ---- Exit code ---- */
    ctx->exit_mc = cb->len;

    /* Patch FOR_LOOP entry "exit if done" jumps to here */
    for (int _i = 0; _i < for_loop_patch_cnt; _i++) {
        patch_rel32(cb, for_loop_entry_patches[_i], ctx->exit_mc);
    }

    /* Clean up any remaining virtual stack */
    TOS_SPILL();  /* flush TOS to memory stack before cleanup */
    if (vstack > 0) {
        /* add rsp, vstack*8 */
        if (vstack * 8 <= 127) {
            emit_byte(cb, 0x48);
            emit_byte(cb, 0x83);
            emit_byte(cb, 0xC4);
            emit_byte(cb, (uint8_t)(vstack * 8));
        } else {
            emit_byte(cb, 0x48);
            emit_byte(cb, 0x81);
            emit_byte(cb, 0xC4);
            emit_uint32(cb, (uint32_t)(vstack * 8));
        }
    }

    /* Write back all locals (type-aware via RBX bitmap) — 提取为宏，
     * exit 块与 framedead 块（异常定向到宿主帧 catch 时）共用 */
    #define EMIT_WRITEBACK_LOCALS() do { \
        for (int _i = 0; _i < n; _i++) { \
            int slot = sr->local_slots[_i]; \
            int disp = scratch_disp(_i); \
            int sd = slot * 8; \
            /* Skip duplicate slot mappings — only write back the first mapping. \
             * When the same slot is mapped to multiple scratch slots (e.g. a   \
             * value is loaded multiple times into different scratch locations)  \
             * only the first scratch was initialised by the prologue; the rest \
             * may hold garbage.  Writing them back would corrupt the local. */ \
            int _dup = 0; \
            for (int _j = 0; _j < _i; _j++) { \
                if (sr->local_slots[_j] == slot) { _dup = 1; break; } \
            } \
            if (_dup) continue; \
            /* BT RBX, i → CF = bit i (48 0F BA E3 imm8) */ \
            emit_byte(cb, 0x48); \
            emit_byte(cb, 0x0F); \
            emit_byte(cb, 0xBA); \
            emit_byte(cb, 0xE3);  /* ModRM(11, 4, 3) = BT RBX, imm8 */ \
            emit_byte(cb, (uint8_t)_i); \
            /* jc .float_wb (rel8 placeholder) */ \
            emit_byte(cb, 0x72); \
            int flt_patch = cb->len; \
            emit_byte(cb, 0x00); \
            /* Int path: load scratch；只有确实是 int48 才重新装箱为 NaN-boxed int。 \
             * 位图 bit=0 只说明「进循环时该槽位是 int」，循环里仍可能被写入     \
             * NaN-boxed 值（bool / null / 对象）—— 按位图盲装箱会把 TRUE_VAL 的  \
             * 位模式搅成垃圾 int。判据与 OP_SET_GLOBAL 的重装箱条件完全一致。 */ \
            if (disp >= -128 && disp <= 127) \
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp); \
            else \
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp); \
            emit_mov_rr(cb, JIT_RDX, JIT_RAX); \
            emit_sar_imm(cb, JIT_RDX, 47); \
            emit_inc_reg(cb, JIT_RDX); \
            emit_cmp_reg_imm8(cb, JIT_RDX, 1); \
            emit_byte(cb, 0x77); /* ja .raw_wb（非 int48 → 原样存回）*/ \
            int raw_wb_patch = cb->len; \
            emit_byte(cb, 0x00); \
            emit_and_rr(cb, JIT_RAX, JIT_R10); \
            emit_or_rr(cb, JIT_RAX, JIT_R11); \
            if (sd >= -128 && sd <= 127) \
                emit_mov_mem8_reg(cb, JIT_RCX, (int8_t)sd, JIT_RAX); \
            else \
                emit_mov_mem32_reg(cb, JIT_RCX, sd, JIT_RAX); \
            /* jmp .next (rel8 placeholder) */ \
            emit_byte(cb, 0xEB); \
            int next_patch = cb->len; \
            emit_byte(cb, 0x00); \
            /* .raw_wb: 原样存回（RAX 未被破坏；RDX 只是判据用的临时值）*/ \
            cb->buf[raw_wb_patch] = (uint8_t)(cb->len - (raw_wb_patch + 1)); \
            if (sd >= -128 && sd <= 127) \
                emit_mov_mem8_reg(cb, JIT_RCX, (int8_t)sd, JIT_RAX); \
            else \
                emit_mov_mem32_reg(cb, JIT_RCX, sd, JIT_RAX); \
            /* jmp .next (rel8 placeholder) —— 同样跳过下面的 float 路径 */ \
            emit_byte(cb, 0xEB); \
            int raw_done_patch = cb->len; \
            emit_byte(cb, 0x00); \
            /* .float_wb: patch jc to here */ \
            cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1)); \
            /* Float path: load raw double bits, store directly (no re-encode) */ \
            if (disp >= -128 && disp <= 127) \
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp); \
            else \
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp); \
            if (sd >= -128 && sd <= 127) \
                emit_mov_mem8_reg(cb, JIT_RCX, (int8_t)sd, JIT_RAX); \
            else \
                emit_mov_mem32_reg(cb, JIT_RCX, sd, JIT_RAX); \
            /* .next: patch jmp to here（装箱分支与裸存回分支都跳到这里）*/ \
            cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1)); \
            cb->buf[raw_done_patch] = (uint8_t)(cb->len - (raw_done_patch + 1)); \
        } \
    } while(0)

/* Reload RCX from jit_reloaded_locals before writeback, in case
     * vm_grow_frames reallocated vm.frames during a callout (e.g. deep
     * recursion via OP_CALL_GLOBAL_FUNC_TYPED or OP_INVOKE_METHOD_TYPED).
     * Without this, RCX still points to the pre-callout (now freed)
     * frame->locals, and the writeback below writes to freed memory. */
    #define EMIT_RELOAD_RCX() do { \
        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_reloaded_locals); \
        emit_mov_reg_mem8(cb, JIT_RCX, JIT_R8, 0); \
    } while(0)

    /* ---- 函数级 JIT（func_mode）：不写回 locals ----
     * 调用方（jit_callout_invoke_method / jit_callout_global_func）为函数
     * JIT 分配的是临时 locals 数组，执行后整体 free；写回无意义且可能把
     * NULL_VAL 初值覆盖到错误地址。正常流程必经 OP_RETURN 返回，此 exit
     * 仅为防御性不可达代码（ret 0；jit_fn_result 由 OP_RETURN 设置）。 */
    if (!ctx->func_mode) {
        EMIT_RELOAD_RCX();
        EMIT_WRITEBACK_LOCALS();
    }

    /* Return 0 (success) */
    emit_xor_eax_eax(cb);
    EMIT_EPILOGUE();

    /* ---- Bailout code ---- */
    ctx->bailout_mc = cb->len;
    /* DEBUG: save RAX to global, then call debug function */
    if (jit_debug_on()) {
        /* Store RAX to global jit_bailout_rax */
        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_bailout_rax);
        /* mov [r8], rax */
        emit_byte(cb, 0x49); emit_byte(cb, 0x89); emit_byte(cb, 0x00);  /* mov [r8], rax */
        /* Save RSP and RAX before callout */
        EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
        EMIT_CALLOUT_BEGIN();
        EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
        EMIT_CALL(jit_bailout_debug);
        EMIT_CALLOUT_END();
    }
    /* Return 1 (bailout) — locals not written back (VM re-executes from back-edge) */
    emit_mov_eax_imm32(cb, 1);
    EMIT_EPILOGUE();

    /* ---- Frame-dead exit: callout 异常且宿主帧存活（catch_ip 已定向）----
     * 写回 locals 后返回 2；调用方（OP_LOOP/OP_FOR_LOOP handler）
     * 重载 frame 后 DISPATCH，从宿主帧 catch_ip 继续执行 */
    ctx->framedead_mc = cb->len;
    EMIT_RELOAD_RCX();
    EMIT_WRITEBACK_LOCALS();
    /* Return 2 (frame-dead, host frame alive, locals written back) */
    emit_mov_eax_imm32(cb, 2);
    EMIT_EPILOGUE();

    /* ---- Frame-dead exit: callout 异常且宿主帧已被展开 ----
     * 宿主帧 locals 已被异常展开释放，禁止写回；返回 3；
     * 调用方重载 frame（外层 handler 帧）后 DISPATCH */
    ctx->framedead_nowb_mc = cb->len;
    /* Return 3 (frame-dead, host frame destroyed, no write back) */
    emit_mov_eax_imm32(cb, 3);
    EMIT_EPILOGUE();

    #undef EMIT_WRITEBACK_LOCALS

    /* ---- 溢出检查：off_map / patch 丢过条目就拒绝编译 ----
     * 残缺的 off_map 会让回边/前向跳转解析失败，残缺的 patch 会留下 rel32=0
     * 的跳转（跳到代码段开头）。两者都是"静默发出错误机器码"，按项目原则
     * （宁可不编、不要猜）一律拒绝，循环退回解释器。 */
    if (ctx->off_overflow || ctx->patch_overflow) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] COMPILE-FAIL: off_map/patch 溢出 "
                            "(off=%d/%d patch=%d/%d) → 拒绝编译\n",
                    ctx->off_count, JIT_MAX_LOOP_OPS * 10,
                    ctx->patch_count, JIT_MAX_PATCHES);
        return 0;
    }

    /* ---- Patch all jumps ---- */
    for (int i = 0; i < ctx->patch_count; i++) {
        Patch* p = &ctx->patches[i];
        if (p->target_bc == -1) {
            /* Bailout target */
            patch_rel32(cb, p->patch_mc, ctx->bailout_mc);
        } else if (p->target_bc == -2) {
            /* Frame-dead (write back) target */
            patch_rel32(cb, p->patch_mc, ctx->framedead_mc);
        } else if (p->target_bc == -3) {
            /* Frame-dead (no write back) target */
            patch_rel32(cb, p->patch_mc, ctx->framedead_nowb_mc);
        } else if (p->target_bc >= 0) {
            /* Intra-body jump (caller or inlined callee).
             * Inlined callee offsets use bc_off = 0x10000 * (inline_idx + 1)
             * —— 按**内联实例**编号，同一深度下多个内联点各有独立命名空间
             * （只按 depth 编号会让第 2..N 个实例的跳转解析到第 1 个实例，
             * 见 ops_callout.inc 的说明与 §8.26）。
             * 因此 target_bc 可能远大于 sr->body_size，靠 offmap_lookup 定位。 */
            int target_mc = offmap_lookup(ctx, p->target_bc);
            if (target_mc >= 0) {
                patch_rel32(cb, p->patch_mc, target_mc);
            } else if (p->target_bc >= 0x10000) {
                /* 内联命名空间的目标**必须**能解析：被内联函数体的条件跳转/前向
                 * 跳转目标都在它自己的 ≤256 字节内。找不到 = off_map 条目被丢或
                 * 扫描失配 → 绝不静默改道（§8.26 就是这么崩的），拒绝编译。 */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] PATCH-FAIL: 内联目标 target_bc=%d 未解析 "
                                    "(mc=%d) → 拒绝编译\n", p->target_bc, p->patch_mc);
                return 0;
            } else if (p->target_bc < sr->body_size) {
                /* 调用方命名空间、目标落在循环体**之内**却找不到 → 真的丢了 */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] PATCH-FAIL: 体内目标 target_bc=%d 未解析 "
                                    "(mc=%d, body_size=%d) → 拒绝编译\n",
                            p->target_bc, p->patch_mc, sr->body_size);
                return 0;
            } else {
                /* 调用方命名空间且落在循环体之后 → 就是 break / while 条件为假
                 * 时的出口（target_bc == body_size 的常见情形），跳到 exit 正确。 */
                patch_rel32(cb, p->patch_mc, ctx->exit_mc);
            }
        } else {
            /* Jump target outside loop body → exit */
            patch_rel32(cb, p->patch_mc, ctx->exit_mc);
        }
    }

    return 1;  /* success */
}


