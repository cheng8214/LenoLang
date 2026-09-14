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
/* 老实现是 `(addr >> 4) & (SIZE-1)`：只用地址的 8 位，不同 chunk 里偏移相同的
 * 循环必然落到同一槽位（direct-mapped 下就互相驱逐）。这里先把低 4 位（16 字节
 * 对齐，无信息）移掉，再把高位异或折回低位，让槽位取决于更多地址位。 */
int cache_hash(const uint8_t* ip) {
    uintptr_t v = (uintptr_t)ip;
    v >>= 4;
    v ^= v >> 8;
    v ^= v >> 16;
    return (int)(v & (JIT_CACHE_SIZE - 1));
}

/* ---- struct 方法返回值个数（按方法名推断；_TYPED 解析失败时的兜底）----
 * OP_INVOKE_METHOD_TYPED 的正常路径用字节码里的静态类型名直接定位方法，但该解析
 * 可能失败（def 尚未注册、方法定义不完整等）。此时只能退回「枚举已注册 struct
 * 定义、按方法名匹配」的推断，前提是方法名在所有 def 中唯一且 return_count 一致。
 * JIT 的栈记账必须知道调用结束后留下几个返回值：多返回值方法（如 Font.measureString
 * → [float, float]）若按 1 个记账，第一个返回值会直接落在实参槽上（读到上一帧
 * 的残留值），表现为数值/位置每帧乱跳。
 *
 * 解析办法：枚举当前线程已注册的 struct 定义，按方法名匹配：
 *   - 匹配到的所有方法 return_count 一致（<=1 归一为 1）→ 返回该值
 *   - 一个都没匹配到 / 定义不完整 / 同名方法返回值个数不一致 → 返回 0
 * 返回 0 表示「无法确定」，调用方（scan/codegen）必须拒绝 JIT 交解释器执行，
 * 绝不能退化成「按 1 个返回值」处理 —— 那正是本 bug 的形态。 */
int jit_resolve_method_ret_count(Chunk* chunk, uint16_t name_const_idx) {
    if (!chunk || !chunk->constants || name_const_idx >= (uint16_t)chunk->const_cnt)
        return 0;
    Value name_val = chunk->constants[name_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)
        return 0;
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    if (!name) return 0;

    int found = 0;
    int rc = 0;
    int n = struct_def_get_count();
    for (int i = 0; i < n; i++) {
        ObjStructDef* def = struct_def_get(i);
        if (!def || !def->methods) continue;
        for (int j = 0; j < def->method_count; j++) {
            StructMethodInfo* m = &def->methods[j];
            if (!m->name || strcmp(m->name, name) != 0) continue;
            ObjFunction* fn = m->func;
            if (!fn && m->closure) fn = m->closure->function;
            if (!fn) return 0;                /* 定义不完整 → 无法确定 */
            int c = (fn->return_count > 1) ? fn->return_count : 1;
            if (found && c != rc) return 0;   /* 同名方法返回值个数不一致 → 无法确定 */
            rc = c;
            found = 1;
        }
    }
    return found ? rc : 0;
}

/* ---- struct 方法定义解析（方法内联用）----
 * 与 jit_resolve_method_ret_count 同一套「枚举已注册 def 按方法名匹配」的思路，
 * 但要求更强：方法名必须**唯一**（只有一个 def 定义了它），否则运行时可能分发到
 * 别的实现，内联哪一份都是猜。返回 NULL 一律退回 callout。 */
ObjFunction* jit_resolve_method_func(Chunk* chunk, uint16_t name_const_idx,
                                     ObjStructDef** out_def) {
    if (out_def) *out_def = NULL;
    if (!chunk || !chunk->constants || name_const_idx >= (uint16_t)chunk->const_cnt)
        return NULL;
    Value name_val = chunk->constants[name_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING)
        return NULL;
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    if (!name) return NULL;

    ObjStructDef* found_def = NULL;
    ObjFunction* found_fn = NULL;
    int n = struct_def_get_count();
    for (int i = 0; i < n; i++) {
        ObjStructDef* def = struct_def_get(i);
        if (!def || !def->methods) continue;
        for (int j = 0; j < def->method_count; j++) {
            StructMethodInfo* m = &def->methods[j];
            if (!m->name || strcmp(m->name, name) != 0) continue;
            ObjFunction* fn = m->func;
            if (!fn && m->closure) fn = m->closure->function;
            if (!fn) return NULL;                 /* 定义不完整 → 不猜 */
            if (found_def) return NULL;           /* 同名方法出现在多个 def → 有歧义 */
            /* 构造/析构不走 OP_INVOKE_METHOD_TYPED 的正常方法语义，内联没有意义 */
            if (def->has_ctor && j == def->ctor_index) return NULL;
            if (def->has_dtor && j == def->dtor_index) return NULL;
            found_def = def;
            found_fn = fn;
        }
    }
    if (!found_def || !found_fn) return NULL;
    if (out_def) *out_def = found_def;
    return found_fn;
}

