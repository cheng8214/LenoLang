/**
 * 上下文检测模块
 * 
 * 职责：分析光标位置，确定当前补全上下文类型
 * 
 * 支持的上下文：
 * - CTX_NORMAL：普通位置，需要关键字+类型+符号
 * - CTX_TYPE_ANNOTATION：类型注解位置
 * - CTX_NEW：new 关键字后
 * - CTX_USE_MODULE / CTX_USE_MEMBER：use 语句
 * - CTX_DOT_ACCESS：expr. 成员访问
 * - CTX_STRING_LITERAL：字符串字面量方法
 */

#include "lsp_completion.h"
#include "leno_builtins.h"
#include "../src/include/leno_types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* ========== 内部辅助 ========== */

extern char* read_module_file(const char* file_path, const char* current_file);

// 字符串字面量检测
static int is_string_literal_before_dot(const char* content, int dot_pos) {
    if (dot_pos <= 0) return -1;
    int pos = dot_pos - 1;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    if (pos < 0 || (content[pos] != '"' && content[pos] != '\'')) return -1;
    
    char quote = content[pos];
    pos--;
    while (pos >= 0) {
        if (content[pos] == quote) {
            int backslash_count = 0;
            int check_pos = pos - 1;
            while (check_pos >= 0 && content[check_pos] == '\\') {
                backslash_count++;
                check_pos--;
            }
            if (backslash_count % 2 == 0) return pos + 1;
        }
        pos--;
    }
    return -1;
}

