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

/* ---- Function-level JIT state ----
 * jit_fn_result: 函数级 JIT 机器码通过 OP_RETURN 写入的返回值（单返回）。
 * jit_func_depth: 当前 JIT 函数嵌套深度（递归保护：超过上限回退解释路径，
 * 避免 JIT 机器码无限递归耗尽 C 栈）。 */
Value jit_fn_result = NULL_VAL;
int jit_func_depth = 0;
Value jit_func_locals_pool[JIT_FUNC_MAX_DEPTH][JIT_MAX_LOCALS];

#ifdef _WIN32
#include <windows.h>
static double jit_ft_accum_pre = 0, jit_ft_accum_call = 0, jit_ft_accum_post = 0;
static long long jit_ft_count = 0;
static int jit_ft_prof = -1;
static double jit_ft_qpc_freq(void) {
    static double f = 0;
    if (f == 0) { LARGE_INTEGER q; QueryPerformanceFrequency(&q); f = (double)q.QuadPart; }
    return f;
}
#define JIT_FT_PROF_ON() (jit_ft_prof < 0 ? (jit_ft_prof = getenv("LENO_JIT_FPROF") ? 1 : 0) : jit_ft_prof)
#define JIT_FT_T0() LARGE_INTEGER _t0, _t1, _t2, _t3; if (JIT_FT_PROF_ON()) QueryPerformanceCounter(&_t0)
#define JIT_FT_T1() if (JIT_FT_PROF_ON()) QueryPerformanceCounter(&_t1)
#define JIT_FT_T2() if (JIT_FT_PROF_ON()) QueryPerformanceCounter(&_t2)
#define JIT_FT_T3() if (JIT_FT_PROF_ON()) QueryPerformanceCounter(&_t3)
#define JIT_FT_ACC() do { \
    if (JIT_FT_PROF_ON()) { \
        jit_ft_accum_pre += (double)(_t1.QuadPart - _t0.QuadPart) / jit_ft_qpc_freq(); \
        jit_ft_accum_call += (double)(_t2.QuadPart - _t1.QuadPart) / jit_ft_qpc_freq(); \
        jit_ft_accum_post += (double)(_t3.QuadPart - _t2.QuadPart) / jit_ft_qpc_freq(); \
        jit_ft_count++; \
    } \
} while(0)
/* 打印累计函数 JIT 调用开销分布（退出时由主程序调用） */
void jit_ft_profile_dump(void) {
    if (jit_ft_count > 0) {
        fprintf(stderr, "[FT-PROF] calls=%lld pre=%.4fs call=%.4fs post=%.4fs total=%.4fs (%.2fus/call)\n",
                (long long)jit_ft_count, jit_ft_accum_pre, jit_ft_accum_call,
                jit_ft_accum_post, jit_ft_accum_pre + jit_ft_accum_call + jit_ft_accum_post,
                (jit_ft_accum_pre + jit_ft_accum_call + jit_ft_accum_post) * 1e6 / (double)jit_ft_count);
    }
}
#else
#define JIT_FT_T0()
#define JIT_FT_T1()
#define JIT_FT_T2()
#define JIT_FT_T3()
#define JIT_FT_ACC()
#endif
/* static 一次性读取的 FT 标志（热路径避免 getenv 扫描环境块） */
static int jit_ft_trace = -1;
#define JIT_FT_TRACE_ON() (jit_ft_trace < 0 ? (jit_ft_trace = getenv("LENO_JIT_FTRACE") ? 1 : 0) : jit_ft_trace)

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
    if (obj->type == OBJ_STRING) {
        /* 对齐解释器 OP_INDEX 字符串分支：str[i] 返回单字符子串（UTF-8 感知） */
        if (!val_is_num(idx_val)) {
            error_add_at(ERR_RUNTIME, 0, 0, "字符串索引必须是数字");
            return NULL_VAL;
        }
        ObjString* str = (ObjString*)obj;
        int index = (int)value_to_double(idx_val);
        if (index < 0 || index >= str->char_len) {
            error_add_at(ERR_RUNTIME, 0, 0, "字符串索引越界");
            return NULL_VAL;
        }
        int byte_offset = utf8_char_offset(str->chars, str->len, index);
        int char_bytes = utf8_char_byte_len(str->chars, str->len, byte_offset);
        ObjString* result = str_new(&str->chars[byte_offset], char_bytes);
        return val_obj((Object*)result);
    }
    error_add_at(ERR_RUNTIME, 0, 0, "索引操作需要数组、字典或字符串");
    return NULL_VAL;
}

