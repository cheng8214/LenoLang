#include "parser_internal.h"

// ============================================================================
// 前向声明
// ============================================================================

static Ast* parse_anon_func(Parser* p);
static Ast* parse_new(Parser* p);

// ============================================================================
// Pratt 规则表 - 定义每个 token 的前缀/中缀解析器和优先级
// ============================================================================

static ParseRule rules[] = {
    [TOK_LPAREN]    = {parse_grouping, parse_call,   PREC_CALL},
    [TOK_RPAREN]    = {NULL,     NULL,   PREC_NONE},
    [TOK_LBRACE]    = {parse_dict,     NULL,   PREC_NONE},
    [TOK_RBRACE]    = {NULL,     NULL,   PREC_NONE},
    [TOK_LBRACKET]  = {parse_array,    parse_index,   PREC_CALL},
    [TOK_RBRACKET]  = {NULL,     NULL,   PREC_NONE},
    [TOK_DOT]       = {NULL,     parse_dot,   PREC_CALL},
    [TOK_COMMA]     = {NULL,     NULL,   PREC_NONE},
    [TOK_COLON]     = {NULL,     NULL,   PREC_NONE},
    [TOK_MINUS]     = {parse_unary,    parse_binary, PREC_TERM},
    [TOK_PLUS]      = {NULL,     parse_binary, PREC_TERM},
    [TOK_INC]       = {parse_prefix_inc_dec, parse_postfix_inc_dec, PREC_POSTFIX},
    [TOK_DEC]       = {parse_prefix_inc_dec, parse_postfix_inc_dec, PREC_POSTFIX},
    [TOK_PLUSEQ]    = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_MINUSEQ]   = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_STAREQ]    = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_SLASHEQ]   = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_MODEQ]     = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_BITANDEQ]  = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_BITOREQ]   = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_BITXOREQ]  = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_SHLEQ]     = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_SHREQ]     = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_USHREQ]    = {NULL,     parse_assignment, PREC_ASSIGNMENT},  // >>>=
    [TOK_SEMI]      = {NULL,     NULL,   PREC_NONE},
    [TOK_SLASH]     = {NULL,     parse_binary, PREC_FACTOR},
    [TOK_STAR]      = {NULL,     parse_binary, PREC_FACTOR},
    [TOK_MOD]       = {NULL,     parse_binary, PREC_FACTOR},
    [TOK_BITAND]    = {NULL,     parse_binary, PREC_BITAND},
    [TOK_BITOR]     = {NULL,     parse_binary, PREC_BITOR},
    [TOK_BITXOR]    = {NULL,     parse_binary, PREC_BITXOR},
    [TOK_BITNOT]    = {parse_unary, NULL,       PREC_NONE},
    [TOK_SHL]       = {NULL,     parse_binary, PREC_SHIFT},
    [TOK_SHR]       = {NULL,     parse_binary, PREC_SHIFT},
    [TOK_USHR]      = {NULL,     parse_binary, PREC_SHIFT},  // >>>
    [TOK_EQ]        = {NULL,     parse_assignment, PREC_ASSIGNMENT},
    [TOK_EQEQ]      = {NULL,     parse_binary, PREC_EQUALITY},
    [TOK_NEQ]       = {NULL,     parse_binary, PREC_EQUALITY},
    [TOK_LT]        = {NULL,     parse_binary, PREC_COMPARISON},
    [TOK_GT]        = {NULL,     parse_binary, PREC_COMPARISON},
    [TOK_LE]        = {NULL,     parse_binary, PREC_COMPARISON},
    [TOK_GE]        = {NULL,     parse_binary, PREC_COMPARISON},
    [TOK_IDENT]     = {parse_identifier_expr, parse_call,   PREC_CALL},
    [TOK_STRING]        = {parse_string,        NULL,   PREC_NONE},
    [TOK_INTERP_STRING] = {parse_interp_string, NULL,   PREC_NONE},
    [TOK_RAW_STRING]    = {parse_raw_string,    NULL,   PREC_NONE},
    [TOK_NUM]           = {parse_number,        NULL,   PREC_NONE},
    [TOK_AND]       = {NULL,     parse_binary, PREC_AND},
    [TOK_OR]        = {NULL,     parse_binary, PREC_OR},
    [TOK_NOT]       = {parse_unary,    NULL,   PREC_NONE},
    [TOK_AWAIT]     = {parse_await,    NULL,   PREC_NONE},
    [TOK_IS]        = {NULL,     parse_type_check, PREC_IN},
    [TOK_AS]        = {NULL,     parse_as_cast,    PREC_AS},
    [TOK_IN]        = {NULL,     parse_binary, PREC_IN},
    [TOK_NOT_IN]    = {NULL,     parse_binary, PREC_IN},
    [TOK_TRUE]      = {parse_literal,  NULL,   PREC_NONE},
    [TOK_FALSE]     = {parse_literal,  NULL,   PREC_NONE},
    [TOK_NULL]      = {parse_literal,  NULL,   PREC_NONE},
    [TOK_IF]        = {parse_if_expr,    NULL,   PREC_NONE},  // if 表达式
    [TOK_FUNC]      = {parse_anon_func,  NULL,   PREC_NONE},  // 匿名函数表达式
    [TOK_NEW]       = {parse_new,        NULL,   PREC_NONE},  // new StructName() struct 实例化
    [TOK_EOF]       = {NULL,     NULL,   PREC_NONE},
    // 类型关键字 - 只在语句解析中使用，不在表达式解析中使用
    [TOK_INT_TYPE]       = {NULL,     NULL,   PREC_NONE},
    [TOK_FLOAT_TYPE]     = {NULL,     NULL,   PREC_NONE},
    [TOK_STRING_TYPE] = {NULL,   NULL,   PREC_NONE},
    [TOK_BOOL_TYPE] = {NULL,     NULL,   PREC_NONE},
    [TOK_ARRAY_TYPE] = {NULL,    NULL,   PREC_NONE},
    [TOK_DICT_TYPE] = {NULL,     NULL,   PREC_NONE},
    // [TOK_BINT]      = {NULL,     NULL,   PREC_NONE},  // 已移除
    // 错误 token - 用于错误恢复
    [TOK_ERROR]     = {NULL,     NULL,   PREC_NONE},
};

