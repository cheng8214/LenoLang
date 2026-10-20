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
    int column;         // 列号（1-based，用于错误报告）
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
    // ★ 2026-10-15：**上一个 token**（每次 `lexer_next` 把旧的 current 挪进来）——
    //   用途：后缀 `++`/`--` 的**跨行判定**（`parse_postfix_inc_dec` 要"操作符与左表达式必须在同一行"，
    //   否则上一行表达式会把下一行行首的 `++i` 吃成 `表达式++` ✗，见那里的注释 ✓）。
    //   用 Token 整体拷贝（≈48 字节）：只在 `lexer_next` 顶部赋值一次，编译期开销可忽略 ✓
    //   ⚠ `Lexer saved = p->lex` 这类**整体保存/恢复**会把 prev 一并带上 ⇒ 行为一致 ✓
    Token prev;
    // 插值字符串状态
    int in_interp;      // 是否在插值字符串中
    int in_expr;        // 是否在表达式中
    int brace_depth;    // 大括号嵌套深度
    // 当前插值字符串是否是**原始**形式（`$@"..."`）—— 片段不处理 `\` 转义、`""` = 一个字面引号
    int in_interp_raw;
} Lexer;

void lexer_init(Lexer* lex, const char* source);
void lexer_next(Lexer* lex);
int lexer_match(Lexer* lex, LenoTokenType type);

// ★ M5b（2026-10-11）：token → 保留字文本（反查 ✓）；不是关键字返回 NULL ✓。
//   给"保留字撞名"的报错用（如 `Outer alias = o` ⇒ "'alias' 是保留字，换个名字 ✓"）。
const char* lexer_keyword_text(LenoTokenType type);

#endif // LENO_LEXER_H
