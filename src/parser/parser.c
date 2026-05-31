#include "../include/lenolang.h"
#include "parser_internal.h"

// ============================================================================
// 语句解析分发器
// ============================================================================

Ast* parse_statement(Parser* p) {
    Ast* stmt = NULL;

    switch (p->lex.current.type) {
        // 词法错误：跳过错误 token 并返回 NULL，让 parser 继续解析
        case TOK_ERROR:
            lexer_next(&p->lex);  // 跳过错误 token
            return NULL;

        // 控制流语句
        case TOK_IF:
        case TOK_EIF:
            stmt = parse_if_stmt(p);
            break;
        case TOK_WHILE:
            stmt = parse_while_stmt(p);
            break;
        case TOK_FOR:
            stmt = parse_for_stmt(p);
            break;
        case TOK_SWITCH:
            stmt = parse_switch_stmt(p);
            break;

        // 函数定义
        case TOK_FUNC:
            stmt = parse_func_stmt(p);
            break;
        
        // async 函数定义
        case TOK_ASYNC:
            stmt = parse_func_stmt(p);
            break;

        // 跳转语句
        case TOK_RETURN:
            stmt = parse_return_stmt(p);
            break;
        case TOK_BREAK:
            stmt = parse_break_stmt(p);
            break;
        case TOK_CONTINUE:
            stmt = parse_continue_stmt(p);
            break;

        // 异常处理
        case TOK_TRY:
            stmt = parse_try_stmt(p);
            break;
        case TOK_THROW:
            stmt = parse_throw_stmt(p);
            break;

        // 模块系统
        case TOK_IMPORT:
            stmt = parse_import_stmt(p);
            break;
        case TOK_EXPORT:
            stmt = parse_export_stmt(p);
            break;
        case TOK_USE:
            stmt = parse_use_stmt(p);
            break;

        // 变量声明（var 关键字或类型关键字开头，或自定义 struct 类型）
        case TOK_VAR:
        case TOK_INT_TYPE:
        case TOK_FLOAT_TYPE:
        case TOK_STRING_TYPE:
        case TOK_BOOL_TYPE:
        case TOK_ARRAY_TYPE:
        case TOK_DICT_TYPE:
        case TOK_FILE_TYPE:
        case TOK_WIN_TYPE:
        case TOK_DRAW_TYPE:
        case TOK_EVENT_TYPE:
        case TOK_RGB_TYPE:
        case TOK_PTR_TYPE:
            stmt = parse_var_decl_internal(p);
            break;

        // struct 定义
        case TOK_STRUCT:
            stmt = parse_struct_stmt(p);
            break;

        // face 定义
        case TOK_FACE:
            stmt = parse_face_stmt(p);
            break;

        // cstruct 定义（C 布局结构体）
        case TOK_CSTRUCT:
            stmt = parse_cstruct_stmt(p);
            break;

        // enum 定义
        case TOK_ENUM:
            stmt = parse_enum_stmt(p);
            break;

        // 代码块
        case TOK_LBRACE:
            stmt = parse_block_internal(p);
            break;

        // 默认情况：检查是否是入口函数定义、自定义类型变量声明或表达式语句
        default:
            // 检查是否是入口函数定义（如 main() { }）
            if (is_entry_function_def(p)) {
                stmt = parse_entry_func_stmt(p);
            }
            // 检查是否是自定义类型变量声明（如 Point b = ...）
            // 当前 token 是标识符，且下一个 token 也是标识符
            else if (p->lex.current.type == TOK_IDENT) {
                // 预读检查下一个 token
                Lexer saved = p->lex;
                lexer_next(&p->lex);
                int is_type_decl = (p->lex.current.type == TOK_IDENT);
                p->lex = saved; // 恢复 lexer 状态
                
                if (is_type_decl) {
                    stmt = parse_var_decl_internal(p);
                } else {
                    stmt = parse_expression_stmt(p);
                }
            } else {
                stmt = parse_expression_stmt(p);
            }
            break;
    }

    // 可选的分号（语句结束符在 Leno 中是可选的）
    match(p, TOK_SEMI);

    return stmt;
}

