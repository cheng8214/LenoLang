/**
 * 补全服务
 * 提供代码自动补全功能 - 使用 LenoC 编译器符号表
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"
#include "leno_builtins.h"
#include "../src/include/native.h"
#include <ctype.h>

// 从模块加载器读取模块文件
extern char* read_module_file(const char* file_path, const char* current_file);

// LenoC 关键字（从 lexer.c 提取）
static const char* leno_keywords[] = {
    // 控制流
    "if", "else", "eif", "then",
    "while", "for", "to", "break", "continue",
    "switch", "case", "default",
    "return",
    // 声明
    "var", "func", "struct", "enum",
    // 模块
    "import", "export", "as",
    // 逻辑
    "and", "or", "not", "is", "in",
    // 异常
    "try", "catch", "throw", "finally",
    // 异步
    "async", "await",
    // 字面量
    "true", "false", "null",
    NULL
};

// LenoC 内置类型
static const char* leno_types[] = {
    "int", "float", "string", "bool",
    "Array", "Dict", "File", "Ptr", "any",
    NULL
};

// 内置函数现在从 leno_builtins.h 的 builtin_functions 表获取

// 解析导入语句，建立别名到模块名的映射
// 支持: import maths, import maths as b, import "./test.leno" as test
typedef struct {
    char* alias;
    char* module_name;
} ImportAlias;

static ImportAlias* parse_imports(const char* content, int* count) {
    *count = 0;
    if (!content) return NULL;
    
    ImportAlias* aliases = malloc(sizeof(ImportAlias) * 64);
    if (!aliases) return NULL;
    
    const char* p = content;
    int idx = 0;
    
    while (*p && idx < 64) {
        // 跳过空白
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        
        // 检查是否是 import
        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            
            // 跳过空白
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            
            // 解析模块名或路径
            char module_name[128] = {0};
            int mod_len = 0;
            
            if (*p == '"') {
                // 字符串路径: import "./test.leno"
                p++;
                while (*p && *p != '"' && mod_len < 127) {
                    module_name[mod_len++] = *p++;
                }
                if (*p == '"') p++;
                module_name[mod_len] = '\0';
                // 从路径提取文件名作为模块名（保留.leno后缀，因为需要用它来读取文件）
                char* slash = strrchr(module_name, '/');
                char* backslash = strrchr(module_name, '\\');
                char* last_sep = slash > backslash ? slash : backslash;
                if (last_sep) {
                    memmove(module_name, last_sep + 1, strlen(last_sep + 1) + 1);
                    mod_len = strlen(module_name);
                }
                // 注意：保留 .leno 后缀在 module_name 中，因为 module_symbol_table_scan 需要完整文件名
            } else {
                // 标识符: import maths
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && mod_len < 127) {
                    module_name[mod_len++] = *p++;
                }
                module_name[mod_len] = '\0';
            }
            
            if (mod_len == 0) continue;
            
            // 跳过空白
            while (*p && isspace((unsigned char)*p)) p++;
            
            // 检查是否有 as 别名
            char alias[128] = {0};
            int alias_len = 0;
            
            if (strncmp(p, "as", 2) == 0 && !isalnum((unsigned char)p[2]) && p[2] != '_') {
                p += 2;
                while (*p && isspace((unsigned char)*p)) p++;
                
                // 解析别名
                while (*p && (isalnum((unsigned char)*p) || *p == '_') && alias_len < 127) {
                    alias[alias_len++] = *p++;
                }
                alias[alias_len] = '\0';
            }
            
            // 存储映射
            aliases[idx].module_name = strdup(module_name);
            if (alias_len > 0) {
                aliases[idx].alias = strdup(alias);
            } else {
                // 没有别名，使用模块名（去掉.leno后缀）作为别名
                // 因为用户代码中引用模块时不会带.leno后缀
                char alias_without_ext[128];
                strncpy(alias_without_ext, module_name, sizeof(alias_without_ext) - 1);
                alias_without_ext[sizeof(alias_without_ext) - 1] = '\0';
                char* dot = strrchr(alias_without_ext, '.');
                if (dot && strcmp(dot, ".leno") == 0) {
                    *dot = '\0';
                }
                aliases[idx].alias = strdup(alias_without_ext);
            }
            idx++;
        } else {
            // 跳过这一行
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    }
    
    *count = idx;
    return aliases;
}

static void free_import_aliases(ImportAlias* aliases, int count) {
    if (!aliases) return;
    for (int i = 0; i < count; i++) {
        free(aliases[i].alias);
        free(aliases[i].module_name);
    }
    free(aliases);
}

// 根据别名查找模块名
static const char* find_module_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    // 如果没有找到别名映射，直接返回别名本身（可能是直接使用模块名）
    return alias;
}

// 根据别名查找模块路径（用于 .leno 文件导入）
static const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias) {
    for (int i = 0; i < count; i++) {
        if (strcmp(aliases[i].alias, alias) == 0) {
            return aliases[i].module_name;
        }
    }
    return NULL;
}

// 前置声明
static void add_completion_item(LspCompletionItem** items, int* count, int* capacity,
                                const char* label, int kind, const char* detail, const char* documentation);

// 从当前文件内容中查找 enum 定义并提供补全
// enum_name: enum 名称（如 "Color"）
static void add_current_file_enum_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                              const char* enum_name) {
    fprintf(stderr, "[LSP DEBUG] add_current_file_enum_completions: enum_name='%s'\n", enum_name);

    // 在内容中查找 enum 定义
    const char* search_pos = content;
    char enum_pattern[256];
    snprintf(enum_pattern, sizeof(enum_pattern), "enum %s", enum_name);

    fprintf(stderr, "[LSP DEBUG] Looking for pattern: '%s'\n", enum_pattern);

    while (*search_pos) {
        const char* enum_kw = strstr(search_pos, enum_pattern);
        if (!enum_kw) {
            fprintf(stderr, "[LSP DEBUG] Pattern not found\n");
            break;
        }
        fprintf(stderr, "[LSP DEBUG] Found pattern at offset %d\n", (int)(enum_kw - content));

        // 确保这是一个 enum 定义（前面是行首或空白字符）
        const char* check = enum_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        fprintf(stderr, "[LSP DEBUG] check offset %d, char='%c' (0x%02x)\n",
                (int)(check - content), *check >= 32 && *check < 127 ? *check : '?', (unsigned char)*check);
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            fprintf(stderr, "[LSP DEBUG] Rejected, char before is not valid\n");
            search_pos = enum_kw + 1;
            continue;
        }

        // 检查 enum 名称后面是否跟着空白或 '{'（避免匹配到 enum Colorful 这种）
        const char* after_name = enum_kw + strlen(enum_pattern);
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            fprintf(stderr, "[LSP DEBUG] Rejected, not a valid enum definition\n");
            search_pos = enum_kw + 1;
            continue;
        }
        fprintf(stderr, "[LSP DEBUG] Accepted as enum definition\n");

        // 找到了 enum 定义，解析 enum 值
        // 支持两种格式：enum Color { ... } 或 enum Color\n{ ... }
        const char* brace = strchr(enum_kw, '{');
        if (!brace) break;

        const char* end_brace = strchr(brace, '}');
        if (!end_brace) break;

        // 解析 enum 值
        const char* p = brace + 1;
        while (p < end_brace) {
            // 跳过空白字符
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            // 跳过注释
            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            // 解析 enum 值名
            const char* name_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;

            int name_len = p - name_start;
            if (name_len > 0) {
                char* value_name = (char*)malloc(name_len + 1);
                if (value_name) {
                    strncpy(value_name, name_start, name_len);
                    value_name[name_len] = '\0';

                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s.%s: int", enum_name, value_name);

                    add_completion_item(items, count, capacity,
                                       value_name,
                                       LSP_COMP_ENUM_MEMBER,
                                       detail,
                                       NULL);
                    free(value_name);
                }
            }

            // 跳过空白字符和逗号
            while (p < end_brace && (isspace((unsigned char)*p) || *p == ',')) p++;
        }

        break;  // 只处理第一个匹配的 enum
    }
}

// 从编译器符号表中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
// ctx: 编译器上下文
static void add_symbol_table_struct_completions(const char* struct_name, const char* var_name,
                                                CompilerContext* ctx, LspCompletionItem** items, int* count, int* capacity) {
    if (!struct_name || !ctx || !ctx->root_scope) {
        fprintf(stderr, "[LSP DEBUG] add_symbol_table_struct_completions: invalid params\n");
        return;
    }

    fprintf(stderr, "[LSP DEBUG] Looking for struct '%s' in symbol table, root_scope=%p, sym_cnt=%d\n",
            struct_name, (void*)ctx->root_scope, ctx->root_scope->sym_cnt);

    // 打印所有符号
    for (int i = 0; i < ctx->root_scope->sym_cnt; i++) {
        Symbol* sym = ctx->root_scope->syms[i];
        fprintf(stderr, "[LSP DEBUG] Symbol[%d]: name='%s', kind=%d, type_kind=%d\n",
                i, sym->name, sym->kind, sym->type ? sym->type->kind : -1);
    }

    // 在符号表中查找 struct 定义
    Symbol* struct_sym = scope_resolve_tree_bfs(ctx->root_scope, struct_name);
    if (!struct_sym) {
        fprintf(stderr, "[LSP DEBUG] Struct '%s' not found in symbol table\n", struct_name);
        return;
    }

    // 检查是否是 struct 类型
    if (struct_sym->type->kind != TYPE_STRUCT) {
        fprintf(stderr, "[LSP DEBUG] Symbol '%s' is not a struct (kind=%d)\n", struct_name, struct_sym->type->kind);
        return;
    }

    fprintf(stderr, "[LSP DEBUG] Found struct '%s' in symbol table, field_count=%d\n",
            struct_name, struct_sym->struct_field_count);

    // 添加字段补全
    for (int i = 0; i < struct_sym->struct_field_count; i++) {
        const char* field_name = struct_sym->struct_field_names[i];
        TypeInfo* field_type = struct_sym->struct_field_types[i];

        const char* type_str = type_to_string(field_type);
        char detail[256];
        snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, field_name, type_str);

        add_completion_item(items, count, capacity,
                           field_name,
                           LSP_COMP_FIELD,
                           detail,
                           NULL);
        fprintf(stderr, "[LSP DEBUG] Added field: %s\n", field_name);
    }
}

// 从当前文件内容中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
static void add_current_file_struct_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                                const char* struct_name, const char* var_name) {
    if (!content || !struct_name) return;

    // 在内容中查找 struct 定义
    const char* search_pos = content;
    char struct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);

    while (*search_pos) {
        const char* struct_kw = strstr(search_pos, struct_pattern);
        if (!struct_kw) break;

        // 确保这是一个 struct 定义（前面是行首、空白字符、或 export 关键字）
        const char* check = struct_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            // 检查前面是否是 "export" 关键字
            if (check >= content + 5) {
                if (strncmp(check - 5, "export", 6) != 0) {
                    search_pos = struct_kw + 1;
                    continue;
                }
            } else {
                search_pos = struct_kw + 1;
                continue;
            }
        }

        // 检查 struct 名称后面是否跟着空白或 '{'
        const char* after_name = struct_kw + strlen(struct_pattern);
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = struct_kw + 1;
            continue;
        }

        // 找到了 struct 定义，解析字段
        const char* brace = strchr(struct_kw, '{');
        if (!brace) break;

        // 使用括号匹配找到 struct 的结束 '}'（考虑嵌套的 {}）
        const char* end_brace = brace + 1;
        int brace_count = 1;
        while (*end_brace && brace_count > 0) {
            if (*end_brace == '{') brace_count++;
            else if (*end_brace == '}') brace_count--;
            end_brace++;
        }
        if (brace_count != 0) break;  // 没有找到匹配的 '}
        end_brace--;  // 回退到 '}' 的位置

        // 解析字段
        const char* p = brace + 1;
        while (p < end_brace) {
            // 跳过空白字符
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            // 跳过注释
            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            // 检查是否是 func 定义
            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                // 解析 func 名称
                const char* func_name_start = p + 4;  // 跳过 "func"
                // 跳过空白
                while (*func_name_start && isspace((unsigned char)*func_name_start)) func_name_start++;
                // 解析函数名
                const char* name_start = func_name_start;
                while (*func_name_start && (isalnum((unsigned char)*func_name_start) || *func_name_start == '_')) func_name_start++;
                int name_len = func_name_start - name_start;

                if (name_len > 0) {
                    char* method_name = (char*)malloc(name_len + 1);
                    if (method_name) {
                        strncpy(method_name, name_start, name_len);
                        method_name[name_len] = '\0';

                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s()", var_name, method_name);

                        add_completion_item(items, count, capacity,
                                           method_name,
                                           LSP_COMP_METHOD,
                                           detail,
                                           NULL);
                        free(method_name);
                    }
                }

                // 跳过 func 体
                const char* func_brace = strchr(p, '{');
                if (func_brace && func_brace < end_brace) {
                    // 跳过 func 体（使用括号匹配）
                    int func_brace_count = 1;
                    p = func_brace + 1;
                    while (*p && func_brace_count > 0 && p < end_brace) {
                        if (*p == '{') func_brace_count++;
                        else if (*p == '}') func_brace_count--;
                        p++;
                    }
                } else {
                    // 没有找到 '{', 跳过这一行
                    while (p < end_brace && *p != '\n') p++;
                }
                continue;
            }

            // 解析类型（如 int, float, string 等）
            const char* type_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int type_len = p - type_start;

            // 跳过空白
            while (p < end_brace && isspace((unsigned char)*p)) p++;

            // 解析字段名
            const char* name_start = p;
            while (p < end_brace && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int name_len = p - name_start;

            if (type_len > 0 && name_len > 0) {
                char* field_name = (char*)malloc(name_len + 1);
                char* field_type = (char*)malloc(type_len + 1);
                if (field_name && field_type) {
                    strncpy(field_name, name_start, name_len);
                    field_name[name_len] = '\0';
                    strncpy(field_type, type_start, type_len);
                    field_type[type_len] = '\0';

                    char detail[256];
                    snprintf(detail, sizeof(detail), "%s.%s: %s", var_name, field_name, field_type);

                    add_completion_item(items, count, capacity,
                                       field_name,
                                       LSP_COMP_FIELD,
                                       detail,
                                       NULL);
                    free(field_name);
                    free(field_type);
                }
            }

            // 跳过空白字符和换行
            while (p < end_brace && (isspace((unsigned char)*p) || *p == '\n' || *p == '\r')) p++;
        }

        break;  // 只处理第一个匹配的 struct
    }
}

// 从模块源代码中解析指定 struct 的方法
// module_source: 模块文件内容
// struct_name: struct 名称
// module_alias: 模块别名（用于构建 detail）
static void add_module_struct_methods(const char* module_source, const char* struct_name, const char* module_alias,
                                      LspCompletionItem** items, int* count, int* capacity) {
    if (!module_source || !struct_name) {
        fprintf(stderr, "[LSP DEBUG] add_module_struct_methods: null argument\n");
        return;
    }

    fprintf(stderr, "[LSP DEBUG] add_module_struct_methods: looking for struct '%s'\n", struct_name);

    // 在内容中查找 struct 定义（支持 export struct 和 struct 两种格式）
    const char* search_pos = module_source;
    char struct_pattern[256];
    char export_struct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);
    snprintf(export_struct_pattern, sizeof(export_struct_pattern), "export struct %s", struct_name);

    while (*search_pos) {
        // 先尝试匹配 export struct，再尝试匹配 struct
        const char* struct_kw = strstr(search_pos, export_struct_pattern);
        int is_export = 1;
        if (!struct_kw) {
            struct_kw = strstr(search_pos, struct_pattern);
            is_export = 0;
        }
        if (!struct_kw) {
            fprintf(stderr, "[LSP DEBUG] struct pattern not found\n");
            break;
        }
        fprintf(stderr, "[LSP DEBUG] found struct keyword at position %d (export=%d)\n", (int)(struct_kw - module_source), is_export);

        // 确保这是一个 struct 定义（前面是行首或空白字符）
        const char* check = struct_kw - 1;
        while (check > module_source && isspace((unsigned char)*check)) check--;
        if (check > module_source && *check != '\n' && *check != ';' && *check != '}') {
            fprintf(stderr, "[LSP DEBUG] rejected, char before is not valid\n");
            search_pos = struct_kw + 1;
            continue;
        }

        // 检查 struct 名称后面是否跟着空白或 '{'
        const char* pattern = is_export ? export_struct_pattern : struct_pattern;
        const char* after_name = struct_kw + strlen(pattern);
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            fprintf(stderr, "[LSP DEBUG] rejected, after name is not valid: '%c'\n", *after_name);
            search_pos = struct_kw + 1;
            continue;
        }
        fprintf(stderr, "[LSP DEBUG] struct definition validated\n");

        // 找到了 struct 定义，解析方法
        fprintf(stderr, "[LSP DEBUG] found struct definition, looking for methods\n");
        const char* brace = strchr(struct_kw, '{');
        if (!brace) {
            fprintf(stderr, "[LSP DEBUG] no opening brace found\n");
            break;
        }

        // 使用括号匹配找到 struct 的结束 '}'（考虑嵌套的 {}）
        const char* end_brace = brace + 1;
        int brace_count = 1;
        while (*end_brace && brace_count > 0) {
            if (*end_brace == '{') brace_count++;
            else if (*end_brace == '}') brace_count--;
            end_brace++;
        }
        if (brace_count != 0) break;  // 没有找到匹配的 '}
        end_brace--;  // 回退到 '}' 的位置

        // 解析方法
        const char* p = brace + 1;
        while (p < end_brace) {
            // 跳过空白字符
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            // 跳过注释
            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            // 检查是否是 func 定义
            fprintf(stderr, "[LSP DEBUG] checking: '%.10s'\n", p);
            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                fprintf(stderr, "[LSP DEBUG] found func keyword\n");
                // 解析 func 名称
                const char* func_name_start = p + 4;  // 跳过 "func"
                // 跳过空白
                while (*func_name_start && isspace((unsigned char)*func_name_start)) func_name_start++;
                // 解析函数名
                const char* name_start = func_name_start;
                while (*func_name_start && (isalnum((unsigned char)*func_name_start) || *func_name_start == '_')) func_name_start++;
                int name_len = func_name_start - name_start;

                fprintf(stderr, "[LSP DEBUG] method name len=%d\n", name_len);
                if (name_len > 0) {
                    char* method_name = (char*)malloc(name_len + 1);
                    if (method_name) {
                        strncpy(method_name, name_start, name_len);
                        method_name[name_len] = '\0';

                        fprintf(stderr, "[LSP DEBUG] adding method '%s'\n", method_name);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s.%s()", module_alias, struct_name, method_name);

                        add_completion_item(items, count, capacity,
                                           method_name,
                                           LSP_COMP_METHOD,
                                           detail,
                                           NULL);
                        free(method_name);
                    }
                }

                // 跳过 func 体
                const char* func_brace = strchr(p, '{');
                if (func_brace && func_brace < end_brace) {
                    // 跳过 func 体（使用括号匹配）
                    int func_brace_count = 1;
                    p = func_brace + 1;
                    while (*p && func_brace_count > 0 && p < end_brace) {
                        if (*p == '{') func_brace_count++;
                        else if (*p == '}') func_brace_count--;
                        p++;
                    }
                } else {
                    // 没有找到 '{', 跳过这一行
                    while (p < end_brace && *p != '\n') p++;
                }
                continue;
            }

            // 跳过非 func 的行（字段定义等）
            while (p < end_brace && *p != '\n') p++;
        }

        break;  // 只处理第一个匹配的 struct
    }
}

// 从导入的 .leno 模块获取补全项（使用编译器获取完整类型）
// module_alias: 模块别名（如 "color_module"）
// member_name: 可选的成员名（如 "Color"），用于补全 enum/struct 的成员
// current_file: 当前文件的完整路径，用于解析相对路径的模块
static void add_module_symbol_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                          const char* module_alias, const char* member_name,
                                          ImportAlias* import_aliases, int import_count,
                                          const char* current_file) {
    (void)content;  // 抑制未使用参数警告
    // 查找模块路径
    const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
    fprintf(stderr, "[LSP DEBUG] add_module_symbol_completions: module_alias='%s', module_path='%s', current_file='%s'\n",
            module_alias ? module_alias : "NULL", module_path ? module_path : "NULL", current_file ? current_file : "NULL");
    if (!module_path) return;

    // 读取模块文件内容
    char* module_source = read_module_file(module_path, current_file);
    if (!module_source) {
        fprintf(stderr, "[LSP DEBUG] Failed to read module file\n");
        return;
    }

    // 使用编译器分析模块
    CompilerContext ctx;
    compiler_context_init(&ctx);
    bool analyzed = compiler_analyze_with_filename(&ctx, module_source, current_file);

    if (!analyzed || !ctx.root_scope) {
        fprintf(stderr, "[LSP DEBUG] Failed to analyze module\n");
        free(module_source);
        compiler_context_cleanup(&ctx);
        return;
    }

    fprintf(stderr, "[LSP DEBUG] Module analyzed successfully\n");

    // 如果有成员名，查找该成员（enum 或 struct）
    if (member_name && *member_name) {
        // 查找符号
        Symbol* sym = scope_resolve_tree_bfs(ctx.root_scope, member_name);
        fprintf(stderr, "[LSP DEBUG] Looking for member '%s', sym=%p\n", member_name, (void*)sym);
        if (sym) {
            fprintf(stderr, "[LSP DEBUG] Found symbol '%s', kind=%d, type=%p\n", sym->name, sym->kind, (void*)sym->type);
            if (sym->type) {
                fprintf(stderr, "[LSP DEBUG] Symbol type kind=%d\n", sym->type->kind);
            }
        }
        // 检查符号是否是类型定义（SYM_TYPE）或者是模块级别的类型（SYM_MODULE 且 type 为 STRUCT/ENUM）
        bool is_type_symbol = (sym && sym->kind == SYM_TYPE) ||
                              (sym && sym->kind == SYM_MODULE && sym->type &&
                               (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_ENUM));
        if (is_type_symbol) {
            if (sym->type && sym->type->kind == TYPE_ENUM) {
                // 是 enum，添加 enum 值补全
                // 从符号的 enum 值列表中获取
                // 注意：enum 值存储在符号的附加信息中，这里简化处理
                // 实际应该从编译器的 enum 定义中获取
                // 暂时使用类型名称显示
                char detail[256];
                snprintf(detail, sizeof(detail), "%s: enum", member_name);
                add_completion_item(items, count, capacity,
                                   member_name,
                                   LSP_COMP_ENUM,
                                   detail,
                                   NULL);
            } else if (sym->type && sym->type->kind == TYPE_STRUCT) {
                // 是 struct，添加字段和方法补全
                // 添加字段
                if (sym->struct_field_names && sym->struct_field_types) {
                    for (int i = 0; i < sym->struct_field_count; i++) {
                        const char* type_str = type_to_string(sym->struct_field_types[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s: %s", member_name, sym->struct_field_names[i], type_str);

                        add_completion_item(items, count, capacity,
                                           sym->struct_field_names[i],
                                           LSP_COMP_FIELD,
                                           detail,
                                           NULL);
                    }
                }
                // 从模块源代码解析 struct 方法
                add_module_struct_methods(module_source, member_name, module_alias, items, count, capacity);
            }
        }
    } else {
        // 没有成员名，遍历所有导出的符号
        // 遍历作用域中的所有符号
        for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
            Symbol* sym = ctx.root_scope->syms[i];
            // 只处理导出的符号（这里简化处理，实际应该检查 export 标记）
            // 在模块文件中，所有顶层符号都是导出的

            fprintf(stderr, "[LSP DEBUG] Checking symbol '%s', kind=%d, scope=%p, root_scope=%p\n",
                    sym->name, sym->kind, (void*)sym->scope, (void*)ctx.root_scope);

            // 跳过非顶层符号（如 struct 内部的方法）
            // 顶层符号的 scope 应该是根作用域
            if (sym->scope != ctx.root_scope) {
                fprintf(stderr, "[LSP DEBUG] Skipping non-top-level symbol '%s'\n", sym->name);
                continue;
            }

            const char* type_str = type_to_string(sym->type);
            char detail[256];

            switch (sym->kind) {
                case SYM_GLOBAL_FUNC:
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, sym->name, type_str);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_FUNCTION,
                                       detail,
                                       NULL);
                    break;
                case SYM_TYPE:
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        snprintf(detail, sizeof(detail), "%s.%s: struct", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_STRUCT,
                                           detail,
                                           NULL);
                    } else if (sym->type && sym->type->kind == TYPE_ENUM) {
                        snprintf(detail, sizeof(detail), "%s.%s: enum", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_ENUM,
                                           detail,
                                           NULL);
                    }
                    break;
                case SYM_GLOBAL:
                case SYM_MODULE:
                    snprintf(detail, sizeof(detail), "%s.%s: %s", module_alias, sym->name, type_str);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_VARIABLE,
                                       detail,
                                       NULL);
                    break;
                default:
                    break;
            }
        }
    }

    free(module_source);
    compiler_context_cleanup(&ctx);
}

// 检查是否是字符串字面量结尾（如 '"...".' 或 "'...'.", '"".', "''"）
// 返回字符串开始的位置（不包括引号），如果不是字符串字面量则返回 -1
static int is_string_literal_before_dot(const char* content, int dot_pos) {
    if (dot_pos <= 0) return -1;
    
    // 跳过点号前的空白
    int pos = dot_pos - 1;
    while (pos >= 0 && isspace((unsigned char)content[pos])) {
        pos--;
    }
    
    // 检查是否以引号结尾
    if (pos < 0 || (content[pos] != '"' && content[pos] != '\'')) {
        return -1;
    }
    
    char quote = content[pos];
    pos--;
    
    // 向前查找匹配的引号
    while (pos >= 0) {
        if (content[pos] == quote) {
            // 检查是否是转义的（前面有奇数个反斜杠）
            int backslash_count = 0;
            int check_pos = pos - 1;
            while (check_pos >= 0 && content[check_pos] == '\\') {
                backslash_count++;
                check_pos--;
            }
            // 如果反斜杠数量是偶数，说明这不是转义的引号
            if (backslash_count % 2 == 0) {
                return pos + 1;  // 返回字符串内容开始的位置
            }
        }
        pos--;
    }
    
    return -1;  // 没有找到匹配的引号
}

// 获取光标前的单词（包含模块前缀，如 "maths.")"
static char* get_word_before_cursor(const char* content, LspPosition pos) {
    if (!content) return NULL;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return NULL;
    
    // 检查光标前是否是点号（如 "maths."）
    int start = offset - 1;
    
    // 如果光标前是点号，需要包含点号前面的模块名
    if (start >= 0 && content[start] == '.') {
        // 检查是否是字符串字面量（如 "". 或 "hello".）
        if (is_string_literal_before_dot(content, start) >= 0) {
            // 返回特殊标记表示字符串字面量
            return strdup("__STRING_LITERAL__");
        }
        
        // 包含点号
        start--;
        // 继续向前查找标识符（如 Color）
        while (start >= 0 && (isalnum((unsigned char)content[start]) ||
                              content[start] == '_')) {
            start--;
        }

        // 检查是否还有一层（如 "cs_module.Color."）
        // 如果前面是点号，继续向前查找模块名
        if (start >= 0 && content[start] == '.') {
            start--;
            // 继续向前查找模块名（如 cs_module）
            while (start >= 0 && (isalnum((unsigned char)content[start]) ||
                                  content[start] == '_')) {
                start--;
            }
            start++;
        } else {
            start++;
        }

        int len = offset - start;
        if (len <= 0) return NULL;

        char* word = (char*)malloc(len + 1);
        if (!word) return NULL;

        memcpy(word, content + start, len);
        word[len] = '\0';

        return word;
    }
    
    // 普通单词（不包含点号）
    while (start >= 0 && (isalnum((unsigned char)content[start]) || 
                          content[start] == '_')) {
        start--;
    }
    start++;
    
    int len = offset - start;
    if (len <= 0) return NULL;
    
    char* word = (char*)malloc(len + 1);
    if (!word) return NULL;
    
    memcpy(word, content + start, len);
    word[len] = '\0';
    
    return word;
}

// 处理补全请求
char* lsp_handle_completion(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED,
                                "Server not initialized");
    }
    
    // 解析参数
    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* position = json_object_get(params, "position");
    
    if (!text_doc || !position) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Missing textDocument or position");
    }
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* line = json_object_get(position, "line");
    JsonValue* character = json_object_get(position, "character");
    
    if (!uri || !line || !character) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Invalid position");
    }
    
    // 获取文档
    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS,
                                "Document not found");
    }
    
    LspPosition pos = {
        .line = json_int_value(line),
        .character = json_int_value(character)
    };

    // 获取文件路径（从 URI 转换并解码）
    const char* uri_str = json_string_value(uri);
    char file_path_buf[1024] = {0};
    char* file_path = NULL;
    if (uri_str && strncmp(uri_str, "file:///", 8) == 0) {
        // 跳过 "file:///" 前缀
        const char* p = uri_str + 8;
        char* q = file_path_buf;
        size_t len = 0;
        while (*p && len < sizeof(file_path_buf) - 1) {
            if (*p == '%' && *(p+1) && *(p+2)) {
                // URL 解码 %XX
                char hex[3] = {*(p+1), *(p+2), '\0'};
                int val = (int)strtol(hex, NULL, 16);
                *q++ = (char)val;
                p += 3;
            } else if (*p == '/') {
#ifdef _WIN32
                *q++ = '\\';  // Windows 上使用反斜杠
#else
                *q++ = '/';   // Linux/Mac 上使用斜杠
#endif
                p++;
            } else {
                *q++ = *p++;
            }
            len++;
        }
        *q = '\0';
        file_path = file_path_buf;
    }

    // 获取补全项
    int count = 0;
    LspCompletionItem* items = lsp_get_completions(doc->content, pos, &count, file_path);
    
    // 构建响应
    JsonValue* result = json_object_new();
    JsonValue* items_array = json_array_new();
    
    for (int i = 0; i < count; i++) {
        JsonValue* item = json_object_new();
        json_object_set(item, "label", json_string_new(items[i].label));
        json_object_set(item, "kind", json_int_new(items[i].kind));
        
        if (items[i].detail) {
            json_object_set(item, "detail", json_string_new(items[i].detail));
        }
        
        if (items[i].documentation) {
            JsonValue* doc_obj = json_object_new();
            json_object_set(doc_obj, "kind", json_string_new("markdown"));
            json_object_set(doc_obj, "value", json_string_new(items[i].documentation));
            json_object_set(item, "documentation", doc_obj);
        }
        
        if (items[i].insertText) {
            json_object_set(item, "insertText", json_string_new(items[i].insertText));
        }
        
        json_array_add(items_array, item);
    }
    
    json_object_set(result, "items", items_array);
    json_object_set(result, "isIncomplete", json_bool_new(false));
    
    char* response = lsp_create_response(id, result);
    
    // 清理
    json_free(result);
    lsp_free_completions(items, count);
    
    return response;
}

// 添加补全项
static void add_completion_item(LspCompletionItem** items, int* count, int* capacity,
                                 const char* label, int kind, const char* detail,
                                 const char* prefix) {
    if (!label) return;
    
    // 前缀匹配检查
    if (prefix && prefix[0] != '\0') {
        if (strncmp(label, prefix, strlen(prefix)) != 0) {
            return;
        }
    }
    
    // 检查是否已存在
    for (int i = 0; i < *count; i++) {
        if (strcmp((*items)[i].label, label) == 0) {
            return;
        }
    }
    
    // 扩容
    if (*count >= *capacity) {
        *capacity *= 2;
        *items = (LspCompletionItem*)realloc(*items,
            sizeof(LspCompletionItem) * (*capacity));
    }
    
    (*items)[*count].label = strdup(label);
    (*items)[*count].kind = kind;
    (*items)[*count].detail = detail ? strdup(detail) : NULL;
    (*items)[*count].documentation = NULL;
    (*items)[*count].insertText = NULL;
    (*count)++;
}

// 从编译器符号表获取补全
static void add_symbols_from_compiler(const char* content, LspCompletionItem** items,
                                       int* count, int* capacity, const char* prefix, const char* file_path) {
    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 编译分析获取符号表
    if (!compiler_analyze_with_filename(&ctx, content, file_path) && !ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return;
    }
    
    // 获取所有符号
    char** names = NULL;
    char** types = NULL;
    int sym_count = compiler_get_all_symbols(&ctx, &names, &types);
    
    for (int i = 0; i < sym_count; i++) {
        if (!names[i]) continue;
        
        // 确定符号类型
        int kind = LSP_COMP_VARIABLE;
        if (types && types[i]) {
            if (strstr(types[i], "func") != NULL) {
                kind = LSP_COMP_FUNCTION;
            } else if (strstr(types[i], "struct") != NULL) {
                kind = LSP_COMP_STRUCT;
            }
        }
        
        // 构建详细信息
        char detail[256];
        if (types && types[i]) {
            snprintf(detail, sizeof(detail), "%s: %s", 
                     kind == LSP_COMP_FUNCTION ? "function" : 
                     kind == LSP_COMP_STRUCT ? "struct" : "variable",
                     types[i]);
        } else {
            snprintf(detail, sizeof(detail), "%s",
                     kind == LSP_COMP_FUNCTION ? "function" : 
                     kind == LSP_COMP_STRUCT ? "struct" : "variable");
        }
        
        add_completion_item(items, count, capacity, names[i], kind, detail, prefix);
    }
    
    // 清理
    compiler_free_symbol_list(names, types, sym_count);
    compiler_context_cleanup(&ctx);
}

// 检查是否是模块前缀（如 "maths." 或 "color_module.Color."）
// 支持两层：module.member.
// module_out: 输出模块名（如 "maths" 或 "color_module"）
// member_out: 输出成员名（如 "Color"），如果没有第二层则为 NULL
static bool parse_module_prefix(const char* word, char** module_out, char** member_out) {
    if (!word) return false;

    const char* first_dot = strchr(word, '.');
    if (!first_dot) return false;

    int module_len = first_dot - word;
    if (module_len == 0) return false;

    *module_out = (char*)malloc(module_len + 1);
    if (!*module_out) return false;

    memcpy(*module_out, word, module_len);
    (*module_out)[module_len] = '\0';

    // 检查是否有第二层（如 "Color."）
    *member_out = NULL;
    const char* second_dot = strchr(first_dot + 1, '.');
    if (second_dot) {
        int member_len = second_dot - (first_dot + 1);
        if (member_len > 0) {
            *member_out = (char*)malloc(member_len + 1);
            if (*member_out) {
                memcpy(*member_out, first_dot + 1, member_len);
                (*member_out)[member_len] = '\0';
            }
        }
    }

    return true;
}

// 获取变量类型（用于实例方法补全）
// 返回 "number", "string", "array" 等，如果无法确定返回 NULL
// 使用编译器分析结果，而不是自己解析
static char* get_variable_type(const char* content, const char* var_name, const char* file_path) {
    if (!content || !var_name) return NULL;

    // 首先检查 var_name 本身是否是字面量
    if (isdigit((unsigned char)var_name[0]) || var_name[0] == '-' || var_name[0] == '.') {
        return strdup("number");
    } else if (var_name[0] == '"' || var_name[0] == '\'') {
        return strdup("string");
    } else if (var_name[0] == '[') {
        return strdup("array");
    } else if (var_name[0] == '{') {
        return strdup("dict");
    }

    // 使用编译器分析获取变量类型
    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 调试：打印内容前100字符
    fprintf(stderr, "[LSP DEBUG] content preview: %.100s\n", content ? content : "(null)");

    // 即使分析有错误，也尝试获取符号信息
    bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);
    fprintf(stderr, "[LSP DEBUG] compiler_analyze returned: %d, root_scope: %p\n", analyze_ok, (void*)ctx.root_scope);

    // 如果解析失败，尝试去掉包含变量引用的那一行（可能是不完整的代码）再解析
    if (!analyze_ok || !ctx.root_scope) {
        // 复制内容
        char* content_copy = strdup(content);
        if (content_copy) {
            // 查找变量名后面跟着 . 的位置（即实例方法调用）
            char* search_pattern = malloc(strlen(var_name) + 2);
            if (search_pattern) {
                sprintf(search_pattern, "%s.", var_name);
                bool found = true;
                int retry_count = 0;
                const int max_retries = 5;  // 最多重试5次
                
                while (found && retry_count < max_retries) {
                    char* var_pos = strstr(content_copy, search_pattern);
                    if (!var_pos) {
                        found = false;
                        break;
                    }
                    
                    // 找到该行的开始（前一个换行符）
                    char* line_start = content_copy;
                    char* p = content_copy;
                    while (p < var_pos) {
                        if (*p == '\n') line_start = p + 1;
                        p++;
                    }
                    // 找到该行的结束（后一个换行符）
                    char* line_end = strchr(var_pos, '\n');
                    if (line_end) {
                        // 去掉这一行：将 line_end 之后的内容移到 line_start
                        memmove(line_start, line_end + 1, strlen(line_end + 1) + 1);
                        fprintf(stderr, "[LSP DEBUG] Removed line with '%s.', retry %d, new content: %.100s\n", var_name, retry_count + 1, content_copy);

                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        fprintf(stderr, "[LSP DEBUG] Retry %d compiler_analyze returned: %d, root_scope: %p\n", retry_count + 1, analyze_ok, (void*)ctx.root_scope);
                        
                        if (analyze_ok || ctx.root_scope) {
                            // 解析成功，跳出循环
                            break;
                        }
                        retry_count++;
                    } else {
                        // 没有找到换行符，可能是最后一行
                        *line_start = '\0';
                        fprintf(stderr, "[LSP DEBUG] Removed last line with '%s.', content: %.100s\n", var_name, content_copy);
                        
                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        fprintf(stderr, "[LSP DEBUG] Retry %d compiler_analyze returned: %d, root_scope: %p\n", retry_count + 1, analyze_ok, (void*)ctx.root_scope);
                        break;
                    }
                }
                free(search_pattern);
            }
            free(content_copy);
        }
    }

    char* type_str = NULL;
    fprintf(stderr, "[LSP DEBUG] root_scope: %p, var_name: '%s'\n", (void*)ctx.root_scope, var_name);
    if (ctx.root_scope && compiler_get_symbol_info(&ctx, var_name, &type_str, NULL)) {
        // 调试输出
        fprintf(stderr, "[LSP DEBUG] Variable '%s' has type: '%s'\n", var_name, type_str);
        
        // 将编译器的类型名转换为实例方法类型名
        char* result = NULL;
        // 先检查 struct，因为 type_str 可能是 "struct Point" 包含 "int"
        if (strstr(type_str, "struct")) {
            // 对于 struct 类型，返回原始类型字符串（如 "struct Point"）
            // 这样调用者可以解析出 struct 名称并获取字段
            result = strdup(type_str);
        } else if (strcmp(type_str, "int") == 0 || strcmp(type_str, "float") == 0 || strcmp(type_str, "bigint") == 0) {
            result = strdup("number");
        } else if (strcmp(type_str, "string") == 0) {
            result = strdup("string");
        } else if (strcmp(type_str, "array") == 0) {
            result = strdup("array");
        } else if (strcmp(type_str, "dict") == 0) {
            result = strdup("dict");
        } else if (strcmp(type_str, "bool") == 0) {
            result = strdup("bool");
        } else if (strcmp(type_str, "null") == 0) {
            result = strdup("null");
        } else if (strcmp(type_str, "ptr") == 0) {
            result = strdup("ptr");
        } else if (strcmp(type_str, "file") == 0) {
            result = strdup("file");
        }
        fprintf(stderr, "[LSP DEBUG] Converted to instance type: '%s'\n", result ? result : "NULL");
        free(type_str);
        compiler_context_cleanup(&ctx);

        // 验证该类型是否有实例方法（只对非struct类型）
        if (result && strstr(result, "struct") == NULL) {
            int test_count = 0;
            char** test_methods = native_get_instance_methods(result, &test_count);
            fprintf(stderr, "[LSP DEBUG] Instance methods count for '%s': %d\n", result, test_count);
            if (test_methods && test_count > 0) {
                native_free_instance_method_list(test_methods, test_count);
            } else {
                // 该类型没有实例方法，返回NULL
                free(result);
                result = NULL;
            }
        }
        return result;
    }

    fprintf(stderr, "[LSP DEBUG] Failed to get type for '%s'\n", var_name);
    compiler_context_cleanup(&ctx);
    return NULL;
}

// 获取补全项
LspCompletionItem* lsp_get_completions(const char* content, LspPosition pos, int* count, const char* file_path) {
    *count = 0;
    
    if (!content) return NULL;
    
    // 解析导入语句，建立别名映射
    int import_count = 0;
    ImportAlias* import_aliases = parse_imports(content, &import_count);
    
    // 获取前缀
    char* prefix = get_word_before_cursor(content, pos);

    // 检查是否是字符串字面量实例方法调用（如 "". 或 "hello".")
    bool is_string_literal = (prefix && strcmp(prefix, "__STRING_LITERAL__") == 0);

    // 检查是否是模块前缀（如 "maths." 或 "color_module.Color."）
    char* module_alias = NULL;
    char* member_name = NULL;
    bool is_module_prefix = parse_module_prefix(prefix, &module_alias, &member_name);

    // DEBUG
    if (prefix) {
        fprintf(stderr, "[LSP DEBUG] prefix='%s', is_module_prefix=%d\n", prefix, is_module_prefix);
        if (is_module_prefix) {
            fprintf(stderr, "[LSP DEBUG] module_alias='%s', member_name='%s'\n",
                    module_alias ? module_alias : "NULL",
                    member_name ? member_name : "NULL");
        }
    }

    // 分配补全数组
    int capacity = 256;
    LspCompletionItem* items = (LspCompletionItem*)malloc(
        sizeof(LspCompletionItem) * capacity);
    if (!items) {
        free(prefix);
        free(module_alias);
        free(member_name);
        free_import_aliases(import_aliases, import_count);
        return NULL;
    }
    
    if (is_string_literal) {
        // 字符串字面量实例方法补全
        const char* display_name = "\"\"";  // 显示为 ""
        int method_count = 0;
        char** methods = native_get_instance_methods("string", &method_count);
        
        if (methods && method_count > 0) {
            for (int i = 0; i < method_count; i++) {
                int arity = native_get_instance_method_arity("string", methods[i]);
                TypeKind return_type = native_get_instance_method_return_type("string", methods[i], NULL);
                
                const char* return_type_str = type_kind_to_string(return_type);
                
                char detail[256];
                if (arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", display_name, methods[i], return_type_str);
                } else if (arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", display_name, methods[i], return_type_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", display_name, methods[i], return_type_str);
                }
                
                add_completion_item(&items, count, &capacity,
                                   methods[i],
                                   LSP_COMP_METHOD,
                                   detail,
                                   NULL);
            }
            native_free_instance_method_list(methods, method_count);
        }
    } else if (is_module_prefix) {
        // 首先尝试从导入的 .leno 模块符号表获取补全
        // 检查是否是导入的 .leno 模块（通过查找模块路径）
        const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
        bool is_leno_module = (module_path != NULL);

        if (is_leno_module) {
            // 是导入的 .leno 模块，从符号表获取补全
            add_module_symbol_completions(content, &items, count, &capacity,
                                          module_alias, member_name,
                                          import_aliases, import_count,
                                          file_path);
        } else {
            // 不是导入的模块，检查是否是当前文件中定义的 enum（如 "Color."）
            // 尝试从当前文件内容中查找 enum 定义
            add_current_file_enum_completions(content, &items, count, &capacity, module_alias);
        }

        // 然后尝试内置模块方法补全
        const char* actual_module = find_module_by_alias(import_aliases, import_count, module_alias);

        // 从编译器获取该模块的所有方法元数据（包含参数类型）
        int method_count = 0;
        ModuleMethodMeta* metas = native_get_module_method_metas(actual_module, &method_count);

        if (metas && method_count > 0) {
            // 是模块方法调用
            for (int i = 0; i < method_count; i++) {
                // 构建详细的签名和文档
                char detail[512];
                char documentation[1024];

                // 获取返回类型字符串
                const char* return_type_str = type_kind_to_string(metas[i].return_type);

                // 构建参数列表
                char params[256] = "";
                if (metas[i].arity == 0) {
                    strcpy(params, "");
                } else if (metas[i].arity < 0) {
                    strcpy(params, "...");
                } else {
                    int offset = 0;
                    for (int p = 0; p < metas[i].arity && p < MAX_METHOD_PARAMS; p++) {
                        const char* param_type = type_kind_to_string(metas[i].param_types[p]);
                        if (p > 0) offset += snprintf(params + offset, sizeof(params) - offset, ", ");
                        offset += snprintf(params + offset, sizeof(params) - offset, "%s", param_type);
                    }
                }

                // 构建显示用的签名 (detail)
                if (metas[i].arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, metas[i].method_name, return_type_str);
                } else if (metas[i].arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, metas[i].method_name, return_type_str);
                } else {
                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", module_alias, metas[i].method_name, params, return_type_str);
                }

                // 构建文档
                snprintf(documentation, sizeof(documentation), "```leno\n%s\n```", detail);

                add_completion_item(&items, count, &capacity,
                                   metas[i].method_name,
                                   LSP_COMP_METHOD,
                                   detail,
                                   NULL);
            }
            native_free_module_method_metas(metas);
        } else if (!is_leno_module) {
            // 不是模块方法，尝试实例方法或 struct 字段补全
            char* var_type = get_variable_type(content, module_alias, file_path);

            if (var_type) {
                // 检查是否是 struct 类型
                if (strstr(var_type, "struct")) {
                    // 解析 struct 名称
                    const char* struct_name = strstr(var_type, "struct");
                    if (struct_name) {
                        struct_name += 6; // 跳过 "struct"
                        while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                        // 首先从当前文件内容中查找 struct 定义
                        int items_before = *count;
                        add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);

                        // 如果在当前文件中找不到，尝试从符号表中查找（支持 use 语句导入的 struct）
                        if (*count == items_before) {
                            fprintf(stderr, "[LSP DEBUG] Struct '%s' not found in current file, trying symbol table\n", struct_name);
                            
                            // 编译代码获取符号表
                            CompilerContext ctx;
                            compiler_context_init(&ctx);
                            fprintf(stderr, "[LSP DEBUG] Calling compiler_analyze_with_filename with file_path: '%s'\n", 
                                    file_path ? file_path : "(null)");
                            bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);
                            fprintf(stderr, "[LSP DEBUG] compiler_analyze_with_filename returned: %d, root_scope: %p\n", 
                                    analyze_ok, (void*)ctx.root_scope);
                            
                            // 如果分析失败且没有 root_scope，尝试去掉包含变量引用的那一行再重试
                            if (!analyze_ok && !ctx.root_scope) {
                                fprintf(stderr, "[LSP DEBUG] First analyze failed, retrying without var line\n");
                                // 去掉包含 "p." 的那一行（未完成的代码）
                                char* content_copy = strdup(content);
                                if (content_copy) {
                                    // 查找 "p." 模式
                                    char* search_pattern = malloc(strlen(module_alias) + 2);
                                    if (search_pattern) {
                                        sprintf(search_pattern, "%s.", module_alias);
                                        char* var_pos = strstr(content_copy, search_pattern);
                                        free(search_pattern);
                                        
                                        if (var_pos) {
                                            // 找到该行的开始（前一个换行符）
                                            char* line_start = content_copy;
                                            char* p = content_copy;
                                            while (p < var_pos) {
                                                if (*p == '\n') line_start = p + 1;
                                                p++;
                                            }
                                            // 找到该行的结束（后一个换行符）
                                            char* line_end = strchr(var_pos, '\n');
                                            if (line_end) {
                                                // 去掉这一行：将 line_end 之后的内容移到 line_start
                                                memmove(line_start, line_end + 1, strlen(line_end + 1) + 1);
                                                fprintf(stderr, "[LSP DEBUG] Retrying without var line, new content: %.100s\n", content_copy);
                                                
                                                compiler_context_cleanup(&ctx);
                                                compiler_context_init(&ctx);
                                                analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                                                fprintf(stderr, "[LSP DEBUG] Retry compiler_analyze_with_filename returned: %d, root_scope: %p\n", 
                                                        analyze_ok, (void*)ctx.root_scope);
                                            }
                                        }
                                    }
                                    free(content_copy);
                                }
                            }
                            
                            if (analyze_ok || ctx.root_scope) {
                                fprintf(stderr, "[LSP DEBUG] Calling add_symbol_table_struct_completions for '%s'\n", struct_name);
                                add_symbol_table_struct_completions(struct_name, module_alias, &ctx, &items, count, &capacity);
                                
                                // 从符号表获取字段后，再从模块源代码中解析方法
                                // 因为符号表中不存储方法信息
                                fprintf(stderr, "[LSP DEBUG] Looking for struct methods in module source\n");
                                for (int i = 0; i < import_count; i++) {
                                    const char* module_path = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                    if (module_path) {
                                        char* module_source = read_module_file(module_path, file_path);
                                        if (module_source) {
                                            // 只解析方法，不解析字段（因为字段已经从符号表获取）
                                            add_module_struct_methods(module_source, struct_name, module_alias, &items, count, &capacity);
                                            free(module_source);
                                            break;
                                        }
                                    }
                                }
                                
                                compiler_context_cleanup(&ctx);
                            } else {
                                fprintf(stderr, "[LSP DEBUG] compiler_analyze failed, skipping symbol table lookup\n");
                            }
                        }

                        // 如果还是找不到，尝试从导入的模块源代码中查找
                        if (*count == items_before) {
                            fprintf(stderr, "[LSP DEBUG] Struct '%s' not found in symbol table, trying imported modules\n", struct_name);
                            // 遍历所有导入的模块，查找 struct 定义
                            for (int i = 0; i < import_count; i++) {
                                const char* module_path = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                if (module_path) {
                                    char* module_source = read_module_file(module_path, file_path);
                                    if (module_source) {
                                        // 尝试从模块源代码中解析 struct 字段和方法
                                        add_current_file_struct_completions(module_source, &items, count, &capacity, struct_name, module_alias);
                                        free(module_source);
                                        if (*count > items_before) {
                                            fprintf(stderr, "[LSP DEBUG] Found struct '%s' in module '%s'\n", struct_name, import_aliases[i].alias);
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    // 获取该类型的实例方法
                    int inst_method_count = 0;
                    char** methods = native_get_instance_methods(var_type, &inst_method_count);

                    if (methods && inst_method_count > 0) {
                        for (int i = 0; i < inst_method_count; i++) {
                            // 获取方法元数据
                            int arity = native_get_instance_method_arity(var_type, methods[i]);
                            TypeKind return_type = native_get_instance_method_return_type(var_type, methods[i], NULL);

                            const char* return_type_str = type_kind_to_string(return_type);

                            char detail[256];
                            if (arity == 0) {
                                snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, methods[i], return_type_str);
                            } else if (arity < 0) {
                                snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                            } else {
                                snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                            }

                            add_completion_item(&items, count, &capacity,
                                               methods[i],
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                        native_free_instance_method_list(methods, inst_method_count);
                    }
                }
                free(var_type);
            }
        }
    } else if (module_alias) {
        // 检查是否是实例方法调用（如 "a." 其中 a 是变量）
        char* var_type = get_variable_type(content, module_alias, file_path);

        if (var_type) {
            // 检查是否是 struct 类型
            if (strstr(var_type, "struct")) {
                // 解析 struct 名称
                const char* struct_name = strstr(var_type, "struct");
                if (struct_name) {
                    struct_name += 6; // 跳过 "struct"
                    while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                    // 从当前文件内容中查找 struct 定义并添加字段补全
                    add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);
                }
            } else {
                // 获取该类型的实例方法
                int method_count = 0;
                char** methods = native_get_instance_methods(var_type, &method_count);

                if (methods && method_count > 0) {
                    for (int i = 0; i < method_count; i++) {
                        // 获取方法元数据
                        int arity = native_get_instance_method_arity(var_type, methods[i]);
                        TypeKind return_type = native_get_instance_method_return_type(var_type, methods[i], NULL);

                        const char* return_type_str = type_kind_to_string(return_type);

                        char detail[256];
                        if (arity == 0) {
                            snprintf(detail, sizeof(detail), "%s.%s() -> %s", module_alias, methods[i], return_type_str);
                        } else if (arity < 0) {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                        } else {
                            snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", module_alias, methods[i], return_type_str);
                        }

                        add_completion_item(&items, count, &capacity,
                                           methods[i],
                                           LSP_COMP_METHOD,
                                           detail,
                                           NULL);
                    }
                    native_free_instance_method_list(methods, method_count);
                }
            }
            free(var_type);
        }
    } else {
        // 正常补全：关键字、类型、内置函数、模块方法、用户符号
        
        // 1. 添加关键字
        for (int i = 0; leno_keywords[i]; i++) {
            add_completion_item(&items, count, &capacity, leno_keywords[i], 
                               LSP_COMP_KEYWORD, "keyword", prefix);
        }
        
        // 2. 添加类型
        for (int i = 0; leno_types[i]; i++) {
            add_completion_item(&items, count, &capacity, leno_types[i],
                               LSP_COMP_CLASS, "type", prefix);
        }
        
        // 3. 添加内置函数（从元数据表获取）
        for (int i = 0; builtin_functions[i].name != NULL; i++) {
            add_completion_item(&items, count, &capacity, builtin_functions[i].name,
                               LSP_COMP_FUNCTION, builtin_functions[i].signature, prefix);
        }
        
        // 4. 添加所有模块的方法（动态获取）
        // 从编译器获取所有可用模块
        int module_count = 0;
        char** modules = native_get_all_modules(&module_count);
        if (modules && module_count > 0) {
            for (int m = 0; m < module_count; m++) {
                int method_count = 0;
                char** methods = native_get_module_methods(modules[m], &method_count);
                if (methods && method_count > 0) {
                    for (int i = 0; i < method_count; i++) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s", modules[m], methods[i]);
                        char signature[256];
                        snprintf(signature, sizeof(signature), "%s.%s(...)", modules[m], methods[i]);
                        add_completion_item(&items, count, &capacity, full_name,
                                           LSP_COMP_METHOD, signature, prefix);
                    }
                    native_free_module_method_list(methods, method_count);
                }
            }
            native_free_module_list(modules, module_count);
        }
        
        // 5. 从编译器符号表添加用户定义的符号
        add_symbols_from_compiler(content, &items, count, &capacity, prefix, file_path);
    }
    
    free(prefix);
    free(module_alias);
    free(member_name);
    free_import_aliases(import_aliases, import_count);

    return items;
}

// 释放补全项
void lsp_free_completions(LspCompletionItem* items, int count) {
    if (!items) return;
    
    for (int i = 0; i < count; i++) {
        free(items[i].label);
        free(items[i].detail);
        free(items[i].documentation);
        free(items[i].insertText);
    }
    
    free(items);
}
