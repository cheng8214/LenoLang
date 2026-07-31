static void find_face_methods_in_content(const char* content, const char* face_name,
                                         LspCompletionItem** items, int* count, int* capacity,
                                         const char* var_name) {
    if (!content || !face_name) return;

    const char* search_pos = content;
    char face_pattern[256];
    char export_face_pattern[256];
    snprintf(face_pattern, sizeof(face_pattern), "face %s", face_name);
    snprintf(export_face_pattern, sizeof(export_face_pattern), "export face %s", face_name);

    while (*search_pos) {
        const char* face_kw = NULL;
        int matched_pattern_len = 0;

        const char* p_export_face = strstr(search_pos, export_face_pattern);
        const char* p_face = strstr(search_pos, face_pattern);

        if (p_export_face && (!p_face || p_export_face < p_face)) {
            face_kw = p_export_face;
            matched_pattern_len = strlen(export_face_pattern);
        } else if (p_face) {
            if (p_face > content) {
                char before = *(p_face - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = p_face + 1;
                    continue;
                }
            }
            face_kw = p_face;
            matched_pattern_len = strlen(face_pattern);
        } else {
            break;
        }

        const char* after_name = face_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = face_kw + 1;
            continue;
        }

        const char* brace = strchr(face_kw, '{');
        if (!brace) break;

        const char* end_brace = brace + 1;
        int brace_count = 1;
        while (*end_brace && brace_count > 0) {
            if (*end_brace == '{') brace_count++;
            else if (*end_brace == '}') brace_count--;
            end_brace++;
        }
        if (brace_count != 0) break;
        end_brace--;

        const char* p = brace + 1;
        while (p < end_brace) {
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                const char* func_name_start = p + 4;
                while (*func_name_start && isspace((unsigned char)*func_name_start)) func_name_start++;
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

                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            while (p < end_brace && *p != '\n') p++;
        }

        break;
    }
}
