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

/* ---- 编译期「当前被编译函数所属模块」（§8.56）----
 * 模块函数调用（`foo(x)` → `OP_GET_MODULE_FUNC` + `OP_CALL`）的返回值个数必须
 * 编译期确定（ObjFunction.return_count），而 callee 只能从「函数所属模块的
 * globals[]」里取 —— 所以 scan 与 codegen 都需要知道当前在编译哪个模块的函数。
 * 由 jit.c 的两个编译入口（jit_compile / jit_compile_function）在调用 scan 前设置。
 * 单线程编译、且编译在机器码开始执行前就结束，故不存在交叉覆盖。 */
static ObjModule* g_jit_scan_module = NULL;

void jit_scan_set_module(ObjModule* module) { g_jit_scan_module = module; }
ObjModule* jit_scan_get_module(void) { return g_jit_scan_module; }

/* ============================================================================
 * JIT 拒收原因**聚合**直方图（`LENO_JIT_GAPS=1`，R6-c 诊断）
 *
 * 为什么必须单独一个开关：`LENO_JIT_DEBUG=1` 会把**每次编译尝试**的 body raw hex
 * （最多 1200 字节）+ 逐 opcode 走查整段打出来 —— 真实应用上实测 stderr **8.4MB 且
 * 60 帧跑不完（>120s）**，根本无法用来做直方图（本项目为此踩过两次）。
 * 本开关**只累加计数**，退出时打印一次（几十行），开销可忽略。
 *
 * 键 = "模式|原因"。模式由 jit.c 在调用 scan 前设置（`loop` / `func`）；
 * 内联侧走 jit_gaps_record_inline()，模式固定为 `inline`。
 * ========================================================================== */
#include <stdarg.h>
typedef struct { char key[72]; int count; } JitGapEntry;
#define JIT_GAP_MAX 96
static JitGapEntry g_gap_tbl[JIT_GAP_MAX];
static int g_gap_n = 0;
static int g_gap_on = -1;
static const char* g_gap_mode = "loop";

static int jit_gaps_on(void) {
    if (g_gap_on < 0) g_gap_on = getenv("LENO_JIT_GAPS") ? 1 : 0;
    return g_gap_on;
}

void jit_gaps_set_mode(const char* m) { g_gap_mode = (m && *m) ? m : "?"; }

/* ---- 按「编译对象身份」去重（R6-c v2，2026-09-16）----
 * 为什么必须去重：编译尝试会因 jit_func_cache 冲突驱逐（§8.73）而**重复发生** ⇒ 逐次计数会被
 * "重编译颠簸"污染：实测同一应用重跑 `inline|OP_CALL` = 259/259/9、
 * `func|函数体含循环` = 13/305/13，而 `FuncCompiled` 在 97~597 之间横跳。
 * 去重后计数 = "**有多少个不同的**函数 / 循环 / 被调方因该原因被拒"，跨运行才可比。 */
static const void* g_gap_func_id = NULL;     /* 当前编译对象（jit.c 编译前设置） */
static const void* g_gap_inline_id = NULL;   /* 当前被内联的 callee chunk（内联扫描入口设置） */
#define JIT_GAP_SEEN_MAX 8192
static struct { const void* id; unsigned h; } g_gap_seen[JIT_GAP_SEEN_MAX];
static int g_gap_seen_n = 0;
static int g_gap_seen_overflow = 0;

void jit_gaps_set_func_id(const void* id) { g_gap_func_id = id; }
void jit_gaps_set_inline_id(const void* id) { g_gap_inline_id = id; }

static unsigned jit_gap_hash(const char* s) {     /* FNV-1a */
    unsigned h = 2166136261u;
    for (; *s; s++) { h ^= (unsigned)(unsigned char)*s; h *= 16777619u; }
    return h;
}

/* 返回 1 = 该 (对象, 原因) 之前已经记过 ⇒ 调用方跳过计数 */
static int jit_gap_seen_before(const void* id, unsigned h) {
    for (int i = 0; i < g_gap_seen_n; i++) {
        if (g_gap_seen[i].id == id && g_gap_seen[i].h == h) return 1;
    }
    if (g_gap_seen_n < JIT_GAP_SEEN_MAX) {
        g_gap_seen[g_gap_seen_n].id = id;
        g_gap_seen[g_gap_seen_n].h = h;
        g_gap_seen_n++;
    } else {
        g_gap_seen_overflow = 1;
    }
    return 0;
}

static void jit_gap_bump(const char* mode, const char* reason, const void* id) {
    char key[72];
    snprintf(key, sizeof(key), "%s|%s", mode, reason);
    if (jit_gap_seen_before(id, jit_gap_hash(key))) return;   /* 同一对象+同一原因只记一次 */
    for (int i = 0; i < g_gap_n; i++) {
        if (strcmp(g_gap_tbl[i].key, key) == 0) { g_gap_tbl[i].count++; return; }
    }
    if (g_gap_n < JIT_GAP_MAX) {
        snprintf(g_gap_tbl[g_gap_n].key, sizeof(g_gap_tbl[g_gap_n].key), "%s", key);
        g_gap_tbl[g_gap_n].count = 1;
        g_gap_n++;
    }
}

void jit_gaps_record(const char* fmt, ...) {
    if (!jit_gaps_on()) return;
    char reason[64];
    va_list ap; va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    jit_gap_bump(g_gap_mode, reason, g_gap_func_id);
}

void jit_gaps_record_inline(const char* fmt, ...) {
    if (!jit_gaps_on()) return;
    char reason[64];
    va_list ap; va_start(ap, fmt);
    vsnprintf(reason, sizeof(reason), fmt, ap);
    va_end(ap);
    jit_gap_bump("inline", reason, g_gap_inline_id);
}

void jit_gaps_print(void) {
    if (g_gap_n == 0) return;
    fprintf(stderr, "=== JIT 拒收原因（LENO_JIT_GAPS；计数 = 不同的函数/循环/被调方数）===\n");
    for (int i = 0; i < g_gap_n; i++) {        /* 计数降序（条目很少，选择排序足够） */
        int best = i;
        for (int j = i + 1; j < g_gap_n; j++)
            if (g_gap_tbl[j].count > g_gap_tbl[best].count) best = j;
        if (best != i) {
            JitGapEntry tmp = g_gap_tbl[i];
            g_gap_tbl[i] = g_gap_tbl[best];
            g_gap_tbl[best] = tmp;
        }
    }
    for (int i = 0; i < g_gap_n; i++)
        fprintf(stderr, "  %6d  %s\n", g_gap_tbl[i].count, g_gap_tbl[i].key);
    if (g_gap_seen_overflow)
        fprintf(stderr, "  （注：去重表已满，部分计数可能偏高）\n");
    fprintf(stderr, "=================================\n");
}

