/*
 * jit_scan.c - 热循环扫描与内联分析：cache/opcode 尺寸、scan_loop_body
 *
 * Split from the original monolithic jit.c (same behavior, no logic change).
 */

#include "jit.h"
#include "jit_mem.h"
#include "../include/leno_error.h"
#include "../include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jit_priv.h"

/* ---- Cache hash ---- */
int cache_hash(const uint8_t* ip) {
    uintptr_t v = (uintptr_t)ip;
    return (int)((v >> 4) & (JIT_CACHE_SIZE - 1));
}

/* ---- Opcode instruction size (bytes) ---- */
/* Takes ip (pointer to opcode byte) because some opcodes are variable-length
 * (e.g. OP_ACC_FIELDS has size 2 + count). */
int opcode_size(const uint8_t* ip) {
    uint8_t op = *ip;
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
        case OP_DIV:  /* 通用除法，运行时类型分发 (callout) */
        case OP_ADD:  /* 通用加法（int fast path，其他类型 callout） */
        case OP_SUB:  /* 通用减法（int fast path，其他类型 callout） */
        case OP_SHL: case OP_SHR: case OP_USHR:  /* 通用移位（int fast path） */
        case OP_LT: case OP_GT: case OP_LE: case OP_GE:  /* 通用比较（int fast path） */
        case OP_INC:  /* ++ (stack-top) */
        case OP_DEC:  /* -- (stack-top) */
        case OP_NOT:  /* logical NOT */
        case OP_INDEX: /* array/dict index access (callout) */
        case OP_ARRAY_APPEND_NOPUSH: /* arr.add(v) statement (callout) */
        case OP_DICT_SET: /* dict[key]=val (callout) */
        case OP_INDEX_SET_NOPUSH: /* arr[idx]=val statement (callout) */
        case OP_NULL:    /* push null */
        case OP_TRUE:    /* push true */
        case OP_FALSE:   /* push false */
        case OP_RETURN:  /* return (single value) */
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
        case OP_ARRAY:          /* opcode + count16 */
        case OP_GET_PROPERTY:   /* opcode + name_const16 */
            return 3;
        /* 5-byte (opcode + slot16 + slot16 or opcode + int32) */
        case OP_MOVE_LOCAL: case OP_MOVE_LOCAL_POP:
        case OP_SET_LOCAL_CONST:
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
        case OP_LOOP:
        case OP_TRY:   /* catch_offset(2) + finally_offset(2) */
        case OP_CALL_GLOBAL_FUNC:        /* func_slot(2) + arg_count(2) (callout) */
        case OP_CALL_GLOBAL_FUNC_TYPED:  /* func_slot(2) + arg_count(2) (callout) */
        case OP_CALL_NATIVE:             /* name_const(2) + arg_count(2) (callout) */
        case OP_CLEAR_LOCAL_RANGE:       /* base(2) + count(2) */
            return 5;
        /* 1-byte try/catch (no operands) */
        case OP_CATCH: case OP_FINALLY: case OP_END_TRY:
            return 1;
        /* 7-byte (OP_FOR_LOOP) */
        case OP_FOR_LOOP:
            return 7;
        /* 5-byte: OP_INVOKE_METHOD = opcode + name_const(2) + arg_count(2) */
        case OP_INVOKE_METHOD:
            return 5;
        /* 2-byte: OP_RETURN_MULTI = opcode + count(1) */
        case OP_RETURN_MULTI:
            return 2;
        /* 7-byte: OP_MODULE_CALL = opcode + module_idx(2) + method_idx(2) + arg_count(2) */
        case OP_MODULE_CALL:
            return 7;
        /* Variable-length: OP_ACC_FIELDS = opcode + count(1) + field_idx[count] */
        case OP_ACC_FIELDS:
            return 2 + ip[1];
        /* 8-byte (OP_FOR_PREP) — recognized so scan doesn't fail on
         * "unknown opcode"; actual nesting is rejected in scan. */
        case OP_FOR_PREP:
            return 8;
        /* 4-byte: OP_GET_FIELD_FAST = opcode + local_slot(2) + field_idx(1) */
        case OP_GET_FIELD_FAST:
            return 4;
        /* 10-byte (CMPJMP variants) */
        case OP_CMPJMP_LL_INT:
        case OP_CMPJMP_LG_INT:
            return 10;
        default:
            return -1;  /* unknown / unsupported */
    }
}




