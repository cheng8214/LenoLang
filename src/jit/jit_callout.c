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
    /* jit_bailout_site 编码约定（见 EMIT_BAILOUT_SITE_WRITE / _NONOVF）：
     *   >= 0         : 溢出/截断类检查，值 = 触发指令的 bc_off
     *   -999 .. -1   : JIT 序言（-1 进入自增溢出、-2 step == 0、-3 step 是 float）
     *   <= -1000     : 其它原因，bc_off = -1000 - site */
    int site = (int)jit_bailout_site;
    if (site <= -1000) {
        fprintf(stderr, "[JIT-DEBUG] BAILOUT(nonovf) bc_off=%d RSP=%lld (0x%llx) RAX=%lld (0x%llx)\n",
                -1000 - site,
                (long long)rsp_val, (unsigned long long)rsp_val,
                (long long)jit_bailout_rax, (unsigned long long)jit_bailout_rax);
    } else if (site < 0) {
        /* 序言三态：-1 进入自增 int48 溢出、-2 step == 0、-3 step 是 float */
        const char* why = (site == -2) ? "step==0"
                        : (site == -3) ? "step-is-float"
                        : "entry-increment";
        fprintf(stderr, "[JIT-DEBUG] BAILOUT(prologue:%s) RSP=%lld (0x%llx) RAX=%lld (0x%llx)\n",
                why,
                (long long)rsp_val, (unsigned long long)rsp_val,
                (long long)jit_bailout_rax, (unsigned long long)jit_bailout_rax);
    } else {
        fprintf(stderr, "[JIT-DEBUG] BAILOUT site=%d RSP=%lld (0x%llx) RAX=%lld (0x%llx)\n",
                site,
                (long long)rsp_val, (unsigned long long)rsp_val,
                (long long)jit_bailout_rax, (unsigned long long)jit_bailout_rax);
    }
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
int jit_loop_depth = 0;

/* 「GC 想让 JIT 让出」标志 —— 契约见 jit_priv.h。
 * 置位只在 gc_alloc 跨阈值那一次；清零在解释器消费让出时。 */
int jit_gc_yield_flag = 0;

/* 当前是否在 JIT 机器码里（循环 JIT 或函数级 JIT）—— 见 §8.36。
 * 只被 GC 的 malloc 失败路径调用，不在热路径上。 */
int jit_in_frame(void) {
    return jit_func_depth > 0 || jit_loop_depth > 0;
}

/* 请求回退到解释器（见 jit.h 的契约）。置 jit_callout_failed 即可：
 * 所有可能失败的 callout 之后，codegen 都会检查这个标志并 bailout。 */