// 获取指定 token 类型的解析规则
ParseRule* get_rule(LenoTokenType type) {
    return &rules[type];
}

// ============================================================================
// 核心表达式解析 - Pratt 解析器
// ============================================================================

// 表达式解析入口
Ast* parse_expression(Parser* p) {
    return parse_precedence(p, PREC_ASSIGNMENT);
}

// 解析调用级别的表达式（用于赋值左侧目标，不包括赋值运算符）
Ast* parse_call_expression(Parser* p) {
    return parse_precedence(p, PREC_CALL);
}

// Pratt 表达式解析核心函数
Ast* parse_precedence(Parser* p, Precedence precedence) {
    LenoTokenType type = p->lex.current.type;
    ParseRule* rule = get_rule(type);
    
    // 没有前缀解析器，报错并消费错误 token 后返回 NULL
    // 消费 token 确保高级别错误恢复不会重复遇到同一个无法解析的 token
    if (!rule->prefix) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望表达式");
        if (p->lex.current.type != TOK_EOF) {
            lexer_next(&p->lex);
        }
        return NULL;
    }
    
    // 解析前缀表达式
    Ast* left = rule->prefix(p);
    
    // 处理中缀表达式（根据优先级）
    while (precedence <= get_rule(p->lex.current.type)->precedence) {
        type = p->lex.current.type;
        rule = get_rule(type);
        // 如果没有中缀解析器，退出循环
        if (!rule->infix) break;
        // 注意：infix 解析器会自己消费操作符
        Ast* new_left = rule->infix(p, left);
        // 如果 infix 解析器没有消费任何 token（返回原 left），则退出循环
        if (new_left == left) break;
        left = new_left;
    }
    
    return left;
}

// ============================================================================
// 前缀表达式解析器
// ============================================================================

// 解析数字字面量
Ast* parse_number(Parser* p) {
    Ast* ast = ast_new(AST_NUM, p->lex.current.line);
    ast->u.num.value = p->lex.current.num_val;
    ast->u.num.is_bigint = p->lex.current.is_bigint;
    ast->u.num.is_float = p->lex.current.is_float;
    if (p->lex.current.is_bigint && p->lex.current.bigint_str) {
        ast->u.num.bigint_str = copy_string(p->lex.current.bigint_str, strlen(p->lex.current.bigint_str));
    } else {
        ast->u.num.bigint_str = NULL;
    }
    lexer_next(&p->lex);
    return ast;
}

// 解析字符串字面量
Ast* parse_string(Parser* p) {
    Ast* ast = ast_new(AST_STRING, p->lex.current.line);
    int processed_len;
    ast->u.string.value = process_escape_sequences(p->lex.current.text, p->lex.current.len, &processed_len);
    ast->u.string.len = processed_len;
    lexer_next(&p->lex);
    return ast;
}

// 解析原始字符串字面量（不处理转义字符，但处理 "" 转义）
Ast* parse_raw_string(Parser* p) {
    Ast* ast = ast_new(AST_STRING, p->lex.current.line);
    // 原始字符串处理 "" 转义
    int raw_len;
    ast->u.string.value = process_raw_string(p->lex.current.text, p->lex.current.len, &raw_len);
    ast->u.string.len = raw_len;
    lexer_next(&p->lex);
    return ast;
}

// 解析插值字符串: $"hello {name} world {a + b}"
Ast* parse_interp_string(Parser* p) {
    int line = p->lex.current.line;
    Ast* ast = ast_new(AST_INTERP_STRING, line);

    // 初始化数组
    ast->u.interp_string.parts = NULL;
    ast->u.interp_string.exprs = NULL;
    ast->u.interp_string.count = 0;

    int capacity = 4;
    ast->u.interp_string.parts = (char**)malloc(sizeof(char*) * capacity);
    ast->u.interp_string.exprs = (Ast**)malloc(sizeof(Ast*) * capacity);

    bool has_content = false;
    int idx = 0;

    // 消费 TOK_INTERP_STRING 标记
    lexer_next(&p->lex);

    while (true) {
        // 读取字符串片段
        if (p->lex.current.type == TOK_INTERP_PART) {
            int processed_len;
            char* text = process_escape_sequences(p->lex.current.text, p->lex.current.len, &processed_len);

            if (processed_len > 0) {
                // 扩展数组
                if (idx >= capacity) {
                    capacity *= 2;
                    ast->u.interp_string.parts = (char**)realloc(ast->u.interp_string.parts, sizeof(char*) * capacity);
                    ast->u.interp_string.exprs = (Ast**)realloc(ast->u.interp_string.exprs, sizeof(Ast*) * capacity);
                }
                ast->u.interp_string.parts[idx] = text;
                has_content = true;
            } else {
                free(text);
            }
            lexer_next(&p->lex);
        }

        // 检查是否是表达式开始
        if (p->lex.current.type == TOK_LBRACE) {
            lexer_next(&p->lex); // 消费 {

            // 解析表达式
            Ast* expr = parse_expression(p);

            // 期望 }
            if (p->lex.current.type == TOK_RBRACE) {
                lexer_next(&p->lex); // 消费 }
            } else {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望 '}'");
                break;
            }

            // 扩展数组
            if (idx >= capacity) {
                capacity *= 2;
                ast->u.interp_string.parts = (char**)realloc(ast->u.interp_string.parts, sizeof(char*) * capacity);
                ast->u.interp_string.exprs = (Ast**)realloc(ast->u.interp_string.exprs, sizeof(Ast*) * capacity);
            }

            // 如果没有前置字符串片段，添加空字符串
            if (!has_content) {
                ast->u.interp_string.parts[idx] = strdup("");
                has_content = true;
            }

            ast->u.interp_string.exprs[idx] = expr;
            idx++;
            has_content = false; // 重置，等待下一个片段
        } else {
            break;
        }
    }

    // 消费结束标记
    if (p->lex.current.type == TOK_INTERP_END) {
        lexer_next(&p->lex);
    }

    // 如果没有内容，添加空字符串
    if (!has_content && idx == 0) {
        ast->u.interp_string.parts[0] = strdup("");
        ast->u.interp_string.count = 1;
    } else {
        // 如果最后一个表达式后没有字符串片段，添加空字符串
        if (!has_content) {
            if (idx >= capacity) {
                capacity *= 2;
                ast->u.interp_string.parts = (char**)realloc(ast->u.interp_string.parts, sizeof(char*) * capacity);
            }
            ast->u.interp_string.parts[idx] = strdup("");
        }
        ast->u.interp_string.count = idx + 1;
    }

    return ast;
}