/* 解析模块 globals[index] 处的函数闭包 → 返回其 return_count。
 * return_count 的语义见 codegen_func.c：>=1 编译期确定（无显式 return 按 1 个，
 * 即隐式 null）；-1 = 静态不可知（各 return 个数不一致 / fall-through）。
 * 返回 0 表示「解析不出来 / 静态不可知」，调用方必须拒绝 JIT（见 jit_priv.h）。 */
int jit_resolve_module_func(uint16_t index) {
    ObjModule* module = g_jit_scan_module;
    if (!module || !module->globals) return 0;
    if (index >= (uint16_t)module->global_count) return 0;
    Value v = module->globals[index];
    if (!val_is_obj(v)) return 0;
    Object* o = val_as_obj(v);
    ObjFunction* fn = NULL;
    if (o->type == OBJ_CLOSURE) fn = ((ObjClosure*)o)->function;
    else if (o->type == OBJ_FUNCTION) fn = (ObjFunction*)o;
    if (!fn || fn->return_count <= 0) return 0;
    return fn->return_count;
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
        case OP_SET_PTR_ELEM_TYPE: /* op + elem_type(1) */
            return 2;
        /* 3-byte (opcode + slot16) */
        case OP_CONST: case OP_GET_LOCAL: case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP: case OP_INC_LOCAL: case OP_DEC_LOCAL:
        case OP_PRE_INC_LOCAL: case OP_PRE_DEC_LOCAL:
        case OP_INC_LOCAL_NOPUSH: case OP_DEC_LOCAL_NOPUSH:
        case OP_GET_GLOBAL: case OP_SET_GLOBAL:
        case OP_ARRAY:          /* opcode + count16 */
        case OP_GET_PROPERTY:   /* opcode + name_const16 */
        case OP_GET_METHOD:     /* opcode + name_const16 */
        case OP_SET_DECLARED_FACE: /* opcode + face_name_const16 */
        case OP_CALL:           /* opcode + arg_count16（裸调用：callee 是运行时值） */
        case OP_GET_MODULE_VAR:  /* opcode + index16 */
        case OP_SET_MODULE_VAR:  /* opcode + index16 */
        case OP_GET_MODULE_FUNC: /* opcode + index16 */
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
        /* 变长：OP_SWITCH_LOOKUP = opcode + const_idx(2) + case_count(2)
         *                        + default_off(4) + [body_off(4)] * case_count
         * → 9 + 4 * case_count 字节（§8.61）。case_count 在 ip[3..4]。 */
        case OP_SWITCH_LOOKUP:
            return 9 + 4 * (int)(((uint16_t)ip[3] << 8) | (uint16_t)ip[4]);
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
         *   + generic_args(generic_count*2) + field_idx[arg_count]
         *   + mod_space(1) + mod_slot(2)   ← S2/2b-2：导入模块的槽位（见 codegen_expr.c） */
        case OP_STRUCT_INIT:
            return 8 + 2 * ip[4] + ip[3];
        /* 10-byte (CMPJMP variants) */
        case OP_CMPJMP_LL_INT:
        case OP_CMPJMP_LG_INT:
            return 10;
        /* 12-byte: CMPJMP local vs imm32 (cmp_op(1) slot(2) imm32(4) offset(4)) */
        case OP_CMPJMP_LI_INT:
            return 12;

        /* ====================================================================
         * R3：补齐余项长度（诊断用 —— 让「缺长度」与「缺 case」一眼可分）。
         * 每条都与 VM 实现里 READ_* 的次数逐条核对过，并与 debug.c 的
         * disassembleInstruction 交叉验证（那里也维护了每个 opcode 的长度）。
         * 长度算错的后果是**静默走错字节流**，所以宁可不填也不猜：
         * 长度依赖常量表（opcode_size 只有 ip，拿不到 chunk）的 OP_CLOSURE
         * 保持返回 -1，归入「长度未知」类。
         * ==================================================================== */

        /* 无操作数（1 字节） */
        case OP_CLOSE_UPVALUE:  /* 关闭 upvalue，无操作数 */
        case OP_NEG:            /* 通用取负：类型分发，无操作数 */
        case OP_IS_NULL:        /* 弹栈顶判 null，无操作数 */
        case OP_IN:             /* in 运算符，无操作数（弹 2 压 1） */
        case OP_ARRAY_GET:      /* 无操作数，从栈读值 */
        case OP_ARRAY_SET:
        case OP_ARRAY_APPEND:
        case OP_DICT_GET:
        case OP_DICT_GET_KEY:   /* 弹索引 + 字典，无操作数 */
        case OP_STRING_ADD:     /* 字符串拼接，无操作数 */
        case OP_INDEX_SET:      /* arr[i] = v，无操作数 */
        case OP_SLICE:          /* s[a:b]，无操作数 */
        case OP_THROW:          /* throw，无操作数 */
        case OP_AWAIT:          /* await，无操作数 */
        case OP_INIT_LENOMODULE:/* 模块初始化，无操作数 */
        case OP_U8_TO_F64:      /* u8 → f64，无操作数 */
            return 1;

        /* 2 字节 */
        case OP_RANGE:          /* inclusive(1) */
        case OP_GET_FIELD_ADDR: /* field_idx(1) */
            return 2;

        /* 3 字节 */
        case OP_GET_UPVALUE: case OP_SET_UPVALUE:   /* slot(2) */
        case OP_DEFINE_GLOBAL: case OP_GET_GLOBAL_FUNC: case OP_DEFINE_GLOBAL_FUNC:
        case OP_GET_NATIVE:                          /* const(2) */
        case OP_TAIL_CALL:                           /* arg_count(2) */
        case OP_DICT:                                /* count(2)（值来自前面的 CONST） */
        case OP_LOAD_NATIVE_MODULE:                  /* const(2) */
        case OP_DEFINE_MODULE_FUNC:                  /* index(2) */
        case OP_GET_CSTRUCT_DEF:                     /* name_const(2) */
        case OP_ASYNC_CALL:                          /* arg_count(2) */
        case OP_DTOR_LOCAL:                          /* slot(2) */
            return 3;

        /* 5 字节 */
        case OP_GET_MODULE_CONST:   /* module_const(2) + const_name(2) */
            return 5;

        /* kind(1) [+ elem_type(1)] 或 [name_const(2)]。
         * **TYPE_ENUM 也读 2 字节名字常量**（op_type_check.inc 里 case TYPE_ENUM 与
         * case TYPE_STRUCT/FACE 同形）—— 早先只写 FACE/STRUCT 是错的，会让含 enum
         * 类型检查的字节流少走 1 字节。教训：长度表必须逐条对 VM 的 READ_* 核对，
         * 不能照抄 debug.c 的反汇编器（那里有同一个漏项，本轮一并修掉）。
         *
         * ⚠ §8.83：OP_TYPE_CHECK 与 OP_AS_CAST 的**名字常量集合不同**，必须分开登记：
         *   · OP_TYPE_CHECK：STRUCT / FACE / **ENUM**（op_type_check.inc:27-30）
         *   · OP_AS_CAST   ：STRUCT / FACE / **CSTRUCT**（op_as_cast.inc 的 case TYPE_CSTRUCT
         *                    用 READ_SHORT）；而 **ENUM 落到 default**、只消费 1 字节
         * 此前两者共用上面一条（STRUCT/FACE/ENUM ⇒ 4），后果：
         *   · AS_CAST + CSTRUCT 少读 1 字节（会把它后面那字节当下一条指令的开始）
         *   · AS_CAST + ENUM   多读 1 字节
         * 这两个缺陷此前被"OP_AS_CAST 一律被 default 拒收（扫描到它就 return）"掩盖着。 */
        case OP_TYPE_CHECK: {
            uint8_t tk = ip[1];
            return (tk == TYPE_FACE || tk == TYPE_STRUCT || tk == TYPE_ENUM) ? 4 : 3;
        }
        case OP_AS_CAST: {
            uint8_t tk = ip[1];
            return (tk == TYPE_FACE || tk == TYPE_STRUCT || tk == TYPE_CSTRUCT) ? 4 : 3;
        }
        /* count(1) + const_index(2) * count */
        case OP_PUSH_TYPE_ARGS:
            return 2 + 2 * (int)ip[1];
        /* ret_type(1) + param_count(1) + param_types[param_count](1 each) */
        case OP_CFUNC_CALLBACK:
            return 3 + (int)ip[2];
        /* arg_count(2) + ret_type(1) + user_arg_count(1) + arg_types[user_arg_count](1 each) */
        case OP_CLIB_CALL:
            return 5 + (int)ip[4];

        default:
            /* 长度未知：要么没登记过，要么长度依赖常量表（OP_CLOSURE —— 它的
             * upvalue 数量记在常量表的函数对象里，opcode_size 只有 ip，无从得知，
             * 硬猜长度会让扫描静默走错字节流）。 */
            return -1;
    }
}




