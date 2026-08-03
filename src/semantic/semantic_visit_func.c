#include "semantic_internal.h"

// ============================================================================
// 辅助：递归检查 TypeInfo 中是否存在未定义的类型（在 resolve_alias_in_type 之后调用）
// 此时所有 alias 已被解析，剩余的 TYPE_STRUCT 只能是合法 struct 或未定义类型
// ============================================================================
static void check_undefined_type(Semantic* s, TypeInfo* type, int line) {
    if (!type) return;

    if (type->kind == TYPE_STRUCT && type->struct_name) {
        // 在作用域中查找
        Symbol* struct_def = scope_resolve_local(s->current, type->struct_name);
        if (!struct_def && s->current) {
            struct_def = scope_resolve(s->current, type->struct_name);
        }
        if (!struct_def) {
            // 完全找不到 → 未定义的类型
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "未定义的类型: %s（请检查是否已通过 use 语句导入该类型，如 use module.%s）", type->struct_name, type->struct_name);
            error_add(ERR_SEMANTIC, line, msg);
        }
        // 找到了就是合法的 struct 类型（alias 已在 resolve_alias_in_type 中解析）
    }

    // 递归检查子类型
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            check_undefined_type(s, type->generic_args[i], line);
        }
    }
    check_undefined_type(s, type->element_type, line);
    check_undefined_type(s, type->key_type, line);
    check_undefined_type(s, type->value_type, line);
    check_undefined_type(s, type->return_type, line);
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            check_undefined_type(s, type->param_types[i], line);
        }
    }
}

// 递归解析 TypeInfo 中的 alias 类型（在 resolve_generic_in_type 之后调用）
// 返回 1 表示有修改，0 表示无修改
int resolve_alias_in_type(Semantic* s, TypeInfo** type_ptr, int line) {
    if (!type_ptr || !*type_ptr) return 0;
    TypeInfo* type = *type_ptr;
    int changed = 0;

    if (type->kind == TYPE_STRUCT && type->struct_name) {
        if (face_def_find(type->struct_name)) {
            type->kind = TYPE_FACE;
            changed = 1;
        } else {
            Symbol* struct_def = scope_resolve_local(s->current, type->struct_name);
            if (!struct_def && s->current) {
                struct_def = scope_resolve(s->current, type->struct_name);
            }
            if (struct_def && struct_def->kind == SYM_TYPE && struct_def->type) {
                // 是 alias（如 FSize = Dict[string,float]），解析为实际类型
                type_free(type);
                *type_ptr = type_copy(struct_def->type);
                changed = 1;
            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                type->kind = TYPE_CSTRUCT;
                changed = 1;
            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CLIB) {
                type->kind = TYPE_CLIB;
                changed = 1;
            }
        }
    }

    // 递归处理子类型
    TypeInfo* t = *type_ptr;
    if (t->generic_args) {
        for (int i = 0; i < t->generic_count; i++) {
            changed |= resolve_alias_in_type(s, &t->generic_args[i], line);
        }
    }
    changed |= resolve_alias_in_type(s, &t->element_type, line);
    changed |= resolve_alias_in_type(s, &t->key_type, line);
    changed |= resolve_alias_in_type(s, &t->value_type, line);
    changed |= resolve_alias_in_type(s, &t->return_type, line);
    if (t->param_types) {
        for (int i = 0; i < t->param_count; i++) {
            changed |= resolve_alias_in_type(s, &t->param_types[i], line);
        }
    }
    return changed;
}

// ============================================================================
// 辅助：将 AST 中的 TYPE_STRUCT 泛型参数名转换为 TYPE_GENERIC_PARAM
// ============================================================================

// 解析单个 TypeInfo 中的泛型参数
void resolve_generic_in_type(TypeInfo* type, char** type_params, char** type_param_constraints, int count) {
    if (!type) return;
    if (type->kind == TYPE_STRUCT && type->struct_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(type->struct_name, type_params[i]) == 0) {
                free(type->struct_name);
                type->struct_name = NULL;
                type->kind = TYPE_GENERIC_PARAM;
                type->type_param_name = strdup(type_params[i]);
                if (type_param_constraints && type_param_constraints[i]) {
                    type->constraint_name = strdup(type_param_constraints[i]);
                }
                return;
            }
        }
    }
    // 递归处理子类型
    resolve_generic_in_type(type->element_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->key_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->value_type, type_params, type_param_constraints, count);
    resolve_generic_in_type(type->return_type, type_params, type_param_constraints, count);
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            resolve_generic_in_type(type->param_types[i], type_params, type_param_constraints, count);
        }
    }
    // 递归处理泛型参数（如 Result[T] 中的 T）
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            resolve_generic_in_type(type->generic_args[i], type_params, type_param_constraints, count);
        }
    }
}