// 解析布尔和 null 字面量
Ast* parse_literal(Parser* p) {
    LenoTokenType type = p->lex.current.type;
    Ast* ast = NULL;
    
    if (type == TOK_TRUE) {
        ast = ast_new(AST_BOOL, p->lex.current.line);
        ast->u.boolean = 1;
    } else if (type == TOK_FALSE) {
        ast = ast_new(AST_BOOL, p->lex.current.line);
        ast->u.boolean = 0;
    } else if (type == TOK_NULL) {
        ast = ast_new(AST_NULL, p->lex.current.line);
    }
    
    lexer_next(&p->lex);
    return ast;
}

// 解析标识符表达式（变量引用）
Ast* parse_identifier_expr(Parser* p) {
    Ast* ast = ast_new(AST_VAR, p->lex.current.line);
    ast->u.var.name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);
    return ast;
}

// 解析括号分组表达式 (expr)
Ast* parse_grouping(Parser* p) {
    lexer_next(&p->lex);
    Ast* expr = parse_expression(p);
    consume(p, TOK_RPAREN, "期望 ')'");
    return expr;
}

// 解析数组字面量 [1, 2, 3]
Ast* parse_array(Parser* p) {
    int line = p->lex.current.line;
    Ast* ast = ast_new(AST_ARRAY, line);
    ast_list_init(&ast->u.array);
    
    lexer_next(&p->lex); // 消费 '['
    
    // 空数组 []
    if (p->lex.current.type == TOK_RBRACKET) {
        lexer_next(&p->lex);
        return ast;
    }
    
    // 解析数组元素
    while (1) {
        Ast* element = parse_expression(p);
        if (element) {
            ast_list_add(&ast->u.array, element);
        }
        
        if (p->lex.current.type == TOK_RBRACKET) {
            lexer_next(&p->lex);
            break;
        }
        
        if (!consume(p, TOK_COMMA, "期望 ',' 或 ']'")) {
            break;
        }
    }
    
    return ast;
}

// 解析字典字面量 {"key": value, "key2": value2}
Ast* parse_dict(Parser* p) {
    int line = p->lex.current.line;
    Ast* ast = ast_new(AST_DICT, line);
    // 初始化字典条目列表
    ast->u.dict.entries = NULL;
    ast->u.dict.count = 0;
    ast->u.dict.capacity = 0;
    
    lexer_next(&p->lex); // 消费 '{'
    
    // 空字典 {}
    if (p->lex.current.type == TOK_RBRACE) {
        lexer_next(&p->lex);
        return ast;
    }
    
    // 解析字典键值对
    while (1) {
        // 键: 字符串、标识符(转为字符串)、整数
        Ast* key_ast = NULL;
        if (p->lex.current.type == TOK_STRING) {
            int key_len;
            char* key_str = process_escape_sequences(p->lex.current.text, p->lex.current.len, &key_len);
            key_ast = ast_new(AST_STRING, line);
            key_ast->u.string.value = key_str;
            key_ast->u.string.len = key_len;
            lexer_next(&p->lex);
        } else if (p->lex.current.type == TOK_IDENT) {
            // 标识符键转为字符串键（向后兼容 {name: value} 语法）
            char* key_str = copy_string(p->lex.current.text, p->lex.current.len);
            key_ast = ast_new(AST_STRING, line);
            key_ast->u.string.value = key_str;
            key_ast->u.string.len = (int)strlen(key_str);
            lexer_next(&p->lex);
        } else if (p->lex.current.type == TOK_NUM) {
            // 整数键（通过 parse_number 解析，自动处理 int/float/bigint）
            key_ast = parse_number(p);
        } else {
            if (is_type_keyword(p->lex.current.type)) {
                char msg[64];
                snprintf(msg, sizeof(msg), "不能使用关键字(%.*s)作为字典键", 
                         p->lex.current.len, p->lex.current.text);
                error_add(ERR_SYNTAX, p->lex.current.line, msg);
            } else {
                error_add(ERR_SYNTAX, p->lex.current.line, "字典键必须是字符串、标识符或整数");
            }
            break;
        }
        
        // 期望 ':'
        if (!consume(p, TOK_COLON, "期望 ':'")) {
            ast_free(key_ast);
            break;
        }
        
        // 解析值
        Ast* value = parse_expression(p);
        
        // 添加到字典条目列表
        if (ast->u.dict.count >= ast->u.dict.capacity) {
            int new_capacity = ast->u.dict.capacity == 0 ? 8 : ast->u.dict.capacity * 2;
            DictEntry* new_entries = (DictEntry*)realloc(ast->u.dict.entries, sizeof(DictEntry) * new_capacity);
            if (!new_entries) {
                ast_free(key_ast);
                error_add(ERR_RUNTIME, line, "内存分配失败");
                break;
            }
            ast->u.dict.entries = new_entries;
            ast->u.dict.capacity = new_capacity;
        }
        ast->u.dict.entries[ast->u.dict.count].key = key_ast;
        ast->u.dict.entries[ast->u.dict.count].value = value;
        ast->u.dict.count++;
        
        if (p->lex.current.type == TOK_RBRACE) {
            lexer_next(&p->lex);
            break;
        }
        
        if (!consume(p, TOK_COMMA, "期望 ',' 或 '}'")) {
            break;
        }
    }
    
    return ast;
}

