#include "parser_internal.h"

// ============================================================================
// 类型解析 - 支持泛型语法 Array[T], Dict[K,V]
// ============================================================================

// 前向声明
static TypeInfo* parse_type_internal(Parser* p);

// 解析基础类型 (int, float, string, bool, var, null) 或自定义 struct 类型
static TypeInfo* parse_base_type(Parser* p) {
    if (p->lex.current.type == TOK_INT_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_INT);
    }
    if (p->lex.current.type == TOK_FLOAT_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_FLOAT);
    }
    if (p->lex.current.type == TOK_STRING_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_STRING);
    }
    if (p->lex.current.type == TOK_BOOL_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_BOOL);
    }
    if (p->lex.current.type == TOK_VAR) {
        lexer_next(&p->lex);
        return type_new(TYPE_INFER);
    }
    if (p->lex.current.type == TOK_NULL) {
        lexer_next(&p->lex);
        return type_new(TYPE_NULL);
    }
    if (p->lex.current.type == TOK_ANY_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_ANY);
    }
    // TOK_BINT 已移除：对外统一用 int，Bint 不再作为独立类型关键字
    // if (p->lex.current.type == TOK_BINT) {
    //     lexer_next(&p->lex);
    //     return type_new(TYPE_BIGINT);
    // }
    if (p->lex.current.type == TOK_FILE_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_FILE);
    }
    if (p->lex.current.type == TOK_WIN_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_WIN);
    }
    if (p->lex.current.type == TOK_DRAW_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_DRAW);
    }
    if (p->lex.current.type == TOK_EVENT_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_EVENT);
    }
    if (p->lex.current.type == TOK_RGB_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_RGB);
    }
    if (p->lex.current.type == TOK_IMAGE_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_IMAGE);
    }
    if (p->lex.current.type == TOK_SOCKET_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_SOCKET);
    }
    if (p->lex.current.type == TOK_FONT_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_FONT);
    }
    if (p->lex.current.type == TOK_BUTTON_TYPE) {
        lexer_next(&p->lex);
        return type_new(TYPE_BUTTON);
    }
    if (p->lex.current.type == TOK_PTR_TYPE) {
        lexer_next(&p->lex);
        // 检查是否有泛型参数 Ptr[T]
        if (p->lex.current.type == TOK_LBRACKET) {
            lexer_next(&p->lex); // 消费 '['
            TypeInfo* element_type = parse_type_internal(p);
            if (!element_type) {
                error_add(ERR_SYNTAX, p->lex.current.line, "Ptr 元素类型解析失败");
                return type_ptr_generic(type_new(TYPE_ANY));
            }
            if (p->lex.current.type != TOK_RBRACKET) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望 ']' 结束 Ptr 类型");
                type_free(element_type);
                return type_ptr_generic(type_new(TYPE_ANY));
            }
            lexer_next(&p->lex); // 消费 ']'
            return type_ptr_generic(element_type);
        }
        return type_new(TYPE_PTR);
    }
    // C 布局类型
    if (p->lex.current.type == TOK_I8) {
        lexer_next(&p->lex);
        return type_new(TYPE_I8);
    }
    if (p->lex.current.type == TOK_U8) {
        lexer_next(&p->lex);
        return type_new(TYPE_U8);
    }
    if (p->lex.current.type == TOK_I16) {
        lexer_next(&p->lex);
        return type_new(TYPE_I16);
    }
    if (p->lex.current.type == TOK_U16) {
        lexer_next(&p->lex);
        return type_new(TYPE_U16);
    }
    if (p->lex.current.type == TOK_I32) {
        lexer_next(&p->lex);
        return type_new(TYPE_I32);
    }
    if (p->lex.current.type == TOK_U32) {
        lexer_next(&p->lex);
        return type_new(TYPE_U32);
    }
    if (p->lex.current.type == TOK_I64) {
        lexer_next(&p->lex);
        return type_new(TYPE_I64);
    }
    if (p->lex.current.type == TOK_U64) {
        lexer_next(&p->lex);
        return type_new(TYPE_U64);
    }
    if (p->lex.current.type == TOK_STR8) {
        lexer_next(&p->lex);
        return type_new(TYPE_STR8);
    }
    if (p->lex.current.type == TOK_STR16) {
        lexer_next(&p->lex);
        return type_new(TYPE_STR16);
    }
    if (p->lex.current.type == TOK_F32) {
        lexer_next(&p->lex);
        return type_new(TYPE_F32);
    }
    if (p->lex.current.type == TOK_F64) {
        lexer_next(&p->lex);
        return type_new(TYPE_F64);
    }
    if (p->lex.current.type == TOK_C_INT) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_INT);
    }
    if (p->lex.current.type == TOK_C_UINT) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_UINT);
    }
    if (p->lex.current.type == TOK_C_LONG) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_LONG);
    }
    if (p->lex.current.type == TOK_C_ULONG) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_ULONG);
    }
    if (p->lex.current.type == TOK_C_LONGLONG) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_LONGLONG);
    }
    if (p->lex.current.type == TOK_C_ULONGLONG) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_ULONGLONG);
    }
    if (p->lex.current.type == TOK_C_SIZE) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_SIZE);
    }
    if (p->lex.current.type == TOK_C_SSIZE) {
        lexer_next(&p->lex);
        return type_new(TYPE_C_SSIZE);
    }
    // 支持自定义 struct 类型：标识符作为类型名
    if (p->lex.current.type == TOK_IDENT) {
        char* type_name = copy_string(p->lex.current.text, p->lex.current.len);
        
        // 检查是否是拼写错误的类型名（大小写不敏感比较）
        // 使用 strcmp 模拟大小写不敏感比较
        int is_dict = (strcmp(type_name, "dict") == 0 || strcmp(type_name, "Dict") == 0 || strcmp(type_name, "DICT") == 0);
        int is_array = (strcmp(type_name, "array") == 0 || strcmp(type_name, "Array") == 0 || strcmp(type_name, "ARRAY") == 0);
        int is_int = (strcmp(type_name, "int") == 0 || strcmp(type_name, "Int") == 0 || strcmp(type_name, "INT") == 0);
        int is_float = (strcmp(type_name, "float") == 0 || strcmp(type_name, "Float") == 0 || strcmp(type_name, "FLOAT") == 0);
        int is_string = (strcmp(type_name, "string") == 0 || strcmp(type_name, "String") == 0 || strcmp(type_name, "STRING") == 0);
        int is_bool = (strcmp(type_name, "bool") == 0 || strcmp(type_name, "Bool") == 0 || strcmp(type_name, "BOOL") == 0);
        int is_any = (strcmp(type_name, "any") == 0 || strcmp(type_name, "Any") == 0 || strcmp(type_name, "ANY") == 0);
        int is_var = (strcmp(type_name, "var") == 0 || strcmp(type_name, "Var") == 0 || strcmp(type_name, "VAR") == 0);
        int is_null = (strcmp(type_name, "null") == 0 || strcmp(type_name, "Null") == 0 || strcmp(type_name, "NULL") == 0);
        int is_file = (strcmp(type_name, "file") == 0 || strcmp(type_name, "File") == 0 || strcmp(type_name, "FILE") == 0);
        int is_ptr = (strcmp(type_name, "ptr") == 0 || strcmp(type_name, "Ptr") == 0 || strcmp(type_name, "PTR") == 0);
        
        if (is_dict || is_array || is_int || is_float || is_string || is_bool || is_any || is_var || is_null || is_file || is_ptr) {
            char msg[BUFFER_MEDIUM];
            const char* correct_name = type_name;
            if (is_dict) correct_name = "Dict";
            else if (is_array) correct_name = "Array";
            else if (is_int) correct_name = "int";
            else if (is_float) correct_name = "float";
            else if (is_string) correct_name = "string";
            else if (is_bool) correct_name = "bool";
            else if (is_any) correct_name = "any";
            else if (is_var) correct_name = "var";
            else if (is_null) correct_name = "null";
            else if (is_file) correct_name = "File";
            else if (is_ptr) correct_name = "Ptr";
            
            snprintf(msg, sizeof(msg), "未知类型 '%s'，您是否想使用 '%s'？", 
                     type_name, correct_name);
            error_add(ERR_SYNTAX, p->lex.current.line, msg);
            free(type_name);
            lexer_next(&p->lex);
            return type_new(TYPE_ANY); // 返回 any 类型继续解析
        }
        
        lexer_next(&p->lex);
        
        TypeInfo* struct_type;
        if (face_def_find(type_name)) {
            struct_type = type_new(TYPE_FACE);
        } else {
            struct_type = type_new(TYPE_STRUCT);
        }
        struct_type->struct_name = type_name;
        return struct_type;
    }
    return NULL;
}

