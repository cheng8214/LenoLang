#include "semantic_internal.h"

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

    for (int i = 0; i < ast->u.func.pcnt; i++) {
        TypeInfo* pt = ast->u.func.param_types[i];
        if (pt && pt->kind == TYPE_STRUCT && pt->struct_name) {
            if (face_def_find(pt->struct_name)) {
                pt->kind = TYPE_FACE;
            } else {
                // 检查是否是 cstruct 类型
                // 先尝试在当前作用域查找
                Symbol* struct_def = scope_resolve_local(s->current, pt->struct_name);
                // 如果找不到，尝试在父作用域查找
                if (!struct_def && s->current) {
                    struct_def = scope_resolve(s->current, pt->struct_name);
                }
                if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                    pt->kind = TYPE_CSTRUCT;
                }
            }
        }
        // 修正数组元素类型中的 face（如 Array[Speaker] 中 Speaker 被解析为 struct）
        if (pt && pt->kind == TYPE_ARRAY && pt->element_type && pt->element_type->kind == TYPE_STRUCT && pt->element_type->struct_name) {
            if (face_def_find(pt->element_type->struct_name)) {
                pt->element_type->kind = TYPE_FACE;
            }
        }
    }

    if (ast->u.func.return_type && ast->u.func.return_type->kind == TYPE_STRUCT && ast->u.func.return_type->struct_name) {
        if (face_def_find(ast->u.func.return_type->struct_name)) {
            ast->u.func.return_type->kind = TYPE_FACE;
        } else {
            // 检查是否是 cstruct 类型
            // 先尝试在当前作用域查找
            Symbol* struct_def = scope_resolve_local(s->current, ast->u.func.return_type->struct_name);
            // 如果找不到，尝试在父作用域查找
            if (!struct_def && s->current) {
                struct_def = scope_resolve(s->current, ast->u.func.return_type->struct_name);
            }
            if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                ast->u.func.return_type->kind = TYPE_CSTRUCT;
            }
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
    if (!is_struct_method) {
        func_table_add(&s->func_table, ast->u.func.name, ast);
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
