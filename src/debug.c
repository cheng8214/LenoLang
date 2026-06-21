#include "include/lenolang.h"
#include <stdio.h>

// 指令名称表（用于调试）- 必须与 leno_vm.h 中的 OpCode 枚举完全一致
static const char* opCodeNames[] = {
    "OP_CONST", "OP_NULL", "OP_TRUE", "OP_FALSE", "OP_ZERO", "OP_ONE", "OP_POP", "OP_DUP",
    "OP_GET_LOCAL", "OP_SET_LOCAL", "OP_SET_LOCAL_POP", "OP_MOVE_LOCAL",
    "OP_GET_GLOBAL", "OP_SET_GLOBAL",
    "OP_GET_UPVALUE", "OP_SET_UPVALUE", "OP_CLOSE_UPVALUE", "OP_DEFINE_GLOBAL",
    "OP_GET_GLOBAL_FUNC", "OP_DEFINE_GLOBAL_FUNC", "OP_GET_NATIVE",
    "OP_ADD", "OP_SUB", "OP_MUL", "OP_DIV", "OP_MOD", "OP_BITAND", "OP_BITOR", "OP_BITXOR", "OP_BITNOT", "OP_SHL", "OP_SHR", "OP_USHR", "OP_NEG", "OP_NOT",
    "OP_CAST_FLOAT", "OP_CAST_INT", "OP_CAST_STRING", "OP_SET_PTR_ELEM_TYPE", "OP_SET_DECLARED_FACE",
    "OP_INC", "OP_DEC", "OP_INC_LOCAL", "OP_DEC_LOCAL", "OP_PRE_INC_LOCAL", "OP_PRE_DEC_LOCAL",
    "OP_EQ", "OP_NEQ", "OP_LT", "OP_GT", "OP_LE", "OP_GE", "OP_IN", "OP_RANGE",
    "OP_JUMP", "OP_JUMP_IF_FALSE", "OP_JUMP_IF_TRUE", "OP_LOOP",
    "OP_CALL", "OP_TAIL_CALL", "OP_CLOSURE", "OP_RETURN",
    "OP_ARRAY", "OP_ARRAY_GET", "OP_ARRAY_SET", "OP_ARRAY_APPEND", "OP_ARRAY_APPEND_NOPUSH",
    "OP_DICT", "OP_DICT_GET", "OP_DICT_SET", "OP_DICT_GET_KEY",
    "OP_LOAD_NATIVE_MODULE",
    "OP_MODULE_CALL",
    "OP_GET_MODULE_CONST",
    "OP_STRING_ADD",
    "OP_INDEX",
    "OP_INDEX_SET",
    "OP_SLICE",
    "OP_LENGTH",
    "OP_ITER_GET",
    "OP_ITER_GET_VALUE",
    "OP_TRY", "OP_CATCH", "OP_FINALLY", "OP_END_TRY", "OP_THROW",
    "OP_GET_MODULE_VAR", "OP_SET_MODULE_VAR", "OP_GET_MODULE_FUNC", "OP_DEFINE_MODULE_FUNC",
    "OP_GET_PROPERTY", "OP_TYPE_CHECK", "OP_AS_CAST", "OP_FOR_PREP", "OP_FOR_LOOP",
    // 类型特化指令
    "OP_ADD_INT", "OP_SUB_INT", "OP_MUL_INT", "OP_DIV_INT", "OP_MOD_INT",
    "OP_ADD_FLOAT", "OP_SUB_FLOAT", "OP_MUL_FLOAT", "OP_DIV_FLOAT",
    "OP_NEG_INT", "OP_NEG_FLOAT",
    "OP_EQ_INT", "OP_LT_INT", "OP_GT_INT", "OP_LE_INT", "OP_GE_INT",
    "OP_EQ_FLOAT", "OP_LT_FLOAT", "OP_GT_FLOAT", "OP_LE_FLOAT", "OP_GE_FLOAT",
    // 立即数操作码
    "OP_ADD_INT_IMM", "OP_SUB_INT_IMM", "OP_MUL_INT_IMM",
    "OP_LT_INT_IMM", "OP_GT_INT_IMM", "OP_LE_INT_IMM", "OP_GE_INT_IMM", "OP_EQ_INT_IMM",
    "OP_SHL_IMM", "OP_SHR_IMM", "OP_USHR_IMM",
    // struct 相关指令
    "OP_STRUCT_DEF", "OP_STRUCT_INIT", "OP_GET_FIELD", "OP_SET_FIELD", "OP_GET_METHOD",
    // enum 相关指令
    "OP_ENUM_DEF",
    // face 相关指令
    "OP_FACE_DEF",
    // cstruct 相关指令
    "OP_CSTRUCT_DEF", "OP_GET_CSTRUCT_DEF",
    // 协程相关指令
    "OP_AWAIT", "OP_ASYNC_CALL",
    // 模块初始化指令
    "OP_INIT_LENOMODULE",
    // clib 调用指令
    "OP_CLIB_CALL",
    "OP_CFUNC_CALLBACK",
    // 原生函数调用合并指令
    "OP_CALL_NATIVE", "OP_TAIL_CALL_NATIVE",
    // C 布局类型转换
    "OP_U8_TO_F64"
};