void jit_request_bailout(void) {
    jit_callout_failed = 1;
}
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
    /* ---- 以下分支对齐解释器 op_utils.inc OP_INDEX 语义 ---- */
    if (obj->type == OBJ_MODULE) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "模块键必须是字符串");
            return NULL_VAL;
        }
        ObjModule* module = (ObjModule*)obj;
        return dict_get(module->exports, idx_val);
    }
    if (obj->type == OBJ_STRUCT) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "struct 字段名必须是字符串");
            return NULL_VAL;
        }
        ObjStruct* st = (ObjStruct*)obj;
        ObjString* key = (ObjString*)val_as_obj(idx_val);
        /* 1) 字段 */
        int field_idx = struct_get_field_index(st->def, key->chars);
        if (field_idx >= 0) {
            return struct_get_field(st, field_idx);
        }
        /* 2) 用户定义方法 → 预创建闭包 */
        ObjStructDef* def = st->def;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->methods[i].name, key->chars) == 0) {
                return val_obj((Object*)def->methods[i].closure);
            }
        }
        /* 3) 原生方法 → 绑定方法 */
        ObjNative* native_method = struct_find_method(key->chars);
        if (native_method) {
            return val_obj((Object*)bound_method_new(obj_val, native_method));
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "struct '%s' 没有字段或方法 '%s'", st->def->name, key->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        return NULL_VAL;
    }
    if (obj->type == OBJ_CSTRUCT) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 字段名必须是字符串");
            return NULL_VAL;
        }
        ObjCStruct* cst = (ObjCStruct*)obj;
        ObjString* key = (ObjString*)val_as_obj(idx_val);
        /* 1) 字段 */
        int field_idx = cstruct_get_field_index(cst->def, key->chars);
        if (field_idx >= 0) {
            return cstruct_get_field_value(cst, field_idx);
        }
        /* 2) 原生方法 → 绑定方法 */
        ObjNative* native_method = cstruct_find_method(key->chars);
        if (native_method) {
            return val_obj((Object*)bound_method_new(obj_val, native_method));
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "cstruct '%s' 没有字段或方法 '%s'", cst->def->name, key->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        return NULL_VAL;
    }
    if (obj->type == OBJ_ENUM_DEF) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "enum 成员名必须是字符串");
            return NULL_VAL;
        }
        ObjEnumDef* def = (ObjEnumDef*)obj;
        ObjString* key = (ObjString*)val_as_obj(idx_val);
        int64_t value = enum_def_get_member_value(def, key->chars);
        if (value < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "enum '%s' 没有成员 '%s'", def->name, key->chars);
            error_add_at(ERR_RUNTIME, 0, 0, msg);
            return NULL_VAL;
        }
        return val_int_safe(value);
    }
    if (obj->type == OBJ_CSTRUCT_DEF) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 方法名必须是字符串");
            return NULL_VAL;
        }
        ObjCStructDef* def = (ObjCStructDef*)obj;
        ObjString* key = (ObjString*)val_as_obj(idx_val);
        ObjNative* native_method = cstruct_find_method(key->chars);
        if (native_method) {
            return val_obj((Object*)bound_method_new(obj_val, native_method));
        }
        char msg[256];
        snprintf(msg, sizeof(msg), "cstruct '%s' 没有方法 '%s'", def->name, key->chars);
        error_add_at(ERR_RUNTIME, 0, 0, msg);
        return NULL_VAL;
    }
    if (obj->type == OBJ_CSTRUCT_ARRAY_VIEW) {
        if (!val_is_num(idx_val)) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引必须是数字");
            return NULL_VAL;
        }
        int index = (int)value_to_double(idx_val);
        ObjCStructArrayView* view = (ObjCStructArrayView*)obj;
        if (index < 0 || index >= view->array_dim) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引越界");
            return NULL_VAL;
        }
        ObjCStruct* cst = view->cstruct;
        CStructFieldInfo* field = &cst->def->fields[view->field_index];
        uint8_t* element_addr = cst->data + field->offset + (index * view->element_size);
        if (view->element_type == TYPE_CSTRUCT && field->struct_name) {
            ObjCStructDef* nested_def = cstruct_def_find(field->struct_name);
            if (nested_def) {
                ObjCStruct* nested_obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
                if (nested_obj) {
                    nested_obj->def = nested_def;
                    nested_obj->data = element_addr;
                    nested_obj->owns_memory = 0;
                    return val_obj((Object*)nested_obj);
                }
            }
        }
        Value result = val_null();
        switch (view->element_type) {
            case TYPE_I8:   result = val_num((double)(*(int8_t*)element_addr)); break;
            case TYPE_U8:   result = val_num((double)(*(uint8_t*)element_addr)); break;
            case TYPE_I16:  result = val_num((double)(*(int16_t*)element_addr)); break;
            case TYPE_U16:  result = val_num((double)(*(uint16_t*)element_addr)); break;
            case TYPE_I32:  result = val_num((double)(*(int32_t*)element_addr)); break;
            case TYPE_U32:  result = val_num((double)(*(uint32_t*)element_addr)); break;
            case TYPE_I64:  result = val_num((double)(*(int64_t*)element_addr)); break;
            case TYPE_U64:  result = val_num((double)(*(uint64_t*)element_addr)); break;
            case TYPE_F32:  result = val_num((double)(*(float*)element_addr)); break;
            case TYPE_F64:  result = val_num((*(double*)element_addr)); break;
            case TYPE_BOOL: result = val_bool(*(uint8_t*)element_addr); break;
            case TYPE_C_INT:       result = val_num((double)(*(int*)element_addr)); break;
            case TYPE_C_UINT:      result = val_num((double)(*(unsigned int*)element_addr)); break;
            case TYPE_C_LONG:      result = val_num((double)(*(long*)element_addr)); break;
            case TYPE_C_ULONG:     result = val_num((double)(*(unsigned long*)element_addr)); break;
            case TYPE_C_LONGLONG:  result = val_num((double)(*(long long*)element_addr)); break;
            case TYPE_C_ULONGLONG: result = val_num((double)(*(unsigned long long*)element_addr)); break;
            case TYPE_C_SIZE:      result = val_num((double)(*(size_t*)element_addr)); break;
            case TYPE_C_SSIZE:     result = val_num((double)(*(ssize_t*)element_addr)); break;
            default: break;
        }
        return result;
    }
    if (obj->type == OBJ_CSTRUCT_ARRAY) {
        if (val_is_num(idx_val)) {
            int index = (int)value_to_double(idx_val);
            ObjCStructArray* array = (ObjCStructArray*)obj;
            if (index < 0 || index >= array->count) {
                error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引越界");
                return NULL_VAL;
            }
            ObjCStruct* element = cstruct_array_get(array, index);
            return element ? val_obj((Object*)element) : val_null();
        }
        if (val_is_obj(idx_val) && val_as_obj(idx_val)->type == OBJ_STRING) {
            ObjString* key = (ObjString*)val_as_obj(idx_val);
            ObjNative* native_method = cstruct_find_method(key->chars);
            if (native_method) {
                return val_obj((Object*)bound_method_new(obj_val, native_method));
            }
            char msg[256];
            snprintf(msg, sizeof(msg), "cstruct 数组没有方法 '%s'", key->chars);
            error_add_at(ERR_RUNTIME, 0, 0, msg);
            return NULL_VAL;
        }
        error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引必须是数字或方法名");
        return NULL_VAL;
    }
    error_add_at(ERR_RUNTIME, 0, 0, "索引操作需要数组、字典、字符串、struct、模块或 cstruct");
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
    /* ---- 以下分支对齐解释器 op_index_slice.inc DO_INDEX_SET 语义 ---- */
    if (obj->type == OBJ_STRUCT) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "struct 字段名必须是字符串");
            return -1;
        }
        ObjStruct* struct_obj = (ObjStruct*)obj;
        ObjStructDef* def = struct_obj->def;
        ObjString* field_name = (ObjString*)val_as_obj(idx_val);
        int field_idx = struct_get_field_index(def, field_name->chars);
        if (field_idx < 0) {
            error_add_at(ERR_RUNTIME, 0, 0, "struct 字段不存在");
            return -1;
        }
        TypeKind expected_type = def->fields[field_idx].type;
        if (expected_type == TYPE_FLOAT && val_is_int(value)) {
            value = val_float((double)val_as_int(value));
        } else if (expected_type == TYPE_FLOAT && val_is_bigint(value)) {
            value = val_float(bigint_to_double(val_as_bigint(value)));
        }
        struct_set_field(struct_obj, field_idx, value);
        return 0;
    }
    if (obj->type == OBJ_CSTRUCT) {
        if (!val_is_obj(idx_val) || val_as_obj(idx_val)->type != OBJ_STRING) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 字段名必须是字符串");
            return -1;
        }
        ObjCStruct* cstruct_obj = (ObjCStruct*)obj;
        ObjCStructDef* def = cstruct_obj->def;
        ObjString* field_name = (ObjString*)val_as_obj(idx_val);
        int field_idx = cstruct_get_field_index(def, field_name->chars);
        if (field_idx < 0) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 字段不存在");
            return -1;
        }
        CStructFieldInfo* field = &def->fields[field_idx];
        if (val_is_int(value) && (field->type == TYPE_F32 || field->type == TYPE_F64)) {
            value = val_float((double)val_as_int(value));
        } else if (val_is_float(value) && (field->type >= TYPE_I8 && field->type <= TYPE_U64)) {
            value = val_num(val_as_num(value));
        }
        cstruct_set_field_value(cstruct_obj, field_idx, value);
        return 0;
    }
    if (obj->type == OBJ_CSTRUCT_ARRAY_VIEW) {
        if (!val_is_num(idx_val)) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引必须是数字");
            return -1;
        }
        int index = (int)value_to_double(idx_val);
        ObjCStructArrayView* view = (ObjCStructArrayView*)obj;
        if (index < 0 || index >= view->array_dim) {
            error_add_at(ERR_RUNTIME, 0, 0, "cstruct 数组索引越界");
            return -1;
        }
        ObjCStruct* cst = view->cstruct;
        CStructFieldInfo* field = &cst->def->fields[view->field_index];
        uint8_t* element_addr = cst->data + field->offset + (index * view->element_size);
        if (val_is_int(value) && (view->element_type == TYPE_F32 || view->element_type == TYPE_F64)) {
            value = val_float((double)val_as_int(value));
        } else if (val_is_float(value) && (view->element_type >= TYPE_I8 && view->element_type <= TYPE_U64)) {
            value = val_num(val_as_num(value));
        }
        switch (view->element_type) {
            case TYPE_I8:   *(int8_t*)element_addr = (int8_t)val_as_num(value); break;
            case TYPE_U8:   *(uint8_t*)element_addr = (uint8_t)val_as_num(value); break;
            case TYPE_I16:  *(int16_t*)element_addr = (int16_t)val_as_num(value); break;
            case TYPE_U16:  *(uint16_t*)element_addr = (uint16_t)val_as_num(value); break;
            case TYPE_I32:  *(int32_t*)element_addr = (int32_t)val_as_num(value); break;
            case TYPE_U32:  *(uint32_t*)element_addr = (uint32_t)val_as_num(value); break;
            case TYPE_I64:  *(int64_t*)element_addr = (int64_t)val_as_num(value); break;
            case TYPE_U64:  *(uint64_t*)element_addr = (uint64_t)val_as_num(value); break;
            case TYPE_F32:  *(float*)element_addr = (float)val_as_num(value); break;
            case TYPE_F64:  *(double*)element_addr = (double)val_as_num(value); break;
            case TYPE_BOOL: *(uint8_t*)element_addr = val_as_num(value) != 0 ? 1 : 0; break;
            case TYPE_C_INT:       *(int*)element_addr = (int)val_as_num(value); break;
            case TYPE_C_UINT:      *(unsigned int*)element_addr = (unsigned int)val_as_num(value); break;
            case TYPE_C_LONG:      *(long*)element_addr = (long)val_as_num(value); break;
            case TYPE_C_ULONG:     *(unsigned long*)element_addr = (unsigned long)val_as_num(value); break;
            case TYPE_C_LONGLONG:  *(long long*)element_addr = (long long)val_as_num(value); break;
            case TYPE_C_ULONGLONG: *(unsigned long long*)element_addr = (unsigned long long)val_as_num(value); break;
            case TYPE_C_SIZE:      *(size_t*)element_addr = (size_t)val_as_num(value); break;
            case TYPE_C_SSIZE:     *(ssize_t*)element_addr = (ssize_t)val_as_num(value); break;
            default: break;
        }
        return 0;
    }
    error_add_at(ERR_RUNTIME, 0, 0, "索引赋值需要数组、字典、struct 或 cstruct");
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

