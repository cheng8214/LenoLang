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
 *   DIV_INT (returns float), any float/string/object operation,
 *   function calls, exceptions, etc.
 */
#include "jit.h"
#include "jit_mem.h"
#include "jit_emit.h"
#include "../include/leno_error.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ---- Global state ---- */
JitState jit_state = {0};

/* ---- Callout VM pointer (set before JIT execution) ---- */
static VM* jit_callout_vm = NULL;

/* ---- Callout helpers ---- */

/* Convert JIT virtual-stack raw value to NaN-boxed Value.
 * int48 raw → re-encode as NaN-boxed int; non-int48 → already NaN-boxed. */
static inline Value jit_raw_to_value(int64_t raw) {
    int64_t test = raw >> 47;
    test++;
    if ((uint64_t)test <= 1) {
        /* int48 → re-encode as NaN-boxed int */
        return (Value)(((uint64_t)raw & JIT_PAYLOAD_MSK) | JIT_INT_TAG);
    }
    /* Not int48 → already NaN-boxed (float or object) */
    return (Value)raw;
}

/* Convert NaN-boxed Value to JIT virtual-stack raw format.
 * NaN-boxed int → extract int48, sign-extend; non-int → raw bits. */
static inline int64_t jit_value_to_raw(Value v) {
    uint64_t bits = (uint64_t)v;
    uint16_t top16 = (uint16_t)(bits >> 48);
    if (top16 == 0xFFFB) {
        /* NaN-boxed int → extract int48, sign-extend */
        int64_t raw = (int64_t)(bits & JIT_PAYLOAD_MSK);
        raw = raw << 16 >> 16;  /* sign-extend from 48 bits */
        return raw;
    }
    /* Not int → return raw bits (float or object) */
    return (int64_t)bits;
}

/* Callout: OP_INDEX (array/dict index access).
 * Returns NaN-boxed result, or NULL_VAL on error. */
static Value jit_callout_index(Value obj_val, Value idx_val) {
    if (!val_is_obj(obj_val)) {
        error_add_at(ERR_RUNTIME, 0, 0, "索引操作需要对象类型");
        return NULL_VAL;
    }
    Object* obj = val_as_obj(obj_val);
    if (obj->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)obj;
        if (val_is_int(idx_val)) {
            int64_t index = val_as_int(idx_val);
            if ((uint64_t)index < (uint64_t)arr->count) {
                return arr->elements[index];
            }
        }
        if (!val_is_num(idx_val)) {
            error_add_at(ERR_RUNTIME, 0, 0, "数组索引必须是数字");
            return NULL_VAL;
        }
        int index = (int)value_to_double(idx_val);
        if (index < 0 || index >= arr->count) {
            char msg[256];
            snprintf(msg, sizeof(msg), "数组索引越界: 索引=%d, 数组长度=%d", index, arr->count);
            error_add_at(ERR_RUNTIME, 0, 0, msg);
            return NULL_VAL;
        }
        return arr->elements[index];
    }
    if (obj->type == OBJ_DICT) {
        ObjDict* dict = (ObjDict*)obj;
        return dict_get(dict, idx_val);
    }
    error_add_at(ERR_RUNTIME, 0, 0, "索引操作需要数组或字典");
    return NULL_VAL;
}

/* Callout: OP_ARRAY_APPEND_NOPUSH.
 * Returns 0 on success, -1 on error. */
static int jit_callout_array_append(Value arr_val, Value value) {
    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) {
        error_add_at(ERR_RUNTIME, 0, 0, "append 操作需要数组");
        return -1;
    }
    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) {
            error_add_at(ERR_RUNTIME, 0, 0, "数组扩容失败");
            return -1;
        }
    }
    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
    return 0;
}

/* Callout: OP_DICT_SET.
 * Returns dict_val on success, NULL_VAL on error. */
static Value jit_callout_dict_set(Value dict_val, Value key_val, Value value) {
    if (!val_is_obj(dict_val) || val_as_obj(dict_val)->type != OBJ_DICT) {
        error_add_at(ERR_RUNTIME, 0, 0, "赋值操作需要字典");
        return NULL_VAL;
    }
    ObjDict* dict = (ObjDict*)val_as_obj(dict_val);
    dict_set(dict, key_val, value);
    return dict_val;
}

/* ---- Cache hash ---- */
static int cache_hash(const uint8_t* ip) {
    uintptr_t v = (uintptr_t)ip;
    return (int)((v >> 4) & (JIT_CACHE_SIZE - 1));
}

/* ---- Bytecode operand readers (big-endian, matching VM) ---- */
static inline uint16_t rd_short(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline int32_t rd_int32(const uint8_t* p) {
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
static inline int8_t rd_byte(const uint8_t* p) {
    return (int8_t)p[0];
}

/* ---- Opcode instruction size (bytes) ---- */
static int opcode_size(uint8_t op) {
    switch (op) {
        /* 1-byte (no operands) */
        case OP_ZERO: case OP_ONE: case OP_POP: case OP_DUP:
        case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
        case OP_MOD_INT: case OP_NEG_INT:
        case OP_EQ_INT: case OP_LT_INT: case OP_GT_INT:
        case OP_LE_INT: case OP_GE_INT:
        case OP_CAST_INT:  /* no-op in int-only JIT context */
        case OP_DIV_INT:   /* int / int -> float, pushes float */
        case OP_BITAND: case OP_BITOR: case OP_BITXOR: case OP_BITNOT:
        case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT:
        case OP_NEG_FLOAT:
        case OP_EQ_FLOAT: case OP_LT_FLOAT: case OP_GT_FLOAT:
        case OP_LE_FLOAT: case OP_GE_FLOAT:
        case OP_CAST_FLOAT: /* int → float */
        case OP_INC:  /* ++ (stack-top) */
        case OP_DEC:  /* -- (stack-top) */
        case OP_NOT:  /* logical NOT */
        case OP_INDEX: /* array/dict index access (callout) */
        case OP_ARRAY_APPEND_NOPUSH: /* arr.add(v) statement (callout) */
        case OP_DICT_SET: /* dict[key]=val (callout) */
            return 1;
        /* 2-byte (opcode + imm8) */
        case OP_ADD_INT_IMM: case OP_SUB_INT_IMM: case OP_MUL_INT_IMM:
        case OP_LT_INT_IMM: case OP_GT_INT_IMM:
        case OP_LE_INT_IMM: case OP_GE_INT_IMM: case OP_EQ_INT_IMM:
        case OP_SHL_IMM: case OP_SHR_IMM: case OP_USHR_IMM:
            return 2;
        /* 3-byte (opcode + slot16) */
        case OP_CONST: case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP: case OP_INC_LOCAL: case OP_DEC_LOCAL:
        case OP_PRE_INC_LOCAL: case OP_PRE_DEC_LOCAL:
        case OP_INC_LOCAL_NOPUSH: case OP_DEC_LOCAL_NOPUSH:
        case OP_GET_GLOBAL: case OP_SET_GLOBAL:
            return 3;
        /* 5-byte (opcode + slot16 + slot16 or opcode + int32) */
        case OP_MOVE_LOCAL: case OP_MOVE_LOCAL_POP:
        case OP_SET_LOCAL_CONST:
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
        case OP_LOOP:
        case OP_TRY:   /* catch_offset(2) + finally_offset(2) */
            return 5;
        /* 1-byte try/catch (no operands) */
        case OP_CATCH: case OP_END_TRY:
            return 1;
        /* 7-byte (OP_FOR_LOOP) */
        case OP_FOR_LOOP:
            return 7;
        /* 8-byte (OP_FOR_PREP) — recognized so scan doesn't fail on
         * "unknown opcode"; actual nesting is rejected in scan. */
        case OP_FOR_PREP:
            return 8;
        /* 10-byte (CMPJMP variants) */
        case OP_CMPJMP_LL_INT:
            return 10;
        default:
            return -1;  /* unknown / unsupported */
    }
}

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
} ScanResult;

/* Mark a local slot as used; assign scratch index */
static void mark_local(ScanResult* r, int slot) {
    if (slot < 0 || slot >= 256) return;
    if (r->local_map[slot] >= 0) return;  /* already mapped */
    if (r->num_locals >= JIT_MAX_LOCALS) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] scan FAIL: too many locals (%d) at slot %d\n", r->num_locals, slot);
        r->capable = 0;
        return;
    }
    r->local_map[slot] = r->num_locals;
    r->local_slots[r->num_locals] = (uint8_t)slot;
    r->num_locals++;
}

