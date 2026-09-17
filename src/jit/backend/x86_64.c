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

/* Epilogue: mov rsp, rbp; pop rbx; pop <pin regs 逆序>; pop rbp; ret
 * (cannot use LEAVE because we pushed RBX/pin regs after RBP)
 * §8.45：R12/R13/R14 不再被 callout 使用（保存已挪到帧槽），改为按需承载 pin。 */
#define EMIT_EPILOGUE() do { \
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP); \
    emit_pop_reg(cb, JIT_RSI); \
    emit_pop_reg(cb, JIT_RDI); \
    emit_pop_reg(cb, JIT_RBX); \
    for (int _pi = pin_n - 1; _pi >= 0; _pi--) emit_pop_reg(cb, JIT_PIN_REGS[_pi]); \
    emit_pop_rbp(cb); \
    emit_ret(cb); \
} while(0)

static int scratch_disp(int scratch_idx) {
    return -8 * (scratch_idx + 1);
}

/* ---- §8.45 pin v2：最多 4 个局部量驻留寄存器 ----
 * 分配表（顺序 = 序言 push / 尾声 pop 的顺序）：R15, R14, R13, R12。
 * 这四个都是 callee-saved：本函数序言保存它们，被调用的 C 函数按 ABI 不会破坏，
 * 所以 pin 的值能**跨 callout 存活**（这正是 §8.42 选 R15 的同一理由）。
 * R12/R13/R14 在 §8.45 之前被 callout 的状态保存占用（RSP/RCX/R9），
 * 现在那三份状态挪到了帧槽 ⇒ 腾出来给 pin。 */
static const int JIT_PIN_REGS[4] = { JIT_R15, JIT_R14, JIT_R13, JIT_R12 };
#define JIT_PIN_MAX 4

