#include "parser_internal.h"

// ============================================================================
// defer 表达式解析（2026-10-22）
// ----------------------------------------------------------------------------
//   defer ffi.free(p)
//
// 语义：**所在代码块退出时执行**（正常结束 / return / break / continue / throw 都算）。
// 实现：本函数只产出一个 AST_DEFER 中间节点；parse_block_internal 把它**收集**起来，
//   在块末尾按**逆序**降级成 `try { 原块 } finally { 逆序表达式 }` ——
//   finally 的语义已被 try/finally 完整覆盖，所以 semantic / codegen **零改动**。
//
// 为什么是块级而不是 Go 的函数级：块级不会在循环里累积（Go 那种写法在
//   `for ... { defer f() }` 里会把内存拖爆）。FFI 包装函数两种语义结果一致，
//   但循环场景差很多 ⇒ 选块级。
//
// ⚠ 限制：defer 的表达式在**块退出时求值**（不是注册时）。所以
//   `defer ffi.free(p)` 里的 p 如果在中间被重新赋值，释放的是**新值**。
//   要"注册时锁定参数"就显式存一个局部副本。
// ============================================================================

Ast* parse_defer_stmt(Parser* p) {
    int line = p->lex.current.line;
    int col = p->lex.current.column;
    lexer_next(&p->lex);   // 消费 defer

    Ast* expr = parse_expression(p);
    if (!expr) {
        error_add_at_fmt(ERR_SYNTAX, line, col,
                         "defer 后面需要一个表达式（如 defer ffi.free(p)）");
        return NULL;
    }

    Ast* ast = ast_new(AST_DEFER, line);
    ast->column = col;
    ast->u.defer_.expr = expr;
    ast->u.defer_.line = line;
    return ast;
}

// ============================================================================
// return 语句解析
// ============================================================================

Ast* parse_return_stmt(Parser* p) {
    int line = p->lex.current.line;
    int col = p->lex.current.column;
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
        ast->column = col;
        ast->u.ret_multi.exprs = exprs;
        ast->u.ret_multi.count = count;
        return ast;
    }

    Ast* ast = ast_new(AST_RETURN, line);
    ast->column = col;
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
        error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "try 语句体必须用大括号 {} 包裹");
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
                error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "catch 变量后缺少右括号 )");
                free(catch_var);
                return NULL;
            }
            lexer_next(&p->lex); // )
        }
        
        // catch 体必须用大括号包裹
        if (p->lex.current.type != TOK_LBRACE) {
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "catch 语句体必须用大括号 {} 包裹");
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
            error_add_at_fmt(ERR_SYNTAX, p->lex.current.line, p->lex.current.column, "finally 语句体必须用大括号 {} 包裹");
            free(catch_var);
            return NULL;
        }
        finally_body = parse_block_internal(p);
    }
    
    // 必须至少有一个 catch 或 finally
    if (!catch_body && !finally_body) {
        error_add_at_fmt(ERR_SYNTAX, line, p->lex.current.column, "try 语句必须至少有一个 catch 或 finally 子句");
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
