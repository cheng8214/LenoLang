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

    // 从当前位置向前搜索 "struct" 或 "cstruct" 关键字
    // 我们需要找到最近的一个 "struct Name {" 或 "cstruct Name {" 定义，且该定义在光标之前
    const char* search_start = content + offset;

    // 向前搜索 "cstruct" 和 "struct" 关键字
    for (const char* p = search_start; p >= content; p--) {
        // 先检查 "cstruct"（7个字符，必须先检查长的）
        if (p + 7 <= search_start &&
            strncmp(p, "cstruct", 7) == 0 &&
            (p == content || !isalnum((unsigned char)*(p-1))) &&
            !isalnum((unsigned char)*(p+7))) {

            // 找到 cstruct 后，解析名称
            const char* name_start = p + 7;

            // 跳过空白字符
            while (*name_start && isspace((unsigned char)*name_start)) {
                name_start++;
            }

            // 现在 name_start 应该指向 cstruct 名称
            const char* name_end = name_start;
            while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_')) {
                name_end++;
            }

            int name_len = name_end - name_start;

            if (name_len > 0) {
                // 检查这个 cstruct 定义是否包含光标位置
                const char* brace = name_end;
                while (*brace && *brace != '{' && *brace != '\n') {
                    brace++;
                }

                if (*brace == '{') {
                    int brace_offset = brace - content;
                    if (is_offset_in_block(content, brace_offset, offset)) {
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

// 前向声明：文本匹配变量类型推断（定义在本文件后面）
// 非 static 以供 comp_symbols.c 等其他模块使用
char* detect_var_type_from_text(const char* content, const char* var_name);

// 前向声明：struct 方法文档生成
static char* generate_struct_method_doc(const char* struct_name, const char* method_name,
                                                      const char* content, const char* file_path);
static char* generate_struct_method_doc_from_modules(const char* struct_name, const char* method_name,
                                                      const char* content, const char* file_path);

// 从编译器获取符号悬停信息
/* GCC -Wformat-truncation 误报：snprintf 的 size 参数已确保足够大，
   但 GCC 无法推断 strlen 结果的上界而报警。此处局部禁用。 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
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
            size_t type_len = type_str ? strlen(type_str) : 0;
            size_t base_len = strlen(base_word);
            /* info_len 足够容纳所有格式字符串和参数 */
            size_t info_len = base_len + type_len + 256;
            char* info = (char*)malloc(info_len);
            if (info) {
                bool is_global = (var_sym->scope == ctx.root_scope);
                const char* kind_str = "局部";
                if (is_global) kind_str = "全局";
                else if (var_sym->kind == SYM_PARAM) kind_str = "参数";
                else if (var_sym->kind == SYM_MODULE) kind_str = "模块";

                int written = snprintf(info, info_len, "**%s**\n\n"
                         "```leno\n"
                         "%s: %s\n"
                         "```\n\n"
                         "%s%s",
                         base_word,
                         base_word,
                         type_str ? type_str : "unknown",
                         kind_str,
                         " 变量");
                (void)written;  /* info_len 已确保足够大 */
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

        // 回退1：尝试文本匹配推断类型（仅对简单变量名，不对带点号的表达式）
        // 带点号的表达式（如 _font.renderGlyphBlended）应该由方法悬停逻辑处理
        if (word && !strchr(word, '.')) {
            char* fallback_type = detect_var_type_from_text(content, word);
            if (fallback_type) {
                size_t info_len = 512 + strlen(word) + strlen(fallback_type);
                char* info = (char*)malloc(info_len);
                if (info) {
                    snprintf(info, info_len, "**%s**\n\n"
                             "```leno\n"
                             "%s: %s\n"
                             "```\n\n"
                             "字段",
                             word, word, fallback_type);
                }
                free(fallback_type);
                return info;
            }
        }
        
        // 回退2：尝试查找 struct 方法（方法在符号表中是 "StructName::method" 格式）
        // 当直接查找 method 名失败时，可能在当前 struct 的方法列表中
        if (word && !strchr(word, '.')) {
            char* current_struct = find_struct_name_at_position(content, pos);
            if (current_struct) {
                char* method_doc = generate_struct_method_doc(current_struct, word, content, file_path);
                if (method_doc) {
                    free(current_struct);
                    return method_doc;
                }
                
                // 也尝试从导入模块中查找
                method_doc = generate_struct_method_doc_from_modules(current_struct, word, content, file_path);
                if (method_doc) {
                    free(current_struct);
                    return method_doc;
                }
                
                free(current_struct);
            }
        }
        
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

        // 对函数类型特殊处理：显示 "func xx(params):ret" 而非 "xx: func(params):ret"
        bool is_func_sym = (sym->kind == SYM_GLOBAL_FUNC || sym->kind == SYM_MODULE);
        if (is_func_sym && type_str && strncmp(type_str, "func", 4) == 0) {
            // type_str 格式: "func(params):ret" → 改为 "func xx(params):ret"
            const char* after_func = type_str + 4;  // 跳过 "func"，指向 "(" 或其他
            snprintf(info, info_len, "**%s**\n\n"
                     "```leno\n"
                     "func %s%s\n"
                     "```\n\n"
                     "%s",
                     word,
                     word,
                     after_func,
                     kind_str);
        } else {
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
    }

    compiler_context_cleanup(&ctx);

    return info;
}
#pragma GCC diagnostic pop  /* -Wformat-truncation */

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
                     "> **注意**: cstruct 实例需要手动管理内存（malloc/free）。\n\n"
                     "> **属性修饰符**: 可使用 `packed` 取消字段间 padding，或使用 `align(N)` 指定整体对齐边界。\n"
                     "> ```leno\n"
                     "> packed cstruct PackedData { ... }\n"
                     "> align(16) cstruct AlignedData { ... }\n"
                     "> packed align(16) cstruct ComboData { ... }\n"
                     "> ```");
    }
    else if (strcmp(word, "packed") == 0) {
        return strdup("**packed** - cstruct 属性修饰符\n\n"
                     "取消结构体中所有字段间的 padding，使字段紧挨排列。\n"
                     "用于精确控制内存布局，常与二进制协议解析、网络封包等场景配合。\n\n"
                     "```leno\n"
                     "packed cstruct Packet {\n"
                     "    u8  type    // offset 0\n"
                     "    i32 seq     // offset 1  ← 无 padding!\n"
                     "    u8  flag    // offset 5\n"
                     "}               // total=6, align=1\n"
                     "```\n\n"
                     "> 可与 `align(N)` 组合使用，顺序可互换: `packed align(16) cstruct` 或 `align(16) packed cstruct`");
    }
    else if (strcmp(word, "align") == 0) {
        return strdup("**align(N)** - cstruct 属性修饰符\n\n"
                     "指定结构体的整体对齐边界为 N 字节。\n"
                     "N 必须是 2 的幂（如 1, 2, 4, 8, 16, 32...）。\n\n"
                     "```leno\n"
                     "align(16) cstruct CacheLine {\n"
                     "    i64 data   // offset 0\n"
                     "}              // total=16, align=16\n"
                     "```\n\n"
                     "> 可与 `packed` 组合使用，顺序可互换: `align(16) packed cstruct` 或 `packed align(16) cstruct`");
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
    else if (strcmp(word, "Socket") == 0) {
        return strdup("**Socket** - Socket 类型\n\n"
                     "用于网络通信，通过 sockets 模块创建。");
    }
    else if (strcmp(word, "Future") == 0) {
        return strdup("**Future** - Future 类型\n\n"
                     "异步操作的结果，通过 async/await 获取。");
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
                    snprintf(result, len, "**%s**\n\n```leno\nfunc %s%s(%s): %s\n```\n\n模块函数",
                             word, word, tparams, params, ret_str);
                } else {
                    snprintf(result, len, "**%s**\n\n```leno\nfunc %s(%s): %s\n```\n\n模块函数",
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
// 如果当前文件中找不到，遍历导入的模块查找 enum 定义
static char* get_enum_value_hover(const char* content, const char* word, const char* file_path) {
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

    // 当前文件中未找到 enum 定义，遍历导入的模块查找
    {
        // 解析 import 语句，获取所有导入的模块路径
        extern ImportAlias* parse_imports(const char* content, int* count);
        extern void free_import_aliases(ImportAlias* aliases, int count);
        extern const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias);
        extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);

        int imp_count = 0;
        ImportAlias* imports = parse_imports(content, &imp_count);

        for (int i = 0; i < imp_count; i++) {
            const char* mod_path = find_module_path_by_alias(imports, imp_count, imports[i].alias);
            char resolved[MAX_PATH_LEN] = {0};

            if (!mod_path) {
                // 尝试通过包搜索路径解析
                if (package_resolve_module_file(imports[i].alias, resolved, sizeof(resolved)) == 1) {
                    mod_path = resolved;
                }
            }

            if (!mod_path) continue;

            module_symbol_table_reset_scan_stack();
            ModuleSymbolTable* table = module_symbol_table_create(mod_path);
            if (!table) continue;

            if (module_symbol_table_scan(table, file_path) != 0) {
                module_symbol_table_destroy(table);
                continue;
            }

            // 在模块符号表中查找 enum
            ModuleEnumSymbol* en = module_symbol_table_find_enum(table, enum_name);
            if (en) {
                // 查找匹配的成员
                for (int ei = 0; ei < en->member_count; ei++) {
                    if (strcmp(en->member_names[ei], value_name) == 0) {
                        size_t info_len = 512 + strlen(word) + strlen(enum_name);
                        char* info = (char*)malloc(info_len);
                        if (info) {
                            // 构建成员列表
                            char members[512] = {0};
                            for (int mi = 0; mi < en->member_count && mi < 12; mi++) {
                                if (mi > 0) strcat(members, ", ");
                                strcat(members, en->member_names[mi]);
                            }
                            if (en->member_count > 12) strcat(members, ", ...");

                            snprintf(info, info_len, "**%s**\n\n"
                                     "```leno\n"
                                     "%s  // %s.%s\n"
                                     "```\n\n"
                                     "%s enum 值 (来自模块 %s)\n\n"
                                     "enum %s { %s }",
                                     word, word, enum_name, value_name,
                                     enum_name, imports[i].alias,
                                     enum_name, members);
                        }
                        module_symbol_table_destroy(table);
                        free_import_aliases(imports, imp_count);
                        return info;
                    }
                }
            }

            module_symbol_table_destroy(table);
        }

        free_import_aliases(imports, imp_count);
    }

    // 也遍历 use 语句导入的模块查找 enum
    {
        const char* use_p = content;
        while (*use_p) {
            while (*use_p && (*use_p == ' ' || *use_p == '\t')) use_p++;
            if (!*use_p) break;

            // 跳过注释
            if (*use_p == '/' && use_p[1] == '/') {
                while (*use_p && *use_p != '\n') use_p++;
                if (*use_p) use_p++;
                continue;
            }

            // 检测 use 语句
            if (strncmp(use_p, "use", 3) != 0 || (use_p[3] && (isalnum((unsigned char)use_p[3]) || use_p[3] == '_'))) {
                while (*use_p && *use_p != '\n') use_p++;
                if (*use_p) use_p++;
                continue;
            }

            use_p += 3;
            while (*use_p && (*use_p == ' ' || *use_p == '\t')) use_p++;

            // 提取模块名
            const char* mod_start = use_p;
            while (*use_p && (isalnum((unsigned char)*use_p) || *use_p == '_')) use_p++;
            int use_mod_len = (int)(use_p - mod_start);
            if (use_mod_len <= 0 || use_mod_len >= 64) {
                while (*use_p && *use_p != '\n') use_p++;
                if (*use_p) use_p++;
                continue;
            }

            char use_mod_name[65];
            memcpy(use_mod_name, mod_start, use_mod_len);
            use_mod_name[use_mod_len] = '\0';

            if (*use_p != '.') {
                while (*use_p && *use_p != '\n') use_p++;
                if (*use_p) use_p++;
                continue;
            }
            use_p++;

            // 处理批量模式 use module.(A, B, C) 和单个模式 use module.Type
            int batch_mode = (*use_p == '(');
            if (batch_mode) use_p++;

            while (1) {
                if (batch_mode) {
                    while (*use_p && (*use_p == ' ' || *use_p == '\t')) use_p++;
                    if (*use_p == ')') { use_p++; break; }
                }

                // 读取类型名
                const char* type_start = use_p;
                while (*use_p && (isalnum((unsigned char)*use_p) || *use_p == '_')) use_p++;
                int use_type_len = (int)(use_p - type_start);
                if (use_type_len <= 0 || use_type_len >= 64) {
                    if (batch_mode) {
                        while (*use_p && *use_p != ',' && *use_p != ')' && *use_p != '\n') use_p++;
                        if (*use_p == ',') { use_p++; continue; }
                        if (*use_p == ')') { use_p++; break; }
                    }
                    break;
                }

                char use_type_name[65];
                memcpy(use_type_name, type_start, use_type_len);
                use_type_name[use_type_len] = '\0';

                // 如果 use 导入的类型名与 enum_name 匹配，查找对应模块
                if (strcmp(use_type_name, enum_name) == 0) {
                    // 解析模块路径
                    extern ImportAlias* parse_imports(const char* content, int* count);
                    extern void free_import_aliases(ImportAlias* aliases, int count);
                    extern const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias);
                    extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);

                    int use_imp_count = 0;
                    ImportAlias* use_imports = parse_imports(content, &use_imp_count);

                    const char* use_mod_path = find_module_path_by_alias(use_imports, use_imp_count, use_mod_name);
                    char use_resolved[MAX_PATH_LEN] = {0};

                    if (!use_mod_path) {
                        if (package_resolve_module_file(use_mod_name, use_resolved, sizeof(use_resolved)) == 1) {
                            use_mod_path = use_resolved;
                        }
                    }

                    if (use_mod_path) {
                        module_symbol_table_reset_scan_stack();
                        ModuleSymbolTable* use_table = module_symbol_table_create(use_mod_path);
                        if (use_table && module_symbol_table_scan(use_table, file_path) == 0) {
                            ModuleEnumSymbol* use_en = module_symbol_table_find_enum(use_table, enum_name);
                            if (use_en) {
                                for (int ei = 0; ei < use_en->member_count; ei++) {
                                    if (strcmp(use_en->member_names[ei], value_name) == 0) {
                                        size_t info_len = 512 + strlen(word) + strlen(enum_name);
                                        char* info = (char*)malloc(info_len);
                                        if (info) {
                                            char members[512] = {0};
                                            for (int mi = 0; mi < use_en->member_count && mi < 12; mi++) {
                                                if (mi > 0) strcat(members, ", ");
                                                strcat(members, use_en->member_names[mi]);
                                            }
                                            if (use_en->member_count > 12) strcat(members, ", ...");

                                            snprintf(info, info_len, "**%s**\n\n"
                                                     "```leno\n"
                                                     "%s  // %s.%s\n"
                                                     "```\n\n"
                                                     "%s enum 值 (来自模块 %s, use 导入)\n\n"
                                                     "enum %s { %s }",
                                                     word, word, enum_name, value_name,
                                                     enum_name, use_mod_name,
                                                     enum_name, members);
                                        }
                                        module_symbol_table_destroy(use_table);
                                        free_import_aliases(use_imports, use_imp_count);
                                        return info;
                                    }
                                }
                            }
                        }
                        if (use_table) module_symbol_table_destroy(use_table);
                    }

                    free_import_aliases(use_imports, use_imp_count);
                }

                if (!batch_mode) break;

                while (*use_p && (*use_p == ' ' || *use_p == '\t')) use_p++;
                if (*use_p == ',') use_p++;
                while (*use_p && (*use_p == ' ' || *use_p == '\t')) use_p++;
                if (*use_p == ')') { use_p++; break; }
            }

            while (*use_p && *use_p != '\n') use_p++;
            if (*use_p) use_p++;
        }
    }

    return NULL;
}

// 从源代码文本中检测变量类型（回退方案）
// 搜索 "Type varname" 声明模式和闭包参数模式 "func(Type varname"
char* detect_var_type_from_text(const char* content, const char* var_name) {
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
            
            // 跳过可空类型后缀 '?'（如 "Font? _font" 中的 '?'）
            if (before >= content && *before == '?') {
                before--;
                while (before >= content && isspace((unsigned char)*before)) before--;
            }
            
            // 模式0: 泛型类型声明（如 "Array[CachedTex] _texCache", "Dict[string, int] _map"）
            // 当 before 指向 ']' 时，向前解析整个泛型类型
            if (before >= content && *before == ']') {
                // 向前匹配 '['
                int bracket_depth = 1;
                const char* bp = before - 1;
                while (bp >= content && bracket_depth > 0) {
                    if (*bp == ']') bracket_depth++;
                    else if (*bp == '[') bracket_depth--;
                    if (bracket_depth > 0) bp--;
                }
                if (bracket_depth == 0 && bp > content) {
                    // bp 指向 '['，向前提取基础类型名
                    const char* gt_end = bp;
                    const char* gt_start = bp - 1;
                    while (gt_start >= content && (isalnum((unsigned char)*gt_start) || *gt_start == '_')) {
                        gt_start--;
                    }
                    gt_start++;
                    int base_len = gt_end - gt_start;
                    if (base_len > 0 && base_len < 64) {
                        char base_buf[64];
                        memcpy(base_buf, gt_start, base_len);
                        base_buf[base_len] = '\0';
                        // 提取完整的泛型类型字符串（如 "Array[CachedTex]"）
                        int full_len = before - gt_start + 1;
                        if (full_len > 0 && full_len < 256) {
                            char* full_type = (char*)malloc(full_len + 1);
                            if (full_type) {
                                memcpy(full_type, gt_start, full_len);
                                full_type[full_len] = '\0';
                                return full_type;
                            }
                        }
                    }
                }
            }
            
            // 模式1: "Type varname" 声明（如 "string a", "Array tokens"）
            // 也支持 "Type? varname" 模式（如 "Font? _font"）
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
                    if (strcmp(type_buf, "any") == 0) return strdup("any");
                    if (strcmp(type_buf, "Ptr") == 0) return strdup("Ptr");
                    if (strcmp(type_buf, "Socket") == 0) return strdup("Socket");
                    if (strcmp(type_buf, "Channel") == 0) return strdup("Channel");
                    if (strcmp(type_buf, "Thread") == 0) return strdup("Thread");
                    if (strcmp(type_buf, "Future") == 0) return strdup("Future");
                    // C 布局类型统一映射为 number
                    if (strcmp(type_buf, "i8") == 0 || strcmp(type_buf, "u8") == 0 ||
                        strcmp(type_buf, "i16") == 0 || strcmp(type_buf, "u16") == 0 ||
                        strcmp(type_buf, "i32") == 0 || strcmp(type_buf, "u32") == 0 ||
                        strcmp(type_buf, "i64") == 0 || strcmp(type_buf, "u64") == 0 ||
                        strcmp(type_buf, "f32") == 0 || strcmp(type_buf, "f64") == 0 ||
                        strcmp(type_buf, "c_int") == 0 || strcmp(type_buf, "c_uint") == 0 ||
                        strcmp(type_buf, "c_long") == 0 || strcmp(type_buf, "c_ulong") == 0 ||
                        strcmp(type_buf, "c_longlong") == 0 || strcmp(type_buf, "c_ulonglong") == 0 ||
                        strcmp(type_buf, "c_size") == 0 || strcmp(type_buf, "c_ssize") == 0)
                        return strdup("number");
                    if (strcmp(type_buf, "str8") == 0) return strdup("string");
                    if (strcmp(type_buf, "str16") == 0) return strdup("string");
                    // clib 类型（如 "sqlite3" 作为 clib 类型名，首字母小写）
                    // 检查是否是已知的 clib 类型名
                    // 注意：必须排除关键字（if/for/while/return等）避免误判
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
                        strcmp(type_buf, "bigint") != 0 &&
                        strcmp(type_buf, "Ptr") != 0 &&
                        strcmp(type_buf, "Socket") != 0 &&
                        strcmp(type_buf, "Channel") != 0 &&
                        strcmp(type_buf, "Thread") != 0 &&
                        strcmp(type_buf, "Future") != 0 &&
                        strcmp(type_buf, "i8") != 0 &&
                        strcmp(type_buf, "u8") != 0 &&
                        strcmp(type_buf, "i16") != 0 &&
                        strcmp(type_buf, "u16") != 0 &&
                        strcmp(type_buf, "i32") != 0 &&
                        strcmp(type_buf, "u32") != 0 &&
                        strcmp(type_buf, "i64") != 0 &&
                        strcmp(type_buf, "u64") != 0 &&
                        strcmp(type_buf, "f32") != 0 &&
                        strcmp(type_buf, "f64") != 0 &&
                        strcmp(type_buf, "str8") != 0 &&
                        strcmp(type_buf, "str16") != 0 &&
                        // 排除语言关键字，避免把 "if/for/while/return" 当成 clib 类型
                        strcmp(type_buf, "if") != 0 &&
                        strcmp(type_buf, "else") != 0 &&
                        strcmp(type_buf, "for") != 0 &&
                        strcmp(type_buf, "while") != 0 &&
                        strcmp(type_buf, "return") != 0 &&
                        strcmp(type_buf, "not") != 0 &&
                        strcmp(type_buf, "and") != 0 &&
                        strcmp(type_buf, "or") != 0 &&
                        strcmp(type_buf, "in") != 0 &&
                        strcmp(type_buf, "is") != 0 &&
                        strcmp(type_buf, "as") != 0 &&
                        strcmp(type_buf, "from") != 0 &&
                        strcmp(type_buf, "import") != 0 &&
                        strcmp(type_buf, "struct") != 0 &&
                        strcmp(type_buf, "face") != 0 &&
                        strcmp(type_buf, "enum") != 0 &&
                        strcmp(type_buf, "match") != 0 &&
                        strcmp(type_buf, "break") != 0 &&
                        strcmp(type_buf, "continue") != 0 &&
                        strcmp(type_buf, "true") != 0 &&
                        strcmp(type_buf, "false") != 0 &&
                        // 类型名必须包含至少一个数字或长度>2且非纯关键字（简单启发式）
                        (strpbrk(type_buf, "0123456789") != NULL || strlen(type_buf) > 5)) {
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
            // 也支持 "func(Type? varname" 模式
            // before 指向非空白字符，检查是否是参数声明
            if (before >= content) {
                // 向前查找，看是否有 "(" 在附近（参数列表开始）
                const char* check = before;
                // 如果 before 指向 '?'，先跳过它再提取类型名
                if (*before == '?') {
                    check = before - 1;
                    while (check > content && (isalnum((unsigned char)check[-1]) || check[-1] == '_')) check--;
                } else {
                    // 如果 before 是类型名的一部分，继续向前找到类型名的开始
                    while (check > content && (isalnum((unsigned char)check[-1]) || check[-1] == '_')) check--;
                }
                // check 现在指向类型名开始位置
                if (check > content) {
                    const char* before_type = check - 1;
                    while (before_type >= content && isspace((unsigned char)*before_type)) before_type--;
                    // 跳过可能存在的 '?'
                    if (before_type >= content && *before_type == '?') {
                        before_type--;
                        while (before_type >= content && isspace((unsigned char)*before_type)) before_type--;
                    }
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
                            if (strcmp(type_buf, "any") == 0) return strdup("any");
                            if (strcmp(type_buf, "Ptr") == 0) return strdup("Ptr");
                            if (strcmp(type_buf, "Socket") == 0) return strdup("Socket");
                            if (strcmp(type_buf, "Channel") == 0) return strdup("Channel");
                            if (strcmp(type_buf, "Thread") == 0) return strdup("Thread");
                            if (strcmp(type_buf, "Future") == 0) return strdup("Future");
                            if (strcmp(type_buf, "i8") == 0 || strcmp(type_buf, "u8") == 0 ||
                                strcmp(type_buf, "i16") == 0 || strcmp(type_buf, "u16") == 0 ||
                                strcmp(type_buf, "i32") == 0 || strcmp(type_buf, "u32") == 0 ||
                                strcmp(type_buf, "i64") == 0 || strcmp(type_buf, "u64") == 0 ||
                                strcmp(type_buf, "f32") == 0 || strcmp(type_buf, "f64") == 0 ||
                                strcmp(type_buf, "c_int") == 0 || strcmp(type_buf, "c_uint") == 0 ||
                                strcmp(type_buf, "c_long") == 0 || strcmp(type_buf, "c_ulong") == 0 ||
                                strcmp(type_buf, "c_longlong") == 0 || strcmp(type_buf, "c_ulonglong") == 0 ||
                                strcmp(type_buf, "c_size") == 0 || strcmp(type_buf, "c_ssize") == 0)
                                return strdup("number");
                            if (strcmp(type_buf, "str8") == 0 || strcmp(type_buf, "str16") == 0) return strdup("string");
                            // clib 类型（首字母小写的自定义类型名）
                            // 必须排除关键字避免误判
                            if (!isupper((unsigned char)type_buf[0]) &&
                                strcmp(type_buf, "var") != 0 &&
                                strcmp(type_buf, "void") != 0 &&
                                strcmp(type_buf, "null") != 0 &&
                                strcmp(type_buf, "any") != 0 &&
                                strcmp(type_buf, "func") != 0 &&
                                strcmp(type_buf, "Ptr") != 0 &&
                                strcmp(type_buf, "Socket") != 0 &&
                                strcmp(type_buf, "Channel") != 0 &&
                                strcmp(type_buf, "Thread") != 0 &&
                                strcmp(type_buf, "Future") != 0 &&
                                strcmp(type_buf, "i8") != 0 &&
                                strcmp(type_buf, "u8") != 0 &&
                                strcmp(type_buf, "i16") != 0 &&
                                strcmp(type_buf, "u16") != 0 &&
                                strcmp(type_buf, "i32") != 0 &&
                                strcmp(type_buf, "u32") != 0 &&
                                strcmp(type_buf, "i64") != 0 &&
                                strcmp(type_buf, "u64") != 0 &&
                                strcmp(type_buf, "f32") != 0 &&
                                strcmp(type_buf, "f64") != 0 &&
                                strcmp(type_buf, "str8") != 0 &&
                                strcmp(type_buf, "str16") != 0 &&
                                strcmp(type_buf, "if") != 0 &&
                                strcmp(type_buf, "for") != 0 &&
                                strcmp(type_buf, "while") != 0 &&
                                strcmp(type_buf, "return") != 0 &&
                                strcmp(type_buf, "not") != 0 &&
                                strcmp(type_buf, "and") != 0 &&
                                strcmp(type_buf, "or") != 0 &&
                                (strpbrk(type_buf, "0123456789") != NULL || strlen(type_buf) > 5)) {
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
                        if (strcmp(type_buf, "any") == 0) return strdup("any");
                        if (strcmp(type_buf, "Ptr") == 0) return strdup("Ptr");
                        if (strcmp(type_buf, "Socket") == 0) return strdup("Socket");
                        if (strcmp(type_buf, "Channel") == 0) return strdup("Channel");
                        if (strcmp(type_buf, "Thread") == 0) return strdup("Thread");
                        if (strcmp(type_buf, "Future") == 0) return strdup("Future");
                        if (strcmp(type_buf, "i8") == 0 || strcmp(type_buf, "u8") == 0 ||
                            strcmp(type_buf, "i16") == 0 || strcmp(type_buf, "u16") == 0 ||
                            strcmp(type_buf, "i32") == 0 || strcmp(type_buf, "u32") == 0 ||
                            strcmp(type_buf, "i64") == 0 || strcmp(type_buf, "u64") == 0 ||
                            strcmp(type_buf, "f32") == 0 || strcmp(type_buf, "f64") == 0 ||
                            strcmp(type_buf, "c_int") == 0 || strcmp(type_buf, "c_uint") == 0 ||
                            strcmp(type_buf, "c_long") == 0 || strcmp(type_buf, "c_ulong") == 0 ||
                            strcmp(type_buf, "c_longlong") == 0 || strcmp(type_buf, "c_ulonglong") == 0 ||
                            strcmp(type_buf, "c_size") == 0 || strcmp(type_buf, "c_ssize") == 0)
                            return strdup("number");
                        if (strcmp(type_buf, "str8") == 0 || strcmp(type_buf, "str16") == 0) return strdup("string");
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
    
    // 回退1：检查是否是 struct 字段
    // struct 方法体中可以直接使用字段名（如 _font），编译器会将其转换为 self.field
    // 但 LSP 的作用域查找可能找不到这些字段，需要通过 struct 定义回退查找
    if (!type_str) {
        char** struct_names = NULL;
        int struct_count = compiler_find_structs_with_field(&ctx, var_name, &struct_names);
        if (struct_count > 0) {
            char* field_type_str = NULL;
            bool found = compiler_get_struct_field_info(&ctx, struct_names[0], var_name, &field_type_str);
            if (found && field_type_str) {
                type_str = field_type_str;
            }
            for (int i = 0; i < struct_count; i++) {
                free(struct_names[i]);
            }
            free(struct_names);
        }
    }

    // 如果位置感知查找和 struct 字段查找都失败，回退到全局 BFS 查找
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
        // 剥离可空类型后缀 '?'（如 "Font?" -> "Font"）
        size_t ts_len = strlen(type_str);
        while (ts_len > 0 && type_str[ts_len - 1] == '?') {
            type_str[ts_len - 1] = '\0';
            ts_len--;
        }
        if (ts_len == 0) {
            free(type_str);
            compiler_context_cleanup(&ctx);
            return NULL;
        }
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
static char* generate_struct_method_doc(const char* struct_name, const char* method_name,
                                                      const char* content, const char* file_path) {
    (void)file_path;  // 保留参数以兼容调用方接口，但此函数不直接使用
    ObjStructDef* sdef = struct_def_find(struct_name);
    if (!sdef) {
        // struct_def_find 失败，尝试 cstruct 内置方法
        // cstruct 的方法注册在 cstructMethodTable 中，struct_def_find 查不到
        // cstruct 的方法是固定的内置方法，使用硬编码元信息
        CStructMethodEntry centry = cstruct_find_method_meta(method_name);
        if (centry.method) {
            int arity = centry.arity;
            char params_str[256] = {0};
            if (arity > 0) {
                int off = 0;
                for (int j = 0; j < arity && off < (int)sizeof(params_str) - 20; j++) {
                    const char* pt_str = type_kind_to_string(centry.param_types[j]);
                    if (j > 0) off += snprintf(params_str + off, sizeof(params_str) - off, ", ");
                    off += snprintf(params_str + off, sizeof(params_str) - off, "%s", pt_str);
                }
            }
            const char* ret_str = type_kind_to_string(centry.return_type);
            int len = 512 + strlen(struct_name) + strlen(method_name) + strlen(params_str) + strlen(ret_str);
            char* info = (char*)malloc(len);
            if (!info) return NULL;
            if (arity == 0) {
                snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s 内置方法",
                         struct_name, method_name, struct_name, method_name, ret_str, struct_name);
            } else {
                snprintf(info, len, "**%s.%s(%s)**\n\n```leno\n%s.%s(%s) -> %s\n```\n\n%s 内置方法（%d 个参数）",
                         struct_name, method_name, params_str,
                         struct_name, method_name, params_str, ret_str,
                         struct_name, arity);
            }
            return info;
        }
        return NULL;
    }
    
    for (int i = 0; i < sdef->method_count; i++) {
        if (strcmp(sdef->methods[i].name, method_name) == 0) {
            ObjFunction* fn = sdef->methods[i].func;
            
            if (fn) {
                int arity = fn->arity;
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
            } else {
                // fn 为 NULL（LSP 模式下未生成 ObjFunction），从源文件解析方法签名
                // 格式: func method_name(params): return_type {  或  func method_name(params) {
                char* method_info = NULL;
                if (content) {
                    // 搜索 "func method_name" 模式
                    char search_pattern[256];
                    snprintf(search_pattern, sizeof(search_pattern), "func %s", method_name);
                    
                    const char* p = content;
                    while ((p = strstr(p, search_pattern)) != NULL) {
                        // 确保是完整的单词（前面不是字母/数字/下划线）
                        if (p > content && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) {
                            p += strlen(search_pattern);
                            continue;
                        }
                        // 确保后面是空白或'('
                        char next = p[strlen(search_pattern)];
                        if (next != ' ' && next != '\t' && next != '\n' && next != '\r' && next != '(') {
                            p += strlen(search_pattern);
                            continue;
                        }
                        
                        // 找到了方法定义，提取整个签名行
                        // 先找到行开始
                        const char* line_start = p;
                        while (line_start > content && line_start[-1] != '\n') line_start--;
                        
                        // 找到行结束（第一个 '{' 或 '\n'）
                        const char* line_end = p + strlen(search_pattern);
                        while (*line_end && *line_end != '{' && *line_end != '\n') line_end++;
                        if (*line_end == '{') line_end--; // 回退到 '{' 前
                        while (line_end > p && isspace((unsigned char)*line_end)) line_end--;
                        line_end++;
                        
                        // 提取签名文本
                        int sig_len = line_end - line_start;
                        if (sig_len > 0 && sig_len < 512) {
                            char sig_text[512];
                            memcpy(sig_text, line_start, sig_len);
                            sig_text[sig_len] = '\0';
                            
                            // 构建悬停信息
                            int len = 512 + strlen(struct_name) + strlen(method_name) + sig_len + 64;
                            method_info = (char*)malloc(len);
                            if (method_info) {
                                snprintf(method_info, len,
                                         "**%s.%s**\n\n```leno\n%s\n```\n\n%s 结构体方法",
                                         struct_name, method_name,
                                         sig_text,
                                         struct_name);
                            }
                            break;
                        }
                        p += strlen(search_pattern);
                    }
                }
                
                if (method_info) return method_info;
                
                // 如果源文件解析失败，回退到基本信息
                int len = 256 + strlen(struct_name) + strlen(method_name);
                char* info = (char*)malloc(len);
                if (!info) return NULL;
                snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s()\n```\n\n%s 结构体方法",
                         struct_name, method_name, struct_name, method_name, struct_name);
                return info;
            }
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

// 从光标位置向前查找函数调用（如 ttfLib()），返回函数名
// 调用者负责 free 返回的字符串
// dot_offset: 点号 '.' 的偏移位置
static char* find_func_call_before_dot(const char* content, int dot_offset) {
    if (!content || dot_offset <= 0) return NULL;

    // 从点号向前，应该遇到 ')'
    int pos = dot_offset - 1;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    if (pos < 0 || content[pos] != ')') return NULL;

    // 向前查找匹配的 '('
    int paren_depth = 1;
    pos--;
    while (pos >= 0 && paren_depth > 0) {
        if (content[pos] == ')') paren_depth++;
        else if (content[pos] == '(') paren_depth--;
        else if (content[pos] == '"' || content[pos] == '\'') {
            // 跳过字符串
            char quote = content[pos];
            pos--;
            while (pos >= 0 && content[pos] != quote) {
                if (pos > 0 && content[pos] == '\\') pos--;
                pos--;
            }
        }
        if (paren_depth > 0) pos--;
    }
    if (paren_depth != 0) return NULL;

    // pos 现在指向 '('，向前提取函数名
    pos--;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    if (pos < 0) return NULL;

    int name_end = pos + 1;
    while (pos >= 0 && (isalnum((unsigned char)content[pos]) || content[pos] == '_')) {
        pos--;
    }
    int name_start = pos + 1;
    int name_len = name_end - name_start;
    if (name_len <= 0) return NULL;

    char* func_name = (char*)malloc(name_len + 1);
    if (!func_name) return NULL;
    memcpy(func_name, content + name_start, name_len);
    func_name[name_len] = '\0';
    return func_name;
}

// 处理函数调用链的悬停（如 ttfLib().TTF_RenderText_Blended_Wrapped）
// 当 word 以 '.' 开头时，尝试从内容中查找前面的函数调用，解析其返回类型
static char* handle_func_call_chain_hover(const char* content, const char* word,
                                            LspPosition pos, const char* file_path) {
    if (!content || !word || word[0] != '.') return NULL;

    const char* method_name = word + 1;
    if (!*method_name) return NULL;

    // 获取光标偏移，找到点号位置
    int cursor_offset = lsp_position_to_offset(content, pos);
    if (cursor_offset < 0) return NULL;

    // 从光标向前查找点号
    int dot_offset = -1;
    int scan = cursor_offset;
    while (scan >= 0) {
        char c = content[scan];
        if (c == '.') { dot_offset = scan; break; }
        if (isalnum((unsigned char)c) || c == '_') { scan--; continue; }
        break;
    }
    if (dot_offset < 0) return NULL;

    // 查找点号前的函数调用
    char* func_name = find_func_call_before_dot(content, dot_offset);
    if (!func_name) return NULL;

    fprintf(stderr, "[HOVER-DEBUG] func call chain: func='%s' method='%s'\n", func_name, method_name);
    fflush(stderr);

    // 通过编译器解析函数返回类型
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);

    char* result = NULL;
    if (ctx.root_scope) {
        // 查找函数符号
        Symbol* func_sym = find_symbol_in_current_function(ctx.root_scope, func_name, content, pos);
        if (!func_sym) {
            func_sym = scope_resolve_tree_bfs(ctx.root_scope, func_name);
        }

        if (func_sym && func_sym->type && func_sym->type->kind == TYPE_FUNCTION &&
            func_sym->type->return_type) {
            TypeInfo* ret_type = func_sym->type->return_type;

            fprintf(stderr, "[HOVER-DEBUG] func '%s' return_type kind=%d\n", func_name, ret_type->kind);
            fflush(stderr);

            // 检查返回类型是否是 clib
            if (ret_type->kind == TYPE_CLIB && ret_type->struct_name) {
                // 构造 "clib <name>" 格式
                char clib_type_str[256];
                snprintf(clib_type_str, sizeof(clib_type_str), "clib %s", ret_type->struct_name);
                result = generate_clib_method_doc(clib_type_str, method_name, content, file_path);
            } else if (ret_type->kind == TYPE_STRUCT && ret_type->struct_name) {
                // struct 返回类型，生成 struct 方法文档
                result = generate_struct_method_doc(ret_type->struct_name, method_name, content, file_path);
                if (!result) {
                    result = generate_struct_method_doc_from_modules(ret_type->struct_name, method_name, content, file_path);
                }
            } else {
                // 其他返回类型，尝试通过 type_to_string 获取类型字符串
                const char* type_str = type_to_string(ret_type);
                fprintf(stderr, "[HOVER-DEBUG] func '%s' return type_str='%s'\n", func_name, type_str ? type_str : "NULL");
                fflush(stderr);
            }
        }
    }
    compiler_context_cleanup(&ctx);

    // 如果编译器途径失败，尝试从模块符号表查找函数返回类型
    if (!result && file_path) {
        int import_count = 0;
        ImportAlias* import_aliases = parse_imports(content, &import_count);
        if (import_aliases && import_count > 0) {
            for (int i = 0; i < import_count && !result; i++) {
                const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                if (!mp) continue;

                module_symbol_table_reset_scan_stack();
                ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                if (!mtable) continue;

                if (module_symbol_table_scan(mtable, file_path) == 0) {
                    // 在模块符号表中查找函数
                    for (int j = 0; j < mtable->func_count; j++) {
                        if (strcmp(mtable->funcs[j].name, func_name) == 0) {
                            // 检查返回类型是否是 clib
                            if (mtable->funcs[j].return_struct_name) {
                                // 尝试 clib
                                char clib_type_str[256];
                                snprintf(clib_type_str, sizeof(clib_type_str), "clib %s", mtable->funcs[j].return_struct_name);
                                result = generate_clib_method_doc(clib_type_str, method_name, content, file_path);
                                if (result) break;

                                // 尝试 struct
                                result = generate_struct_method_doc(mtable->funcs[j].return_struct_name, method_name, content, file_path);
                                if (result) break;
                                result = generate_struct_method_doc_from_modules(mtable->funcs[j].return_struct_name, method_name, content, file_path);
                                if (result) break;
                            }
                        }
                    }
                }
                module_symbol_table_destroy(mtable);
            }
        }
        if (import_aliases) free_import_aliases(import_aliases, import_count);
    }

    free(func_name);
    return result;
}

// 从点号位置向前查找数组索引表达式：var[idx]
// 返回变量名（如 "_texCache"），需要调用者 free
// 如果不是数组索引模式（点号前不是 ']'），返回 NULL
static char* find_array_var_before_dot(const char* content, int dot_offset) {
    if (!content || dot_offset <= 0) return NULL;

    // 从点号向前，跳过空白，应该遇到 ']'
    int pos = dot_offset - 1;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    if (pos < 0 || content[pos] != ']') return NULL;

    // 向前查找匹配的 '['
    int bracket_depth = 1;
    pos--;
    while (pos >= 0 && bracket_depth > 0) {
        if (content[pos] == ']') bracket_depth++;
        else if (content[pos] == '[') bracket_depth--;
        else if (content[pos] == '"' || content[pos] == '\'') {
            // 跳过字符串
            char quote = content[pos];
            pos--;
            while (pos >= 0 && content[pos] != quote) {
                if (pos > 0 && content[pos] == '\\') pos--;
                pos--;
            }
        }
        if (bracket_depth > 0) pos--;
    }
    if (bracket_depth != 0) return NULL;

    // pos 现在指向 '['，向前提取变量名
    pos--;
    while (pos >= 0 && isspace((unsigned char)content[pos])) pos--;
    if (pos < 0) return NULL;

    int name_end = pos + 1;
    while (pos >= 0 && (isalnum((unsigned char)content[pos]) || content[pos] == '_')) {
        pos--;
    }
    int name_start = pos + 1;
    int name_len = name_end - name_start;
    if (name_len <= 0) return NULL;

    char* var_name = (char*)malloc(name_len + 1);
    if (!var_name) return NULL;
    memcpy(var_name, content + name_start, name_len);
    var_name[name_len] = '\0';
    return var_name;
}

// 处理数组索引成员访问的悬停（如 _texCache[i].texture）
// 当 word 以 '.' 开头时，尝试从内容中查找前面的数组索引表达式，
// 解析数组变量的元素类型，然后在该元素类型中查找字段/方法
static char* handle_array_index_member_hover(const char* content, const char* word,
                                              LspPosition pos, const char* file_path) {
    if (!content || !word || word[0] != '.') return NULL;

    const char* member_name = word + 1;
    if (!*member_name) return NULL;

    // 获取光标偏移，找到点号位置
    int cursor_offset = lsp_position_to_offset(content, pos);
    if (cursor_offset < 0) return NULL;

    // 从光标向前查找点号
    int dot_offset = -1;
    int scan = cursor_offset;
    while (scan >= 0) {
        char c = content[scan];
        if (c == '.') { dot_offset = scan; break; }
        if (isalnum((unsigned char)c) || c == '_') { scan--; continue; }
        break;
    }
    if (dot_offset < 0) return NULL;

    // 查找点号前的数组索引变量
    char* var_name = find_array_var_before_dot(content, dot_offset);
    if (!var_name) return NULL;

    fprintf(stderr, "[HOVER-DEBUG] array index member: var='%s' member='%s'\n", var_name, member_name);
    fflush(stderr);

    // 通过编译器解析变量类型
    CompilerContext ctx;
    compiler_context_init(&ctx);
    compiler_analyze_with_filename(&ctx, content, file_path);

    char* result = NULL;
    if (ctx.root_scope) {
        Symbol* var_sym = find_symbol_in_current_function(ctx.root_scope, var_name, content, pos);
        if (!var_sym) {
            var_sym = scope_resolve_tree_bfs(ctx.root_scope, var_name);
        }

        if (!var_sym) {
            fprintf(stderr, "[HOVER-DEBUG] array var '%s' NOT FOUND in scope\n", var_name);
            fflush(stderr);
        } else if (!var_sym->type) {
            fprintf(stderr, "[HOVER-DEBUG] array var '%s' found but type is NULL\n", var_name);
            fflush(stderr);
        }

        if (var_sym && var_sym->type) {
            fprintf(stderr, "[HOVER-DEBUG] array var '%s' type kind=%d\n", var_name, var_sym->type->kind);
            fflush(stderr);

            // 如果是数组类型，获取元素类型
            TypeInfo* elem_type = NULL;
            if (var_sym->type->kind == TYPE_ARRAY && var_sym->type->element_type) {
                elem_type = var_sym->type->element_type;
            } else {
                fprintf(stderr, "[HOVER-DEBUG] array var '%s' type kind=%d (not TYPE_ARRAY or no element_type)\n",
                        var_name, var_sym->type->kind);
                fflush(stderr);
            }

            if (elem_type) {
                fprintf(stderr, "[HOVER-DEBUG] array element type kind=%d struct_name='%s'\n",
                        elem_type->kind, elem_type->struct_name ? elem_type->struct_name : "NULL");
                fflush(stderr);

                if (elem_type->kind == TYPE_STRUCT && elem_type->struct_name) {
                    // struct 元素类型，查找字段
                    char* type_str = NULL;
                    bool found = compiler_get_struct_field_info(&ctx, elem_type->struct_name, member_name, &type_str);
                    if (found && type_str) {
                        size_t info_len = 512 + strlen(member_name) + strlen(type_str) + strlen(elem_type->struct_name);
                        result = (char*)malloc(info_len);
                        if (result) {
                            snprintf(result, info_len, "**%s**\n\n"
                                     "```leno\n"
                                     "%s: %s\n"
                                     "```\n\n"
                                     "%s 字段 (%s)",
                                     member_name, member_name, type_str,
                                     elem_type->struct_name, elem_type->struct_name);
                        }
                        free(type_str);
                    }

                    if (!result) {
                        // 尝试 struct 方法
                        result = generate_struct_method_doc(elem_type->struct_name, member_name, content, file_path);
                    }
                    if (!result) {
                        // 从导入的模块中查找
                        result = generate_struct_method_doc_from_modules(elem_type->struct_name, member_name, content, file_path);
                    }
                } else if (elem_type->kind == TYPE_CLIB && elem_type->struct_name) {
                    // clib 元素类型
                    char clib_type_str[256];
                    snprintf(clib_type_str, sizeof(clib_type_str), "clib %s", elem_type->struct_name);
                    result = generate_clib_method_doc(clib_type_str, member_name, content, file_path);
                }
            }
        }
    }
    compiler_context_cleanup(&ctx);

    // 如果编译器途径失败，尝试从源文件文本中解析变量声明
    if (!result) {
        char* var_type_str = detect_var_type_from_text(content, var_name);
        fprintf(stderr, "[HOVER-DEBUG] detect_var_type_from_text('%s') = '%s'\n",
                var_name, var_type_str ? var_type_str : "NULL");
        fflush(stderr);
        if (var_type_str) {
            // 解析类型字符串，提取 Array[X] 中的 X
            const char* arr_prefix = strstr(var_type_str, "Array[");
            if (arr_prefix) {
                const char* inner_start = arr_prefix + 6;
                const char* inner_end = strchr(inner_start, ']');
                if (inner_end && inner_end > inner_start) {
                    int elem_len = inner_end - inner_start;
                    char elem_name[256];
                    if (elem_len < (int)sizeof(elem_name)) {
                        memcpy(elem_name, inner_start, elem_len);
                        elem_name[elem_len] = '\0';

                        // 在导入的模块中查找 struct 定义
                        if (file_path) {
                            int import_count = 0;
                            ImportAlias* import_aliases = parse_imports(content, &import_count);
                            if (import_aliases && import_count > 0) {
                                for (int i = 0; i < import_count && !result; i++) {
                                    const char* mp = find_module_path_by_alias(import_aliases, import_count, import_aliases[i].alias);
                                    if (!mp) continue;
                                    module_symbol_table_reset_scan_stack();
                                    ModuleSymbolTable* mtable = module_symbol_table_create(mp);
                                    if (!mtable) continue;
                                    if (module_symbol_table_scan(mtable, file_path) == 0) {
                                        ModuleStructSymbol* mst = module_symbol_table_find_struct(mtable, elem_name);
                                        if (mst) {
                                            for (int k = 0; k < mst->field_count && !result; k++) {
                                                if (strcmp(mst->fields[k].name, member_name) == 0) {
                                                    const char* fts = mst->fields[k].struct_name ?
                                                        mst->fields[k].struct_name : type_kind_to_string(mst->fields[k].type);
                                                    size_t info_len = 512 + strlen(member_name) + strlen(fts) + strlen(elem_name);
                                                    result = (char*)malloc(info_len);
                                                    if (result) {
                                                        snprintf(result, info_len, "**%s**\n\n"
                                                                 "```leno\n"
                                                                 "%s: %s\n"
                                                                 "```\n\n"
                                                                 "%s 字段",
                                                                 member_name, member_name, fts, elem_name);
                                                    }
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                    module_symbol_table_destroy(mtable);
                                }
                            }
                            if (import_aliases) free_import_aliases(import_aliases, import_count);
                        }

                        // 也扫描当前文件
                        if (!result && file_path) {
                            module_symbol_table_reset_scan_stack();
                            ModuleSymbolTable* cur_table = module_symbol_table_create(file_path);
                            if (cur_table) {
                                if (module_symbol_table_scan(cur_table, file_path) == 0) {
                                    ModuleStructSymbol* mst = module_symbol_table_find_struct(cur_table, elem_name);
                                    if (mst) {
                                        for (int k = 0; k < mst->field_count && !result; k++) {
                                            if (strcmp(mst->fields[k].name, member_name) == 0) {
                                                const char* fts = mst->fields[k].struct_name ?
                                                    mst->fields[k].struct_name : type_kind_to_string(mst->fields[k].type);
                                                size_t info_len = 512 + strlen(member_name) + strlen(fts) + strlen(elem_name);
                                                result = (char*)malloc(info_len);
                                                if (result) {
                                                    snprintf(result, info_len, "**%s**\n\n"
                                                             "```leno\n"
                                                             "%s: %s\n"
                                                             "```\n\n"
                                                             "%s 字段",
                                                             member_name, member_name, fts, elem_name);
                                                }
                                                break;
                                            }
                                        }
                                    }
                                }
                                module_symbol_table_destroy(cur_table);
                            }
                        }
                    }
                }
            }
            free(var_type_str);
        }
    }

    free(var_name);
    return result;
}
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
    
    // 如果 word 是数字字面量（如 0, 0.0, 123, 3.14 等），不提供悬停提示
    // 这处理浮点数字面量被点号分隔器误拆分的情况（如 0.0 → module=0, method=0）
    {
        bool is_number = true;
        bool has_dot = false;
        for (const char* p = word; *p; p++) {
            if (*p == '.') {
                if (has_dot) { is_number = false; break; }  // 多个点号，不是数字
                has_dot = true;
            } else if (!isdigit((unsigned char)*p)) {
                is_number = false;
                break;
            }
        }
        if (is_number && *word) {
            free(word);
            return NULL;
        }
    }
    
    char* info = NULL;
    
    // 0. 检查是否是导入模块的导出符号 (如 "color_module.Color.red")
    // 这需要在模块方法检查之前，因为模块方法只支持两层（module.method）
    info = get_module_symbol_hover(content, word, file_path);

    // 0.5 检查是否是当前文件中的 enum 值 (如 "Color.red")
    if (!info) {
        info = get_enum_value_hover(content, word, file_path);
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

    // 3.5 处理函数调用链（如 ttfLib().TTF_RenderText_Blended_Wrapped）
    //     当 word 以 '.' 开头时，parse_module_method 会失败
    //     此时尝试从内容中查找前面的函数调用，解析其返回类型
    if (!info && word[0] == '.') {
        info = handle_func_call_chain_hover(content, word, pos, file_path);
    }

    // 3.6 处理数组索引成员访问（如 _texCache[i].texture）
    //     当 word 以 '.' 开头且函数调用链处理失败时，
    //     尝试从内容中查找前面的数组索引表达式，解析元素类型
    if (!info && word[0] == '.') {
        info = handle_array_index_member_hover(content, word, pos, file_path);
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
                // 判断光标在链式表达式中的哪个段
                // 对于 root.children.add，需要确定光标在 root / children / add 哪个段上
                //
                // 策略：从光标向前扫描到最近的点号（不在嵌套括号内），
                //   如果没遇到点号 → 光标在第一个段（变量名部分）
                //   如果遇到点号 → 光标在该点号之后的段中
                //     再从该点号向后扫描到下一个点号或非单词字符，
                //     得到光标所在的段名

                // 向前找最近的点号
                int prev_dot = -1;
                {
                    int scan = offset;
                    int bracket_depth = 0;
                    while (scan >= 0) {
                        char c = content[scan];
                        if (c == ')' || c == ']') bracket_depth++;
                        else if (c == '(' || c == '[') bracket_depth--;
                        else if (c == '.' && bracket_depth == 0) {
                            prev_dot = scan;
                            break;
                        }
                        if (isalnum((unsigned char)c) || c == '_') { scan--; continue; }
                        scan--;
                    }
                }

                // 提取光标所在的段名
                char cursor_segment[128] = {0};
                {
                    int seg_start = offset;
                    int seg_end = offset;
                    // 向前
                    while (seg_start > 0 && (isalnum((unsigned char)content[seg_start-1]) || content[seg_start-1] == '_')) seg_start--;
                    // 向后
                    while (seg_end < (int)strlen(content) && (isalnum((unsigned char)content[seg_end]) || content[seg_end] == '_')) seg_end++;
                    int slen = seg_end - seg_start;
                    if (slen > 0 && slen < 127) {
                        memcpy(cursor_segment, content + seg_start, slen);
                        cursor_segment[slen] = '\0';
                    }
                }

                if (prev_dot < 0) {
                    // 光标在第一个段（变量名部分），显示变量类型
                    char* var_type = get_variable_type_from_compiler(content, module, file_path, pos);
                    if (var_type) {
                        fprintf(stderr, "[HOVER-DEBUG] var='%s' (cursor on var) resolved type='%s'\n", module, var_type);
                        fflush(stderr);
                        size_t info_len = strlen(module) + strlen(var_type) + 256;
                        info = (char*)malloc(info_len);
                        if (info) {
                            snprintf(info, info_len, "**%s**\n\n```leno\n%s: %s\n```\n\n变量",
                                     module, module, var_type);
                        }
                        free(var_type);
                    }
                } else {
                    // 光标在某个点号之后
                    // 判断光标在最后一个段（method 部分）还是中间段（字段部分）
                    // 向后找下一个点号
                    int next_dot = -1;
                    {
                        int scan = offset;
                        int bracket_depth = 0;
                        while (scan < (int)strlen(content)) {
                            char c = content[scan];
                            if (c == '(' || c == '[') bracket_depth++;
                            else if (c == ')' || c == ']') bracket_depth--;
                            else if (c == '.' && bracket_depth == 0) {
                                next_dot = scan;
                                break;
                            }
                            if (isalnum((unsigned char)c) || c == '_') { scan++; continue; }
                            break;
                        }
                    }

                    if (next_dot >= 0) {
                        // 光标在中间段（如 root.children.add 中的 children）
                        // 这是一个字段访问，需要解析字段类型
                        // module = root（第一个段），cursor_segment = children
                        char* var_type = get_variable_type_from_compiler(content, module, file_path, pos);
                        if (var_type) {
                            fprintf(stderr, "[HOVER-DEBUG] chained field: var='%s' type='%s' field='%s'\n",
                                    module, var_type, cursor_segment);
                            fflush(stderr);

                            // 从 var_type 对应的 struct 中查找字段类型
                            CompilerContext fctx;
                            compiler_context_init(&fctx);
                            if (compiler_analyze_with_filename(&fctx, content, file_path) && fctx.root_scope) {
                                char* field_type_str = NULL;
                                if (compiler_get_struct_field_info(&fctx, var_type, cursor_segment, &field_type_str)) {
                                    size_t info_len = 256 + strlen(cursor_segment) + strlen(field_type_str) + strlen(var_type);
                                    info = (char*)malloc(info_len);
                                    if (info) {
                                        snprintf(info, info_len,
                                                 "**%s**\n\n```leno\n%s: %s\n```\n\n%s 字段",
                                                 cursor_segment, cursor_segment, field_type_str, var_type);
                                    }
                                    free(field_type_str);
                                }
                            }
                            compiler_context_cleanup(&fctx);
                            free(var_type);
                        }
                    } else {
                        // 光标在最后一个段（方法部分），正常处理方法悬停
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
                            info = generate_struct_method_doc(var_type, method, content, file_path);
                        }
                        
                        // 如果 struct_def_find 也失败，从导入的模块符号表中查找
                        if (!info) {
                            info = generate_struct_method_doc_from_modules(var_type, method, content, file_path);
                        }
                        
                        // 如果仍然没找到，尝试 clib 方法
                        if (!info) {
                            info = generate_clib_method_doc(var_type, method, content, file_path);
                        }

                        // 4.4b 链式字段访问解析（如 root.children.add）
                        //     method 包含 '.' 时，先解析字段，再在字段类型上找方法
                        if (!info && strchr(method, '.')) {
                            char* field_dot = strchr(method, '.');
                            if (field_dot) {
                                int field_len = field_dot - method;
                                char field_name[128] = {0};
                                if (field_len > 0 && field_len < 127) {
                                    memcpy(field_name, method, field_len);
                                    field_name[field_len] = '\0';
                                    const char* remaining_method = field_dot + 1;

                                    // 从 var_type 对应的 struct 中查找字段类型
                                    CompilerContext fctx;
                                    compiler_context_init(&fctx);
                                    if (compiler_analyze_with_filename(&fctx, content, file_path) && fctx.root_scope) {
                                        char* field_type_str = NULL;
                                        if (compiler_get_struct_field_info(&fctx, var_type, field_name, &field_type_str)) {
                                            // 提取基础类型（如 "Array[HtmlNode]" → "Array"）
                                            char base_type[128] = {0};
                                            const char* bracket = strchr(field_type_str, '[');
                                            int bt_len = bracket ? (int)(bracket - field_type_str) : (int)strlen(field_type_str);
                                            if (bt_len > 0 && bt_len < 127) {
                                                memcpy(base_type, field_type_str, bt_len);
                                                base_type[bt_len] = '\0';
                                            }

                                            if (base_type[0]) {
                                                // 在字段类型上查找方法
                                                int f_arity = native_get_instance_method_arity(base_type, remaining_method);
                                                if (f_arity >= 0) {
                                                    info = generate_instance_method_doc(base_type, remaining_method);
                                                }
                                                if (!info) {
                                                    info = generate_struct_method_doc(base_type, remaining_method, content, file_path);
                                                }
                                                if (!info) {
                                                    info = generate_struct_method_doc_from_modules(base_type, remaining_method, content, file_path);
                                                }
                                            }
                                            free(field_type_str);
                                        }
                                    }
                                    compiler_context_cleanup(&fctx);
                                }
                            }
                        }

                        // 4.5 回退：如果方法查找全部失败但已知变量类型，显示基本成员提示
                        // 这处理 _font.ok 这种字段访问（ok 是 Font 的字段而非方法）
                        if (!info) {
                            size_t fallback_len = 256 + strlen(module) + strlen(method) + strlen(var_type);
                            info = (char*)malloc(fallback_len);
                            if (info) {
                                snprintf(info, fallback_len, 
                                         "**%s.%s**\n\n```leno\n// %s 类型的成员\n```\n\n`%s` 是 `%s` 类型变量的成员",
                                         module, method, var_type, method, var_type);
                            }
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
                    } // end else (光标在最后一个段)
                } // end else (prev_dot >= 0)
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

    // 6. 尝试附加定义来源（文件名 + 行号）
    //    即使 info 为 NULL，如果能找到定义，也生成基本的悬停信息
    if (file_path) {
        // 直接使用 lsp_get_definition，它与跳转(Go to Definition)使用相同的逻辑
        // 能正确处理模块前缀（如 fnt.ttfQuit → 在 fnt 模块中查找 ttfQuit）
        char* current_uri = lsp_path_to_uri(file_path);
        int def_count = 0;
        LspLocation* locations = lsp_get_definition(content, pos, &def_count, current_uri);
        if (current_uri) free(current_uri);

        if (locations && def_count > 0) {
            LspLocation* loc = &locations[0];
            int def_line = (int)loc->range.start.line + 1;

            // 从 URI 中提取文件名
            const char* short_name = loc->uri;
            if (strncmp(short_name, "file:///", 8) == 0) short_name += 8;
            const char* slash = strrchr(short_name, '/');
            const char* backslash = strrchr(short_name, '\\');
            const char* last_sep = slash > backslash ? slash : backslash;
            if (last_sep) short_name = last_sep + 1;

            // 判断是否是当前文件
            char* cur_uri = lsp_path_to_uri(file_path);
            bool is_current_file = (cur_uri && strcmp(loc->uri, cur_uri) == 0);
            if (cur_uri) free(cur_uri);

            // 确定用于显示的 word
            // 如果光标在变量部分（点号前），使用变量名而非方法名
            const char* def_word = word;
            char* short_word = NULL;
            const char* dot = strrchr(word, '.');
            if (dot && *(dot + 1)) {
                // 检查光标是否在点号之前（变量部分）
                int cur_off = lsp_position_to_offset(content, pos);
                bool cursor_in_var_part = true;
                if (cur_off >= 0) {
                    int scan = cur_off;
                    while (scan >= 0) {
                        char c = content[scan];
                        if (c == '.') {
                            cursor_in_var_part = false;
                            break;
                        }
                        if (isalnum((unsigned char)c) || c == '_') {
                            scan--;
                            continue;
                        }
                        break;
                    }
                }
                if (cursor_in_var_part) {
                    // 光标在变量部分，使用点号前的变量名
                    int base_len = dot - word;
                    short_word = (char*)malloc(base_len + 1);
                    if (short_word) {
                        memcpy(short_word, word, base_len);
                        short_word[base_len] = '\0';
                        def_word = short_word;
                    }
                } else {
                    short_word = strdup(dot + 1);
                    def_word = short_word;
                }
            }

            // 如果 info 为 NULL，生成基本悬停信息
            if (!info) {
                size_t base_len = strlen(def_word) + 256;
                info = (char*)malloc(base_len);
                if (info) {
                    snprintf(info, base_len, "**%s**\n\n```leno\n%s\n```", def_word, def_word);
                }
            }

            // 追加来源信息到 hover 文本
            if (info) {
                size_t old_len = strlen(info);
                size_t add_len = 256 + strlen(short_name);
                char* new_info = (char*)malloc(old_len + add_len);
                if (new_info) {
                    memcpy(new_info, info, old_len);
                    if (is_current_file) {
                        snprintf(new_info + old_len, add_len, "\n\n---\n%s:%d", short_name, def_line);
                    } else {
                        snprintf(new_info + old_len, add_len, "\n\n---\n%s:%d（导入模块）", short_name, def_line);
                    }
                    free(info);
                    info = new_info;
                }
            }
            if (short_word) free(short_word);
        }
        if (locations) lsp_free_locations(locations, def_count);
    }

    free(word);

    clock_t t_end = clock();
    fprintf(stderr, "[HOVER] END found=%s time=%.0fms\n",
            info ? "yes" : "no", (double)(t_end - t_start) * 1000.0 / CLOCKS_PER_SEC);
    fflush(stderr);
    
    return info;
}