// 解析 if 表达式: if condition then expr1 else expr2
// 也支持 eif 链: if cond then val eif cond2 then val2 else val3
Ast* parse_if_expr(Parser* p) {
    int line = p->lex.current.line;
    // 支持 if 和 eif 关键字开头
    if (p->lex.current.type == TOK_IF || p->lex.current.type == TOK_EIF) {
        lexer_next(&p->lex); // 消费 if/eif
    } else {
        error_add(ERR_SYNTAX, line, "期望 if 或 eif 关键字");
        return NULL;
    }

    // 括号是可选的
    int has_paren = (p->lex.current.type == TOK_LPAREN);
    if (has_paren) {
        lexer_next(&p->lex); // 消费 '('
    }

    // 解析条件表达式
    Ast* cond = parse_expression(p);

    if (has_paren) {
        consume(p, TOK_RPAREN, "期望 ')'");
    }

    // 表达式 if 必须有 then
    if (!consume(p, TOK_THEN, "if 表达式需要使用 'then' 关键字")) {
        return NULL;
    }

    // 解析 then 分支表达式（不是代码块）
    Ast* then_expr = parse_expression(p);

    // 检查是否有 eif（else if 链）或 else
    if (p->lex.current.type == TOK_EIF) {
        // eif 链：递归解析下一个 if 表达式作为 else 分支
        Ast* else_branch = parse_if_expr(p);  // 递归调用

        // 创建 AST 节点
        Ast* ast = ast_new(AST_IF, line);
        ast->u.if_.cond = cond;
        ast->u.if_.then = then_expr;
        ast->u.if_.else_ = else_branch;
        ast->u.if_.guard_var = NULL;
        ast->u.if_.guard_type = NULL;
        type_guard_list_init(&ast->u.if_.guard_conds);

        return ast;
    }

    // 表达式 if 必须有 else
    if (!consume(p, TOK_ELSE, "if 表达式需要使用 'else' 关键字")) {
        return NULL;
    }

    // 解析 else 分支表达式
    Ast* else_expr = parse_expression(p);

    // 创建 AST 节点 - 复用 AST_IF，但 then 和 else 是表达式而不是代码块
    Ast* ast = ast_new(AST_IF, line);
    ast->u.if_.cond = cond;
    ast->u.if_.then = then_expr;
    ast->u.if_.else_ = else_expr;
    ast->u.if_.guard_var = NULL;
    ast->u.if_.guard_type = NULL;
    type_guard_list_init(&ast->u.if_.guard_conds);

    return ast;
}

// 解析一元表达式（前缀）
Ast* parse_unary(Parser* p) {
    LenoTokenType op = p->lex.current.type;
    int line = p->lex.current.line;
    lexer_next(&p->lex);

    Ast* operand = parse_precedence(p, PREC_UNARY);

    Ast* ast = ast_new(AST_UNARY, line);
    ast->u.unary.op = op;
    ast->u.unary.operand = operand;
    return ast;
}

// 前缀 ++ 和 -- 解析器
Ast* parse_prefix_inc_dec(Parser* p) {
    LenoTokenType op = p->lex.current.type;
    int line = p->lex.current.line;
    lexer_next(&p->lex);

    // 解析操作数（必须是变量）
    Ast* operand = parse_precedence(p, PREC_UNARY);

    Ast* ast = ast_new(AST_UNARY, line);
    ast->u.unary.op = op;
    ast->u.unary.operand = operand;
    ast->u.unary.is_postfix = 0;  // 前缀形式
    return ast;
}

// ============================================================================
// 中缀表达式解析器
// ============================================================================

// 解析二元表达式
Ast* parse_binary(Parser* p, Ast* left) {
    LenoTokenType op = p->lex.current.type;
    int line = p->lex.current.line;
    
    ParseRule* rule = get_rule(op);
    lexer_next(&p->lex);
    
    Ast* right = parse_precedence(p, (Precedence)(rule->precedence + 1));
    
    // in / not in 右侧支持范围表达式: a in 0:9
    // 当 ':' 不是通用中缀操作符时，范围表达式需要在这里特殊处理
    if ((op == TOK_IN || op == TOK_NOT_IN) && p->lex.current.type == TOK_COLON) {
        int range_line = p->lex.current.line;
        lexer_next(&p->lex); // 消费 ':'
        Ast* end = parse_precedence(p, PREC_TERM);
        Ast* range = ast_new(AST_RANGE, range_line);
        range->u.range.start = right;
        range->u.range.end = end;
        range->u.range.inclusive = 1;
        right = range;
    }
    
    Ast* ast = ast_new(AST_BINOP, line);
    ast->u.binop.l = left;
    ast->u.binop.r = right;
    ast->u.binop.op = op;
    return ast;
}