// 解析泛型参数列表 (用于 Dict[K, V])
static TypeInfo* parse_dict_type(Parser* p) {
    lexer_next(&p->lex); // 消费 'Dict'
    
    if (p->lex.current.type != TOK_LBRACKET) {
        // Dict 不带参数，类型未指定（NULL表示），可以接受任何 Dict 赋值
        return type_dict(NULL, NULL);
    }
    
    lexer_next(&p->lex); // 消费 '['
    
    // 解析键类型
    TypeInfo* key_type = parse_type_internal(p);
    if (!key_type) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Dict 键类型解析失败");
        return type_dict(type_new(TYPE_ANY), type_new(TYPE_ANY));
    }
    
    if (p->lex.current.type != TOK_COMMA) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Dict 类型需要两个参数: Dict[KeyType, ValueType]");
        type_free(key_type);
        return type_dict(type_new(TYPE_ANY), type_new(TYPE_ANY));
    }
    lexer_next(&p->lex); // 消费 ','
    
    // 解析值类型
    TypeInfo* value_type = parse_type_internal(p);
    if (!value_type) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Dict 值类型解析失败");
        type_free(key_type);
        return type_dict(type_new(TYPE_ANY), type_new(TYPE_ANY));
    }
    
    if (p->lex.current.type != TOK_RBRACKET) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 ']' 结束 Dict 类型");
        type_free(key_type);
        type_free(value_type);
        return type_dict(type_new(TYPE_ANY), type_new(TYPE_ANY));
    }
    lexer_next(&p->lex); // 消费 ']'
    
    return type_dict(key_type, value_type);
}

// 解析 Array 类型
static TypeInfo* parse_array_type(Parser* p) {
    lexer_next(&p->lex); // 消费 'Array'
    
    if (p->lex.current.type != TOK_LBRACKET) {
        // Array 不带参数，元素类型未指定（NULL），可以接受任何具体类型
        return type_array(NULL);
    }
    
    lexer_next(&p->lex); // 消费 '['
    
    // 解析元素类型（递归支持 Array[Array[int]]）
    TypeInfo* element_type = parse_type_internal(p);
    if (!element_type) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Array 元素类型解析失败");
        return type_array(type_new(TYPE_ANY));
    }
    
    if (p->lex.current.type != TOK_RBRACKET) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 ']' 结束 Array 类型");
        type_free(element_type);
        return type_array(type_new(TYPE_ANY));
    }
    lexer_next(&p->lex); // 消费 ']'
    
    return type_array(element_type);
}

// 解析 Style 类型: Style[target]
static TypeInfo* parse_style_type(Parser* p) {
    lexer_next(&p->lex); // 消费 'Style'
    
    if (p->lex.current.type != TOK_LBRACKET) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Style 类型需要指定目标控件: Style[window] 或 Style[button]");
        return type_style("");
    }
    
    lexer_next(&p->lex); // 消费 '['
    
    // 解析目标控件名（标识符）
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "Style 类型需要有效的控件名称，如 window 或 button");
        // 尝试跳过到 ]
        while (p->lex.current.type != TOK_RBRACKET && p->lex.current.type != TOK_EOF) {
            lexer_next(&p->lex);
        }
        if (p->lex.current.type == TOK_RBRACKET) {
            lexer_next(&p->lex);
        }
        return type_style("");
    }
    
    char* target_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex); // 消费标识符
    
    if (p->lex.current.type != TOK_RBRACKET) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 ']' 结束 Style 类型");
        free(target_name);
        return type_style("");
    }
    lexer_next(&p->lex); // 消费 ']'
    
    return type_style(target_name);
}

// 解析函数类型: func 或 func():ReturnType 或 func(ParamType1, ParamType2):ReturnType
static TypeInfo* parse_function_type(Parser* p) {
    lexer_next(&p->lex); // 消费 'func'
    
    TypeInfo* return_type = NULL;
    TypeInfo** param_types = NULL;
    int param_count = 0;
    int param_capacity = 8;
    
    // 检查是否有参数列表
    if (p->lex.current.type == TOK_LPAREN) {
        lexer_next(&p->lex); // 消费 '('
        
        // 解析参数类型列表
        if (p->lex.current.type != TOK_RPAREN) {
            param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_capacity);
            
            do {
                TypeInfo* param_type = parse_type_internal(p);
                if (!param_type) {
                    error_add(ERR_SYNTAX, p->lex.current.line, "期望参数类型");
                    break;
                }
                
                // 扩容检查
                if (param_count >= param_capacity) {
                    param_capacity *= 2;
                    param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_capacity);
                }
                
                param_types[param_count++] = param_type;
            } while (match(p, TOK_COMMA));
        }
        
        consume(p, TOK_RPAREN, "期望 ')'");
        
        // 解析可选的返回类型
        if (p->lex.current.type == TOK_COLON) {
            lexer_next(&p->lex); // 消费 ':'
            return_type = parse_type_internal(p);
            if (!return_type) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望返回类型");
                return_type = type_new(TYPE_ANY);
            }
        }
    }
    
    return type_function(return_type, param_types, param_count);
}

// 内部类型解析 - 支持泛型
static TypeInfo* parse_type_internal(Parser* p) {
    // 尝试解析函数类型
    if (p->lex.current.type == TOK_FUNC) {
        return parse_function_type(p);
    }
    
    // 尝试解析 Array[T]
    if (p->lex.current.type == TOK_ARRAY_TYPE) {
        return parse_array_type(p);
    }
    
    // 尝试解析 Dict[K,V]
    if (p->lex.current.type == TOK_DICT_TYPE) {
        return parse_dict_type(p);
    }
    
    // 尝试解析 Style[target]
    if (p->lex.current.type == TOK_STYLE_TYPE) {
        return parse_style_type(p);
    }
    
    // 解析基础类型
    return parse_base_type(p);
}

// 公共接口：解析类型
TypeInfo* parse_type(Parser* p) {
    return parse_type_internal(p);
}

// ============================================================================
// 变量声明解析 - 支持新类型语法
// ============================================================================

