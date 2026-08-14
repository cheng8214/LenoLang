/**
 * 悬停提示服务
 * 提供鼠标悬停时的类型信息 - 使用 LenoC 编译器
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"
#include "leno_builtins.h"
#include "lsp_completion.h"
#include "../src/include/native.h"
#include "../src/include/leno_value.h"
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

// 检查指定位置是否在字符串字面量中
static bool is_inside_string_literal(const char* content, int offset) {
    if (!content || offset < 0) return false;
    
    int len = strlen(content);
    if (offset >= len) return false;
    
    // 向前遍历，检查是否在字符串中
    bool in_string = false;
    char quote_char = 0;
    
    for (int i = 0; i < offset && i < len; i++) {
        char c = content[i];
        
        if (in_string) {
            // 检查字符串结束
            if (c == quote_char) {
                // 检查是否是转义的
                int backslash_count = 0;
                int check_pos = i - 1;
                while (check_pos >= 0 && content[check_pos] == '\\') {
                    backslash_count++;
                    check_pos--;
                }
                // 如果反斜杠数量是偶数，说明这不是转义的引号
                if (backslash_count % 2 == 0) {
                    in_string = false;
                    quote_char = 0;
                }
            }
        } else {
            // 检查字符串开始
            if (c == '"' || c == '\'') {
                quote_char = c;
                in_string = true;
            }
        }
    }
    
    return in_string;
}

// 检查指定位置是否在注释中
static bool is_inside_comment(const char* content, int offset) {
    if (!content || offset < 0) return false;
    
    int len = strlen(content);
    if (offset >= len) return false;
    
    // 向前遍历，检查是否在注释中
    bool in_line_comment = false;   // // 注释
    bool in_block_comment = false;  // /* */ 注释
    
    for (int i = 0; i < offset && i < len; i++) {
        char c = content[i];
        char next_c = (i + 1 < len) ? content[i + 1] : 0;
        
        if (in_line_comment) {
            // 行注释在换行时结束
            if (c == '\n') {
                in_line_comment = false;
            }
        } else if (in_block_comment) {
            // 块注释在 */ 时结束
            if (c == '*' && next_c == '/') {
                in_block_comment = false;
                i++;  // 跳过 '/'
            }
        } else {
            // 检查注释开始
            if (c == '/' && next_c == '/') {
                in_line_comment = true;
                i++;  // 跳过第二个 '/'
            } else if (c == '/' && next_c == '*') {
                in_block_comment = true;
                i++;  // 跳过 '*'
            }
        }
    }
    
    return in_line_comment || in_block_comment;
}

// 获取光标下的单词（支持点号表达式如 ffi.load）
char* get_word_at_position(const char* content, LspPosition pos) {
    if (!content) return NULL;
    
    int offset = lsp_position_to_offset(content, pos);
    if (offset < 0) return NULL;
    
    int len = strlen(content);
    if (offset >= len) offset = len - 1;
    if (offset < 0) return NULL;
    
    // 如果光标位置不是字母数字、下划线或点号，尝试向前或向后查找
    if (!isalnum((unsigned char)content[offset]) && content[offset] != '_' && content[offset] != '.') {
        // 先尝试向后查找
        int search = offset;
        while (search < len && !isalnum((unsigned char)content[search]) && 
               content[search] != '_' && content[search] != '.') {
            search++;
        }
        if (search < len && (isalnum((unsigned char)content[search]) || 
                             content[search] == '_' || content[search] == '.')) {
            offset = search;
        } else {
            // 向后没找到，尝试向前查找
            search = offset;
            while (search >= 0 && !isalnum((unsigned char)content[search]) && 
                   content[search] != '_' && content[search] != '.') {
                search--;
            }
            if (search >= 0 && (isalnum((unsigned char)content[search]) || 
                                content[search] == '_' || content[search] == '.')) {
                offset = search;
            } else {
                return NULL;  // 没找到单词
            }
        }
    }
    
    // 向前查找单词开始（支持点号表达式）
    int start = offset;
    while (start > 0) {
        char c = content[start - 1];
        if (isalnum((unsigned char)c) || c == '_' || c == '.') {
            start--;
        } else {
            break;
        }
    }
    
    // 向后查找单词结束
    int end = offset;
    while (end < len) {
        char c = content[end];
        if (isalnum((unsigned char)c) || c == '_' || c == '.') {
            end++;
        } else {
            break;
        }
    }
    
    int word_len = end - start;
    if (word_len <= 0) return NULL;
    
    char* word = (char*)malloc(word_len + 1);
    if (!word) return NULL;
    
    memcpy(word, content + start, word_len);
    word[word_len] = '\0';
    
    return word;
}

// 处理悬停请求
char* lsp_handle_hover(LspServer* server, int id, JsonValue* params) {
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

    // 获取文件路径（从 URI 转换并解码）- 与补全功能使用相同的方式
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

    // 获取悬停信息
    char* hover_info = lsp_get_hover_info(doc->content, pos, file_path);
    
    // 构建响应
    JsonValue* result = NULL;
    if (hover_info) {
        result = json_object_new();
        
        JsonValue* contents = json_object_new();
        json_object_set(contents, "kind", json_string_new("markdown"));
        json_object_set(contents, "value", json_string_new(hover_info));
        
        json_object_set(result, "contents", contents);
        
        free(hover_info);
    }
    
    char* response = lsp_create_response(id, result);
    
    if (result) {
        json_free(result);
    }
    
    return response;
}

// 检查指定位置是否在指定偏移范围的代码块内（考虑嵌套）
// start_offset: 代码块开始位置（'{' 的位置）
// cursor_offset: 光标位置
// 返回 true 如果光标在代码块内
static bool is_offset_in_block(const char* content, int start_offset, int cursor_offset) {
    if (cursor_offset <= start_offset) return false;

    int brace_count = 1;
    const char* p = content + start_offset + 1;

    while (*p && brace_count > 0) {
        int current_offset = p - content;
        if (current_offset >= cursor_offset) {
            // 光标位置在匹配的 '}' 之前，说明光标在代码块内
            return true;
        }

        if (*p == '{') {
            brace_count++;
        } else if (*p == '}') {
            brace_count--;
        } else if (*p == '"' || *p == '\'') {
            // 跳过字符串
            char quote = *p;
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1)) p++;
                p++;
            }
        } else if (*p == '/' && *(p+1) == '/') {
            // 跳过行注释
            while (*p && *p != '\n') p++;
        } else if (*p == '/' && *(p+1) == '*') {
            // 跳过块注释
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p++;
        }

        if (*p) p++;
    }

    // 如果 brace_count > 0，说明没有找到匹配的 '}'，光标在代码块内
    // 如果 brace_count == 0，说明找到了匹配的 '}'，需要检查光标位置
    return brace_count > 0;
}

// 从光标位置向前查找所在的 struct 名称
// 通过向上查找 "struct XXX {" 或 "export struct XXX {" 的模式来确定
static char* find_struct_name_at_position(const char* content, LspPosition pos) {
    if (!content) return NULL;

    int offset = lsp_position_to_offset(content, pos);
    if (offset < 0) return NULL;

    // 从当前位置向前搜索 "struct" 关键字
    // 我们需要找到最近的一个 "struct Name {" 定义，且该定义在光标之前
    const char* search_start = content + offset;

    // 向前搜索 "struct" 关键字
    for (const char* p = search_start; p >= content; p--) {
        // 检查是否是 "struct" 关键字的开始
        if (p + 6 <= search_start &&
            strncmp(p, "struct", 6) == 0 &&
            (p == content || !isalnum((unsigned char)*(p-1))) &&
            !isalnum((unsigned char)*(p+6))) {

            // 找到 struct 后，解析 struct 名称
            // 格式应该是: struct Name { 或 export struct Name {
            const char* name_start = p + 6;

            // 跳过空白字符
            while (*name_start && isspace((unsigned char)*name_start)) {
                name_start++;
            }

            // 现在 name_start 应该指向 struct 名称
            const char* name_end = name_start;
            while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_')) {
                name_end++;
            }

            int name_len = name_end - name_start;

            if (name_len > 0) {
                // 检查这个 struct 定义是否包含光标位置（通过查找匹配的 }）
                // 简单检查：从 name_end 开始查找 {
                const char* brace = name_end;
                while (*brace && *brace != '{' && *brace != '\n') {
                    brace++;
                }

                if (*brace == '{') {
                    // 找到了 struct 定义的开始，检查光标是否在这个定义内
                    int brace_offset = brace - content;

                    // 使用改进的方法检查光标是否在代码块内
                    if (is_offset_in_block(content, brace_offset, offset)) {
                        // 光标在 { 和匹配的 } 之间，确实在这个 struct 定义内
                        char* struct_name = (char*)malloc(name_len + 1);
                        if (struct_name) {
                            strncpy(struct_name, name_start, name_len);
                            struct_name[name_len] = '\0';
                            return struct_name;
                        }
                    }
                }
            }
        }
    }

    return NULL;
}

// 在 LSP 中根据光标位置查找符号
// 策略：使用编译器的 scope_resolve 从当前作用域向上查找
// 编译器已经正确处理了作用域链，包括 enum 类型推断为 int
static Symbol* find_symbol_in_current_function(Scope* root_scope, const char* name, const char* content, LspPosition pos) {
    if (!root_scope || !name) return NULL;

    // 获取光标偏移
    int cursor_offset = lsp_position_to_offset(content, pos);
    if (cursor_offset < 0) return NULL;



    // 找到光标所在的函数作用域
    Scope* cursor_scope = NULL;

    // 首先收集所有函数定义的位置
    typedef struct {
        char* name;
        int start;
        int end;
    } FuncInfo;

    FuncInfo* funcs = NULL;
    int func_count = 0;
    int func_capacity = 16;
    funcs = (FuncInfo*)malloc(sizeof(FuncInfo) * func_capacity);
    if (!funcs) return NULL;

    // 在代码中查找所有函数定义
    // 支持两种格式: "func name() {" 或 "main() {"（main函数特殊，不需要func关键字）
    const char* search_pos = content;
    while (*search_pos) {
        // 查找 "func " 或 "main() {" 模式
        const char* func_kw = strstr(search_pos, "func ");
        const char* main_kw = strstr(search_pos, "main() {");

        // 确定函数定义的起始位置
        const char* def_start = NULL;
        bool has_func_keyword = false;

        if (func_kw && (!main_kw || func_kw < main_kw)) {
            // 找到 "func " 关键字
            if (func_kw == content || !isalnum((unsigned char)*(func_kw - 1))) {
                def_start = func_kw;
                has_func_keyword = true;
            }
        } else if (main_kw) {
            // 找到 "main() {"，检查前面是否是行首或空白字符
            const char* check = main_kw - 1;
            while (check > content && isspace((unsigned char)*check)) check--;
            // 接受行首、换行符、分号、右花括号，或者任何非 ASCII 字符（如 UTF-8 中文字符）
            if (check <= content || *check == '\n' || *check == ';' || *check == '}' ||
                (unsigned char)*check >= 0x80) {
                def_start = main_kw;
                has_func_keyword = false;
            }
        }

        if (!def_start) {
            if (func_kw) search_pos = func_kw + 1;
            else if (main_kw) search_pos = main_kw + 1;
            else break;
            continue;
        }

        // 解析函数名
        const char* name_start = has_func_keyword ? def_start + 5 : def_start;
        if (has_func_keyword) {
            while (*name_start && isspace((unsigned char)*name_start)) name_start++;
        }

        const char* name_end = name_start;
        while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_')) {
            name_end++;
        }

        int name_len = name_end - name_start;
        if (name_len > 0) {
            // 查找函数体的起始 '{'
            const char* brace = name_end;
            while (*brace && *brace != '{') {
                // 跳过字符串
                if (*brace == '"' || *brace == '\'') {
                    char quote = *brace;
                    brace++;
                    while (*brace && *brace != quote) {
                        if (*brace == '\\' && *(brace+1)) brace++;
                        brace++;
                    }
                }
                if (*brace == '{') break;
                brace++;
            }

            if (*brace == '{') {
                // 计算函数范围
                int func_start = def_start - content;
                int func_end = -1;

                int brace_count = 1;
                const char* p = brace + 1;
                while (*p && brace_count > 0) {
                    // 跳过字符串字面量
                    if (*p == '"' || *p == '\'') {
                        char quote = *p;
                        p++;
                        while (*p && *p != quote) {
                            if (*p == '\\' && *(p+1)) p++;
                            p++;
                        }
                        if (*p) p++;
                        continue;
                    }
                    // 跳过行注释
                    if (*p == '/' && *(p+1) == '/') {
                        while (*p && *p != '\n') p++;
                        continue;
                    }
                    // 跳过块注释
                    if (*p == '/' && *(p+1) == '*') {
                        p += 2;
                        while (*p && !(*p == '*' && *(p+1) == '/')) p++;
                        if (*p) p += 2;
                        continue;
                    }
                    if (*p == '{') brace_count++;
                    else if (*p == '}') brace_count--;
                    p++;
                }
                func_end = p - content;

                // 保存函数信息
                if (func_count >= func_capacity) {
                    func_capacity *= 2;
                    FuncInfo* new_funcs = (FuncInfo*)realloc(funcs, sizeof(FuncInfo) * func_capacity);
                    if (!new_funcs) {
                        free(funcs);
                        return NULL;
                    }
                    funcs = new_funcs;
                }
                funcs[func_count].name = (char*)malloc(name_len + 1);
                strncpy(funcs[func_count].name, name_start, name_len);
                funcs[func_count].name[name_len] = '\0';
                funcs[func_count].start = func_start;
                funcs[func_count].end = func_end;
                func_count++;
            }
        }

        search_pos = name_end + 1;
    }

    // 找到光标所在的函数索引
    int cursor_func_idx = -1;
    for (int i = 0; i < func_count; i++) {
        if (cursor_offset >= funcs[i].start && cursor_offset <= funcs[i].end) {
            cursor_func_idx = i;
            break;
        }
    }

    // 如果找到了光标所在的函数，找到对应的作用域
    if (cursor_func_idx >= 0) {
        // 遍历所有 depth=1 的函数作用域，找到第 cursor_func_idx 个
        int func_scope_idx = 0;
        for (int i = 0; i < root_scope->child_count; i++) {
            Scope* child = root_scope->children[i];
            if (child->depth != 1 || !child->is_func) continue;

            if (func_scope_idx == cursor_func_idx) {
                cursor_scope = child;
                break;
            }
            func_scope_idx++;
        }
    }

    // 释放函数信息
    for (int i = 0; i < func_count; i++) {
        free(funcs[i].name);
    }
    free(funcs);

    // 如果找到了光标所在的函数作用域，使用编译器的 scope_resolve 查找符号
    // scope_resolve 会从当前作用域向上查找，正确处理作用域链
    if (cursor_scope) {
        Symbol* sym = scope_resolve(cursor_scope, name);
        if (sym) {
            return sym;
        }
    }

    // 如果不在任何函数中，或者函数中没找到，在全局作用域中查找
    Symbol* sym = scope_resolve(root_scope, name);
    if (sym) {
        return sym;
    }

    return NULL;
}

