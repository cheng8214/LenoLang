#include "semantic_internal.h"
#include "include/module_symbol_table.h"

// ============================================================================
// 从函数体推断返回类型
// ============================================================================

static TypeInfo* infer_return_type_from_body(Semantic* s, Ast* body) {
    if (!body) return NULL;
    
    TypeInfo* inferred_type = NULL;
    
    switch (body->kind) {
        case AST_BLOCK: {
            // 遍历块中的所有语句，查找 return 语句
            for (int i = 0; i < body->u.block.count; i++) {
                Ast* stmt = body->u.block.items[i];
                if (stmt->kind == AST_RETURN && stmt->u.ret) {
                    TypeInfo* ret_type = infer_expr_type(s, stmt->u.ret);
                    if (ret_type) {
                        if (!inferred_type) {
                            inferred_type = ret_type;
                        } else if (type_is_compatible(inferred_type, ret_type)) {
                            type_free(ret_type);
                        } else {
                            // 类型不兼容，返回 any
                            type_free(inferred_type);
                            type_free(ret_type);
                            return type_new(TYPE_ANY);
                        }
                    }
                }
            }
            break;
        }
        case AST_RETURN: {
            if (body->u.ret) {
                inferred_type = infer_expr_type(s, body->u.ret);
            }
            break;
        }
        default:
            break;
    }
    
    return inferred_type;
}

// ============================================================================
// 类型推断
// ============================================================================

