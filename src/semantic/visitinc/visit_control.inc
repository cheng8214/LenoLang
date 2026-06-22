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