// 反汇编单条指令
int disassembleInstruction(Chunk* chunk, int offset) {
    printf("%04d ", offset);
    
    uint8_t instruction = chunk->code[offset];
    if (instruction < sizeof(opCodeNames) / sizeof(opCodeNames[0])) {
        printf("%-20s", opCodeNames[instruction]);
    } else {
        printf("%-20s", "UNKNOWN");
    }
    
    switch (instruction) {
        case OP_CONST: {
            int const_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d (", const_idx);
            // 检查常量索引是否有效
            if (const_idx >= 0 && const_idx < chunk->const_cnt) {
                const char* str = val_to_string(chunk->constants[const_idx]);
                if (str) {
                    printf("%s", str);
                } else {
                    printf("<null string>");
                }
            } else {
                printf("<invalid index>");
            }
            printf(")");
            return offset + 3;
        }
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE: {
            int32_t jump_offset = (int32_t)((chunk->code[offset + 1] << 24) | (chunk->code[offset + 2] << 16) | (chunk->code[offset + 3] << 8) | chunk->code[offset + 4]);
            printf(" %d (to %d)", jump_offset, offset + 5 + jump_offset);
            return offset + 5;
        }
        case OP_LOOP: {
            int32_t loop_offset = (int32_t)((chunk->code[offset + 1] << 24) | (chunk->code[offset + 2] << 16) | (chunk->code[offset + 3] << 8) | chunk->code[offset + 4]);
            printf(" %d (to %d)", loop_offset, offset + 5 - loop_offset);
            return offset + 5;
        }
        case OP_CALL:
        case OP_TAIL_CALL: {
            int arg_count = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", arg_count);
            return offset + 3;
        }
        case OP_CALL_NATIVE:
        case OP_TAIL_CALL_NATIVE: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int arg_count = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
            printf(" %d (%s) args=%d", name_idx,
                   name_idx < chunk->const_cnt ? val_to_string(chunk->constants[name_idx]) : "?",
                   arg_count);
            return offset + 5;
        }
        case OP_ARRAY_GET:
        case OP_ARRAY_SET:
        case OP_DICT_GET:
            // 无操作数，从栈读取值
            return offset + 1;
        case OP_ARRAY_APPEND:
        case OP_ARRAY_APPEND_NOPUSH:
        case OP_DICT_SET:
            return offset + 1;
        case OP_ARRAY:
        case OP_DICT: {
            int count = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", count);
            return offset + 3;
        }
        case OP_INC_LOCAL:
        case OP_DEC_LOCAL:
        case OP_PRE_INC_LOCAL:
        case OP_PRE_DEC_LOCAL: {
            int idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", idx);
            return offset + 3;
        }
        case OP_MOVE_LOCAL: {
            int src_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int dst_idx = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
            printf(" %d->%d", src_idx, dst_idx);
            return offset + 5;
        }
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_SET_LOCAL_POP:
        case OP_GET_UPVALUE:
        case OP_SET_UPVALUE: {
            int idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", idx);
            return offset + 3;
        }
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_DEFINE_GLOBAL:
        case OP_GET_GLOBAL_FUNC:
        case OP_DEFINE_GLOBAL_FUNC:
        case OP_GET_MODULE_VAR:
        case OP_SET_MODULE_VAR:
        case OP_GET_MODULE_FUNC:
        case OP_DEFINE_MODULE_FUNC: {
            int idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", idx);
            return offset + 3;
        }
        case OP_GET_NATIVE: {
            int const_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", const_idx);
            return offset + 3;
        }
        case OP_CLOSURE: {
            int const_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", const_idx);
            // 获取函数对象以确定 upvalue 数量
            int upvalue_count = 0;
            if (const_idx >= 0 && const_idx < chunk->const_cnt) {
                Value func_val = chunk->constants[const_idx];
                if (val_is_obj(func_val) && val_as_obj(func_val)->type == OBJ_FUNCTION) {
                    ObjFunction* func = (ObjFunction*)val_as_obj(func_val);
                    upvalue_count = func->upvalue_count;
                }
            }
            // 打印 upvalue 信息
            int data_offset = offset + 3;
            for (int i = 0; i < upvalue_count; i++) {
                uint16_t is_local = (chunk->code[data_offset] << 8) | chunk->code[data_offset + 1];
                uint16_t idx = (chunk->code[data_offset + 2] << 8) | chunk->code[data_offset + 3];
                uint16_t is_value = (chunk->code[data_offset + 4] << 8) | chunk->code[data_offset + 5];
                printf(" [up(%d): local=%d idx=%d val=%d]", i, is_local, idx, is_value);
                data_offset += 6;
            }
            return data_offset;
        }
        case OP_LOAD_NATIVE_MODULE: {
            int const_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d (%s)", const_idx, val_to_string(chunk->constants[const_idx]));
            return offset + 3;
        }
        case OP_MODULE_CALL: {
            int module_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int method_idx = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
            int arg_count = (chunk->code[offset + 5] << 8) | chunk->code[offset + 6];
            printf(" %d %d %d", module_idx, method_idx, arg_count);
            return offset + 7;
        }
        case OP_GET_PROPERTY: {
            int const_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d (%s)", const_idx, val_to_string(chunk->constants[const_idx]));
            return offset + 3;
        }
        case OP_TRY: {
            int catch_offset = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int finally_offset = (chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
            printf(" catch=%d finally=%d", catch_offset, finally_offset);
            return offset + 5;
        }

        case OP_CATCH:
        case OP_FINALLY:
        case OP_END_TRY:
        case OP_THROW:
            return offset + 1;

        case OP_INDEX:
        case OP_INDEX_SET:
        case OP_SLICE:
        case OP_LENGTH:
        case OP_STRING_ADD:
            return offset + 1;
        case OP_TYPE_CHECK:
        case OP_AS_CAST: {
            int type_kind = chunk->code[offset + 1];
            if (type_kind == TYPE_FACE) {
                int face_name_idx = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
                printf(" TYPE_FACE face_name=%d", face_name_idx);
                if (face_name_idx >= 0 && face_name_idx < chunk->const_cnt) {
                    Value val = chunk->constants[face_name_idx];
                    if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRING) {
                        printf(" (%s)", ((ObjString*)val_as_obj(val))->chars);
                    }
                }
                return offset + 4;
            } else if (type_kind == TYPE_STRUCT) {
                int struct_name_idx = (chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
                printf(" TYPE_STRUCT struct_name=%d", struct_name_idx);
                if (struct_name_idx >= 0 && struct_name_idx < chunk->const_cnt) {
                    Value val = chunk->constants[struct_name_idx];
                    if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRING) {
                        printf(" (%s)", ((ObjString*)val_as_obj(val))->chars);
                    }
                }
                return offset + 4;
            } else {
                int elem_type = chunk->code[offset + 2];
                printf(" %d %d", type_kind, elem_type);
                return offset + 3;
            }
        }
        case OP_STRUCT_DEF: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int field_count = chunk->code[offset + 3];
            int method_count = chunk->code[offset + 4];
            // 读取 impl 声明信息
            int impl_count = chunk->code[offset + 5];
            printf(" name=%d fields=%d methods=%d impls=%d", name_idx, field_count, method_count, impl_count);
            // 跳过指令头 (6字节: opcode + name(2) + fields(1) + methods(1) + impl_count(1)) + impl信息 + 字段信息 + 方法信息
            // 每个impl: 2字节face名称常量索引
            // 每个字段: 2字节字段名 + 1字节类型 + [如果是struct: 1字节是否有类型名 + (可选2字节类型名)] + [如果是Ptr[T]: 1字节元素类型] + 1字节是否有默认值 + (可选2字节默认值)
            // 每个方法: 2字节方法名 + 2字节函数
            int data_offset = offset + 6; // 指令头 6 字节
            // 跳过 impl 名称
            for (int i = 0; i < impl_count; i++) {
                data_offset += 2; // impl face名称(2)
            }
            for (int i = 0; i < field_count; i++) {
                data_offset += 3; // 字段名(2) + 类型(1)
                // 如果是 struct 类型，读取是否有类型名
                if (chunk->code[data_offset - 1] == 21) { // TYPE_STRUCT = 21
                    data_offset += 1; // 是否有类型名字节
                    if (chunk->code[data_offset - 1]) { // 如果有类型名
                        data_offset += 2; // 类型名常量索引
                    }
                }
                // 如果是 Ptr[T] 类型，读取元素类型
                if (chunk->code[data_offset - 1] == 26) { // TYPE_PTR_GENERIC = 26
                    data_offset += 1; // 元素类型
                }
                data_offset += 1; // 是否有默认值
                if (chunk->code[data_offset - 1]) { // 如果有默认值
                    data_offset += 2; // 默认值常量索引
                }
            }
            data_offset += method_count * 4; // 每个方法: 方法名(2) + 函数(2)
            return data_offset;
        }
        case OP_STRUCT_INIT: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int arg_count = chunk->code[offset + 3];
            int generic_count = chunk->code[offset + 4];
            printf(" name=%d args=%d generics=%d", name_idx, arg_count, generic_count);
            // 跳过指令头 (5字节) + 泛型参数 (每个2字节) + 字段索引 (每个1字节)
            return offset + 5 + generic_count * 2 + arg_count;
        }
        case OP_GET_FIELD:
        case OP_SET_FIELD: {
            int field_idx = chunk->code[offset + 1];
            printf(" %d", field_idx);
            return offset + 2;
        }
        case OP_GET_METHOD: {
            int method_name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d", method_name_idx);
            if (method_name_idx >= 0 && method_name_idx < chunk->const_cnt) {
                Value val = chunk->constants[method_name_idx];
                if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRING) {
                    printf(" (%s)", ((ObjString*)val_as_obj(val))->chars);
                }
            }
            return offset + 3;
        }
        case OP_ENUM_DEF: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int member_count = chunk->code[offset + 3];
            printf(" name=%d members=%d", name_idx, member_count);
            // 跳过指令头 (4字节) + 成员信息 (每个成员: 2字节名称 + 2字节值)
            return offset + 4 + member_count * 4;
        }
        case OP_FACE_DEF: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int method_count = chunk->code[offset + 3];
            printf(" name=%d methods=%d", name_idx, method_count);
            // 跳过指令头 (4字节) + 方法信息
            int data_offset = offset + 4;
            for (int i = 0; i < method_count; i++) {
                data_offset += 2; // 方法名常量索引
                int param_count = chunk->code[data_offset];
                data_offset += 1; // 参数数量
                data_offset += 1; // 返回类型
                data_offset += param_count; // 参数类型
            }
            return data_offset;
        }
        case OP_CSTRUCT_DEF: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int field_count = chunk->code[offset + 3];
            int total_size = (chunk->code[offset + 4] << 8) | chunk->code[offset + 5];
            int alignment = chunk->code[offset + 6];
            printf(" name=%d fields=%d size=%d align=%d", name_idx, field_count, total_size, alignment);
            // 跳过指令头 (7字节) + 字段信息 (每个字段: 2字节名称 + 1字节类型 + 2字节偏移 + 2字节数组维度 + 2字节结构体类型名)
            // 注意：如果字段类型是 TYPE_PTR_GENERIC (19)，还有一个额外的字节表示元素类型
            int data_offset = offset + 7;
            for (int i = 0; i < field_count; i++) {
                int field_type = chunk->code[data_offset + 2]; // 字段类型在字段名(2字节)之后
                data_offset += 9; // 基础字段信息大小
                if (field_type == 19) { // TYPE_PTR_GENERIC
                    data_offset += 1; // 额外1字节元素类型
                }
            }
            return data_offset;
        }
        case OP_GET_CSTRUCT_DEF: {
            int name_idx = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" %d (", name_idx);
            if (name_idx >= 0 && name_idx < chunk->const_cnt) {
                const char* str = val_to_string(chunk->constants[name_idx]);
                if (str) {
                    printf("%s", str);
                } else {
                    printf("<null string>");
                }
            } else {
                printf("<invalid index>");
            }
            printf(")");
            return offset + 3;
        }
        case OP_FOR_PREP: {
            // OP_FOR_PREP: start_slot(1), end_slot(1), step_slot(1), loop_var_slot(1), inclusive(1), jump_offset(2) = 7字节
            int start_slot = chunk->code[offset + 1];
            int end_slot = chunk->code[offset + 2];
            int step_slot = chunk->code[offset + 3];
            int loop_var_slot = chunk->code[offset + 4];
            int inclusive = chunk->code[offset + 5];
            int jump_offset = (chunk->code[offset + 6] << 8) | chunk->code[offset + 7];
            printf(" start=%d end=%d step=%d var=%d inc=%d ->%d", start_slot, end_slot, step_slot, loop_var_slot, inclusive, jump_offset);
            return offset + 8;
        }
        case OP_FOR_LOOP: {
            // OP_FOR_LOOP: loop_var_slot(1), step_slot(1), end_slot(1), inclusive(1), jump_offset(2) = 6字节
            int loop_var_slot = chunk->code[offset + 1];
            int step_slot = chunk->code[offset + 2];
            int end_slot = chunk->code[offset + 3];
            int inclusive = chunk->code[offset + 4];
            int jump_offset = (chunk->code[offset + 5] << 8) | chunk->code[offset + 6];
            printf(" var=%d step=%d end=%d inc=%d ->%d", loop_var_slot, step_slot, end_slot, inclusive, jump_offset);
            return offset + 7;
        }
        // 立即数操作码（单字节有符号立即数）
        case OP_ADD_INT_IMM:
        case OP_SUB_INT_IMM:
        case OP_MUL_INT_IMM:
        case OP_LT_INT_IMM:
        case OP_GT_INT_IMM:
        case OP_LE_INT_IMM:
        case OP_GE_INT_IMM:
        case OP_EQ_INT_IMM: {
            int8_t imm = (int8_t)chunk->code[offset + 1];
            printf(" %d", imm);
            return offset + 2;
        }
        // 位移立即数操作码（单字节无符号立即数）
        case OP_SHL_IMM:
        case OP_SHR_IMM:
        case OP_USHR_IMM: {
            uint8_t imm = chunk->code[offset + 1];
            printf(" %u", (unsigned int)imm);
            return offset + 2;
        }
        case OP_AWAIT: {
            printf(" (await Future)");
            return offset + 1;
        }
        case OP_ASYNC_CALL: {
            int arg_count = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf(" args=%d", arg_count);
            return offset + 3;
        }
        case OP_INIT_LENOMODULE: {
            printf(" (init .leno module)");
            return offset + 1;
        }
        case OP_CLIB_CALL: {
            // OP_CLIB_CALL: arg_count(2), ret_type_kind(1), user_arg_count(1), arg_types[user_arg_count](1 each)
            int arg_count = (chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int ret_type_kind = chunk->code[offset + 3];
            int user_arg_count = chunk->code[offset + 4];
            printf(" args=%d ret_kind=%d user_args=%d types=[", arg_count, ret_type_kind, user_arg_count);
            for (int i = 0; i < user_arg_count; i++) {
                if (i > 0) printf(",");
                printf("%d", chunk->code[offset + 5 + i]);
            }
            printf("]");
            return offset + 5 + user_arg_count;
        }
        case OP_CFUNC_CALLBACK: {
            // OP_CFUNC_CALLBACK: ret_type(1) param_count(1) param_types[param_count](1 each)
            int ret_type = chunk->code[offset + 1];
            int param_count = chunk->code[offset + 2];
            printf(" ret=%d params=%d types=[", ret_type, param_count);
            for (int i = 0; i < param_count; i++) {
                if (i > 0) printf(",");
                printf("%d", chunk->code[offset + 3 + i]);
            }
            printf("]");
            return offset + 3 + param_count;
        }
        default:
            return offset + 1;
    }
}

// 前向声明
static void disassembleChunkRecursive(Chunk* chunk, const char* name, int depth);

// 递归反汇编字节码块（包括函数体内的字节码）
static void disassembleChunkRecursive(Chunk* chunk, const char* name, int depth) {
    // 打印缩进
    for (int i = 0; i < depth; i++) printf("  ");
    printf("===== %s =====\n", name);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("代码长度: %d bytes\n", chunk->len);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("常量数量: %d\n", chunk->const_cnt);
    for (int i = 0; i < depth; i++) printf("  ");
    printf("局部变量槽位数: %d\n", chunk->local_count);
    printf("\n");
    
    for (int i = 0; i < depth; i++) printf("  ");
    printf("Offset Instruction          Operand\n");
    for (int i = 0; i < depth; i++) printf("  ");
    printf("------ -------------------- -------\n");
    
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
                disassembleChunkRecursive(func->chunk, func_name, depth + 1);
            }
        }
    }
}

// 反汇编整个字节码块（公共接口）
void disassembleChunk(Chunk* chunk, const char* name) {
    disassembleChunkRecursive(chunk, name, 0);
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
