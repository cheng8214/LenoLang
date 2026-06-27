#include "parser_internal.h"

// ============================================================================
// 代码块解析
// ============================================================================

// 解析代码块 { stmt1; stmt2; ... }
Ast* parse_block_internal(Parser* p) {
    int line = p->lex.current.line;
    consume(p, TOK_LBRACE, "期望 '{'");
    
    Ast* ast = ast_new(AST_BLOCK, line);
    ast_list_init(&ast->u.block);
    
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        Ast* stmt = parse_statement(p);
        if (stmt) {
            ast_list_add(&ast->u.block, stmt);
        }
        // 错误恢复：同步到块内下一个语句开始位置
        // 跳过混乱的 token 直到找到语句边界或块结束
        if (!stmt && p->lex.current.type != TOK_EOF && p->lex.current.type != TOK_RBRACE) {
            parser_synchronize(p);
            // 在块内，只消费分号，不消费右大括号（它属于当前块）
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
        }
    }
    
    consume(p, TOK_RBRACE, "期望 '}'");
    return ast;
}

// ============================================================================
// if 语句解析（支持类型守卫和多条件）
// ============================================================================

Ast* parse_if_stmt(Parser* p) {
    int line = p->lex.current.line;
    // 支持 if 和 eif 关键字
    if (p->lex.current.type == TOK_IF || p->lex.current.type == TOK_EIF) {
        lexer_next(&p->lex); // 消费 if/eif
    } else {
        error_add(ERR_SYNTAX, line, "期望 if 或 eif 关键字");
        return NULL;
    }

    // 检查是否是类型守卫语法：if var is type
    // 注意：让 parse_expression 自己处理括号，不要在外部干预
    // 这样可以正确解析 if (i + 1) % 100 == 0 这样的表达式
    char* guard_var = NULL;
    TypeInfo* guard_type = NULL;
    Ast* cond = NULL;
    TypeGuardList guard_conds;
    int has_guard_conds = 0;  // 标记是否有类型守卫条件列表

    if (p->lex.current.type == TOK_IDENT) {
        // 保存变量名位置
        char* var_name = copy_string(p->lex.current.text, p->lex.current.len);
        int var_line = p->lex.current.line;

        // 向前看，检查是否是类型守卫语法
        Lexer saved_lex = p->lex;
        lexer_next(&p->lex); // 消费变量名

        // 检查是否是属性访问守卫：obj.field is Type
        char* field_name = NULL;
        if (p->lex.current.type == TOK_DOT) {
            Lexer dot_saved_lex = p->lex;
            lexer_next(&p->lex); // 消费 "."
            if (p->lex.current.type == TOK_IDENT) {
                // 保存字段名，继续看后面是否是 "is"
                field_name = copy_string(p->lex.current.text, p->lex.current.len);
                lexer_next(&p->lex); // 消费字段名
                if (p->lex.current.type != TOK_IS && p->lex.current.type != TOK_NOT) {
                    // 不是类型守卫，恢复到 "." 之前
                    free(field_name);
                    field_name = NULL;
                    p->lex = dot_saved_lex;
                }
            } else {
                // "." 后面不是标识符，恢复
                p->lex = dot_saved_lex;
            }
        }

        // 检查是否是 "not is"（否定类型守卫）
        int is_not_is = 0;
        if (field_name == NULL) {
            if (p->lex.current.type == TOK_NOT) {
                Lexer not_saved_lex = p->lex;
                lexer_next(&p->lex); // 消费 "not"
                if (p->lex.current.type == TOK_IS) {
                    is_not_is = 1;
                } else {
                    p->lex = not_saved_lex;
                }
            }
        } else {
            if (p->lex.current.type == TOK_NOT) {
                Lexer not_saved_lex = p->lex;
                lexer_next(&p->lex); // 消费 "not"
                if (p->lex.current.type == TOK_IS) {
                    is_not_is = 1;
                } else {
                    p->lex = not_saved_lex;
                }
            }
        }

        // 是类型守卫语法：var is Type 或 var.field is Type
        if ((field_name == NULL && (p->lex.current.type == TOK_IS || is_not_is)) ||
            (field_name != NULL && (p->lex.current.type == TOK_IS || is_not_is))) {
            lexer_next(&p->lex); // 消费 "is"

            // 使用新的类型解析函数解析类型（支持 Array[int], Dict[string, int] 等）
            TypeInfo* type_info = parse_type(p);
            if (!type_info) {
                error_add(ERR_SYNTAX, p->lex.current.line, "类型守卫期望类型名（如 int, Array[int], Dict[string, int] 等）");
                free(var_name);
                if (field_name) free(field_name);
                return NULL;
            }

            guard_var = strdup(var_name);
            guard_type = type_info;

            // 创建条件表达式
            // 对于 "not is"，创建否定表达式
            if (is_not_is) {
                // 创建一元非表达式：!(type_check)
                Ast* type_check = ast_new(AST_TYPE_CHECK, var_line);
                if (field_name) {
                    // 属性访问守卫：s.age not is int
                    type_check->u.type_check.expr = ast_new(AST_FIELD_ACCESS, var_line);
                    type_check->u.type_check.expr->u.field_access.obj = ast_new(AST_VAR, var_line);
                    type_check->u.type_check.expr->u.field_access.obj->u.var.name = strdup(var_name);
                    type_check->u.type_check.expr->u.field_access.field_name = strdup(field_name);
                    type_check->u.type_check.expr->u.field_access.field_index = -1;
                } else {
                    // 简单变量守卫：x not is int
                    type_check->u.type_check.expr = ast_new(AST_VAR, var_line);
                    type_check->u.type_check.expr->u.var.name = strdup(var_name);
                }
                type_check->u.type_check.type = type_copy(type_info);
                
                cond = ast_new(AST_UNARY, var_line);
                cond->u.unary.op = TOK_NOT;
                cond->u.unary.operand = type_check;
            } else {
                cond = ast_new(AST_TYPE_CHECK, var_line);
                if (field_name) {
                    // 属性访问守卫：s.age is int
                    cond->u.type_check.expr = ast_new(AST_FIELD_ACCESS, var_line);
                    cond->u.type_check.expr->u.field_access.obj = ast_new(AST_VAR, var_line);
                    cond->u.type_check.expr->u.field_access.obj->u.var.name = strdup(var_name);
                    cond->u.type_check.expr->u.field_access.field_name = strdup(field_name);
                    cond->u.type_check.expr->u.field_access.field_index = -1;
                } else {
                    // 简单变量守卫：x is int
                    cond->u.type_check.expr = ast_new(AST_VAR, var_line);
                    cond->u.type_check.expr->u.var.name = strdup(var_name);
                }
                cond->u.type_check.type = type_copy(type_info);
            }

            // 初始化类型守卫条件列表，添加第一个条件
            type_guard_list_init(&guard_conds);
            has_guard_conds = 1;
            TypeGuardCond first_cond;
            memset(&first_cond, 0, sizeof(first_cond));
            first_cond.var_name = strdup(var_name);
            first_cond.field_name = field_name ? strdup(field_name) : NULL;
            first_cond.guard_type = type_copy(type_info);
            type_guard_list_add(&guard_conds, first_cond);

            free(var_name);
            if (field_name) free(field_name);

            // 检查是否有 or/and 连接多个类型守卫条件
            // 如果 or/and 后不是类型守卫，则回退并用普通表达式解析余下部分
            int fallback_to_expr = 0;
            while (p->lex.current.type == TOK_OR || p->lex.current.type == TOK_AND) {
                LenoTokenType op = p->lex.current.type;
                lexer_next(&p->lex); // 消费 or/and
                Lexer saved_after_op = p->lex; // or/and 之后的位置

                // 解析下一个类型守卫条件
                if (p->lex.current.type == TOK_IDENT) {
                    char* next_var_name = copy_string(p->lex.current.text, p->lex.current.len);
                    lexer_next(&p->lex); // 消费变量名

                    // 检查是否是属性访问守卫
                    char* next_field_name = NULL;
                    if (p->lex.current.type == TOK_DOT) {
                        Lexer dot_saved = p->lex;
                        lexer_next(&p->lex); // 消费 "."
                        if (p->lex.current.type == TOK_IDENT) {
                            next_field_name = copy_string(p->lex.current.text, p->lex.current.len);
                            lexer_next(&p->lex); // 消费字段名
                            if (p->lex.current.type != TOK_IS) {
                                free(next_field_name);
                                next_field_name = NULL;
                                p->lex = dot_saved;
                            }
                        } else {
                            p->lex = dot_saved;
                        }
                    }

                    if (p->lex.current.type == TOK_IS) {
                        lexer_next(&p->lex); // 消费 "is"

                        // 使用新的类型解析函数
                        TypeInfo* next_type_info = parse_type(p);
                        if (!next_type_info) {
                            error_add(ERR_SYNTAX, p->lex.current.line, "期望类型名");
                            free(next_var_name);
                            if (next_field_name) free(next_field_name);
                            type_guard_list_free(&guard_conds);
                            return NULL;
                        }

                        // 添加到类型守卫条件列表
                        TypeGuardCond next_cond;
                        memset(&next_cond, 0, sizeof(next_cond));
                        next_cond.var_name = next_var_name;
                        next_cond.field_name = next_field_name;
                        next_cond.guard_type = next_type_info;
                        type_guard_list_add(&guard_conds, next_cond);

                        // 创建下一个条件的占位
                        Ast* next_cond_ast = ast_new(AST_BOOL, var_line);
                        next_cond_ast->u.boolean = 1;

                        // 创建二元操作节点
                        Ast* binop = ast_new(AST_BINOP, var_line);
                        binop->u.binop.l = cond;
                        binop->u.binop.r = next_cond_ast;
                        binop->u.binop.op = (op == TOK_OR) ? TOK_OR : TOK_AND;
                        cond = binop;
                    } else {
                        // or/and 后不是类型守卫，回退到 or/and 之后用普通表达式解析
                        free(next_var_name);
                        if (next_field_name) free(next_field_name);
                        p->lex = saved_after_op;
                        fallback_to_expr = 1;
                        break;
                    }
                } else {
                    // or/and 后不是标识符，回退用普通表达式解析
                    p->lex = saved_after_op;
                    fallback_to_expr = 1;
                    break;
                }
            }

            // 回退：or/and 后跟的是普通表达式，放弃类型守卫路径，
            // 恢复 lexer 到变量名前，用 parse_expression 重新解析整个条件
            // 注意：var_name/field_name 已在前面 free（line 178-179），
            //       guard_conds 中的拷本由 type_guard_list_free 负责释放
            if (fallback_to_expr) {
                type_guard_list_free(&guard_conds);
                p->lex = saved_lex;
                cond = parse_expression(p);
            }
        } else {
            // 不是类型守卫，恢复 lexer 位置并正常解析表达式
            if (field_name) free(field_name);
            p->lex = saved_lex;
            free(var_name);
            cond = parse_expression(p);
        }
    } else {
        // 正常解析条件表达式
        // parse_expression 会自己处理括号
        cond = parse_expression(p);
    }

    // then 分支必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        error_add(ERR_SYNTAX, p->lex.current.line, "if 语句体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* then_branch = parse_block_internal(p);

    Ast* else_branch = NULL;

    // 支持 else if 和 eif
    if (match(p, TOK_ELSE)) {
        // 检查是否是 else if
        if (p->lex.current.type == TOK_IF) {
            // else if 情况：递归解析 if 语句
            else_branch = parse_if_stmt(p);
        } else if (p->lex.current.type == TOK_EIF) {
            // else eif 不合法，报错
            error_add(ERR_SYNTAX, p->lex.current.line, "else 后面不能直接跟 eif，请使用 else if 或单独的 eif");
            return NULL;
        } else {
            // else 分支必须是代码块 {}
            if (p->lex.current.type != TOK_LBRACE) {
                error_add(ERR_SYNTAX, p->lex.current.line, "else 语句体必须用大括号 {} 包裹");
                return NULL;
            }
            else_branch = parse_block_internal(p);
        }
    } else if (p->lex.current.type == TOK_EIF) {
        // eif 情况：递归解析 if 语句
        else_branch = parse_if_stmt(p);
    }

    Ast* ast = ast_new(AST_IF, line);
    ast->u.if_.cond = cond;
    ast->u.if_.then = then_branch;
    ast->u.if_.else_ = else_branch;
    ast->u.if_.guard_var = guard_var;
    ast->u.if_.guard_type = guard_type;
    if (has_guard_conds) {
        ast->u.if_.guard_conds = guard_conds;
    } else {
        type_guard_list_init(&ast->u.if_.guard_conds);
    }
    return ast;
}

