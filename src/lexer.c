#include "include/lenolang.h"
#include "include/leno_lexer.h"
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// ============================================================================
// Unicode 支持函数
// ============================================================================

// 检查字符是否是 UTF-8 多字节序列的开始（中文等）
static int is_utf8_start(char c) {
    unsigned char uc = (unsigned char)c;
    // 110xxxxx - 2字节序列开始（大部分常用中文）
    // 1110xxxx - 3字节序列开始
    // 11110xxx - 4字节序列开始
    return (uc >= 0xC0 && uc <= 0xFD);
}

// 检查字符是否是 UTF-8 的后续字节
static int is_utf8_continuation(char c) {
    unsigned char uc = (unsigned char)c;
    // 10xxxxxx - 后续字节
    return (uc >= 0x80 && uc <= 0xBF);
}

// 检查字符是否可以作为标识符的开始（字母或中文）
static int is_ident_start(char c) {
    return isalpha(c) || c == '_' || is_utf8_start(c);
}

// 检查字符是否可以作为标识符的后续字符（字母、数字、中文）
static int is_ident_char(char c) {
    return isalnum(c) || c == '_' || is_utf8_start(c) || is_utf8_continuation(c);
}

// ============================================================================
// 关键字哈希表 - O(1) 查找
// ============================================================================

#define KEYWORD_TABLE_SIZE 64  // 2的幂，便于位运算
#define KEYWORD_TABLE_MASK (KEYWORD_TABLE_SIZE - 1)

typedef struct KeywordEntry {
    const char* keyword;
    LenoTokenType type;
    struct KeywordEntry* next;
} KeywordEntry;

static KeywordEntry* keyword_table[KEYWORD_TABLE_SIZE];
static int keyword_table_initialized = 0;

// FNV-1a 哈希函数
static uint32_t hash_keyword(const char* str, int len) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)str[i];
        hash *= 16777619;
    }
    return hash;
}