/* Callout: OP_ADD 字符串拼接（对齐解释器 OP_ADD 语义）。
 * 返回拼接结果字符串；两个操作数都不是字符串（float 等）时返回 NULL_VAL，
 * 由 codegen 决定 bailout 回解释器（语义不变，仅性能回退）。 */
Value jit_callout_concat(Value a, Value b) {
    /* 任一操作数为 ObjString → 字符串拼接路径 */
    if ((val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING) ||
        (val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING)) {
        if (val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING &&
            val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING) {
            ObjString* result = str_concat((ObjString*)val_as_obj(a), (ObjString*)val_as_obj(b));
            return val_obj((Object*)result);
        }
        int a_is_str = val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING;
        ObjString* str_obj = a_is_str ? (ObjString*)val_as_obj(a) : (ObjString*)val_as_obj(b);
        Value other = a_is_str ? b : a;
        char* other_str = value_to_string(other);
        int other_len = (int)strlen(other_str);
        int total_len = str_obj->len + other_len;
        char* buf = (char*)malloc((size_t)total_len + 1);
        if (!buf) {
            free(other_str);
            return NULL_VAL;
        }
        if (a_is_str) {
            memcpy(buf, str_obj->chars, (size_t)str_obj->len);
            memcpy(buf + str_obj->len, other_str, (size_t)other_len);
        } else {
            memcpy(buf, other_str, (size_t)other_len);
            memcpy(buf + other_len, str_obj->chars, (size_t)str_obj->len);
        }
        buf[total_len] = '\0';
        ObjString* result = str_new(buf, total_len);
        free(buf);
        free(other_str);
        return val_obj((Object*)result);
    }
    /* 双方都非字符串：交给解释器（bailout），避免在此处复刻 float/bigint/null 全语义 */
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
/* Callout: OP_STRUCT_INIT (struct 实例构造，函数级 JIT / 循环 JIT 共用)。
 * 直接构造实例、设置字段并返回（不重入 VM）——与 VM 的 OP_STRUCT_INIT
 * 语义一致（无构造函数时：查 def → struct_instance_new → 设字段 → push）。
 *
 * 参数：
 *   vstack_top   JIT 虚拟栈顶（实参按调用顺序从 vstack_top[arg_count-1] 到
 *                vstack_top[0]，vstack_top[0] = 最后一个实参 = 原生 TOS）
 *   name_idx     struct 名常量索引（chunk->constants[name_idx]）
 *   arg_count    参数字段数量（= ip[3]）
 *   ip           指向 OP_STRUCT_INIT 操作码（含后续操作数）
 *   chunk        所在 chunk（读常量用）
 *
 * 返回：新实例 Value；失败返回 NULL_VAL（jit_callout_failed 置 1 → JIT bailout，
 * 由解释器重放保证 ctor/错误语义完整）。
 */
Value jit_callout_struct_init(int64_t* vstack_top, uint16_t name_const_idx,
                              uint8_t arg_count, const uint8_t* ip, Chunk* chunk) {
    VM* vm = jit_callout_vm;
    if (!vm) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    /* 泛型构造（generic_type_count = ip[4]）：scan 阶段已拒绝；
     * 防御性检查，不应到达。 */
    if (ip[4] > 0) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    Value name_val = chunk->constants[name_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        error_add_at(ERR_RUNTIME, 0, 0, "结构体名称必须是字符串");
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    ObjStructDef* def = struct_def_find(name->chars);
    if (!def) {
        char msg[256];
        snprintf(msg, sizeof(msg), "未定义的结构体 '%s'", name->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    /* 有构造函数：callout 无法在返回前自动调用 ctor（需重入 VM 执行方法体），
     * 置 failed 让 JIT bailout，解释器完整执行 ctor 语义。 */
    if (def->has_ctor) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    ObjStruct* obj = struct_instance_new(def);

    /* 字段索引字节位于 ip + 5 + 2*generic_count 起，共 arg_count 个。
     * codegen 反序生成字段索引+按调用顺序压栈，VM 循环内第 i 次 pop 的
     * 值即第 i 个字段索引对应的实参；JIT 栈上 vstack_top[i] 正是
     * 第 i 次 pop 会取到的值（vstack_top[0] = 最后入栈实参）。 */
    const uint8_t* field_ip = ip + 5;
    for (int i = 0; i < arg_count; i++) {
        uint8_t field_idx = field_ip[i];
        if (field_idx >= def->field_count) {
            char msg[256];
            snprintf(msg, sizeof(msg), "struct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", (int)field_idx, def->field_count);
            error_add_at(ERR_RUNTIME, 0, 0, msg);
            jit_callout_failed = 1;
            return NULL_VAL;
        }
        Value field_value = jit_raw_to_value(vstack_top[i]);
        /* int → float 自动提升（与 VM OP_STRUCT_INIT 一致） */
        TypeKind expected_type = def->fields[field_idx].type;
        if (expected_type == TYPE_FLOAT && val_is_int(field_value)) {
            field_value = val_float((double)val_as_int(field_value));
        }
        struct_set_field(obj, field_idx, field_value);
    }

    return val_obj((Object*)obj);
}

/* Callout: OP_INVOKE_METHOD (struct method call, VM re-entry) ---- */
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

    /* Receiver = first arg (JIT vstack: receiver pushed first = HIGHER addr).
     * 不 push VM 栈——快路径与回退路径各自按需处理。 */
    Value obj_val = jit_raw_to_value(vstack_top[arg_count - 1]);
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_STRUCT) {
        char msg[256];
        snprintf(msg, sizeof(msg), "尝试在非 struct 类型上调用方法 '%s'", method_name->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
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
        return NULL_VAL;
    }

    /* ---- 函数级 JIT 快路径 ----
     * 方法整体已编译为机器码 fn(locals, globals) 时，直接执行它，
     * 跳过解释器 VM frame push/pop + 字节码分发循环。
     * locals 是临时数组：locals[0] = self(接收者)，locals[1..arg_count-1]
     * = 其余实参，其余 slot 置 NULL_VAL（VM 语义：未定义 slot 为 NULL）。
     * 返回 0 且无失败标志 → jit_fn_result 即返回值。
     * 任何不成功情况 → 回退到下方 VM 重入路径（栈/VM 状态在快路径中
     * 保持不变：只有 jit_callout_failed 可能被内部嵌套 callout 设置，
     * 回退前必须复位）。 */
    {
        ObjFunction* mfunc = closure->function;
        if (mfunc && jit_state.enabled && jit_func_depth < JIT_FUNC_MAX_DEPTH) {
            JIT_FT_T0();
            JitLoopFn jfn = jit_func_lookup_or_compile(mfunc, vm);
            if (jfn) {
                int lcount = mfunc->local_count > mfunc->arity
                                 ? mfunc->local_count : mfunc->arity;
                if (lcount < arg_count) lcount = arg_count;
                if (lcount > JIT_MAX_LOCALS) lcount = JIT_MAX_LOCALS;
                Value* flocals = jit_func_locals_pool[jit_func_depth];
                for (int i = 0; i < lcount; i++) flocals[i] = NULL_VAL;
                /* JIT 栈：vstack_top[0]=TOS=最后实参；函数参数 slot 0=第一个实参。 */
                for (int i = 0; i < arg_count && i < lcount; i++) {
                    flocals[i] = jit_raw_to_value(vstack_top[arg_count - 1 - i]);
                }
                jit_func_depth++;
                jit_fn_result = NULL_VAL;
                if (JIT_FT_TRACE_ON()) {
                    fprintf(stderr, "[FT] jfn=%p func='%s' lc=%d ac=%d depth=%d\n",
                            (void*)jfn, mfunc ? (mfunc->name ? mfunc->name : "?") : "?", lcount, arg_count, jit_func_depth);
                }
                JIT_FT_T1();
                int jr = jfn(flocals, vm->globals);
                JIT_FT_T2();
                jit_func_depth--;
                if (JIT_FT_TRACE_ON())
                    fprintf(stderr, "[FT] jfn done jr=%d failed=%d result=%p depth=%d\n",
                            jr, jit_callout_failed, (void*)(uintptr_t)jit_fn_result, jit_func_depth);
                if (jr == 0 && !jit_callout_failed) {
                    jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
                    JIT_FT_T3();
                    JIT_FT_ACC();
                    return jit_fn_result;
                }
                jit_callout_failed = 0;  /* 回退解释路径前复位 */
            }
        }
    }

    /* ---- VM 重入路径（慢路径）：push args + callee，调用解释器 ---- */
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
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] invoke_method: vm_call_value failed\n");
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

/* ---- 函数级 JIT 快路径 ----
     * 全局函数整体已编译为机器码 fn(locals, globals) 时直接执行，
     * 跳过解释器 VM frame push/pop + 字节码分发循环。
     * 仅支持单返回值（ret_count == 1）；多返回值回退 VM 路径。
     * locals 临时数组：slot 0..arg_count-1 = 实参，其余置 NULL_VAL。 */
    {
        ObjFunction* gfunc = NULL;
        if (obj->type == OBJ_CLOSURE) {
            ObjClosure* clo = (ObjClosure*)obj;
            if (clo->function) gfunc = clo->function;
        } else if (obj->type == OBJ_FUNCTION) {
            gfunc = (ObjFunction*)obj;
        }
        if (gfunc && ret_count == 1 && jit_state.enabled && jit_func_depth < JIT_FUNC_MAX_DEPTH) {
            JitLoopFn jfn = jit_func_lookup_or_compile(gfunc, vm);
            if (jfn) {
                int lcount = gfunc->local_count > gfunc->arity
                                 ? gfunc->local_count : gfunc->arity;
                if (lcount < arg_count) lcount = arg_count;
                if (lcount > JIT_MAX_LOCALS) lcount = JIT_MAX_LOCALS;
                Value* flocals = jit_func_locals_pool[jit_func_depth];
                for (int i = 0; i < lcount; i++) flocals[i] = NULL_VAL;
                for (int i = 0; i < arg_count && i < lcount; i++) {
                    flocals[i] = jit_raw_to_value(vstack_top[arg_count - 1 - i]);
                }
                jit_func_depth++;
                jit_fn_result = NULL_VAL;
                if (JIT_FT_TRACE_ON()) {
                    fprintf(stderr, "[FT-G] jfn=%p func='%s' lc=%d ac=%d depth=%d\n",
                            (void*)jfn, gfunc->name ? gfunc->name : "?", lcount, arg_count, jit_func_depth);
                }
                int jr = jfn(flocals, vm->globals);
                jit_func_depth--;
                if (JIT_FT_TRACE_ON())
                    fprintf(stderr, "[FT] jfn done jr=%d failed=%d result=%p depth=%d\n",
                            jr, jit_callout_failed, (void*)(uintptr_t)jit_fn_result, jit_func_depth);
                if (jr == 0 && !jit_callout_failed) {
                    if (vm->frame_cnt > 0) {
                        jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
                    }
                    return jit_fn_result;
                }
                jit_callout_failed = 0;  /* 回退解释路径前复位 */
            }
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
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] global_func: vm_call_value failed\n");
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
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] module_call: native raised exception\n");
        return NULL_VAL;
    }

    return result;
}

/* ---- Local equivalents of vm_ic.inc helpers (those are static inline in
 * ---- the VM translation unit and NOT visible from here) ---- */
static ObjType jitc_receiver_type(Value receiver) {
    if (val_is_int(receiver)) return OBJ_INT;
    if (val_is_float(receiver)) return OBJ_FLOAT;
    if (val_is_obj(receiver)) return val_as_obj(receiver)->type;
    return OBJ_NONE;
}

static ObjNative* jitc_find_method(ObjType type, const char* name) {
    switch (type) {
        case OBJ_ARRAY:   return array_find_method(name);
        case OBJ_STRING:  return string_find_method(name);
        case OBJ_DICT:    return dict_find_method(name);
        case OBJ_FILE:    return file_find_method(name);
        case OBJ_SOCKET:  return socket_find_method(name);
        case OBJ_STRUCT:  return struct_find_method(name);
        case OBJ_CSTRUCT_DEF:
        case OBJ_CSTRUCT: return cstruct_find_method(name);
        case OBJ_THREAD:  return thread_find_method(name);
        case OBJ_CHANNEL: return channel_find_method(name);
        case OBJ_INT:
        case OBJ_FLOAT:
        case OBJ_BIGINT:  return number_find_method(name);
        default:          return NULL;
    }
}

/* Callout: OP_ARRAY (array literal [e1, ..., eN]).
 * JIT virtual stack grows downward: vstack_top[0] = LAST pushed element,
 * vstack_top[count-1] = FIRST pushed element. VM wants [e1..eN] in order. */
Value jit_callout_array_new(int64_t* vstack_top, uint16_t count) {
    VM* vm = jit_callout_vm;
    if (!vm) return NULL_VAL;

    ObjArray* arr = arr_new(count);
    if (!arr) {
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] array_new: alloc failed\n");
        return NULL_VAL;
    }
    for (int i = 0; i < count; i++) {
        arr->elements[i] = jit_raw_to_value(vstack_top[count - 1 - i]);
    }
    arr->count = count;

    /* Set runtime element type info (mirror VM OP_ARRAY) */
    if (count > 0) {
        Value first = arr->elements[0];
        int all_same = 1;
        for (int i = 1; i < count; i++) {
            if (val_get_type(arr->elements[i]) != val_get_type(first)) {
                all_same = 0;
                break;
            }
        }
        if (all_same) {
            TypeKind elem_kind = TYPE_ANY;
            switch (val_get_type(first)) {
                case VAL_NULL:  elem_kind = TYPE_NULL; break;
                case VAL_BOOL:  elem_kind = TYPE_BOOL; break;
                case VAL_INT:   elem_kind = TYPE_INT; break;
                case VAL_FLOAT: elem_kind = TYPE_FLOAT; break;
                case VAL_OBJ: {
                    switch (val_as_obj(first)->type) {
                        case OBJ_STRING: elem_kind = TYPE_STRING; break;
                        case OBJ_ARRAY:  elem_kind = TYPE_ARRAY; break;
                        case OBJ_DICT:   elem_kind = TYPE_DICT; break;
                        default: break;
                    }
                    break;
                }
            }
            if (elem_kind != TYPE_ANY) {
                arr->type_info = type_get_array_cached(elem_kind);
            }
        }
    }
    return val_obj((Object*)arr);
}