/* Callout: OP_LENGTH（`.len()`）的非数字路径。
 * 语义逐条对齐解释器 vm/vminc/op_utils.inc 的 OP_LENGTH。数字在 codegen 里已原生处理，
 * 这里一并实现（保持 callout 单独可用 / 语义单点可查）。
 * **非法类型不在这里造错误**：只置 jit_callout_failed → JIT bailout → 解释器重放本条
 * 指令，报错文本与行号与 NO_JIT 完全一致（JIT 里凭空造错误会让报错位置错位）。 */
Value jit_callout_length(Value v) {
    if (val_is_num(v)) {
        int len = (int)value_to_double(v);
        if (len < 0) len = 0;
        return val_int(len);
    }
    if (!val_is_obj(v)) {
        if (jit_debug_on())
            fprintf(stderr, "[LEN-FAIL] non-obj: int=%d float=%d null=%d bool=%d bits=%016llx\n",
                    val_is_int(v), val_is_float(v), val_is_null(v), val_is_bool(v),
                    (unsigned long long)v);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    switch (val_as_obj(v)->type) {
        case OBJ_STRING:        return val_int(((ObjString*)val_as_obj(v))->char_len);
        case OBJ_ARRAY:         return val_int(((ObjArray*)val_as_obj(v))->count);
        case OBJ_DICT:          return val_int(((ObjDict*)val_as_obj(v))->order_count);
        case OBJ_ENUM_DEF:      return val_int(((ObjEnumDef*)val_as_obj(v))->member_count);
        case OBJ_CSTRUCT_ARRAY: return val_int(((ObjCStructArray*)val_as_obj(v))->count);
        case OBJ_STRUCT:        return val_int(((ObjStruct*)val_as_obj(v))->def->field_count);
        default:
            if (jit_debug_on())
                fprintf(stderr, "[LEN-FAIL] unsupported obj type=%d\n", (int)val_as_obj(v)->type);
            jit_callout_failed = 1;
            return NULL_VAL;
    }
}

/* Callout: OP_ITER_GET / OP_ITER_GET_VALUE（for-in 迭代）。
 * want_value = 0 → ITER_GET：数字→索引本身、array→元素、dict→**键**、enum→成员值、
 *                          string→单字符、struct→**字段名**
 * want_value = 1 → ITER_GET_VALUE：只支持 dict→值、struct→字段值
 * 语义逐条对齐 vm/vminc/op_utils.inc（ITER_GET）与 vm/vminc/op_iter.inc（ITER_GET_VALUE）。
 * **所有错误/越界都不在这里造**：只置 jit_callout_failed → bailout → 解释器重放本条指令，
 * 抛出与 NO_JIT 一致、可被 try/catch 捕获的异常（只 error_add_at + 返回 NULL 会被 JIT 静默吞掉）。
 * 数组 + int 索引的形态在 codegen 里有原生快路径，走不到这里。 */
Value jit_callout_iter_get(Value obj_val, Value index_val, int want_value) {
    const char* why = NULL;
    if (!val_is_num(index_val)) { why = "index not num"; goto fail; }
    int index = (int)value_to_double(index_val);

    /* 数字迭代（`for n to ...` 的索引本身）；ITER_GET_VALUE 不支持数字 */
    if (val_is_num(obj_val)) {
        if (want_value) { why = "numeric + want_value"; goto fail; }
        int max = (int)value_to_double(obj_val);
        if (index < 0 || index >= max) { why = "numeric oob"; goto fail; }
        return val_int(index);
    }
    if (!val_is_obj(obj_val)) { why = "obj not obj"; goto fail; }

    switch (val_as_obj(obj_val)->type) {
        case OBJ_ARRAY: {
            if (want_value) { why = "array + want_value"; goto fail; }
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            if (index < 0 || index >= arr->count) { why = "array oob"; goto fail; }
            return arr->elements[index];
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            if (index < 0 || index >= dict->order_count) { why = "dict oob"; goto fail; }
            return want_value ? dict_get_value_by_index(dict, index)
                              : dict_get_key_by_index(dict, index);
        }
        case OBJ_STRING: {
            if (want_value) { why = "string + want_value"; goto fail; }
            ObjString* str = (ObjString*)val_as_obj(obj_val);
            if (index < 0 || index >= str->char_len) { why = "string oob"; goto fail; }
            int byte_offset = utf8_char_offset(str->chars, str->len, index);
            int char_bytes = utf8_char_byte_len(str->chars, str->len, byte_offset);
            ObjString* one = str_new(&str->chars[byte_offset], char_bytes);
            if (!one) { why = "str alloc failed"; goto fail; }
            return val_obj((Object*)one);
        }
        case OBJ_ENUM_DEF: {
            if (want_value) { why = "enum + want_value"; goto fail; }
            ObjEnumDef* ed = (ObjEnumDef*)val_as_obj(obj_val);
            if (index < 0 || index >= ed->member_count) { why = "enum oob"; goto fail; }
            return val_int((int)ed->members[index].value);
        }
        case OBJ_STRUCT: {
            ObjStruct* so = (ObjStruct*)val_as_obj(obj_val);
            if (index < 0 || index >= so->def->field_count) { why = "struct oob"; goto fail; }
            if (want_value) return struct_get_field(so, index);
            ObjString* fname = str_copy(so->def->fields[index].name,
                                        (int)strlen(so->def->fields[index].name));
            if (!fname) { why = "field name alloc failed"; goto fail; }
            return val_obj((Object*)fname);
        }
        default:
            why = "unsupported obj type";
            goto fail;
    }

fail:
    if (jit_debug_on())
        fprintf(stderr, "[ITER-FAIL] %s (want_value=%d, idx=%lld/0x%016llx, obj=0x%016llx, objtype=%d)\n",
                why ? why : "?", want_value,
                (long long)value_to_double(index_val), (unsigned long long)index_val,
                (unsigned long long)obj_val,
                val_is_obj(obj_val) ? (int)val_as_obj(obj_val)->type : -1);
    jit_callout_failed = 1;
    return NULL_VAL;
}

/* Callout: OP_GET_FIELD（读 struct / cstruct 字段）。
 * 语义对齐 vm/vminc/op_struct.inc 的 OP_GET_FIELD：
 *   struct  → 越界检查 + struct_get_field（一次指针解引用）
 *   cstruct → 有 str16 转换 / 数组视图 / 嵌套 cstruct 三条**分配**路径，形态复杂
 * 这里实现 struct 路径（热循环里的绝对多数）；cstruct 与「非 struct/cstruct 类型」的
 * 报错路径一律置 jit_callout_failed → bailout → 解释器重放本条指令
 * （报错文本、分配语义、行号全部与 NO_JIT 一致）。 */
Value jit_callout_get_field(Value obj_val, uint8_t field_idx) {
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_STRUCT) {
        if (jit_debug_on())
            fprintf(stderr, "[FIELD-FAIL] get_field: obj 不是 struct（bits=0x%016llx, type=%d）\n",
                    (unsigned long long)obj_val,
                    val_is_obj(obj_val) ? (int)val_as_obj(obj_val)->type : -1);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
    if (field_idx >= obj->def->field_count) {
        if (jit_debug_on())
            fprintf(stderr, "[FIELD-FAIL] get_field: idx=%u >= count=%d（def=%s）\n",
                    (unsigned)field_idx, obj->def->field_count,
                    obj->def->name ? obj->def->name : "?");
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    return struct_get_field(obj, field_idx);
}

/* Callout: OP_SET_FIELD（写 struct / cstruct 字段，并把写进去的值压回栈 —— 赋值表达式的值）。
 * 语义对齐 vm/vminc/op_struct.inc 的 OP_SET_FIELD：
 *   struct  → 越界检查 + int→float / bigint→float 自动提升 + struct_set_field（**含 GC 写屏障**，
 *             它是 static inline 的唯一写入入口，JIT 不自己实现屏障）
 *   cstruct → 数值类型自动转换 + cstruct_set_field_value
 * 错误路径同样只置 failed（交解释器报错），语义与 NO_JIT 一致。 */
Value jit_callout_set_field(Value obj_val, uint8_t field_idx, Value value) {
    if (!val_is_obj(obj_val)) {
        if (jit_debug_on())
            fprintf(stderr, "[FIELD-FAIL] set_field: obj 不是对象（bits=0x%016llx）\n",
                    (unsigned long long)obj_val);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    ObjType t = val_as_obj(obj_val)->type;

    if (t == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
        ObjStructDef* def = obj->def;
        if (field_idx >= def->field_count) {
            if (jit_debug_on())
                fprintf(stderr, "[FIELD-FAIL] set_field: idx=%u >= count=%d（def=%s）\n",
                        (unsigned)field_idx, def->field_count, def->name ? def->name : "?");
            jit_callout_failed = 1;
            return NULL_VAL;
        }
        TypeKind expected = def->fields[field_idx].type;
        /* 自动提升（与解释器同序：先 int，后 bigint） */
        if (expected == TYPE_FLOAT && val_is_int(value)) {
            value = val_float((double)val_as_int(value));
        } else if (expected == TYPE_FLOAT && val_is_bigint(value)) {
            value = val_float(bigint_to_double(val_as_bigint(value)));
        }
        struct_set_field(obj, field_idx, value);
        return value;   /* 解释器把（可能已提升的）值压回栈 */
    }

    if (t == OBJ_CSTRUCT) {
        ObjCStruct* obj = (ObjCStruct*)val_as_obj(obj_val);
        ObjCStructDef* def = obj->def;
        if (field_idx >= def->field_count) {
            if (jit_debug_on())
                fprintf(stderr, "[FIELD-FAIL] set_field(cstruct): idx=%u >= count=%d\n",
                        (unsigned)field_idx, def->field_count);
            jit_callout_failed = 1;
            return NULL_VAL;
        }
        CStructFieldInfo* field = &def->fields[field_idx];
        if (val_is_int(value) && (field->type == TYPE_F32 || field->type == TYPE_F64)) {
            value = val_float((double)val_as_int(value));
        } else if (val_is_float(value) && (field->type >= TYPE_I8 && field->type <= TYPE_U64)) {
            value = val_num(val_as_num(value));
        }
        cstruct_set_field_value(obj, field_idx, value);
        return value;
    }

    if (jit_debug_on())
        fprintf(stderr, "[FIELD-FAIL] set_field: 不支持的对象类型 type=%d\n", (int)t);
    jit_callout_failed = 1;
    return NULL_VAL;
}

/* Callout: OP_GET_MODULE_VAR / OP_GET_MODULE_FUNC（读当前被编译函数所属模块的模块级变量/函数）。
 * 语义对齐 vm/vminc/op_module_var.inc：无模块 / 索引越界 → 报错（这里置 failed → bailout）。
 *
 * ⚠ module 是**编译期已知**的（jit_scan_set_module / jit_scan_get_module），由 codegen 直接
 * 嵌进机器码，**不是**运行时去查 `vm.frames[frame_cnt-1].module`：
 *   - 函数级 JIT 的快路径（jit_try_hot_func_call / jit_callout_invoke_method 的快路径）
 *     **不压帧**（flocals 直接从 VM 栈装填、机器码直调），此时栈顶帧是**调用方**的帧，
 *     查帧会读到调用方的模块 → 跨模块调用时静默读错变量（§8.56 的教训）。
 *   - 内联 callee 体内的模块变量访问已被 scan 直接拒绝（内联体的模块与调用方不同），
 *     所以「按被编译函数的模块」处理对循环 JIT / 函数级 JIT 都成立。
 * 模块对象的生命周期覆盖 JIT 机器码（模块在进程内一直可达）。 */
Value jit_callout_get_module_var(ObjModule* module, uint16_t index) {
    if (!module) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] module_var: 不在模块上下文中（idx=%u）\n",
                    (unsigned)index);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    if (index >= module->global_count) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] module_var: 索引越界 idx=%u >= count=%d\n",
                    (unsigned)index, module->global_count);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    return module->globals[index];
}

