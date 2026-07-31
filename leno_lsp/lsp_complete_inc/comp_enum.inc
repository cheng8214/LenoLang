// 从当前文件内容中查找 enum 定义并提供补全
// enum_name: enum 名称（如 "Color"）
static void add_current_file_enum_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                              const char* enum_name) {
    // 在内容中查找 enum 定义
    const char* search_pos = content;
    char enum_pattern[256];
    snprintf(enum_pattern, sizeof(enum_pattern), "enum %s", enum_name);

    while (*search_pos) {
        const char* enum_kw = strstr(search_pos, enum_pattern);
        if (!enum_kw) {
            break;
        }

        // 确保这是一个 enum 定义（前面是行首或空白字符）
        const char* check = enum_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            search_pos = enum_kw + 1;
            continue;
        }

        // 检查 enum 名称后面是否跟着空白或 '{'（避免匹配到 enum Colorful 这种）
        const char* after_name = enum_kw + strlen(enum_pattern);
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = enum_kw + 1;
            continue;
        }

        // 找到了 enum 定义，解析 enum 值
        // 支持两种格式：enum Color { ... } 或 enum Color\n{ ... }
        const char* brace = strchr(enum_kw, '{');
        if (!brace) break;

        const char* end_brace = strchr(brace, '}');
        if (!end_brace) break;

        // 解析 enum 值
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

            // 解析 enum 值名
            const char* name_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;

            int name_len = p - name_start;
            if (name_len > 0) {
                char* value_name = (char*)malloc(name_len + 1);
                if (value_name) {
                    strncpy(value_name, name_start, name_len);
                    value_name[name_len] = '\0';

                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s.%s: int", enum_name, value_name);

                    add_completion_item(items, count, capacity,
                                       value_name,
                                       LSP_COMP_ENUM_MEMBER,
                                       detail,
                                       NULL);
                    free(value_name);
                }
            }

            // 跳过空白字符和逗号
            while (p < end_brace && (isspace((unsigned char)*p) || *p == ',')) p++;
        }

        break;  // 只处理第一个匹配的 enum
    }
}
