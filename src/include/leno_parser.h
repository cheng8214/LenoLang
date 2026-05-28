#ifndef LENO_PARSER_H
#define LENO_PARSER_H

#include "leno_types.h"
#include "leno_lexer.h"
#include "leno_ast.h"
#include "leno_vm.h"

// ============================================================================
// Parser 递归下降 + Pratt 表达式
// ============================================================================

typedef struct {
    Lexer lex;
    Ast* root;
    Scope* current_scope;
    // 预扫描收集的 struct 名称，用于支持前向引用
    char** struct_names;
    int struct_count;
    int struct_capacity;
} Parser;

void parser_init(Parser* p, const char* src);
int parser_parse(Parser* p);

// 语句（递归下降）
Ast* parse_program(Parser* p);
Ast* parse_stmt(Parser* p);
Ast* parse_block(Parser* p);
Ast* parse_if(Parser* p);
Ast* parse_while(Parser* p);
Ast* parse_func(Parser* p);
Ast* parse_return(Parser* p);
Ast* parse_var_decl(Parser* p);
Ast* parse_assign_or_call(Parser* p);
Ast* parse_expr_stmt(Parser* p);

// 表达式（Pratt）
Ast* parse_expr(Parser* p);
Ast* parse_pratt(Parser* p, int precedence);
int get_prefix_precedence(LenoTokenType type);
int get_infix_precedence(LenoTokenType type);

#endif // LENO_PARSER_H