/* Callout: OP_SET_MODULE_VAR（写模块级变量）。
 * 解释器是 **peek**（vm_stack_peek(&vm, 0)），写完后 TOS 原样保留 → JIT 侧净效应 0、
 * 不改 vstack。写入必须带 gc_write_barrier（模块是 GC 根，与解释器同款）。
 * module 同 jit_callout_get_module_var：编译期嵌入，不查运行时帧。 */
Value jit_callout_set_module_var(ObjModule* module, uint16_t index, Value value) {
    if (!module) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] set_module_var: 不在模块上下文中（idx=%u）\n",
                    (unsigned)index);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    if (index >= module->global_count) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] set_module_var: 索引越界 idx=%u >= count=%d\n",
                    (unsigned)index, module->global_count);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    module->globals[index] = value;
    gc_write_barrier((Object*)module, value);
    return value;   /* 解释器 peek 后 TOS 不变；返回值仅便于调试 */
}

/* Callout: 模块函数调用（`OP_GET_MODULE_FUNC + OP_CALL` 窥孔，§8.56）。
 * JIT 栈约定与 jit_callout_invoke_method / OP_CALL_GLOBAL_FUNC 完全一致：
 *   vstack_top[0] = TOS = 最后一个实参；vstack_top[arg_count-1-i] = 第 i 个实参。
 *   ret_count == 1：RAX = 返回值（新 TOS）；
 *   ret_count >  1：把前 (ret_count-1) 个写回 vstack 内存（沿用实参槽），RAX = 最后一个
 *                   —— codegen 只弹掉 (arg_count - ret_count + 1) 个槽。
 * callee 从**编译期嵌入的模块**的 globals[index] 现取（模块变量可能被重新赋值换了
 * callee），并复核其 return_count 与编译期假设一致：不一致就 bailout 交解释器，
 * 绝不按错误的返回值个数记账（多返回值错记账 = 第一个结果落在实参槽上，§8.48 的老 bug 形态）。 */