TypeInfo* infer_expr_type(Semantic* s, Ast* ast) {
    if (!ast) return type_new(TYPE_ANY);
    
    // 检查类型缓存，避免重复推断同一表达式
    if (ast->cached_type) {
        return type_copy(ast->cached_type);
    }
    
    TypeInfo* result = NULL;
    
    switch (ast->kind) {
        case AST_NUM: {
            if (ast->u.num.is_bigint) {
                ast->cached_type = type_new(TYPE_BIGINT);
                return type_copy(ast->cached_type);
            }
            // 根据原始字面量是否有小数点来判断类型
            if (ast->u.num.is_float) {
                ast->cached_type = type_new(TYPE_FLOAT);
            } else {
                ast->cached_type = type_new(TYPE_INT);
            }
            return type_copy(ast->cached_type);
        }
        case AST_STRING:
            ast->cached_type = type_new(TYPE_STRING);
            return type_copy(ast->cached_type);
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                if (ast->u.interp_string.exprs[i]) {
                    TypeInfo* expr_type = infer_expr_type(s, ast->u.interp_string.exprs[i]);
                    if (expr_type) {
                        // 检查插值表达式类型是否可转换为字符串
                        // 允许：string, int, float, bool, null, bigint, array, dict
                        // 所有类型都可以通过运行时 toString 转换为字符串
                        (void)expr_type; // 避免未使用警告
                        type_free(expr_type);
                    }
                }
            }
            ast->cached_type = type_new(TYPE_STRING);
            return type_copy(ast->cached_type);
        case AST_BOOL:
            ast->cached_type = type_new(TYPE_BOOL);
            return type_copy(ast->cached_type);
        case AST_NULL:
            ast->cached_type = type_new(TYPE_NULL);
            return type_copy(ast->cached_type);
        case AST_ARRAY: {
            TypeInfo* result = NULL;
            if (ast->u.array.count == 0) {
                // 空数组推断为 Array（元素类型未指定）
                result = type_array(NULL);
            } else {
                TypeInfo* element_type = NULL;
                for (int i = 0; i < ast->u.array.count; i++) {
                    TypeInfo* elem_type = infer_expr_type(s, ast->u.array.items[i]);
                    if (!element_type) {
                        element_type = elem_type;
                    } else if (type_equals(element_type, elem_type)) {
                        // 类型相同，继续
                        type_free(elem_type);
                    } else {
                        // 类型不同，尝试类型提升
                        int promoted = 0;
                        
                        // int + float -> float
                        if ((element_type->kind == TYPE_INT && elem_type->kind == TYPE_FLOAT) ||
                            (element_type->kind == TYPE_FLOAT && elem_type->kind == TYPE_INT)) {
                            TypeInfo* new_type = type_new(TYPE_FLOAT);
                            type_free(element_type);
                            type_free(elem_type);
                            element_type = new_type;
                            promoted = 1;
                        }
                        // int/float + bigint -> bigint
                        else if ((element_type->kind == TYPE_INT && elem_type->kind == TYPE_BIGINT) ||
                                 (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_INT) ||
                                 (element_type->kind == TYPE_FLOAT && elem_type->kind == TYPE_BIGINT) ||
                                 (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_FLOAT)) {
                            TypeInfo* new_type = type_new(TYPE_BIGINT);
                            type_free(element_type);
                            type_free(elem_type);
                            element_type = new_type;
                            promoted = 1;
                        }
                        // float/bigint + bigint/float 已经是 BIGINT
                        else if (element_type->kind == TYPE_BIGINT && elem_type->kind == TYPE_BIGINT) {
                            type_free(elem_type);
                            promoted = 1;
                        }
                        
                        if (!promoted) {
                            // 无法类型提升，返回 any[]
                            type_free(elem_type);
                            type_free(element_type);
                            result = type_array(type_new(TYPE_ANY));
                            break;
                        }
                    }
                }
                if (!result) {
                    if (!element_type) {
                        element_type = type_new(TYPE_ANY);
                    }
                    result = type_array(element_type);
                }
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_DICT: {
            TypeInfo* result = NULL;
            if (ast->u.dict.count == 0) {
                // 空字典返回 NULL 键值类型（类似空数组），允许后续类型推断
                result = type_dict(NULL, NULL);
            } else {
                TypeInfo* key_type = type_new(TYPE_STRING);
                TypeInfo* value_type = NULL;
                for (int i = 0; i < ast->u.dict.count; i++) {
                    TypeInfo* curr_value = infer_expr_type(s, ast->u.dict.entries[i].value);
                    if (!value_type) {
                        value_type = curr_value;
                    } else {
                        if (!type_equals(value_type, curr_value)) {
                            type_free(value_type);
                            value_type = type_new(TYPE_ANY);
                        }
                        type_free(curr_value);
                    }
                }
                if (!value_type) value_type = type_new(TYPE_ANY);
                result = type_dict(key_type, value_type);
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_VAR: {
            if (ast->cached_type) {
                return type_copy(ast->cached_type);
            }
            Symbol* sym = scope_resolve(s->current, ast->u.var.name);
            if (sym && sym->type) {
                ast->cached_type = type_copy(sym->type);
                return type_copy(sym->type);
            }
            if (ast->u.var.ref.type_kind != TYPE_ANY && ast->u.var.ref.type_kind != TYPE_INFER) {
                ast->cached_type = type_new(ast->u.var.ref.type_kind);
                if (ast->u.var.ref.struct_name &&
                    (ast->u.var.ref.type_kind == TYPE_STRUCT ||
                     ast->u.var.ref.type_kind == TYPE_FACE ||
                     ast->u.var.ref.type_kind == TYPE_CSTRUCT ||
                     ast->u.var.ref.type_kind == TYPE_ENUM)) {
                    ast->cached_type->struct_name = strdup(ast->u.var.ref.struct_name);
                }
            } else {
                ast->cached_type = type_new(TYPE_ANY);
            }
            return type_copy(ast->cached_type);
        }
        case AST_BINOP: {
            TypeInfo* left = infer_expr_type(s, ast->u.binop.l);
            TypeInfo* right = infer_expr_type(s, ast->u.binop.r);
            TypeInfo* result = NULL;

            switch (ast->u.binop.op) {
                case TOK_PLUS: {
                    // 字符串拼接：任一操作数是 string 时结果为 string
                    if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        result = type_new(TYPE_STRING);
                    }
                    // null 参与加法 -> any（运行时错误）
                    else if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        result = type_new(TYPE_ANY);
                    }
                    // 如果任一操作数是 ANY，结果是 ANY
                    else if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与加法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 数值类型推导：int + int = int, int + float = float, float + float = float
                    // bigint + int/float/bigint = bigint
                    else if (left && right) {
                        if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else {
                            result = type_new(TYPE_FLOAT);
                        }
                    } else {
                        result = type_new(TYPE_ANY);
                    }
                    break;
                }
                case TOK_MINUS:
                case TOK_STAR: {
                    // null 参与减法/乘法 -> any
                    if (left && right &&
                        (left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        result = type_new(TYPE_ANY);
                    }
                    // 如果任一操作数是 ANY，结果是 ANY
                    else if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与减法/乘法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与减法/乘法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 数值类型推导
                    else if (left && right) {
                        if (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT) {
                            result = type_new(TYPE_BIGINT);
                        } else if (left->kind == TYPE_INT && right->kind == TYPE_INT) {
                            result = type_new(TYPE_INT);
                        } else {
                            result = type_new(TYPE_FLOAT);
                        }
                    } else {
                        result = type_new(TYPE_ANY);
                    }
                    break;
                }
                case TOK_SLASH:
                    // 如果任一操作数是 ANY 或 NULL，结果是 ANY
                    if (left && right &&
                        (left->kind == TYPE_ANY || right->kind == TYPE_ANY ||
                         left->kind == TYPE_NULL || right->kind == TYPE_NULL)) {
                        result = type_new(TYPE_ANY);
                    }
                    // bool 参与除法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与除法 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // int / int = int
                    else if (left && right &&
                             (left->kind == TYPE_INT && right->kind == TYPE_INT)) {
                        result = type_new(TYPE_INT);
                    }
                    // float involved → float (int/float, bigint/float, etc.)
                    else if (left && right &&
                             (left->kind == TYPE_FLOAT || right->kind == TYPE_FLOAT)) {
                        result = type_new(TYPE_FLOAT);
                    }
                    // bigint involved → bigint (bigint/int, bigint/bigint)
                    else if (left && right &&
                             (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT)) {
                        result = type_new(TYPE_BIGINT);
                    }
                    else {
                        result = type_new(TYPE_FLOAT);
                    }
                    break;
                case TOK_MOD:
                case TOK_BITAND:
                case TOK_BITOR:
                case TOK_BITXOR:
                case TOK_SHL:
                case TOK_SHR:
                    // bool 参与位运算/取模 -> 编译错误
                    if (left && right &&
                        (left->kind == TYPE_BOOL || right->kind == TYPE_BOOL)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "bool 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // string 参与位运算/取模 -> 编译错误
                    else if (left && right &&
                        (left->kind == TYPE_STRING || right->kind == TYPE_STRING)) {
                        error_add(ERR_TYPE_MISMATCH, ast->line,
                                  "string 类型不能参与算术运算");
                        result = type_new(TYPE_ANY);
                    }
                    // 位运算和取模：如果任一操作数是 bigint，返回 bigint
                    else if (left && right &&
                        (left->kind == TYPE_BIGINT || right->kind == TYPE_BIGINT)) {
                        result = type_new(TYPE_BIGINT);
                    }
                    // 否则返回整数
                    else {
                        result = type_new(TYPE_INT);
                    }
                    break;
                case TOK_LT:
                case TOK_GT:
                case TOK_LE:
                case TOK_GE:
                    // 大小比较：检查类型兼容性
                    if (left && right &&
                        left->kind != TYPE_ANY && right->kind != TYPE_ANY) {
                        // 允许：int/float 之间比较
                        int left_is_num = (left->kind == TYPE_INT || left->kind == TYPE_FLOAT || left->kind == TYPE_BIGINT);
                        int right_is_num = (right->kind == TYPE_INT || right->kind == TYPE_FLOAT || right->kind == TYPE_BIGINT);
                        // 允许：string 和 string 比较
                        int left_is_string = (left->kind == TYPE_STRING);
                        int right_is_string = (right->kind == TYPE_STRING);
                        // 检查不兼容的组合
                        if ((left_is_num && right_is_string) || (left_is_string && right_is_num)) {
                            error_add(ERR_TYPE_MISMATCH, ast->line,
                                      "数值类型不能和 string 类型进行大小比较");
                        } else if (left_is_string && right_is_string) {
                            // string 和 string 比较是允许的
                        } else if (!left_is_num || !right_is_num) {
                            // 其他不兼容类型
                            error_add(ERR_TYPE_MISMATCH, ast->line,
                                      "不兼容的类型不能进行大小比较");
                        }
                    }
                    result = type_new(TYPE_BOOL);
                    break;
                case TOK_EQEQ:
                case TOK_NEQ:
                case TOK_AND:
                case TOK_OR:
                    result = type_new(TYPE_BOOL);
                    break;
                default:
                    result = type_new(TYPE_ANY);
                    break;
            }
            type_free(left);
            type_free(right);
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_UNARY: {
            TypeInfo* operand = infer_expr_type(s, ast->u.unary.operand);
            TypeInfo* result = NULL;
            if (ast->u.unary.op == TOK_NOT) {
                type_free(operand);
                result = type_new(TYPE_BOOL);
            } else if (ast->u.unary.op == TOK_MINUS) {
                // 负号：bool 参与 -> 编译错误
                if (operand && operand->kind == TYPE_BOOL) {
                    error_add(ERR_TYPE_MISMATCH, ast->line,
                              "bool 类型不能参与算术运算");
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 负号：string 参与 -> 编译错误
                else if (operand && operand->kind == TYPE_STRING) {
                    error_add(ERR_TYPE_MISMATCH, ast->line,
                              "string 类型不能参与算术运算");
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 负号：null 参与 -> any
                else if (operand && operand->kind == TYPE_NULL) {
                    type_free(operand);
                    result = type_new(TYPE_ANY);
                }
                // 保持原有数值类型
                else {
                    result = operand;
                }
            } else {
                result = operand;
            }
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_CALL: {
            // 先对所有参数进行类型推断（触发参数中的类型检查）
            for (int i = 0; i < ast->u.call.args.count; i++) {
                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                if (arg_type) type_free(arg_type);
            }

            if (ast->u.call.callee && ast->u.call.callee->kind == AST_VAR) {
                const char* func_name = ast->u.call.callee->u.var.name;

                // 首先检查变量符号的类型（可能是函数类型）
                Symbol* sym = scope_resolve(s->current, func_name);
                if (sym && sym->type && sym->type->kind == TYPE_FUNCTION) {
                    // 从函数类型中获取返回类型
                    if (sym->type->return_type) {
                        ast->cached_type = type_copy(sym->type->return_type);
                        return type_copy(ast->cached_type);
                    }
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                }

                // 使用哈希表 O(1) 查找函数定义
                Ast* func_def = func_table_find(&s->func_table, func_name);

                if (func_def && func_def->kind == AST_FUNC_DEF) {
                    if (func_def->u.func.return_type) {
                        ast->cached_type = type_copy(func_def->u.func.return_type);
                        return type_copy(ast->cached_type);
                    }
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                }

                // 找不到函数定义时，检查变量符号的类型
                if (sym && sym->type) {
                    ast->cached_type = type_copy(sym->type);
                    return type_copy(ast->cached_type);
                }

                TypeKind return_type = native_get_return_type(func_name);
                ast->cached_type = type_new(return_type);
                return type_copy(ast->cached_type);
            }
            // 处理实例方法调用：obj.method()
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
                Ast* index_ast = ast->u.call.callee;
                if (index_ast->u.index.index && index_ast->u.index.index->kind == AST_STRING) {
                    const char* method_name = index_ast->u.index.index->u.string.value;
                    
                    // 推断对象类型
                    TypeInfo* obj_type = infer_expr_type(s, index_ast->u.index.obj);
                    if (obj_type) {
                        // 处理 struct/cstruct 类型的方法调用
                        if (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE) {
                            if (obj_type->kind == TYPE_FACE) {
                                ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                                if (fdef) {
                                    for (int mi = 0; mi < fdef->method_count; mi++) {
                                        if (strcmp(fdef->methods[mi].name, method_name) == 0) {
                                            if (fdef->methods[mi].return_type) {
                                                ast->cached_type = type_copy(fdef->methods[mi].return_type);
                                            } else {
                                                ast->cached_type = type_new(TYPE_ANY);
                                            }
                                            type_free(obj_type);
                                            return type_copy(ast->cached_type);
                                        }
                                    }
                                }
                                // face 可能定义在导入的模块中，从模块符号表查找方法返回类型
                                for (int mi = 0; mi < s->imported_module_count; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->struct_name);
                                        if (face_sym) {
                                            for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                    TypeInfo* ret_type = type_new(face_sym->methods[fi].return_type);
                                                    if (face_sym->methods[fi].return_type == TYPE_STRUCT && face_sym->methods[fi].return_struct_name) {
                                                        ret_type->struct_name = strdup(face_sym->methods[fi].return_struct_name);
                                                    }
                                                    // 检查返回类型是否是 face
                                                    if (face_sym->methods[fi].return_struct_name) {
                                                        ModuleFaceSymbol* ret_face = module_symbol_table_find_face(m->sym_table, face_sym->methods[fi].return_struct_name);
                                                        if (ret_face) {
                                                            ret_type->kind = TYPE_FACE;
                                                            ret_type->struct_name = strdup(ret_face->name);
                                                        }
                                                    }
                                                    ast->cached_type = ret_type;
                                                    type_free(obj_type);
                                                    return type_copy(ast->cached_type);
                                                }
                                            }
                                        }
                                    }
                                }
                                type_free(obj_type);
                                return type_new(TYPE_ANY);
                            }
                            // 从函数表查找方法定义（用户定义的方法）
                            // 使用 struct_name::method_name 格式，避免与全局函数冲突
                            char method_key[256];
                            if (obj_type->struct_name) {
                                snprintf(method_key, sizeof(method_key), "%s::%s", obj_type->struct_name, method_name);
                            } else {
                                strncpy(method_key, method_name, sizeof(method_key) - 1);
                                method_key[sizeof(method_key) - 1] = '\0';
                            }
                            Ast* func_def = func_table_find(&s->func_table, method_key);
                            if (func_def && func_def->kind == AST_FUNC_DEF) {
                                if (func_def->u.func.return_type && func_def->u.func.return_type->kind != TYPE_INFER) {
                                    ast->cached_type = type_copy(func_def->u.func.return_type);
                                    type_free(obj_type);
                                    return type_copy(ast->cached_type);
                                }
                                // 尝试推断返回类型从 return 语句
                                if (func_def->u.func.body) {
                                    TypeInfo* inferred_return = infer_return_type_from_body(s, func_def->u.func.body);
                                    if (inferred_return) {
                                        ast->cached_type = type_copy(inferred_return);
                                        type_free(inferred_return);
                                        type_free(obj_type);
                                        return type_copy(ast->cached_type);
                                    }
                                }
                                ast->cached_type = type_new(TYPE_ANY);
                                type_free(obj_type);
                                return type_copy(ast->cached_type);
                            }
                            
                            // 检查是否是原生方法（如 copy, malloc_array）
                            int arity;
                            const char* type_name = (obj_type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
                            TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);

                            // 如果返回类型是结构体（如 copy, malloc_array 方法），应该与对象类型相同
                            if ((return_type == TYPE_STRUCT || return_type == TYPE_CSTRUCT) &&
                                (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT)) {
                                // 复制对象类型作为返回类型
                                TypeInfo* result = type_copy(obj_type);
                                type_free(obj_type);
                                return result;
                            }

                            if (return_type != TYPE_ANY) {
                                type_free(obj_type);
                                return type_new(return_type);
                            }

                            // 检查是否是从模块导入的 struct 的方法
                            if (obj_type->struct_name) {
                                // 遍历所有导入的模块，查找 struct 方法
                                for (int i = 0; i < s->imported_module_count; i++) {
                                    ImportedModuleInfo* info = &s->imported_modules[i];
                                    if (info->sym_table) {
                                        ModuleStructMethod* method = module_symbol_table_find_struct_method(
                                            info->sym_table, obj_type->struct_name, method_name);
                                        if (method) {
                                            TypeInfo* result = type_new(method->return_type);
                                            if (method->return_type == TYPE_STRUCT && method->return_struct_name) {
                                                result->struct_name = strdup(method->return_struct_name);
                                            }
                                            type_free(obj_type);
                                            return result;
                                        }
                                    }
                                }
                            }

                            type_free(obj_type);
                            return type_new(TYPE_ANY);
                        }
                        
                        const char* type_name = native_get_type_name(obj_type->kind);
                        
                        // 特殊处理 cstruct 数组类型
                        if (obj_type->kind == TYPE_CSTRUCT && obj_type->struct_name && 
                            strcmp(obj_type->struct_name, "__CSTRUCT_ARRAY__") == 0) {
                            // cstruct 数组类型使用 cstruct 的方法表
                            type_name = "cstruct";
                        }
                        
                        if (type_name) {
                            // 获取实例方法的返回类型
                            int arity;
                            TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);
                            
                            // 如果返回类型是数组（如 copy 方法），应该与对象类型相同
                            if (return_type == TYPE_ARRAY && obj_type->kind == TYPE_ARRAY) {
                                // 复制对象类型作为返回类型
                                TypeInfo* result = type_copy(obj_type);
                                type_free(obj_type);

                                return result;
                            }

                            type_free(obj_type);
                            return type_new(return_type);
                        }
                        type_free(obj_type);
                    }
                }
            }
            return type_new(TYPE_ANY);
        }
        
        case AST_MODULE_CALL: {
            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            // 检查是否是原生模块
            int is_native_module = native_is_module(actual_module);

            if (is_native_module) {
                TypeKind return_type = native_get_module_method_return_type(
                    actual_module,
                    ast->u.module_call.method_name);
                return type_new(return_type);
            }

            // 检查是否是导入的用户模块
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_call.module_name);
            if (module_info && module_info->file_path && module_info->sym_table) {
                const char* method_name = ast->u.module_call.method_name;

                ModuleFuncSymbol* func = module_symbol_table_find_func(module_info->sym_table, method_name);
                if (func) {
                    TypeInfo* type = type_new(func->return_type);
                    if (func->return_struct_name) {
                        // 检查返回类型是否是模块中定义的 face
                        ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, func->return_struct_name);
                        if (face_sym) {
                            type->kind = TYPE_FACE;
                            type->struct_name = strdup(face_sym->name);
                        } else if (func->return_type == TYPE_STRUCT) {
                            type->struct_name = strdup(func->return_struct_name);
                        }
                    }
                    return type;
                }

                // 检查是否是 struct 初始化（如 new module.Point()）
                ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, method_name);
                if (struct_sym) {
                    TypeInfo* type = type_new(TYPE_STRUCT);
                    type->struct_name = strdup(struct_sym->name);
                    return type;
                }

                // 如果没有找到，返回 TYPE_ANY
                return type_new(TYPE_ANY);
            }

            // 不是模块调用，可能是实例方法调用（如 arr1.copy()）
            // 查找变量
            Symbol* obj_sym = scope_resolve(s->current, ast->u.module_call.module_name);
            if (obj_sym && obj_sym->type) {
                const char* type_name = native_get_type_name(obj_sym->type->kind);
                if (type_name) {
                    const char* method_name = ast->u.module_call.method_name;
                    int arity;
                    TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &arity);

                    // 如果返回类型是数组（如 copy 方法），应该与对象类型相同
                    if (return_type == TYPE_ARRAY && obj_sym->type->kind == TYPE_ARRAY) {
                        // 复制对象类型作为返回类型
                        return type_copy(obj_sym->type);
                    }

                    // 如果返回类型是结构体（如 copy 方法），应该与对象类型相同
                    if (return_type == TYPE_STRUCT && obj_sym->type->kind == TYPE_STRUCT) {
                        // 复制对象类型作为返回类型
                        return type_copy(obj_sym->type);
                    }

                    return type_new(return_type);
                }
            }

            return type_new(TYPE_ANY);
        }
        case AST_MODULE_ACCESS: {
            // 模块成员访问：尝试查找导入的模块
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_access.module_name);
            if (module_info) {
                // 检查是否是 enum 成员访问（如 math_enum.Color.RED）
                // module_name 是模块别名，member_name 是 enum 名或函数名
                if (module_info->sym_table) {
                    // 检查是否是 enum 名
                    ModuleEnumSymbol* enum_sym = module_symbol_table_find_enum(module_info->sym_table, ast->u.module_access.member_name);
                    if (enum_sym) {
                        // 这是 enum 名，返回 enum 类型
                        // 实际的成员访问会通过 AST_INDEX 处理
                        TypeInfo* enum_type = type_new(TYPE_ENUM);
                        enum_type->struct_name = strdup(enum_sym->name);
                        ast->cached_type = enum_type;
                        return type_copy(ast->cached_type);
                    }

                    // 检查是否是变量名
                    ModuleVarSymbol* var_sym = module_symbol_table_find_var(module_info->sym_table, ast->u.module_access.member_name);
                    if (var_sym) {
                        // 这是变量，返回变量类型
                        TypeInfo* var_type = type_new(var_sym->type);
                        if (var_sym->type == TYPE_STRUCT && var_sym->struct_name) {
                            var_type->struct_name = strdup(var_sym->struct_name);
                        }
                        ast->cached_type = var_type;
                        return type_copy(ast->cached_type);
                    }
                }
                // 用户模块的其他成员，无法静态推导类型，返回 ANY
                return type_new(TYPE_ANY);
            }
            // 原生模块的成员访问
            const char* actual_module = native_resolve_module_alias(ast->u.module_access.module_name);
            if (native_is_module(actual_module)) {
                // 原生模块成员（如 maths.PI），返回 ANY（无法静态确定）
                return type_new(TYPE_ANY);
            }
            // 检查是否是当前文件中定义的 enum 访问（如 Color.green）
            Symbol* enum_type_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            if (enum_type_sym && enum_type_sym->kind == SYM_ENUM) {
                // 这是当前文件中定义的 enum 类型，enum 成员值是 int 类型
                // 检查 member_name 是否是该 enum 的有效成员
                TypeInfo* enum_type = type_new(TYPE_ENUM);
                enum_type->struct_name = strdup(ast->u.module_access.module_name);
                ast->cached_type = enum_type;
                return type_copy(ast->cached_type);
            }
            // 可能是变量属性访问
            Symbol* var_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            if (var_sym && var_sym->type) {
                if (var_sym->type->kind == TYPE_DICT && var_sym->type->value_type) {
                    return type_copy(var_sym->type->value_type);
                }
            }
            return type_new(TYPE_ANY);
        }
        case AST_INDEX: {
            // 检查是否有字段类型守卫收窄（如 if s.age is int）
            // dict 的 s.age 语法被解析为 AST_INDEX（obj=AST_VAR("s"), index=AST_STRING("age")）
            if (ast->u.index.obj && ast->u.index.obj->kind == AST_VAR &&
                ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                const char* var_name = ast->u.index.obj->u.var.name;
                const char* field_name = ast->u.index.index->u.string.value;
                int guard_name_len = strlen(var_name) + strlen(field_name) + 2;
                char* guard_name = (char*)malloc(guard_name_len);
                snprintf(guard_name, guard_name_len, "%s.%s", var_name, field_name);
                Symbol* guard_sym = scope_resolve(s->current, guard_name);
                free(guard_name);
                if (guard_sym && guard_sym->type) {
                    result = type_copy(guard_sym->type);
                    return result;
                }
            }

            TypeInfo* obj_type = infer_expr_type(s, ast->u.index.obj);
            if (obj_type) {
                if (obj_type->kind == TYPE_ARRAY) {
                    // 返回数组的元素类型
                    if (obj_type->element_type) {
                        ast->cached_type = type_copy(obj_type->element_type);
                        type_free(obj_type);
                        return type_copy(ast->cached_type);
                    } else {
                        // 数组没有指定元素类型，返回 any
                        type_free(obj_type);
                        ast->cached_type = type_new(TYPE_ANY);
                        return type_copy(ast->cached_type);
                    }
                } else if (obj_type->kind == TYPE_DICT && obj_type->value_type) {
                    ast->cached_type = type_copy(obj_type->value_type);
                    type_free(obj_type);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_STRING) {
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_STRING);
                    return type_copy(ast->cached_type);
                } else if ((obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT) && obj_type->struct_name) {
                    // 处理 struct/cstruct 字段访问：从符号表查找 struct/cstruct 定义并获取字段类型
                    Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (struct_sym && struct_sym->struct_field_names && struct_sym->struct_field_types) {
                        // 获取索引的字段名（应该是字符串字面量）
                        if (ast->u.index.index && ast->u.index.index->kind == AST_STRING) {
                            const char* field_name = ast->u.index.index->u.string.value;
                            for (int i = 0; i < struct_sym->struct_field_count; i++) {
                                if (strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
                                    ast->cached_type = type_copy(struct_sym->struct_field_types[i]);
                                    type_free(obj_type);
                                    return type_copy(ast->cached_type);
                                }
                            }
                        }
                    }

                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_ANY);
                    return type_copy(ast->cached_type);
                } else if (obj_type->kind == TYPE_ENUM) {
                    // enum 成员访问返回 int 或 bigint（根据值的大小）
                    // 这里我们统一返回 int，因为大多数 enum 值都在 int 范围内
                    // 如果值超过 int 范围，运行时会自动处理为 bigint
                    type_free(obj_type);
                    ast->cached_type = type_new(TYPE_INT);
                    return type_copy(ast->cached_type);
                }
                type_free(obj_type);
            }
            ast->cached_type = type_new(TYPE_ANY);
            return type_copy(ast->cached_type);
        }
        case AST_SLICE: {
            // 切片操作返回数组类型，元素类型与原数组相同
            TypeInfo* obj_type = infer_expr_type(s, ast->u.slice.obj);
            TypeInfo* result = NULL;
            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                // 返回相同类型的数组
                result = type_copy(obj_type);
            } else {
                result = type_new(TYPE_ANY);
            }
            if (obj_type) type_free(obj_type);
            ast->cached_type = type_copy(result);
            return result;
        }
        case AST_COMPOUND_ASSIGN: {
            // 使用resolve_variable_with_upvalue处理复合赋值，支持闭包
            SymRef ref;
            memset(&ref, 0, sizeof(ref));
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.compound_assign.name, &ref);
            if (sym && sym->type) {
                result = type_new(sym->type->kind);
            } else {
                result = infer_expr_type(s, ast->u.compound_assign.value);
            }
            break;
        }
        case AST_ASSIGN: {
            // 赋值表达式返回被赋值的类型
            // 对于连续赋值如 x = y = 0，需要推断赋值目标的类型
            if (ast->u.assign.name_count > 0 && ast->u.assign.names[0]) {
                Symbol* sym = scope_resolve(s->current, ast->u.assign.names[0]);
                if (sym && sym->type) {
                    result = type_copy(sym->type);
                } else {
                    result = infer_expr_type(s, ast->u.assign.value);
                }
            } else {
                // 如果无法确定目标类型，返回右侧表达式的类型
                result = infer_expr_type(s, ast->u.assign.value);
            }
            break;
        }
        case AST_INDEX_ASSIGN: {
            // 索引赋值表达式返回被赋值的类型
            // 例如 self["y"] = 0，返回字段 y 的类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index_assign.obj);
            if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
                // 获取字段名
                if (ast->u.index_assign.index->kind == AST_STRING) {
                    const char* field_name = ast->u.index_assign.index->u.string.value;
                    // 查找 struct 定义
                    Symbol* struct_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (struct_sym && struct_sym->struct_field_count > 0) {
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(struct_sym->struct_field_types[i]);
                                type_free(obj_type);
                                break;
                            }
                        }
                    }
                }
            }
            if (!result) {
                if (obj_type) type_free(obj_type);
                // 如果无法确定字段类型，返回右侧表达式的类型
                result = infer_expr_type(s, ast->u.index_assign.value);
            }
            break;
        }
        case AST_TYPE_CHECK:
            // is 表达式返回 bool
            result = type_new(TYPE_BOOL);
            break;
        case AST_AS_CAST: {
            // as 转型表达式：匹配时返回目标类型，不匹配时返回 null
            // 类型推断返回目标类型（与 is 不同，as 的结果类型是目标类型）
            if (ast->u.type_check.type) {
                result = type_copy(ast->u.type_check.type);
            } else {
                result = type_new(TYPE_ANY);
            }
            break;
        }
        case AST_STRUCT_INIT: {
            // struct 构造函数调用返回对应的 struct 类型
            TypeInfo* struct_type = type_new(TYPE_STRUCT);
            // 处理模块限定的 struct 名称（如 "math.Point"），提取实际的 struct 名称
            const char* dot_pos = strchr(ast->u.struct_init.struct_name, '.');
            struct_type->struct_name = strdup(dot_pos ? dot_pos + 1 : ast->u.struct_init.struct_name);
            result = struct_type;
            break;
        }
        case AST_FIELD_ACCESS: {
            // 字段访问：需要知道对象的类型和字段的类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.field_access.obj);

            // 初始化字段索引为 -1（未确定）
            ast->u.field_access.field_index = -1;

            // 检查是否有字段类型守卫收窄（如 if s.age is int）
            if (ast->u.field_access.obj->kind == AST_VAR) {
                const char* var_name = ast->u.field_access.obj->u.var.name;
                const char* field_name = ast->u.field_access.field_name;
                int guard_name_len = strlen(var_name) + strlen(field_name) + 2;
                char* guard_name = (char*)malloc(guard_name_len);
                snprintf(guard_name, guard_name_len, "%s.%s", var_name, field_name);
                Symbol* guard_sym = scope_resolve(s->current, guard_name);
                free(guard_name);
                if (guard_sym && guard_sym->type) {
                    result = type_copy(guard_sym->type);
                    if (obj_type) type_free(obj_type);
                    break;
                }
            }

            if (obj_type && obj_type->kind == TYPE_STRUCT) {
                // 获取字段名
                const char* field_name = ast->u.field_access.field_name;

                // 从对象类型获取 struct 类型名称，查找 struct 定义
                if (obj_type->struct_name) {
                    Symbol* struct_def_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (struct_def_sym && struct_def_sym->struct_field_count > 0) {
                        // 在 struct 定义中查找字段类型和索引
                        for (int i = 0; i < struct_def_sym->struct_field_count; i++) {
                            if (strcmp(struct_def_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(struct_def_sym->struct_field_types[i]);
                                ast->u.field_access.field_index = i; // 存储字段索引
                                break;
                            }
                        }
                    }
                }

                // 如果没找到字段类型，尝试从变量符号查找
                if (!result && ast->u.field_access.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.field_access.obj->u.var.name;
                    Symbol* var_sym = scope_resolve(s->current, var_name);
                    if (var_sym && var_sym->struct_field_count > 0) {
                        for (int i = 0; i < var_sym->struct_field_count; i++) {
                            if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(var_sym->struct_field_types[i]);
                                ast->u.field_access.field_index = i; // 存储字段索引
                                break;
                            }
                        }
                    }
                }

                // 如果还是没找到，返回 ANY
                if (!result) {
                    result = type_new(TYPE_ANY);
                }
            }
            // 处理 cstruct 字段访问
            else if (obj_type && obj_type->kind == TYPE_CSTRUCT) {
                // 获取字段名
                const char* field_name = ast->u.field_access.field_name;
                
                // 从对象类型获取 cstruct 类型名称，查找 cstruct 定义
                if (obj_type->struct_name) {
                    Symbol* cstruct_def_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (cstruct_def_sym && cstruct_def_sym->struct_field_count > 0) {
                        // 在 cstruct 定义中查找字段类型和索引
                        for (int i = 0; i < cstruct_def_sym->struct_field_count; i++) {
                            if (strcmp(cstruct_def_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(cstruct_def_sym->struct_field_types[i]);
                                ast->u.field_access.field_index = i; // 存储字段索引
                                break;
                            }
                        }
                    }
                }
                
                // 如果没找到字段类型，尝试从变量符号查找
                if (ast->u.field_access.field_index < 0 && ast->u.field_access.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.field_access.obj->u.var.name;
                    Symbol* var_sym = scope_resolve(s->current, var_name);
                    if (var_sym && var_sym->struct_field_count > 0) {
                        for (int i = 0; i < var_sym->struct_field_count; i++) {
                            if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                result = type_copy(var_sym->struct_field_types[i]);
                                ast->u.field_access.field_index = i; // 存储字段索引
                                break;
                            }
                        }
                    }
                }
                
                // 如果还是没找到，返回 ANY
                if (ast->u.field_access.field_index < 0) {
                    result = type_new(TYPE_ANY);
                }
            }
            else {
                result = type_new(TYPE_ANY);
            }
            if (obj_type) type_free(obj_type);
            break;
        }
        case AST_FUNC_DEF: {
            // 匿名函数表达式：根据返回类型注解构建函数类型
            TypeInfo* return_type = (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER)
                ? type_copy(ast->u.func.return_type)
                : type_new(TYPE_ANY);
            
            // 构建参数类型数组
            TypeInfo** param_types = NULL;
            if (ast->u.func.pcnt > 0) {
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    if (ast->u.func.param_types && ast->u.func.param_types[i]) {
                        param_types[i] = type_copy(ast->u.func.param_types[i]);
                    } else {
                        param_types[i] = type_new(TYPE_ANY);
                    }
                }
            }
            
            result = type_function(return_type, param_types, ast->u.func.pcnt);
            break;
        }
        default:
            result = type_new(TYPE_ANY);
            break;
    }
    
    // 缓存推断结果（简单类型直接缓存，复杂类型不缓存以避免内存问题）
    if (result && ast->kind != AST_CALL && ast->kind != AST_INDEX && ast->kind != AST_SLICE) {
        ast->cached_type = type_copy(result);
    }

    return result;
}
