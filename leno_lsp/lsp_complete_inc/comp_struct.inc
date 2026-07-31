static char* find_enclosing_struct_name(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return NULL;

    int brace_depth = 0;
    int pos = cursor_offset - 1;

    while (pos >= 0) {
        char c = content[pos];

        if (c == '}') {
            brace_depth++;
            pos--;
            continue;
        }

        if (c == '{') {
            brace_depth--;
            if (brace_depth < 0) {
                int check_pos = pos - 1;
                while (check_pos >= 0 && isspace((unsigned char)content[check_pos])) check_pos--;

                int name_end = check_pos + 1;
                int name_start = name_end;
                while (name_start > 0 && (isalnum((unsigned char)content[name_start - 1]) || content[name_start - 1] == '_')) {
                    name_start--;
                }
                int name_len = name_end - name_start;

                if (name_len == 0) {
                    brace_depth = 0;
                    pos--;
                    continue;
                }

                int kw_end = name_start - 1;
                while (kw_end >= 0 && isspace((unsigned char)content[kw_end])) kw_end--;

                const char* struct_kw = "struct";
                const char* cstruct_kw = "cstruct";
                int struct_kw_len = 6;
                int cstruct_kw_len = 7;

                bool is_struct = false;
                bool is_cstruct = false;

                if (kw_end >= cstruct_kw_len - 1) {
                    int kw_start = kw_end - cstruct_kw_len + 1;
                    if (strncmp(content + kw_start, cstruct_kw, cstruct_kw_len) == 0 &&
                        (kw_start == 0 || (!isalnum((unsigned char)content[kw_start - 1]) && content[kw_start - 1] != '_'))) {
                        is_cstruct = true;
                    }
                }

                if (!is_cstruct && kw_end >= struct_kw_len - 1) {
                    int kw_start = kw_end - struct_kw_len + 1;
                    if (strncmp(content + kw_start, struct_kw, struct_kw_len) == 0 &&
                        (kw_start == 0 || (!isalnum((unsigned char)content[kw_start - 1]) && content[kw_start - 1] != '_'))) {
                        is_struct = true;
                    }
                }

                if (is_struct || is_cstruct) {
                    char* result = (char*)malloc(name_len + 1);
                    if (result) {
                        strncpy(result, content + name_start, name_len);
                        result[name_len] = '\0';
                    }
                    return result;
                }

                brace_depth = 0;
            }
            pos--;
            continue;
        }

        pos--;
    }

    return NULL;
}

// 从编译器符号表中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
// ctx: 编译器上下文
static void add_symbol_table_struct_completions(const char* struct_name, const char* var_name,
                                                CompilerContext* ctx, LspCompletionItem** items, int* count, int* capacity) {
    if (!struct_name || !ctx || !ctx->root_scope) {
        return;
    }

    // 在符号表中查找 struct 定义
    Symbol* struct_sym = scope_resolve_tree_bfs(ctx->root_scope, struct_name);
    if (!struct_sym) {
        return;
    }

    if (struct_sym->type->kind != TYPE_STRUCT && struct_sym->type->kind != TYPE_CSTRUCT) {
        return;
    }

    // 添加字段补全
    for (int i = 0; i < struct_sym->struct_field_count; i++) {
        const char* field_name = struct_sym->struct_field_names[i];
        TypeInfo* field_type = struct_sym->struct_field_types[i];

        const char* type_str = type_to_string(field_type);
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, field_name, type_str);

        add_completion_item(items, count, capacity,
                           field_name,
                           LSP_COMP_FIELD,
                           detail,
                           NULL);
    }
}

