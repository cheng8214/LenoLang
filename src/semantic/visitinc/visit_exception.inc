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
                        // ★ catch 变量的静态类型是 Dict[string, string]，**不能是 string**：
                        //   运行时它绑定的是异常对象 —— 字符串异常会被 OP_THROW 包成
                        //   Dict{msg,line,file,stack}（见 wrap_string_exception）。
                        //   标成 string 会让 `var x = null; try{...}catch e { x = e }`
                        //   把 x **锁成 string**（见 visit_var.inc「第一次赋非 null 值时
                        //   锁定类型」）⇒ 赋值处插入 OP_CAST_STRING，把异常 Dict 变成
                        //   字符串 "[object]" ✗（实测 test_try_catch_edge：
                        //   `e_info.msg.find("除零")` 报「字符串索引必须是数字」）。
                        //   用 Dict[string, string] 后成员访问的推断结果与原来的 string
                        //   口径完全一致（`.msg/.file/.stack` 都是 string），而赋值侧
                        //   目标类型是 Dict ⇒ 不会插入错误的字符串 CAST ✓。
                        sym->type = type_dict(type_new(TYPE_STRING), type_new(TYPE_STRING));
                        // 保存 catch 变量的引用信息
                        ast->u.try_.catch_var_ref.kind = sym->kind;
                        ast->u.try_.catch_var_ref.index = sym->index;
                        ast->u.try_.catch_var_ref.name = strdup(sym->name);
                        ast->u.try_.catch_var_ref.type_kind = TYPE_DICT;
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