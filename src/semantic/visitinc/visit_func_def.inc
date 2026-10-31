        case AST_FUNC_DEF:
            // 注解（2026-10-31）：登记本函数的 @optkeys，供后面的调用点检查 ✓
            //   （必须在 visit_func 之前：函数体的调用也要查得到自己的注解 ✓）
            annos_collect_func(ast);
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