// ============================================================================
// while 语句解析（支持多条件组合）
// ============================================================================

Ast* parse_while_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // while
    
    // 让 parse_expression 自己处理括号
    // 这样可以正确解析 while (i + 1) % 100 == 0 这样的表达式
    Ast* cond = parse_expression(p);

    // while 循环体必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        error_add(ERR_SYNTAX, p->lex.current.line, "while 循环体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* body = parse_block_internal(p);
    
    Ast* ast = ast_new(AST_WHILE, line);
    ast->u.while_.cond = cond;
    ast->u.while_.body = body;
    return ast;
}

// ============================================================================
// for 语句解析（支持多种语法形式）
// ============================================================================

Ast* parse_for_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // for

    Ast* start = NULL;
    Ast* end = NULL;
    Ast* step = NULL;
    int inclusive = 0;
    char* var_name = NULL;

    // 检查是否是简单循环次数形式: for 5 { } 或 for expr { }
    if (p->lex.current.type == TOK_NUM || p->lex.current.type == TOK_IDENT) {
        // 保存当前位置
        Lexer saved_lex = p->lex;
        
        // 先尝试解析表达式作为可能的 start
        start = parse_expression(p);
        
        // 检查解析后遇到的是 : 还是 to 或 {
        if (p->lex.current.type == TOK_COLON) {
            // for start:end 形式
            inclusive = 1;
            lexer_next(&p->lex); // :
            
            // 解析结束值
            end = parse_expression(p);
            
            // 检查是否有步进值
            if (p->lex.current.type == TOK_COLON) {
                lexer_next(&p->lex); // :
                step = parse_expression(p);
            }
        } else if (p->lex.current.type == TOK_LBRACE || p->lex.current.type == TOK_TO) {
            // for 5 { } 或 for 5 to var { } 形式 - 只有结束值，从0开始
            end = start;
            start = NULL; // 表示从0开始
        } else {
            // 其他情况，无法继续解析，报错并恢复
            ast_free(start);
            p->lex = saved_lex;
            start = NULL;
            end = parse_expression(p);
        }
    } else if (p->lex.current.type == TOK_COLON) {
        // for :5 { } 形式 - 从0到5（包含）
        inclusive = 1;
        lexer_next(&p->lex); // :
        end = parse_expression(p);
        start = NULL;
    } else {
        // 其他表达式作为结束值
        end = parse_expression(p);
        start = NULL;
    }

    // 解析可选的 to varname
    char* index_var_name = NULL;
    if (p->lex.current.type == TOK_TO) {
        lexer_next(&p->lex); // to
        
        // 可选的 var 关键字
        if (p->lex.current.type == TOK_VAR) {
            lexer_next(&p->lex); // var
        }
        
        // 期望标识符作为变量名
        if (p->lex.current.type == TOK_IDENT) {
            var_name = (char*)malloc(p->lex.current.len + 1);
            if (var_name) {
                memcpy(var_name, p->lex.current.text, p->lex.current.len);
                var_name[p->lex.current.len] = '\0';
            }
            lexer_next(&p->lex);
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "for 循环变量名必须是标识符");
            return NULL;
        }
        
        // 解析可选的索引变量: to item, index
        if (match(p, TOK_COMMA)) {
            // 可选的 var 关键字
            if (p->lex.current.type == TOK_VAR) {
                lexer_next(&p->lex); // var
            }
            
            // 期望标识符作为索引变量名
            if (p->lex.current.type == TOK_IDENT) {
                index_var_name = (char*)malloc(p->lex.current.len + 1);
                if (index_var_name) {
                    memcpy(index_var_name, p->lex.current.text, p->lex.current.len);
                    index_var_name[p->lex.current.len] = '\0';
                }
                lexer_next(&p->lex);
            } else {
                error_add(ERR_SYNTAX, p->lex.current.line, "for 循环索引变量名必须是标识符");
                return NULL;
            }
        }
    }

    // for 循环体必须是代码块 {}
    if (p->lex.current.type != TOK_LBRACE) {
        error_add(ERR_SYNTAX, p->lex.current.line, "for 循环体必须用大括号 {} 包裹");
        return NULL;
    }
    Ast* body = parse_block_internal(p);

    Ast* ast = ast_new(AST_FOR, line);
    ast->u.for_.start = start;
    ast->u.for_.end = end;
    ast->u.for_.step = step;
    ast->u.for_.inclusive = inclusive;
    ast->u.for_.var_name = var_name;
    ast->u.for_.index_var_name = index_var_name;
    ast->u.for_.body = body;
    return ast;
}