Value jit_callout_call_module_func(ObjModule* module, int64_t* vstack_top,
                                   int arg_count, uint16_t index, int ret_count) {
    VM* vm = jit_callout_vm;
    if (!vm || arg_count < 0 || ret_count < 1) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    if (!module || index >= module->global_count) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_module_func: 无模块/索引越界（idx=%u）\n",
                    (unsigned)index);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    Value callee = module->globals[index];
    if (!val_is_obj(callee) || val_as_obj(callee)->type != OBJ_CLOSURE) {
        /* 非闭包（原生函数 / 别的可调用对象）：交解释器的 call_value 完整语义 */
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_module_func: callee 不是闭包\n");
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
    ObjFunction* mfunc = closure->function;
    int rc = (mfunc && mfunc->return_count > 0) ? mfunc->return_count : -1;
    if (rc != ret_count) {
        /* 模块变量被重新赋值换成了别的函数（或返回值个数不同）→ 编译期假设失效 */
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_module_func: return_count 变了"
                            "（编译期 %d，实际 %d）\n", ret_count, rc);
        jit_callout_failed = 1;
        return NULL_VAL;
    }

    /* ---- 快路径：callee 自身走函数级 JIT（与 jit_callout_invoke_method 同构）----
     * 这条路径不压帧，因此下面的模块访问/嵌套调用一律用**编译期模块**（参数传入）。 */
    if (rc == 1 && mfunc && jit_state.enabled && jit_func_depth < JIT_FUNC_MAX_DEPTH) {
        JitLoopFn jfn = jit_func_lookup_or_compile(mfunc, vm);
        if (jfn) {
            int lcount = mfunc->local_count > mfunc->arity ? mfunc->local_count : mfunc->arity;
            if (lcount < arg_count) lcount = arg_count;
            if (lcount > JIT_MAX_LOCALS) lcount = JIT_MAX_LOCALS;
            Value* flocals = jit_func_locals_pool[jit_func_depth];
            for (int i = 0; i < lcount; i++) flocals[i] = NULL_VAL;
            /* JIT 栈：vstack_top[0]=TOS=最后实参；函数参数 slot 0=第一个实参。 */
            for (int i = 0; i < arg_count && i < lcount; i++)
                flocals[i] = jit_raw_to_value(vstack_top[arg_count - 1 - i]);
            jit_func_depth++;
            jit_fn_result = NULL_VAL;
            int jr = jfn(flocals, vm->globals);
            jit_func_depth--;
            if (jr == 0 && !jit_callout_failed) {
                jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
                return jit_fn_result;
            }
            jit_callout_failed = 0;  /* 回退解释路径前复位 */
        }
    }

    /* ---- 慢路径：VM 重入（push args + callee，跑完折叠栈）---- */
    int saved_sp = vm->sp;
    for (int i = 0; i < arg_count; i++)
        vm_stack_push(vm, jit_raw_to_value(vstack_top[arg_count - 1 - i]));
    vm_stack_push(vm, callee);
    int saved_frame_cnt = vm->frame_cnt;
    if (vm_call_value(callee, arg_count, 0) == 0) {
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
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_module_func: vm_call_value failed\n");
        return NULL_VAL;
    }
    if (vm->frame_cnt > 0)
        jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;

    Value result = vm->last_return_value;
    /* 多返回值回填：与 jit_callout_invoke_method / jit_callout_global_func 同构 */
    if (rc > 1 && vm->sp >= rc) {
        Value ret_vals[16];
        for (int i = 0; i < rc && i < 16; i++)
            ret_vals[i] = vm->stack[vm->sp - rc + i];
        result = ret_vals[rc - 1];
        for (int i = 0; i < rc - 1 && i < 15; i++) {
            int slot = arg_count - 1 - i;
            if (slot >= 0)
                vstack_top[slot] = jit_value_to_raw(ret_vals[i]);
        }
    }
    vm->sp = saved_sp;
    return result;
}

/* Callouts: 存值前的「类型标记」（§8.58，镜像 vm/vminc/op_unary.inc 的两条 peek 指令）。
 * 都是 peek TOS、无返回值、**类型不匹配时静默什么都不做**（与解释器逐字一致），
 * 因此不需要 bailout 通道，也不会产生错误文本差异。 */
void jit_callout_set_ptr_elem_type(Value v, int elem_type) {
    if (!val_is_obj(v) || val_as_obj(v)->type != OBJ_FFI_POINTER) return;
    ObjFFIPointer* ptr = (ObjFFIPointer*)val_as_obj(v);
    if (!ptr->freed) ptr->element_type = (TypeKind)elem_type;
}

void jit_callout_set_declared_face(Value v, uint16_t name_const_idx, Chunk* chunk) {
    if (!chunk || name_const_idx >= (uint16_t)chunk->const_cnt) return;
    Value name_val = chunk->constants[name_const_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) return;
    if (!val_is_obj(v) || val_as_obj(v)->type != OBJ_STRUCT) return;
    ((ObjStruct*)val_as_obj(v))->declared_face = (ObjString*)val_as_obj(name_val);
}

/* ---- 通用相等比较的 C 实现（镜像解释器 vm/vminc/op_compare.inc 的 OP_EQ）----
 * 逐条对齐解释器规则：
 *   1) int/int 精确比较；任一是 float 时按 double 比较（BigInt 与 float 混合也走这里，
 *      与解释器的判定顺序一致）
 *   2) BigInt 按值比较
 *   3) 与 null 比较的 Ptr / cfunc：看包装地址是否为空（**只在顶层**，数组元素不适用）
 *   4) 类型不同 → false
 *   5) 同类型：null 相等、bool 比 bool、string 按内容、array 逐元素、其余对象按身份
 * 注意：这里是 JIT 与解释器共用的一份语义，改动必须同步 op_compare.inc，
 *       否则同一表达式在 JIT 里与在解释器里会算出不同结果。 */
static int jit_value_eq(Value a, Value b) {
    /* 1) 数值 */
    if (val_is_int(a) && val_is_int(b)) return val_as_int(a) == val_as_int(b);
    if (val_is_float(a) || val_is_float(b)) return val_as_num(a) == val_as_num(b);

    /* 2) BigInt（promote_to_bigint 是 vm 内部 static inline，这里等价展开） */
    if (val_is_bigint(a) || val_is_bigint(b)) {
        ObjBigInt* ba = val_is_bigint(a) ? val_as_bigint(a)
                                         : bigint_from_int64((int64_t)val_as_num(a));
        ObjBigInt* bb = val_is_bigint(b) ? val_as_bigint(b)
                                         : bigint_from_int64((int64_t)val_as_num(b));
        return bigint_compare(ba, bb) == 0;
    }

    /* 3) Ptr / cfunc 与 null：比较包装的地址 */
    if (val_is_null(a) && val_is_obj(b)) {
        ObjType tb = val_as_obj(b)->type;
        if (tb == OBJ_FFI_POINTER)  return ((ObjFFIPointer*)val_as_obj(b))->ptr == NULL;
        if (tb == OBJ_FFI_CALLBACK) return ((ObjFFICallback*)val_as_obj(b))->trampoline == NULL;
    }
    if (val_is_null(b) && val_is_obj(a)) {
        ObjType ta = val_as_obj(a)->type;
        if (ta == OBJ_FFI_POINTER)  return ((ObjFFIPointer*)val_as_obj(a))->ptr == NULL;
        if (ta == OBJ_FFI_CALLBACK) return ((ObjFFICallback*)val_as_obj(a))->trampoline == NULL;
    }

    /* 4) 类型不同 → 不相等 */
    if (val_get_type(a) != val_get_type(b)) return 0;

    /* 5) 同类型比较 */
    switch (val_get_type(a)) {
        case VAL_NULL:
            return 1;
        case VAL_BOOL:
            return val_as_bool(a) == val_as_bool(b);
        case VAL_INT:
        case VAL_FLOAT:
            return 0;   /* 数值路径在前面已处理 */
        case VAL_OBJ: {
            ObjType ta = val_as_obj(a)->type;
            ObjType tb = val_as_obj(b)->type;
            if (ta == OBJ_STRING && tb == OBJ_STRING) {
                ObjString* sa = (ObjString*)val_as_obj(a);
                ObjString* sb = (ObjString*)val_as_obj(b);
                return sa->len == sb->len &&
                       memcmp(sa->chars, sb->chars, (size_t)sa->len) == 0;
            }
            if (ta == OBJ_ARRAY && tb == OBJ_ARRAY) {
                ObjArray* aa = (ObjArray*)val_as_obj(a);
                ObjArray* ab = (ObjArray*)val_as_obj(b);
                if (aa->count != ab->count) return 0;
                for (int i = 0; i < aa->count; i++) {
                    Value ea = aa->elements[i];
                    Value eb = ab->elements[i];
                    /* 元素比较：数值按值、其余严格按类型（**不套用顶层 3) 的 Ptr/null 规则**） */
                    if (val_is_int(ea) && val_is_int(eb)) {
                        if (val_as_int(ea) != val_as_int(eb)) return 0;
                    } else if (val_is_float(ea) || val_is_float(eb)) {
                        if (val_as_num(ea) != val_as_num(eb)) return 0;
                    } else if (val_is_bigint(ea) || val_is_bigint(eb)) {
                        ObjBigInt* bea = val_is_bigint(ea) ? val_as_bigint(ea)
                                                          : bigint_from_int64((int64_t)val_as_num(ea));
                        ObjBigInt* beb = val_is_bigint(eb) ? val_as_bigint(eb)
                                                          : bigint_from_int64((int64_t)val_as_num(eb));
                        if (bigint_compare(bea, beb) != 0) return 0;
                    } else if (val_get_type(ea) != val_get_type(eb)) {
                        return 0;
                    } else {
                        switch (val_get_type(ea)) {
                            case VAL_NULL:  break;   /* null == null → 相等 */
                            case VAL_BOOL:
                                if (val_as_bool(ea) != val_as_bool(eb)) return 0;
                                break;
                            case VAL_INT:
                            case VAL_FLOAT:
                                return 0;            /* 前面已处理 */
                            case VAL_OBJ:
                                if (val_as_obj(ea)->type == OBJ_STRING &&
                                    val_as_obj(eb)->type == OBJ_STRING) {
                                    ObjString* sea = (ObjString*)val_as_obj(ea);
                                    ObjString* seb = (ObjString*)val_as_obj(eb);
                                    if (sea->len != seb->len ||
                                        memcmp(sea->chars, seb->chars, (size_t)sea->len) != 0) return 0;
                                } else if (val_as_obj(ea) != val_as_obj(eb)) {
                                    return 0;
                                }
                                break;
                        }
                    }
                }
                return 1;
            }
            return val_as_obj(a) == val_as_obj(b);
        }
    }
    return 0;
}

