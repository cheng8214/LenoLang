#include "codegen.h"
#include "../semantic/semantic_internal.h"
#include "../module/ffi/ffi_clib.h"
#include <ctype.h>

static void gen_binary(CodeGen* gen, Ast* ast);
static TypeKind get_expr_type_kind(Ast* ast);

static TypeKind get_expr_type_kind(Ast* ast) {
    if (!ast || !ast->cached_type) return TYPE_ANY;
    return ast->cached_type->kind;
}

// 辅助函数：收集 "obj.f0 + obj.f1 + ..." 链中的字段访问信息
// 返回 1 表示可以优化（全部是同一对象的字段访问），0 表示不行
// 收集到的 obj_ast 指针、字段索引列表存入 out 参数
static int collect_acc_fields(Ast* ast, Ast** out_obj, int* out_fields, int* out_count, int max_fields) {
    if (!ast || *out_count >= max_fields) return 0;

    if (ast->kind == AST_FIELD_ACCESS && ast->u.field_access.field_index >= 0) {
        // 叶子：字段访问
        out_fields[*out_count] = ast->u.field_access.field_index;
        (*out_count)++;
        if (*out_obj == NULL) {
            *out_obj = ast->u.field_access.obj;
            return 1;
        }
        // 检查是否是同一个对象（AST_VAR 且同名）
        Ast* new_obj = ast->u.field_access.obj;
        if (new_obj->kind == (*out_obj)->kind &&
            new_obj->kind == AST_VAR &&
            new_obj->u.var.ref.index == (*out_obj)->u.var.ref.index &&
            new_obj->u.var.ref.kind == (*out_obj)->u.var.ref.kind) {
            return 1;
        }
        return 0;
    }

    if (ast->kind == AST_BINOP && ast->u.binop.op == TOK_PLUS) {
        // 递归收集左子树和右子树
        if (!collect_acc_fields(ast->u.binop.l, out_obj, out_fields, out_count, max_fields))
            return 0;
        return collect_acc_fields(ast->u.binop.r, out_obj, out_fields, out_count, max_fields);
    }

    return 0;
}

// 尝试发射 OP_ACC_FIELDS，返回 1 表示成功优化
static int try_emit_acc_fields(CodeGen* gen, Ast* ast) {
    // 只处理 float 字段加法（编译器已知类型）
    TypeKind result_type = get_expr_type_kind(ast);
    if (result_type != TYPE_FLOAT) return 0;

    Ast* obj = NULL;
    int fields[16];
    int count = 0;
    if (!collect_acc_fields(ast, &obj, fields, &count, 16)) return 0;
    if (count < 2) return 0;  // 至少 2 个字段才值得优化

    // 检查对象表达式是否是简单的 local 变量
    if (!obj || obj->kind != AST_VAR) return 0;
    if (obj->u.var.ref.kind != SYM_LOCAL) return 0;

    // 发射：GET_LOCAL obj + OP_ACC_FIELDS count field_indices...
    emit_bytes_2(gen, OP_GET_LOCAL, obj->u.var.ref.index, ast->line);
    emit_byte(gen, OP_ACC_FIELDS, ast->line);
    emit_byte(gen, (uint8_t)count, ast->line);
    for (int i = 0; i < count; i++) {
        emit_byte(gen, (uint8_t)fields[i], ast->line);
    }
    return 1;
}
static void gen_unary(CodeGen* gen, Ast* ast);
static void gen_variable(CodeGen* gen, Ast* ast, int can_assign);
static void gen_call(CodeGen* gen, Ast* ast);

// 生成数组 add 操作（公共函数）
// receiver_ast: 数组表达式
// arg_ast: 要添加的元素表达式
// need_result: 是否需要返回值（表达式用 OP_ARRAY_APPEND，语句用 OP_ARRAY_APPEND_NOPUSH）
// line: 行号
void gen_array_add(CodeGen* gen, Ast* receiver_ast, Ast* arg_ast, int need_result, int line) {
    gen_expr(gen, receiver_ast);
    gen_expr(gen, arg_ast);
    emit_byte(gen, need_result ? OP_ARRAY_APPEND : OP_ARRAY_APPEND_NOPUSH, line);
}

// 通过变量符号生成数组 add 操作（公共函数）
// var_sym: 数组变量符号
// arg_ast: 要添加的元素表达式
// need_result: 是否需要返回值
// line: 行号
void gen_array_add_by_symbol(CodeGen* gen, Symbol* var_sym, Ast* arg_ast, int need_result, int line) {
    switch (var_sym->kind) {
        case SYM_LOCAL:
        case SYM_PARAM:
            emit_bytes_2(gen, OP_GET_LOCAL, var_sym->index, line);
            break;
        case SYM_GLOBAL:
            emit_get_global(gen, var_sym->index, line);
            break;
        case SYM_UPVALUE:
            emit_bytes_2(gen, OP_GET_UPVALUE, var_sym->index, line);
            break;
        default:
            error_add_at(ERR_SEMANTIC, line, 0, "未知的变量类型");
            return;
    }
    gen_expr(gen, arg_ast);
    emit_byte(gen, need_result ? OP_ARRAY_APPEND : OP_ARRAY_APPEND_NOPUSH, line);
}

static void gen_binary(CodeGen* gen, Ast* ast) {
    // 短路运算符需要特殊处理，不能提前生成右操作数
    if (ast->u.binop.op == TOK_AND) {
        gen_expr(gen, ast->u.binop.l);
        int end_jump = emit_jump(gen, OP_JUMP_IF_FALSE, ast->line);
        emit_byte(gen, OP_POP, ast->line);
        gen_expr(gen, ast->u.binop.r);
        patch_jump(gen, end_jump);
        return;
    }

    if (ast->u.binop.op == TOK_OR) {
        gen_expr(gen, ast->u.binop.l);
        // 优化：用 OP_JUMP_IF_TRUE 直接跳到末尾，消除原来 OP_JUMP_IF_FALSE + OP_JUMP 的死跳转
        int end_jump = emit_jump(gen, OP_JUMP_IF_TRUE, ast->line);
        emit_byte(gen, OP_POP, ast->line);
        gen_expr(gen, ast->u.binop.r);
        patch_jump(gen, end_jump);
        return;
    }

    // ?? 空值合并：left ?? right
    // 若 left 不为 null，返回 left；否则求值 right
    if (ast->u.binop.op == TOK_NULL_COALESCE) {
        gen_expr(gen, ast->u.binop.l);        // [left]
        emit_byte(gen, OP_DUP, ast->line);    // [left, left]
        emit_byte(gen, OP_IS_NULL, ast->line); // [left, is_null]（弹出 dup，压入 bool）
        // 注意：?? 不能用 or 那样的反转优化（OP_JUMP_IF_FALSE→直接跳末尾），
        // 因为条件值(is_null)不是结果值(left)，跳转后 is_null 仍留在栈上导致不平衡
        int null_jump = emit_jump(gen, OP_JUMP_IF_TRUE, ast->line); // is_null → 跳到 null 路径

        // 非 null 路径：保留 left
        emit_byte(gen, OP_POP, ast->line);    // [left] 弹出 is_null (false)
        int end_jump = emit_jump(gen, OP_JUMP, ast->line);

        // null 路径：弹出 left，求值 right
        patch_jump(gen, null_jump);
        emit_byte(gen, OP_POP, ast->line);    // [null] 弹出 is_null (true)
        emit_byte(gen, OP_POP, ast->line);    // [] 弹出 null left
        gen_expr(gen, ast->u.binop.r);        // [right]

        patch_jump(gen, end_jump);
        return;
    }

    // 类型特化：如果编译器已知操作数类型，使用特化指令
    TypeKind left_type = get_expr_type_kind(ast->u.binop.l);
    TypeKind right_type = get_expr_type_kind(ast->u.binop.r);

    // 检查是否可以使用立即数操作码（右操作数是 -128~127 的整数字面量）
    Ast* right_ast = ast->u.binop.r;
    int can_use_imm = 0;
    int8_t imm_val = 0;
    if (right_ast->kind == AST_NUM && !right_ast->u.num.is_float && !right_ast->u.num.is_bigint) {
        double val = right_ast->u.num.value;
        if (val >= -128 && val <= 127 && val == (double)(int8_t)val) {
            can_use_imm = 1;
            imm_val = (int8_t)val;
        }
    }

    // 融合优化：检测 "obj.f0 + obj.f1 + ..." 模式（在生成左操作数之前）
    if (ast->u.binop.op == TOK_PLUS && try_emit_acc_fields(gen, ast)) return;

    // 普通二元运算符（非立即数路径）
    gen_expr(gen, ast->u.binop.l);

    switch (ast->u.binop.op) {
        case TOK_PLUS:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_ADD_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_ADD_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_ADD_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_ADD, ast->line);
            }
            break;
        case TOK_MINUS:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_SUB_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_SUB_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_SUB_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_SUB, ast->line);
            }
            break;
        case TOK_STAR:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_MUL_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_MUL_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_MUL_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_MUL, ast->line);
            }
            break;
        case TOK_SLASH:
            gen_expr(gen, ast->u.binop.r);
            if (left_type == TYPE_INT && right_type == TYPE_INT)
                emit_byte(gen, OP_DIV_INT, ast->line);
            else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                emit_byte(gen, OP_DIV_FLOAT, ast->line);
            else
                emit_byte(gen, OP_DIV, ast->line);
            break;
        case TOK_MOD:
            gen_expr(gen, ast->u.binop.r);
            if (left_type == TYPE_INT && right_type == TYPE_INT)
                emit_byte(gen, OP_MOD_INT, ast->line);
            else
                emit_byte(gen, OP_MOD, ast->line);
            break;
        case TOK_BITAND: gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_BITAND, ast->line); break;
        case TOK_BITOR:  gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_BITOR, ast->line); break;
        case TOK_BITXOR: gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_BITXOR, ast->line); break;
        case TOK_SHL:
            if (can_use_imm && imm_val >= 0) {
                emit_byte_imm(gen, OP_SHL_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                emit_byte(gen, OP_SHL, ast->line);
            }
            break;
        case TOK_SHR:
            if (can_use_imm && imm_val >= 0) {
                emit_byte_imm(gen, OP_SHR_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                emit_byte(gen, OP_SHR, ast->line);
            }
            break;
        case TOK_USHR:
            if (can_use_imm && imm_val >= 0) {
                emit_byte_imm(gen, OP_USHR_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                emit_byte(gen, OP_USHR, ast->line);
            }
            break;
        case TOK_EQEQ:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_EQ_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_EQ_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_EQ_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_EQ, ast->line);
            }
            break;
        case TOK_NEQ:   gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_NEQ, ast->line); break;
        case TOK_LT:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_LT_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_LT_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_LT_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_LT, ast->line);
            }
            break;
        case TOK_GT:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_GT_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_GT_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_GT_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_GT, ast->line);
            }
            break;
        case TOK_LE:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_LE_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_LE_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_LE_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_LE, ast->line);
            }
            break;
        case TOK_GE:
            if (can_use_imm && left_type == TYPE_INT) {
                emit_byte_imm(gen, OP_GE_INT_IMM, imm_val, ast->line);
            } else {
                gen_expr(gen, ast->u.binop.r);
                if (left_type == TYPE_INT && right_type == TYPE_INT)
                    emit_byte(gen, OP_GE_INT, ast->line);
                else if (left_type == TYPE_FLOAT && right_type == TYPE_FLOAT)
                    emit_byte(gen, OP_GE_FLOAT, ast->line);
                else
                    emit_byte(gen, OP_GE, ast->line);
            }
            break;
        case TOK_IN:
            gen_expr(gen, ast->u.binop.r);
            emit_byte(gen, OP_IN, ast->line);
            break;
        case TOK_NOT_IN:
            gen_expr(gen, ast->u.binop.r);
            emit_byte(gen, OP_IN, ast->line);
            emit_byte(gen, OP_NOT, ast->line);  // not in = in + not
            break;
        default:
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未知的二元操作符");
            break;
    }
}

