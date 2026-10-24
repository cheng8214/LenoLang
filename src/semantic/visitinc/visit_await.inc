        case AST_AWAIT:
            // 检查 await 只能在 async 函数中使用
            //   ★ 2026-10-24（round5 的 N6）：只报「只能在 async 函数中使用」时，在 main 里
            //     写 await 的人不知道**该改成什么**（round2 的用例就是这么写的，行为变严后
            //     整批用例报错却看不出改法）⇒ 追加可操作的写法指引 ✓
            if (!s->current_func || !s->current_func->u.func.is_async) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                    "await 只能在 async 函数中使用 —— main 里请先拿到协程 `var f = 任务()`，"
                    "再用 `asyncs.run()` 驱动事件循环（限时用 `asyncs.run_for(毫秒)`），"
                    "需要等结果就把这段逻辑搬进 `async func 名字()` 里");
            }
            // 递归访问被等待的表达式
            if (ast->u.await.expr) {
                visit(s, ast->u.await.expr);
            }
            break;