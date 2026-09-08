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
#include "jit_emit.h"
#include "../include/leno_error.h"
#include "../include/native.h"
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

/* ---- Callout failure flag ----
 * Set by callouts when vm_call_value fails (exception thrown, not caught).
 * JIT codegen checks this after each callout that calls vm_call_value,
 * and bails out to the interpreter if set. */
static volatile int jit_callout_failed = 0;

/* ---- Bailout debug function ---- */
static int64_t jit_bailout_rax = 0;
static int32_t jit_bailout_site = 0;
static void jit_bailout_debug(int64_t rsp_val) {
    fprintf(stderr, "[JIT-DEBUG] BAILOUT site=%d RSP=%lld (0x%llx) RAX=%lld (0x%llx)\n",
            (int)jit_bailout_site,
            (long long)rsp_val, (unsigned long long)rsp_val,
            (long long)jit_bailout_rax, (unsigned long long)jit_bailout_rax);
}

/* ---- Reloaded locals pointer (updated by callouts after vm_call_value,
 * in case vm_grow_frames reallocates vm.frames; JIT writeback reloads
 * RCX from this before storing locals back) ---- */
static Value* jit_reloaded_locals = NULL;

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

/* Callout: OP_INDEX_SET_NOPUSH (arr[idx]=val, statement form, no push).
 * Returns 0 on success, -1 on error. */
static int jit_callout_index_set(Value obj_val, Value idx_val, Value value) {
    if (!val_is_obj(obj_val)) {
        error_add_at(ERR_RUNTIME, 0, 0, "索引赋值需要对象类型");
        return -1;
    }
    Object* obj = val_as_obj(obj_val);
    if (obj->type == OBJ_ARRAY) {
        if (!val_is_num(idx_val)) {
            error_add_at(ERR_RUNTIME, 0, 0, "数组索引必须是数字");
            return -1;
        }
        ObjArray* arr = (ObjArray*)obj;
        int index = val_is_int(idx_val) ? (int)val_as_int(idx_val) : (int)value_to_double(idx_val);
        if (index < 0 || index >= arr->capacity) {
            error_add_at(ERR_RUNTIME, 0, 0, "数组索引越界");
            return -1;
        }
        arr->elements[index] = value;
        gc_write_barrier((Object*)arr, value);
        if (index >= arr->count) arr->count = index + 1;
        return 0;
    }
    if (obj->type == OBJ_DICT) {
        ObjDict* dict = (ObjDict*)obj;
        dict_set(dict, idx_val, value);
        return 0;
    }
    error_add_at(ERR_RUNTIME, 0, 0, "索引赋值需要数组或字典");
    return -1;
}

/* Callout: OP_DIV (通用除法，运行时类型分发: int/int, float 混合, BigInt).
 * 语义与 VM 的 OP_DIV 一致；出错时记录错误并返回 NULL_VAL（与其它 callout 一致）。 */
static Value jit_callout_div(Value a, Value b) {
    /* int / int = int（整数除法，向零取整） */
    if (val_is_int(a) && val_is_int(b)) {
        int64_t b_val = val_as_int(b);
        if (b_val == 0) {
            error_add_at(ERR_RUNTIME, 0, 0, "除零错误：除数为 0");
            return NULL_VAL;
        }
        int64_t a_val = val_as_int(a);
        return val_int(a_val / b_val);
    }
    /* float / 任意 = float */
    if (val_is_float(a) || val_is_float(b)) {
        double b_val = val_as_num_ex(b);
        if (b_val == 0) {
            error_add_at(ERR_RUNTIME, 0, 0, "除零错误：除数为 0");
            return NULL_VAL;
        }
        double result = val_as_num_ex(a) / b_val;
        return val_float(result);
    }
    /* BigInt / BigInt（promote_to_bigint 是 vm 内部 static inline，这里等价内联） */
    if (val_is_bigint(a) || val_is_bigint(b)) {
        if (val_is_bigint(b)) {
            ObjBigInt* bb = val_as_bigint(b);
            if (bb->limb_count == 1 && bb->limbs[0] == 0) {
                error_add_at(ERR_RUNTIME, 0, 0, "除零错误：除数为 0");
                return NULL_VAL;
            }
        } else if (val_as_num(b) == 0) {
            error_add_at(ERR_RUNTIME, 0, 0, "除零错误：除数为 0");
            return NULL_VAL;
        }
        ObjBigInt* ba = val_is_bigint(a) ? val_as_bigint(a)
                                         : bigint_from_int64((int64_t)val_as_num(a));
        ObjBigInt* bb = val_is_bigint(b) ? val_as_bigint(b)
                                         : bigint_from_int64((int64_t)val_as_num(b));
        return bigint_div(ba, bb);
    }
    error_add_at(ERR_RUNTIME, 0, 0, "操作数必须是数字");
    return NULL_VAL;
}

/* Callout: OP_ACC_FIELDS (pop struct, sum N fields as float, push result).
 * Pure computation — no VM re-entry. */
static Value jit_callout_acc_fields(Value obj_val, uint8_t count,
                                    const uint8_t* field_indices) {
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_STRUCT) {
        error_add_at(ERR_RUNTIME, 0, 0, "OP_ACC_FIELDS: 需要 struct 类型");
        return NULL_VAL;
    }
    ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
    ObjStructDef* def = obj->def;
    double sum = 0.0;
    for (int i = 0; i < count; i++) {
        uint8_t idx = field_indices[i];
        if (idx >= def->field_count) {
            error_add_at(ERR_RUNTIME, 0, 0, "字段索引越界");
            return NULL_VAL;
        }
        Value fv = struct_get_field(obj, idx);
        sum += val_as_num_ex(fv);
    }
    return val_float(sum);
}

/* Callout: OP_INVOKE_METHOD (struct method call via VM re-entry).
 * arg_count includes self (receiver is first arg).
 * Returns NaN-boxed result from vm->last_return_value. */
