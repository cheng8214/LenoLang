#include "semantic_internal.h"

// ============================================================================
// 泛型类型推断辅助
// ============================================================================

// 从实参类型推断泛型参数的绑定
static void infer_generic_from_type(TypeInfo* generic_type, TypeInfo* concrete_type,
                                     char** param_names, TypeInfo** inferred, int count) {
    if (!generic_type || !concrete_type) return;
    
    // 如果是泛型参数，记录绑定
    if (generic_type->kind == TYPE_GENERIC_PARAM && generic_type->type_param_name) {
        for (int i = 0; i < count; i++) {
            if (strcmp(generic_type->type_param_name, param_names[i]) == 0) {
                if (!inferred[i]) {
                    inferred[i] = type_copy(concrete_type);
                }
                return;
            }
        }
        return;
    }
    
    // 类型不匹配，无法推断子类型
    if (generic_type->kind != concrete_type->kind) return;
    
    // 递归进入子类型
    switch (generic_type->kind) {
        case TYPE_ARRAY:
            infer_generic_from_type(generic_type->element_type, concrete_type->element_type,
                                    param_names, inferred, count);
            break;
        case TYPE_DICT:
            infer_generic_from_type(generic_type->key_type, concrete_type->key_type,
                                    param_names, inferred, count);
            infer_generic_from_type(generic_type->value_type, concrete_type->value_type,
                                    param_names, inferred, count);
            break;
        case TYPE_FUNCTION:
            for (int i = 0; i < generic_type->param_count && i < concrete_type->param_count; i++) {
                infer_generic_from_type(generic_type->param_types[i], concrete_type->param_types[i],
                                        param_names, inferred, count);
            }
            infer_generic_from_type(generic_type->return_type, concrete_type->return_type,
                                    param_names, inferred, count);
            break;
        case TYPE_PTR_GENERIC:
            infer_generic_from_type(generic_type->element_type, concrete_type->element_type,
                                    param_names, inferred, count);
            break;
        default:
            break;
    }
}

// 从实参推断泛型参数绑定（入口函数，兼容不同结构）
void infer_generic_bindings(TypeInfo* param_type, TypeInfo* arg_type,
                            char** param_names, TypeInfo** inferred, int count) {
    infer_generic_from_type(param_type, arg_type, param_names, inferred, count);
}

// ============================================================================
// 访问者模式 - 单遍处理
// ============================================================================

void visit_list(Semantic* s, AstList* list);