// 获取光标前的单词/前缀
// 返回值约定：
//   普通单词：直接返回单词（如 "str"、"export"）
//   点访问：返回 "__DOT__:varname"（如 "__DOT__:str"）
//   字符串点访问：返回 "__STRING_LITERAL__"
static char* get_word_before_cursor(const char* content, LspPosition pos) {
    if (!content) return NULL;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return NULL;
    
    int start = offset - 1;
    
    // 点号场景
    if (start >= 0 && content[start] == '.') {
        if (is_string_literal_before_dot(content, start) >= 0) {
            return strdup("__STRING_LITERAL__");
        }

        // 提取点号前的变量名/标识符
        int var_end = start;       // 点号的位置
        start--;
        // 检查点号前是否是 ')'（函数调用链，如 ttfLib().）
        if (start >= 0 && content[start] == ')') {
            // 向前查找匹配的 '('
            int paren_depth = 1;
            int p = start - 1;
            while (p >= 0 && paren_depth > 0) {
                if (content[p] == ')') paren_depth++;
                else if (content[p] == '(') paren_depth--;
                else if (content[p] == '"' || content[p] == '\'') {
                    char quote = content[p];
                    p--;
                    while (p >= 0 && content[p] != quote) {
                        if (p > 0 && content[p] == '\\') p--;
                        p--;
                    }
                }
                if (paren_depth > 0) p--;
            }
            if (paren_depth == 0 && p > 0) {
                // p 指向 '('，向前提取函数名
                p--;
                while (p >= 0 && isspace((unsigned char)content[p])) p--;
                int name_end = p + 1;
                while (p >= 0 && (isalnum((unsigned char)content[p]) || content[p] == '_')) {
                    p--;
                }
                int name_start = p + 1;
                int name_len = name_end - name_start;
                if (name_len > 0) {
                    char* func_name = (char*)malloc(name_len + 1);
                    if (!func_name) return NULL;
                    memcpy(func_name, content + name_start, name_len);
                    func_name[name_len] = '\0';
                    // 返回 __FUNC_CALL__:funcName 格式，表示函数调用链
                    const char* fc_prefix = "__FUNC_CALL__:";
                    size_t plen = strlen(fc_prefix);
                    char* result = (char*)malloc(plen + name_len + 1);
                    if (!result) { free(func_name); return NULL; }
                    memcpy(result, fc_prefix, plen);
                    memcpy(result + plen, func_name, name_len);
                    result[plen + name_len] = '\0';
                    free(func_name);
                    return result;
                }
            }
            return NULL;
        }
        // 检查点号前是否是 ']'（数组索引成员访问，如 _texCache[i].）
        if (start >= 0 && content[start] == ']') {
            // 向前查找匹配的 '['
            int bracket_depth = 1;
            int p = start - 1;
            while (p >= 0 && bracket_depth > 0) {
                if (content[p] == ']') bracket_depth++;
                else if (content[p] == '[') bracket_depth--;
                else if (content[p] == '"' || content[p] == '\'') {
                    char quote = content[p];
                    p--;
                    while (p >= 0 && content[p] != quote) {
                        if (p > 0 && content[p] == '\\') p--;
                        p--;
                    }
                }
                if (bracket_depth > 0) p--;
            }
            if (bracket_depth == 0 && p > 0) {
                // p 指向 '['，向前提取变量名
                p--;
                while (p >= 0 && isspace((unsigned char)content[p])) p--;
                int name_end = p + 1;
                while (p >= 0 && (isalnum((unsigned char)content[p]) || content[p] == '_')) {
                    p--;
                }
                int name_start = p + 1;
                int name_len = name_end - name_start;
                if (name_len > 0) {
                    char* arr_var = (char*)malloc(name_len + 1);
                    if (!arr_var) return NULL;
                    memcpy(arr_var, content + name_start, name_len);
                    arr_var[name_len] = '\0';
                    // 返回 __ARRAY_INDEX__:varName 格式，表示数组索引成员访问
                    const char* ai_prefix = "__ARRAY_INDEX__:";
                    size_t plen = strlen(ai_prefix);
                    char* result = (char*)malloc(plen + name_len + 1);
                    if (!result) { free(arr_var); return NULL; }
                    memcpy(result, ai_prefix, plen);
                    memcpy(result + plen, arr_var, name_len);
                    result[plen + name_len] = '\0';
                    free(arr_var);
                    return result;
                }
            }
            return NULL;
        }
        while (start >= 0 && (isalnum((unsigned char)content[start]) || content[start] == '_')) {
            start--;
        }
        start++;

        int var_len = var_end - start;
        if (var_len <= 0) return NULL;

        // 返回标记前缀让上层知道这是点访问上下文
        // 格式: "__DOT__:varname"
        char* var_name = (char*)malloc(var_len + 1);
        if (!var_name) return NULL;
        memcpy(var_name, content + start, var_len);
        var_name[var_len] = '\0';

        // 拼接成 __DOT__:varname 格式
        const char* prefix = "__DOT__:";
        size_t prefix_len = strlen(prefix);
        char* result = (char*)malloc(prefix_len + var_len + 1);
        if (!result) { free(var_name); return NULL; }
        memcpy(result, prefix, prefix_len);
        memcpy(result + prefix_len, var_name, var_len);
        result[prefix_len + var_len] = '\0';
        free(var_name);

        // 注意：暂时不支持 "cs_module.Color." 两层前缀，
        // 这类复杂表达式需要更高级的类型推断
        return result;
    }
    
    // 普通单词
    while (start >= 0 && (isalnum((unsigned char)content[start]) || content[start] == '_')) {
        start--;
    }
    start++;
    
    int len = offset - start;
    if (len <= 0) return NULL;
    char* word = (char*)malloc(len + 1);
    if (!word) return NULL;
    memcpy(word, content + start, len);
    word[len] = '\0';
    
    // 检查是否在点号表达式的成员部分（如 "bar.add" 中的 "add"）
    // 如果 word 前面有 "." 和变量名，则返回 __DOT__:varname
    {
        int check_pos = start - 1;  // word 开始位置的前一个字符
        if (check_pos >= 0 && content[check_pos] == '.') {
            // 找到点号，继续向前提取变量名
            int var_end = check_pos;
            int vstart = check_pos - 1;
            // 检查点号前是否是 ')'（函数调用链，如 ttfLib().method）
            if (vstart >= 0 && content[vstart] == ')') {
                // 向前查找匹配的 '('
                int paren_depth = 1;
                int p = vstart - 1;
                while (p >= 0 && paren_depth > 0) {
                    if (content[p] == ')') paren_depth++;
                    else if (content[p] == '(') paren_depth--;
                    else if (content[p] == '"' || content[p] == '\'') {
                        char quote = content[p];
                        p--;
                        while (p >= 0 && content[p] != quote) {
                            if (p > 0 && content[p] == '\\') p--;
                            p--;
                        }
                    }
                    if (paren_depth > 0) p--;
                }
                if (paren_depth == 0 && p > 0) {
                    p--;
                    while (p >= 0 && isspace((unsigned char)content[p])) p--;
                    int name_end = p + 1;
                    while (p >= 0 && (isalnum((unsigned char)content[p]) || content[p] == '_')) {
                        p--;
                    }
                    int name_start = p + 1;
                    int name_len = name_end - name_start;
                    if (name_len > 0) {
                        char* func_name = (char*)malloc(name_len + 1);
                        if (func_name) {
                            memcpy(func_name, content + name_start, name_len);
                            func_name[name_len] = '\0';
                            const char* fc_prefix = "__FUNC_CALL__:";
                            size_t plen = strlen(fc_prefix);
                            char* result = (char*)malloc(plen + name_len + 1);
                            if (result) {
                                memcpy(result, fc_prefix, plen);
                                memcpy(result + plen, func_name, name_len);
                                result[plen + name_len] = '\0';
                                free(func_name);
                                free(word);
                                return result;
                            }
                            free(func_name);
                        }
                    }
                }
            }
            // 检查点号前是否是 ']'（数组索引成员访问，如 _texCache[i].method）
            if (vstart >= 0 && content[vstart] == ']') {
                // 向前查找匹配的 '['
                int bracket_depth = 1;
                int p = vstart - 1;
                while (p >= 0 && bracket_depth > 0) {
                    if (content[p] == ']') bracket_depth++;
                    else if (content[p] == '[') bracket_depth--;
                    else if (content[p] == '"' || content[p] == '\'') {
                        char quote = content[p];
                        p--;
                        while (p >= 0 && content[p] != quote) {
                            if (p > 0 && content[p] == '\\') p--;
                            p--;
                        }
                    }
                    if (bracket_depth > 0) p--;
                }
                if (bracket_depth == 0 && p > 0) {
                    // p 指向 '['，向前提取变量名
                    p--;
                    while (p >= 0 && isspace((unsigned char)content[p])) p--;
                    int name_end = p + 1;
                    while (p >= 0 && (isalnum((unsigned char)content[p]) || content[p] == '_')) {
                        p--;
                    }
                    int name_start = p + 1;
                    int name_len = name_end - name_start;
                    if (name_len > 0) {
                        char* arr_var = (char*)malloc(name_len + 1);
                        if (arr_var) {
                            memcpy(arr_var, content + name_start, name_len);
                            arr_var[name_len] = '\0';
                            const char* ai_prefix = "__ARRAY_INDEX__:";
                            size_t plen = strlen(ai_prefix);
                            char* result = (char*)malloc(plen + name_len + 1);
                            if (result) {
                                memcpy(result, ai_prefix, plen);
                                memcpy(result + plen, arr_var, name_len);
                                result[plen + name_len] = '\0';
                                free(arr_var);
                                free(word);
                                return result;
                            }
                            free(arr_var);
                        }
                    }
                }
            }
            while (vstart >= 0 && (isalnum((unsigned char)content[vstart]) || content[vstart] == '_')) {
                vstart--;
            }
            vstart++;
            
            int var_len = var_end - vstart;
            if (var_len > 0) {
                // 提取变量名
                char* var_name = (char*)malloc(var_len + 1);
                if (var_name) {
                    memcpy(var_name, content + vstart, var_len);
                    var_name[var_len] = '\0';
                    
                    // 返回 __DOT__:varname 格式
                    const char* dot_prefix = "__DOT__:";
                    size_t plen = strlen(dot_prefix);
                    char* result = (char*)malloc(plen + var_len + 1);
                    if (result) {
                        memcpy(result, dot_prefix, plen);
                        memcpy(result + plen, var_name, var_len);
                        result[plen + var_len] = '\0';
                        free(var_name);
                        free(word);
                        return result;
                    }
                    free(var_name);
                }
            }
        }
    }
    
    return word;
}