static Value jit_callout_invoke_method(int64_t* vstack_top, int arg_count,
                                       const uint8_t* ip, Chunk* chunk) {
    VM* vm = jit_callout_vm;
    if (!vm) return NULL_VAL;

    if (getenv("LENO_JIT_DEBUG")) {
        uint16_t mni = rd_short(ip + 1);
        const char* mname = "?";
        if (mni < chunk->const_cnt) {
            Value mv = chunk->constants[mni];
            if (val_is_obj(mv) && val_as_obj(mv)->type == OBJ_STRING)
                mname = ((ObjString*)val_as_obj(mv))->chars;
        }
        fprintf(stderr, "[JIT-DEBUG] callout_invoke_method: '%s' arg_count=%d vstack_top=%p\n",
                mname, arg_count, (void*)vstack_top);
    }

    /* Read method name constant from bytecode: name_const(2) at ip+1 */
    uint16_t method_name_idx = rd_short(ip + 1);
    /* arg_count from bytecode at ip+3 */
    /* int arg_count already passed as parameter (matches ip[3..4]) */

    Value method_name_val = chunk->constants[method_name_idx];
    if (!val_is_obj(method_name_val) || val_as_obj(method_name_val)->type != OBJ_STRING) {
        error_add_at(ERR_RUNTIME, 0, 0, "方法名必须是字符串");
        return NULL_VAL;
    }
    ObjString* method_name = (ObjString*)val_as_obj(method_name_val);

    /* Save VM stack state */
    int saved_sp = vm->sp;

    /* Push args from JIT virtual stack to VM stack (receiver first, i.e. bottom).
     * JIT virtual stack grows downward (x86 push/pop): RSP points to topmost
     * element (last pushed = lowest address). First-pushed elements are at
     * HIGHER addresses, so receiver is at vstack_top + (arg_count - 1). */
    for (int i = 0; i < arg_count; i++) {
        int64_t raw = vstack_top[arg_count - 1 - i];
        vm_stack_push(vm, jit_raw_to_value(raw));
    }

    /* Now receiver is at vm->stack[vm->sp - arg_count] */
    Value obj_val = vm->stack[vm->sp - arg_count];
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_STRUCT) {
        char msg[256];
        snprintf(msg, sizeof(msg), "尝试在非 struct 类型上调用方法 '%s'", method_name->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        vm->sp = saved_sp;
        return NULL_VAL;
    }

    ObjStructDef* def = ((ObjStruct*)val_as_obj(obj_val))->def;
    ObjClosure* closure = NULL;
    for (int i = 0; i < def->method_count; i++) {
        if (strcmp(def->methods[i].name, method_name->chars) == 0) {
            if (def->has_ctor && i == def->ctor_index) continue;
            if (def->has_dtor && i == def->dtor_index) continue;
            closure = def->methods[i].closure;
            break;
        }
    }
    if (!closure) {
        char msg[256];
        snprintf(msg, sizeof(msg), "类型 '%s' 没有方法 '%s'",
                 def->name ? def->name : "?", method_name->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        vm->sp = saved_sp;
        return NULL_VAL;
    }

    /* Push callee and call */
    vm_stack_push(vm, val_obj((Object*)closure));
    int saved_frame_cnt = vm->frame_cnt;
    int call_r = vm_call_value(val_obj((Object*)closure), arg_count, 0);

    Value result = vm->last_return_value;

    if (getenv("LENO_JIT_DEBUG")) {
        fprintf(stderr, "[JIT-DEBUG] callout_invoke_method: vm_call_value returned %d, frame_cnt %d->%d\n",
                call_r, saved_frame_cnt, vm->frame_cnt);
    }

    /* Check if vm_call_value failed or didn't complete the callee */
    if (call_r == 0) {
        /* vm_call_value failed — exception was thrown and not caught.
         * Clean up leaked callee frame(s). */
        while (vm->frame_cnt > saved_frame_cnt) {
            vm->frame_cnt--;
            CallFrame* leaked = &vm->frames[vm->frame_cnt];
            if (leaked->locals && leaked->locals_is_dynamic) {
                free(leaked->locals);
                leaked->locals = NULL;
            }
        }
        vm->sp = saved_sp;
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    /* Reload locals pointer in case vm_grow_frames reallocated vm.frames */
    if (vm->frame_cnt > 0) {
        jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
    }

    /* Restore VM stack */
    vm->sp = saved_sp;

    return result;
}

/* Callout: OP_CALL_GLOBAL_FUNC / OP_CALL_GLOBAL_FUNC_TYPED (global function call via VM re-entry).
 * callee is directly available from vm->global_funcs[func_slot].
 * Returns NaN-boxed first result in RAX. For multi-return (ret_count > 1),
 * writes additional results directly into the JIT vstack memory at
 * vstack_top[arg_count - ret_count + i] (for i = 1..ret_count-1). */
static Value jit_callout_global_func(int64_t* vstack_top, int arg_count,
                                     uint16_t func_slot) {
    VM* vm = jit_callout_vm;
    if (!vm) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] global_func: jit_callout_vm is NULL!\n");
        return NULL_VAL;
    }

    if (func_slot >= vm->global_func_capacity) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] global_func: slot %d >= capacity %d\n", func_slot, vm->global_func_capacity);
        error_add_at(ERR_RUNTIME, 0, 0, "全局函数索引越界");
        return NULL_VAL;
    }

    Value callee = vm->global_funcs[func_slot];
    if (!val_is_obj(callee)) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] global_func: callee not obj, slot=%d\n", func_slot);
        error_add_at(ERR_RUNTIME, 0, 0, "全局函数未定义");
        return NULL_VAL;
    }

    /* Determine return_count from callee's ObjFunction.
     * return_count <= 0 or inconsistent (-1) → treat as single return. */
    int ret_count = 1;
    Object* obj = val_as_obj(callee);
    if (obj->type == OBJ_CLOSURE) {
        ObjClosure* closure = (ObjClosure*)obj;
        if (closure->function && closure->function->return_count > 1) {
            ret_count = closure->function->return_count;
        }
    } else if (obj->type == OBJ_FUNCTION) {
        ObjFunction* func = (ObjFunction*)obj;
        if (func && func->return_count > 1) {
            ret_count = func->return_count;
        }
    }

    /* Save VM stack state */
    int saved_sp = vm->sp;
    int saved_frame_cnt = vm->frame_cnt;

    /* Push args from JIT virtual stack to VM stack (first arg at bottom).
     * JIT virtual stack grows downward: first-pushed elements are at
     * HIGHER addresses, so first arg is at vstack_top + (arg_count - 1). */
    for (int i = 0; i < arg_count; i++) {
        int64_t raw = vstack_top[arg_count - 1 - i];
        vm_stack_push(vm, jit_raw_to_value(raw));
    }

    /* Push callee on top of stack */
    vm_stack_push(vm, callee);

    int call_result = vm_call_value(callee, arg_count, 0);

    /* If vm_call_value failed (exception thrown, not caught within callee),
     * clean up leaked callee frame(s) and set failure flag for JIT codegen. */
    if (call_result == 0) {
        /* Pop leaked frames (callee frame was pushed by call_value but
         * never popped because vm_run_with_vm returned -1). */
        if (getenv("LENO_JIT_DEBUG")) {
            fprintf(stderr, "[JIT-DEBUG] global_func callout FAILED: func_slot=%d arg_count=%d has_exception=%d frame_cnt=%d saved=%d\n",
                    func_slot, arg_count, vm->has_exception, vm->frame_cnt, saved_frame_cnt);
            if (vm->has_exception && val_is_obj(vm->exception) && val_as_obj(vm->exception)->type == OBJ_DICT) {
                ObjDict* ed = (ObjDict*)val_as_obj(vm->exception);
                ObjString* mk = str_copy("msg", 3);
                Value mv = dict_get(ed, val_obj((Object*)mk));
                if (val_is_string(mv))
                    fprintf(stderr, "[JIT-DEBUG] exception: %s\n", ((ObjString*)val_as_obj(mv))->chars);
            }
        }
        while (vm->frame_cnt > saved_frame_cnt) {
            vm->frame_cnt--;
            CallFrame* leaked = &vm->frames[vm->frame_cnt];
            if (leaked->locals && leaked->locals_is_dynamic) {
                free(leaked->locals);
                leaked->locals = NULL;
            }
        }
        vm->sp = saved_sp;
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    /* Read return values from VM stack.
     * OP_RETURN / OP_RETURN_MULTI (stop_frame_cnt path) pushed ret_count
     * values onto the VM stack. They are at vm->stack[vm->sp - ret_count]
     * through vm->stack[vm->sp - 1]. */
    Value result = vm->last_return_value;  /* first return value (fallback) */

    if (ret_count > 1 && vm->sp >= ret_count) {
        /* Read all return values from VM stack */
        Value ret_vals[16];
        for (int i = 0; i < ret_count && i < 16; i++) {
            ret_vals[i] = vm->stack[vm->sp - ret_count + i];
        }
        result = ret_vals[ret_count - 1];  /* last return value → RAX (TOS) */

        /* Write ret_vals[0..ret_count-2] into the JIT vstack memory.
         * The VM pushes results[0] first, results[N-1] last (TOS).
         * The JIT must match: RAX = results[N-1] (TOS), and the memory
         * stack has results[0] (deepest) through results[N-2] (TOS of memory).
         *
         * vstack_top[0] is at the lowest address (native TOS before pop).
         * After popping (arg_count - ret_count + 1) args, RSP moves up.
         * [RSP] = vstack_top[pop_count] is the new TOS of memory.
         *
         * We write ret_vals[i] to vstack_top[arg_count - 1 - i]:
         *   ret_vals[0]           → vstack_top[arg_count-1] (deepest, [RSP + (ret_count-2)*8])
         *   ret_vals[ret_count-2] → vstack_top[arg_count - ret_count + 1] ([RSP], TOS of memory)
         */
        for (int i = 0; i < ret_count - 1 && i < 15; i++) {
            int slot = arg_count - 1 - i;
            if (slot >= 0) {
                vstack_top[slot] = jit_value_to_raw(ret_vals[i]);
            }
        }
    }

    /* Reload locals pointer in case vm_grow_frames reallocated vm.frames
     * during the nested VM execution (e.g. deep recursion). The JIT's
     * RCX register still holds the pre-callout locals pointer, which may
     * now be dangling. jit_reloaded_locals is read by the JIT writeback
     * code before storing locals back to the VM. */
    if (vm->frame_cnt > 0) {
        jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
    }

    /* Restore VM stack */
    vm->sp = saved_sp;

    return result;
}

