/*
 * jit.c - JIT compiler for Leno VM (Phase 1: template JIT for hot loops)
 *
 * Architecture:
 *   1. Hot loop detection: OP_LOOP / OP_FOR_LOOP hit counter
 *   2. Scanning: walk loop bytecode, verify all opcodes are JIT-able,
 *      collect used local slots
 *   3. Codegen: emit x86_64 machine code
 *      - Entry: type-guard locals, extract raw int64_t to scratch area
 *      - Loop body: PUSH/POP on real x86_64 stack, raw int arithmetic
 *      - Exit: re-encode int64_t to NaN-boxed, write back to locals
 *      - Bailout: return 1 (locals untouched)
 *
 * Supported opcodes (int-only fast path):
 *   Stack:    ZERO, ONE, POP, DUP
 *   Locals:   GET_LOCAL, SET_LOCAL, SET_LOCAL_POP, MOVE_LOCAL, MOVE_LOCAL_POP,
 *             SET_LOCAL_CONST
 *   Arith:    ADD_INT, SUB_INT, MUL_INT, MOD_INT, NEG_INT
 *             ADD_INT_IMM, SUB_INT_IMM, MUL_INT_IMM
 *   Inc/Dec:  INC_LOCAL_NOPUSH, DEC_LOCAL_NOPUSH, INC_LOCAL, DEC_LOCAL,
 *             PRE_INC_LOCAL, PRE_DEC_LOCAL
 *   Compare:  EQ_INT, LT_INT, GT_INT, LE_INT, GE_INT
 *             LT_INT_IMM, GT_INT_IMM, LE_INT_IMM, GE_INT_IMM, EQ_INT_IMM
 *   Jumps:    JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE, CMPJMP_LL_INT
 *   Back:     LOOP, FOR_LOOP
 *   Const:    CONST (only if int constant)
 *
 * NOT supported (causes scan to reject the loop):
 *   any string/object operation not listed above, exceptions, etc.
 *
 * Callout-based opcodes (fallback to C helpers):
 *   DIV (runtime type dispatch: int/int, float mix, BigInt),
 *   INDEX, ARRAY_APPEND_NOPUSH, DICT_SET
 *
 * NOT supported (call-heavy loops stay interpreted):
 *   CALL_GLOBAL_FUNC / CALL_GLOBAL_FUNC_TYPED —— callout 重入解释循环的
 *   单次开销远大于解释器原生调用路径，负优化；待内联实现后再支持
 */

#include "jit.h"
#include "jit_mem.h"
#include "../include/leno_error.h"
#include "../include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit_priv.h"

