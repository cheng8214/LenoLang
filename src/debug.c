#include "include/lenolang.h"
#include <stdio.h>

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
    "OP_INDEX", "OP_INDEX_SET", "OP_SLICE",
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
    // --- 扩展指令 ---
    "OP_EXTRAARG", "OP_EXTEND",
    "OP_OPCODE_COUNT",
};

/* ---- opcode 编号 → 名 ----
 * 表与 leno_vm.h 的 OpCode 枚举严格同序（本文件开头已声明该约束），所以直接按编号取。
 * 反汇编、调试输出、按编号统计都靠它 —— 别人工对着枚举数编号（那是最容易出错的一步）。 */
const char* opcode_name(int op) {
    if (op < 0 || op >= (int)(sizeof(opCodeNames) / sizeof(opCodeNames[0])))
        return "?";
    return opCodeNames[op];
}

// 反汇编单条指令
// 寄存器式定长 4 字节：op8 A8 B8 C8（或 Bx/sBx/sJ 占 B/C 两字节）
int disassembleInstruction(Chunk* chunk, int offset) {
    int line = (chunk->lines && offset < chunk->len) ? chunk->lines[offset] : 0;
    printf("%04d %4d ", offset, line);

    uint8_t instruction = chunk->code[offset];
    if (instruction < sizeof(opCodeNames) / sizeof(opCodeNames[0])) {
        printf("%-20s", opCodeNames[instruction]);
    } else {
        printf("%-20s", "UNKNOWN");
    }

    // 寄存器式定长 4 字节，统一打印 A/B/C（或 Bx/sBx/sJ）
    // 编码形态见 leno_vm.h：iABC / iABx / iAsBx / iAsJ / iAx
    // 这里只做最简反汇编：把后续 3 字节按 A/B/C 打印，便于人工核对
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
            case OP_INIT_LENOMODULE:
            case OP_STRUCT_DEF:
            case OP_ENUM_DEF:
            case OP_FACE_DEF:
            case OP_CSTRUCT_DEF:
            case OP_GET_CSTRUCT_DEF:
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
                     instruction == OP_DEFGLOBALFUNC) &&
                    bx >= 0 && bx < chunk->const_cnt) {
                    // 对 LOADK / CLOSURE 顺便打印常量值
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
    }
    return offset + 4;  // 寄存器式所有指令定长 4 字节
}

// 前向声明
// local_count_override > 0 时使用该值（函数场景），否则回退到 chunk->local_count（顶层场景）
static void disassembleChunkRecursive(Chunk* chunk, const char* name, int depth, int local_count_override);

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
