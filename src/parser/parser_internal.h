#ifndef PARSER_INTERNAL_H
#define PARSER_INTERNAL_H

#include "include/lenolang.h"
#include "include/leno_parser.h"
#include "include/native.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

// ============================================================================
// Pratt 优先级表
// ============================================================================

typedef enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT,  // =
    PREC_OR,          // or
    PREC_AND,         // and
    PREC_BITOR,       // |
    PREC_BITXOR,      // ^
    PREC_BITAND,      // &
    PREC_EQUALITY,    // == !=
    PREC_IN,          // in, not in
    PREC_SHIFT,       // << >>
    PREC_COMPARISON,  // < > <= >=
    PREC_TERM,        // + -
    PREC_FACTOR,      // * / %
    PREC_UNARY,       // not, !, -, ~, ++, -- (前缀)
    PREC_POSTFIX,     // ++ -- (后缀)
    PREC_CALL,        // () .
    PREC_PRIMARY
} Precedence;

typedef Ast* (*ParsePrefixFn)(Parser* p);
typedef Ast* (*ParseInfixFn)(Parser* p, Ast* left);

typedef struct {
    ParsePrefixFn prefix;
    ParseInfixFn infix;
    Precedence precedence;
} ParseRule;

// ============================================================================
// 全局规则表（在 parser_expr.c 中定义）
// ============================================================================

extern ParseRule* get_rule(LenoTokenType type);

// ============================================================================
// 工具函数声明（在 parser_utils.c 中实现）
// ============================================================================

// 辅助函数
int match(Parser* p, LenoTokenType type);
int consume(Parser* p, LenoTokenType type, const char* msg);
char* process_escape_sequences(const char* text, int len, int* out_len);
char* copy_string(const char* text, int len);
char* process_raw_string(const char* text, int len, int* out_len);
int is_type_keyword(LenoTokenType type);

// struct 前向引用支持（在 parser.c 中实现）
int is_struct_name(Parser* p, const char* name);

// ============================================================================
// 表达式解析函数声明（在 parser_expr.c 中实现）
// ============================================================================

Ast* parse_expression(Parser* p);
Ast* parse_call_expression(Parser* p);  // 解析调用级别的表达式（不包括赋值）
Ast* parse_precedence(Parser* p, Precedence precedence);

// 前缀表达式解析器
Ast* parse_number(Parser* p);
Ast* parse_string(Parser* p);
Ast* parse_raw_string(Parser* p);
Ast* parse_interp_string(Parser* p);
Ast* parse_literal(Parser* p);
Ast* parse_identifier_expr(Parser* p);
Ast* parse_grouping(Parser* p);
Ast* parse_array(Parser* p);
Ast* parse_dict(Parser* p);
Ast* parse_if_expr(Parser* p);
Ast* parse_unary(Parser* p);
Ast* parse_prefix_inc_dec(Parser* p);

// 中缀表达式解析器
Ast* parse_binary(Parser* p, Ast* left);
Ast* parse_call(Parser* p, Ast* callee);
Ast* parse_struct_init(Parser* p, const char* struct_name);
Ast* parse_index(Parser* p, Ast* obj);
Ast* parse_dot(Parser* p, Ast* left);
Ast* parse_assignment(Parser* p, Ast* left);
Ast* parse_postfix_inc_dec(Parser* p, Ast* left);
Ast* parse_type_check(Parser* p, Ast* left);
Ast* parse_as_cast(Parser* p, Ast* left);

// 协程表达式解析器
Ast* parse_await(Parser* p);

// ============================================================================
// 语句解析函数声明（在 parser_stmt*.c 中实现）
// ============================================================================

Ast* parse_statement(Parser* p);
Ast* parse_block_internal(Parser* p);

// 控制流语句
Ast* parse_if_stmt(Parser* p);
Ast* parse_while_stmt(Parser* p);
Ast* parse_for_stmt(Parser* p);
Ast* parse_switch_stmt(Parser* p);

// 其他语句
Ast* parse_return_stmt(Parser* p);
Ast* parse_break_stmt(Parser* p);
Ast* parse_continue_stmt(Parser* p);
Ast* parse_throw_stmt(Parser* p);
Ast* parse_try_stmt(Parser* p);

// ============================================================================
// 函数和变量声明（在 parser_func.c 中实现）
// ============================================================================

Ast* parse_var_decl_internal(Parser* p);
Ast* parse_func_stmt(Parser* p);
Ast* parse_entry_func_stmt(Parser* p);
Ast* parse_anonymous_func(Parser* p);
Ast* parse_func_body_and_create(Parser* p, char* name, int line);
Ast* parse_expression_stmt(Parser* p);
TypeInfo* parse_type(Parser* p);
int is_entry_function_def(Parser* p);
Ast* parse_struct_stmt(Parser* p);
Ast* parse_face_stmt(Parser* p);
Ast* parse_cstruct_stmt(Parser* p);
Ast* parse_enum_stmt(Parser* p);

// ============================================================================
// 模块导入导出（在 parser_module.c 中实现）
// ============================================================================

Ast* parse_import_stmt(Parser* p);
Ast* parse_export_stmt(Parser* p);
Ast* parse_use_stmt(Parser* p);

// ============================================================================
// 主解析逻辑（在 parser.c 中实现）
// ============================================================================

Ast* parse_program_internal(Parser* p);

#endif // PARSER_INTERNAL_H
