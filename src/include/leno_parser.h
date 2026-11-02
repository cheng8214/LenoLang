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
    // 类型别名表
    char** alias_names;
    TypeInfo** alias_types;
    char*** alias_type_params;  // 每个别名的泛型参数名数组
    int* alias_type_param_counts; // 每个别名的泛型参数数量
    int alias_count;
    int alias_capacity;
    // 箭头函数（C2）的"投机解析失败原因"，只用于把报错指向真因：
    //   0 = 没有；1 = 写了块体 `x => { … }`；2 = 写了返回类型 `(int x): int => …`
    // 为什么要这个字段：`(…)` 的参数表是**静默投机**解析的（失败就回退成普通括号表达式），
    //   期间不能报错；只能先记下"为什么失败"，回到 parse_grouping 再给定向提示。
    int arrow_hint;
} Parser;

void parser_init(Parser* p, const char* src);
void parser_free(Parser* p);
int parser_parse(Parser* p);

// 把一段**独立表达式文本**按语言真语法求值（实现见 parser_func.c）。
// 符号表扫描器靠它复用同一套词法/语法/求值，不再自己复刻一份求值器（见 docs 的 Phase 1）。
//   text: NUL 结尾的稳定缓冲区；names/values/count: 先前 enum 成员引用上下文（可为空）
//   成功返回 1 并写 *out；失败返回 0（*out 不动）且**不产生任何诊断**
int parser_eval_const_expr_text(const char* text, char** names, int64_t* values,
                                int count, int64_t* out);

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