// 前向声明（此函数也被 comp_symbols.c 使用）
char* detect_closure_param_type_at_position(const char* content, int cursor_offset, const char* var_name);

// 从编译器获取符号悬停信息
static char* get_symbol_hover_from_compiler(const char* content, const char* word, LspPosition pos, const char* file_path) {
    if (!content || !word) return NULL;

    // 首先尝试基于光标位置的闭包参数类型检测
    // 闭包参数（如 func(Renderer r) 中的 r）在编译器作用域树中可能无法正确查找
    {
        int cursor_offset = lsp_position_to_offset(content, pos);
        // 对于带点号的表达式（如 r.clearColor），提取点号前的变量名
        const char* dot = strchr(word, '.');
        if (dot) {
            // 提取点号前的变量名（如 "r.clearColor" 中的 "r"）
            int base_len = dot - word;
            char base_var[128];
            if (base_len > 0 && base_len < 127) {
                memcpy(base_var, word, base_len);
                base_var[base_len] = '\0';
            } else {
                base_var[0] = '\0';
            }
            // 只有当光标在变量部分（点号前）时才检测
            // 光标在方法部分时由方法悬停逻辑处理
            if (cursor_offset >= 0 && base_var[0]) {
                // 向前找点号
                int scan = cursor_offset;
                bool found_dot = false;
                while (scan >= 0) {
                    char c = content[scan];
                    if (c == '.') { found_dot = true; break; }
                    if (isalnum((unsigned char)c) || c == '_') { scan--; continue; }
                    break;
                }
                if (!found_dot) {
                    // 光标不在点号后，检查是否是闭包参数
                    // 使用点号前的变量名（如 "r"）而非完整表达式（如 "r.clearColor"）
                    char* closure_type = detect_closure_param_type_at_position(content, cursor_offset, base_var);
                    if (closure_type) {
                        size_t info_len = 512 + strlen(base_var) + strlen(closure_type);
                        char* info = (char*)malloc(info_len);
                        if (info) {
                            snprintf(info, info_len, "**%s**\n\n"
                                     "```leno\n"
                                     "%s: %s\n"
                                     "```\n\n"
                                     "闭包参数",
                                     base_var, base_var, closure_type);
                        }
                        free(closure_type);
                        return info;
                    }
                }
            }
        } else {
            // 无点号的普通变量
            char* closure_type = detect_closure_param_type_at_position(content, cursor_offset, word);
            if (closure_type) {
                size_t info_len = 512 + strlen(word) + strlen(closure_type);
                char* info = (char*)malloc(info_len);
                if (info) {
                    snprintf(info, info_len, "**%s**\n\n"
                             "```leno\n"
                             "%s: %s\n"
                             "```\n\n"
                             "闭包参数",
                             word, word, closure_type);
                }
                free(closure_type);
                return info;
            }
        }
    }

    // 如果 word 包含点号（如 "tokens.add"），提取第一个段作为变量名查找
    // 这样悬停在 tokens.add 的 tokens 部分时，能显示变量类型信息
    char* base_word = NULL;
    const char* dot_pos = strchr(word, '.');
    if (dot_pos) {
        int base_len = dot_pos - word;
        if (base_len > 0) {
            base_word = (char*)malloc(base_len + 1);
            memcpy(base_word, word, base_len);
            base_word[base_len] = '\0';
        }
        
        // 判断光标是在点号前（变量部分）还是点号后（方法部分）
        // 如果光标在方法部分，应该跳过变量查找，让方法悬停逻辑处理
        int cursor_offset = lsp_position_to_offset(content, pos);
        if (cursor_offset >= 0 && base_word) {
            // 从光标位置向前扫描，找到最近的点号
            // 向前查找点号（在同一表达式内）
            bool found_dot_before_cursor = false;
            int scan = cursor_offset;
            while (scan >= 0) {
                char c = content[scan];
                if (c == '.') {
                    found_dot_before_cursor = true;
                    break;
                }
                if (isalnum((unsigned char)c) || c == '_') {
                    scan--;
                    continue;
                }
                // 遇到非单词非点号字符，停止
                break;
            }
            
            if (found_dot_before_cursor) {
                // 光标在点号之后（方法部分），跳过变量查找
                free(base_word);
                base_word = NULL;
            }
        }
    }

    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 编译分析（即使失败也可能有符号表）
    compiler_analyze_with_filename(&ctx, content, file_path);

    // 检查是否有符号表
    if (!ctx.root_scope) {
        free(base_word);
        compiler_context_cleanup(&ctx);
        return NULL;
    }

    // 如果有 base_word（带点号的表达式），优先查找变量本身
    if (base_word) {
        // 先在当前函数作用域中查找
        Symbol* var_sym = find_symbol_in_current_function(ctx.root_scope, base_word, content, pos);
        if (!var_sym) {
            var_sym = scope_resolve_tree_bfs(ctx.root_scope, base_word);
        }
        if (var_sym) {
            // 找到了变量，构建悬停信息
            const char* type_str = type_to_string(var_sym->type);
            size_t info_len = 512 + strlen(base_word) + (type_str ? strlen(type_str) : 0);
            char* info = (char*)malloc(info_len);
            if (info) {
                bool is_global = (var_sym->scope == ctx.root_scope);
                const char* kind_str = "局部";
                if (is_global) kind_str = "全局";
                else if (var_sym->kind == SYM_PARAM) kind_str = "参数";
                else if (var_sym->kind == SYM_MODULE) kind_str = "模块";

                snprintf(info, info_len, "**%s**\n\n"
                         "```leno\n"
                         "%s: %s\n"
                         "```\n\n"
                         "%s%s",
                         base_word,
                         base_word,
                         type_str ? type_str : "unknown",
                         kind_str,
                         " 变量");
            }
            free(base_word);
            compiler_context_cleanup(&ctx);
            return info;
        }
    }

    // 首先尝试根据光标位置确定所在的 struct（优先处理 struct 字段）
    char* current_struct_name = find_struct_name_at_position(content, pos);

    if (current_struct_name) {
        char* type_str = NULL;
        bool found = compiler_get_struct_field_info(&ctx, current_struct_name, word, &type_str);
        if (found) {
            char* info = (char*)malloc(4096);
            if (info) {
                snprintf(info, 4096, "**%s**\n\n"
                         "```leno\n"
                         "%.100s: %.500s\n"
                         "```\n\n"
                         "%.100s 字段 (%.100s)",
                         word,
                         word,
                         type_str ? type_str : "unknown",
                         current_struct_name,
                         current_struct_name);
            }
            free(type_str);
            free(current_struct_name);
            compiler_context_cleanup(&ctx);
            return info;
        }
    }

    // 如果不在 struct 定义中，尝试在作用域树中查找符号
    // 优先查找当前函数作用域中的符号，避免其他函数的参数干扰
    // 使用 base_word（不带点号的变量名）查找，如果没有 base_word 则用原始 word
    const char* lookup_word = base_word ? base_word : word;
    Symbol* sym = find_symbol_in_current_function(ctx.root_scope, lookup_word, content, pos);

    if (!sym) {
        sym = scope_resolve_tree_bfs(ctx.root_scope, lookup_word);
    }
    free(base_word);
    base_word = NULL;

    // 如果没找到，尝试查找所有包含该字段的 struct
    if (!sym) {
        char** struct_names = NULL;
        int struct_count = compiler_find_structs_with_field(&ctx, word, &struct_names);

        if (struct_count > 0) {
            char* type_str = NULL;
            bool found = compiler_get_struct_field_info(&ctx, struct_names[0], word, &type_str);

            // 释放 struct 名称列表
            for (int i = 0; i < struct_count; i++) {
                free(struct_names[i]);
            }
            free(struct_names);

            if (found && type_str) {
                size_t info_len = 512 + strlen(word) + strlen(type_str) + strlen(struct_names[0]);
                char* info = (char*)malloc(info_len);
                if (info) {
                    snprintf(info, info_len, "**%s**\n\n"
                             "```leno\n"
                             "%s: %s\n"
                             "```\n\n"
                             "%s 字段 (%s)",
                             word,
                             word,
                             type_str,
                             struct_names[0],
                             struct_names[0]);
                }
                free(type_str);
                free(current_struct_name);
                compiler_context_cleanup(&ctx);
                return info;
            }
        }

        free(current_struct_name);
        free(base_word);
        compiler_context_cleanup(&ctx);
        return NULL;
    }

    // 构建悬停信息
    const char* type_str = type_to_string(sym->type);
    size_t info_len = 512 + strlen(word) + (type_str ? strlen(type_str) : 0);
    char* info = (char*)malloc(info_len);
    if (!info) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }

    // 检查符号类型
    bool is_global = (sym->scope == ctx.root_scope);

    if (sym->type && sym->type->kind == TYPE_ENUM) {
        // enum 值
        if (sym->type->struct_name) {
            snprintf(info, info_len, "**%s**\n\n"
                     "```leno\n"
                     "%s: %s\n"
                     "```\n\n"
                     "%s enum 值 (%s)",
                     word,
                     word,
                     sym->type->struct_name,
                     is_global ? "全局" : "局部",
                     sym->type->struct_name);
        } else {
            snprintf(info, info_len, "**%s**\n\n"
                     "```leno\n"
                     "%s: enum\n"
                     "```\n\n"
                     "%s enum 值",
                     word,
                     word,
                     is_global ? "全局" : "局部");
        }
    } else {
        // 其他符号 - 显示更详细的类型信息
        const char* kind_str = "符号";
        if (sym->kind == SYM_LOCAL) kind_str = "局部变量";
        else if (sym->kind == SYM_PARAM) kind_str = "函数参数";
        else if (sym->kind == SYM_GLOBAL) kind_str = "全局变量";
        else if (sym->kind == SYM_GLOBAL_FUNC) kind_str = "全局函数";
        else if (sym->kind == SYM_MODULE) kind_str = "模块变量";
        else if (sym->kind == SYM_NATIVE) kind_str = "内置函数";
        else if (sym->kind == SYM_STRUCT) kind_str = "struct 类型";
        else if (sym->kind == SYM_CSTRUCT) kind_str = "cstruct 类型";
        else if (sym->kind == SYM_ENUM) kind_str = "enum 类型";
        else if (sym->kind == SYM_TYPE) kind_str = "类型别名";

        snprintf(info, info_len, "**%s**\n\n"
                 "```leno\n"
                 "%s: %s\n"
                 "```\n\n"
                 "%s",
                 word,
                 word,
                 type_str ? type_str : "unknown",
                 kind_str);
    }

    compiler_context_cleanup(&ctx);

    return info;
}