/* 槽 si 是否被 pin；返回承载它的寄存器（0 = 未 pin） */
static int pin_reg_of(const int pin_si[], int pin_n, int si) {
    if (si < 0) return 0;
    for (int i = 0; i < pin_n; i++)
        if (pin_si[i] == si) return JIT_PIN_REGS[i];
    return 0;
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

/* §8.41：该字节码偏移是否有跳转进来。
 * 用于「上一条指令的证明」型窥孔（目前只有 OP_CAST_INT 的省略）：三目/短路的
 * 合并点恰好会落在编译器插入的 CAST 上，从别的路径跳进来时前一条指令并不是那个
 * int48 检查 —— 那时前提不成立，不能省。 */
static int bc_is_jump_target(const CodegenCtx* ctx, int bc_off) {
    for (int i = 0; i < ctx->patch_count; i++) {
        if (ctx->patches[i].target_bc == bc_off) return 1;
    }
    return 0;
}

/* ---- §8.42 局部量驻留寄存器：选择要 pin 的 scratch 槽 ----
 *
 * v1 是**保守版**：只 pin「访问点全部已转换」的槽 —— 即只被 OP_GET_LOCAL /
 * OP_SET_LOCAL / OP_SET_LOCAL_POP 访问的槽。任何还被其他 opcode 按**内存**方式
 * 访问的槽一律排除（否则寄存器与 scratch 槽会出现两份值 → 读到过期值、静默算错）。
 *
 * 排除表来自 src/jit/backend 下 `cur_local_map[` 的 grep（§8.42 的清单，20 处）。
 * **新增任何局部量访问点都必须同步这里。** */
#define JIT_PIN_MAX_SCRATCH 256

static void pin_excl(const ScanResult* sr, int excluded[], int slot) {
    if (slot < 0 || slot >= 256) return;
    int si = sr->local_map[slot];
    if (si >= 0 && si < JIT_PIN_MAX_SCRATCH) excluded[si] = 1;
}

static int pick_pin_locals(const uint8_t* body_start, const ScanResult* sr, Chunk* chunk,
                           int out[], int cnt_out[], int excluded[]) {
    int count[JIT_PIN_MAX_SCRATCH];
    for (int i = 0; i < JIT_PIN_MAX_SCRATCH; i++) { count[i] = 0; excluded[i] = 0; }
    const uint8_t* ip = body_start;
    const uint8_t* end = body_start + sr->body_size;
    while (ip < end) {
        uint8_t op = *ip;
        /* R5-P1：走查必须能**跨过** OP_CLOSURE（它的长度依赖常量表）。以前用
         * opcode_size 会在闭包处拿不到长度而 `break` ⇒ 走查被截断 ⇒ 后面的排除项
         * 全部漏记 ⇒ 可能把本该按内存访问的槽 pin 进寄存器（两份值 → 读到过期值、
         * 静默算错）。这正是本文件开头那句"新增任何局部量访问点都必须同步这里"的场景。 */
        int size = opcode_size_chunk(chunk, ip);
        if (size <= 0) break;
        switch (op) {
            /* 已转换（EMIT_LOAD_LOCAL / EMIT_STORE_LOCAL）：计数 */
            case OP_GET_LOCAL: case OP_SET_LOCAL: case OP_SET_LOCAL_POP: {
                int si = sr->local_map[rd_short(ip + 1)];
                if (si >= 0 && si < JIT_PIN_MAX_SCRATCH) count[si]++;
                break;
            }
            /* 未转换：排除（按内存侧操作数布局，与 codegen 的读法一致） */
            case OP_MOVE_LOCAL: case OP_MOVE_LOCAL_POP:
                pin_excl(sr, excluded, rd_short(ip + 1));
                pin_excl(sr, excluded, rd_short(ip + 3));
                break;
            case OP_SET_LOCAL_CONST:
                pin_excl(sr, excluded, rd_short(ip + 3));
                break;
            case OP_CLEAR_LOCAL_RANGE: {
                uint16_t base = rd_short(ip + 1);
                uint16_t cnt  = rd_short(ip + 3);
                for (uint16_t s = base; s < (uint16_t)(base + cnt); s++)
                    pin_excl(sr, excluded, s);
                break;
            }
            case OP_INC_LOCAL_NOPUSH: case OP_DEC_LOCAL_NOPUSH:
            case OP_INC_LOCAL:       case OP_DEC_LOCAL:
            case OP_PRE_INC_LOCAL:   case OP_PRE_DEC_LOCAL:
                pin_excl(sr, excluded, rd_short(ip + 1));
                break;
            /* §8.45：这两条已经转换（codegen 全部走 EMIT_LOAD_LOCAL /
             * EMIT_STORE_LOCAL）⇒ 与 GET/SET_LOCAL 一样**计数**，不再排除。
             * 这是「计数器能进寄存器」的前提：一旦排除，计数循环的三元组就
             * 永远拿不到寄存器，FOR_LOOP 回边那道尾部也就无法折成 add/cmp。 */
            case OP_FOR_PREP:
                for (int k = 1; k <= 4; k++) {   /* start, end, step, loop var */
                    int si = sr->local_map[ip[k]];
                    if (si >= 0 && si < JIT_PIN_MAX_SCRATCH) count[si]++;
                }
                break;
            case OP_FOR_LOOP:
                for (int k = 1; k <= 3; k++) {   /* loop var, step, end */
                    int si = sr->local_map[ip[k]];
                    if (si >= 0 && si < JIT_PIN_MAX_SCRATCH) count[si]++;
                }
                break;
            case OP_CMPJMP_LL_INT:
                pin_excl(sr, excluded, rd_short(ip + 2));
                pin_excl(sr, excluded, rd_short(ip + 4));
                break;
            case OP_CMPJMP_LG_INT:
            case OP_CMPJMP_LI_INT:
                pin_excl(sr, excluded, rd_short(ip + 2));
                break;
            case OP_GET_FIELD_FAST:
                pin_excl(sr, excluded, rd_short(ip + 1));
                break;
            default:
                break;   /* 不访问局部量 */
        }
        ip += size;
    }
    /* ---- ① 计数循环的三元组优先 ----
     * lv/step/end 三者都进寄存器时，FOR_LOOP 回边的「增量 + 比较」可以直接在
     * 寄存器上做（`add lv,step` / `cmp lv,end`），省掉 4 次访存。只 pin 其中
     * 一两个拿不到这个收益（§8.43：寄存器化本身指令数中性 ⇒ 必须整组选）。
     * 三元组都是编译器生成的 temp 槽，循环体内不会被按名访问，所以整组可用是常态。 */
    int n = 0;
    if (sr->back_edge_type == 2 && JIT_PIN_MAX >= 3) {
        int trio[3];
        trio[0] = sr->local_map[sr->for_loop_var_slot];
        trio[1] = sr->local_map[sr->for_step_slot];
        trio[2] = sr->local_map[sr->for_end_slot];
        int ok = 1;
        for (int i = 0; i < 3 && ok; i++) {
            if (trio[i] < 0 || trio[i] >= JIT_PIN_MAX_SCRATCH || excluded[trio[i]]) { ok = 0; break; }
            for (int j = 0; j < i; j++) if (trio[j] == trio[i]) ok = 0;   /* 槽重复（防御） */
        }
        if (ok) {
            for (int i = 0; i < 3; i++) {
                out[n] = trio[i];
                cnt_out[n] = count[trio[i]];
                excluded[trio[i]] = 1;   /* 不参与 ② 的挑选 */
                n++;
            }
        }
    }
    /* ---- ② 其余按访问次数填补；访问 < 2 次不值得占一个寄存器 ---- */
    while (n < JIT_PIN_MAX) {
        int best = -1, best_n = 1;
        for (int i = 0; i < JIT_PIN_MAX_SCRATCH; i++) {
            if (excluded[i] || count[i] <= best_n) continue;
            best = i;
            best_n = count[i];
        }
        if (best < 0) break;
        out[n] = best;
        cnt_out[n] = count[best];
        excluded[best] = 1;
        n++;
    }
    return n;
}

/* ---- §8.44 语句级折叠：GET_LOCAL(pinned) … SET_LOCAL_POP(同槽) ----
 *
 * 编译器对 `x = x ⊕ k` / `x = x + k` / `x = x >> k` 生成的是固定形状：
 *   OP_GET_LOCAL x
 *   [OP_CONST k | OP_ADD/SUB/MUL_INT_IMM i | OP_SHL/SHR_IMM i]   ← 立即数（必要）
 *   [OP_BITAND/BITOR/BITXOR]        ← 仅 CONST 形态需要（必须可交换）
 *   [OP_CAST_INT]                   ← §8.41 已证明它是恒等变换，这里一起吃掉
 *   OP_SET_LOCAL_POP x
 *
 * 当 x 是 pinned 槽时，整段可以折成**在寄存器上直接做一次 ALU 运算**：
 *   <op> r15, imm                    1 条
 *   int48 检查                        5 条
 * 而不是现在的 ~12 条（取局部量、把常量物化进 RAX、搬进 RDX、弹左操作数、运算、
 * 检查、写回）。**不新增任何语义假设**：操作数同样假定为 int48（与现有 codegen
 * 一致，由扫描器的类型检查保证），结果同样做 int48 检查。
 *
 * 只折叠**语义等价**的组合：AND/OR/XOR 可交换（常量在左在右都对）；
 * ADD/SUB/SHL/SHR 的立即数本来就在右侧（取自各自的 *_IMM opcode）。
 *
 * 返回消费的字节数（0 = 不折叠），并写出要发的运算与立即数。 */
static int peek_local_fold(CodegenCtx* ctx, const uint8_t* ip, Chunk* chunk,
                           int slot, int bc_off, uint8_t* out_alu, int32_t* out_imm) {
    int n = 0;
    const uint8_t* p = ip;
    if (p[0] != OP_GET_LOCAL) return 0;
    if (rd_short(p + 1) != (uint16_t)slot) return 0;
    n += opcode_size(p); p += opcode_size(p);

    uint8_t op = *p;
    int32_t imm = 0;
    uint8_t alu = 0;
    if (op == OP_ADD_INT_IMM || op == OP_SUB_INT_IMM ||
        op == OP_SHL_IMM || op == OP_SHR_IMM) {
        int8_t i8 = (int8_t)p[1];
        /* 与 codegen 一致：SHL_IMM 的 imm >= 32 走 BigInt 路径（无条件 bailout），不折叠 */
        if (op == OP_SHL_IMM && i8 >= 32) return 0;
        imm = (int32_t)i8;
        alu = op;
        n += opcode_size(p); p += opcode_size(p);
    } else if (op == OP_CONST) {
        uint16_t ci = rd_short(p + 1);
        if (!chunk || ci >= chunk->const_cnt) return 0;
        Value cv = chunk->constants[ci];
        if (!val_is_int(cv)) return 0;
        int64_t iv = val_as_int(cv);
        if (iv < -2147483648LL || iv > 2147483647LL) return 0;  /* 折叠只支持 imm32 */
        imm = (int32_t)iv;
        n += opcode_size(p); p += opcode_size(p);
        uint8_t b = *p;
        if (b != OP_BITAND && b != OP_BITOR && b != OP_BITXOR) return 0;
        alu = b;
        n += opcode_size(p); p += opcode_size(p);
    } else {
        return 0;
    }

    if (*p == OP_CAST_INT) { n += opcode_size(p); p += opcode_size(p); }

    if (*p != OP_SET_LOCAL_POP) return 0;
    if (rd_short(p + 1) != (uint16_t)slot) return 0;
    n += opcode_size(p);

    /* 被吃掉的偏移都不能是跳转目标：off_map 会缺条目，收尾时按「体内目标未解析」
     * 拒绝编译（宁可不编，不要猜）。本形状内部没有分支，正常情况下不会有跳进来。 */
    int off = bc_off;
    const uint8_t* q = ip;
    while (off < bc_off + n) {
        if (off != bc_off && bc_is_jump_target(ctx, off)) return 0;
        int qs = opcode_size(q);
        if (qs <= 0) return 0;
        off += qs;
        q += qs;
    }

    *out_alu = alu;
    *out_imm = imm;
    return n;
}

/* 形态 B：OP_CONST k; OP_GET_LOCAL x; <可交换 binop>; [OP_CAST_INT]; OP_SET_LOCAL_POP x
 * —— `x = k ⊕ x`（常量在左，编译器把常量先压栈）。只有可交换的位运算能这么折。 */
static int peek_local_fold_constfirst(CodegenCtx* ctx, const uint8_t* ip, Chunk* chunk,
                                      int bc_off, int* out_slot, uint8_t* out_alu,
                                      int32_t* out_imm) {
    int n = 0;
    const uint8_t* p = ip;
    if (p[0] != OP_CONST) return 0;
    uint16_t ci = rd_short(p + 1);
    if (!chunk || ci >= chunk->const_cnt) return 0;
    Value cv = chunk->constants[ci];
    if (!val_is_int(cv)) return 0;
    int64_t iv = val_as_int(cv);
    if (iv < -2147483648LL || iv > 2147483647LL) return 0;
    n += opcode_size(p); p += opcode_size(p);

    if (p[0] != OP_GET_LOCAL) return 0;
    int slot = rd_short(p + 1);
    n += opcode_size(p); p += opcode_size(p);

    uint8_t b = *p;
    if (b != OP_BITAND && b != OP_BITOR && b != OP_BITXOR) return 0;
    n += opcode_size(p); p += opcode_size(p);

    if (*p == OP_CAST_INT) { n += opcode_size(p); p += opcode_size(p); }

    if (*p != OP_SET_LOCAL_POP) return 0;
    if (rd_short(p + 1) != (uint16_t)slot) return 0;
    n += opcode_size(p);

    int off = bc_off;
    const uint8_t* q = ip;
    while (off < bc_off + n) {
        if (off != bc_off && bc_is_jump_target(ctx, off)) return 0;
        int qs = opcode_size(q);
        if (qs <= 0) return 0;
        off += qs;
        q += qs;
    }
    *out_slot = slot;
    *out_alu = b;
    *out_imm = (int32_t)iv;
    return n;
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
    /* §8.95：当前生效的模块。调用方 = `jit_scan_get_module()`；进入内联体时切到
     * **callee 的模块**（`InlineSite.callee_module`）—— 内联体里的模块变量访问必须按
     * 被调方的 globals 下标解析，否则会读到调用方模块的同名下标（静默算错 ✗）。 */
    ObjModule* cur_module = jit_scan_get_module();

    /* Inline frame stack: saved when entering an inlined callee,
     * restored when the callee body ends. */
    typedef struct {
        const uint8_t* ip;
        const uint8_t* end;
        const int* local_map;
        Chunk* chunk;
        ObjModule* module;      /* §8.95：进入内联体时切 module，退出时恢复 */
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

    /* ---- 回边让出（§8.37 路线 3）----
     * body_has_callout: 本循环体内是否发射过 callout（可能分配）。
     *   由 EMIT_CALLOUT_BEGIN() 置位 —— 这样内联进来的 callee 体里的 callout
     *   也自动算进去。只有它置位时才在回边发射 GC 轮询，纯算术热循环零开销
     *   （否则给 i++ 那种 2.5 cycle/iter 的循环加 2 条指令就是百分比级的税）。
     * yield_jmp_*: 回边轮询命中时的前向跳转补丁点 → 统一的 yield 出口块。 */
    int body_has_callout = 0;
    #define JIT_YIELD_MAX 8
    int yield_jmp_patches[JIT_YIELD_MAX];
    int yield_jmp_cnt = 0;

    /* ---- §8.41 窥孔状态：上一条指令的结果是否已被证明是 raw int48 ----
     * 由白名单 opcode（单路径、且 int48 检查覆盖所有到达路径）置位，在每条
     * 指令开头消费一次。用途：编译器为「非字面量赋给 int 局部量」无条件插入
     * 的 OP_CAST_INT 在这种情形下是恒等变换，可整段省掉。
     * 白名单见 EMIT_INT48_CHECK_TOS / MARK_TOS_RAW_INT48 的调用点；
     * 明确**不能**置位的：OP_ADD/SUB/MUL 通用版（int/float/concat 多路径）、
     * OP_USHR_IMM（shr 结果可能越出 int48）、OP_CAST_INT 自身（null 路径原样返回）。 */
    int prev_raw_int48 = 0;

    /* ---- §8.46 折叠「一步回看」状态：紧邻的上一条被折叠语句留下的值域掩码 ----
     * 语义：prev_fold_si >= 0 时，该槽的值 ∈ [0, prev_fold_mask]（掩码的位即
     * 「可能置位」的位，且全部落在低 47 位内 ⇒ 值非负且在 int48 内）。
     * 只由 §8.44 折叠路径写入，且只在**同槽、直线相邻**时有效：主循环每轮开头
     * 快照一次并清空（任何非折叠 opcode ⇒ 失效），另加扫描器的 jt_fwd /
     * has_back_jump 两个条件（见 ScanResult 的说明）。 */
    int prev_fold_si = -1;
    uint64_t prev_fold_mask = 0;

    /* ---- §8.45 局部量驻留寄存器 v2（最多 4 个槽，仅循环 JIT）----
     * pin_si[i] = 第 i 个被 pin 的 scratch 槽；承载寄存器 = JIT_PIN_REGS[i]
     * （R15/R14/R13/R12 —— R12/R13/R14 由 §8.45 把 callout 状态保存挪到帧槽
     * 之后腾出来的，见那里的说明）。只对真的被选中的寄存器 push/pop/装载。
     * 函数级 JIT 不 pin：那种 JIT 每次调用都进入，push/pop + 装载的固定成本更高。 */
    int pin_excluded[JIT_PIN_MAX_SCRATCH];
    int pin_si[JIT_PIN_MAX] = { -1, -1, -1, -1 };
    int pin_cnt[JIT_PIN_MAX] = { 0, 0, 0, 0 };
    int pin_n = 0;
    if (!ctx->func_mode)
        pin_n = pick_pin_locals(ctx->body_start, sr, ctx->chunk, pin_si, pin_cnt, pin_excluded);
    if (pin_n > 0 && jit_debug_on()) {
        fprintf(stderr, "[JIT-CG] pin: %d 槽 ->", pin_n);
        for (int _i = 0; _i < pin_n; _i++)
            fprintf(stderr, " scratch[%d](access=%d)", pin_si[_i], pin_cnt[_i]);
        fprintf(stderr, "\n");
    }

    /* 局部量读/写：pinned 槽走寄存器，其余走 scratch 槽（内存） */
    #define EMIT_LOAD_LOCAL(reg, si, disp) do { \
        int _pr = pin_reg_of(pin_si, pin_n, (si)); \
        if (_pr) { \
            emit_mov_rr(cb, (reg), _pr); \
        } else if ((disp) >= -128 && (disp) <= 127) { \
            emit_mov_reg_mem8(cb, (reg), JIT_RBP, (int8_t)(disp)); \
        } else { \
            emit_mov_reg_mem32(cb, (reg), JIT_RBP, (disp)); \
        } \
    } while(0)
    #define EMIT_STORE_LOCAL(reg, si, disp) do { \
        int _pr = pin_reg_of(pin_si, pin_n, (si)); \
        if (_pr) { \
            emit_mov_rr(cb, _pr, (reg)); \
        } else if ((disp) >= -128 && (disp) <= 127) { \
            emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)(disp), (reg)); \
        } else { \
            emit_mov_mem32_reg(cb, JIT_RBP, (disp), (reg)); \
        } \
    } while(0)

    /*
     * int48 overflow check: bail out to VM (which handles BigInt promotion)
     * if RAX doesn't fit in signed 48-bit range [-2^47, 2^47-1].
     *
     * sar r8, 47 yields 0 for valid positive, -1 for valid negative.
     * inc r8 maps 0→1, -1→0 (both ≤ 1 unsigned), anything else → > 1.
     * ja bailout catches the "anything else" case.
     *
     * site param: bytecode offset of the check. 它不再就地写进 jit_bailout_site
     * （那要在热路径上多付 4 条指令：push r9; movabs r9,&site; mov [r9],imm32;
     * pop r9，其中 movabs 占 10 字节），而是**只记录待消费的站点值**，由紧随
     * 其后的 patch_add(..., -1, ...) 转成「这个 bailout 分支自己的桩」——
     * 站点写入被移到桩里，只有真的 bailout 才执行（§8.40）。诊断信息一字不差。
     */
      #define EMIT_BAILOUT_SITE_WRITE(site) do { \
         ctx->pending_site       = (int)(site); \
         ctx->pending_site_valid = 1; \
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

     /* §8.41：int48 结果检查 + 标记「本指令产出的 TOS 是 raw int48」。
      * 只有**单路径**、且检查覆盖所有到达后续指令的路径的 case 才能用（白名单）。 */
     #define EMIT_INT48_CHECK_TOS(site) do { \
        EMIT_INT48_CHECK(site); \
        prev_raw_int48 = 1; \
     } while(0)

     /* §8.44：与 EMIT_INT48_CHECK 同形，但检查任意寄存器（折叠后的值在 pin 寄存器里，
      * 不在 RAX）。bailout 语义不受影响：JIT 运行整体被丢弃、解释器重放（§8.37）。 */
     #define EMIT_INT48_CHECK_REG(reg, site) do { \
        emit_mov_rr(cb, JIT_R8, (reg)); \
        emit_sar_imm(cb, JIT_R8, 47); \
        emit_inc_reg(cb, JIT_R8); \
        emit_cmp_reg_imm8(cb, JIT_R8, 1); \
        EMIT_BAILOUT_SITE_WRITE(site); \
        int _p = emit_jcc(cb, 0x87); \
        patch_add(ctx, _p, -1, 0); \
     } while(0)

     /* 结果必在 int48 内但没发检查的 case（OP_MOD_INT：|a%b| < |b|）。 */
     #define MARK_TOS_RAW_INT48() do { prev_raw_int48 = 1; } while(0)

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
     * Uses R8 as scratch, R10/R11 as constants.
     *
     * §8.104：若本指令的 TOS 来自某个 scratch 槽（`tos_from_si`，仅 OP_GET_LOCAL 置），
     * 先用 **RBX 类型位图**判该槽是否"已知非 int"（bit=1）——是则**跳过装箱**。
     * 为什么：位型启发式（raw>>47 ∈ {-1,0} ⇒ 当 int48）与浮点 **+0.0/正次正规数**撞码
     * ⇒ 会把 float 0.0 贴成 int 0（§8.99/§8.100/§8.101/§8.103 同一根因）。
     * RBX 位图由 prologue 按入参"是否 int48"建立、写回路径也按它判定（见下文写回宏）
     * ⇒ bit=1 表示该槽的值**不是裸 int48**（是裸 double 的 float / 对象 / bool / null），
     * 这些全都**已经是合法的 Value** ⇒ 跳过装箱总是对的（保守且精确）。
     * 仅在 `tos_from_si ∈ [0, 64)` 时启用（BT 的立即数位索引必须落在 64 位寄存器内）；
     * 其余情形保持原启发式 —— §8.103 的"不猜"守卫仍在各类型敏感边界上兜底。
     * ⚠ 本宏以 R8 为 scratch；带 _skip 分支多用一个 rel8 补丁，不改变寄存器约定。 */
    #define EMIT_RAW_TO_VALUE() do { \
        int _skip_p = -1; \
        if (tos_from_si >= 0 && tos_from_si < 64) { \
            emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xBA); \
            emit_byte(cb, 0xE3);  /* ModRM(11, 4, 3) = BT RBX, imm8 */ \
            emit_byte(cb, (uint8_t)(tos_from_si & 0xFF)); \
            emit_byte(cb, 0x72);  /* JC rel8 → 跳过装箱（该槽已知非 int） */ \
            _skip_p = cb->len; \
            emit_byte(cb, 0x00); \
        } \
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
        if (_skip_p >= 0) cb->buf[_skip_p] = (uint8_t)(cb->len - (_skip_p + 1)); \
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
    /* §8.97 曾给 `OP_FOR_PREP` 的"step == 0"守卫单独一个基址 `-2000 - off` 以便区分它与
     * float 守卫 —— **已停用并删除** ✗：该基址会与"非溢出类"的 `-1000 - off` **撞号**
     * （只要 off ≥ 1000，`-1000-off` 就落进 `<= -2000` 区间 ⇒ 被误报成 FOR_PREP ✗；
     * 实测 fm 的 off=64674 就撞上了）。§8.113 之后那个守卫本身也不再存在（step == 0
     * 改为**跳过循环**而不是 bailout）。诊断区间约定见 jit_bailout_debug 的注释。 */

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

    /* 浮点结果（xmm0）写回 RAX，作为新的 TOS —— 并做 §8.110 的"歧义区"守卫
     *
     * §8.110：位型落进 `[0, 2^47)` 的浮点只有两类 ——
     *   · `+0.0`（raw == 0）：**数值上无害**（被当 int 0 参与算术结果相同）⇒ 放行 ✓
     *   · **正次正规数**（raw != 0）：会被下游的位型启发式当成 int48 ⇒ **数值失真**
     *     （实测 2^-1060 → `16384.0`、2^-1074 → `1.0`；探针 probe_mul_float_min /
     *      probe_tiny_make_where / probe_tiny_fetch_or_store）⇒ 在这里就 bailout，
     *     交解释器算这一轮 ✓。
     * 为什么放这里：本宏只用于**浮点结果**（add/sub/mul/div/neg/cast_float）⇒ 一处收口 ✓；
     * 且 cast_float 的结果恒为 0 或 |x|≥1（cvtsi2sd）⇒ 守卫在那里不触发 ✓。
     * 代价：只有当**结果本身是正次正规数**才回退 —— 值域上极罕见 ✓（正常代码几乎不产生次正规）。
     * 注：负次正规（`0x8000…1`）的 `raw>>47` 非 0 ⇒ 不在歧义区 ⇒ 不受影响 ✓（与 §8.100 的值域表一致）。 */
    #define EMIT_MOVQ_RAX_XMM0_AT(off) do { \
        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); \
        emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0)); \
        emit_mov_rr(cb, JIT_R8, JIT_RAX); \
        emit_sar_imm(cb, JIT_R8, 47); \
        emit_test_rr(cb, JIT_R8, JIT_R8); \
        int _fl_ok = emit_jcc(cb, 0x85);       /* JNZ → 非歧义区，安全，跳过 */ \
        emit_test_rr(cb, JIT_RAX, JIT_RAX); \
        int _fl_zero = emit_jcc(cb, 0x84);     /* JZ（raw == 0 ⇒ +0.0）→ 放行 */ \
        EMIT_BAILOUT_SITE_NONOVF(off); \
        { int _fl_bail = emit_jmp(cb); patch_add(ctx, _fl_bail, -1, 0); } \
        patch_rel32(cb, _fl_ok, cb->len); \
        patch_rel32(cb, _fl_zero, cb->len); \
    } while(0)
    /* 便捷形式：调用点在循环体内（`bc_off` 在作用域内，bailout 报告指向该指令）。
     * 序言里没有 `bc_off`（§8.113 的 float 循环入口），用 `..._AT(0)`。 */
    #define EMIT_MOVQ_RAX_XMM0() EMIT_MOVQ_RAX_XMM0_AT(bc_off)

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
        body_has_callout = 1; \
        EMIT_STORE_TMP(co_rsp_disp, JIT_RSP); \
        EMIT_STORE_TMP(co_rcx_disp, JIT_RCX); \
        EMIT_STORE_TMP(co_r9_disp,  JIT_R9); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xE4); emit_byte(cb, 0xF0); \
        EMIT_CALLOUT_ALLOC(); \
    } while(0)

    /* Restore state, reload R10/R11 */
    #define EMIT_CALLOUT_END() do { \
        EMIT_LOAD_TMP(JIT_RSP, co_rsp_disp); \
        EMIT_LOAD_TMP(JIT_RCX, co_rcx_disp); \
        EMIT_LOAD_TMP(JIT_R9,  co_r9_disp); \
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

    /* §8.45：callout 跨调用的状态保存槽（原来放在 R12/R13/R14）。
     * 挪到帧槽之后，那三个 callee-saved 寄存器被腾出来给局部量 pin 用
     * （§8.42 的 v1 只有 R15 可用的直接原因）。代价只是每次 callout 3 存 + 3 取，
     * 都在 L1 里且被被调函数的延迟掩盖。 */
    int co_rsp_disp = -8 * (total_locals + sr->max_vstack + 4);
    int co_rcx_disp = -8 * (total_locals + sr->max_vstack + 5);
    int co_r9_disp  = -8 * (total_locals + sr->max_vstack + 6);

    /* R5-P2：本帧的 closure（第三参）。序言存入、upvalue 访问从这里取。
     * 放**本帧**而不是全局槽：A 调 B 时 B 的 closure 覆盖不到 A 的槽，
     * 嵌套天然隔离（设计文档不变量 I7）。 */
    int closure_disp = -8 * (total_locals + sr->max_vstack + 7);

    /* ---- Function prologue ---- */
    emit_push_rbp(cb);                        /* push rbp          */
    for (int _i = 0; _i < pin_n; _i++)        /* §8.45 pin 值寄存器（callee-saved） */
        emit_push_reg(cb, JIT_PIN_REGS[_i]);
    emit_push_reg(cb, JIT_RBX);              /* push rbx (type bitmap, callee-saved) */
    /* ★ RDI / RSI 保存（2026-09-14 修复「长跑后栈溢出/访问违例」）：
     * JIT 内部把 RDI/RSI 当 callout 参数寄存器用（JIT_ARG1/JIT_ARG2，见 EMIT_CALLOUT），
     * 但它们是**被调方必须保存**的寄存器（Win64 nonvolatile；SysV 上虽为 volatile，
     * 多两条 push/pop 也无害）—— 序言漏了它们，等于每次执行完一个含 callout 的 JIT
     * 循环都把 C 调用方（jit_try_hot_loop → 解释器主循环）的 RDI/RSI 破坏掉。
     * 解释器把活值放在这两个寄存器里时就会用被污染的值访存/设栈 → 随机崩溃，
     * 且与「已经执行了多少次 JIT」相关（matrix_rain 10s 必崩、3s 不崩即此）。
     * 位置必须在 `mov rbp, rsp` 之前，尾声按逆序 pop。 */
    emit_push_reg(cb, JIT_RDI);
    emit_push_reg(cb, JIT_RSI);
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
    /* R5-P2：第三参（closure）在 SysV 的 RDX 里 —— 必须**先**搬走，否则被
     * 下面的 `RDX = globals` 覆盖。Win64 的 arg3 本来就在 R8，无需搬运。 */
    emit_mov_rr(cb, JIT_R8, JIT_RDX);         /* R8 = closure (arg3) */
    emit_mov_rr(cb, JIT_RCX, JIT_RDI);        /* RCX = locals  (arg1) */
    emit_mov_rr(cb, JIT_RDX, JIT_RSI);        /* RDX = globals (arg2) */