/*
 * Scan the loop body to determine JIT-ability and collect metadata.
 * body_start: pointer to first bytecode instruction of the loop body
 * body_size:  total bytecode bytes (including the back-edge instruction)
 * back_edge:  1=OP_LOOP, 2=OP_FOR_LOOP
 */
static void scan_loop_body(const uint8_t* body_start, int body_size,
                           int back_edge, ScanResult* r) {
    memset(r, 0, sizeof(*r));
    r->capable = 1;
    r->back_edge_type = back_edge;
    for (int i = 0; i < 256; i++) r->local_map[i] = -1;

    const uint8_t* ip = body_start;
    const uint8_t* end = body_start + body_size;
    int vstack = 0;

    while (ip < end) {
        uint8_t op = *ip;
        int size = opcode_size(op);
        if (size < 0) {
            if (getenv("LENO_JIT_DEBUG"))
                fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d (size<0) at offset %d\n", op, (int)(ip - body_start));
            r->capable = 0;
            return;
        }
        if (ip + size > end) {
            if (getenv("LENO_JIT_DEBUG"))
                fprintf(stderr, "[JIT-DEBUG] scan FAIL: ip+size>end for opcode %d at offset %d, size=%d, remaining=%d\n", op, (int)(ip - body_start), size, (int)(end - ip));
            r->capable = 0;
            return;
        }

        switch (op) {
            /* 1-byte stack ops */
            case OP_ZERO: case OP_ONE:
                vstack++;
                break;
            case OP_POP:
                vstack--;
                break;
            case OP_DUP:
                vstack++;
                break;
            /* 1-byte arithmetic */
            case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
            case OP_MOD_INT: case OP_NEG_INT:
                vstack -= (op == OP_NEG_INT) ? 0 : 1;
                /* ADD/SUB/MUL/MOD pop 2 push 1 → net -1; NEG pop 1 push 1 → net 0 */
                break;
            case OP_CAST_INT:
                /* no-op in int-only JIT: value is already int, just pass through */
                break;
            case OP_DIV_INT:
                /* int / int → float result, pop 2 push 1 → net -1 */
                vstack -= 1;
                break;
            case OP_BITAND: case OP_BITOR: case OP_BITXOR:
                /* bitwise: pop 2 push 1 → net -1 */
                vstack -= 1;
                break;
            case OP_BITNOT:
                /* pop 1 push 1 → net 0 */
                break;
            /* 1-byte float arithmetic: pop 2 push 1 → net -1 */
            case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT:
                vstack -= 1;
                break;
            case OP_NEG_FLOAT:
                /* pop 1 push 1 → net 0 */
                break;
            case OP_EQ_FLOAT: case OP_LT_FLOAT: case OP_GT_FLOAT:
            case OP_LE_FLOAT: case OP_GE_FLOAT:
                vstack--;  /* pop 2 push 1 → net -1 */
                break;
            case OP_CAST_FLOAT:
                /* int → float, pop 1 push 1 → net 0 */
                break;
            case OP_INC: case OP_DEC:
                /* stack-top ++/--: pop 1 push 1 → net 0 */
                break;
            case OP_NOT:
                /* pop 1 push 1 → net 0 */
                break;
            case OP_INDEX:
                /* pop 2 (obj, index) push 1 → net -1 */
                vstack--;
                break;
            case OP_ARRAY_APPEND_NOPUSH:
                /* pop 2 (arr, value) → net -2 */
                vstack -= 2;
                break;
            case OP_DICT_SET:
                /* pop 3 (dict, key, value), push 1 (dict) → net -2 */
                vstack -= 2;
                break;
            case OP_TRY:
            case OP_CATCH:
            case OP_END_TRY:
                /* Exception handling involves complex VM state (exception
                 * handler stack, try/catch tables) that the JIT cannot
                 * safely replicate. Reject the entire loop. */
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: try/catch opcode %d at offset %d\n", op, (int)(ip - body_start));
                r->capable = 0;
                return;
            case OP_FOR_PREP:
                /* Nested for-loop's FOR_PREP appears in the outer loop body.
                 * The JIT cannot handle nested for loops (inner FOR_PREP
                 * initializes loop vars and may skip the inner body entirely). */
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: OP_FOR_PREP (nested for) at offset %d\n", (int)(ip - body_start));
                r->capable = 0;
                return;
            case OP_EQ_INT: case OP_LT_INT: case OP_GT_INT:
            case OP_LE_INT: case OP_GE_INT:
                vstack--;  /* pop 2 push 1 → net -1 */
                break;
            /* 2-byte immediate arith: pop 1, push 1 → net 0 */
            case OP_ADD_INT_IMM: case OP_SUB_INT_IMM: case OP_MUL_INT_IMM:
                break;
            /* 2-byte immediate compare: pop 1, push 1 → net 0 */
            case OP_LT_INT_IMM: case OP_GT_INT_IMM:
            case OP_LE_INT_IMM: case OP_GE_INT_IMM: case OP_EQ_INT_IMM:
                break;
            /* 3-byte local ops */
            case OP_CONST: {
                /* Constant index is checked at codegen time; scan just tracks stack */
                vstack++;
                break;
            }
            case OP_GET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                vstack++;
                break;
            }
            case OP_SET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                /* peek (no pop) */
                break;
            }
            case OP_SET_LOCAL_POP: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                vstack--;
                break;
            }
            case OP_INC_LOCAL: case OP_DEC_LOCAL:
            case OP_PRE_INC_LOCAL: case OP_PRE_DEC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                vstack++;  /* pushes old/new value */
                break;
            }
            case OP_INC_LOCAL_NOPUSH: case OP_DEC_LOCAL_NOPUSH: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                /* no stack change */
                break;
            }
            /* 3-byte global ops */
            case OP_GET_GLOBAL: {
                /* push globals[slot] → vstack++ */
                vstack++;
                break;
            }
            case OP_SET_GLOBAL: {
                /* peek (no pop) - write to globals[slot] */
                break;
            }
            /* 5-byte move/const */
            case OP_MOVE_LOCAL: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                mark_local(r, src);
                mark_local(r, dst);
                vstack++;
                break;
            }
            case OP_MOVE_LOCAL_POP: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                mark_local(r, src);
                mark_local(r, dst);
                break;
            }
            case OP_SET_LOCAL_CONST: {
                uint16_t slot = rd_short(ip + 3);
                mark_local(r, slot);
                break;
            }
            /* 5-byte jumps */
            case OP_JUMP:
                /* no stack change */
                break;
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
                /* peek only — VM uses vm_stack_peek_fast, explicit POP follows */
                break;
            case OP_LOOP:
                /* Back-edge: only allowed as the LAST instruction in the body.
                 * If OP_LOOP appears mid-body, it's a nested loop's back-edge —
                 * the JIT codegen would emit JMP loop_start_mc, creating a
                 * wrong jump that bypasses the outer loop's condition check. */
                if (ip + size < end) {
                    if (getenv("LENO_JIT_DEBUG"))
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: OP_LOOP mid-body at offset %d\n", (int)(ip - body_start));
                    r->capable = 0;
                    return;
                }
                break;
            /* 7-byte FOR_LOOP — back-edge: only allowed as the LAST instruction */
            case OP_FOR_LOOP: {
                if (ip + size < end) {
                    if (getenv("LENO_JIT_DEBUG"))
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: OP_FOR_LOOP mid-body at offset %d\n", (int)(ip - body_start));
                    r->capable = 0;
                    return;
                }
                r->for_loop_var_slot = ip[1];
                r->for_step_slot = ip[2];
                r->for_end_slot = ip[3];
                r->for_inclusive = ip[4];
                mark_local(r, ip[1]);
                mark_local(r, ip[2]);
                mark_local(r, ip[3]);
                break;
            }
            /* 10-byte CMPJMP */
            case OP_CMPJMP_LL_INT: {
                uint16_t sa = rd_short(ip + 2);
                uint16_t sb = rd_short(ip + 4);
                mark_local(r, sa);
                mark_local(r, sb);
                /* no stack change */
                break;
            }
            /* Shift immediates — supported, pop 1 push 1 */
            case OP_SHL_IMM: case OP_SHR_IMM: case OP_USHR_IMM:
                break;
            default:
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d at offset %d\n", op, (int)(ip - body_start));
                r->capable = 0;
                return;
        }

    if (vstack < 0 || vstack > JIT_MAX_VSTACK) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] scan FAIL: vstack=%d at opcode %d\n", vstack, op);
        r->capable = 0;
        return;
    }
        r->max_vstack = vstack > r->max_vstack ? vstack : r->max_vstack;
        ip += size;
    }

    r->body_size = body_size;
}

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
    OffMap off_map[JIT_MAX_LOOP_OPS * 2];
    int off_count;
    Patch patches[JIT_MAX_PATCHES];
    int patch_count;
    int loop_start_mc;   /* machine code offset of loop body start */
    int exit_mc;         /* machine code offset of exit code */
    int bailout_mc;      /* machine code offset of bailout code */
    const ScanResult* sr;
    const uint8_t* body_start;
    Chunk* chunk;        /* for constant table access */
} CodegenCtx;

