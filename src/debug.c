#include "include/lenolang.h"
#include <stdio.h>
#include <stdlib.h>

// 指令名称表（用于调试）- 必须与 leno_vm.h 中的 OpCode 枚举完全一致
// 寄存器式定长 4 字节指令
static const char* opCodeNames[] = {
    // --- 空操作 / 杂项 ---
    "OP_NOP",
    // --- 装载 / 移动 ---
    "OP_LOADK", "OP_LOADI", "OP_LOADF", "OP_LOADNIL", "OP_LOADTRUE", "OP_LOADFALSE", "OP_MOV",
    // --- 全局变量 ---
    "OP_GETGLOBAL", "OP_SETGLOBAL", "OP_DEFGLOBAL", "OP_GETGLOBALFUNC", "OP_DEFGLOBALFUNC",
    // --- upvalue / 闭包 ---
    "OP_GETUPVAL", "OP_SETUPVAL", "OP_CLOSE", "OP_CLOSURE",
    // --- 算术运算（通用） ---
    "OP_ADD", "OP_SUB", "OP_MUL", "OP_DIV", "OP_MOD", "OP_NEG", "OP_NOT",
    // --- 算术运算 int 特化 ---
    "OP_ADD_INT", "OP_SUB_INT", "OP_MUL_INT", "OP_NEG_INT",
    // --- 算术运算 float 特化 ---
    "OP_ADD_F", "OP_SUB_F", "OP_MUL_F", "OP_DIV_F", "OP_NEG_F",
    // --- 比较运算 ---
    "OP_EQ", "OP_LT", "OP_GT", "OP_LE", "OP_GE", "OP_NEQ",
    // --- 比较 int 特化 ---
    "OP_LT_INT", "OP_GT_INT", "OP_LE_INT", "OP_GE_INT", "OP_EQ_INT",
    // --- 位运算 ---
    "OP_BITAND", "OP_BITOR", "OP_BITXOR", "OP_BITNOT", "OP_SHL", "OP_SHR", "OP_USHR",
    // --- 类型转换 ---
    "OP_CAST_FLOAT", "OP_CAST_INT", "OP_CAST_STRING", "OP_IS_NULL",
    // --- 字符串拼接 ---
    "OP_STRCAT",
    // --- 跳转 ---
    "OP_JMP", "OP_JMP_IF_FALSE", "OP_JMP_IF_TRUE", "OP_TEST",
    // --- 自增自减 ---
    "OP_INC", "OP_DEC",
    // --- 调用 / 返回 ---
    "OP_CALL", "OP_CALL_NATIVE", "OP_RETURN", "OP_RETURN_MULTI", "OP_TAIL_CALL",
    // --- 数组 ---
    "OP_NEWARRAY", "OP_ARRAY_GET", "OP_ARRAY_SET", "OP_ARRAY_APPEND", "OP_LEN",
    // --- 字典 ---
    "OP_NEWDICT", "OP_DICT_GET", "OP_DICT_SET", "OP_DICT_GET_KEY",
    // --- 通用索引 ---
    "OP_INDEX", "OP_INDEX_ARRAY_INT", "OP_INDEX_ARRAY_FLOAT", "OP_INDEX_SET", "OP_SLICE",
    // --- 迭代 ---
    "OP_ITER_GET", "OP_ITER_GET_VALUE",
    // --- 范围 ---
    "OP_RANGE",
    // --- in ---
    "OP_IN",
    // --- 异常处理 ---
    "OP_TRY", "OP_CATCH", "OP_FINALLY", "OP_END_TRY", "OP_THROW",
    // --- 模块 ---
    "OP_LOAD_NATIVE_MODULE", "OP_MODULE_CALL", "OP_GET_MODULE_CONST",
    "OP_GET_MODULE_VAR", "OP_SET_MODULE_VAR", "OP_GET_MODULE_FUNC", "OP_DEFINE_MODULE_FUNC",
    "OP_INIT_LENOMODULE",
    // --- struct ---
    "OP_STRUCT_DEF", "OP_STRUCT_INIT", "OP_GET_FIELD", "OP_SET_FIELD", "OP_GET_FIELD_ADDR",
    "OP_GET_METHOD", "OP_SET_PTR_ELEM_TYPE", "OP_SET_DECLARED_FACE",
    // --- enum / face / cstruct ---
    "OP_ENUM_DEF", "OP_FACE_DEF", "OP_CSTRUCT_DEF", "OP_GET_CSTRUCT_DEF",
    // --- 类型检查 / 转换 ---
    "OP_TYPE_CHECK", "OP_AS_CAST",
    // --- 协程 ---
    "OP_AWAIT", "OP_ASYNC_CALL",
    // --- FFI ---
    "OP_CLIB_CALL", "OP_CFUNC_CALLBACK", "OP_U8_TO_F64",
    // --- 泛型 / 析构 ---
    "OP_PUSH_TYPE_ARGS", "OP_DTOR_LOCAL",
    // --- switch ---
    "OP_SWITCH_LOOKUP",
    // --- for 数值循环 ---
    "OP_FOR_PREP", "OP_FOR_LOOP",
    // --- 扩展指令 ---
    "OP_EXTRAARG", "OP_EXTEND",
    // --- 立即数快速路径（与 leno_vm.h 末尾新增的两个 opcode 同序）---
    "OP_ADD_INT_IMM", "OP_SUB_INT_IMM",
    "OP_LT_INT_IMM", "OP_GT_INT_IMM", "OP_LE_INT_IMM", "OP_GE_INT_IMM",
    "OP_CALL_GLOBAL_FUNC",
    "OP_CMPJMP_LT", "OP_CMPJMP_LE", "OP_CMPJMP_GT", "OP_CMPJMP_GE",
    "OP_MUL_INT_IMM",
    "OP_LT_F", "OP_LE_F", "OP_GT_F", "OP_GE_F",
    "OP_CALL_GLOBAL_FUNC_TYPED",
    "OP_GET_FIELD_FAST",
    "OP_ACC_FIELDS",
    "OP_INVOKE_METHOD_TYPED",
    "OP_INDEX_ARRAY_IMM",
    "OP_CMPJMP_ITER",
    "OP_BITAND_K", "OP_BITOR_K", "OP_BITXOR_K", "OP_SHL_K", "OP_SHR_K", "OP_USHR_K",
    "OP_INDEX_SET_ARRAY_INT",
    "OP_TYPE_CHECK_DICT", "OP_AS_CAST_DICT",
    "OP_OPCODE_COUNT",
};

