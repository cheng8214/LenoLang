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