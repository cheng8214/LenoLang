#include "parser_internal.h"

// ============================================================================
// return 语句解析
// ============================================================================

Ast* parse_return_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // return

    Ast* value = NULL;
    // 如果后面不是分号或右大括号，则解析返回值表达式
    if (p->lex.current.type != TOK_SEMI && p->lex.current.type != TOK_RBRACE) {
        value = parse_expression(p);
    }

    // 检测多值返回: return expr1, expr2, ...
    if (value && p->lex.current.type == TOK_COMMA) {
        int count = 1;
        int capacity = 4;
        Ast** exprs = (Ast**)malloc(sizeof(Ast*) * capacity);
        exprs[0] = value;

        while (match(p, TOK_COMMA)) {
            Ast* next_expr = parse_expression(p);
            if (count >= capacity) {
                capacity *= 2;
                exprs = (Ast**)realloc(exprs, sizeof(Ast*) * capacity);
            }
            exprs[count++] = next_expr;
        }

        Ast* ast = ast_new(AST_RETURN_MULTI, line);
        ast->u.ret_multi.exprs = exprs;
        ast->u.ret_multi.count = count;
        return ast;
    }

    Ast* ast = ast_new(AST_RETURN, line);
    ast->u.ret = value;
    return ast;
}

// ============================================================================
// break 语句解析
// ============================================================================

Ast* parse_break_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // break
    return ast_new(AST_BREAK, line);
}

// ============================================================================
// continue 语句解析
// ============================================================================

Ast* parse_continue_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // continue
    return ast_new(AST_CONTINUE, line);
}

// ============================================================================
// throw 语句解析
// ============================================================================

Ast* parse_throw_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // throw
    
    Ast* expr = NULL;
    // 如果后面不是分号或右大括号，则解析抛出表达式
    if (p->lex.current.type != TOK_SEMI && p->lex.current.type != TOK_RBRACE) {
        expr = parse_expression(p);
    }
    
    Ast* ast = ast_new(AST_THROW, line);
    ast->u.throw_.expr = expr;
    return ast;
}

// ============================================================================
// try-catch-finally 语句解析
// ============================================================================

Ast* parse_try_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // try
    
    // try 体必须用大括号包裹
    if (p->lex.current.type != TOK_LBRACE) {
        error_add_at(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "try 语句体必须用大括号 {} 包裹");
        return NULL;
    }
    
    // 解析 try 块
    Ast* try_body = parse_block_internal(p);
    
    char* catch_var = NULL;
    Ast* catch_body = NULL;
    Ast* finally_body = NULL;
    
    // 可选的 catch 子句
    if (p->lex.current.type == TOK_CATCH) {
        lexer_next(&p->lex); // catch
        
        // 可选的 catch 变量（支持 catch e 或 catch(e)）
        int has_paren = 0;
        if (p->lex.current.type == TOK_LPAREN) {
            has_paren = 1;
            lexer_next(&p->lex); // (
        }
        
        // 解析 catch 变量名
        if (p->lex.current.type == TOK_IDENT) {
            catch_var = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);
        }
        
        // 如果有左括号，需要匹配右括号
        if (has_paren) {
            if (p->lex.current.type != TOK_RPAREN) {
                error_add_at(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "catch 变量后缺少右括号 )");
                free(catch_var);
                return NULL;
            }
            lexer_next(&p->lex); // )
        }
        
        // catch 体必须用大括号包裹
        if (p->lex.current.type != TOK_LBRACE) {
            error_add_at(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "catch 语句体必须用大括号 {} 包裹");
            free(catch_var);
            return NULL;
        }
        catch_body = parse_block_internal(p);
    }
    
    // 可选的 finally 子句
    if (p->lex.current.type == TOK_FINALLY) {
        lexer_next(&p->lex); // finally
        
        // finally 体必须用大括号包裹
        if (p->lex.current.type != TOK_LBRACE) {
            error_add_at(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "finally 语句体必须用大括号 {} 包裹");
            free(catch_var);
            return NULL;
        }
        finally_body = parse_block_internal(p);
    }
    
    // 必须至少有一个 catch 或 finally
    if (!catch_body && !finally_body) {
        error_add_at(ERR_SYNTAX, line, p->lex.current.column, "try 语句必须至少有一个 catch 或 finally 子句");
        return NULL;
    }
    
    // 创建 try-catch-finally AST 节点
    Ast* ast = ast_new(AST_TRY, line);
    ast->u.try_.try_body = try_body;
    ast->u.try_.catch_var = catch_var;
    ast->u.try_.catch_body = catch_body;
    ast->u.try_.finally_body = finally_body;
    return ast;
}
