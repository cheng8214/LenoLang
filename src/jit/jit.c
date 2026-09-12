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
                             int body_size, int back_edge, VM* vm_ptr) {
    ScanResult sr;
    scan_loop_body(body_start, body_size, back_edge, &sr, vm_ptr);
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
            int sz = opcode_size(p);
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
                int sz = opcode_size(p);
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
    codebuf_free(&ctx.cb);

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

/* 快速预扫描：函数体含循环回边（LOOP/FOR_LOOP/FOR_PREP）或非法指令
 * → 拒绝函数级 JIT（func_mode 的编译语义只对无循环函数保证正确）。
 * FOR_INCREMENT 也会出现在 FOR 循环中（含 FOR_PREP 时已被拒绝）。 */
static int func_body_is_simple(const uint8_t* code, int len) {
    const uint8_t* p = code;
    const uint8_t* end = code + (size_t)len;
    while (p < end) {
        uint8_t op = *p;
        if (op == OP_LOOP || op == OP_FOR_LOOP || op == OP_FOR_PREP)
            return 0;
        int sz = opcode_size(p);
        if (sz < 0 || p + sz > end)
            return 0;
        p += sz;
    }
    return 1;
}

static JitLoopFn jit_compile_function(ObjFunction* func, VM* vm_ptr) {
    if (!func || !func->chunk || func->chunk->len <= 0)
        return NULL;
    if (func->has_try || func->return_count > 1)
        return NULL;  /* 异常处理 / 多返回值语义复杂，回退解释器 */

    const uint8_t* code = func->chunk->code;
    int len = func->chunk->len;
    if (!func_body_is_simple(code, len))
        return NULL;

    ScanResult sr;
    scan_loop_body(code, len, 0, &sr, vm_ptr);
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
    codebuf_free(&ctx.cb);

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
}

void jit_close(void) {
    for (int i = 0; i < JIT_CACHE_SIZE; i++) {
        JitCacheEntry* e = &jit_state.cache[i];
        if (e->fn) {
            jit_mem_free((void*)e->fn, 0);
        }
    }
    for (int i = 0; i < JIT_FUNC_CACHE_SIZE; i++) {
        JitFuncCacheEntry* e = &jit_func_cache[i];
        if (e->fn) {
            jit_mem_free((void*)e->fn, 0);
        }
    }
    memset(jit_func_cache, 0, sizeof(jit_func_cache));
    memset(&jit_state, 0, sizeof(jit_state));
    jit_ft_profile_dump();
}

/* 查找/编译函数级 JIT 缓存（direct-mapped，按 func 指针哈希）。
 * 编译失败缓存 tried 状态，避免重复编译开销。 */
JitLoopFn jit_func_lookup_or_compile(ObjFunction* func, VM* vm_ptr) {
    if (!func || !jit_state.enabled)
        return NULL;
    uintptr_t h = (uintptr_t)func;
    int idx = (int)((h >> 4) & (JIT_FUNC_CACHE_SIZE - 1));
    JitFuncCacheEntry* e = &jit_func_cache[idx];
    if (e->func == func) {
        return e->fn;   /* 命中（含尝试失败缓存 NULL） */
    }
    /* 新函数（或哈希冲突覆盖旧条目） */
    if (e->func && e->fn) {
        jit_mem_free((void*)e->fn, 0);
    }
    memset(e, 0, sizeof(*e));
    e->func = func;
    e->fn = jit_compile_function(func, vm_ptr);
    return e->fn;
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

    /* Cache lookup */
    int idx = cache_hash(body_start);
    JitCacheEntry* entry = &jit_state.cache[idx];

    /* Verify it's the right loop (handle hash collisions) */
    if (entry->loop_ip != body_start) {
        /* Different loop — reset entry */
        if (entry->fn) {
            jit_mem_free((void*)entry->fn, 0);
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
        entry->fn = jit_compile(frame, body_start, body_size, back_edge, vm_ptr);
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
    int result = entry->fn(frame->locals, vm_ptr->globals);
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
        snprintf(out, out_sz, "非溢出类 @bc_off=%d（= loop_bc %d + %d）",
                 loop_bc + rel, loop_bc, rel);
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
    fprintf(stderr, "======================\n");
}