/* Callout: OP_EQ / OP_NEQ 的 NaN-boxed 操作数路径。
 * invert = 0 → ==，invert != 0 → !=（OP_NEQ 就是 OP_EQ 取反）。
 * 纯计算：不分配、不重入 VM，不会置 jit_callout_failed。
 * 动机：这两个操作数形态以前一律 bailout，而 `x != null` / `s != ""` 在渲染、
 * 布局、菜单等热循环里随处可见（file_manager 的 _topRects / _colW / Table.render /
 * _ensure_fitted_range 都因此整循环退回解释器）。 */
Value jit_callout_value_eq(Value a, Value b, int invert) {
    int result = jit_value_eq(a, b);
    if (invert) result = !result;
    return val_bool(result);
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

/* Callout: OP_INVOKE_METHOD_TYPED (struct method call via VM re-entry).
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
    if (!obj) {
        /* 分配失败（gc_alloc 已 error_add_at；JIT 帧内不再同步回收，见 §8.37）：
         * 置 failed 让 JIT bailout → 解释器重跑本轮、在安全状态下回收并重试。
         * 绝不能带着 NULL 继续写字段。 */
        jit_callout_failed = 1;
        return NULL_VAL;
    }

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

/* ============================================================================
 * 公共调用核心：jit_invoke_closure —— 「调用一个闭包」这件事一次做完（§8.59 抽自
 * jit_callout_invoke_method，供 INVOKE_METHOD_TYPED / GET_METHOD+CALL / 裸 OP_CALL 共用）。
 *
 * 约定（三个调用方必须一致）：
 *   - vstack_top[0] = TOS = **最后一个实参**；vstack_top[arg_count-1] = 第 0 个实参。
 *     调用方负责把「接收者 / GET_METHOD 消费的额外 receiver」排除在外（各处 codegen 的 +8）。
 *   - ret_count 由调用方按同一规则算好：被调函数 return_count（<=1 归一为 1）。
 *   - 成功：RAX = results[ret_count-1]（新 TOS），前 (ret_count-1) 个写回
 *     vstack_top[arg_count-1-i]。
 *   - 失败：置 jit_callout_failed 并返回 NULL_VAL（codegen 走 bailout，交解释器重放）。
 *
 * 为什么入参是「ObjFunction* + callee 原始 Value」而不是 ObjClosure*：
 *   函数值可能是**裸 ObjFunction**（`OP_GET_GLOBAL_FUNC` / `OP_GET_MODULE_FUNC` 取出来的
 *   就是这种；解释器的 call_value 有专门的 OBJ_FUNCTION 分支，内部才包成闭包）。
 *   快路径本来就只用 function；慢路径把**原始 callee 值**交给 vm_call_value，
 *   由解释器按自己的分支处理 ⇒ JIT 不需要为裸函数多做一次闭包分配（与解释器同语义、无额外开销）。
 *
 * 两级路径：
 *   快路径：callee 自身已（或可）编成函数级 JIT → 直接调机器码，不压 VM 帧
 *           （注意：这条路径**不压帧**，所以任何依赖「当前帧」的东西都不能用 —— 见 §8.56）。
 *   慢路径：VM 重入（vm_call_value），语义与解释器完全一致（异常/多返回值都在这里兜底）。
 * ========================================================================== */