/* ---- Compile a loop and cache the result ---- */
static JitLoopFn jit_compile(CallFrame* frame, const uint8_t* body_start,
                             int body_size, int back_edge, VM* vm_ptr,
                             size_t* out_size) {
    ScanResult sr;
    /* out_size：把机器码映射长度带回给缓存条目（Linux munmap 必需，见 jit.h） */
    if (out_size) *out_size = 0;
    /* 解析「当前被编译函数所属模块」：模块函数调用的 ret_count 必须编译期确定，
       而函数所属模块记录在 ObjFunction 上（frame->closure->function->module）。 */
    jit_scan_set_module((frame && frame->closure && frame->closure->function)
                            ? frame->closure->function->module : NULL);
    scan_loop_body(body_start, body_size, back_edge, &sr, vm_ptr, frame->chunk);
    if (jit_debug_on()) {
        const char* fname = "?";
        int bc_off = -1;
        if (frame && frame->chunk) {
            bc_off = (int)(body_start - frame->chunk->code);
            if (frame->closure && frame->closure->function && frame->closure->function->name)
                fname = frame->closure->function->name;
            else
                fname = "<main>";
        }
        fprintf(stderr, "[JIT-DEBUG] COMPILE: fn='%s' bc_off=%d back_edge=%d, body_size=%d, capable=%d, n_locals=%d, max_vstack=%d, inline=%d\n",
                fname, bc_off, back_edge, body_size, sr.capable, sr.num_locals, sr.max_vstack, sr.inline_count);
        fprintf(stderr, "[JIT-DEBUG] body raw hex:");
        for (int i = 0; i < body_size && i < 1200; i++) {
            fprintf(stderr, " %02x", body_start[i]);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "[JIT-DEBUG] body opcodes:");
        const uint8_t* p = body_start;
        int printed = 0;
        int rel_off = 0;
        while (p < body_start + body_size && printed < 400) {
            int op = *p;
            int sz = opcode_size_chunk(frame ? frame->chunk : NULL, p);
            fprintf(stderr, " %d", op);
            if (sz < 0) break;
            fprintf(stderr, "(%d,", rel_off);
            /* Also print operands for key opcodes */
            if (sz >= 3) {
                for (int j = 1; j < sz && j <= 6; j++)
                    fprintf(stderr, "%d%s", p[j], j < sz-1 && j < 6 ? "," : "");
            }
            fprintf(stderr, ")");
            rel_off += sz;
            p += sz;
            printed++;
        }
        fprintf(stderr, "\n");
    }
    if (!sr.capable) {
        return NULL;
    }

    /* 循环体内有可达的 return：循环 JIT 无法从机器码里真正返回函数
     * （codegen 只能把 OP_RETURN 当作 no-op 继续执行），必须交解释器。
     * 五子棋 nearStone 的 `if 有子 { return true }` 就是此形态 —— JIT 下
     * return 被丢弃，函数恒返回 false，AI 每步都落天元。
     * 函数级 JIT（func_mode）与内联 callee 能正确处理 return，不走这里。 */
    if (sr.has_reachable_return) {
        if (jit_debug_on()) {
            const char* fname = "<main>";
            int bc_off = -1;
            if (frame && frame->chunk) {
                bc_off = (int)(body_start - frame->chunk->code);
                if (frame->closure && frame->closure->function && frame->closure->function->name)
                    fname = frame->closure->function->name;
            }
            fprintf(stderr, "[JIT-DEBUG] scan REJECT: fn='%s' bc_off=%d 循环体内含可达 return（循环 JIT 不支持）\n",
                    fname, bc_off);
        }
        return NULL;
    }

    /* Remap inline callee locals: place them after all caller locals
     * (si = n..n+inline_extra_locals-1), so caller locals (0..n-1) and
     * callee locals never overlap.  During scan, callee_local_map was
     * set with a base that may overlap caller locals marked later;
     * fix it now that n is final. */
    {
        int base = sr.num_locals;
        for (int i = 0; i < sr.inline_count; i++) {
            int clc = sr.inline_sites[i].callee_local_count;
            for (int j = 0; j < clc && j < 256; j++)
                sr.inline_sites[i].callee_local_map[j] = base + j;
            base += clc;
        }
    }

    CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    codebuf_init(&ctx.cb, 512);
    ctx.sr = &sr;
    ctx.body_start = body_start;
    ctx.chunk = frame->chunk;
    ctx.vm_ptr = vm_ptr;

    int ok = compile_loop(&ctx);

    /* LENO_JIT_DUMP=1 时把生成的机器码写入 jitdump<N>.bin（供反汇编调试） */
    if (getenv("LENO_JIT_DUMP") && ok && ctx.cb.len > 0) {
        char dumpname[64];
        snprintf(dumpname, sizeof(dumpname), "jitdump%d.bin", jit_state.compile_count);
        FILE* f = fopen(dumpname, "wb");
        if (f) {
            fwrite(ctx.cb.buf, 1, (size_t)ctx.cb.len, f);
            fclose(f);
        }
    }

    if (!ok || ctx.cb.len == 0) {
        if (jit_debug_on()) {
            fprintf(stderr, "[JIT-DEBUG] compile_loop returned %d, cb.len=%d, capable=%d, n_locals=%d, body_size=%d\n",
                    ok, ctx.cb.len, sr.capable, sr.num_locals, body_size);
            /* Print raw bytes and opcode walk */
            fprintf(stderr, "[JIT-DEBUG] raw bytes:");
            for (int i = 0; i < body_size && i < 30; i++)
                fprintf(stderr, " %d", body_start[i]);
            fprintf(stderr, "\n");
            fprintf(stderr, "[JIT-DEBUG] body opcodes:");
            const uint8_t* p = body_start;
            int printed = 0;
            while (p < body_start + body_size && printed < 20) {
                fprintf(stderr, " %d", *p);
                int sz = opcode_size_chunk(frame ? frame->chunk : NULL, p);
                if (sz < 0) break;
                p += sz;
                printed++;
            }
            fprintf(stderr, "\n");
        }
        codebuf_free(&ctx.cb);
        return NULL;
    }

    /* DEBUG: dump machine code */
    {
        const char* dbg = getenv("LENO_JIT_DUMP");
        if (dbg && (dbg[0] == '1' || dbg[0] == 't' || dbg[0] == 'T')) {
            FILE* df = fopen("jit_mc_dump.txt", "w");
            if (df) {
                fprintf(df, "JIT MC dump (%d bytes, n_locals=%d, max_vstack=%d):\n",
                        ctx.cb.len, sr.num_locals, sr.max_vstack);
                for (int i = 0; i < ctx.cb.len; i++) {
                    fprintf(df, "%02X ", ctx.cb.buf[i]);
                    if ((i + 1) % 16 == 0) fprintf(df, "\n");
                }
                fprintf(df, "\n");
                fclose(df);
            }
        }
    }

    /* Copy to executable memory */
    void* exec_mem = jit_mem_alloc((size_t)ctx.cb.len);
    if (!exec_mem) {
        codebuf_free(&ctx.cb);
        return NULL;
    }
    memcpy(exec_mem, ctx.cb.buf, (size_t)ctx.cb.len);
    /* 写完代码必须刷指令缓存才能执行（见 jit_mem.h 的说明） */
    jit_mem_flush(exec_mem, (size_t)ctx.cb.len);
    codebuf_free(&ctx.cb);
    if (out_size) *out_size = (size_t)ctx.cb.len;

return (JitLoopFn)exec_mem;
}


