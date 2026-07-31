// 从模块源代码中解析指定 struct 的方法
// module_source: 模块文件内容
// struct_name: struct 名称
// module_alias: 模块别名（用于构建 detail）
static void add_module_struct_methods(const char* module_source, const char* struct_name, const char* module_alias,
                                      LspCompletionItem** items, int* count, int* capacity) {
    if (!module_source || !struct_name) {
        return;
    }

    const char* search_pos = module_source;
    char struct_pattern[256];
    char export_struct_pattern[256];
    char cstruct_pattern[256];
    char export_cstruct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);
    snprintf(export_struct_pattern, sizeof(export_struct_pattern), "export struct %s", struct_name);
    snprintf(cstruct_pattern, sizeof(cstruct_pattern), "cstruct %s", struct_name);
    snprintf(export_cstruct_pattern, sizeof(export_cstruct_pattern), "export cstruct %s", struct_name);

    while (*search_pos) {
        const char* struct_kw = NULL;
        int matched_pattern_len = 0;

        const char* p_export_struct = strstr(search_pos, export_struct_pattern);
        const char* p_export_cstruct = strstr(search_pos, export_cstruct_pattern);
        const char* p_struct = strstr(search_pos, struct_pattern);
        const char* p_cstruct = strstr(search_pos, cstruct_pattern);

        if (p_export_cstruct && p_export_cstruct < (p_export_struct ? p_export_struct : (const char*)-1)) {
            struct_kw = p_export_cstruct;
            matched_pattern_len = strlen(export_cstruct_pattern);
        } else if (p_export_struct) {
            struct_kw = p_export_struct;
            matched_pattern_len = strlen(export_struct_pattern);
        } else if (p_cstruct && p_cstruct < (p_struct ? p_struct : (const char*)-1)) {
            struct_kw = p_cstruct;
            matched_pattern_len = strlen(cstruct_pattern);
        } else if (p_struct) {
            if (p_struct > module_source) {
                char before = *(p_struct - 1);
                if (before == 'c') {
                    search_pos = p_struct + 1;
                    continue;
                }
            }
            struct_kw = p_struct;
            matched_pattern_len = strlen(struct_pattern);
        }

        if (!struct_kw) {
            break;
        }

        // 确保这是一个 struct 定义（前面是行首或空白字符）
        const char* check = struct_kw - 1;
        while (check > module_source && isspace((unsigned char)*check)) check--;
        if (check > module_source && *check != '\n' && *check != ';' && *check != '}') {
            search_pos = struct_kw + 1;
            continue;
        }

        const char* after_name = struct_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = struct_kw + 1;
            continue;
        }

        // 找到了 struct 定义，解析方法
        const char* brace = strchr(struct_kw, '{');
        if (!brace) {
            break;
        }

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

        // 解析方法
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
                        snprintf(detail, sizeof(detail), "%s.%s.%s()", module_alias, struct_name, method_name);

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

            // 跳过非 func 的行（字段定义等）
            while (p < end_brace && *p != '\n') p++;
        }

        break;  // 只处理第一个匹配的 struct
    }
}
