// 获取变量类型（用于实例方法补全）
// 返回 "number", "string", "array" 等，如果无法确定返回 NULL
// 使用编译器分析结果，而不是自己解析
static char* get_variable_type(const char* content, const char* var_name, const char* file_path) {
    if (!content || !var_name) return NULL;

    // 首先检查 var_name 本身是否是字面量
    if (isdigit((unsigned char)var_name[0]) || var_name[0] == '-' || var_name[0] == '.') {
        return strdup("number");
    } else if (var_name[0] == '"' || var_name[0] == '\'') {
        return strdup("string");
    } else if (var_name[0] == '[') {
        return strdup("array");
    } else if (var_name[0] == '{') {
        return strdup("dict");
    }

    // 使用编译器分析获取变量类型
    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 即使分析有错误，也尝试获取符号信息
    bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

    // 如果解析失败，尝试去掉包含变量引用的那一行（可能是不完整的代码）再解析
    if (!analyze_ok || !ctx.root_scope) {
        // 复制内容
        char* content_copy = strdup(content);
        if (content_copy) {
            // 查找变量名后面跟着 . 的位置（即实例方法调用）
            char* search_pattern = malloc(strlen(var_name) + 2);
            if (search_pattern) {
                sprintf(search_pattern, "%s.", var_name);
                bool found = true;
                int retry_count = 0;
                const int max_retries = 5;  // 最多重试5次
                
                while (found && retry_count < max_retries) {
                    char* var_pos = strstr(content_copy, search_pattern);
                    if (!var_pos) {
                        found = false;
                        break;
                    }
                    
                    // 找到该行的开始（前一个换行符）
                    char* line_start = content_copy;
                    char* p = content_copy;
                    while (p < var_pos) {
                        if (*p == '\n') line_start = p + 1;
                        p++;
                    }
                    // 找到该行的结束（后一个换行符）
                    char* line_end = strchr(var_pos, '\n');
                    if (line_end) {
                        // 去掉这一行：将 line_end 之后的内容移到 line_start
                        memmove(line_start, line_end + 1, strlen(line_end + 1) + 1);

                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        
                        if (analyze_ok || ctx.root_scope) {
                            // 解析成功，跳出循环
                            break;
                        }
                        retry_count++;
                    } else {
                        // 没有找到换行符，可能是最后一行
                        *line_start = '\0';
                        
                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        break;
                    }
                }
                free(search_pattern);
            }
            free(content_copy);
        }
    }

    char* type_str = NULL;
    if (ctx.root_scope && compiler_get_symbol_info(&ctx, var_name, &type_str, NULL)) {
        // 将编译器的类型名转换为实例方法类型名
        char* result = NULL;
        // 先检查 struct，因为 type_str 可能是 "struct Point" 包含 "int"
        if (strstr(type_str, "struct")) {
            result = strdup(type_str);
        } else if (strstr(type_str, "face")) {
            result = strdup(type_str);
        } else if (strcmp(type_str, "int") == 0 || strcmp(type_str, "float") == 0 || strcmp(type_str, "bigint") == 0) {
            result = strdup("number");
        } else if (strcmp(type_str, "string") == 0) {
            result = strdup("string");
        } else if (strncmp(type_str, "Array", 5) == 0) {
            result = strdup("array");
        } else if (strncmp(type_str, "Dict", 4) == 0) {
            result = strdup("dict");
        } else if (strcmp(type_str, "bool") == 0) {
            result = strdup("bool");
        } else if (strcmp(type_str, "null") == 0) {
            result = strdup("null");
        } else if (strncmp(type_str, "Ptr", 3) == 0) {
            result = strdup("ptr");
        } else if (strcmp(type_str, "File") == 0) {
            result = strdup("file");
        } else if (strcmp(type_str, "thread") == 0) {
            result = strdup("thread");
        } else if (strcmp(type_str, "channel") == 0) {
            result = strdup("channel");
        } else {
            // 尝试将类型名转为小写作为实例方法查找键
            char lower[64];
            int len = strlen(type_str);
            if (len < 64) {
                for (int i = 0; i <= len; i++) lower[i] = tolower((unsigned char)type_str[i]);
                int test_count = 0;
                char** test_methods = native_get_instance_methods(lower, &test_count);
                if (test_methods && test_count > 0) {
                    native_free_instance_method_list(test_methods, test_count);
                    result = strdup(lower);
                }
            }
        }
        free(type_str);
        compiler_context_cleanup(&ctx);

        // 验证该类型是否有实例方法（只对非struct类型）
        if (result && strstr(result, "struct") == NULL && strstr(result, "face") == NULL) {
            int test_count = 0;
            char** test_methods = native_get_instance_methods(result, &test_count);
            if (test_methods && test_count > 0) {
                native_free_instance_method_list(test_methods, test_count);
            } else {
                // 该类型没有实例方法，返回NULL
                free(result);
                result = NULL;
            }
        }
        return result;
    }

    compiler_context_cleanup(&ctx);
    return NULL;
}

// 从编译器符号表获取补全
static void add_symbols_from_compiler(const char* content, LspCompletionItem** items,
                                       int* count, int* capacity, const char* prefix, const char* file_path) {
    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 编译分析获取符号表
    if (!compiler_analyze_with_filename(&ctx, content, file_path) && !ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return;
    }
    
    // 获取所有符号
    char** names = NULL;
    char** types = NULL;
    int sym_count = compiler_get_all_symbols(&ctx, &names, &types);
    
    for (int i = 0; i < sym_count; i++) {
        if (!names[i]) continue;
        
        // 确定符号类型（使用更精确的分类）
        int kind = LSP_COMP_VARIABLE;
        if (types && types[i]) {
            if (strstr(types[i], "func") != NULL) {
                kind = LSP_COMP_FUNCTION;
            } else if (strstr(types[i], "struct") != NULL || strstr(types[i], "cstruct") != NULL) {
                kind = LSP_COMP_STRUCT;
            } else if (strstr(types[i], "face") != NULL) {
                kind = LSP_COMP_INTERFACE;
            } else if (strstr(types[i], "enum") != NULL) {
                kind = LSP_COMP_ENUM;
            } else if (strstr(types[i], "alias") != NULL) {
                kind = LSP_COMP_CLASS;  // alias 显示为类型
            }
        }
        
        // 构建详细信息
        char detail[256];
        if (types && types[i]) {
            const char* category = "variable";
            if (kind == LSP_COMP_FUNCTION) category = "function";
            else if (kind == LSP_COMP_STRUCT) category = "struct";
            else if (kind == LSP_COMP_INTERFACE) category = "face";
            else if (kind == LSP_COMP_ENUM) category = "enum";
            snprintf(detail, sizeof(detail), "%s: %s", category, types[i]);
        } else {
            const char* category = "variable";
            if (kind == LSP_COMP_FUNCTION) category = "function";
            else if (kind == LSP_COMP_STRUCT) category = "struct";
            else if (kind == LSP_COMP_INTERFACE) category = "face";
            else if (kind == LSP_COMP_ENUM) category = "enum";
            snprintf(detail, sizeof(detail), "%s", category);
        }
        
        add_completion_item(items, count, capacity, names[i], kind, detail, prefix);
    }
    
    // 清理
    compiler_free_symbol_list(names, types, sym_count);
    compiler_context_cleanup(&ctx);
}