static void gen_unary(CodeGen* gen, Ast* ast) {
    if (ast->u.unary.op == TOK_INC || ast->u.unary.op == TOK_DEC) {
        if (ast->u.unary.operand->kind != AST_VAR) {
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, "++ 和 -- 只能用于变量");
            return;
        }

        SymRef* ref = &ast->u.unary.operand->u.var.ref;

        if (ast->u.unary.is_postfix) {
            if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
                emit_bytes_2(gen, ast->u.unary.op == TOK_INC ? OP_INC_LOCAL : OP_DEC_LOCAL,
                          ref->index, ast->line);
            } else if (ref->kind == SYM_GLOBAL) {
                emit_get_global(gen, ref->index, ast->line);
                emit_byte(gen, OP_DUP, ast->line);
                emit_byte(gen, ast->u.unary.op == TOK_INC ? OP_INC : OP_DEC, ast->line);
                emit_set_global(gen, ref->index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else if (ref->kind == SYM_UPVALUE) {
                // 后缀 ++/-- for upvalue: 先获取值，然后增加/减少，再设置
                emit_bytes_2(gen, OP_GET_UPVALUE, ref->index, ast->line);
                emit_byte(gen, OP_DUP, ast->line);
                emit_byte(gen, ast->u.unary.op == TOK_INC ? OP_INC : OP_DEC, ast->line);
                emit_bytes_2(gen, OP_SET_UPVALUE, ref->index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "不支持的变量类型用于 ++/--");
                return;
            }
        } else {
            if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
                emit_bytes_2(gen, ast->u.unary.op == TOK_INC ? OP_PRE_INC_LOCAL : OP_PRE_DEC_LOCAL,
                          ref->index, ast->line);
            } else if (ref->kind == SYM_GLOBAL) {
                emit_get_global(gen, ref->index, ast->line);
                emit_byte(gen, ast->u.unary.op == TOK_INC ? OP_INC : OP_DEC, ast->line);
                emit_byte(gen, OP_DUP, ast->line);
                emit_set_global(gen, ref->index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else if (ref->kind == SYM_UPVALUE) {
                // 前缀 ++/-- for upvalue: 先增加/减少，然后获取值，再设置
                emit_bytes_2(gen, OP_GET_UPVALUE, ref->index, ast->line);
                emit_byte(gen, ast->u.unary.op == TOK_INC ? OP_INC : OP_DEC, ast->line);
                emit_byte(gen, OP_DUP, ast->line);
                emit_bytes_2(gen, OP_SET_UPVALUE, ref->index, ast->line);
                emit_byte(gen, OP_POP, ast->line);
            } else {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "不支持的变量类型用于 ++/--");
                return;
            }
        }

        return;
    }

    gen_expr(gen, ast->u.unary.operand);

    TypeKind operand_type = get_expr_type_kind(ast->u.unary.operand);

    switch (ast->u.unary.op) {
        case TOK_MINUS:
            if (operand_type == TYPE_INT)
                emit_byte(gen, OP_NEG_INT, ast->line);
            else if (operand_type == TYPE_FLOAT)
                emit_byte(gen, OP_NEG_FLOAT, ast->line);
            else
                emit_byte(gen, OP_NEG, ast->line);
            break;
        case TOK_NOT:    emit_byte(gen, OP_NOT, ast->line); break;
        case TOK_BITNOT: emit_byte(gen, OP_BITNOT, ast->line); break;
        default:
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未知的一元操作符");
            break;
    }
}

static void gen_variable(CodeGen* gen, Ast* ast, int can_assign) {
    (void)can_assign;
    SymRef* ref = &ast->u.var.ref;
    if (!ref->name) {
        error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未解析的变量");
        return;
    }

    switch (ref->kind) {
        case SYM_LOCAL:
        case SYM_PARAM:
            emit_bytes_2(gen, OP_GET_LOCAL, ref->index, ast->line);
            break;
        case SYM_GLOBAL:
            emit_get_global(gen, ref->index, ast->line);
            break;
        case SYM_GLOBAL_FUNC:
            emit_get_global_func(gen, ref->index, ast->line);
            break;
        case SYM_UPVALUE:
            emit_bytes_2(gen, OP_GET_UPVALUE, ref->index, ast->line);
            break;
        case SYM_NATIVE:
            {
                ObjString* nameStr = str_copy(ref->name, (int)strlen(ref->name));
                int constant = make_constant(gen, val_obj((Object*)nameStr));
                emit_native(gen, constant, ast->line);
            }
            break;
        case SYM_MODULE:
            // 模块级别的变量或函数，根据类型区分
            if (ref->type_kind == TYPE_FUNCTION) {
                emit_bytes_2(gen, OP_GET_MODULE_FUNC, ref->index, ast->line);
            } else {
                emit_bytes_2(gen, OP_GET_MODULE_VAR, ref->index, ast->line);
            }
            break;
        case SYM_TYPE:
        case SYM_STRUCT:
        case SYM_ENUM:
            // 类型定义，生成类型名称常量
            // 注意：类型定义在运行时不占用变量槽位
            // 类型名称会被运行时用于查找类型定义
            {
                ObjString* type_name = str_copy(ref->name, strlen(ref->name));
                int name_const = make_constant(gen, val_obj((Object*)type_name));
                emit_byte(gen, OP_CONST, ast->line);
                emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, name_const & 0xff, ast->line);
            }
            break;
        case SYM_CSTRUCT:
            // cstruct 类型定义，生成代码获取 cstruct 定义对象
            // 使用 OP_GET_CSTRUCT_DEF 操作码在运行时查找 cstruct 定义
            {
                ObjString* type_name = str_copy(ref->name, strlen(ref->name));
                int name_const = make_constant(gen, val_obj((Object*)type_name));
                emit_byte(gen, OP_GET_CSTRUCT_DEF, ast->line);
                emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, name_const & 0xff, ast->line);
            }
            break;
        default:
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未知的符号类型");
            return;
    }
}

// 生成默认参数值
static void gen_default_value(CodeGen* gen, Ast* default_expr) {
    if (!default_expr) return;
    
    switch (default_expr->kind) {
        case AST_NUM:
            if (default_expr->u.num.is_float) {
                emit_constant(gen, val_float(default_expr->u.num.value), default_expr->line);
            } else {
                emit_constant(gen, val_num(default_expr->u.num.value), default_expr->line);
            }
            break;
        case AST_STRING: {
            ObjString* str = str_copy(default_expr->u.string.value, default_expr->u.string.len);
            emit_constant(gen, val_obj((Object*)str), default_expr->line);
            break;
        }
        case AST_BOOL:
            emit_byte(gen, default_expr->u.boolean ? OP_TRUE : OP_FALSE, default_expr->line);
            break;
        case AST_NULL:
            emit_byte(gen, OP_NULL, default_expr->line);
            break;
        default:
            // 非字面量表达式（如全局变量引用、enum 常量、常量表达式）
            // 直接生成表达式代码
            gen_expr(gen, default_expr);
            break;
    }
}

// 从文本生成默认参数值（用于跨模块函数调用的默认参数填充）
// 支持的字面量格式: "10", "3.14", "\"hi\"", "true", "false", "null", "-1"
static void gen_default_value_from_text(CodeGen* gen, const char* text, int line) {
    if (!text || !*text) {
        emit_byte(gen, OP_NULL, line);
        return;
    }

    // 跳过前导空白
    while (*text && isspace((unsigned char)*text)) text++;
    int len = (int)strlen(text);
    // 去除尾部空白
    while (len > 0 && isspace((unsigned char)text[len - 1])) len--;

    if (len == 0) {
        emit_byte(gen, OP_NULL, line);
        return;
    }

    // null
    if (len == 4 && strncmp(text, "null", 4) == 0) {
        emit_byte(gen, OP_NULL, line);
        return;
    }
    // true
    if (len == 4 && strncmp(text, "true", 4) == 0) {
        emit_byte(gen, OP_TRUE, line);
        return;
    }
    // false
    if (len == 5 && strncmp(text, "false", 5) == 0) {
        emit_byte(gen, OP_FALSE, line);
        return;
    }
    // 字符串字面量: "..."（文本中包含引号）
    if (text[0] == '"') {
        // 提取引号内的内容
        const char* content_start = text + 1;
        const char* content_end = text + len - 1;
        if (content_end > content_start && *content_end == '"') {
            // 处理转义字符
            int raw_len = (int)(content_end - content_start);
            char* buf = (char*)malloc(raw_len + 1);
            int bi = 0;
            for (int i = 0; i < raw_len; i++) {
                if (content_start[i] == '\\' && i + 1 < raw_len) {
                    char next = content_start[i + 1];
                    switch (next) {
                        case 'n': buf[bi++] = '\n'; break;
                        case 't': buf[bi++] = '\t'; break;
                        case 'r': buf[bi++] = '\r'; break;
                        case '\\': buf[bi++] = '\\'; break;
                        case '"': buf[bi++] = '"'; break;
                        case '\'': buf[bi++] = '\''; break;
                        case '0': buf[bi++] = '\0'; break;
                        default: buf[bi++] = next; break;
                    }
                    i++; // 跳过转义字符
                } else {
                    buf[bi++] = content_start[i];
                }
            }
            buf[bi] = '\0';
            ObjString* str = str_copy(buf, bi);
            free(buf);
            emit_constant(gen, val_obj((Object*)str), line);
            return;
        }
    }
    // 数值字面量（含负号）
    {
        const char* num_start = text;
        if (text[0] == '-' || text[0] == '+') {
            num_start = text + 1;
        }
        // 检查是否是纯数字（含小数点）
        int has_dot = 0;
        int is_numeric = 1;
        int num_len = len - (int)(num_start - text);
        for (int i = 0; i < num_len; i++) {
            char c = num_start[i];
            if (c == '.') { has_dot = 1; continue; }
            if (c < '0' || c > '9') { is_numeric = 0; break; }
        }
        if (is_numeric && num_len > 0) {
            if (has_dot) {
                double val = atof(text);
                emit_constant(gen, val_float(val), line);
            } else {
                long long val = atoll(text);
                emit_constant(gen, val_num((int64_t)val), line);
            }
            return;
        }
    }

    // 无法解析，回退为 null
    emit_byte(gen, OP_NULL, line);
}