/* Callout: OP_GET_FIELD_FAST (read struct from local, get field value, push).
 * Pure computation — no VM re-entry.
 * Same semantics as VM's OP_GET_FIELD_FAST: locals[slot] → struct → field_values[idx]. */
static Value jit_callout_get_field_fast(Value obj_val, uint8_t field_idx) {
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_STRUCT) {
        error_add_at(ERR_RUNTIME, 0, 0, "OP_GET_FIELD_FAST: 需要 struct 类型");
        return NULL_VAL;
    }
    ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
    return struct_get_field(obj, field_idx);
}

/* Callout: OP_MODULE_CALL (native module method like maths.sqrt).
 * Looks up module+method from callee chunk constants, calls the native
 * function, and returns the result. No VM re-entry needed. */
static Value jit_callout_module_call(int64_t* vstack_top, int arg_count,
                                     uint16_t module_idx, uint16_t method_idx,
                                     Chunk* chunk) {
    (void)jit_callout_vm;  /* not needed for pure native calls */

    if (module_idx >= (uint16_t)chunk->const_cnt ||
        method_idx >= (uint16_t)chunk->const_cnt) {
        error_add_at(ERR_RUNTIME, 0, 0, "OP_MODULE_CALL: 常量索引越界");
        return NULL_VAL;
    }

    Value module_val = chunk->constants[module_idx];
    Value method_val = chunk->constants[method_idx];
    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_STRING ||
        !val_is_obj(method_val) || val_as_obj(method_val)->type != OBJ_STRING) {
        error_add_at(ERR_RUNTIME, 0, 0, "模块方法名必须是字符串");
        return NULL_VAL;
    }

    const char* module_name = ((ObjString*)val_as_obj(module_val))->chars;
    const char* method_name = ((ObjString*)val_as_obj(method_val))->chars;

    ModuleMethodMeta* meta = native_find_module_method(module_name, method_name);
    if (!meta) {
        char msg[256];
        snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s", module_name, method_name);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        return NULL_VAL;
    }

    /* Build Value args from JIT virtual stack raw values.
     * JIT virtual stack grows downward: first-pushed at higher address.
     * vstack_top[0] = TOS (last pushed), vstack_top[arg_count-1] = first pushed.
     * Native function expects args[0] = first argument. */
    Value args[16];
    if (arg_count > 16) {
        error_add_at(ERR_RUNTIME, 0, 0, "模块方法参数过多");
        return NULL_VAL;
    }
    for (int i = 0; i < arg_count; i++) {
        args[i] = jit_raw_to_value(vstack_top[arg_count - 1 - i]);
        if (getenv("LENO_JIT_DEBUG")) {
            fprintf(stderr, "[JIT-DEBUG] module_call arg[%d]: raw=0x%016llx val=0x%016llx is_obj=%d is_int=%d is_float=%d\n",
                    i, (unsigned long long)vstack_top[arg_count - 1 - i],
                    (unsigned long long)args[i],
                    val_is_obj(args[i]), val_is_int(args[i]),
                    (args[i] != val_null() && (args[i] & (QNAN | SIGN_BIT)) != (QNAN | SIGN_BIT)));
        }
    }

    Value result = meta->function(arg_count, args);

    /* Check for exception set by native function */
    if (jit_callout_vm && jit_callout_vm->has_exception) {
        if (getenv("LENO_JIT_DEBUG")) {
            fprintf(stderr, "[JIT-DEBUG] module_call FAILED: %s.%s (has_exception=1)\n", module_name, method_name);
        }
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    return result;
}