/* ---- R5-P0：`OP_CLOSURE` 的变长操作数解析（只读探针）----
 *
 * 操作数格式（与 VM 的 `op_call.inc` / 反汇编器 `debug.c` 逐字节对齐）：
 *   opcode(1) + func_const(2)
 *   + upvalue_count × { is_local(2) index(2) is_value_capture(2) }
 *   ⇒ 指令总长 = 3 + 6 × upvalue_count
 *   （注意：**操作数区**从 `ip+1` 开始，捕获记录从 `ip+3` 开始；早期版本把总长
 *     写成 `2 + 6n`（漏了 opcode 那 1 字节），后果是字节流从闭包之后整体错位 ——
 *     因为错位后仍可能解出"合法"的 opcode 序列，所以 scan 不会报错，
 *     只会让后面的局部槽存取落到错槽上。教训：长度必须与反汇编器逐字节核对。）
 * upvalue_count 记在**常量表里那个函数对象**上 —— 这正是 `opcode_size(ip)` 拿不到
 * chunk 而故意返回 -1（"长度未知"）的原因（R3）。本函数补上带 chunk 的版本，
 * 并把捕获形态拆成三类计数：
 *   ref_local  : is_local=1 & is_value_capture=0  → C3（引用捕获本帧 locals，最危险）
 *   value_local: is_local=1 & is_value_capture=1  → C2（值捕获，安全）
 *   byup       : is_local=0                       → C1（复用外层 upvalue，安全）
 *
 * 解析不出来返回 -1（调用方按"长度未知"照旧拒收，绝不猜长度）。
 */
static int closure_probe(Chunk* chunk, const uint8_t* ip,
                         int* out_caps, int* out_ref_local,
                         int* out_val_local, int* out_byup) {
    if (out_caps) *out_caps = 0;
    if (out_ref_local) *out_ref_local = 0;
    if (out_val_local) *out_val_local = 0;
    if (out_byup) *out_byup = 0;
    if (!chunk || !chunk->constants) return -1;

    uint16_t ci = rd_short(ip + 1);
    if ((int)ci >= chunk->const_cnt) return -1;

    Value fv = chunk->constants[ci];
    if (!val_is_obj(fv) || val_as_obj(fv)->type != OBJ_FUNCTION) return -1;
    ObjFunction* fn = (ObjFunction*)val_as_obj(fv);

    int n = fn->upvalue_count;
    if (n < 0 || n > 256) return -1;   /* MAX_UPVALUES；越界视为不可信 */

    for (int i = 0; i < n; i++) {
        const uint8_t* p = ip + 3 + i * 6;
        uint16_t is_local = rd_short(p);
        uint16_t index    = rd_short(p + 2);
        uint16_t is_value = rd_short(p + 4);
        (void)index;
        if (is_local) {
            if (is_value) { if (out_val_local) (*out_val_local)++; }
            else          { if (out_ref_local) (*out_ref_local)++; }
        } else {
            if (out_byup) (*out_byup)++;
        }
    }
    if (out_caps) *out_caps = n;
    return 3 + 6 * n;   /* opcode(1) + func_const(2) + 6 × 捕获记录 */
}

