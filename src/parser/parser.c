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

        // 函数定义 或 函数类型变量声明（func():void v = ...）
        case TOK_FUNC: {
            // 预读：func 后面跟 ( 说明是函数类型（如 func(int):int），走变量声明
            // func 后面跟 IDENT 说明是函数定义（如 func foo()）
            Lexer saved = p->lex;
            lexer_next(&p->lex);  // 跳过 func
            int is_func_type = (p->lex.current.type == TOK_LPAREN);
            p->lex = saved;  // 恢复
            error_set_column(saved.current.column);  // 恢复列号

            if (is_func_type) {
                stmt = parse_var_decl_internal(p);
            } else {
                stmt = parse_func_stmt(p);
            }
            break;
        }
        
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

        // 变量声明（var/const 关键字或类型关键字开头，或自定义 struct 类型）
        case TOK_VAR:
        case TOK_CONST:
        case TOK_INT_TYPE:
        case TOK_FLOAT_TYPE:
        case TOK_STRING_TYPE:
        case TOK_BOOL_TYPE:
        case TOK_ARRAY_TYPE:
        case TOK_DICT_TYPE:
        // case TOK_BINT:  // 已移除：对外统一用 int
        case TOK_FILE_TYPE:
        case TOK_SOCKET_TYPE:
        case TOK_CHANNEL_TYPE:
        case TOK_THREAD_TYPE:
        case TOK_FUTURE_TYPE:
        case TOK_PTR_TYPE:
        case TOK_ANY_TYPE:  // any 类型变量声明（语义分析阶段会限制）
        // C 布局类型（i32/u8 等）：允许解析为变量声明，语义分析阶段会限制使用范围
        case TOK_I8:  case TOK_U8:
        case TOK_I16: case TOK_U16:
        case TOK_I32: case TOK_U32:
        case TOK_I64: case TOK_U64:
        case TOK_F32: case TOK_F64:
        case TOK_C_INT: case TOK_C_UINT:
        case TOK_C_LONG: case TOK_C_ULONG:
        case TOK_C_LONGLONG: case TOK_C_ULONGLONG:
        case TOK_C_SIZE: case TOK_C_SSIZE:
        case TOK_STR8: case TOK_STR16:
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
        // packed cstruct / align(N) cstruct 也路由到这里
        // 注意：packed 和 align 是上下文关键字，在 default 分支中通过文本匹配识别
        case TOK_CSTRUCT:
            stmt = parse_cstruct_stmt(p);
            break;

        // clib 定义（C 库函数签名声明）
        case TOK_CLIB:
            stmt = parse_clib_stmt(p);
            break;
        // cfunc 声明（C 回调函数签名）
        case TOK_CFUNC:
            stmt = parse_cfunc_stmt(p);
            break;

        // enum 定义
        case TOK_ENUM:
            stmt = parse_enum_stmt(p);
            break;

        // 类型别名
        case TOK_ALIAS:
            stmt = parse_alias_stmt(p);
            break;

        // 代码块
        case TOK_LBRACE:
            stmt = parse_block_internal(p);
            break;

        // 默认情况：检查是否是入口函数定义、自定义类型变量声明或表达式语句
        default:
            // 检查是否是未支持的关键字（this）
            if (p->lex.current.type == TOK_IDENT) {
                int ident_len = p->lex.current.len;
                const char* ident_text = p->lex.current.text;
                if (ident_len == 4 && strncmp(ident_text, "this", 4) == 0) {
                    error_add_at(ERR_SYNTAX, p->lex.current.line, p->lex.current.column,
                        "不支持的关键字 'this'，请使用 'self' 引用当前对象");
                    lexer_next(&p->lex);  // 跳过 this
                    return NULL;
                }
            }
            // 检查是否是 packed/align cstruct 定义
            // packed 和 align 是上下文关键字，仅在 cstruct 前缀位置才特殊
            if (is_cstruct_layout_attr(p)) {
                stmt = parse_cstruct_stmt(p);
                break;
            }
            // 检查是否是入口函数定义（如 main() { }）
            if (is_entry_function_def(p)) {
                stmt = parse_entry_func_stmt(p);
            }
            // 检查是否是自定义类型变量声明（如 Point b = ... 或 Inner2? a = ...）
            // 当前 token 是标识符，且下一个 token 也是标识符（或 ? 后跟标识符）
            else if (p->lex.current.type == TOK_IDENT) {
                // 预读检查下一个 token
                Lexer saved = p->lex;
                lexer_next(&p->lex);
                int is_type_decl = 0;
                if (p->lex.current.type == TOK_IDENT) {
                    // Type varName = ...
                    is_type_decl = 1;
                } else if (p->lex.current.type == TOK_QUESTION) {
                    // Type? varName = ... （可空类型声明）
                    lexer_next(&p->lex);
                    is_type_decl = (p->lex.current.type == TOK_IDENT);
                } else if (p->lex.current.type == TOK_LBRACKET) {
                    // Type[T] varName = ... （泛型类型声明，如 Array[int] arr）
                    // 需要找到匹配的 ] 后检查是否跟标识符
                    int bracket_depth = 1;
                    lexer_next(&p->lex);
                    while (p->lex.current.type != TOK_EOF && bracket_depth > 0) {
                        if (p->lex.current.type == TOK_LBRACKET) bracket_depth++;
                        else if (p->lex.current.type == TOK_RBRACKET) bracket_depth--;
                        if (bracket_depth > 0) lexer_next(&p->lex);
                    }
                    if (bracket_depth == 0) {
                        lexer_next(&p->lex);
                        if (p->lex.current.type == TOK_IDENT) {
                            is_type_decl = 1;
                        } else if (p->lex.current.type == TOK_QUESTION) {
                            // Type[T]? varName = ...
                            lexer_next(&p->lex);
                            is_type_decl = (p->lex.current.type == TOK_IDENT);
                        }
                    }
                }
                p->lex = saved; // 恢复 lexer 状态
                error_set_column(saved.current.column);  // 恢复列号

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
        // 错误恢复：如果解析语句失败，同步到下一个语句开始位置
        // 这样可以收集更多错误而不是遇到第一个错误就停止
        if (!stmt && p->lex.current.type != TOK_EOF) {
            parser_synchronize(p);
            // 在顶层，消费碰到的分号和大括号（它们是分隔符，不属于任何语句）
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
    p->alias_names = NULL;
    p->alias_types = NULL;
    p->alias_type_params = NULL;
    p->alias_type_param_counts = NULL;
    p->alias_count = 0;
    p->alias_capacity = 0;
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

// 添加别名到表
void add_alias(Parser* p, const char* name, TypeInfo* type) {
    add_alias_with_params(p, name, type, 0, NULL);
}

void add_alias_with_params(Parser* p, const char* name, TypeInfo* type, int tp_count, char** tp_names) {
    if (p->alias_count >= p->alias_capacity) {
        p->alias_capacity = p->alias_capacity == 0 ? 8 : p->alias_capacity * 2;
        p->alias_names = realloc(p->alias_names, sizeof(char*) * p->alias_capacity);
        p->alias_types = realloc(p->alias_types, sizeof(TypeInfo*) * p->alias_capacity);
        p->alias_type_params = realloc(p->alias_type_params, sizeof(char**) * p->alias_capacity);
        p->alias_type_param_counts = realloc(p->alias_type_param_counts, sizeof(int) * p->alias_capacity);
    }
    p->alias_names[p->alias_count] = strdup(name);
    p->alias_types[p->alias_count] = type ? type_copy(type) : NULL;
    if (tp_count > 0 && tp_names) {
        p->alias_type_params[p->alias_count] = (char**)malloc(sizeof(char*) * tp_count);
        for (int i = 0; i < tp_count; i++) {
            p->alias_type_params[p->alias_count][i] = strdup(tp_names[i]);
        }
        p->alias_type_param_counts[p->alias_count] = tp_count;
    } else {
        p->alias_type_params[p->alias_count] = NULL;
        p->alias_type_param_counts[p->alias_count] = 0;
    }
    p->alias_count++;
}

// 查找别名
TypeInfo* find_alias(Parser* p, const char* name) {
    for (int i = 0; i < p->alias_count; i++) {
        if (strcmp(p->alias_names[i], name) == 0) {
            return p->alias_types[i];
        }
    }
    return NULL;
}

// 查找别名并获取类型参数信息
TypeInfo* find_alias_with_params(Parser* p, const char* name, char*** out_params, int* out_param_count) {
    for (int i = 0; i < p->alias_count; i++) {
        if (strcmp(p->alias_names[i], name) == 0) {
            if (out_params) *out_params = p->alias_type_params[i];
            if (out_param_count) *out_param_count = p->alias_type_param_counts[i];
            return p->alias_types[i];
        }
    }
    return NULL;
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
            error_set_column(saved.current.column);  // 恢复列号
}

// 执行解析
int parser_parse(Parser* p) {
    // 记录解析前的错误数量，只关注本次解析新增的错误
    // 避免模块编译时，前序模块的语义错误导致后续模块的 parser_parse 误判为失败
    int errors_before = errors.count;

    // 预扫描收集 struct 定义，支持前向引用
    prescan_struct_definitions(p);

    p->root = parse_program_internal(p);
    return (errors.count > errors_before) ? -1 : 0;
}