/* ---- Cache hash ---- */
static int cache_hash(const uint8_t* ip) {
    uintptr_t v = (uintptr_t)ip;
    return (int)((v >> 4) & (JIT_CACHE_SIZE - 1));
}

/* ---- Opcode instruction size (bytes) ---- */
/* Takes ip (pointer to opcode byte) because some opcodes are variable-length
 * (e.g. OP_ACC_FIELDS has size 2 + count). */
static int opcode_size(const uint8_t* ip) {
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
            return 3;
        /* 5-byte (opcode + slot16 + slot16 or opcode + int32) */
        case OP_MOVE_LOCAL: case OP_MOVE_LOCAL_POP:
        case OP_SET_LOCAL_CONST:
        case OP_JUMP: case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
        case OP_LOOP:
        case OP_TRY:   /* catch_offset(2) + finally_offset(2) */
        case OP_CALL_GLOBAL_FUNC:        /* func_slot(2) + arg_count(2) (callout) */
        case OP_CALL_GLOBAL_FUNC_TYPED:  /* func_slot(2) + arg_count(2) (callout) */
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
            if (getenv("LENO_JIT_DEBUG"))
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
            default:
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: unsupported opcode %d at off %d\n",
                            op, (int)(ip - cc->code));
                return 0;
        }

        if (vstack > JIT_MAX_VSTACK) {
            if (getenv("LENO_JIT_DEBUG"))
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
static void scan_loop_body(const uint8_t* body_start, int body_size,
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
            case OP_DIV:
                /* 通用除法 (callout): pop 2 push 1 → net -1 */
                vstack -= 1;
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
                                        if (getenv("LENO_JIT_DEBUG"))
                                            fprintf(stderr, "[JIT-DEBUG] scan FAIL: max_vstack=%d after inline\n", r->max_vstack);
                                        r->capable = 0;
                                        return;
                                    }
                                    if (getenv("LENO_JIT_DEBUG"))
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
                if (getenv("LENO_JIT_DEBUG"))
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d at offset %d\n", op, (int)(ip - body_start));
                r->capable = 0;
                return;
        }

    /* Allow vstack < 0: nested while loops have an exit OP_POP at the
     * jump target that the linear scan double-counts (true path's POP
     * already decremented; false path's POP decrements again). This is
     * a linear-scan artifact — the actual runtime never goes negative. */
    if (vstack > JIT_MAX_VSTACK) {
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] scan FAIL: vstack=%d at opcode %d\n", vstack, op);
        r->capable = 0;
        return;
    }
        r->max_vstack = vstack > r->max_vstack ? vstack : r->max_vstack;
        scan_next:
        ip += size;
    }

    r->body_size = body_size;

    if (getenv("LENO_JIT_DEBUG")) {
        fprintf(stderr, "[JIT-DEBUG] scan result: n_locals=%d max_vstack=%d\n", r->num_locals, r->max_vstack);
        for (int i = 0; i < r->num_locals; i++) {
            fprintf(stderr, "[JIT-DEBUG]   scratch[%d] = slot %d\n", i, r->local_slots[i]);
        }
        fprintf(stderr, "[JIT-DEBUG]   for_loop_var_slot=%d for_end_slot=%d for_step_slot=%d inclusive=%d\n",
                r->for_loop_var_slot, r->for_end_slot, r->for_step_slot, r->for_inclusive);
    }
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
     * Each OP_RETURN emits a jmp that needs to be patched to inline_end. */
    int inline_ret_patches[64];
    int inline_ret_patch_cnt = 0;

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

    /* Save volatile state, align RSP, allocate shadow space (32) + 16 bytes
     * for stack arguments (e.g. MODULE_CALL's 5th arg at [RSP+32]).
     * Without the extra 16, [RSP+32] would overlap vstack_top[0]. */
    #define EMIT_CALLOUT_BEGIN() do { \
        emit_mov_rr(cb, JIT_R12, JIT_RSP); \
        emit_mov_rr(cb, JIT_R13, JIT_RCX); \
        emit_mov_rr(cb, JIT_R14, JIT_R9); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xE4); emit_byte(cb, 0xF0); \
        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xEC); emit_byte(cb, 0x30); \
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

    /* ---- Step direction check (for OP_FOR_LOOP) ---- */
    if (sr->back_edge_type == 2) {
        int step_scratch = cur_local_map[sr->for_step_slot];
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
        for_loop_entry_patches[for_loop_patch_cnt++] = emit_jcc(cb, cc_exit);
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
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                TOS_PRODUCE();
                vstack++;
                if (getenv("LENO_JIT_DEBUG")) fprintf(stderr, "[JIT-CG] GET_LOCAL slot=%d -> scratch[%d] disp=%d vstack=%d\n", slot, si, disp, vstack);
                break;
            }
            case OP_SET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                /* consume TOS → store to local */
                TOS_CONSUME_RAX();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                vstack--;
                if (getenv("LENO_JIT_DEBUG")) fprintf(stderr, "[JIT-CG] SET_LOCAL_POP slot=%d -> scratch[%d] disp=%d vstack=%d\n", slot, si, disp, vstack);
                break;
            }
            case OP_MOVE_LOCAL: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                int si_src = cur_local_map[src];
                int si_dst = cur_local_map[dst];
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
                int si_src = cur_local_map[src];
                int si_dst = cur_local_map[dst];
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
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                Value cv = cur_chunk->constants[ci];
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
                Value cv = cur_chunk->constants[ci];
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

                /* Non-int value (float or object).
                 * For floats: RAX already has raw double bits (same as NaN-boxed).
                 * For objects (arrays, dicts, strings): RAX has NaN-boxed
                 * value, pushed as-is for callout consumption (OP_INDEX,
                 * OP_ARRAY_APPEND, etc.). EMIT_RAW_TO_VALUE() in callouts
                 * correctly handles non-int48 values by leaving them as-is. */

                /* Float/object path: push RAX as-is */
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
            case OP_DIV: {
                /* 通用除法 callout: 运行时类型分发 (int/int, float 混合, BigInt) */
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* b (divisor) */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* a (dividend) */
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_RCX, JIT_RDX);   /* a */
                emit_mov_rr(cb, JIT_RDX, JIT_R8);    /* b */
                EMIT_CALL(jit_callout_div);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack--;
                break;
            }
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
            /* ---- Callout: OP_INDEX_SET_NOPUSH (arr[idx]=val, no push) ---- */
            case OP_INDEX_SET_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* value (top) */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* index */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* array */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RCX, tmp3_disp);  /* array (1st arg) */
                EMIT_LOAD_TMP(JIT_RDX, tmp2_disp);  /* index (2nd arg) */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);   /* value (3rd arg) */
                EMIT_CALL(jit_callout_index_set);
                EMIT_CALLOUT_END();
                vstack -= 3;
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
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
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
                int si = cur_local_map[slot];
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
                    /* RAX already holds the condition value.
                     * Proper falsey check: NaN-boxed FALSE_VAL (0xFFF9..)
                     * and NULL_VAL (0xFFF8..) are non-zero, so a simple
                     * test rax,rax is insufficient. Must also compare
                     * against FALSE_VAL and NULL_VAL. */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);   /* JZ = falsey (zero: int 0 / float +0.0) */
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);  /* JE = falsey (FALSE_VAL) */
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);  /* JE = falsey (NULL_VAL) */
                    /* Truthy: jump over falsey code */
                    int truthy_jmp = emit_jmp(cb);
                    /* Falsey path: push condition to stack for jump target
                     * (target may have tos_live=0 from linear codegen) */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    emit_push_reg(cb, JIT_RAX);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    /* Patch truthy jump to here */
                    patch_rel32(cb, truthy_jmp, cb->len);
                    /* Fall-through: tos_live stays 1, next OP_POP will TOS_DISCARD */
                } else {
                    /* Value on memory stack: peek + conditional jump (no pop) */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);  /* mov rax, [rsp] */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);
                    /* Truthy: jump over falsey code */
                    int truthy_jmp = emit_jmp(cb);
                    /* Falsey path: value stays on stack for target POP */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    /* Patch truthy jump to here */
                    patch_rel32(cb, truthy_jmp, cb->len);
                    /* Fall-through: value still on stack, next OP_POP pops it */
                }
                break;
            }
            case OP_JUMP_IF_TRUE: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                if (tos_live) {
                    /* RAX already holds the condition value.
                     * If falsey (zero, FALSE_VAL, NULL_VAL): skip jump.
                     * Otherwise (truthy): push and jump to target. */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);   /* JZ = falsey (zero) */
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);  /* JE = falsey (FALSE_VAL) */
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);  /* JE = falsey (NULL_VAL) */
                    /* Truthy: push condition to stack and jump to target */
                    emit_push_reg(cb, JIT_RAX);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    /* Patch all falsey jumps to here (skip) */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    /* Fall-through: tos_live stays 1 */
                } else {
                    /* Value on memory stack: peek + conditional jump (no pop) */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);  /* mov rax, [rsp] */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);
                    /* Truthy: value stays on stack, jump to target */
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, 0);
                    /* Patch all falsey jumps to here (skip) */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    /* Fall-through: value still on stack, next OP_POP pops it */
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
                int si_a = cur_local_map[sa];
                int si_b = cur_local_map[sb];
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

            /* ---- CMPJMP: local int vs global int + conditional jump ---- */
            /* Operands: cmp_op(1) slot(2) global_idx(2) offset(4) = 9 bytes + opcode = 10 */
            case OP_CMPJMP_LG_INT: {
                uint8_t cmp_op = ip[1];
                uint16_t sa = rd_short(ip + 2);
                uint16_t gi = rd_short(ip + 4);
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                int si_a = cur_local_map[sa];
                int da = scratch_disp(si_a);
                int gd = gi * 8;
                TOS_SPILL();  /* clobbers RAX */
                /* Load local (raw int48) from scratch area: mov rax, [rbp+da] */
                if (da >= -128 && da <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)da);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, da);
                }
                /* Load global (NaN-boxed int) from globals: mov rdx, [r9+gd] */
                if (gd >= -128 && gd <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_R9, (int8_t)gd);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_R9, gd);
                }
                /* Extract int48 payload from NaN-boxed value: shl rdx,16; sar rdx,16 */
                {
                    int b = (JIT_RDX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 4, JIT_RDX & 7));  /* /4 = SHL */
                    emit_byte(cb, 16);
                }
                {
                    int b = (JIT_RDX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 7, JIT_RDX & 7));  /* /7 = SAR */
                    emit_byte(cb, 16);
                }
                /* cmp rax, rdx */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Jump if comparison is FALSE (same inversion as CMPJMP_LL_INT) */
                uint8_t cc;
                switch (cmp_op) {
                    case 0: cc = 0x85; break;  /* EQ → JNE */
                    case 1: cc = 0x84; break;  /* NE → JE  */
                    case 2: cc = 0x8D; break;  /* LT → JGE */
                    case 3: cc = 0x8E; break;  /* GT → JLE */
                    case 4: cc = 0x8F; break;  /* LE → JG  */
                    case 5: cc = 0x8C; break;  /* GE → JL  */
                    default: cc = 0x85; break;
                }
                int patch = emit_jcc(cb, cc);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }

            /* ---- Inner for-loop init: OP_FOR_PREP ---- */
            case OP_FOR_PREP: {
                /* 8 bytes: op, start_slot, end_slot, step_slot,
                 *         loop_var_slot, inclusive, jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t start_sl = ip[1];
                uint8_t end_sl   = ip[2];
                uint8_t step_sl  = ip[3];
                uint8_t lv_sl    = ip[4];
                uint8_t incl     = ip[5];
                uint16_t joff    = rd_short(ip + 6);

                int si_start = cur_local_map[start_sl];
                int si_end   = cur_local_map[end_sl];
                int si_step  = cur_local_map[step_sl];
                int si_lv    = cur_local_map[lv_sl];
                int d_start = scratch_disp(si_start);
                int d_end   = scratch_disp(si_end);
                int d_step  = scratch_disp(si_step);
                int d_lv    = scratch_disp(si_lv);

                /* Check step > 0 (bailout if step <= 0) */
                if (d_step >= -128 && d_step <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_step);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                int step_bail = emit_jcc(cb, 0x8E);  /* JLE → bailout */
                patch_add(ctx, step_bail, -1, 0);

                /* Set loop_var = start */
                if (d_start >= -128 && d_start <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_start);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_start);
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);

                /* Check initial condition: start vs end (step > 0) */
                if (d_end >= -128 && d_end <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_end);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_end);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* step > 0, exclusive: JGE (start >= end) → skip inner loop
                 * step > 0, inclusive: JG  (start > end)  → skip inner loop */
                uint8_t cc_skip = incl ? 0x8F /*JG*/ : 0x8D /*JGE*/;
                int skip_patch = emit_jcc(cb, cc_skip);
                /* Forward jump: target = bc_off + 8 + joff (past inner body + FOR_LOOP) */
                int skip_target = bc_off + size + (int)joff;
                patch_add(ctx, skip_patch, skip_target, 0);
                break;
            }

            /* ---- Back-edge: OP_LOOP ---- */
            case OP_LOOP: {
                /* 5 bytes: op + 4-byte big-endian int32 backward offset */
                TOS_SPILL();
                int32_t loff = rd_int32(ip + 1);
                int target_bc = bc_off + size - loff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) target_mc = ctx->loop_start_mc;
                int patch = emit_jmp(cb);
                patch_rel32(cb, patch, target_mc);
                break;
            }

            /* ---- Back-edge: OP_FOR_LOOP ---- */
            case OP_FOR_LOOP: {
                /* 7 bytes: op, lv_slot, step_slot, end_slot, inclusive,
                 *         jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t lv_sl  = ip[1];
                uint8_t st_sl  = ip[2];
                uint8_t en_sl  = ip[3];
                uint8_t incl   = ip[4];
                uint16_t joff  = rd_short(ip + 5);

                int si_lv = cur_local_map[lv_sl];
                int si_st = cur_local_map[st_sl];
                int si_en = cur_local_map[en_sl];
                int d_lv = scratch_disp(si_lv);
                int d_st = scratch_disp(si_st);
                int d_en = scratch_disp(si_en);

                /* Increment loop_var by step */
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv);
                if (d_st >= -128 && d_st <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st);
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);
                /* Compare with end */
                if (d_en >= -128 && d_en <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Conditional jump to inner loop start */
                uint8_t cc = incl ? 0x8E /*JLE*/ : 0x8C /*JL*/;
                int target_bc = bc_off + size - (int)joff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) target_mc = ctx->loop_start_mc;
                int patch = emit_jcc(cb, cc);
                patch_rel32(cb, patch, target_mc);
                break;
            }

            /* ---- Callout: OP_ACC_FIELDS (struct field accumulation) ---- */
            case OP_ACC_FIELDS: {
                /* Pop struct obj, convert to NaN-boxed, save to tmp */
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* struct obj */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);   /* obj (1st arg) */
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)ip[1]);  /* count (2nd arg) */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)(ip + 2));  /* field_indices ptr (3rd) */
                EMIT_CALL(jit_callout_acc_fields);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                break;
            }
            /* ---- Callout: OP_INVOKE_METHOD (struct method call, VM re-entry) ---- */
            case OP_INVOKE_METHOD: {
                int arg_count = rd_short(ip + 3);
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into RCX (1st arg). */
                EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)ip);  /* bytecode ip (3rd) */
                emit_mov_reg_imm64(cb, JIT_R9, (uint64_t)(uintptr_t)ctx->chunk);  /* chunk (4th) */
                EMIT_CALL(jit_callout_invoke_method);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop arg_count values from JIT virtual stack */
                int pop_bytes = arg_count * 8;
                if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

            /* ---- Callout: OP_GET_FIELD_FAST (read local struct + get field) ---- */
            case OP_GET_FIELD_FAST: {
                uint16_t slot = rd_short(ip + 1);
                uint8_t field_idx = ip[3];
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                /* Load local (struct obj raw) from scratch area */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);  /* obj_val (1st arg) */
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)field_idx);  /* field_idx (2nd arg) */
                EMIT_CALL(jit_callout_get_field_fast);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack++;
                break;
            }

            /* ---- Callout: OP_CALL_GLOBAL_FUNC / OP_CALL_GLOBAL_FUNC_TYPED ---- */
            case OP_CALL_GLOBAL_FUNC:
            case OP_CALL_GLOBAL_FUNC_TYPED: {
                uint16_t func_slot = rd_short(ip + 1);
                int arg_count = rd_short(ip + 3);

                /* ---- Check if this call site is marked for inlining ---- */
                if (op == OP_CALL_GLOBAL_FUNC_TYPED) {
                    int inline_idx = FIND_INLINE_SITE(bc_off);
                    if (inline_idx >= 0) {
                        const InlineSite* is = &sr->inline_sites[inline_idx];
                        /* Spill TOS to memory stack */
                        TOS_SPILL();
                        /* Pop args from vstack and store to callee locals.
                         * Args are on the stack in order: last arg = TOS.
                         * Callee local slot 0 = first arg, slot (arg_count-1) = last arg.
                         * Pop in reverse: last arg first (slot arg_count-1), ..., first arg last (slot 0). */
                        for (int i = arg_count - 1; i >= 0; i--) {
                            /* pop rax */
                            emit_byte(cb, 0x58);  /* pop rax */
                            /* store to callee local slot i */
                            int si = is->callee_local_map[i];
                            if (si < 0) {
                                /* shouldn't happen: scan pre-mapped all locals */
                                if (getenv("LENO_JIT_DEBUG"))
                                    fprintf(stderr, "[JIT-DEBUG] inline FAIL: unmapped callee slot %d\n", i);
                                return 0;
                            }
                            int disp = scratch_disp(si);
                            if (disp >= -128 && disp <= 127)
                                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                            else
                                emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                        }
                        vstack -= arg_count;

                        /* Save caller context and switch to callee */
                        InlineFrame* f = &inline_frames[inline_depth];
                        f->ip = ip + size;  /* next instruction after the call */
                        f->end = end;
                        f->local_map = cur_local_map;
                        f->chunk = cur_chunk;
                        f->bc_off = bc_off + size;
                        f->vstack = vstack + arg_count;  /* vstack before args were popped */
                        f->tos_live = 0;  /* TOS already spilled */
                        f->callee_ret_count = is->ret_count;
                        f->callee_arg_count = arg_count;
                        inline_depth++;

                        /* Switch to callee bytecode.
                         * Use a large bc_off base to avoid offmap conflicts
                         * with the caller's bytecode offsets. */
                        ip = is->callee_chunk->code;
                        end = ip + is->callee_body_size;
                        cur_local_map = is->callee_local_map;
                        cur_chunk = is->callee_chunk;
                        bc_off = 0x10000 * inline_depth;
                        vstack = 0;
                        tos_live = 0;

                        if (getenv("LENO_JIT_DEBUG"))
                            fprintf(stderr, "[JIT-DEBUG] inline ENTER: func_slot=%d arg_count=%d ret_count=%d\n",
                                    func_slot, arg_count, is->ret_count);
                        continue;  /* skip normal ip+=size advance */
                    }
                }

                /* ---- Non-inline: callout path (original code) ---- */
                int ret_count = 1;
                if (op == OP_CALL_GLOBAL_FUNC_TYPED && ctx->vm_ptr) {
                    if (func_slot < ctx->vm_ptr->global_func_capacity) {
                        Value callee = ctx->vm_ptr->global_funcs[func_slot];
                        if (val_is_obj(callee)) {
                            Object* obj2 = val_as_obj(callee);
                            ObjFunction* func2 = NULL;
                            if (obj2->type == OBJ_CLOSURE)
                                func2 = ((ObjClosure*)obj2)->function;
                            else if (obj2->type == OBJ_FUNCTION)
                                func2 = (ObjFunction*)obj2;
                            if (func2 && func2->return_count > 1)
                                ret_count = func2->return_count;
                        }
                    }
                }
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into RCX (1st arg). */
                EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)func_slot);   /* func_slot (3rd) */
                EMIT_CALL(jit_callout_global_func);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out.
                 * RAX holds the callout return value; save it, check flag, restore. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);  /* RAX = *flag */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);  /* bailout */
                }
                /* Clear flag */
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);  /* *flag = 0 */
                /* Restore callout return value */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop (arg_count - ret_count + 1) args from JIT virtual stack.
                 * The remaining (ret_count - 1) slots hold the extra return
                 * values that the callout wrote to vstack_top. */
                int pop_bytes = (arg_count - ret_count + 1) * 8;
                if (pop_bytes <= 0) {
                    /* ret_count >= arg_count + 1: no pop needed (or even push) */
                } else if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - ret_count);
                break;
            }

            /* ---- Inline return handling ---- */
            case OP_RETURN: {
                if (inline_depth > 0) {
                    /* In inline mode, OP_RETURN's value is already on vstack.
                     * We just emit a jump to the inline_end label (to be patched
                     * when the callee body ends). The return value stays on the
                     * virtual stack as the call result. */
                    TOS_SPILL();  /* ensure value is on memory stack */
                    /* Emit: jmp <inline_end> (rel32 placeholder) */
                    emit_byte(cb, 0xE9);
                    int patch_loc = cb->len;
                    emit_uint32(cb, 0);
                    inline_ret_patches[inline_ret_patch_cnt++] = patch_loc;
                    /* Match scan phase: vstack-- (return value "consumed").
                     * The value stays on the hardware stack for inline_end,
                     * but vstack tracking must match scan for dead code after
                     * the return to have correct vstack. */
                    vstack--;
                } else {
                    /* Shouldn't happen in loop body, but handle gracefully */
                    TOS_SPILL();
                    vstack--;
                }
                break;
            }
            case OP_RETURN_MULTI: {
                uint8_t rc = ip[1];
                if (inline_depth > 0) {
                    /* Multiple return values are on vstack. Emit jump to inline_end. */
                    TOS_SPILL();
                    emit_byte(cb, 0xE9);
                    int patch_loc = cb->len;
                    emit_uint32(cb, 0);
                    inline_ret_patches[inline_ret_patch_cnt++] = patch_loc;
                    /* Match scan phase: vstack -= rc.
                     * The return values stay on the hardware stack for
                     * inline_end, but vstack tracking must match scan for
                     * dead code after the return. */
                    vstack -= rc;
                } else {
                    TOS_SPILL();
                    vstack -= rc;
                }
                break;
            }

            /* ---- Module call (e.g., maths.sqrt) ---- */
            case OP_MODULE_CALL: {
                uint16_t module_idx = rd_short(ip + 1);
                uint16_t method_idx = rd_short(ip + 3);
                int arg_count = rd_short(ip + 5);
                TOS_SPILL();
                /* Save RSP before callout */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);  /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)arg_count);    /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)module_idx);   /* module_idx (3rd) */
                emit_mov_reg_imm64(cb, JIT_R9, (uint64_t)method_idx);   /* method_idx (4th) */
                /* 5th param (chunk ptr) in shadow space [rsp+32] */
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)(uintptr_t)cur_chunk);
                emit_mov_mem8_reg(cb, JIT_RSP, 32, JIT_RAX);
                EMIT_CALL(jit_callout_module_call);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop arg_count args, push 1 result */
                int pop_bytes2 = arg_count * 8;
                if (pop_bytes2 <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes2);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes2);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

            /* ---- Simple constant pushes ---- */
            case OP_NULL:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, 0);  /* NULL = 0 */
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_TRUE:
                TOS_SPILL();
                /* Leno true value = NaN-boxed bool true */
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)0xFFFFFFFFFFFF0003ULL);
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_FALSE:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)0xFFFFFFFFFFFF0001ULL);
                TOS_PRODUCE();
                vstack++;
                break;

            /* ---- Exception handling opcodes (no-op in JIT) ---- */
            /* The JIT skips try/catch/finally setup entirely.
             * In normal execution (no exception), these are effectively
             * no-ops: OP_TRY sets up catch_ip (skipped), OP_JUMP after
             * try body skips the catch body, OP_END_TRY restores state.
             * If an exception occurs from a callout, the JIT bails out
             * and the interpreter re-executes with full try/catch. */
            case OP_TRY:
            case OP_CATCH:
            case OP_FINALLY:
            case OP_END_TRY:
                break;

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
            /* Int path: load scratch, re-encode as NaN-boxed int */ \
            if (disp >= -128 && disp <= 127) \
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp); \
            else \
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp); \
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
            /* .next: patch jmp to here */ \
            cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1)); \
        } \
    } while(0)

    /* Reload RCX from jit_reloaded_locals before writeback, in case
     * vm_grow_frames reallocated vm.frames during a callout (e.g. deep
     * recursion via OP_CALL_GLOBAL_FUNC_TYPED or OP_INVOKE_METHOD).
     * Without this, RCX still points to the pre-callout (now freed)
     * frame->locals, and the writeback below writes to freed memory. */
    #define EMIT_RELOAD_RCX() do { \
        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_reloaded_locals); \
        emit_mov_reg_mem8(cb, JIT_RCX, JIT_R8, 0); \
    } while(0)

    EMIT_RELOAD_RCX();
    EMIT_WRITEBACK_LOCALS();

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
    /* DEBUG: save RAX to global, then call debug function */
    if (getenv("LENO_JIT_DEBUG")) {
        /* Store RAX to global jit_bailout_rax */
        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_bailout_rax);
        /* mov [r8], rax */
        emit_byte(cb, 0x49); emit_byte(cb, 0x89); emit_byte(cb, 0x00);  /* mov [r8], rax */
        /* Save RSP and RAX before callout */
        EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
        EMIT_CALLOUT_BEGIN();
        EMIT_LOAD_TMP(JIT_RCX, tmp1_disp);
        EMIT_CALL(jit_bailout_debug);
        EMIT_CALLOUT_END();
    }
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

    /* ---- Frame-dead exit: callout 异常且宿主帧存活（catch_ip 已定向）----
     * 写回 locals 后返回 2；调用方（OP_LOOP/OP_FOR_LOOP handler）
     * 重载 frame 后 DISPATCH，从宿主帧 catch_ip 继续执行 */
    ctx->framedead_mc = cb->len;
    EMIT_RELOAD_RCX();
    EMIT_WRITEBACK_LOCALS();
    /* Return 2 (frame-dead, host frame alive, locals written back) */
    emit_mov_eax_imm32(cb, 2);
    /* Epilogue: mov rsp, rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret */
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP);
    emit_pop_reg(cb, JIT_R14);
    emit_pop_reg(cb, JIT_R13);
    emit_pop_reg(cb, JIT_R12);
    emit_pop_reg(cb, JIT_RBX);
    emit_pop_rbp(cb);
    emit_ret(cb);

    /* ---- Frame-dead exit: callout 异常且宿主帧已被展开 ----
     * 宿主帧 locals 已被异常展开释放，禁止写回；返回 3；
     * 调用方重载 frame（外层 handler 帧）后 DISPATCH */
    ctx->framedead_nowb_mc = cb->len;
    /* Return 3 (frame-dead, host frame destroyed, no write back) */
    emit_mov_eax_imm32(cb, 3);
    /* Epilogue: mov rsp, rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret */
    emit_rr(cb, 0x89, JIT_RSP, JIT_RBP);
    emit_pop_reg(cb, JIT_R14);
    emit_pop_reg(cb, JIT_R13);
    emit_pop_reg(cb, JIT_R12);
    emit_pop_reg(cb, JIT_RBX);
    emit_pop_rbp(cb);
    emit_ret(cb);

    #undef EMIT_WRITEBACK_LOCALS

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
             * Inlined callee offsets use bc_off = 0x10000 * inline_depth,
             * so target_bc can far exceed sr->body_size.
             * Rely on offmap_lookup to find valid targets. */
            int target_mc = offmap_lookup(ctx, p->target_bc);
            if (target_mc >= 0) {
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
                             int body_size, int back_edge, VM* vm_ptr) {
    ScanResult sr;
    scan_loop_body(body_start, body_size, back_edge, &sr, vm_ptr);
    if (getenv("LENO_JIT_DEBUG")) {
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
        for (int i = 0; i < body_size && i < 200; i++) {
            fprintf(stderr, " %02x", body_start[i]);
        }
        fprintf(stderr, "\n");
        fprintf(stderr, "[JIT-DEBUG] body opcodes:");
        const uint8_t* p = body_start;
        int printed = 0;
        while (p < body_start + body_size && printed < 60) {
            int op = *p;
            int sz = opcode_size(p);
            fprintf(stderr, " %d", op);
            if (sz < 0) break;
            /* Also print operands for key opcodes */
            if (sz >= 3) {
                fprintf(stderr, "(");
                for (int j = 1; j < sz && j <= 6; j++)
                    fprintf(stderr, "%d%s", p[j], j < sz-1 && j < 6 ? "," : "");
                fprintf(stderr, ")");
            }
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
        entry->fn = jit_compile(frame, body_start, body_size, back_edge, vm_ptr);
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
    /* Initialize reloaded locals to current value; callouts will update
     * this if vm_grow_frames reallocates vm.frames during nested execution. */
    jit_reloaded_locals = vm_ptr->frames[vm_ptr->frame_cnt - 1].locals;
    if (getenv("LENO_JIT_DEBUG"))
        fprintf(stderr, "[JIT-DEBUG] EXEC call #%d, fn=%p, locals=%p\n",
                jit_state.execute_count, (void*)entry->fn, (void*)frame->locals);
    int result = entry->fn(frame->locals, vm_ptr->globals);
    if (getenv("LENO_JIT_DEBUG")) {
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
        if (getenv("LENO_JIT_DEBUG"))
            fprintf(stderr, "[JIT-DEBUG] FRAME-DEAD exit (%d) at body_start=%d\n",
                    result, (int)(body_start - frame->chunk->code));
        return 2;
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