/* Callout: OP_CALL_NATIVE (direct native function call, e.g. sha256_init).
 * Args are on the JIT virtual stack: vstack_top[0] = last arg.
 * native was resolved at compile time (see codegen) to avoid a strcmp scan
 * of the global native table on every execution. */
Value jit_callout_call_native(int64_t* vstack_top, ObjNative* native,
                              uint16_t arg_count) {
    VM* vm = jit_callout_vm;
    if (!vm || !native) return NULL_VAL;

    /* Push args onto the VM stack (GC roots during the call), in order:
     * VM stack [arg1..argN] = JIT vstack_top[N-1..0]. */
    int saved_sp = vm->sp;
    for (int i = 0; i < arg_count; i++) {
        vm_stack_push(vm, jit_raw_to_value(vstack_top[arg_count - 1 - i]));
    }
    Value result = native->function(arg_count, vm->stack + vm->sp - arg_count);
    vm->sp = saved_sp;

    if (vm->has_exception) {
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] call_native: native raised exception\n");
        return NULL_VAL;
    }
    return result;
}

/* Callout: OP_GET_PROPERTY (receiver.name), optionally peephole-merged with
 * a following OP_CALL. call_or_args == 0xFFFF means standalone property
 * access; otherwise it is the merged call's arg_count.
 * JIT virtual stack in call mode: [..., receiver, arg1..argN] with
 * vstack_top[0] = argN and vstack_top[arg_count] = receiver. */