static Value jit_invoke_closure(ObjFunction* mfunc, Value callee_val, int arg_count,
                                int64_t* vstack_top, int ret_count, const char* who) {
    VM* vm = jit_callout_vm;
    if (!vm) return NULL_VAL;

    /* ---- 快路径：callee 自身走函数级 JIT ----
     * locals 是临时数组：locals[0] = self(接收者)，locals[1..arg_count-1]
     * = 其余实参，其余 slot 置 NULL_VAL（VM 语义：未定义 slot 为 NULL）。
     * 返回 0 且无失败标志 → jit_fn_result 即返回值。
     * 任何不成功情况 → 回退到下方 VM 重入路径（栈/VM 状态在快路径中
     * 保持不变：只有 jit_callout_failed 可能被内部嵌套 callout 设置，
     * 回退前必须复位）。 */
    if (mfunc && ret_count == 1 && jit_state.enabled && jit_func_depth < JIT_FUNC_MAX_DEPTH) {
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
                fprintf(stderr, "[FT] %s jfn=%p func='%s' lc=%d ac=%d depth=%d\n",
                        who, (void*)jfn, mfunc->name ? mfunc->name : "?", lcount, arg_count, jit_func_depth);
            }
            JIT_FT_T1();
            int jr = jfn(flocals, vm->globals);
            JIT_FT_T2();
            jit_func_depth--;
            if (JIT_FT_TRACE_ON())
                fprintf(stderr, "[FT] %s done jr=%d failed=%d result=%p depth=%d\n",
                        who, jr, jit_callout_failed, (void*)(uintptr_t)jit_fn_result, jit_func_depth);
            if (jr == 0 && !jit_callout_failed) {
                jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
                JIT_FT_T3();
                JIT_FT_ACC();
                return jit_fn_result;
            }
            jit_callout_failed = 0;  /* 回退解释路径前复位 */
        }
    }

    /* ---- VM 重入路径（慢路径）：push args + callee，调用解释器 ---- */
    int saved_sp = vm->sp;

    /* JIT 虚拟栈向下增长（x86 push/pop）：vstack_top[0] 是最深压入的那个（最低地址），
     * 先压的元素在高地址 ⇒ 第 0 个实参在 vstack_top + (arg_count - 1)。 */
    for (int i = 0; i < arg_count; i++) {
        int64_t raw = vstack_top[arg_count - 1 - i];
        vm_stack_push(vm, jit_raw_to_value(raw));
    }

    /* Push callee and call：callee_val 可能是闭包或**裸函数**，
     * 交给 vm_call_value 按自己的分支处理（OBJ_FUNCTION 会在解释器侧包成闭包）。 */
    vm_stack_push(vm, callee_val);
    int saved_frame_cnt = vm->frame_cnt;
    int call_r = vm_call_value(callee_val, arg_count, 0);

    Value result = vm->last_return_value;

    if (call_r == 0) {
        /* vm_call_value 失败（异常未被捕获）：清理泄漏的 callee 帧 */
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
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] %s: vm_call_value failed\n", who);
        return NULL_VAL;
    }

    /* vm_grow_frames 可能重分配 vm.frames ⇒ 重载 locals 指针 */
    if (vm->frame_cnt > 0) {
        jit_reloaded_locals = vm->frames[vm->frame_cnt - 1].locals;
    }

    /* ---- 多返回值：与 jit_callout_global_func 完全同构 ----
     * OP_RETURN_MULTI 把全部返回值按序压在 VM 栈上（results[0] 最深、
     * results[ret_count-1] 为 TOS）。JIT 侧约定：
     *   RAX            = results[ret_count-1]（新 TOS，由返回值带回）
     *   vstack_top[arg_count-1-i] = results[i]（i = 0..ret_count-2）
     * 缺了这段回填，调用方 `var[float,float](a, b) = f.m()` 的 a 会读到
     * 实参槽残留（数值每帧不同 → 画面抖动）。 */
    if (ret_count > 1 && vm->sp >= ret_count) {
        Value ret_vals[16];
        for (int i = 0; i < ret_count && i < 16; i++) {
            ret_vals[i] = vm->stack[vm->sp - ret_count + i];
        }
        result = ret_vals[ret_count - 1];
        for (int i = 0; i < ret_count - 1 && i < 15; i++) {
            int slot = arg_count - 1 - i;
            if (slot >= 0) {
                vstack_top[slot] = jit_value_to_raw(ret_vals[i]);
            }
        }
    }

    /* Restore VM stack */
    vm->sp = saved_sp;

    return result;
}

/* Callout: OP_INVOKE_METHOD_TYPED (struct method call, VM re-entry) ---- */
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
        /* §8.57：本 callout 现在也被 `OP_GET_METHOD + OP_CALL` 的动态派发复用，
         * 而那种调用点**可能**是原生方法布局（先压实参再压 receiver）——
         * 此时这里取到的不是接收者。必须置 failed 让 JIT bailout 交解释器，
         * 绝不能带着 NULL 继续（那会把"布局不对"变成静默算错）。
         * 循环侧有 JIT_BAILOUT_LIMIT 兜底，最坏只是该循环回退几次后被拉黑。 */
        jit_callout_failed = 1;
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
        jit_callout_failed = 1;   /* §8.57：动态派发路径可能落到这里，必须交解释器 */
        return NULL_VAL;
    }

    /* 返回值个数：JIT codegen 已按编译期解析结果规划了栈布局，这里必须按
     * 同一规则回填。多返回值方法（如 Font.measureString → [float, float]）
     * 的额外返回值由调用方在 vstack 上留出的槽位承载（见下方 VM 路径）。
     * 函数级 JIT 拒收 return_count > 1 的函数，所以快路径只对 ret_count == 1 生效。 */
    int ret_count = 1;
    if (closure->function && closure->function->return_count > 1) {
        ret_count = closure->function->return_count;
        if (ret_count > 16) ret_count = 16;
    }

    /* 快路径（函数级 JIT）+ VM 重入 + 多返回值回填都在 jit_invoke_closure 里，
     * 与 GET_METHOD+CALL / 裸 OP_CALL 共用同一份机械。 */
    return jit_invoke_closure(closure->function, val_obj((Object*)closure),
                              arg_count, vstack_top, ret_count, "invoke_method");
}

/* Callout: 裸 OP_CALL（callee 是运行时值：局部闭包 / 回调表元素 / 字段 …）—— §8.59
 * VM 侧语义见 op_call.inc：`callee = peek(0)`（栈约定 [args...][callee]），
 *   OBJ_CLOSURE → call()；OBJ_FUNCTION → 新建闭包（分配）再 call；
 *   OBJ_NATIVE / bound method → call_value 其它分支；null → 报"函数未定义"。
 * JIT 只处理 OBJ_CLOSURE；其余一律 failed → bailout，报错/分配语义交解释器。
 *
 * ★ 返回值个数守卫放在**调用之前**：调用点消费几个值由编译期静态函数类型决定，
 *   `return_count != 1`（多返回值解构）在这里就挡下。若改成"先调用再检查"，
 *   bailout 后解释器会重跑整轮 ⇒ callee 执行两次、副作用翻倍
 *   （§14 的"堆侧副作用无法回滚"只在不可避时才接受；能提前判断的必须提前判断）。 */