// 解析函数调用表达式 func(arg1, arg2)
Ast* parse_call(Parser* p, Ast* callee) {
    // 检查当前 token 是否是 '('
    if (p->lex.current.type != TOK_LPAREN) {
        return callee;  // 不是函数调用，返回原表达式
    }

    int line = p->lex.current.line;

    // 普通函数调用
    Ast* ast = ast_new(AST_CALL, line);
    ast->u.call.callee = callee;
    ast_list_init(&ast->u.call.args);
    
    lexer_next(&p->lex); // 消费 '('
    
    if (p->lex.current.type != TOK_RPAREN) {
        do {
            Ast* arg = parse_expression(p);
            ast_list_add(&ast->u.call.args, arg);
        } while (match(p, TOK_COMMA));
    }
    
    if (!consume(p, TOK_RPAREN, "期望 ')'")) {
        // 如果 consume 失败，继续尝试同步
        // 消费 token 直到找到 ')' 或 EOF
        while (p->lex.current.type != TOK_RPAREN && p->lex.current.type != TOK_EOF) {
            lexer_next(&p->lex);
        }
        // 如果找到 ')'，消费它
        if (p->lex.current.type == TOK_RPAREN) {
            lexer_next(&p->lex);
        }
    }
    return ast;
}

// 解析 new StructName() 或 new StructName(field=value, ...) 或 new module.StructName(...) struct 实例化
Ast* parse_new(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'new'

    // 期望 struct 名称（标识符）
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "new 后面期望 struct 名称");
        return ast_new(AST_NULL, line);
    }

    char* struct_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex); // 消费 struct 名称

    // 解析可选的泛型类型参数: new Box[int](value: 42)
    TypeInfo** generic_type_args = NULL;
    int generic_type_count = 0;
    if (p->lex.current.type == TOK_LBRACKET) {
        lexer_next(&p->lex);  // 跳过 '['
        int gt_capacity = 8;
        generic_type_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * gt_capacity);

        do {
            if (generic_type_count >= gt_capacity) {
                gt_capacity *= 2;
                generic_type_args = (TypeInfo**)realloc(generic_type_args, sizeof(TypeInfo*) * gt_capacity);
            }
            TypeInfo* type_arg = parse_type(p);
            if (!type_arg) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望类型参数");
                type_arg = type_new(TYPE_ANY);
            }
            generic_type_args[generic_type_count++] = type_arg;
        } while (match(p, TOK_COMMA));

        consume(p, TOK_RBRACKET, "期望 ']' 结束泛型参数列表");
    }

    // 支持 new module.StructName(...) 语法
    if (p->lex.current.type == TOK_DOT) {
        lexer_next(&p->lex); // 消费 '.'
        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望 struct 名称");
            free(struct_name);
            return ast_new(AST_NULL, line);
        }
        // struct_name 作为模块名，读取真正的 struct 名称
        char* real_struct_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex); // 消费 struct 名称

        // 期望 '('
        if (!consume(p, TOK_LPAREN, "new 后面期望 '('")) {
            free(struct_name);
            free(real_struct_name);
            return ast_new(AST_NULL, line);
        }

        // 创建 AST_STRUCT_INIT，struct_name 为 "module.StructName" 格式
        Ast* ast = ast_new(AST_STRUCT_INIT, line);
        // 拼接模块名和 struct 名
        int full_name_len = strlen(struct_name) + 1 + strlen(real_struct_name) + 1;
        char* full_name = (char*)malloc(full_name_len);
        snprintf(full_name, full_name_len, "%s.%s", struct_name, real_struct_name);
        ast->u.struct_init.struct_name = full_name;
        free(struct_name);
        free(real_struct_name);
        ast->u.struct_init.field_names = NULL;
        ast->u.struct_init.field_values = NULL;
        ast->u.struct_init.field_count = 0;
        ast->u.struct_init.generic_type_args = generic_type_args;
        ast->u.struct_init.generic_type_count = generic_type_count;

        int capacity = 8;
        ast->u.struct_init.field_names = (char**)malloc(sizeof(char*) * capacity);
        ast->u.struct_init.field_values = (Ast**)malloc(sizeof(Ast*) * capacity);

        if (p->lex.current.type != TOK_RPAREN) {
            do {
                if (p->lex.current.type != TOK_IDENT) {
                    error_add(ERR_SYNTAX, p->lex.current.line, "期望字段名");
                    break;
                }
                char* field_name = copy_string(p->lex.current.text, p->lex.current.len);
                lexer_next(&p->lex);
                if (!consume(p, TOK_EQ, "期望 '='")) {
                    free(field_name);
                    break;
                }
                Ast* field_value = parse_expression(p);
                if (ast->u.struct_init.field_count >= capacity) {
                    capacity *= 2;
                    ast->u.struct_init.field_names = (char**)realloc(ast->u.struct_init.field_names,
                                                                      sizeof(char*) * capacity);
                    ast->u.struct_init.field_values = (Ast**)realloc(ast->u.struct_init.field_values,
                                                                      sizeof(Ast*) * capacity);
                }
                ast->u.struct_init.field_names[ast->u.struct_init.field_count] = field_name;
                ast->u.struct_init.field_values[ast->u.struct_init.field_count] = field_value;
                ast->u.struct_init.field_count++;
            } while (match(p, TOK_COMMA));
        }

        consume(p, TOK_RPAREN, "期望 ')'");
        return ast;
    }

    // 普通 new StructName(...) 语法
    // 期望 '('
    if (!consume(p, TOK_LPAREN, "new 后面期望 '('")) {
        free(struct_name);
        return ast_new(AST_NULL, line);
    }

    // 复用 parse_struct_init 的逻辑：解析命名参数列表
    Ast* ast = ast_new(AST_STRUCT_INIT, line);
    ast->u.struct_init.struct_name = struct_name;
    ast->u.struct_init.field_names = NULL;
    ast->u.struct_init.field_values = NULL;
    ast->u.struct_init.field_count = 0;
    ast->u.struct_init.generic_type_args = generic_type_args;
    ast->u.struct_init.generic_type_count = generic_type_count;

    int capacity = 8;
    ast->u.struct_init.field_names = (char**)malloc(sizeof(char*) * capacity);
    ast->u.struct_init.field_values = (Ast**)malloc(sizeof(Ast*) * capacity);

    if (p->lex.current.type != TOK_RPAREN) {
        do {
            // 期望字段名
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望字段名");
                break;
            }

            char* field_name = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);

            // 期望 '='
            if (!consume(p, TOK_EQ, "期望 '='")) {
                free(field_name);
                break;
            }

            // 解析字段值
            Ast* field_value = parse_expression(p);

            // 扩容检查
            if (ast->u.struct_init.field_count >= capacity) {
                capacity *= 2;
                ast->u.struct_init.field_names = (char**)realloc(ast->u.struct_init.field_names,
                                                                  sizeof(char*) * capacity);
                ast->u.struct_init.field_values = (Ast**)realloc(ast->u.struct_init.field_values,
                                                                  sizeof(Ast*) * capacity);
            }

            ast->u.struct_init.field_names[ast->u.struct_init.field_count] = field_name;
            ast->u.struct_init.field_values[ast->u.struct_init.field_count] = field_value;
            ast->u.struct_init.field_count++;
        } while (match(p, TOK_COMMA));
    }

    consume(p, TOK_RPAREN, "期望 ')'");
    return ast;
}