static void offmap_add(CodegenCtx* ctx, int bc_off, int mc_off) {
    if (ctx->off_count < JIT_MAX_LOOP_OPS * 2) {
        ctx->off_map[ctx->off_count].bc_off = bc_off;
        ctx->off_map[ctx->off_count].mc_off = mc_off;
        ctx->off_count++;
    }
}

static int offmap_lookup(CodegenCtx* ctx, int bc_off) {
    for (int i = 0; i < ctx->off_count; i++) {
        if (ctx->off_map[i].bc_off == bc_off)
            return ctx->off_map[i].mc_off;
    }
    return -1;
}

static void patch_add(CodegenCtx* ctx, int patch_mc, int target_bc, int vstack) {
    if (ctx->patch_count < JIT_MAX_PATCHES) {
        ctx->patches[ctx->patch_count].patch_mc = patch_mc;
        ctx->patches[ctx->patch_count].target_bc = target_bc;
        ctx->patches[ctx->patch_count].vstack = vstack;
        ctx->patch_count++;
    }
}

/*
 * Scratch area offset: scratch[i] is at [RBP - 8*(i+1)]
 * Returns the displacement for emit_mov_reg_mem8/32
 */
static int scratch_disp(int scratch_idx) {
    return -8 * (scratch_idx + 1);
}