Ast* parse_var_decl_internal(Parser* p) {
    int line = p->lex.current.line;
    TypeInfo* shared_type = NULL;
    
    // 解析类型
    shared_type = parse_type(p);
    if (!shared_type) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望类型 (int, float, string, bool, Array, Dict, var)");
        return NULL;
    }
    
    // 解析第一个变量名
    if (p->lex.current.type != TOK_IDENT) {
        if (is_type_keyword(p->lex.current.type)) {
            char msg[64];
            snprintf(msg, sizeof(msg), "不能使用关键字(%.*s)作为变量名", 
                     p->lex.current.len, p->lex.current.text);
            error_add(ERR_SYNTAX, p->lex.current.line, msg);
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望变量名");
        }
        type_free(shared_type);
        return NULL;
    }
    
    // 使用动态数组存储变量声明
    Ast** decls = NULL;
    int decl_count = 0;
    int decl_capacity = 4;
    decls = (Ast**)malloc(sizeof(Ast*) * decl_capacity);
    
    do {
        // 解析变量名
        char* name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);
        
        // 复制类型
        TypeInfo* var_type = type_copy(shared_type);
        
        // 解析可选的初始值
        Ast* init = NULL;
        if (match(p, TOK_EQ)) {
            init = parse_expression(p);
        }
        
        // 创建变量声明节点
        Ast* ast = ast_new(AST_VAR_DECL, line);
        ast->u.var_decl.name = name;
        ast->u.var_decl.init = init;
        ast->u.var_decl.type = var_type;
        
        // 添加到数组
        if (decl_count >= decl_capacity) {
            decl_capacity *= 2;
            decls = (Ast**)realloc(decls, sizeof(Ast*) * decl_capacity);
        }
        decls[decl_count++] = ast;
        
    } while (match(p, TOK_COMMA)); // 如果有逗号，继续解析下一个变量
    
    type_free(shared_type);
    
    // 如果只有一个变量，直接返回
    if (decl_count == 1) {
        Ast* result = decls[0];
        free(decls);
        return result;
    }
    
    // 多个变量：返回一个 block 包含所有声明
    Ast* block = ast_new(AST_BLOCK, line);
    ast_list_init(&block->u.block);
    for (int i = 0; i < decl_count; i++) {
        ast_list_add(&block->u.block, decls[i]);
    }
    free(decls);
    return block;
}

// ============================================================================
// 函数定义解析
// ============================================================================

// 解析函数体和创建函数定义 AST
Ast* parse_func_body_and_create(Parser* p, char* name, int line) {
    // 解析泛型类型参数: func name[T, U](...)
    char** type_params = NULL;
    int type_param_count = 0;
    if (p->lex.current.type == TOK_LBRACKET) {
        lexer_next(&p->lex);  // 跳过 '['
        type_params = (char**)malloc(sizeof(char*) * 8);
        int tp_capacity = 8;
        
        do {
            if (type_param_count >= tp_capacity) {
                tp_capacity *= 2;
                type_params = (char**)realloc(type_params, sizeof(char*) * tp_capacity);
            }
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望类型参数名");
                break;
            }
            type_params[type_param_count] = copy_string(p->lex.current.text, p->lex.current.len);
            type_param_count++;
            lexer_next(&p->lex);
        } while (match(p, TOK_COMMA));
        
        consume(p, TOK_RBRACKET, "期望 ']'");
    }
    
    consume(p, TOK_LPAREN, "期望 '('");

    // 参数列表 - 新设计：每个参数有自己的完整类型
    char** params = NULL;
    TypeInfo** param_types = NULL;  // 改为 TypeInfo* 数组
    Ast** param_defaults = NULL;    // 参数默认值表达式数组
    int pcnt = 0;
    int param_capacity = 16;

    if (p->lex.current.type != TOK_RPAREN) {
        params = (char**)malloc(sizeof(char*) * param_capacity);
        param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_capacity);
        param_defaults = (Ast**)malloc(sizeof(Ast*) * param_capacity);
        // 初始化为 NULL
        memset(param_defaults, 0, sizeof(Ast*) * param_capacity);
        
        do {
            // 解析参数类型
            TypeInfo* param_type = parse_type(p);
            if (!param_type) {
                // 检查是否是省略了 var 的情况（如 func test(n)）
                if (p->lex.current.type == TOK_IDENT) {
                    error_add(ERR_SYNTAX, p->lex.current.line, 
                        "函数参数需要使用关键字，例如：func test(var a)");
                } else {
                    error_add(ERR_SYNTAX, p->lex.current.line, 
                        "期望参数类型（如 var, int, float, string, bool）");
                }
                break;
            }

            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望参数名");
                type_free(param_type);
                break;
            }
            
            // 扩容检查
            if (pcnt >= param_capacity) {
                param_capacity *= 2;
                params = (char**)realloc(params, sizeof(char*) * param_capacity);
                param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_capacity);
                param_defaults = (Ast**)realloc(param_defaults, sizeof(Ast*) * param_capacity);
                // 新分配的空间初始化为 NULL
                memset(&param_defaults[pcnt], 0, sizeof(Ast*) * (param_capacity - pcnt));
            }
            
            params[pcnt] = copy_string(p->lex.current.text, p->lex.current.len);
            param_types[pcnt] = param_type;
            
            lexer_next(&p->lex);
            
            // 解析可选的默认值: 类型 名 = 默认值
            if (match(p, TOK_EQ)) {
                // 解析默认值表达式
                param_defaults[pcnt] = parse_expression(p);
            } else {
                param_defaults[pcnt] = NULL;
            }
            
            pcnt++;
        } while (match(p, TOK_COMMA));
    }

    consume(p, TOK_RPAREN, "期望 ')'");
    
    // 支持可选的返回类型注解: func test(): Array[int] { }
    TypeInfo* return_type = type_new(TYPE_INFER);  // 默认为推断类型
    
    if (p->lex.current.type == TOK_COLON) {
        lexer_next(&p->lex);
        
        // 解析返回类型
        TypeInfo* parsed_return = parse_type(p);
        if (parsed_return) {
            type_free(return_type);
            return_type = parsed_return;
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望返回类型");
        }
    }

    Ast* body = parse_block_internal(p);

    Ast* ast = ast_new(AST_FUNC_DEF, line);
    ast->u.func.name = name;
    ast->u.func.params = params;
    ast->u.func.param_types = param_types;
    ast->u.func.param_defaults = param_defaults;
    ast->u.func.pcnt = pcnt;
    ast->u.func.return_type = return_type;
    ast->u.func.body = body;
    ast->u.func.type_params = type_params;
    ast->u.func.type_param_count = type_param_count;
    
    // 统计有默认值的参数数量
    ast->u.func.default_count = 0;
    for (int i = 0; i < pcnt; i++) {
        if (param_defaults && param_defaults[i] != NULL) {
            ast->u.func.default_count++;
        }
    }
    
    return ast;
}

// 解析普通函数定义（带 func 关键字）
Ast* parse_func_stmt(Parser* p) {
    int line = p->lex.current.line;
    int is_async = 0;
    
    // 检查是否是 async 函数
    if (p->lex.current.type == TOK_ASYNC) {
        is_async = 1;
        lexer_next(&p->lex); // async
        
        // async 后面必须是 func
        if (p->lex.current.type != TOK_FUNC) {
            error_add(ERR_SYNTAX, p->lex.current.line, "async 后面必须是 func 关键字");
            return NULL;
        }
    }
    
    lexer_next(&p->lex); // func

    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望函数名");
        return NULL;
    }

    char* name = copy_string(p->lex.current.text, p->lex.current.len);
    
    // 检查是否是入口函数（如 main），入口函数不需要 func 关键字
    if (strcmp(name, "main") == 0) {
        error_add(ERR_SYNTAX, p->lex.current.line, "入口函数 main 不需要 func 关键字，直接使用 main() { ... }");
        free(name);
        return NULL;
    }
    
    // main 函数不能是 async
    if (is_async && strcmp(name, "main") == 0) {
        error_add(ERR_SYNTAX, p->lex.current.line, "main 函数不能是 async 函数，请在 main 中启动协程并调用 async.run()");
        free(name);
        return NULL;
    }
    
    lexer_next(&p->lex);

    Ast* ast = parse_func_body_and_create(p, name, line);
    if (ast) {
        ast->u.func.is_async = is_async;
    }
    return ast;
}