// 解析索引表达式 arr[index] 或切片 arr[start:end]
Ast* parse_index(Parser* p, Ast* obj) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 '['
    
    // 检查是否是切片语法：arr[start:end]
    if (p->lex.current.type == TOK_COLON) {
        // 语法: arr[:end] - 从开头到end
        lexer_next(&p->lex); // 消费 ':'
        Ast* end = parse_expression(p);
        consume(p, TOK_RBRACKET, "期望 ']'");
        
        Ast* ast = ast_new(AST_SLICE, line);
        ast->u.slice.obj = obj;
        ast->u.slice.start = NULL; // 从开头开始
        ast->u.slice.end = end;
        return ast;
    }
    
    Ast* first = parse_expression(p);
    
    // 检查是否是切片语法：arr[start:end]
    if (p->lex.current.type == TOK_COLON) {
        lexer_next(&p->lex); // 消费 ':'
        
        Ast* end = NULL;
        if (p->lex.current.type != TOK_RBRACKET) {
            end = parse_expression(p);
        }
        consume(p, TOK_RBRACKET, "期望 ']'");
        
        Ast* ast = ast_new(AST_SLICE, line);
        ast->u.slice.obj = obj;
        ast->u.slice.start = first;
        ast->u.slice.end = end;
        return ast;
    }
    
    // 普通索引
    consume(p, TOK_RBRACKET, "期望 ']'");
    
    Ast* ast = ast_new(AST_INDEX, line);
    ast->u.index.obj = obj;
    ast->u.index.index = first;
    return ast;
}

// 点号访问解析器（dict.key 转换为 dict["key"]，或方法调用 arr.method() / io.print()）
Ast* parse_dot(Parser* p, Ast* left) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 '.'
    
    // 点号后面必须是标识符
    if (p->lex.current.type != TOK_IDENT) {
        if (is_type_keyword(p->lex.current.type)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "不能使用关键字(%.*s)作为属性名", 
                     p->lex.current.len, p->lex.current.text);
            error_add(ERR_SYNTAX, p->lex.current.line, msg);
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望属性名");
        }
        return left;
    }
    
    // 保存方法名/属性名
    char* name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex); // 消费标识符
    
    // 检查后面是否是 '(' - 方法调用
    if (p->lex.current.type == TOK_LPAREN) {
        // 方法调用：left.name(...)
        if (left->kind == AST_VAR) {
            // 变量方法调用：arr.add(...) 或 module.func(...)
            // 统一创建为 MODULE_CALL，由语义分析阶段确定具体类型
            Ast* ast = ast_new(AST_MODULE_CALL, line);
            ast->u.module_call.module_name = left->u.var.name;
            ast->u.module_call.method_name = name;
            ast_list_init(&ast->u.module_call.args);

            // 释放原始 AST_VAR 节点（name 已转移，需置 NULL 避免双重释放）
            left->u.var.name = NULL;
            free(left->u.var.ref.name);
            left->u.var.ref.name = NULL;
            if (left->cached_type) { type_free(left->cached_type); left->cached_type = NULL; }
            free(left);
            
            lexer_next(&p->lex); // 消费 '('
            
            // 解析参数列表
            if (p->lex.current.type != TOK_RPAREN) {
                do {
                    Ast* arg = parse_expression(p);
                    ast_list_add(&ast->u.module_call.args, arg);
                } while (match(p, TOK_COMMA));
            }
            
            consume(p, TOK_RPAREN, "期望 ')'");
            return ast;
        } else {
            // 表达式方法调用：(expr).method(...)
            // 创建为 CALL，callee 是 INDEX 节点 (expr)["method"]
            Ast* method_str = ast_new(AST_STRING, line);
            method_str->u.string.value = name;
            method_str->u.string.len = (int)strlen(name);

            Ast* index_ast = ast_new(AST_INDEX, line);
            index_ast->u.index.obj = left;
            index_ast->u.index.index = method_str;
            
            Ast* ast = ast_new(AST_CALL, line);
            ast->u.call.callee = index_ast;
            ast_list_init(&ast->u.call.args);
            ast->u.call.is_tail_call = 0;
            
            lexer_next(&p->lex); // 消费 '('
            
            // 解析参数列表
            if (p->lex.current.type != TOK_RPAREN) {
                do {
                    Ast* arg = parse_expression(p);
                    ast_list_add(&ast->u.call.args, arg);
                } while (match(p, TOK_COMMA));
            }
            
            consume(p, TOK_RPAREN, "期望 ')'");
            return ast;
        }
    }
    
    // 不是方法调用，是属性访问
    if (left->kind == AST_VAR) {
        // 检查是否是模块成员访问（如 test.PI）或 struct 字段访问
        // 暂时无法确定，先创建 MODULE_ACCESS，由语义分析阶段判断
        Ast* ast = ast_new(AST_MODULE_ACCESS, line);
        ast->u.module_access.module_name = left->u.var.name;
        ast->u.module_access.member_name = name;
        ast->u.module_access.ref.kind = SYM_GLOBAL;
        ast->u.module_access.ref.index = -1;
        ast->u.module_access.ref.name = NULL;

        // 释放原始 AST_VAR 节点（name 已转移，需置 NULL 避免双重释放）
        left->u.var.name = NULL;
        free(left->u.var.ref.name);
        left->u.var.ref.name = NULL;
        if (left->cached_type) { type_free(left->cached_type); left->cached_type = NULL; }
        free(left);

        return ast;
    }
    
    // 对于非变量类型（如索引表达式 arr[index]），使用 INDEX 节点
    // 将属性名转换为字符串作为索引
    Ast* index = ast_new(AST_STRING, line);
    index->u.string.value = name;
    index->u.string.len = (int)strlen(name);

    Ast* ast = ast_new(AST_INDEX, line);
    ast->u.index.obj = left;
    ast->u.index.index = index;
    return ast;
}