/* ---- Main codegen function ---- */
static int compile_loop(CodegenCtx* ctx) {
    const ScanResult* sr = ctx->sr;
    CodeBuf* cb = &ctx->cb;
    int n = sr->num_locals;

    /*
     * int48 overflow check: bail out to VM (which handles BigInt promotion)
     * if RAX doesn't fit in signed 48-bit range [-2^47, 2^47-1].
     *
     * sar r8, 47 yields 0 for valid positive, -1 for valid negative.
     * inc r8 maps 0→1, -1→0 (both ≤ 1 unsigned), anything else → > 1.
     * ja bailout catches the "anything else" case.
     */
    #define EMIT_INT48_CHECK() do { \
        emit_mov_rr(cb, JIT_R8, JIT_RAX);  \
        emit_sar_imm(cb, JIT_R8, 47);     \
        emit_inc_reg(cb, JIT_R8);          \
        emit_cmp_reg_imm8(cb, JIT_R8, 1);  \
        int _p = emit_jcc(cb, 0x87);      \
        patch_add(ctx, _p, -1, 0);          \
    } while(0)

    /* int64 overflow check for MUL: bail out if OF flag set */
    #define EMIT_INT64_OVF_CHECK() do { \
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

    /* Save volatile state, align RSP, allocate shadow space */
    #define EMIT_CALLOUT_BEGIN() do { \
        emit_mov_rr(cb, JIT_R12, JIT_RSP); \
        emit_mov_rr(cb, JIT_R13, JIT_RCX); \
        emit_mov_rr(cb, JIT_R14, JIT_R9); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xE4); emit_byte(cb, 0xF0); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xEC); emit_byte(cb, 0x20); \
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

    /* Callout temp slot offsets (3 slots below virtual stack area) */
    int tmp1_disp = -8 * (n + sr->max_vstack + 1);
    int tmp2_disp = -8 * (n + sr->max_vstack + 2);
    int tmp3_disp = -8 * (n + sr->max_vstack + 3);

    /* ---- Function prologue ---- */
    emit_push_rbp(cb);                        /* push rbp          */
    emit_push_reg(cb, JIT_RBX);              /* push rbx (type bitmap, callee-saved) */
    emit_push_reg(cb, JIT_R12);              /* push r12 (callout: saved RSP) */
    emit_push_reg(cb, JIT_R13);              /* push r13 (callout: saved RCX=locals) */
    emit_push_reg(cb, JIT_R14);              /* push r14 (callout: saved R9=globals) */
    emit_mov_rbp_rsp(cb);                     /* mov rbp, rsp      */
    /* Allocate: scratch area (n*8) + max_vstack*8 + callout temps (3*8), rounded to 16 */
    int frame_sz = n * 8 + sr->max_vstack * 8 + 16 + 24;  /* +24 for callout temps */
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

    /* ---- Step direction check (for OP_FOR_LOOP) ---- */
    if (sr->back_edge_type == 2) {
        int step_scratch = sr->local_map[sr->for_step_slot];
        if (step_scratch < 0) {
            if (getenv("LENO_JIT_DEBUG"))
                fprintf(stderr, "[JIT-DEBUG] codegen FAIL: FOR_LOOP step_scratch<0\n");
            return 0;  /* shouldn't happen */
        }
        int disp = scratch_disp(step_scratch);
        /* Load step: mov rax, [rbp + disp] */
        if (disp >= -128 && disp <= 127) {
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
        } else {
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
        }
        /* Test: cmp rax, 0; jle bailout */
        /* test rax, rax */
        emit_test_rr(cb, JIT_RAX, JIT_RAX);
        /* jle bailout (0x8E = JLE) */
        int patch = emit_jcc(cb, 0x8E);
        patch_add(ctx, patch, -1, 0);  /* bailout */
    }

    /* ---- FOR_LOOP first-iteration increment + check ---- */
    /* The JIT is triggered at OP_FOR_LOOP BEFORE the interpreter does the increment.
     * So the JIT must do the increment itself on the first iteration to avoid
     * re-running the body with the same loop_var value the interpreter already used.
     */
    int for_loop_entry_patch = -1;  /* patch for "exit if loop done" */
    if (sr->back_edge_type == 2) {
        int si_lv = sr->local_map[sr->for_loop_var_slot];
        int si_st = sr->local_map[sr->for_step_slot];
        int si_en = sr->local_map[sr->for_end_slot];
        int d_lv = scratch_disp(si_lv);
        int d_st = scratch_disp(si_st);
        int d_en = scratch_disp(si_en);

        /* mov rax, [rbp+d_lv] (loop_var) */
        if (d_lv >= -128 && d_lv <= 127)
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv);
        else
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv);
        /* mov rdx, [rbp+d_st] (step) */
        if (d_st >= -128 && d_st <= 127)
            emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st);
        else
            emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st);
        /* add rax, rdx (loop_var += step) */
        emit_add_rr(cb, JIT_RAX, JIT_RDX);
        EMIT_INT48_CHECK();
        /* mov [rbp+d_lv], rax (store back) */
        if (d_lv >= -128 && d_lv <= 127)
            emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
        else
            emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);
        /* mov rdx, [rbp+d_en] (end) */
        if (d_en >= -128 && d_en <= 127)
            emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en);
        else
            emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en);
        /* cmp rax, rdx */
        emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
        /* If condition NOT met, jump to exit (loop is done) */
        /* step > 0, exclusive: JGE (loop_var >= end) → exit */
        /* step > 0, inclusive: JG  (loop_var > end) → exit  */
        uint8_t cc_exit = sr->for_inclusive ? 0x8F /*JG*/ : 0x8D /*JGE*/;
        for_loop_entry_patch = emit_jcc(cb, cc_exit);
        /* Will be patched to exit_mc later */
    }

    /* ---- Loop body start ---- */
    ctx->loop_start_mc = cb->len;

    /* ---- Generate loop body code ---- */
    const uint8_t* ip = ctx->body_start;
    const uint8_t* end = ctx->body_start + sr->body_size;
    int bc_off = 0;
    int vstack = 0;
    int tos_live = 0;  /* TOS register cache: 1=RAX holds TOS, 0=all on memory stack */

    while (ip < end) {
        uint8_t op = *ip;
        int size = opcode_size(op);
        offmap_add(ctx, bc_off, cb->len);

        switch (op) {
            /* ---- Stack ops ---- */
            case OP_ZERO:
                TOS_SPILL();
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);  /* xor rax, rax → 0 */
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_ONE:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, 1);
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_POP:
                TOS_DISCARD();
                vstack--;
                break;
            case OP_DUP:
                if (tos_live) {
                    /* RAX has TOS, push a copy to stack; RAX stays as TOS */
                    emit_push_reg(cb, JIT_RAX);
                } else {
                    /* Peek TOS from stack into RAX, mark as live */
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RSP, 0);
                    TOS_PRODUCE();
                }
                vstack++;
                break;

            /* ---- Local variable ops ---- */
            case OP_GET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_SET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                /* peek TOS → store to local */
                TOS_PEEK_TO(JIT_RAX);
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_SET_LOCAL_POP: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                /* consume TOS → store to local */
                TOS_CONSUME_RAX();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                vstack--;
                break;
            }
            case OP_MOVE_LOCAL: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                int si_src = sr->local_map[src];
                int si_dst = sr->local_map[dst];
                int d_src = scratch_disp(si_src);
                int d_dst = scratch_disp(si_dst);
                TOS_SPILL();
                if (d_src >= -128 && d_src <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_src);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_src);
                }
                if (d_dst >= -128 && d_dst <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_dst, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, d_dst, JIT_RAX);
                }
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_MOVE_LOCAL_POP: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                int si_src = sr->local_map[src];
                int si_dst = sr->local_map[dst];
                int d_src = scratch_disp(si_src);
                int d_dst = scratch_disp(si_dst);
                /* No stack effect (pop + local op), but clobbers RAX */
                TOS_SPILL();
                if (d_src >= -128 && d_src <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_src);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_src);
                }
                if (d_dst >= -128 && d_dst <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_dst, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, d_dst, JIT_RAX);
                }
                break;
            }
            case OP_SET_LOCAL_CONST: {
                uint16_t ci = rd_short(ip + 1);
                uint16_t slot = rd_short(ip + 3);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                Value cv = ctx->chunk->constants[ci];
                TOS_SPILL();  /* clobbers RAX */
                if (val_is_int(cv)) {
                    int64_t iv = val_as_int(cv);
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)iv);
                } else if (val_is_float(cv)) {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                } else {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x0F);
                    emit_byte(cb, 0xBA);
                    emit_byte(cb, 0xEB);
                    emit_byte(cb, (uint8_t)si);
                }
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_CONST: {
                uint16_t ci = rd_short(ip + 1);
                Value cv = ctx->chunk->constants[ci];
                TOS_SPILL();
                if (val_is_int(cv)) {
                    int64_t iv = val_as_int(cv);
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)iv);
                    TOS_PRODUCE();
                } else if (val_is_float(cv)) {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    TOS_PRODUCE();
                } else {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    TOS_PRODUCE();
                }
                vstack++;
                break;
            }

            /* ---- Global variable ops ---- */
            /* R9 = globals pointer (saved from RDX at prologue) */
            /* Globals store NaN-boxed Values; JIT virtual stack holds raw int64. */
            /* OP_GET_GLOBAL: load Value, type-guard (int OR float), extract, push raw */
            case OP_GET_GLOBAL: {
                uint16_t slot = rd_short(ip + 1);
                int gd = slot * 8;
                TOS_SPILL();  /* clobbers RAX, R8 */
                /* Load NaN-boxed Value: mov rax, [r9 + gd] */
                if (gd >= -128 && gd <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_R9, (int8_t)gd);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_R9, gd);
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
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));  /* /7 = CMP */
                    emit_uint32(cb, 0x0000FFFB);
                }
                /* je .is_int (rel8 placeholder) */
                emit_byte(cb, 0x74);
                int isint_patch = cb->len;
                emit_byte(cb, 0x00);

                /* Check non-number: cmp r8, 0xFFF8; jae bailout */
                {
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFF8);
                }
                {
                    int p = emit_jcc(cb, 0x83);  /* JAE → bailout */
                    patch_add(ctx, p, -1, 0);
                }

                /* Float path: RAX already has raw double bits */
                TOS_PRODUCE();
                /* jmp .next (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);

                /* .is_int: patch je to here */
                cb->buf[isint_patch] = (uint8_t)(cb->len - (isint_patch + 1));
                /* Int path: extract int48 */
                emit_and_rr(cb, JIT_RAX, JIT_R10);
                emit_shl_imm(cb, JIT_RAX, 16);
                emit_sar_imm(cb, JIT_RAX, 16);
                TOS_PRODUCE();

                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                vstack++;
                break;
            }
            /* OP_SET_GLOBAL: peek raw int64, runtime type dispatch:
             * - If value fits int48 → re-encode as NaN-boxed int
             * - If not (likely float) → store raw double bits directly */
            case OP_SET_GLOBAL: {
                uint16_t slot = rd_short(ip + 1);
                int gd = slot * 8;
                /* peek TOS → RAX (clobbers RAX, R8) */
                TOS_PEEK_TO(JIT_RAX);
                /* Check if value fits int48: sar r8, 47; inc r8; cmp r8, 1; ja .is_float */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .is_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: re-encode as NaN-boxed int */
                emit_and_rr(cb, JIT_RAX, JIT_R10);
                emit_or_rr(cb, JIT_RAX, JIT_R11);
                if (gd >= -128 && gd <= 127)
                    emit_mov_mem8_reg(cb, JIT_R9, (int8_t)gd, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_R9, gd, JIT_RAX);
                /* jmp .done (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int done_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here — reload TOS and store raw bits */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                TOS_PEEK_TO(JIT_RAX);  /* reload raw value */
                if (gd >= -128 && gd <= 127)
                    emit_mov_mem8_reg(cb, JIT_R9, (int8_t)gd, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_R9, gd, JIT_RAX);
                /* .done: patch jmp to here */
                cb->buf[done_patch] = (uint8_t)(cb->len - (done_patch + 1));
                break;
            }

            /* ---- Arithmetic ---- */
            case OP_ADD_INT:
                TOS_CONSUME_TO(JIT_RDX);   /* right → RDX */
                TOS_CONSUME_TO(JIT_RAX);   /* left → RAX */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MUL_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK();
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MOD_INT:
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                TOS_PRODUCE_FROM_RDX();
                vstack--;
                break;
            case OP_NEG_INT:
                TOS_CONSUME_RAX();
                emit_neg_reg(cb, JIT_RAX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;
            case OP_CAST_INT:
                /* no-op: value is already int in JIT context */
                break;

            /* ---- Extra integer ops ---- */
            case OP_DIV_INT: {
                /* int / int -> float result (pushed as raw double bits) */
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                /* Convert quotient (int64 in RAX) to double in XMM0 */
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E);
                emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_BITAND:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_and_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_or_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITXOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_xor_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITNOT:
                TOS_CONSUME_RAX();
                {
                    int b = 0;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xF7);
                    emit_byte(cb, modrm(3, 2, JIT_RAX & 7));  /* /2 = NOT */
                }
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;

            /* ---- Float arithmetic ---- */
            /* Float values on the virtual stack are stored as raw double bits in int64 slots. */
            /* We load them into XMM registers, do SSE2 arithmetic, store back. */
            case OP_ADD_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);   /* b (raw double bits) */
                TOS_CONSUME_TO(JIT_RDX);   /* a */
                /* movq xmm0, rdx; movq xmm1, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x58, 0, 1);  /* ADDSD xmm0, xmm1 → xmm0 = a + b */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x5C, 0, 1);  /* SUBSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MUL_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x59, 0, 1);  /* MULSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_DIV_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x5E, 0, 1);  /* DIVSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_NEG_FLOAT:
                TOS_CONSUME_RAX();
                /* movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* Load sign mask into xmm1: mov rdx, 0x8000000000000000; movq xmm1, rdx */
                emit_mov_reg_imm64(cb, JIT_RDX, 0x8000000000000000ULL);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RDX & 7));
                emit_xorpd_xmm_xmm(cb, 0, 1);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                break;
            case OP_CAST_FLOAT: {
                /* Runtime type dispatch:
                 * - If value fits int48 → it's an int, convert via CVTSI2SD
                 * - If not (likely float) → already float, pass through as-is */
                TOS_CONSUME_RAX();
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .already_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: convert int64 to double */
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                /* .already_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                TOS_PRODUCE();
                break;
            }

            /* ---- Stack-top increment/decrement (int or float dispatch) ---- */
            case OP_INC: case OP_DEC: {
                int is_inc = (op == OP_INC);
                TOS_CONSUME_RAX();
                /* Type dispatch: check if int48 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .is_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: add/sub 1 */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, is_inc ? 0xC0 : 0xE8);
                emit_byte(cb, 0x01);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                /* jmp .next (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                /* Float path: movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* Load 1.0 or -1.0 into xmm1 */
                emit_mov_reg_imm64(cb, JIT_RDX, is_inc ? 0x3FF0000000000000ULL : 0xBFF0000000000000ULL);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RDX & 7));
                /* addsd xmm0, xmm1 (0x58) or subsd (0x5C) */
                emit_sse2_rr(cb, is_inc ? 0x58 : 0x5C, 0, 1);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                break;
            }
            /* ---- Logical NOT (int or float dispatch) ---- */
            case OP_NOT: {
                TOS_CONSUME_RAX();
                /* Type dispatch: check if int48 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .is_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: test rax, rax; sete al; movzx eax, al */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                emit_byte(cb, 0x0F); emit_byte(cb, 0x94); emit_byte(cb, modrm(3, 0, JIT_RAX & 7)); /* sete al */
                emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xB6); emit_byte(cb, modrm(3, JIT_RAX & 7, JIT_RAX & 7)); /* movzx rax, al */
                TOS_PRODUCE();
                /* jmp .next */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                /* Float path: movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* xorpd xmm1, xmm1 (zero) */
                emit_byte(cb, 0x66); emit_byte(cb, 0x0F); emit_byte(cb, 0x57); emit_byte(cb, modrm(3, 1, 1));
                /* ucomisd xmm0, xmm1 → ZF=1 if equal */
                emit_ucomisd_rr(cb, 0, 1);
                /* sete al; movzx rax, al */
                emit_byte(cb, 0x0F); emit_byte(cb, 0x94); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xB6); emit_byte(cb, modrm(3, JIT_RAX & 7, JIT_RAX & 7));
                TOS_PRODUCE();
                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                break;
            }
            /* ---- Callout: OP_INDEX (array/dict index access) ---- */
            case OP_INDEX: {
                /* Pop index, convert to NaN-boxed, save to tmp1 */
                TOS_SPILL();  /* ensure all values on memory stack for callout */
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                /* Pop obj, convert to NaN-boxed → RDX */
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                /* Load index from tmp1 → R8 */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                /* Callout: RCX=obj, RDX=index */
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_RCX, JIT_RDX);
                emit_mov_rr(cb, JIT_RDX, JIT_R8);
                EMIT_CALL(jit_callout_index);
                EMIT_CALLOUT_END();
                /* Convert result */
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- Callout: OP_ARRAY_APPEND_NOPUSH ---- */
            case OP_ARRAY_APPEND_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_RCX, JIT_RDX);
                emit_mov_rr(cb, JIT_RDX, JIT_R8);
                EMIT_CALL(jit_callout_array_append);
                EMIT_CALLOUT_END();
                vstack -= 2;
                break;
            }
            /* ---- Callout: OP_DICT_SET (dict[key]=val, returns dict) ---- */
            case OP_DICT_SET: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RCX, tmp3_disp);
                EMIT_LOAD_TMP(JIT_RDX, tmp2_disp);
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALL(jit_callout_dict_set);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= 2;
                break;
            }
            case OP_EQ_FLOAT: case OP_LT_FLOAT: case OP_GT_FLOAT:
            case OP_LE_FLOAT: case OP_GE_FLOAT: {
                TOS_CONSUME_TO(JIT_RAX);  /* b */
                TOS_CONSUME_TO(JIT_RDX);  /* a */
                /* movq xmm0, rdx; movq xmm1, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                /* UCOMISD xmm0, xmm1 → sets EFLAGS for xmm0 OP xmm1 */
                emit_ucomisd_rr(cb, 0, 1);
                /* Need to handle unordered (NaN) case: PF=1 means unordered */
                /* For simplicity, if NaN, bail out (jp bailout) */
                {
                    int jp_patch = emit_jcc(cb, 0x8A);  /* JP (unordered/NaN) → bailout */
                    patch_add(ctx, jp_patch, -1, 0);
                }
                /* SETcc based on comparison type.
                 * UCOMISD sets CF/ZF/PF but clears SF/OF, so we MUST use
                 * unsigned condition codes (CF-based), NOT signed (SF/OF-based). */
                uint8_t cc;
                switch (op) {
                    case OP_EQ_FLOAT: cc = 0x4; break;  /* ZF=1 → equal */
                    case OP_LT_FLOAT: cc = 0x2; break;  /* CF=1 → below (unsigned less) */
                    case OP_GT_FLOAT: cc = 0x7; break;  /* CF=0 and ZF=0 → above (unsigned greater) */
                    case OP_LE_FLOAT: cc = 0x6; break;  /* CF=1 or ZF=1 → below or equal */
                    case OP_GE_FLOAT: cc = 0x3; break;  /* CF=0 → above or equal */
                    default: cc = 0x4; break;
                }
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_ADD_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* add rax, imm8 (sign-extended) */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* /0 = ADD */
                emit_byte(cb, (uint8_t)imm);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;
            }
            case OP_SUB_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* /5 = SUB */
                emit_byte(cb, (uint8_t)imm);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;
            }
            case OP_MUL_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)(int64_t)imm);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK();
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;
            }

            /* ---- Inc/Dec locals ---- */
            case OP_INC_LOCAL_NOPUSH: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                /* mov rax, [rbp+disp]; add rax, 1; mov [rbp+disp], rax */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_DEC_LOCAL_NOPUSH: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_INC_LOCAL: {
                /* push old value, then inc local */
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                /* Save old value in R8 before incrementing */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                /* RAX = old value (from R8) */
                emit_mov_rr(cb, JIT_RAX, JIT_R8);
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_DEC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                /* Save old value in R8 before decrementing */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                emit_mov_rr(cb, JIT_RAX, JIT_R8);  /* old value */
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_PRE_INC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                TOS_PRODUCE();  /* RAX = new value */
                vstack++;
                break;
            }
            case OP_PRE_DEC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = sr->local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                TOS_PRODUCE();  /* RAX = new value */
                vstack++;
                break;
            }

            /* ---- Comparisons (push 0/1) ---- */
            case OP_EQ_INT: case OP_LT_INT: case OP_GT_INT:
            case OP_LE_INT: case OP_GE_INT: {
                uint8_t cc;
                switch (op) {
                    case OP_EQ_INT: cc = 0x4; break;  /* JE  */
                    case OP_LT_INT: cc = 0xC; break;  /* JL  */
                    case OP_GT_INT: cc = 0xF; break;  /* JG  */
                    case OP_LE_INT: cc = 0xE; break;  /* JLE */
                    case OP_GE_INT: cc = 0xD; break;  /* JGE */
                    default: cc = 0x4; break;
                }
                TOS_CONSUME_TO(JIT_RDX);  /* b */
                TOS_CONSUME_RAX();         /* a */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                vstack--;
                break;
            }

            /* ---- Immediate comparisons ---- */
            case OP_LT_INT_IMM: case OP_GT_INT_IMM:
            case OP_LE_INT_IMM: case OP_GE_INT_IMM: case OP_EQ_INT_IMM: {
                uint8_t cc;
                switch (op) {
                    case OP_LT_INT_IMM: cc = 0xC; break;
                    case OP_GT_INT_IMM: cc = 0xF; break;
                    case OP_LE_INT_IMM: cc = 0xE; break;
                    case OP_GE_INT_IMM: cc = 0xD; break;
                    case OP_EQ_INT_IMM: cc = 0x4; break;
                    default: cc = 0x4; break;
                }
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* cmp rax, imm8 (sign-extended) */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 7, JIT_RAX & 7));  /* /7 = CMP */
                emit_byte(cb, (uint8_t)imm);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                break;
            }

            /* ---- Shift immediates ---- */
            case OP_SHL_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_shl_imm(cb, JIT_RAX, (uint8_t)imm);
                TOS_PRODUCE();
                break;
            }
            case OP_SHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_sar_imm(cb, JIT_RAX, (uint8_t)imm);
                TOS_PRODUCE();
                break;
            }
            case OP_USHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* SHR rax, imm (unsigned right shift) */
                {
                    int b = (JIT_RAX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* /5 = SHR */
                    emit_byte(cb, (uint8_t)imm);
                }
                TOS_PRODUCE();
                break;
            }

            /* ---- Jumps ---- */
            case OP_JUMP: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                TOS_SPILL();  /* jump targets expect tos_live=0 */
                int patch = emit_jmp(cb);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }
            case OP_JUMP_IF_FALSE: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                if (tos_live) {
                    /* RAX already holds the condition value */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jnz_patch = emit_jcc(cb, 0x85);  /* JNZ = skip if true */
                    /* Jump path: condition is false, TOS abandoned in RAX (zero-cost) */
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    patch_rel32(cb, jnz_patch, cb->len);
                    /* Fall-through: tos_live stays 1, next OP_POP will TOS_DISCARD */
                } else {
                    /* Value on memory stack: peek + conditional pop + jump */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jnz_patch = emit_jcc(cb, 0x85);
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x83);
                    emit_byte(cb, 0xC4);
                    emit_byte(cb, 8);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    patch_rel32(cb, jnz_patch, cb->len);
                }
                break;
            }
            case OP_JUMP_IF_TRUE: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                if (tos_live) {
                    /* RAX already holds the condition value */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);  /* JZ = skip if false */
                    /* Jump path: condition is true, TOS abandoned in RAX (zero-cost) */
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    patch_rel32(cb, jz_patch, cb->len);
                    /* Fall-through: tos_live stays 1 */
                } else {
                    /* Value on memory stack: peek + conditional pop + jump */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x83);
                    emit_byte(cb, 0xC4);
                    emit_byte(cb, 8);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    patch_rel32(cb, jz_patch, cb->len);
                }
                break;
            }

            /* ---- Fused compare + jump ---- */
            case OP_CMPJMP_LL_INT: {
                uint8_t cmp_op = ip[1];
                uint16_t sa = rd_short(ip + 2);
                uint16_t sb = rd_short(ip + 4);
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                int si_a = sr->local_map[sa];
                int si_b = sr->local_map[sb];
                int da = scratch_disp(si_a);
                int db = scratch_disp(si_b);
                TOS_SPILL();  /* clobbers RAX/RDX */
                /* mov rax, [rbp+da]; mov rdx, [rbp+db]; cmp rax, rdx */
                if (da >= -128 && da <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)da);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, da);
                }
                if (db >= -128 && db <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)db);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, db);
                }
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Jump if comparison is FALSE (i.e., NOT the condition) */
                /* cmp_op: 0=EQ,1=NE,2=LT,3=GT,4=LE,5=GE */
                /* We need: jump if NOT condition → invert the condition code */
                uint8_t cc;
                switch (cmp_op) {
                    case 0: cc = 0x85; break;  /* EQ → JNE (jump if not eq) */
                    case 1: cc = 0x84; break;  /* NE → JE  (jump if not ne = eq) */
                    case 2: cc = 0x8D; break;  /* LT → JGE (jump if not lt) */
                    case 3: cc = 0x8E; break;  /* GT → JLE (jump if not gt) */
                    case 4: cc = 0x8F; break;  /* LE → JG  (jump if not le) */
                    case 5: cc = 0x8C; break;  /* GE → JL  (jump if not ge) */
                    default: cc = 0x85; break;
                }
                int patch = emit_jcc(cb, cc);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }

            /* ---- Back-edge: OP_LOOP ---- */
            case OP_LOOP: {
                /* Unconditional jump to loop start */
                TOS_SPILL();  /* back-edge: loop entry expects tos_live=0 */
                int patch = emit_jmp(cb);
                /* Target is loop_start_mc */
                patch_rel32(cb, patch, ctx->loop_start_mc);
                break;
            }

            /* ---- Back-edge: OP_FOR_LOOP ---- */
            case OP_FOR_LOOP: {
                /* Increment loop_var by step, compare with end, conditional jump */
                TOS_SPILL();  /* back-edge: clobbers RAX/RDX, loop entry expects tos_live=0 */
                int si_lv = sr->local_map[sr->for_loop_var_slot];
                int si_st = sr->local_map[sr->for_step_slot];
                int si_en = sr->local_map[sr->for_end_slot];
                int d_lv = scratch_disp(si_lv);
                int d_st = scratch_disp(si_st);
                int d_en = scratch_disp(si_en);

                /* mov rax, [rbp+d_lv] (loop_var) */
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv);
                /* mov rdx, [rbp+d_st] (step) */
                if (d_st >= -128 && d_st <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st);
                /* add rax, rdx (loop_var += step) */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                /* mov [rbp+d_lv], rax (store back) */
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);
                /* mov rdx, [rbp+d_en] (end) */
                if (d_en >= -128 && d_en <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en);
                /* cmp rax, rdx */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Conditional jump to loop start:
                 * step > 0, exclusive: JL  (loop_var < end)
                 * step > 0, inclusive: JLE (loop_var <= end)
                 */
                uint8_t cc = sr->for_inclusive ? 0x8E /*JLE*/ : 0x8C /*JL*/;
                int patch = emit_jcc(cb, cc);
                patch_rel32(cb, patch, ctx->loop_start_mc);
                break;
            }

            default:
                /* Should not reach here if scan passed */
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] codegen FAIL: unsupported opcode %d at bc_off=%d\n", op, bc_off);
                return 0;
        }

        ip += size;
        bc_off += size;
    }

    /* ---- Exit code ---- */
    ctx->exit_mc = cb->len;

    /* Patch FOR_LOOP entry "exit if done" jump to here */
    if (for_loop_entry_patch >= 0) {
        patch_rel32(cb, for_loop_entry_patch, ctx->exit_mc);
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

    /* Write back all locals (type-aware via RBX bitmap) */
    for (int i = 0; i < n; i++) {
        int slot = sr->local_slots[i];
        int disp = scratch_disp(i);
        int sd = slot * 8;
        /* BT RBX, i → CF = bit i (48 0F BA E3 imm8) */
        emit_byte(cb, 0x48);
        emit_byte(cb, 0x0F);
        emit_byte(cb, 0xBA);
        emit_byte(cb, 0xE3);  /* ModRM(11, 4, 3) = BT RBX, imm8 */
        emit_byte(cb, (uint8_t)i);
        /* jc .float_wb (rel8 placeholder) */
        emit_byte(cb, 0x72);
        int flt_patch = cb->len;
        emit_byte(cb, 0x00);
        /* Int path: load scratch, re-encode as NaN-boxed int */
        if (disp >= -128 && disp <= 127)
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
        else
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
        emit_and_rr(cb, JIT_RAX, JIT_R10);
        emit_or_rr(cb, JIT_RAX, JIT_R11);
        if (sd >= -128 && sd <= 127)
            emit_mov_mem8_reg(cb, JIT_RCX, (int8_t)sd, JIT_RAX);
        else
            emit_mov_mem32_reg(cb, JIT_RCX, sd, JIT_RAX);
        /* jmp .next (rel8 placeholder) */
        emit_byte(cb, 0xEB);
        int next_patch = cb->len;
        emit_byte(cb, 0x00);
        /* .float_wb: patch jc to here */
        cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
        /* Float path: load raw double bits, store directly (no re-encode) */
        if (disp >= -128 && disp <= 127)
            emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
        else
            emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
        if (sd >= -128 && sd <= 127)
            emit_mov_mem8_reg(cb, JIT_RCX, (int8_t)sd, JIT_RAX);
        else
            emit_mov_mem32_reg(cb, JIT_RCX, sd, JIT_RAX);
        /* .next: patch jmp to here */
        cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
    }

    /* Return 0 (success) */
    emit_xor_eax_eax(cb);
    /* Epilogue: mov rsp, rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret
     * (cannot use LEAVE because we pushed R12/R13/R14/RBX after RBP) */
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP);  /* mov rsp, rbp */
    emit_pop_reg(cb, JIT_R14);
    emit_pop_reg(cb, JIT_R13);
    emit_pop_reg(cb, JIT_R12);
    emit_pop_reg(cb, JIT_RBX);
    emit_pop_rbp(cb);
    emit_ret(cb);

    /* ---- Bailout code ---- */
    ctx->bailout_mc = cb->len;
    /* Return 1 (bailout) — locals not written back (VM re-executes from back-edge) */
    emit_mov_eax_imm32(cb, 1);
    /* Epilogue: mov rsp, rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret */
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP);
    emit_pop_reg(cb, JIT_R14);
    emit_pop_reg(cb, JIT_R13);
    emit_pop_reg(cb, JIT_R12);
    emit_pop_reg(cb, JIT_RBX);
    emit_pop_rbp(cb);
    emit_ret(cb);

    /* ---- Patch all jumps ---- */
    for (int i = 0; i < ctx->patch_count; i++) {
        Patch* p = &ctx->patches[i];
        if (p->target_bc == -1) {
            /* Bailout target */
            patch_rel32(cb, p->patch_mc, ctx->bailout_mc);
        } else if (p->target_bc >= 0 && p->target_bc < sr->body_size) {
            /* Intra-body jump */
            int target_mc = offmap_lookup(ctx, p->target_bc);
            if (target_mc >= 0) {
                /* Clean up virtual stack if needed */
                if (p->vstack > 0) {
                    /* Insert stack cleanup before the jump target?
                     * No — we already emitted the jump. We need to fix this.
                     * For the MVP, we'll skip stack cleanup on intra-body jumps.
                     * This is correct for well-formed bytecode where the stack
                     * is balanced at all jump targets. */
                }
                patch_rel32(cb, p->patch_mc, target_mc);
            } else {
                /* Target not found — redirect to exit */
                patch_rel32(cb, p->patch_mc, ctx->exit_mc);
            }
        } else {
            /* Jump target outside loop body → exit */
            patch_rel32(cb, p->patch_mc, ctx->exit_mc);
        }
    }

    return 1;  /* success */
}

