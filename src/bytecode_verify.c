#include "include/bytecode_verify.h"
#include <stdint.h>
#include <stdlib.h>

// ============================================================================
// .lenb 字节码校验器
// ----------------------------------------------------------------------------
// 反序列化后的 chunk 是**不可信输入**。这里按"指令网格"逐条趟一遍：
//   pass 1：算出每条指令的总长度（含所有变长尾随数据），并标记合法的指令起点；
//   pass 2：用起点位图校验跳转目标，同时校验常量索引 / 寄存器（槽位）索引。
//
// 长度规则必须与 codegen 的发射顺序、debug.c 的反汇编、VM 的读取顺序**三处一致**；
// 任何一处漏掉一条变长指令，后面整段网格就错位（历史事故：dump 后半段全是 OP_NOP）。
// 详见 docs/VM字节码校验与崩溃防护设计方案.md。
// ============================================================================

#define VERIFY_MAX_REG 256               // 与 codegen 的 MAX_REG 同口径（寄存器号 8 位）
#define VERIFY_CLIB_MAX_ARGS 12          // codegen 的 CLIB_CALL_MAX_ARGS
#define VERIFY_CFUNC_MAX_ARGS 12         // VM OP_CFUNC_CALLBACK 的上限
#define VERIFY_SPEC_MAX_DEPTH 64         // 与 vm.c 的 type_spec_size_impl 同口径

typedef struct {
    const Chunk* c;
    int p;      // 绝对偏移（chunk->code 下标）
    int bad;    // 1 = 越界 / 截断
} Cur;

static int cur_u8(Cur* k) {
    if (k->p < 0 || k->p >= k->c->len) { k->bad = 1; k->p++; return 0; }
    return (int)k->c->code[k->p++];
}

static int cur_u16(Cur* k) {
    int hi = cur_u8(k);
    int lo = cur_u8(k);
    return (hi << 8) | lo;
}

static int const_ok(const Chunk* c, int idx) {
    return idx >= 0 && idx < c->const_cnt;
}

static int ck_const(Cur* k, int idx) {
    if (!const_ok(k->c, idx)) { k->bad = 1; return 0; }
    return 1;
}

static int ck_const_direct(const Chunk* c, int idx) {
    return const_ok(c, idx);
}

// ----------------------------------------------------------------------------
// 尾随数据的长度/边界扫描（checks=1 时同时校验其中的常量索引）
// ----------------------------------------------------------------------------

// OP_STRUCT_DEF：字段/方法/impl/泛型参数/关联常量表
static int scan_struct_def(const Chunk* c, int base, int checks, Cur* k) {
    k->c = c; k->p = base; k->bad = 0;
    int fields = cur_u8(k);
    int methods = cur_u8(k);
    int impls = cur_u8(k);
    for (int i = 0; i < impls && !k->bad; i++) {
        int idx = cur_u16(k);
        if (checks) ck_const(k, idx);
    }
    int tps = cur_u8(k);
    for (int i = 0; i < tps && !k->bad; i++) {
        int idx = cur_u16(k);
        if (checks) ck_const(k, idx);
    }
    for (int i = 0; i < fields && !k->bad; i++) {
        int name_idx = cur_u16(k);
        if (checks) ck_const(k, name_idx);
        int ftype = cur_u8(k);
        cur_u8(k);                                    // nullable
        if (ftype == TYPE_STRUCT) {
            int has_name = cur_u8(k);
            if (has_name) {
                int idx = cur_u16(k);
                if (checks) ck_const(k, idx);
            }
        }
        if (ftype == TYPE_PTR_GENERIC) cur_u8(k);     // 元素类型
        int has_default = cur_u8(k);
        if (has_default) {
            int idx = cur_u16(k);
            if (checks) ck_const(k, idx);
        }
    }
    for (int i = 0; i < methods && !k->bad; i++) {
        int name_idx = cur_u16(k);
        int func_idx = cur_u16(k);
        if (checks) {
            ck_const(k, name_idx);
            ck_const(k, func_idx);
        }
    }
    int flags = cur_u8(k);
    if (flags & 1) cur_u8(k);                         // ctor 下标
    if (flags & 2) cur_u8(k);                         // dtor 下标
    int consts = cur_u8(k);
    for (int i = 0; i < consts && !k->bad; i++) {
        int name_idx = cur_u16(k);
        int value_idx = cur_u16(k);
        if (checks) {
            ck_const(k, name_idx);
            ck_const(k, value_idx);
        }
    }
    return !k->bad;
}

