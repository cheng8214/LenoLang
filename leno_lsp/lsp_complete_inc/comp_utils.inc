// 检查是否是字符串字面量结尾（如 '"...".' 或 "'...'.", '"".', "''"）
// 返回字符串开始的位置（不包括引号），如果不是字符串字面量则返回 -1
static int is_string_literal_before_dot(const char* content, int dot_pos) {
    if (dot_pos <= 0) return -1;
    
    // 跳过点号前的空白
    int pos = dot_pos - 1;
    while (pos >= 0 && isspace((unsigned char)content[pos])) {
        pos--;
    }
    
    // 检查是否以引号结尾
    if (pos < 0 || (content[pos] != '"' && content[pos] != '\'')) {
        return -1;
    }
    
    char quote = content[pos];
    pos--;
    
    // 向前查找匹配的引号
    while (pos >= 0) {
        if (content[pos] == quote) {
            // 检查是否是转义的（前面有奇数个反斜杠）
            int backslash_count = 0;
            int check_pos = pos - 1;
            while (check_pos >= 0 && content[check_pos] == '\\') {
                backslash_count++;
                check_pos--;
            }
            // 如果反斜杠数量是偶数，说明这不是转义的引号
            if (backslash_count % 2 == 0) {
                return pos + 1;  // 返回字符串内容开始的位置
            }
        }
        pos--;
    }
    
    return -1;
}

static bool is_type_annotation_context(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return false;

    int pos = cursor_offset - 1;

    while (pos >= 0 && (isalnum((unsigned char)content[pos]) || content[pos] == '_')) {
        pos--;
    }

    while (pos >= 0 && isspace((unsigned char)content[pos])) {
        pos--;
    }

    if (pos < 0) return false;

    if (content[pos] == ':') {
        int check = pos - 1;
        while (check >= 0 && isspace((unsigned char)content[check])) check--;

        if (check >= 0 && content[check] == ')') {
            return true;
        }

        if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
            while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                check--;
            }
            while (check >= 0 && isspace((unsigned char)content[check])) check--;

            if (check >= 2 && content[check - 2] == 'v' && content[check - 1] == 'a' && content[check] == 'r' &&
                (check == 2 || (!isalnum((unsigned char)content[check - 3]) && content[check - 3] != '_'))) {
            }

            int paren_depth = 0;
            int p2 = check;
            while (p2 >= 0) {
                if (content[p2] == ')') paren_depth++;
                else if (content[p2] == '(') {
                    paren_depth--;
                    if (paren_depth < 0) {
                        p2--;
                        while (p2 >= 0 && isspace((unsigned char)content[p2])) p2--;
                        if (p2 >= 0 && (isalnum((unsigned char)content[p2]) || content[p2] == '_')) {
                            while (p2 >= 0 && (isalnum((unsigned char)content[p2]) || content[p2] == '_')) p2--;
                            while (p2 >= 0 && isspace((unsigned char)content[p2])) p2--;
                            if (p2 >= 3 && content[p2 - 3] == 'f' && content[p2 - 2] == 'u' && content[p2 - 1] == 'n' && content[p2] == 'c' &&
                                (p2 == 3 || (!isalnum((unsigned char)content[p2 - 4]) && content[p2 - 4] != '_'))) {
                                return true;
                            }
                        }
                        break;
                    }
                }
                p2--;
            }
        }
        return false;
    }

    if (pos >= 1 && content[pos - 1] == 'a' && content[pos] == 's') {
        if (pos == 1 || (!isalnum((unsigned char)content[pos - 2]) && content[pos - 2] != '_')) {
            int line_start = pos - 2;
            while (line_start >= 0 && content[line_start] != '\n') line_start--;
            if (line_start < 0) line_start = 0;
            else line_start++;

            bool is_import = false;
            const char* line = content + line_start;
            const char* ip = strstr(line, "import");
            if (ip && ip < content + pos - 1) {
                if (ip == line || (!isalnum((unsigned char)*(ip - 1)) && *(ip - 1) != '_')) {
                    if (!isalnum((unsigned char)ip[6]) && ip[6] != '_') {
                        is_import = true;
                    }
                }
            }

            if (!is_import) {
                return true;
            }
        }
    }

    // 检测 "is" 后的类型上下文 (type guard: `x is int`)
    if (pos >= 2 && content[pos - 2] == 'i' && content[pos - 1] == 's') {
        if (pos == 2 || (!isalnum((unsigned char)content[pos - 3]) && content[pos - 3] != '_')) {
            return true;
        }
    }

    if (content[pos] == '(' || content[pos] == ',') {
        if (content[pos] == '(') {
            int check = pos - 1;
            while (check >= 0 && isspace((unsigned char)content[check])) check--;
            if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) check--;
                while (check >= 0 && isspace((unsigned char)content[check])) check--;
                if (check >= 3 && content[check - 3] == 'f' && content[check - 2] == 'u' && content[check - 1] == 'n' && content[check] == 'c' &&
                    (check == 3 || (!isalnum((unsigned char)content[check - 4]) && content[check - 4] != '_'))) {
                    return true;
                }
            }
        } else {
            int paren_depth = 0;
            int check = pos - 1;
            while (check >= 0) {
                if (content[check] == ')') paren_depth++;
                else if (content[check] == '(') {
                    paren_depth--;
                    if (paren_depth < 0) {
                        check--;
                        while (check >= 0 && isspace((unsigned char)content[check])) check--;
                        if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                            while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) check--;
                            while (check >= 0 && isspace((unsigned char)content[check])) check--;
                            if (check >= 3 && content[check - 3] == 'f' && content[check - 2] == 'u' && content[check - 1] == 'n' && content[check] == 'c' &&
                                (check == 3 || (!isalnum((unsigned char)content[check - 4]) && content[check - 4] != '_'))) {
                                return true;
                            }
                        }
                        break;
                    }
                }
                check--;
            }
        }
    }

    {
        int line_start = pos;
        while (line_start > 0 && content[line_start - 1] != '\n') line_start--;
        const char* line = content + line_start;
        int line_len = pos - line_start;
        if (line_len == 0) return false;

        char* line_copy = (char*)malloc(line_len + 1);
        memcpy(line_copy, line, line_len);
        line_copy[line_len] = '\0';

        char* trimmed = line_copy;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;

        bool is_type_context = false;

        if (trimmed[0] == '\0') {
            is_type_context = true;
        } else {
        static const char* type_prefixes[] = {
            "func ", "export func ", "struct ", "export struct ",
            "cstruct ", "export cstruct ", "enum ", "export enum ",
            "face ", "export face ",
            "alias ", "export alias ",
            "clib ", "export clib ",
            "cfunc ", "export cfunc ",
            NULL
        };
            for (int i = 0; type_prefixes[i]; i++) {
                if (strncmp(trimmed, type_prefixes[i], strlen(type_prefixes[i])) == 0) {
                    is_type_context = true;
                    break;
                }
            }
        }

        free(line_copy);

        if (is_type_context) {
            return true;
        }
    }

    return false;
}