// 表必须与 leno_vm.h 的 OpCode **逐项同序同长**（末尾多一项 OP_OPCODE_COUNT 的名字）：
// 少一项/多一项都会让"按编号取名"从那一项起整体错位 —— 曾经就漏了
// OP_FOR_PREP / OP_FOR_LOOP 两项，于是 OP_FOR_PREP 一直被反汇编成 OP_EXTRAARG
// （调试时按名字查错指令，白折腾）。这里用一条编译期断言钉住长度。
typedef char opcode_names_len_check[
    (sizeof(opCodeNames) / sizeof(opCodeNames[0]) == (size_t)(OP_OPCODE_COUNT) + 1) ? 1 : -1];

/* ---- opcode 编号 → 名 ----
 * 表与 leno_vm.h 的 OpCode 枚举严格同序（本文件开头已声明该约束），所以直接按编号取。
 * 反汇编、调试输出、按编号统计都靠它 —— 别人工对着枚举数编号（那是最容易出错的一步）。 */
const char* opcode_name(int op) {
    if (op < 0 || op >= (int)(sizeof(opCodeNames) / sizeof(opCodeNames[0])))
        return "?";
    return opCodeNames[op];
}

// ============================================================================
// 编译期诊断：寄存器号越过 8 位上限
// ----------------------------------------------------------------------------
// 指令里的 A/B/C 各 8 位 ⇒ 寄存器号 ≥ 256 会被 &0xFF **静默截断**，运行期表现为
// "读到 null / 写坏别人的槽位"，很难从现象反推（实测：SDL 的 Window.run 寄存器
// 高水位 655，循环变量 i 的读取被截断到另一个槽位、该槽位恰好是 null ⇒
// `_runEvts[i]` 报「数组索引必须是数字」）。这里在编码处告警，直接给出
// opcode / 操作数 / 行号。
// ============================================================================
void codegen_reg_overflow_warn(OpCode op, int a, int b, int c, int line) {
    fprintf(stderr, "[寄存器号溢出] %s A=%d B=%d C=%d line=%d（>255 会被截断成另一个槽位）\n",
            opcode_name((int)op), a, b, c, line);
}

// ============================================================================
// 尾随数据的安全游标（越界只标记、不越读：截断/损坏的 chunk 也不能把 dump 搞崩）
// ============================================================================
static int dbg_take_u8(Chunk* chunk, int base, int* p, int* over) {
    int idx = base + *p;
    if (idx < 0 || idx >= chunk->len) { *over = 1; *p += 1; return 0; }
    *p += 1;
    return chunk->code[idx];
}

static int dbg_take_u16(Chunk* chunk, int base, int* p, int* over) {
    int hi = dbg_take_u8(chunk, base, p, over);
    int lo = dbg_take_u8(chunk, base, p, over);
    return (hi << 8) | lo;
}

// 常量表第 idx 项的可读形式（字符串加引号；越界/非字符串退化；buf 始终以 0 结尾）
static void dbg_const_str(Chunk* chunk, int idx, char* buf, int buf_size) {
    if (buf_size <= 0) return;
    buf[0] = '\0';
    // ⚠ K[0] 是**合法**下标（实测 dump 里 `OP_LOADK Bx=0` 就是 K[0]）；
    //   "0 = 走 EXTRAARG" 只是那几条指令自己的哨兵约定，不能拿来当越界判据。
    if (!chunk || idx < 0 || idx >= chunk->const_cnt) {
        snprintf(buf, (size_t)buf_size, "?");
        return;
    }
    Value v = chunk->constants[idx];
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
        // ⚠ 必须按 len 走并转义内嵌 '\0'：原生模块调用的常量是 "模块名\0方法名"
        //   组合串，直接用 %s 打会在 \0 处截断，看起来就像"常量是模块名"（误导）。
        ObjString* sv = (ObjString*)val_as_obj(v);
        int w = 0;
        if (w < buf_size - 1) buf[w++] = '"';
        for (int i = 0; i < sv->len && w < buf_size - 6; i++) {
            unsigned char ch = (unsigned char)sv->chars[i];
            if (ch == '\0') { buf[w++] = '\\'; buf[w++] = '0'; }
            else if (ch == '"') { buf[w++] = '\\'; buf[w++] = '"'; }
            else if (ch == '\n') { buf[w++] = '\\'; buf[w++] = 'n'; }
            else buf[w++] = (char)ch;
        }
        if (w < buf_size - 1) buf[w++] = '"';
        buf[w] = '\0';
        return;
    }
    const char* s = val_to_string(v);
    snprintf(buf, (size_t)buf_size, "%s", s ? s : "?");
}

