#include "parser_internal.h"
#include "../include/native.h"
#include "../include/leno_package.h"
#include <string.h>

// ============================================================================
// import 语句解析（支持原生模块和文件模块）
// ============================================================================

Ast* parse_import_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // import
    
    char* module_name = NULL;
    char* alias = NULL;
    
    // 支持标识符或字符串作为模块名
    if (p->lex.current.type == TOK_IDENT) {
        // 标识符形式：import io
        module_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);
        
        // 检查是否有 as 别名
        if (p->lex.current.type == TOK_AS) {
            lexer_next(&p->lex); // as
            
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "as 后期望别名");
                free(module_name);
                return NULL;
            }
            
            alias = copy_string(p->lex.current.text, p->lex.current.len);
            
            // 检查别名是否是内部模块名称
            if (native_is_builtin_module(alias)) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg),
                    "import 别名不能使用内部模块名称: %s", alias);
                error_add(ERR_SYNTAX, p->lex.current.line, err_msg);
                free(module_name);
                free(alias);
                return NULL;
            }

            lexer_next(&p->lex);
        }
    } else if (p->lex.current.type == TOK_STRING) {
        // 字符串形式：import "test.leno" 或 import "test_pkg"
        module_name = copy_string(p->lex.current.text, p->lex.current.len);
        lexer_next(&p->lex);

        /*
         * 如果字符串不包含 .leno（即不是明确的文件路径），
         * 尝试在搜索路径中查找该模块。
         * 例如 import "test_pkg" → 在 lib/ 搜索 test_pkg.leno
         */
        if (strstr(module_name, ".leno") == NULL) {
            char resolved_path[1024];
            if (package_resolve_module_file(module_name, resolved_path, sizeof(resolved_path)) == 1) {
                /* 找到了！用解析后的路径替换 module_name */
                free(module_name);
                module_name = strdup(resolved_path);
            }
            /* 找不到也不报错，留给后续阶段（语义分析/codegen）判断 */
        }

        // 检查是否有 as 别名
        if (p->lex.current.type == TOK_AS) {
            lexer_next(&p->lex); // as

            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "as 后期望别名");
                free(module_name);
                return NULL;
            }

            alias = copy_string(p->lex.current.text, p->lex.current.len);

            // 检查别名是否是内部模块名称
            if (native_is_builtin_module(alias)) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg),
                    "import 别名不能使用内部模块名称: %s", alias);
                error_add(ERR_SYNTAX, p->lex.current.line, err_msg);
                free(module_name);
                free(alias);
                return NULL;
            }

            lexer_next(&p->lex);
        } else {
            // 没有别名时，从文件名提取模块名（去掉路径和扩展名）
            const char* base = strrchr(module_name, '/');
            if (!base) base = strrchr(module_name, '\\');
            if (!base) base = module_name;
            else base++; // 跳过路径分隔符

            // 去掉扩展名
            const char* dot = strrchr(base, '.');
            if (dot) {
                alias = (char*)malloc(dot - base + 1);
                strncpy(alias, base, dot - base);
                alias[dot - base] = '\0';
            } else {
                alias = copy_string(base, strlen(base));
            }

            // 检查自动提取的别名是否是内部模块名称
            if (native_is_builtin_module(alias)) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg),
                    "import 文件模块隐式别名不能使用内部模块名称: %s，请使用显式别名", alias);
                error_add(ERR_SYNTAX, line, err_msg);
                free(module_name);
                free(alias);
                return NULL;
            }
        }
    } else {
        error_add(ERR_SYNTAX, p->lex.current.line, "import 语句期望模块名或文件路径");
        return NULL;
    }
    
    Ast* ast = ast_new(AST_IMPORT, line);
    ast->u.import.module_name = module_name;
    ast->u.import.alias = alias;  // 如果没有别名，alias 为 NULL
    
    // 如果是文件模块（包含 .leno），保存文件路径
    if (strstr(module_name, ".leno") != NULL) {
        ast->u.import.file_path = copy_string(module_name, strlen(module_name));
    } else {
        ast->u.import.file_path = NULL;
    }
    
    return ast;
}

// ============================================================================
// export 语句解析（支持导出变量、函数和结构体）
// ============================================================================

Ast* parse_export_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // export

    // export 后面可以是 var 声明、func 定义、struct 定义、cstruct 定义或 enum 定义
    if (p->lex.current.type == TOK_VAR) {
        Ast* decl = parse_var_decl_internal(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_FUNC) {
        Ast* decl = parse_func_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_STRUCT) {
        Ast* decl = parse_struct_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_FACE) {
        Ast* decl = parse_face_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_CSTRUCT) {
        Ast* decl = parse_cstruct_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_ENUM) {
        Ast* decl = parse_enum_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else {
        error_add(ERR_SYNTAX, p->lex.current.line, "export 后面期望 var、func、struct、cstruct 或 enum");
        return NULL;
    }
}

// ============================================================================
// use 语句解析（支持将模块中的类型导入到当前作用域）
// 语法: use module.Type 
// ============================================================================

Ast* parse_use_stmt(Parser* p) {
    int line = p->lex.current.line;
    lexer_next(&p->lex); // use

    // 解析模块名
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "use 语句期望模块名");
        return NULL;
    }

    char* module_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    // 检查是否有 . 符号
    if (p->lex.current.type != TOK_DOT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "use 语句语法: use module.Type");
        free(module_name);
        return NULL;
    }
    lexer_next(&p->lex); // .

    // 解析要导入的符号名
    if (p->lex.current.type != TOK_IDENT) {
        error_add(ERR_SYNTAX, p->lex.current.line, "use 语句期望类型名");
        free(module_name);
        return NULL;
    }

    char* symbol_name = copy_string(p->lex.current.text, p->lex.current.len);
    lexer_next(&p->lex);

    Ast* ast = ast_new(AST_USE, line);
    ast->u.use.module_name = module_name;
    ast->u.use.symbol_name = symbol_name;

    return ast;
}