// 检测类型注解上下文
static bool is_type_annotation_context(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return false;
    
    int pos = cursor_offset - 1;
    
    // 跳过光标前的标识符字符
    while (pos >= 0 && (isalnum((unsigned char)content[pos]) || content[pos] == '_')) pos--;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    
    if (pos < 0) return false;
    
    // 冒号后面
    if (content[pos] == ':') {
        int check = pos - 1;
        while (check >= 0 && isspace((unsigned char)content[check])) check--;
        
        if (check >= 0 && content[check] == ')') return true;
        
        if (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) {
            // 检查 var/func 前缀
            while (check >= 0 && (isalnum((unsigned char)content[check]) || content[check] == '_')) check--;
            while (check >= 0 && isspace((unsigned char)content[check])) check--;
            
            // 检查 func(...) : 参数类型
            if (check >= 0) {
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
                                if (p2 >= 3 && content[p2 - 3] == 'f' && content[p2 - 2] == 'u' && 
                                    content[p2 - 1] == 'n' && content[p2] == 'c') {
                                    return true;
                                }
                            }
                            break;
                        }
                    }
                    p2--;
                }
            }
        }
        return false;
    }
    
    // "as" 后面（类型转换）
    if (pos >= 1 && content[pos - 1] == 'a' && content[pos] == 's') {
        if (pos == 1 || (!isalnum((unsigned char)content[pos - 2]) && content[pos - 2] != '_')) {
            return true;
        }
    }
    
    // "is" 后面（类型守卫）
    if (pos >= 2 && content[pos - 2] == 'i' && content[pos - 1] == 's') {
        if (pos == 2 || (!isalnum((unsigned char)content[pos - 3]) && content[pos - 3] != '_')) {
            return true;
        }
    }
    
    // 行首关键字后的类型位置
    // 注意：必须检查光标所在行，而不是 pos 所在行
    // 因为 pos 可能已经回退到了上一行（如 func 定义行），导致误判
    {
        int cursor_line_start = cursor_offset - 1;
        while (cursor_line_start > 0 && content[cursor_line_start - 1] != '\n') cursor_line_start--;
        const char* line = content + cursor_line_start;
        int line_len = cursor_offset - cursor_line_start;
        
        char* line_copy = (char*)malloc(line_len + 1);
        if (!line_copy) return false;
        memcpy(line_copy, line, line_len);
        line_copy[line_len] = '\0';
        
        char* trimmed = line_copy;
        while (*trimmed && isspace((unsigned char)*trimmed)) trimmed++;
        
        static const char* type_prefixes[] = {
            "func ", "export func ", "struct ", "export struct ",
            "cstruct ", "export cstruct ", "enum ", "export enum ",
            "face ", "export face ", "alias ", "export alias ",
            "clib ", "export clib ", "cfunc ", "export cfunc ",
            NULL
        };
        
        bool result = false;
        // 只有当行首确实是类型定义关键字开头时，才认为是类型注解上下文
        // 不再把空行当作类型注解上下文（那会导致普通位置的补全失效）
        for (int i = 0; type_prefixes[i]; i++) {
            if (strncmp(trimmed, type_prefixes[i], strlen(type_prefixes[i])) == 0) {
                result = true;
                break;
            }
        }
        
        free(line_copy);
        if (result) return true;
    }
    
    return false;
}