// ============================================================================
// 指令总长度 + 尾随数据摘要
// ----------------------------------------------------------------------------
// 寄存器式的**指令头**定长 4 字节（op8 A8 B8 C8 / iABx / iAsBx / iAsJ / iAx），但
// 下面这些指令在头后面还带数据。长度务必与 codegen 的发射顺序**严格一致**：
// 差一个字节，后面整段反汇编就错位 —— 这正是"只要出现带尾随数据的指令，dump
// 后半段就全是 OP_NOP 垃圾"的原因（早先这里一律按 4 字节推进）。
//
// 尾随数据一览（括号内 = 发射点，便于对账）：
//   OP_STRUCT_DEF     字段/方法/impl/泛型参数/关联常量表            (gen_struct_def)
//   OP_FACE_DEF       方法签名表（名字 + 参数个数 + 返回/参数类型）  (gen_face_def)
//   OP_ENUM_DEF       成员表（名字 + 值常量）                      (gen_enum_def)
//   OP_CSTRUCT_DEF    字段布局表（名字/类型/偏移/数组维度/嵌套名）   (gen_cstruct_def)
//   OP_STRUCT_INIT    名字 + 泛型实参 + 字段名表 + 3 字节 module_slot16 (gen_struct_init)
//   OP_CLOSURE        每个 upvalue 4 字节捕获描述                  (emit_closure_upvals)
//   OP_CLIB_CALL      1 字节返回类型 + min(nargs,12) 个参数类型     (gen_clib_call)
//   OP_CFUNC_CALLBACK 1+1 字节头 + pcnt 个参数类型                 (gen_module_call/ffi.callback)
//   OP_TYPE_CHECK     / OP_AS_CAST：需要名字的类型再 +2 字节常量索引 (emit_type_op)
//   OP_FOR_PREP       / OP_FOR_LOOP：定长 8 字节（含 sBx16）        (gen_for 系列)
//   EXTRAARG 前缀：常量索引为 0 时紧随 4 字节（iAx）——
//     OP_CALL_NATIVE(B=0) / OP_MODULE_CALL(B=0) / OP_GET_METHOD(C=0)
//     （对应 VM 的 READ_CONST_IDX；这里用"下一条是不是 OP_EXTRAARG"做保险）
//
// desc 非空时写入一段人类可读摘要；返回该指令占用的总字节数。
// ============================================================================
static int decode_trailing(Chunk* chunk, int offset, char* desc, size_t desc_size) {
    uint8_t op = chunk->code[offset];
    uint8_t b = chunk->code[offset + 2];
    uint8_t c = chunk->code[offset + 3];
    int base = offset + 4;
    int p = 0, over = 0;
    if (desc && desc_size) desc[0] = '\0';

    switch (op) {
        case OP_STRUCT_DEF: {
            int fields = dbg_take_u8(chunk, base, &p, &over);
            int methods = dbg_take_u8(chunk, base, &p, &over);
            int impls = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < impls; i++) dbg_take_u16(chunk, base, &p, &over);
            int tps = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < tps; i++) dbg_take_u16(chunk, base, &p, &over);
            for (int i = 0; i < fields; i++) {
                dbg_take_u16(chunk, base, &p, &over);                 // 字段名
                int ftype = dbg_take_u8(chunk, base, &p, &over);      // 类型
                dbg_take_u8(chunk, base, &p, &over);                  // nullable
                if (ftype == TYPE_STRUCT) {
                    if (dbg_take_u8(chunk, base, &p, &over)) dbg_take_u16(chunk, base, &p, &over);
                }
                if (ftype == TYPE_PTR_GENERIC) dbg_take_u8(chunk, base, &p, &over);
                if (dbg_take_u8(chunk, base, &p, &over)) dbg_take_u16(chunk, base, &p, &over);
            }
            for (int i = 0; i < methods; i++) {
                dbg_take_u16(chunk, base, &p, &over);
                dbg_take_u16(chunk, base, &p, &over);
            }
            int flags = dbg_take_u8(chunk, base, &p, &over);
            if (flags & 1) dbg_take_u8(chunk, base, &p, &over);
            if (flags & 2) dbg_take_u8(chunk, base, &p, &over);
            int consts = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < consts; i++) {
                dbg_take_u16(chunk, base, &p, &over);
                dbg_take_u16(chunk, base, &p, &over);
            }
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, ((int)b << 8) | (int)c, nm, (int)sizeof(nm));
                snprintf(desc, desc_size, "name=%s 字段=%d 方法=%d impl=%d 泛型参数=%d 关联常量=%d%s",
                         nm, fields, methods, impls, tps, consts, over ? " [截断]" : "");
            }
            break;
        }
        case OP_FACE_DEF: {
            int methods = dbg_take_u8(chunk, base, &p, &over);
            int tps = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < tps; i++) dbg_take_u16(chunk, base, &p, &over);
            for (int i = 0; i < methods; i++) {
                dbg_take_u16(chunk, base, &p, &over);                 // 方法名
                int pc = dbg_take_u8(chunk, base, &p, &over);
                dbg_take_u8(chunk, base, &p, &over);                  // 返回类型
                for (int j = 0; j < pc; j++) dbg_take_u8(chunk, base, &p, &over);
            }
            if (desc) snprintf(desc, desc_size, "方法=%d 泛型参数=%d%s", methods, tps, over ? " [截断]" : "");
            break;
        }
        case OP_ENUM_DEF: {
            int members = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < members; i++) {
                dbg_take_u16(chunk, base, &p, &over);
                dbg_take_u16(chunk, base, &p, &over);
            }
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, ((int)b << 8) | (int)c, nm, (int)sizeof(nm));
                snprintf(desc, desc_size, "name=%s 成员=%d%s", nm, members, over ? " [截断]" : "");
            }
            break;
        }
        case OP_CSTRUCT_DEF: {
            int fields = dbg_take_u8(chunk, base, &p, &over);
            int total_size = dbg_take_u16(chunk, base, &p, &over);
            int align = dbg_take_u8(chunk, base, &p, &over);
            int packed = dbg_take_u8(chunk, base, &p, &over);
            int explicit_align = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < fields; i++) {
                dbg_take_u16(chunk, base, &p, &over);                 // 字段名
                int ftype = dbg_take_u8(chunk, base, &p, &over);
                dbg_take_u16(chunk, base, &p, &over);                 // 偏移
                dbg_take_u16(chunk, base, &p, &over);                 // 数组维度
                dbg_take_u16(chunk, base, &p, &over);                 // 嵌套 cstruct 名
                if (ftype == TYPE_PTR_GENERIC) dbg_take_u8(chunk, base, &p, &over);
            }
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, ((int)b << 8) | (int)c, nm, (int)sizeof(nm));
                snprintf(desc, desc_size, "name=%s 字段=%d size=%d align=%d packed=%d explicit_align=%d%s",
                         nm, fields, total_size, align, packed, explicit_align, over ? " [截断]" : "");
            }
            break;
        }
        case OP_STRUCT_INIT: {
            int name_idx = dbg_take_u16(chunk, base, &p, &over);
            int gc = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < gc; i++) dbg_take_u16(chunk, base, &p, &over);
            for (int i = 0; i < (int)b; i++) dbg_take_u16(chunk, base, &p, &over);   // 字段名
            int mod_space = dbg_take_u8(chunk, base, &p, &over);
            int mod_slot = dbg_take_u16(chunk, base, &p, &over);
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, name_idx, nm, (int)sizeof(nm));
                if (mod_space) {
                    snprintf(desc, desc_size, "name=%s 泛型实参=%d 字段名=%d 模块槽位=%d:%d%s",
                             nm, gc, (int)b, mod_space, mod_slot, over ? " [截断]" : "");
                } else {
                    snprintf(desc, desc_size, "name=%s 泛型实参=%d 字段名=%d%s",
                             nm, gc, (int)b, over ? " [截断]" : "");
                }
            }
            break;
        }
        case OP_CLOSURE: {
            int bx = ((int)b << 8) | (int)c;
            int upvals = 0;
            if (bx >= 0 && bx < chunk->const_cnt) {
                Value kv = chunk->constants[bx];
                if (val_is_obj(kv) && val_as_obj(kv)->type == OBJ_FUNCTION) {
                    upvals = ((ObjFunction*)val_as_obj(kv))->upvalue_count;
                }
            }
            for (int i = 0; i < upvals; i++) {
                dbg_take_u8(chunk, base, &p, &over);   // is_local
                dbg_take_u8(chunk, base, &p, &over);   // index
                dbg_take_u8(chunk, base, &p, &over);   // is_value_capture
                dbg_take_u8(chunk, base, &p, &over);   // pad
            }
            if (desc) snprintf(desc, desc_size, "upvalue=%d%s", upvals, over ? " [截断]" : "");
            break;
        }
        case OP_CLIB_CALL: {
            int nargs = (int)c;
            int lim = nargs < 12 ? nargs : 12;   // CLIB_CALL_MAX_ARGS（与 codegen 同口径）
            int ret = dbg_take_u8(chunk, base, &p, &over);
            char args_buf[64];
            int written = 0;
            written += snprintf(args_buf + written, sizeof(args_buf) - (size_t)written, "[");
            for (int i = 0; i < lim && written < (int)sizeof(args_buf) - 4; i++) {
                int t = dbg_take_u8(chunk, base, &p, &over);
                written += snprintf(args_buf + written, sizeof(args_buf) - (size_t)written,
                                    "%s%d", i ? "," : "", t);
            }
            snprintf(args_buf + written, sizeof(args_buf) - (size_t)written, "]");
            if (desc) snprintf(desc, desc_size, "返回类型=%d 参数类型=%s%s", ret, args_buf, over ? " [截断]" : "");
            break;
        }
        case OP_CFUNC_CALLBACK: {
            int ret = dbg_take_u8(chunk, base, &p, &over);
            int pcnt = dbg_take_u8(chunk, base, &p, &over);
            for (int i = 0; i < pcnt; i++) dbg_take_u8(chunk, base, &p, &over);
            if (desc) snprintf(desc, desc_size, "返回类型=%d 参数个数=%d%s", ret, pcnt, over ? " [截断]" : "");
            break;
        }
        // Dict[K,V] 的逐键值校验：K/V 直接在 B/C 里（无尾随数据，定长 4 字节）
        case OP_TYPE_CHECK_DICT:
        case OP_AS_CAST_DICT: {
            if (desc) {
                snprintf(desc, desc_size, "%s[K=%s, V=%s]（逐键值校验）",
                         (op == OP_TYPE_CHECK_DICT) ? "Dict" : "as Dict",
                         type_kind_to_string((TypeKind)b), type_kind_to_string((TypeKind)c));
            }
            break;
        }
        case OP_TYPE_CHECK:
        case OP_AS_CAST: {
            TypeKind kind = (TypeKind)b;
            int need_name = (op == OP_TYPE_CHECK)
                                ? (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_ENUM)
                                : (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_CSTRUCT);
            int idx = 0;
            if (need_name) {
                idx = dbg_take_u16(chunk, base, &p, &over);
                char nm[128];
                dbg_const_str(chunk, idx, nm, (int)sizeof(nm));
                if (desc) snprintf(desc, desc_size, "类型名=%s%s", nm, over ? " [截断]" : "");
            }
            break;
        }
        case OP_FOR_PREP: {
            int var = dbg_take_u8(chunk, base, &p, &over);
            int inclusive = dbg_take_u8(chunk, base, &p, &over);
            int sbx = dbg_take_u16(chunk, base, &p, &over) - 32768;   // 与 patch_sbx_at 同约定
            if (desc) snprintf(desc, desc_size, "循环变量槽位=%d inclusive=%d sBx=%d (跳过体 → %d)",
                               var, inclusive, sbx, offset + 8 + sbx);
            break;
        }
        case OP_FOR_LOOP: {
            int inclusive = dbg_take_u8(chunk, base, &p, &over);
            dbg_take_u8(chunk, base, &p, &over);   // pad
            int sbx = dbg_take_u16(chunk, base, &p, &over) - 32768;   // 与 patch_sbx_at 同约定
            if (desc) snprintf(desc, desc_size, "inclusive=%d sBx=%d (回跳 → %d)",
                               inclusive, sbx, offset + 8 + sbx);
            break;
        }
        // 「比较 + 条件跳转」融合（T10-①）：紧随一个 4 字节字，前 2 字节是跳转偏移
        //   （offset + 32768，与 iAsBx 同约定；后 2 字节是填充）。
        case OP_CMPJMP_LT:
        case OP_CMPJMP_LE:
        case OP_CMPJMP_GT:
        case OP_CMPJMP_GE: {
            int bx = dbg_take_u16(chunk, base, &p, &over);
            dbg_take_u16(chunk, base, &p, &over);   // 第二个字的另外 2 字节是填充
            if (desc) {
                int off = bx - 32768;
                int lhs = (int)chunk->code[offset + 1];
                // C 的 bit6 是**极性**（0 = 比较为假则跳 / 1 = 比较为真则跳）——
                //   必须照实打印：`while` 的回边就是用 bit6 融合出来的，
                //   都印成"为假则跳"会让人按着反汇编反推错语义（文档里记过
                //   "调试时按名字查错指令、白折腾"的教训）。
                const char* pol = (c & 0x40) ? "为真则跳" : "为假则跳";
                if (c & 0x80) {
                    snprintf(desc, desc_size, "%s %+d（比较 R[%d] 与立即数 %d）",
                             pol, off, lhs, (int)(int8_t)b);
                } else {
                    snprintf(desc, desc_size, "%s %+d（比较 R[%d] 与 R[%d]）",
                             pol, off, lhs, (int)b);
                }
            }
            break;
        }
        // for 容器迭代的条件融合（T13）：同样是 8 字节，第二个字前 2 字节是跳转偏移。
        //   A = 索引寄存器；B = 容器寄存器；C 的 bit6 = 真则跳、bit5 = 先自增索引。
        case OP_CMPJMP_ITER: {
            int bx = dbg_take_u16(chunk, base, &p, &over);
            dbg_take_u16(chunk, base, &p, &over);   // 第二个字的另外 2 字节是填充
            if (desc) {
                int off = bx - 32768;
                int idx_reg = (int)chunk->code[offset + 1];
                snprintf(desc, desc_size, "%s%s %+d（R[%d] < len(R[%d])）",
                         (c & 0x20) ? "先自增再测：" : "",
                         (c & 0x40) ? "为真则跳" : "为假则跳",
                         off, idx_reg, (int)b);
            }
            break;
        }
        // 位运算 / 移位的常量化形式（T14）：C 是常量索引（C 为 0 ⇒ 紧随 EXTRAARG 携带 24 位索引）。
        //   ⚠ 这里必须把那个 EXTRAARG 也吃掉（p += 4），否则反汇编会把它的 4 个字节
        //     当成下一条指令 —— 与 OP_CALL_NATIVE / OP_GET_METHOD 同一注意事项。
        case OP_BITAND_K:
        case OP_BITOR_K:
        case OP_BITXOR_K:
        case OP_SHL_K:
        case OP_SHR_K:
        case OP_USHR_K: {
            int cidx = (int)c;
            if (cidx == 0 && base + 4 <= chunk->len && chunk->code[base] == OP_EXTRAARG) {
                cidx = ((int)chunk->code[base + 1] << 16) | ((int)chunk->code[base + 2] << 8) |
                       (int)chunk->code[base + 3];
                p += 4;
            }
            if (desc) {
                char kv[128];
                dbg_const_str(chunk, cidx, kv, (int)sizeof(kv));
                snprintf(desc, desc_size, "R[%d] = R[%d] <op> K[%d]=%s%s",
                         (int)chunk->code[offset + 1], (int)b, cidx, kv, over ? " [截断]" : "");
            }
            break;
        }
        // 常量索引为 0 时紧随一条 EXTRAARG（iAx，24 位常量索引）
        case OP_CALL_NATIVE:
        case OP_MODULE_CALL: {
            if (b != 0) break;
            if (base + 4 > chunk->len || chunk->code[base] != OP_EXTRAARG) break;   // 保险：不是 EXTRAARG 就不吃
            int ax = ((int)chunk->code[base + 1] << 16) | ((int)chunk->code[base + 2] << 8) |
                     (int)chunk->code[base + 3];
            p += 4;
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, ax, nm, (int)sizeof(nm));
                snprintf(desc, desc_size, "EXTRAARG K[%d]=%s", ax, nm);
            }
            break;
        }
        case OP_GET_METHOD: {
            if (c != 0) break;
            if (base + 4 > chunk->len || chunk->code[base] != OP_EXTRAARG) break;
            int ax = ((int)chunk->code[base + 1] << 16) | ((int)chunk->code[base + 2] << 8) |
                     (int)chunk->code[base + 3];
            p += 4;
            if (desc) {
                char nm[128];
                dbg_const_str(chunk, ax, nm, (int)sizeof(nm));
                snprintf(desc, desc_size, "EXTRAARG K[%d]=%s", ax, nm);
            }
            break;
        }
        // struct 方法调用融合：紧随 4 字节 = 方法名常量(16) + 静态类型名常量(16)。
        case OP_INVOKE_METHOD_TYPED: {
            int name_idx = dbg_take_u16(chunk, base, &p, &over);
            int type_idx = dbg_take_u16(chunk, base, &p, &over);
            if (desc) {
                char nm[128], tn[128];
                dbg_const_str(chunk, name_idx, nm, (int)sizeof(nm));
                dbg_const_str(chunk, type_idx, tn, (int)sizeof(tn));
                snprintf(desc, desc_size, "方法=%s 静态类型=%s 实参个数=%d（同 OP_CALL 口径）%s",
                         nm, over ? "?" : tn, (int)b, over ? " [截断]" : "");
            }
            break;
        }
        // 多字段累加融合：紧随 C 个字节的字段索引（C = 字段个数）。
        // 长度必须是 4 + C，否则后面整段反汇编错位。
        case OP_ACC_FIELDS: {
            int cnt = c;
            char idx_list[128];
            int w = 0;
            idx_list[0] = '\0';
            for (int i = 0; i < cnt; i++) {
                int fi = dbg_take_u8(chunk, base, &p, &over);
                if (w < (int)sizeof(idx_list) - 8)
                    w += snprintf(idx_list + w, sizeof(idx_list) - (size_t)w,
                                  "%s%d", i ? "," : "", fi);
            }
            if (desc)
                snprintf(desc, desc_size, "字段个数=%d 索引=[%s]%s",
                         cnt, idx_list, over ? " [截断]" : "");
            break;
        }
        default:
            break;
    }
    return 4 + p;
}