// 初始化关键字哈希表
static void init_keyword_table(void) {
    if (keyword_table_initialized) return;
    
    // 清空哈希表
    for (int i = 0; i < KEYWORD_TABLE_SIZE; i++) {
        keyword_table[i] = NULL;
    }
    
    // 关键字列表
    struct { const char* kw; LenoTokenType type; } keywords[] = {
        {"if", TOK_IF},
        {"else", TOK_ELSE},
        {"eif", TOK_EIF},
        {"then", TOK_THEN},
        {"while", TOK_WHILE},
        {"for", TOK_FOR},
        {"to", TOK_TO},
        {"break", TOK_BREAK},
        {"continue", TOK_CONTINUE},
        {"switch", TOK_SWITCH},
        {"case", TOK_CASE},
        {"default", TOK_DEFAULT},
        {"var", TOK_VAR},
        {"const", TOK_CONST},
        {"return", TOK_RETURN},
        {"func", TOK_FUNC},
        {"true", TOK_TRUE},
        {"false", TOK_FALSE},
        {"null", TOK_NULL},
        {"import", TOK_IMPORT},
        {"export", TOK_EXPORT},
        {"as", TOK_AS},
        {"use", TOK_USE},
        {"and", TOK_AND},
        {"or", TOK_OR},
        {"not", TOK_NOT},
        {"is", TOK_IS},
        {"in", TOK_IN},
        {"try", TOK_TRY},
        {"catch", TOK_CATCH},
        {"throw", TOK_THROW},
        {"finally", TOK_FINALLY},
        {"struct", TOK_STRUCT},
        {"enum", TOK_ENUM},
        {"face", TOK_FACE},
        {"alias", TOK_ALIAS},
        {"impl", TOK_IMPL},
        {"new", TOK_NEW},
        {"int", TOK_INT_TYPE},
        {"float", TOK_FLOAT_TYPE},
        {"string", TOK_STRING_TYPE},
        {"bool", TOK_BOOL_TYPE},
        {"Array", TOK_ARRAY_TYPE},
        {"Dict", TOK_DICT_TYPE},
        {"any", TOK_ANY_TYPE},
        // {"Bint", TOK_BINT},  // 已移除：对外统一用 int
        {"File", TOK_FILE_TYPE},

        {"Socket", TOK_SOCKET_TYPE},
        {"Channel", TOK_CHANNEL_TYPE},
        {"Thread", TOK_THREAD_TYPE},
        {"Future", TOK_FUTURE_TYPE},
        {"Ptr", TOK_PTR_TYPE},
        {"async", TOK_ASYNC},
        {"await", TOK_AWAIT},
        // C 布局类型关键字
        {"i8", TOK_I8},
        {"u8", TOK_U8},
        {"i16", TOK_I16},
        {"u16", TOK_U16},
        {"i32", TOK_I32},
        {"u32", TOK_U32},
        {"i64", TOK_I64},
        {"u64", TOK_U64},
        {"f32", TOK_F32},
        {"f64", TOK_F64},
        {"c_int", TOK_C_INT},
        {"c_uint", TOK_C_UINT},
        {"c_long", TOK_C_LONG},
        {"c_ulong", TOK_C_ULONG},
        {"c_longlong", TOK_C_LONGLONG},
        {"c_ulonglong", TOK_C_ULONGLONG},
        {"c_size", TOK_C_SIZE},
        {"c_ssize", TOK_C_SSIZE},
        {"cstruct", TOK_CSTRUCT},
        {"clib", TOK_CLIB},
        {"cfunc", TOK_CFUNC},
        {"str8", TOK_STR8},
        {"str16", TOK_STR16},
    };
    
    int keyword_count = sizeof(keywords) / sizeof(keywords[0]);
    
    // 插入到哈希表
    for (int i = 0; i < keyword_count; i++) {
        uint32_t hash = hash_keyword(keywords[i].kw, strlen(keywords[i].kw));
        int index = hash & KEYWORD_TABLE_MASK;
        
        KeywordEntry* entry = (KeywordEntry*)malloc(sizeof(KeywordEntry));
        entry->keyword = keywords[i].kw;
        entry->type = keywords[i].type;
        entry->next = keyword_table[index];
        keyword_table[index] = entry;
    }
    
    keyword_table_initialized = 1;
}

// 在 check_keyword 之前自动初始化
static LenoTokenType lookup_keyword(const char* text, int len) {
    if (!keyword_table_initialized) {
        init_keyword_table();
    }
    
    uint32_t hash = hash_keyword(text, len);
    int index = hash & KEYWORD_TABLE_MASK;
    
    KeywordEntry* entry = keyword_table[index];
    while (entry) {
        if (strncmp(entry->keyword, text, len) == 0 && entry->keyword[len] == '\0') {
            return entry->type;
        }
        entry = entry->next;
    }
    return TOK_IDENT;
}

static char peek(Lexer* lex) {
    return lex->src[lex->pos];
}

static char peek_next(Lexer* lex) {
    return lex->src[lex->pos + 1];
}

static char advance(Lexer* lex) {
    char c = lex->src[lex->pos];
    if (c != '\0') {
        lex->pos++;
        if (c == '\n') {
            lex->line++;
            lex->line_start = lex->pos;
        }
    }
    return c;
}

static Token make_token(Lexer* lex, LenoTokenType type) {
    Token tok;
    tok.type = type;
    tok.text = NULL;
    tok.len = 0;
    tok.line = lex->line;
    tok.num_val = 0;
    tok.is_bigint = 0;
    tok.bigint_str = NULL;
    tok.is_float = 0;
    return tok;
}

