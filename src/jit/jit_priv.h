/*
 * jit_priv.h - 内部共享声明（jit_callout.c / jit_scan.c / jit.c 共用）
 * 公共平台部分：见 jit.h（API）、jit_mem.h（可执行内存，平台分支）。
 */
#ifndef LENO_JIT_PRIV_H
#define LENO_JIT_PRIV_H

#include "jit.h"
#include "../include/native.h"   /* ModuleMethodMeta（模块方法编译期解析用） */
#include <stdint.h>
/* ---- CodeBuf ---- */
typedef struct {
    uint8_t* buf;
    int      len;
    int      cap;
} CodeBuf;

static inline void codebuf_init(CodeBuf* cb, int initial_cap) {
    cb->cap   = initial_cap > 0 ? initial_cap : 256;
    cb->buf   = (uint8_t*)malloc((size_t)cb->cap);
    cb->len   = 0;
}

static inline void codebuf_free(CodeBuf* cb) {
    free(cb->buf);
    cb->buf = NULL;
    cb->len = cb->cap = 0;
}

static inline void codebuf_ensure(CodeBuf* cb, int need) {
    if (cb->len + need > cb->cap) {
        while (cb->len + need > cb->cap) cb->cap *= 2;
        cb->buf = (uint8_t*)realloc(cb->buf, (size_t)cb->cap);
    }
}

static inline void emit_byte(CodeBuf* cb, uint8_t b) {
    codebuf_ensure(cb, 1);
    cb->buf[cb->len++] = b;
}

static inline void emit_uint32(CodeBuf* cb, uint32_t v) {
    codebuf_ensure(cb, 4);
    cb->buf[cb->len++] = (uint8_t)(v);
    cb->buf[cb->len++] = (uint8_t)(v >> 8);
    cb->buf[cb->len++] = (uint8_t)(v >> 16);
    cb->buf[cb->len++] = (uint8_t)(v >> 24);
}

static inline void emit_uint64(CodeBuf* cb, uint64_t v) {
    codebuf_ensure(cb, 8);
    for (int i = 0; i < 8; i++)
        cb->buf[cb->len++] = (uint8_t)(v >> (i * 8));
}

/* ---- NaN-boxing constants (must match leno_value.h) ---- */
/* QNAN | SIGN_BIT | TAG_MASK = 0xFFFF000000000000 */
#define JIT_TYPE_MASK   0xFFFF000000000000ULL
/* QNAN | SIGN_BIT | TAG_INT  = 0xFFFB000000000000 */
#define JIT_INT_TAG     0xFFFB000000000000ULL
/* PAYLOAD_MASK                        = 0x0000FFFFFFFFFFFF */
#define JIT_PAYLOAD_MSK 0x0000FFFFFFFFFFFFULL
/* QNAN | SIGN_BIT                     = 0xFFF8000000000000
 * Any Value whose top 16 bits (after shr 48) >= 0xFFF8 is NaN-boxed
 * (null/bool/int/obj/bigint) and NOT a float. */
#define JIT_NAN_SIG     0xFFF8000000000000ULL