// 解析赋值表达式（复合赋值 +=, -= 等，单个赋值）
// 并行赋值 a, b = c, d 在 parse_expression_stmt 中处理
Ast* parse_assignment(Parser* p, Ast* left) {
    int line = p->lex.current.line;
    LenoTokenType op = p->lex.current.type;  // 保存运算符类型
    lexer_next(&p->lex); // 消费 '=' 或 '+=', '-=' 等

    // 解析右侧表达式
    Ast* value = parse_expression(p);

    // 处理索引赋值: dict["key"] = value 或 arr[0] = value
    if (left->kind == AST_INDEX) {
        // 处理复合赋值运算符: += -= *= /= %= &= |= ^= <<= >>=
        if (op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ || op == TOK_SLASHEQ ||
            op == TOK_MODEQ || op == TOK_BITANDEQ || op == TOK_BITOREQ || op == TOK_BITXOREQ ||
            op == TOK_SHLEQ || op == TOK_SHREQ || op == TOK_USHREQ) {
            // 创建二元运算: obj[index] + value
            Ast* binop = ast_new(AST_BINOP, line);
            // 创建新的 AST_INDEX 节点，避免与 left 共享指针导致双重释放
            Ast* index_access = ast_new(AST_INDEX, line);
            index_access->u.index.obj = left->u.index.obj;
            index_access->u.index.index = left->u.index.index;
            binop->u.binop.l = index_access;  // 使用新的索引访问节点
            binop->u.binop.r = value;
            switch (op) {
                case TOK_PLUSEQ:  binop->u.binop.op = TOK_PLUS; break;
                case TOK_MINUSEQ: binop->u.binop.op = TOK_MINUS; break;
                case TOK_STAREQ:  binop->u.binop.op = TOK_STAR; break;
                case TOK_SLASHEQ: binop->u.binop.op = TOK_SLASH; break;
                case TOK_MODEQ:   binop->u.binop.op = TOK_MOD; break;
                case TOK_BITANDEQ: binop->u.binop.op = TOK_BITAND; break;
                case TOK_BITOREQ:  binop->u.binop.op = TOK_BITOR; break;
                case TOK_BITXOREQ: binop->u.binop.op = TOK_BITXOR; break;
                case TOK_SHLEQ:    binop->u.binop.op = TOK_SHL; break;
                case TOK_SHREQ:    binop->u.binop.op = TOK_SHR; break;
                case TOK_USHREQ:   binop->u.binop.op = TOK_USHR; break;
                default: break;
            }
            value = binop;
        }

        // 创建专门的索引赋值节点
        Ast* assign_ast = ast_new(AST_INDEX_ASSIGN, line);
        assign_ast->u.index_assign.obj = left->u.index.obj;
        assign_ast->u.index_assign.index = left->u.index.index;
        assign_ast->u.index_assign.value = value;

        // 释放原来的 AST_INDEX 节点（但不释放 obj 和 index，它们现在属于 assign_ast）
        free(left);

        return assign_ast;
    }

    // 处理属性赋值: obj.prop = value（AST_MODULE_ACCESS 在赋值上下文中）
    // 保留 AST_MODULE_ACCESS，让语义分析阶段决定是字段访问还是索引访问
    if (left->kind == AST_MODULE_ACCESS) {
        // 处理复合赋值运算符
        if (op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ || op == TOK_SLASHEQ ||
            op == TOK_MODEQ || op == TOK_BITANDEQ || op == TOK_BITOREQ || op == TOK_BITXOREQ ||
            op == TOK_SHLEQ || op == TOK_SHREQ || op == TOK_USHREQ) {
            // 创建变量节点作为对象
            Ast* obj = ast_new(AST_VAR, line);
            obj->u.var.name = strdup(left->u.module_access.module_name);

            // 创建索引访问节点用于二元运算
            Ast* index = ast_new(AST_STRING, line);
            index->u.string.value = strdup(left->u.module_access.member_name);
            index->u.string.len = (int)strlen(left->u.module_access.member_name);

            Ast* index_access = ast_new(AST_INDEX, line);
            index_access->u.index.obj = obj;
            index_access->u.index.index = index;

            // 创建二元运算: obj["prop"] + value
            Ast* binop = ast_new(AST_BINOP, line);
            binop->u.binop.l = index_access;
            binop->u.binop.r = value;
            switch (op) {
                case TOK_PLUSEQ:  binop->u.binop.op = TOK_PLUS; break;
                case TOK_MINUSEQ: binop->u.binop.op = TOK_MINUS; break;
                case TOK_STAREQ:  binop->u.binop.op = TOK_STAR; break;
                case TOK_SLASHEQ: binop->u.binop.op = TOK_SLASH; break;
                case TOK_MODEQ:   binop->u.binop.op = TOK_MOD; break;
                case TOK_BITANDEQ: binop->u.binop.op = TOK_BITAND; break;
                case TOK_BITOREQ:  binop->u.binop.op = TOK_BITOR; break;
                case TOK_BITXOREQ: binop->u.binop.op = TOK_BITXOR; break;
                case TOK_SHLEQ:    binop->u.binop.op = TOK_SHL; break;
                case TOK_SHREQ:    binop->u.binop.op = TOK_SHR; break;
                case TOK_USHREQ:   binop->u.binop.op = TOK_USHR; break;
                default: break;
            }
            
            // 创建索引赋值节点
            Ast* index_ast = ast_new(AST_STRING, line);
            index_ast->u.string.value = strdup(left->u.module_access.member_name);
            index_ast->u.string.len = (int)strlen(left->u.module_access.member_name);
            
            Ast* obj_ast = ast_new(AST_VAR, line);
            obj_ast->u.var.name = strdup(left->u.module_access.module_name);
            
            Ast* assign_ast = ast_new(AST_INDEX_ASSIGN, line);
            assign_ast->u.index_assign.obj = obj_ast;
            assign_ast->u.index_assign.index = index_ast;
            assign_ast->u.index_assign.value = binop;
            
            // 释放原来的 AST_MODULE_ACCESS 节点
            free(left->u.module_access.member_name);
            free(left->u.module_access.module_name);
            free(left);
            
            return assign_ast;
        }
        
        // 普通赋值：保留 AST_MODULE_ACCESS，让语义分析阶段决定
        Ast* ast = ast_new(AST_ASSIGN, line);
        ast->u.assign.names = (char**)malloc(sizeof(char*));
        ast->u.assign.name_count = 1;
        ast->u.assign.targets = (Ast**)malloc(sizeof(Ast*));
        ast->u.assign.targets[0] = left;  // 保留 AST_MODULE_ACCESS
        ast->u.assign.value = value;
        ast->u.assign.refs = (SymRef*)calloc(1, sizeof(SymRef));
        ast->u.assign.names[0] = strdup(left->u.module_access.module_name);  // 使用模块名作为变量名
        
        return ast;
    }

    if (left->kind != AST_VAR) {
        error_add(ERR_SYNTAX, p->lex.current.line, "赋值目标必须是变量");
        return NULL;
    }

    // 处理复合赋值运算符: += -= *= /= %= &= |= ^= <<= >>=
    if (op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ || op == TOK_SLASHEQ ||
        op == TOK_MODEQ || op == TOK_BITANDEQ || op == TOK_BITOREQ || op == TOK_BITXOREQ ||
        op == TOK_SHLEQ || op == TOK_SHREQ || op == TOK_USHREQ) {
        // 创建复合赋值节点
        Ast* ast = ast_new(AST_COMPOUND_ASSIGN, line);
        ast->u.compound_assign.name = strdup(left->u.var.name);
        ast->u.compound_assign.value = value;
        ast->u.compound_assign.op = op;
        // ref 信息在语义分析时填充
        
        // 释放临时变量节点
        free(left->u.var.name);
        free(left);
        
        return ast;
    }

    // 普通单个赋值
    Ast* ast = ast_new(AST_ASSIGN, line);
    ast->u.assign.names = (char**)malloc(sizeof(char*));
    ast->u.assign.name_count = 1;
    ast->u.assign.targets = (Ast**)malloc(sizeof(Ast*));
    ast->u.assign.targets[0] = left;
    ast->u.assign.value = value;
    ast->u.assign.refs = (SymRef*)calloc(1, sizeof(SymRef));
    ast->u.assign.names[0] = strdup(left->u.var.name);

    return ast;
}

