#include "codegen.h"
#include "../semantic/semantic_internal.h"

static void gen_binary(CodeGen* gen, Ast* ast);
static void gen_unary(CodeGen* gen, Ast* ast);
static void gen_variable(CodeGen* gen, Ast* ast, int can_assign);
static void gen_call(CodeGen* gen, Ast* ast);

// 生成数组 add 操作（公共函数）
// receiver_ast: 数组表达式
// arg_ast: 要添加的元素表达式
// line: 行号
static void gen_array_add(CodeGen* gen, Ast* receiver_ast, Ast* arg_ast, int line) {
    gen_expr(gen, receiver_ast);
    gen_expr(gen, arg_ast);
    emit_byte(gen, OP_ARRAY_APPEND, line);
}

// 通过变量符号生成数组 add 操作（公共函数）
// var_sym: 数组变量符号
// arg_ast: 要添加的元素表达式
// line: 行号
static void gen_array_add_by_symbol(CodeGen* gen, Symbol* var_sym, Ast* arg_ast, int line) {
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
            error_add(ERR_SEMANTIC, line, "未知的变量类型");
            return;
    }
    gen_expr(gen, arg_ast);
    emit_byte(gen, OP_ARRAY_APPEND, line);
}

static TypeKind get_expr_type_kind(Ast* ast) {
    if (!ast || !ast->cached_type) return TYPE_ANY;
    return ast->cached_type->kind;
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
        int else_jump = emit_jump(gen, OP_JUMP_IF_FALSE, ast->line);
        int end_jump = emit_jump(gen, OP_JUMP, ast->line);
        patch_jump(gen, else_jump);
        emit_byte(gen, OP_POP, ast->line);
        gen_expr(gen, ast->u.binop.r);
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
        case TOK_SHL:    gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_SHL, ast->line); break;
        case TOK_SHR:    gen_expr(gen, ast->u.binop.r); emit_byte(gen, OP_SHR, ast->line); break;
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
            error_add(ERR_SEMANTIC, ast->line, "未知的二元操作符");
            break;
    }
}