// ============================================================================
// switch 语句解析（支持多值 case）
// ============================================================================

Ast* parse_switch_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // switch
    
    // 解析 switch 表达式
    Ast* expr = parse_expression(p);
    
    // 提取 switch 表达式的变量名（用于 case is Type 的类型收窄）
    char* switch_var_name = NULL;
    if (expr->kind == AST_VAR) {
        switch_var_name = strdup(expr->u.var.name);
    }
    
    // switch 体必须用大括号包裹
    if (p->lex.current.type != TOK_LBRACE) {
        error_add(ERR_SYNTAX, p->lex.current.line, "switch 语句体必须用大括号 {} 包裹");
        free(switch_var_name);
        return NULL;
    }
    lexer_next(&p->lex); // 消费 '{'
    
    // 分配 case 数组（初始容量 8）
    int case_capacity = 8;
    struct SwitchCase* cases = malloc(sizeof(struct SwitchCase) * case_capacity);
    int case_count = 0;
    Ast* default_body = NULL;
    
    // 解析 case 和 default
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_CASE) {
            lexer_next(&p->lex); // case
            
            // 扩展 case 数组
            if (case_count >= case_capacity) {
                case_capacity *= 2;
                cases = realloc(cases, sizeof(struct SwitchCase) * case_capacity);
            }
            
            // 初始化 case 字段
            ast_list_init(&cases[case_count].values);
            cases[case_count].is_type_match = 0;
            cases[case_count].match_type = NULL;
            cases[case_count].guard_var = NULL;
            memset(&cases[case_count].guard_var_ref, 0, sizeof(SymRef));
            
            // 检查是否是 case is Type 模式
            if (p->lex.current.type == TOK_IS) {
                lexer_next(&p->lex); // is
                
                // 解析类型
                TypeInfo* type_info = parse_type(p);
                if (!type_info) {
                    error_add(ERR_SYNTAX, p->lex.current.line, "case is 后期望类型名");
                    free(switch_var_name);
                    free(cases);
                    return NULL;
                }
                
                cases[case_count].is_type_match = 1;
                cases[case_count].match_type = type_info;
                if (switch_var_name) {
                    cases[case_count].guard_var = strdup(switch_var_name);
                }
            } else {
                // 普通值匹配：解析 case 值（支持多个值，用逗号分隔）
                do {
                    Ast* value = parse_expression(p);
                    if (value) {
                        ast_list_add(&cases[case_count].values, value);
                    }
                } while (match(p, TOK_COMMA));
            }
            
            // case 体必须用大括号包裹
            if (p->lex.current.type != TOK_LBRACE) {
                error_add(ERR_SYNTAX, p->lex.current.line, "case 语句体必须用大括号 {} 包裹");
                free(switch_var_name);
                free(cases);
                return NULL;
            }
            cases[case_count].body = parse_block_internal(p);
            case_count++;
        } else if (p->lex.current.type == TOK_DEFAULT) {
            lexer_next(&p->lex); // default
            
            // default 体必须用大括号包裹
            if (p->lex.current.type != TOK_LBRACE) {
                error_add(ERR_SYNTAX, p->lex.current.line, "default 语句体必须用大括号 {} 包裹");
                free(switch_var_name);
                free(cases);
                return NULL;
            }
            default_body = parse_block_internal(p);
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "switch 语句中期望 case 或 default");
            free(switch_var_name);
            free(cases);
            return NULL;
        }
    }
    
    consume(p, TOK_RBRACE, "期望 '}'");
    
    // 创建 AST 节点
    Ast* ast = ast_new(AST_SWITCH, line);
    ast->u.switch_.expr = expr;
    ast->u.switch_.cases = cases;
    ast->u.switch_.case_count = case_count;
    ast->u.switch_.default_body = default_body;
    
    free(switch_var_name);
    return ast;
}
