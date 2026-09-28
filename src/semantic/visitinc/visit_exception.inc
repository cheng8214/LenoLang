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
                // ★ 空 catch 提示（WARN_EMPTY_CATCH）：`catch { }` 会把异常**静默吞掉** ——
                //   其中就包括 `x as T` **收窄失败**这类运行期类型错误 ⇒ 表现为"功能没反应"，极难排查。
                //   真实案例：`ti.now() as Array[int]`（ti.now 返回 int 时间戳而不是数组）抛异常被
                //   `catch { }` 吞掉 ⇒ 应用里「上次清理记录」与「导出报告」**静默失效很久** ✗，
                //   直到排查"导出为什么没文件"才挖出来。
                //   有意忽略时写 `catch e { return }`（体非空 ⇒ 不再提示 ✓）。
                if (ast->u.try_.catch_body->kind == AST_BLOCK &&
                    ast->u.try_.catch_body->u.block.count == 0) {
                    // ⚠ 2026-09-28 起由**警告升为错误**（用户要求 ✓）：空 catch 一律不允许 ✗，
                    //   体里放一个真语句即可 —— `catch e { return }` / `catch e { continue }` /
                    //   `catch e { _dlog(...) }` ✓（只写注释不算 ⇒ 注释不是 AST 节点 ✗）
                    error_add_at(ERR_SEMANTIC, ast->line, ast->column,
                        "[空catch吞异常] 空 catch 会静默吞掉异常（含 as 收窄失败等运行期错误 ⇒ "
                        "功能会「没反应」）；请至少记一行日志，确定要忽略就写 catch e { return }");
                }
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