static inline uint16_t rd_short(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline int32_t rd_int32(const uint8_t* p) {
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
static inline int8_t rd_byte(const uint8_t* p) {
    return (int8_t)p[0];
}

/* ---- Inline site (for OP_CALL_GLOBAL_FUNC_TYPED inlining) ---- */
typedef struct {
    int bc_off;              /* bytecode offset of the call in caller */
    uint16_t func_slot;      /* global func slot of callee */
    int arg_count;           /* number of args */
    int ret_count;           /* number of return values (1 or 2) */
    Chunk* callee_chunk;     /* callee's bytecode chunk */
    int callee_local_count;  /* callee's local_count */
    int callee_local_base;   /* base scratch index for callee locals */
    int callee_body_size;    /* bytecode size of callee body */
    int callee_local_map[256]; /* callee local slot → scratch index */
    int inline_end_mc;       /* mc offset of inline-end label (set during codegen) */
} InlineSite;

/* ---- Scan result ---- */
typedef struct {
    int capable;
    int num_locals;
    uint8_t local_slots[JIT_MAX_LOCALS];
    int local_map[256];         /* slot → scratch index, -1 if unused */
    int max_vstack;
    int body_size;              /* bytecode bytes (including back-edge) */
    int back_edge_type;         /* 1=OP_LOOP, 2=OP_FOR_LOOP */
    /* FOR_LOOP info */
    uint8_t for_loop_var_slot;
    uint8_t for_step_slot;
    uint8_t for_end_slot;
    uint8_t for_inclusive;
    /* Inline sites */
    InlineSite inline_sites[4]; /* max 4 inline calls per loop */
    int inline_count;
    int inline_extra_locals;   /* total extra locals from inlining */
} ScanResult;

/* ---- Codegen: offset map and patch list ---- */
typedef struct {
    int bc_off;  /* bytecode offset (relative to body_start) */
    int mc_off;  /* machine code offset (relative to codebuf) */
} OffMap;

typedef struct {
    int patch_mc;     /* offset in codebuf where rel32 lives */
    int target_bc;    /* bytecode offset of jump target */
    int vstack;       /* virtual stack depth at this point (for cleanup) */
} Patch;

typedef struct {
    CodeBuf cb;
    OffMap off_map[JIT_MAX_LOOP_OPS * 10];
    int off_count;
    Patch patches[JIT_MAX_PATCHES];
    int patch_count;
    int loop_start_mc;   /* machine code offset of loop body start */
    int exit_mc;         /* machine code offset of exit code */
    int bailout_mc;      /* machine code offset of bailout code */
    int framedead_mc;    /* machine code offset of frame-dead exit (write back, ret 2) */
    int framedead_nowb_mc; /* machine code offset of frame-dead exit (no write back, ret 3) */
    const ScanResult* sr;
    const uint8_t* body_start;
    Chunk* chunk;        /* for constant table access */
    VM* vm_ptr;          /* for looking up global_funcs at codegen time */
    int func_mode;       /* 1 = 函数级 JIT：整个函数编译为 fn(locals, globals)，
                          * OP_RETURN 真正返回（结果写 jit_fn_result），
                          * exit 不写回 locals（调用方用临时数组） */
} CodegenCtx;

/* ---- Platform-independent codegen helpers (shared by all backends) ----
 * Used by every backend's compile_loop (off_map / patches are generic
 * bytecode-offset <-> machine-code-offset bookkeeping, no ISA specifics). */
static inline void offmap_add(CodegenCtx* ctx, int bc_off, int mc_off) {
    if (ctx->off_count < JIT_MAX_LOOP_OPS * 2) {
        ctx->off_map[ctx->off_count].bc_off = bc_off;
        ctx->off_map[ctx->off_count].mc_off = mc_off;
        ctx->off_count++;
    }
}

static inline int offmap_lookup(CodegenCtx* ctx, int bc_off) {
    for (int i = 0; i < ctx->off_count; i++) {
        if (ctx->off_map[i].bc_off == bc_off)
            return ctx->off_map[i].mc_off;
    }
    return -1;
}

static inline void patch_add(CodegenCtx* ctx, int patch_mc, int target_bc, int vstack) {
    if (ctx->patch_count < JIT_MAX_PATCHES) {
        ctx->patches[ctx->patch_count].patch_mc = patch_mc;
        ctx->patches[ctx->patch_count].target_bc = target_bc;
        ctx->patches[ctx->patch_count].vstack = vstack;
        ctx->patch_count++;
    }
}

/* ---- Global state (defined in jit_callout.c) ---- */
extern VM* jit_callout_vm;
extern volatile int jit_callout_failed;
extern Value* jit_reloaded_locals;
extern int64_t jit_bailout_rax;
extern int32_t jit_bailout_site;

/* ---- Function-level JIT (defined in jit_callout.c) ----
 * jit_fn_result:  函数级 JIT 机器码通过 OP_RETURN 写入的返回值（单返回）。
 * jit_func_depth: 当前 JIT 函数嵌套深度（递归保护，超过上限回退解释路径）。
 * jit_func_locals_pool: 按 depth 索引的固定 locals 池（零分配快路径）。
 * 池行宽 = JIT_MAX_LOCALS(64) 槽；depth 上限受 jit_func_depth < 64 约束。 */
extern Value jit_fn_result;
extern int jit_func_depth;
#define JIT_FUNC_MAX_DEPTH 64
extern Value jit_func_locals_pool[JIT_FUNC_MAX_DEPTH][JIT_MAX_LOCALS];

/* ---- Function-level JIT cache (defined in jit.c) ---- */
#define JIT_FUNC_CACHE_SIZE 32   /* direct-mapped cache, power of 2 */
typedef struct {
    ObjFunction* func;   /* cache key: ObjFunction pointer */
    int tried;           /* 1 = compilation attempted */
    JitLoopFn fn;        /* compiled machine code (NULL if compilation failed) */
} JitFuncCacheEntry;
extern JitFuncCacheEntry jit_func_cache[JIT_FUNC_CACHE_SIZE];

/* Look up (or compile on first use) a function-level JIT entry.
 * Returns NULL if JIT disabled / unsupported / compilation failed. */
JitLoopFn jit_func_lookup_or_compile(ObjFunction* func, VM* vm_ptr);

/* ---- Runtime helpers (jit_callout.c) ---- */
int jit_debug_on(void);
void jit_ft_profile_dump(void);
void jit_bailout_debug(int64_t rsp_val);
Value jit_callout_index(Value obj_val, Value idx_val);
Value jit_callout_concat(Value a, Value b);
int jit_callout_array_append(Value arr_val, Value value);
Value jit_callout_dict_set(Value dict_val, Value key_val, Value value);
int jit_callout_index_set(Value obj_val, Value idx_val, Value value);
Value jit_callout_div(Value a, Value b);
Value jit_callout_acc_fields(Value obj_val, uint8_t count, const uint8_t* field_indices);
Value jit_callout_struct_init(int64_t* vstack_top, uint16_t name_const_idx,
                              uint8_t arg_count, const uint8_t* ip, Chunk* chunk);
Value jit_callout_invoke_method(int64_t* vstack_top, int arg_count, const uint8_t* ip, Chunk* chunk);
Value jit_callout_global_func(int64_t* vstack_top, int arg_count, uint16_t func_slot);
Value jit_callout_get_field_fast(Value obj_val, uint8_t field_idx);
Value jit_callout_module_call(int64_t* vstack_top, int arg_count, uint16_t module_idx, uint16_t method_idx, Chunk* chunk);

/* ---- 模块方法编译期解析（codegen 用）----
 * 旧实现让 callout 每次调用都做「字符串哈希 + strcmp」查模块方法表（解释器侧
 * 有 inline cache 规避，JIT 没有）。现在在 JIT 编译期解析一次，把
 * ModuleMethodMeta* 直接嵌进机器码。模块方法表在启动注册后不再变更
 * （native_reset_registry 未被调用），指针长期有效。 */
ModuleMethodMeta* jit_resolve_module_method(Chunk* chunk, uint16_t module_idx, uint16_t method_idx);

/* Callout：已解析 meta 的模块方法调用（无运行时查找） */
Value jit_callout_module_call_meta(int64_t* vstack_top, int arg_count, ModuleMethodMeta* meta);

/* ---- 通用「数值薄调用」桥（jit_callout.c）----
 * 模块方法若注册为「全部参数 + 返回值都是 float」（param_types/return_type），
 * codegen 可以把实参当 double 直接放进 xmm0..3 并调用下面的薄桥：
 *   - 桥内部仍调用该模块原本的 NativeFn（数学实现只有模块里那一份）
 *   - 省掉 callout 的 vstack 数组、逐参数装箱、模块方法查表
 *   - native 内部抛错 → 置 jit_callout_failed，codegen 检查后 bailout
 * 支持 1..4 个 double 参数（xmm0..3；首参寄存器位放桥函数指针）。 */
void* jit_thin_bridge_for(int arity);
Value jit_callout_array_new(int64_t* vstack_top, uint16_t count);
Value jit_callout_call_native(int64_t* vstack_top, ObjNative* native, uint16_t arg_count);
Value jit_callout_get_property(int64_t* vstack_top, uint16_t name_const_idx, uint16_t call_or_args, Chunk* chunk);

/* ---- Scanning (jit_scan.c) ---- */
int cache_hash(const uint8_t* ip);
int opcode_size(const uint8_t* ip);
void scan_loop_body(const uint8_t* body_start, int body_size, int back_edge, ScanResult* r, VM* vm_ptr);

/* ---- Codegen (backend/x86_64.c) ---- */
int compile_loop(CodegenCtx* ctx);

#endif /* LENO_JIT_PRIV_H */


