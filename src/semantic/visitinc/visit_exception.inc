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

        case AST_DEFER:
            // defer 表达式在当前作用域中求值
            if (ast->u.defer_.expr) {
                visit(s, ast->u.defer_.expr);
            }
            break;