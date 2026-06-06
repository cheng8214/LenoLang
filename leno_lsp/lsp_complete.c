/**
 * 补全服务
 * 提供代码自动补全功能 - 使用 LenoC 编译器符号表
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"
#include "leno_builtins.h"
#include "../src/include/native.h"
#include <ctype.h>
#include <stdlib.h>

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
    "var", "func", "struct", "cstruct", "enum", "face", "impl",
    // 实例化
    "new",
    // 模块
    "import", "export", "as", "use",
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
    "Array", "Dict", "File", "Ptr", "any", "face",
    "i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64",
    "c_int", "c_uint", "c_long", "c_ulong", "c_longlong", "c_ulonglong", "c_size", "c_ssize",
    "bigint", "str16", "Thread", "Channel",
    NULL
};

static const char* cstruct_instance_methods[] = {
    "free", "to_ptr", "to_str", "size", "alignment", "debug", "hex", "free_all",
    NULL
};

static const char* cstruct_static_methods[] = {
    "malloc", "from_ptr", "malloc_array",
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
    // 在内容中查找 enum 定义
    const char* search_pos = content;
    char enum_pattern[256];
    snprintf(enum_pattern, sizeof(enum_pattern), "enum %s", enum_name);

    while (*search_pos) {
        const char* enum_kw = strstr(search_pos, enum_pattern);
        if (!enum_kw) {
            break;
        }

        // 确保这是一个 enum 定义（前面是行首或空白字符）
        const char* check = enum_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            search_pos = enum_kw + 1;
            continue;
        }

        // 检查 enum 名称后面是否跟着空白或 '{'（避免匹配到 enum Colorful 这种）
        const char* after_name = enum_kw + strlen(enum_pattern);
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = enum_kw + 1;
            continue;
        }

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

static char* find_enclosing_struct_name(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return NULL;

    int brace_depth = 0;
    int pos = cursor_offset - 1;

    while (pos >= 0) {
        char c = content[pos];

        if (c == '}') {
            brace_depth++;
            pos--;
            continue;
        }

        if (c == '{') {
            brace_depth--;
            if (brace_depth < 0) {
                int check_pos = pos - 1;
                while (check_pos >= 0 && isspace((unsigned char)content[check_pos])) check_pos--;

                int name_end = check_pos + 1;
                int name_start = name_end;
                while (name_start > 0 && (isalnum((unsigned char)content[name_start - 1]) || content[name_start - 1] == '_')) {
                    name_start--;
                }
                int name_len = name_end - name_start;

                if (name_len == 0) {
                    brace_depth = 0;
                    pos--;
                    continue;
                }

                int kw_end = name_start - 1;
                while (kw_end >= 0 && isspace((unsigned char)content[kw_end])) kw_end--;

                const char* struct_kw = "struct";
                const char* cstruct_kw = "cstruct";
                int struct_kw_len = 6;
                int cstruct_kw_len = 7;

                bool is_struct = false;
                bool is_cstruct = false;

                if (kw_end >= cstruct_kw_len - 1) {
                    int kw_start = kw_end - cstruct_kw_len + 1;
                    if (strncmp(content + kw_start, cstruct_kw, cstruct_kw_len) == 0 &&
                        (kw_start == 0 || (!isalnum((unsigned char)content[kw_start - 1]) && content[kw_start - 1] != '_'))) {
                        is_cstruct = true;
                    }
                }

                if (!is_cstruct && kw_end >= struct_kw_len - 1) {
                    int kw_start = kw_end - struct_kw_len + 1;
                    if (strncmp(content + kw_start, struct_kw, struct_kw_len) == 0 &&
                        (kw_start == 0 || (!isalnum((unsigned char)content[kw_start - 1]) && content[kw_start - 1] != '_'))) {
                        is_struct = true;
                    }
                }

                if (is_struct || is_cstruct) {
                    char* result = (char*)malloc(name_len + 1);
                    if (result) {
                        strncpy(result, content + name_start, name_len);
                        result[name_len] = '\0';
                    }
                    return result;
                }

                brace_depth = 0;
            }
            pos--;
            continue;
        }

        pos--;
    }

    return NULL;
}

// 从编译器符号表中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
// ctx: 编译器上下文
static void add_symbol_table_struct_completions(const char* struct_name, const char* var_name,
                                                CompilerContext* ctx, LspCompletionItem** items, int* count, int* capacity) {
    if (!struct_name || !ctx || !ctx->root_scope) {
        return;
    }

    // 在符号表中查找 struct 定义
    Symbol* struct_sym = scope_resolve_tree_bfs(ctx->root_scope, struct_name);
    if (!struct_sym) {
        return;
    }

    if (struct_sym->type->kind != TYPE_STRUCT && struct_sym->type->kind != TYPE_CSTRUCT) {
        return;
    }

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
    }
}

// 从当前文件内容中查找 struct 定义并提供字段补全
// struct_name: struct 名称（如 "Point"）
// var_name: 变量名（如 "p"），用于构建 detail
static void add_current_file_struct_completions(const char* content, LspCompletionItem** items, int* count, int* capacity,
                                                const char* struct_name, const char* var_name) {
    if (!content || !struct_name) return;

    const char* search_pos = content;
    char struct_pattern[256];
    char cstruct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);
    snprintf(cstruct_pattern, sizeof(cstruct_pattern), "cstruct %s", struct_name);

    while (*search_pos) {
        const char* struct_kw = strstr(search_pos, struct_pattern);
        const char* cstruct_kw = strstr(search_pos, cstruct_pattern);
        const char* found_kw = NULL;
        int matched_pattern_len = 0;

        if (cstruct_kw && (!struct_kw || cstruct_kw < struct_kw)) {
            if (cstruct_kw > content) {
                char before = *(cstruct_kw - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = cstruct_kw + 1;
                    continue;
                }
            }
            found_kw = cstruct_kw;
            matched_pattern_len = strlen(cstruct_pattern);
        } else if (struct_kw) {
            if (struct_kw > content) {
                char before = *(struct_kw - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = struct_kw + 1;
                    continue;
                }
            }
            found_kw = struct_kw;
            matched_pattern_len = strlen(struct_pattern);
        } else {
            break;
        }

        const char* check = found_kw - 1;
        while (check > content && isspace((unsigned char)*check)) check--;
        if (check > content && *check != '\n' && *check != ';' && *check != '}') {
            if (check >= content + 5) {
                if (strncmp(check - 5, "export", 6) != 0) {
                    search_pos = found_kw + 1;
                    continue;
                }
            } else {
                search_pos = found_kw + 1;
                continue;
            }
        }

        const char* after_name = found_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = found_kw + 1;
            continue;
        }

        const char* brace = strchr(found_kw, '{');
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

static void find_face_methods_in_content(const char* content, const char* face_name,
                                         LspCompletionItem** items, int* count, int* capacity,
                                         const char* var_name) {
    if (!content || !face_name) return;

    const char* search_pos = content;
    char face_pattern[256];
    char export_face_pattern[256];
    snprintf(face_pattern, sizeof(face_pattern), "face %s", face_name);
    snprintf(export_face_pattern, sizeof(export_face_pattern), "export face %s", face_name);

    while (*search_pos) {
        const char* face_kw = NULL;
        int matched_pattern_len = 0;

        const char* p_export_face = strstr(search_pos, export_face_pattern);
        const char* p_face = strstr(search_pos, face_pattern);

        if (p_export_face && (!p_face || p_export_face < p_face)) {
            face_kw = p_export_face;
            matched_pattern_len = strlen(export_face_pattern);
        } else if (p_face) {
            if (p_face > content) {
                char before = *(p_face - 1);
                if (isalnum((unsigned char)before) || before == '_') {
                    search_pos = p_face + 1;
                    continue;
                }
            }
            face_kw = p_face;
            matched_pattern_len = strlen(face_pattern);
        } else {
            break;
        }

        const char* after_name = face_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = face_kw + 1;
            continue;
        }

        const char* brace = strchr(face_kw, '{');
        if (!brace) break;

        const char* end_brace = brace + 1;
        int brace_count = 1;
        while (*end_brace && brace_count > 0) {
            if (*end_brace == '{') brace_count++;
            else if (*end_brace == '}') brace_count--;
            end_brace++;
        }
        if (brace_count != 0) break;
        end_brace--;

        const char* p = brace + 1;
        while (p < end_brace) {
            while (p < end_brace && isspace((unsigned char)*p)) p++;
            if (p >= end_brace) break;

            if (p + 1 < end_brace && p[0] == '/' && p[1] == '/') {
                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            if (strncmp(p, "func", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
                const char* func_name_start = p + 4;
                while (*func_name_start && isspace((unsigned char)*func_name_start)) func_name_start++;
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

                while (p < end_brace && *p != '\n') p++;
                continue;
            }

            while (p < end_brace && *p != '\n') p++;
        }

        break;
    }
}

// 从模块源代码中解析指定 struct 的方法
// module_source: 模块文件内容
// struct_name: struct 名称
// module_alias: 模块别名（用于构建 detail）
static void add_module_struct_methods(const char* module_source, const char* struct_name, const char* module_alias,
                                      LspCompletionItem** items, int* count, int* capacity) {
    if (!module_source || !struct_name) {
        return;
    }

    const char* search_pos = module_source;
    char struct_pattern[256];
    char export_struct_pattern[256];
    char cstruct_pattern[256];
    char export_cstruct_pattern[256];
    snprintf(struct_pattern, sizeof(struct_pattern), "struct %s", struct_name);
    snprintf(export_struct_pattern, sizeof(export_struct_pattern), "export struct %s", struct_name);
    snprintf(cstruct_pattern, sizeof(cstruct_pattern), "cstruct %s", struct_name);
    snprintf(export_cstruct_pattern, sizeof(export_cstruct_pattern), "export cstruct %s", struct_name);

    while (*search_pos) {
        const char* struct_kw = NULL;
        int matched_pattern_len = 0;

        const char* p_export_struct = strstr(search_pos, export_struct_pattern);
        const char* p_export_cstruct = strstr(search_pos, export_cstruct_pattern);
        const char* p_struct = strstr(search_pos, struct_pattern);
        const char* p_cstruct = strstr(search_pos, cstruct_pattern);

        if (p_export_cstruct && p_export_cstruct < (p_export_struct ? p_export_struct : (const char*)-1)) {
            struct_kw = p_export_cstruct;
            matched_pattern_len = strlen(export_cstruct_pattern);
        } else if (p_export_struct) {
            struct_kw = p_export_struct;
            matched_pattern_len = strlen(export_struct_pattern);
        } else if (p_cstruct && p_cstruct < (p_struct ? p_struct : (const char*)-1)) {
            struct_kw = p_cstruct;
            matched_pattern_len = strlen(cstruct_pattern);
        } else if (p_struct) {
            if (p_struct > module_source) {
                char before = *(p_struct - 1);
                if (before == 'c') {
                    search_pos = p_struct + 1;
                    continue;
                }
            }
            struct_kw = p_struct;
            matched_pattern_len = strlen(struct_pattern);
        }

        if (!struct_kw) {
            break;
        }

        // 确保这是一个 struct 定义（前面是行首或空白字符）
        const char* check = struct_kw - 1;
        while (check > module_source && isspace((unsigned char)*check)) check--;
        if (check > module_source && *check != '\n' && *check != ';' && *check != '}') {
            search_pos = struct_kw + 1;
            continue;
        }

        const char* after_name = struct_kw + matched_pattern_len;
        while (*after_name && isspace((unsigned char)*after_name)) after_name++;
        if (*after_name != '{' && *after_name != '\n' && *after_name != '\r' && *after_name != '\0') {
            search_pos = struct_kw + 1;
            continue;
        }

        // 找到了 struct 定义，解析方法
        const char* brace = strchr(struct_kw, '{');
        if (!brace) {
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
                                          const char* current_file, const char* prefix) {
    (void)content;
    const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
    if (!module_path) return;

    char* module_source = read_module_file(module_path, current_file);
    if (!module_source) {
        return;
    }

    CompilerContext ctx;
    compiler_context_init(&ctx);
    bool analyzed = compiler_analyze_with_filename(&ctx, module_source, current_file);

    if (!analyzed || !ctx.root_scope) {
        free(module_source);
        compiler_context_cleanup(&ctx);
        return;
    }

    if (member_name && *member_name) {
        Symbol* sym = scope_resolve_tree_bfs(ctx.root_scope, member_name);
        bool is_type_symbol = (sym && sym->kind == SYM_TYPE) ||
                              (sym && sym->kind == SYM_MODULE && sym->type &&
                               (sym->type->kind == TYPE_STRUCT || sym->type->kind == TYPE_ENUM));
        if (is_type_symbol) {
            if (sym->type && sym->type->kind == TYPE_ENUM) {
                char detail[256];
                snprintf(detail, sizeof(detail), "%s: enum", member_name);
                add_completion_item(items, count, capacity,
                                   member_name,
                                   LSP_COMP_ENUM,
                                   detail,
                                   prefix);
            } else if (sym->type && sym->type->kind == TYPE_STRUCT) {
                if (sym->struct_field_names && sym->struct_field_types) {
                    for (int i = 0; i < sym->struct_field_count; i++) {
                        const char* type_str = type_to_string(sym->struct_field_types[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s: %s", member_name, sym->struct_field_names[i], type_str);

                        add_completion_item(items, count, capacity,
                                           sym->struct_field_names[i],
                                           LSP_COMP_FIELD,
                                           detail,
                                           prefix);
                    }
                }
                add_module_struct_methods(module_source, member_name, module_alias, items, count, capacity);
            }
        }
    } else {
        for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
            Symbol* sym = ctx.root_scope->syms[i];

            if (sym->scope != ctx.root_scope) {
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
                                       prefix);
                    break;
                case SYM_TYPE:
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        snprintf(detail, sizeof(detail), "%s.%s: struct", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_STRUCT,
                                           detail,
                                           prefix);
                    } else if (sym->type && sym->type->kind == TYPE_ENUM) {
                        snprintf(detail, sizeof(detail), "%s.%s: enum", module_alias, sym->name);
                        add_completion_item(items, count, capacity,
                                           sym->name,
                                           LSP_COMP_ENUM,
                                           detail,
                                           prefix);
                    }
                    break;
                case SYM_CLIB:
                    snprintf(detail, sizeof(detail), "%s.%s: clib", module_alias, sym->name);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_CLASS,
                                       detail,
                                       prefix);
                    break;
                case SYM_GLOBAL:
                case SYM_MODULE:
                    snprintf(detail, sizeof(detail), "%s.%s: %s", module_alias, sym->name, type_str);
                    add_completion_item(items, count, capacity,
                                       sym->name,
                                       LSP_COMP_VARIABLE,
                                       detail,
                                       prefix);
                    break;
                default:
                    break;
            }
        }
    }

    free(module_source);
    compiler_context_cleanup(&ctx);
}

// 检测光标是否在 Style[xxx] 变量的初始化 Dict 中
// 返回 Style 目标控件名（如 "window", "button"），如果不是 Style 上下文则返回 NULL
static char* detect_style_context(const char* content, LspPosition pos) {
    if (!content) return NULL;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return NULL;
    
    // 向前查找最近的 "Style[" 模式
    int search_pos = offset - 1;
    int brace_depth = 0;
    int in_string = 0;
    char string_quote = 0;
    
    // 先确定当前光标在 Dict 字面量内部（{...}）
    while (search_pos >= 0) {
        char c = content[search_pos];
        
        if (in_string) {
            if (c == string_quote) {
                // 检查是否是转义的
                int backslash_count = 0;
                int check = search_pos - 1;
                while (check >= 0 && content[check] == '\\') {
                    backslash_count++;
                    check--;
                }
                if (backslash_count % 2 == 0) {
                    in_string = 0;
                }
            }
        } else {
            if (c == '"' || c == '\'') {
                in_string = 1;
                string_quote = c;
            } else if (c == '}') {
                brace_depth++;
            } else if (c == '{') {
                if (brace_depth == 0) {
                    // 找到了包含光标的 Dict 开始
                    break;
                }
                brace_depth--;
            }
        }
        search_pos--;
    }
    
    if (search_pos < 0) return NULL;
    
    // 现在 search_pos 指向 Dict 的 '{'，向前查找 "Style[xxx]"
    // 格式: Style[xxx] varname = { 或 Style[xxx] varname={
    int style_pos = search_pos - 1;
    // 跳过 '{' 前的空白
    while (style_pos >= 0 && isspace((unsigned char)content[style_pos])) style_pos--;
    // 跳过 '=' 号
    if (style_pos >= 0 && content[style_pos] == '=') style_pos--;
    // 跳过 '=' 前的空白
    while (style_pos >= 0 && isspace((unsigned char)content[style_pos])) style_pos--;
    // 跳过变量名（如 "st"）
    while (style_pos >= 0 && (isalnum((unsigned char)content[style_pos]) || content[style_pos] == '_')) style_pos--;
    // 跳过变量名前的空白
    while (style_pos >= 0 && isspace((unsigned char)content[style_pos])) style_pos--;
    // 现在 style_pos 应该指向 ']'（Style[xxx] 的结束括号）
    if (style_pos >= 0 && content[style_pos] == ']') {
        // 向前查找匹配的 '['
        int bracket_end = style_pos;
        int bracket_start = bracket_end - 1;
        while (bracket_start >= 0 && content[bracket_start] != '[') bracket_start--;
        if (bracket_start >= 0 && content[bracket_start] == '[') {
            // 检查 '[' 前面是否是 "Style"
            int style_kw_end = bracket_start; // 指向 '['
            int style_kw_start = style_kw_end - 1;
            while (style_kw_start >= 0 && (isalnum((unsigned char)content[style_kw_start]) || content[style_kw_start] == '_')) {
                style_kw_start--;
            }
            style_kw_start++; // 指向关键字首字母
            int kw_len = style_kw_end - style_kw_start;
            if (kw_len == 5 && strncmp(content + style_kw_start, "Style", 5) == 0) {
                // 找到了 Style[xxx]
                int target_start = bracket_start + 1; // 跳过 '['
                int target_end = bracket_end; // 在 ']' 之前
                
                // 跳过空白
                while (target_start < target_end && isspace((unsigned char)content[target_start])) target_start++;
                while (target_end > target_start && isspace((unsigned char)content[target_end - 1])) target_end--;
                
                int target_len = target_end - target_start;
                if (target_len > 0 && target_len < 64) {
                    char* target = (char*)malloc(target_len + 1);
                    if (target) {
                        memcpy(target, content + target_start, target_len);
                        target[target_len] = '\0';
                        return target;
                    }
                }
            }
        }
    }
    
    return NULL;
}

// 复用 guis_style.c 中的字段定义
extern const char** guis_get_style_fields(const char* target, int* count);
extern void* guis_get_style_field_info(const char* target, const char* field_name);
extern const char* guis_style_field_type_name(int type);

typedef struct {
    const char* name;
    int type;
    const char* description;
    const char* default_value;
    const char** options;
    int option_count;
} StyleFieldInfo;

// 添加 Style 字段补全（带类型信息）
static void add_style_field_completions(const char* target, LspCompletionItem** items,
                                         int* count, int* capacity, const char* prefix) {
    if (!target) return;
    
    int field_count = 0;
    const char** fields = guis_get_style_fields(target, &field_count);
    
    if (fields && field_count > 0) {
        for (int i = 0; i < field_count; i++) {
            StyleFieldInfo* info = (StyleFieldInfo*)guis_get_style_field_info(target, fields[i]);
            char detail[512];
            
            if (info) {
                const char* type_name = guis_style_field_type_name(info->type);
                if (info->type == 5 && info->options && info->option_count > 0) {
                    // 枚举类型，显示可选值
                    char options_str[256] = {0};
                    int pos = 0;
                    for (int j = 0; j < info->option_count && pos < 200; j++) {
                        int len = strlen(info->options[j]);
                        if (pos + len + 2 < (int)sizeof(options_str)) {
                            if (j > 0) {
                                options_str[pos++] = '|';
                            }
                            memcpy(options_str + pos, info->options[j], len);
                            pos += len;
                        }
                    }
                    options_str[pos] = '\0';
                    snprintf(detail, sizeof(detail), "%s %s (%s) 默认:%s", 
                            type_name, info->description, options_str, info->default_value);
                } else {
                    snprintf(detail, sizeof(detail), "%s %s 默认:%s", 
                            type_name, info->description, info->default_value);
                }
            } else {
                snprintf(detail, sizeof(detail), "Style[%s].%s", target, fields[i]);
            }
            
            add_completion_item(items, count, capacity,
                               fields[i],
                               LSP_COMP_FIELD,
                               detail,
                               prefix);
        }
        free(fields);
    }
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
    
    return -1;
}

static bool is_type_annotation_context(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return false;

    int pos = cursor_offset - 1;

    while (pos >= 0 && (isalnum((unsigned char)content[pos]) || content[pos] == '_')) {
        pos--;
    }

    while (pos >= 0 && isspace((unsigned char)content[pos])) {
        pos--;
    }

    if (pos < 0) return false;

    if (content[pos] == ':') {
        int check = pos - 1;
        while (check >= 0 && isspace((unsigned char)content[check])) check--;

        if (check >= 0 && content[check] == ')') {
            return true;
        }

        if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
            while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                check--;
            }
            while (check >= 0 && isspace((unsigned char)content[check])) check--;

            if (check >= 2 && content[check - 2] == 'v' && content[check - 1] == 'a' && content[check] == 'r' &&
                (check == 2 || (!isalnum((unsigned char)content[check - 3]) && content[check - 3] != '_'))) {
            }

            int paren_depth = 0;
            int p2 = check;
            while (p2 >= 0) {
                if (content[p2] == ')') paren_depth++;
                else if (content[p2] == '(') {
                    paren_depth--;
                    if (paren_depth < 0) {
                        p2--;
                        while (p2 >= 0 && isspace((unsigned char)content[p2])) p2--;
                        if (p2 >= 0 && (isalnum((unsigned char)content[p2]) || content[p2] == '_')) {
                            while (p2 >= 0 && (isalnum((unsigned char)content[p2]) || content[p2] == '_')) p2--;
                            while (p2 >= 0 && isspace((unsigned char)content[p2])) p2--;
                            if (p2 >= 3 && content[p2 - 3] == 'f' && content[p2 - 2] == 'u' && content[p2 - 1] == 'n' && content[p2] == 'c' &&
                                (p2 == 3 || (!isalnum((unsigned char)content[p2 - 4]) && content[p2 - 4] != '_'))) {
                                return true;
                            }
                        }
                        break;
                    }
                }
                p2--;
            }
        }
        return false;
    }

    if (pos >= 1 && content[pos - 1] == 'a' && content[pos] == 's') {
        if (pos == 1 || (!isalnum((unsigned char)content[pos - 2]) && content[pos - 2] != '_')) {
            int line_start = pos - 2;
            while (line_start >= 0 && content[line_start] != '\n') line_start--;
            if (line_start < 0) line_start = 0;
            else line_start++;

            bool is_import = false;
            const char* line = content + line_start;
            const char* ip = strstr(line, "import");
            if (ip && ip < content + pos - 1) {
                if (ip == line || (!isalnum((unsigned char)*(ip - 1)) && *(ip - 1) != '_')) {
                    if (!isalnum((unsigned char)ip[6]) && ip[6] != '_') {
                        is_import = true;
                    }
                }
            }

            if (!is_import) {
                return true;
            }
        }
    }

    if (content[pos] == '(' || content[pos] == ',') {
        if (content[pos] == '(') {
            int check = pos - 1;
            while (check >= 0 && isspace((unsigned char)content[check])) check--;
            if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) check--;
                while (check >= 0 && isspace((unsigned char)content[check])) check--;
                if (check >= 3 && content[check - 3] == 'f' && content[check - 2] == 'u' && content[check - 1] == 'n' && content[check] == 'c' &&
                    (check == 3 || (!isalnum((unsigned char)content[check - 4]) && content[check - 4] != '_'))) {
                    return true;
                }
            }
        } else {
            int paren_depth = 0;
            int check = pos - 1;
            while (check >= 0) {
                if (content[check] == ')') paren_depth++;
                else if (content[check] == '(') {
                    paren_depth--;
                    if (paren_depth < 0) {
                        check--;
                        while (check >= 0 && isspace((unsigned char)content[check])) check--;
                        if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
                            while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) check--;
                            while (check >= 0 && isspace((unsigned char)content[check])) check--;
                            if (check >= 3 && content[check - 3] == 'f' && content[check - 2] == 'u' && content[check - 1] == 'n' && content[check] == 'c' &&
                                (check == 3 || (!isalnum((unsigned char)content[check - 4]) && content[check - 4] != '_'))) {
                                return true;
                            }
                        }
                        break;
                    }
                }
                check--;
            }
        }
    }

    {
        int line_start = pos;
        while (line_start > 0 && content[line_start - 1] != '\n') line_start--;
        const char* line = content + line_start;
        int line_len = pos - line_start;
        if (line_len == 0) return false;

        char* line_copy = (char*)malloc(line_len + 1);
        memcpy(line_copy, line, line_len);
        line_copy[line_len] = '\0';

        char* trimmed = line_copy;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;

        bool is_type_context = false;

        if (trimmed[0] == '\0') {
            is_type_context = true;
        } else {
            static const char* type_prefixes[] = {
                "func ", "export func ", "struct ", "export struct ",
                "cstruct ", "export cstruct ", "enum ", "export enum ",
                "face ", "export face ",
                NULL
            };
            for (int i = 0; type_prefixes[i]; i++) {
                if (strncmp(trimmed, type_prefixes[i], strlen(type_prefixes[i])) == 0) {
                    is_type_context = true;
                    break;
                }
            }
        }

        free(line_copy);

        if (is_type_context) {
            return true;
        }
    }

    return false;
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

    // 即使分析有错误，也尝试获取符号信息
    bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

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

                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        
                        if (analyze_ok || ctx.root_scope) {
                            // 解析成功，跳出循环
                            break;
                        }
                        retry_count++;
                    } else {
                        // 没有找到换行符，可能是最后一行
                        *line_start = '\0';
                        
                        // 清理之前的上下文
                        compiler_context_cleanup(&ctx);
                        compiler_context_init(&ctx);

                        // 重新解析
                        analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                        break;
                    }
                }
                free(search_pattern);
            }
            free(content_copy);
        }
    }

    char* type_str = NULL;
    if (ctx.root_scope && compiler_get_symbol_info(&ctx, var_name, &type_str, NULL)) {
        // 将编译器的类型名转换为实例方法类型名
        char* result = NULL;
        // 先检查 struct，因为 type_str 可能是 "struct Point" 包含 "int"
        if (strstr(type_str, "struct")) {
            result = strdup(type_str);
        } else if (strstr(type_str, "face")) {
            result = strdup(type_str);
        } else if (strcmp(type_str, "int") == 0 || strcmp(type_str, "float") == 0 || strcmp(type_str, "bigint") == 0) {
            result = strdup("number");
        } else if (strcmp(type_str, "string") == 0) {
            result = strdup("string");
        } else if (strncmp(type_str, "Array", 5) == 0) {
            result = strdup("array");
        } else if (strncmp(type_str, "Dict", 4) == 0) {
            result = strdup("dict");
        } else if (strcmp(type_str, "bool") == 0) {
            result = strdup("bool");
        } else if (strcmp(type_str, "null") == 0) {
            result = strdup("null");
        } else if (strncmp(type_str, "Ptr", 3) == 0) {
            result = strdup("ptr");
        } else if (strcmp(type_str, "File") == 0) {
            result = strdup("file");
        } else if (strcmp(type_str, "Win") == 0) {
            result = strdup("win");
        } else if (strcmp(type_str, "Draw") == 0) {
            result = strdup("draw");
        } else if (strcmp(type_str, "Event") == 0) {
            result = strdup("event");
        } else if (strcmp(type_str, "Image") == 0) {
            result = strdup("image");
        } else if (strcmp(type_str, "Font") == 0) {
            result = strdup("font");
        } else if (strncmp(type_str, "Style", 5) == 0) {
            // Style[window] -> "style:window"，保留目标信息用于字段补全
            if (type_str[5] == '[') {
                const char* start = type_str + 6;
                const char* end = strchr(start, ']');
                if (end) {
                    int len = (int)(end - start);
                    char* buf = malloc(len + 8);
                    memcpy(buf, "style:", 6);
                    memcpy(buf + 6, start, len);
                    buf[6 + len] = '\0';
                    result = buf;
                }
            }
            if (!result) result = strdup("style");
        } else if (strcmp(type_str, "Rgb") == 0) {
            result = strdup("rgb");
        } else if (strcmp(type_str, "thread") == 0) {
            result = strdup("thread");
        } else if (strcmp(type_str, "channel") == 0) {
            result = strdup("channel");
        } else {
            // 尝试将类型名转为小写作为实例方法查找键
            char lower[64];
            int len = strlen(type_str);
            if (len < 64) {
                for (int i = 0; i <= len; i++) lower[i] = tolower((unsigned char)type_str[i]);
                int test_count = 0;
                char** test_methods = native_get_instance_methods(lower, &test_count);
                if (test_methods && test_count > 0) {
                    native_free_instance_method_list(test_methods, test_count);
                    result = strdup(lower);
                }
            }
        }
        free(type_str);
        compiler_context_cleanup(&ctx);

        // 验证该类型是否有实例方法（只对非struct类型）
        if (result && strstr(result, "struct") == NULL && strstr(result, "face") == NULL) {
            int test_count = 0;
            char** test_methods = native_get_instance_methods(result, &test_count);
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

    compiler_context_cleanup(&ctx);
    return NULL;
}

static int detect_use_context(const char* content, LspPosition pos, char** use_module_out, char** use_prefix_out) {
    if (!content) return 0;

    *use_module_out = NULL;
    *use_prefix_out = NULL;

    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return 0;

    int line_start = offset - 1;
    while (line_start > 0 && content[line_start - 1] != '\n') line_start--;

    int p = line_start;
    while (p < offset && isspace((unsigned char)content[p])) p++;

    if (p + 3 > offset) return 0;
    if (strncmp(content + p, "use", 3) != 0) return 0;

    if (p > line_start && (isalnum((unsigned char)content[p-1]) || content[p-1] == '_')) return 0;

    if (p + 3 < offset && (isalnum((unsigned char)content[p+3]) || content[p+3] == '_')) return 0;

    if (p + 3 >= offset) return 0;

    int after_use = p + 3;
    while (after_use < offset && isspace((unsigned char)content[after_use])) after_use++;

    if (after_use >= offset) return 1;

    int alias_start = after_use;
    int q = after_use;
    while (q < offset && (isalnum((unsigned char)content[q]) || content[q] == '_')) q++;

    int alias_len = q - alias_start;
    if (alias_len == 0) return 1;

    if (q < offset && content[q] == '.') {
        *use_module_out = (char*)malloc(alias_len + 1);
        if (*use_module_out) {
            memcpy(*use_module_out, content + alias_start, alias_len);
            (*use_module_out)[alias_len] = '\0';
        }

        int after_dot = q + 1;
        if (after_dot < offset) {
            int sym_start = after_dot;
            int r = after_dot;
            while (r < offset && (isalnum((unsigned char)content[r]) || content[r] == '_')) r++;
            int sym_len = r - sym_start;
            if (sym_len > 0) {
                *use_prefix_out = (char*)malloc(sym_len + 1);
                if (*use_prefix_out) {
                    memcpy(*use_prefix_out, content + sym_start, sym_len);
                    (*use_prefix_out)[sym_len] = '\0';
                }
            }
        }

        return 2;
    }

    *use_prefix_out = (char*)malloc(alias_len + 1);
    if (*use_prefix_out) {
        memcpy(*use_prefix_out, content + alias_start, alias_len);
        (*use_prefix_out)[alias_len] = '\0';
    }
    return 1;
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

    // 检查是否是 "new " 后面（需要补全 struct 名称）
    bool is_new_prefix = false;
    {
        int offset = lsp_position_to_offset(content, pos);
        if (offset > 4) {
            int start = offset - 1;
            while (start >= 0 && isspace((unsigned char)content[start])) start--;
            if (start >= 3 &&
                (content[start-3] == 'n' || content[start-3] == 'N') &&
                (content[start-2] == 'e' || content[start-2] == 'E') &&
                (content[start-1] == 'w' || content[start-1] == 'W') &&
                (start == 3 || !isalnum((unsigned char)content[start-4]))) {
                is_new_prefix = true;
            }
        }
    }

    // 检查是否是模块前缀（如 "maths." 或 "color_module.Color."）
    char* module_alias = NULL;
    char* member_name = NULL;
    bool is_module_prefix = parse_module_prefix(prefix, &module_alias, &member_name);

    char* use_module = NULL;
    char* use_prefix = NULL;
    int use_context = detect_use_context(content, pos, &use_module, &use_prefix);

    // 分配补全数组
    int capacity = 256;
    LspCompletionItem* items = (LspCompletionItem*)malloc(
        sizeof(LspCompletionItem) * capacity);
    if (!items) {
        free(prefix);
        free(module_alias);
        free(member_name);
        free(use_module);
        free(use_prefix);
        free_import_aliases(import_aliases, import_count);
        return NULL;
    }
    
    if (is_new_prefix) {
        // "new " 后补全：列出所有 struct 名称和模块别名
        CompilerContext ctx;
        compiler_context_init(&ctx);
        compiler_analyze_with_filename(&ctx, content, file_path);

        if (ctx.root_scope) {
            for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                Symbol* sym = ctx.root_scope->syms[i];
                if (sym->type && sym->type->kind == TYPE_STRUCT) {
                    char detail[256];
                    snprintf(detail, sizeof(detail), "struct %s", sym->name);
                    add_completion_item(&items, count, &capacity,
                                       sym->name,
                                       LSP_COMP_STRUCT,
                                       detail,
                                       prefix);
                }
            }
        }
        compiler_context_cleanup(&ctx);

        // 也列出模块别名（用于 new module.StructName()）
        for (int i = 0; i < import_count; i++) {
            char detail[256];
            snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
            add_completion_item(&items, count, &capacity,
                               import_aliases[i].alias,
                               LSP_COMP_MODULE,
                               detail,
                               prefix);
        }
    } else if (use_context == 1) {
        for (int i = 0; i < import_count; i++) {
            char detail[256];
            snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
            add_completion_item(&items, count, &capacity,
                               import_aliases[i].alias,
                               LSP_COMP_MODULE,
                               detail,
                               use_prefix);
        }
    } else if (use_context == 2) {
        const char* mod_path = find_module_path_by_alias(import_aliases, import_count, use_module);
        bool is_leno_mod = (mod_path != NULL);

        if (is_leno_mod) {
            add_module_symbol_completions(content, &items, count, &capacity,
                                          use_module, NULL,
                                          import_aliases, import_count,
                                          file_path, use_prefix);
        }

        const char* actual_mod = find_module_by_alias(import_aliases, import_count, use_module);
        int mtd_count = 0;
        ModuleMethodMeta* metas = native_get_module_method_metas(actual_mod, &mtd_count);
        if (metas && mtd_count > 0) {
            for (int i = 0; i < mtd_count; i++) {
                char detail[512];
                const char* ret_str = type_kind_to_string(metas[i].return_type);
                if (metas[i].arity == 0) {
                    snprintf(detail, sizeof(detail), "%s.%s() -> %s", use_module, metas[i].method_name, ret_str);
                } else if (metas[i].arity < 0) {
                    snprintf(detail, sizeof(detail), "%s.%s(...) -> %s", use_module, metas[i].method_name, ret_str);
                } else {
                    char params[256] = "";
                    int off = 0;
                    for (int pp = 0; pp < metas[i].arity && pp < MAX_METHOD_PARAMS; pp++) {
                        const char* pt = type_kind_to_string(metas[i].param_types[pp]);
                        if (pp > 0) off += snprintf(params + off, sizeof(params) - off, ", ");
                        off += snprintf(params + off, sizeof(params) - off, "%s", pt);
                    }
                    snprintf(detail, sizeof(detail), "%s.%s(%s) -> %s", use_module, metas[i].method_name, params, ret_str);
                }
                add_completion_item(&items, count, &capacity,
                                   metas[i].method_name,
                                   LSP_COMP_METHOD,
                                   detail,
                                   use_prefix);
            }
            native_free_module_method_metas(metas);
        }
    } else if (is_string_literal) {
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
        if (strcmp(module_alias, "self") == 0) {
            int cursor_offset = lsp_position_to_offset(content, pos);
            char* enclosing_struct = find_enclosing_struct_name(content, cursor_offset);

            if (enclosing_struct) {
                add_current_file_struct_completions(content, &items, count, &capacity, enclosing_struct, "self");

                CompilerContext ctx;
                compiler_context_init(&ctx);
                bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

                if (analyze_ok || ctx.root_scope) {
                    add_symbol_table_struct_completions(enclosing_struct, "self", &ctx, &items, count, &capacity);
                }
                compiler_context_cleanup(&ctx);

                free(enclosing_struct);
            }
        } else {
        const char* module_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
        bool is_leno_module = (module_path != NULL);

        if (is_leno_module) {
            add_module_symbol_completions(content, &items, count, &capacity,
                                          module_alias, member_name,
                                          import_aliases, import_count,
                                          file_path, NULL);
        } else {
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
            char* var_type = get_variable_type(content, module_alias, file_path);

            if (var_type) {
                bool is_cstruct_type = (strstr(var_type, "cstruct") != NULL);
                bool is_face_type = (strstr(var_type, "face") != NULL);
                if (is_cstruct_type || strstr(var_type, "struct")) {
                    const char* keyword = is_cstruct_type ? "cstruct" : "struct";
                    const char* struct_name = strstr(var_type, keyword);
                    if (struct_name) {
                        struct_name += strlen(keyword);
                        while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                        if (is_cstruct_type && strcmp(module_alias, struct_name) == 0) {
                            for (int i = 0; cstruct_static_methods[i]; i++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_static_methods[i]);
                                add_completion_item(&items, count, &capacity,
                                                   cstruct_static_methods[i],
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        } else {
                            int items_before = *count;
                            add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);

                            if (*count == items_before) {
                                CompilerContext ctx;
                                compiler_context_init(&ctx);
                                bool analyze_ok = compiler_analyze_with_filename(&ctx, content, file_path);

                                if (!analyze_ok && !ctx.root_scope) {
                                    char* content_copy = strdup(content);
                                    if (content_copy) {
                                        char* search_pattern = malloc(strlen(module_alias) + 2);
                                        if (search_pattern) {
                                            sprintf(search_pattern, "%s.", module_alias);
                                            char* var_pos = strstr(content_copy, search_pattern);
                                            free(search_pattern);
                                            
                                            if (var_pos) {
                                                char* line_start = content_copy;
                                                char* p = content_copy;
                                                while (p < var_pos) {
                                                    if (*p == '\n') line_start = p + 1;
                                                    p++;
                                                }
                                                char* line_end = strchr(var_pos, '\n');
                                                if (line_end) {
                                                    memmove(line_start, line_end + 1, strlen(line_end + 1) + 1);
                                                    
                                                    compiler_context_cleanup(&ctx);
                                                    compiler_context_init(&ctx);
                                                    analyze_ok = compiler_analyze_with_filename(&ctx, content_copy, file_path);
                                                }
                                            }
                                        }
                                        free(content_copy);
                                    }
                                }
                                
                                if (analyze_ok || ctx.root_scope) {
                                    add_symbol_table_struct_completions(struct_name, module_alias, &ctx, &items, count, &capacity);

                                    for (int i = 0; i < import_count; i++) {
                                        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                        if (mp) {
                                            char* module_source = read_module_file(mp, file_path);
                                            if (module_source) {
                                                add_module_struct_methods(module_source, struct_name, module_alias, &items, count, &capacity);
                                                free(module_source);
                                                break;
                                            }
                                        }
                                    }
                                    
                                    compiler_context_cleanup(&ctx);
                                }
                            }

                            if (*count == items_before) {
                                for (int i = 0; i < import_count; i++) {
                                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                    if (mp) {
                                        char* module_source = read_module_file(mp, file_path);
                                        if (module_source) {
                                            add_current_file_struct_completions(module_source, &items, count, &capacity, struct_name, module_alias);
                                            free(module_source);
                                            if (*count > items_before) {
                                                break;
                                            }
                                        }
                                    }
                                }
                            }

                            if (is_cstruct_type) {
                                for (int i = 0; cstruct_instance_methods[i]; i++) {
                                    char detail[256];
                                    snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_instance_methods[i]);
                                    add_completion_item(&items, count, &capacity,
                                                       cstruct_instance_methods[i],
                                                       LSP_COMP_METHOD,
                                                       detail,
                                                       NULL);
                                }
                            } else {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.copy()", module_alias);
                                add_completion_item(&items, count, &capacity,
                                                   "copy",
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        }
                    }
                } else if (is_face_type) {
                    const char* face_name = strstr(var_type, "face");
                    face_name += 4;
                    while (*face_name && isspace((unsigned char)*face_name)) face_name++;

                    int items_before = *count;
                    find_face_methods_in_content(content, face_name, &items, count, &capacity, module_alias);

                    if (*count == items_before) {
                        for (int i = 0; i < import_count; i++) {
                            const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                            if (mp) {
                                char* module_source = read_module_file(mp, file_path);
                                if (module_source) {
                                    find_face_methods_in_content(module_source, face_name, &items, count, &capacity, module_alias);
                                    free(module_source);
                                    if (*count > items_before) break;
                                }
                            }
                        }
                    }
                } else if (strncmp(var_type, "style:", 6) == 0) {
                    // Style[window] 等类型的字段补全
                    const char* style_target = var_type + 6;
                    add_style_field_completions(style_target, &items, count, &capacity, prefix);
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
        }
    } else if (module_alias) {
        char* var_type = get_variable_type(content, module_alias, file_path);

        if (var_type) {
            bool is_cstruct_var = (strstr(var_type, "cstruct") != NULL);
            bool is_face_var = (strstr(var_type, "face") != NULL);
            if (is_cstruct_var || strstr(var_type, "struct")) {
                const char* keyword = is_cstruct_var ? "cstruct" : "struct";
                const char* struct_name = strstr(var_type, keyword);
                if (struct_name) {
                    struct_name += strlen(keyword);
                    while (*struct_name && isspace((unsigned char)*struct_name)) struct_name++;

                    if (is_cstruct_var && strcmp(module_alias, struct_name) == 0) {
                        for (int i = 0; cstruct_static_methods[i]; i++) {
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_static_methods[i]);
                            add_completion_item(&items, count, &capacity,
                                               cstruct_static_methods[i],
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                    } else {
                        add_current_file_struct_completions(content, &items, count, &capacity, struct_name, module_alias);

                        if (is_cstruct_var) {
                            for (int i = 0; cstruct_instance_methods[i]; i++) {
                                char detail[256];
                                snprintf(detail, sizeof(detail), "%s.%s()", module_alias, cstruct_instance_methods[i]);
                                add_completion_item(&items, count, &capacity,
                                                   cstruct_instance_methods[i],
                                                   LSP_COMP_METHOD,
                                                   detail,
                                                   NULL);
                            }
                        } else {
                            char detail[256];
                            snprintf(detail, sizeof(detail), "%s.copy()", module_alias);
                            add_completion_item(&items, count, &capacity,
                                               "copy",
                                               LSP_COMP_METHOD,
                                               detail,
                                               NULL);
                        }
                    }
                }
            } else if (is_face_var) {
                const char* face_name = strstr(var_type, "face");
                face_name += 4;
                while (*face_name && isspace((unsigned char)*face_name)) face_name++;

                int items_before = *count;
                find_face_methods_in_content(content, face_name, &items, count, &capacity, module_alias);

                if (*count == items_before) {
                    for (int i = 0; i < import_count; i++) {
                        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                        if (mp) {
                            char* module_source = read_module_file(mp, file_path);
                            if (module_source) {
                                find_face_methods_in_content(module_source, face_name, &items, count, &capacity, module_alias);
                                free(module_source);
                                if (*count > items_before) break;
                            }
                        }
                    }
                }
            } else if (strncmp(var_type, "style:", 6) == 0) {
                // Style[window] 等类型的字段补全
                const char* style_target = var_type + 6;
                add_style_field_completions(style_target, &items, count, &capacity, prefix);
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
        // 检测是否在 Style[xxx] = { } 初始化上下文中
        char* style_target = detect_style_context(content, pos);
        if (style_target) {
            add_style_field_completions(style_target, &items, count, &capacity, prefix);
            free(style_target);
            
            free(prefix);
            free(module_alias);
            free(member_name);
            free(use_module);
            free(use_prefix);
            free_import_aliases(import_aliases, import_count);
            return items;
        }
        
        int type_ctx_offset = lsp_position_to_offset(content, pos);
        if (is_type_annotation_context(content, type_ctx_offset)) {
            for (int i = 0; leno_types[i]; i++) {
                add_completion_item(&items, count, &capacity, leno_types[i],
                                   LSP_COMP_CLASS, "type", prefix);
            }

            add_completion_item(&items, count, &capacity, "var",
                               LSP_COMP_KEYWORD, "type inference", prefix);

            CompilerContext ctx;
            compiler_context_init(&ctx);
            compiler_analyze_with_filename(&ctx, content, file_path);
            if (ctx.root_scope) {
                for (int i = 0; i < ctx.root_scope->sym_cnt; i++) {
                    Symbol* sym = ctx.root_scope->syms[i];
                    if (sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT ||
                        sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM ||
                        sym->kind == SYM_CLIB) {
                        const char* type_label = "type";
                        int comp_kind = LSP_COMP_CLASS;
                        if (sym->type) {
                            switch (sym->type->kind) {
                                case TYPE_STRUCT: type_label = "struct"; comp_kind = LSP_COMP_STRUCT; break;
                                case TYPE_CSTRUCT: type_label = "cstruct"; comp_kind = LSP_COMP_STRUCT; break;
                                case TYPE_ENUM: type_label = "enum"; comp_kind = LSP_COMP_ENUM; break;
                                case TYPE_FACE: type_label = "face"; comp_kind = LSP_COMP_INTERFACE; break;
                                case TYPE_CLIB: type_label = "clib"; comp_kind = LSP_COMP_CLASS; break;
                                default: break;
                            }
                        }
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s %s", type_label, sym->name);
                        add_completion_item(&items, count, &capacity,
                                           sym->name, comp_kind, detail, prefix);
                    }
                }
            }
            compiler_context_cleanup(&ctx);

            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (mp) {
                    char* module_source = read_module_file(mp, file_path);
                    if (module_source) {
                        CompilerContext mctx;
                        compiler_context_init(&mctx);
                        bool analyzed = compiler_analyze_with_filename(&mctx, module_source, file_path);
                        if (analyzed || mctx.root_scope) {
                            for (int j = 0; j < mctx.root_scope->sym_cnt; j++) {
                                Symbol* sym = mctx.root_scope->syms[j];
                                if (sym->kind == SYM_TYPE || sym->kind == SYM_STRUCT ||
                                    sym->kind == SYM_CSTRUCT || sym->kind == SYM_ENUM) {
                                    const char* type_label = "type";
                                    int comp_kind = LSP_COMP_CLASS;
                                    if (sym->type) {
                                        switch (sym->type->kind) {
                                            case TYPE_STRUCT: type_label = "struct"; comp_kind = LSP_COMP_STRUCT; break;
                                            case TYPE_CSTRUCT: type_label = "cstruct"; comp_kind = LSP_COMP_STRUCT; break;
                                            case TYPE_ENUM: type_label = "enum"; comp_kind = LSP_COMP_ENUM; break;
                                            case TYPE_FACE: type_label = "face"; comp_kind = LSP_COMP_INTERFACE; break;
                                            default: break;
                                        }
                                    }
                                    char detail[256];
                                    snprintf(detail, sizeof(detail), "%s.%s: %s", import_aliases[i].alias, sym->name, type_label);
                                    add_completion_item(&items, count, &capacity,
                                                       sym->name, comp_kind, detail, prefix);
                                }
                            }
                        }
                        compiler_context_cleanup(&mctx);
                        free(module_source);
                    }
                }
            }

            for (int i = 0; i < import_count; i++) {
                char detail[256];
                snprintf(detail, sizeof(detail), "module %s", import_aliases[i].alias);
                add_completion_item(&items, count, &capacity,
                                   import_aliases[i].alias,
                                   LSP_COMP_MODULE,
                                   detail,
                                   prefix);
            }
        } else {
        // 正常补全：关键字、类型、内置函数、模块方法、用户符号
        
        // 1. 添加关键字
        for (int i = 0; leno_keywords[i]; i++) {
            add_completion_item(&items, count, &capacity, leno_keywords[i], 
                               LSP_COMP_KEYWORD, "keyword", prefix);
        }

        // 1.5 如果在结构体方法体内，添加 self 关键字补全
        {
            int cursor_offset = lsp_position_to_offset(content, pos);
            char* enclosing_struct = find_enclosing_struct_name(content, cursor_offset);
            if (enclosing_struct) {
                add_completion_item(&items, count, &capacity, "self",
                                   LSP_COMP_KEYWORD, enclosing_struct, prefix);
                free(enclosing_struct);
            }
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
                // 模块方法
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
                // 模块常量
                int const_count = 0;
                char** consts = native_get_module_consts(modules[m], &const_count);
                if (consts && const_count > 0) {
                    for (int i = 0; i < const_count; i++) {
                        char full_name[256];
                        snprintf(full_name, sizeof(full_name), "%s.%s", modules[m], consts[i]);
                        char detail[256];
                        snprintf(detail, sizeof(detail), "%s.%s (const)", modules[m], consts[i]);
                        add_completion_item(&items, count, &capacity, full_name,
                                           LSP_COMP_CONSTANT, detail, prefix);
                    }
                    native_free_module_const_list(consts, const_count);
                }
            }
            native_free_module_list(modules, module_count);
        }
        
        // 5. 从编译器符号表添加用户定义的符号
        add_symbols_from_compiler(content, &items, count, &capacity, prefix, file_path);
        }
    }
    
    free(prefix);
    free(module_alias);
    free(member_name);
    free(use_module);
    free(use_prefix);
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