// 递归遍历 AST 树，解析所有类型中的泛型参数
static void resolve_generic_in_ast(Semantic* s, Ast* ast, char** type_params, char** type_param_constraints, int count) {
    if (!ast) return;
    
    switch (ast->kind) {
        case AST_VAR_DECL:
            resolve_generic_in_type(ast->u.var_decl.type, type_params, type_param_constraints, count);
            if (ast->u.var_decl.init) resolve_generic_in_ast(s, ast->u.var_decl.init, type_params, type_param_constraints, count);
            break;
        case AST_FUNC_DEF:
            // 嵌套函数：解析其参数和返回类型中的泛型
            for (int i = 0; i < ast->u.func.pcnt; i++) {
                resolve_generic_in_type(ast->u.func.param_types[i], type_params, type_param_constraints, count);
            }
            resolve_generic_in_type(ast->u.func.return_type, type_params, type_param_constraints, count);
            if (ast->u.func.body) resolve_generic_in_ast(s, ast->u.func.body, type_params, type_param_constraints, count);
            break;
        case AST_IF:
            if (ast->u.if_.guard_type) {
                resolve_generic_in_type(ast->u.if_.guard_type, type_params, type_param_constraints, count);
            }
            if (ast->u.if_.guard_conds.count > 0) {
                for (int gi = 0; gi < ast->u.if_.guard_conds.count; gi++) {
                    resolve_generic_in_type(ast->u.if_.guard_conds.items[gi].guard_type, type_params, type_param_constraints, count);
                }
            }
            if (ast->u.if_.cond) resolve_generic_in_ast(s, ast->u.if_.cond, type_params, type_param_constraints, count);
            if (ast->u.if_.then) resolve_generic_in_ast(s, ast->u.if_.then, type_params, type_param_constraints, count);
            if (ast->u.if_.else_) resolve_generic_in_ast(s, ast->u.if_.else_, type_params, type_param_constraints, count);
            break;
        case AST_SWITCH:
            if (ast->u.switch_.expr) resolve_generic_in_ast(s, ast->u.switch_.expr, type_params, type_param_constraints, count);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                if (ast->u.switch_.cases[i].match_type) {
                    resolve_generic_in_type(ast->u.switch_.cases[i].match_type, type_params, type_param_constraints, count);
                }
                if (ast->u.switch_.cases[i].body) {
                    resolve_generic_in_ast(s, ast->u.switch_.cases[i].body, type_params, type_param_constraints, count);
                }
            }
            if (ast->u.switch_.default_body) resolve_generic_in_ast(s, ast->u.switch_.default_body, type_params, type_param_constraints, count);
            break;
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.count; i++) {
                resolve_generic_in_ast(s, ast->u.block.items[i], type_params, type_param_constraints, count);
            }
            break;
        case AST_RETURN:
            if (ast->u.ret) resolve_generic_in_ast(s, ast->u.ret, type_params, type_param_constraints, count);
            break;
        case AST_CALL: {
            Ast* callee = ast->u.call.callee;
            if (callee) resolve_generic_in_ast(s, callee, type_params, type_param_constraints, count);
            for (int i = 0; i < ast->u.call.args.count; i++) {
                resolve_generic_in_ast(s, ast->u.call.args.items[i], type_params, type_param_constraints, count);
            }
            break;
        }
        case AST_EXPR_STMT:
            resolve_generic_in_ast(s, ast->u.expr_stmt.expr, type_params, type_param_constraints, count);
            break;
        case AST_BINOP:
            if (ast->u.binop.l) resolve_generic_in_ast(s, ast->u.binop.l, type_params, type_param_constraints, count);
            if (ast->u.binop.r) resolve_generic_in_ast(s, ast->u.binop.r, type_params, type_param_constraints, count);
            break;
        case AST_UNARY:
            if (ast->u.unary.operand) resolve_generic_in_ast(s, ast->u.unary.operand, type_params, type_param_constraints, count);
            break;
        case AST_FOR:
            if (ast->u.for_.start) resolve_generic_in_ast(s, ast->u.for_.start, type_params, type_param_constraints, count);
            if (ast->u.for_.end) resolve_generic_in_ast(s, ast->u.for_.end, type_params, type_param_constraints, count);
            if (ast->u.for_.body) resolve_generic_in_ast(s, ast->u.for_.body, type_params, type_param_constraints, count);
            break;
        case AST_TRY:
            if (ast->u.try_.try_body) resolve_generic_in_ast(s, ast->u.try_.try_body, type_params, type_param_constraints, count);
            if (ast->u.try_.catch_body) resolve_generic_in_ast(s, ast->u.try_.catch_body, type_params, type_param_constraints, count);
            if (ast->u.try_.finally_body) resolve_generic_in_ast(s, ast->u.try_.finally_body, type_params, type_param_constraints, count);
            break;
        case AST_ASSIGN:
            if (ast->u.assign.value) resolve_generic_in_ast(s, ast->u.assign.value, type_params, type_param_constraints, count);
            break;
        default:
            // 对于其他节点（字面量等），不需要处理
            break;
    }
}