/* ---- Function-level JIT (hot function bodies, no loop semantics) ----
 *
 * 函数级 JIT 的目标：把整函数（无 OP_LOOP/OP_FOR_LOOP）编译为
 * fn(locals, globals) 机器码，供 jit_callout.c 中的 invoke/global call
 * 快路径直接调用。执行约定与循环 JIT 一致：
 *   - locals 数组契约：slot i = Value（NaN-boxed）；参数在低地址
 *   - 返回：0 成功（返回值写 jit_fn_result），非 0 失败/异常
 *   - func_mode=1：CodegenCtx 反馈用新栈帧（add rsp,24），
 *     OP_RETURN 写 jit_fn_result 后 ret；不做 locals 写回/RELOAD_RCX
 */
JitFuncCacheEntry jit_func_cache[JIT_FUNC_CACHE_SIZE];

/* ---- 机器码延迟释放队列（§8.60 / R4）----
 * 见 jit_priv.h 的契约说明。这里给出实现与唯一的两个入口：
 *   jit_code_retire() —— 驱逐/重编时替代 jit_mem_free（不安全就挂队列）
 *   jit_retire_drain() —— 回到「C 栈上无 JIT 机器码」时真释放
 * 顺带提供 LENO_JIT_FUNC_CACHE_SMALL=1（把函数缓存掩码缩到 3 位 = 4 槽）用于
 * **确定性复现** R4：4 槽下"函数级 JIT 正在执行 + callout 里编译别的函数"必然撞槽。*/
#define JIT_RETIRE_MAX 4096
typedef struct { void* ptr; size_t size; } JitRetireEntry;
static JitRetireEntry jit_retire_list[JIT_RETIRE_MAX];
static int jit_retire_count = 0;
static long jit_retire_leaked = 0;
static uintptr_t jit_func_cache_mask = JIT_FUNC_CACHE_SIZE - 1;

void jit_code_retire(void* ptr, size_t size) {
    if (!ptr) return;
    if (jit_func_depth == 0 && jit_loop_depth == 0) {
        jit_mem_free(ptr, size);   /* 没有任何机器码在 C 栈上：直接释放 */
        return;
    }
    if (jit_retire_count < JIT_RETIRE_MAX) {
        jit_retire_list[jit_retire_count].ptr = ptr;
        jit_retire_list[jit_retire_count].size = size;
        jit_retire_count++;
    } else {
        /* 宁可泄漏也不 free：那可能是正在执行的代码 */
        jit_retire_leaked++;
    }
}

void jit_retire_drain(void) {
    if (jit_retire_count == 0) return;
    if (jit_func_depth != 0 || jit_loop_depth != 0) return;   /* 仍在机器码里，不能放 */
    for (int i = 0; i < jit_retire_count; i++)
        jit_mem_free(jit_retire_list[i].ptr, jit_retire_list[i].size);
    if (jit_debug_on() && jit_retire_leaked > 0)
        fprintf(stderr, "[JIT-DEBUG] retire: 冲刷 %d 块（累计被迫泄漏 %ld 块）\n",
                jit_retire_count, jit_retire_leaked);
    jit_retire_count = 0;
}

/* 快速预扫描：函数体含循环回边（LOOP/FOR_LOOP/FOR_PREP）或非法指令
 * → 拒绝函数级 JIT（func_mode 的编译语义只对无循环函数保证正确）。
 * FOR_INCREMENT 也会出现在 FOR 循环中（含 FOR_PREP 时已被拒绝）。 */
static int func_body_is_simple(const uint8_t* code, int len, Chunk* chunk) {
    const uint8_t* p = code;
    const uint8_t* end = code + (size_t)len;
    while (p < end) {
        uint8_t op = *p;
        if (op == OP_LOOP || op == OP_FOR_LOOP || op == OP_FOR_PREP)
            return 0;
        /* R5-P0：这里以前用 opcode_size(p)，遇到 OP_CLOSURE 会因"长度未知"返回 -1
         * 而**静默**拒绝整个函数级 JIT —— 闭包在诊断里因此完全不可见。
         * 改用带 chunk 的解析后，含闭包的函数会走到 scan，由 scan 明确报出
         * 「含 OP_CLOSURE」并拒收；结论不变（仍然不编），但形态可测了。 */
        int sz = opcode_size_chunk(chunk, p);
        if (sz < 0 || p + sz > end)
            return 0;
        p += sz;
    }
    return 1;
}