// OP_FACE_DEF：方法签名表
static int scan_face_def(const Chunk* c, int base, int checks, Cur* k) {
    k->c = c; k->p = base; k->bad = 0;
    int methods = cur_u8(k);
    int tps = cur_u8(k);
    for (int i = 0; i < tps && !k->bad; i++) {
        int idx = cur_u16(k);
        if (checks) ck_const(k, idx);
    }
    for (int i = 0; i < methods && !k->bad; i++) {
        int name_idx = cur_u16(k);
        if (checks) ck_const(k, name_idx);
        int pc = cur_u8(k);
        cur_u8(k);                                    // 返回类型
        for (int j = 0; j < pc && !k->bad; j++) cur_u8(k);
    }
    return !k->bad;
}

// OP_ENUM_DEF：成员表（名字常量 + 值常量）
static int scan_enum_def(const Chunk* c, int base, int checks, Cur* k) {
    k->c = c; k->p = base; k->bad = 0;
    int members = cur_u8(k);
    for (int i = 0; i < members && !k->bad; i++) {
        int name_idx = cur_u16(k);
        int value_idx = cur_u16(k);
        if (checks) {
            ck_const(k, name_idx);
            ck_const(k, value_idx);
        }
    }
    return !k->bad;
}

// OP_CSTRUCT_DEF：字段布局表
static int scan_cstruct_def(const Chunk* c, int base, int checks, Cur* k) {
    k->c = c; k->p = base; k->bad = 0;
    int fields = cur_u8(k);
    cur_u16(k);                                       // total_size
    cur_u8(k);                                        // alignment
    cur_u8(k);                                        // is_packed
    cur_u8(k);                                        // explicit_align
    for (int i = 0; i < fields && !k->bad; i++) {
        int name_idx = cur_u16(k);
        if (checks) ck_const(k, name_idx);
        int ftype = cur_u8(k);
        cur_u16(k);                                   // offset
        cur_u16(k);                                   // array_dim
        int sname_idx = cur_u16(k);                   // 0xFFFF = 无嵌套 cstruct
        if (sname_idx != 0xFFFF && checks) ck_const(k, sname_idx);
        if (ftype == TYPE_PTR_GENERIC) cur_u8(k);     // 元素类型
    }
    return !k->bad;
}

// OP_STRUCT_INIT：名字 + 泛型实参 + 字段名表 + 3 字节 module_slot
static int scan_struct_init(const Chunk* c, int base, int checks, Cur* k, int field_count) {
    k->c = c; k->p = base; k->bad = 0;
    int name_idx = cur_u16(k);
    if (checks) ck_const(k, name_idx);
    int gc = cur_u8(k);
    for (int i = 0; i < gc && !k->bad; i++) {
        int idx = cur_u16(k);
        if (checks) ck_const(k, idx);
    }
    for (int i = 0; i < field_count && !k->bad; i++) {
        int idx = cur_u16(k);
        // 0 = 按位置赋值（不是常量索引）
        if (checks && idx != 0) ck_const(k, idx);
    }
    cur_u8(k);                                        // mod_space
    cur_u16(k);                                       // mod_slot
    return !k->bad;
}

// OP_CLIB_CALL：返回类型(1) + min(nargs,12) 个参数类型
static int scan_clib_call(const Chunk* c, int base, Cur* k, int nargs) {
    k->c = c; k->p = base; k->bad = 0;
    cur_u8(k);
    int lim = nargs < VERIFY_CLIB_MAX_ARGS ? nargs : VERIFY_CLIB_MAX_ARGS;
    for (int i = 0; i < lim && !k->bad; i++) cur_u8(k);
    return !k->bad;
}