static void gen_call(CodeGen* gen, Ast* ast) {
    // 检测 clib 调用：lib.func(args) 或 expr.method(args)
    // 生成 ffi.call_xxx 模块调用
    Ast* clib_obj_ast = NULL;
    const char* clib_func_name = NULL;

    if (ast->u.call.callee && ast->u.call.callee->kind == AST_FIELD_ACCESS) {
        Ast* field_access = ast->u.call.callee;
        clib_obj_ast = field_access->u.field_access.obj;
        clib_func_name = field_access->u.field_access.field_name;
    } else if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
        Ast* index_ast = ast->u.call.callee;
        if (index_ast->u.index.index && index_ast->u.index.index->kind == AST_STRING) {
            clib_obj_ast = index_ast->u.index.obj;
            clib_func_name = index_ast->u.index.index->u.string.value;
        }
    }

    if (clib_obj_ast && clib_func_name) {
        TypeInfo* obj_type = infer_expr_type(gen->sem, clib_obj_ast);
        if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
            // 查找 clib 符号
            Symbol* clib_sym = scope_resolve(gen->sem->root_scope, obj_type->struct_name);
            if (!clib_sym) {
                clib_sym = scope_resolve(gen->sem->current, obj_type->struct_name);
            }
            if (clib_sym) {
                int is_imported_clib = (clib_sym->clib_func_count == 0);
                if (is_imported_clib) {
                    // 导入的 clib：生成 OP_CLIB_CALL（默认类型）
                    gen_expr(gen, clib_obj_ast);

                    ObjString* func_name_str = str_copy(clib_func_name, (int)strlen(clib_func_name));
                    emit_constant(gen, val_obj((Object*)func_name_str), ast->line);

                    for (int j = 0; j < ast->u.call.args.count; j++) {
                        gen_expr(gen, ast->u.call.args.items[j]);
                    }

                    int total_arg_count = 2 + ast->u.call.args.count;
                    int user_arg_count = ast->u.call.args.count;

                    emit_byte(gen, OP_CLIB_CALL, ast->line);
                    emit_byte(gen, (total_arg_count >> 8) & 0xff, ast->line);
                    emit_byte(gen, total_arg_count & 0xff, ast->line);
                    emit_byte(gen, (uint8_t)TYPE_I32, ast->line);
                    emit_byte(gen, (uint8_t)user_arg_count, ast->line);
                    for (int ai = 0; ai < user_arg_count; ai++) {
                        emit_byte(gen, (uint8_t)TYPE_I32, ast->line);
                    }

                    type_free(obj_type);
                    return;
                }

                const char* func_name = clib_func_name;
                for (int i = 0; i < clib_sym->clib_func_count; i++) {
                    if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                        // 获取返回类型
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];

                        // 生成参数：先评估库对象
                        gen_expr(gen, clib_obj_ast);

                        // 生成函数名字符串参数（推入栈）
                        ObjString* func_name_str = str_copy(func_name, (int)strlen(func_name));
                        emit_constant(gen, val_obj((Object*)func_name_str), ast->line);

                        // 生成用户参数
                        for (int j = 0; j < ast->u.call.args.count; j++) {
                            gen_expr(gen, ast->u.call.args.items[j]);
                        }

                        // 总参数数 = 库对象 + 函数名 + 用户参数
                        int total_arg_count = 2 + ast->u.call.args.count;
                        int user_arg_count = ast->u.call.args.count;

                        // 使用 OP_CLIB_CALL 统一处理（支持 str16 自动转换）
                        // 返回类型直接编码 TypeKind，VM 端根据 TypeKind 做自动展开
                        int ret_type_kind = ret_type ? ret_type->kind : TYPE_NULL;

                        // OP_CLIB_CALL arg_count(2) ret_type_kind(1) user_arg_count(1) arg_types[user_arg_count](1 each)
                        emit_byte(gen, OP_CLIB_CALL, ast->line);
                        emit_byte(gen, (total_arg_count >> 8) & 0xff, ast->line);
                        emit_byte(gen, total_arg_count & 0xff, ast->line);
                        emit_byte(gen, (uint8_t)ret_type_kind, ast->line);
                        emit_byte(gen, (uint8_t)user_arg_count, ast->line);
                        // 编码每个用户参数的类型（TypeKind 枚举值）
                        for (int ai = 0; ai < user_arg_count; ai++) {
                            TypeInfo* param_type = clib_sym->clib_func_param_types[i][ai];
                            int type_kind = param_type ? param_type->kind : 0;
                            emit_byte(gen, (uint8_t)type_kind, ast->line);
                        }

                        type_free(obj_type);
                        return;
                    }
                }
            }
        }
        if (obj_type) type_free(obj_type);
    }

    // 检测 arr.add(x) 模式并优化为 OP_ARRAY_APPEND
    // 注意：需要排除 struct 方法调用（如 self["add"]()）
    if (ast->u.call.callee->kind == AST_INDEX &&
        ast->u.call.callee->u.index.index->kind == AST_STRING &&
        ast->u.call.args.count == 1 &&
        strcmp(ast->u.call.callee->u.index.index->u.string.value, "add") == 0) {
        // 检查 receiver 类型，排除 struct 类型
        TypeInfo* receiver_type = infer_expr_type(gen->sem, ast->u.call.callee->u.index.obj);
        int is_array_type = (receiver_type && receiver_type->kind == TYPE_ARRAY);
        if (receiver_type) type_free(receiver_type);
        
        if (is_array_type) {
            // 使用公共函数生成数组 add 操作（表达式上下文，需要返回值）
            gen_array_add(gen, ast->u.call.callee->u.index.obj, ast->u.call.args.items[0], 1, ast->line);
            return;
        }
    }

    // 检测 dict.set(key, value) 模式并优化为 OP_DICT_SET
    // 注意：struct 方法调用时 self 会被加入 args，导致 args.count==2，需要排除 struct/face receiver
    if (ast->u.call.callee->kind == AST_INDEX &&
        ast->u.call.callee->u.index.index->kind == AST_STRING &&
        ast->u.call.args.count == 2 &&
        strcmp(ast->u.call.callee->u.index.index->u.string.value, "set") == 0) {
        // 检查 receiver 是否是 struct/face，如果是则跳过 dict.set 优化
        TypeInfo* receiver_type = infer_expr_type(gen->sem, ast->u.call.callee->u.index.obj);
        int is_struct_or_face = (receiver_type && (receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_FACE || receiver_type->kind == TYPE_CSTRUCT));
        if (receiver_type) type_free(receiver_type);

        if (!is_struct_or_face) {
            // 生成字典对象
            gen_expr(gen, ast->u.call.callee->u.index.obj);
            // 生成键参数
            gen_expr(gen, ast->u.call.args.items[0]);
            // 生成值参数
            gen_expr(gen, ast->u.call.args.items[1]);
            // 使用专用字节码
            emit_byte(gen, OP_DICT_SET, ast->line);
            return;
        }
    }

    // 检测 struct 方法调用: obj.method(args) 或 obj["method"](args)
    // 使用 OP_GET_METHOD 从 struct 方法表获取方法函数
    // 支持 AST_INDEX (obj["method"]()) 和 AST_FIELD_ACCESS (obj.method()) 两种语法
    const char* method_name = NULL;
    Ast* obj_ast = NULL;
    if (ast->u.call.callee->kind == AST_INDEX &&
        ast->u.call.callee->u.index.index->kind == AST_STRING) {
        method_name = ast->u.call.callee->u.index.index->u.string.value;
        obj_ast = ast->u.call.callee->u.index.obj;
    } else if (ast->u.call.callee->kind == AST_FIELD_ACCESS) {
        method_name = ast->u.call.callee->u.field_access.field_name;
        obj_ast = ast->u.call.callee->u.field_access.obj;
    }
    if (method_name && obj_ast) {

        // 推断 receiver 的类型
        TypeInfo* receiver_type = infer_expr_type(gen->sem, obj_ast);
        int is_native_obj_type = 0;
        const char* native_type_name = NULL;
        if (receiver_type && (receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_FACE) && receiver_type->struct_name) {
            // struct/face 类型，已有 struct_name
        } else if (receiver_type && (receiver_type->kind == TYPE_FILE
            || receiver_type->kind == TYPE_SOCKET)) {
            // 原生对象类型（File, Socket 等）
            is_native_obj_type = 1;
            native_type_name = native_get_type_name(receiver_type->kind);
        }
        if (receiver_type && (((receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_FACE) && receiver_type->struct_name) || is_native_obj_type)) {
            char method_key[256];
            Ast* method_def = NULL;
            int provided_args = ast->u.call.args.count;
            int expected_args = 0;
            int required_args = 0;
            int has_method_def = 0;
            int is_face_call = (receiver_type->kind == TYPE_FACE);

            int is_native_method = 0;
            int native_arity = 0;

            if (is_native_obj_type) {
                // 原生对象类型：直接从原生方法元信息获取
                TypeKind return_type = native_get_instance_method_return_type(native_type_name, method_name, &native_arity);
                if (return_type != TYPE_ANY && native_arity >= 0) {
                    is_native_method = 1;
                    expected_args = native_arity;  // 不包含 self
                    required_args = expected_args;
                    has_method_def = 1;
                } else {
                    expected_args = provided_args + (obj_ast->kind == AST_VAR ? 0 : 1);
                    required_args = expected_args;
                }
            } else if (!is_face_call) {
                snprintf(method_key, sizeof(method_key), "%s::%s", receiver_type->struct_name, method_name);
                method_def = func_table_find(&gen->sem->func_table, method_key);
                if (!method_def) {
                    int face_param_count = 0;
                    TypeKind face_return_type = TYPE_ANY;
                    if (struct_def_has_face_method(receiver_type->struct_name, method_name, &face_param_count, &face_return_type)) {
                        expected_args = face_param_count + 1;
                        required_args = expected_args;
                        has_method_def = 1;
                    }
                    if (!has_method_def) {
                        for (int mi = 0; mi < gen->sem->imported_module_count && !has_method_def; mi++) {
                            ImportedModuleInfo* m = &gen->sem->imported_modules[mi];
                            if (m && m->sym_table) {
                                ObjStructDef* sd = struct_def_find(receiver_type->struct_name);
                                if (sd && sd->impl_count > 0) {
                                    for (int ii = 0; ii < sd->impl_count && !has_method_def; ii++) {
                                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, sd->impl_names[ii]);
                                        if (face_sym) {
                                            for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                    expected_args = face_sym->methods[fi].param_count + 1;
                                                    required_args = expected_args;
                                                    has_method_def = 1;
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                ObjFaceDef* fdef = face_def_find(receiver_type->struct_name);
                if (fdef) {
                    for (int fi = 0; fi < fdef->method_count; fi++) {
                        if (strcmp(fdef->methods[fi].name, method_name) == 0) {
                            expected_args = fdef->methods[fi].param_count + 1;
                            required_args = expected_args;
                            has_method_def = 1;
                            break;
                        }
                    }
                }
            }
            
            if (!is_native_obj_type && method_def && method_def->kind == AST_FUNC_DEF) {
                expected_args = method_def->u.func.pcnt;
                required_args = expected_args - method_def->u.func.default_count;
                has_method_def = 1;
            } else if (!is_native_obj_type && !has_method_def) {
                // 方法不在全局函数表中，尝试从原生方法元信息获取参数数量
                TypeKind return_type = native_get_instance_method_return_type("struct", method_name, &native_arity);
                if (return_type != TYPE_ANY && native_arity >= 0) {
                    // 原生方法（如 copy），native_arity 不包含 self
                    is_native_method = 1;
                    expected_args = native_arity;  // 不包含 self，因为 BoundMethod 已经包含 receiver
                    required_args = expected_args;
                } else {
                    // 检查是否是函数类型字段（如 func(int,int):int op）
                    int is_func_field = 0;
                    if (receiver_type->struct_name && (receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_CSTRUCT)) {
                        Symbol* struct_sym = scope_resolve(gen->sem->current, receiver_type->struct_name);
                        if (struct_sym && struct_sym->struct_field_names && struct_sym->struct_field_types) {
                            for (int fi = 0; fi < struct_sym->struct_field_count; fi++) {
                                if (strcmp(struct_sym->struct_field_names[fi], method_name) == 0 &&
                                    struct_sym->struct_field_types[fi]->kind == TYPE_FUNCTION) {
                                    is_func_field = 1;
                                    break;
                                }
                            }
                        }
                    }
                    if (is_func_field) {
                        // 函数类型字段：生成"获取字段值 + 调用"
                        // 先压入参数
                        for (int i = 0; i < ast->u.call.args.count; i++) {
                            gen_expr(gen, ast->u.call.args.items[i]);
                        }
                        // 获取字段值：生成 obj["field_name"]
                        gen_expr(gen, obj_ast);
                        ObjString* field_name_str = str_copy(method_name, strlen(method_name));
                        int field_name_const = make_constant(gen, val_obj((Object*)field_name_str));
                        emit_byte(gen, OP_CONST, ast->line);
                        emit_byte(gen, (field_name_const >> 8) & 0xff, ast->line);
                        emit_byte(gen, field_name_const & 0xff, ast->line);
                        emit_byte(gen, OP_INDEX, ast->line);
                        emit_call(gen, ast->u.call.args.count, ast->line);
                        return;
                    }
                    expected_args = provided_args + (obj_ast->kind == AST_VAR ? 0 : 1);
                    required_args = expected_args;
                }
            }

            // 检查参数数量
            // 语义分析（visit_module.inc）将 MODULE_CALL 转换为 CALL 时，
            // 已将 receiver 作为 args[0] 加入，此时 args[0] 与 obj_ast 指代同一变量
            // 对于方法体内部调用 self.method(args)（transform_method_body），self 也作为 args[0] 加入
            // 所以 self_in_args 判断：args[0] 是 AST_VAR 且名字与 obj_ast 相同（即 receiver 已在 args 中）
            int self_in_args = 0;
            if (ast->u.call.args.count > 0 &&
                ast->u.call.args.items[0]->kind == AST_VAR &&
                ast->u.call.args.items[0]->u.var.name &&
                obj_ast->kind == AST_VAR &&
                obj_ast->u.var.name &&
                strcmp(ast->u.call.args.items[0]->u.var.name, obj_ast->u.var.name) == 0) {
                self_in_args = 1;
            }
            int actual_provided = provided_args + (self_in_args ? 0 : 1);  // self_in_args 时已包含 self，否则需要 +1
            // 原生方法的 expected_args 不包含 self，跳过 codegen 层的参数校验（语义分析已校验）
            // 自定义方法的 expected_args 包含 self，需要校验
            if (!is_native_method && !is_native_obj_type && has_method_def && actual_provided < required_args) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数不足: 至少需要 %d 个参数，实际传入 %d 个",
                         method_name, required_args - 1, provided_args - (self_in_args ? 1 : 0));
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                return;
            }

            if (!is_native_method && !is_native_obj_type && has_method_def && actual_provided > expected_args) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数过多: 最多接受 %d 个参数，实际传入 %d 个",
                         method_name, expected_args - 1, provided_args - (self_in_args ? 1 : 0));
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                return;
            }

            if (is_native_method || is_native_obj_type) {
                // 原生方法：先压入参数，再压入 receiver，OP_GET_METHOD 会创建 BoundMethod（包含 receiver）
                for (int i = 0; i < ast->u.call.args.count; i++) {
                    gen_expr(gen, ast->u.call.args.items[i]);
                }
                gen_expr(gen, obj_ast);

                // 生成 OP_GET_METHOD 获取方法函数
                ObjString* method_name_str = str_copy(method_name, strlen(method_name));
                int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));
                emit_byte(gen, OP_GET_METHOD, ast->line);
                emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, method_name_const & 0xff, ast->line);

                // 原生方法的 expected_args 不包含 self
                emit_call(gen, expected_args, ast->line);
            } else {
                if (is_face_call) {
                    // face 动态派发调用：f.method(args)
                    // 栈布局：[self][args...] [self_for_get_method] -> OP_GET_METHOD -> [self][args...] [closure] -> OP_CALL
                    // 当 self_in_args 时语义分析已将 self 加入 args，否则需额外压入
                    if (!self_in_args) {
                        gen_expr(gen, obj_ast);
                    }
                    for (int i = 0; i < ast->u.call.args.count; i++) {
                        gen_expr(gen, ast->u.call.args.items[i]);
                    }
                    // 压入 receiver 供 OP_GET_METHOD 消费（从 struct 方法表查找方法）
                    gen_expr(gen, obj_ast);

                    ObjString* method_name_str = str_copy(method_name, strlen(method_name));
                    int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));
                    emit_byte(gen, OP_GET_METHOD, ast->line);
                    emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, method_name_const & 0xff, ast->line);

                    int call_arg_count = provided_args + (self_in_args ? 0 : 1);
                    if (has_method_def) {
                        call_arg_count = expected_args;
                    }
                    emit_call(gen, call_arg_count, ast->line);
                } else {
                    // struct 方法调用：s.method(args)
                    // 栈布局目标: [self][arg1]...[argN][default_args...][callee]
                    // 当 self_in_args 时语义分析已将 self 作为 args[0] 加入，否则需额外压入 self
                    // OP_CALL 从 vm.sp - arg_count - 1 开始读参数，跳过栈顶的 callee
                    if (!self_in_args) {
                        gen_expr(gen, obj_ast);
                    }
                    for (int i = 0; i < ast->u.call.args.count; i++) {
                        gen_expr(gen, ast->u.call.args.items[i]);
                    }

                    // 在压入 callee 之前填充缺失的默认参数
                    // actual_provided 已包含 self（无论是否在 args 中），直接与 expected_args 比较
                    if (has_method_def && method_def && actual_provided < expected_args) {
                        for (int i = actual_provided; i < expected_args; i++) {
                            Ast* default_expr = method_def->u.func.param_defaults[i];
                            if (default_expr) {
                                gen_default_value(gen, default_expr);
                            } else {
                                emit_byte(gen, OP_NULL, ast->line);
                            }
                        }
                    }

                    // 查找/调用方法
                    ObjString* method_name_str = str_copy(method_name, strlen(method_name));
                    int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));

                    if (has_method_def && method_def && !method_def->u.func.is_async) {
                        // 融合指令：receiver 只求值一次（避免 spheres[si] 二次求值），
                        // 方法查找 + 调用一条指令完成（内联缓存在指令内）
                        // 栈布局: [self][args...][defaults...] -> OP_INVOKE_METHOD -> 结果
                        emit_byte(gen, OP_INVOKE_METHOD, ast->line);
                        emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                        emit_byte(gen, method_name_const & 0xff, ast->line);
                        emit_byte(gen, (expected_args >> 8) & 0xff, ast->line);
                        emit_byte(gen, expected_args & 0xff, ast->line);
                    } else {
                        // async 方法或未知方法定义：走旧路径
                        // 压入 receiver，OP_GET_METHOD 获取方法闭包压入栈顶
                        gen_expr(gen, obj_ast);

                        emit_byte(gen, OP_GET_METHOD, ast->line);
                        emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                        emit_byte(gen, method_name_const & 0xff, ast->line);

                        if (has_method_def && method_def && method_def->u.func.is_async) {
                            emit_byte(gen, OP_ASYNC_CALL, ast->line);
                            emit_byte(gen, (expected_args >> 8) & 0xff, ast->line);
                            emit_byte(gen, expected_args & 0xff, ast->line);
                        } else {
                            emit_call(gen, expected_args, ast->line);
                        }
                    }
                }
            }
            return;
        }
    }

    // ========== 默认参数处理 ==========
    Ast* func_def = NULL;
    const char* func_name = NULL;

    // 尝试获取函数名和定义
    if (ast->u.call.callee->kind == AST_VAR) {
        func_name = ast->u.call.callee->u.var.name;
        // 从函数表中查找函数定义
        func_def = func_table_find(&gen->sem->func_table, func_name);
    }

    int provided_args = ast->u.call.args.count;
    int expected_args = 0;
    int required_args = 0;  // 必选参数数量

    if (func_def && func_def->kind == AST_FUNC_DEF) {
        expected_args = func_def->u.func.pcnt;
        required_args = expected_args - func_def->u.func.default_count;

        // 检查是否是 struct 方法调用（第一个参数是 self）
        int is_struct_method_call = 0;
        if (func_def->u.func.pcnt > 0 && strcmp(func_def->u.func.params[0], "self") == 0) {
            is_struct_method_call = 1;
        }

        // 3.2 检查参数数量
        if (provided_args < required_args) {
            char msg[BUFFER_MEDIUM];
            if (is_struct_method_call) {
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数不足: 至少需要 %d 个参数，实际传入 %d 个",
                         func_name, required_args - 1, provided_args - 1);
            } else {
                snprintf(msg, sizeof(msg), "函数 '%s' 调用参数不足: 至少需要 %d 个参数，实际传入 %d 个",
                         func_name, required_args, provided_args);
            }
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            return;
        }

        if (provided_args > expected_args) {
            char msg[BUFFER_MEDIUM];
            if (is_struct_method_call) {
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数过多: 最多接受 %d 个参数，实际传入 %d 个",
                         func_name, expected_args - 1, provided_args - 1);
            } else {
                snprintf(msg, sizeof(msg), "函数 '%s' 调用参数过多: 最多接受 %d 个参数，实际传入 %d 个",
                         func_name, expected_args, provided_args);
            }
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            return;
        }
    }

    // 生成提供的参数
    for (int i = 0; i < ast->u.call.args.count; i++) {
        gen_expr(gen, ast->u.call.args.items[i]);
    }

    // 3.1 填充缺失的默认参数
    if (func_def && func_def->kind == AST_FUNC_DEF && provided_args < expected_args) {
        for (int i = provided_args; i < expected_args; i++) {
            Ast* default_expr = func_def->u.func.param_defaults[i];
            if (default_expr) {
                gen_default_value(gen, default_expr);
            } else {
                // 不应发生：语义分析已确保默认值存在
                emit_byte(gen, OP_NULL, ast->line);
            }
        }
    }

    // 检测原生函数调用，合并为 OP_CALL_NATIVE
    if (ast->u.call.callee->kind == AST_VAR && !func_def) {
        Symbol* callee_sym = scope_resolve(gen->sem->current, ast->u.call.callee->u.var.name);
        if (callee_sym && callee_sym->kind == SYM_NATIVE) {
            ObjString* nameStr = str_copy(callee_sym->name, (int)strlen(callee_sym->name));
            int name_const = make_constant(gen, val_obj((Object*)nameStr));
            int total_args = ast->u.call.args.count;
            emit_call_native(gen, name_const, total_args, ast->line);
            return;
        }
    }

    // 使用完整的参数数量（包括默认值）
    int total_args = func_def ? expected_args : ast->u.call.args.count;

    // 检测全局函数调用，融合为 OP_CALL_GLOBAL_FUNC（省掉 OP_GET_GLOBAL_FUNC + OP_CALL）
    // 条件：callee 是简单变量、是全局函数、非 async、无泛型类型参数
    if (ast->u.call.callee->kind == AST_VAR) {
        Symbol* callee_sym = scope_resolve(gen->sem->current, ast->u.call.callee->u.var.name);
        if (callee_sym && callee_sym->kind == SYM_GLOBAL_FUNC
            && !ast->u.call.callee_is_async
            && ast->u.call.generic_type_count == 0) {

            // 尝试函数内联：小函数体直接嵌入调用点
            if (func_def && try_inline_call(gen, ast, func_def)) {
                return;
            }

            emit_byte(gen, OP_CALL_GLOBAL_FUNC, ast->line);
            emit_byte(gen, (callee_sym->index >> 8) & 0xff, ast->line);
            emit_byte(gen, callee_sym->index & 0xff, ast->line);
            emit_byte(gen, (total_args >> 8) & 0xff, ast->line);
            emit_byte(gen, total_args & 0xff, ast->line);
            return;
        }
    }

    gen_expr(gen, ast->u.call.callee);

    // 检查是否是 async 函数调用
    // 优先查 func_table，其次用语义分析标记的 callee_is_async
    int is_async_call = (func_def && func_def->kind == AST_FUNC_DEF && func_def->u.func.is_async)
                      || ast->u.call.callee_is_async;
    if (is_async_call) {
        // async 函数使用 OP_ASYNC_CALL
        emit_byte(gen, OP_ASYNC_CALL, ast->line);
        emit_byte(gen, (total_args >> 8) & 0xff, ast->line);
        emit_byte(gen, total_args & 0xff, ast->line);
    } else {
        // 泛型类型参数传递：在 OP_CALL 前发射 OP_PUSH_TYPE_ARGS
        int gt_count = ast->u.call.generic_type_count;
        if (gt_count > 0 && ast->u.call.generic_type_args) {
            emit_byte(gen, OP_PUSH_TYPE_ARGS, ast->line);
            emit_byte(gen, (uint8_t)gt_count, ast->line);
            for (int gi = 0; gi < gt_count; gi++) {
                const char* type_str = type_kind_to_string(ast->u.call.generic_type_args[gi]->kind);
                int type_const = make_constant(gen, val_obj((Object*)str_copy(type_str, (int)strlen(type_str))));
                emit_byte(gen, (type_const >> 8) & 0xff, ast->line);
                emit_byte(gen, type_const & 0xff, ast->line);
            }
        }
        // 普通函数使用 OP_CALL
        emit_call(gen, total_args, ast->line);
    }
}

