        case AST_AWAIT:
            // 检查 await 只能在 async 函数中使用
            if (!s->current_func || !s->current_func->u.func.is_async) {
                error_add_at(ERR_SEMANTIC, ast->line, ast->column, "await 只能在 async 函数中使用");
            }
            // 递归访问被等待的表达式
            if (ast->u.await.expr) {
                visit(s, ast->u.await.expr);
            }
            break;