// OP_CFUNC_CALLBACK：返回类型(1) + 参数个数(1) + 参数类型
static int scan_cfunc_callback(const Chunk* c, int base, Cur* k) {
    k->c = c; k->p = base; k->bad = 0;
    cur_u8(k);                                        // ret type
    int pcnt = cur_u8(k);
    for (int i = 0; i < pcnt && !k->bad; i++) cur_u8(k);
    return !k->bad;
}

// 类型规格（OP_TYPE_CHECK_SPEC / OP_AS_CAST_SPEC 的尾随数据）
//   前缀、自定界；格式见 leno_vm.h。这里校验 kind 合法、名字常量索引在范围内。
static int spec_scan(const Chunk* c, int p, int end, int checks, int depth, int* out) {
    if (depth > VERIFY_SPEC_MAX_DEPTH) return 0;
    if (p < 0 || p >= end) return 0;
    int kind = (int)c->code[p++];
    if (kind < 0 || kind >= (int)TYPE_KIND_COUNT) return 0;
    if (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_ENUM) {
        if (p + 2 > end) return 0;
        int idx = ((int)c->code[p] << 8) | (int)c->code[p + 1];
        p += 2;
        if (checks && !ck_const_direct(c, idx)) return 0;
    } else if (kind == TYPE_ARRAY) {
        if (!spec_scan(c, p, end, checks, depth + 1, &p)) return 0;
    } else if (kind == TYPE_DICT) {
        if (!spec_scan(c, p, end, checks, depth + 1, &p)) return 0;
        if (!spec_scan(c, p, end, checks, depth + 1, &p)) return 0;
    } else if (kind == TYPE_PTR_GENERIC) {
        if (!spec_scan(c, p, end, checks, depth + 1, &p)) return 0;
    }
    *out = p;
    return 1;
}