static JitLoopFn jit_compile_function(ObjFunction* func, VM* vm_ptr,
                                      size_t* out_size) {
    if (out_size) *out_size = 0;   /* Linux munmap 需要长度，见 jit.h */
    if (!func || !func->chunk || func->chunk->len <= 0)
        return NULL;
    if (func->has_try || func->return_count > 1)
        return NULL;  /* 异常处理 / 多返回值语义复杂，回退解释器 */

    const uint8_t* code = func->chunk->code;
    int len = func->chunk->len;
    if (!func_body_is_simple(code, len, func->chunk))
        return NULL;

    ScanResult sr;
    jit_scan_set_module(func->module);
    scan_loop_body(code, len, 0, &sr, vm_ptr, func->chunk);
    if (!sr.capable)
        return NULL;

    /* 函数级 JIT 禁用内联：callee 调用统一走 callout 快路径
     * （递归/互调由 jit_func_lookup_or_compile 缓存兜底），
     * 避免递归函数自内联导致 codegen 语义复杂化。 */
    sr.inline_count = 0;
    sr.inline_extra_locals = 0;

    /* Remap inline callee locals: skipped (inline disabled) */

    CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    codebuf_init(&ctx.cb, 512);
    ctx.sr = &sr;
    ctx.body_start = code;
    ctx.chunk = func->chunk;
    ctx.vm_ptr = vm_ptr;
    ctx.func_mode = 1;   /* 函数模式：OP_RETURN 直接返回 */
    ctx.func = func;     /* §8.48：形参类型化快路径需要 arity / param_types */

    int ok = compile_loop(&ctx);

    if (getenv("LENO_JIT_DUMP") && ok && ctx.cb.len > 0) {
        char dumpname[64];
        snprintf(dumpname, sizeof(dumpname), "jitdump_func_%d.bin", jit_state.compile_count);
        FILE* f = fopen(dumpname, "wb");
        if (f) {
            fwrite(ctx.cb.buf, 1, (size_t)ctx.cb.len, f);
            fclose(f);
        }
    }

    if (!ok || ctx.cb.len == 0) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] func compile FAIL: '%s' capable=%d cb=%d\n",
                    func->name ? func->name : "?", sr.capable, ctx.cb.len);
        codebuf_free(&ctx.cb);
        return NULL;
    }

    void* exec_mem = jit_mem_alloc((size_t)ctx.cb.len);
    if (!exec_mem) {
        codebuf_free(&ctx.cb);
        return NULL;
    }
    memcpy(exec_mem, ctx.cb.buf, (size_t)ctx.cb.len);
    /* 写完代码必须刷指令缓存才能执行（见 jit_mem.h 的说明） */
    jit_mem_flush(exec_mem, (size_t)ctx.cb.len);
    codebuf_free(&ctx.cb);
    if (out_size) *out_size = (size_t)ctx.cb.len;

    jit_state.func_compile_count++;
    if (jit_debug_on())
        fprintf(stderr, "[JIT-DEBUG] func compiled: '%s' locals=%d bytes=%d\n",
                func->name ? func->name : "?", sr.num_locals, len);
    return (JitLoopFn)exec_mem;
}


/* ---- Public API ---- */

void jit_init(void) {
    memset(&jit_state, 0, sizeof(jit_state));
    jit_state.enabled = 1;

    /* Allow disabling JIT via environment variable for benchmarking */
    const char* no_jit = getenv("LENO_NO_JIT");
    if (no_jit && (no_jit[0] == '1' || no_jit[0] == 't' || no_jit[0] == 'T')) {
        jit_state.enabled = 0;
    }

    /* 基准开关一次性解析（§8.47）。只在启动读一次，热路径上是一次 test。 */
    jit_state.no_callcache = getenv("LENO_NO_CALLCACHE") ? 1 : 0;

    /* R4 复现开关（§8.60）：把函数缓存掩码缩到 4 槽，让"正在执行的函数级 JIT 被驱逐"
     * 必然发生（修好延迟释放后应不再崩）。仅测试用，默认全 256 槽。 */
    jit_func_cache_mask = getenv("LENO_JIT_FUNC_CACHE_SMALL")
                              ? (uintptr_t)3 : (uintptr_t)(JIT_FUNC_CACHE_SIZE - 1);
}

void jit_close(void) {
    for (int i = 0; i < JIT_CACHE_SIZE; i++) {
        JitCacheEntry* e = &jit_state.cache[i];
        if (e->fn) {
            jit_mem_free((void*)e->fn, e->code_size);   /* size：Linux munmap 必需 */
        }
    }
    for (int i = 0; i < JIT_FUNC_CACHE_SIZE; i++) {
        JitFuncCacheEntry* e = &jit_func_cache[i];
        if (e->fn) {
            jit_mem_free((void*)e->fn, e->code_size);
        }
    }
    /* 延迟释放队列里还挂着已驱逐的机器码（§8.60）—— 到这里已无机器码在执行，可直接放 */
    for (int i = 0; i < jit_retire_count; i++)
        jit_mem_free(jit_retire_list[i].ptr, jit_retire_list[i].size);
    jit_retire_count = 0;
    memset(jit_func_cache, 0, sizeof(jit_func_cache));
    memset(&jit_state, 0, sizeof(jit_state));
    jit_ft_profile_dump();
}

/* 取得（必要时新建 / 驱逐重建）func 对应的缓存条目。
 * 返回的条目保证 e->func == func；新建/驱逐时 hit_count / tried /
 * hot_disabled 均为 0，由调用方决定是否编译。 */
static JitFuncCacheEntry* jit_func_entry_claim(ObjFunction* func) {
    uintptr_t h = (uintptr_t)func;
    JitFuncCacheEntry* e = &jit_func_cache[(h >> 4) & jit_func_cache_mask];
    if (e->func == func)
        return e;
    if (e->fn)
        jit_code_retire((void*)e->fn, e->code_size);   /* §8.60：不能立即 free —— 可能正在执行；size：Linux munmap */
    memset(e, 0, sizeof(*e));
    e->func = func;
    return e;
}

