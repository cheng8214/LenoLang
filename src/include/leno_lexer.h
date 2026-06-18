#ifndef LENO_LEXER_H
#define LENO_LEXER_H

#include "leno_types.h"
#include <stdint.h>

// ============================================================================
// Lexer 词法分析
// ============================================================================

typedef struct {
    LenoTokenType type;
    const char* text;
    int len;
    int line;
    double num_val;
    int is_bigint;      // 标记是否是大整数
    char* bigint_str;   // 大整数字符串（当 is_bigint 为 1 时有效）
    int is_float;       // 标记是否是浮点数（有小数点）
} Token;

typedef struct {
    const char* src;
    int pos;
    int line;
    int line_start;     // 当前行起始位置（用于计算列号）
    Token current;
    // 插值字符串状态
    int in_interp;      // 是否在插值字符串中
    int in_expr;        // 是否在表达式中
    int brace_depth;    // 大括号嵌套深度
} Lexer;

void lexer_init(Lexer* lex, const char* source);
void lexer_next(Lexer* lex);
int lexer_match(Lexer* lex, LenoTokenType type);

#endif // LENO_LEXER_H
