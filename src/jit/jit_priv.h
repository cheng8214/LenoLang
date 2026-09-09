/*
 * jit_priv.h - 内部共享声明（jit_callout.c / jit_scan.c / jit.c 共用）
 * 公共平台部分：见 jit.h（API）、jit_mem.h（可执行内存，平台分支）。
 */
#ifndef LENO_JIT_PRIV_H
#define LENO_JIT_PRIV_H

#include "jit.h"
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

/* ---- Runtime helpers (jit_callout.c) ---- */
int jit_debug_on(void);
void jit_bailout_debug(int64_t rsp_val);
Value jit_callout_index(Value obj_val, Value idx_val);
int jit_callout_array_append(Value arr_val, Value value);
Value jit_callout_dict_set(Value dict_val, Value key_val, Value value);
int jit_callout_index_set(Value obj_val, Value idx_val, Value value);
Value jit_callout_div(Value a, Value b);
Value jit_callout_acc_fields(Value obj_val, uint8_t count, const uint8_t* field_indices);
Value jit_callout_invoke_method(int64_t* vstack_top, int arg_count, const uint8_t* ip, Chunk* chunk);
Value jit_callout_global_func(int64_t* vstack_top, int arg_count, uint16_t func_slot);
Value jit_callout_get_field_fast(Value obj_val, uint8_t field_idx);
Value jit_callout_module_call(int64_t* vstack_top, int arg_count, uint16_t module_idx, uint16_t method_idx, Chunk* chunk);
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


