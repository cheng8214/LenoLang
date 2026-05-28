// 获取光标前的单词（包含模块前缀，如 "maths.")"
// 支持两层前缀：如 "cs_module.Color."
static char* get_word_before_cursor(const char* content, LspPosition pos) {
    if (!content) return NULL;

    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return NULL;

    // 检查光标前是否是点号（如 "maths." 或 "cs_module.Color."）
    int start = offset - 1;

    // 如果光标前是点号，需要包含点号前面的模块名
    if (start >= 0 && content[start] == '.') {
        // 检查是否是字符串字面量（如 "". 或 "hello".")
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