/* 查找/编译函数级 JIT 缓存（direct-mapped，按 func 指针哈希）。
 * 编译失败缓存 tried 状态，避免重复编译开销。
 * 注意 e->tried 的语义：0 = 条目已建立但还没编译过（解释器热入口会先
 * 建条目做计数），此时必须补编译，不能因为「命中条目」就直接返回 NULL。 */
JitLoopFn jit_func_lookup_or_compile(ObjFunction* func, VM* vm_ptr) {
    if (!func || !jit_state.enabled)
        return NULL;
    JitFuncCacheEntry* e = jit_func_entry_claim(func);
    if (!e->tried) {
        e->tried = 1;
        e->fn = jit_compile_function(func, vm_ptr, &e->code_size);
    }
    return e->fn;   /* 命中（含尝试失败缓存 NULL） */
}

/* 解释器侧函数级 JIT 热入口（见 jit.h 的契约说明）。
 *
 * 与 callout 快路径的分工：
 *   - callout 路径（JIT 循环调用函数）保持「第一次就急切编译」，不受这里的
 *     热度计数影响，避免回归既有性能；
 *   - 这里只服务「解释器发起的调用」，阈值 + 一次失败即停用，宁可不编不猜。 */
int jit_try_hot_func_call(ObjClosure* closure, int arg_count, int typed, VM* vm_ptr) {
    if (!jit_state.enabled || !closure || !vm_ptr)
        return 0;
    ObjFunction* func = closure->function;
    if (!func || !func->chunk || func->chunk->len <= 0)
        return 0;
    /* 泛型实例化的闭包带 type_param_args，其语义依赖解释器在调用点设置
     * 类型参数（OP_PUSH_TYPE_ARGS）—— 函数级 JIT 不传递这些信息，直接拒收。 */
    if (closure->type_param_count > 0)
        return 0;
    /* JIT 函数是普通 C 调用链（JIT body → callout → JIT body），深度上限
     * 保护 C 栈；超限交回解释器（解释器的递归不消耗 C 栈）。 */
    if (jit_func_depth >= JIT_FUNC_MAX_DEPTH)
        return 0;
    /* 与 call() 同一栈约定：sp 以下必须恰好有 arg_count 个实参 + 1 个 callee */
    if (arg_count < 0 || vm_ptr->sp < arg_count + 1)
        return 0;

    JitFuncCacheEntry* e = jit_func_entry_claim(func);
    if (e->hot_disabled)
        return 0;
    if (!e->fn) {
        if (e->tried) {          /* 编译过但失败 → 永久走解释器 */
            e->hot_disabled = 1;
            return 0;
        }
        if (++e->hit_count < JIT_FUNC_HOT_THRESHOLD)
            return 0;
    }
    JitLoopFn jfn = jit_func_lookup_or_compile(func, vm_ptr);
    if (!jfn) {
        e->hot_disabled = 1;
        return 0;
    }

    /* ---- 装填 flocals：与 callout 快路径同一契约
     * （slot 0..arg_count-1 = 实参，其余 slot = null） ---- */
    int lcount = func->local_count > func->arity ? func->local_count : func->arity;
    if (lcount < arg_count) lcount = arg_count;
    if (lcount > JIT_MAX_LOCALS) lcount = JIT_MAX_LOCALS;
    Value* flocals = jit_func_locals_pool[jit_func_depth];
    for (int i = 0; i < lcount; i++)
        flocals[i] = NULL_VAL;
    TypeKind* pt = func->param_types;
    for (int i = 0; i < arg_count && i < lcount; i++) {
        Value a = vm_ptr->stack[vm_ptr->sp - arg_count - 1 + i];
        /* 参数类型提升与 call() 逐条对齐；typed 调用点（call_no_type_check）
         * 已由编译期保证类型匹配，跳过转换以保持两条路径一致。 */
        if (!typed && pt && i < func->arity) {
            TypeKind t = pt[i];
            if (t == TYPE_FLOAT && val_is_int(a))
                a = val_float((double)val_as_int(a));
            else if (t == TYPE_FLOAT && val_is_bigint(a))
                a = val_float(bigint_to_double(val_as_bigint(a)));
            else if (t == TYPE_INT && val_is_float(a))
                a = val_int((int)val_as_num(a));
        }
        flocals[i] = a;
    }

    jit_callout_vm = vm_ptr;   /* callout 依赖的全局 VM 指针（同循环 JIT） */
    jit_func_depth++;
    jit_fn_result = NULL_VAL;
    jit_callout_failed = 0;
    int jr = jfn(flocals, vm_ptr->globals);
    jit_func_depth--;
    jit_retire_drain();   /* §8.60：回到「C 栈上无机器码」时冲刷延迟释放队列 */
    if (jr != 0 || jit_callout_failed) {
        /* 失败/异常：整迭代语义交回解释器。JIT 中途的堆侧副作用无法回滚，
         * 与 callout 快路径的既有取舍一致（见文档 §14）。一次失败即停用
         * 解释器侧入口，避免「每次调用都进 JIT 再回退」的反复试探 ——
         * 同一份数据触发的 bailout 会稳定复现，重试没有意义。 */
        jit_callout_failed = 0;
        e->hot_disabled = 1;
        return 0;
    }

    /* ---- 折叠栈：arg_count+1 个槽 → 原 callee 槽里的 1 个返回值 ----
     * 与 call() + OP_RETURN 的净效应一致（结果落在 stack[sp-arg_count-1]）。 */
    vm_ptr->sp -= arg_count;
    vm_ptr->stack[vm_ptr->sp - 1] = jit_fn_result;
    vm_ptr->last_return_value = jit_fn_result;
    vm_ptr->last_return_count = 1;
    vm_ptr->last_return_values[0] = jit_fn_result;
    jit_state.func_execute_count++;
    return 1;
}