// 反汇编单条指令；返回**下一条指令的偏移**（含尾随数据，见 decode_trailing）
int disassembleInstruction(Chunk* chunk, int offset) {
    int line = (chunk->lines && offset < chunk->len) ? chunk->lines[offset] : 0;
    printf("%04d %4d ", offset, line);

    uint8_t instruction = chunk->code[offset];
    if (instruction < sizeof(opCodeNames) / sizeof(opCodeNames[0])) {
        printf("%-20s", opCodeNames[instruction]);
    } else {
        printf("%-20s", "UNKNOWN");
    }

    // 统一打印 A/B/C（或 Bx/sBx/sJ），编码形态见 leno_vm.h
    if (offset + 4 <= chunk->len) {
        uint8_t a = chunk->code[offset + 1];
        uint8_t b = chunk->code[offset + 2];
        uint8_t c = chunk->code[offset + 3];
        int   bx  = ((int)b << 8) | (int)c;
        int   sbx = bx - 32768;
        int   sj  = (int)(((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c);
        if (sj & 0x800000) sj |= (int)0xFF000000;  // 24 位符号扩展

        // 对不同编码形态选合适的字段打印
        switch (instruction) {
            // iAsJ（24 位跳转偏移，A 字段空）
            case OP_JMP:
                printf(" sJ=%d (to %d)", sj, offset + 4 + sj);
                break;
            // iAsBx（A + 有符号 16 位）
            case OP_LOADI:
            case OP_JMP_IF_FALSE:
            case OP_JMP_IF_TRUE:
                printf(" A=%d sBx=%d (to %d)", a, sbx, offset + 4 + sbx);
                break;
            // iABx（A + 16 位常量/全局/upvalue 索引）
            case OP_LOADK:
            case OP_GETGLOBAL:
            case OP_SETGLOBAL:
            case OP_DEFGLOBAL:
            case OP_GETGLOBALFUNC:
            case OP_DEFGLOBALFUNC:
            case OP_GETUPVAL:
            case OP_SETUPVAL:
            case OP_CLOSURE:
            case OP_LOAD_NATIVE_MODULE:
            case OP_GET_MODULE_CONST:
            case OP_GET_MODULE_VAR:
            case OP_SET_MODULE_VAR:
            case OP_GET_MODULE_FUNC:
            case OP_DEFINE_MODULE_FUNC:
            case OP_INIT_LENOMODULE:
            case OP_STRUCT_DEF:
            case OP_ENUM_DEF:
            case OP_FACE_DEF:
            case OP_CSTRUCT_DEF:
            case OP_GET_CSTRUCT_DEF:
            case OP_SET_DECLARED_FACE:
            case OP_TYPE_CHECK:
            case OP_AS_CAST:
            case OP_CFUNC_CALLBACK:
            case OP_PUSH_TYPE_ARGS:
            case OP_SWITCH_LOOKUP:
            case OP_CLOSE:
            case OP_TRY:
            case OP_CATCH:
            case OP_FINALLY:
                printf(" A=%d Bx=%d", a, bx);
                if ((instruction == OP_LOADK || instruction == OP_CLOSURE ||
                     instruction == OP_GETGLOBAL || instruction == OP_GETGLOBALFUNC ||
                     instruction == OP_DEFGLOBALFUNC ||
                     instruction == OP_STRUCT_DEF || instruction == OP_ENUM_DEF ||
                     instruction == OP_FACE_DEF || instruction == OP_CSTRUCT_DEF ||
                     instruction == OP_GET_CSTRUCT_DEF || instruction == OP_GET_MODULE_CONST ||
                     instruction == OP_INIT_LENOMODULE) &&
                    bx >= 0 && bx < chunk->const_cnt) {
                    // 顺便打印常量值（定义类指令的名字常量在这里最有用）
                    printf(" (K[%d]=", bx);
                    const char* str = val_to_string(chunk->constants[bx]);
                    if (str) printf("%s", str); else printf("<null>");
                    printf(")");
                }
                break;
            // iAx（24 位扩展值，A 字段空）
            case OP_EXTRAARG:
                printf(" Ax=%d", (int)(((uint32_t)a << 16) | ((uint32_t)b << 8) | (uint32_t)c));
                break;
            // iABC（默认：A/B/C 三寄存器）
            default:
                printf(" A=%d B=%d C=%d", a, b, c);
                break;
        }

        // 尾随数据摘要（384 ≥ decode_trailing 各分支最坏输出 326 字节，见 OP_INVOKE_METHOD_TYPED）
        char desc[384];
        int total = decode_trailing(chunk, offset, desc, sizeof(desc));
        if (desc[0]) printf("  | %s", desc);
        return offset + total;
    }
    return offset + 4;   // 头部被截断：至少推进 4，避免死循环
}

// 前向声明
// local_count_override > 0 时使用该值（函数场景），否则回退到 chunk->local_count（顶层场景）
static void disassembleChunkRecursive(Chunk* chunk, const char* name, int depth, int local_count_override);

// ============================================================================
// 反汇编自检：跳转目标必须落在指令边界上
// ----------------------------------------------------------------------------
// 长度表一旦与 codegen 的发射顺序对不上，dump 就会错位（"后半段全是 OP_NOP"这类）。
// 用"跳转目标是否正好落在某条指令的起点"做交叉验证：目标不在边界上 ⇒ 要么
// decode_trailing 少算/多算了字节，要么字节码本身被改坏了。只在出错时打印。
// ============================================================================
static void disasm_self_check(Chunk* chunk, const char* name) {
    if (!chunk || !chunk->code || chunk->len < 4) return;
    // ⚠ 指令**不保证 4 字节对齐**：带尾随数据的指令会打破网格（如 OP_CSTRUCT_DEF
    //   每字段 9 字节、OP_STRUCT_INIT 追加的 mod_space16 是 3 字节）⇒ 起点表必须按
    //   字节下标记，不能按 offset/4 记。
    uint8_t* is_start = (uint8_t*)calloc((size_t)chunk->len + 1, 1);
    if (!is_start) return;

    // 第一遍：标记所有指令起点
    for (int off = 0; off < chunk->len; ) {
        is_start[off] = 1;
        int len = decode_trailing(chunk, off, NULL, 0);
        off += (len > 0) ? len : 4;
    }

    // 第二遍：核对跳转目标
    int bad = 0;
    for (int off = 0; off + 4 <= chunk->len; ) {
        uint8_t op = chunk->code[off];
        int target = -1;
        if (op == OP_JMP) {
            int sj = (int)(((uint32_t)chunk->code[off + 1] << 16) |
                           ((uint32_t)chunk->code[off + 2] << 8) | (uint32_t)chunk->code[off + 3]);
            if (sj & 0x800000) sj |= (int)0xFF000000;
            target = off + 4 + sj;
        } else if (op == OP_JMP_IF_FALSE || op == OP_JMP_IF_TRUE) {
            int bx = ((int)chunk->code[off + 2] << 8) | (int)chunk->code[off + 3];
            target = off + 4 + (bx - 32768);
        } else if (op == OP_CMPJMP_LT || op == OP_CMPJMP_LE ||
                   op == OP_CMPJMP_GT || op == OP_CMPJMP_GE || op == OP_CMPJMP_ITER) {
            // 融合指令占 8 字节：偏移在第二个字的前 2 字节（相对"本指令之后"，即 off+8）
            int bx = ((int)chunk->code[off + 4] << 8) | (int)chunk->code[off + 5];
            target = off + 8 + (bx - 32768);
        } else if (op == OP_TRY || op == OP_CATCH) {
            // 这两条的 Bx 相对基准是**本指令起点**（见 gen_try 的 catch_pos - try_pos）
            int bx = ((int)chunk->code[off + 2] << 8) | (int)chunk->code[off + 3];
            target = off + bx;
        } else if (op == OP_FOR_PREP || op == OP_FOR_LOOP) {
            // 偏移编码为 sbx+32768（见 patch_sbx_at）
            int sbx = (((int)chunk->code[off + 6] << 8) | chunk->code[off + 7]) - 32768;
            target = off + 8 + sbx;
        }
        if (target >= 0 && target != chunk->len && target < chunk->len) {
            if (!is_start[target]) {
                if (bad < 8) {
                    printf("  [反汇编自检] %s 的跳转目标 %d 不在指令边界上\n", opcode_name(op), target);
                }
                bad++;
            }
        }
        int len = decode_trailing(chunk, off, NULL, 0);
        off += (len > 0) ? len : 4;
    }
    if (bad) {
        printf("  [反汇编自检] %d 处跳转目标不在边界上（%s）—— 长度表或字节码有问题\n",
               bad, name ? name : "?");
    }
    free(is_start);
}

// 递归反汇编字节码块（包括函数体内的字节码）
static void disassembleChunkRecursive(Chunk* chunk, const char* name, int depth, int local_count_override) {
    int effective_local_count = (local_count_override > 0) ? local_count_override : chunk->local_count;
    // 打印缩进
    for (int i = 0; i < depth; i++) printf("  ");
    printf("===== %s =====\n", name);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("代码长度: %d bytes\n", chunk->len);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("常量数量: %d\n", chunk->const_cnt);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("局部变量槽位数: %d\n", effective_local_count);
    printf("\n");

    for (int i = 0; i < depth; i++) printf("  ");
    printf("Offset Line Instruction          Operand\n");
    for (int i = 0; i < depth; i++) printf("  ");
    printf("------ ---- -------------------- -------\n");

    int offset = 0;
    while (offset < chunk->len) {
        for (int i = 0; i < depth; i++) printf("  ");
        offset = disassembleInstruction(chunk, offset);
        printf("\n");
    }
    disasm_self_check(chunk, name);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("==================\n\n");

    // 递归打印函数常量的字节码
    for (int i = 0; i < chunk->const_cnt; i++) {
        Value constant = chunk->constants[i];
        if (val_is_obj(constant) && val_as_obj(constant)->type == OBJ_FUNCTION) {
            ObjFunction* func = (ObjFunction*)val_as_obj(constant);
            if (func->chunk) {
                char func_name[BUFFER_MEDIUM];
                snprintf(func_name, sizeof(func_name), "函数: %s",
                         func->name ? func->name : "<anonymous>");
                // 传入 func->local_count，因为函数的 local_count 存在 ObjFunction 上而非 Chunk 上
                disassembleChunkRecursive(func->chunk, func_name, depth + 1, func->local_count);
            }
        }
    }
}

// 反汇编整个字节码块（公共接口）
void disassembleChunk(Chunk* chunk, const char* name) {
    disassembleChunkRecursive(chunk, name, 0, 0);
}

// 打印栈内容
void debugPrintStack(Value* stack, int sp) {
    printf("          [");
    for (int i = 0; i < sp; i++) {
        if (i > 0) printf(", ");
        printf(" %s ", val_to_string(stack[i]));
    }
    printf("]\n");
}