// 获取光标前的单词（包含模块前缀，如 "maths.")"
static char* get_word_before_cursor(const char* content, LspPosition pos) {
    if (!content) return NULL;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return NULL;
    
    // 检查光标前是否是点号（如 "maths."）
    int start = offset - 1;
    
    // 如果光标前是点号，需要包含点号前面的模块名
    if (start >= 0 && content[start] == '.') {
        // 检查是否是字符串字面量（如 "". 或 "hello".）
        if (is_string_literal_before_dot(content, start) >= 0) {
            // 返回特殊标记表示字符串字面量
            return strdup("__STRING_LITERAL__");
        }
        
        // 包含点号
        start--;
        // 继续向前查找标识符（如 Color）
        while (start >= 0 && (isalnum((unsigned char)content[start]) ||
                              content[start] == '_')) {
            start--;
        }

        // 检查是否还有一层（如 "cs_module.Color."）
        // 如果前面是点号，继续向前查找模块名
        if (start >= 0 && content[start] == '.') {
            start--;
            // 继续向前查找模块名（如 cs_module）
            while (start >= 0 && (isalnum((unsigned char)content[start]) ||
                                  content[start] == '_')) {
                start--;
            }
            start++;
        } else {
            start++;
        }

        int len = offset - start;
        if (len <= 0) return NULL;

        char* word = (char*)malloc(len + 1);
        if (!word) return NULL;

        memcpy(word, content + start, len);
        word[len] = '\0';

        return word;
    }
    
    // 普通单词（不包含点号）
    while (start >= 0 && (isalnum((unsigned char)content[start]) || 
                          content[start] == '_')) {
        start--;
    }
    start++;
    
    int len = offset - start;
    if (len <= 0) return NULL;
    
    char* word = (char*)malloc(len + 1);
    if (!word) return NULL;
    
    memcpy(word, content + start, len);
    word[len] = '\0';
    
    return word;
}