// ============================================================================
// 入口函数检测和解析（main() {} 这种省略 func 关键字的函数）
// ============================================================================

// 检查是否是入口函数定义（如 main() { }，没有 func 关键字）
// 模式：main ( ) {
// 只有 main 函数可以省略 func 关键字
int is_entry_function_def(Parser* p) {
    // 当前必须是标识符 "main"
    if (p->lex.current.type != TOK_IDENT) return 0;
    
    // 检查是否是 main 函数名
    if (strncmp(p->lex.current.text, "main", p->lex.current.len) != 0 ||
        p->lex.current.len != 4) {
        return 0;
    }

    // 保存当前状态，用于预读
    Lexer saved_lex = p->lex;

    // 预读：跳过标识符
    lexer_next(&p->lex);

    // 检查下一个 token 是否是 '('
    if (p->lex.current.type != TOK_LPAREN) {
        // 恢复状态
        p->lex = saved_lex;
        return 0;
    }

    // 预读：跳过 '('
    lexer_next(&p->lex);

    // 检查是否是 ')'
    if (p->lex.current.type != TOK_RPAREN) {
        // 恢复状态
        p->lex = saved_lex;
        return 0;
    }

    // 预读：跳过 ')'
    lexer_next(&p->lex);

    // 检查是否是 '{'
    int result = (p->lex.current.type == TOK_LBRACE);

    // 恢复状态
    p->lex = saved_lex;
    return result;
}

// 解析入口函数定义（如 main() { }）
Ast* parse_entry_func_stmt(Parser* p) {
    int line = p->lex.current.line;

    // 获取函数名
    char* name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex); // 消费标识符

    return parse_func_body_and_create(p, name, line);
}

// ============================================================================
// 匿名函数解析（用于表达式中的 func() {}）
// ============================================================================

Ast* parse_anonymous_func(Parser* p) {
    int line = p->lex.current.line;
    
    // 消费 func 关键字
    lexer_next(&p->lex);
    
    // 匿名函数使用空字符串作为名称（或生成唯一名称）
    char* name = copy_string("<anonymous>", 11);
    
    return parse_func_body_and_create(p, name, line);
}

// ============================================================================
// 表达式语句解析
// ============================================================================

// 前向声明
Ast* parse_expression(Parser* p);

// 辅助函数：检查 AST 节点是否是合法的赋值目标
static int is_valid_assign_target(Ast* ast) {
    return ast && (ast->kind == AST_VAR || ast->kind == AST_INDEX);
}

// 辅助函数：释放赋值目标列表
static void free_assign_targets(Ast** targets, int count) {
    for (int i = 0; i < count; i++) {
        ast_free(targets[i]);
    }
    free(targets);
}

Ast* parse_expression_stmt(Parser* p) {
    int line = p->lex.current.line;
    
    // 检查是否是并行赋值：a, b = c, d 或 arr[0], arr[4] = x, y
    Ast** left_targets = NULL;
    int left_count = 0;
    int left_capacity = 8;
    
    // 保存当前位置，以便失败时恢复
    Lexer save_lex = p->lex;
    
    // 尝试解析第一个赋值目标（变量或索引表达式）
    if (p->lex.current.type == TOK_IDENT) {
        // 调用 parse_call_expression 来解析完整的表达式（可能是索引如 arr[0]）
        // 使用 parse_call_expression 而不是 parse_expression，避免把 '=' 当作运算符
        Ast* first_target = parse_call_expression(p);
        
        // 检查是否是合法的赋值目标
        if (!is_valid_assign_target(first_target)) {
            // 不是合法的赋值目标，回退并作为普通表达式解析
            if (first_target) ast_free(first_target);
            p->lex = save_lex;
            goto normal_parse;
        }
        
        left_targets = (Ast**)malloc(sizeof(Ast*) * left_capacity);
        left_targets[left_count++] = first_target;
        
        // 检查是否有更多赋值目标（逗号分隔）
        while (p->lex.current.type == TOK_COMMA) {
            lexer_next(&p->lex); // 消费 ','
            
            if (p->lex.current.type != TOK_IDENT) {
                // 不是标识符开头，回退
                free_assign_targets(left_targets, left_count);
                p->lex = save_lex;
                goto normal_parse;
            }
            
            // 解析下一个赋值目标
            Ast* next_target = parse_call_expression(p);
            
            if (!is_valid_assign_target(next_target)) {
                // 不是合法的赋值目标，回退
                free_assign_targets(left_targets, left_count);
                if (next_target) ast_free(next_target);
                p->lex = save_lex;
                goto normal_parse;
            }
            
            if (left_count >= left_capacity) {
                left_capacity *= 2;
                left_targets = (Ast**)realloc(left_targets, sizeof(Ast*) * left_capacity);
            }
            
            left_targets[left_count++] = next_target;
        }
        
        // 检查是否是赋值操作
        if (p->lex.current.type == TOK_EQ || p->lex.current.type == TOK_PLUSEQ ||
            p->lex.current.type == TOK_MINUSEQ || p->lex.current.type == TOK_STAREQ ||
            p->lex.current.type == TOK_SLASHEQ || p->lex.current.type == TOK_MODEQ ||
            p->lex.current.type == TOK_BITANDEQ || p->lex.current.type == TOK_BITOREQ ||
            p->lex.current.type == TOK_BITXOREQ || p->lex.current.type == TOK_SHLEQ ||
            p->lex.current.type == TOK_SHREQ || p->lex.current.type == TOK_USHREQ) {
            
            // 是赋值语句
            LenoTokenType op = p->lex.current.type;
            
            // 处理复合赋值运算符 (+= -= *= /= %= &= |= ^= <<= >>= >>>=)
            if (op == TOK_PLUSEQ || op == TOK_MINUSEQ || op == TOK_STAREQ || op == TOK_SLASHEQ ||
                op == TOK_MODEQ || op == TOK_BITANDEQ || op == TOK_BITOREQ || op == TOK_BITXOREQ ||
                op == TOK_SHLEQ || op == TOK_SHREQ || op == TOK_USHREQ) {
                
                // 复合赋值只支持单个变量
                if (left_count != 1) {
                    error_add(ERR_SYNTAX, line, "复合赋值只支持单个变量");
                    return NULL;
                }
                
                // 复合赋值只支持简单变量，不支持索引
                if (left_targets[0]->kind != AST_VAR) {
                    error_add(ERR_SYNTAX, line, "复合赋值只支持简单变量");
                    return NULL;
                }
                
                lexer_next(&p->lex); // 消费复合赋值运算符
                
                // 解析右侧表达式
                Ast* value = parse_expression(p);
                
                // 创建复合赋值节点
                Ast* ast = ast_new(AST_COMPOUND_ASSIGN, line);
                ast->u.compound_assign.name = strdup(left_targets[0]->u.var.name);
                ast->u.compound_assign.value = value;
                ast->u.compound_assign.op = op;
                // ref 信息在语义分析时填充
                
                // 释放临时变量节点
                free(left_targets[0]->u.var.name);
                free(left_targets[0]);
                free(left_targets);
                
                // 包装成表达式语句
                Ast* expr_stmt = ast_new(AST_EXPR_STMT, line);
                expr_stmt->u.expr_stmt.expr = ast;
                return expr_stmt;
            }
            
            // 普通赋值 (=)
            lexer_next(&p->lex); // 消费 '='
            
            // 收集右侧表达式
            Ast** right_exprs = (Ast**)malloc(sizeof(Ast*) * left_count);
            int right_count = 0;
            
            // 解析第一个右侧表达式
            Ast* first_expr = parse_expression(p);
            if (first_expr) {
                right_exprs[right_count++] = first_expr;
            }
            
            // 解析更多右侧表达式
            while (p->lex.current.type == TOK_COMMA) {
                lexer_next(&p->lex); // 消费 ','
                Ast* next_expr = parse_expression(p);
                if (next_expr) {
                    if (right_count >= left_count) {
                        // 右侧表达式太多，稍后报错
                    }
                    right_exprs[right_count++] = next_expr;
                }
            }
            
            // 创建赋值节点
            Ast* assign_ast = ast_new(AST_ASSIGN, line);
            assign_ast->u.assign.names = (char**)malloc(sizeof(char*) * left_count);
            assign_ast->u.assign.name_count = left_count;
            assign_ast->u.assign.targets = left_targets;
            assign_ast->u.assign.refs = (SymRef*)calloc(left_count, sizeof(SymRef));
            
            for (int i = 0; i < left_count; i++) {
                if (left_targets[i]->kind == AST_VAR) {
                    assign_ast->u.assign.names[i] = strdup(left_targets[i]->u.var.name);
                } else {
                    // 索引表达式没有变量名，设为 NULL
                    assign_ast->u.assign.names[i] = NULL;
                }
            }
            
            // 处理右侧表达式
            if (right_count == 1) {
                assign_ast->u.assign.value = right_exprs[0];
                free(right_exprs);
            } else {
                // 多个右侧表达式，包装成数组
                Ast* arr = ast_new(AST_ARRAY, line);
                arr->u.array.items = right_exprs;
                arr->u.array.count = right_count;
                arr->u.array.capacity = right_count;
                assign_ast->u.assign.value = arr;
            }
            
            // 包装成表达式语句
            Ast* ast = ast_new(AST_EXPR_STMT, line);
            ast->u.expr_stmt.expr = assign_ast;
            return ast;
        }
        
        // 不是赋值，回退并作为普通表达式解析
        free_assign_targets(left_targets, left_count);
        p->lex = save_lex;
    }
    
normal_parse:
    // 普通表达式解析
    Ast* expr = parse_expression(p);
    
    // 如果表达式解析失败，消费当前 token 避免无限循环
    if (expr == NULL) {
        if (p->lex.current.type != TOK_EOF) {
            lexer_next(&p->lex);
        }
        return NULL;
    }
    
    Ast* ast = ast_new(AST_EXPR_STMT, line);
    ast->u.expr_stmt.expr = expr;
    return ast;
}