void visit(Semantic* s, Ast* ast) {
    if (!ast) return;

    switch (ast->kind) {
        case AST_BLOCK: {
            // 第零轮预扫描：提前注册所有 struct 和 face 定义
            // 确保函数体中引用 struct/face 时能通过 struct_def_find/face_def_find 找到
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_STRUCT_DEF) {
                    if (!struct_def_find(stmt->u.struct_def.name)) {
                        ObjStructDef* early_def = struct_def_new(stmt->u.struct_def.name, stmt->u.struct_def.field_count, stmt->u.struct_def.method_count);
                        if (early_def) {
                            early_def->impl_count = stmt->u.struct_def.impl_count;
                            if (early_def->impl_count > 0) {
                                early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                                for (int j = 0; j < early_def->impl_count; j++) {
                                    early_def->impl_names[j] = strdup(stmt->u.struct_def.impl_names[j]);
                                }
                            }
                            for (int j = 0; j < stmt->u.struct_def.method_count; j++) {
                                early_def->methods[j].name = strdup(stmt->u.struct_def.methods[j]->u.func.name);
                            }
                            struct_def_register(early_def);
                        }
                    }
                } else if (stmt->kind == AST_FACE_DEF) {
                    if (!face_def_find(stmt->u.face_def.name)) {
                        ObjFaceDef* early_fdef = face_def_new(stmt->u.face_def.name, stmt->u.face_def.method_count);
                        if (early_fdef) {
                            for (int j = 0; j < stmt->u.face_def.method_count; j++) {
                                early_fdef->methods[j].name = strdup(stmt->u.face_def.method_names[j]);
                                early_fdef->methods[j].param_count = stmt->u.face_def.method_param_counts[j];
                            }
                            face_def_register(early_fdef);
                        }
                    }
                }
            }

            // 预注册所有 cstruct 定义（确保函数参数类型可以正确解析）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CSTRUCT_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.cstruct_def.name);
                        if (!existing) {
                            SymKind kind = (s->current->parent == NULL) ? (s->is_module ? SYM_MODULE : SYM_GLOBAL) : SYM_LOCAL;
                            Symbol* sym = scope_define(s->current, stmt->u.cstruct_def.name, kind);
                            if (sym) {
                                TypeInfo* cstruct_type = type_new(TYPE_CSTRUCT);
                                cstruct_type->struct_name = strdup(stmt->u.cstruct_def.name);
                                sym->type = cstruct_type;
                                stmt->u.cstruct_def.ref.kind = sym->kind;
                                stmt->u.cstruct_def.ref.index = sym->index;
                                stmt->u.cstruct_def.ref.name = strdup(sym->name);
                                stmt->u.cstruct_def.ref.type_kind = TYPE_CSTRUCT;
                            }
                        }
                    }
                }
            }

            // 预注册所有 clib 定义（确保变量类型声明可以正确解析）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CLIB_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.clib_def.name);
                        if (!existing) {
                            Symbol* sym = scope_define(s->current, stmt->u.clib_def.name, SYM_CLIB);
                            if (sym) {
                                TypeInfo* clib_type = type_new(TYPE_CLIB);
                                clib_type->struct_name = strdup(stmt->u.clib_def.name);
                                sym->type = clib_type;
                                stmt->u.clib_def.ref.kind = sym->kind;
                                stmt->u.clib_def.ref.index = sym->index;
                                stmt->u.clib_def.ref.name = strdup(sym->name);
                                stmt->u.clib_def.ref.type_kind = TYPE_CLIB;
                            }
                        }
                    }
                }
            }

            // 预注册所有 cfunc 定义
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_CFUNC_DECL) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.cfunc_decl.name);
                        if (!existing) {
                            Symbol* sym = scope_define(s->current, stmt->u.cfunc_decl.name, SYM_CFUNC);
                            if (sym) {
                                TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                                cfunc_type->struct_name = strdup(stmt->u.cfunc_decl.name);
                                sym->type = cfunc_type;
                                stmt->u.cfunc_decl.ref.kind = sym->kind;
                                stmt->u.cfunc_decl.ref.index = sym->index;
                                stmt->u.cfunc_decl.ref.name = strdup(sym->name);
                                stmt->u.cfunc_decl.ref.type_kind = TYPE_CFUNC;
                            }
                        }
                    }
                }
            }

            // 第一轮：收集所有命名函数定义
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    if (s->current) {
                        Symbol* existing = scope_resolve_local(s->current, stmt->u.func.name);
                        if (!existing) {
                            SymKind kind = (s->current->parent == NULL) ? SYM_GLOBAL_FUNC : SYM_LOCAL;
                            Symbol* sym = scope_define(s->current, stmt->u.func.name, kind);
                            if (sym) {
                                stmt->u.func.ref.kind = sym->kind;
                                if (kind == SYM_LOCAL) {
                                    sym->index = allocate_local_index(s);
                                }
                                stmt->u.func.ref.index = sym->index;
                                free(stmt->u.func.ref.name);
                                stmt->u.func.ref.name = strdup(sym->name);
                            }
                        } else {
                            stmt->u.func.ref.kind = existing->kind;
                            stmt->u.func.ref.index = existing->index;
                            free(stmt->u.func.ref.name);
                            stmt->u.func.ref.name = strdup(existing->name);
                        }
                    }
                }
            }
            // 第二轮：转换所有函数的参数类型（struct -> cstruct/face）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    // 转换参数类型：struct -> cstruct/face
                    for (int j = 0; j < stmt->u.func.pcnt; j++) {
                        TypeInfo* pt = stmt->u.func.param_types[j];
                        if (pt && pt->kind == TYPE_STRUCT && pt->struct_name) {
                            if (face_def_find(pt->struct_name)) {
                                pt->kind = TYPE_FACE;
                            } else {
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
                    }
                    // 转换返回类型
                    if (stmt->u.func.return_type && stmt->u.func.return_type->kind == TYPE_STRUCT &&
                        stmt->u.func.return_type->struct_name) {
                        if (face_def_find(stmt->u.func.return_type->struct_name)) {
                            stmt->u.func.return_type->kind = TYPE_FACE;
                        } else {
                            // 先尝试在当前作用域查找
                            Symbol* struct_def = scope_resolve_local(s->current, stmt->u.func.return_type->struct_name);
                            // 如果找不到，尝试在父作用域查找
                            if (!struct_def && s->current) {
                                struct_def = scope_resolve(s->current, stmt->u.func.return_type->struct_name);
                            }
                            if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CSTRUCT) {
                                stmt->u.func.return_type->kind = TYPE_CSTRUCT;
                            } else if (struct_def && struct_def->type && struct_def->type->kind == TYPE_CLIB) {
                                stmt->u.func.return_type->kind = TYPE_CLIB;
                            }
                        }
                    }
                }
            }

            // 第三轮：设置所有函数的类型（此时所有函数名已注册，可以正确解析相互引用）
            for (int i = 0; i < ast->u.block.count; i++) {
                Ast* stmt = ast->u.block.items[i];
                if (stmt->kind == AST_FUNC_DEF) {
                    if (s->current) {
                        Symbol* sym = scope_resolve_local(s->current, stmt->u.func.name);
                        if (sym && !sym->type) {
                            // 预扫描时设置函数类型（用于类型检查）
                            TypeInfo* return_type = (stmt->u.func.return_type && stmt->u.func.return_type->kind != TYPE_INFER)
                                ? type_copy(stmt->u.func.return_type) : NULL;
                            TypeInfo** param_types = NULL;
                            if (stmt->u.func.pcnt > 0) {
                                param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * stmt->u.func.pcnt);
                                for (int j = 0; j < stmt->u.func.pcnt; j++) {
                                    param_types[j] = (stmt->u.func.param_types[j] && stmt->u.func.param_types[j]->kind != TYPE_INFER)
                                        ? type_copy(stmt->u.func.param_types[j]) : NULL;
                                }
                            }
                            sym->type = type_function(return_type, param_types, stmt->u.func.pcnt);
                            if (param_types) {
                                for (int j = 0; j < stmt->u.func.pcnt; j++) {
                                    if (param_types[j]) type_free(param_types[j]);
                                }
                                free(param_types);
                            }
                        }
                    }
                }
            }
            // 然后正常处理所有语句
            visit_list(s, &ast->u.block);
            break;
        }

        case AST_FUNC_DEF:
            // 检查函数是否在循环作用域内定义
            {
                Scope* check_scope = s->current;
                while (check_scope && !check_scope->is_func) {
                    if (check_scope->is_loop) {
                        ast->u.func.is_in_loop = 1;
                        break;
                    }
                    check_scope = check_scope->parent;
                }
            }
            visit_func(s, ast);
            break;
            
        case AST_VAR_DECL: {
            // 在当前作用域定义变量
            SymKind kind;
            if (s->current && s->current->parent == NULL) {
                // 根作用域：模块模式下使用 SYM_MODULE，否则使用 SYM_GLOBAL
                kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
            } else {
                kind = SYM_LOCAL;
            }
            Symbol* sym = scope_define(s->current, ast->u.var_decl.name, kind);
            if (sym) {
                // 局部变量使用allocate_local_index分配索引，与函数local_count保持一致
                if (kind == SYM_LOCAL) {
                    sym->index = allocate_local_index(s);
                }
                // 检查是否在循环作用域内
                Scope* check_scope = s->current;
                while (check_scope && !check_scope->is_func) {
                    if (check_scope->is_loop) {
                        sym->is_in_loop = 1;
                        break;
                    }
                    check_scope = check_scope->parent;
                }
                ast->u.var_decl.ref.kind = sym->kind;
                ast->u.var_decl.ref.index = sym->index;
                ast->u.var_decl.ref.name = strdup(sym->name);
            } else {
                // 重复定义错误 - 报告准确的行号
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "变量 '%s' 重复定义", ast->u.var_decl.name);
                error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                break;
            }

            if (ast->u.var_decl.init) {
                // 在访问初始化表达式前临时设置声明的类型，以便初始化表达式中引用自身时能获得正确类型
                // 保存原始 sym->type，visit 后恢复以避免后续代码覆盖时内存泄漏
                TypeInfo* saved_type = sym ? sym->type : NULL;
                if (sym && ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    sym->type = type_copy(ast->u.var_decl.type);
                }
                // 标记变量正在初始化，防止初始化表达式中引用自身
                if (sym) sym->is_initialized = 2;
                visit(s, ast->u.var_decl.init);
                // 标记变量已初始化
                if (sym) sym->is_initialized = 1;
                // 恢复原始 type（后续类型推断会重新设置正确的类型）
                if (sym && ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    type_free(sym->type);
                    sym->type = saved_type;
                }

                // 检查声明的类型是否存在（对于自定义 struct/cstruct/clib 类型）
                if ((ast->u.var_decl.type->kind == TYPE_STRUCT || ast->u.var_decl.type->kind == TYPE_CLIB) &&
                    ast->u.var_decl.type->struct_name) {
                    const char* type_name = ast->u.var_decl.type->struct_name;
                    if (face_def_find(type_name)) {
                        ast->u.var_decl.type->kind = TYPE_FACE;
                    } else {
                        Symbol* struct_def = scope_resolve(s->current, type_name);
                        if (!struct_def) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的类型: %s", type_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        } else if (struct_def->type->kind == TYPE_CLIB) {
                            // 如果是 clib 类型，更新为 TYPE_CLIB
                            ast->u.var_decl.type->kind = TYPE_CLIB;
                        } else if (struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            ast->u.var_decl.type->kind = TYPE_CSTRUCT;
                        } else if (struct_def->type->kind != TYPE_STRUCT &&
                                   struct_def->type->kind != TYPE_FACE) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的类型: %s", type_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        }
                    }
                } else if (ast->u.var_decl.type && ast->u.var_decl.type->kind == TYPE_STRUCT &&
                           ast->u.var_decl.type->struct_name) {
                    // 保持原有的 struct 检查路径（当 type->kind 明确是 TYPE_STRUCT 时）
                    const char* type_name = ast->u.var_decl.type->struct_name;
                    if (face_def_find(type_name)) {
                        ast->u.var_decl.type->kind = TYPE_FACE;
                    } else {
                        Symbol* struct_def = scope_resolve(s->current, type_name);
                        if (!struct_def || (struct_def->type->kind != TYPE_STRUCT &&
                                            struct_def->type->kind != TYPE_CSTRUCT &&
                                            struct_def->type->kind != TYPE_FACE)) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的 struct 类型: %s", type_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        } else if (struct_def && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            ast->u.var_decl.type->kind = TYPE_CSTRUCT;
                        }
                    }
                }

                TypeInfo* init_type = infer_expr_type(s, ast->u.var_decl.init);
                if (init_type) {
                    if (ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                        if (!type_is_compatible(ast->u.var_decl.type, init_type)) {
                            char msg[BUFFER_MEDIUM];
                            char context[256];
                            snprintf(context, sizeof(context), "变量 '%s' 声明类型与初始化值类型不匹配",
                                     ast->u.var_decl.name);
                            format_detailed_type_error(msg, sizeof(msg),
                                ast->u.var_decl.type, init_type, context);
                            // 如果是 struct 未实现 face，追加提示
                            if (ast->u.var_decl.type->kind == TYPE_FACE && init_type->kind == TYPE_STRUCT
                                && ast->u.var_decl.type->struct_name && init_type->struct_name) {
                                char hint[256];
                                snprintf(hint, sizeof(hint), " (struct '%s' 未实现 face '%s'，请添加 impl: struct %s impl %s { ... })",
                                    init_type->struct_name, ast->u.var_decl.type->struct_name,
                                    init_type->struct_name, ast->u.var_decl.type->struct_name);
                                strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                            }
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            sym->type = type_copy(ast->u.var_decl.type);
                        } else {
                            // 检查是否需要更新类型（如 Dict -> Dict[string, any]）
                            int need_update = 0;
                            if (ast->u.var_decl.type->kind == TYPE_ARRAY && 
                                !ast->u.var_decl.type->element_type &&
                                init_type->element_type) {
                                need_update = 1;
                            }
                            if (ast->u.var_decl.type->kind == TYPE_DICT && 
                                !ast->u.var_decl.type->key_type &&
                                init_type->key_type) {
                                need_update = 1;
                            }
                            if (need_update) {
                                sym->type = type_copy(init_type); // 使用具体的推断类型
                            } else {
                                sym->type = type_copy(ast->u.var_decl.type);
                            }
                        }
                        type_free(init_type);
                    } else {
                        // var 声明：如果初始化值是 null，将变量类型设为 any（允许后续赋任何值）
                        if (init_type->kind == TYPE_NULL) {
                            type_free(init_type);
                            sym->type = type_new(TYPE_ANY);
                        } else {
                            sym->type = init_type; // 使用推断的类型
                        }
                    }
                }

                if (ast->u.var_decl.init->kind == AST_DICT && sym) {
                    for (int i = 0; i < ast->u.var_decl.init->u.dict.count; i++) {
                        Ast* key_ast = ast->u.var_decl.init->u.dict.entries[i].key;
                        if (key_ast->kind == AST_STRING) {
                            symbol_add_dict_key(sym, key_ast->u.string.value);
                        }
                    }
                }

                // 如果变量类型是 Style[xxx]，记录 style_target 供 LSP 补全使用
                if (sym && sym->type && sym->type->kind == TYPE_STYLE && sym->type->style_target) {
                    sym->style_target = strdup(sym->type->style_target);
                }

                // 如果变量类型是 struct 或 cstruct，复制 struct/cstruct 定义的字段信息
                if (sym && sym->type && (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_CSTRUCT)) {
                    // 从初始化表达式获取类型名称
                    const char* type_name = NULL;
                    if (ast->u.var_decl.init->kind == AST_STRUCT_INIT) {
                        type_name = ast->u.var_decl.init->u.struct_init.struct_name;
                    } else if (sym->type->struct_name) {
                        // 对于 cstruct，从类型信息中获取名称
                        type_name = sym->type->struct_name;
                    }
                    
                    if (type_name) {
                        Symbol* type_def_sym = scope_resolve(s->current, type_name);
                        if (type_def_sym && type_def_sym->struct_field_count > 0) {
                            // 设置符号的类型名称
                            sym->struct_type_name = strdup(type_name);
                            // 同时设置类型信息的 struct 名称（供字段索引优化使用）
                            if (!sym->type->struct_name) {
                                sym->type->struct_name = strdup(type_name);
                            }
                            sym->struct_field_count = type_def_sym->struct_field_count;
                            sym->struct_field_names = (char**)malloc(sizeof(char*) * type_def_sym->struct_field_count);
                            sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * type_def_sym->struct_field_count);
                            for (int i = 0; i < type_def_sym->struct_field_count; i++) {
                                sym->struct_field_names[i] = strdup(type_def_sym->struct_field_names[i]);
                                sym->struct_field_types[i] = type_copy(type_def_sym->struct_field_types[i]);
                            }
                        }
                    }
                }
            } else {
                // 变量没有初始化，使用声明的类型或 ANY
                if (sym) sym->is_initialized = 1;
                if (ast->u.var_decl.type && ast->u.var_decl.type->kind != TYPE_INFER) {
                    sym->type = type_copy(ast->u.var_decl.type);
                    
                    // 检查自定义 struct/cstruct 类型是否已定义
                    if (sym->type->kind == TYPE_STRUCT && sym->type->struct_name) {
                        Symbol* struct_def = scope_resolve(s->current, sym->type->struct_name);
                        if (!struct_def || (struct_def->type->kind != TYPE_STRUCT &&
                                            struct_def->type->kind != TYPE_CSTRUCT)) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "未定义的 struct 类型: %s", sym->type->struct_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        } else if (struct_def && struct_def->type->kind == TYPE_CSTRUCT) {
                            // 如果实际是 cstruct，更新类型为 TYPE_CSTRUCT
                            sym->type->kind = TYPE_CSTRUCT;
                        }
                    }
                } else {
                    sym->type = type_new(TYPE_ANY);
                }
            }

            if (sym) {
                ast->u.var_decl.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                if (sym->type && sym->type->struct_name) {
                    free(ast->u.var_decl.ref.struct_name);
                    ast->u.var_decl.ref.struct_name = strdup(sym->type->struct_name);
                }
            }
            break;
        }
        
        case AST_VAR: {
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.var.name, &ast->u.var.ref);
            if (!sym) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.var.name);
                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
            } else if (sym->is_initialized == 2) {
                // 变量正在初始化中，不允许引用自身
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "变量 '%s' 在初始化前使用", ast->u.var.name);
                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                // 但仍然设置类型信息（从声明的类型获取），避免后续类型推断产生级联错误
                if (sym->type) {
                    ast->u.var.ref.type_kind = sym->type->kind;
                    if (sym->type->struct_name) {
                        free(ast->u.var.ref.struct_name);
                        ast->u.var.ref.struct_name = strdup(sym->type->struct_name);
                    }
                    if (!ast->cached_type) {
                        ast->cached_type = type_copy(sym->type);
                    }
                }
            } else {
                ast->u.var.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                if (sym->type && sym->type->struct_name) {
                    free(ast->u.var.ref.struct_name);
                    ast->u.var.ref.struct_name = strdup(sym->type->struct_name);
                }
                if (sym->type && !ast->cached_type) {
                    ast->cached_type = type_copy(sym->type);
                }
            }
            break;
        }
        
        case AST_ASSIGN: {
            // 处理并行赋值：a, b = c, d 或 a, b = [c, d] 或 arr[0], arr[1] = x, y
            int left_count = ast->u.assign.name_count;
            
            // 先访问右侧值
            visit(s, ast->u.assign.value);
            
            // 检查右侧值数量
            // 只有当左侧有多个变量且右侧是数组时，才展开数组作为并行赋值
            int right_count = 1;  // 默认是单个值
            if (left_count > 1 && ast->u.assign.value->kind == AST_ARRAY) {
                right_count = ast->u.assign.value->u.array.count;
            }
            
            // 检查左右数量是否相等
            if (left_count != right_count) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "并行赋值数量不匹配：左侧 %d 个变量，右侧 %d 个值",
                         left_count, right_count);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }
            
            // 为每个左侧目标解析符号或访问索引表达式
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];
                
                if (target->kind == AST_INDEX) {
                    // 索引表达式：访问索引节点进行语义分析
                    visit(s, target);
                    
                    // 对于索引目标，获取索引对象变量的符号信息
                    // 这用于代码生成时计算临时槽位基址
                    if (target->u.index.obj && target->u.index.obj->kind == AST_VAR) {
                        const char* obj_name = target->u.index.obj->u.var.name;
                        Symbol* sym = resolve_variable_with_upvalue(s, obj_name, &ast->u.assign.refs[i]);
                        if (sym) {
                            ast->u.assign.refs[i].type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                        }
                    }
                } else if (target->kind == AST_FIELD_ACCESS) {
                    // 字段访问：访问字段访问节点进行语义分析
                    visit(s, target);
                } else if (target->kind == AST_MODULE_ACCESS) {
                    // 模块访问（可能是 struct/cstruct 字段访问）：访问节点进行语义分析
                    // 这会触发转换为 AST_FIELD_ACCESS 或 AST_INDEX
                    visit(s, target);
                } else if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量：解析符号
                    Symbol* sym = resolve_variable_with_upvalue(s, ast->u.assign.names[i], &ast->u.assign.refs[i]);
                    if (!sym) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.assign.names[i]);
                        error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                    } else {
                        ast->u.assign.refs[i].type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                    }
                }
            }

            // 类型检查（对简单变量和索引赋值）
            for (int i = 0; i < left_count; i++) {
                Ast* target = ast->u.assign.targets[i];
                
                // 获取对应的右侧值类型
                TypeInfo* value_type = NULL;
                if (left_count > 1 && ast->u.assign.value->kind == AST_ARRAY && i < ast->u.assign.value->u.array.count) {
                    value_type = infer_expr_type(s, ast->u.assign.value->u.array.items[i]);
                } else {
                    value_type = infer_expr_type(s, ast->u.assign.value);
                }
                
                if (target->kind == AST_VAR && ast->u.assign.names[i]) {
                    // 简单变量的类型检查
                    Symbol* sym = scope_resolve(s->current, ast->u.assign.names[i]);
                    if (sym && sym->type) {
                        if (value_type) {
                            if (!type_is_compatible(sym->type, value_type)) {
                                char msg[BUFFER_MEDIUM];
                                format_detailed_type_error(msg, sizeof(msg),
                                    sym->type, value_type,
                                    "无法将值赋值给变量");
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            } else {
                                // var 声明的变量（类型为 any），第一次赋非 null 值时锁定类型
                                if (sym->type->kind == TYPE_ANY && value_type->kind != TYPE_NULL) {
                                    type_free(sym->type);
                                    sym->type = type_copy(value_type);
                                    ast->u.assign.refs[i].type_kind = sym->type->kind;
                                }
                            }
                        }
                    }
                } else if (target->kind == AST_INDEX) {
                    // 数组或字典索引赋值的类型检查
                    TypeInfo* obj_type = infer_expr_type(s, target->u.index.obj);
                    if (obj_type && obj_type->kind == TYPE_ARRAY) {
                        type_utils_check_array_index_assignment(obj_type, value_type, ast->line);
                    } else if (obj_type && obj_type->kind == TYPE_DICT) {
                        // 字典索引赋值
                        if (target->u.index.obj->kind == AST_VAR) {
                            const char* var_name = target->u.index.obj->u.var.name;
                            Symbol* sym = scope_resolve(s->current, var_name);
                            if (sym) {
                                type_utils_check_dict_index_assignment(sym, value_type, ast->line);
                            }
                        }
                    }
                    if (obj_type) type_free(obj_type);
                }
                
                if (value_type) type_free(value_type);
            }
            break;
        }
        
        case AST_COMPOUND_ASSIGN: {
            Symbol* sym = resolve_variable_with_upvalue(s, ast->u.compound_assign.name, &ast->u.compound_assign.ref);
            if (!sym) {
                // 检查是否是 struct 字段（通过 __self_field__ 标记）
                if (ast->u.compound_assign.ref.name && strcmp(ast->u.compound_assign.ref.name, "__self_field__") == 0) {
                    // struct 字段的复合赋值，标记为已处理
                    // 类型将在后续从 struct 定义推断
                    ast->u.compound_assign.ref.type_kind = TYPE_ANY;
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.compound_assign.name);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                }
            } else {
                ast->u.compound_assign.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
            }
            visit(s, ast->u.compound_assign.value);

            // 复合赋值类型检查
            if (sym && sym->type) {
                TypeInfo* value_type = infer_expr_type(s, ast->u.compound_assign.value);
                if (value_type) {
                    if (!type_is_compatible(sym->type, value_type)) {
                        char msg[BUFFER_MEDIUM];
                        format_detailed_type_error(msg, sizeof(msg),
                            sym->type, value_type,
                            "复合赋值类型不匹配");
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                    type_free(value_type);
                }
            }
            break;
        }
        
        case AST_IF: {
            visit(s, ast->u.if_.cond);

            if (ast->u.if_.guard_var != NULL) {
                if (ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_STRUCT && ast->u.if_.guard_type->struct_name) {
                    Symbol* sym = scope_resolve(s->current, ast->u.if_.guard_type->struct_name);
                    if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                        ast->u.if_.guard_type->kind = TYPE_FACE;
                    }
                }
                // 修正数组元素类型中的 face（如 Array[Speaker] 中 Speaker 被解析为 struct）
                if (ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_ARRAY &&
                    ast->u.if_.guard_type->element_type &&
                    ast->u.if_.guard_type->element_type->kind == TYPE_STRUCT &&
                    ast->u.if_.guard_type->element_type->struct_name) {
                    Symbol* sym = scope_resolve(s->current, ast->u.if_.guard_type->element_type->struct_name);
                    if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                        ast->u.if_.guard_type->element_type->kind = TYPE_FACE;
                    }
                }
                for (int gi = 0; gi < ast->u.if_.guard_conds.count; gi++) {
                    TypeGuardCond* gc = &ast->u.if_.guard_conds.items[gi];
                    if (gc->guard_type && gc->guard_type->kind == TYPE_STRUCT && gc->guard_type->struct_name) {
                        Symbol* sym = scope_resolve(s->current, gc->guard_type->struct_name);
                        if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                            gc->guard_type->kind = TYPE_FACE;
                        }
                    }
                    // 修正数组元素类型中的 face
                    if (gc->guard_type && gc->guard_type->kind == TYPE_ARRAY &&
                        gc->guard_type->element_type &&
                        gc->guard_type->element_type->kind == TYPE_STRUCT &&
                        gc->guard_type->element_type->struct_name) {
                        Symbol* sym = scope_resolve(s->current, gc->guard_type->element_type->struct_name);
                        if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                            gc->guard_type->element_type->kind = TYPE_FACE;
                        }
                    }
                }
            }

            if (ast->u.if_.then) {
                // 类型守卫：在进入 then_scope 之前先查找原始变量
                if (ast->u.if_.guard_var != NULL) {
                    // 使用resolve_variable_with_upvalue支持闭包
                    SymRef ref;
                    memset(&ref, 0, sizeof(ref));
                    Symbol* original_sym = resolve_variable_with_upvalue(s, ast->u.if_.guard_var, &ref);
                    if (ref.name) {
                        // 使用resolve_variable_with_upvalue返回的引用信息（支持upvalue）
                        ast->u.if_.guard_var_ref.kind = ref.kind;
                        ast->u.if_.guard_var_ref.index = ref.index;
                        ast->u.if_.guard_var_ref.name = strdup(ref.name);
                    } else if (original_sym) {
                        // 保存符号引用供代码生成使用
                        ast->u.if_.guard_var_ref.kind = original_sym->kind;
                        ast->u.if_.guard_var_ref.index = original_sym->index;
                        ast->u.if_.guard_var_ref.name = strdup(original_sym->name);
                    }
                }

                Scope* then_scope = scope_new(s->current, 0);
                s->current = then_scope;

                // 类型守卫：在 then 分支内收窄变量类型
                // 支持多条件类型守卫（如 if x is int or y is string）
                // 注意：or 条件下不进行类型收窄，因为类型不安全
                int has_or_in_cond = 0;
                if (ast->u.if_.cond && ast->u.if_.cond->kind == AST_BINOP && ast->u.if_.cond->u.binop.op == TOK_OR) {
                    has_or_in_cond = 1;
                }
                
                if (ast->u.if_.guard_conds.count > 0 && !has_or_in_cond) {
                    for (int gi = 0; gi < ast->u.if_.guard_conds.count; gi++) {
                        TypeGuardCond* cond = &ast->u.if_.guard_conds.items[gi];
                        if (cond->var_name == NULL) continue;

                        if (cond->field_name != NULL) {
                            // 属性访问守卫：s.age is int
                            // 在作用域中创建 "varname.fieldname" 符号记录收窄类型
                            int guard_name_len = strlen(cond->var_name) + strlen(cond->field_name) + 2;
                            char* guard_name = (char*)malloc(guard_name_len);
                            snprintf(guard_name, guard_name_len, "%s.%s", cond->var_name, cond->field_name);
                            Symbol* field_guard_sym = scope_define(s->current, guard_name, SYM_LOCAL);
                            if (field_guard_sym) {
                                field_guard_sym->type = type_copy(cond->guard_type);
                                field_guard_sym->index = -1;
                            }
                            free(guard_name);
                            continue;
                        }

                        // 使用resolve_variable_with_upvalue支持闭包
                        SymRef ref;
                        memset(&ref, 0, sizeof(ref));
                        Symbol* original_sym = resolve_variable_with_upvalue(s, cond->var_name, &ref);
                        if (ref.name) {
                            // 如果变量是upvalue，guard_sym也应该是upvalue
                            // 如果变量是global，guard_sym也应该是global
                            SymKind guard_kind;
                            if (ref.kind == SYM_UPVALUE) {
                                guard_kind = SYM_UPVALUE;
                            } else if (ref.kind == SYM_GLOBAL) {
                                guard_kind = SYM_GLOBAL;
                            } else {
                                guard_kind = SYM_LOCAL;
                            }
                            Symbol* guard_sym = scope_define(s->current, cond->var_name, guard_kind);
                            if (guard_sym) {
                                // 检查数组类型兼容性
                                int types_compatible = 1;
                                if (original_sym && original_sym->type && original_sym->type->kind == TYPE_ARRAY &&
                                    cond->guard_type && cond->guard_type->kind == TYPE_ARRAY) {
                                    // 如果守卫类型有具体元素类型，检查是否与原始类型兼容
                                    TypeInfo* orig_elem = original_sym->type->element_type;
                                    TypeInfo* guard_elem = cond->guard_type->element_type;
                                    if (guard_elem && guard_elem->kind != TYPE_ANY && orig_elem && orig_elem->kind != TYPE_ANY) {
                                        // 两者都有具体元素类型，必须相同才兼容
                                        if (orig_elem->kind != guard_elem->kind) {
                                            types_compatible = 0;
                                        }
                                    }
                                }
                                
                                if (types_compatible) {
                                    // 类型兼容，正常收窄
                                    if (original_sym && original_sym->type && original_sym->type->kind == TYPE_ARRAY &&
                                        cond->guard_type && cond->guard_type->kind == TYPE_ARRAY) {
                                        // 守卫类型有具体元素类型时，使用守卫类型（更具体）
                                        // 否则保持原始类型（如 Array[int] + is Array，保持 Array[int]）
                                        if (cond->guard_type->element_type) {
                                            guard_sym->type = type_copy(cond->guard_type);
                                        } else {
                                            guard_sym->type = type_copy(original_sym->type);
                                        }
                                    } else if (cond->guard_type) {
                                        guard_sym->type = type_copy(cond->guard_type);
                                    } else {
                                        guard_sym->type = type_new(TYPE_ANY);
                                    }
                                } else {
                                    // 类型不兼容，保持原始类型（不收窄）
                                    guard_sym->type = type_copy(original_sym->type);
                                }
                                // 复用原始变量的索引，这样访问时会读取同一个槽位
                                guard_sym->index = ref.index;
                            }
                        }
                    }
                } else if (ast->u.if_.guard_var != NULL) {
                    // 兼容旧版本单条件类型守卫
                    // 使用resolve_variable_with_upvalue支持闭包
                    SymRef ref;
                    memset(&ref, 0, sizeof(ref));
                    Symbol* original_sym = resolve_variable_with_upvalue(s, ast->u.if_.guard_var, &ref);
                    if (ref.name) {
                        // 如果变量是upvalue，guard_sym也应该是upvalue
                        // 如果变量是global，guard_sym也应该是global
                        SymKind guard_kind;
                        if (ref.kind == SYM_UPVALUE) {
                            guard_kind = SYM_UPVALUE;
                        } else if (ref.kind == SYM_GLOBAL) {
                            guard_kind = SYM_GLOBAL;
                        } else {
                            guard_kind = SYM_LOCAL;
                        }
                        Symbol* guard_sym = scope_define(s->current, ast->u.if_.guard_var, guard_kind);
                        if (guard_sym) {
                            if (original_sym && original_sym->type && original_sym->type->kind == TYPE_ARRAY &&
                                ast->u.if_.guard_type && ast->u.if_.guard_type->kind == TYPE_ARRAY) {
                                guard_sym->type = type_copy(original_sym->type);
                            } else if (ast->u.if_.guard_type) {
                                guard_sym->type = type_copy(ast->u.if_.guard_type);
                            } else {
                                guard_sym->type = type_new(TYPE_ANY);
                            }
                            guard_sym->index = ref.index;
                        }
                    }
                }
                
                if (ast->u.if_.then->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.if_.then->u.block);
                } else {
                    visit(s, ast->u.if_.then);
                }
                s->current = then_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, then_scope);
                    scope_free(then_scope);
                }
            }
            
            if (ast->u.if_.else_) {
                Scope* else_scope = scope_new(s->current, 0);
                s->current = else_scope;

                // 类型守卫：在 else 分支内，变量类型为原始类型减去守卫类型
                // 简化处理：如果原始类型是 ANY，则 else 分支也是 ANY
                // 如果原始类型与守卫类型相同，则 else 分支应该是 ANY（因为不匹配）
                // 支持多条件类型守卫
                if (ast->u.if_.guard_conds.count > 0) {
                    for (int gi = 0; gi < ast->u.if_.guard_conds.count; gi++) {
                        TypeGuardCond* cond = &ast->u.if_.guard_conds.items[gi];
                        if (cond->var_name == NULL) continue;

                        if (cond->field_name != NULL) {
                            continue;
                        }

                        // 使用resolve_variable_with_upvalue支持闭包
                        SymRef ref;
                        memset(&ref, 0, sizeof(ref));
                        Symbol* original_sym = resolve_variable_with_upvalue(s, cond->var_name, &ref);
                        if (ref.name && original_sym && original_sym->type) {
                            // 如果变量是upvalue，else_guard_sym也应该是upvalue
                            // 如果变量是global，else_guard_sym也应该是global
                            SymKind guard_kind;
                            if (ref.kind == SYM_UPVALUE) {
                                guard_kind = SYM_UPVALUE;
                            } else if (ref.kind == SYM_GLOBAL) {
                                guard_kind = SYM_GLOBAL;
                            } else {
                                guard_kind = SYM_LOCAL;
                            }
                            Symbol* else_guard_sym = scope_define(s->current, cond->var_name, guard_kind);
                            if (else_guard_sym) {
                                if (cond->guard_type && original_sym->type->kind == cond->guard_type->kind) {
                                    else_guard_sym->type = type_new(TYPE_ANY);
                                } else {
                                    else_guard_sym->type = type_copy(original_sym->type);
                                }
                                else_guard_sym->index = ref.index;
                            }
                        }
                    }
                } else if (ast->u.if_.guard_var != NULL) {
                    // 兼容旧版本单条件类型守卫
                    // 使用resolve_variable_with_upvalue支持闭包
                    SymRef ref;
                    memset(&ref, 0, sizeof(ref));
                    Symbol* original_sym = resolve_variable_with_upvalue(s, ast->u.if_.guard_var, &ref);
                    if (ref.name && original_sym && original_sym->type) {
                        // 如果变量是upvalue，else_guard_sym也应该是upvalue
                        // 如果变量是global，else_guard_sym也应该是global
                        SymKind guard_kind;
                        if (ref.kind == SYM_UPVALUE) {
                            guard_kind = SYM_UPVALUE;
                        } else if (ref.kind == SYM_GLOBAL) {
                            guard_kind = SYM_GLOBAL;
                        } else {
                            guard_kind = SYM_LOCAL;
                        }
                        Symbol* else_guard_sym = scope_define(s->current, ast->u.if_.guard_var, guard_kind);
                        if (else_guard_sym) {
                            if (ast->u.if_.guard_type && original_sym->type->kind == ast->u.if_.guard_type->kind) {
                                else_guard_sym->type = type_new(TYPE_ANY);
                            } else {
                                else_guard_sym->type = type_copy(original_sym->type);
                            }
                            else_guard_sym->index = ref.index;
                        }
                    }
                }

                if (ast->u.if_.else_->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.if_.else_->u.block);
                } else {
                    visit(s, ast->u.if_.else_);
                }
                s->current = else_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, else_scope);
                    scope_free(else_scope);
                }
            }

            // if 表达式：触发类型推断并缓存到 AST 节点，供代码生成器使用
            TypeInfo* if_type = infer_expr_type(s, ast);
            if (if_type) {
                type_free(if_type);
            }
            break;
        }
            
        case AST_WHILE: {
            visit(s, ast->u.while_.cond);

            if (ast->u.while_.body) {
                Scope* while_scope = scope_new(s->current, 0);
                while_scope->is_loop = 1;  // 标记为循环作用域
                s->current = while_scope;
                if (ast->u.while_.body->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.while_.body->u.block);
                } else {
                    visit(s, ast->u.while_.body);
                }
                s->current = while_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, while_scope);
                    scope_free(while_scope);
                }
            }
            break;
        }

        case AST_FOR: {
            if (ast->u.for_.start) visit(s, ast->u.for_.start);
            if (ast->u.for_.end) visit(s, ast->u.for_.end);
            if (ast->u.for_.step) visit(s, ast->u.for_.step);

            ast->u.for_.loop_var_index = allocate_local_index(s);
            ast->u.for_.end_index = allocate_local_index(s);
            ast->u.for_.step_index = allocate_local_index(s);
            ast->u.for_.start_index = allocate_local_index(s);
            ast->u.for_.counter_index = allocate_local_index(s);
            
            // 如果有索引变量，分配索引
            if (ast->u.for_.index_var_name) {
                ast->u.for_.index_var_index = allocate_local_index(s);
            }

            if (ast->u.for_.body) {
                Scope* for_scope = scope_new(s->current, 0);
                for_scope->is_loop = 1;  // 标记为循环作用域
                s->current = for_scope;

                if (ast->u.for_.var_name) {
                    Symbol* sym = scope_define(s->current, ast->u.for_.var_name, SYM_LOCAL);
                    if (sym) {
                        sym->index = ast->u.for_.loop_var_index;
                        // 根据循环类型推断变量类型
                        // 如果 start 为 NULL，说明是遍历语法 (for d to v)
                        // 否则是数值循环 (for 1:10 to i)
                        if (ast->u.for_.start == NULL && ast->u.for_.end) {
                            // 遍历语法：根据被遍历对象推断类型
                            TypeInfo* iterable_type = infer_expr_type(s, ast->u.for_.end);
                            if (iterable_type) {
                                if (iterable_type->kind == TYPE_DICT) {
                                    // 字典遍历，键是 string 类型
                                    sym->type = type_new(TYPE_STRING);
                                } else if (iterable_type->kind == TYPE_STRUCT) {
                                    // struct 遍历，键是字段名（string 类型）
                                    sym->type = type_new(TYPE_STRING);
                                } else if (iterable_type->kind == TYPE_ARRAY && iterable_type->element_type) {
                                    // 数组遍历，元素类型
                                    sym->type = type_copy(iterable_type->element_type);
                                } else if (iterable_type->kind == TYPE_ARRAY) {
                                    // 空数组遍历，元素类型未知，设为 any
                                    sym->type = type_new(TYPE_ANY);
                                } else if (iterable_type->kind == TYPE_STRING) {
                                    // 字符串遍历，字符是 string 类型
                                    sym->type = type_new(TYPE_STRING);
                                } else if (iterable_type->kind == TYPE_INT) {
                                    // 数值简写循环：for 5 to i（等价于 for 0:4 to i）
                                    sym->type = type_new(TYPE_INT);
                                } else if (iterable_type->kind == TYPE_FLOAT) {
                                    // 浮点简写循环：for 5.0 to i
                                    sym->type = type_new(TYPE_FLOAT);
                                } else if (iterable_type->kind == TYPE_ANY) {
                                    // any 类型：编译期无法确定，推断为 any
                                    sym->type = type_new(TYPE_ANY);
                                } else {
                                    // 默认为 any
                                    sym->type = type_new(TYPE_ANY);
                                }
                            } else {
                                sym->type = type_new(TYPE_ANY);
                            }
                        } else {
                            // 数值循环，变量是 int 类型
                            sym->type = type_new(TYPE_INT);
                        }
                        sym->is_in_loop = 1;  // 标记为循环变量
                    }
                }
                
                // 定义索引/值变量（如果有）
                if (ast->u.for_.index_var_name) {
                    Symbol* idx_sym = scope_define(s->current, ast->u.for_.index_var_name, SYM_LOCAL);
                    if (idx_sym) {
                        idx_sym->index = ast->u.for_.index_var_index;
                        // 根据遍历类型确定第二个变量的类型
                        if (ast->u.for_.start == NULL && ast->u.for_.end) {
                            TypeInfo* iterable_type = infer_expr_type(s, ast->u.for_.end);
                            if (iterable_type && iterable_type->kind == TYPE_DICT) {
                                // 字典遍历：第二个变量是值
                                if (iterable_type->value_type) {
                                    idx_sym->type = type_copy(iterable_type->value_type);
                                } else {
                                    idx_sym->type = type_new(TYPE_ANY);
                                }
                            } else if (iterable_type && iterable_type->kind == TYPE_STRUCT) {
                                // struct 遍历：第二个变量是字段值（类型未知，设为 any）
                                idx_sym->type = type_new(TYPE_ANY);
                            } else {
                                // 数组/字符串遍历：第二个变量是索引（int）
                                idx_sym->type = type_new(TYPE_INT);
                            }
                        } else {
                            // 数值循环：第二个变量是索引（int）
                            idx_sym->type = type_new(TYPE_INT);
                        }
                        idx_sym->is_in_loop = 1;
                    }
                }

                if (ast->u.for_.body->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.for_.body->u.block);
                } else {
                    visit(s, ast->u.for_.body);
                }
                s->current = for_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, for_scope);
                    scope_free(for_scope);
                }
            }
            break;
        }

        case AST_SWITCH: {
            visit(s, ast->u.switch_.expr);

            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                Scope* case_scope = scope_new(s->current, 0);
                s->current = case_scope;

                // case is Type 模式：解析 guard_var 的符号引用，并修正类型（struct -> face）
                if (ast->u.switch_.cases[i].is_type_match && ast->u.switch_.cases[i].match_type) {
                    TypeInfo* mt = ast->u.switch_.cases[i].match_type;
                    // 如果类型是 TYPE_STRUCT 但实际上是 face，修正为 TYPE_FACE
                    if (mt->kind == TYPE_STRUCT && mt->struct_name && face_def_find(mt->struct_name)) {
                        mt->kind = TYPE_FACE;
                    }
                    // 解析 guard_var 的符号引用
                    if (ast->u.switch_.cases[i].guard_var) {
                        SymRef ref;
                        memset(&ref, 0, sizeof(ref));
                        Symbol* sym = resolve_variable_with_upvalue(s, ast->u.switch_.cases[i].guard_var, &ref);
                        if (ref.name) {
                            ast->u.switch_.cases[i].guard_var_ref.kind = ref.kind;
                            ast->u.switch_.cases[i].guard_var_ref.index = ref.index;
                            ast->u.switch_.cases[i].guard_var_ref.name = strdup(ref.name);
                        } else if (sym) {
                            ast->u.switch_.cases[i].guard_var_ref.kind = sym->kind;
                            ast->u.switch_.cases[i].guard_var_ref.index = sym->index;
                            ast->u.switch_.cases[i].guard_var_ref.name = strdup(sym->name);
                        }
                    }
                }

                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++) {
                    visit(s, ast->u.switch_.cases[i].values.items[j]);
                }

                if (ast->u.switch_.cases[i].body->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.switch_.cases[i].body->u.block);
                } else {
                    visit(s, ast->u.switch_.cases[i].body);
                }

                s->current = case_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, case_scope);
                    scope_free(case_scope);
                }
            }

            if (ast->u.switch_.default_body) {
                Scope* default_scope = scope_new(s->current, 0);
                s->current = default_scope;
                if (ast->u.switch_.default_body->kind == AST_BLOCK) {
                    visit_list(s, &ast->u.switch_.default_body->u.block);
                } else {
                    visit(s, ast->u.switch_.default_body);
                }
                s->current = default_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, default_scope);
                    scope_free(default_scope);
                }
            }
            break;
        }

        case AST_BINOP: {
            visit(s, ast->u.binop.l);
            visit(s, ast->u.binop.r);
            // 触发类型推断并缓存到 AST 节点，供代码生成器使用
            TypeInfo* binop_type = infer_expr_type(s, ast);
            if (binop_type) {
                type_free(binop_type);
            }
            break;
        }

        case AST_INDEX_ASSIGN: {
            // 初始化字段索引为 -1（未确定）
            ast->u.index_assign.field_index = -1;
            
            visit(s, ast->u.index_assign.obj);
            visit(s, ast->u.index_assign.index);
            visit(s, ast->u.index_assign.value);

            // 获取被赋值对象的类型（支持嵌套字段访问如 r.position.x）
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index_assign.obj);

            if (obj_type && obj_type->kind == TYPE_STRUCT) {
                if (ast->u.index_assign.index->kind == AST_STRING) {
                    char* field_name = ast->u.index_assign.index->u.string.value;

                    // 从 struct 定义中查找字段类型和索引
                    TypeInfo* expected_type = NULL;
                    if (obj_type->struct_name) {
                        Symbol* struct_def_sym = scope_resolve(s->current, obj_type->struct_name);
                        if (struct_def_sym && struct_def_sym->struct_field_count > 0) {
                            for (int j = 0; j < struct_def_sym->struct_field_count; j++) {
                                if (strcmp(struct_def_sym->struct_field_names[j], field_name) == 0) {
                                    expected_type = struct_def_sym->struct_field_types[j];
                                    ast->u.index_assign.field_index = j; // 存储字段索引
                                    break;
                                }
                            }
                        }
                    }

                    if (expected_type) {
                        // 推断值的类型
                        TypeInfo* value_type = infer_expr_type(s, ast->u.index_assign.value);

                        // 检查类型兼容性
                        if (value_type && !type_is_compatible(expected_type, value_type)) {
                            char msg[BUFFER_MEDIUM];
                            format_type_error(msg, sizeof(msg),
                                "字段 '%s3' 类型不匹配: 期望 '%s1'，实际 '%s2'",
                                expected_type, value_type,
                                field_name, NULL);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }

                        if (value_type) type_free(value_type);
                    }
                }
            }
            // 检查数组索引赋值的元素类型
            else if (obj_type && obj_type->kind == TYPE_ARRAY) {
                TypeInfo* value_type = infer_expr_type(s, ast->u.index_assign.value);
                type_utils_check_array_index_assignment(obj_type, value_type, ast->line);
                if (value_type) type_free(value_type);
            }
            // 检查字典值类型和键是否存在
            else if (obj_type && obj_type->kind == TYPE_DICT) {
                // 对于变量访问，检查字典值类型
                if (ast->u.index_assign.obj->kind == AST_VAR) {
                    const char* var_name = ast->u.index_assign.obj->u.var.name;
                    SymRef ref;
                    memset(&ref, 0, sizeof(ref));
                    Symbol* sym = resolve_variable_with_upvalue(s, var_name, &ref);
                    
                    if (sym) {
                        TypeInfo* assign_type = infer_expr_type(s, ast->u.index_assign.value);
                        type_utils_check_dict_index_assignment(sym, assign_type, ast->line);
                        if (assign_type) type_free(assign_type);
                    }
                }
                // 注意：允许通过 d["key"] = value 添加新键或更新已有键
            }

            if (obj_type) type_free(obj_type);
            break;
        }

        case AST_UNARY: {
            visit(s, ast->u.unary.operand);
            // 触发类型推断并缓存到 AST 节点，供代码生成器使用
            TypeInfo* unary_type = infer_expr_type(s, ast);
            if (unary_type) {
                type_free(unary_type);
            }
            break;
        }
            
        case AST_CALL: {
            Symbol* sym = NULL;
            const char* func_name = NULL;
            
            // 处理方法调用：callee 是 INDEX (obj["method"])
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_INDEX) {
                Ast* index_ast = ast->u.call.callee;
                visit(s, index_ast->u.index.obj);
                visit(s, index_ast->u.index.index);
                visit_list(s, &ast->u.call.args);
                
                // 检查是否是实例方法调用
                if (index_ast->u.index.index->kind == AST_STRING) {
                    const char* method_name = index_ast->u.index.index->u.string.value;
                    TypeInfo* receiver_type = infer_expr_type(s, index_ast->u.index.obj);
                    
                    if (receiver_type) {
                        const char* type_name = native_get_type_name(receiver_type->kind);
                        if (type_name) {
                            int expected_arity;
                            TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, &expected_arity);

                            // 当 return_type == TYPE_ANY 且 expected_arity == -1 时表示方法不存在
                            if (!(return_type == TYPE_ANY && expected_arity == -1)) {
                                // 是实例方法，检查参数
                                int actual_arity = ast->u.call.args.count;
                                const InstanceMethodMeta* meta = native_find_instance_method(type_name, method_name);

                                // arity >= 0: 固定参数数量
                                if (expected_arity >= 0 && actual_arity != expected_arity) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                                             method_name, expected_arity, actual_arity);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }
                                // arity < 0: 可变参数，检查是否少于 min_arity
                                else if (expected_arity < 0 && meta && meta->min_arity >= 0 && actual_arity < meta->min_arity) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                             method_name, meta->min_arity, actual_arity);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }
                                // arity < 0: 可变参数，检查是否超过 max_arity
                                else if (expected_arity < 0 && meta && meta->max_arity >= 0 && actual_arity > meta->max_arity) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                             method_name, meta->max_arity, actual_arity);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }

                                // 检查参数类型
                                if (expected_arity == -1 || actual_arity == expected_arity) {
                                    // 处理数组方法
                                    if (receiver_type && receiver_type->kind == TYPE_ARRAY) {
                                        // 对于数组方法，从 receiver_type 获取元素类型（如果有的话）
                                        TypeKind array_elem_kind = TYPE_ANY;
                                        if (receiver_type->element_type) {
                                            array_elem_kind = receiver_type->element_type->kind;
                                        }
                                        
                                        for (int i = 0; i < actual_arity; i++) {
                                            TypeKind expected_param_type = native_get_instance_method_param_type(type_name, method_name, i);
                                            
                                            // 如果是数组的 add/insert 方法，且 receiver 有具体元素类型，使用该类型
                                            if (type_utils_is_array_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0);
                                                if (i == elem_param_idx && array_elem_kind != TYPE_ANY) {
                                                    expected_param_type = array_elem_kind;
                                                }
                                            }
                                            
                                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                            
                                            // 处理数组元素修改方法（add/insert）的类型更新
                                            if (type_utils_is_array_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0);
                                                if (i == elem_param_idx) {
                                                    // 尝试更新数组元素类型
                                                    Symbol* arr_sym = type_utils_resolve_var_symbol(s, index_ast->u.index.obj);
                                                    int updated = 0;
                                                    // 先尝试普通数组更新（arr.add() 这种情况）
                                                    if (type_utils_try_update_array_element_type(arr_sym, arg_type)) {
                                                        expected_param_type = arg_type->kind;
                                                        updated = 1;
                                                    } else {
                                                        // 再尝试嵌套数组更新（arr[0].add()、arr[0][0].add() 等任意层级）
                                                        if (type_utils_try_update_nested_array_element_type_ex(arr_sym, index_ast->u.index.obj, arg_type)) {
                                                            expected_param_type = arg_type->kind;
                                                            updated = 1;
                                                        }
                                                    }
                                                    // 如果更新失败（类型已确定），从符号表重新获取元素类型
                                                    if (!updated && arr_sym && arr_sym->type && arr_sym->type->kind == TYPE_ARRAY) {
                                                        // 根据索引深度获取正确的元素类型
                                                        TypeInfo* current_type = arr_sym->type;
                                                        Ast* current_index = index_ast->u.index.obj;
                                                        while (current_index && current_index->kind == AST_INDEX) {
                                                            if (current_type->element_type) {
                                                                current_type = current_type->element_type;
                                                            }
                                                            current_index = current_index->u.index.obj;
                                                        }
                                                        if (current_type && current_type->kind == TYPE_ARRAY && current_type->element_type) {
                                                            expected_param_type = current_type->element_type->kind;
                                                        }
                                                    }
                                                }
                                            }
                                            
                                            if (expected_param_type != TYPE_ANY && arg_type && arg_type->kind != TYPE_ANY) {
                                                if (arg_type->kind != expected_param_type) {
                                                    // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                                                    bool type_compatible = false;
                                                    if ((expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                                                        (expected_param_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                                                        (expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                                                        type_compatible = true;
                                                    }
                                                    // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                                                    if ((expected_param_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                                                        (expected_param_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                                                        type_compatible = true;
                                                    }
                                                    // 允许 Style[xxx] 传给 Dict 参数
                                                    if (expected_param_type == TYPE_DICT && arg_type->kind == TYPE_STYLE) {
                                                        type_compatible = true;
                                                    }
                                                    if (!type_compatible) {
                                                        char msg[BUFFER_MEDIUM];
                                                        char context[256];
                                                        snprintf(context, sizeof(context), "函数 '%s' 的参数 %d 类型不匹配",
                                                                 method_name, i + 1);
                                                        TypeInfo* expected_type = type_new(expected_param_type);
                                                        format_detailed_type_error(msg, sizeof(msg),
                                                            expected_type, arg_type, context);
                                                        type_free(expected_type);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                            }
                                            if (arg_type) type_free(arg_type);
                                        }
                                    }
                                    // 处理字典 set 方法的值类型检查
                                    else if (receiver_type && receiver_type->kind == TYPE_DICT) {
                                        // 对于字典的 set 方法，从 receiver_type 获取值类型（如果有的话）
                                        TypeKind dict_value_kind = TYPE_ANY;
                                        TypeInfo* dict_value_type = NULL;
                                        if (receiver_type->value_type) {
                                            dict_value_kind = receiver_type->value_type->kind;
                                            dict_value_type = receiver_type->value_type;
                                        }
                                        
                                        // 获取参数数量
                                        int actual_arity = ast->u.call.args.count;
                                        
                                        for (int i = 0; i < actual_arity; i++) {
                                            TypeKind expected_param_type = native_get_instance_method_param_type(type_name, method_name, i);
                                            
                                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                            
                                            // 处理字典 set 方法的值类型更新和检查
                                            TypeInfo* expected_type_for_check = NULL;
                                            if (type_utils_is_dict_element_mutator(method_name)) {
                                                int elem_param_idx = type_utils_get_dict_element_param_index(method_name);
                                                if (i == elem_param_idx) {
                                                    // 获取字典变量符号
                                                    Symbol* dict_sym = type_utils_resolve_var_symbol(s, index_ast->u.index.obj);
                                                    // 首先尝试更新字典值类型（如果是第一次 set）
                                                    if (type_utils_try_update_dict_value_type(dict_sym, arg_type)) {
                                                        // 类型已更新，同时更新期望参数类型
                                                        expected_param_type = arg_type->kind;
                                                    } else if (dict_value_kind != TYPE_ANY) {
                                                        // 字典已有具体值类型，使用该类型检查
                                                        expected_param_type = dict_value_kind;
                                                        expected_type_for_check = dict_value_type;
                                                    }
                                                }
                                            }
                                            
                                            // 类型检查：简单类型比较 kind，复杂类型使用 type_is_compatible 深度比较
                                            if (arg_type && arg_type->kind != TYPE_ANY) {
                                                if (expected_type_for_check) {
                                                    // 有完整类型信息，进行深度类型比较（支持嵌套类型）
                                                    if (!type_is_compatible(expected_type_for_check, arg_type)) {
                                                        char msg[BUFFER_MEDIUM];
                                                        // 先保存字符串避免 buffer 覆盖
                                                        const char* exp_str = type_to_string(expected_type_for_check);
                                                        char exp_buf[BUFFER_SMALL];
                                                        strncpy(exp_buf, exp_str, sizeof(exp_buf) - 1);
                                                        exp_buf[sizeof(exp_buf) - 1] = '\0';
                                                        const char* hint = get_type_conversion_hint(expected_type_for_check->kind, arg_type->kind);
                                                        if (hint) {
                                                            snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                                     i + 1, exp_buf, type_to_string(arg_type), hint);
                                                        } else {
                                                            snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                                     i + 1, exp_buf, type_to_string(arg_type));
                                                        }
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                } else if (expected_param_type != TYPE_ANY) {
                                                    // 只有 kind 信息，进行简单类型比较
                                                    if (arg_type->kind != expected_param_type) {
                                                        // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                                                        bool type_compatible = false;
                                                        if ((expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                                                            (expected_param_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                                                            (expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                                                            type_compatible = true;
                                                        }
                                                        // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                                                        if ((expected_param_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                                                            (expected_param_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                                                            type_compatible = true;
                                                        }
                                                        // 允许 Style[xxx] 传给 Dict 参数
                                                        if (expected_param_type == TYPE_DICT && arg_type->kind == TYPE_STYLE) {
                                                            type_compatible = true;
                                                        }
                                                        if (!type_compatible) {
                                                            char msg[BUFFER_MEDIUM];
                                                            const char* hint = get_type_conversion_hint(expected_param_type, arg_type->kind);
                                                            if (hint) {
                                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                                         i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type), hint);
                                                            } else {
                                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                                         i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type));
                                                            }
                                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                                        }
                                                    }
                                                }
                                            }
                                            if (arg_type) type_free(arg_type);
                                        }
                                    }
                                }
                            }
                        }
                        // 自定义 struct 方法的参数类型检查
                        else if (receiver_type->kind == TYPE_STRUCT && receiver_type->struct_name) {
                            // 在 AST 中查找 struct 定义
                            Ast* struct_def_ast = NULL;
                            for (int si = 0; si < s->root->u.block.count; si++) {
                                Ast* stmt = s->root->u.block.items[si];
                                if (stmt->kind == AST_STRUCT_DEF && strcmp(stmt->u.struct_def.name, receiver_type->struct_name) == 0) {
                                    struct_def_ast = stmt;
                                    break;
                                }
                            }
                            if (struct_def_ast) {
                                // 查找方法定义
                                for (int mi = 0; mi < struct_def_ast->u.struct_def.method_count; mi++) {
                                    Ast* method_ast = struct_def_ast->u.struct_def.methods[mi];
                                    if (method_ast && method_ast->kind == AST_FUNC_DEF &&
                                        strcmp(method_ast->u.func.name, method_name) == 0) {
                                        // 找到方法，检查参数类型
                                        int actual_arity = ast->u.call.args.count;
                                        // 方法参数从第1个开始（第0个是 self）
                                        int method_param_count = method_ast->u.func.pcnt - 1;
                                        if (method_param_count >= 0 && actual_arity != method_param_count) {
                                            char msg[BUFFER_MEDIUM];
                                            snprintf(msg, sizeof(msg), "方法 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                     method_name, method_param_count, actual_arity);
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        } else {
                                            // 构建泛型参数替换表
                                            char** gp_names = NULL;
                                            TypeInfo** gp_types = NULL;
                                            int gp_count = 0;

                                            if (receiver_type->generic_count > 0 && receiver_type->generic_args) {
                                                gp_count = receiver_type->generic_count;
                                                gp_names = (char**)malloc(sizeof(char*) * gp_count);
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * gp_count);
                                                if (struct_def_ast->u.struct_def.type_params) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        gp_names[gi] = struct_def_ast->u.struct_def.type_params[gi];
                                                        gp_types[gi] = type_copy(receiver_type->generic_args[gi]);
                                                    }
                                                }
                                            }

                                            for (int i = 0; i < actual_arity; i++) {
                                                TypeInfo* expected_type = method_ast->u.func.param_types[i + 1]; // +1 跳过 self
                                                if (!expected_type) continue;

                                                // 替换泛型参数
                                                TypeInfo* check_type = expected_type;
                                                if (gp_count > 0) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        TypeInfo* sub = type_substitute(check_type, gp_names[gi], gp_types[gi]);
                                                        if (sub != check_type) {
                                                            check_type = sub;
                                                        }
                                                    }
                                                }

                                                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                                if (arg_type && arg_type->kind != TYPE_ANY && check_type->kind != TYPE_ANY) {
                                                    if (!type_is_compatible(check_type, arg_type)) {
                                                        char msg[BUFFER_MEDIUM];
                                                        char idx_str[16];
                                                        snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                                        format_type_error(msg, sizeof(msg),
                                                            "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                                            check_type, arg_type,
                                                            method_name, idx_str);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                                if (arg_type) type_free(arg_type);
                                                if (check_type != expected_type) type_free(check_type);
                                            }

                                            // 清理泛型参数替换表
                                            if (gp_types) {
                                                for (int gi = 0; gi < gp_count; gi++) {
                                                    type_free(gp_types[gi]);
                                                }
                                                free(gp_types);
                                                free(gp_names);
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }
                        type_free(receiver_type);
                    }
                }
                break;
            }
            
            // 处理 clib 调用：lib.func(args)
            if (ast->u.call.callee && ast->u.call.callee->kind == AST_FIELD_ACCESS) {
                Ast* field_access = ast->u.call.callee;
                visit(s, field_access->u.field_access.obj);
                visit_list(s, &ast->u.call.args);

                // 检查是否是 clib 类型的方法调用
                TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                    // 查找 clib 符号以获取函数签名
                    Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (clib_sym && clib_sym->clib_func_count > 0) {
                        const char* func_name = field_access->u.field_access.field_name;
                        int func_found = 0;

                        for (int i = 0; i < clib_sym->clib_func_count; i++) {
                            if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                                func_found = 1;
                                int expected_params = clib_sym->clib_func_param_counts[i];
                                int actual_params = ast->u.call.args.count;

                                // 检查参数数量
                                if (actual_params != expected_params) {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "clib '%s' 的函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                             obj_type->struct_name, func_name, expected_params, actual_params);
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }

                                // 设置缓存类型：C 类型映射为 Leno 类型（零摩擦）
                                // i32/u32/i64/u64/i8/u8/i16/u16 → int
                                // f32/f64 → float
                                // str8/str16 → string
                                // Ptr → Ptr
                                // bool → bool
                                // void → null
                                TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                                if (!ast->cached_type) {
                                    TypeKind rk = ret_type ? ret_type->kind : TYPE_NULL;
                                    switch (rk) {
                                        case TYPE_I8: case TYPE_U8:
                                        case TYPE_I16: case TYPE_U16:
                                        case TYPE_I32: case TYPE_U32:
                                        case TYPE_I64: case TYPE_U64:
                                            ast->cached_type = type_new(TYPE_INT);
                                            break;
                                        case TYPE_F32: case TYPE_F64:
                                            ast->cached_type = type_new(TYPE_FLOAT);
                                            break;
                                        case TYPE_STR8: case TYPE_STR16:
                                            ast->cached_type = type_new(TYPE_STRING);
                                            break;
                                        case TYPE_PTR: case TYPE_PTR_GENERIC:
                                            ast->cached_type = type_new(TYPE_PTR);
                                            ast->cached_type->struct_name = strdup("Ptr");
                                            break;
                                        case TYPE_BOOL:
                                            ast->cached_type = type_new(TYPE_BOOL);
                                            break;
                                        case TYPE_NULL:
                                            ast->cached_type = type_new(TYPE_NULL);
                                            break;
                                        default:
                                            ast->cached_type = type_copy(ret_type);
                                            break;
                                    }
                                }
                                break;
                            }
                        }

                        if (!func_found) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                     obj_type->struct_name, func_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                }
                if (obj_type) type_free(obj_type);
                break;
            }

            if (ast->u.call.callee && ast->u.call.callee->kind == AST_VAR) {
                func_name = ast->u.call.callee->u.var.name;
                // 使用resolve_variable_with_upvalue处理函数调用，支持闭包
                sym = resolve_variable_with_upvalue(s, func_name, &ast->u.call.callee->u.var.ref);
                if (!sym) {
                    // 函数未定义，检查是否是 struct 类型
                    Symbol* struct_sym = scope_resolve(s->current, func_name);
                    if (struct_sym && struct_sym->type && struct_sym->type->kind == TYPE_STRUCT) {
                        // 是 struct 类型，提示使用 new 关键字
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "struct '%s' 需要使用 new 关键字实例化，例如: new %s()",
                                func_name, func_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "未定义的函数: %s", func_name);
                        error_add(ERR_UNDEFINED_FUNC, ast->line, msg);
                    }
                } else {
                    ast->u.call.callee->u.var.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;

                    // 检查是否是 struct 类型调用（未使用 new 关键字）
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "struct '%s' 需要使用 new 关键字实例化，例如: new %s()",
                                func_name, func_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                }
            } else {
                visit(s, ast->u.call.callee);
            }
            visit_list(s, &ast->u.call.args);
            
            // 检查 native 函数的参数数量
            if (sym && sym->kind == SYM_NATIVE) {
                int count;
                const NativeFunctionMeta* builtins = native_get_all_functions(&count);
                for (int i = 0; i < count; i++) {
                    if (strcmp(builtins[i].name, func_name) == 0) {
                        int actual_arity = ast->u.call.args.count;
                        // arity >= 0: 固定参数数量
                        if (builtins[i].arity >= 0 && actual_arity != builtins[i].arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数数量不匹配: 期望 %d, 实际 %d",
                                     func_name, builtins[i].arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否少于 min_arity
                        else if (builtins[i].arity < 0 && builtins[i].min_arity >= 0 && actual_arity < builtins[i].min_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数过少: 最少 %d 个, 实际 %d",
                                     func_name, builtins[i].min_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否超过 max_arity
                        else if (builtins[i].arity < 0 && builtins[i].max_arity >= 0 && actual_arity > builtins[i].max_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "%s 参数过多: 最多 %d 个, 实际 %d",
                                     func_name, builtins[i].max_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        else if (builtins[i].arity >= 0 && actual_arity == builtins[i].arity) {
                            // 参数数量正确，检查参数类型
                            for (int j = 0; j < actual_arity; j++) {
                                TypeKind expected_param_type = builtins[i].param_types[j];
                                // 如果参数类型不是 TYPE_ANY，则进行类型检查
                                if (expected_param_type != TYPE_ANY) {
                                    Ast* arg = ast->u.call.args.items[j];
                                    TypeInfo* arg_type = infer_expr_type(s, arg);
                                    if (arg_type && arg_type->kind != TYPE_ANY) {
                                        if (arg_type->kind != expected_param_type) {
                                            // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                                            bool type_compatible = false;
                                            if ((expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                                                (expected_param_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                                                (expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                                                type_compatible = true;
                                            }
                                            // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                                            if ((expected_param_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                                                (expected_param_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                                                type_compatible = true;
                                            }
                                            // 允许 Style[xxx] 传给 Dict 参数
                                            if (expected_param_type == TYPE_DICT && arg_type->kind == TYPE_STYLE) {
                                                type_compatible = true;
                                            }
                                            if (!type_compatible) {
                                                char msg[BUFFER_MEDIUM];
                                                const char* hint = get_type_conversion_hint(expected_param_type, arg_type->kind);
                                                if (hint) {
                                                    snprintf(msg, sizeof(msg), "类型错误: 函数 '%s' 的参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                             func_name, j + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type), hint);
                                                } else {
                                                    snprintf(msg, sizeof(msg), "类型错误: 函数 '%s' 的参数 %d 期望 '%s'，但传入 '%s'",
                                                             func_name, j + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type));
                                                }
                                                error_add(ERR_SEMANTIC, ast->line, msg);
                                            }
                                        }
                                    }
                                    if (arg_type) type_free(arg_type);
                                }
                            }
                        }
                        break;
                    }
                }
            }
            
            // 检查用户定义函数的参数类型
            if (sym && (sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_LOCAL)) {
                // 在函数表中查找函数定义（O(1) 哈希表查找）
                Ast* func_def = func_table_find(&s->func_table, func_name);
                
                if (func_def && func_def->kind == AST_FUNC_DEF) {
                    // 泛型类型参数推断
                    int is_generic = (func_def->u.func.type_param_count > 0);
                    char** inferred_names = NULL;
                    TypeInfo** inferred_types = NULL;
                    
                    if (is_generic) {
                        inferred_names = (char**)malloc(sizeof(char*) * func_def->u.func.type_param_count);
                        inferred_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * func_def->u.func.type_param_count);
                        
                        // 从实参中推断类型参数
                        for (int i = 0; i < func_def->u.func.type_param_count; i++) {
                            inferred_names[i] = func_def->u.func.type_params[i];
                            inferred_types[i] = NULL;
                        }
                        
                        // 如果调用时显式提供了泛型类型参数（如 identity[int](42)），优先使用
                        if (ast->u.call.generic_type_count > 0 && ast->u.call.generic_type_args) {
                            for (int i = 0; i < func_def->u.func.type_param_count && i < ast->u.call.generic_type_count; i++) {
                                inferred_types[i] = type_copy(ast->u.call.generic_type_args[i]);
                            }
                        } else {
                            // 从实参推断类型参数
                            for (int i = 0; i < ast->u.call.args.count && i < func_def->u.func.pcnt; i++) {
                                TypeInfo* param_type = func_def->u.func.param_types[i];
                                TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                                if (param_type && arg_type) {
                                    infer_generic_bindings(param_type, arg_type, inferred_names, inferred_types, func_def->u.func.type_param_count);
                                }
                            }
                        }
                        
                        // 为未推断的类型参数填默认 any
                        for (int i = 0; i < func_def->u.func.type_param_count; i++) {
                            if (!inferred_types[i]) {
                                inferred_types[i] = type_new(TYPE_ANY);
                            }
                        }
                        
                        // 存储特化信息
                        ast->u.call.generic_type_names = inferred_names;
                        ast->u.call.generic_type_args = inferred_types;
                        ast->u.call.generic_type_count = func_def->u.func.type_param_count;
                    }
                    
                    int expected_cnt = func_def->u.func.pcnt;
                    int actual_cnt = ast->u.call.args.count;
                    int default_cnt = func_def->u.func.default_count;
                    int required_cnt = expected_cnt - default_cnt;  // 必选参数数量
                    
                    // 检查参数数量（支持默认参数）
                    if (actual_cnt < required_cnt || actual_cnt > expected_cnt) {
                        char msg[BUFFER_MEDIUM];
                        if (actual_cnt < required_cnt) {
                            snprintf(msg, sizeof(msg), "%s 参数数量不足: 至少需要 %d 个参数，实际传入 %d 个",
                                     func_name, required_cnt, actual_cnt);
                        } else {
                            snprintf(msg, sizeof(msg), "%s 参数数量过多: 最多接受 %d 个参数，实际传入 %d 个",
                                     func_name, expected_cnt, actual_cnt);
                        }
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        // 检查每个提供的参数的类型
                        for (int i = 0; i < actual_cnt; i++) {
                            TypeInfo* expected_type = func_def->u.func.param_types[i];
                            if (!expected_type || expected_type->kind == TYPE_INFER) continue; // 类型推断模式，跳过检查

                            // 泛型函数：用推断的类型替换泛型参数
                            TypeInfo* check_type = expected_type;
                            if (is_generic && inferred_types) {
                                check_type = expected_type;
                                for (int p = 0; p < func_def->u.func.type_param_count; p++) {
                                    TypeInfo* sub = type_substitute(check_type, func_def->u.func.type_params[p], inferred_types[p]);
                                    if (sub != check_type) {
                                        check_type = sub;  // 使用替换后的类型
                                    }
                                }
                            }

                            TypeInfo* arg_type = infer_expr_type(s, ast->u.call.args.items[i]);
                            if (arg_type) {
                                // 使用类型兼容性检查
                                if (!type_is_compatible(check_type, arg_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    char idx_str[16];
                                    snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                    format_type_error(msg, sizeof(msg),
                                        "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                        check_type, arg_type,
                                        func_name, idx_str);
                                    // 如果是 struct 未实现 face，追加提示
                                    if (check_type->kind == TYPE_FACE && arg_type->kind == TYPE_STRUCT
                                        && check_type->struct_name && arg_type->struct_name) {
                                        char hint[256];
                                        snprintf(hint, sizeof(hint), " (struct '%s' 未实现 face '%s'，请添加 impl: struct %s impl %s { ... })",
                                            arg_type->struct_name, check_type->struct_name,
                                            arg_type->struct_name, check_type->struct_name);
                                        strncat(msg, hint, sizeof(msg) - strlen(msg) - 1);
                                    }
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                            }
                            if (arg_type) type_free(arg_type);
                            // 清理泛型替换产生的临时类型
                            if (check_type != expected_type) type_free(check_type);
                        }
                    }
                }
            }
            
            {
                TypeInfo* t = infer_expr_type(s, ast);
                if (t) type_free(t);
            }
            break;
        }

            case AST_INDEX:
            visit(s, ast->u.index.obj);
            visit(s, ast->u.index.index);

            // 编译时检查数组索引类型
            TypeInfo* obj_type = infer_expr_type(s, ast->u.index.obj);
            if (obj_type && obj_type->kind == TYPE_ARRAY) {
                TypeInfo* index_type = infer_expr_type(s, ast->u.index.index);
                if (index_type && index_type->kind != TYPE_INT && index_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "数组索引必须是整数类型，实际为 %s",
                             type_kind_to_string(index_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (index_type) type_free(index_type);
            }

            // 检查嵌套字段访问中的 null 默认值（如 self["head"]["next"] 或 self["items"][0]）
            // 当 obj 是 INDEX 类型，且最内层是 self 变量时，检查中间字段的 null 默认值
            if (ast->u.index.obj->kind == AST_INDEX) {
                Ast* inner_index = ast->u.index.obj;
                // 检查是否是最内层 self["field"] 模式
                if (inner_index->u.index.obj->kind == AST_VAR &&
                    strcmp(inner_index->u.index.obj->u.var.name, "self") == 0 &&
                    inner_index->u.index.index->kind == AST_STRING) {

                    char* inner_field = inner_index->u.index.index->u.string.value;
                    // 外部索引可能是字符串（字段名）或数字（数组索引）
                    char* outer_field = NULL;
                    char outer_field_str[32];
                    if (ast->u.index.index->kind == AST_STRING) {
                        outer_field = ast->u.index.index->u.string.value;
                    } else if (ast->u.index.index->kind == AST_NUM) {
                        snprintf(outer_field_str, sizeof(outer_field_str), "%d", (int)ast->u.index.index->u.num.value);
                        outer_field = outer_field_str;
                    } else if (ast->u.index.index->kind == AST_VAR) {
                        outer_field = ast->u.index.index->u.var.name;
                    }

                    // 通过 self 变量的类型来确定 struct 定义
                    // self 的类型就是当前正在定义的 struct 类型（如 LinkedList）
                    Symbol* self_sym = scope_resolve(s->current, "self");
                    Symbol* struct_sym = NULL;

                    if (self_sym && self_sym->type && self_sym->type->kind == TYPE_STRUCT &&
                        self_sym->type->struct_name) {
                        // 查找该类型的 struct 定义
                        Symbol* type_sym = scope_resolve(s->current, self_sym->type->struct_name);
                        if (type_sym && type_sym->type && type_sym->type->kind == TYPE_STRUCT &&
                            type_sym->struct_field_count > 0) {
                            struct_sym = type_sym;
                        }
                    }

                    // 如果找不到，尝试查找当前正在定义的 struct（用于方法体内）
                    if (!struct_sym) {
                        Scope* current_scope = s->current;
                        while (current_scope) {
                            for (int j = 0; j < current_scope->sym_cnt; j++) {
                                Symbol* sym = current_scope->syms[j];
                                if (sym->type && sym->type->kind == TYPE_STRUCT &&
                                    sym->struct_field_count > 0) {
                                    // 检查是否有匹配的字段
                                    for (int k = 0; k < sym->struct_field_count; k++) {
                                        if (strcmp(sym->struct_field_names[k], inner_field) == 0) {
                                            struct_sym = sym;
                                            break;
                                        }
                                    }
                                    if (struct_sym) break;
                                }
                            }
                            if (struct_sym) break;
                            current_scope = current_scope->parent;
                        }
                    }

                    // 检查内层字段是否为 null 默认值
                    if (struct_sym && struct_sym->struct_field_null_default) {
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], inner_field) == 0) {
                                if (struct_sym->struct_field_null_default[i]) {
                                    char msg[BUFFER_MEDIUM];
                                    if (outer_field) {
                                        snprintf(msg, sizeof(msg),
                                            "字段 'self.%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                            inner_field, outer_field);
                                    } else {
                                        snprintf(msg, sizeof(msg),
                                            "字段 'self.%s' 默认值为 null，访问其成员可能导致运行时错误",
                                            inner_field);
                                    }
                                    error_add(ERR_SEMANTIC, ast->line, msg);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // 检查外部变量链式访问中的 null 默认值（如 o.inner.value 或 o.items[0]）
            // 当 obj 是 FIELD_ACCESS 类型时，检查该字段是否有 null 默认值
            if (ast->u.index.obj->kind == AST_FIELD_ACCESS) {
                Ast* field_access = ast->u.index.obj;
                char* inner_field = field_access->u.field_access.field_name;

                // 外部索引可能是字符串（字段名）或数字（数组索引）
                char* outer_field = NULL;
                char outer_field_str[32];
                if (ast->u.index.index->kind == AST_STRING) {
                    outer_field = ast->u.index.index->u.string.value;
                } else if (ast->u.index.index->kind == AST_NUM) {
                    snprintf(outer_field_str, sizeof(outer_field_str), "%d", (int)ast->u.index.index->u.num.value);
                    outer_field = outer_field_str;
                } else if (ast->u.index.index->kind == AST_VAR) {
                    outer_field = ast->u.index.index->u.var.name;
                }

                // 获取被访问对象（如 o）的类型信息，而不是字段（如 inner）的类型
                TypeInfo* obj_type_info = NULL;
                Symbol* struct_sym = NULL;
                const char* base_var_name = NULL;

                if (field_access->u.field_access.obj) {
                    obj_type_info = infer_expr_type(s, field_access->u.field_access.obj);
                }

                // 从对象类型获取 struct 定义（如 Outer）
                if (obj_type_info && obj_type_info->kind == TYPE_STRUCT && obj_type_info->struct_name) {
                    Symbol* type_sym = scope_resolve(s->current, obj_type_info->struct_name);
                    if (type_sym && type_sym->type && type_sym->type->kind == TYPE_STRUCT &&
                        type_sym->struct_field_count > 0) {
                        struct_sym = type_sym;
                    }
                }

                // 尝试获取基础变量名（如 o.inner 中的 o）
                if (field_access->u.field_access.obj && field_access->u.field_access.obj->kind == AST_VAR) {
                    base_var_name = field_access->u.field_access.obj->u.var.name;
                }

                if (obj_type_info) type_free(obj_type_info);

                // 检查内层字段（如 inner）是否为 null 默认值
                if (struct_sym && struct_sym->struct_field_null_default) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        if (strcmp(struct_sym->struct_field_names[i], inner_field) == 0) {
                            if (struct_sym->struct_field_null_default[i]) {
                                char msg[BUFFER_MEDIUM];
                                if (base_var_name && outer_field) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s.%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                        base_var_name, inner_field, outer_field);
                                } else if (base_var_name) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s.%s' 默认值为 null，访问其成员可能导致运行时错误",
                                        base_var_name, inner_field);
                                } else if (outer_field) {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s' 默认值为 null，访问其字段 '%s' 可能导致运行时错误",
                                        inner_field, outer_field);
                                } else {
                                    snprintf(msg, sizeof(msg),
                                        "字段 '%s' 默认值为 null，访问其成员可能导致运行时错误",
                                        inner_field);
                                }
                                error_add(ERR_SEMANTIC, ast->line, msg);
                            }
                            break;
                        }
                    }
                }
            }

            if (obj_type) type_free(obj_type);

            // 推断并缓存 INDEX 节点的类型
            infer_expr_type(s, ast);
            break;
            
        case AST_SLICE:
            visit(s, ast->u.slice.obj);
            if (ast->u.slice.start) visit(s, ast->u.slice.start);
            if (ast->u.slice.end) visit(s, ast->u.slice.end);
            
            // 编译时检查切片对象类型
            TypeInfo* slice_obj_type = infer_expr_type(s, ast->u.slice.obj);
            bool is_valid_slice_target = false;
            
            if (slice_obj_type) {
                if (slice_obj_type->kind == TYPE_ARRAY || slice_obj_type->kind == TYPE_ANY) {
                    is_valid_slice_target = true;
                }
                // 检查是否是 cstruct 数组字段访问
                else if (ast->u.slice.obj->kind == AST_FIELD_ACCESS) {
                    Ast* field_access = ast->u.slice.obj;
                    if (field_access->u.field_access.obj) {
                        TypeInfo* obj_type = infer_expr_type(s, field_access->u.field_access.obj);
                        if (obj_type && obj_type->kind == TYPE_CSTRUCT) {
                            // 检查字段是否是数组字段
                            const char* field_name = field_access->u.field_access.field_name;
                            const char* var_name = NULL;
                            
                            if (field_access->u.field_access.obj->kind == AST_VAR) {
                                var_name = field_access->u.field_access.obj->u.var.name;
                            }
                            
                            if (var_name) {
                                Symbol* var_sym = scope_resolve(s->current, var_name);
                                if (var_sym && var_sym->struct_field_count > 0) {
                                    for (int i = 0; i < var_sym->struct_field_count; i++) {
                                        if (strcmp(var_sym->struct_field_names[i], field_name) == 0) {
                                            // 检查符号表中是否存储了数组维度信息
                                            // 这里我们假设如果字段名匹配，可能是数组字段
                                            // 运行时会在 OP_SLICE 中再次检查
                                            is_valid_slice_target = true;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                        if (obj_type) type_free(obj_type);
                    }
                }
                
                if (!is_valid_slice_target) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片操作需要数组类型，实际为 %s",
                             type_kind_to_string(slice_obj_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
            }
            if (slice_obj_type) type_free(slice_obj_type);
            
            // 检查起始索引类型
            if (ast->u.slice.start) {
                TypeInfo* start_type = infer_expr_type(s, ast->u.slice.start);
                if (start_type && start_type->kind != TYPE_INT && start_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片起始索引必须是整数类型，实际为 %s",
                             type_kind_to_string(start_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (start_type) type_free(start_type);
            }
            
            // 检查结束索引类型
            if (ast->u.slice.end) {
                TypeInfo* end_type = infer_expr_type(s, ast->u.slice.end);
                if (end_type && end_type->kind != TYPE_INT && end_type->kind != TYPE_ANY) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "切片结束索引必须是整数类型，实际为 %s",
                             type_kind_to_string(end_type->kind));
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                }
                if (end_type) type_free(end_type);
            }
            break;
            
        case AST_RETURN: {
            visit(s, ast->u.ret);

            if (s->current_func) {
                TypeInfo* return_type_info = infer_expr_type(s, ast->u.ret);
                if (return_type_info) {
                    TypeInfo* expected_type = s->current_func->u.func.return_type;
                    
                    // TYPE_INFER 表示函数声明时未指定返回类型，保持为 any（不推断）
                    if (!expected_type || expected_type->kind == TYPE_INFER) {
                        // 省略返回类型的函数返回 any，不进行推断
                        // 保持 TYPE_INFER（在类型系统中等同于 any）
                    } else if (!type_is_compatible(expected_type, return_type_info)) {
                        // 返回类型不匹配
                        const char* expected_str = type_to_string(expected_type);
                        char expected_buf[64];
                        strncpy(expected_buf, expected_str, 63);
                        expected_buf[63] = '\0';
                        const char* actual_str = type_to_string(return_type_info);
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "返回类型不匹配：期望 %s，实际 %s",
                                 expected_buf, actual_str);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                    type_free(return_type_info);
                }
            }
            break;
        }
            
        case AST_EXPR_STMT:
            visit(s, ast->u.expr_stmt.expr);
            break;

        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) {
                visit(s, ast->u.array.items[i]);
            }
            break;

        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                visit(s, ast->u.dict.entries[i].key);
                visit(s, ast->u.dict.entries[i].value);
            }
            break;

        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count - 1; i++) {
                if (ast->u.interp_string.exprs[i]) {
                    visit(s, ast->u.interp_string.exprs[i]);
                }
            }
            break;

        case AST_IMPORT: {
            if (strstr(ast->u.import.module_name, ".leno") != NULL) {
                const char* alias = ast->u.import.alias;
                char* module_name = NULL;
                // 在模块上下文中使用 SYM_MODULE，否则使用 SYM_GLOBAL
                SymKind module_sym_kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                if (!alias) {
                    alias = strrchr(ast->u.import.module_name, '/');
                    if (!alias) alias = strrchr(ast->u.import.module_name, '\\');
                    if (alias) alias++;
                    else alias = ast->u.import.module_name;
                    
                    module_name = strdup(alias);
                    char* dot = strrchr(module_name, '.');
                    if (dot) *dot = '\0';
                    
                    Symbol* sym = scope_define(s->current, module_name, module_sym_kind);
                    if (sym) {
                        sym->type = type_new(TYPE_DICT);
                    }
                } else {
                    module_name = strdup(alias);
                    
                    Symbol* sym = scope_define(s->current, alias, module_sym_kind);
                    if (sym) {
                        sym->type = type_new(TYPE_DICT);
                    }
                }
                
                if (s->imported_module_count < 64 && ast->u.import.file_path) {
                    ImportedModuleInfo* info = &s->imported_modules[s->imported_module_count++];
                    info->alias = module_name;
                    info->file_path = strdup(ast->u.import.file_path);

                    // 创建模块符号表并扫描模块文件
                    info->sym_table = module_symbol_table_create(ast->u.import.file_path);
                    if (info->sym_table) {
                        const char* current_file = error_get_filename();
                        module_symbol_table_scan(info->sym_table, current_file);

                        // 注意：不再将模块中的 struct 定义注册到当前作用域
                        // struct 必须通过模块名访问，例如：new module.Point()
                        // 这样可以保持与 func 和 var 的一致性

                        // 但是，需要将模块中的 face 定义注册到全局表
                        // 这样模块中的 struct 可以实现这些 face
                        for (int fi = 0; fi < info->sym_table->face_count; fi++) {
                            ModuleFaceSymbol* face_sym = &info->sym_table->faces[fi];
                            if (!face_def_find(face_sym->name)) {
                                ObjFaceDef* fdef = face_def_new(face_sym->name, face_sym->method_count);
                                if (fdef) {
                                    for (int mi = 0; mi < face_sym->method_count; mi++) {
                                        fdef->methods[mi].name = strdup(face_sym->methods[mi].name);
                                        fdef->methods[mi].return_type = type_new(face_sym->methods[mi].return_type);
                                        fdef->methods[mi].param_count = face_sym->methods[mi].param_count;
                                    }
                                    face_def_register(fdef);
                                }
                            }
                        }

                        // 将模块中的 alias 定义注册到当前作用域
                        for (int ai = 0; ai < info->sym_table->alias_count; ai++) {
                            ModuleAliasSymbol* alias_sym = &info->sym_table->aliases[ai];
                            // 注册为类型符号，让后续 parse_type 能识别
                            Symbol* sym = scope_define(s->current, alias_sym->name, SYM_TYPE);
                            if (sym && alias_sym->type_info) {
                                sym->type = type_copy(alias_sym->type_info);
                            }
                        }

                        // 将模块中的 struct 定义注册到全局表
                        // 这样类型检查时可以验证 struct 是否实现了 face
                        for (int si = 0; si < info->sym_table->struct_count; si++) {
                            ModuleStructSymbol* struct_sym = &info->sym_table->structs[si];
                            if (!struct_sym->is_cstruct && !struct_def_find(struct_sym->name)) {
                                ObjStructDef* sdef = struct_def_new(struct_sym->name, struct_sym->field_count, struct_sym->method_count);
                                if (sdef) {
                                    // 设置 struct 方法名
                                    for (int mi = 0; mi < struct_sym->method_count; mi++) {
                                        const char* full_method_name = struct_sym->methods[mi].name;
                                        const char* method_name = full_method_name;
                                        const char* sep = strstr(full_method_name, "::");
                                        if (sep) {
                                            method_name = sep + 2;
                                        }
                                        sdef->methods[mi].name = strdup(method_name);
                                    }

                                    // 设置 impl 信息
                                    if (struct_sym->impl_count > 0) {
                                        sdef->impl_count = struct_sym->impl_count;
                                        sdef->impl_names = (char**)malloc(sizeof(char*) * struct_sym->impl_count);
                                        for (int ii = 0; ii < struct_sym->impl_count; ii++) {
                                            sdef->impl_names[ii] = strdup(struct_sym->impl_names[ii]);
                                        }
                                    }

                                    struct_def_register(sdef);
                                }
                            }
                        }
                    }
                } else {
                    free(module_name);
                }
            } else {
                // 原生模块：运行时通过 OP_LOAD_NATIVE_MODULE 加载
                // 记录原生模块导入，用于编译时检查
                const char* alias = ast->u.import.alias ? ast->u.import.alias : ast->u.import.module_name;
                if (s->imported_module_count < 64) {
                    ImportedModuleInfo* info = &s->imported_modules[s->imported_module_count++];
                    info->alias = strdup(alias);
                    info->file_path = NULL;  // 原生模块没有文件路径
                }

                // 注册模块别名（如果有显式别名）
                if (ast->u.import.alias) {
                    native_register_module_alias(ast->u.import.alias, ast->u.import.module_name);
                }
                
                // 将别名或模块名定义为一个全局符号，以便在模块调用时可以解析
                // 在模块上下文中使用 SYM_MODULE，否则使用 SYM_GLOBAL
                SymKind module_sym_kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                Symbol* sym = scope_define(s->current, alias, module_sym_kind);
                if (sym) {
                    sym->type = type_new(TYPE_DICT);
                }
            }
            break;
        }

        case AST_USE: {
            // use 语句：将模块中的 struct 类型导入到当前作用域
            // 注意：use 只支持 struct 类型，不支持 func、var 和 enum
            // - struct：编译时类型，需要用于变量声明（如 Point p）
            // - func/var：运行时实体，必须通过模块名访问（如 module.func()）
            // - enum：成员访问通过模块名（如 module.enum.member），不需要 use
            const char* module_name = ast->u.use.module_name;
            const char* symbol_name = ast->u.use.symbol_name;

            // 查找导入的模块
            ImportedModuleInfo* module_info = find_imported_module(s, module_name);
            if (!module_info) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 未导入", module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            if (!module_info->sym_table) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 没有符号表", module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            // 尝试查找 struct 定义
            ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(module_info->sym_table, symbol_name);
            if (struct_sym) {
                // 将 struct/cstruct 定义注册到当前作用域
                SymKind kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                Symbol* sym = scope_define(s->current, symbol_name, kind);
                if (sym) {
                    // 根据 is_cstruct 标记决定类型
                    TypeKind struct_type = struct_sym->is_cstruct ? TYPE_CSTRUCT : TYPE_STRUCT;
                    sym->type = type_new(struct_type);
                    sym->type->struct_name = strdup(symbol_name);
                    sym->struct_field_count = struct_sym->field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * struct_sym->field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * struct_sym->field_count);
                    for (int i = 0; i < struct_sym->field_count; i++) {
                        sym->struct_field_names[i] = strdup(struct_sym->fields[i].name);
                        sym->struct_field_types[i] = type_new(struct_sym->fields[i].type);
                    }
                }
                // 同时注册 struct 定义到全局表，用于类型检查（face 实现检查）
                if (!struct_sym->is_cstruct && !struct_def_find(symbol_name)) {
                    ObjStructDef* sdef = struct_def_new(symbol_name, struct_sym->field_count, struct_sym->method_count);
                    if (sdef) {
                        // 设置 struct 方法名（用于方法查找）
                        for (int i = 0; i < struct_sym->method_count; i++) {
                            // 方法名在模块符号表中是 "StructName::method_name" 格式
                            // 需要提取出纯方法名部分
                            const char* full_method_name = struct_sym->methods[i].name;
                            const char* method_name = full_method_name;
                            // 查找 :: 分隔符
                            const char* sep = strstr(full_method_name, "::");
                            if (sep) {
                                method_name = sep + 2;  // 跳过 "::"
                            }
                            sdef->methods[i].name = strdup(method_name);
                        }

                        // 设置 impl 信息（face 实现）
                        if (struct_sym->impl_count > 0) {
                            sdef->impl_count = struct_sym->impl_count;
                            sdef->impl_names = (char**)malloc(sizeof(char*) * struct_sym->impl_count);
                            for (int i = 0; i < struct_sym->impl_count; i++) {
                                sdef->impl_names[i] = strdup(struct_sym->impl_names[i]);
                            }
                        }

                        struct_def_register(sdef);
                    }
                }

                // 将 struct 方法注册到 func_table，以便方法调用时能正确解析
                // 注意：即使 struct 定义已存在，也需要注册方法到 func_table
                for (int mi = 0; mi < struct_sym->method_count; mi++) {
                    const char* full_method_name = struct_sym->methods[mi].name;
                    // 检查是否已注册
                    Ast* existing = func_table_find(&s->func_table, full_method_name);
                    if (!existing) {
                        // 创建一个占位符函数定义 AST
                        Ast* placeholder = ast_new(AST_FUNC_DEF, 0);
                        if (placeholder) {
                            placeholder->u.func.name = strdup(full_method_name);
                            // pcnt = 参数数量 + 1 (self) + param_count (其他参数)
                            // struct 方法的 pcnt 包含 self 参数
                            placeholder->u.func.pcnt = 1 + struct_sym->methods[mi].param_count;
                            placeholder->u.func.return_type = type_new(struct_sym->methods[mi].return_type);
                            if (struct_sym->methods[mi].return_struct_name) {
                                placeholder->u.func.return_type->struct_name = strdup(struct_sym->methods[mi].return_struct_name);
                            }
                            placeholder->u.func.default_count = 0;
                            func_table_add(&s->func_table, full_method_name, placeholder);
                        }
                    }
                }
                break;
            }

            // 尝试查找 face 定义
            ModuleFaceSymbol* face_sym = module_symbol_table_find_face(module_info->sym_table, symbol_name);
            if (face_sym) {
                // 将 face 定义注册到当前作用域
                // face 类型使用 SYM_TYPE，不占用运行时索引
                Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
                if (sym) {
                    sym->type = type_new(TYPE_FACE);
                    sym->type->struct_name = strdup(symbol_name);
                }
                // 同时注册 face 定义到全局表，用于类型检查
                if (!face_def_find(symbol_name)) {
                    ObjFaceDef* fdef = face_def_new(symbol_name, face_sym->method_count);
                    if (fdef) {
                        for (int i = 0; i < face_sym->method_count; i++) {
                            fdef->methods[i].name = strdup(face_sym->methods[i].name);
                            fdef->methods[i].return_type = type_new(face_sym->methods[i].return_type);
                            fdef->methods[i].param_count = 0;
                            fdef->methods[i].param_types = NULL;
                        }
                        face_def_register(fdef);
                    }
                }
                break;
            }

            // 没有找到 struct 或 face
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "use 语句错误：模块 '%s' 中没有 struct 或 face 类型 '%s'", module_name, symbol_name);
            error_add(ERR_SEMANTIC, ast->line, msg);
            break;
        }

        case AST_MODULE_CALL: {
            visit_list(s, &ast->u.module_call.args);
            const char* actual_module = native_resolve_module_alias(ast->u.module_call.module_name);

            int is_native_module = native_is_module(actual_module);

            int is_imported_module = 0;
            ImportedModuleInfo* module_info = find_imported_module(s, ast->u.module_call.module_name);
            if (module_info) {
                is_imported_module = 1;
            }

            // 检查模块是否已导入：原生模块必须显式导入，用户模块也必须导入
            if (is_native_module && !is_imported_module) {
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "模块 '%s' 未导入，请先使用 'import %s' 导入模块",
                         ast->u.module_call.module_name, ast->u.module_call.module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
                break;
            }

            if (is_native_module) {
                // 原生模块调用，检查方法是否存在
                // 先检查方法是否存在（通过查找元信息）
                extern ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name);
                ModuleMethodMeta* meta = native_find_module_method(actual_module, ast->u.module_call.method_name);
                
                if (!meta) {
                    // 方法不存在
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未找到模块方法: %s.%s",
                             actual_module, ast->u.module_call.method_name);
                    error_add(ERR_SEMANTIC, ast->line, msg);
                } else {
                    // 方法存在，检查参数数量
                    int expected_arity = meta->arity;
                    int actual_arity = ast->u.module_call.args.count;

                    // arity >= 0: 固定参数数量
                    if (expected_arity >= 0 && actual_arity != expected_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数数量不匹配: 期望 %d, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, expected_arity, actual_arity);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    // arity < 0: 可变参数，检查是否少于 min_arity
                    else if (expected_arity < 0 && meta->min_arity >= 0 && actual_arity < meta->min_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数过少: 最少 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->min_arity, actual_arity);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    // arity < 0: 可变参数，检查是否超过 max_arity
                    else if (expected_arity < 0 && meta->max_arity >= 0 && actual_arity > meta->max_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "%s.%s 参数过多: 最多 %d 个, 实际 %d",
                                 actual_module, ast->u.module_call.method_name, meta->max_arity, actual_arity);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    
                    // 检查参数类型
                    // 先找到第一个数组参数及其元素类型
                    TypeInfo* array_elem_type = NULL;
                    for (int j = 0; j < actual_arity; j++) {
                        TypeKind param_type = native_get_module_method_param_type(actual_module, ast->u.module_call.method_name, j);
                        if (param_type == TYPE_ARRAY) {
                            Ast* arg = ast->u.module_call.args.items[j];
                            TypeInfo* arg_type = infer_expr_type(s, arg);
                            if (arg_type && arg_type->kind == TYPE_ARRAY && arg_type->element_type) {
                                array_elem_type = arg_type->element_type;
                                break;
                            }
                        }
                    }
                    
                    for (int i = 0; i < actual_arity; i++) {
                        TypeKind expected_type = native_get_module_method_param_type(actual_module, ast->u.module_call.method_name, i);
                        Ast* arg = ast->u.module_call.args.items[i];
                        TypeInfo* arg_type = infer_expr_type(s, arg);
                        
                        // 如果注册表中是 TYPE_ANY，且已知数组元素类型，则用元素类型检查
                        if (expected_type == TYPE_ANY && array_elem_type) {
                            expected_type = array_elem_type->kind;
                        }
                        
                        // 处理数组元素修改方法（add/insert）：向空数组添加第一个元素时更新类型
                        const char* method_name = ast->u.module_call.method_name;
                        if (type_utils_is_array_element_mutator(method_name)) {
                        int elem_param_idx = type_utils_get_array_element_param_index(method_name, 1); // 1 = 模块调用
                        if (i == elem_param_idx) {
                            // 获取第一个参数（数组）
                            Ast* arr_arg = ast->u.module_call.args.items[0];
                            Symbol* arr_sym = type_utils_resolve_var_symbol(s, arr_arg);
                            if (type_utils_try_update_array_element_type(arr_sym, arg_type)) {
                                // 类型已更新，同时更新期望参数类型
                                expected_type = arg_type->kind;
                            }
                        }
                    }
                    
                    if (expected_type != TYPE_ANY && arg_type && arg_type->kind != TYPE_ANY && arg_type->kind != expected_type) {
                        // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                        bool type_compatible = false;
                        if ((expected_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                            (expected_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                            (expected_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                            type_compatible = true;
                        }
                        // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                        if ((expected_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                            (expected_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                            type_compatible = true;
                        }
                        // 允许 Style[xxx] 传给 Dict 参数
                        if (expected_type == TYPE_DICT && arg_type->kind == TYPE_STYLE) {
                            type_compatible = true;
                        }
                        // 允许 cfunc 类型匹配
                        if (expected_type == TYPE_CFUNC && arg_type->kind == TYPE_CFUNC) {
                            type_compatible = true;
                        }
                        if (!type_compatible) {
                            char msg[BUFFER_MEDIUM];
                            const char* hint = get_type_conversion_hint(expected_type, arg_type->kind);
                            if (hint) {
                                snprintf(msg, sizeof(msg), "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'\n%s",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type), hint);
                            } else {
                                snprintf(msg, sizeof(msg), "%s.%s 参数 %d 类型错误: 期望 '%s'，但传入 '%s'",
                                         actual_module, ast->u.module_call.method_name, i + 1,
                                         type_kind_to_string(expected_type), type_to_string(arg_type));
                            }
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                    
                    if (arg_type) type_free(arg_type);
                } // 关闭 for 循环
                } // 关闭 else 块（方法存在时的处理）
                
                // 如果返回类型是 TYPE_ANY，方法可能不存在，但不做错误报告
                // 因为某些原生方法可能返回 TYPE_ANY
            } else if (is_imported_module && module_info && module_info->file_path) {
                // 用户模块调用，检查方法是否存在
                const char* current_file = error_get_filename();
                
                // 首先检查是否是 struct 定义
                ModuleSymbolTable* sym_table = module_symbol_table_create(module_info->file_path);
                int is_struct = 0;
                if (sym_table) {
                    if (module_symbol_table_scan(sym_table, current_file) == 0) {
                        ModuleStructSymbol* struct_sym = module_symbol_table_find_struct(sym_table, ast->u.module_call.method_name);
                        if (struct_sym) {
                            is_struct = 1;
                            // 是 struct 定义，提示使用 new 关键字
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "struct '%s.%s' 需要使用 new 关键字实例化，例如: new %s.%s()",
                                    ast->u.module_call.module_name, struct_sym->name,
                                    ast->u.module_call.module_name, struct_sym->name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                    module_symbol_table_destroy(sym_table);
                }
                
                // 如果不是 struct，检查是否是函数，并推断返回类型
                if (!is_struct) {
                    int has_method = module_has_method(module_info->file_path, current_file, 
                                                       ast->u.module_call.method_name);
                    if (has_method == 0) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "模块 '%s' 中没有方法 '%s'",
                                 ast->u.module_call.module_name, ast->u.module_call.method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else if (module_info->sym_table) {
                        // 从模块符号表推断返回类型
                        ModuleFuncSymbol* func_sym = module_symbol_table_find_func(
                            module_info->sym_table, ast->u.module_call.method_name);
                        if (func_sym && func_sym->return_type != TYPE_ANY && func_sym->return_type != TYPE_INFER) {
                            ast->cached_type = type_new(func_sym->return_type);
                            if (func_sym->return_struct_name) {
                                ast->cached_type->struct_name = strdup(func_sym->return_struct_name);
                            }
                        }
                    }
                }
            } else {
                // 不是模块调用，可能是实例方法调用
                // 使用resolve_variable_with_upvalue处理变量，支持闭包
                SymRef ref;
                memset(&ref, 0, sizeof(ref));
                Symbol* obj_sym = resolve_variable_with_upvalue(s, ast->u.module_call.module_name, &ref);
                if (obj_sym) {
                    const char* method_name = ast->u.module_call.method_name;
                    int expected_arity = -1;
                    TypeKind return_type;

                    // 检查是否是 struct 方法调用
                    int is_struct_method = 0;
                    Ast* method_def = NULL;

                    // 使用 infer_expr_type 获取对象类型（更准确）
                    Ast* temp_var = ast_new(AST_VAR, ast->line);
                    temp_var->u.var.name = strdup(ast->u.module_call.module_name);
                    temp_var->u.var.ref.kind = ref.kind;
                    temp_var->u.var.ref.index = ref.index;
                    temp_var->u.var.ref.name = ref.name ? strdup(ref.name) : NULL;
                    temp_var->u.var.ref.type_kind = ref.type_kind;
                    temp_var->cached_type = NULL;  // 确保 cached_type 为 NULL
                    TypeInfo* obj_type = infer_expr_type(s, temp_var);

                    // 检查类型是否是 clib
                    int is_clib_call = 0;
                    if (!is_struct_method && obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                        // 查找 clib 符号
                        Symbol* clib_sym = scope_resolve_tree_bfs(s->root_scope, obj_type->struct_name);
                        if (!clib_sym) {
                            clib_sym = scope_resolve(s->current, obj_type->struct_name);
                        }
                        if (clib_sym && clib_sym->clib_func_count > 0) {
                            for (int fi = 0; fi < clib_sym->clib_func_count; fi++) {
                                if (strcmp(clib_sym->clib_func_names[fi], method_name) == 0) {
                                    is_clib_call = 1;
                                    // 检查参数数量
                                    int expected_count = clib_sym->clib_func_param_counts[fi];
                                    int actual_count = ast->u.module_call.args.count;
                                    if (actual_count != expected_count) {
                                        char msg[BUFFER_MEDIUM];
                                        snprintf(msg, sizeof(msg), "clib '%s' 函数 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                 obj_type->struct_name, method_name, expected_count, actual_count);
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                    // 检查每个参数类型
                                    for (int pi = 0; pi < actual_count && pi < expected_count; pi++) {
                                        TypeInfo* expected_param = clib_sym->clib_func_param_types[fi][pi];
                                        Ast* arg = ast->u.module_call.args.items[pi];
                                        TypeInfo* arg_type = infer_expr_type(s, arg);
                                        if (expected_param && arg_type && !type_is_compatible(expected_param, arg_type)) {
                                            if (!(expected_param->kind == TYPE_FLOAT && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR) &&
                                                !(expected_param->kind == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) &&
                                                !(expected_param->kind == TYPE_PTR && arg_type->kind == TYPE_NULL) &&
                                                !(expected_param->kind == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) &&
                                                !(expected_param->kind == TYPE_STR8 && arg_type->kind == TYPE_STRING) &&
                                                !(expected_param->kind == TYPE_STR8 && arg_type->kind == TYPE_NULL) &&
                                                !(expected_param->kind == TYPE_STR16 && arg_type->kind == TYPE_STRING) &&
                                                !(expected_param->kind == TYPE_STR16 && arg_type->kind == TYPE_NULL) &&
                                                !(expected_param->kind == TYPE_I32 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_I64 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_U32 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_U64 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_I8 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_U8 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_I16 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_U16 && arg_type->kind == TYPE_INT) &&
                                                !(expected_param->kind == TYPE_F64 && arg_type->kind == TYPE_FLOAT) &&
                                                !(expected_param->kind == TYPE_F32 && arg_type->kind == TYPE_FLOAT) &&
                                                !(expected_param->kind == TYPE_CSTRUCT && arg_type->kind == TYPE_CSTRUCT) &&
                                                !(expected_param->kind == TYPE_PTR && arg_type->kind == TYPE_CSTRUCT) &&
                                                !(expected_param->kind == TYPE_PTR_GENERIC && arg_type->kind == TYPE_CSTRUCT) &&
                                                !(expected_param->kind == TYPE_CSTRUCT && arg_type->kind == TYPE_PTR) &&
                                                !(expected_param->kind == TYPE_CSTRUCT && arg_type->kind == TYPE_NULL)) {
                                                char msg[BUFFER_MEDIUM];
                                                format_type_error(msg, sizeof(msg),
                                                    "clib '%s3' 函数 '%s4' 参数 %d 类型不匹配: 期望 '%s1', 实际 '%s2'",
                                                    expected_param, arg_type,
                                                    obj_type->struct_name, method_name);
                                                // 追加参数序号
                                                char final_msg[BUFFER_MEDIUM];
                                                snprintf(final_msg, sizeof(final_msg), msg, pi + 1);
                                                error_add(ERR_TYPE_MISMATCH, ast->line, final_msg);
                                            }
                                        }
                                        if (arg_type) type_free(arg_type);
                                    }
                                    break;
                                }
                            }
                        }
                        if (!is_clib_call) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                     obj_type->struct_name, method_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        } else {
                            // clib 调用验证通过，标记 cached_type 让 codegen 识别
                            ast->cached_type = type_new(TYPE_CLIB);
                            ast->cached_type->struct_name = strdup(obj_type->struct_name);
                            // 同时保存 lib 变量的 ref 信息
                            ast->u.module_call.lib_ref = ref;
                            if (ref.name) ast->u.module_call.lib_ref.name = strdup(ref.name);
                        }
                    }

                    // clib 调用不继续走 struct 检查
                    if (is_clib_call) {
                        // 释放临时对象
                        if (obj_type) type_free(obj_type);
                        free(temp_var->u.var.name);
                        if (temp_var->u.var.ref.name) free(temp_var->u.var.ref.name);
                        free(temp_var);
                        break;
                    }

                    // 检查类型是否是 struct 或 cstruct
                    if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE) && obj_type->struct_name) {
                        if (obj_type->kind == TYPE_FACE) {
                            ObjFaceDef* fdef = face_def_find(obj_type->struct_name);
                            if (fdef) {
                                for (int fi = 0; fi < fdef->method_count; fi++) {
                                    if (strcmp(fdef->methods[fi].name, method_name) == 0) {
                                        is_struct_method = 1;
                                        break;
                                    }
                                }
                            } else {
                                // face 可能定义在导入的模块中，从模块符号表查找
                                ImportedModuleInfo* mod_info = find_imported_module(s, ast->u.module_call.module_name);
                                if (mod_info && mod_info->sym_table) {
                                    ModuleFaceSymbol* face_sym = module_symbol_table_find_face(mod_info->sym_table, obj_type->struct_name);
                                    if (face_sym) {
                                        for (int fi = 0; fi < face_sym->method_count; fi++) {
                                            if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                is_struct_method = 1;
                                                break;
                                            }
                                        }
                                    }
                                }
                                // 也检查其他导入的模块
                                if (!is_struct_method) {
                                    for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                        ImportedModuleInfo* m = &s->imported_modules[mi];
                                        if (m && m->sym_table) {
                                            ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, obj_type->struct_name);
                                            if (face_sym) {
                                                for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                    if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                        is_struct_method = 1;
                                                        break;
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } else {
                            char method_key[256];
                            snprintf(method_key, sizeof(method_key), "%s::%s", obj_type->struct_name, method_name);
                            method_def = func_table_find(&s->func_table, method_key);
                            if (method_def && method_def->kind == AST_FUNC_DEF) {
                                is_struct_method = 1;
                            }

                            if (!is_struct_method) {
                                int face_param_count = 0;
                                TypeKind face_return_type = TYPE_ANY;
                                if (struct_def_has_face_method(obj_type->struct_name, method_name, &face_param_count, &face_return_type)) {
                                    is_struct_method = 1;
                                }
                            }

                            if (!is_struct_method) {
                                for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ModuleStructMethod* mod_method = module_symbol_table_find_struct_method(
                                            m->sym_table, obj_type->struct_name, method_name);
                                        if (mod_method) {
                                            is_struct_method = 1;
                                            break;
                                        }
                                    }
                                }
                            }

                            if (!is_struct_method) {
                                for (int mi = 0; mi < s->imported_module_count && !is_struct_method; mi++) {
                                    ImportedModuleInfo* m = &s->imported_modules[mi];
                                    if (m && m->sym_table) {
                                        ObjStructDef* sd = struct_def_find(obj_type->struct_name);
                                        if (sd && sd->impl_count > 0) {
                                            for (int ii = 0; ii < sd->impl_count && !is_struct_method; ii++) {
                                                ModuleFaceSymbol* face_sym = module_symbol_table_find_face(m->sym_table, sd->impl_names[ii]);
                                                if (face_sym) {
                                                    for (int fi = 0; fi < face_sym->method_count; fi++) {
                                                        if (strcmp(face_sym->methods[fi].name, method_name) == 0) {
                                                            is_struct_method = 1;
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
                    }
                    if (is_struct_method) {
                        // 自定义 struct 方法的参数类型检查
                        if (obj_type && obj_type->kind == TYPE_STRUCT && obj_type->struct_name) {
                            Ast* struct_def_ast = NULL;
                            for (int si = 0; si < s->root->u.block.count; si++) {
                                Ast* stmt = s->root->u.block.items[si];
                                if (stmt->kind == AST_STRUCT_DEF && strcmp(stmt->u.struct_def.name, obj_type->struct_name) == 0) {
                                    struct_def_ast = stmt;
                                    break;
                                }
                            }
                            if (struct_def_ast) {
                                for (int mi = 0; mi < struct_def_ast->u.struct_def.method_count; mi++) {
                                    Ast* method_ast = struct_def_ast->u.struct_def.methods[mi];
                                    if (method_ast && method_ast->kind == AST_FUNC_DEF &&
                                        strcmp(method_ast->u.func.name, method_name) == 0) {
                                        int actual_arity = ast->u.module_call.args.count;
                                        int method_param_count = method_ast->u.func.pcnt - 1;
                                        if (method_param_count >= 0 && actual_arity != method_param_count) {
                                            char msg[BUFFER_MEDIUM];
                                            snprintf(msg, sizeof(msg), "方法 '%s' 参数数量不匹配: 期望 %d, 实际 %d",
                                                     method_name, method_param_count, actual_arity);
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        } else {
                                            // 构建泛型参数替换表（包含 struct 的泛型参数和方法的泛型参数）
                                            char** gp_names = NULL;
                                            TypeInfo** gp_types = NULL;
                                            int gp_count = 0;

                                            // struct 的泛型参数（如 Box[T] 的 T）
                                            if (obj_type->generic_count > 0 && obj_type->generic_args) {
                                                gp_count = obj_type->generic_count;
                                                gp_names = (char**)malloc(sizeof(char*) * (gp_count + method_ast->u.func.type_param_count));
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * (gp_count + method_ast->u.func.type_param_count));
                                                if (struct_def_ast->u.struct_def.type_params) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        gp_names[gi] = struct_def_ast->u.struct_def.type_params[gi];
                                                        gp_types[gi] = type_copy(obj_type->generic_args[gi]);
                                                    }
                                                }
                                            } else {
                                                gp_names = (char**)malloc(sizeof(char*) * method_ast->u.func.type_param_count);
                                                gp_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * method_ast->u.func.type_param_count);
                                                gp_count = 0;
                                            }

                                            // 方法的泛型参数（如 map[U] 的 U）
                                            if (method_ast->u.func.type_param_count > 0 && method_ast->u.func.type_params) {
                                                for (int gi = 0; gi < method_ast->u.func.type_param_count; gi++) {
                                                    int idx = gp_count;
                                                    gp_names[idx] = method_ast->u.func.type_params[gi];
                                                    if (ast->u.module_call.generic_type_args && gi < ast->u.module_call.generic_type_count) {
                                                        gp_types[idx] = type_copy(ast->u.module_call.generic_type_args[gi]);
                                                    } else {
                                                        gp_types[idx] = type_new(TYPE_ANY);
                                                    }
                                                    gp_count++;
                                                }
                                            }

                                            for (int i = 0; i < actual_arity; i++) {
                                                TypeInfo* expected_type = method_ast->u.func.param_types[i + 1];
                                                if (!expected_type) continue;

                                                TypeInfo* check_type = expected_type;
                                                if (gp_count > 0) {
                                                    for (int gi = 0; gi < gp_count; gi++) {
                                                        TypeInfo* sub = type_substitute(check_type, gp_names[gi], gp_types[gi]);
                                                        if (sub != check_type) {
                                                            check_type = sub;
                                                        }
                                                    }
                                                }

                                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);
                                                if (arg_type && arg_type->kind != TYPE_ANY && check_type->kind != TYPE_ANY) {
                                                    if (!type_is_compatible(check_type, arg_type)) {
                                                        char msg[BUFFER_MEDIUM];
                                                        char idx_str[16];
                                                        snprintf(idx_str, sizeof(idx_str), "%d", i + 1);
                                                        format_type_error(msg, sizeof(msg),
                                                            "%s3 第 %s4 个参数类型不匹配: 期望 %s1, 实际 %s2",
                                                            check_type, arg_type,
                                                            method_name, idx_str);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                                if (arg_type) type_free(arg_type);
                                                if (check_type != expected_type) type_free(check_type);
                                            }

                                            if (gp_types) {
                                                for (int gi = 0; gi < gp_count; gi++) {
                                                    type_free(gp_types[gi]);
                                                }
                                                free(gp_types);
                                                free(gp_names);
                                            }
                                        }
                                        break;
                                    }
                                }
                            }
                        }

                        // 保存原始对象名（用于创建 self 参数）
                        char* original_obj_name = strdup(ast->u.module_call.module_name);
                        // 将 AST_MODULE_CALL 转换为 AST_CALL，callee 为 INDEX(self, "method")
                        free(ast->u.module_call.module_name);

                        // 创建 self 变量节点
                        Ast* self_var = ast_new(AST_VAR, ast->line);
                        self_var->u.var.name = original_obj_name;
                        if (ref.name) {
                            self_var->u.var.ref.kind = ref.kind;
                            self_var->u.var.ref.index = ref.index;
                            self_var->u.var.ref.name = strdup(ref.name);
                            self_var->u.var.ref.type_kind = ref.type_kind;
                            self_var->u.var.ref.struct_name = (ref.struct_name) ? strdup(ref.struct_name) : NULL;
                        } else if (obj_sym) {
                            self_var->u.var.ref.kind = obj_sym->kind;
                            self_var->u.var.ref.index = obj_sym->index;
                            self_var->u.var.ref.name = strdup(obj_sym->name);
                            self_var->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                            self_var->u.var.ref.struct_name = (obj_sym->type && obj_sym->type->struct_name) ? strdup(obj_sym->type->struct_name) : NULL;
                        }
                        if (obj_type) {
                            self_var->cached_type = type_copy(obj_type);
                        }

                        // 创建方法名字符串节点
                        Ast* method_str = ast_new(AST_STRING, ast->line);
                        method_str->u.string.value = strdup(method_name);
                        method_str->u.string.len = (int)strlen(method_name);

                        // 创建 INDEX 节点：self["method"]
                        Ast* index_ast = ast_new(AST_INDEX, ast->line);
                        index_ast->u.index.obj = self_var;
                        index_ast->u.index.index = method_str;

                        // 将 self 参数添加到参数列表开头
                        AstList new_args;
                        ast_list_init(&new_args);
                        Ast* self_arg = ast_new(AST_VAR, ast->line);
                        self_arg->u.var.name = strdup(original_obj_name);
                        if (ref.name) {
                            self_arg->u.var.ref.kind = ref.kind;
                            self_arg->u.var.ref.index = ref.index;
                            self_arg->u.var.ref.name = strdup(ref.name);
                            self_arg->u.var.ref.type_kind = ref.type_kind;
                            self_arg->u.var.ref.struct_name = (ref.struct_name) ? strdup(ref.struct_name) : NULL;
                        } else if (obj_sym) {
                            self_arg->u.var.ref.kind = obj_sym->kind;
                            self_arg->u.var.ref.index = obj_sym->index;
                            self_arg->u.var.ref.name = strdup(obj_sym->name);
                            self_arg->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                            self_arg->u.var.ref.struct_name = (obj_sym->type && obj_sym->type->struct_name) ? strdup(obj_sym->type->struct_name) : NULL;
                        }
                        ast_list_add(&new_args, self_arg);
                        for (int i = 0; i < ast->u.module_call.args.count; i++) {
                            ast_list_add(&new_args, ast->u.module_call.args.items[i]);
                        }
                        free(ast->u.module_call.args.items);

                        // 转换 AST 类型
                        ast->kind = AST_CALL;
                        ast->u.call.callee = index_ast;
                        ast->u.call.args = new_args;
                        ast->u.call.is_tail_call = 0;
                    }

                    // 释放临时对象
                    if (obj_type) type_free(obj_type);
                    free(temp_var->u.var.name);
                    if (temp_var->u.var.ref.name) free(temp_var->u.var.ref.name);
                    free(temp_var);

                    if (!is_struct_method) {
                        // 根据对象类型确定类型名称
                        const char* obj_type_name = NULL;
                        if (obj_sym->type) {
                            obj_type_name = native_get_type_name(obj_sym->type->kind);
                        }

                        // 根据对象类型名称和方法名查找方法
                        if (obj_type_name) {
                            return_type = native_get_instance_method_return_type(obj_type_name, method_name, &expected_arity);
                        } else {
                            // 如果无法确定对象类型，尝试全局查找（向后兼容）
                            obj_type_name = native_find_instance_method_type(method_name, &expected_arity, &return_type);
                        }

                        // 判断方法是否找到：当 return_type == TYPE_ANY 且 expected_arity == -1 时表示方法不存在
                        int method_found = !(return_type == TYPE_ANY && expected_arity == -1);

                        // 如果没找到，检查是否是从模块导入的 struct/cstruct 的方法
                        if (!method_found && obj_sym->type &&
                            (obj_sym->type->kind == TYPE_STRUCT || obj_sym->type->kind == TYPE_CSTRUCT) &&
                            obj_sym->type->struct_name) {
                            for (int i = 0; i < s->imported_module_count; i++) {
                                ImportedModuleInfo* info = &s->imported_modules[i];
                                if (info->sym_table) {
                                    ModuleStructMethod* method = module_symbol_table_find_struct_method(
                                        info->sym_table, obj_sym->type->struct_name, method_name);
                                    if (method) {
                                        method_found = 1;
                                        return_type = method->return_type;
                                        expected_arity = method->param_count;
                                        break;
                                    }
                                }
                            }
                        }

                        if (method_found) {
                        // 方法在该类型的注册表中存在
                        int actual_arity = ast->u.module_call.args.count;
                        const InstanceMethodMeta* inst_meta = NULL;
                        if (obj_type_name) {
                            inst_meta = native_find_instance_method(obj_type_name, method_name);
                        }

                        // arity >= 0: 固定参数数量
                        if (expected_arity >= 0 && actual_arity != expected_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                                     method_name, expected_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否少于 min_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->min_arity >= 0 && actual_arity < inst_meta->min_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                     method_name, inst_meta->min_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        // arity < 0: 可变参数，检查是否超过 max_arity
                        else if (expected_arity < 0 && inst_meta && inst_meta->max_arity >= 0 && actual_arity > inst_meta->max_arity) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                     method_name, inst_meta->max_arity, actual_arity);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }

                        // 检查参数类型（仅当参数数量匹配或可变参数时）
                        if (obj_type_name && (expected_arity == -1 || actual_arity == expected_arity)) {
                            for (int i = 0; i < actual_arity; i++) {
                                TypeKind expected_param_type = native_get_instance_method_param_type(obj_type_name, method_name, i);
                                TypeInfo* arg_type = infer_expr_type(s, ast->u.module_call.args.items[i]);
                                
                                // 处理数组/字典元素修改方法的类型更新和检查
                                TypeInfo* expected_type_for_check = NULL;
                                
                                // 处理数组元素修改方法（add/insert）
                                if (type_utils_is_array_element_mutator(method_name)) {
                                    int elem_param_idx = type_utils_get_array_element_param_index(method_name, 0); // 0 = 实例方法
                                    if (i == elem_param_idx) {
                                        // 首先尝试更新数组元素类型（如果是第一次 add）
                                        if (type_utils_try_update_array_element_type(obj_sym, arg_type)) {
                                            // 类型已更新，同时更新期望参数类型
                                            expected_param_type = arg_type->kind;
                                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_ARRAY) {
                                            // 数组已有具体元素类型，检查是否匹配
                                            TypeInfo* elem_type = obj_sym->type->element_type;
                                            if (elem_type && elem_type->kind != TYPE_ANY) {
                                                expected_param_type = elem_type->kind;
                                                // 保存完整类型信息用于嵌套类型检查
                                                expected_type_for_check = elem_type;
                                            }
                                        }
                                    }
                                }
                                // 处理字典 set 方法的值类型更新和检查
                                else if (type_utils_is_dict_element_mutator(method_name)) {
                                    int elem_param_idx = type_utils_get_dict_element_param_index(method_name);
                                    if (i == elem_param_idx) {
                                        // 首先尝试更新字典值类型（如果是第一次 set）
                                        if (type_utils_try_update_dict_value_type(obj_sym, arg_type)) {
                                            // 类型已更新，同时更新期望参数类型
                                            expected_param_type = arg_type->kind;
                                        } else if (obj_sym->type && obj_sym->type->kind == TYPE_DICT) {
                                            // 字典已有具体值类型，检查是否匹配
                                            TypeInfo* value_type = obj_sym->type->value_type;
                                            if (value_type && value_type->kind != TYPE_ANY) {
                                                expected_param_type = value_type->kind;
                                                // 保存完整类型信息用于嵌套类型检查
                                                expected_type_for_check = value_type;
                                            }
                                        }
                                    }
                                }
                                // 对于其他方法，如果注册表中是 TYPE_ANY，但容器有具体元素类型，则用该类型检查
                                else if (expected_param_type == TYPE_ANY && obj_sym->type) {
                                    if (obj_sym->type->kind == TYPE_ARRAY) {
                                        TypeInfo* elem_type = obj_sym->type->element_type;
                                        if (elem_type && elem_type->kind != TYPE_ANY) {
                                            expected_param_type = elem_type->kind;
                                            expected_type_for_check = elem_type;
                                        }
                                    } else if (obj_sym->type->kind == TYPE_DICT && i > 0) {
                                        TypeInfo* value_type = obj_sym->type->value_type;
                                        if (value_type && value_type->kind != TYPE_ANY) {
                                            expected_param_type = value_type->kind;
                                            expected_type_for_check = value_type;
                                        }
                                    }
                                }
                                
                                // 类型检查：简单类型比较 kind，复杂类型使用 type_equals 深度比较
                                if (arg_type && arg_type->kind != TYPE_ANY) {
                                    if (expected_type_for_check) {
                                        // 有完整类型信息，进行深度类型比较（支持嵌套类型）
                                        if (!type_is_compatible(expected_type_for_check, arg_type)) {
                                            char msg[BUFFER_MEDIUM];
                                            // 先保存字符串避免 buffer 覆盖
                                            const char* exp_str = type_to_string(expected_type_for_check);
                                            char exp_buf[BUFFER_SMALL];
                                            strncpy(exp_buf, exp_str, sizeof(exp_buf) - 1);
                                            exp_buf[sizeof(exp_buf) - 1] = '\0';
                                            const char* hint = get_type_conversion_hint(expected_type_for_check->kind, arg_type->kind);
                                            if (hint) {
                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                         i + 1, exp_buf, type_to_string(arg_type), hint);
                                            } else {
                                                snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                         i + 1, exp_buf, type_to_string(arg_type));
                                            }
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        }
                                    } else if (expected_param_type != TYPE_ANY) {
                                        // 只有 kind 信息，进行简单类型比较
                                        if (arg_type->kind != expected_param_type) {
                                            // 允许 int -> float, bigint -> int, bigint -> float 的隐式转换
                                            bool type_compatible = false;
                                            if ((expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_INT) ||
                                                (expected_param_type == TYPE_INT && arg_type->kind == TYPE_BIGINT) ||
                                                (expected_param_type == TYPE_FLOAT && arg_type->kind == TYPE_BIGINT)) {
                                                type_compatible = true;
                                            }
                                            // 允许 Ptr[T] <-> Ptr 之间的隐式转换
                                            if ((expected_param_type == TYPE_PTR && arg_type->kind == TYPE_PTR_GENERIC) ||
                                                (expected_param_type == TYPE_PTR_GENERIC && arg_type->kind == TYPE_PTR)) {
                                                type_compatible = true;
                                            }
                                            // 允许 Style[xxx] 传给 Dict 参数
                                            if (expected_param_type == TYPE_DICT && arg_type->kind == TYPE_STYLE) {
                                                type_compatible = true;
                                            }
                                            if (!type_compatible) {
                                                char msg[BUFFER_MEDIUM];
                                                const char* hint = get_type_conversion_hint(expected_param_type, arg_type->kind);
                                                if (hint) {
                                                    snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'\n%s",
                                                             i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type), hint);
                                                } else {
                                                    snprintf(msg, sizeof(msg), "类型错误: 参数 %d 期望 '%s'，但传入 '%s'",
                                                             i + 1, type_kind_to_string(expected_param_type), type_to_string(arg_type));
                                                }
                                                error_add(ERR_SEMANTIC, ast->line, msg);
                                            }
                                        }
                                    }
                                }
                                
                                if (arg_type) type_free(arg_type);
                            }
                        }
                    } else if (!obj_sym->type || obj_sym->type->kind == TYPE_ANY) {
                        // 未初始化变量
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "不能对未初始化的变量 '%s' 调用方法 '%s'",
                                 ast->u.module_call.module_name, method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    } else {
                        // 方法不存在于该类型
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "类型 '%s' 没有方法 '%s'",
                                 type_to_string(obj_sym->type), method_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }

                    // 转换为实例方法调用：obj.method(args) -> obj["method"](args)
                    // 创建属性访问节点 obj["method"]
                    Ast* obj_var = ast_new(AST_VAR, ast->line);
                    obj_var->u.var.name = strdup(ast->u.module_call.module_name);
                    // 使用ref（由resolve_variable_with_upvalue填充）来支持闭包
                    if (ref.name) {
                        obj_var->u.var.ref.kind = ref.kind;
                        obj_var->u.var.ref.index = ref.index;
                        obj_var->u.var.ref.name = strdup(ref.name);
                        obj_var->u.var.ref.type_kind = ref.type_kind;
                    } else if (obj_sym) {
                        obj_var->u.var.ref.kind = obj_sym->kind;
                        obj_var->u.var.ref.index = obj_sym->index;
                        obj_var->u.var.ref.name = strdup(obj_sym->name);
                        obj_var->u.var.ref.type_kind = obj_sym->type ? obj_sym->type->kind : TYPE_ANY;
                    }

                    Ast* method_str = ast_new(AST_STRING, ast->line);
                    method_str->u.string.value = strdup(ast->u.module_call.method_name);
                    method_str->u.string.len = (int)strlen(ast->u.module_call.method_name);

                    Ast* prop_access = ast_new(AST_INDEX, ast->line);
                    prop_access->u.index.obj = obj_var;
                    prop_access->u.index.index = method_str;

                    // 转换为普通函数调用
                    ast->kind = AST_CALL;
                    ast->u.call.callee = prop_access;
                    // 参数列表已经解析好了，直接使用
                    AstList args = ast->u.module_call.args;
                    ast->u.call.args = args;
                }
            } else {
                // 模块和变量都未定义
                char msg[BUFFER_MEDIUM];
                snprintf(msg, sizeof(msg), "未定义的模块或变量: %s", ast->u.module_call.module_name);
                error_add(ERR_SEMANTIC, ast->line, msg);
            }
            }
            break;
        }

        case AST_MODULE_ACCESS: {
            Symbol* module_sym = scope_resolve(s->current, ast->u.module_access.module_name);
            int is_module = 0;
            
            if (module_sym) {
                for (int i = 0; i < s->imported_module_count; i++) {
                    if (strcmp(s->imported_modules[i].alias, ast->u.module_access.module_name) == 0) {
                        is_module = 1;
                        break;
                    }
                }
            }
            
            if (!is_module) {
                // 使用resolve_variable_with_upvalue处理变量，支持闭包
                SymRef ref;
                memset(&ref, 0, sizeof(ref));
                Symbol* var_sym = resolve_variable_with_upvalue(s, ast->u.module_access.module_name, &ref);

                Ast* var_ast = ast_new(AST_VAR, ast->line);
                var_ast->u.var.name = strdup(ast->u.module_access.module_name);
                if (ref.name) {
                    var_ast->u.var.ref.kind = ref.kind;
                    var_ast->u.var.ref.index = ref.index;
                    var_ast->u.var.ref.name = strdup(ref.name);
                    var_ast->u.var.ref.type_kind = ref.type_kind;
                    var_ast->u.var.ref.struct_name = ref.struct_name ? strdup(ref.struct_name) : NULL;
                } else if (var_sym) {
                    var_ast->u.var.ref.kind = var_sym->kind;
                    var_ast->u.var.ref.index = var_sym->index;
                    var_ast->u.var.ref.name = strdup(var_sym->name);
                    var_ast->u.var.ref.type_kind = var_sym->type ? var_sym->type->kind : TYPE_ANY;
                    var_ast->u.var.ref.struct_name = (var_sym->type && var_sym->type->struct_name) ? strdup(var_sym->type->struct_name) : NULL;
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的变量: %s", ast->u.module_access.module_name);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                }
                if (var_sym && var_sym->type) {
                    var_ast->cached_type = type_copy(var_sym->type);
                }
                
                // 检查变量是否是 struct 或 cstruct 类型
                int is_struct_type = 0;
                int is_cstruct_type = 0;
                if (var_sym && var_sym->type) {
                    if (var_sym->type->kind == TYPE_STRUCT) {
                        is_struct_type = 1;
                    } else if (var_sym->type->kind == TYPE_CSTRUCT) {
                        is_cstruct_type = 1;
                    }
                }

                if (is_struct_type || is_cstruct_type) {
                    // struct/cstruct 字段访问：转换为 AST_FIELD_ACCESS
                    char* field_name = strdup(ast->u.module_access.member_name);
                    char* module_name = ast->u.module_access.module_name;
                    char* member_name = ast->u.module_access.member_name;

                    ast->kind = AST_FIELD_ACCESS;
                    ast->u.field_access.obj = var_ast;
                    ast->u.field_access.field_name = field_name;
                    ast->u.field_access.field_index = -1;

                    // 释放旧的内存
                    free(member_name);
                    free(module_name);

                    // 继续访问转换后的节点
                    visit(s, ast);
                } else {
                    // 普通属性访问：转换为 AST_INDEX
                    Ast* index_ast = ast_new(AST_STRING, ast->line);
                    index_ast->u.string.value = strdup(ast->u.module_access.member_name);
                    index_ast->u.string.len = (int)strlen(ast->u.module_access.member_name);

                    ast->kind = AST_INDEX;
                    ast->u.index.obj = var_ast;
                    ast->u.index.index = index_ast;

                    visit(s, index_ast);
                }
            } else {
                // 检查是否是原生模块的常量访问
                const char* actual_module = native_resolve_module_alias(ast->u.module_access.module_name);
                if (native_is_module(actual_module)) {
                    bool found = false;
                    native_find_module_const(actual_module, ast->u.module_access.member_name, &found);
                    if (!found) {
                        // 不是常量，可能是方法名被误解析为 MODULE_ACCESS
                        // 报告错误
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "模块 '%s' 中没有常量 '%s'，如果是方法调用请加括号 ()",
                                 actual_module, ast->u.module_access.member_name);
                        error_add(ERR_SEMANTIC, ast->line, msg);
                    }
                    // 原生模块常量：保留 AST_MODULE_ACCESS，设置类型为 TYPE_INT
                    ast->u.module_access.ref.kind = SYM_MODULE;
                    ast->u.module_access.ref.index = 0;
                    ast->u.module_access.ref.name = strdup(ast->u.module_access.module_name);
                    ast->u.module_access.ref.type_kind = TYPE_INT;
                } else {
                    // .leno 用户模块：按原有逻辑处理
                    ast->u.module_access.ref.kind = module_sym->kind;
                    ast->u.module_access.ref.index = module_sym->index;
                    ast->u.module_access.ref.name = strdup(module_sym->name);
                    ast->u.module_access.ref.type_kind = module_sym->type ? module_sym->type->kind : TYPE_ANY;
                }
            }
            break;
        }

        case AST_EXPORT:
            if (ast->u.export.decl) {
                visit(s, ast->u.export.decl);
            }
            break;

        case AST_TRY: {
            // 访问 try 体（创建独立子作用域）
            {
                Scope* try_scope = scope_new(s->current, 0);
                s->current = try_scope;
                visit(s, ast->u.try_.try_body);
                s->current = try_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, try_scope);
                    scope_free(try_scope);
                }
            }
            
            // 访问 catch 体（如果有，创建独立子作用域）
            if (ast->u.try_.catch_body) {
                Scope* catch_scope = scope_new(s->current, 0);
                s->current = catch_scope;
                
                // 定义 catch 变量
                if (ast->u.try_.catch_var) {
                    Symbol* sym = scope_define(s->current, ast->u.try_.catch_var, SYM_LOCAL);
                    if (sym) {
                        sym->index = allocate_local_index(s);
                        // catch 变量类型为 string（错误信息）
                        sym->type = type_new(TYPE_STRING);
                        // 保存 catch 变量的引用信息
                        ast->u.try_.catch_var_ref.kind = sym->kind;
                        ast->u.try_.catch_var_ref.index = sym->index;
                        ast->u.try_.catch_var_ref.name = strdup(sym->name);
                        ast->u.try_.catch_var_ref.type_kind = TYPE_STRING;
                    }
                }
                
                visit(s, ast->u.try_.catch_body);
                s->current = catch_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, catch_scope);
                    scope_free(catch_scope);
                }
            }
            
            // 访问 finally 体（如果有，创建独立子作用域）
            if (ast->u.try_.finally_body) {
                Scope* finally_scope = scope_new(s->current, 0);
                s->current = finally_scope;
                visit(s, ast->u.try_.finally_body);
                s->current = finally_scope->parent;
                if (!s->is_lsp_mode) {
                    scope_detach_child(s->current, finally_scope);
                    scope_free(finally_scope);
                }
            }
            break;
        }

        case AST_THROW:
            if (ast->u.throw_.expr) {
                visit(s, ast->u.throw_.expr);
            }
            break;

        case AST_TYPE_CHECK:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                Symbol* sym = scope_resolve(s->current, ast->u.type_check.type->struct_name);
                if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                    ast->u.type_check.type->kind = TYPE_FACE;
                }
            }
            break;

        case AST_AS_CAST:
            if (ast->u.type_check.expr) {
                visit(s, ast->u.type_check.expr);
            }
            if (ast->u.type_check.type && ast->u.type_check.type->kind == TYPE_STRUCT && ast->u.type_check.type->struct_name) {
                Symbol* sym = scope_resolve(s->current, ast->u.type_check.type->struct_name);
                if (sym && sym->type && sym->type->kind == TYPE_FACE) {
                    ast->u.type_check.type->kind = TYPE_FACE;
                }
            }
            break;

        case AST_STRUCT_DEF:
            // 注册 struct 类型
            {
                TypeInfo* struct_type = type_new(TYPE_STRUCT);
                struct_type->struct_name = strdup(ast->u.struct_def.name);

                // 泛型 struct：将字段类型中的泛型参数标记为 TYPE_GENERIC_PARAM
                if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        resolve_generic_in_type(ast->u.struct_def.field_types[i],
                            ast->u.struct_def.type_params, ast->u.struct_def.type_param_count);
                    }
                }

                // 先验证所有字段类型是否有效（在注册 struct 之前）
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.struct_def.field_types[i];

                    // 如果字段类型是 struct，验证该 struct 是否已定义
                    if (field_type && field_type->kind == TYPE_STRUCT && field_type->struct_name) {
                        // 允许自引用（struct 包含自身类型的字段，如链表节点）
                        if (strcmp(field_type->struct_name, ast->u.struct_def.name) != 0) {
                            // 检查引用的 struct 是否已定义
                            Symbol* ref_struct = scope_resolve(s->current, field_type->struct_name);
                            if (!ref_struct || ref_struct->type->kind != TYPE_STRUCT) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "struct '%s' 的字段 '%s' 使用了未定义的类型 '%s'",
                                         ast->u.struct_def.name,
                                         ast->u.struct_def.field_names[i],
                                         field_type->struct_name);
                                error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                            }
                        }
                    }
                }

                // 注册到符号表（或更新已预注册的符号）
                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    // 根作用域：模块模式下使用 SYM_MODULE，否则使用 SYM_GLOBAL
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.struct_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_STRUCT) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = struct_type;
                } else {
                    // 未注册，创建新符号
                    sym = scope_define(s->current, ast->u.struct_def.name, kind);
                }

                if (sym) {
                    sym->type = struct_type;

                    // 存储 struct 字段信息
                    sym->struct_field_count = ast->u.struct_def.field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.struct_def.field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.struct_def.field_count);
                    sym->struct_field_null_default = (int*)malloc(sizeof(int) * ast->u.struct_def.field_count);

                    for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                        sym->struct_field_names[i] = strdup(ast->u.struct_def.field_names[i]);
                        sym->struct_field_types[i] = type_copy(ast->u.struct_def.field_types[i]);
                        // 检查字段默认值是否为 null
                        if (ast->u.struct_def.field_defaults[i] &&
                            ast->u.struct_def.field_defaults[i]->kind == AST_NULL) {
                            sym->struct_field_null_default[i] = 1;
                        } else {
                            sym->struct_field_null_default[i] = 0;
                        }
                    }

                    // 存储泛型类型参数信息
                    if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                        sym->struct_type_param_count = ast->u.struct_def.type_param_count;
                        sym->struct_type_params = (char**)malloc(sizeof(char*) * ast->u.struct_def.type_param_count);
                        for (int i = 0; i < ast->u.struct_def.type_param_count; i++) {
                            sym->struct_type_params[i] = strdup(ast->u.struct_def.type_params[i]);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（cstruct/enum）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.struct_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_CSTRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 struct", ast->u.struct_def.name);
                        } else if (existing->type->kind == TYPE_ENUM) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 struct", ast->u.struct_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "struct '%s' 重复定义", ast->u.struct_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }

                // 处理默认值表达式
                for (int i = 0; i < ast->u.struct_def.field_count; i++) {
                    if (ast->u.struct_def.field_defaults[i]) {
                        visit(s, ast->u.struct_def.field_defaults[i]);
                    }
                }

                // 处理方法定义：分两阶段
                // 阶段1：准备所有方法（转换方法体、添加self参数、注册到函数表）
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        // 1. 转换方法体：将字段名访问转换为 self.字段名，方法名调用转换为 self.方法名
                        // 收集所有方法名
                        char** method_names = NULL;
                        int method_count = ast->u.struct_def.method_count;
                        if (method_count > 0) {
                            method_names = (char**)malloc(sizeof(char*) * method_count);
                            for (int m = 0; m < method_count; m++) {
                                method_names[m] = ast->u.struct_def.methods[m]->u.func.name;
                            }
                        }
                        transform_method_body(method_ast->u.func.body, ast->u.struct_def.field_names, ast->u.struct_def.field_count, method_names, method_count, ast->u.struct_def.name);
                        if (method_names) free(method_names);

                        // 2. 添加隐式 self 参数作为第一个参数
                        int new_pcnt = method_ast->u.func.pcnt + 1;
                        char** new_params = (char**)malloc(sizeof(char*) * new_pcnt);
                        TypeInfo** new_param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * new_pcnt);
                        Ast** new_param_defaults = (Ast**)calloc(new_pcnt, sizeof(Ast*));

                        new_params[0] = strdup("self");
                        new_param_types[0] = type_new(TYPE_STRUCT);
                        new_param_types[0]->struct_name = strdup(ast->u.struct_def.name);
                        // 携带泛型类型参数（如 Calc[T] 的 T）
                        if (ast->u.struct_def.type_param_count > 0 && ast->u.struct_def.type_params) {
                            new_param_types[0]->generic_count = ast->u.struct_def.type_param_count;
                            new_param_types[0]->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.struct_def.type_param_count);
                            for (int gi = 0; gi < ast->u.struct_def.type_param_count; gi++) {
                                TypeInfo* gp = type_new(TYPE_GENERIC_PARAM);
                                gp->type_param_name = strdup(ast->u.struct_def.type_params[gi]);
                                new_param_types[0]->generic_args[gi] = gp;
                            }
                        }
                        new_param_defaults[0] = NULL;

                        for (int j = 0; j < method_ast->u.func.pcnt; j++) {
                            new_params[j + 1] = method_ast->u.func.params[j];
                            new_param_types[j + 1] = method_ast->u.func.param_types[j];
                            new_param_defaults[j + 1] = method_ast->u.func.param_defaults ? method_ast->u.func.param_defaults[j] : NULL;
                        }

                        free(method_ast->u.func.params);
                        free(method_ast->u.func.param_types);
                        free(method_ast->u.func.param_defaults);

                        method_ast->u.func.params = new_params;
                        method_ast->u.func.param_types = new_param_types;
                        method_ast->u.func.param_defaults = new_param_defaults;
                        method_ast->u.func.pcnt = new_pcnt;

                        // 3. 注册方法到函数表（使用 struct_name::method_name 格式，避免与全局函数冲突）
                        // 但不注册到符号表作用域链，struct 方法不能从外部直接调用
                        char method_key[256];
                        snprintf(method_key, sizeof(method_key), "%s::%s", ast->u.struct_def.name, method_ast->u.func.name);
                        func_table_add(&s->func_table, method_key, method_ast);
                    }
                }
                
                // 阶段2：处理所有方法体（语义分析）
                // 这样方法间可以互相引用，因为所有方法都已注册到函数表
                // 使用 visit_func_as_struct_method 避免将方法注册到外部作用域
                for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                    Ast* method_ast = ast->u.struct_def.methods[i];
                    if (method_ast && method_ast->kind == AST_FUNC_DEF) {
                        visit_func_as_struct_method(s, method_ast);
                    }
                }

                // 检查 impl 声明：struct 是否真正满足声明的 face
                for (int i = 0; i < ast->u.struct_def.impl_count; i++) {
                    const char* face_name = ast->u.struct_def.impl_names[i];
                    ObjFaceDef* fdef = face_def_find(face_name);
                    if (!fdef) {
                        int found_in_module = 0;
                        for (int mi = 0; mi < s->imported_module_count && !found_in_module; mi++) {
                            ImportedModuleInfo* m = &s->imported_modules[mi];
                            if (m && m->sym_table) {
                                ModuleFaceSymbol* mface = module_symbol_table_find_face(m->sym_table, face_name);
                                if (mface) {
                                    found_in_module = 1;
                                    for (int j = 0; j < mface->method_count; j++) {
                                        const char* mname = mface->methods[j].name;
                                        int found = 0;
                                        for (int k = 0; k < ast->u.struct_def.method_count; k++) {
                                            if (strcmp(ast->u.struct_def.methods[k]->u.func.name, mname) == 0) {
                                                found = 1;
                                                if (mface->methods[j].return_type != TYPE_ANY && ast->u.struct_def.methods[k]->u.func.return_type) {
                                                    if (mface->methods[j].return_type != ast->u.struct_def.methods[k]->u.func.return_type->kind) {
                                                        char msg[256];
                                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                                 ast->u.struct_def.name, mname, face_name,
                                                                 type_kind_to_string(mface->methods[j].return_type),
                                                                 type_kind_to_string(ast->u.struct_def.methods[k]->u.func.return_type->kind));
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                                if (mface->methods[j].param_count >= 0 && ast->u.struct_def.methods[k]->u.func.pcnt >= 1) {
                                                    int expected_params = mface->methods[j].param_count;
                                                    int actual_params = ast->u.struct_def.methods[k]->u.func.pcnt - 1;
                                                    if (expected_params != actual_params) {
                                                        char msg[256];
                                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 参数数量与 face '%s' 不匹配: 期望 %d, 实际 %d",
                                                                 ast->u.struct_def.name, mname, face_name,
                                                                 expected_params, actual_params);
                                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                                    }
                                                }
                                                break;
                                            }
                                        }
                                        if (!found) {
                                            char msg[256];
                                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                                     ast->u.struct_def.name, face_name, mname);
                                            error_add(ERR_SEMANTIC, ast->line, msg);
                                        }
                                    }
                                }
                            }
                        }
                        if (!found_in_module) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "未定义的 face: '%s'", face_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                        continue;
                    }
                    for (int j = 0; j < fdef->method_count; j++) {
                        const char* mname = fdef->methods[j].name;
                        int found = 0;
                        for (int k = 0; k < ast->u.struct_def.method_count; k++) {
                            if (strcmp(ast->u.struct_def.methods[k]->u.func.name, mname) == 0) {
                                found = 1;
                                if (fdef->methods[j].return_type && ast->u.struct_def.methods[k]->u.func.return_type) {
                                    if (fdef->methods[j].return_type->kind != TYPE_ANY &&
                                        fdef->methods[j].return_type->kind != ast->u.struct_def.methods[k]->u.func.return_type->kind) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 返回类型与 face '%s' 不匹配: 期望 %s, 实际 %s",
                                                 ast->u.struct_def.name, mname, face_name,
                                                 type_kind_to_string(fdef->methods[j].return_type->kind),
                                                 type_kind_to_string(ast->u.struct_def.methods[k]->u.func.return_type->kind));
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                }
                                if (fdef->methods[j].param_count >= 0 && ast->u.struct_def.methods[k]->u.func.pcnt >= 0) {
                                    int expected_params = fdef->methods[j].param_count;
                                    int actual_params = ast->u.struct_def.methods[k]->u.func.pcnt - 1;
                                    if (expected_params != actual_params) {
                                        char msg[256];
                                        snprintf(msg, sizeof(msg), "struct '%s' 的方法 '%s' 参数数量与 face '%s' 不匹配: 期望 %d, 实际 %d",
                                                 ast->u.struct_def.name, mname, face_name,
                                                 expected_params, actual_params);
                                        error_add(ERR_SEMANTIC, ast->line, msg);
                                    }
                                }
                                break;
                            }
                        }
                        if (!found) {
                            char msg[256];
                            snprintf(msg, sizeof(msg), "struct '%s' 声明 impl '%s' 但缺少方法 '%s'",
                                     ast->u.struct_def.name, face_name, mname);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                }

                ObjStructDef* early_def = struct_def_find(ast->u.struct_def.name);
                if (!early_def) {
                    early_def = struct_def_new(ast->u.struct_def.name, ast->u.struct_def.field_count, ast->u.struct_def.method_count);
                    if (early_def) {
                        early_def->impl_count = ast->u.struct_def.impl_count;
                        if (early_def->impl_count > 0) {
                            early_def->impl_names = (char**)malloc(sizeof(char*) * early_def->impl_count);
                            for (int i = 0; i < early_def->impl_count; i++) {
                                early_def->impl_names[i] = strdup(ast->u.struct_def.impl_names[i]);
                            }
                        }
                        for (int i = 0; i < ast->u.struct_def.method_count; i++) {
                            early_def->methods[i].name = strdup(ast->u.struct_def.methods[i]->u.func.name);
                        }
                        struct_def_register(early_def);
                    }
                }
            }
            break;

        case AST_FACE_DEF:
            {
                TypeInfo* face_type = type_new(TYPE_FACE);
                face_type->struct_name = strdup(ast->u.face_def.name);

                Symbol* existing = scope_resolve_local(s->current, ast->u.face_def.name);
                if (existing && existing->type && existing->type->kind == TYPE_FACE) {
                    type_free(existing->type);
                    existing->type = face_type;
                } else {
                    SymKind kind = SYM_TYPE;
                    Symbol* sym = scope_define(s->current, ast->u.face_def.name, kind);
                    if (sym) {
                        type_free(sym->type);
                        sym->type = face_type;
                        sym->struct_field_count = ast->u.face_def.method_count;
                        sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.face_def.method_count);
                        sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.face_def.method_count);
                        for (int i = 0; i < ast->u.face_def.method_count; i++) {
                            sym->struct_field_names[i] = strdup(ast->u.face_def.method_names[i]);
                            if (ast->u.face_def.method_return_types[i]) {
                                sym->struct_field_types[i] = type_copy(ast->u.face_def.method_return_types[i]);
                            } else {
                                sym->struct_field_types[i] = type_new(TYPE_ANY);
                            }
                        }
                    }
                }

                ObjFaceDef* fdef = face_def_new(ast->u.face_def.name, ast->u.face_def.method_count);
                if (fdef) {
                    for (int i = 0; i < ast->u.face_def.method_count; i++) {
                        fdef->methods[i].name = strdup(ast->u.face_def.method_names[i]);
                        fdef->methods[i].param_count = ast->u.face_def.method_param_counts[i];
                    }
                    face_def_register(fdef);
                }
            }
            break;

        case AST_CSTRUCT_DEF:
            // 注册 cstruct 类型并计算布局
            {
                // 1. 处理嵌套 cstruct 字段类型
                // 如果字段类型是 TYPE_STRUCT 且类型名对应一个已定义的 cstruct，
                // 则将其转换为 TYPE_CSTRUCT
                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    if (field_type && field_type->kind == TYPE_STRUCT && field_type->struct_name) {
                        // 查找是否是已定义的 cstruct
                        Symbol* nested_cstruct = scope_resolve(s->current, field_type->struct_name);
                        if (nested_cstruct && nested_cstruct->type && nested_cstruct->type->kind == TYPE_CSTRUCT) {
                            // 转换为 TYPE_CSTRUCT
                            type_free(field_type);
                            ast->u.cstruct_def.field_types[i] = type_new(TYPE_CSTRUCT);
                            ast->u.cstruct_def.field_types[i]->struct_name = strdup(nested_cstruct->name);
                        }
                    }
                }

                // 2. 验证所有字段类型是否有效（必须是 C 布局类型）
                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    if (!field_type || !c_layout_is_valid_field_type(field_type->kind)) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cstruct '%s' 的字段 '%s' 使用了无效的 C 布局类型",
                                 ast->u.cstruct_def.name,
                                 ast->u.cstruct_def.field_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    }
                }

                // 3. 计算 C 布局（字段偏移、总大小、对齐要求）
                int* field_offsets = (int*)malloc(sizeof(int) * ast->u.cstruct_def.field_count);
                int current_offset = 0;
                int max_alignment = 1;

                for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                    TypeInfo* field_type = ast->u.cstruct_def.field_types[i];
                    int field_size = c_layout_type_size(field_type->kind);
                    int field_align = c_layout_type_align(field_type->kind);
                    int array_dim = ast->u.cstruct_def.field_array_dims[i];

                    // 如果是嵌套 cstruct，需要从符号表获取其大小和对齐
                    if (field_type->kind == TYPE_CSTRUCT && field_type->struct_name) {
                        Symbol* nested_cstruct = scope_resolve(s->current, field_type->struct_name);
                        if (nested_cstruct && nested_cstruct->type && nested_cstruct->type->kind == TYPE_CSTRUCT) {
                            // 使用已存储的 cstruct 布局信息
                            field_size = nested_cstruct->cstruct_size;
                            field_align = nested_cstruct->cstruct_alignment;
                        }
                    }

                    // 更新最大对齐要求
                    if (field_align > max_alignment) {
                        max_alignment = field_align;
                    }

                    // 对齐当前偏移
                    current_offset = c_layout_align_up(current_offset, field_align);
                    field_offsets[i] = current_offset;
                    
                    // 计算字段大小（数组字段需要乘以维度）
                    int field_total_size = field_size;
                    if (array_dim > 0) {
                        field_total_size = field_size * array_dim;
                    }
                    current_offset += field_total_size;
                }

                // 结构体总大小需要按最大对齐要求对齐
                int total_size = c_layout_align_up(current_offset, max_alignment);

                // 保存布局信息到 AST
                ast->u.cstruct_def.field_offsets = field_offsets;
                ast->u.cstruct_def.total_size = total_size;
                ast->u.cstruct_def.alignment = max_alignment;

                // 4. 注册到符号表（或更新已预注册的符号）
                TypeInfo* cstruct_type = type_new(TYPE_CSTRUCT);
                cstruct_type->struct_name = strdup(ast->u.cstruct_def.name);

                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.cstruct_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = cstruct_type;
                } else {
                    // 未注册，创建新符号
                    sym = scope_define(s->current, ast->u.cstruct_def.name, kind);
                }

                if (sym) {
                    sym->type = cstruct_type;
                    // 保存符号引用信息
                    ast->u.cstruct_def.ref.kind = sym->kind;
                    ast->u.cstruct_def.ref.index = sym->index;
                    ast->u.cstruct_def.ref.name = strdup(sym->name);
                    ast->u.cstruct_def.ref.type_kind = sym->type ? sym->type->kind : TYPE_CSTRUCT;

                    // 存储 cstruct 字段信息
                    sym->struct_field_count = ast->u.cstruct_def.field_count;
                    sym->struct_field_names = (char**)malloc(sizeof(char*) * ast->u.cstruct_def.field_count);
                    sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.cstruct_def.field_count);
                    sym->struct_field_null_default = (int*)calloc(ast->u.cstruct_def.field_count, sizeof(int));

                    for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                        sym->struct_field_names[i] = strdup(ast->u.cstruct_def.field_names[i]);
                        sym->struct_field_types[i] = type_copy(ast->u.cstruct_def.field_types[i]);
                    }

                    // 存储 cstruct 布局信息
                    sym->cstruct_size = total_size;
                    sym->cstruct_alignment = max_alignment;
                    sym->cstruct_field_offsets = (int*)malloc(sizeof(int) * ast->u.cstruct_def.field_count);
                    for (int i = 0; i < ast->u.cstruct_def.field_count; i++) {
                        sym->cstruct_field_offsets[i] = field_offsets[i];
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（struct/enum）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.cstruct_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_STRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 struct，不能重复定义为 cstruct", ast->u.cstruct_def.name);
                        } else if (existing->type->kind == TYPE_ENUM) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 enum，不能重复定义为 cstruct", ast->u.cstruct_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "cstruct '%s' 重复定义", ast->u.cstruct_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "cstruct '%s' 重复定义", ast->u.cstruct_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_CLIB_DEF:
            // 注册 clib 类型并存储函数签名信息
            {
                // 1. 验证函数签名中的类型是否有效（clib 只允许 C 布局类型）
                for (int i = 0; i < ast->u.clib_def.func_count; i++) {
                    TypeInfo* ret_type = ast->u.clib_def.func_return_types[i];
                    // 允许 TYPE_NULL (void), TYPE_I32, TYPE_U32, TYPE_I64, TYPE_U64, TYPE_F32, TYPE_F64,
                    // TYPE_PTR, TYPE_PTR_GENERIC, TYPE_BOOL, TYPE_STR8, TYPE_STR16
                    // 不允许 TYPE_INT, TYPE_FLOAT, TYPE_STRING 等 Leno 类型
                    if (ret_type->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'int'，请使用 'i32' 或 'i64'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'float'，请使用 'f32' 或 'f64'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                 ast->u.clib_def.func_names[i]);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                        // 可能是自定义类型名（如 Ptr 被解析为 TYPE_STRUCT 带 struct_name="Ptr"）
                        if (strcmp(ret_type->struct_name, "Ptr") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_PTR);
                            ast->u.clib_def.func_return_types[i]->struct_name = strdup("Ptr");
                        } else if (strcmp(ret_type->struct_name, "string") == 0 ||
                                   strcmp(ret_type->struct_name, "String") == 0) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                     ast->u.clib_def.func_names[i]);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (strcmp(ret_type->struct_name, "str8") == 0 ||
                                   strcmp(ret_type->struct_name, "Str8") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_STR8);
                        } else if (strcmp(ret_type->struct_name, "str16") == 0 ||
                                   strcmp(ret_type->struct_name, "Str16") == 0) {
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_STR16);
                        } else if (cstruct_def_find(ret_type->struct_name)) {
                            // cstruct 类型名 — 修正为 TYPE_CSTRUCT
                            char* name = strdup(ret_type->struct_name);
                            type_free(ret_type);
                            ast->u.clib_def.func_return_types[i] = type_new(TYPE_CSTRUCT);
                            ast->u.clib_def.func_return_types[i]->struct_name = name;
                        } else {
                            // 尝试从符号表查找（cstruct 可能尚未注册到 cstruct_def_table）
                            Symbol* sym = scope_resolve(s->current, ret_type->struct_name);
                            if (!sym) sym = scope_resolve(s->root_scope, ret_type->struct_name);
                            if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                                char* name = strdup(ret_type->struct_name);
                                type_free(ret_type);
                                ast->u.clib_def.func_return_types[i] = type_new(TYPE_CSTRUCT);
                                ast->u.clib_def.func_return_types[i]->struct_name = name;
                            } else {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "clib 函数 '%s' 返回类型 '%s' 不是有效的 C 布局类型，请使用 i32/i64/f32/f64/str8/str16/ptr/bool/void",
                                         ast->u.clib_def.func_names[i], ret_type->struct_name);
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            }
                        }
                    }

                    // 验证参数类型
                    for (int j = 0; j < ast->u.clib_def.func_param_counts[i]; j++) {
                        TypeInfo* param_type = ast->u.clib_def.func_param_types[i][j];
                        if (param_type->kind == TYPE_INT) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'int'，请使用 'i32' 或 'i64'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_FLOAT) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'float'，请使用 'f32' 或 'f64'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_STRING) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                     ast->u.clib_def.func_names[i], j + 1);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        } else if (param_type->kind == TYPE_STRUCT && param_type->struct_name) {
                            if (strcmp(param_type->struct_name, "Ptr") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_PTR);
                                ast->u.clib_def.func_param_types[i][j]->struct_name = strdup("Ptr");
                            } else if (strcmp(param_type->struct_name, "string") == 0 ||
                                       strcmp(param_type->struct_name, "String") == 0) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'",
                                         ast->u.clib_def.func_names[i], j + 1);
                                error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                            } else if (strcmp(param_type->struct_name, "str8") == 0 ||
                                       strcmp(param_type->struct_name, "Str8") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_STR8);
                            } else if (strcmp(param_type->struct_name, "str16") == 0 ||
                                       strcmp(param_type->struct_name, "Str16") == 0) {
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_STR16);
                            } else if (cstruct_def_find(param_type->struct_name)) {
                                // cstruct 类型名 — 修正为 TYPE_CSTRUCT，语义为传指针
                                char* name = strdup(param_type->struct_name);
                                type_free(param_type);
                                ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_CSTRUCT);
                                ast->u.clib_def.func_param_types[i][j]->struct_name = name;
                            } else {
                                // 尝试从符号表查找（cstruct 可能尚未注册到 cstruct_def_table）
                                Symbol* sym = scope_resolve(s->current, param_type->struct_name);
                                if (!sym) sym = scope_resolve(s->root_scope, param_type->struct_name);
                                if (sym && sym->type && sym->type->kind == TYPE_CSTRUCT) {
                                    char* name = strdup(param_type->struct_name);
                                    type_free(param_type);
                                    ast->u.clib_def.func_param_types[i][j] = type_new(TYPE_CSTRUCT);
                                    ast->u.clib_def.func_param_types[i][j]->struct_name = name;
                                } else {
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg), "clib 函数 '%s' 参数 %d 类型 '%s' 不是有效的 C 布局类型，请使用 i32/i64/f32/f64/str8/str16/ptr/bool/void",
                                             ast->u.clib_def.func_names[i], j + 1, param_type->struct_name);
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                            }
                        }
                    }
                }

                // 2. 注册到符号表（更新已预注册的符号）
                TypeInfo* clib_type = type_new(TYPE_CLIB);
                clib_type->struct_name = strdup(ast->u.clib_def.name);

                // 先检查是否已经预注册
                Symbol* sym = scope_resolve_local(s->current, ast->u.clib_def.name);
                if (sym && sym->type && sym->type->kind == TYPE_CLIB) {
                    // 已预注册，更新现有符号
                    type_free(sym->type);
                    sym->type = clib_type;
                } else {
                    // 未注册，创建新符号（不应发生，因为预注册已处理）
                    sym = scope_define(s->current, ast->u.clib_def.name, SYM_CLIB);
                }

                if (sym) {
                    sym->type = clib_type;
                    ast->u.clib_def.ref.kind = sym->kind;
                    ast->u.clib_def.ref.index = sym->index;
                    ast->u.clib_def.ref.name = strdup(sym->name);
                    ast->u.clib_def.ref.type_kind = TYPE_CLIB;

                    // 存储 clib 函数签名信息
                    sym->clib_func_count = ast->u.clib_def.func_count;
                    sym->clib_func_names = (char**)malloc(sizeof(char*) * ast->u.clib_def.func_count);
                    sym->clib_func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.clib_def.func_count);
                    sym->clib_func_param_counts = (int*)malloc(sizeof(int) * ast->u.clib_def.func_count);
                    sym->clib_func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * ast->u.clib_def.func_count);

                    for (int i = 0; i < ast->u.clib_def.func_count; i++) {
                        sym->clib_func_names[i] = strdup(ast->u.clib_def.func_names[i]);
                        sym->clib_func_return_types[i] = type_copy(ast->u.clib_def.func_return_types[i]);
                        sym->clib_func_param_counts[i] = ast->u.clib_def.func_param_counts[i];
                        sym->clib_func_param_types[i] = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.clib_def.func_param_counts[i]);
                        for (int j = 0; j < ast->u.clib_def.func_param_counts[i]; j++) {
                            sym->clib_func_param_types[i][j] = type_copy(ast->u.clib_def.func_param_types[i][j]);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "clib '%s' 重复定义", ast->u.clib_def.name);
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_CFUNC_DECL:
            // 注册 cfunc 回调签名类型
            {
                // 1. 验证返回类型（cfunc 只允许 C 布局类型）
                TypeInfo* ret_type = ast->u.cfunc_decl.return_type;
                if (ret_type) {
                    if (ret_type->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'int'，请使用 'i32' 或 'i64'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'float'，请使用 'f32' 或 'f64'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型不能使用 'string'，请使用 'str8' 或 'ptr'", ast->u.cfunc_decl.name);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                        if (strcmp(ret_type->struct_name, "Ptr") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_PTR);
                            ast->u.cfunc_decl.return_type->struct_name = strdup("Ptr");
                        } else if (strcmp(ret_type->struct_name, "str8") == 0 || strcmp(ret_type->struct_name, "Str8") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_STR8);
                        } else if (strcmp(ret_type->struct_name, "str16") == 0 || strcmp(ret_type->struct_name, "Str16") == 0) {
                            type_free(ret_type);
                            ast->u.cfunc_decl.return_type = type_new(TYPE_STR16);
                        } else {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "cfunc '%s' 返回类型 '%s' 不是有效的 C 布局类型", ast->u.cfunc_decl.name, ret_type->struct_name);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }
                    }
                }

                // 2. 验证参数类型
                for (int j = 0; j < ast->u.cfunc_decl.param_count; j++) {
                    TypeInfo* ptype = ast->u.cfunc_decl.param_types[j];
                    if (ptype->kind == TYPE_INT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'int'，请使用 'i32' 或 'i64'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_FLOAT) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'float'，请使用 'f32' 或 'f64'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_STRING) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型不能使用 'string'，请使用 'str8' 或 'ptr'", ast->u.cfunc_decl.name, j + 1);
                        error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                    } else if (ptype->kind == TYPE_STRUCT && ptype->struct_name) {
                        if (strcmp(ptype->struct_name, "Ptr") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_PTR);
                            ast->u.cfunc_decl.param_types[j]->struct_name = strdup("Ptr");
                        } else if (strcmp(ptype->struct_name, "str8") == 0 || strcmp(ptype->struct_name, "Str8") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_STR8);
                        } else if (strcmp(ptype->struct_name, "str16") == 0 || strcmp(ptype->struct_name, "Str16") == 0) {
                            type_free(ptype);
                            ast->u.cfunc_decl.param_types[j] = type_new(TYPE_STR16);
                        } else {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "cfunc '%s' 参数 %d 类型 '%s' 不是有效的 C 布局类型", ast->u.cfunc_decl.name, j + 1, ptype->struct_name);
                            error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                        }
                    }
                }

                // 3. 注册到符号表
                Symbol* sym = scope_resolve_local(s->current, ast->u.cfunc_decl.name);
                if (sym && sym->type && sym->type->kind == TYPE_CFUNC) {
                    // 已预注册，更新签名信息
                    type_free(sym->type);
                    TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                    cfunc_type->struct_name = strdup(ast->u.cfunc_decl.name);
                    sym->type = cfunc_type;
                } else {
                    sym = scope_define(s->current, ast->u.cfunc_decl.name, SYM_CFUNC);
                    if (sym) {
                        TypeInfo* cfunc_type = type_new(TYPE_CFUNC);
                        cfunc_type->struct_name = strdup(ast->u.cfunc_decl.name);
                        sym->type = cfunc_type;
                    }
                }

                if (sym) {
                    ast->u.cfunc_decl.ref.kind = sym->kind;
                    ast->u.cfunc_decl.ref.index = sym->index;
                    free(ast->u.cfunc_decl.ref.name);
                    ast->u.cfunc_decl.ref.name = strdup(sym->name);
                    ast->u.cfunc_decl.ref.type_kind = TYPE_CFUNC;

                    // 存储 cfunc 签名信息
                    sym->cfunc_param_count = ast->u.cfunc_decl.param_count;
                    sym->cfunc_param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * ast->u.cfunc_decl.param_count);
                    for (int j = 0; j < ast->u.cfunc_decl.param_count; j++) {
                        sym->cfunc_param_types[j] = type_copy(ast->u.cfunc_decl.param_types[j]);
                    }
                    sym->cfunc_return_type = ast->u.cfunc_decl.return_type ? type_copy(ast->u.cfunc_decl.return_type) : NULL;
                } else {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "cfunc '%s' 重复定义", ast->u.cfunc_decl.name);
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_ENUM_DEF:
            // 注册 enum 类型
            {
                TypeInfo* enum_type = type_new(TYPE_ENUM);
                enum_type->struct_name = strdup(ast->u.enum_def.name);

                // 注册到符号表
                SymKind kind;
                if (s->current && s->current->parent == NULL) {
                    kind = s->is_module ? SYM_MODULE : SYM_GLOBAL;
                } else {
                    kind = SYM_LOCAL;
                }
                Symbol* sym = scope_define(s->current, ast->u.enum_def.name, kind);
                if (sym) {
                    sym->type = enum_type;
                    // 保存符号引用信息
                    ast->u.enum_def.ref.kind = sym->kind;
                    ast->u.enum_def.ref.index = sym->index;
                    ast->u.enum_def.ref.name = strdup(sym->name);
                    ast->u.enum_def.ref.type_kind = sym->type ? sym->type->kind : TYPE_ANY;
                    
                    // 注册 enum 成员到作用域
                    for (int i = 0; i < ast->u.enum_def.member_count; i++) {
                        char* member_name = ast->u.enum_def.member_names[i];
                        Symbol* member_sym = scope_define(s->current, member_name, kind);
                        if (member_sym) {
                            member_sym->type = type_new(TYPE_ENUM);
                            member_sym->type->struct_name = strdup(ast->u.enum_def.name);
                        }
                    }
                } else {
                    char msg[BUFFER_MEDIUM];
                    // 检查是否已经被定义为其他类型（struct/cstruct）
                    Symbol* existing = scope_resolve_local(s->current, ast->u.enum_def.name);
                    if (existing && existing->type) {
                        if (existing->type->kind == TYPE_STRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 struct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else if (existing->type->kind == TYPE_CSTRUCT) {
                            snprintf(msg, sizeof(msg), "类型 '%s' 已经定义为 cstruct，不能重复定义为 enum", ast->u.enum_def.name);
                        } else {
                            snprintf(msg, sizeof(msg), "enum '%s' 重复定义", ast->u.enum_def.name);
                        }
                    } else {
                        snprintf(msg, sizeof(msg), "enum '%s' 重复定义", ast->u.enum_def.name);
                    }
                    error_add(ERR_DUPLICATE_VAR, ast->line, msg);
                }
            }
            break;

        case AST_ALIAS:
            // 类型别名已在解析阶段注册到解析器别名表，无需运行时处理
            break;

        case AST_STRUCT_INIT:
            // 处理 struct 构造函数调用
            {
                // 检查是否是模块限定的 struct 名称（如 "math.Point"）
                const char* dot_pos = strchr(ast->u.struct_init.struct_name, '.');
                Symbol* sym = NULL;

                if (dot_pos) {
                    // 模块限定的 struct：从导入的模块中查找
                    int mod_name_len = dot_pos - ast->u.struct_init.struct_name;
                    char* module_name = (char*)malloc(mod_name_len + 1);
                    memcpy(module_name, ast->u.struct_init.struct_name, mod_name_len);
                    module_name[mod_name_len] = '\0';
                    const char* struct_name_part = dot_pos + 1;
                    ImportedModuleInfo* module_info = find_imported_module(s, module_name);
                    if (module_info && module_info->sym_table) {
                        ModuleStructSymbol* mod_struct = module_symbol_table_find_struct(module_info->sym_table, struct_name_part);
                        if (mod_struct) {
                            // 从模块符号表创建虚拟符号用于字段检查
                            sym = (Symbol*)calloc(1, sizeof(Symbol));
                            sym->name = strdup(struct_name_part);
                            sym->type = type_new(TYPE_STRUCT);
                            sym->type->struct_name = strdup(struct_name_part);
                            sym->struct_field_count = mod_struct->field_count;
                            if (mod_struct->field_count > 0) {
                                sym->struct_field_names = (char**)malloc(sizeof(char*) * mod_struct->field_count);
                                sym->struct_field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * mod_struct->field_count);
                                for (int fi = 0; fi < mod_struct->field_count; fi++) {
                                    sym->struct_field_names[fi] = strdup(mod_struct->fields[fi].name);
                                    sym->struct_field_types[fi] = type_new(mod_struct->fields[fi].type);
                                }
                            }
                        }
                    }
                    free(module_name);
                } else {
                    // 直接的 struct 名称
                    sym = scope_resolve(s->current, ast->u.struct_init.struct_name);
                }

                if (!sym) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "未定义的 struct 类型 '%s'", ast->u.struct_init.struct_name);
                    error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                } else if (sym->type->kind == TYPE_FACE) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "face '%s' 不能实例化", ast->u.struct_init.struct_name);
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                } else if (sym->type->kind != TYPE_STRUCT) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "'%s' 不是 struct 类型", ast->u.struct_init.struct_name);
                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                } else {
                    // 检查字段类型
                    for (int i = 0; i < ast->u.struct_init.field_count; i++) {
                        char* field_name = ast->u.struct_init.field_names[i];
                        Ast* field_value = ast->u.struct_init.field_values[i];
                        
                        // 查找字段定义
                        TypeInfo* expected_type = NULL;
                        for (int j = 0; j < sym->struct_field_count; j++) {
                            if (strcmp(sym->struct_field_names[j], field_name) == 0) {
                                expected_type = sym->struct_field_types[j];
                                break;
                            }
                        }
                        
                        if (!expected_type) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "struct '%s' 没有字段 '%s'", 
                                     ast->u.struct_init.struct_name, field_name);
                            error_add(ERR_UNDEFINED_VAR, ast->line, msg);
                        } else if (field_value) {
                            // 推断参数字段的类型
                            TypeInfo* actual_type = infer_expr_type(s, field_value);
                            
                            // 泛型 struct：用具体类型参数替换字段类型中的泛型参数
                            TypeInfo* check_type = expected_type;
                            if (ast->u.struct_init.generic_type_count > 0) {
                                // 查找 struct 定义获取 type_params
                                Ast* struct_def_ast = NULL;
                                // 遍历 root AST 查找
                                if (s->root) {
                                    for (int si = 0; si < s->root->u.block.count; si++) {
                                        Ast* stmt = s->root->u.block.items[si];
                                        if (stmt->kind == AST_STRUCT_DEF && 
                                            stmt->u.struct_def.name &&
                                            strcmp(stmt->u.struct_def.name, ast->u.struct_init.struct_name) == 0) {
                                            struct_def_ast = stmt;
                                            break;
                                        }
                                    }
                                }
                                if (struct_def_ast && struct_def_ast->u.struct_def.type_param_count > 0) {
                                    // 用具体类型替换泛型参数
                                    check_type = type_copy(expected_type);
                                    for (int gi = 0; gi < struct_def_ast->u.struct_def.type_param_count && gi < ast->u.struct_init.generic_type_count; gi++) {
                                        TypeInfo* sub = type_substitute(check_type, 
                                            struct_def_ast->u.struct_def.type_params[gi],
                                            ast->u.struct_init.generic_type_args[gi]);
                                        type_free(check_type);
                                        check_type = sub;
                                    }
                                }
                            }
                            
                            // 检查类型兼容性
                            if (actual_type) {
                                if (!type_is_compatible(check_type, actual_type)) {
                                    char msg[BUFFER_MEDIUM];
                                    format_type_error(msg, sizeof(msg),
                                        "字段 '%s3' 类型不匹配: 期望 '%s1'，实际 '%s2'",
                                        check_type, actual_type,
                                        field_name, NULL);
                                    error_add(ERR_TYPE_MISMATCH, ast->line, msg);
                                }
                                type_free(actual_type);
                            }
                            // 释放替换后的类型（如果做了替换）
                            if (check_type != expected_type) {
                                type_free(check_type);
                            }
                        }
                        
                        // 递归访问字段值
                        visit(s, field_value);
                    }
                }
            }
            break;

        case AST_AWAIT:
            // 检查 await 只能在 async 函数中使用
            if (!s->current_func || !s->current_func->u.func.is_async) {
                error_add(ERR_SEMANTIC, ast->line, "await 只能在 async 函数中使用");
            }
            // 递归访问被等待的表达式
            if (ast->u.await.expr) {
                visit(s, ast->u.await.expr);
            }
            break;

        case AST_FIELD_ACCESS:
            ast->u.field_access.field_index = -1;
            if (ast->u.field_access.obj) {
                visit(s, ast->u.field_access.obj);

                // 检查 clib 类型的函数调用
                TypeInfo* obj_type = infer_expr_type(s, ast->u.field_access.obj);
                if (obj_type && obj_type->kind == TYPE_CLIB && obj_type->struct_name) {
                    // 检查 clib 函数是否存在
                    Symbol* clib_sym = scope_resolve(s->current, obj_type->struct_name);
                    if (clib_sym && clib_sym->clib_func_count > 0) {
                        const char* func_name = ast->u.field_access.field_name;
                        int func_found = 0;
                        for (int i = 0; i < clib_sym->clib_func_count; i++) {
                            if (strcmp(clib_sym->clib_func_names[i], func_name) == 0) {
                                func_found = 1;
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }
                        if (!func_found) {
                            char msg[BUFFER_MEDIUM];
                            snprintf(msg, sizeof(msg), "clib '%s' 没有声明函数 '%s'",
                                     obj_type->struct_name, func_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }
                    }
                    type_free(obj_type);
                    break;
                }

                // 检查 struct/cstruct 字段是否存在，并设置字段索引
                if (obj_type && (obj_type->kind == TYPE_STRUCT || obj_type->kind == TYPE_CSTRUCT || obj_type->kind == TYPE_FACE)) {
                    const char* field_name = ast->u.field_access.field_name;
                    Symbol* struct_sym = NULL;
                    const char* var_name = NULL;

                    // 获取变量名（支持嵌套访问）
                    if (ast->u.field_access.obj->kind == AST_VAR) {
                        var_name = ast->u.field_access.obj->u.var.name;
                        struct_sym = scope_resolve(s->current, var_name);
                        if (struct_sym && struct_sym->struct_field_count == 0 && obj_type->struct_name) {
                            struct_sym = scope_resolve(s->current, obj_type->struct_name);
                        }
                    }

                    // 如果找不到符号，尝试通过类型名称查找 struct/cstruct 定义
                    if (!struct_sym && obj_type->struct_name) {
                    struct_sym = scope_resolve(s->current, obj_type->struct_name);
                }

                    // 检查是否是 self["field_name"] 形式（在 struct 方法中）
                    if (!struct_sym && ast->u.field_access.obj->kind == AST_INDEX) {
                        Ast* index_obj = ast->u.field_access.obj->u.index.obj;
                        Ast* index_index = ast->u.field_access.obj->u.index.index;
                        if (index_obj && index_obj->kind == AST_VAR &&
                            strcmp(index_obj->u.var.name, "self") == 0 &&
                            index_index && index_index->kind == AST_STRING) {
                            // 这是 self["field_name"] 形式，查找 self 的类型
                            Symbol* self_sym = scope_resolve(s->current, "self");
                            if (self_sym && self_sym->type && self_sym->type->struct_name) {
                                struct_sym = scope_resolve(s->current, self_sym->type->struct_name);
                            }
                        }
                    }

                    if (struct_sym && struct_sym->struct_field_count > 0) {
                        int field_found = 0;
                        for (int i = 0; i < struct_sym->struct_field_count; i++) {
                            if (strcmp(struct_sym->struct_field_names[i], field_name) == 0) {
                                field_found = 1;
                                ast->u.field_access.field_index = i;
                                break;
                            }
                        }

                        if (!field_found) {
                            char msg[BUFFER_MEDIUM];
                            const char* type_kind_name = (obj_type->kind == TYPE_CSTRUCT) ? "cstruct" : "struct";
                            snprintf(msg, sizeof(msg), "%s 没有字段 '%s'", type_kind_name, field_name);
                            error_add(ERR_SEMANTIC, ast->line, msg);
                        }

                        // 注意：单层字段访问（如 o.inner）不需要检查 null 默认值
                        // 因为获取 null 本身不会运行时出错
                        // 链式访问（如 o.inner.value）的 null 检查在 AST_INDEX 中处理
                    }
                }
                if (obj_type) type_free(obj_type);
            }
            break;

        default:
            break;
    }
}

void visit_list(Semantic* s, AstList* list) {
    for (int i = 0; i < list->count; i++) {
        visit(s, list->items[i]);
    }
}
