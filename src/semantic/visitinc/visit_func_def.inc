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