// ============================================================================
// struct 定义解析
// ============================================================================

Ast* parse_struct_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'struct'

    // 期望 struct 名称
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 struct 名称");
        return NULL;
    }

    char* struct_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 解析可选的 impl 声明: struct Name impl Face1, Face2 { ... }
    char** impl_names = NULL;
    int impl_count = 0;
    int impl_capacity = 4;

    if (p->lex.current.type == TOK_IMPL) {
        lexer_next(&p->lex); // 消费 'impl'
        impl_names = (char**)malloc(sizeof(char*) * impl_capacity);

        while (1) {
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望 face 名称");
                break;
            }
            char* iface_name = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);

            if (impl_count >= impl_capacity) {
                impl_capacity *= 2;
                impl_names = (char**)realloc(impl_names, sizeof(char*) * impl_capacity);
            }
            impl_names[impl_count++] = iface_name;

            if (p->lex.current.type == TOK_COMMA) {
                lexer_next(&p->lex);
            } else {
                break;
            }
        }
    }

    // 期望 '{'
    if (!consume(p, TOK_LBRACE, "期望 '{' 开始 struct 定义")) {
        free(struct_name);
        return NULL;
    }

    // 动态数组存储字段
    char** field_names = NULL;
    TypeInfo** field_types = NULL;
    Ast** field_defaults = NULL;
    int field_count = 0;
    int field_capacity = 8;

    field_names = (char**)malloc(sizeof(char*) * field_capacity);
    field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * field_capacity);
    field_defaults = (Ast**)calloc(field_capacity, sizeof(Ast*));

    // 动态数组存储方法
    Ast** methods = NULL;
    int method_count = 0;
    int method_capacity = 8;
    methods = (Ast**)malloc(sizeof(Ast*) * method_capacity);

    // 解析字段列表和方法
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        // 检查是否是注释（跳过）
        if (p->lex.current.type == TOK_ERROR) {
            lexer_next(&p->lex);
            continue;
        }

        // 检查是否是方法定义（func 或 async func）
        int is_async = 0;
        if (p->lex.current.type == TOK_ASYNC) {
            is_async = 1;
            lexer_next(&p->lex); // 消费 'async'
            // 消费完 async 后，期望 func
            if (p->lex.current.type != TOK_FUNC) {
                error_add(ERR_SYNTAX, p->lex.current.line, "async 后面必须跟 func");
                // 跳过错误恢复
                while (p->lex.current.type != TOK_SEMI &&
                       p->lex.current.type != TOK_RBRACE &&
                       p->lex.current.type != TOK_EOF) {
                    lexer_next(&p->lex);
                }
                continue;
            }
        }

        if (p->lex.current.type == TOK_FUNC) {
            int func_line = p->lex.current.line;
            lexer_next(&p->lex); // 消费 'func'

            // 期望方法名
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望方法名");
                // 跳过错误恢复
                while (p->lex.current.type != TOK_SEMI &&
                       p->lex.current.type != TOK_RBRACE &&
                       p->lex.current.type != TOK_EOF) {
                    lexer_next(&p->lex);
                }
                continue;
            }

            char* method_name = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);

            // 解析函数体
            Ast* func_ast = parse_func_body_and_create(p, method_name, func_line);
            if (func_ast) {
                // 设置 async 标志
                func_ast->u.func.is_async = is_async;
                // 扩容检查
                if (method_count >= method_capacity) {
                    method_capacity *= 2;
                    methods = (Ast**)realloc(methods, sizeof(Ast*) * method_capacity);
                }
                methods[method_count++] = func_ast;
            }

            // 可选的分号
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
            continue;
        }

        // 解析字段类型
        TypeInfo* field_type = NULL;

        // struct 字段不能使用 var，必须有具体类型
        if (p->lex.current.type == TOK_VAR) {
            error_add(ERR_SYNTAX, p->lex.current.line, "struct 字段不能使用 var，必须使用具体类型");
            lexer_next(&p->lex);
            // 尝试继续解析
        }

        field_type = parse_type(p);
        if (!field_type) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望字段类型");
            // 跳过错误恢复
            while (p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_EOF) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
            continue;
        }

        // 期望字段名
        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望字段名");
            type_free(field_type);
            break;
        }

        char* field_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        // 扩容检查
        if (field_count >= field_capacity) {
            field_capacity *= 2;
            field_names = (char**)realloc(field_names, sizeof(char*) * field_capacity);
            field_types = (TypeInfo**)realloc(field_types, sizeof(TypeInfo*) * field_capacity);
            field_defaults = (Ast**)realloc(field_defaults, sizeof(Ast*) * field_capacity);
            memset(&field_defaults[field_count], 0, sizeof(Ast*) * (field_capacity - field_count));
        }

        field_names[field_count] = field_name;
        field_types[field_count] = field_type;

        // 解析可选的默认值: 类型 名 = 默认值
        if (match(p, TOK_EQ)) {
            field_defaults[field_count] = parse_expression(p);
        }

        field_count++;

        // 可选的分号
        if (p->lex.current.type == TOK_SEMI) {
            lexer_next(&p->lex);
        }
    }

    // 期望 '}'
    if (!consume(p, TOK_RBRACE, "期望 '}' 结束 struct 定义")) {
        // 清理已分配的内存
        for (int i = 0; i < field_count; i++) {
            free(field_names[i]);
            type_free(field_types[i]);
            if (field_defaults[i]) {
                ast_free(field_defaults[i]);
            }
        }
        free(field_names);
        free(field_types);
        free(field_defaults);
        for (int i = 0; i < method_count; i++) {
            ast_free(methods[i]);
        }
        free(methods);
        free(struct_name);
        return NULL;
    }

    // 创建 struct 定义 AST 节点
    Ast* ast = ast_new(AST_STRUCT_DEF, line);
    ast->u.struct_def.name = struct_name;
    ast->u.struct_def.field_names = field_names;
    ast->u.struct_def.field_types = field_types;
    ast->u.struct_def.field_defaults = field_defaults;
    ast->u.struct_def.field_count = field_count;
    ast->u.struct_def.methods = methods;
    ast->u.struct_def.method_count = method_count;
    ast->u.struct_def.impl_names = impl_names;
    ast->u.struct_def.impl_count = impl_count;

    return ast;
}