// ----------------------------------------------------------------------------
// 单条指令：算长度 + 校验
// ----------------------------------------------------------------------------
// 返回该指令占用的总字节数；0 = 非法（*bad 记录位置）。
// starts 非空时（且 checks=1）同时校验跳转目标必须落在合法指令起点。
static int verify_one(const Chunk* c, int pos, int cap,
                      const uint8_t* starts, int checks, int* bad) {
    int len = c->len;
    if (pos < 0 || pos + 4 > len) goto bad;

    uint8_t op = c->code[pos];
    if (op >= (uint8_t)OP_OPCODE_COUNT) goto bad;

    int a = (int)c->code[pos + 1];
    int b = (int)c->code[pos + 2];
    int cc = (int)c->code[pos + 3];
    int size = 4;
    int bx = (b << 8) | cc;

    #define FAIL_AT() do { if (bad) *bad = pos; return 0; } while (0)
    #define REG_CHK(r) do { if (checks && (r) < 0) FAIL_AT(); \
                            if (checks && (r) >= cap) FAIL_AT(); } while (0)
    #define SPAN_CHK(base, n) do { \
        if (checks) { \
            int _b = (base), _n = (n); \
            if (_b < 0 || _n < 0 || _b + _n > cap) FAIL_AT(); \
        } } while (0)
    #define CONST_CHK(idx) do { if (checks && !ck_const_direct(c, (idx))) FAIL_AT(); } while (0)

    switch (op) {
        // --- 空操作 / 装载 / 移动 ---
        case OP_NOP:
            break;
        case OP_LOADK:
            REG_CHK(a);
            CONST_CHK(bx);
            break;
        case OP_LOADI:
        case OP_LOADF:
            REG_CHK(a);
            break;
        case OP_LOADNIL:
        case OP_LOADTRUE:
        case OP_LOADFALSE:
        case OP_LOCK_REF:
            REG_CHK(a);
            break;
        case OP_MOV:
            REG_CHK(a); REG_CHK(b);
            break;

        // --- 全局变量 ---
        case OP_GETGLOBAL:
        case OP_SETGLOBAL:
        case OP_DEFGLOBAL:
        case OP_GETGLOBALFUNC:
        case OP_DEFGLOBALFUNC:
            REG_CHK(a);                                   // Bx = 全局槽位（VM 会扩容/报错）
            break;

        // --- upvalue / 闭包 ---
        case OP_GETUPVAL:
        case OP_SETUPVAL:
            REG_CHK(a);                                   // Bx = upvalue 下标（VM 有检查）
            break;
        case OP_CLOSE:
            REG_CHK(a);
            if (checks && a + bx > cap) FAIL_AT();        // 关闭 R[A..A+Bx-1]
            break;
        case OP_CLOSURE: {
            // Bx 必须是函数常量（VM 在这里**先读 constants[bx]**，越界即崩）
            if (!ck_const_direct(c, bx)) goto bad;
            Value kv = c->constants[bx];
            if (!val_is_obj(kv) || val_as_obj(kv)->type != OBJ_FUNCTION) goto bad;
            int up = ((ObjFunction*)val_as_obj(kv))->upvalue_count;
            if (up < 0 || up > MAX_UPVALUES) goto bad;
            size = 4 + up * 4;
            if (pos + size > len) goto bad;
            if (checks) {
                int d = pos + 4;
                for (int i = 0; i < up; i++, d += 4) {
                    int is_local = (int)c->code[d];
                    int index = (int)c->code[d + 1];
                    if (is_local && index >= cap) FAIL_AT();
                }
            }
            REG_CHK(a);
            break;
        }

        // --- 算术 / 比较 / 位运算（A/B/C 都是寄存器）---
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_MOD:
        case OP_NEG: case OP_NOT:
        case OP_ADD_INT: case OP_SUB_INT: case OP_MUL_INT: case OP_NEG_INT:
        case OP_ADD_F: case OP_SUB_F: case OP_MUL_F: case OP_DIV_F: case OP_NEG_F:
        case OP_EQ: case OP_LT: case OP_GT: case OP_LE: case OP_GE: case OP_NEQ:
        case OP_LT_INT: case OP_GT_INT: case OP_LE_INT: case OP_GE_INT: case OP_EQ_INT:
        case OP_BITAND: case OP_BITOR: case OP_BITXOR: case OP_BITNOT:
        case OP_SHL: case OP_SHR: case OP_USHR:
        case OP_CAST_FLOAT: case OP_CAST_INT: case OP_CAST_STRING: case OP_IS_NULL:
        case OP_STRCAT:
        case OP_INC: case OP_DEC:
        case OP_LEN:
        case OP_ITER_GET: case OP_ITER_GET_VALUE:
        case OP_RANGE:
        case OP_IN:
        case OP_DICT_SET:
        case OP_INDEX:
        case OP_INDEX_ARRAY_INT: case OP_INDEX_ARRAY_FLOAT:
        case OP_INDEX_SET:
        case OP_INDEX_SET_ARRAY_INT:
        case OP_INDEX_DICT_INT: case OP_INDEX_SET_DICT_INT:
        case OP_LT_F: case OP_LE_F: case OP_GT_F: case OP_GE_F:
        case OP_STR_APPEND:
            REG_CHK(a); REG_CHK(b); REG_CHK(cc);
            break;

        // --- 跳转 ---
        case OP_JMP:
            break;                                        // 目标在 pass 2 校验
        case OP_JMP_IF_FALSE:
        case OP_JMP_IF_TRUE:
            REG_CHK(a);
            break;
        case OP_TEST:
            REG_CHK(b);
            // 语义是"条件成立则跳过后一条指令" ⇒ 后面必须还有一条指令
            if (pos + 8 > len) goto bad;
            break;

        // --- 调用 / 返回 ---
        case OP_CALL:
        case OP_TAIL_CALL: {
            REG_CHK(a);
            SPAN_CHK(a, b);                               // callee + 实参：R[A..A+B-1]
            int nr = (cc == 0) ? 1 : cc - 1;              // 结果个数（C = nresults+1）
            if (nr > 0) SPAN_CHK(a, nr);
            break;
        }
        case OP_CALL_NATIVE: {
            REG_CHK(a);
            int cidx = b;
            if (b == 0) {
                if (pos + 8 > len || c->code[pos + 4] != (uint8_t)OP_EXTRAARG) goto bad;
                cidx = ((int)c->code[pos + 5] << 16) | ((int)c->code[pos + 6] << 8) |
                       (int)c->code[pos + 7];
                size = 8;
            }
            CONST_CHK(cidx);
            SPAN_CHK(a + 1, cc);                          // 实参 R[A+1..A+C]
            break;
        }
        case OP_RETURN: {
            int nr = b - 1;
            if (nr > 0) {
                REG_CHK(a);
                SPAN_CHK(a, nr);
            } else {
                // nresults == 0：codegen 传入的 A 可能是未初始化占位寄存器，VM 不读
            }
            break;
        }
        case OP_RETURN_MULTI:
            REG_CHK(a);
            REG_CHK(b);                                   // B = 存放个数的寄存器
            break;
        case OP_ASYNC_CALL:
            REG_CHK(a);
            SPAN_CHK(a + 1, b);                           // 实参 R[A+1..A+B]
            break;

        // --- 数组 / 字典 ---
        case OP_NEWARRAY:
            REG_CHK(a);
            SPAN_CHK(a + 1, cc);                          // R[A+1..A+C]
            break;
        case OP_ARRAY_APPEND:
            REG_CHK(a); REG_CHK(b);                       // C = 结果标志
            break;
        case OP_NEWDICT:
            REG_CHK(a);
            SPAN_CHK(a + 1, cc * 2);                      // 交替的键/值
            break;
        case OP_SLICE:
            REG_CHK(a); REG_CHK(b);
            SPAN_CHK(a, 3);                               // 起始/结束在 R[A+1],R[A+2]
            break;
        case OP_INDEX_ARRAY_IMM:
        case OP_INDEX_SET_ARRAY_IMM:
            REG_CHK(a); REG_CHK(b);                       // C = 字面量下标
            break;

        // --- 异常处理 ---
        case OP_TRY:
            // A = 0/1（catch / finally），Bx = 相对本指令起点的跳转
            break;
        case OP_CATCH:
            REG_CHK(a);
            break;
        case OP_FINALLY:
            break;
        case OP_END_TRY:
            break;
        case OP_THROW:
            REG_CHK(a);
            break;

        // --- 模块 ---
        case OP_LOAD_NATIVE_MODULE:
            REG_CHK(a);
            CONST_CHK(bx);
            break;
        case OP_MODULE_CALL: {
            REG_CHK(a);
            int cidx = b;
            if (b == 0) {
                if (pos + 8 > len || c->code[pos + 4] != (uint8_t)OP_EXTRAARG) goto bad;
                cidx = ((int)c->code[pos + 5] << 16) | ((int)c->code[pos + 6] << 8) |
                       (int)c->code[pos + 7];
                size = 8;
            }
            CONST_CHK(cidx);
            SPAN_CHK(a + 1, cc);                          // 实参 R[A+1..A+C]
            break;
        }
        case OP_GET_MODULE_CONST:
            break;                                        // VM 侧是空操作
        case OP_GET_MODULE_VAR:
        case OP_SET_MODULE_VAR:
        case OP_GET_MODULE_FUNC:
        case OP_DEFINE_MODULE_FUNC:
            REG_CHK(a);                                   // Bx = 模块 globals 下标（VM 有检查）
            break;
        case OP_INIT_LENOMODULE:
            REG_CHK(a);
            break;

        // --- struct ---
        case OP_STRUCT_DEF: {
            CONST_CHK(bx);
            Cur k;
            if (!scan_struct_def(c, pos + 4, checks, &k)) goto bad;
            size = k.p - pos;
            break;
        }
        case OP_STRUCT_INIT: {
            REG_CHK(a);
            SPAN_CHK(a + 1, b);                           // 字段值 R[A+1..A+B]
            Cur k;
            if (!scan_struct_init(c, pos + 4, checks, &k, b)) goto bad;
            size = k.p - pos;
            break;
        }
        case OP_GET_FIELD:
        case OP_SET_FIELD:
        case OP_GET_FIELD_ADDR:
        case OP_GET_FIELD_FAST:
            REG_CHK(a); REG_CHK(b);                       // C = 字段下标
            break;
        case OP_GET_METHOD: {
            REG_CHK(a); REG_CHK(b);
            int cidx = cc;
            if (cc == 0) {
                if (pos + 8 > len || c->code[pos + 4] != (uint8_t)OP_EXTRAARG) goto bad;
                cidx = ((int)c->code[pos + 5] << 16) | ((int)c->code[pos + 6] << 8) |
                       (int)c->code[pos + 7];
                size = 8;
            }
            CONST_CHK(cidx);
            break;
        }
        case OP_SET_PTR_ELEM_TYPE:
            REG_CHK(a);                                   // B = TypeKind
            break;
        case OP_SET_DECLARED_FACE:
            REG_CHK(a);
            CONST_CHK(bx);
            break;

        // --- enum / face / cstruct ---
        case OP_ENUM_DEF: {
            REG_CHK(a);
            CONST_CHK(bx);
            Cur k;
            if (!scan_enum_def(c, pos + 4, checks, &k)) goto bad;
            size = k.p - pos;
            break;
        }
        case OP_FACE_DEF: {
            CONST_CHK(bx);
            Cur k;
            if (!scan_face_def(c, pos + 4, checks, &k)) goto bad;
            size = k.p - pos;
            break;
        }
        case OP_CSTRUCT_DEF: {
            REG_CHK(a);
            CONST_CHK(bx);
            Cur k;
            if (!scan_cstruct_def(c, pos + 4, checks, &k)) goto bad;
            size = k.p - pos;
            break;
        }
        case OP_GET_CSTRUCT_DEF:
            REG_CHK(a);
            CONST_CHK(bx);
            break;

        // --- 类型检查 / 转换 ---
        case OP_TYPE_CHECK:
        case OP_AS_CAST: {
            REG_CHK(a);
            int kind = b;
            int need_name = (op == OP_TYPE_CHECK)
                                ? (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_ENUM)
                                : (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_CSTRUCT);
            if (need_name) {
                if (pos + 6 > len) goto bad;
                int idx = ((int)c->code[pos + 4] << 8) | (int)c->code[pos + 5];
                CONST_CHK(idx);
                size = 6;
            }
            break;
        }
        case OP_TYPE_CHECK_SPEC:
        case OP_AS_CAST_SPEC: {
            REG_CHK(a);
            int endp = 0;
            if (!spec_scan(c, pos + 4, len, checks, 0, &endp)) goto bad;
            size = endp - pos;
            break;
        }

        // --- 协程 ---
        case OP_AWAIT:
            REG_CHK(a); REG_CHK(b);
            break;

        // --- FFI ---
        case OP_CLIB_CALL: {
            REG_CHK(a);
            if (cc > VERIFY_CLIB_MAX_ARGS) goto bad;      // VM 会按 C 个字节读取类型
            Cur k;
            if (!scan_clib_call(c, pos + 4, &k, cc)) goto bad;
            size = k.p - pos;
            SPAN_CHK(a, cc + 2);                          // 库对象 + 函数名 + 实参
            break;
        }
        case OP_CFUNC_CALLBACK: {
            REG_CHK(a);
            Cur k;
            if (!scan_cfunc_callback(c, pos + 4, &k)) goto bad;
            size = k.p - pos;
            int pcnt = (int)c->code[pos + 5];
            if (pcnt > VERIFY_CFUNC_MAX_ARGS) goto bad;
            break;
        }
        case OP_U8_TO_F64:
            REG_CHK(a); REG_CHK(b);
            break;

        // --- 泛型 / 析构 ---
        case OP_PUSH_TYPE_ARGS:
            break;                                        // VM 侧是空操作
        case OP_DTOR_LOCAL:
            REG_CHK(a);
            break;
        case OP_SWITCH_LOOKUP:
            break;                                        // VM 侧是空操作

        // --- for 数值循环 ---
        case OP_FOR_PREP: {
            if (pos + 10 > len) goto bad;
            size = 10;
            if (checks) {
                int var_slot = (int)c->code[pos + 4];
                int step_slot = (int)c->code[pos + 8];
                int flags = (int)c->code[pos + 9];
                if (b >= cap || var_slot >= cap || step_slot >= cap) FAIL_AT();
                if ((flags & 1) == 0 && a >= cap) FAIL_AT();      // A 是起始槽位
                if ((flags & 2) == 0 && cc >= cap) FAIL_AT();     // C 是步长槽位
            }
            break;
        }
        case OP_FOR_LOOP:
            if (pos + 8 > len) goto bad;
            size = 8;
            REG_CHK(a); REG_CHK(b); REG_CHK(cc);
            break;

        // --- 扩展指令 ---
        case OP_EXTRAARG:
        case OP_EXTEND:
            break;                                        // 前缀 / 无操作

        // --- 立即数快速路径 ---
        case OP_ADD_INT_IMM:
        case OP_SUB_INT_IMM:
        case OP_MUL_INT_IMM:
        case OP_LT_INT_IMM:
        case OP_GT_INT_IMM:
        case OP_LE_INT_IMM:
        case OP_GE_INT_IMM:
            REG_CHK(a); REG_CHK(b);                       // C = int8 立即数
            break;
        case OP_CALL_GLOBAL_FUNC:
        case OP_CALL_GLOBAL_FUNC_TYPED:
            REG_CHK(a);                                   // C = 全局函数槽位
            SPAN_CHK(a + 1, b);                           // 实参 R[A+1..A+B]
            break;

        // --- 比较 + 跳转融合 ---
        case OP_CMPJMP_LT:
        case OP_CMPJMP_LE:
        case OP_CMPJMP_GT:
        case OP_CMPJMP_GE:
        case OP_CMPJMP_EQ:
            if (pos + 8 > len) goto bad;
            size = 8;
            REG_CHK(a);
            if (checks && (cc & 0x80) == 0) {
                if (b >= cap) FAIL_AT();                  // 非立即数形式时 B 是寄存器
            }
            break;
        case OP_CMPJMP_ITER:
            if (pos + 8 > len) goto bad;
            size = 8;
            REG_CHK(a); REG_CHK(b);                       // C = 标志位
            break;

        // --- 多字段累加 / 方法调用融合 ---
        case OP_ACC_FIELDS:
            REG_CHK(a); REG_CHK(b);
            if (pos + 4 + cc > len) goto bad;
            size = 4 + cc;
            break;
        case OP_INVOKE_METHOD_TYPED: {
            if (pos + 8 > len) goto bad;
            size = 8;
            REG_CHK(a);
            SPAN_CHK(a + 1, b);                           // 实参 R[A+1..A+B]
            int name_idx = ((int)c->code[pos + 4] << 8) | (int)c->code[pos + 5];
            int type_idx = ((int)c->code[pos + 6] << 8) | (int)c->code[pos + 7];
            CONST_CHK(name_idx);
            CONST_CHK(type_idx);
            break;
        }

        // --- 位运算常量化（C 是常量索引；C == 0 时后随 EXTRAARG）---
        case OP_BITAND_K:
        case OP_BITOR_K:
        case OP_BITXOR_K:
        case OP_SHL_K:
        case OP_SHR_K:
        case OP_USHR_K: {
            REG_CHK(a); REG_CHK(b);
            int cidx = cc;
            if (cc == 0) {
                if (pos + 8 > len || c->code[pos + 4] != (uint8_t)OP_EXTRAARG) goto bad;
                cidx = ((int)c->code[pos + 5] << 16) | ((int)c->code[pos + 6] << 8) |
                       (int)c->code[pos + 7];
                size = 8;
            }
            CONST_CHK(cidx);
            break;
        }

        // --- FIN_GOTO（break/continue 穿过 finally）---
        case OP_FIN_GOTO:
            // A = finally 层数（不是寄存器）；Bx = 原始 16 位相对偏移
            break;

        default:
            // 枚举里新增了 opcode 但这里没跟上 ⇒ 直接判非法（宁可拒绝也不放过）
            goto bad;
    }

    #undef FAIL_AT
    #undef REG_CHK
    #undef SPAN_CHK
    #undef CONST_CHK

    if (pos + size > len) goto bad;

    // ---- 跳转目标：必须落在合法指令起点（pass 2）----
    if (checks && starts) {
        #define TARGET_OK(t) ((t) >= 0 && (t) < len && \
                              (starts[(t) >> 3] & (uint8_t)(1u << ((t) & 7))))
        int target = -1;
        switch (op) {
            case OP_JMP: {
                int sj = ((int)c->code[pos + 1] << 16) | ((int)c->code[pos + 2] << 8) |
                         (int)c->code[pos + 3];
                if (sj & 0x800000) sj -= 0x1000000;
                target = pos + 4 + sj;
                break;
            }
            case OP_JMP_IF_FALSE:
            case OP_JMP_IF_TRUE:
                target = pos + 4 + (bx - 32768);
                break;
            case OP_CMPJMP_LT:
            case OP_CMPJMP_LE:
            case OP_CMPJMP_GT:
            case OP_CMPJMP_GE:
            case OP_CMPJMP_EQ:
            case OP_CMPJMP_ITER: {
                int off = ((int)c->code[pos + 4] << 8) | (int)c->code[pos + 5];
                target = pos + 8 + (off - 32768);
                break;
            }
            case OP_FOR_PREP: {
                int off = ((int)c->code[pos + 6] << 8) | (int)c->code[pos + 7];
                target = pos + 10 + (off - 32768);
                break;
            }
            case OP_FOR_LOOP: {
                int off = ((int)c->code[pos + 6] << 8) | (int)c->code[pos + 7];
                target = pos + 8 + (off - 32768);
                break;
            }
            case OP_TRY:
            case OP_FINALLY:
            case OP_FIN_GOTO:
                target = pos + bx;
                break;
            default:
                break;
        }
        if (target >= 0 && !TARGET_OK(target)) goto bad;
        #undef TARGET_OK
    }

    return size;

