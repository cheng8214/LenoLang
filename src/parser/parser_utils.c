#include "parser_internal.h"

// ============================================================================
// 辅助函数
// ============================================================================

// 匹配并消费指定类型的 token
int match(Parser* p, LenoTokenType type) {
    if (p->lex.current.type == type) {
        lexer_next(&p->lex);
        return 1;
    }
    return 0;
}

// 消费指定类型的 token，如果不匹配则报错
int consume(Parser* p, LenoTokenType type, const char* msg) {
    if (p->lex.current.type == type) {
        lexer_next(&p->lex);
        return 1;
    }
    error_add(ERR_SYNTAX, p->lex.current.line, msg);
    return 0;
}

// ============================================================================
// 字符串处理函数
// ============================================================================

// 处理字符串转义字符（如 \n, \t 等）
char* process_escape_sequences(const char* text, int len, int* out_len) {
    char* str = (char*)malloc(len + 1);
    if (!str) return NULL;
    
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '\\' && i + 1 < len) {
            char next = text[i + 1];
            switch (next) {
                case 'n': str[j++] = '\n'; i++; break;
                case 't': str[j++] = '\t'; i++; break;
                case 'r': str[j++] = '\r'; i++; break;
                case '\\': str[j++] = '\\'; i++; break;
                case '"': str[j++] = '"'; i++; break;
                case '0': str[j++] = '\0'; i++; break;
                default:
                    // 未知的转义序列，保留反斜杠
                    str[j++] = text[i];
                    break;
            }
        } else {
            str[j++] = text[i];
        }
    }
    str[j] = '\0';
    if (out_len) *out_len = j;
    return str;
}

// 复制字符串（从 lexer 的 token 文本）
char* copy_string(const char* text, int len) {
    char* str = (char*)malloc(len + 1);
    if (str) {
        memcpy(str, text, len);
        str[len] = '\0';
    }
    return str;
}

// 处理原始字符串中的 "" 转义（转换为 "）
char* process_raw_string(const char* text, int len, int* out_len) {
    char* str = (char*)malloc(len + 1);
    if (!str) return NULL;
    
    int j = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '"' && i + 1 < len && text[i + 1] == '"') {
            // "" 转换为 "
            str[j++] = '"';
            i++; // 跳过第二个引号
        } else {
            str[j++] = text[i];
        }
    }
    str[j] = '\0';
    if (out_len) *out_len = j;
    return str;
}

// ============================================================================
// 错误恢复 - 同步到语句边界
// ============================================================================

// 检查 token 是否是一个语句的开始位置（用于错误恢复同步）
// 包括语句关键词、分号、大括号、标识符、文件结尾
static int is_statement_start(LenoTokenType type) {
    return type == TOK_SEMI || type == TOK_RBRACE || type == TOK_EOF ||
           type == TOK_IF || type == TOK_EIF ||
           type == TOK_WHILE || type == TOK_FOR || type == TOK_SWITCH ||
           type == TOK_FUNC || type == TOK_ASYNC ||
           type == TOK_STRUCT || type == TOK_FACE ||
           type == TOK_CSTRUCT || type == TOK_ENUM ||
           type == TOK_VAR || type == TOK_RETURN ||
           type == TOK_BREAK || type == TOK_CONTINUE ||
           type == TOK_THROW || type == TOK_TRY ||
           type == TOK_IMPORT || type == TOK_EXPORT || type == TOK_USE ||
           type == TOK_INT_TYPE || type == TOK_FLOAT_TYPE ||
           type == TOK_STRING_TYPE || type == TOK_BOOL_TYPE ||
           type == TOK_ARRAY_TYPE || type == TOK_DICT_TYPE ||
           // TOK_BINT 已移除
           type == TOK_IDENT || type == TOK_LBRACE;
}

// 同步解析器到下一个语句边界
// 在解析错误后调用，跳过混乱的 token 直到找到下一个可识别的语句开始位置
// 不会消费边界 token（如 if/func/var/; 等），由调用者决定如何处理
void parser_synchronize(Parser* p) {
    // 如果是词法错误 token，先消费掉
    if (p->lex.current.type == TOK_ERROR) {
        lexer_next(&p->lex);
    }

    while (p->lex.current.type != TOK_EOF &&
           !is_statement_start(p->lex.current.type)) {
        lexer_next(&p->lex);
    }
}

// ============================================================================
// 类型检查函数
// ============================================================================

// 检查 token 是否是类型关键字（int, float, string, bool, array, dict, File, Ptr）
int is_type_keyword(LenoTokenType type) {
    return type == TOK_INT_TYPE ||
           type == TOK_FLOAT_TYPE ||
           type == TOK_STRING_TYPE ||
           type == TOK_BOOL_TYPE ||
           type == TOK_ARRAY_TYPE ||
           type == TOK_DICT_TYPE ||
           // TOK_BINT 已移除
           type == TOK_FILE_TYPE ||
           type == TOK_WIN_TYPE ||
           type == TOK_DRAW_TYPE ||
           type == TOK_EVENT_TYPE ||
           type == TOK_RGB_TYPE ||
           type == TOK_IMAGE_TYPE ||
           type == TOK_FONT_TYPE ||
           type == TOK_STYLE_TYPE ||
           type == TOK_SOCKET_TYPE ||
           type == TOK_PTR_TYPE;
}