// ============================================================================
// face 定义解析
// ============================================================================

Ast* parse_face_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'face'

    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 face 名称");
        return NULL;
    }

    char* face_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    if (!consume(p, TOK_LBRACE, "期望 '{' 开始 face 定义")) {
        free(face_name);
        return NULL;
    }

    char** method_names = NULL;
    TypeInfo** method_return_types = NULL;
    TypeInfo*** method_param_types = NULL;
    int* method_param_counts = NULL;
    int method_count = 0;
    int method_capacity = 8;

    method_names = (char**)malloc(sizeof(char*) * method_capacity);
    method_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * method_capacity);
    method_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * method_capacity);
    method_param_counts = (int*)malloc(sizeof(int) * method_capacity);

    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_ERROR) {
            lexer_next(&p->lex);
            continue;
        }

        if (p->lex.current.type != TOK_FUNC) {
            error_add(ERR_SYNTAX, p->lex.current.line, "face 中只允许方法签名");
            while (p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_EOF) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_SEMI) lexer_next(&p->lex);
            continue;
        }

        lexer_next(&p->lex); // 消费 'func'

        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望方法名");
            while (p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_EOF) {
                lexer_next(&p->lex);
            }
            continue;
        }

        char* method_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        TypeInfo** param_types = NULL;
        int param_count = 0;
        int param_capacity = 4;

        if (match(p, TOK_LPAREN)) {
            param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_capacity);
            while (p->lex.current.type != TOK_RPAREN && p->lex.current.type != TOK_EOF) {
                TypeInfo* ptype = parse_type(p);
                if (!ptype) {
                    error_add(ERR_SYNTAX, p->lex.current.line, "期望参数类型");
                    break;
                }
                if (p->lex.current.type == TOK_IDENT) {
                    lexer_next(&p->lex);
                }
                if (param_count >= param_capacity) {
                    param_capacity *= 2;
                    param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_capacity);
                }
                param_types[param_count++] = ptype;
                if (p->lex.current.type == TOK_COMMA) lexer_next(&p->lex);
            }
            consume(p, TOK_RPAREN, "期望 ')'");
        }

        TypeInfo* return_type = NULL;
        if (match(p, TOK_COLON)) {
            return_type = parse_type(p);
        }

        if (method_count >= method_capacity) {
            method_capacity *= 2;
            method_names = (char**)realloc(method_names, sizeof(char*) * method_capacity);
            method_return_types = (TypeInfo**)realloc(method_return_types, sizeof(TypeInfo*) * method_capacity);
            method_param_types = (TypeInfo***)realloc(method_param_types, sizeof(TypeInfo**) * method_capacity);
            method_param_counts = (int*)realloc(method_param_counts, sizeof(int) * method_capacity);
        }

        method_names[method_count] = method_name;
        method_return_types[method_count] = return_type;
        method_param_types[method_count] = param_types;
        method_param_counts[method_count] = param_count;
        method_count++;

        if (p->lex.current.type == TOK_SEMI) lexer_next(&p->lex);
    }

    if (!consume(p, TOK_RBRACE, "期望 '}' 结束 face 定义")) {
        for (int i = 0; i < method_count; i++) {
            free(method_names[i]);
            if (method_return_types[i]) type_free(method_return_types[i]);
            for (int j = 0; j < method_param_counts[i]; j++) {
                type_free(method_param_types[i][j]);
            }
            free(method_param_types[i]);
        }
        free(method_names);
        free(method_return_types);
        free(method_param_types);
        free(method_param_counts);
        free(face_name);
        return NULL;
    }

    Ast* ast = ast_new(AST_FACE_DEF, line);
    ast->u.face_def.name = face_name;
    ast->u.face_def.method_names = method_names;
    ast->u.face_def.method_return_types = method_return_types;
    ast->u.face_def.method_param_types = method_param_types;
    ast->u.face_def.method_param_counts = method_param_counts;
    ast->u.face_def.method_count = method_count;

    return ast;
}

// ============================================================================
// cstruct 定义解析（C 布局结构体）
// ============================================================================