/* Mark a local slot as used; assign scratch index */
static void mark_local(ScanResult* r, int slot) {
    if (slot < 0 || slot >= 256) return;
    if (r->local_map[slot] >= 0) return;  /* already mapped */
    if (r->num_locals >= JIT_MAX_LOCALS) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] scan FAIL: too many locals (%d) at slot %d\n", r->num_locals, slot);
        r->capable = 0;
        return;
    }
    r->local_map[slot] = r->num_locals;
    r->local_slots[r->num_locals] = (uint8_t)slot;
    r->num_locals++;
}

/* ---- Scan a callee function body for inlining ----
 * Walks the callee's bytecode, checks all opcodes are JIT-supported,
 * tracks max vstack, and counts distinct local slots used.
 * Returns 1 if inlinable, 0 otherwise.
 * Fills callee_local_map[slot] = base_scratch + slot for slots 0..local_count-1.
 * Sets *out_max_vstack to the max vstack depth in the callee body. */
static int scan_callee_for_inline(Chunk* cc, int local_count,
                                   int base_scratch,
                                   int callee_local_map[256],
                                   int* out_max_vstack) {
    const uint8_t* ip = cc->code;
    const uint8_t* end = cc->code + cc->len;
    int vstack = 0;
    int mv = 0;

    /* Pre-map all callee locals 0..local_count-1 to base_scratch+0..base_scratch+N-1 */
    for (int i = 0; i < 256; i++) callee_local_map[i] = -1;
    for (int i = 0; i < local_count && i < 256; i++) {
        callee_local_map[i] = base_scratch + i;
    }

    while (ip < end) {
        uint8_t op = *ip;
        int size = opcode_size(ip);
        if (size < 0 || ip + size > end) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: opcode %d size=%d at off %d\n",
                        op, size, (int)(ip - cc->code));
            return 0;
        }

        /* Track vstack changes (simplified — same logic as scan_loop_body) */
        switch (op) {
            case OP_ZERO: case OP_ONE: case OP_DUP: case OP_CONST:
            case OP_NULL: case OP_TRUE: case OP_FALSE:
                vstack++; break;
            case OP_POP:
                vstack--; break;
            case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT:
            case OP_MOD_INT: case OP_DIV_INT:
            case OP_BITAND: case OP_BITOR: case OP_BITXOR:
            case OP_ADD_FLOAT: case OP_SUB_FLOAT: case OP_MUL_FLOAT: case OP_DIV_FLOAT:
            case OP_EQ_INT: case OP_LT_INT: case OP_GT_INT:
            case OP_LE_INT: case OP_GE_INT:
            case OP_EQ_FLOAT: case OP_LT_FLOAT: case OP_GT_FLOAT:
            case OP_LE_FLOAT: case OP_GE_FLOAT:
            case OP_DIV: case OP_INDEX:
            case OP_ADD: case OP_SUB:
            case OP_SHL: case OP_SHR: case OP_USHR:
            case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                vstack--; break;
            case OP_NEG_INT: case OP_NEG_FLOAT: case OP_NOT:
            case OP_CAST_INT: case OP_CAST_FLOAT:
            case OP_BITNOT: case OP_INC: case OP_DEC:
                break;
            case OP_ARRAY_APPEND_NOPUSH: vstack -= 2; break;
            case OP_DICT_SET: vstack -= 2; break;
            case OP_INDEX_SET_NOPUSH: vstack -= 3; break;
            case OP_RETURN:
                vstack--; break;
            case OP_RETURN_MULTI:
                vstack -= ip[1]; break;
            case OP_MODULE_CALL: {
                int ac = rd_short(ip + 5);
                vstack -= (ac - 1);
                break;
            }
            case OP_GET_LOCAL:
                vstack++; break;
            case OP_SET_LOCAL: break;
            case OP_SET_LOCAL_POP: vstack--; break;
            case OP_MOVE_LOCAL: vstack++; break;
            case OP_MOVE_LOCAL_POP: break;
            case OP_SET_LOCAL_CONST: break;
            case OP_GET_GLOBAL: vstack++; break;
            case OP_SET_GLOBAL: break;
            case OP_INC_LOCAL: case OP_DEC_LOCAL:
            case OP_PRE_INC_LOCAL: case OP_PRE_DEC_LOCAL:
                vstack++; break;
            case OP_INC_LOCAL_NOPUSH: case OP_DEC_LOCAL_NOPUSH: break;
            case OP_ADD_INT_IMM: case OP_SUB_INT_IMM: case OP_MUL_INT_IMM:
            case OP_LT_INT_IMM: case OP_GT_INT_IMM:
            case OP_LE_INT_IMM: case OP_GE_INT_IMM: case OP_EQ_INT_IMM:
            case OP_SHL_IMM: case OP_SHR_IMM: case OP_USHR_IMM:
                break;
            case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
            case OP_LOOP:
                break;
            case OP_FOR_PREP: break;
            case OP_FOR_LOOP: break;
            case OP_CMPJMP_LL_INT: break;
            case OP_CMPJMP_LG_INT: break;
            case OP_GET_FIELD_FAST: vstack++; break;
            case OP_INVOKE_METHOD: {
                int ac = rd_short(ip + 3);
                vstack -= (ac - 1);
                break;
            }
            case OP_ACC_FIELDS: break;
            case OP_TRY: case OP_CATCH: case OP_FINALLY: case OP_END_TRY:
                break;
            case OP_ARRAY: {
                uint16_t cnt = rd_short(ip + 1);
                vstack -= (cnt - 1);
                break;
            }
            case OP_GET_PROPERTY: {
                /* Peephole: GET_PROPERTY + OP_CALL merges into a single
                 * method call (receiver + args collapsed into result). */
                if (ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t ac = rd_short(ip + 4);
                    size = 6;                 /* consume OP_CALL too */
                    vstack -= ac;             /* receiver+args -> result */
                }
                /* standalone property access: vstack unchanged */
                break;
            }
            case OP_CALL_NATIVE: {
                uint16_t ac = rd_short(ip + 3);
                vstack -= (ac - 1);
                break;
            }
            case OP_CLEAR_LOCAL_RANGE: break;
            default:
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: unsupported opcode %d at off %d\n",
                            op, (int)(ip - cc->code));
                return 0;
        }

        if (vstack > JIT_MAX_VSTACK) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: vstack=%d\n", vstack);
            return 0;
        }
        if (vstack > mv) mv = vstack;
        ip += size;
    }

    *out_max_vstack = mv;
    return 1;
}