void jit_set_enabled(int enabled) {
    jit_state.enabled = enabled;
}

int jit_try_hot_loop(CallFrame* frame, VM* vm_ptr, int32_t loop_offset, int back_edge) {
    (void)vm_ptr;
    if (!jit_state.enabled) return 0;
    if (frame->locals == NULL) return 0;  /* need locals array */

    /* Compute loop body start and back-edge position */
    /* Compute loop body start */
    const uint8_t* after_be = frame->ip;       /* past the back-edge instruction */
    const uint8_t* body_start = after_be - loop_offset;
    int body_size = (int)(after_be - body_start);

    /* Sanity check */
    if (body_size <= 0 || body_size > 4096) return 0;

    /* Loop identity (for bailout diagnostics in stats output).
     * 必须在执行 JIT 之前取好：callout 可能触发 vm_grow_frames 重分配 frames，
     * 执行后 frame 指针可能失效。 */
    const char* loop_fn = "<main>";
    if (frame->closure && frame->closure->function && frame->closure->function->name)
        loop_fn = frame->closure->function->name;
    const int loop_bc_off = (int)(body_start - frame->chunk->code);

    /* Cache lookup：哈希 + 线性探测。窗口内先找同一循环，再找空槽；窗口满了才
     * 挑一个「最不值得留」的条目驱逐。
     * 老实现是 direct-mapped、冲突就地覆盖：两个别名循环互相驱逐（刚被驱逐的又要
     * 重新攒 50 次命中才能再编），实测 120 个热循环时 Compiled = 360（3 轮全量重编）。 */
    JitCacheEntry* entry = NULL;
    JitCacheEntry* victim = NULL;
    int base = cache_hash(body_start);
    for (int p = 0; p < JIT_CACHE_PROBES; p++) {
        JitCacheEntry* e = &jit_state.cache[(base + p) & (JIT_CACHE_SIZE - 1)];
        if (e->loop_ip == body_start) { entry = e; break; }   /* 命中同一循环 */
        if (e->loop_ip == NULL) { entry = e; break; }         /* 空槽 */
        /* 候选牺牲者：「价值」最低的 —— 没编译成功的优先，其次命中次数少的 */
        if (!victim) {
            victim = e;
        } else {
            int e_worth = (e->is_compiled ? 1 : 0) * 2 + (e->hit_count >= JIT_HOT_THRESHOLD ? 1 : 0);
            int v_worth = (victim->is_compiled ? 1 : 0) * 2 + (victim->hit_count >= JIT_HOT_THRESHOLD ? 1 : 0);
            if (e_worth < v_worth ||
                (e_worth == v_worth && e->hit_count < victim->hit_count))
                victim = e;
        }
    }
    if (!entry) {
        entry = victim;          /* 窗口内无空槽 → 驱逐最差候选（罕见） */
        if (!entry) return 0;    /* 理论不可达：窗口至少含 1 个槽 */
        jit_state.cache_evictions++;
    }

    if (entry->loop_ip != body_start) {
        /* 新循环（空槽，或驱逐后重用）—— 重置条目 */
        if (entry->fn) {
            /* §8.60：被驱逐的可能是**另一个正在执行**的循环 JIT（A 的机器码经 callout
             * 重入解释器，解释器又热启了循环 B；B 的编译撞到 A 的槽）⇒ 不能立即 free */
            jit_code_retire((void*)entry->fn, entry->code_size);
        }
        memset(entry, 0, sizeof(*entry));
        entry->loop_ip = body_start;
        entry->after_ip = after_be;
        entry->back_edge = back_edge;
    }

    /* Already tried and compilation failed — fast exit, don't even increment hit_count */
    if (entry->tried && !entry->is_compiled) {
        return 0;
    }

    entry->hit_count++;

    /* Not hot yet */
    if (entry->hit_count < JIT_HOT_THRESHOLD) {
        return 0;
    }

    /* Already tried and failed too many times */
    if (entry->bailout_count >= JIT_BAILOUT_LIMIT) {
        return 0;
    }

    /* Not compiled yet — try to compile (only once) */
    if (!entry->tried) {
        entry->tried = 1;
        entry->fn = jit_compile(frame, body_start, body_size, back_edge, vm_ptr,
                                &entry->code_size);
        entry->is_compiled = (entry->fn != NULL);
        jit_state.compile_count++;
        if (jit_debug_on() && !entry->is_compiled)
            fprintf(stderr, "[JIT-DEBUG] compile FAIL at body_start=%d, back_edge=%d\n",
                    (int)(body_start - frame->chunk->code), back_edge);
    }

    if (!entry->is_compiled || !entry->fn) {
        return 0;
    }

/* Execute JIT */
    jit_state.execute_count++;
    jit_callout_vm = vm_ptr;  /* set global VM pointer for callouts */
    /* Initialize reloaded locals to current value; callouts will update
     * this if vm_grow_frames reallocates vm.frames during nested execution. */
    jit_reloaded_locals = vm_ptr->frames[vm_ptr->frame_cnt - 1].locals;
    if (jit_debug_on()) {
fprintf(stderr, "[JIT-DEBUG] EXEC call #%d, fn=%p, locals=%p\n",
                jit_state.execute_count, (void*)entry->fn, (void*)frame->locals);
        {
            const unsigned char* cp = (const unsigned char*)entry->fn;
            fprintf(stderr, "[JIT-DUMP] ");
            for (int _i = 0; _i < 420; _i++)
                fprintf(stderr, "%02x ", cp[_i]);
            fprintf(stderr, "\n");
        }
        if (getenv("LENO_JIT_TRACE")) {
            fprintf(stderr, "[JIT-TRACE] PRE  #%d body_off=%-4d n_locals(scratch)=%d:", jit_state.execute_count, (int)(body_start - frame->chunk->code), 0);
            for (int _si = 0; _si < 40; _si++) {
                fprintf(stderr, " L[%02d]=%p", _si, (void*)(uintptr_t)frame->locals[_si]);
            }
            fprintf(stderr, "\n");
        }
    }
    /* jit_loop_depth：标记"正在执行循环 JIT 机器码"。
     * GC 的同步回收路径（gc_alloc 分配失败）据此避让 —— 见 §8.36。 */
    jit_loop_depth++;
    /* jit_callout_failed 是**粘滞**的：codegen 只在「检查通过的成功路径」上清它，
     * bailout 那条路不清（也没机会清）。若不在这里复位，一次真实的 callout 失败会让
     * 此后每一次进入任何带检查的循环都在第一条检查处立刻 bailout ——
     * 统计上表现为「1 次真因 + N 次连锁」（§8.53 实测 15 次里只有 1 次是真因），
     * 白烧 JIT_BAILOUT_LIMIT 的预算、把本该能编的循环拉黑。
     * 函数级 JIT 入口早已这么做（见 jit_try_hot_func_call 的 `jit_callout_failed = 0`），
     * 这里补齐循环侧。复位是安全的：本次执行中任何真实失败都会在 codegen 检查前重新置位。 */
    jit_callout_failed = 0;
    int result = entry->fn(frame->locals, vm_ptr->globals);
    jit_loop_depth--;
    jit_retire_drain();   /* §8.60：循环退出时若已无机器码在跑，冲刷延迟释放队列 */
    if (jit_debug_on()) {
        if (getenv("LENO_JIT_TRACE")) {
            fprintf(stderr, "[JIT-TRACE] POST #%d body_off=%-4d n_locals(scratch)=%d:", jit_state.execute_count, (int)(body_start - frame->chunk->code), 0);
            for (int _si = 0; _si < 40; _si++) {
                fprintf(stderr, " L[%02d]=%p", _si, (void*)(uintptr_t)frame->locals[_si]);
            }
            fprintf(stderr, "\n");
        }
        fprintf(stderr, "[JIT-DEBUG] EXEC returned %d\n", result);
        if (result == 0) {
            CallFrame* cf = &vm_ptr->frames[vm_ptr->frame_cnt - 1];
            fprintf(stderr, "[JIT-DEBUG] post-JIT: frame_cnt=%d ip_off=%d sp=%d locals=%p\n",
                    vm_ptr->frame_cnt, (int)(cf->ip - cf->chunk->code), vm_ptr->sp, (void*)cf->locals);
        }
    }

    if (result == 0) {
        /* Success — frame->ip is already past the back-edge */
        return 1;
    } else if (result == 2 || result == 3) {
        /* 函数调用 callout 内抛出异常，控制流已转移到新的当前帧
         * （宿主帧 catch_ip 或外层 handler 帧）。
         * 不计入 bailout；返回 2 让调用方重载 frame 后继续执行 */
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] FRAME-DEAD exit (%d) at body_start=%d\n",
                    result, loop_bc_off);
        return 2;
    } else if (result == 4) {
        /* Yield exit（§8.37 路线 3）：机器码在回边看到 jit_gc_yield_flag，
         * 走 exit 出口块（locals 已写回、vstack 已平衡）返回 4，请解释器接手
         * 并在安全状态下回收。**不是失败**：不计 bailout、不碰 hot_disabled，
         * 否则每跨一次年轻代阈值都会烧掉 1/3 的 JIT_BAILOUT_LIMIT。 */
        jit_state.yield_count++;
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] YIELD at body_start=%d (#%d)\n",
                    loop_bc_off, jit_state.yield_count);
        return 4;
    } else {
        /* Bailout — let interpreter handle it */
        entry->bailout_count++;
        jit_state.bailout_count++;
        /* 记录定位信息：stats 输出可看出是哪个循环、哪条字节码触发的 */
        entry->last_bailout_site = (int)jit_bailout_site;
        entry->last_bailout_bc_off = loop_bc_off;
        entry->last_bailout_fn = loop_fn;
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] BAILOUT at body_start=%d, count=%d\n",
                    loop_bc_off, entry->bailout_count);
        return 0;
    }
}