Ast* parse_cstruct_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'cstruct'

    // 期望 cstruct 名称
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 cstruct 名称");
        return NULL;
    }

    char* cstruct_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 期望 '{'
    if (!consume(p, TOK_LBRACE, "期望 '{' 开始 cstruct 定义")) {
        free(cstruct_name);
        return NULL;
    }

    // 动态数组存储字段
    char** field_names = NULL;
    TypeInfo** field_types = NULL;
    int* field_array_dims = NULL;
    int field_count = 0;
    int field_capacity = 8;

    field_names = (char**)malloc(sizeof(char*) * field_capacity);
    field_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * field_capacity);
    field_array_dims = (int*)malloc(sizeof(int) * field_capacity);

    // 解析字段列表（cstruct 不支持方法）
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        // 检查是否是注释（跳过）
        if (p->lex.current.type == TOK_ERROR) {
            lexer_next(&p->lex);
            continue;
        }

        // cstruct 不支持方法
        if (p->lex.current.type == TOK_FUNC || p->lex.current.type == TOK_ASYNC) {
            error_add(ERR_SYNTAX, p->lex.current.line, "cstruct 不支持方法定义，请使用普通 struct");
            // 跳过错误恢复
            while (p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_EOF) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
            continue;
        }

        // 解析字段类型
        TypeInfo* field_type = NULL;

        // cstruct 字段不能使用 var，必须有具体类型
        if (p->lex.current.type == TOK_VAR) {
            error_add(ERR_SYNTAX, p->lex.current.line, "cstruct 字段不能使用 var，必须使用 C 布局类型");
            lexer_next(&p->lex);
            // 尝试继续解析
        }

        field_type = parse_type(p);
        if (!field_type) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望字段类型");
            // 跳过错误恢复
            while (p->lex.current.type != TOK_SEMI &&
                   p->lex.current.type != TOK_RBRACE &&
                   p->lex.current.type != TOK_EOF) {
                lexer_next(&p->lex);
            }
            if (p->lex.current.type == TOK_SEMI) {
                lexer_next(&p->lex);
            }
            continue;
        }

        // 期望字段名
        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望字段名");
            type_free(field_type);
            break;
        }

        char* field_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        // 解析可选的数组维度 [N]
        int array_dim = 0;
        if (p->lex.current.type == TOK_LBRACKET) {
            lexer_next(&p->lex); // 消费 '['
            
            if (p->lex.current.type != TOK_NUM) {
                error_add(ERR_SYNTAX, p->lex.current.line, "数组维度必须是整数常量");
                free(field_name);
                type_free(field_type);
                break;
            }
            
            // 检查是否是整数
            if (p->lex.current.is_float) {
                error_add(ERR_SYNTAX, p->lex.current.line, "数组维度必须是整数，不能是浮点数");
                free(field_name);
                type_free(field_type);
                break;
            }
            
            array_dim = (int)p->lex.current.num_val;
            if (array_dim <= 0) {
                error_add(ERR_SYNTAX, p->lex.current.line, "数组维度必须是正整数");
                free(field_name);
                type_free(field_type);
                break;
            }
            
            lexer_next(&p->lex); // 消费数字
            
            if (!consume(p, TOK_RBRACKET, "期望 ']' 结束数组维度")) {
                free(field_name);
                type_free(field_type);
                break;
            }
        }

        // 扩容检查
        if (field_count >= field_capacity) {
            field_capacity *= 2;
            field_names = (char**)realloc(field_names, sizeof(char*) * field_capacity);
            field_types = (TypeInfo**)realloc(field_types, sizeof(TypeInfo*) * field_capacity);
            field_array_dims = (int*)realloc(field_array_dims, sizeof(int) * field_capacity);
        }

        field_names[field_count] = field_name;
        field_types[field_count] = field_type;
        field_array_dims[field_count] = array_dim;
        field_count++;

        // 可选的分号
        if (p->lex.current.type == TOK_SEMI) {
            lexer_next(&p->lex);
        }
    }

    // 期望 '}'
    if (!consume(p, TOK_RBRACE, "期望 '}' 结束 cstruct 定义")) {
        // 清理已分配的内存
        for (int i = 0; i < field_count; i++) {
            free(field_names[i]);
            type_free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        free(field_array_dims);
        free(cstruct_name);
        return NULL;
    }

    // 创建 cstruct 定义 AST 节点
    Ast* ast = ast_new(AST_CSTRUCT_DEF, line);
    ast->u.cstruct_def.name = cstruct_name;
    ast->u.cstruct_def.field_names = field_names;
    ast->u.cstruct_def.field_types = field_types;
    ast->u.cstruct_def.field_array_dims = field_array_dims;
    ast->u.cstruct_def.field_count = field_count;
    ast->u.cstruct_def.total_size = 0;      // 语义分析时计算
    ast->u.cstruct_def.alignment = 0;       // 语义分析时计算
    ast->u.cstruct_def.field_offsets = NULL; // 语义分析时分配和计算
    ast->u.cstruct_def.ref.kind = SYM_CSTRUCT;
    ast->u.cstruct_def.ref.index = -1;
    ast->u.cstruct_def.ref.name = strdup(cstruct_name);
    ast->u.cstruct_def.ref.type_kind = TYPE_CSTRUCT;

    return ast;
}

// ============================================================================
// clib 定义解析 - C 库函数签名声明
// ============================================================================

Ast* parse_clib_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'clib'

    // 期望 clib 名称
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 clib 名称");
        return NULL;
    }

    char* clib_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 期望 '{'
    if (!consume(p, TOK_LBRACE, "期望 '{' 开始 clib 定义")) {
        free(clib_name);
        return NULL;
    }

    // 动态数组存储函数签名
    char** func_names = NULL;
    TypeInfo** func_return_types = NULL;
    TypeInfo*** func_param_types = NULL;
    int* func_param_counts = NULL;
    int func_count = 0;
    int func_capacity = 8;

    func_names = (char**)malloc(sizeof(char*) * func_capacity);
    func_return_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * func_capacity);
    func_param_types = (TypeInfo***)malloc(sizeof(TypeInfo**) * func_capacity);
    func_param_counts = (int*)malloc(sizeof(int) * func_capacity);

    // 解析函数签名列表
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_SEMI) {
            lexer_next(&p->lex);
            continue;
        }

        // 解析返回类型
        TypeInfo* return_type = parse_type(p);
        if (!return_type) {
            error_add(ERR_SYNTAX, p->lex.current.line, "clib 函数缺少返回类型");
            break;
        }

        // 支持 void 返回类型 - "void" 被 parse_type 解析为 TYPE_STRUCT
        if (return_type->kind == TYPE_STRUCT && return_type->struct_name &&
            (strcmp(return_type->struct_name, "void") == 0 ||
             strcmp(return_type->struct_name, "void") == 0)) {
            type_free(return_type);
            return_type = type_new(TYPE_NULL);  // TYPE_NULL 表示 void 返回
        }

        // 期望函数名
        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望函数名");
            type_free(return_type);
            break;
        }

        char* func_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        // 期望 '('
        if (!consume(p, TOK_LPAREN, "期望 '(' 开始参数列表")) {
            free(func_name);
            type_free(return_type);
            break;
        }

        // 解析参数列表
        int param_cap = 4;
        TypeInfo** params = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_cap);
        int param_count = 0;

        while (p->lex.current.type != TOK_RPAREN && p->lex.current.type != TOK_EOF) {
            if (p->lex.current.type == TOK_COMMA) {
                lexer_next(&p->lex);
                continue;
            }

            // 解析参数类型
            TypeInfo* param_type = parse_type(p);
            if (!param_type) {
                error_add(ERR_SYNTAX, p->lex.current.line, "期望参数类型");
                break;
            }

            // 解析参数名（可选）
            if (p->lex.current.type == TOK_IDENT) {
                lexer_next(&p->lex); // 消费参数名
            }

            // 扩容
            if (param_count >= param_cap) {
                param_cap *= 2;
                params = (TypeInfo**)realloc(params, sizeof(TypeInfo*) * param_cap);
            }
            params[param_count] = param_type;
            param_count++;
        }

        // 期望 ')'
        if (p->lex.current.type == TOK_RPAREN) {
            lexer_next(&p->lex);
        } else {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望 ')' 结束参数列表");
        }

        // 扩容函数数组
        if (func_count >= func_capacity) {
            func_capacity *= 2;
            func_names = (char**)realloc(func_names, sizeof(char*) * func_capacity);
            func_return_types = (TypeInfo**)realloc(func_return_types, sizeof(TypeInfo*) * func_capacity);
            func_param_types = (TypeInfo***)realloc(func_param_types, sizeof(TypeInfo**) * func_capacity);
            func_param_counts = (int*)realloc(func_param_counts, sizeof(int) * func_capacity);
        }

        func_names[func_count] = func_name;
        func_return_types[func_count] = return_type;
        func_param_types[func_count] = params;
        func_param_counts[func_count] = param_count;
        func_count++;

        // 可选的分号
        if (p->lex.current.type == TOK_SEMI) {
            lexer_next(&p->lex);
        }
    }

    // 期望 '}'
    if (!consume(p, TOK_RBRACE, "期望 '}' 结束 clib 定义")) {
        for (int i = 0; i < func_count; i++) {
            free(func_names[i]);
            type_free(func_return_types[i]);
            for (int j = 0; j < func_param_counts[i]; j++) {
                type_free(func_param_types[i][j]);
            }
            free(func_param_types[i]);
        }
        free(func_names);
        free(func_return_types);
        free(func_param_types);
        free(func_param_counts);
        free(clib_name);
        return NULL;
    }

    // 创建 clib 定义 AST 节点
    Ast* ast = ast_new(AST_CLIB_DEF, line);
    ast->u.clib_def.name = clib_name;
    ast->u.clib_def.func_names = func_names;
    ast->u.clib_def.func_return_types = func_return_types;
    ast->u.clib_def.func_param_types = func_param_types;
    ast->u.clib_def.func_param_counts = func_param_counts;
    ast->u.clib_def.func_count = func_count;
    ast->u.clib_def.ref.kind = SYM_CLIB;
    ast->u.clib_def.ref.index = -1;
    ast->u.clib_def.ref.name = strdup(clib_name);
    ast->u.clib_def.ref.type_kind = TYPE_CLIB;

    return ast;
}