/*
 * Scan the loop body to determine JIT-ability and collect metadata.
 * body_start: pointer to first bytecode instruction of the loop body
 * body_size:  total bytecode bytes (including the back-edge instruction)
 * back_edge:  1=OP_LOOP, 2=OP_FOR_LOOP
 */
void scan_loop_body(const uint8_t* body_start, int body_size,
                           int back_edge, ScanResult* r, VM* vm_ptr) {
    memset(r, 0, sizeof(*r));
    r->capable = 1;
    r->back_edge_type = back_edge;
    for (int i = 0; i < 256; i++) r->local_map[i] = -1;

    const uint8_t* ip = body_start;
    const uint8_t* end = body_start + body_size;
    int vstack = 0;

    while (ip < end) {
        uint8_t op = *ip;
        int size = opcode_size(ip);
        if (size < 0) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d (size<0) at offset %d\n", op, (int)(ip - body_start));
            r->capable = 0;
            return;
        }
        if (ip + size > end) {
            if (jit_debug_on())
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
            case OP_DIV:
                /* 通用除法 (callout): pop 2 push 1 → net -1 */
                vstack -= 1;
                break;
            case OP_ADD: case OP_SUB:
            case OP_SHL: case OP_SHR: case OP_USHR:
            case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                /* 通用算术/移位/比较：int fast path（pop 2 push 1 → net -1） */
                vstack -= 1;
                break;
            case OP_ARRAY: {
                /* opcode + count16: pop count 元素 push 1 数组 → net -(count-1) */
                uint16_t cnt = rd_short(ip + 1);
                vstack -= (cnt - 1);
                break;
            }
            case OP_GET_PROPERTY: {
                /* opcode + name_const(2)。窥孔：后随 OP_CALL(arg_count16) 时
                 * 合并为一次方法调用：pop receiver+args push 1 → net -arg_count。
                 * 独立属性访问：弹 receiver push 值 → net 0。 */
                if (ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t ac = rd_short(ip + 4);
                    size = 6;   /* 连同 OP_CALL 一起消费 */
                    vstack -= ac;
                }
                break;
            }
            case OP_CALL_NATIVE: {
                /* name_const(2) + arg_count(2)，arg_count 包含 self */
                uint16_t ac = rd_short(ip + 3);
                vstack -= (ac - 1);
                break;
            }
            case OP_CLEAR_LOCAL_RANGE:
                /* base(2) + count(2)：只写 locals，不压栈 */
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
            case OP_INDEX_SET_NOPUSH:
                /* pop 3 (arr, index, value), push 0 → net -3 */
                vstack -= 3;
                break;
            case OP_NULL:
            case OP_TRUE:
            case OP_FALSE:
                /* push 1 value */
                vstack++;
                break;
            case OP_RETURN:
                /* pop 1 (return value) → vstack-- */
                vstack--;
                break;
            case OP_RETURN_MULTI: {
                /* opcode + count(1); pop count values */
                uint8_t rc = ip[1];
                vstack -= rc;
                break;
            }
            case OP_MODULE_CALL: {
                /* opcode + module_idx(2) + method_idx(2) + arg_count(2) */
                int ac = rd_short(ip + 5);
                vstack -= (ac - 1);
                break;
            }
            case OP_ACC_FIELDS:
                /* pop 1 (struct obj), push 1 (float sum) -> net 0 */
                break;
            case OP_INVOKE_METHOD: {
                /* name_const(2) + arg_count(2); arg_count includes self (receiver).
                 * pop arg_count, push 1 result -> net -(arg_count - 1) */
                int arg_count = rd_short(ip + 3);
                vstack -= (arg_count - 1);
                break;
            }
            case OP_CALL_GLOBAL_FUNC:
            case OP_CALL_GLOBAL_FUNC_TYPED: {
                /* func_slot(2) + arg_count(2); callout mode (VM re-entry).
                 * pop arg_count, push ret_count results.
                 * ret_count is 1 by default; for OP_CALL_GLOBAL_FUNC_TYPED
                 * with a known ObjFunction, use its return_count. */
                int arg_count = rd_short(ip + 3);
                int ret_count = 1;
                if (op == OP_CALL_GLOBAL_FUNC_TYPED && vm_ptr) {
                    uint16_t func_slot = rd_short(ip + 1);
                    if (func_slot < vm_ptr->global_func_capacity) {
                        Value callee = vm_ptr->global_funcs[func_slot];
                        if (val_is_obj(callee)) {
                            Object* obj2 = val_as_obj(callee);
                            ObjFunction* func2 = NULL;
                            if (obj2->type == OBJ_CLOSURE)
                                func2 = ((ObjClosure*)obj2)->function;
                            else if (obj2->type == OBJ_FUNCTION)
                                func2 = (ObjFunction*)obj2;
                            if (func2 && func2->return_count > 1)
                                ret_count = func2->return_count;

                            /* ---- Try to inline the callee ---- */
                            if (func2 && func2->chunk && r->inline_count < 4
                                && !getenv("LENO_JIT_NOINLINE")) {
                                Chunk* cc = func2->chunk;
                                int callee_lc = func2->local_count;
                                int base = r->num_locals + r->inline_extra_locals;
                                /* Check capacity: base + callee_lc must fit in JIT_MAX_LOCALS */
                                if (base + callee_lc <= JIT_MAX_LOCALS && cc->len <= 256) {
                                    int callee_mv = 0;
                                    InlineSite* is = &r->inline_sites[r->inline_count];
                                    if (scan_callee_for_inline(cc, callee_lc, base,
                                                               is->callee_local_map,
                                                               &callee_mv)) {
                                        is->bc_off = (int)(ip - body_start);
                                        is->func_slot = func_slot;
                                        is->arg_count = arg_count;
                                        is->ret_count = ret_count;
                                        is->callee_chunk = cc;
                                        is->callee_local_count = callee_lc;
                                        is->callee_local_base = base;
                                        is->callee_body_size = cc->len;
                                        is->inline_end_mc = -1;
                                        r->inline_count++;
                                        r->inline_extra_locals += callee_lc;
                                    /* Update max_vstack: the callee's vstack
                                     * operates on top of (caller_vstack - arg_count).
                                     * Also, the args themselves are on the stack
                                     * before the call. */
                                    int vstack_at_call = vstack;
                                    int vstack_after_pop = vstack - arg_count;
                                    int callee_total_max = vstack_after_pop + callee_mv;
                                    if (vstack_at_call > r->max_vstack)
                                        r->max_vstack = vstack_at_call;
                                    if (callee_total_max > r->max_vstack)
                                        r->max_vstack = callee_total_max;
                                    if (r->max_vstack > JIT_MAX_VSTACK) {
                                        if (jit_debug_on())
                                            fprintf(stderr, "[JIT-DEBUG] scan FAIL: max_vstack=%d after inline\n", r->max_vstack);
                                        r->capable = 0;
                                        return;
                                    }
                                    if (jit_debug_on())
                                        fprintf(stderr, "[JIT-DEBUG] inline: func_slot=%d arg_count=%d ret_count=%d callee_lc=%d base=%d mv=%d vstack_at_call=%d callee_total_max=%d\n",
                                                func_slot, arg_count, ret_count, callee_lc, base, callee_mv, vstack_at_call, callee_total_max);
                                    vstack -= (arg_count - ret_count);
                                    goto scan_next;
                                    }
                                }
                            }
                        }
                    }
                }
                vstack -= (arg_count - ret_count);
                break;
            }
            case OP_TRY:
            case OP_CATCH:
            case OP_FINALLY:
            case OP_END_TRY:
                /* Exception handling opcodes: treated as no-ops in JIT.
                 * The JIT skips try/catch/finally setup; if an exception
                 * actually occurs (from a callout), the JIT will bail out
                 * and the interpreter re-executes with full try/catch. */
                break;
            case OP_FOR_PREP:
                /* Inner for-loop init: mark locals, no stack change.
                 * Codegen handles init + condition check + skip jump. */
                mark_local(r, ip[1]);  /* start_slot */
                mark_local(r, ip[2]);  /* end_slot */
                mark_local(r, ip[3]);  /* step_slot */
                mark_local(r, ip[4]);  /* loop_var_slot */
                break;
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
                /* Back-edge: may be outer (last instruction) or inner (mid-body).
                 * Codegen uses offmap_lookup to find the correct jump target
                 * from the instruction's own offset. */
                break;
            /* 7-byte FOR_LOOP — back-edge: may be outer (last) or inner (mid-body) */
            case OP_FOR_LOOP: {
                /* Mark locals for both inner and outer for-loops */
                mark_local(r, ip[1]);  /* loop_var_slot */
                mark_local(r, ip[2]);  /* step_slot */
                mark_local(r, ip[3]);  /* end_slot */
                /* Record outer back-edge metadata (last instruction only) */
                if (ip + size >= end) {
                    r->for_loop_var_slot = ip[1];
                    r->for_step_slot = ip[2];
                    r->for_end_slot = ip[3];
                    r->for_inclusive = ip[4];
                }
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
            case OP_CMPJMP_LG_INT: {
                uint16_t sa = rd_short(ip + 2);
                mark_local(r, sa);
                /* global is not a local, no need to mark */
                /* no stack change */
                break;
            }
            /* Shift immediates — supported, pop 1 push 1 */
            case OP_SHL_IMM: case OP_SHR_IMM: case OP_USHR_IMM:
                break;
            /* 4-byte: OP_GET_FIELD_FAST = local read + struct field push (net +1) */
            case OP_GET_FIELD_FAST: {
                uint16_t slot = rd_short(ip + 1);
                mark_local(r, slot);
                vstack++;
                break;
            }
            default:
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d at offset %d\n", op, (int)(ip - body_start));
                r->capable = 0;
                return;
        }

    /* Allow vstack < 0: nested while loops have an exit OP_POP at the
     * jump target that the linear scan double-counts (true path's POP
     * already decremented; false path's POP decrements again). This is
     * a linear-scan artifact — the actual runtime never goes negative. */
    if (vstack > JIT_MAX_VSTACK) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] scan FAIL: vstack=%d at opcode %d\n", vstack, op);
        r->capable = 0;
        return;
    }
        r->max_vstack = vstack > r->max_vstack ? vstack : r->max_vstack;
        scan_next:
        ip += size;
    }

    r->body_size = body_size;

    if (jit_debug_on()) {
        fprintf(stderr, "[JIT-DEBUG] scan result: n_locals=%d max_vstack=%d\n", r->num_locals, r->max_vstack);
        for (int i = 0; i < r->num_locals; i++) {
            fprintf(stderr, "[JIT-DEBUG]   scratch[%d] = slot %d\n", i, r->local_slots[i]);
        }
        fprintf(stderr, "[JIT-DEBUG]   for_loop_var_slot=%d for_end_slot=%d for_step_slot=%d inclusive=%d\n",
                r->for_loop_var_slot, r->for_end_slot, r->for_step_slot, r->for_inclusive);
    }
}