void gen_expr(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_NUM:
            if (ast->u.num.is_bigint && ast->u.num.bigint_str) {
                if (bigint_str_fits_in_int32(ast->u.num.bigint_str)) {
                    // 使用 strtoll 并指定基数 0，自动检测十六进制 (0x 前缀)
                    long long val = strtoll(ast->u.num.bigint_str, NULL, 0);
                    emit_constant(gen, val_int_safe(val), ast->line);
                } else {
                    emit_constant(gen, val_bigint_from_string(ast->u.num.bigint_str), ast->line);
                }
            } else if (ast->u.num.is_float) {
                emit_constant(gen, val_float(ast->u.num.value), ast->line);
            } else {
                // 使用 val_int_safe 处理超出 32 位 int 范围的整数值
                emit_constant(gen, val_int_safe((int64_t)ast->u.num.value), ast->line);
            }
            break;
        case AST_STRING: {
            ObjString* str = str_copy(ast->u.string.value, ast->u.string.len);
            emit_constant(gen, val_obj((Object*)str), ast->line);
            break;
        }
        case AST_INTERP_STRING: {
            int count = ast->u.interp_string.count;

            if (count > 0 && ast->u.interp_string.parts[0] && strlen(ast->u.interp_string.parts[0]) > 0) {
                ObjString* str = str_copy(ast->u.interp_string.parts[0], strlen(ast->u.interp_string.parts[0]));
                emit_constant(gen, val_obj((Object*)str), ast->line);
            } else {
                ObjString* str = str_copy("", 0);
                emit_constant(gen, val_obj((Object*)str), ast->line);
            }

            for (int i = 0; i < count - 1; i++) {
                gen_expr(gen, ast->u.interp_string.exprs[i]);

                emit_byte(gen, OP_STRING_ADD, ast->line);

                if (i + 1 < count && ast->u.interp_string.parts[i + 1] && strlen(ast->u.interp_string.parts[i + 1]) > 0) {
                    ObjString* str = str_copy(ast->u.interp_string.parts[i + 1], strlen(ast->u.interp_string.parts[i + 1]));
                    emit_constant(gen, val_obj((Object*)str), ast->line);
                    emit_byte(gen, OP_STRING_ADD, ast->line);
                }
            }
            break;
        }
        case AST_BOOL:
            emit_byte(gen, ast->u.boolean ? OP_TRUE : OP_FALSE, ast->line);
            break;
        case AST_NULL:
            emit_byte(gen, OP_NULL, ast->line);
            break;
        case AST_ARRAY: {
            for (int i = 0; i < ast->u.array.count; i++) {
                gen_expr(gen, ast->u.array.items[i]);
            }
            emit_byte(gen, OP_ARRAY, ast->line);
            emit_byte(gen, (ast->u.array.count >> 8) & 0xff, ast->line);
            emit_byte(gen, ast->u.array.count & 0xff, ast->line);
            break;
        }
        case AST_DICT: {
            // 检查是否有任何 value 包含函数调用（包括普通调用和模块调用）
            int has_call = 0;
            for (int i = 0; i < ast->u.dict.count; i++) {
                Ast* value = ast->u.dict.entries[i].value;
                if (value->kind == AST_CALL ||
                    value->kind == AST_MODULE_CALL ||
                    (value->kind == AST_INDEX && value->u.index.obj->kind == AST_CALL)) {
                    has_call = 1;
                    break;
                }
            }
            
            if (has_call && ast->u.dict.count > 1) {
                // 如果有函数调用且字典有多个条目，使用临时变量策略
                // 先计算所有 value 并保存到临时变量，然后再组装字典
                
                // 计算需要的临时槽位数（每个 value 一个槽位）
                int count = ast->u.dict.count;
                
                // 查找当前最大局部变量索引
                int max_local_index = -1;
                if (gen->current_func) {
                    for (int j = 0; j < gen->current_func->local_count; j++) {
                        if (j > max_local_index) max_local_index = j;
                    }
                }
                int temp_slot_base = max_local_index + 1;
                int max_temp_slot = temp_slot_base + count - 1;
                if (max_temp_slot > gen->max_local_slot) {
                    gen->max_local_slot = max_temp_slot;
                }
                
                // 计算所有 value 并保存到临时变量（使用 OP_SET_LOCAL_POP 优化）
                for (int i = 0; i < count; i++) {
                    gen_expr(gen, ast->u.dict.entries[i].value);
                    emit_bytes_2(gen, OP_SET_LOCAL_POP, temp_slot_base + i, ast->line);
                }
                
                // 按顺序生成 key-value 对（从临时变量加载 value）
                for (int i = 0; i < count; i++) {
                    gen_expr(gen, ast->u.dict.entries[i].key);
                    emit_bytes_2(gen, OP_GET_LOCAL, temp_slot_base + i, ast->line);
                }
            } else {
                // 简单情况：直接生成 key-value 对
                for (int i = 0; i < ast->u.dict.count; i++) {
                    gen_expr(gen, ast->u.dict.entries[i].key);
                    gen_expr(gen, ast->u.dict.entries[i].value);
                }
            }
            
            emit_byte(gen, OP_DICT, ast->line);
            emit_byte(gen, (ast->u.dict.count >> 8) & 0xff, ast->line);
            emit_byte(gen, ast->u.dict.count & 0xff, ast->line);
            break;
        }
        case AST_RANGE: {
            gen_expr(gen, ast->u.range.start);
            gen_expr(gen, ast->u.range.end);
            emit_byte(gen, OP_RANGE, ast->line);
            emit_byte(gen, ast->u.range.inclusive, ast->line);
            break;
        }
        case AST_VAR:
            gen_variable(gen, ast, 0);
            break;
        case AST_BINOP:
            gen_binary(gen, ast);
            break;
        case AST_UNARY:
            gen_unary(gen, ast);
            break;
        case AST_CALL:
            gen_call(gen, ast);
            // 多返回值函数在非解构上下文中：只保留第一个返回值，弹出多余的
            // 栈布局: [ret0][ret1]...[retN-1]（retN-1 在栈顶）
            // 需要弹出 retN-1 到 ret1，只留 ret0 在栈顶
            if (!gen->suppress_multi_pop &&
                ast->cached_type && ast->cached_type->kind == TYPE_MULTI_RET &&
                ast->cached_type->param_count > 1) {
                int extra = ast->cached_type->param_count - 1;
                for (int i = 0; i < extra; i++) {
                    emit_byte(gen, OP_POP, ast->line);
                }
            }
            break;
        case AST_INDEX: {
            gen_expr(gen, ast->u.index.obj);
            // 属性访问：obj.name 使用 OP_GET_PROPERTY 获取方法或属性
            if (ast->u.index.index->kind == AST_STRING) {
                // 检查对象类型是否是 struct、cstruct 或 enum，如果是则使用通用 OP_INDEX
                TypeInfo* obj_type = infer_expr_type(gen->sem, ast->u.index.obj);
                // 检查对象是否是模块访问（如 color_module.Color），这种情况下也使用 OP_INDEX
                int is_module_access = (ast->u.index.obj->kind == AST_MODULE_ACCESS);
                if ((obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_ENUM)) || is_module_access) {
                    // struct/cstruct 字段访问、enum 成员访问或模块访问使用通用索引
                    gen_expr(gen, ast->u.index.index);
                    emit_byte(gen, OP_INDEX, ast->line);
                } else {
                    ObjString* prop_name = str_copy(ast->u.index.index->u.string.value,
                                                    ast->u.index.index->u.string.len);
                    int const_idx = make_constant(gen, val_obj((Object*)prop_name));
                    emit_byte(gen, OP_GET_PROPERTY, ast->line);
                    emit_byte(gen, (const_idx >> 8) & 0xff, ast->line);
                    emit_byte(gen, const_idx & 0xff, ast->line);
                }
            } else {
                gen_expr(gen, ast->u.index.index);
                emit_byte(gen, OP_INDEX, ast->line);
            }
            break;
        }
        case AST_SLICE: {
            // 生成数组对象
            gen_expr(gen, ast->u.slice.obj);
            
            // 生成起始索引（如果为NULL则压入null）
            if (ast->u.slice.start) {
                gen_expr(gen, ast->u.slice.start);
            } else {
                emit_byte(gen, OP_NULL, ast->line);
            }
            
            // 生成结束索引（如果为NULL则压入null）
            if (ast->u.slice.end) {
                gen_expr(gen, ast->u.slice.end);
            } else {
                emit_byte(gen, OP_NULL, ast->line);
            }
            
            emit_byte(gen, OP_SLICE, ast->line);
            break;
        }
        case AST_INDEX_ASSIGN: {
            Ast* obj = ast->u.index_assign.obj;
            Ast* index = ast->u.index_assign.index;
            Ast* value = ast->u.index_assign.value;
            
            if (obj->cached_type && obj->cached_type->kind == TYPE_STRUCT &&
                index->kind == AST_STRING) {
                // OP_SET_FIELD 期望栈: [obj, value] (value 在栈顶，先被pop)
                gen_expr(gen, obj);
                gen_expr(gen, value);
                
                // 使用编译期确定的字段索引（优化：避免运行时线性搜索）
                int field_idx = ast->u.index_assign.field_index;
                
                if (field_idx < 0) {
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, "无法确定字段索引，struct 类型可能未定义");
                    field_idx = 0; // 使用 0 作为默认值，避免生成无效字节码
                }
                
                // 使用 OP_SET_FIELD
                emit_byte(gen, OP_SET_FIELD, ast->line);
                emit_byte(gen, (uint8_t)field_idx, ast->line);
            } else {
                // 普通索引赋值
                gen_expr(gen, obj);
                gen_expr(gen, index);
                gen_expr(gen, value);
                emit_byte(gen, OP_INDEX_SET, ast->line);
            }
            break;
        }
        case AST_ASSIGN:
            gen_assign(gen, ast);
            break;
        case AST_COMPOUND_ASSIGN:
            gen_compound_assign(gen, ast);
            break;
        case AST_IF:
            // if 表达式：if cond then expr1 else expr2
            gen_if(gen, ast);
            break;
        case AST_MODULE_CALL: {
            // 检查是否是 clib 调用（semantic 阶段已标记 cached_type）
            // 但需要排除模块函数调用（如 ml.load() 返回 clib 类型）
            int is_module_func_call = (find_imported_module(gen->sem, ast->u.module_call.module_name) != NULL);
            if (!is_module_func_call && ast->cached_type && ast->cached_type->kind == TYPE_CLIB && ast->cached_type->struct_name) {
                // 查找 clib 符号获取函数签名
                Symbol* clib_sym = scope_resolve(gen->sem->root_scope, ast->cached_type->struct_name);
                if (!clib_sym) {
                    clib_sym = scope_resolve(gen->sem->current, ast->cached_type->struct_name);
                }
                if (clib_sym) {
                    const char* func_name = ast->u.module_call.method_name;

                    // 判断是否是导入的 clib（函数表为空）
                    int is_imported_clib = (clib_sym->clib_func_count == 0);
                    int ret_type_kind = TYPE_I32;  // 默认返回 i32

                    if (!is_imported_clib) {
                        // 本地 clib：查找函数签名
                        int found = 0;
                        for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                            if (strcmp(clib_sym->clib_func_names[fi], func_name) == 0) {
                                TypeInfo* ret_type = clib_sym->clib_func_return_types[fi];
                                ret_type_kind = ret_type ? ret_type->kind : TYPE_NULL;
                                if (ret_type_kind == TYPE_CSTRUCT) ret_type_kind = TYPE_PTR;
                                found = 1;
                                break;
                            }
                        }
                        if (!found) {
                            // 未找到函数声明，但仍然生成调用（运行时处理）
                        }
                    }

                    // 生成 lib 变量值到栈上（使用 semantic 阶段存好的 lib_ref）
                    SymRef* lib_ref = &ast->u.module_call.lib_ref;
                    switch (lib_ref->kind) {
                        case SYM_LOCAL:
                        case SYM_PARAM:
                            emit_bytes_2(gen, OP_GET_LOCAL, lib_ref->index, ast->line);
                            break;
                        case SYM_GLOBAL:
                            emit_get_global(gen, lib_ref->index, ast->line);
                            break;
                        case SYM_UPVALUE:
                            emit_bytes_2(gen, OP_GET_UPVALUE, lib_ref->index, ast->line);
                            break;
                        case SYM_MODULE:
                            emit_bytes_2(gen, OP_GET_MODULE_VAR, lib_ref->index, ast->line);
                            break;
                        default:
                            break;
                    }

                    // 生成函数名字符串
                    ObjString* func_name_str = str_copy(func_name, (int)strlen(func_name));
                    emit_constant(gen, val_obj((Object*)func_name_str), ast->line);

                    // 生成用户参数
                    for (int ai = 0; ai < ast->u.module_call.args.count; ai++) {
                        gen_expr(gen, ast->u.module_call.args.items[ai]);
                    }

                    int total_arg_count = 2 + ast->u.module_call.args.count;
                    int user_arg_count = ast->u.module_call.args.count;

                    // OP_CLIB_CALL arg_count(2) ret_type_kind(1) user_arg_count(1) arg_types[user_arg_count](1 each)
                    emit_byte(gen, OP_CLIB_CALL, ast->line);
                    emit_byte(gen, (total_arg_count >> 8) & 0xff, ast->line);
                    emit_byte(gen, total_arg_count & 0xff, ast->line);
                    emit_byte(gen, (uint8_t)ret_type_kind, ast->line);
                    emit_byte(gen, (uint8_t)user_arg_count, ast->line);

                    if (is_imported_clib) {
                        // 导入的 clib：参数类型全部编码为 TYPE_I32
                        for (int ai = 0; ai < user_arg_count; ai++) {
                            emit_byte(gen, (uint8_t)TYPE_I32, ast->line);
                        }
                    } else {
                        // 本地 clib：使用声明的参数类型
                        for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                            if (strcmp(clib_sym->clib_func_names[fi], func_name) == 0) {
                                for (int ai = 0; ai < user_arg_count && ai < clib_sym->clib_func_param_counts[fi]; ai++) {
                                    TypeInfo* param_type = clib_sym->clib_func_param_types[fi][ai];
                                    int type_kind = param_type ? param_type->kind : 0;
                                    if (type_kind == TYPE_CSTRUCT) type_kind = TYPE_PTR;
                                    emit_byte(gen, (uint8_t)type_kind, ast->line);
                                }
                                break;
                            }
                        }
                    }
                    break;  // 跳出 AST_MODULE_CALL
                }
            }

            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            if (native_is_module(actual_module)) {
                // 检测 ffi.callback(func, CfuncName) - 使用 cfunc 签名创建回调
                if (strcmp(actual_module, "ffi") == 0 && strcmp(ast->u.module_call.method_name, "callback") == 0 &&
                    ast->u.module_call.args.count == 2) {
                    Ast* second_arg = ast->u.module_call.args.items[1];
                    if (second_arg->kind == AST_VAR) {
                        Symbol* cfunc_sym = scope_resolve(gen->sem->current, second_arg->u.var.name);
                        if (!cfunc_sym) cfunc_sym = scope_resolve(gen->sem->root_scope, second_arg->u.var.name);
                        if (cfunc_sym && cfunc_sym->type && cfunc_sym->type->kind == TYPE_CFUNC) {
                            // 生成: 先压入函数参数
                            gen_expr(gen, ast->u.module_call.args.items[0]);

                            // 计算返回类型的 FFIType
                            int ffi_ret_type = 1;
                            TypeInfo* ret = cfunc_sym->cfunc_return_type;
                            if (!ret || ret->kind == TYPE_NULL) ffi_ret_type = 0;
                            else if (ret->kind == TYPE_F32) ffi_ret_type = 10;
                            else if (ret->kind == TYPE_F64 || ret->kind == TYPE_FLOAT) ffi_ret_type = 2;
                            else if (ret->kind == TYPE_PTR || ret->kind == TYPE_PTR_GENERIC || ret->kind == TYPE_STR8 || ret->kind == TYPE_STR16) ffi_ret_type = 3;
                            else if (ret->kind == TYPE_BOOL) ffi_ret_type = 11;
                            else if (ret->kind == TYPE_I8) ffi_ret_type = 5;
                            else if (ret->kind == TYPE_U8) ffi_ret_type = 4;
                            else if (ret->kind == TYPE_I16) ffi_ret_type = 7;
                            else if (ret->kind == TYPE_U16) ffi_ret_type = 6;
                            else if (ret->kind == TYPE_I32) ffi_ret_type = 9;
                            else if (ret->kind == TYPE_U32) ffi_ret_type = 8;

                            int cfunc_param_count = cfunc_sym->cfunc_param_count;
                            uint8_t ffi_param_types[12];
                            for (int j = 0; j < cfunc_param_count && j < 12; j++) {
                                TypeInfo* pt = cfunc_sym->cfunc_param_types[j];
                                int ft = 1;
                                if (pt->kind == TYPE_F32) ft = 10;
                                else if (pt->kind == TYPE_F64 || pt->kind == TYPE_FLOAT) ft = 2;
                                else if (pt->kind == TYPE_PTR || pt->kind == TYPE_PTR_GENERIC || pt->kind == TYPE_STR8 || pt->kind == TYPE_STR16) ft = 3;
                                else if (pt->kind == TYPE_BOOL) ft = 11;
                                else if (pt->kind == TYPE_I8) ft = 5;
                                else if (pt->kind == TYPE_U8) ft = 4;
                                else if (pt->kind == TYPE_I16) ft = 7;
                                else if (pt->kind == TYPE_U16) ft = 6;
                                else if (pt->kind == TYPE_I32) ft = 9;
                                else if (pt->kind == TYPE_U32) ft = 8;
                                ffi_param_types[j] = (uint8_t)ft;
                            }

                            // OP_CFUNC_CALLBACK: ret_type(1) param_count(1) param_types[param_count]
                            emit_byte(gen, OP_CFUNC_CALLBACK, ast->line);
                            emit_byte(gen, (uint8_t)ffi_ret_type, ast->line);
                            emit_byte(gen, (uint8_t)cfunc_param_count, ast->line);
                            for (int j = 0; j < cfunc_param_count; j++) {
                                emit_byte(gen, ffi_param_types[j], ast->line);
                            }
                            break;
                        }
                    }
                }

                for (int i = 0; i < ast->u.module_call.args.count; i++) {
                    gen_expr(gen, ast->u.module_call.args.items[i]);
                }

                ObjString* module_name = str_copy(actual_module,
                                                  (int)strlen(actual_module));
                int module_const = make_constant(gen, val_obj((Object*)module_name));

                ObjString* method_name = str_copy(ast->u.module_call.method_name,
                                                  (int)strlen(ast->u.module_call.method_name));
                int method_const = make_constant(gen, val_obj((Object*)method_name));

                emit_byte(gen, OP_MODULE_CALL, ast->line);
                emit_byte(gen, (module_const >> 8) & 0xff, ast->line);
                emit_byte(gen, module_const & 0xff, ast->line);
                emit_byte(gen, (method_const >> 8) & 0xff, ast->line);
                emit_byte(gen, method_const & 0xff, ast->line);
                emit_byte(gen, (ast->u.module_call.args.count >> 8) & 0xff, ast->line);
                emit_byte(gen, ast->u.module_call.args.count & 0xff, ast->line);
            } else {
                // 查找模块符号
                Symbol* var_sym = scope_resolve(gen->sem->root_scope, ast->u.module_call.module_name);
                if (!var_sym) {
                    var_sym = scope_resolve(gen->sem->current, ast->u.module_call.module_name);
                }

                // 检查是否是模块级别的数组 add 方法调用：arrays.add(arr, x)
                if (var_sym && var_sym->type && var_sym->type->kind == TYPE_ARRAY &&
                    strcmp(ast->u.module_call.method_name, "add") == 0 &&
                    ast->u.module_call.args.count == 1) {
                    // 使用公共函数生成数组 add 操作（表达式上下文，需要返回值）
                    gen_array_add_by_symbol(gen, var_sym, ast->u.module_call.args.items[0], 1, ast->line);
                    break;
                }

                // 用户定义的模块调用
                int provided_args = ast->u.module_call.args.count;
                int total_args = provided_args;

                // 查找导入的模块信息，获取函数默认参数信息
                ImportedModuleInfo* mod_info = find_imported_module(gen->sem, ast->u.module_call.module_name);
                if (mod_info && mod_info->sym_table) {
                    ModuleFuncSymbol* func_sym = module_symbol_table_find_func(
                        mod_info->sym_table, ast->u.module_call.method_name);
                    if (func_sym && func_sym->param_count > provided_args) {
                        total_args = func_sym->param_count;
                    }
                }

                // 生成用户提供的参数
                for (int i = 0; i < provided_args; i++) {
                    gen_expr(gen, ast->u.module_call.args.items[i]);
                }

                // 填充缺失的默认参数值
                if (total_args > provided_args && mod_info && mod_info->sym_table) {
                    ModuleFuncSymbol* func_sym = module_symbol_table_find_func(
                        mod_info->sym_table, ast->u.module_call.method_name);
                    if (func_sym && func_sym->param_default_texts) {
                        for (int i = provided_args; i < func_sym->param_count; i++) {
                            if (func_sym->param_default_texts[i]) {
                                gen_default_value_from_text(gen, func_sym->param_default_texts[i], ast->line);
                            } else {
                                emit_byte(gen, OP_NULL, ast->line);
                            }
                        }
                    }
                }

                Symbol* module_sym = scope_resolve(gen->sem->root_scope, ast->u.module_call.module_name);
                if (!module_sym || (module_sym->kind != SYM_GLOBAL && module_sym->kind != SYM_MODULE)) {
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未定义的模块");
                    break;
                }

                // 获取模块对象
                if (module_sym->kind == SYM_MODULE) {
                    // 模块级别的变量，使用 OP_GET_MODULE_VAR
                    emit_bytes_2(gen, OP_GET_MODULE_VAR, module_sym->index, ast->line);
                } else {
                    emit_get_global(gen, module_sym->index, ast->line);
                }

                // 从模块的导出表中获取方法
                ObjString* method_name = str_copy(ast->u.module_call.method_name,
                                                  (int)strlen(ast->u.module_call.method_name));
                emit_constant(gen, val_obj((Object*)method_name), ast->line);
                emit_byte(gen, OP_INDEX, ast->line);

                emit_call(gen, total_args, ast->line);
            }
            break;
        }
        case AST_MODULE_ACCESS: {
            // 检查是否是原生模块常量（语义分析阶段标记为 TYPE_INT）
            const char* actual_module = native_resolve_module_alias(ast->u.module_access.module_name);
            if (native_is_module(actual_module)) {
                // 原生模块常量：生成 OP_GET_MODULE_CONST
                ObjString* module_name = str_copy(actual_module,
                                                  (int)strlen(actual_module));
                int module_const = make_constant(gen, val_obj((Object*)module_name));

                ObjString* const_name = str_copy(ast->u.module_access.member_name,
                                                 (int)strlen(ast->u.module_access.member_name));
                int const_idx = make_constant(gen, val_obj((Object*)const_name));

                emit_byte(gen, OP_GET_MODULE_CONST, ast->line);
                emit_byte(gen, (module_const >> 8) & 0xff, ast->line);
                emit_byte(gen, module_const & 0xff, ast->line);
                emit_byte(gen, (const_idx >> 8) & 0xff, ast->line);
                emit_byte(gen, const_idx & 0xff, ast->line);
                break;
            }

            // 检查是否是类型访问（struct/cstruct/face），直接获取类型定义
            if (ast->cached_type) {
                if (ast->cached_type->kind == TYPE_CSTRUCT && ast->cached_type->struct_name) {
                    ObjString* type_name = str_copy(ast->cached_type->struct_name,
                                                      (int)strlen(ast->cached_type->struct_name));
                    int name_const = make_constant(gen, val_obj((Object*)type_name));
                    emit_byte(gen, OP_GET_CSTRUCT_DEF, ast->line);
                    emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, name_const & 0xff, ast->line);
                    break;
                }
                if (ast->cached_type->kind == TYPE_STRUCT && ast->cached_type->struct_name) {
                    // struct 类型：从模块对象中通过 OP_INDEX 获取
                    // fall through to default handling below
                }
            }

            // .leno 用户模块成员访问
            Symbol* module_sym = scope_resolve(gen->sem->root_scope, ast->u.module_access.module_name);
            if (!module_sym || (module_sym->kind != SYM_GLOBAL && module_sym->kind != SYM_MODULE)) {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "未定义的模块");
                break;
            }

            if (module_sym->kind == SYM_MODULE) {
                // 模块级别的变量，使用 OP_GET_MODULE_VAR
                emit_bytes_2(gen, OP_GET_MODULE_VAR, module_sym->index, ast->line);
            } else {
                emit_get_global(gen, module_sym->index, ast->line);
            }

            ObjString* member_name = str_copy(ast->u.module_access.member_name,
                                              (int)strlen(ast->u.module_access.member_name));
            emit_constant(gen, val_obj((Object*)member_name), ast->line);
            emit_byte(gen, OP_INDEX, ast->line);
            break;
        }
        case AST_TYPE_CHECK: {
            gen_expr(gen, ast->u.type_check.expr);
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_FACE && ast->u.type_check.type->struct_name) {
                ObjString* face_name_str = str_copy(ast->u.type_check.type->struct_name,
                                                     strlen(ast->u.type_check.type->struct_name));
                int face_name_const = make_constant(gen, val_obj((Object*)face_name_str));
                emit_byte(gen, OP_TYPE_CHECK, ast->line);
                emit_byte(gen, (uint8_t)TYPE_FACE, ast->line);
                emit_byte(gen, (face_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, face_name_const & 0xff, ast->line);
            } else if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                ObjString* struct_name_str = str_copy(ast->u.type_check.type->struct_name,
                                                       strlen(ast->u.type_check.type->struct_name));
                int struct_name_const = make_constant(gen, val_obj((Object*)struct_name_str));
                emit_byte(gen, OP_TYPE_CHECK, ast->line);
                emit_byte(gen, (uint8_t)TYPE_STRUCT, ast->line);
                emit_byte(gen, (struct_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, struct_name_const & 0xff, ast->line);
            } else if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_ENUM && ast->u.type_check.type->struct_name) {
                ObjString* enum_name_str = str_copy(ast->u.type_check.type->struct_name,
                                                     strlen(ast->u.type_check.type->struct_name));
                int enum_name_const = make_constant(gen, val_obj((Object*)enum_name_str));
                emit_byte(gen, OP_TYPE_CHECK, ast->line);
                emit_byte(gen, (uint8_t)TYPE_ENUM, ast->line);
                emit_byte(gen, (enum_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, enum_name_const & 0xff, ast->line);
            } else {
                emit_byte(gen, OP_TYPE_CHECK, ast->line);
                emit_byte(gen, (uint8_t)ast->u.type_check.type->kind, ast->line);
                if (ast->u.type_check.type->element_type) {
                    emit_byte(gen, (uint8_t)ast->u.type_check.type->element_type->kind, ast->line);
                } else {
                    emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                }
            }
            break;
        }
        case AST_AS_CAST: {
            gen_expr(gen, ast->u.type_check.expr);
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_FACE && ast->u.type_check.type->struct_name) {
                ObjString* face_name_str = str_copy(ast->u.type_check.type->struct_name,
                                                     strlen(ast->u.type_check.type->struct_name));
                int face_name_const = make_constant(gen, val_obj((Object*)face_name_str));
                emit_byte(gen, OP_AS_CAST, ast->line);
                emit_byte(gen, (uint8_t)TYPE_FACE, ast->line);
                emit_byte(gen, (face_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, face_name_const & 0xff, ast->line);
            } else if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                ObjString* struct_name_str = str_copy(ast->u.type_check.type->struct_name,
                                                       strlen(ast->u.type_check.type->struct_name));
                int struct_name_const = make_constant(gen, val_obj((Object*)struct_name_str));
                emit_byte(gen, OP_AS_CAST, ast->line);
                emit_byte(gen, (uint8_t)TYPE_STRUCT, ast->line);
                emit_byte(gen, (struct_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, struct_name_const & 0xff, ast->line);
            } else {
                emit_byte(gen, OP_AS_CAST, ast->line);
                emit_byte(gen, (uint8_t)ast->u.type_check.type->kind, ast->line);
                if (ast->u.type_check.type->element_type) {
                    emit_byte(gen, (uint8_t)ast->u.type_check.type->element_type->kind, ast->line);
                } else {
                    emit_byte(gen, (uint8_t)TYPE_ANY, ast->line);
                }
            }
            break;
        }
        case AST_STRUCT_INIT: {
            // 生成结构体构造函数调用
            // 处理模块限定的 struct 名称（如 "math.Point"），提取实际的 struct 名称
            const char* dot_pos = strchr(ast->u.struct_init.struct_name, '.');
            const char* actual_struct_name = dot_pos ? dot_pos + 1 : ast->u.struct_init.struct_name;

            ObjString* struct_name = str_copy(actual_struct_name,
                                              strlen(actual_struct_name));
            int name_const = make_constant(gen, val_obj((Object*)struct_name));
            
            // 编译期从符号表查找 struct 定义和字段索引
            Symbol* struct_sym = scope_resolve(gen->sem->root_scope, actual_struct_name);
            if (!struct_sym) {
                struct_sym = scope_resolve(gen->sem->current, actual_struct_name);
            }

            // 模块限定的 struct：从导入模块的符号表中获取字段信息
            if (!struct_sym && dot_pos) {
                int mod_name_len = dot_pos - ast->u.struct_init.struct_name;
                char* module_name = (char*)malloc(mod_name_len + 1);
                memcpy(module_name, ast->u.struct_init.struct_name, mod_name_len);
                module_name[mod_name_len] = '\0';

                ImportedModuleInfo* module_info = find_imported_module(gen->sem, module_name);
                if (module_info && module_info->sym_table) {
                    ModuleStructSymbol* mod_struct = module_symbol_table_find_struct(module_info->sym_table, actual_struct_name);
                    if (mod_struct) {
                        struct_sym = (Symbol*)calloc(1, sizeof(Symbol));
                        struct_sym->name = strdup(actual_struct_name);
                        struct_sym->type = type_new(TYPE_STRUCT);
                        struct_sym->type->struct_name = strdup(actual_struct_name);
                        struct_sym->struct_field_count = mod_struct->field_count;
                        if (mod_struct->field_count > 0) {
                            struct_sym->struct_field_names = (char**)malloc(sizeof(char*) * mod_struct->field_count);
                            for (int fi = 0; fi < mod_struct->field_count; fi++) {
                                struct_sym->struct_field_names[fi] = strdup(mod_struct->fields[fi].name);
                            }
                        }
                    }
                }
                free(module_name);
            }
            
            // 编译期检查参数数量是否超过字段数量
            if (struct_sym && struct_sym->struct_field_count > 0) {
                if (ast->u.struct_init.field_count > struct_sym->struct_field_count) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "创建结构体 '%s' 时参数数量过多: 期望最多 %d, 实际 %d",
                            ast->u.struct_init.struct_name, struct_sym->struct_field_count, 
                            ast->u.struct_init.field_count);
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
                }
            }
            
            // 生成参数字段值
            for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                gen_expr(gen, ast->u.struct_init.field_values[i]);
            }
            
            emit_byte(gen, OP_STRUCT_INIT, ast->line);
            emit_byte(gen, (name_const >> 8) & 0xff, ast->line);
            emit_byte(gen, name_const & 0xff, ast->line);
            emit_byte(gen, ast->u.struct_init.field_count, ast->line);

            // 泛型参数信息（如 Box[int] 中的 [int]）
            int gt_count = ast->u.struct_init.generic_type_count;
            emit_byte(gen, (uint8_t)gt_count, ast->line);
            for (int gi = 0; gi < gt_count; gi++) {
                // 将泛型参数类型名作为常量写入
                const char* type_name_str = type_to_string(ast->u.struct_init.generic_type_args[gi]);
                int type_name_const = make_constant(gen, val_obj((Object*)str_copy(type_name_str, (int)strlen(type_name_str))));
                emit_byte(gen, (type_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, type_name_const & 0xff, ast->line);
            }
            
            // 参数字段索引（反序生成，与栈顺序匹配）
            // 编译期确定字段索引，避免运行时线性搜索
            for (int i = ast->u.struct_init.field_count - 1; i >= 0; i--) {
                int field_idx = 0; // 默认值为 0
                if (struct_sym && struct_sym->struct_field_count > 0) {
                    // 在符号表的字段列表中查找字段索引
                    for (int j = 0; j < struct_sym->struct_field_count; j++) {
                        if (strcmp(struct_sym->struct_field_names[j], ast->u.struct_init.field_names[i]) == 0) {
                            field_idx = j;
                            break;
                        }
                    }
                }
                emit_byte(gen, (uint8_t)field_idx, ast->line);
            }
            break;
        }
        case AST_FIELD_ACCESS: {
            // 检查是否 clib 函数调用（无参数，如 get_k32().GetTickCount()）
            TypeInfo* obj_type = infer_expr_type(gen->sem, ast->u.field_access.obj);
            if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                Symbol* clib_sym = scope_resolve(gen->sem->root_scope, obj_type->struct_name);
                if (!clib_sym) {
                    clib_sym = scope_resolve(gen->sem->current, obj_type->struct_name);
                }
                if (clib_sym && clib_sym->clib_func_count > 0 && ast->u.field_access.field_index >= 0) {
                    int fi = ast->u.field_access.field_index;
                    TypeInfo* ret_type = clib_sym->clib_func_return_types[fi];
                    const char* func_name = ast->u.field_access.field_name;

                    // 生成库对象
                    gen_expr(gen, ast->u.field_access.obj);
                    // 生成函数名字符串
                    ObjString* func_name_str = str_copy(func_name, (int)strlen(func_name));
                    emit_constant(gen, val_obj((Object*)func_name_str), ast->line);

                    int ret_type_kind = ret_type ? ret_type->kind : TYPE_NULL;
                    // OP_CLIB_CALL: arg_count(2) ret_type_kind(1) user_arg_count(1) arg_types(0)
                    emit_byte(gen, OP_CLIB_CALL, ast->line);
                    emit_byte(gen, 0, ast->line);
                    emit_byte(gen, 2, ast->line);  // total_arg_count = 库 + 函数名 = 2
                    emit_byte(gen, (uint8_t)ret_type_kind, ast->line);
                    emit_byte(gen, 0, ast->line);  // user_arg_count = 0
                    if (obj_type) type_free(obj_type);
                    break;
                }
            }

            // 优化：如果对象是 struct 类型、字段索引编译期已知、且对象是 local 变量，
            // 发射 OP_GET_FIELD_FAST 融合指令（合并 GET_LOCAL + GET_FIELD，跳过栈操作和类型检查）
            int field_idx = ast->u.field_access.field_index;
            if (obj_type && obj_type->kind == TYPE_STRUCT && field_idx >= 0) {
                Ast* obj_ast = ast->u.field_access.obj;
                if (obj_ast && obj_ast->kind == AST_VAR) {
                    SymRef* ref = &obj_ast->u.var.ref;
                    if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
                        // 快速路径：直接从 local slot 读对象 + 取字段
                        emit_byte(gen, OP_GET_FIELD_FAST, ast->line);
                        emit_byte(gen, (ref->index >> 8) & 0xff, ast->line);
                        emit_byte(gen, ref->index & 0xff, ast->line);
                        emit_byte(gen, (uint8_t)field_idx, ast->line);
                        if (obj_type) type_free(obj_type);
                        break;
                    }
                }
            }

            if (obj_type) type_free(obj_type);

            // 生成对象表达式
            gen_expr(gen, ast->u.field_access.obj);

            // 使用编译期确定的字段索引（优化：避免运行时线性搜索）
            if (field_idx >= 0) {
                // 优化路径：使用字段索引直接访问
                emit_byte(gen, OP_GET_FIELD, ast->line);
                emit_byte(gen, (uint8_t)field_idx, ast->line);
            } else {
                // 回退路径：使用 OP_INDEX 通过字段名访问
                ObjString* field_name = str_copy(ast->u.field_access.field_name,
                                                  strlen(ast->u.field_access.field_name));
                int field_name_const = make_constant(gen, val_obj((Object*)field_name));
                emit_byte(gen, OP_CONST, ast->line);
                emit_bytes(gen, (field_name_const >> 8) & 0xff, field_name_const & 0xff, ast->line);
                emit_byte(gen, OP_INDEX, ast->line);
            }
            break;
        }
        case AST_ADDRESS_OF: {
            // &expr 取地址：生成对象表达式 + OP_GET_FIELD_ADDR
            Ast* operand = ast->u.address_of.operand;
            if (operand && operand->kind == AST_FIELD_ACCESS) {
                // 生成对象表达式（递归处理链式访问，如 &(c.nested).field）
                gen_expr(gen, operand->u.field_access.obj);
                // 发射 OP_GET_FIELD_ADDR + 字段索引
                int field_idx = operand->u.field_access.field_index;
                if (field_idx >= 0) {
                    emit_byte(gen, OP_GET_FIELD_ADDR, ast->line);
                    emit_byte(gen, (uint8_t)field_idx, ast->line);
                } else {
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column, "无法确定字段索引，& 取地址失败");
                }
            } else {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "& 取地址运算符只能用于字段访问");
            }
            break;
        }
        case AST_AWAIT: {
            // 生成 await 表达式的代码
            // 先生成被等待的表达式（应该返回 Future）
            gen_expr(gen, ast->u.await.expr);
            // 生成 OP_AWAIT 指令
            emit_byte(gen, OP_AWAIT, ast->line);
            break;
        }
        case AST_SAFE_ACCESS: {
            // 安全访问：expr?.field / expr?.method()
            // 栈布局：
            //   gen_expr(obj)     → [obj]
            //   OP_DUP            → [obj, obj]
            //   OP_IS_NULL        → [obj, is_null]（弹出 dup，压入 bool）
            //   OP_JUMP_IF_TRUE   → null_jump (跳到 null 路径)
            //   OP_POP            → [obj] 弹出比较结果
            //   ... 非 null 路径（字段/方法访问）
            //   OP_JUMP           → end_jump
            //   null 路径：OP_POP + OP_POP + OP_NULL
            //   结束
            gen_expr(gen, ast->u.safe_access.obj);    // [obj]
            emit_byte(gen, OP_DUP, ast->line);         // [obj, obj]
            emit_byte(gen, OP_IS_NULL, ast->line);     // [obj, is_null]
            int null_jump = emit_jump(gen, OP_JUMP_IF_TRUE, ast->line);

            // 非 null 路径
            emit_byte(gen, OP_POP, ast->line);         // [obj] 弹出比较结果 bool

            if (!ast->u.safe_access.is_call) {
                // === 字段访问：obj?.field ===
                int field_idx = ast->u.safe_access.field_index;
                if (field_idx >= 0) {
                    emit_byte(gen, OP_GET_FIELD, ast->line);
                    emit_byte(gen, (uint8_t)field_idx, ast->line);
                } else {
                    // 回退：通过字段名索引
                    ObjString* fname = str_copy(ast->u.safe_access.name,
                                                 strlen(ast->u.safe_access.name));
                    int fname_const = make_constant(gen, val_obj((Object*)fname));
                    emit_byte(gen, OP_CONST, ast->line);
                    emit_bytes(gen, (fname_const >> 8) & 0xff, fname_const & 0xff, ast->line);
                    emit_byte(gen, OP_INDEX, ast->line);
                }
            } else {
                // === 方法调用：obj?.method(args) ===
                // 临时变量保存 obj
                int temp_slot = -1;
                if (gen->current_func) {
                    temp_slot = gen->current_func->local_count;
                    if (gen->max_local_slot > temp_slot) {
                        temp_slot = gen->max_local_slot;
                    }
                }
                if (temp_slot < 0) temp_slot = 0;
                if (temp_slot + 1 > gen->max_local_slot) {
                    gen->max_local_slot = temp_slot + 1;
                }

                emit_bytes_2(gen, OP_SET_LOCAL_POP, temp_slot, ast->line);  // [] 保存 obj

                // 生成参数（self + args）
                emit_bytes_2(gen, OP_GET_LOCAL, temp_slot, ast->line);  // [self]
                for (int i = 0; i < ast->u.safe_access.args.count; i++) {
                    gen_expr(gen, ast->u.safe_access.args.items[i]);    // [self, arg1, ...]
                }

                // 压入 receiver 供 OP_GET_METHOD 消费
                emit_bytes_2(gen, OP_GET_LOCAL, temp_slot, ast->line);  // [self, args..., obj]

                // 生成 OP_GET_METHOD
                ObjString* method_name_str = str_copy(ast->u.safe_access.name,
                                                       strlen(ast->u.safe_access.name));
                int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));
                emit_byte(gen, OP_GET_METHOD, ast->line);
                emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                emit_byte(gen, method_name_const & 0xff, ast->line);

                // 生成调用
                int call_arg_count = ast->u.safe_access.args.count + 1; // +1 for self
                if (ast->u.safe_access.callee_is_async) {
                    emit_byte(gen, OP_ASYNC_CALL, ast->line);
                    emit_byte(gen, (call_arg_count >> 8) & 0xff, ast->line);
                    emit_byte(gen, call_arg_count & 0xff, ast->line);
                } else {
                    emit_call(gen, call_arg_count, ast->line);
                }
            }

            // 跳过 null 路径
            int end_jump = emit_jump(gen, OP_JUMP, ast->line);

            // null 路径
            patch_jump(gen, null_jump);
            emit_byte(gen, OP_POP, ast->line);     // [] 弹出比较结果 bool
            emit_byte(gen, OP_POP, ast->line);     // [] 弹出 obj
            emit_byte(gen, OP_NULL, ast->line);    // [null] 推入 null 作为结果

            // 结束
            patch_jump(gen, end_jump);
            break;
        }
        case AST_FUNC_DEF: {
            // 生成匿名函数表达式
            // 1. 生成函数原型
            ObjFunction* func = gen_func_proto(gen, ast);
            if (!func) {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "生成匿名函数失败");
                break;
            }
            
            // 2. 将函数对象作为常量
            int func_const = make_constant(gen, val_obj((Object*)func));
            emit_bytes(gen, OP_CLOSURE, (func_const >> 8) & 0xff, ast->line);
            emit_byte(gen, func_const & 0xff, ast->line);
            
            // 3. 处理 upvalues
            for (int i = 0; i < ast->u.func.upvalue_count; i++) {
                uint16_t is_local = (uint16_t)ast->u.func.upvalue_is_local[i];
                uint16_t index = (uint16_t)ast->u.func.upvalue_indices[i];
                uint16_t is_value_capture = (uint16_t)ast->u.func.upvalue_is_value_capture[i];
                emit_byte(gen, (is_local >> 8) & 0xff, ast->line);
                emit_byte(gen, is_local & 0xff, ast->line);
                emit_byte(gen, (index >> 8) & 0xff, ast->line);
                emit_byte(gen, index & 0xff, ast->line);
                emit_byte(gen, (is_value_capture >> 8) & 0xff, ast->line);
                emit_byte(gen, is_value_capture & 0xff, ast->line);
            }
            break;
        }
        default: {
            char msg[256];
            const char* ast_type_name = "UNKNOWN";
            switch (ast->kind) {
                case AST_NUM: ast_type_name = "AST_NUM"; break;
                case AST_STRING: ast_type_name = "AST_STRING"; break;
                case AST_BOOL: ast_type_name = "AST_BOOL"; break;
                case AST_NULL: ast_type_name = "AST_NULL"; break;
                case AST_ARRAY: ast_type_name = "AST_ARRAY"; break;
                case AST_DICT: ast_type_name = "AST_DICT"; break;
                case AST_RANGE: ast_type_name = "AST_RANGE"; break;
                case AST_VAR: ast_type_name = "AST_VAR"; break;
                case AST_BINOP: ast_type_name = "AST_BINOP"; break;
                case AST_UNARY: ast_type_name = "AST_UNARY"; break;
                case AST_CALL: ast_type_name = "AST_CALL"; break;
                case AST_INDEX: ast_type_name = "AST_INDEX"; break;
                case AST_SLICE: ast_type_name = "AST_SLICE"; break;
                case AST_INDEX_ASSIGN: ast_type_name = "AST_INDEX_ASSIGN"; break;
                case AST_BLOCK: ast_type_name = "AST_BLOCK"; break;
                case AST_IF: ast_type_name = "AST_IF"; break;
                case AST_WHILE: ast_type_name = "AST_WHILE"; break;
                case AST_FOR: ast_type_name = "AST_FOR"; break;
                case AST_SWITCH: ast_type_name = "AST_SWITCH"; break;
                case AST_FUNC_DEF: ast_type_name = "AST_FUNC_DEF"; break;
                case AST_RETURN: ast_type_name = "AST_RETURN"; break;
                case AST_BREAK: ast_type_name = "AST_BREAK"; break;
                case AST_CONTINUE: ast_type_name = "AST_CONTINUE"; break;
                case AST_ASSIGN: ast_type_name = "AST_ASSIGN"; break;
                case AST_COMPOUND_ASSIGN: ast_type_name = "AST_COMPOUND_ASSIGN"; break;
                case AST_VAR_DECL: ast_type_name = "AST_VAR_DECL"; break;
                case AST_EXPR_STMT: ast_type_name = "AST_EXPR_STMT"; break;
                case AST_IMPORT: ast_type_name = "AST_IMPORT"; break;
                case AST_EXPORT: ast_type_name = "AST_EXPORT"; break;
                case AST_USE: ast_type_name = "AST_USE"; break;
                case AST_MODULE_CALL: ast_type_name = "AST_MODULE_CALL"; break;
                case AST_MODULE_ACCESS: ast_type_name = "AST_MODULE_ACCESS"; break;
                case AST_INTERP_STRING: ast_type_name = "AST_INTERP_STRING"; break;
                case AST_TRY: ast_type_name = "AST_TRY"; break;
                case AST_THROW: ast_type_name = "AST_THROW"; break;
                case AST_TYPE_CHECK: ast_type_name = "AST_TYPE_CHECK"; break;
                case AST_STRUCT_DEF: ast_type_name = "AST_STRUCT_DEF"; break;
                case AST_CSTRUCT_DEF: ast_type_name = "AST_CSTRUCT_DEF"; break;
                case AST_CLIB_DEF: ast_type_name = "AST_CLIB_DEF"; break;
                case AST_ENUM_DEF: ast_type_name = "AST_ENUM_DEF"; break;
                case AST_STRUCT_INIT: ast_type_name = "AST_STRUCT_INIT"; break;
                case AST_FIELD_ACCESS: ast_type_name = "AST_FIELD_ACCESS"; break;
                case AST_ADDRESS_OF: ast_type_name = "AST_ADDRESS_OF"; break;
                case AST_AWAIT: ast_type_name = "AST_AWAIT"; break;
                case AST_SAFE_ACCESS: ast_type_name = "AST_SAFE_ACCESS"; break;
                case AST_DESTRUCT_DECL: ast_type_name = "AST_DESTRUCT_DECL"; break;
                default: ast_type_name = "UNKNOWN_AST_KIND"; break;
            }
            snprintf(msg, sizeof(msg), "未知的表达式类型: %s (kind=%d)", ast_type_name, ast->kind);
            error_add_at(ERR_SEMANTIC, ast->line, ast->column, msg);
            break;
        }
    }
}