// 后缀 ++ 和 -- 解析器
Ast* parse_postfix_inc_dec(Parser* p, Ast* left) {
    LenoTokenType op = p->lex.current.type;
    int line = p->lex.current.line;
    lexer_next(&p->lex);

    Ast* ast = ast_new(AST_UNARY, line);
    ast->u.unary.op = op;
    ast->u.unary.operand = left;
    ast->u.unary.is_postfix = 1;  // 后缀形式
    return ast;
}

// 解析类型检查表达式: expr is type
Ast* parse_type_check(Parser* p, Ast* left) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 "is"

    // 解析类型
    TypeInfo* type_info = parse_type(p);
    if (!type_info) {
        error_add(ERR_SYNTAX, p->lex.current.line, "类型检查表达式期望类型名（如 int, float, string, bool, Array[int] 等）");
        return left;
    }

    // 创建类型检查 AST 节点
    Ast* ast = ast_new(AST_TYPE_CHECK, line);
    ast->u.type_check.expr = left;
    ast->u.type_check.type = type_info;
    return ast;
}

Ast* parse_as_cast(Parser* p, Ast* left) {
    int line = p->lex.current.line;
    lexer_next(&p->lex);

    TypeInfo* type_info = parse_type(p);
    if (!type_info) {
        error_add(ERR_SYNTAX, p->lex.current.line, "as 转型表达式期望类型名");
        return left;
    }

    Ast* ast = ast_new(AST_AS_CAST, line);
    ast->u.type_check.expr = left;
    ast->u.type_check.type = type_info;
    return ast;
}

// 解析 await 表达式: await expr
Ast* parse_await(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 "await"

    // 解析被等待的表达式
    Ast* expr = parse_precedence(p, PREC_UNARY);

    // 创建 await AST 节点
    Ast* ast = ast_new(AST_AWAIT, line);
    ast->u.await.expr = expr;
    return ast;
}

// 解析匿名函数表达式: func() { ... }
static Ast* parse_anon_func(Parser* p) {
    return parse_anonymous_func(p);
}