// 检查是否是模块前缀（如 "maths." 或 "color_module.Color."）
// 支持两层：module.member.
// module_out: 输出模块名（如 "maths" 或 "color_module"）
// member_out: 输出成员名（如 "Color"），如果没有第二层则为 NULL
static bool parse_module_prefix(const char* word, char** module_out, char** member_out) {
    if (!word) return false;

    const char* first_dot = strchr(word, '.');
    if (!first_dot) return false;

    int module_len = first_dot - word;
    if (module_len == 0) return false;

    *module_out = (char*)malloc(module_len + 1);
    if (!*module_out) return false;

    memcpy(*module_out, word, module_len);
    (*module_out)[module_len] = '\0';

    // 检查是否有第二层（如 "Color."）
    *member_out = NULL;
    const char* second_dot = strchr(first_dot + 1, '.');
    if (second_dot) {
        int member_len = second_dot - (first_dot + 1);
        if (member_len > 0) {
            *member_out = (char*)malloc(member_len + 1);
            if (*member_out) {
                memcpy(*member_out, first_dot + 1, member_len);
                (*member_out)[member_len] = '\0';
            }
        }
    }

    return true;
}

static int detect_use_context(const char* content, LspPosition pos, char** use_module_out, char** use_prefix_out) {
    if (!content) return 0;

    *use_module_out = NULL;
    *use_prefix_out = NULL;

    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return 0;

    int line_start = offset - 1;
    while (line_start > 0 && content[line_start - 1] != '\n') line_start--;

    int p = line_start;
    while (p < offset && isspace((unsigned char)content[p])) p++;

    if (p + 3 > offset) return 0;
    if (strncmp(content + p, "use", 3) != 0) return 0;

    if (p > line_start && (isalnum((unsigned char)content[p-1]) || content[p-1] == '_')) return 0;

    if (p + 3 < offset && (isalnum((unsigned char)content[p+3]) || content[p+3] == '_')) return 0;

    if (p + 3 >= offset) return 0;

    int after_use = p + 3;
    while (after_use < offset && isspace((unsigned char)content[after_use])) after_use++;

    if (after_use >= offset) return 1;

    int alias_start = after_use;
    int q = after_use;
    while (q < offset && (isalnum((unsigned char)content[q]) || content[q] == '_')) q++;

    int alias_len = q - alias_start;
    if (alias_len == 0) return 1;

    if (q < offset && content[q] == '.') {
        *use_module_out = (char*)malloc(alias_len + 1);
        if (*use_module_out) {
            memcpy(*use_module_out, content + alias_start, alias_len);
            (*use_module_out)[alias_len] = '\0';
        }

        int after_dot = q + 1;

        // 批量模式: use module.(A, B, C)
        if (after_dot < offset && content[after_dot] == '(') {
            int paren_start = after_dot;
            // 在括号内查找光标前的最后一个标识符前缀
            int r = paren_start + 1;
            int last_ident_start = -1;
            int last_ident_len = 0;

            while (r < offset) {
                if (isalnum((unsigned char)content[r]) || content[r] == '_') {
                    if (last_ident_start < 0 || r != last_ident_start + last_ident_len) {
                        // 新标识符开始
                        last_ident_start = r;
                        last_ident_len = 0;
                    }
                    last_ident_len++;
                    r++;
                } else if (content[r] == ',') {
                    // 逗号分隔，重置标识符追踪
                    r++;
                    last_ident_start = -1;
                    last_ident_len = 0;
                } else if (isspace((unsigned char)content[r])) {
                    r++;
                } else {
                    break;
                }
            }

            if (last_ident_start >= 0 && last_ident_len > 0) {
                *use_prefix_out = (char*)malloc(last_ident_len + 1);
                if (*use_prefix_out) {
                    memcpy(*use_prefix_out, content + last_ident_start, last_ident_len);
                    (*use_prefix_out)[last_ident_len] = '\0';
                }
            }
            // 如果光标刚在 '(' 之后或 ',' 之后，prefix 为空（返回空串，提供全部补全）

            return 2;
        }

        // 单模式: use module.Type
        if (after_dot < offset) {
            int sym_start = after_dot;
            int r = after_dot;
            while (r < offset && (isalnum((unsigned char)content[r]) || content[r] == '_')) r++;
            int sym_len = r - sym_start;
            if (sym_len > 0) {
                *use_prefix_out = (char*)malloc(sym_len + 1);
                if (*use_prefix_out) {
                    memcpy(*use_prefix_out, content + sym_start, sym_len);
                    (*use_prefix_out)[sym_len] = '\0';
                }
            }
        }

        return 2;
    }

    *use_prefix_out = (char*)malloc(alias_len + 1);
    if (*use_prefix_out) {
        memcpy(*use_prefix_out, content + alias_start, alias_len);
        (*use_prefix_out)[alias_len] = '\0';
    }
    return 1;
}