// ============================================================================
// 函数处理 - 单作用域
// ============================================================================

void visit_func_impl(Semantic* s, Ast* ast, int is_struct_method);

void visit_func(Semantic* s, Ast* ast) {
    visit_func_impl(s, ast, 0);
}

void visit_func_as_struct_method(Semantic* s, Ast* ast) {
    visit_func_impl(s, ast, 1);
}

void visit_func_impl(Semantic* s, Ast* ast, int is_struct_method) {
    if (ast->u.func.local_count > 0) {
        return;
    }

    // 将解析为 struct 的泛型类型参数转换为 TYPE_GENERIC_PARAM（递归处理嵌套类型如 Array[T]）
    if (ast->u.func.type_param_count > 0 && ast->u.func.type_params) {
        for (int i = 0; i < ast->u.func.pcnt; i++) {
            resolve_generic_in_type(ast->u.func.param_types[i], ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
        }
        resolve_generic_in_type(ast->u.func.return_type, ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
    }

    // struct 方法：从 self 参数类型获取 struct 的泛型参数并解析
    if (is_struct_method && ast->u.func.pcnt > 0 && ast->u.func.param_types[0]) {
        TypeInfo* self_type = ast->u.func.param_types[0];
        if (self_type->kind == TYPE_STRUCT && self_type->generic_count > 0 && self_type->generic_args) {
            // 构建泛型参数名数组
            int gp_count = self_type->generic_count;
            char** gp_names = (char**)malloc(sizeof(char*) * gp_count);
            for (int i = 0; i < gp_count; i++) {
                if (self_type->generic_args[i]->kind == TYPE_GENERIC_PARAM && self_type->generic_args[i]->type_param_name) {
                    gp_names[i] = strdup(self_type->generic_args[i]->type_param_name);
                } else {
                    gp_names[i] = NULL;
                }
            }
            // 解析方法参数和返回类型中的泛型参数
            for (int i = 1; i < ast->u.func.pcnt; i++) {
                resolve_generic_in_type(ast->u.func.param_types[i], gp_names, NULL, gp_count);
            }
            resolve_generic_in_type(ast->u.func.return_type, gp_names, NULL, gp_count);
            // 保存到 ast 以便后续 resolve_generic_in_ast 使用
            if (!ast->u.func.type_params && !ast->u.func.type_param_count) {
                ast->u.func.type_param_count = gp_count;
                ast->u.func.type_params = gp_names;
            } else {
                for (int i = 0; i < gp_count; i++) free(gp_names[i]);
                free(gp_names);
            }
        }
    }

    // ========== 解析 alias 类型 + 检查未定义类型 ==========
    // 在 resolve_generic_in_type 之后执行，此时泛型参数 T/K/V 已转换为 TYPE_GENERIC_PARAM
    // 剩余的 TYPE_STRUCT 只能是合法 struct、alias 或未定义类型

    // 1. 解析参数类型中的 alias（如 FSize = Dict[string,float]）
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        resolve_alias_in_type(s, &ast->u.func.param_types[i], ast->line);
    }
    // 2. 解析返回类型中的 alias
    resolve_alias_in_type(s, &ast->u.func.return_type, ast->line);

    // 3. 同步更新符号的返回类型（当 alias 或类型被解析时）
    if (ast->u.func.return_type) {
        Symbol* sym = scope_resolve_local(s->current, ast->u.func.name);
        if (!sym && s->current) sym = scope_resolve(s->current, ast->u.func.name);
        if (sym && sym->type && sym->type->kind == TYPE_FUNCTION && sym->type->return_type) {
            type_free(sym->type->return_type);
            sym->type->return_type = type_copy(ast->u.func.return_type);
        }
    }

    // 4. 检查参数类型中是否有未定义的类型
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        check_undefined_type(s, ast->u.func.param_types[i], ast->line);
    }
    // 5. 检查返回类型中是否有未定义的类型
    check_undefined_type(s, ast->u.func.return_type, ast->line);

    // 5.5 检查参数类型和返回类型是否使用了 C 布局类型（i32/u8/f32 等）
    // C 布局类型只能在 clib 声明、cstruct 字段、Ptr[T] 中使用
    // 在 clib/cfunc 上下文中的函数定义不检查（clib 内部允许）
    if (!s->in_clib) {
        for (int i = 0; i < ast->u.func.pcnt; i++) {
            if (ast->u.func.param_types[i] && is_c_layout_type(ast->u.func.param_types[i]->kind)) {
                char msg[BUFFER_MEDIUM];
                const char* type_str = type_kind_to_string(ast->u.func.param_types[i]->kind);
                snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于函数参数，请使用 Leno 类型（如 int/float/string）", type_str);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
        }
        if (ast->u.func.return_type && is_c_layout_type(ast->u.func.return_type->kind)) {
            char msg[BUFFER_MEDIUM];
            const char* type_str = type_kind_to_string(ast->u.func.return_type->kind);
            snprintf(msg, sizeof(msg), "C 布局类型 '%s' 不能用于函数返回值，请使用 Leno 类型（如 int/float/string）", type_str);
            error_add(ERR_SEMANTIC, ast->line, msg);
        }
    }

    // 6. 检查参数类型和返回类型中是否将 var 用作了类型参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        if (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER) {
            TypeKind parent_kind = TYPE_UNKNOWN;
            if (type_has_infer_as_param(ast->u.func.param_types[i], &parent_kind)) {
                char msg[BUFFER_MEDIUM];
                const char* parent_name = type_kind_to_string(parent_kind);
                snprintf(msg, sizeof(msg), "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
        }
    }
    if (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER) {
        TypeKind parent_kind = TYPE_UNKNOWN;
        if (type_has_infer_as_param(ast->u.func.return_type, &parent_kind)) {
            char msg[BUFFER_MEDIUM];
            const char* parent_name = type_kind_to_string(parent_kind);
            snprintf(msg, sizeof(msg), "var 不能用作类型参数，请改用 any（如 %s[any]）", parent_name);
            error_add(ERR_SEMANTIC, ast->line, msg);
        }
    }

    // 7. 检查参数是否使用了 var 类型（参数位置不允许 var，请改用 any）
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        if (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind == TYPE_INFER) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "参数 '%s' 不能使用 var 类型，请改用 any", ast->u.func.params[i]);
            error_add(ERR_SEMANTIC, ast->line, msg);
        }
    }

    // ========== 默认参数语义检查 ==========
    int found_default = 0;  // 标记是否已遇到有默认值的参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        Ast* default_expr = ast->u.func.param_defaults ? ast->u.func.param_defaults[i] : NULL;
        
        if (default_expr) {
            found_default = 1;
            
            // 2.2 检查：默认值只能是字面量常量
            int is_literal = (default_expr->kind == AST_NUM || 
                             default_expr->kind == AST_STRING || 
                             default_expr->kind == AST_BOOL || 
                             default_expr->kind == AST_NULL);
            if (!is_literal) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "参数 '%s' 的默认值必须是字面量常量", ast->u.func.params[i]);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
            
            // 2.3 & 2.4 检查：默认值类型与参数类型匹配
            TypeInfo* param_type = ast->u.func.param_types[i];
            if (param_type && param_type->kind != TYPE_INFER) {
                // 推断默认值类型
                TypeKind default_kind = TYPE_ANY;
                if (default_expr->kind == AST_NUM) {
                    default_kind = default_expr->u.num.is_float ? TYPE_FLOAT : TYPE_INT;
                } else if (default_expr->kind == AST_STRING) {
                    default_kind = TYPE_STRING;
                } else if (default_expr->kind == AST_BOOL) {
                    default_kind = TYPE_BOOL;
                } else if (default_expr->kind == AST_NULL) {
                    default_kind = TYPE_NULL;
                }
                
                // 检查类型兼容性
                if (default_kind != TYPE_ANY && default_kind != TYPE_NULL) {
                    if (!type_is_compatible(param_type, type_new(default_kind))) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "参数 '%s' 的默认值类型 '%s' 与参数类型 '%s' 不匹配",
                                 ast->u.func.params[i],
                                 type_kind_to_string(default_kind),
                                 type_kind_to_string(param_type->kind));
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                }
            } else if (param_type && param_type->kind == TYPE_INFER) {
                // 2.4 var 参数：根据默认值推断类型
                TypeKind inferred_kind = TYPE_ANY;
                if (default_expr->kind == AST_NUM) {
                    inferred_kind = default_expr->u.num.is_float ? TYPE_FLOAT : TYPE_INT;
                } else if (default_expr->kind == AST_STRING) {
                    inferred_kind = TYPE_STRING;
                } else if (default_expr->kind == AST_BOOL) {
                    inferred_kind = TYPE_BOOL;
                } else if (default_expr->kind == AST_NULL) {
                    inferred_kind = TYPE_NULL;
                }
                
                // 更新参数类型为推断的类型
                type_free(param_type);
                ast->u.func.param_types[i] = type_new(inferred_kind);
            }
        } else {
            // 2.1 检查：无默认值的参数不能在有默认值的参数之后
            if (found_default) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "参数 '%s' 没有默认值，但它位于有默认值的参数之后。所有有默认值的参数必须放在参数列表末尾",
                         ast->u.func.params[i]);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
        }
    }

    // 将函数添加到函数表（哈希表自动处理重复）
    // 跳过 struct 方法的注册，因为它们已经在 semantic_visit_ast.c 中以 struct_name::method_name 格式注册
    // 全局函数正常注册；局部函数仅在是 async 且未被全局定义覆盖时注册（确保代码生成能识别 async）
    int is_global_func = s->current && s->current->parent == NULL;
    if (!is_struct_method && is_global_func) {
        func_table_add(&s->func_table, ast->u.func.name, ast);
    } else if (!is_struct_method && !is_global_func && ast->u.func.is_async) {
        // 局部 async 函数：仅当 func_table 中尚无该名称时才注册，避免覆盖全局定义
        Ast* existing_def = func_table_find(&s->func_table, ast->u.func.name);
        if (!existing_def) {
            func_table_add(&s->func_table, ast->u.func.name, ast);
        }
    }

    // 在父作用域注册函数名（如果还没有注册）
    // 注意：函数名可能已经在 AST_BLOCK 的预扫描中注册了
    // 在创建函数作用域之前注册，以便正确判断是否为全局函数
    // 跳过 struct 方法的注册，因为它们不应该在 struct 外部作用域中可见
    if (s->current && !is_struct_method) {
        Symbol* existing = scope_resolve_local(s->current, ast->u.func.name);
        if (!existing) {
            // 判断是否为全局函数：当前作用域的父作用域为 NULL 时表示全局作用域
            SymKind kind = (s->current->parent == NULL) ? SYM_GLOBAL_FUNC : SYM_LOCAL;
            Symbol* sym = scope_define(s->current, ast->u.func.name, kind);
            if (sym) {
                ast->u.func.ref.kind = sym->kind;
                // 局部函数使用函数级别的local_index分配索引，与变量一致
                if (kind == SYM_LOCAL) {
                    sym->index = allocate_local_index(s);
                }
                ast->u.func.ref.index = sym->index;
                ast->u.func.ref.name = strdup(sym->name);
                // 设置函数符号的类型为函数类型
                TypeInfo* return_type = (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER)
                    ? type_copy(ast->u.func.return_type) : NULL;
                TypeInfo** param_types = NULL;
                if (ast->u.func.pcnt > 0) {
                    param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                    for (int i = 0; i < ast->u.func.pcnt; i++) {
                        param_types[i] = (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER)
                            ? type_copy(ast->u.func.param_types[i]) : NULL;
                    }
                }
                sym->type = type_function(return_type, param_types, ast->u.func.pcnt);
                if (param_types) {
                    for (int i = 0; i < ast->u.func.pcnt; i++) {
                        type_free(param_types[i]);
                    }
                    free(param_types);
                }
                ast->u.func.ref.type_kind = TYPE_FUNCTION;
            }
        } else {
            // 使用已注册的符号（可能来自预扫描）
            ast->u.func.ref.kind = existing->kind;
            ast->u.func.ref.index = existing->index;
            if (!ast->u.func.ref.name) {
                ast->u.func.ref.name = strdup(existing->name);
            }
            // 设置函数类型（即使预扫描阶段已设置，也要更新为完整的类型信息）
            TypeInfo* return_type = (ast->u.func.return_type && ast->u.func.return_type->kind != TYPE_INFER)
                ? type_copy(ast->u.func.return_type) : NULL;
            TypeInfo** param_types = NULL;
            if (ast->u.func.pcnt > 0) {
                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.func.pcnt);
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    param_types[i] = (ast->u.func.param_types[i] && ast->u.func.param_types[i]->kind != TYPE_INFER)
                        ? type_copy(ast->u.func.param_types[i]) : NULL;
                }
            }
            if (existing->type) type_free(existing->type);
            existing->type = type_function(return_type, param_types, ast->u.func.pcnt);
            if (param_types) {
                for (int i = 0; i < ast->u.func.pcnt; i++) {
                    type_free(param_types[i]);
                }
                free(param_types);
            }
            ast->u.func.ref.type_kind = TYPE_FUNCTION;
        }
    }
    
    // 保存状态
    int prev_local_index = s->local_index;

    // 将当前函数压入栈（所有函数都入栈，用于建立upvalue链）
    s->func_stack[s->func_stack_depth++] = ast;
    s->current_func = ast;
    s->local_index = ast->u.func.pcnt;
    
    // 创建单一函数作用域（同时包含参数和局部变量）
    Scope* func_scope = scope_new(s->current, 1);
    s->current = func_scope;

    // 注册泛型类型参数到函数作用域（用于类型解析）
    if (ast->u.func.type_param_count > 0 && ast->u.func.type_params) {
        for (int i = 0; i < ast->u.func.type_param_count; i++) {
            // 注册为 TYPE 符号，使类型名在函数体内可解析
            Symbol* tp_sym = scope_define(s->current, ast->u.func.type_params[i], SYM_TYPE);
            if (tp_sym) {
                tp_sym->type = type_generic_param(ast->u.func.type_params[i]);
                tp_sym->index = -1;  // 类型符号不占运行时索引
            }
        }
    }

    // 定义参数
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        Symbol* sym = scope_define(s->current, ast->u.func.params[i], SYM_PARAM);
        if (sym) {
            sym->index = i;
            // 设置参数类型
            TypeInfo* param_type = ast->u.func.param_types[i];
            if (param_type) {
                if (param_type->kind == TYPE_INFER) {
                    // var 参数默认为 any
                    sym->type = type_new(TYPE_ANY);
                } else if (param_type->kind == TYPE_FUNCTION && !param_type->return_type) {
                    // 对于函数类型参数，如果没有返回类型签名，视为any
                    sym->type = type_new(TYPE_ANY);
                } else {
                    sym->type = type_copy(param_type);
                }
            } else {
                // 没有类型信息，默认为 any
                sym->type = type_new(TYPE_ANY);
            }
        }
    }

    // 预注册嵌套函数名（支持前向引用）
    if (ast->u.func.body && ast->u.func.body->kind == AST_BLOCK) {
        AstList* block = &ast->u.func.body->u.block;
        for (int i = 0; i < block->count; i++) {
            Ast* stmt = block->items[i];
            if (stmt->kind == AST_FUNC_DEF) {
                Symbol* sym = scope_define(s->current, stmt->u.func.name, SYM_LOCAL);
                if (sym) {
                    sym->index = allocate_local_index(s);
                    stmt->u.func.ref.kind = sym->kind;
                    stmt->u.func.ref.index = sym->index;
                    free(stmt->u.func.ref.name);
                    stmt->u.func.ref.name = strdup(sym->name);
                }
            }
        }
    }

    // 将函数体中的泛型类型参数名解析为 TYPE_GENERIC_PARAM
    if (ast->u.func.type_param_count > 0) {
        resolve_generic_in_ast(s, ast->u.func.body, ast->u.func.type_params, ast->u.func.type_param_constraints, ast->u.func.type_param_count);
    }

    // 处理函数体（单遍完成所有分析）
    // 注意：使用 visit 而不是 visit_list，以确保 AST_BLOCK 的预扫描逻辑被执行
    visit(s, ast->u.func.body);

    // 保存局部变量数量
    ast->u.func.local_count = s->local_index;

    // 恢复状态
    s->current = func_scope->parent;

    // 在 LSP 模式下保留函数作用域，以便后续符号查询
    if (!s->is_lsp_mode) {
        scope_detach_child(s->current, func_scope);
        scope_free(func_scope);
    }

    // 弹出函数栈
    s->func_stack_depth--;
    s->current_func = (s->func_stack_depth > 0) ? s->func_stack[s->func_stack_depth - 1] : NULL;
    s->local_index = prev_local_index;
}
