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
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
                    }
                    type_free(return_type_info);
                }
            }
            break;
        }
            
        case AST_RETURN_MULTI: {
            // 多值返回: return a, b, c
            int count = ast->u.ret_multi.count;
            
            // 访问每个返回值表达式
            for (int i = 0; i < count; i++) {
                visit(s, ast->u.ret_multi.exprs[i]);
            }
            
            if (s->current_func) {
                TypeInfo* expected_type = s->current_func->u.func.return_type;
                
                if (expected_type && expected_type->kind == TYPE_MULTI_RET) {
                    // 函数声明了多返回值类型，检查数量和类型
                    if (count != expected_type->param_count) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg),
                            "多返回值数量不匹配：期望 %d 个，实际 %d 个",
                            expected_type->param_count, count);
                        error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
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
                                    char msg[BUFFER_MEDIUM];
                                    snprintf(msg, sizeof(msg),
                                        "返回值 %d 类型不匹配：期望 %s，实际 %s",
                                        i + 1, expected_buf, actual_buf);
                                    error_add_at(ERR_TYPE_MISMATCH, ast->u.ret_multi.exprs[i]->line, ast->u.ret_multi.exprs[i]->column, msg);
                                }
                                type_free(actual);
                            }
                        }
                    }
                } else if (expected_type && expected_type->kind != TYPE_INFER) {
                    // 函数声明了单返回值类型但 return 了多个值
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg),
                        "函数声明返回单个值，但 return 语句返回了 %d 个值", count);
                    error_add_at(ERR_TYPE_MISMATCH, ast->line, ast->column, msg);
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