// ============================================================================
// 程序解析
// ============================================================================

Ast* parse_program_internal(Parser* p) {
    int line = p->lex.current.line;
    Ast* ast = ast_new(AST_BLOCK, line);
    ast_list_init(&ast->u.block);

    // 循环解析所有语句直到文件结束
    while (p->lex.current.type != TOK_EOF) {
        Ast* stmt = parse_statement(p);
        if (stmt) {
            ast_list_add(&ast->u.block, stmt);
        }
        // 错误恢复：如果解析语句失败，尝试跳过当前token继续解析
        // 这样可以收集更多错误而不是遇到第一个错误就停止
        if (!stmt && p->lex.current.type != TOK_EOF) {
            // 尝试同步到下一个语句的开始
            // 跳过当前token，寻找下一个可能的语句开始位置
            while (p->lex.current.type != TOK_EOF &&
                   p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_IF &&
                   p->lex.current.type != TOK_WHILE &&
                   p->lex.current.type != TOK_FOR &&
                   p->lex.current.type != TOK_FUNC &&
                   p->lex.current.type != TOK_ASYNC &&
                   p->lex.current.type != TOK_STRUCT &&
                   p->lex.current.type != TOK_FACE &&
                   p->lex.current.type != TOK_VAR &&
                   p->lex.current.type != TOK_RETURN &&
                   p->lex.current.type != TOK_INT_TYPE &&
                   p->lex.current.type != TOK_FLOAT_TYPE &&
                   p->lex.current.type != TOK_STRING_TYPE &&
                   p->lex.current.type != TOK_BOOL_TYPE &&
                   p->lex.current.type != TOK_IDENT) {
                lexer_next(&p->lex);
            }
            // 如果遇到分号或右大括号，消费它并继续
            if (p->lex.current.type == TOK_SEMI || p->lex.current.type == TOK_RBRACE) {
                lexer_next(&p->lex);
            }
        }
    }

    return ast;
}

// ============================================================================
// 公共 API 实现
// ============================================================================

// 初始化解析器
void parser_init(Parser* p, const char* src) {
    lexer_init(&p->lex, src);
    p->root = NULL;
    p->current_scope = NULL;
    p->struct_names = NULL;
    p->struct_count = 0;
    p->struct_capacity = 0;
}

// 添加 struct 名称到集合
static void add_struct_name(Parser* p, const char* name) {
    // 检查是否已存在
    for (int i = 0; i < p->struct_count; i++) {
        if (strcmp(p->struct_names[i], name) == 0) {
            return;
        }
    }

    // 扩容
    if (p->struct_count >= p->struct_capacity) {
        p->struct_capacity = p->struct_capacity == 0 ? 16 : p->struct_capacity * 2;
        p->struct_names = realloc(p->struct_names, sizeof(char*) * p->struct_capacity);
    }

    // 添加
    p->struct_names[p->struct_count++] = strdup(name);
}

// 检查名称是否是 struct 类型
int is_struct_name(Parser* p, const char* name) {
    for (int i = 0; i < p->struct_count; i++) {
        if (strcmp(p->struct_names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

// 预扫描收集所有 struct 定义
static void prescan_struct_definitions(Parser* p) {
    // 保存当前 lexer 状态
    Lexer saved = p->lex;

    // 扫描所有 token
    while (p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_STRUCT) {
            lexer_next(&p->lex); // 跳过 'struct'
            if (p->lex.current.type == TOK_IDENT) {
                add_struct_name(p, p->lex.current.text);
            }
        }
        lexer_next(&p->lex);
    }

    // 恢复 lexer 状态
    p->lex = saved;
}

// 执行解析
int parser_parse(Parser* p) {
    // 预扫描收集 struct 定义，支持前向引用
    prescan_struct_definitions(p);

    p->root = parse_program_internal(p);
    return error_has_any() ? -1 : 0;
}