Value jit_callout_get_property(int64_t* vstack_top, uint16_t name_const_idx,
                               uint16_t call_or_args, Chunk* chunk) {
    VM* vm = jit_callout_vm;
    if (!vm) return NULL_VAL;

    Value name_val = chunk->constants[name_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        error_add_at(ERR_RUNTIME, 0, 0, "属性名必须是字符串");
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: name not string\n");
        return NULL_VAL;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

/* ---- Merged call: receiver.name(args) ---- */
    if (call_or_args != 0xFFFF) {
        uint16_t arg_count = call_or_args;
        int saved_sp = vm->sp;

        /* Compiler push order: args first, receiver LAST (on top), so the
         * physical/virtual stack at this point is [..., arg1..argN, receiver]
         * with vstack_top[0] = receiver and vstack_top[i] = arg_i (i>=1).
         * Mirror op_property.inc: pop receiver from TOS, then reshape the VM
         * stack to [receiver, arg1..argN] and call the native with
         * (arg_count + 1) values. */
        Value receiver = jit_raw_to_value(vstack_top[0]);
        for (int i = 0; i < arg_count; i++) {
            vm_stack_push(vm, jit_raw_to_value(vstack_top[i + 1]));
        }
        int arg_start = vm->sp - arg_count;
        for (int i = arg_count - 1; i >= 0; i--) {
            vm->stack[arg_start + i + 1] = vm->stack[arg_start + i];
        }
        vm->stack[arg_start] = receiver;
        vm->sp++;

        ObjType rt = jitc_receiver_type(receiver);
        ObjNative* method = jitc_find_method(rt, name->chars);
        if (!method) {
            /* Non-native call (struct closure method etc.) → bail out,
             * the VM re-executes with the full op_property path. */
            vm->sp = saved_sp;
            jit_callout_failed = 1;
            if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: method not native (name='%s', rt=%d)\n", name ? name->chars : "?", (int)rt);
            return NULL_VAL;
        }
        Value result = method->function(arg_count + 1, vm->stack + arg_start);
        vm->sp = saved_sp;
        if (vm->has_exception) {
            jit_callout_failed = 1;
            if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: method raised exception\n");
            return NULL_VAL;
        }
        return result;
    }

    /* ---- Standalone property access ---- */
    Value receiver = jit_raw_to_value(vstack_top[0]);
    if (!val_is_obj(receiver)) {
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: receiver not obj\n");
        return NULL_VAL;
    }
    ObjType rt = jitc_receiver_type(receiver);

    /* Dict key first (non-call context) */
    if (rt == OBJ_DICT) {
        ObjDict* dict = (ObjDict*)val_as_obj(receiver);
        if (dict_has(dict, val_obj((Object*)name))) {
            return dict_get(dict, val_obj((Object*)name));
        }
    }

    /* Native method → bound method */
    ObjNative* method = jitc_find_method(rt, name->chars);
    if (method) {
        int saved_sp = vm->sp;
        vm_stack_push(vm, receiver);  /* GC root while allocating */
        ObjBoundMethod* bound = bound_method_new(receiver, method);
        vm->sp = saved_sp;
        if (!bound) {
            jit_callout_failed = 1;
            if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: bound_method_new failed\n");
            return NULL_VAL;
        }
        return val_obj((Object*)bound);
    }

    /* Dict fallback (no such key → dict_get returns NULL_VAL) */
    if (rt == OBJ_DICT) {
        ObjDict* dict = (ObjDict*)val_as_obj(receiver);
        return dict_get(dict, val_obj((Object*)name));
    }

    /* Struct/cstruct methods & fields → bail out to the VM */
    jit_callout_failed = 1;
    if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] get_property: struct bailout\n");
    return NULL_VAL;
}