bad:
    if (bad) *bad = pos;
    return 0;
}

// 寄存器/槽位物理容量：
//   local_count <= INLINE_LOCALS_MAX ⇒ 帧用内联 inline_locals，物理容量是 INLINE_LOCALS_MAX；
//   local_count  > INLINE_LOCALS_MAX ⇒ 帧用 malloc(local_count)。
// 这样判的是"VM 真的会不会越界"，而不是"寄存器号是否超过高水位"
// （后者对合法字节码也成立，但会让老 .lenb 里的合法大函数被误拒）。
static int reg_capacity(int local_count) {
    int lc = local_count;
    if (lc < 1) lc = 1;                     // VM 建帧时 local_count <= 0 会抬到 1
    if (lc <= INLINE_LOCALS_MAX) return INLINE_LOCALS_MAX;
    return lc;
}

int bytecode_verify_chunk(const Chunk* chunk, int local_count, int* bad_offset) {
    if (bad_offset) *bad_offset = -1;
    if (!chunk) return 0;
    if (chunk->len < 0) return 0;
    if (chunk->len > 0 && !chunk->code) return 0;
    if (chunk->const_cnt < 0) return 0;
    if (chunk->const_cnt > 0 && !chunk->constants) return 0;

    // 防损坏的 local_count 让 VM 在 call_reg 里按 8*local_count 巨量 malloc
    if (local_count < 0 || local_count > VERIFY_MAX_REG) return 0;

    if (chunk->len == 0) return 1;          // 空 chunk（模块可序列化 NULL chunk）

    int cap = reg_capacity(local_count);
    int len = chunk->len;

    uint8_t* starts = (uint8_t*)calloc((size_t)((len + 7) / 8), 1);
    if (!starts) return 0;

    // ---- pass 1：指令边界 ----
    int pos = 0;
    while (pos < len) {
        int sz = verify_one(chunk, pos, cap, NULL, 0, bad_offset);
        if (sz <= 0 || pos + sz > len) {
            free(starts);
            return 0;
        }
        starts[pos >> 3] |= (uint8_t)(1u << (pos & 7));
        pos += sz;
    }
    if (pos != len) {
        free(starts);
        return 0;
    }

    // ---- pass 2：跳转目标 / 常量 / 寄存器 ----
    pos = 0;
    while (pos < len) {
        int sz = verify_one(chunk, pos, cap, starts, 1, bad_offset);
        if (sz <= 0 || pos + sz > len) {
            free(starts);
            return 0;
        }
        pos += sz;
    }

    free(starts);
    return 1;
}
