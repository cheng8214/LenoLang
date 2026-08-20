#include "parser_internal.h"
#include <stdint.h>
#include <ctype.h>

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

// 处理字符串转义字符（如 \n, \t, \xHH, \uHHHH 等）
char* process_escape_sequences(const char* text, int len, int* out_len) {
    // \uHHHH 最多产生 4 字节 UTF-8（实际上 BMP 范围最多 3 字节），预分配足够空间
    char* str = (char*)malloc(len * 4 + 1);
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
                case '\'': str[j++] = '\''; i++; break;
                case '0': str[j++] = '\0'; i++; break;
                case 'b': str[j++] = '\b'; i++; break;   // 退格 0x08
                case 'f': str[j++] = '\f'; i++; break;   // 换页 0x0C
                case 'x': {
                    // \xHH — 十六进制字节转义，HH 为两位十六进制数字
                    if (i + 3 < len && isxdigit((unsigned char)text[i+2]) && isxdigit((unsigned char)text[i+3])) {
                        char hex[3] = { text[i+2], text[i+3], '\0' };
                        str[j++] = (char)strtol(hex, NULL, 16);
                        i += 3; // 跳过 x 和两位十六进制
                    } else {
                        // \x 后不是两位hex，保留反斜杠原样
                        str[j++] = text[i];
                    }
                    break;
                }
                case 'u': {
                    // \uHHHH — Unicode BMP 转义，HHHH 为四位十六进制数字
                    if (i + 5 < len && isxdigit((unsigned char)text[i+2]) && isxdigit((unsigned char)text[i+3])
                                     && isxdigit((unsigned char)text[i+4]) && isxdigit((unsigned char)text[i+5])) {
                        char hex[5] = { text[i+2], text[i+3], text[i+4], text[i+5], '\0' };
                        uint32_t cp = (uint32_t)strtol(hex, NULL, 16);
                        // 编码为 UTF-8
                        if (cp <= 0x7F) {
                            str[j++] = (char)cp;
                        } else if (cp <= 0x7FF) {
                            str[j++] = (char)(0xC0 | (cp >> 6));
                            str[j++] = (char)(0x80 | (cp & 0x3F));
                        } else {
                            str[j++] = (char)(0xE0 | (cp >> 12));
                            str[j++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                            str[j++] = (char)(0x80 | (cp & 0x3F));
                        }
                        i += 5; // 跳过 u 和四位十六进制
                    } else {
                        // \u 后不是四位hex，保留反斜杠原样
                        str[j++] = text[i];
                    }
                    break;
                }
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
           type == TOK_STRUCT || type == TOK_FACE || type == TOK_ALIAS ||
           type == TOK_CSTRUCT || type == TOK_CLIB || type == TOK_CFUNC ||
           type == TOK_ENUM ||
           type == TOK_VAR || type == TOK_RETURN ||
           type == TOK_BREAK || type == TOK_CONTINUE ||
           type == TOK_THROW || type == TOK_TRY ||
           type == TOK_IMPORT || type == TOK_EXPORT || type == TOK_USE ||
           type == TOK_INT_TYPE || type == TOK_FLOAT_TYPE ||
           type == TOK_STRING_TYPE || type == TOK_BOOL_TYPE ||
           type == TOK_ARRAY_TYPE || type == TOK_DICT_TYPE ||
           type == TOK_PTR_TYPE ||
           type == TOK_FILE_TYPE ||
           type == TOK_SOCKET_TYPE ||
           type == TOK_CHANNEL_TYPE ||
           type == TOK_THREAD_TYPE ||
           type == TOK_FUTURE_TYPE ||
           type == TOK_I8 || type == TOK_U8 || type == TOK_I16 || type == TOK_U16 ||
           type == TOK_I32 || type == TOK_U32 || type == TOK_I64 || type == TOK_U64 ||
           type == TOK_F32 || type == TOK_F64 ||
           type == TOK_C_INT || type == TOK_C_UINT ||
           type == TOK_C_LONG || type == TOK_C_ULONG ||
           type == TOK_C_LONGLONG || type == TOK_C_ULONGLONG ||
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
           type == TOK_FILE_TYPE ||
           type == TOK_SOCKET_TYPE ||
           type == TOK_CHANNEL_TYPE ||
           type == TOK_THREAD_TYPE ||
           type == TOK_FUTURE_TYPE ||
           type == TOK_PTR_TYPE ||
           type == TOK_FUNC;
}

// 检查当前 token 是否是 cstruct 布局属性标识符（packed / align）
// packed 和 align 不是全局关键字，而是"上下文关键字"——
// 仅在 cstruct 定义前缀位置才具有特殊含义，其他位置（如字典键、变量名）
// 仍作为普通标识符使用。
int is_cstruct_layout_attr(Parser* p) {
    if (p->lex.current.type != TOK_IDENT) return 0;
    int len = p->lex.current.len;
    const char* text = p->lex.current.text;
    if (len == 6 && strncmp(text, "packed", 6) == 0) return 1;
    if (len == 5 && strncmp(text, "align", 5) == 0) return 1;
    return 0;
}