// 获取关键字文档
static char* get_keyword_doc(const char* word) {
    if (strcmp(word, "var") == 0) {
        return strdup("**var** - 变量声明关键字\n\n"
                     "用于声明变量，类型由右侧表达式推断。\n\n"
                     "```leno\n"
                     "var x = 10        // int\n"
                     "var s = \"hello\"  // string\n"
                     "var arr = [1,2,3] // Array[int]\n"
                     "```");
    }
    else if (strcmp(word, "func") == 0) {
        return strdup("**func** - 函数定义关键字\n\n"
                     "用于定义函数。\n\n"
                     "```leno\n"
                     "func add(int a, int b):int {\n"
                     "    return a + b\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "struct") == 0) {
        return strdup("**struct** - 结构体定义关键字\n\n"
                     "用于定义自定义数据结构。\n\n"
                     "```leno\n"
                     "struct Point {\n"
                     "    int x = 0\n"
                     "    int y = 0\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "impl") == 0) {
        return strdup("**impl** - 接口实现关键字\n\n"
                     "用于声明 struct 实现某个 face（接口）。\n\n"
                     "```leno\n"
                     "face Speaker {\n"
                     "    func speak():string\n"
                     "}\n\n"
                     "struct Dog impl Speaker {\n"
                     "    string name = \"\"\n"
                     "    func speak():string { return \"woof\" }\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "cstruct") == 0) {
        return strdup("**cstruct** - C 布局结构体定义关键字\n\n"
                     "定义与 C 兼容的内存布局结构体，用于 FFI 互操作。\n"
                     "cstruct 的字段必须是 C 布局类型（i8, u8, i32, f64, Ptr 等）。\n\n"
                     "```leno\n"
                     "cstruct Point {\n"
                     "    i32 x\n"
                     "    i32 y\n"
                     "}\n\n"
                     "var p = Point.malloc()\n"
                     "p.x = 10\n"
                     "p.y = 20\n"
                     "p.free()\n"
                     "```\n\n"
                     "> **注意**: cstruct 实例需要手动管理内存（malloc/free）。");
    }
    else if (strcmp(word, "import") == 0) {
        return strdup("**import** - 模块导入关键字\n\n"
                     "用于导入其他模块。\n\n"
                     "```leno\n"
                     "import \"math.leno\"\n"
                     "import \"utils.leno\" as u\n"
                     "```");
    }
    else if (strcmp(word, "if") == 0) {
        return strdup("**if** - 条件语句\n\n"
                     "```leno\n"
                     "if x > 0 {\n"
                     "    print(\"positive\")\n"
                     "} else if x < 0 {\n"
                     "    print(\"negative\")\n"
                     "} else {\n"
                     "    print(\"zero\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "else") == 0) {
        return strdup("**else** - 条件语句分支\n\n"
                     "与 if 配合使用，表示条件不满足时的分支。");
    }
    else if (strcmp(word, "eif") == 0) {
        return strdup("**eif** (else if) - 多条件分支\n\n"
                     "用于多个条件判断。\n\n"
                     "```leno\n"
                     "if x > 0 {\n"
                     "    print(\"positive\")\n"
                     "} eif x < 0 {\n"
                     "    print(\"negative\")\n"
                     "} else {\n"
                     "    print(\"zero\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "then") == 0) {
        return strdup("**then** - 单行 if 语句\n\n"
                     "用于单行条件语句。\n\n"
                     "```leno\n"
                     "if x > 0 then print(\"positive\")\n"
                     "```");
    }
    else if (strcmp(word, "for") == 0) {
        return strdup("**for** - 循环语句\n\n"
                     "```leno\n"
                     "// 范围循环\n"
                     "for 0:10 to i {\n"
                     "    print(i)\n"
                     "}\n\n"
                     "// 数组遍历\n"
                     "for arr to item {\n"
                     "    print(item)\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "while") == 0) {
        return strdup("**while** - 条件循环\n\n"
                     "```leno\n"
                     "while x > 0 {\n"
                     "    x = x - 1\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "break") == 0) {
        return strdup("**break** - 跳出循环\n\n"
                     "用于跳出当前循环。");
    }
    else if (strcmp(word, "continue") == 0) {
        return strdup("**continue** - 继续下一次循环\n\n"
                     "跳过当前迭代，继续下一次循环。");
    }
    else if (strcmp(word, "return") == 0) {
        return strdup("**return** - 返回值\n\n"
                     "从函数返回一个值。\n\n"
                     "```leno\n"
                     "func add(int a, int b):int {\n"
                     "    return a + b\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "is") == 0) {
        return strdup("**is** - 类型守卫关键字\n\n"
                     "用于运行时类型检查，在 if 块内收窄类型。\n\n"
                     "```leno\n"
                     "if x is int {\n"
                     "    // x 在此块内被视为 int\n"
                     "    int n = x + 10\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "in") == 0) {
        return strdup("**in** - 成员检查\n\n"
                     "检查元素是否在数组或字典中。\n\n"
                     "```leno\n"
                     "if 5 in arr {\n"
                     "    print(\"found\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "and") == 0) {
        return strdup("**and** - 逻辑与\n\n"
                     "逻辑与操作符。\n\n"
                     "```leno\n"
                     "if x > 0 and y > 0 {\n"
                     "    print(\"both positive\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "or") == 0) {
        return strdup("**or** - 逻辑或\n\n"
                     "逻辑或操作符。\n\n"
                     "```leno\n"
                     "if x > 0 or y > 0 {\n"
                     "    print(\"at least one positive\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "not") == 0) {
        return strdup("**not** - 逻辑非\n\n"
                     "逻辑非操作符。\n\n"
                     "```leno\n"
                     "if not x > 0 {\n"
                     "    print(\"not positive\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "switch") == 0) {
        return strdup("**switch** - 多分支选择\n\n"
                     "```leno\n"
                     "switch x {\n"
                     "    case 1: print(\"one\")\n"
                     "    case 2: print(\"two\")\n"
                     "    default: print(\"other\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "case") == 0) {
        return strdup("**case** - switch 分支\n\n"
                     "switch 语句中的一个分支。");
    }
    else if (strcmp(word, "default") == 0) {
        return strdup("**default** - switch 默认分支\n\n"
                     "switch 语句中的默认分支。");
    }
    else if (strcmp(word, "try") == 0) {
        return strdup("**try** - 异常处理\n\n"
                     "```leno\n"
                     "try {\n"
                     "    risky_operation()\n"
                     "} catch e {\n"
                     "    print(\"error: \" + e)\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "catch") == 0) {
        return strdup("**catch** - 捕获异常\n\n"
                     "捕获 try 块中抛出的异常。");
    }
    else if (strcmp(word, "throw") == 0) {
        return strdup("**throw** - 抛出异常\n\n"
                     "抛出一个异常。");
    }
    else if (strcmp(word, "finally") == 0) {
        return strdup("**finally** - 最终执行块\n\n"
                     "无论是否发生异常都会执行的代码块。");
    }
    else if (strcmp(word, "async") == 0) {
        return strdup("**async** - 异步函数\n\n"
                     "定义一个异步函数。\n\n"
                     "```leno\n"
                     "async func fetch_data():string {\n"
                     "    return await http_get(\"url\")\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "await") == 0) {
        return strdup("**await** - 等待异步操作\n\n"
                     "等待异步操作完成并返回结果。");
    }
    else if (strcmp(word, "enum") == 0) {
        return strdup("**enum** - 枚举类型\n\n"
                     "定义一组命名的常量。\n\n"
                     "```leno\n"
                     "enum Color {\n"
                     "    RED, GREEN, BLUE\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "face") == 0) {
        return strdup("**face** - 接口定义关键字\n\n"
                     "用于定义接口类型，指定 struct 必须实现的方法。\n\n"
                     "```leno\n"
                     "face Speaker {\n"
                     "    func speak():string\n"
                     "}\n\n"
                     "struct Dog impl Speaker {\n"
                     "    string name = \"\"\n"
                     "    func speak():string { return \"woof\" }\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "new") == 0) {
        return strdup("**new** - struct 实例化关键字\n\n"
                     "用于创建 struct 实例，必须使用命名参数。\n\n"
                     "```leno\n"
                     "struct Point {\n"
                     "    int x = 0\n"
                     "    int y = 0\n"
                     "}\n\n"
                     "var p1 = new Point(x = 10, y = 20)\n"
                     "var p2 = new Point()\n"
                     "var p3 = new math.Point(x = 1, y = 2)\n"
                     "```\n\n"
                     "> **注意**: `new` 关键字区分 struct 实例化和函数调用。");
    }
    else if (strcmp(word, "use") == 0) {
        return strdup("**use** - 导入符号关键字\n\n"
                     "从导入的模块中选择性导入符号到当前作用域。\n\n"
                     "```leno\n"
                     "import \"math.leno\" as math\n"
                     "use math.Point\n"
                     "use math.createPoint\n\n"
                     "var p = new Point(x = 10, y = 20)\n"
                     "```");
    }
    else if (strcmp(word, "true") == 0) {
        return strdup("**true** - 布尔真值\n\n"
                     "布尔类型的真值。");
    }
    else if (strcmp(word, "false") == 0) {
        return strdup("**false** - 布尔假值\n\n"
                     "布尔类型的假值。");
    }
    else if (strcmp(word, "null") == 0) {
        return strdup("**null** - 空值\n\n"
                     "表示空值或缺失值。");
    }
    else if (strcmp(word, "export") == 0) {
        return strdup("**export** - 导出符号\n\n"
                     "导出模块中的符号供其他模块使用。\n\n"
                     "```leno\n"
                     "export func add(int a, int b):int {\n"
                     "    return a + b\n"
                     "}\n"
                     "```");
    }
    else if (strcmp(word, "as") == 0) {
        return strdup("**as** - 别名\n\n"
                     "为导入的模块或导出的符号指定别名。\n\n"
                     "```leno\n"
                     "import \"math.leno\" as m\n"
                     "export func add as plus\n"
                     "```");
    }
    // 类型文档
    else if (strcmp(word, "int") == 0) {
        return strdup("**int** - 整数类型\n\n"
                     "64位有符号整数。\n\n"
                     "范围: -9,223,372,036,854,775,808 到 9,223,372,036,854,775,807\n\n"
                     "> **注意**: 当数值超过 int 范围时，会自动提升为 bigint，但对外仍然显示为 int 类型。");
    }
    else if (strcmp(word, "float") == 0) {
        return strdup("**float** - 浮点数类型\n\n"
                     "64位双精度浮点数。");
    }
    else if (strcmp(word, "string") == 0) {
        return strdup("**string** - 字符串类型\n\n"
                     "UTF-8 编码的字符串。");
    }
    else if (strcmp(word, "bool") == 0) {
        return strdup("**bool** - 布尔类型\n\n"
                     "值为 `true` 或 `false`。");
    }
    else if (strcmp(word, "Array") == 0) {
        return strdup("**Array** - 数组类型\n\n"
                     "动态数组，支持泛型。\n\n"
                     "```leno\n"
                     "var arr = [1, 2, 3]        // Array[int]\n"
                     "var strs = [\"a\", \"b\"]    // Array[string]\n"
                     "Array[int] nums = []       // 空数组\n"
                     "```");
    }
    else if (strcmp(word, "Dict") == 0) {
        return strdup("**Dict** - 字典类型\n\n"
                     "键值对映射，支持泛型。\n\n"
                     "```leno\n"
                     "var d = {\"a\": 1, \"b\": 2}  // Dict[string, int]\n"
                     "d[\"c\"] = 3\n"
                     "```");
    }
    else if (strcmp(word, "File") == 0) {
        return strdup("**File** - 文件类型\n\n"
                     "文件句柄类型。");
    }
    else if (strcmp(word, "Ptr") == 0) {
        return strdup("**Ptr** - 指针类型\n\n"
                     "原生指针类型，用于 FFI。");
    }
    else if (strcmp(word, "any") == 0) {
        return strdup("**any** - 任意类型\n\n"
                     "可以表示任何类型的值。");
    }
    else if (strcmp(word, "bigint") == 0) {
        return strdup("**bigint** - 大整数类型\n\n"
                     "支持任意精度整数运算。");
    }
    else if (strcmp(word, "i8") == 0) {
        return strdup("**i8** - C 布局 8 位有符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "u8") == 0) {
        return strdup("**u8** - C 布局 8 位无符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "i16") == 0) {
        return strdup("**i16** - C 布局 16 位有符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "u16") == 0) {
        return strdup("**u16** - C 布局 16 位无符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "i32") == 0) {
        return strdup("**i32** - C 布局 32 位有符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "u32") == 0) {
        return strdup("**u32** - C 布局 32 位无符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "i64") == 0) {
        return strdup("**i64** - C 布局 64 位有符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "u64") == 0) {
        return strdup("**u64** - C 布局 64 位无符号整数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "f32") == 0) {
        return strdup("**f32** - C 布局 32 位浮点数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "f64") == 0) {
        return strdup("**f64** - C 布局 64 位浮点数\n\n"
                     "用于 cstruct 字段定义和 FFI 互操作。");
    }
    else if (strcmp(word, "c_int") == 0) {
        return strdup("**c_int** - C 平台 int 类型\n\n"
                     "与 C 语言的 int 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "c_uint") == 0) {
        return strdup("**c_uint** - C 平台 unsigned int 类型\n\n"
                     "与 C 语言的 unsigned int 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "c_long") == 0) {
        return strdup("**c_long** - C 平台 long 类型\n\n"
                     "与 C 语言的 long 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "c_ulong") == 0) {
        return strdup("**c_ulong** - C 平台 unsigned long 类型\n\n"
                     "与 C 语言的 unsigned long 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "c_char") == 0) {
        return strdup("**c_char** - C 平台 char 类型\n\n"
                     "与 C 语言的 char 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "c_size") == 0) {
        return strdup("**c_size** - C 平台 size_t 类型\n\n"
                     "与 C 语言的 size_t 类型大小一致，用于 FFI 互操作。");
    }
    else if (strcmp(word, "str16") == 0) {
        return strdup("**str16** - UTF-16 字符串数组类型\n\n"
                     "用于 Windows API 的宽字符字符串互操作。");
    }
    else if (strcmp(word, "Thread") == 0) {
        return strdup("**Thread** - 线程类型\n\n"
                     "用于并发编程，通过 async 模块创建。");
    }
    else if (strcmp(word, "File") == 0) {
        return strdup("**File** - 文件类型\n\n"
                     "文件对象，通过 io.open() 创建。\n\n"
                     "```leno\n"
                     "File f = io.open(\"test.txt\", \"r\")\n"
                     "```");
    }
    else if (strcmp(word, "Ptr") == 0) {
        return strdup("**Ptr** - 指针类型\n\n"
                     "原始指针类型，用于 FFI 互操作。\n\n"
                     "```leno\n"
                     "Ptr[int] p = ffi.malloc(4)\n"
                     "```");
    }
    else if (strcmp(word, "Channel") == 0) {
        return strdup("**Channel** - Channel 类型\n\n"
                     "用于协程间通信，通过 async 模块创建。");
    }
    else if (strcmp(word, "face") == 0) {
        return strdup("**face** - 接口类型\n\n"
                     "定义接口类型，用于声明变量类型和类型守卫。\n\n"
                     "```leno\n"
                     "face Speaker {\n"
                     "    func speak():string\n"
                     "}\n\n"
                     "Speaker s = new Dog()\n"
                     "if s is Speaker {\n"
                     "    print(s.speak())\n"
                     "}\n"
                     "```");
    }
    // 内置函数
    else if (strcmp(word, "input") == 0) {
        return strdup("**input()** - 读取用户输入\n\n"
                     "从标准输入读取一行文本。\n\n"
                     "```leno\n"
                     "var name = input(\"请输入姓名: \")\n"
                     "```");
    }
    else if (strcmp(word, "sleep") == 0) {
        return strdup("**sleep(ms)** - 休眠\n\n"
                     "暂停当前线程指定毫秒数。\n\n"
                     "```leno\n"
                     "sleep(1000)  // 休眠 1 秒\n"
                     "```");
    }
    else if (strcmp(word, "assert") == 0) {
        return strdup("**assert(condition)** - 断言\n\n"
                     "断言条件为真，否则抛出异常。");
    }
    else if (strcmp(word, "assert_eq") == 0) {
        return strdup("**assert_eq(a, b)** - 相等断言\n\n"
                     "断言两个值相等，否则抛出异常。");
    }
    else if (strcmp(word, "assert_ne") == 0) {
        return strdup("**assert_ne(a, b)** - 不等断言\n\n"
                     "断言两个值不相等，否则抛出异常。");
    }
    else if (strcmp(word, "assert_true") == 0) {
        return strdup("**assert_true(value)** - 真值断言\n\n"
                     "断言值为 true，否则抛出异常。");
    }
    else if (strcmp(word, "assert_false") == 0) {
        return strdup("**assert_false(value)** - 假值断言\n\n"
                     "断言值为 false，否则抛出异常。");
    }
    else if (strcmp(word, "assert_null") == 0) {
        return strdup("**assert_null(value)** - 空值断言\n\n"
                     "断言值为 null，否则抛出异常。");
    }
    else if (strcmp(word, "format") == 0) {
        return strdup("**format(template, args...)** - 格式化字符串\n\n"
                     "使用模板和参数生成格式化字符串。");
    }
    else if (strcmp(word, "_args") == 0) {
        return strdup("**_args** - 命令行参数\n\n"
                     "返回命令行参数数组。");
    }
    else if (strcmp(word, "_script") == 0) {
        return strdup("**_script** - 脚本路径\n\n"
                     "返回当前脚本的文件路径。");
    }
    else if (strcmp(word, "_executable") == 0) {
        return strdup("**_executable** - 可执行文件路径\n\n"
                     "返回当前解释器的可执行文件路径。");
    }
    else if (strcmp(word, "_gc") == 0) {
        return strdup("**_gc()** - 垃圾回收\n\n"
                     "手动触发垃圾回收。");
    }
    else if (strcmp(word, "_os") == 0) {
        return strdup("**_os** - 操作系统名称\n\n"
                     "返回当前操作系统名称字符串（如 \"windows\", \"linux\", \"macos\"）。");
    }
    // 内置函数 - 从元数据表获取
    const BuiltinFunctionMeta* builtin = find_builtin_function(word);
    if (builtin) {
        return generate_builtin_doc(builtin);
    }
    
    // main 函数特殊处理
    if (strcmp(word, "main") == 0) {
        return strdup("**main** - 程序入口\n\n"
                     "程序的入口函数。\n\n"
                     "```leno\n"
                     "main() {\n"
                     "    print(\"Hello, World!\")\n"
                     "}\n"
                     "```");
    }
    
    return NULL;
}

// 解析模块方法调用 (如 "io.print", "maths.abs")
static bool parse_module_method(const char* word, char** module_out, char** method_out) {
    if (!word) return false;

    const char* dot = strchr(word, '.');
    if (!dot) return false;

    int module_len = dot - word;
    int method_len = strlen(dot + 1);

    if (module_len == 0 || method_len == 0) return false;

    *module_out = (char*)malloc(module_len + 1);
    *method_out = (char*)malloc(method_len + 1);

    if (!*module_out || !*method_out) {
        free(*module_out);
        free(*method_out);
        return false;
    }

    memcpy(*module_out, word, module_len);
    (*module_out)[module_len] = '\0';
    strcpy(*method_out, dot + 1);

    return true;
}

// 解析多层模块路径 (如 "color_module.Color.red")
// 返回解析的段数，segments 数组需要调用者提供足够空间
static int parse_module_path(const char* word, char** segments, int max_segments) {
    if (!word || !segments || max_segments <= 0) return 0;

    int count = 0;
    const char* p = word;

    while (*p && count < max_segments) {
        const char* dot = strchr(p, '.');
        int len = dot ? (int)(dot - p) : (int)strlen(p);

        if (len > 0) {
            segments[count] = (char*)malloc(len + 1);
            if (segments[count]) {
                memcpy(segments[count], p, len);
                segments[count][len] = '\0';
                count++;
            }
        }

        if (!dot) break;
        p = dot + 1;
    }

    return count;
}

// 从导入的模块获取导出符号的悬停信息（使用编译器获取完整类型）
// 支持: module.func, module.Enum, module.Enum.value, module.Struct.field
static char* get_module_symbol_hover(const char* content, const char* word, const char* current_file) {
    if (!content || !word) return NULL;

    char* segments[4];
    int segment_count = parse_module_path(word, segments, 4);
    if (segment_count < 2) {
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    // 获取模块路径（从 import 语句解析）
    char* module_path = NULL;

    // 查找 import 语句
    const char* p = content;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        if (strncmp(p, "import", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            while (*p && isspace((unsigned char)*p)) p++;

            char import_path[256] = {0};
            int path_len = 0;

            if (*p == '"') {
                p++;
                // 保留完整路径用于 read_module_file
                char full_import_path[256] = {0};
                int full_path_len = 0;
                while (*p && *p != '"' && path_len < 255 && full_path_len < 255) {
                    char c = *p++;
                    import_path[path_len++] = c;
                    full_import_path[full_path_len++] = c;
                }
                if (*p == '"') p++;
                import_path[path_len] = '\0';
                full_import_path[full_path_len] = '\0';

                // 从路径提取文件名用于别名匹配（无 as 时）
                char* slash = strrchr(import_path, '/');
                char* backslash = strrchr(import_path, '\\');
                char* last_sep = slash > backslash ? slash : backslash;
                if (last_sep) {
                    memmove(import_path, last_sep + 1, strlen(last_sep + 1) + 1);
                    path_len = strlen(import_path);
                }

                while (*p && isspace((unsigned char)*p)) p++;
                char alias[128] = {0};
                if (strncmp(p, "as", 2) == 0 && !isalnum((unsigned char)p[2])) {
                    p += 2;
                    while (*p && isspace((unsigned char)*p)) p++;
                    int alias_len = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && alias_len < 127) {
                        alias[alias_len++] = *p++;
                    }
                    alias[alias_len] = '\0';
                }

                char module_name_without_ext[256];
                if (alias[0]) {
                    if (strcmp(alias, segments[0]) == 0) {
                        module_path = strdup(full_import_path);  // 使用完整路径
                        break;
                    }
                } else {
                    strncpy(module_name_without_ext, import_path, sizeof(module_name_without_ext) - 1);
                    module_name_without_ext[sizeof(module_name_without_ext) - 1] = '\0';
                    char* dot = strrchr(module_name_without_ext, '.');
                    if (dot && strcmp(dot, ".leno") == 0) {
                        *dot = '\0';
                    }
                    if (strcmp(module_name_without_ext, segments[0]) == 0) {
                        module_path = strdup(full_import_path);  // 使用完整路径
                        break;
                    }
                }
            }
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!module_path) {
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    
        
    // 重置模块扫描栈（防止 LSP 多次请求间残留导致误报循环依赖）
    module_symbol_table_reset_scan_stack();
    
    // 使用轻量级 module_symbol_table（带磁盘缓存），不触发完整编译
    ModuleSymbolTable* table = module_symbol_table_create(module_path);
    free(module_path);

    if (!table) {
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    int scan_result = module_symbol_table_scan(table, current_file);
    if (scan_result != 0) {
        module_symbol_table_destroy(table);
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    char* result = NULL;

    if (segment_count == 2) {
        const char* symbol_name = segments[1];

        // 查找函数
        ModuleFuncSymbol* func = module_symbol_table_find_func(table, symbol_name);
        if (func) {
            int len = 1024 + strlen(word);
            result = (char*)malloc(len);
            if (result) {
                const char* ret_str = func->return_struct_name ? func->return_struct_name : type_kind_to_string(func->return_type);
                const char* params = (func->param_text && func->param_text[0]) ? func->param_text : "";
                if (func->type_param_count > 0) {
                    char tparams[128] = "[";
                    for (int gi = 0; gi < func->type_param_count; gi++) {
                        if (gi > 0) strcat(tparams, ", ");
                        char tb[16]; snprintf(tb, sizeof(tb), "T%d", gi+1);
                        strcat(tparams, tb);
                    }
                    strcat(tparams, "]");
                    snprintf(result, len, "**%s**\n\n```leno\n%s%s(%s) -> %s\n```\n\n模块函数",
                             word, word, tparams, params, ret_str);
                } else {
                    snprintf(result, len, "**%s**\n\n```leno\n%s(%s) -> %s\n```\n\n模块函数",
                             word, word, params, ret_str);
                }
            }
        }

        // 查找 struct
        if (!result) {
            ModuleStructSymbol* st = module_symbol_table_find_struct(table, symbol_name);
            if (st) {
                int len = 1024 + strlen(word);
                const char* kind_label = st->is_cstruct ? "cstruct" : "struct";
                result = (char*)malloc(len);
                if (result) {
                    if (st->type_param_count > 0) {
                        char params[128] = "[";
                        for (int gi = 0; gi < st->type_param_count; gi++) {
                            if (gi > 0) strcat(params, ", ");
                            if (st->type_param_names && st->type_param_names[gi])
                                strcat(params, st->type_param_names[gi]);
                            else { char tb[16]; snprintf(tb, sizeof(tb), "T%d", gi+1); strcat(params, tb); }
                        }
                        strcat(params, "]");
                        snprintf(result, len, "**%s**\n\n```leno\n%s %s%s\n```\n\n模块导出的 %s (%d 字段, %d 方法)",
                                 word, kind_label, symbol_name, params, kind_label, st->field_count, st->method_count);
                    } else {
                        snprintf(result, len, "**%s**\n\n```leno\n%s %s\n```\n\n模块导出的 %s (%d 字段, %d 方法)",
                                 word, kind_label, symbol_name, kind_label, st->field_count, st->method_count);
                    }
                }
            }
        }

        // 查找 enum
        if (!result) {
            ModuleEnumSymbol* en = module_symbol_table_find_enum(table, symbol_name);
            if (en) {
                int len = 1024 + strlen(word) + en->member_count * 32;
                result = (char*)malloc(len);
                if (result) {
                    char members[512] = {0};
                    for (int ei = 0; ei < en->member_count && ei < 8; ei++) {
                        if (ei > 0) strcat(members, ", ");
                        strcat(members, en->member_names[ei]);
                    }
                    if (en->member_count > 8) strcat(members, ", ...");
                    snprintf(result, len, "**%s**\n\n```leno\nenum %s { %s }\n```\n\n模块导出的 enum (%d 个值)",
                             word, symbol_name, members, en->member_count);
                }
            }
        }

        // 查找 face
        if (!result) {
            ModuleFaceSymbol* face = module_symbol_table_find_face(table, symbol_name);
            if (face) {
                int len = 1024 + strlen(word);
                result = (char*)malloc(len);
                if (result) {
                    snprintf(result, len, "**%s**\n\n```leno\nface %s\n```\n\n模块导出的 face (%d 个方法)",
                             word, symbol_name, face->method_count);
                }
            }
        }

        // 查找变量
        if (!result) {
            ModuleVarSymbol* var = module_symbol_table_find_var(table, symbol_name);
            if (var) {
                int len = 1024 + strlen(word);
                result = (char*)malloc(len);
                if (result) {
                    const char* tstr = var->struct_name ? var->struct_name : type_kind_to_string(var->type);
                    snprintf(result, len, "**%s**\n\n```leno\n%s: %s\n```\n\n模块导出的%s变量",
                             word, word, tstr, var->is_const ? "常量 " : " ");
                }
            }
        }

        // 查找 clib
        if (!result) {
            for (int ci = 0; ci < table->clib_count; ci++) {
                if (strcmp(table->clibs[ci].name, symbol_name) == 0) {
                    int len = 1024 + strlen(word);
                    result = (char*)malloc(len);
                    if (result) {
                        snprintf(result, len, "**%s**\n\n```leno\nclib %s\n```\n\n模块导出的 C 库类型 (%d 个函数)",
                                 word, symbol_name, table->clibs[ci].func_count);
                    }
                    break;
                }
            }
        }

        // 查找别名
        if (!result) {
            ModuleAliasSymbol* als = module_symbol_table_find_alias(table, symbol_name);
            if (als && als->type_info) {
                int len = 1024 + strlen(word);
                result = (char*)malloc(len);
                if (result) {
                    snprintf(result, len, "**%s**\n\n```leno\n%s: %s\n```\n\n模块导出的类型别名",
                             word, word, type_kind_to_string(als->type_info->kind));
                }
            }
        }

    } else if (segment_count == 3) {
        const char* type_name = segments[1];
        const char* member_name = segments[2];

        // 查找 enum 值
        ModuleEnumSymbol* en = module_symbol_table_find_enum(table, type_name);
        if (en) {
            for (int ei = 0; ei < en->member_count; ei++) {
                if (strcmp(en->member_names[ei], member_name) == 0) {
                    int len = 1024 + strlen(word);
                    result = (char*)malloc(len);
                    if (result) {
                        snprintf(result, len, "**%s**\n\n```leno\n%s.%s.%s\n```\n\n%s enum 值 (%s)",
                                 word, segments[0], type_name, member_name, type_name, type_name);
                    }
                    break;
                }
            }
        }

        // 查找 struct 字段
        if (!result) {
            ModuleStructSymbol* st = module_symbol_table_find_struct(table, type_name);
            if (st) {
                for (int fi = 0; fi < st->field_count; fi++) {
                    if (strcmp(st->fields[fi].name, member_name) == 0) {
                        int len = 1024 + strlen(word);
                        result = (char*)malloc(len);
                        if (result) {
                            const char* fts = st->fields[fi].struct_name ? st->fields[fi].struct_name : type_kind_to_string(st->fields[fi].type);
                            snprintf(result, len, "**%s**\n\n```leno\n%s: %s\n```\n\n%s 字段 (%s)",
                                     word, word, fts, type_name, type_name);
                        }
                        break;
                    }
                }
            }
        }

        // 查找 struct 方法
        if (!result) {
            ModuleStructSymbol* st = module_symbol_table_find_struct(table, type_name);
            if (st) {
                for (int mi = 0; mi < st->method_count; mi++) {
                    if (strcmp(st->methods[mi].name, member_name) == 0) {
                        int len = 1024 + strlen(word);
                        result = (char*)malloc(len);
                        if (result) {
                            const char* rts = st->methods[mi].return_struct_name ? st->methods[mi].return_struct_name :
                                                type_kind_to_string(st->methods[mi].return_type);
                                                        snprintf(result, len, "**%s**\n\n```leno\n%s.%s(...) -> %s\n```\n\n%s 方法",
                                     word, type_name, member_name, rts, type_name);
                        }
                        break;
                    }
                }
            }
        }
    }

    module_symbol_table_destroy(table);
    for (int i = 0; i < segment_count; i++) free(segments[i]);

    return result;
}

// 获取当前文件中 enum 值的悬停信息 (如 "Color.red")
static char* get_enum_value_hover(const char* content, const char* word) {
    if (!content || !word) return NULL;

    // 解析 enum 名称和值名称
    const char* dot = strchr(word, '.');
    if (!dot) return NULL;

    int enum_name_len = dot - word;
    const char* value_name = dot + 1;

    if (enum_name_len == 0 || !*value_name) return NULL;

    char enum_name[128];
    if (enum_name_len >= 127) enum_name_len = 127;
    memcpy(enum_name, word, enum_name_len);
    enum_name[enum_name_len] = '\0';

    // 在当前文件中查找 enum 定义
    const char* p = content;
    while (*p) {
        // 跳过空白和注释
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        // 跳过行注释
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            continue;
        }

        // 跳过块注释
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) p++;
            if (*p) p += 2;
            continue;
        }

        // 检查是否是 enum 定义（跳过 export 关键字）
        if (strncmp(p, "export", 6) == 0 && !isalnum((unsigned char)p[6]) && p[6] != '_') {
            p += 6;
            while (*p && isspace((unsigned char)*p)) p++;
        }

        if (strncmp(p, "enum", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            p += 4;
            while (*p && isspace((unsigned char)*p)) p++;

            // 解析 enum 名称
            char found_enum_name[128] = {0};
            int name_len = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && name_len < 127) {
                found_enum_name[name_len++] = *p++;
            }
            found_enum_name[name_len] = '\0';

            // 检查是否是我们要找的 enum
            if (strcmp(found_enum_name, enum_name) == 0) {
                // 跳过空白和 {
                while (*p && isspace((unsigned char)*p)) p++;
                if (*p == '{') p++;

                // 查找 enum 值
                while (*p) {
                    // 跳过空白
                    while (*p && isspace((unsigned char)*p)) p++;
                    if (!*p) break;

                    // 检查是否是结束
                    if (*p == '}') break;

                    // 解析值名称
                    char value_name_found[128] = {0};
                    int value_len = 0;
                    while (*p && (isalnum((unsigned char)*p) || *p == '_') && value_len < 127) {
                        value_name_found[value_len++] = *p++;
                    }
                    value_name_found[value_len] = '\0';

                    // 检查是否匹配
                    if (strcmp(value_name_found, value_name) == 0) {
                        // 找到了，构建悬停信息
                        size_t info_len = 512 + strlen(word) + strlen(enum_name);
                        char* info = (char*)malloc(info_len);
                        if (info) {
                            snprintf(info, info_len, "**%s**\n\n"
                                     "```leno\n"
                                     "%s\n"
                                     "```\n\n"
                                     "%s enum 值 (%s)",
                                     word, word, enum_name, enum_name);
                        }
                        return info;
                    }

                    // 跳过 = 和值
                    while (*p && *p != ',' && *p != '\n' && *p != '}') p++;
                    if (*p == ',') p++;
                }
            }
        }

        // 跳过这一行
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    return NULL;
}

// 从源代码文本中检测变量类型（回退方案）
// 搜索 "Type varname" 声明模式和闭包参数模式 "func(Type varname"
static char* detect_var_type_from_text(const char* content, const char* var_name) {
    if (!content || !var_name) return NULL;
    
    int var_len = strlen(var_name);
    const char* p = content;
    
    while (*p) {
        const char* found = strstr(p, var_name);
        if (!found) break;
        
        // 确保是完整的单词
        if ((found == content || (!isalnum((unsigned char)found[-1]) && found[-1] != '_')) &&
            (found[var_len] == '\0' || (!isalnum((unsigned char)found[var_len]) && found[var_len] != '_'))) {
            
            // 检查 found 之前的内容
            const char* before = found - 1;
            while (before >= content && isspace((unsigned char)*before)) before--;
            
            // 模式1: "Type varname" 声明（如 "string a", "Array tokens"）
            if (before >= content && (isalnum((unsigned char)*before) || *before == '_')) {
                const char* type_end = before + 1;
                const char* type_start = before;
                while (type_start > content && (isalnum((unsigned char)type_start[-1]) || type_start[-1] == '_')) {
                    type_start--;
                }
                int type_len = type_end - type_start;
                if (type_len > 0 && type_len < 64) {
                    char type_buf[64];
                    memcpy(type_buf, type_start, type_len);
                    type_buf[type_len] = '\0';
                    
                    if (strcmp(type_buf, "string") == 0) return strdup("string");
                    if (strcmp(type_buf, "Array") == 0) return strdup("Array");
                    if (strcmp(type_buf, "Dict") == 0) return strdup("Dict");
                    if (strcmp(type_buf, "int") == 0 || strcmp(type_buf, "float") == 0 ||
                        strcmp(type_buf, "bigint") == 0) return strdup("number");
                    if (strcmp(type_buf, "bool") == 0) return strdup("bool");
                    if (strcmp(type_buf, "File") == 0) return strdup("File");
                    // clib 类型（如 "sqlite3" 作为 clib 类型名，首字母小写）
                    // 检查是否是已知的 clib 类型名
                    // 注意：clib 名称通常不是大写开头，需要额外检测
                    // 这里通过检查前一个关键字是否是 "clib" 来确认
                    // 但在变量声明中 "sqlite3 l = ..." 不会带 "clib" 前缀
                    // 所以我们检查类型名是否以已知 clib 方式出现
                    // 回退方案：如果类型名不是大写开头且不是已知基本类型，
                    // 可能是 clib 类型，直接返回 "clib <name>"
                    if (!isupper((unsigned char)type_buf[0]) &&
                        strcmp(type_buf, "var") != 0 &&
                        strcmp(type_buf, "void") != 0 &&
                        strcmp(type_buf, "null") != 0 &&
                        strcmp(type_buf, "any") != 0 &&
                        strcmp(type_buf, "func") != 0 &&
                        strcmp(type_buf, "int") != 0 &&
                        strcmp(type_buf, "float") != 0 &&
                        strcmp(type_buf, "string") != 0 &&
                        strcmp(type_buf, "bool") != 0 &&
                        strcmp(type_buf, "Array") != 0 &&
                        strcmp(type_buf, "Dict") != 0 &&
                        strcmp(type_buf, "File") != 0 &&
                        strcmp(type_buf, "bigint") != 0) {
                        // 可能是 clib 类型（如 sqlite3, sdl3 等）
                        char* result = (char*)malloc(strlen(type_buf) + 6);
                        sprintf(result, "clib %s", type_buf);
                        return result;
                    }
                    // 其他类型名（如 struct 名）直接返回
                    if (isupper((unsigned char)type_buf[0])) return strdup(type_buf);
                }
            }
            
            // 模式2: 闭包参数 "func(Type varname" 或 "func(Type varname)"
            // before 指向非空白字符，检查是否是参数声明
            if (before >= content) {
                // 向前查找，看是否有 "(" 在附近（参数列表开始）
                const char* check = before;
                // 如果 before 是类型名的一部分，继续向前找到类型名的开始
                while (check > content && (isalnum((unsigned char)check[-1]) || check[-1] == '_')) check--;
                // check 现在指向类型名开始位置
                if (check > content) {
                    const char* before_type = check - 1;
                    while (before_type >= content && isspace((unsigned char)*before_type)) before_type--;
                    if (before_type >= content && *before_type == '(') {
                        // 找到了 "func(Type varname" 模式
                        // 提取类型名
                        int type_len = before - check + 1;
                        if (type_len > 0 && type_len < 64) {
                            char type_buf[64];
                            memcpy(type_buf, check, type_len);
                            type_buf[type_len] = '\0';
                            
                            if (strcmp(type_buf, "string") == 0) return strdup("string");
                            if (strcmp(type_buf, "Array") == 0) return strdup("Array");
                            if (strcmp(type_buf, "Dict") == 0) return strdup("Dict");
                            if (strcmp(type_buf, "int") == 0 || strcmp(type_buf, "float") == 0 ||
                                strcmp(type_buf, "bigint") == 0) return strdup("number");
                            if (strcmp(type_buf, "bool") == 0) return strdup("bool");
                            if (strcmp(type_buf, "File") == 0) return strdup("File");
                            // clib 类型（首字母小写的自定义类型名）
                            if (!isupper((unsigned char)type_buf[0]) &&
                                strcmp(type_buf, "var") != 0 &&
                                strcmp(type_buf, "void") != 0 &&
                                strcmp(type_buf, "null") != 0 &&
                                strcmp(type_buf, "any") != 0 &&
                                strcmp(type_buf, "func") != 0) {
                                char* result = (char*)malloc(strlen(type_buf) + 6);
                                sprintf(result, "clib %s", type_buf);
                                return result;
                            }
                            // struct 类型名（首字母大写）
                            if (isupper((unsigned char)type_buf[0])) return strdup(type_buf);
                        }
                    }
                }
            }
        }
        
        p = found + var_len;
    }
    
    return NULL;
}

// 从光标位置向前查找最近的闭包参数类型声明
// 模式: func(TypeName paramname) { ... 光标在此 ... }
// 返回 malloc 的类型名字符串，或 NULL
char* detect_closure_param_type_at_position(const char* content, int cursor_offset, const char* var_name) {
    if (!content || cursor_offset < 0 || !var_name) return NULL;
    
    int var_len = strlen(var_name);
    
    // 从光标向前搜索 "func(" 关键字
    // 搜索范围：光标前 2048 个字符（足够覆盖大多数闭包体）
    int search_start = cursor_offset - 2048;
    if (search_start < 0) search_start = 0;
    
    // 从光标向前找 "func("
    for (int i = cursor_offset; i >= search_start; i--) {
        if (i >= 4 && strncmp(content + i - 4, "func", 4) == 0 &&
            (i - 4 == 0 || !isalnum((unsigned char)content[i - 5])) &&
            content[i] == '(') {
            
            // 找到了 func(  ，现在解析参数列表
            // 在这个 func( 的参数列表中查找 var_name
            const char* paren_start = content + i;
            const char* p = paren_start + 1;
            
            // 跟踪括号深度（处理嵌套泛型如 Array[string]）
            int depth = 1;
            while (*p && depth > 0) {
                if (*p == '(') depth++;
                else if (*p == ')') depth--;
                if (depth == 0) break;
                
                // 跳过字符串
                if (*p == '"') {
                    p++;
                    while (*p && *p != '"') { if (*p == '\\') p++; p++; }
                    if (*p) p++;
                    continue;
                }
                
                // 尝试解析参数: "Type name" 或 "Type name, ..."
                // 跳过空白和逗号
                while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                if (!*p || *p == ')') break;
                
                // 读取类型名（支持泛型，如 Array[string], Dict[string, int]）
                const char* type_start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                
                // 处理泛型参数 [...]
                if (*p == '[') {
                    int bdepth = 1;
                    p++;
                    while (*p && bdepth > 0) {
                        if (*p == '[') bdepth++;
                        else if (*p == ']') bdepth--;
                        p++;
                    }
                }
                
                int type_len = p - type_start;
                
                // 跳过空白
                while (*p && isspace((unsigned char)*p)) p++;
                
                // 读取参数名
                const char* name_start = p;
                while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
                int name_len = p - name_start;
                
                if (name_len == var_len && strncmp(name_start, var_name, var_len) == 0) {
                    // 找到了匹配的参数名，提取类型
                    if (type_len > 0 && type_len < 128) {
                        // 去除泛型参数部分，只取基本类型名
                        char type_buf[128];
                        int copy_len = type_len < 127 ? type_len : 127;
                        memcpy(type_buf, type_start, copy_len);
                        type_buf[copy_len] = '\0';
                        
                        // 截取到 '[' 之前
                        char* bracket = strchr(type_buf, '[');
                        if (bracket) *bracket = '\0';
                        
                        // 映射已知类型
                        if (strcmp(type_buf, "string") == 0) return strdup("string");
                        if (strcmp(type_buf, "Array") == 0) return strdup("Array");
                        if (strcmp(type_buf, "Dict") == 0) return strdup("Dict");
                        if (strcmp(type_buf, "int") == 0 || strcmp(type_buf, "float") == 0 ||
                            strcmp(type_buf, "bigint") == 0) return strdup("number");
                        if (strcmp(type_buf, "bool") == 0) return strdup("bool");
                        if (strcmp(type_buf, "File") == 0) return strdup("File");
                        // struct 类型名（首字母大写）直接返回
                        if (isupper((unsigned char)type_buf[0])) return strdup(type_buf);
                        return strdup(type_buf);
                    }
                }
                
                // 继续查找下一个参数
                // p 已经在参数名之后，继续循环
            }
            
            // 这个 func( 中没找到匹配的参数名，继续向前搜索其他 func(
        }
    }
    
    return NULL;
}

// 根据变量名获取其类型（使用编译器分析）
// pos 用于位置感知的符号查找，避免在不同函数中同名变量混淆
static char* get_variable_type_from_compiler(const char* content, const char* var_name, const char* file_path, LspPosition pos) {
    if (!content || !var_name) return NULL;
    
    // 首先尝试基于光标位置的闭包参数类型检测
    // 闭包参数（如 func(Renderer r) 中的 r）在编译器作用域树中可能无法正确查找
    int cursor_offset = lsp_position_to_offset(content, pos);
    char* closure_type = detect_closure_param_type_at_position(content, cursor_offset, var_name);
    if (closure_type) {
        fprintf(stderr, "[HOVER-DEBUG] var='%s' closure param type detected='%s'\n", var_name, closure_type);
        fflush(stderr);
        return closure_type;
    }
    
    CompilerContext ctx;
    compiler_context_init(&ctx);
    
    // 编译分析
    compiler_analyze_with_filename(&ctx, content, file_path);
    
    if (!ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }
    
    // 获取符号信息
    char* type_str = NULL;
    
    // 优先使用位置感知的符号查找，避免不同函数中同名变量混淆
    Symbol* sym = find_symbol_in_current_function(ctx.root_scope, var_name, content, pos);
    if (sym) {
        type_str = strdup(type_to_string(sym->type));
    }
    
    // 如果位置感知查找失败，回退到全局 BFS 查找
    if (!type_str) {
        bool is_global = false;
        if (!compiler_get_symbol_info(&ctx, var_name, &type_str, &is_global)) {
            // 编译器未找到符号，尝试文本匹配回退
            char* fallback = detect_var_type_from_text(content, var_name);
            fprintf(stderr, "[HOVER-DEBUG] var='%s' compiler_get_symbol_info failed, fallback='%s'\n", var_name, fallback ? fallback : "NULL");
            fflush(stderr);
            compiler_context_cleanup(&ctx);
            return fallback;
        }
    }
    
    // 解析类型字符串，提取基本类型
    char* result = NULL;
    if (type_str) {
        // 检查类型字符串是否包含特定类型
        if (strstr(type_str, "string") != NULL) {
            result = strdup("string");
        } else if (strstr(type_str, "Array") != NULL || strstr(type_str, "array") != NULL) {
            result = strdup("Array");
        } else if (strstr(type_str, "Dict") != NULL || strstr(type_str, "dict") != NULL) {
            result = strdup("Dict");
        } else if (strncmp(type_str, "File", 4) == 0) {
            result = strdup("File");
        } else if (strncmp(type_str, "Ptr", 3) == 0) {
            result = strdup("ptr");
        } else if (strstr(type_str, "struct") != NULL) {
            // 对于 struct 类型，提取 struct 名称
            const char* struct_ptr = strstr(type_str, "struct");
            if (struct_ptr) {
                struct_ptr += 6; // 跳过 "struct"
                while (*struct_ptr && isspace((unsigned char)*struct_ptr)) struct_ptr++;
                if (*struct_ptr) {
                    result = strdup(struct_ptr);
                }
            }
        } else if (strncmp(type_str, "clib ", 5) == 0) {
            // clib 类型（如 "clib sqlite3"）
            // 提取 clib 名称并返回 "clib <name>" 格式
            result = strdup(type_str);
            fprintf(stderr, "[HOVER-DEBUG] var='%s' type_str='%s' (clib type)\n", var_name, type_str);
            fflush(stderr);
        } else if (isupper((unsigned char)type_str[0])) {
            // 可能是用户定义的 struct 类型名（如 MenuBar, TreeView, Window）
            // type_to_string 对 TYPE_STRUCT 直接返回 struct_name，不含 "struct" 前缀
            // 注意：不要求 struct_def_find 成功，因为 struct 可能定义在导入的模块中
            // （轻量级分析不会将导入模块的 struct 注册到全局表）
            result = strdup(type_str);
            fprintf(stderr, "[HOVER-DEBUG] var='%s' type_str='%s' struct_def_find=%s (returning anyway)\n",
                    var_name, type_str, struct_def_find(type_str) ? "found" : "NOT found");
            fflush(stderr);
        }
        free(type_str);
    }
    
    compiler_context_cleanup(&ctx);
    return result;
}

// 生成用户定义 struct 方法的悬停文档
static char* generate_struct_method_doc(const char* struct_name, const char* method_name) {
    ObjStructDef* sdef = struct_def_find(struct_name);
    if (!sdef) return NULL;
    
    for (int i = 0; i < sdef->method_count; i++) {
        if (strcmp(sdef->methods[i].name, method_name) == 0) {
            ObjFunction* fn = sdef->methods[i].func;
            if (!fn) break;
            
            int arity = fn->arity;
            
            // 构建参数列表
            char params_str[256] = {0};
            if (arity > 0 && fn->param_types) {
                int offset = 0;
                for (int j = 0; j < arity && offset < (int)sizeof(params_str) - 20; j++) {
                    const char* pt_str = type_kind_to_string(fn->param_types[j]);
                    if (j > 0) offset += snprintf(params_str + offset, sizeof(params_str) - offset, ", ");
                    offset += snprintf(params_str + offset, sizeof(params_str) - offset, "%s", pt_str);
                }
            }
            
            int len = 512 + strlen(struct_name) + strlen(method_name) + strlen(params_str);
            char* info = (char*)malloc(len);
            if (!info) return NULL;
            
            if (arity == 0) {
                snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s()\n```\n\n%s 结构体方法",
                         struct_name, method_name, struct_name, method_name, struct_name);
            } else {
                snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s)\n```\n\n%s 结构体方法（%d 个参数）",
                         struct_name, method_name, params_str,
                         struct_name, method_name, params_str,
                         struct_name, arity);
            }
            return info;
        }
    }
    return NULL;
}

// 从导入的模块符号表中查找 struct 方法并生成悬停文档
// 当 struct_def_find 失败时（struct 定义在导入的模块中），使用此回退
static char* generate_struct_method_doc_from_modules(const char* struct_name, const char* method_name,
                                                      const char* content, const char* file_path) {
    if (!struct_name || !method_name || !content || !file_path) return NULL;
    
    // 解析当前文件的导入
    int import_count = 0;
    ImportAlias* import_aliases = parse_imports(content, &import_count);
    if (!import_aliases || import_count <= 0) {
        if (import_aliases) free_import_aliases(import_aliases, import_count);
        fprintf(stderr, "[HOVER-DEBUG] generate_struct_method_doc_from_modules: no imports found\n");
        fflush(stderr);
        return NULL;
    }
    
    fprintf(stderr, "[HOVER-DEBUG] generate_struct_method_doc_from_modules: struct='%s' method='%s' imports=%d\n",
            struct_name, method_name, import_count);
    fflush(stderr);
    
    char* result = NULL;
    
    // 遍历所有导入的模块
    for (int i = 0; i < import_count && !result; i++) {
        const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
        fprintf(stderr, "[HOVER-DEBUG]   import[%d] alias='%s' module_path='%s'\n",
                i, import_aliases[i].alias, mp ? mp : "NULL");
        fflush(stderr);
        if (!mp) continue;
        
        module_symbol_table_reset_scan_stack();
        ModuleSymbolTable* mtable = module_symbol_table_create(mp);
        if (!mtable) {
            fprintf(stderr, "[HOVER-DEBUG]     module_symbol_table_create failed for '%s'\n", mp);
            fflush(stderr);
            continue;
        }
        
        int scan_result = module_symbol_table_scan(mtable, file_path);
        fprintf(stderr, "[HOVER-DEBUG]     scan result=%d struct_count=%d\n", scan_result, mtable->struct_count);
        fflush(stderr);
        
        if (scan_result == 0) {
            // 先列出所有 struct 名称
            for (int si = 0; si < mtable->struct_count; si++) {
                fprintf(stderr, "[HOVER-DEBUG]     struct[%d]='%s' methods=%d\n",
                        si, mtable->structs[si].name, mtable->structs[si].method_count);
                fflush(stderr);
            }
            
            ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, struct_name);
            if (mst) {
                fprintf(stderr, "[HOVER-DEBUG]     found struct '%s' method_count=%d\n", struct_name, mst->method_count);
                fflush(stderr);
                // 查找方法
                for (int j = 0; j < mst->method_count; j++) {
                    // 方法名可能带 "StructName::" 前缀（如 "Event::isQuit"），需要去除前缀后比较
                    const char* mname = mst->methods[j].name;
                    const char* sep = strstr(mname, "::");
                    if (sep) sep += 2; else sep = mname;
                    
                    fprintf(stderr, "[HOVER-DEBUG]       method[%d]='%s' (comparing with '%s')\n", j, mname, method_name);
                    fflush(stderr);
                    if (strcmp(sep, method_name) == 0) {
                        int arity = mst->methods[j].param_count;
                        const char* ret_str = mst->methods[j].return_struct_name ?
                            mst->methods[j].return_struct_name :
                            type_kind_to_string(mst->methods[j].return_type);
                        
                        // 构建参数列表
                        char params_str[256] = {0};
                        if (arity > 0 && mst->methods[j].param_types) {
                            int offset = 0;
                            for (int k = 0; k < arity && offset < (int)sizeof(params_str) - 20; k++) {
                                const char* pt_str = type_kind_to_string(mst->methods[j].param_types[k]);
                                if (k > 0) offset += snprintf(params_str + offset, sizeof(params_str) - offset, ", ");
                                offset += snprintf(params_str + offset, sizeof(params_str) - offset, "%s", pt_str);
                            }
                        }
                        
                        int len = 512 + strlen(struct_name) + strlen(method_name) + strlen(params_str) + strlen(ret_str);
                        result = (char*)malloc(len);
                        if (result) {
                            if (arity == 0) {
                                snprintf(result, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s 结构体方法",
                                         struct_name, method_name, struct_name, method_name, ret_str, struct_name);
                            } else {
                                snprintf(result, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s 结构体方法（%d 个参数）",
                                         struct_name, method_name, params_str,
                                         struct_name, method_name, params_str, ret_str,
                                         struct_name, arity);
                            }
                        }
                        break;
                    }
                }
            } else {
                fprintf(stderr, "[HOVER-DEBUG]     struct '%s' NOT found in this module\n", struct_name);
                fflush(stderr);
            }
        }
        module_symbol_table_destroy(mtable);
    }
    
    free_import_aliases(import_aliases, import_count);
    return result;
}

// 生成 clib 方法的悬停文档
// clib_type_str 格式为 "clib sqlite3"，从中提取 clib 名称并在编译器作用域或模块符号表中查找方法
static char* generate_clib_method_doc(const char* clib_type_str, const char* method_name,
                                       const char* content, const char* file_path) {
    if (!clib_type_str || !method_name || !content) return NULL;

    // 从 "clib sqlite3" 中提取 clib 名称
    const char* prefix = "clib ";
    if (strncmp(clib_type_str, prefix, 5) != 0) return NULL;
    const char* clib_name = clib_type_str + 5;
    if (!*clib_name) return NULL;

    fprintf(stderr, "[HOVER-DEBUG] generate_clib_method_doc: clib='%s' method='%s'\n", clib_name, method_name);
    fflush(stderr);

    // 途径1：从当前文件的编译器作用域中查找 clib 定义 Symbol
    {
        CompilerContext ctx;
        compiler_context_init(&ctx);
        compiler_analyze_with_filename(&ctx, content, file_path);

        if (ctx.root_scope) {
            Symbol* clib_sym = scope_resolve_tree_bfs(ctx.root_scope, clib_name);
            if (clib_sym && clib_sym->clib_func_count > 0) {
                for (int i = 0; i < clib_sym->clib_func_count; i++) {
                    if (strcmp(clib_sym->clib_func_names[i], method_name) == 0) {
                        TypeInfo* ret_type = clib_sym->clib_func_return_types[i];
                        const char* ret_str = ret_type ? type_to_string(ret_type) : "unknown";
                        int param_count = clib_sym->clib_func_param_counts[i];

                        // 构建参数列表
                        char params_str[512] = {0};
                        if (param_count > 0 && clib_sym->clib_func_param_types) {
                            int off = 0;
                            for (int j = 0; j < param_count && off < (int)sizeof(params_str) - 20; j++) {
                                TypeInfo* pt = clib_sym->clib_func_param_types[i][j];
                                const char* pt_str = pt ? type_to_string(pt) : "unknown";
                                if (j > 0) off += snprintf(params_str + off, sizeof(params_str) - off, ", ");
                                off += snprintf(params_str + off, sizeof(params_str) - off, "%s", pt_str);
                            }
                        }

                        int len = 512 + strlen(clib_name) + strlen(method_name) + strlen(params_str) + strlen(ret_str);
                        char* info = (char*)malloc(len);
                        if (info) {
                            if (param_count == 0) {
                                snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s clib 函数",
                                         clib_name, method_name, clib_name, method_name, ret_str, clib_name);
                            } else {
                                snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s clib 函数（%d 个参数）",
                                         clib_name, method_name, params_str,
                                         clib_name, method_name, params_str, ret_str,
                                         clib_name, param_count);
                            }
                        }
                        compiler_context_cleanup(&ctx);
                        return info;
                    }
                }
            }
        }
        compiler_context_cleanup(&ctx);
    }

    // 途径2：从导入的模块符号表中查找
    if (file_path) {
        int import_count = 0;
        ImportAlias* import_aliases = parse_imports(content, &import_count);
        if (import_aliases && import_count > 0) {
            for (int i = 0; i < import_count; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (!mp) continue;

                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                if (!mtable) continue;

                if (module_symbol_table_scan(mtable, file_path) == 0) {
                    ModuleClibSymbol* mclib = module_symbol_table_find_clib(mtable, clib_name);
                    if (mclib) {
                        for (int j = 0; j < mclib->func_count; j++) {
                            if (strcmp(mclib->funcs[j].name, method_name) == 0) {
                                const char* ret_str = mclib->funcs[j].return_struct_name ?
                                    mclib->funcs[j].return_struct_name :
                                    type_kind_to_string(mclib->funcs[j].return_type);
                                int param_count = mclib->funcs[j].param_count;

                                // 构建参数列表
                                char params_str[512] = {0};
                                if (param_count > 0) {
                                    int off = 0;
                                    for (int k = 0; k < param_count && off < (int)sizeof(params_str) - 20; k++) {
                                        const char* pt_str = type_kind_to_string(mclib->funcs[j].param_types[k]);
                                        if (k > 0) off += snprintf(params_str + off, sizeof(params_str) - off, ", ");
                                        off += snprintf(params_str + off, sizeof(params_str) - off, "%s", pt_str);
                                    }
                                }

                                int len = 512 + strlen(clib_name) + strlen(method_name) + strlen(params_str) + strlen(ret_str);
                                char* info = (char*)malloc(len);
                                if (info) {
                                    if (param_count == 0) {
                                        snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s clib 函数",
                                                 clib_name, method_name, clib_name, method_name, ret_str, clib_name);
                                    } else {
                                        snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s clib 函数（%d 个参数）",
                                                 clib_name, method_name, params_str,
                                                 clib_name, method_name, params_str, ret_str,
                                                 clib_name, param_count);
                                    }
                                }
                                module_symbol_table_destroy(mtable);
                                free_import_aliases(import_aliases, import_count);
                                return info;
                            }
                        }
                    }
                }
                module_symbol_table_destroy(mtable);
            }
        }
        if (import_aliases) free_import_aliases(import_aliases, import_count);
    }

    // 途径3：当编译器解析失败时，使用模块符号表扫描当前文件本身
    // 模块符号表的扫描是纯文本解析，不依赖编译器
    if (file_path) {
        module_symbol_table_reset_scan_stack();
        ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
        if (cur_table) {
            if (module_symbol_table_scan(cur_table, file_path) == 0) {
                ModuleClibSymbol* mclib = module_symbol_table_find_clib(cur_table, clib_name);
                if (mclib) {
                    for (int j = 0; j < mclib->func_count; j++) {
                        if (strcmp(mclib->funcs[j].name, method_name) == 0) {
                            const char* ret_str = mclib->funcs[j].return_struct_name ?
                                mclib->funcs[j].return_struct_name :
                                type_kind_to_string(mclib->funcs[j].return_type);
                            int param_count = mclib->funcs[j].param_count;

                            // 构建参数列表
                            char params_str[512] = {0};
                            if (param_count > 0) {
                                int off = 0;
                                for (int k = 0; k < param_count && off < (int)sizeof(params_str) - 20; k++) {
                                    const char* pt_str = type_kind_to_string(mclib->funcs[j].param_types[k]);
                                    if (k > 0) off += snprintf(params_str + off, sizeof(params_str) - off, ", ");
                                    off += snprintf(params_str + off, sizeof(params_str) - off, "%s", pt_str);
                                }
                            }

                            int len = 512 + strlen(clib_name) + strlen(method_name) + strlen(params_str) + strlen(ret_str);
                            char* info = (char*)malloc(len);
                            if (info) {
                                if (param_count == 0) {
                                    snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s clib 函数",
                                             clib_name, method_name, clib_name, method_name, ret_str, clib_name);
                                } else {
                                    snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s clib 函数（%d 个参数）",
                                             clib_name, method_name, params_str,
                                             clib_name, method_name, params_str, ret_str,
                                             clib_name, param_count);
                                }
                            }
                            module_symbol_table_destroy(cur_table);
                            return info;
                        }
                    }
                }
            }
            module_symbol_table_destroy(cur_table);
        }
    }

    return NULL;
}

// 检查是否是实例方法名（如 add, insert, len 等）
// 如果是，返回对应的类型名（如 "array", "string", "dict" 等）
static const char* is_instance_method(const char* method_name) {
    // 按优先级检查各类型的实例方法
    // 注意：类型名必须与 method_table_register_with_params 中注册的名称一致
    static const char* type_keys[] = {
        "Array", "string", "Dict", "File", "Thread", "Channel", "number", NULL
    };
    
    int count;
    for (int i = 0; type_keys[i] != NULL; i++) {
        char** methods = native_get_instance_methods(type_keys[i], &count);
        if (methods) {
            for (int j = 0; j < count; j++) {
                if (strcmp(methods[j], method_name) == 0) {
                    native_free_instance_method_list(methods, count);
                    return type_keys[i];
                }
            }
            native_free_instance_method_list(methods, count);
        }
    }
    
    return NULL;
}

// 生成实例方法文档
static char* generate_instance_method_doc(const char* type_name, const char* method_name) {
    int arity = native_get_instance_method_arity(type_name, method_name);
    if (arity < 0) return NULL;
    
    TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, NULL);
    const char* return_type_str = type_kind_to_string(return_type);
    
    // 构建参数列表字符串
    char params_str[256] = {0};
    if (arity > 0) {
        int offset = 0;
        for (int i = 0; i < arity && offset < (int)sizeof(params_str) - 20; i++) {
            TypeKind pt = native_get_instance_method_param_type(type_name, method_name, i);
            const char* pt_str = type_kind_to_string(pt);
            if (i > 0) offset += snprintf(params_str + offset, sizeof(params_str) - offset, ", ");
            offset += snprintf(params_str + offset, sizeof(params_str) - offset, "%s", pt_str);
        }
    }
    
    int len = 512 + strlen(type_name) + strlen(method_name) + strlen(params_str);
    char* info = (char*)malloc(len);
    if (!info) return NULL;
    
    if (arity == 0) {
        snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s 类型的实例方法",
                 type_name, method_name, type_name, method_name, return_type_str, type_name);
    } else if (arity < 0) {
        snprintf(info, len, "**%s.%s(...)**\n\n```leno\n%s.%s(...) -> %s\n```\n\n%s 类型的实例方法（可变参数）",
                 type_name, method_name, type_name, method_name, return_type_str, type_name);
    } else {
        snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s 类型的实例方法（%d 个参数）",
                 type_name, method_name, params_str,
                 type_name, method_name, params_str, return_type_str,
                 type_name, arity);
    }
    
    return info;
}

// 获取悬停信息
char* lsp_get_hover_info(const char* content, LspPosition pos, const char* file_path) {
    if (!content) return NULL;
    
    clock_t t_start = clock();
    fprintf(stderr, "[HOVER] START line=%d char=%d\n", pos.line, pos.character);
    fflush(stderr);
    
    char* word = get_word_at_position(content, pos);
    if (!word) return NULL;
    
    // 如果光标在注释或字符串字面量中，不提供悬停提示
    int hover_offset = lsp_position_to_offset(content, pos);
    if (is_inside_comment(content, hover_offset) || is_inside_string_literal(content, hover_offset)) {
        free(word);
        return NULL;
    }
    
    char* info = NULL;
    
    // 0. 检查是否是导入模块的导出符号 (如 "color_module.Color.red")
    // 这需要在模块方法检查之前，因为模块方法只支持两层（module.method）
    info = get_module_symbol_hover(content, word, file_path);

    // 0.5 检查是否是当前文件中的 enum 值 (如 "Color.red")
    if (!info) {
        info = get_enum_value_hover(content, word);
    }

    // 1. 尝试获取关键字文档
    if (!info) {
        info = get_keyword_doc(word);
    }

    // 2. 尝试获取内置函数文档
    if (!info) {
        const BuiltinFunctionMeta* builtin = find_builtin_function(word);
        if (builtin) {
            info = generate_builtin_doc(builtin);
        }
    }

    // 3. 尝试从编译器获取符号信息（局部变量、全局变量、函数等）
    //    这一步在模块方法/实例方法之前，使得悬停在 "tokens.add" 的 "tokens" 上时
    //    能显示变量类型信息，而不是方法文档
    if (!info) {
        info = get_symbol_hover_from_compiler(content, word, pos, file_path);
    }

    // 4. 检查是否是模块方法调用 (如 "io.print") 或实例方法调用 (如 "s.len")
    char* module = NULL;
    char* method = NULL;
    if (!info && parse_module_method(word, &module, &method)) {
        // 首先尝试作为模块方法
        int arity = native_get_module_method_arity(module, method);
        if (arity >= 0) {
            // 模块方法存在，生成文档
            TypeKind return_type = native_get_module_method_return_type(module, method);
            const char* return_type_str = type_kind_to_string(return_type);
            
            // 构建参数列表字符串
            char params_str[256] = {0};
            if (arity > 0) {
                int offset = 0;
                for (int i = 0; i < arity && offset < (int)sizeof(params_str) - 20; i++) {
                    TypeKind pt = native_get_module_method_param_type(module, method, i);
                    const char* pt_str = type_kind_to_string(pt);
                    if (i > 0) offset += snprintf(params_str + offset, sizeof(params_str) - offset, ", ");
                    offset += snprintf(params_str + offset, sizeof(params_str) - offset, "%s", pt_str);
                }
            }

            int len = 512 + strlen(module) + strlen(method) + strlen(params_str);
            info = (char*)malloc(len);
            if (info) {
                if (arity == 0) {
                    snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```",
                             module, method, module, method, return_type_str);
                } else if (arity < 0) {
                    snprintf(info, len, "**%s.%s(...)**\n\n```leno\n%s.%s(...) -> %s\n```",
                             module, method, module, method, return_type_str);
                } else {
                    snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```",
                             module, method, params_str,
                             module, method, params_str, return_type_str);
                }
            }
        } else {
            // 不是模块方法，尝试作为实例方法
            int offset = lsp_position_to_offset(content, pos);
            if (!is_inside_string_literal(content, offset) && !is_inside_comment(content, offset)) {
                // 首先尝试使用编译器确定变量类型
                char* var_type = get_variable_type_from_compiler(content, module, file_path, pos);
                if (var_type) {
                    fprintf(stderr, "[HOVER-DEBUG] var='%s' resolved type='%s', method='%s'\n", module, var_type, method);
                    fflush(stderr);
                    
                    // 先尝试原生实例方法
                    int arity = native_get_instance_method_arity(var_type, method);
                    if (arity >= 0) {
                        info = generate_instance_method_doc(var_type, method);
                    }
                    
                    // 如果原生方法没找到，尝试用户定义的 struct 方法
                    if (!info) {
                        info = generate_struct_method_doc(var_type, method);
                    }
                    
                    // 如果 struct_def_find 也失败，从导入的模块符号表中查找
                    if (!info) {
                        info = generate_struct_method_doc_from_modules(var_type, method, content, file_path);
                    }
                    
                    // 如果仍然没找到，尝试 clib 方法
                    if (!info) {
                        info = generate_clib_method_doc(var_type, method, content, file_path);
                    }
                    free(var_type);
                } else {
                    fprintf(stderr, "[HOVER-DEBUG] var='%s' type resolution failed (NULL)\n", module);
                    fflush(stderr);
                }

                // 如果编译器无法确定类型，使用启发式方法
                if (!info) {
                    const char* type_name = is_instance_method(method);
                    if (type_name) {
                        info = generate_instance_method_doc(type_name, method);
                    }
                }
            }
        }
        free(module);
        free(method);
    }

    // 5. 检查是否在 Style[xxx] = { } 字典初始化上下文中的字段
    if (!info) {
        int offset = lsp_position_to_offset(content, pos);
        if (offset > 0 && !is_inside_string_literal(content, offset) && !is_inside_comment(content, offset)) {
            // 检测光标是否在 Style[xxx] = { ... } 的字典内部
            int search_pos = offset - 1;
            int brace_depth = 0;
            int in_str = 0;
            char str_quote = 0;
            
            // 向前查找包含光标的 '{'
            while (search_pos >= 0) {
                char c = content[search_pos];
                if (in_str) {
                    if (c == str_quote) {
                        int bc = 0;
                        int ck = search_pos - 1;
                        while (ck >= 0 && content[ck] == '\\') { bc++; ck--; }
                        if (bc % 2 == 0) in_str = 0;
                    }
                } else {
                    if (c == '"' || c == '\'') { in_str = 1; str_quote = c; }
                    else if (c == '}') brace_depth++;
                    else if (c == '{') {
                        if (brace_depth == 0) break;
                        brace_depth--;
                    }
                }
                search_pos--;
            }
        }
    }
    
    // 5. 检查是否是实例方法名（如 add, insert, len 等）
    // 但不在字符串字面量或注释中显示实例方法提示
    if (!info) {
        int offset = lsp_position_to_offset(content, pos);
        if (!is_inside_string_literal(content, offset) && !is_inside_comment(content, offset)) {
            const char* type_name = is_instance_method(word);
            if (type_name) {
                info = generate_instance_method_doc(type_name, word);
            }
        }
    }

    free(word);
    
    clock_t t_end = clock();
    fprintf(stderr, "[HOVER] END found=%s time=%.0fms\n",
            info ? "yes" : "no", (double)(t_end - t_start) * 1000.0 / CLOCKS_PER_SEC);
    fflush(stderr);
    
    return info;
}