// 检测 new 关键字后
static bool detect_new_prefix(const char* content, LspPosition pos) {
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 4) return false;
    
    int start = offset - 1;
    while (start >= 0 && isspace((unsigned char)content[start])) start--;
    
    if (start >= 3 &&
        (content[start-3] == 'n' || content[start-3] == 'N') &&
        (content[start-2] == 'e' || content[start-2] == 'E') &&
        (content[start-1] == 'w' || content[start-1] == 'W') &&
        (start == 3 || !isalnum((unsigned char)content[start-4]))) {
        return true;
    }
    return false;
}

// 检测 use 上下文
static int detect_use_context(const char* content, LspPosition pos, 
                               char** use_module_out, char** use_prefix_out) {
    *use_module_out = NULL;
    *use_prefix_out = NULL;
    if (!content) return 0;
    
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

// 检测模块前缀（如 "maths." 或 "color_module.Color."）
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

/* ========== 查找 enclosing struct ========== */

char* find_enclosing_struct_name(const char* content, int cursor_offset) {
    if (!content || cursor_offset <= 0) return NULL;
    
    int brace_depth = 0;
    int pos = cursor_offset - 1;
    
    while (pos >= 0) {
        char c = content[pos];
        if (c == '}') { brace_depth++; pos--; continue; }
        
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
                if (name_len == 0) { brace_depth = 0; pos--; continue; }
                
                int kw_end = name_start - 1;
                while (kw_end >= 0 && isspace((unsigned char)content[kw_end])) kw_end--;
                
                bool is_struct = false, is_cstruct = false;
                if (kw_end >= 6) {
                    int kw_start = kw_end - 6;
                    if (strncmp(content + kw_start, "cstruct", 7) == 0) is_cstruct = true;
                }
                if (!is_cstruct && kw_end >= 5) {
                    int kw_start = kw_end - 5;
                    if (strncmp(content + kw_start, "struct", 6) == 0) is_struct = true;
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

/* ========== 主入口 ========== */

CompletionContextInfo comp_detect_context(
    const char* content,
    LspPosition pos,
    const char* file_path,
    int import_count,
    ImportAlias* import_aliases
) {
    (void)file_path;
    (void)import_count;
    (void)import_aliases;
    
    CompletionContextInfo ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.type = CTX_NORMAL;
    
    if (!content) return ctx;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset <= 0) return ctx;
    
    // 获取光标前单词
    char* prefix = get_word_before_cursor(content, pos);
    
    fprintf(stderr, "[CTX-DEBUG] pos line=%d char=%d offset=%d prefix='%s'\n",
            pos.line, pos.character, offset, prefix ? prefix : "NULL");
    fflush(stderr);
    
    // 1. 字符串字面量
    if (prefix && strcmp(prefix, "__STRING_LITERAL__") == 0) {
        ctx.type = CTX_STRING_LITERAL;
        free(prefix);
        return ctx;
    }
    
    // 1.5 点访问上下文（如 str.SDL3.）
    //    get_word_before_cursor 返回 "__DOT__:varname" 格式的标记
    if (prefix && strncmp(prefix, "__DOT__:", 8) == 0) {
        ctx.type = CTX_DOT_ACCESS;
        ctx.module_alias = strdup(prefix + 8);  // 提取变量名/模块别名
        ctx.member_prefix = NULL;  // 无成员前缀
        free(prefix);
        return ctx;
    }

    // 1.6 函数调用链上下文（如 ttfLib().method）
    //     get_word_before_cursor 返回 "__FUNC_CALL__:funcName" 格式的标记
    if (prefix && strncmp(prefix, "__FUNC_CALL__:", 14) == 0) {
        ctx.type = CTX_DOT_ACCESS;
        ctx.module_alias = strdup(prefix + 14);  // 提取函数名
        ctx.member_prefix = NULL;
        // 标记为函数调用链，用于后续补全提供者
        ctx.is_func_call_chain = 1;
        free(prefix);
        return ctx;
    }

    // 1.7 数组索引成员访问上下文（如 _texCache[i].method）
    //     get_word_before_cursor 返回 "__ARRAY_INDEX__:varName" 格式的标记
    if (prefix && strncmp(prefix, "__ARRAY_INDEX__:", 16) == 0) {
        ctx.type = CTX_DOT_ACCESS;
        ctx.module_alias = strdup(prefix + 16);  // 提取数组变量名
        ctx.member_prefix = NULL;
        ctx.is_array_index = 1;  // 标记为数组索引访问
        free(prefix);
        return ctx;
    }
    
    // 2. use 上下文
    char* use_module = NULL;
    char* use_prefix = NULL;
    int use_ctx = detect_use_context(content, pos, &use_module, &use_prefix);
    if (use_ctx == 1) {
        ctx.type = CTX_USE_MODULE;
        ctx.prefix = use_prefix;
        free(use_module);
        free(prefix);
        return ctx;
    }
    if (use_ctx == 2) {
        ctx.type = CTX_USE_MEMBER;
        ctx.module_alias = use_module;
        ctx.member_prefix = use_prefix;
        free(prefix);
        return ctx;
    }
    
    // 3. new 关键字后
    if (detect_new_prefix(content, pos)) {
        ctx.type = CTX_NEW;
        ctx.prefix = prefix;
        return ctx;
    }
    
    // 4. 类型注解上下文
    if (is_type_annotation_context(content, offset)) {
        ctx.type = CTX_TYPE_ANNOTATION;
        ctx.prefix = prefix;
        return ctx;
    }
    
    // 5. 模块前缀（如 "maths."）
    char* module_alias = NULL;
    char* member_name = NULL;
    if (prefix && parse_module_prefix(prefix, &module_alias, &member_name)) {
        // 检查是否是变量点访问 vs 模块访问
        const char* mod_path = find_module_path_by_alias(import_aliases, import_count, module_alias);
        bool is_leno_module = (mod_path != NULL);
        
        ctx.type = CTX_DOT_ACCESS;
        ctx.module_alias = module_alias;
        ctx.member_prefix = member_name;
        (void)is_leno_module;  // 在后续提供者中使用
        free(prefix);
        return ctx;
    }
    
    // 6. 普通上下文
    ctx.type = CTX_NORMAL;
    ctx.prefix = prefix;
    return ctx;
}

void comp_context_free(CompletionContextInfo* ctx) {
    if (!ctx) return;
    free(ctx->module_alias);
    free(ctx->member_prefix);
    free(ctx->prefix);
    free(ctx->var_type);
    memset(ctx, 0, sizeof(*ctx));
}