// ============================================================================
// cfunc 声明解析（C 回调函数签名）
// 语法: cfunc Name(param_type param_name, ...): return_type
// ============================================================================
Ast* parse_cfunc_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'cfunc'

    // 期望 cfunc 名称
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 cfunc 名称");
        return NULL;
    }

    char* cfunc_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 期望 '('
    if (!consume(p, TOK_LPAREN, "期望 '(' 开始参数列表")) {
        free(cfunc_name);
        return NULL;
    }

    // 解析参数列表
    int param_cap = 4;
    TypeInfo** param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_cap);
    char** param_names = (char**)malloc(sizeof(char*) * param_cap);
    int param_count = 0;

    while (p->lex.current.type != TOK_RPAREN && p->lex.current.type != TOK_EOF) {
        if (p->lex.current.type == TOK_COMMA) {
            lexer_next(&p->lex);
            continue;
        }

        // 解析参数类型
        TypeInfo* ptype = parse_type(p);
        if (!ptype) {
            error_add(ERR_SYNTAX, p->lex.current.line, "cfunc 期望参数类型");
            break;
        }

        // 解析参数名
        char* pname = NULL;
        if (p->lex.current.type == TOK_IDENT) {
            pname = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);
        }

        // 扩容
        if (param_count >= param_cap) {
            param_cap *= 2;
            param_types = (TypeInfo**)realloc(param_types, sizeof(TypeInfo*) * param_cap);
            param_names = (char**)realloc(param_names, sizeof(char*) * param_cap);
        }
        param_types[param_count] = ptype;
        param_names[param_count] = pname;
        param_count++;
    }

    // 期望 ')'
    if (p->lex.current.type == TOK_RPAREN) {
        lexer_next(&p->lex);
    } else {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 ')' 结束参数列表");
    }

    // 解析返回类型: ': return_type'
    TypeInfo* return_type = NULL;
    if (p->lex.current.type == TOK_COLON) {
        lexer_next(&p->lex);
        return_type = parse_type(p);
        if (!return_type) {
            error_add(ERR_SYNTAX, p->lex.current.line, "cfunc 期望返回类型");
        }
        // void 返回类型处理
        if (return_type && return_type->kind == TYPE_STRUCT && return_type->struct_name &&
            strcmp(return_type->struct_name, "void") == 0) {
            type_free(return_type);
            return_type = type_new(TYPE_NULL);  // TYPE_NULL 表示 void 返回
        }
    }

    // 可选的分号
    if (p->lex.current.type == TOK_SEMI) {
        lexer_next(&p->lex);
    }

    // 创建 cfunc 声明 AST 节点
    Ast* ast = ast_new(AST_CFUNC_DECL, line);
    ast->u.cfunc_decl.name = cfunc_name;
    ast->u.cfunc_decl.param_types = param_types;
    ast->u.cfunc_decl.param_names = param_names;
    ast->u.cfunc_decl.param_count = param_count;
    ast->u.cfunc_decl.return_type = return_type;
    ast->u.cfunc_decl.ref.kind = SYM_CFUNC;
    ast->u.cfunc_decl.ref.index = -1;
    ast->u.cfunc_decl.ref.name = strdup(cfunc_name);
    ast->u.cfunc_decl.ref.type_kind = TYPE_CFUNC;

    return ast;
}

// ============================================================================
// enum 定义解析
// ============================================================================

Ast* parse_enum_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // 消费 'enum'

    // 期望 enum 名称
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "期望 enum 名称");
        return NULL;
    }

    char* enum_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 期望 '{'
    if (!consume(p, TOK_LBRACE, "期望 '{' 开始 enum 定义")) {
        free(enum_name);
        return NULL;
    }

    // 动态数组存储成员
    char** member_names = NULL;
    int64_t* member_values = NULL;
    int member_count = 0;
    int member_capacity = 8;

    member_names = (char**)malloc(sizeof(char*) * member_capacity);
    member_values = (int64_t*)malloc(sizeof(int64_t) * member_capacity);

    int64_t next_auto_value = 0; // 下一个自动分配的值

    // 解析成员列表
    while (p->lex.current.type != TOK_RBRACE && p->lex.current.type != TOK_EOF) {
        // 检查是否是注释（跳过）
        if (p->lex.current.type == TOK_ERROR) {
            lexer_next(&p->lex);
            continue;
        }

        // 期望成员名
        if (p->lex.current.type != TOK_IDENT) {
            error_add(ERR_SYNTAX, p->lex.current.line, "期望 enum 成员名称");
            break;
        }

        char* member_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        int64_t member_value = next_auto_value;

        // 检查是否有显式值: member = value
        if (p->lex.current.type == TOK_EQ) {
            lexer_next(&p->lex); // 消费 '='

            // 期望整数常量
            if (p->lex.current.type != TOK_NUM) {
                error_add(ERR_SYNTAX, p->lex.current.line, "enum 成员显式值必须是整数常量");
                free(member_name);
                break;
            }

            // 解析整数值
            if (p->lex.current.is_bigint && p->lex.current.bigint_str) {
                // BigInt 值，使用 strtoll 解析
                member_value = (int64_t)strtoll(p->lex.current.bigint_str, NULL, 10);
            } else {
                // 普通数值
                member_value = (int64_t)p->lex.current.num_val;
            }
            lexer_next(&p->lex);
        }

        // 扩容检查
        if (member_count >= member_capacity) {
            member_capacity *= 2;
            member_names = (char**)realloc(member_names, sizeof(char*) * member_capacity);
            member_values = (int64_t*)realloc(member_values, sizeof(int64_t) * member_capacity);
        }

        member_names[member_count] = member_name;
        member_values[member_count] = member_value;
        member_count++;

        // 下一个自动分配的值为当前值 + 1
        next_auto_value = member_value + 1;

        // 可选的分号
        if (p->lex.current.type == TOK_SEMI) {
            lexer_next(&p->lex);
        }
    }

    // 期望 '}'
    if (!consume(p, TOK_RBRACE, "期望 '}' 结束 enum 定义")) {
        // 清理已分配的内存
        for (int i = 0; i < member_count; i++) {
            free(member_names[i]);
        }
        free(member_names);
        free(member_values);
        free(enum_name);
        return NULL;
    }

    // 创建 enum 定义 AST 节点
    Ast* ast = ast_new(AST_ENUM_DEF, line);
    ast->u.enum_def.name = enum_name;
    ast->u.enum_def.member_names = member_names;
    ast->u.enum_def.member_values = member_values;
    ast->u.enum_def.member_count = member_count;

    return ast;
}
