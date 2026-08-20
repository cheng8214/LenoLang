#include "parser_internal.h"
#include "../include/native.h"
#include "../include/leno_package.h"
#include <string.h>
#include <ctype.h>

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
    
    // 如果模块名不含 .leno，尝试通过搜索路径解析为文件模块
    // 例如 import "SDL3" → 解析为 leno_module/LenoSDL3/lib/SDL3.leno
    if (strstr(module_name, ".leno") == NULL) {
        extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);
        char resolved[MAX_PATH_LEN] = {0};
        if (package_resolve_module_file(module_name, resolved, sizeof(resolved)) == 1) {
            // 找到了，将 module_name 替换为完整路径
            free(module_name);
            module_name = strdup(resolved);
            // 同时更新 AST 中的 module_name
            ast->u.import.module_name = module_name;
        }
    }

    // 如果是文件模块（包含 .leno），保存文件路径
    if (strstr(module_name, ".leno") != NULL) {
        ast->u.import.file_path = copy_string(module_name, strlen(module_name));
        
        // 解析期快速扫描模块的 use 语句，传导依赖模块的 export alias
        // 注意：不再自动注册 import 模块的 export alias，必须通过 use 显式导入
        extern char* read_module_file(const char* file_path, const char* current_file);
        const char* current_file = error_get_filename();
        char* module_source = read_module_file(module_name, current_file);
        if (module_source) {
            // 先收集模块中的 import 语句，建立模块别名→文件路径映射
            // 格式: import "path.leno" as alias
            struct { char* alias; char* path; } import_map[16] = {{0}};
            int import_cnt = 0;
            {
                char* scan = module_source;
                while (*scan) {
                    while (*scan && (*scan == ' ' || *scan == '\t' || *scan == '\n' || *scan == '\r')) scan++;
                    if (!*scan) break;
                    if (*scan == '/' && *(scan+1) == '/') { while (*scan && *scan != '\n') scan++; continue; }
                    if (strncmp(scan, "import", 6) == 0 && !isalnum((unsigned char)scan[6]) && scan[6] != '_') {
                        scan += 6;
                        while (*scan && (*scan == ' ' || *scan == '\t')) scan++;
                        // 读取文件路径
                        if (*scan == '"') {
                            scan++;
                            const char* path_start = scan;
                            while (*scan && *scan != '"') scan++;
                            int path_len = (int)(scan - path_start);
                            if (*scan == '"') scan++;
                            while (*scan && (*scan == ' ' || *scan == '\t')) scan++;
                            // 检查 as 别名
                            if (strncmp(scan, "as", 2) == 0 && !isalnum((unsigned char)scan[2]) && scan[2] != '_') {
                                scan += 2;
                                while (*scan && (*scan == ' ' || *scan == '\t')) scan++;
                                const char* alias_start = scan;
                                while (*scan && (isalnum((unsigned char)*scan) || *scan == '_')) scan++;
                                int alias_len = (int)(scan - alias_start);
                                if (alias_len > 0 && alias_len < 64 && path_len > 0 && import_cnt < 16) {
                                    import_map[import_cnt].alias = (char*)malloc(alias_len + 1);
                                    memcpy(import_map[import_cnt].alias, alias_start, alias_len);
                                    import_map[import_cnt].alias[alias_len] = '\0';
                                    import_map[import_cnt].path = (char*)malloc(path_len + 1);
                                    memcpy(import_map[import_cnt].path, path_start, path_len);
                                    import_map[import_cnt].path[path_len] = '\0';
                                    import_cnt++;
                                }
                            }
                        }
                    }
                    if (*scan) scan++;
                }
            }

            char* s = module_source;
            while (*s) {
                // 跳过空白
                while (*s && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) s++;
                if (!*s) break;
                
                // 跳过注释
                if (*s == '/' && *(s+1) == '/') { while (*s && *s != '\n') s++; continue; }

                // 查找 use 语句，传导依赖模块的 alias
                // 格式: use module.AliasName         — 单个导入
                //       use module.(A, B, C)         — 批量导入
                if (strncmp(s, "use", 3) == 0 && !isalnum((unsigned char)s[3]) && s[3] != '_') {
                    s += 3;
                    while (*s && (*s == ' ' || *s == '\t')) s++;
                    // 读取模块名
                    const char* mod_start = s;
                    while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
                    int mod_len = (int)(s - mod_start);
                    if (mod_len > 0 && *s == '.') {
                        char mod_name[64];
                        memcpy(mod_name, mod_start, mod_len);
                        mod_name[mod_len] = '\0';
                        s++; // skip '.'

                        // 判断批量模式: use module.(A, B, C) 还是 use module.Type
                        int batch_mode = (*s == '(');
                        if (batch_mode) s++; // skip '('

                        // 循环处理每个类型名（单模式只循环一次，批量模式循环到 ')'）
                        while (1) {
                            if (batch_mode) {
                                while (*s && (*s == ' ' || *s == '\t')) s++;
                                if (*s == ')') { s++; break; } // 批量模式结束
                            }

                            // 读取类型名
                            const char* type_start = s;
                            while (*s && (isalnum((unsigned char)*s) || *s == '_')) s++;
                            int type_len = (int)(s - type_start);
                            if (type_len > 0 && type_len < 64) {
                                char use_type_name[64];
                                memcpy(use_type_name, type_start, type_len);
                                use_type_name[type_len] = '\0';

                                // 查找 import_map 中模块名对应的文件路径
                                for (int im = 0; im < import_cnt; im++) {
                                    if (strcmp(import_map[im].alias, mod_name) == 0) {
                                        // 递归读取依赖模块，查找 export alias
                                        // 需要用当前模块的完整路径作为基准来解析依赖模块的相对路径
                                        // module_name 是 import 语句中的模块路径，需要从当前文件目录解析
                                        char dep_base_path[MAX_PATH_LEN] = {0};
                                        const char* current_parse_file = error_get_filename();
                                        if (current_parse_file) {
                                            strncpy(dep_base_path, current_parse_file, MAX_PATH_LEN - 1);
                                            char* last_sep = strrchr(dep_base_path, '\\');
                                            if (!last_sep) last_sep = strrchr(dep_base_path, '/');
                                            if (last_sep) {
                                                *(last_sep + 1) = '\0';
                                                strncat(dep_base_path, import_map[im].path, MAX_PATH_LEN - strlen(dep_base_path) - 1);
                                            } else {
                                                strncpy(dep_base_path, import_map[im].path, MAX_PATH_LEN - 1);
                                            }
                                        } else {
                                            strncpy(dep_base_path, import_map[im].path, MAX_PATH_LEN - 1);
                                        }
                                        char* dep_source = read_module_file(dep_base_path, current_parse_file);
                                        if (dep_source) {
                                            // 扫描依赖模块的 export alias
                                            struct { char* name; TypeInfo* type; } dep_aliases[32] = {{0}};
                                            int dep_cnt = 0;
                                            char* ds = dep_source;
                                            while (*ds) {
                                                while (*ds && (*ds == ' ' || *ds == '\t' || *ds == '\n' || *ds == '\r')) ds++;
                                                if (!*ds) break;
                                                if (*ds == '/' && *(ds+1) == '/') { while (*ds && *ds != '\n') ds++; continue; }
                                                if (strncmp(ds, "export", 6) == 0 && !isalnum((unsigned char)ds[6]) && ds[6] != '_') {
                                                    ds += 6;
                                                    while (*ds && (*ds == ' ' || *ds == '\t' || *ds == '\n' || *ds == '\r')) ds++;
                                                    if (strncmp(ds, "alias", 5) == 0 && !isalnum((unsigned char)ds[5]) && ds[5] != '_') {
                                                        ds += 5;
                                                        while (*ds && (*ds == ' ' || *ds == '\t')) ds++;
                                                        const char* dn_start = ds;
                                                        while (*ds && (isalnum((unsigned char)*ds) || *ds == '_')) ds++;
                                                        int dn_len = (int)(ds - dn_start);
                                                        if (dn_len > 0 && dn_len < 64) {
                                                            char dn[64];
                                                            memcpy(dn, dn_start, dn_len);
                                                            dn[dn_len] = '\0';
                                                            while (*ds && (*ds == ' ' || *ds == '\t')) ds++;
                                                            if (*ds == '=') {
                                                                ds++;
                                                                while (*ds && (*ds == ' ' || *ds == '\t')) ds++;
                                                                char dt_str[256] = {0};
                                                                int dti = 0;
                                                                while (*ds && dti < 255 && *ds != '\n' && *ds != '\r' &&
                                                                       *ds != ';' && *ds != '{' && *ds != '/') {
                                                                    if (!(*ds == '_' || isalnum((unsigned char)*ds) ||
                                                                          *ds == '[' || *ds == ']' || *ds == ',' ||
                                                                          *ds == ' ' || *ds == '\t')) break;
                                                                    if ((*ds == ' ' || *ds == '\t') && dti > 0 && (dt_str[dti-1] == ' ' || dt_str[dti-1] == '\t')) {
                                                                        ds++;
                                                                        continue;
                                                                    }
                                                                    dt_str[dti++] = (*ds == '\t') ? ' ' : *ds;
                                                                    ds++;
                                                                }
                                                                dt_str[dti] = '\0';
                                                                if (dti > 0) {
                                                                    extern TypeInfo* parse_type_from_string(const char* type_str);
                                                                    TypeInfo* dt_ptr = NULL;
                                                                    // 检查依赖模块的本地别名链
                                                                    if (!strchr(dt_str, '[') && !strchr(dt_str, ',')) {
                                                                        for (int da = 0; da < dep_cnt; da++) {
                                                                            if (strcmp(dep_aliases[da].name, dt_str) == 0) {
                                                                                dt_ptr = type_copy(dep_aliases[da].type);
                                                                                break;
                                                                            }
                                                                        }
                                                                    }
                                                                    if (!dt_ptr) dt_ptr = parse_type_from_string(dt_str);
                                                                    if (dt_ptr) {
                                                                        if (dep_cnt < 32) {
                                                                            dep_aliases[dep_cnt].name = strdup(dn);
                                                                            dep_aliases[dep_cnt].type = type_copy(dt_ptr);
                                                                            dep_cnt++;
                                                                        }
                                                                        // 如果是被 use 的那个别名，注册到当前 parser
                                                                        if (strcmp(dn, use_type_name) == 0) {
                                                                            add_alias(p, use_type_name, dt_ptr);
                                                                        }
                                                                        type_free(dt_ptr);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                }
                                                // 也扫描依赖模块的 use 语句（二级传导）
                                                if (strncmp(ds, "use", 3) == 0 && !isalnum((unsigned char)ds[3]) && ds[3] != '_') {
                                                    // 简单跳过，不递归更深层
                                                    ds += 3;
                                                    while (*ds && *ds != '\n' && *ds != '\r') ds++;
                                                    continue;
                                                }
                                                if (*ds) ds++;
                                            }
                                            // 清理依赖模块别名表
                                            for (int da = 0; da < dep_cnt; da++) {
                                                free(dep_aliases[da].name);
                                                type_free(dep_aliases[da].type);
                                            }
                                            free(dep_source);
                                        }
                                        break; // 找到匹配的 import，停止查找
                                    }
                                }
                            }

                            if (!batch_mode) break; // 单模式：只处理一次

                            // 批量模式：跳过逗号和空白，继续下一个类型名
                            while (*s && (*s == ' ' || *s == '\t')) s++;
                            if (*s == ',') s++;
                            while (*s && (*s == ' ' || *s == '\t')) s++;
                            // 允许尾逗号
                            if (*s == ')') { s++; break; }
                        }
                    }
                }

                if (*s) s++;
            }
            // 清理 import_map
            for (int im = 0; im < import_cnt; im++) {
                free(import_map[im].alias);
                free(import_map[im].path);
            }
            free(module_source);
        }
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

    // export 后面可以是 var/const 声明、func 定义、struct 定义、cstruct 定义或 enum 定义
    if (p->lex.current.type == TOK_VAR || p->lex.current.type == TOK_CONST) {
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
    } else if (p->lex.current.type == TOK_CSTRUCT ||
               is_cstruct_layout_attr(p)) {
        Ast* decl = parse_cstruct_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_CLIB) {
        Ast* decl = parse_clib_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_CFUNC) {
        Ast* decl = parse_cfunc_stmt(p);
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
    } else if (p->lex.current.type == TOK_ALIAS) {
        Ast* decl = parse_alias_stmt(p);
        if (!decl) return NULL;

        Ast* ast = ast_new(AST_EXPORT, line);
        ast->u.export.decl = decl;
        return ast;
    } else if (p->lex.current.type == TOK_USE) {
        error_add(ERR_SYNTAX, p->lex.current.line,
            "'use' 已自带重导出语义，不需要 'export'，请直接使用 'use module.Type'");
        return NULL;
    } else {
        error_add(ERR_SYNTAX, p->lex.current.line, "export 后面期望 var、const、func、struct、cstruct、packed、align、clib、enum 或 alias");
        return NULL;
    }
}

// ============================================================================
// use 语句解析（支持将模块中的类型导入到当前作用域）
// 语法: use module.Type           — 单个导入
//       use module.(A, B, C)      — 批量导入
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
        error_add(ERR_SYNTAX, p->lex.current.line, "use 语句语法: use module.Type 或 use module.(A, B, C)");
        free(module_name);
        return NULL;
    }
    lexer_next(&p->lex); // .

    // 批量模式: use module.(A, B, C)
    if (p->lex.current.type == TOK_LPAREN) {
        lexer_next(&p->lex); // (

        // 创建 AST_BLOCK 容器
        Ast* block = ast_new(AST_BLOCK, line);
        ast_list_init(&block->u.block);

        // 括号内至少需要一个符号名
        if (p->lex.current.type == TOK_RPAREN) {
            error_add(ERR_SYNTAX, p->lex.current.line, "use 批量导入语法: use module.(A, B, C)，括号内不能为空");
            free(module_name);
            ast_free(block);
            return NULL;
        }

        while (1) {
            if (p->lex.current.type != TOK_IDENT) {
                error_add(ERR_SYNTAX, p->lex.current.line, "use 批量导入期望类型名");
                free(module_name);
                ast_free(block);
                return NULL;
            }

            char* symbol_name = copy_string(p->lex.current.text, p->lex.current.len);
            lexer_next(&p->lex);

            // 创建 AST_USE 节点，每个子节点持有 module_name 的独立拷贝
            Ast* use_ast = ast_new(AST_USE, line);
            use_ast->u.use.module_name = strdup(module_name);
            use_ast->u.use.symbol_name = symbol_name;
            ast_list_add(&block->u.block, use_ast);

            // 逗号则继续，右括号则结束
            if (p->lex.current.type == TOK_COMMA) {
                lexer_next(&p->lex); // ,
                // 允许尾逗号: use module.(A, B, C,)
                if (p->lex.current.type == TOK_RPAREN) {
                    lexer_next(&p->lex); // )
                    break;
                }
                continue;
            } else if (p->lex.current.type == TOK_RPAREN) {
                lexer_next(&p->lex); // )
                break;
            } else {
                error_add(ERR_SYNTAX, p->lex.current.line, "use 批量导入语法: 期望 ',' 或 ')'");
                free(module_name);
                ast_free(block);
                return NULL;
            }
        }

        free(module_name); // module_name 已被 strdup 到每个子节点

        // 如果只有一个符号，直接返回单个 AST_USE 而非 AST_BLOCK
        if (block->u.block.count == 1) {
            Ast* single = block->u.block.items[0];
            block->u.block.count = 0; // 防止 ast_free 递归释放子节点
            ast_free(block);
            return single;
        }

        return block;
    }

    // 单模式: use module.Type（原有逻辑）
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