int opcode_size_chunk(Chunk* chunk, const uint8_t* ip) {
    if (*ip == OP_CLOSURE)
        return closure_probe(chunk, ip, NULL, NULL, NULL, NULL);
    return opcode_size(ip);
}

/* R5-P0 计量输出：每次遇到 OP_CLOSURE 打一行形态，便于 Group-Object 统计。
 *
 * ⚠ 为什么要单独的开关：`LENO_JIT_DEBUG=1` 会把每次编译尝试的 body raw hex
 * （最多 1200 字节）+ opcode 走查整段打印，真实负载上输出量极大（观测时表现为
 * "跑不完"）。做形态盘点只需要下面这一行，所以给一个**只打形态**的轻量开关：
 *   LENO_JIT_CLOSURE=1  → 仅输出 CLOSURE(...) 形态行与闭包拒收行。 */
static int jit_closure_probe_on(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("LENO_JIT_CLOSURE") != NULL ? 1 : 0;
    return cached;
}

/* 形态行/闭包拒收行是否输出：全量 debug 或轻量开关任一开启。 */
static int jit_closure_log_on(void) {
    return jit_debug_on() || jit_closure_probe_on();
}

static void jit_debug_closure_shape(Chunk* chunk, const uint8_t* ip, int off, const char* who) {
    int caps = 0, ref_local = 0, val_local = 0, byup = 0;
    if (closure_probe(chunk, ip, &caps, &ref_local, &val_local, &byup) < 0) return;
    fprintf(stderr, "[JIT-CLOSURE] %s caps=%d ref_local=%d value_local=%d byup=%d at offset %d\n",
            who, caps, ref_local, val_local, byup, off);
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
static int scan_callee_for_inline(Chunk* cc, ObjModule* callee_module,
                                   int local_count,
                                   int base_scratch,
                                   int callee_local_map[256],
                                   int* out_max_vstack) {
    /* R6-c v2：内联侧的拒收计数按**被调方 chunk** 去重（"有多少个不同的被调函数
     * 因该原因不能被内联"），这是这一侧真正有意义的指标。 */
    jit_gaps_set_inline_id((const void*)cc);
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
        /* R5-P0：用带 chunk 的解析，让 OP_CLOSURE 也能算出长度（只读计量） */
        int size = opcode_size_chunk(cc, ip);
        if (op == OP_CLOSURE && size > 0) {
            /* 含闭包创建的被调方暂不能内联（与 P0 之前同为拒绝，这里补上形态计量） */
            if (jit_closure_log_on()) {
                jit_debug_closure_shape(cc, ip, (int)(ip - cc->code), "inline");
                fprintf(stderr, "[JIT-CLOSURE] inline-scan FAIL: 含 OP_CLOSURE（R5 未支持）at off %d\n",
                        (int)(ip - cc->code));
            }
            jit_gaps_record_inline("含 OP_CLOSURE");
            return 0;
        }
        if (size < 0 || ip + size > end) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: opcode %d size=%d at off %d\n",
                        op, size, (int)(ip - cc->code));
            jit_gaps_record_inline("长度未知/越界 op=%d(%s) size=%d", op, opcode_name(op), size);
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
            case OP_STRING_ADD:                         /* pop 2 push 1 → net -1，R2 批次 2 */
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
            case OP_IS_NULL:  /* pop 1 push 1 → net 0，R2 批次 1 */
            case OP_TYPE_CHECK: /* pop 1 push 1（`is`）→ net 0，R2 批次 3 */
            case OP_AS_CAST:    /* pop 1 push 1（`as`）→ net 0，R6-g */
            case OP_GET_FIELD_ADDR: /* pop 1（cstruct）push 1（字段地址）→ net 0，R6-h */
            case OP_SET_PTR_ELEM_TYPE: case OP_SET_DECLARED_FACE: /* peek TOS → net 0 */
                break;
            case OP_GET_MODULE_VAR: case OP_SET_MODULE_VAR: case OP_GET_MODULE_FUNC: {
                /* R7①：模块变量/函数访问**能否内联**，取决于「被调函数与调用方是否同一个模块」。
                 *
                 * 内联后 codegen 用的是**编译期嵌入**的 module（`jit_scan_get_module()`，
                 * 见 jit_priv.h）：它等于调用方函数的 module —— 因为函数级 JIT 的快路径
                 * 不压帧，编译期就得知道是哪个模块（§8.55/§8.56 的教训）。
                 * ⇒ 同模块时该指针正确（模块变量的 index 就是该模块 globals 的下标），
                 *   跨模块时它会指向**调用方**的模块 ⇒ 用错 globals 下标，静默读错变量。
                 * 所以只在「callee 与 caller 同模块且都非 NULL」时放行，其余照旧拒绝内联
                 * （循环本身仍可 JIT，只是不内联该函数）。 */
                if (!callee_module || callee_module != jit_scan_get_module()) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: 模块变量访问 op=%d at off %d"
                                        "（callee 与 caller 不同模块）\n",
                                op, (int)(ip - cc->code));
                    jit_gaps_record_inline("模块变量/函数访问 op=%d(%s)（跨模块）", op, opcode_name(op));
                    return 0;
                }
                /* 记账与 scan_loop_body 的同名 case **完全一致**：
                 *   GET_MODULE_FUNC + OP_CALL 配对形态 → `size = 6`、`vstack -= (argc - rc)`
                 *     （rc 来自 callee 的 return_count，解析不出来就拒绝内联）；
                 *   单独取函数值 / 读模块变量 → push 1（+1）；
                 *   写模块变量 → **peek** TOS，不弹不推（net 0）。 */
                if (op == OP_GET_MODULE_FUNC && ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t func_idx = rd_short(ip + 1);
                    int rc = jit_resolve_module_func(func_idx);
                    if (rc <= 0) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: 模块函数 ret_count 不可知"
                                            "（globals[%u]）at off %d\n",
                                    (unsigned)func_idx, (int)(ip - cc->code));
                        jit_gaps_record_inline("模块函数 ret_count 不可知");
                        return 0;
                    }
                    int argc = rd_short(ip + 4);
                    size = 6;                    /* 连同 OP_CALL 一起消费 */
                    vstack -= (argc - rc);
                    break;
                }
                if (op == OP_GET_MODULE_VAR || op == OP_GET_MODULE_FUNC)
                    vstack++;                    /* 单独取变量值 / 函数值：push 1 */
                break;
            }
            case OP_SWITCH_LOOKUP:
                /* §8.61 v1：保守拒绝内联。这条是**变长 + 多目标**控制流：每个 case 体
                 * 都只能「由跳转表进入」，而本函数只是简化的 vstack 走查，没有
                 * scan_loop_body 那套 fwd_targets/dead-code 机制 —— 一旦内联体的栈深
                 * 与 JIT 记账错位就是静默算错。循环本身照常 JIT，只是不内联含 switch
                 * 的函数（与 §8.55 拒绝模块变量访问同一取舍）。 */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: OP_SWITCH_LOOKUP at off %d\n",
                            (int)(ip - cc->code));
                jit_gaps_record_inline("OP_SWITCH_LOOKUP");
                return 0;
            case OP_ARRAY_APPEND_NOPUSH: vstack -= 2; break;
            case OP_DICT_SET: vstack -= 2; break;
            case OP_INDEX_SET_NOPUSH: vstack -= 3; break;
            case OP_RETURN:
                vstack--; break;
            case OP_RETURN_MULTI:
                vstack -= ip[1]; break;
            case OP_CALL: {
                /* ---- 裸 OP_CALL（R7④，2026-09-16）----
                 * callee 是**运行期值**：局部函数值 / 参数 / 字段 / 容器元素
                 * （`var f = foo; f(x)`、`arr[i](x)`、`self.cb(x)` …）。
                 * 记账与 scan_loop_body 的同名 case **完全一致**：
                 *   [args(ac)][callee] → 返回后留下 rc 个值 ⇒ 净效应 = rc - (ac + 1)，
                 *   而 rc 静态不可知（callee 是值）⇒ **按 rc = 1 记账**，
                 *   由 callout 在**调用前**用 callee 的 return_count 复核：
                 *   不等于 1 就 bailout（多返回值解构调用点交解释器）。
                 * 少了这个 case 的后果：**被调函数体内只要有一次"经函数值调用"，
                 * 该函数就永远不能被内联进热循环**（落到 default 报
                 * `inline-scan FAIL: unsupported opcode 59`）—— 实测 file_manager 里
                 * 有 20 个这样的被调方（`LENO_JIT_GAPS=1`）。 */
                int ac = rd_short(ip + 1);
                vstack -= ac;
                break;
            }
            case OP_TAIL_CALL:
                /* R6-b：内联体里的尾调用**不能**内联 —— 尾调用要求「callee 的结果直接成为
                 * **外层函数**的返回值」，而内联机制只能把结果变成「这次调用的结果」
                 * （jump 到 inline_end）。语义不等价 ⇒ 拒绝内联该 callee（循环照常 JIT）。
                 * 与 OP_SWITCH_LOOKUP 同一取舍：宁可不编，不要猜。 */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: OP_TAIL_CALL at off %d\n",
                            (int)(ip - cc->code));
                jit_gaps_record_inline("OP_TAIL_CALL");
                return 0;
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
            case OP_GET_GLOBAL_FUNC:
                /* push global_funcs[slot]（函数值）→ vstack++。
                 * 与 scan_loop_body 的同名 case 一致（R6-b 补的那条）；内联侧此前没有 ⇒
                 * 被调函数体内只要有 `var f = foo`（取全局函数值）就不能内联。 */
                vstack++;
                break;
            case OP_GET_CSTRUCT_DEF:
                /* 与 scan_loop_body 的同名 case 一致（R6-d）：push def → vstack++。
                 * 查找在 callout 里按名字做（不做编译期解析，理由见 ops_callout.inc
                 * 的 case 注释：定义运行时注册、重声明会覆盖并废弃旧 def）。 */
                vstack++;
                break;
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
                    jit_gaps_record_inline("INVOKE_METHOD ret_count 无法确定");
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
            case OP_GET_METHOD: {
                /* 窥孔：`obj.m(args)` 的**动态派发**形态 = `OP_GET_METHOD name(2)` +
                 * `OP_CALL argc(2)`（face 接收者调方法，或接收者静态类型解析不出来时的
                 * struct 方法调用）—— roadmap R7②：这条 case 长期缺失 ⇒ 落到 default，
                 * 报 `inline-scan FAIL: unsupported opcode 134` ⇒ **只要被调函数体里有
                 * 一次动态派发方法调用，该函数就永远不能被内联进热循环**
                 * （循环本身照常 JIT，只是这个调用点退回 callout / 函数级 JIT）。
                 *
                 * 记账与 scan_loop_body 的同名 case **完全一致**：
                 *   栈布局 [self][args...] 之外，调用方再多压一个 receiver 供 GET_METHOD
                 *   消费，随后是 `OP_CALL(argc)`（argc 含 self）⇒ 合并净效应 = -(argc + 1) + rc。
                 *   rc 只能按「方法名在所有 struct def 中唯一且 return_count 一致」推断
                 *   （内联体没有 INVOKE_METHOD_TYPED 那样的静态类型名常量可用）——
                 *   **推不出来就拒绝内联**：内联体的栈深记账一旦错位就是静默算错，
                 *   宁可不内联。
                 * 判定条件必须与 codegen（ops_callout.inc 的 case OP_GET_METHOD）
                 * 一字不差，且 `end` 与 codegen 的内联体一致（都是
                 * callee_chunk->code + callee_body_size），否则 size 记账错位。 */
                if (ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t gm_name_idx = rd_short(ip + 1);
                    int gm_rc = jit_resolve_method_ret_count(cc, gm_name_idx);
                    if (gm_rc <= 0) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: GET_METHOD 的 ret_count 不可知（方法名唯一性推断失败）at off %d\n",
                                    (int)(ip - cc->code));
                        jit_gaps_record_inline("GET_METHOD 的 ret_count 不可知");
                        return 0;
                    }
                    int gm_argc = rd_short(ip + 4);
                    size = 6;                       /* 连同 OP_CALL 一起消费 */
                    vstack -= (gm_argc + 1 - gm_rc);
                    break;
                }
                /* 独立的 OP_GET_METHOD（只取方法值、不调用）：pop 1(obj) push 1(方法值)
                 * → net 0。codegen 走 callout（§8.66）。 */
                break;
            }
            case OP_CALL_NATIVE: {
                uint16_t ac = rd_short(ip + 3);
                vstack -= (ac - 1);
                break;
            }
            case OP_CLIB_CALL: {
                /* 与 scan_loop_body 的同名 case 一致（R6-e）：FFI 动态库调用
                 * → 弹 arg_count（含 lib/func_name）压 1 个结果 ⇒ 净 -(arg_count-1)。
                 * 内联体里允许它：callout 属可内联的调用形态（与 OP_CALL_NATIVE 同）。 */
                uint16_t cc_ac = rd_short(ip + 1);
                if (cc_ac < 2) {
                    jit_gaps_record_inline("OP_CLIB_CALL arg_count<2（畸形）");
                    return 0;
                }
                vstack -= (cc_ac - 1);
                break;
            }
            case OP_CLEAR_LOCAL_RANGE: break;
            default:
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] inline-scan FAIL: unsupported opcode %d at off %d\n",
                            op, (int)(ip - cc->code));
                jit_gaps_record_inline("unsupported opcode %d(%s)", op, opcode_name(op));
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
        /* R5-P0：OP_CLOSURE 的长度依赖常量表 ⇒ 走带 chunk 的解析。
         * 解析成功时**只计量、继续扫描**（把同一循环体里的闭包形态与其他缺口一次测全），
         * 最终由末尾的 closure_seen 统一拒收 —— 对编译结果而言行为不变。 */
        int size = opcode_size_chunk(chunk, ip);
        if (op == OP_CLOSURE && size > 0) {
            int clo_caps = 0, clo_rl = 0, clo_vl = 0, clo_by = 0;
            closure_probe(chunk, ip, &clo_caps, &clo_rl, &clo_vl, &clo_by);
            if (clo_rl == 0) {
                /* ---- R5-P1 / P2b / P3：可放行的闭包 ----
                 * (a) 零捕获（C0）：不新建 upvalue、不捕获帧槽，只分配闭包对象；
                 * (b) by-upvalue（C1，`is_local=0`）：复制**当前闭包**的 upvalues[index]
                 *     指针（VM op_call.inc:600-611），生命周期归 upvalue 的创建者；
                 * (c) 值捕获（C2，`is_local=1 & is_value_capture=1`）：读**本帧局部槽的
                 *     值**，建成 closed upvalue（VM op_call.inc:577-581）—— 这正是语义分析
                 *     给"循环体内声明的变量"标的形态（semantic_upvalue.c:200）。
                 * 三者都**不产生 open upvalue** ⇒ 不触碰"locals 没有稳定 Value*"
                 * 这个核心冲突（设计文档 §3/§4，不变量 I1）。
                 *
                 * ⚠ C2 的代价：局部槽的值必须先压到 JIT 栈上才能交给 callout
                 * （callout 看不到 scratch/寄存器）。压入/弹出都在本指令内部完成，
                 * 但**峰值会临时抬高 vstack** ⇒ 必须计入 max_vstack，否则帧尺寸
                 * 不够、压栈会踩到帧内其它槽（tmp/co/closure 区）。 */
                if (clo_vl > 0) {
                    int clo_peak = vstack + clo_vl;
                    if (clo_peak > r->max_vstack) r->max_vstack = clo_peak;
                }
                if (jit_closure_log_on())
                    jit_debug_closure_shape(chunk, ip, (int)(ip - body_start),
                                            clo_caps == 0 ? "scan:ALLOW-C0"
                                                          : (clo_vl > 0 ? "scan:ALLOW-C2" : "scan:ALLOW-C1"));
            } else {
                /* C3（引用捕获本帧局部）未实现（P4）⇒ 维持拒收：它需要把本帧某个
                 * 局部槽的**地址**长期交给新闭包，而 JIT 的 locals 在 scratch/机器栈上
                 * （迭代即复用、退出即失效）。 */
                if (jit_closure_log_on())
                    jit_debug_closure_shape(chunk, ip, (int)(ip - body_start), "scan:REJECT");
                /* R6-c：C3 单独记账 —— 这是"P4（C3 提升槽）值不值得做"的 **gate 数字**：
                 * 计数 = 有多少个**热循环**（能走到编译尝试 ⇒ 回边 ≥ JIT_HOT_THRESHOLD）
                 * 因为 C3 被整循环拒收。见 §8.79。 */
                jit_gaps_record("OP_CLOSURE C3（引用捕获本帧局部，P4 未实现）");
                r->closure_seen = 1;
            }
        }
        if (size < 0) {
            if (jit_debug_on())
                fprintf(stderr, "[JIT-DEBUG] scan FAIL: unknown opcode %d（长度未知，R3）at offset %d\n",
                        op, (int)(ip - body_start));
            jit_gaps_record("unknown opcode %d(%s)（长度未知）", op, opcode_name(op));
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
            case OP_STRING_ADD:
                /* 字符串拼接（字符串插值编译出）：pop 2 push 1 → net -1，R2 批次 2 */
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
            case OP_IS_NULL:
                /* pop 1 push 1（判空结果 bool）→ net 0，R2 批次 1（见 ops_misc.inc） */
                break;
            case OP_TYPE_CHECK:
                /* pop 1 push 1（`is` 的 bool 结果）→ net 0，R2 批次 3 */
                break;
            case OP_AS_CAST:
                /* pop 1 push 1（`as` 的转换结果或 null）→ net 0，R6-g。
                 * 转换语义在 vm_as_cast（vm.c，与解释器共用）⇒ 这里只记栈效应。 */
                break;
            case OP_GET_FIELD_ADDR:
                /* pop 1（cstruct 实例）push 1（字段地址 FFI 指针）→ net 0，R6-h。
                 * 构造语义在 cstruct_field_addr_new（object_cstruct.c，与解释器共用）。 */
                break;
            case OP_SET_PTR_ELEM_TYPE: case OP_SET_DECLARED_FACE:
                /* 存值前的类型标记：**peek TOS**、不弹不推 → net 0（见 op_unary.inc） */
                break;
            case OP_GET_MODULE_FUNC: {
                /* 窥孔：`foo(x)` 的模块函数调用形态 = OP_GET_MODULE_FUNC + OP_CALL
                 * （codegen_expr.c 的 SYM_MODULE 分支：函数值先 GET_MODULE_FUNC，再 OP_CALL）。
                 * 与 OP_CALL_GLOBAL_FUNC_TYPED 同构：JIT 的栈记账必须知道调用后留下几个
                 * 返回值，它来自 callee 的 ObjFunction.return_count（编译期算好；无显式
                 * return 按 1 个=隐式 null；-1=静态不可知）。callee 只能从「函数所属模块的
                 * globals[idx]」解析 → 解析不出来就拒绝整个循环（宁可不编，不要猜）。 */
                if (ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t func_idx = rd_short(ip + 1);
                    int rc = jit_resolve_module_func(func_idx);
                    if (rc <= 0) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] scan FAIL: 模块函数 ret_count 不可知"
                                            "（globals[%u]）at offset %d\n",
                                    (unsigned)func_idx, (int)(ip - body_start));
                        r->capable = 0;
                        return;
                    }
                    int argc = rd_short(ip + 4);
                    size = 6;               /* 连同 OP_CALL 一起消费 */
                    vstack -= (argc - rc);  /* 压 callee(1) + argc 实参 → 留 rc 个返回值 */
                    break;
                }
                /* 单独取函数值：push 1 → net +1 */
                vstack++; break;
            }
            case OP_GET_MODULE_VAR:
                /* 模块变量读取：push 1 → net +1 */
                vstack++; break;
            case OP_SET_MODULE_VAR:
                /* 模块变量写入：**peek** TOS（不弹）→ net 0 */
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
            case OP_TAIL_CALL: {
                /* ---- R6-b：尾调用 ----
                 * VM 语义（op_call.inc:35-134）：关闭本帧 upvalue → 释放本帧 locals →
                 * **复用本帧**执行 callee ⇒ **永不返回本函数**（结果直接给本函数的调用者）。
                 * 对两种模式的含义完全不同：
                 *   · 循环模式 —— 与 OP_RETURN 完全同类（loop JIT 无法从机器码返回函数），
                 *     置 has_reachable_return 让 jit_compile 拒绝整循环；
                 *   · 函数模式 —— 实现为「调用 + 发布结果 + epilogue」
                 *     （见 ops_callout.inc 的同名 case），它**不检查**这个标志 ⇒ 正常编译。
                 * 记账：[args(ac)][callee] 一次性全部消费（后面的代码不可达）。 */
                int tc_ac = rd_short(ip + 1);
                vstack -= (tc_ac + 1);
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
            case OP_CALL: {
                /* 裸 OP_CALL：callee 是**运行时值**（前一条指令压的局部闭包 / 回调表元素 /
                 * 字段 …；探针实测形态：`OP_GET_LOCAL f` + `OP_CALL` 或 `OP_INDEX` + `OP_CALL`）。
                 * 栈约定与 VM 一致：[args(ac)][callee]（op_call.inc 的 `peek(0)`），
                 * 返回后留下 rc 个值 ⇒ 净效应 = rc - (ac + 1)。
                 * rc 静态不可知（callee 是值），这里**按 rc = 1 记账**，由 callout 在调用前
                 * 用 `closure->function->return_count` 复核：不等于 1 就 bailout（多返回解构
                 * 调用点交解释器），循环侧有 JIT_BAILOUT_LIMIT 兜底 ⇒ 代价有界。 */
                int ac = rd_short(ip + 1);
                vstack -= ac;          /* = 1 - (ac + 1) */
                break;
            }
            case OP_GET_METHOD: {
                /* 窥孔：`obj.m(args)` 的**动态派发**形态 = `OP_GET_METHOD name(2)` + `OP_CALL argc(2)`
                 * （face 调用，或接收者静态类型解析不出来时的 struct 方法调用）。
                 * 栈布局（用最小探针反汇编实测）：
                 *     [self][args...] 已就位，调用方**再多压一个 receiver** 供 GET_METHOD 消费，
                 *     然后 OP_CALL(argc)（argc 含 self）。
                 *   ⇒ 合并后的净效应 = -(argc + 1) + rc（那个多压的 receiver 被 GET_METHOD 吃掉）。
                 *   原生方法形态（先压实参、再压 receiver，OP_CALL 的 argc 不含 self）也恰好是
                 *   同样的「argc + 1 个值待消费」布局，所以记账统一。
                 * rc 只能按「方法名在所有 struct def 中唯一且 return_count 一致」推断
                 * （这里没有 INVOKE_METHOD_TYPED 那样的静态类型名常量）—— 推不出来就拒绝整个循环。 */
                if (ip + 6 <= end && ip[3] == OP_CALL) {
                    uint16_t gm_name_idx = rd_short(ip + 1);
                    int gm_rc = jit_resolve_method_ret_count(chunk, gm_name_idx);
                    if (gm_rc <= 0) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] scan FAIL: GET_METHOD 的 ret_count 不可知"
                                            "（方法名唯一性推断失败）at offset %d\n",
                                    (int)(ip - body_start));
                        r->capable = 0;
                        return;
                    }
                    int gm_argc = rd_short(ip + 4);
                    size = 6;                          /* 连同 OP_CALL 一起消费 */
                    vstack -= (gm_argc + 1 - gm_rc);
                    break;
                }
                /* 独立的 OP_GET_METHOD（只取方法值、不调用）：解释器要建 bound method /
                 * 闭包 ⇒ 走 callout（§8.66）。栈效应 pop 1(obj) push 1(方法值) → net 0。
                 * （codegen 用**完全相同的条件**分辨这两种形态，size 记账才不会错位。） */
                break;
            }
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
                            if (scan_callee_for_inline(mf->chunk, mf->module, callee_lc, base,
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
                                    if (scan_callee_for_inline(cc, func2->module, callee_lc, base,
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
            case OP_GET_GLOBAL_FUNC: {
                /* ---- R6-b 前置：push global_funcs[slot]（函数值）→ vstack++ ----
                 * 为什么必须补：`return f(x)` 的尾调用形态是
                 * `OP_GET_GLOBAL_FUNC + OP_TAIL_CALL`（**不是**合体的
                 * OP_CALL_GLOBAL_FUNC），而这条指令此前没有 case ⇒ 落在 default 被报成
                 * "unsupported opcode 18" ⇒ **含尾调用的函数整体被拒**（实测：`scale`）。
                 * 顺带也解锁「取函数值」的一般形态（`var f = foo` 后再调用：
                 * `OP_GET_GLOBAL_FUNC + OP_CALL`）。
                 * 越界由 callout 置 failed → bailout，报错文本交解释器。 */
                vstack++;
                break;
            }
            case OP_CLIB_CALL: {
                /* ---- R6-e：FFI 动态库调用 → 净 -(arg_count-1) ----
                 * 解释器语义（op_clib_call.inc）：VM 栈 [lib, func_name, user_args...]，
                 * arg_count = user_arg_count + 2（含前两个）⇒ 弹 arg_count、压 1 个结果。
                 * 长度是变长的（5 + ip[4]），`opcode_size` 已收录 —— 之前只是没有 case，
                 * 所以被报成 "unsupported opcode 142"（§8.77 census 的"已收录长度、未实现"）。
                 * arg_count < 2 属畸形字节码：拒收，交解释器（那里有自己的报错路径）。 */
                uint16_t cc_argc = rd_short(ip + 1);
                if (cc_argc < 2) {
                    r->capable = 0;
                    return;
                }
                vstack -= (cc_argc - 1);
                break;
            }
            case OP_GET_CSTRUCT_DEF: {
                /* ---- R6-d：push cstruct 定义对象 → vstack++ ----
                 * 解释器语义（op_cstruct.inc:103-129）：名字常量（字符串）→
                 * `cstruct_def_find` → 查到 push def，查不到报"找不到 cstruct 定义 'X'"。
                 * codegen 走 callout，**每次执行现查**（不做编译期解析：定义是运行时注册的、
                 * 同名重声明还会覆盖条目并废弃旧 def ⇒ 嵌入指针会分叉；见 ops_callout.inc
                 * 的 case 注释）。查不到 → failed → bailout，报错文本交解释器。 */
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
            case OP_SWITCH_LOOKUP: {
                /* switch 查找表（§8.61）。编码：const_idx(2) case_count(2)
                 * default_off(4) [body_off(4)]...；**偏移基准 = 指令起点 + size**
                 * （= 偏移表之后，见 op_switch_lookup.inc 的 frame->ip 推进）。
                 * 栈效应：弹出 switch 值、不压回 → net -1。
                 * 每个 case 体与 default 体都是一个**前向跳转目标**，且都从
                 * "弹出后的 vstack" 开始（与 if/else 的各分支同理）。 */
                uint16_t case_count = (uint16_t)((ip[3] << 8) | ip[4]);
                int32_t default_off = rd_int32(ip + 5);
                int base_bc = bc_off + size;   /* 各 body_off 的基准 */
                vstack--;                      /* 弹 switch 值 */
                for (int k = 0; k <= (int)case_count; k++) {
                    int32_t body_off = (k < (int)case_count)
                                           ? rd_int32(ip + 9 + k * 4) : default_off;
                    int target_bc = base_bc + body_off;
                    MARK_JT_FWD(target_bc);   /* §8.46：目标偏移不接受回看 */
                    if (fwd_count >= JIT_SCAN_MAX_FWD) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] scan FAIL: 前向跳转目标超过 %d 个"
                                            "（OP_SWITCH_LOOKUP case_count=%u）→ 拒绝 JIT\n",
                                    JIT_SCAN_MAX_FWD, (unsigned)case_count);
                        r->capable = 0;
                        return;
                    }
                    fwd_targets[fwd_count].target_bc = target_bc;
                    fwd_targets[fwd_count].vstack = vstack;
                    fwd_count++;
                }
                dead = 1;   /* switch 之后没有 fall-through：体一律由跳转表进入 */
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
            case OP_CLOSURE: {
                /* R5-P1：`OP_CLOSURE` 是「弹 0 压 1」（长度/形态在上面的 OP_CLOSURE 块里
                 * 解析：零捕获放行，C1/C2/C3 置 closure_seen 由末尾统一拒收）。 */
                vstack++;
                break;
            }
            case OP_GET_UPVALUE: {
                /* R5-P2：读捕获变量 = 弹 0 压 1。语义与长度见 op_variables.inc /
                 * jit_scan.c 的 3 字节长度表；codegen 用本帧闭包 + slot 走 callout。 */
                vstack++;
                break;
            }
            case OP_SET_UPVALUE: {
                /* R5-P2：写捕获变量 = **peek TOS**（不弹不压），net 0。 */
                break;
            }
            default:
                /* R3：走到这里说明**长度已登记**（上面 size<0 已经拦掉了未知长度），
                 * 只是还没实现 → 报 "unsupported" 而不是 "unknown"，两者一眼可分。 */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] scan FAIL: unsupported opcode %d（已收录长度、未实现）at offset %d\n",
                            op, (int)(ip - body_start));
                jit_gaps_record("unsupported opcode %d(%s)（已收录长度、未实现）", op, opcode_name(op));
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

    /* R5-P0：本次扫描遇到过 OP_CLOSURE ⇒ 统一拒收（与 P0 之前的行为一致：
     * 以前是在第一条闭包处直接 return，现在改成走完再拒，只为把形态测全）。
     * 放行闭包要等 P1（C0）/P2（C1）/P3（C2）的实现，见设计文档。 */
    if (r->closure_seen) {
        if (jit_closure_log_on())
            fprintf(stderr, "[JIT-CLOSURE] scan REJECT: 含 OP_CLOSURE（R5 未支持）body_size=%d\n", body_size);
        jit_gaps_record("含 OP_CLOSURE（合计：见上面各形态明细）");
        r->capable = 0;
        return;
    }

    if (jit_debug_on()) {
        fprintf(stderr, "[JIT-DEBUG] scan result: n_locals=%d max_vstack=%d\n", r->num_locals, r->max_vstack);
        for (int i = 0; i < r->num_locals; i++) {
            fprintf(stderr, "[JIT-DEBUG]   scratch[%d] = slot %d\n", i, r->local_slots[i]);
        }
        fprintf(stderr, "[JIT-DEBUG]   for_loop_var_slot=%d for_end_slot=%d for_step_slot=%d inclusive=%d\n",
                r->for_loop_var_slot, r->for_end_slot, r->for_step_slot, r->for_inclusive);
    }
}