/* 把 jit_bailout_site 的编码翻译成可读原因（约定见 x86_64.c）。
 * 注意：溢出类与非溢出类 site 里带的偏移都是**相对循环体起点**的
 * （codegen 里 bc_off 从 body_start 开始计数），所以这里同时给出
 * 绝对偏移 = loop_bc + rel，和 `--debug` 的字节码偏移对齐。 */
static void jit_bailout_reason(int site, int loop_bc, char* out, size_t out_sz) {
    if (site >= 0)
        snprintf(out, out_sz, "int48 溢出/截断 @bc_off=%d（= loop_bc %d + %d）",
                 loop_bc + site, loop_bc, site);
    else if (site <= -1000) {
        int rel = -1000 - site;
        /* 内联帧（inlined callee）的偏移带 0x10000 * depth 基址（见 x86_64.c 的
         * site 约定），必须先剥掉，否则会报出 131425 这种天文数字误导排查。 */
        int depth = rel >> 16;
        int rel_in_body = rel & 0xFFFF;
        if (depth > 0)
            snprintf(out, out_sz, "非溢出类 @bc_off=%d（= loop_bc %d + %d，内联深度 %d）",
                     loop_bc + rel_in_body, loop_bc, rel_in_body, depth);
        else
            snprintf(out, out_sz, "非溢出类 @bc_off=%d（= loop_bc %d + %d）",
                     loop_bc + rel_in_body, loop_bc, rel_in_body);
    }
    else if (site == -3)
        snprintf(out, out_sz, "序言: step 为 float（非 int 循环）");
    else if (site == -2)
        snprintf(out, out_sz, "序言: step == 0");
    else if (site == -1)
        snprintf(out, out_sz, "序言: 进入自增 int48 溢出");
    else
        snprintf(out, out_sz, "序言: 未知 site=%d", site);
}

