        case AST_RETURN: {
            visit(s, ast->u.ret);

            if (s->current_func) {
                TypeInfo* return_type_info = infer_expr_type(s, ast->u.ret);
                if (return_type_info) {
                    TypeInfo* expected_type = s->current_func->u.func.return_type;

                    // main 未声明返回类型时，返回值作为进程退出码，按 int 检查
                    // （声明为 int 时走下方通用检查；声明为其他类型已在 visit_func_impl 报错）
                    int main_exit_check = s->in_main_func &&
                        (!expected_type || expected_type->kind == TYPE_INFER);

                    // TYPE_INFER 表示函数声明时未指定返回类型，保持为 any（不推断）
                    if (main_exit_check) {
                        if (return_type_info->kind != TYPE_ANY &&
                            return_type_info->kind != TYPE_INFER &&
                            return_type_info->kind != TYPE_INT) {
                            error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                "main 函数的返回值将作为进程退出码，只能返回 int，实际 %s",
                                type_kind_to_string(return_type_info->kind));
                        }
                    } else if (!expected_type || expected_type->kind == TYPE_INFER) {
                        // 省略返回类型的函数返回 any，不进行推断
                        // 保持 TYPE_INFER（在类型系统中等同于 any）
                    } else if (!type_is_compatible(expected_type, return_type_info)) {
                        // 返回类型不匹配
                        const char* expected_str = type_to_string(expected_type);
                        char expected_buf[64];
                        strncpy(expected_buf, expected_str, 63);
                        expected_buf[63] = '\0';
                        const char* actual_str = type_to_string(return_type_info);
                        // ★ 2026-10-24（round6 复核 N3）：**函数返回函数**时最常见的原因是
                        //   "签名没写全"—— `func(int)`（无返回类型）与 `func(int):int` 是两个
                        //   不同类型，而报错只摆两个长得几乎一样的字符串，看不出差在哪 ✗
                        //   ⇒ 两侧都是函数类型时补一句口径说明（不改变判据，只加提示 ✓）
                        char fn_hint[256];
                        fn_hint[0] = '\0';
                        if (expected_type->kind == TYPE_FUNCTION &&
                            return_type_info->kind == TYPE_FUNCTION) {
                            snprintf(fn_hint, sizeof(fn_hint),
                                "（函数类型按**完整签名**比较：`func(int)` 与 `func(int):int` 是两个不同类型 —— "
                                "返回闭包时括号右边那截返回类型也要写全）");
                        }
                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                                 "返回类型不匹配：期望 %s，实际 %s%s",
                                 expected_buf, actual_str, fn_hint);
                    }
                    type_free(return_type_info);
                }
            }
            break;
        }
            
        case AST_RETURN_MULTI: {
            // 多值返回: return a, b, c
            int count = ast->u.ret_multi.count;

            // main 的返回值作为进程退出码，不支持多值返回
            if (s->in_main_func) {
                error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                    "main 函数的返回值将作为进程退出码，不支持多值返回");
            }
            
            // 访问每个返回值表达式
            for (int i = 0; i < count; i++) {
                visit(s, ast->u.ret_multi.exprs[i]);
            }
            
            if (s->current_func) {
                TypeInfo* expected_type = s->current_func->u.func.return_type;
                
                if (expected_type && expected_type->kind == TYPE_MULTI_RET) {
                    // 函数声明了多返回值类型，检查数量和类型
                    if (count != expected_type->param_count) {
                        error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                            "多返回值数量不匹配：期望 %d 个，实际 %d 个",
                            expected_type->param_count, count);
                    } else {
                        // 逐个检查类型兼容性
                        for (int i = 0; i < count; i++) {
                            TypeInfo* actual = infer_expr_type(s, ast->u.ret_multi.exprs[i]);
                            if (actual) {
                                if (!type_is_compatible(expected_type->param_types[i], actual)) {
                                    // type_to_string 使用线程本地单缓冲区，
                                    // 连续调用会覆盖，必须先拷贝到独立缓冲区
                                    char expected_buf[64];
                                    char actual_buf[64];
                                    strncpy(expected_buf, type_to_string(expected_type->param_types[i]), 63);
                                    expected_buf[63] = '\0';
                                    strncpy(actual_buf, type_to_string(actual), 63);
                                    actual_buf[63] = '\0';
                                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->u.ret_multi.exprs[i]->line, ast->u.ret_multi.exprs[i]->column,
                                        "返回值 %d 类型不匹配：期望 %s，实际 %s",
                                        i + 1, expected_buf, actual_buf);
                                }
                                type_free(actual);
                            }
                        }
                    }
                } else if (expected_type && expected_type->kind != TYPE_INFER) {
                    // 函数声明了单返回值类型但 return 了多个值
                    error_add_at_fmt(ERR_TYPE_MISMATCH, ast->line, ast->column,
                        "函数声明返回单个值，但 return 语句返回了 %d 个值", count);
                }
                // TYPE_INFER（无注解）允许多返回值，不报错
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

        case AST_BREAK:
        case AST_CONTINUE: {
            // B3：循环外使用 break/continue——此前静默通过（编译不报错、运行期什么都不做）
            // 沿作用域链向上找循环作用域；先碰到函数边界（闭包体）即视为不在循环内
            int in_loop = 0;
            for (Scope* sc = s->current; sc; sc = sc->parent) {
                if (sc->is_loop) { in_loop = 1; break; }
                if (sc->is_func) break;
            }
            if (!in_loop) {
                error_add_at_fmt(ERR_SEMANTIC, ast->line, ast->column,
                         "'%s' 不能用在循环外（只能出现在 for/while 循环体内）",
                         ast->kind == AST_BREAK ? "break" : "continue");
            }
            break;
        }