// 从当前文件内容中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
static void add_current_file_struct_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                                const char* struct_name, const char* var_name) {
    if (!content || !struct_name) return;

    const char* search_pos = content;
    char struct_pattern[256];
    char cstruct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);
    snprintf(cstruct_pattern, sizeof(cstruct_pattern), "cstruct %s", struct_name);

    while (*search_pos) {
        const char* struct_kw = strstr(search_pos, struct_pattern);
        const char* cstruct_kw = strstr(search_pos, cstruct_pattern);
        const char* found_kw = NULL;
        int matched_pattern_len = 0;

        if (cstruct_kw && (!struct_kw || cstruct_kw < struct_kw)) {
            if (cstruct_kw > content) {
                char before = *(cstruct_kw - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = cstruct_kw + 1;
                    continue;
                }
            }
            found_kw = cstruct_kw;
            matched_pattern_len = strlen(cstruct_pattern);
        } else if (struct_kw) {
            if (struct_kw > content) {
                char before = *(struct_kw - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = struct_kw + 1;
                    continue;
                }
            }
            found_kw = struct_kw;
            matched_pattern_len = strlen(struct_pattern);
        } else {
            break;
        }

        const char* check = found_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            if (check >= content + 5) {
                if (strncmp(check - 5, "export", 6) != 0) {
                    search_pos = found_kw + 1;
                    continue;
                }
            } else {
                search_pos = found_kw + 1;
                continue;
            }
        }

        const char* after_name = found_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = found_kw + 1;
            continue;
        }

        const char* brace = strchr(found_kw, '{');
        if (!brace) break;

        // 使用括号匹配找到 struct 的结束 '}'（考虑嵌套的 {}）
        const char* end_brace = brace + 1;
        int brace_count = 1;
        while (*end_brace && brace_count > 0) {
            if (*end_brace == '{') brace_count++;
            else if (*end_brace == '}') brace_count--;
            end_brace++;
        }
        if (brace_count != 0) break;  // 没有找到匹配的 '}
        end_brace--;  // 回退到 '}' 的位置

        // 解析字段
        const char* p = brace + 1;
        while (p < end_brace) {
            // 跳过空白字符
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            // 跳过注释
            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            // 检查是否是 func 定义
            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                // 解析 func 名称
                const char* func_name_start = p + 4;  // 跳过 "func"
                // 跳过空白
                while (*func_name_start && isspace((unsigned char)*func_name_start)) func_name_start++;
                // 解析函数名
                const char* name_start = func_name_start;
                while (*func_name_start && (isalnum((unsigned char)*func_name_start) || *func_name_start == '_')) func_name_start++;
                int name_len = func_name_start - name_start;

                if (name_len > 0) {
                    char* method_name = (char*)malloc(name_len + 1);
                    if (method_name) {
                        strncpy(method_name, name_start, name_len);
                        method_name[name_len] = '\0';

                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s()", var_name, method_name);

                        add_completion_item(items, count, capacity,
                                           method_name,
                                           LSP_COMP_METHOD,
                                           detail,
                                           NULL);
                        free(method_name);
                    }
                }

                // 跳过 func 体
                const char* func_brace = strchr(p, '{');
                if (func_brace && func_brace < end_brace) {
                    // 跳过 func 体（使用括号匹配）
                    int func_brace_count = 1;
                    p = func_brace + 1;
                    while (*p && func_brace_count > 0 && p < end_brace) {
                        if (*p == '{') func_brace_count++;
                        else if (*p == '}') func_brace_count--;
                        p++;
                    }
                } else {
                    // 没有找到 '{', 跳过这一行
                    while (p < end_brace && *p != '\n') p++;
                }
                continue;
            }

            // 解析类型（如 int, float, string 等）
            const char* type_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int type_len = p - type_start;

            // 跳过空白
            while (p < end_brace && isspace((unsigned char)*p)) p++;

            // 解析字段名
            const char* name_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int name_len = p - name_start;

            if (type_len > 0 && name_len > 0) {
                char* field_name = (char*)malloc(name_len + 1);
                char* field_type = (char*)malloc(type_len + 1);
                if (field_name && field_type) {
                    strncpy(field_name, name_start, name_len);
                    field_name[name_len] = '\0';
                    strncpy(field_type, type_start, type_len);
                    field_type[type_len] = '\0';

                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, field_name, field_type);

                    add_completion_item(items, count, capacity,
                                       field_name,
                                       LSP_COMP_FIELD,
                                       detail,
                                       NULL);
                    free(field_name);
                    free(field_type);
                }
            }

            // 跳过空白字符和换行
            while (p < end_brace && (isspace((unsigned char)*p) || *p == '\n' || *p == '\r')) p++;
        }

        break;  // 只处理第一个匹配的 struct
    }
}