/* ---- Compile a loop and cache the result ---- */
static JitLoopFn jit_compile(CallFrame* frame, const uint8_t* body_start,
                             int body_size, int back_edge) {
    ScanResult sr;
    scan_loop_body(body_start, body_size, back_edge, &sr);
    if (!sr.capable) {
        return NULL;
    }

    CodegenCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    codebuf_init(&ctx.cb, 512);
    ctx.sr = &sr;
    ctx.body_start = body_start;
    ctx.chunk = frame->chunk;

    int ok = compile_loop(&ctx);

    if (!ok || ctx.cb.len == 0) {
        if (getenv("LENO_JIT_DEBUG")) {
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
                int sz = opcode_size(*p);
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
    memset(&jit_state, 0, sizeof(jit_state));
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
        entry->fn = jit_compile(frame, body_start, body_size, back_edge);
        entry->is_compiled = (entry->fn != NULL);
        jit_state.compile_count++;
        if (getenv("LENO_JIT_DEBUG") && !entry->is_compiled)
            fprintf(stderr, "[JIT-DEBUG] compile FAIL at body_start=%d, back_edge=%d\n",
                    (int)(body_start - frame->chunk->code), back_edge);
    }

    if (!entry->is_compiled || !entry->fn) {
        return 0;
    }

    /* Execute JIT */
    jit_state.execute_count++;
    jit_callout_vm = vm_ptr;  /* set global VM pointer for callouts */
    int result = entry->fn(frame->locals, vm_ptr->globals);

    if (result == 0) {
        /* Success — frame->ip is already past the back-edge */
        return 1;
    } else {
        /* Bailout — let interpreter handle it */
        entry->bailout_count++;
        jit_state.bailout_count++;
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] BAILOUT at body_start=%d, count=%d\n",
                    (int)(body_start - frame->chunk->code), entry->bailout_count);
        return 0;
    }
}

void jit_print_stats(void) {
    fprintf(stderr, "=== JIT Statistics ===\n");
    fprintf(stderr, "  Compiled: %d\n", jit_state.compile_count);
    fprintf(stderr, "  Executed: %d\n", jit_state.execute_count);
    fprintf(stderr, "  Bailouts: %d\n", jit_state.bailout_count);
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