void jit_print_stats(void) {
    fprintf(stderr, "=== JIT Statistics ===\n");
    fprintf(stderr, "  Compiled: %d\n", jit_state.compile_count);
    fprintf(stderr, "  Executed: %d\n", jit_state.execute_count);
    fprintf(stderr, "  Bailouts: %d\n", jit_state.bailout_count);
    /* 回边让出（§8.37 路线 3）：不是失败，是「让解释器在安全状态回收一次」。
     * 长期为 0 而循环内又有分配 ⇒ 说明轮询没发射（循环体没有 callout）或
     * jit_gc_yield_flag 没被置起来。 */
    if (jit_state.yield_count > 0)
        fprintf(stderr, "  Yields:   %d\n", jit_state.yield_count);
    /* 列出发生过 bailout 的循环及其触发点，便于定位未消除的 bailout */
    if (jit_state.bailout_count > 0) {
        for (int i = 0; i < JIT_CACHE_SIZE; i++) {
            JitCacheEntry* e = &jit_state.cache[i];
            if (e->bailout_count <= 0) continue;
            char reason[96];
            jit_bailout_reason(e->last_bailout_site, e->last_bailout_bc_off, reason, sizeof(reason));
            fprintf(stderr, "  Bailout: fn='%s' loop_bc=%d x%d — %s\n",
                    e->last_bailout_fn ? e->last_bailout_fn : "?",
                    e->last_bailout_bc_off, e->bailout_count, reason);
        }
    }
    fprintf(stderr, "  Enabled:  %s\n", jit_state.enabled ? "yes" : "no");
    int compiled = 0;
    int tried = 0;
    for (int i = 0; i < JIT_CACHE_SIZE; i++) {
        if (jit_state.cache[i].is_compiled) compiled++;
        if (jit_state.cache[i].tried) tried++;
    }
    fprintf(stderr, "  Cached:   %d\n", compiled);
    fprintf(stderr, "  Tried:    %d\n", tried);
    /* 只有探测窗口满了才会 > 0；非 0 说明 JIT_CACHE_SIZE 或窗口该调大了 */
    if (jit_state.cache_evictions > 0)
        fprintf(stderr, "  Evicted:  %d\n", jit_state.cache_evictions);
    /* 函数级 JIT：FuncCompiled = 编译成功的函数个数（含 callout 急切编译），
     * FuncExecuted = 其中由「解释器侧热入口」执行的次数。后者长期为 0
     * 说明解释器调用点始终没热起来（或全部不可编译）。 */
    if (jit_state.func_compile_count > 0 || jit_state.func_execute_count > 0) {
        fprintf(stderr, "  FuncCompiled: %d\n", jit_state.func_compile_count);
        fprintf(stderr, "  FuncExecuted: %d\n", jit_state.func_execute_count);
    }
    fprintf(stderr, "======================\n");
}

