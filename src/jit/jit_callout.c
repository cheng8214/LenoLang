/*
 * jit_callout.c - JIT 运行时辅助：全局状态、NaN-box 转换、callout C helpers
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

/* ---- Global state ---- */
JitState jit_state = {0};

/* ---- Callout VM pointer (set before JIT execution) ---- */
VM* jit_callout_vm = NULL;

/* ---- Callout failure flag ----
 * Set by callouts when vm_call_value fails (exception thrown, not caught).
 * JIT codegen checks this after each callout that calls vm_call_value,
 * and bails out to the interpreter if set. */
volatile int jit_callout_failed = 0;

/* ---- Debug flags: read once (getenv() scans the environment block on every
 * call — far too slow for JIT hot paths that run millions of times). ---- */
int jit_debug_on(void) {
    static int v = -1;
    if (v < 0) v = getenv("LENO_JIT_DEBUG") ? 1 : 0;
    return v;
}

/* ---- Bailout debug function ---- */
int64_t jit_bailout_rax = 0;
int32_t jit_bailout_site = 0;
void jit_bailout_debug(int64_t rsp_val) {
    fprintf(stderr, "[JIT-DEBUG] BAILOUT site=%d RSP=%lld (0x%llx) RAX=%lld (0x%llx)\n",
            (int)jit_bailout_site,
            (long long)rsp_val, (unsigned long long)rsp_val,
            (long long)jit_bailout_rax, (unsigned long long)jit_bailout_rax);
}

/* ---- Reloaded locals pointer (updated by callouts after vm_call_value,
 * in case vm_grow_frames reallocates vm.frames; JIT writeback reloads
 * RCX from this before storing locals back) ---- */
Value* jit_reloaded_locals = NULL;

/* ---- Bytecode operand readers (big-endian, matching VM) ---- */

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
Value jit_callout_index(Value obj_val, Value idx_val) {
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
int jit_callout_array_append(Value arr_val, Value value) {
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
Value jit_callout_dict_set(Value dict_val, Value key_val, Value value) {
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
int jit_callout_index_set(Value obj_val, Value idx_val, Value value) {
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
Value jit_callout_div(Value a, Value b) {
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
Value jit_callout_acc_fields(Value obj_val, uint8_t count,
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
Value jit_callout_invoke_method(int64_t* vstack_top, int arg_count,
                                       const uint8_t* ip, Chunk* chunk) {
VM* vm = jit_callout_vm;
    if (!vm) return NULL_VAL;

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
Value jit_callout_global_func(int64_t* vstack_top, int arg_count,
                                     uint16_t func_slot) {
    VM* vm = jit_callout_vm;
    if (!vm) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] global_func: jit_callout_vm is NULL!\n");
        return NULL_VAL;
    }

    if (func_slot >= vm->global_func_capacity) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-DEBUG] global_func: slot %d >= capacity %d\n", func_slot, vm->global_func_capacity);
        error_add_at(ERR_RUNTIME, 0, 0, "全局函数索引越界");
        return NULL_VAL;
    }

    Value callee = vm->global_funcs[func_slot];
    if (!val_is_obj(callee)) {
        if (jit_debug_on())
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
        if (jit_debug_on()) {
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
Value jit_callout_get_field_fast(Value obj_val, uint8_t field_idx) {
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
Value jit_callout_module_call(int64_t* vstack_top, int arg_count,
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
    }

    Value result = meta->function(arg_count, args);

    /* Check for exception set by native function */
    if (jit_callout_vm && jit_callout_vm->has_exception) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    return result;
}