static void gen_unary(CodeGen* gen, Ast* ast) {
    if (ast->u.unary.op == TOK_INC || ast->u.unary.op == TOK_DEC) {
        if (ast->u.unary.operand->kind != AST_VAR) {
            error_add(ERR_SEMANTIC, ast->line, "++ 和 -- 只能用于变量");
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
                error_add(ERR_SEMANTIC, ast->line, "不支持的变量类型用于 ++/--");
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
                error_add(ERR_SEMANTIC, ast->line, "不支持的变量类型用于 ++/--");
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
            error_add(ERR_SEMANTIC, ast->line, "未知的一元操作符");
            break;
    }
}

static void gen_variable(CodeGen* gen, Ast* ast, int can_assign) {
    (void)can_assign;
    SymRef* ref = &ast->u.var.ref;
    if (!ref->name) {
        error_add(ERR_SEMANTIC, ast->line, "未解析的变量");
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
            error_add(ERR_SEMANTIC, ast->line, "未知的符号类型");
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
            ObjString* str = str_copy(default_expr->u.string.value, strlen(default_expr->u.string.value));
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
            // 其他情况不应发生（语义分析已确保是字面量）
            emit_byte(gen, OP_NULL, default_expr->line);
            break;
    }
}

static void gen_call(CodeGen* gen, Ast* ast) {
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
            // 使用公共函数生成数组 add 操作
            gen_array_add(gen, ast->u.call.callee->u.index.obj, ast->u.call.args.items[0], ast->line);
            return;
        }
    }

    // 检测 dict.set(key, value) 模式并优化为 OP_DICT_SET
    if (ast->u.call.callee->kind == AST_INDEX &&
        ast->u.call.callee->u.index.index->kind == AST_STRING &&
        ast->u.call.args.count == 2 &&
        strcmp(ast->u.call.callee->u.index.index->u.string.value, "set") == 0) {
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

    // 检测 struct 方法调用: obj.method(args)
    // 使用 OP_GET_METHOD 从 struct 方法表获取方法函数
    if (ast->u.call.callee->kind == AST_INDEX &&
        ast->u.call.callee->u.index.index->kind == AST_STRING) {
        const char* method_name = ast->u.call.callee->u.index.index->u.string.value;
        Ast* obj_ast = ast->u.call.callee->u.index.obj;

        // 推断 receiver 的类型
        TypeInfo* receiver_type = infer_expr_type(gen->sem, obj_ast);
        int is_native_obj_type = 0;
        const char* native_type_name = NULL;
        if (receiver_type && (receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_FACE) && receiver_type->struct_name) {
            // struct/face 类型，已有 struct_name
        } else if (receiver_type && (receiver_type->kind == TYPE_EVENT || receiver_type->kind == TYPE_DRAW || receiver_type->kind == TYPE_WIN || receiver_type->kind == TYPE_FILE)) {
            // 原生对象类型（Event, Draw, Win, File 等）
            is_native_obj_type = 1;
            native_type_name = native_get_type_name(receiver_type->kind);
        }
        if (receiver_type && ((receiver_type->kind == TYPE_STRUCT || receiver_type->kind == TYPE_FACE) && receiver_type->struct_name) || is_native_obj_type) {
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
                    expected_args = provided_args + (obj_ast->kind == AST_VAR ? 0 : 1);
                    required_args = expected_args;
                }
            }

            // 检查参数数量
            // 注意：对于方法调用，如果 obj 是 AST_VAR（如 list.add(10)），
            // 则 provided_args 已经包含了 self（在语义分析阶段添加）
            // 如果 obj 不是 AST_VAR（如 cs(x=10).test()），
            // 则 provided_args 不包含 self，需要 +1
            int self_in_args = (obj_ast->kind == AST_VAR) ? 1 : 0;
            int actual_provided = provided_args + (self_in_args ? 0 : 1);  // +1 for self if not already in args
            if (has_method_def && actual_provided < required_args) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数不足: 至少需要 %d 个参数，实际传入 %d 个",
                         method_name, required_args - 1, provided_args - (self_in_args ? 1 : 0));
                error_add(ERR_SEMANTIC, ast->line, msg);
                return;
            }

            if (has_method_def && actual_provided > expected_args) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "方法 '%s' 调用参数过多: 最多接受 %d 个参数，实际传入 %d 个",
                         method_name, expected_args - 1, provided_args - (self_in_args ? 1 : 0));
                error_add(ERR_SEMANTIC, ast->line, msg);
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
                    // 栈布局：[self?] [args...] [self_for_get_method] -> OP_GET_METHOD -> [self?] [args...] [closure] -> OP_CALL
                    // 注意：当 self_in_args 时，语义分析已将 self 加入 args，不需要额外压入
                    if (!self_in_args) {
                        gen_expr(gen, obj_ast);
                    }
                    for (int i = 0; i < ast->u.call.args.count; i++) {
                        gen_expr(gen, ast->u.call.args.items[i]);
                    }
                    // 再次压入 receiver 供 OP_GET_METHOD 消费（从 struct 方法表查找方法）
                    gen_expr(gen, obj_ast);

                    ObjString* method_name_str = str_copy(method_name, strlen(method_name));
                    int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));
                    emit_byte(gen, OP_GET_METHOD, ast->line);
                    emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, method_name_const & 0xff, ast->line);

                    // self_in_args 时 args 已包含 self，否则需要 +1 补上 self
                    int call_arg_count = provided_args + (self_in_args ? 0 : 1);
                    if (has_method_def) {
                        call_arg_count = expected_args;
                    }
                    emit_call(gen, call_arg_count, ast->line);
                } else {
                    // struct 方法调用：s.method(args)
                    // 栈布局与 face 调用相同，self_in_args 时跳过首次 self 压栈避免重复
                    if (!self_in_args) {
                        gen_expr(gen, obj_ast);
                    }
                    for (int i = 0; i < ast->u.call.args.count; i++) {
                        gen_expr(gen, ast->u.call.args.items[i]);
                    }
                    gen_expr(gen, obj_ast);

                    ObjString* method_name_str = str_copy(method_name, strlen(method_name));
                    int method_name_const = make_constant(gen, val_obj((Object*)method_name_str));
                    emit_byte(gen, OP_GET_METHOD, ast->line);
                    emit_byte(gen, (method_name_const >> 8) & 0xff, ast->line);
                    emit_byte(gen, method_name_const & 0xff, ast->line);

                    // 计算已提供的参数数量（含 self），用于默认参数填充
                    int effective_provided = self_in_args ? provided_args : (provided_args + 1);
                    if (has_method_def && method_def && effective_provided < expected_args) {
                        for (int i = effective_provided; i < expected_args; i++) {
                            Ast* default_expr = method_def->u.func.param_defaults[i];
                            if (default_expr) {
                                gen_default_value(gen, default_expr);
                            } else {
                                emit_byte(gen, OP_NULL, ast->line);
                            }
                        }
                    }

                    if (has_method_def && method_def && method_def->u.func.is_async) {
                        emit_byte(gen, OP_ASYNC_CALL, ast->line);
                        emit_byte(gen, (expected_args >> 8) & 0xff, ast->line);
                        emit_byte(gen, expected_args & 0xff, ast->line);
                    } else {
                        emit_call(gen, expected_args, ast->line);
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
            error_add(ERR_SEMANTIC, ast->line, msg);
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
            error_add(ERR_SEMANTIC, ast->line, msg);
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

    gen_expr(gen, ast->u.call.callee);

    // 使用完整的参数数量（包括默认值）
    int total_args = func_def ? expected_args : ast->u.call.args.count;
    
    // 检查是否是 async 函数调用
    if (func_def && func_def->kind == AST_FUNC_DEF && func_def->u.func.is_async) {
        // async 函数使用 OP_ASYNC_CALL
        emit_byte(gen, OP_ASYNC_CALL, ast->line);
        emit_byte(gen, (total_args >> 8) & 0xff, ast->line);
        emit_byte(gen, total_args & 0xff, ast->line);
    } else {
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
            ObjString* str = str_copy(ast->u.string.value, strlen(ast->u.string.value));
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
                    ObjString* key = str_copy(ast->u.dict.entries[i].key,
                                              (int)strlen(ast->u.dict.entries[i].key));
                    emit_constant(gen, val_obj((Object*)key), ast->line);
                    emit_bytes_2(gen, OP_GET_LOCAL, temp_slot_base + i, ast->line);
                }
            } else {
                // 简单情况：直接生成 key-value 对
                for (int i = 0; i < ast->u.dict.count; i++) {
                    ObjString* key = str_copy(ast->u.dict.entries[i].key,
                                              (int)strlen(ast->u.dict.entries[i].key));
                    emit_constant(gen, val_obj((Object*)key), ast->line);
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
                                                    (int)strlen(ast->u.index.index->u.string.value));
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
                    error_add(ERR_SEMANTIC, ast->line, "无法确定字段索引，struct 类型可能未定义");
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
            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            if (native_is_module(actual_module)) {
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
                    // 使用公共函数生成数组 add 操作
                    gen_array_add_by_symbol(gen, var_sym, ast->u.module_call.args.items[0], ast->line);
                    break;
                }

                // 用户定义的模块调用
                for (int i = 0; i < ast->u.module_call.args.count; i++) {
                    gen_expr(gen, ast->u.module_call.args.items[i]);
                }

                Symbol* module_sym = scope_resolve(gen->sem->root_scope, ast->u.module_call.module_name);
                if (!module_sym || (module_sym->kind != SYM_GLOBAL && module_sym->kind != SYM_MODULE)) {
                    error_add(ERR_SEMANTIC, ast->line, "未定义的模块");
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

                emit_call(gen, ast->u.module_call.args.count, ast->line);
            }
            break;
        }
        case AST_MODULE_ACCESS: {
            Symbol* module_sym = scope_resolve(gen->sem->root_scope, ast->u.module_access.module_name);
            if (!module_sym || (module_sym->kind != SYM_GLOBAL && module_sym->kind != SYM_MODULE)) {
                error_add(ERR_SEMANTIC, ast->line, "未定义的模块");
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
                    error_add(ERR_SEMANTIC, ast->line, msg);
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
            // 生成对象表达式
            gen_expr(gen, ast->u.field_access.obj);

            // 使用编译期确定的字段索引（优化：避免运行时线性搜索）
            int field_idx = ast->u.field_access.field_index;

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
        case AST_AWAIT: {
            // 生成 await 表达式的代码
            // 先生成被等待的表达式（应该返回 Future）
            gen_expr(gen, ast->u.await.expr);
            // 生成 OP_AWAIT 指令
            emit_byte(gen, OP_AWAIT, ast->line);
            break;
        }
        case AST_FUNC_DEF: {
            // 生成匿名函数表达式
            // 1. 生成函数原型
            ObjFunction* func = gen_func_proto(gen, ast);
            if (!func) {
                error_add(ERR_SEMANTIC, ast->line, "生成匿名函数失败");
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
                case AST_ENUM_DEF: ast_type_name = "AST_ENUM_DEF"; break;
                case AST_STRUCT_INIT: ast_type_name = "AST_STRUCT_INIT"; break;
                case AST_FIELD_ACCESS: ast_type_name = "AST_FIELD_ACCESS"; break;
                case AST_AWAIT: ast_type_name = "AST_AWAIT"; break;
                default: ast_type_name = "UNKNOWN_AST_KIND"; break;
            }
            snprintf(msg, sizeof(msg), "未知的表达式类型: %s (kind=%d)", ast_type_name, ast->kind);
            error_add(ERR_SEMANTIC, ast->line, msg);
            break;
        }
    }
}