Value jit_callout_call_value(int64_t* vstack_top, int arg_count) {
    VM* vm = jit_callout_vm;
    if (!vm || arg_count < 0) {
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    /* callee = VM 侧的 peek(0)（栈顶）。注意本 callout 的 vstack_top 是 **+8 后的实参块**
     * 指针（vstack_top[0] = 最后一个实参），而 callee 在实参之上（JIT 栈向下增长 ⇒ 更低地址）
     * ⇒ callee 在 vstack_top[-1]。 */
    Value callee = jit_raw_to_value(vstack_top[-1]);
    ObjFunction* fn = NULL;
    if (val_is_obj(callee)) {
        Object* o = val_as_obj(callee);
        if (o->type == OBJ_CLOSURE) fn = ((ObjClosure*)o)->function;
        else if (o->type == OBJ_FUNCTION) fn = (ObjFunction*)o;  /* 裸函数：慢路径由解释器包闭包 */
    }
    if (!fn) {
        /* null（"函数未定义"）、OBJ_NATIVE / bound method 等 ⇒ 交解释器，报错文本一致 */
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_value: callee 不是闭包/函数（交解释器）\n");
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    int rc = (fn->return_count > 0) ? fn->return_count : -1;
    if (rc != 1) {
        if (jit_debug_on())
            fprintf(stderr, "[JIT-CALLOUT-FAIL] call_value: return_count=%d != 1（交解释器）\n", rc);
        jit_callout_failed = 1;
        return NULL_VAL;
    }
    /* 实参块在 callee 槽之上：codegen 已经把 rsp+8（= 最后一个实参）传进来了，
     * 这里直接把它交给 helper（**不要再 +1** —— 那会跳到更深的槽、把外层变量当成实参，
     * 本轮就是踩了这个：addOne 拿到的是累加器而不是循环变量）。 */
    return jit_invoke_closure(fn, callee, arg_count, vstack_top, 1, "call_value");
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
            /* ---- callee 解析缓存（§8.47）----
             * 递归调用点 / 固定调用点每次都是同一个 callee，而
             * jit_func_lookup_or_compile 是一次跨 TU 调用 + 哈希探测 + tried 判定。
             * 这里缓存「上次解析成功的 callee」及其槽地址，命中即跳过整条查找。
             *
             * **不缓存机器码指针**：jit_func_entry_claim 在槽冲突时会
             * jit_mem_free 掉被驱逐函数的机器码（jit_priv.h 有明确记载），缓存裸
             * 指针会变成 use-after-free。这里只缓存槽地址（jit_func_cache 是静态
             * 数组，地址稳定）与 lcount，每次命中都重读 e->func / e->fn 校验。 */
            static ObjFunction*       lc_func  = NULL;
            static JitFuncCacheEntry* lc_entry = NULL;
            static int                lc_base  = 0;   /* max(local_count, arity) */
            /* LENO_NO_CALLCACHE：关掉本缓存（基准/诊断用）。
             * 开关值在 jit_init() 里解析好（jit_state.no_callcache）—— **不能**
             * 在这里现读 getenv 或做「首次调用判负的 static」：本函数是每次
             * Leno 调用都进的（fib 8.67 亿次），那会在热路径上白付 2~3 条指令。 */
            JitLoopFn jfn = NULL;
            int base = 0;
            if (!jit_state.no_callcache && gfunc == lc_func && lc_entry->func == gfunc) {
                jfn  = lc_entry->fn;      /* 每次重读：槽可能已被回收或重编译 */
                base = lc_base;
            }
            if (!jfn) {
                jfn = jit_func_lookup_or_compile(gfunc, vm);
                if (jfn) {
                    base = gfunc->local_count > gfunc->arity
                               ? gfunc->local_count : gfunc->arity;
                    lc_func  = gfunc;
                    lc_entry = &jit_func_cache[((uintptr_t)gfunc >> 4) & (JIT_FUNC_CACHE_SIZE - 1)];
                    lc_base  = base;
                }
            }
            if (jfn) {
                int lcount = base;
                if (lcount < arg_count) lcount = arg_count;
                if (lcount > JIT_MAX_LOCALS) lcount = JIT_MAX_LOCALS;
                Value* flocals = jit_func_locals_pool[jit_func_depth];
                /* 实参槽由下面第二个循环覆写 ⇒ 不必先置 NULL（省 arg_count 次写） */
                for (int i = arg_count; i < lcount; i++) flocals[i] = NULL_VAL;
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

/* ---- 模块方法编译期解析 ----
 * 从 chunk 常量表取「模块名/方法名」字符串，查一次模块方法表并返回 meta。
 * codegen 把结果指针嵌进机器码；解析失败返回 NULL，此时 codegen 退回
 * 通用 callout（保留原有的报错信息与语义）。 */
ModuleMethodMeta* jit_resolve_module_method(Chunk* chunk, uint16_t module_idx, uint16_t method_idx) {
    if (!chunk) return NULL;
    if (module_idx >= (uint16_t)chunk->const_cnt || method_idx >= (uint16_t)chunk->const_cnt)
        return NULL;
    Value module_val = chunk->constants[module_idx];
    Value method_val = chunk->constants[method_idx];
    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_STRING ||
        !val_is_obj(method_val) || val_as_obj(method_val)->type != OBJ_STRING)
        return NULL;
    return native_find_module_method(((ObjString*)val_as_obj(module_val))->chars,
                                     ((ObjString*)val_as_obj(method_val))->chars);
}

/* ---- 通用「数值薄调用」桥 ----
 * 供 codegen 对「全 float 参数 + float 返回」的模块方法使用（如 maths.*）。
 *
 * 调用约定（两个 ABI 都成立的关键：double 参数放最前面）：
 *   - 第 1..N 个参数是 double → xmm0..xmm(N-1)（Win64 与 SysV 一致）
 *   - 最后一个参数是 NativeFn 指针：
 *       Win64：按“位置”分配整型寄存器，第 N+1 位 → RDX/R8/R9
 *       SysV ：按“第几个整型参数”分配，它是第 1 个整型参数 → RDI
 *     （若把 fn 放最前，Win64 会把第 1 个 double 参数放到 xmm1，两边不一致）
 *   - 返回 double 在 xmm0
 * 桥内部把参数装箱后调用模块原本的 NativeFn，因此数学实现只有模块里那一份。 */
static double jit_thin_bridge_impl(NativeFn fn, int argc, const double* argv) {
    VM* vm = jit_callout_vm;
    Value args[4];
    for (int i = 0; i < argc; i++) args[i] = val_float(argv[i]);
    Value result = fn(argc, args);
    if (vm && vm->has_exception) {
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] thin bridge: native raised exception\n");
        return 0.0;
    }
    return val_as_num(result);
}

double jit_thin_f1(double a, NativeFn fn) {
    double v[1]; v[0] = a;
    return jit_thin_bridge_impl(fn, 1, v);
}
double jit_thin_f2(double a, double b, NativeFn fn) {
    double v[2]; v[0] = a; v[1] = b;
    return jit_thin_bridge_impl(fn, 2, v);
}
double jit_thin_f3(double a, double b, double c, NativeFn fn) {
    double v[3]; v[0] = a; v[1] = b; v[2] = c;
    return jit_thin_bridge_impl(fn, 3, v);
}

void* jit_thin_bridge_for(int arity) {
    switch (arity) {
        case 1: return (void*)jit_thin_f1;
        case 2: return (void*)jit_thin_f2;
        case 3: return (void*)jit_thin_f3;
        default: return NULL;   /* 0 或 >3 个 double 参数：走通用 callout */
    }
}

/* Callout: 已解析 meta 的模块方法调用（快路径，无哈希/strcmp 查找）。
 * 与 jit_callout_module_call 的语义完全一致，只是省掉了解析步骤。 */
Value jit_callout_module_call_meta(int64_t* vstack_top, int arg_count,
                                   ModuleMethodMeta* meta) {
    if (!meta || !meta->function) {
        error_add_at(ERR_RUNTIME, 0, 0, "OP_MODULE_CALL: 模块方法未解析");
        return NULL_VAL;
    }
    if (arg_count > 16) {
        error_add_at(ERR_RUNTIME, 0, 0, "模块方法参数过多");
        return NULL_VAL;
    }

    /* JIT 虚拟栈向低地址增长：vstack_top[0] = TOS（最后压入的实参），
     * native 期望 args[0] = 第一个实参。 */
    Value args[16];
    for (int i = 0; i < arg_count; i++) {
        args[i] = jit_raw_to_value(vstack_top[arg_count - 1 - i]);
    }

    Value result = meta->function(arg_count, args);

    if (jit_callout_vm && jit_callout_vm->has_exception) {
        jit_callout_failed = 1;
        if (jit_debug_on()) fprintf(stderr, "[JIT-CALLOUT-FAIL] module_call(meta): native raised exception\n");
        return NULL_VAL;
    }

    return result;
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
         * physical/virtual stack at this point is [..., arg1..argN, receiver].
         *
         * JIT 虚拟栈是「栈顶在低地址」，即 vstack_top[k] 是栈顶往下第 k 个值，
         * 于是 vstack_top[0] = receiver、vstack_top[1] = 最后一个实参 argN、
         * …、vstack_top[arg_count] = 第一个实参 arg1。
         * 所以第 i 个实参（i 从 0 计，源序）在 vstack_top[arg_count - i]。
         * （此前误用 vstack_top[i+1]，把实参顺序整体颠倒：o.get(key, def)
         * 变成 get(def, key)，键落空即返回默认值——例如 _relayout 里
         * o.get("grow", 0.0) 返回字符串 "grow"。）
         * Mirror op_property.inc: pop receiver from TOS, then reshape the VM
         * stack to [receiver, arg1..argN] and call the native with
         * (arg_count + 1) values. */
        Value receiver = jit_raw_to_value(vstack_top[0]);
        for (int i = 0; i < arg_count; i++) {
            vm_stack_push(vm, jit_raw_to_value(vstack_top[arg_count - i]));
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