#endif

    /* Allocate: scratch area (total_locals*8) + max_vstack*8 + callout temps (3*8)
     * + callout 状态保存槽 (3*8，§8.45) + closure 槽 (1*8，R5-P2)，rounded to 16 */
    int frame_sz = total_locals * 8 + sr->max_vstack * 8 + 16 + 24 + 24 + 8;
    frame_sz = (frame_sz + 15) & ~15;        /* align to 16 */
    if (frame_sz <= 127) {
        emit_sub_rsp_imm8(cb, (uint8_t)frame_sz);
    } else {
        emit_sub_rsp_imm32(cb, frame_sz);
    }

    /* R5-P2：把闭包指针落进本帧固定槽。R8 = 第三参（Win64 原生；SysV 已由上面的
     * shim 搬进来）。**必须在这里就存**：后续 callout 会把 R8 当 ARG3 用。 */
    EMIT_STORE_TMP(closure_disp, JIT_R8);

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
        /* ---- §8.48 类型化形参快路径 —— **§8.125 起停用** ✗（原因见下）----
         *
         * 它原来的前提是"写了具体类型的形参，运行期一定是该类型" ✓ —— 但对 **TYPE_INT**
         * 这个前提**不成立** ✗：语言里的 `int` **包含 bigint**（bigint 的静态类型同样是
         * `int`），而 bigint 的值是 `0xFFFC|对象指针`。原实现无条件 `<<16 >>16` 取
         * "int48 载荷" ⇒ 对 bigint 就是**削掉标签、只剩裸指针** ✗ ⇒ 其后所有运算都在
         * 指针上做 ⇒ **静默错值** ✗。
         *
         * 实测（决定性）：
         *   · `_jit_bugs/diag_add64_many.leno`：`add64(int a, int b)` 第 0..48 次调用 ✓
         *     全对，**第 49 次起**（= 函数级 JIT 阈值 `JIT_FUNC_HOT_THRESHOLD` 50 ✓）
         *     返回值变成裸指针 ✗；
         *   · `examples/crypto/sha512.leno`：三个标准向量全 FAIL ✗（NO_JIT 全 PASS ✓）；
         *   · `LENO_NO_TYPEDPARAM=1`（关掉本路径）⇒ 上面两个验收件**全对** ✓✓。
         *
         * 处理：**一律走下面的通用路径** ✓（它按 tag 分流：int48 取载荷 ✓ /
         * float·对象·null·bool 原样存 + 置 RBX 位图 ✓）。代价 = 每个形参多 4 条指令
         * （load + shr + cmp + je）。恢复快路径**必须带运行期 tag 确认**并重跑上面两个
         * 验收件：曾在本块里加过 `tag != 0xFFFB ⇒ jmp 通用路径`，但实测**未生效**
         * （行为与 `&& 0` 不同 ✗）⇒ 先停用，不把未验证的写法留在树里 ✓。
         * `LENO_NO_TYPEDPARAM` 保留：现在等价于默认行为（A/B 对比历史用）。 */
        {
            ObjFunction* _f = ctx->func;
            /* LENO_NO_TYPEDPARAM：A/B 对比用（当前默认就是"停用"，故无行为差异） */
            static int tp_disabled = -1;
            if (tp_disabled < 0) tp_disabled = getenv("LENO_NO_TYPEDPARAM") ? 1 : 0;
            if (tp_disabled && ctx->func_mode && _f && _f->param_types &&
                slot < _f->arity && _f->param_types[slot] == TYPE_INT && 0 /* §8.125 停用 */) {
                int sd = slot * 8;
                if (sd >= -128 && sd <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RCX, (int8_t)sd);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RCX, sd);
                /* ---- §8.125：**必须运行期确认这条实参真的是 NaN-boxed int** ----
                 * 语言里的 `int` 形参在运行期**也可能是 bigint**（bigint 的静态类型
                 * 同样是 `int` ✓，见 §8.125 的说明）而 bigint 的值是 `0xFFFC|对象指针`。
                 * 原实现无条件 `<<16 >>16` 取"int48 载荷" ⇒ 对 bigint 就是**削掉标签、
                 * 只剩裸指针** ✗ ⇒ 其后所有运算都在指针上做 ⇒ **静默错值**。
                 * 实测（`_jit_bugs/diag_add64_many.leno`）：`add64(int a, int b)` 在第 50 次
                 * 调用（函数级 JIT 阈值 `JIT_FUNC_HOT_THRESHOLD`）起返回值变成
                 * `0xFFFB|<堆指针>` ✗，同一条路径把 sha512 的三个向量全打坏 ✗。
                 * 修法：tag != 0xFFFB（float / 对象 / null / bool）⇒ 跳到下面的**通用路径**
                 * —— 它按 tag 分流：int 取载荷 ✓、其余原样存 + 置 RBX 位图 ✓（写回据此
                 * 不重新装箱 ✓）。只有 base 为 0 的快路径要付这三条指令。 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                {   /* shr r8, 48 → 取 Value 的 tag */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                    emit_byte(cb, 48);
                }
                {   /* cmp r8, 0x0000FFFB（TAG_INT >> 48） */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0x81);              /* CMP r/m64, imm32 */
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFFB);
                }
                emit_byte(cb, 0x0F); emit_byte(cb, 0x85);   /* JNE rel32 → 通用路径 */
                int tp_fallback_patch = cb->len;
                emit_uint32(cb, 0);
                emit_shl_imm(cb, JIT_RAX, 16);   /* 取 int48 载荷 */
                emit_sar_imm(cb, JIT_RAX, 16);   /* 符号扩展 */
                if (disp >= -128 && disp <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                patch_rel32(cb, tp_fallback_patch, cb->len);   /* 非 int ⇒ 通用路径 */
                continue;
            }
        }
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

    /* §8.45：把 pin 的槽装进寄存器。scratch 槽仍是权威副本（写回路径按内存读它），
     * 但循环体与入口检查对它的访问都已改为走宏（EMIT_LOAD_LOCAL / STORE_LOCAL）
     * ⇒ 不会再从内存读到过期值。 */
    for (int _i = 0; _i < pin_n; _i++) {
        int _pd = scratch_disp(pin_si[_i]);
        if (_pd >= -128 && _pd <= 127)
            emit_mov_reg_mem8(cb, JIT_PIN_REGS[_i], JIT_RBP, (int8_t)_pd);
        else
            emit_mov_reg_mem32(cb, JIT_PIN_REGS[_i], JIT_RBP, _pd);
    }

    /* ---- Step check (for OP_FOR_LOOP) ----
     * 支持正/负步长（倒序循环）：方向在入口处按 step 符号分流，不在此 bailout。
     * 仅两种情况仍回退解释器：
     *   site -3: step 是 float（类型位图为 1）——JIT 的 FOR_LOOP 走 int48 快路径，
     *            无法对 double 位模式做方向/比较判断；
     *   site -2: step == 0 —— VM 语义为“不进循环”，交给解释器处理。 */
    /* §8.113：float 步长入口里"条件满足 ⇒ 进循环体"的跳转要等循环体起点
     * （loop_start_mc）确定后才能补 ⇒ 先声明在函数作用域。
     * for_loop_entry_patches 也一并上移：float 入口的"条件不满足 ⇒ 退出"复用同一张表，
     * 由既有代码统一补到 exit_mc（不必在此处知道 exit_mc）。 */
    int for_loop_entry_patches[16];
    int for_loop_patch_cnt = 0;
    int float_body_patches[4];
    int float_body_cnt = 0;

    if (sr->back_edge_type == 2) {
        int step_scratch = cur_local_map[sr->for_step_slot];
        if (step_scratch < 0) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] codegen FAIL: FOR_LOOP step_scratch<0\n");
            return 0;  /* shouldn't happen */
        }
        int disp = scratch_disp(step_scratch);

        /* 类型位图：bit(step_scratch) == 1 表示该 local 是 float → bailout
         * （§8.113 曾尝试在这里也接一条 SSE 入口，实测会让嵌套的浮点内层循环
         *   少跑一轮 ⇒ 数值分叉 ✗，已回退；详见 §8.113 的残余记录。此处保持 bail。） */
        emit_byte(cb, 0x48);
        emit_byte(cb, 0x0F);
        emit_byte(cb, 0xBA);
        emit_byte(cb, 0xE3);  /* ModRM(11, 4, 3) = BT RBX, imm8 */
        emit_byte(cb, (uint8_t)(step_scratch & 0xFF));
        EMIT_BAILOUT_SITE_WRITE(-3);
        int float_patch = emit_jcc(cb, 0x82);  /* JC → bailout */
        patch_add(ctx, float_patch, -1, 0);

        /* Load step（§8.45：step 可能已被 pin，走宏） */
        EMIT_LOAD_LOCAL(JIT_RAX, step_scratch, disp);
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
            EMIT_LOAD_LOCAL(JIT_RAX, si_lv, d_lv);   /* loop_var */ \
            EMIT_LOAD_LOCAL(JIT_RDX, si_st, d_st);   /* step     */ \
            emit_add_rr(cb, JIT_RAX, JIT_RDX);       /* loop_var += step */ \
            /* -1: this check is the loop-entry increment, bc_off not yet in scope */ \
            EMIT_INT48_CHECK(-1); \
            EMIT_STORE_LOCAL(JIT_RAX, si_lv, d_lv); \
            EMIT_LOAD_LOCAL(JIT_RDX, si_en, d_en);   /* end */ \
            emit_cmp_rr(cb, JIT_RAX, JIT_RDX); \
        } while(0)

        /* 读取 step 符号：step < 0 走反向入口（支持倒序 for 循环） */
        EMIT_LOAD_LOCAL(JIT_RAX, si_st, d_st);
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
    /* §8.113：float 入口里"条件满足 ⇒ 进循环体"的跳转，此时才知道体起点 */
    for (int _fi = 0; _fi < float_body_cnt; _fi++) {
        patch_rel32(cb, float_body_patches[_fi], ctx->loop_start_mc);
    }

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
        /* §8.46：一步回看快照 —— 只有**紧邻的上一条语句**是同槽折叠时掩码才有效；
         * 本轮若不是那条语句（任何别的 opcode，含 callout / 跳转 / 内联点），
         * 下面就把状态清掉。 */
        int fold_prev_si = prev_fold_si;
        uint64_t fold_prev_mask = prev_fold_mask;
        prev_fold_si = -1;
        prev_fold_mask = 0;

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
                cur_module = f->module;      /* §8.95：恢复调用方模块 */
                continue;
            }
            break;  /* normal end of loop body */
        }

        uint8_t op = *ip;
        /* R5-P1：OP_CLOSURE 的长度取决于常量表（upvalue_count），opcode_size 拿不到
         * chunk ⇒ 用带 chunk 的版本（其余 opcode 转发给 opcode_size，行为不变）。 */
        int size = opcode_size_chunk(ctx->chunk, ip);

        /* §8.41：消费上一条指令留下的「结果是 raw int48」标记（一次性：
         * 只有**紧邻的前一条**指令作过证明才算数，跨一条就失效）。 */
        int prev_int48 = prev_raw_int48;
        prev_raw_int48 = 0;

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

        /* ---- ★ 合并点 TOS 归一化（2026-09-14 修复「每轮迭代漏 8 字节 → RSP 漂移
         *      → 长跑栈溢出」）----
         * 背景：跳转路径**一律**以「TOS 落在内存栈」的形态到达目标
         *   - OP_JUMP            ：先 TOS_SPILL 再跳；
         *   - 条件跳转（真假分支）：tos_live 时先 push 再跳，否则值本就在内存栈上；
         * 而**直落**到达目标时，上一条指令可能把 TOS 留在 RAX 里（tos_live=1）。
         * 两条到达路径形态不一致 ⇒ 按直落形态（mem = vstack - tos_live）发码的目标代码
         * 会比运行时少算一个 slot，于是每次经过这个合并点，跳转路径 push 的那一项就
         * 永远没人消费 ⇒ **每轮迭代泄漏 8 字节**，RSP 单调下漂，最终撞栈底崩溃
         * （matrix_rain 报告里的 `if dc >= b0 and dc <= b1` 短路恰好构成这种形态：
         *  第一条 JUMP_IF_FALSE 的 false 分支跳到第二条 JUMP_IF_FALSE 本身，
         *  而第二条又能从上一条 LE_INT 直落到达 —— 实测 1,599,992 = 199,999 × 8）。
         *
         * 修法：在**直落路径**上把 TOS 落到内存（`push rax`；vstack 不变，只把
         * tos_live 清 0），并把 offmap_add 挪到这条 spill **之后** —— 于是跳转路径落在
         * spill 之后（本来就是内存形态，不需要也不该再 push），直落路径执行 spill 后
         * 也变成内存形态，两条路径一致。值本身两条路径都在 RAX 里（条件跳转的
         * 内存形态分支会先 mov rax,[rsp] 把值读回 RAX），所以 push 的是正确的值。
         * 注意只在 vstack 可达（直落会执行）时才需要；直落不可达时下面的 restore
         * 已经把 tos_live 清 0，不会有形态分歧。 */
        if (vstack != VSTACK_UNREACHABLE && tos_live && patch_targets(ctx, bc_off)) {
            emit_push_reg(cb, JIT_RAX);
            tos_live = 0;
        }

        /* offmap_add 必须记在 spill 之后：跳转目标要落在 spill 之后。 */
        offmap_add(ctx, bc_off, cb->len);

        /* §8.104：本指令 TOS 的"来源 scratch 槽"（-1 = 未知）。只有 OP_GET_LOCAL 会置它，
         * 供 EMIT_RAW_TO_VALUE 用 RBX 类型位图判断"该槽已知非 int"（见宏内注释）。
         * 每轮迭代重置 ⇒ 只对"紧跟 GET_LOCAL 的那次转换"生效，绝不外溢到别的操作数。 */
        int tos_from_si = -1;

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
        /* §8.45：先把 pin 的寄存器刷回 scratch 槽 —— 下面的写回循环按内存读它。 \
         * 只在写出路径（exit / framedead / yield）执行，属于冷路径。 \
         * bailout 路径**刻意不写回**（解释器从回边重跑本轮，见那里的注释）， \
         * 所以 pin 也不需要在那里刷。 */ \
        for (int _pi = 0; _pi < pin_n; _pi++) { \
            int _pd = scratch_disp(pin_si[_pi]); \
            if (_pd >= -128 && _pd <= 127) \
                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)_pd, JIT_PIN_REGS[_pi]); \
            else \
                emit_mov_mem32_reg(cb, JIT_RBP, _pd, JIT_PIN_REGS[_pi]); \
        } \
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

    /* ---- bailout 站点桩（§8.40）----
     * 每个 bailout 分支的落点：先把「是哪条检查失败的」写进全局 jit_bailout_site
     * （stats / [JIT-DEBUG] 靠它报出失败位置），再 jmp 到上面的共享 bailout 块。
     * 这些代码只有真的 bailout 时才执行，所以热路径上省掉了原来那 4 条指令
     * （push r9; movabs r9,&site; mov [r9],imm32; pop r9）。
     * R9 在这里可以随便用：马上要 bailout，调用方（解释器）会自己重新装载 R9。 */
    for (int _si = 0; _si < ctx->bail_stub_count; _si++) {
        int stub_mc = cb->len;
        patch_rel32(cb, ctx->bail_stub_mc[_si], stub_mc);
        emit_mov_reg_imm64(cb, JIT_R9, (uint64_t)(uintptr_t)&jit_bailout_site);
        emit_byte(cb, 0x41); emit_byte(cb, 0xC7); emit_byte(cb, 0x01);  /* mov [r9], imm32 */
        emit_uint32(cb, (uint32_t)(int32_t)ctx->bail_stub_site[_si]);
        int _j = emit_jmp(cb);
        patch_rel32(cb, _j, ctx->bailout_mc);
    }

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

    /* ---- Yield exit（§8.37 路线 3）：回边轮询命中 → 让出到解释器 ----
     * 与上面的 exit 块**同形**（先平衡 vstack 再写回 locals），唯一差别是返回码 4。
     * 之所以安全：回边是迭代边界、且回边处 vstack 平衡（OP_LOOP 的 codegen 开头
     * 就 TOS_SPILL），所以解释器 `frame->ip -= offset` 从循环头继续，语义等价于
     * 「刚执行完一次回边」—— 不会重放、不会重复副作用（§8.37 里 bailout 的
     * 6.36M 次死循环正是因为 bailout 不满足这两点）。
     * 让出**不计入 bailout**（jit_try_hot_loop 单独映射返回码 4），否则每跨一次
     * 年轻代阈值就烧掉 1/3 的 JIT_BAILOUT_LIMIT，循环很快被永久放弃。 */
    if (yield_jmp_cnt > 0) {
        int yield_mc = cb->len;
        for (int _i = 0; _i < yield_jmp_cnt; _i++)
            patch_rel32(cb, yield_jmp_patches[_i], yield_mc);
        if (vstack > 0) {
            if (vstack * 8 <= 127) {
                emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                emit_byte(cb, (uint8_t)(vstack * 8));
            } else {
                emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                emit_uint32(cb, (uint32_t)(vstack * 8));
            }
        }
        if (!ctx->func_mode) {
            EMIT_RELOAD_RCX();
            EMIT_WRITEBACK_LOCALS();
        }
        emit_mov_eax_imm32(cb, 4);
        EMIT_EPILOGUE();
    }

    #undef EMIT_WRITEBACK_LOCALS

    /* ---- 溢出检查：off_map / patch 丢过条目就拒绝编译 ----
     * 残缺的 off_map 会让回边/前向跳转解析失败，残缺的 patch 会留下 rel32=0
     * 的跳转（跳到代码段开头）。两者都是"静默发出错误机器码"，按项目原则
     * （宁可不编、不要猜）一律拒绝，循环退回解释器。 */
    if (ctx->off_overflow || ctx->patch_overflow) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] COMPILE-FAIL: off_map/patch/桩表 溢出 "
                            "(off=%d/%d patch=%d/%d stub=%d/%d) → 拒绝编译\n",
                    ctx->off_count, JIT_MAX_LOOP_OPS * 10,
                    ctx->patch_count, JIT_MAX_PATCHES,
                    ctx->bail_stub_count, JIT_MAX_BAILOUT_STUBS);
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