static void skip_whitespace(Lexer* lex) {
    while (1) {
        char c = peek(lex);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
            case '\n':
                advance(lex);
                break;
            case '/':
                if (peek_next(lex) == '/') {
                    while (peek(lex) != '\n' && peek(lex) != '\0') {
                        advance(lex);
                    }
                } else if (peek_next(lex) == '*') {
                    advance(lex);
                    advance(lex);
                    while (!(peek(lex) == '*' && peek_next(lex) == '/') && peek(lex) != '\0') {
                        advance(lex);
                    }
                    if (peek(lex) == '*') {
                        advance(lex);
                        advance(lex);
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static LenoTokenType check_keyword(const char* text, int len) {
    // 使用哈希表 O(1) 查找
    return lookup_keyword(text, len);
}

static Token read_string(Lexer* lex) {
    Token tok = make_token(lex, TOK_STRING);
    const char* start = lex->src + lex->pos;
    advance(lex);

    while (peek(lex) != '\0') {
        if (peek(lex) == '\\') {
            advance(lex);
            if (peek(lex) != '\0') {
                advance(lex);
            }
        } else if (peek(lex) == '"') {
            break;
        } else {
            advance(lex);
        }
    }

    if (peek(lex) == '"') {
        tok.text = start + 1;
        tok.len = (lex->src + lex->pos) - tok.text;
        advance(lex);
    } else {
        error_add(ERR_SYNTAX, lex->line, "未终止的字符串");
    }

    return tok;
}

// 读取原始字符串字面量（不处理转义字符，但支持 "" 表示 "）
static Token read_raw_string(Lexer* lex) {
    Token tok = make_token(lex, TOK_RAW_STRING);
    advance(lex); // 跳过 @
    
    if (peek(lex) != '"') {
        error_add(ERR_SYNTAX, lex->line, "原始字符串需要使用 @\"...\"");
        return tok;
    }
    advance(lex); // 跳过 "
    
    const char* start = lex->src + lex->pos;
    int raw_len = 0;
    
    while (peek(lex) != '\0') {
        if (peek(lex) == '"') {
            // 检查是否是 ""（两个连续的引号）
            if (peek_next(lex) == '"') {
                // 这是转义的引号，跳过第一个，保留第二个
                advance(lex); // 跳过第一个 "
                advance(lex); // 跳过第二个 "
                raw_len += 2; // 计两个字符（解析器会处理 "" -> " 的转换）
            } else {
                // 字符串结束
                break;
            }
        } else {
            advance(lex);
            raw_len++;
        }
    }
    
    if (peek(lex) == '"') {
        tok.text = start;
        tok.len = raw_len;
        advance(lex); // 跳过结尾的 "
    } else {
        error_add(ERR_SYNTAX, lex->line, "未终止的原始字符串");
    }
    
    return tok;
}

// 读取插值字符串片段（到 { 或 " 为止）
static Token read_interp_part(Lexer* lex) {
    Token tok = make_token(lex, TOK_INTERP_PART);
    const char* start = lex->src + lex->pos;

    while (peek(lex) != '\0') {
        char c = peek(lex);
        if (c == '\\') {
            advance(lex);
            if (peek(lex) != '\0') advance(lex);
        } else if (c == '"' || c == '{') {
            break;
        } else {
            advance(lex);
        }
    }

    tok.text = start;
    tok.len = (lex->src + lex->pos) - start;
    return tok;
}

static Token read_number(Lexer* lex) {
    Token tok = make_token(lex, TOK_NUM);
    const char* start = lex->src + lex->pos;

    // 检查十六进制前缀 (0x 或 0X)
    int is_hex = 0;
    if (peek(lex) == '0' && (peek_next(lex) == 'x' || peek_next(lex) == 'X')) {
        is_hex = 1;
        advance(lex); // 跳过 '0'
        advance(lex); // 跳过 'x' 或 'X'
        // 读取十六进制数字
        while (isxdigit(peek(lex))) {
            advance(lex);
        }
    } else {
        // 普通十进制数字
        while (isdigit(peek(lex))) {
            advance(lex);
        }
    }

    int is_float = 0;
    if (!is_hex && peek(lex) == '.' && isdigit(peek_next(lex))) {
        is_float = 1;
        advance(lex);
        while (isdigit(peek(lex))) {
            advance(lex);
        }
    }

    tok.text = start;
    tok.len = (lex->src + lex->pos) - start;

    if (is_float) {
        tok.num_val = strtod(start, NULL);
        tok.is_bigint = 0;
        tok.bigint_str = NULL;
        tok.is_float = 1;
    } else if (is_hex) {
        // 解析十六进制数字
        // 检查是否需要 BigInt（长度超过16位，或值超过 2^53 无法精确表示为 double）
        if (tok.len > 18) {  // 0x + 16位 = 18字符，超过需要 BigInt
            tok.is_bigint = 1;
            tok.bigint_str = (char*)malloc(tok.len + 1);
            if (tok.bigint_str) {
                memcpy(tok.bigint_str, start, tok.len);
                tok.bigint_str[tok.len] = '\0';
            }
            tok.num_val = 0;
            tok.is_float = 0;
        } else {
            // 使用 strtoull 解析十六进制（无符号）
            unsigned long long val = strtoull(start, NULL, 16);
            // 如果值超过 2^53（9007199254740992），double 无法精确表示
            if (val > 9007199254740992ULL) {
                tok.is_bigint = 1;
                tok.bigint_str = (char*)malloc(tok.len + 1);
                if (tok.bigint_str) {
                    memcpy(tok.bigint_str, start, tok.len);
                    tok.bigint_str[tok.len] = '\0';
                }
                tok.num_val = 0;
                tok.is_float = 0;
            } else {
                tok.is_bigint = 0;
                tok.bigint_str = NULL;
                tok.num_val = (double)val;
                tok.is_float = 0;
            }
        }
    } else {
        double val = strtod(start, NULL);
        if (tok.len > 10 || val > 2147483647.0 || val < -2147483648.0) {
            tok.is_bigint = 1;
            tok.bigint_str = (char*)malloc(tok.len + 1);
            if (tok.bigint_str) {
                memcpy(tok.bigint_str, start, tok.len);
                tok.bigint_str[tok.len] = '\0';
            }
            tok.num_val = 0;
            tok.is_float = 0;
        } else {
            tok.is_bigint = 0;
            tok.bigint_str = NULL;
            tok.num_val = val;
            tok.is_float = 0;
        }
    }

    return tok;
}

static Token read_identifier(Lexer* lex) {
    const char* start = lex->src + lex->pos;

    while (is_ident_char(peek(lex))) {
        advance(lex);
    }

    int len = (lex->src + lex->pos) - start;
    Token tok = make_token(lex, check_keyword(start, len));
    tok.text = start;
    tok.len = len;
    return tok;
}

void lexer_init(Lexer* lex, const char* source) {
    lex->src = source;
    lex->pos = 0;
    lex->line = 1;
    lex->line_start = 0;
    lex->in_interp = 0;
    lex->in_expr = 0;
    lex->brace_depth = 0;
    if ((unsigned char)source[0] == 0xEF &&
        (unsigned char)source[1] == 0xBB &&
        (unsigned char)source[2] == 0xBF) {
        lex->pos = 3;
        lex->line_start = 3;
    }
    lexer_next(lex);
}

void lexer_next(Lexer* lex) {
    // 在插值字符串中（不在表达式内）不要跳过空格，空格是字符串的一部分
    if (!(lex->in_interp && !lex->in_expr)) {
        skip_whitespace(lex);
    }
    
    // 更新当前列号（1-based，用于错误报告）
    error_set_column(lex->pos - lex->line_start + 1);

    char c = peek(lex);

    if (c == '\0') {
        lex->current = make_token(lex, TOK_EOF);
        return;
    }

    // 如果在插值字符串中但不在表达式中
    if (lex->in_interp && !lex->in_expr) {
        // 检查是否是字符串结束
        if (c == '"') {
            advance(lex);
            lex->in_interp = 0;
            lex->current = make_token(lex, TOK_INTERP_END);
            return;
        }
        // 检查是否是表达式开始
        if (c == '{') {
            advance(lex);
            lex->in_expr = 1;
            lex->brace_depth = 1;
            lex->current = make_token(lex, TOK_LBRACE);
            return;
        }
        // 读取字符串片段
        lex->current = read_interp_part(lex);
        return;
    }

    // 如果在表达式中，追踪大括号嵌套
    if (lex->in_interp && lex->in_expr) {
        if (c == '{') {
            lex->brace_depth++;
        } else if (c == '}') {
            lex->brace_depth--;
            if (lex->brace_depth == 0) {
                advance(lex);
                lex->in_expr = 0;
                lex->current = make_token(lex, TOK_RBRACE);
                return;
            }
        }
        // 继续正常处理
    }

    switch (c) {
        case '(': lex->current = make_token(lex, TOK_LPAREN); advance(lex); break;
        case ')': lex->current = make_token(lex, TOK_RPAREN); advance(lex); break;
        case '{': lex->current = make_token(lex, TOK_LBRACE); advance(lex); break;
        case '}': lex->current = make_token(lex, TOK_RBRACE); advance(lex); break;
        case '[': lex->current = make_token(lex, TOK_LBRACKET); advance(lex); break;
        case ']': lex->current = make_token(lex, TOK_RBRACKET); advance(lex); break;
        case ';': lex->current = make_token(lex, TOK_SEMI); advance(lex); break;
        case ',': lex->current = make_token(lex, TOK_COMMA); advance(lex); break;
        case ':': lex->current = make_token(lex, TOK_COLON); advance(lex); break;
        case '.': lex->current = make_token(lex, TOK_DOT); advance(lex); break;
        case '+':
            if (peek_next(lex) == '+') {
                lex->current = make_token(lex, TOK_INC);
                advance(lex);
                advance(lex);
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_PLUSEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_PLUS);
                advance(lex);
            }
            break;
        case '-':
            if (peek_next(lex) == '-') {
                lex->current = make_token(lex, TOK_DEC);
                advance(lex);
                advance(lex);
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_MINUSEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_MINUS);
                advance(lex);
            }
            break;
        case '*':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_STAREQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_STAR);
                advance(lex);
            }
            break;
        case '/':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_SLASHEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_SLASH);
                advance(lex);
            }
            break;

        case '"': lex->current = read_string(lex); break;
        case '$':
            if (peek_next(lex) == '"') {
                advance(lex); // 跳过 $
                advance(lex); // 跳过 "
                lex->in_interp = 1;
                lex->in_expr = 0;
                lex->brace_depth = 0;
                lex->current = make_token(lex, TOK_INTERP_STRING);
            } else {
                error_add(ERR_SYNTAX, lex->line, "意外的字符 '$'，插值字符串需要使用 $\"...\"");
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            }
            break;
        case '@':
            if (peek_next(lex) == '"') {
                lex->current = read_raw_string(lex);
            } else {
                error_add(ERR_SYNTAX, lex->line, "意外的字符 '@'，原始字符串需要使用 @\"...\"");
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            }
            break;
        case '=':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_EQEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_EQ);
                advance(lex);
            }
            break;
        case '!':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_NEQ);
                advance(lex);
                advance(lex);
            } else {
                error_add(ERR_SYNTAX, lex->line, "意外的字符 '!'，请使用 'not' 关键字表示逻辑非");
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            }
            break;
        case '<':
            if (peek_next(lex) == '<') {
                advance(lex);
                advance(lex);
                if (peek(lex) == '=') {
                    lex->current = make_token(lex, TOK_SHLEQ);
                    advance(lex);
                } else {
                    lex->current = make_token(lex, TOK_SHL);
                }
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_LE);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_LT);
                advance(lex);
            }
            break;
        case '>':
            if (peek_next(lex) == '>') {
                advance(lex);
                advance(lex);
                if (peek(lex) == '>') {
                    // >>> 逻辑右移
                    advance(lex);
                    if (peek(lex) == '=') {
                        lex->current = make_token(lex, TOK_USHREQ);
                        advance(lex);
                    } else {
                        lex->current = make_token(lex, TOK_USHR);
                    }
                } else if (peek(lex) == '=') {
                    lex->current = make_token(lex, TOK_SHREQ);
                    advance(lex);
                } else {
                    lex->current = make_token(lex, TOK_SHR);
                }
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_GE);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_GT);
                advance(lex);
            }
            break;
        case '&':
            if (peek_next(lex) == '&') {
                error_add(ERR_SYNTAX, lex->line, "意外的字符 '&&'，请使用 'and' 关键字表示逻辑与");
                advance(lex);
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_BITANDEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_BITAND);
                advance(lex);
            }
            break;
        case '|':
            if (peek_next(lex) == '|') {
                error_add(ERR_SYNTAX, lex->line, "意外的字符 '||'，请使用 'or' 关键字表示逻辑或");
                advance(lex);
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            } else if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_BITOREQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_BITOR);
                advance(lex);
            }
            break;
        case '^':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_BITXOREQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_BITXOR);
                advance(lex);
            }
            break;
        case '~':
            lex->current = make_token(lex, TOK_BITNOT);
            advance(lex);
            break;
        case '?':
            if (peek_next(lex) == '.') {
                lex->current = make_token(lex, TOK_QUESTION_DOT);
                advance(lex);  // 消费 '?'
                advance(lex);  // 消费 '.'
            } else if (peek_next(lex) == '?') {
                lex->current = make_token(lex, TOK_NULL_COALESCE);
                advance(lex);  // 消费第一个 '?'
                advance(lex);  // 消费第二个 '?'
            } else {
                lex->current = make_token(lex, TOK_QUESTION);
                advance(lex);
            }
            break;
        case '#':
            // Leno 不支持 # 开头的语法（注释使用 // 或 /* */）
            error_add(ERR_SYNTAX, lex->line, "意外的字符 '#'，请使用 // 或 /* */ 进行注释");
            advance(lex);
            lex->current = make_token(lex, TOK_ERROR);
            break;
        case '%':
            if (peek_next(lex) == '=') {
                lex->current = make_token(lex, TOK_MODEQ);
                advance(lex);
                advance(lex);
            } else {
                lex->current = make_token(lex, TOK_MOD);
                advance(lex);
            }
            break;
        default:
            if (isdigit(c)) {
                lex->current = read_number(lex);
            } else if (is_ident_start(c)) {
                lex->current = read_identifier(lex);
                // 检查是否是 "not in" 复合 token
                if (lex->current.type == TOK_NOT) {
                    skip_whitespace(lex);
                    // 向前看是否是 in
                    if (peek(lex) == 'i' && peek_next(lex) == 'n') {
                        // 检查是否是完整的 "in" 单词
                        int saved_pos = lex->pos;
                        advance(lex); // i
                        advance(lex); // n
                        char next_after_in = peek(lex);
                        if (!is_ident_char(next_after_in)) {
                            // 确认是 "not in"
                            lex->current.type = TOK_NOT_IN;
                        } else {
                            // 不是 in，恢复位置
                            lex->pos = saved_pos;
                        }
                    }
                }
            } else {
                char err_msg[64];
                if (c >= 32 && c < 127) {
                    snprintf(err_msg, sizeof(err_msg), "意外的字符 '%c'", c);
                } else {
                    snprintf(err_msg, sizeof(err_msg), "意外的字符 (0x%02X)", (unsigned char)c);
                }
                error_add(ERR_SYNTAX, lex->line, err_msg);
                advance(lex);
                lex->current = make_token(lex, TOK_ERROR);
            }
            break;
    }
}

int lexer_match(Lexer* lex, LenoTokenType type) {
    if (lex->current.type == type) {
        lexer_next(lex);
        return 1;
    }
    return 0;
}