/* ---- 带**编译期静态类型**的方法调用点解析（OP_INVOKE_METHOD_TYPED）----
 * 字节码里带了接收者的静态 struct 类型名常量，所以可以直接定位 def，再在该 def 的
 * 方法表里按名找方法 —— 不需要 jit_resolve_method_func 那种「方法名必须在所有 def
 * 中唯一」的推断，因此 init/update/clone 这类同名方法也能安全解析、内联。
 * 成功返回 1（out_* 均有效）；失败返回 0（类型名常量非法 / def 未注册 / 该方法不
 * 存在或定义不完整 / 命中构造析构）。失败时调用方退回按名唯一解析，再不行拒绝 JIT。 */
int jit_resolve_method_typed(Chunk* chunk, uint16_t name_const_idx, uint16_t type_const_idx,
                             ObjStructDef** out_def, ObjFunction** out_fn, int* out_ret_count) {
    if (out_def) *out_def = NULL;
    if (out_fn) *out_fn = NULL;
    if (out_ret_count) *out_ret_count = 0;
    if (!chunk || !chunk->constants) return 0;
    if (name_const_idx >= (uint16_t)chunk->const_cnt ||
        type_const_idx >= (uint16_t)chunk->const_cnt)
        return 0;
    Value name_val = chunk->constants[name_const_idx];
    Value type_val = chunk->constants[type_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) return 0;
    if (!val_is_obj(type_val) || val_as_obj(type_val)->type != OBJ_STRING) return 0;
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    const char* type_name = ((ObjString*)val_as_obj(type_val))->chars;
    if (!name || !type_name) return 0;

    ObjStructDef* def = struct_def_find(type_name);
    if (!def || !def->methods) return 0;

    for (int j = 0; j < def->method_count; j++) {
        StructMethodInfo* m = &def->methods[j];
        if (!m->name || strcmp(m->name, name) != 0) continue;
        /* 构造/析构不走 OP_INVOKE_METHOD_TYPED 的正常方法语义 */
        if (def->has_ctor && j == def->ctor_index) return 0;
        if (def->has_dtor && j == def->dtor_index) return 0;
        ObjFunction* fn = m->func;
        if (!fn && m->closure) fn = m->closure->function;
        if (!fn) return 0;                       /* 定义不完整 → 不猜 */
        if (out_def) *out_def = def;
        if (out_fn) *out_fn = fn;
        if (out_ret_count) *out_ret_count = (fn->return_count > 1) ? fn->return_count : 1;
        return 1;
    }
    return 0;                                    /* 该 def 没有这个方法 */
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
        case OP_CAST_STRING: /* 通用值转换：已是 string 原样，否则 bailout */
        case OP_DIV:  /* 通用除法，运行时类型分发 (callout) */
        case OP_ADD:  /* 通用加法（int fast path，其他类型 callout） */
        case OP_SUB:  /* 通用减法（int fast path，float 慢路径 / bailout） */
        case OP_MUL:  /* 通用乘法（int fast path，float 慢路径 / bailout） */
        case OP_MOD:  /* 通用取模（仅 int fast path，其余 bailout） */
        case OP_EQ: case OP_NEQ:  /* 通用相等比较（int/float 快路径，其余 bailout） */
        case OP_SHL: case OP_SHR: case OP_USHR:  /* 通用移位（int fast path） */
        case OP_LT: case OP_GT: case OP_LE: case OP_GE:  /* 通用比较（int fast path） */
        case OP_INC:  /* ++ (stack-top) */
        case OP_DEC:  /* -- (stack-top) */
        case OP_NOT:  /* logical NOT */
        case OP_INDEX: /* array/dict index access (callout) */
        case OP_LENGTH: /* x.len()：数字原生 / 其余 callout（pop1 push1） */
        case OP_ITER_GET:        /* for-in：数组元素原生 / 其余 callout（pop2 push1） */
        case OP_ITER_GET_VALUE:  /* for-in 取值：dict/struct callout（pop2 push1） */
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
        case OP_GET_FIELD:   /* op + field_idx(1) */
        case OP_SET_FIELD:   /* op + field_idx(1) */
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
        /* 7-byte: OP_INVOKE_METHOD_TYPED = opcode + name_const(2) + arg_count(2)
         *         + struct_type_name_const(2) */
        case OP_INVOKE_METHOD_TYPED:
            return 7;
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
        /* Variable-length: OP_STRUCT_INIT =
         *   opcode(1) + name_const(2) + arg_count(1) + generic_count(1)
         *   + generic_args(generic_count*2) + field_idx[arg_count] */
        case OP_STRUCT_INIT:
            return 5 + 2 * ip[4] + ip[3];
        /* 10-byte (CMPJMP variants) */
        case OP_CMPJMP_LL_INT:
        case OP_CMPJMP_LG_INT:
            return 10;
        /* 12-byte: CMPJMP local vs imm32 (cmp_op(1) slot(2) imm32(4) offset(4)) */
        case OP_CMPJMP_LI_INT:
            return 12;
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
            case OP_ITER_GET: case OP_ITER_GET_VALUE:   /* pop 2 push 1 → net -1 */
            case OP_SET_FIELD:                          /* pop 2 push 1 → net -1 */
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
            case OP_EQ: case OP_NEQ:
            case OP_SHL: case OP_SHR: case OP_USHR:
            case OP_LT: case OP_GT: case OP_LE: case OP_GE:
                vstack--; break;
            case OP_NEG_INT: case OP_NEG_FLOAT: case OP_NOT:
            case OP_CAST_INT: case OP_CAST_FLOAT: case OP_CAST_STRING:
            case OP_BITNOT: case OP_INC: case OP_DEC:
            case OP_LENGTH:   /* pop 1 push 1 → net 0 */
            case OP_GET_FIELD: /* pop 1 push 1 → net 0 */
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
            case OP_CMPJMP_LI_INT: break;
case OP_GET_FIELD_FAST: vstack++; break;
            case OP_STRUCT_INIT: {
                /* opcode + name16 + arg8 + generic8 + generic2*N + field[arg]。
                 * 泛型 struct 的构造语义依赖调用栈帧解析（OP_STRUCT_INIT），
                 * JIT callout 不支持 → 拒绝 inline。 */
                if (ip[4] > 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: generic OP_STRUCT_INIT at off %d\n",
                                (int)(ip - cc->code));
                    return 0;
                }
                vstack -= ((int)ip[3] - 1);
                break;
            }
            case OP_INVOKE_METHOD_TYPED: {
                /* 返回值个数按编译期解析结果记账；解析不出来则拒绝内联
                 * （callee 的返回值个数未知时无法保证栈记账正确）。 */
                int ac = rd_short(ip + 3);
                int rc = 0;
                /* 按字节码里的静态类型名直接定位 def/方法（无需"方法名唯一"） */
                (void)jit_resolve_method_typed(cc, rd_short(ip + 1), rd_short(ip + 5),
                                               NULL, NULL, &rc);
                if (rc <= 0)
                    rc = jit_resolve_method_ret_count(cc, rd_short(ip + 1));
                if (rc <= 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: INVOKE_METHOD ret_count 无法确定 at off %d\n",
                                (int)(ip - cc->code));
                    return 0;
                }
                vstack -= (ac - rc);
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
                           int back_edge, ScanResult* r, VM* vm_ptr, Chunk* chunk) {
    memset(r, 0, sizeof(*r));
    r->capable = 1;
    r->back_edge_type = back_edge;
    r->jt_ok = (body_size <= JIT_SCAN_JT_MAX);   /* §8.46：超上限则放弃回看 */
    for (int i = 0; i < 256; i++) r->local_map[i] = -1;

    /* §8.46：标记某个偏移是前向跳转目标（超上限/越界则整体标记不可信） */
    #define MARK_JT_FWD(t) do { int _t = (int)(t); \
        if (_t >= 0 && _t < JIT_SCAN_JT_MAX) r->jt_fwd[_t] = 1; else r->jt_ok = 0; } while(0)

    const uint8_t* ip = body_start;
    const uint8_t* end = body_start + body_size;
    int vstack = 0;

    /* Dead-code tracking: after OP_JUMP/OP_RETURN, code is unreachable
     * until a forward jump target. This prevents double-counting POPs
     * in mutually exclusive truthy/falsey paths (if/else, if/continue). */
    typedef struct { int target_bc; int vstack; } ScanJumpTarget;
    #define JIT_SCAN_MAX_FWD 256
    ScanJumpTarget fwd_targets[JIT_SCAN_MAX_FWD];
    int fwd_count = 0;
    int dead = 0;

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

        int bc_off = (int)(ip - body_start);

        /* If in dead code, check whether we've reached a recorded
         * forward-jump target. If so, restore vstack and resume live
         * scanning. If not, skip this instruction entirely. */
        if (dead) {
            int found = 0;
            for (int i = 0; i < fwd_count; i++) {
                if (fwd_targets[i].target_bc == bc_off) {
                    vstack = fwd_targets[i].vstack;
                    dead = 0;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                goto scan_next;
            }
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
            case OP_CAST_STRING:
                /* 通用值转换：pop 1 push 1 → net 0
                 * （JIT 只放行「已是 string」的值，其余 bailout 交解释器） */
                break;
            case OP_DIV:
                /* 通用除法 (callout): pop 2 push 1 → net -1 */
                vstack -= 1;
                break;
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_MOD:
            case OP_EQ: case OP_NEQ:
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
            case OP_LENGTH:
                /* pop 1 push 1 → net 0（结果恒为 int，见 ops_misc.inc 的 OP_LENGTH） */
                break;
            case OP_GET_FIELD:
                /* pop 1 (obj) push 1 (field) → net 0 */
                break;
            case OP_SET_FIELD:
                /* pop 2 (obj, value) push 1（赋值表达式的值）→ net -1 */
                vstack--;
                break;
            case OP_INDEX:
            case OP_ITER_GET:
            case OP_ITER_GET_VALUE:
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
                /* 到这里都是可达代码：循环体内可达的 return 循环 JIT 处理不了
                 * （见 ScanResult.has_reachable_return 说明），标出来由
                 * jit_compile 拒绝整循环。 */
                r->has_reachable_return = 1;
                dead = 1;
                break;
            case OP_RETURN_MULTI: {
                /* opcode + count(1); pop count values */
                uint8_t rc = ip[1];
                vstack -= rc;
                r->has_reachable_return = 1;
                dead = 1;
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
            case OP_INVOKE_METHOD_TYPED: {
                /* name_const(2) + arg_count(2) + struct_type_name_const(2)；
                 * arg_count includes self (receiver)。
                 * pop arg_count, push ret_count results -> net -(arg_count - ret_count)。
                 * ret_count 不能假定为 1：多返回值方法（如 Font.measureString）
                 * 的后续栈布局全靠它，解析不出来就拒绝 JIT（capable=0）。 */
                int arg_count = rd_short(ip + 3);
                int name_idx = rd_short(ip + 1);
                int ret_count = 0;
                /* 字节码带编译期静态类型名：直接定位 def/方法，不再需要
                 * 「方法名在所有 def 中唯一」的推断（同名方法也能安全解析/内联）。 */
                ObjStructDef* typed_def = NULL;
                ObjFunction* typed_fn = NULL;
                (void)jit_resolve_method_typed(chunk, name_idx, rd_short(ip + 5),
                                               &typed_def, &typed_fn, &ret_count);
                if (ret_count <= 0)
                    ret_count = jit_resolve_method_ret_count(chunk, name_idx);
                if (ret_count <= 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: INVOKE_METHOD ret_count 无法确定 at offset %d\n",
                                (int)(ip - body_start));
                    r->capable = 0;
                    return;
                }
                /* ---- 尝试内联 struct 方法体（P5 续：方法调用内联）----
                 * 条件（任一不满足就退回 callout，语义不变）：
                 *   - 单返回值（=1），与 codegen/内联退出记账一致；
                 *   - 能定位方法体：优先用字节码里的静态类型名直接定位；失败再
                 *     退回「方法名在所有 def 中唯一」的推断（有歧义就放弃）；
                 *   - 体内可内联（scan_callee_for_inline）、无 try、体长 ≤256；
                 *   - callee locals 放得下（callee_lc >= arg_count，避免越界映射）。
                 * 接收者的运行时类型由 codegen 生成 def 守卫兜底。 */
                if (ret_count == 1 && arg_count >= 1 && r->inline_count < 4
                    && !getenv("LENO_JIT_NOINLINE")) {
                    ObjStructDef* mdef = typed_def;
                    ObjFunction* mf = typed_fn;
                    if (!mf) mf = jit_resolve_method_func(chunk, name_idx, &mdef);
                    if (mf && mdef && mf->chunk && !mf->has_try &&
                        mf->chunk->len > 0 && mf->chunk->len <= 256) {
                        int callee_lc = mf->local_count;
                        int base = r->num_locals + r->inline_extra_locals;
                        if (callee_lc >= arg_count && base + callee_lc <= JIT_MAX_LOCALS) {
                            int callee_mv = 0;
                            InlineSite* is = &r->inline_sites[r->inline_count];
                            if (scan_callee_for_inline(mf->chunk, callee_lc, base,
                                                       is->callee_local_map, &callee_mv)) {
                                is->bc_off = bc_off;
                                is->func_slot = 0xFFFF;      /* 非全局函数调用点 */
                                is->arg_count = arg_count;
                                is->ret_count = ret_count;
                                is->callee_chunk = mf->chunk;
                                is->callee_local_count = callee_lc;
                                is->callee_local_base = base;
                                is->callee_body_size = mf->chunk->len;
                                is->inline_end_mc = -1;
                                is->is_method = 1;
                                is->method_def = mdef;
                                r->inline_count++;
                                r->inline_extra_locals += callee_lc;
                                /* 与全局函数内联同一套 max_vstack 估算 */
                                int vstack_at_call = vstack;
                                int callee_total_max = (vstack - arg_count) + callee_mv;
                                if (vstack_at_call > r->max_vstack)
                                    r->max_vstack = vstack_at_call;
                                if (callee_total_max > r->max_vstack)
                                    r->max_vstack = callee_total_max;
                                if (r->max_vstack > JIT_MAX_VSTACK) {
                                    if (jit_debug_on())
                                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: max_vstack=%d after method inline\n",
                                                r->max_vstack);
                                    r->capable = 0;
                                    return;
                                }
                                if (jit_debug_on())
                                    fprintf(stderr, "[JIT-DEBUG] inline(method): '%s' bc_off=%d arg_count=%d callee_lc=%d base=%d mv=%d\n",
                                            mf->name ? mf->name : "?", bc_off, arg_count,
                                            callee_lc, base, callee_mv);
                                vstack -= (arg_count - ret_count);
                                goto scan_next;
                            }
                        }
                    }
                }
                vstack -= (arg_count - ret_count);
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
            case OP_JUMP: {
                /* Forward jump: record target+vstack for restore,
                 * then mark code as dead until target is reached */
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                if (fwd_count >= JIT_SCAN_MAX_FWD) {
                    /* 目标表满：旧实现静默丢弃 → dead-code 段的 vstack 无法恢复
                     * → 扫描与 codegen 的栈深不一致 → RSP 漂移 → 栈溢出。
                     * 宁可不编，拒绝整个循环（循环退回解释器执行，语义正确）。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                        "（body_size=%d）→ 拒绝 JIT\n",
                                JIT_SCAN_MAX_FWD, body_size);
                    r->capable = 0;
                    return;
                }
                fwd_targets[fwd_count].target_bc = target_bc;
                fwd_targets[fwd_count].vstack = vstack;
                fwd_count++;
                dead = 1;
                break;
            }
            case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: {
                /* Conditional jump: record target+vstack for restore
                 * (falsey path arrives with condition still on stack).
                 * Fall-through (truthy) path is still live. */
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                if (fwd_count >= JIT_SCAN_MAX_FWD) {
                    /* 目标表满：旧实现静默丢弃 → dead-code 段的 vstack 无法恢复
                     * → 扫描与 codegen 的栈深不一致 → RSP 漂移 → 栈溢出。
                     * 宁可不编，拒绝整个循环（循环退回解释器执行，语义正确）。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                        "（body_size=%d）→ 拒绝 JIT\n",
                                JIT_SCAN_MAX_FWD, body_size);
                    r->capable = 0;
                    return;
                }
                fwd_targets[fwd_count].target_bc = target_bc;
                fwd_targets[fwd_count].vstack = vstack;
                fwd_count++;
                break;
            }
            case OP_LOOP:
                /* Back-edge: may be outer (last instruction) or inner (mid-body).
                 * Codegen uses offmap_lookup to find the correct jump target
                 * from the instruction's own offset. */
                /* §8.46：只有「非本条回边」的内层回边才是回看的不安全因素
                 * （它的目标可能正好落在两条相邻语句之间） */
                if (ip + size < end) r->has_back_jump = 1;
                break;
            /* 7-byte FOR_LOOP — back-edge: may be outer (last) or inner (mid-body) */
            case OP_FOR_LOOP: {
                /* Mark locals for both inner and outer for-loops */
                mark_local(r, ip[1]);  /* loop_var_slot */
                mark_local(r, ip[2]);  /* step_slot */
                mark_local(r, ip[3]);  /* end_slot */
                if (ip + size < end) r->has_back_jump = 1;   /* §8.46：内层回边 */
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
                /* Conditional jump: record target for restore */
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                if (fwd_count >= JIT_SCAN_MAX_FWD) {
                    /* 目标表满：旧实现静默丢弃 → dead-code 段的 vstack 无法恢复
                     * → 扫描与 codegen 的栈深不一致 → RSP 漂移 → 栈溢出。
                     * 宁可不编，拒绝整个循环（循环退回解释器执行，语义正确）。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                        "（body_size=%d）→ 拒绝 JIT\n",
                                JIT_SCAN_MAX_FWD, body_size);
                    r->capable = 0;
                    return;
                }
                fwd_targets[fwd_count].target_bc = target_bc;
                fwd_targets[fwd_count].vstack = vstack;
                fwd_count++;
                break;
            }
            case OP_CMPJMP_LI_INT: {
                /* local vs 立即数：只有一个局部量 */
                uint16_t sa = rd_short(ip + 2);
                mark_local(r, sa);
                /* Conditional jump: record target for restore */
                int32_t off = rd_int32(ip + 8);   /* imm32 占 ip+4..ip+7 */
                int target_bc = bc_off + size + off;
                MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                if (fwd_count >= JIT_SCAN_MAX_FWD) {
                    /* 目标表满：旧实现静默丢弃 → dead-code 段的 vstack 无法恢复
                     * → 扫描与 codegen 的栈深不一致 → RSP 漂移 → 栈溢出。
                     * 宁可不编，拒绝整个循环（循环退回解释器执行，语义正确）。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                        "（body_size=%d）→ 拒绝 JIT\n",
                                JIT_SCAN_MAX_FWD, body_size);
                    r->capable = 0;
                    return;
                }
                fwd_targets[fwd_count].target_bc = target_bc;
                fwd_targets[fwd_count].vstack = vstack;
                fwd_count++;
                break;
            }
            case OP_CMPJMP_LG_INT: {
                uint16_t sa = rd_short(ip + 2);
                mark_local(r, sa);
                /* Conditional jump: record target for restore */
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                if (fwd_count >= JIT_SCAN_MAX_FWD) {
                    /* 目标表满：旧实现静默丢弃 → dead-code 段的 vstack 无法恢复
                     * → 扫描与 codegen 的栈深不一致 → RSP 漂移 → 栈溢出。
                     * 宁可不编，拒绝整个循环（循环退回解释器执行，语义正确）。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                        "（body_size=%d）→ 拒绝 JIT\n",
                                JIT_SCAN_MAX_FWD, body_size);
                    r->capable = 0;
                    return;
                }
                fwd_targets[fwd_count].target_bc = target_bc;
                fwd_targets[fwd_count].vstack = vstack;
                fwd_count++;
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
            /* Variable-length: OP_STRUCT_INIT (struct 实例构造, callout)
             * 布局: op(1) + name16(2) + arg8(1) + generic8(1) + [generic2*N] + field[arg]。
             * 泛型构造在 scan 阶段拒绝（callout 无法解析调用栈帧的泛型参数）。 */
            case OP_STRUCT_INIT: {
                if (ip[4] > 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] scan FAIL: generic OP_STRUCT_INIT at offset %d\n",
                                (int)(ip - body_start));
                    r->capable = 0;
                    return;
                }
                vstack -= ((int)ip[3] - 1);   /* pop arg 实参, push 1 实例 → net -(arg-1) */
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

