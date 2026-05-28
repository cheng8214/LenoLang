/**
 * 悬停提示服务
 * 提供鼠标悬停时的类型信息 - 使用 LenoC 编译器
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"
#include "leno_builtins.h"
#include "../src/include/native.h"
#include <ctype.h>
#include <string.h>

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

    fprintf(stderr, "[LSP DEBUG] find_struct_name_at_position: offset=%d, line=%d\n", offset, pos.line);

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

            fprintf(stderr, "[LSP DEBUG] Found 'struct' keyword at offset %d\n", (int)(p - content));

            // 找到 struct 后，解析 struct 名称
            // 格式应该是: struct Name { 或 export struct Name {
            const char* name_start = p + 6;

            // 跳过空白字符
            while (*name_start && isspace((unsigned char)*name_start)) {
                name_start++;
            }

            fprintf(stderr, "[LSP DEBUG] After 'struct', name_start='%c'\n", *name_start);

            // 现在 name_start 应该指向 struct 名称
            const char* name_end = name_start;
            while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_')) {
                name_end++;
            }

            int name_len = name_end - name_start;
            fprintf(stderr, "[LSP DEBUG] name_len=%d\n", name_len);

            if (name_len > 0) {
                // 检查这个 struct 定义是否包含光标位置（通过查找匹配的 }）
                // 简单检查：从 name_end 开始查找 {
                const char* brace = name_end;
                while (*brace && *brace != '{' && *brace != '\n') {
                    brace++;
                }

                fprintf(stderr, "[LSP DEBUG] brace char='%c', brace_offset=%d, cursor_offset=%d\n",
                        *brace, (int)(brace - content), offset);

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
                            fprintf(stderr, "[LSP DEBUG] Found struct '%s' at position line=%d\n",
                                    struct_name, pos.line);
                            return struct_name;
                        }
                    } else {
                        fprintf(stderr, "[LSP DEBUG] Cursor is outside struct block, continuing search\n");
                    }
                }
            }
        }
    }

    fprintf(stderr, "[LSP DEBUG] No struct found at position\n");
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
            while (*brace && *brace != '{') brace++;

            if (*brace == '{') {
                // 计算函数范围
                int func_start = def_start - content;
                int func_end = -1;

                int brace_count = 1;
                const char* p = brace + 1;
                while (*p && brace_count > 0) {
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

// 从编译器获取符号悬停信息
static char* get_symbol_hover_from_compiler(const char* content, const char* word, LspPosition pos, const char* file_path) {
    if (!content || !word) return NULL;

    CompilerContext ctx;
    compiler_context_init(&ctx);

    // 编译分析（即使失败也可能有符号表）
    compiler_analyze_with_filename(&ctx, content, file_path);

    // 检查是否有符号表
    if (!ctx.root_scope) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }

    // 首先尝试根据光标位置确定所在的 struct（优先处理 struct 字段）
    char* current_struct_name = find_struct_name_at_position(content, pos);

    if (current_struct_name) {
        // 如果确定了当前 struct，检查该字段是否在其中
        char* type_str = NULL;
        fprintf(stderr, "[LSP DEBUG] Current struct at cursor: '%s', looking for field '%s'\n",
                current_struct_name, word);
        bool found = compiler_get_struct_field_info(&ctx, current_struct_name, word, &type_str);
        if (found) {
            fprintf(stderr, "[LSP DEBUG] Found field '%s' in struct '%s' with type '%s'\n",
                    word, current_struct_name, type_str ? type_str : "unknown");
            // 构建悬停信息（使用固定大小缓冲区避免警告）
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
        fprintf(stderr, "[LSP DEBUG] Field '%s' not found in struct '%s'\n", word, current_struct_name);
    }

    // 如果不在 struct 定义中，尝试在作用域树中查找符号
    // 优先查找当前函数作用域中的符号，避免其他函数的参数干扰
    Symbol* sym = find_symbol_in_current_function(ctx.root_scope, word, content, pos);
    if (sym) {
        fprintf(stderr, "[LSP DEBUG] Found symbol '%s' in current function, kind=%d, type_kind=%d\n",
                word, sym->kind, sym->type ? (int)sym->type->kind : -1);
    }

    // 如果没找到，尝试在整个作用域树中查找（使用 BFS）
    if (!sym) {
        fprintf(stderr, "[LSP DEBUG] Trying to find symbol '%s' in entire scope tree\n", word);
        sym = scope_resolve_tree_bfs(ctx.root_scope, word);
        if (sym) {
            fprintf(stderr, "[LSP DEBUG] Found symbol '%s' in scope tree, kind=%d, type_kind=%d\n",
                    word, sym->kind, sym->type ? (int)sym->type->kind : -1);
        }
    }

    // 如果没找到，尝试查找所有包含该字段的 struct
    if (!sym) {
        char** struct_names = NULL;
        int struct_count = compiler_find_structs_with_field(&ctx, word, &struct_names);

        if (struct_count > 0) {
            // 使用第一个找到的 struct
            fprintf(stderr, "[LSP DEBUG] Found %d structs with field '%s', using first: '%s'\n",
                    struct_count, word, struct_names[0]);
            char* type_str = NULL;
            bool found = compiler_get_struct_field_info(&ctx, struct_names[0], word, &type_str);

            // 释放 struct 名称列表
            for (int i = 0; i < struct_count; i++) {
                free(struct_names[i]);
            }
            free(struct_names);

            if (found && type_str) {
                int info_len = 512 + strlen(word) + strlen(type_str) + strlen(struct_names[0]);
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
        compiler_context_cleanup(&ctx);
        return NULL;
    }

    // 构建悬停信息
    const char* type_str = type_to_string(sym->type);
    fprintf(stderr, "[LSP DEBUG] sym->type kind=%d, type_str='%s'\n",
            sym->type ? (int)sym->type->kind : -1, type_str ? type_str : "null");
    int info_len = 512 + strlen(word) + (type_str ? strlen(type_str) : 0);
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
        // 其他符号
        snprintf(info, info_len, "**%s**\n\n"
                 "```leno\n"
                 "%s: %s\n"
                 "```\n\n"
                 "%s%s",
                 word,
                 word,
                 type_str ? type_str : "unknown",
                 is_global ? "全局" : "局部",
                 " 符号");
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

    fprintf(stderr, "[LSP DEBUG] get_module_symbol_hover: word='%s', current_file='%s'\n",
            word, current_file ? current_file : "NULL");

    // 解析模块路径
    char* segments[4];
    int segment_count = parse_module_path(word, segments, 4);
    fprintf(stderr, "[LSP DEBUG] parse_module_path returned: %d segments\n", segment_count);
    for (int i = 0; i < segment_count; i++) {
        fprintf(stderr, "[LSP DEBUG] segment[%d]='%s'\n", i, segments[i]);
    }
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
                while (*p && *p != '"' && path_len < 255) {
                    import_path[path_len++] = *p++;
                }
                if (*p == '"') p++;

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
                        module_path = strdup(import_path);
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
                        module_path = strdup(import_path);
                        break;
                    }
                }
            }
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }

    if (!module_path) {
        fprintf(stderr, "[LSP DEBUG] module_path not found for alias '%s'\n", segments[0]);
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    fprintf(stderr, "[LSP DEBUG] Found module_path='%s'\n", module_path);

    // 读取模块文件内容
    extern char* read_module_file(const char* file_path, const char* current_file);
    char* module_source = read_module_file(module_path, current_file);
    free(module_path);

    if (!module_source) {
        fprintf(stderr, "[LSP DEBUG] Failed to read module file\n");
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    // 使用编译器分析模块
    CompilerContext ctx;
    compiler_context_init(&ctx);
    bool analyzed = compiler_analyze_with_filename(&ctx, module_source, current_file);
    free(module_source);

    if (!analyzed || !ctx.root_scope) {
        fprintf(stderr, "[LSP DEBUG] Failed to analyze module\n");
        compiler_context_cleanup(&ctx);
        for (int i = 0; i < segment_count; i++) free(segments[i]);
        return NULL;
    }

    fprintf(stderr, "[LSP DEBUG] Module analyzed successfully\n");

    char* result = NULL;

    if (segment_count == 2) {
        // module.symbol - 可能是函数、struct、enum、变量
        const char* symbol_name = segments[1];

        // 从编译器符号表中查找
        Symbol* sym = scope_resolve_tree_bfs(ctx.root_scope, symbol_name);
        if (sym) {
            fprintf(stderr, "[LSP DEBUG] Found symbol '%s' in module, kind=%d\n", symbol_name, sym->kind);
            const char* type_str = type_to_string(sym->type);

            switch (sym->kind) {
                case SYM_GLOBAL_FUNC: {
                    int len = 512 + strlen(word) + strlen(type_str);
                    result = (char*)malloc(len);
                    if (result) {
                        snprintf(result, len, "**%s**\n\n"
                                 "```leno\n"
                                 "%s() -> %s\n"
                                 "```\n\n"
                                 "模块函数",
                                 word, word, type_str);
                    }
                    break;
                }
                case SYM_TYPE: {
                    fprintf(stderr, "[LSP DEBUG] SYM_TYPE: symbol_name='%s', sym->type=%p\n", symbol_name, (void*)sym->type);
                    if (sym->type && sym->type->kind == TYPE_STRUCT) {
                        fprintf(stderr, "[LSP DEBUG] It's a struct\n");
                        int len = 512 + strlen(word);
                        result = (char*)malloc(len);
                        if (result) {
                            snprintf(result, len, "**%s**\n\n"
                                 "```leno\n"
                                 "struct %s\n"
                                 "```\n\n"
                                 "模块导出的 struct",
                                 word, symbol_name);
                        }
                    } else {
                        fprintf(stderr, "[LSP DEBUG] It's an enum or other type\n");
                        int len = 512 + strlen(word);
                        result = (char*)malloc(len);
                        if (result) {
                            snprintf(result, len, "**%s**\n\n"
                                 "```leno\n"
                                 "enum %s\n"
                                 "```\n\n"
                                 "模块导出的 enum",
                                 word, symbol_name);
                        }
                    }
                    fprintf(stderr, "[LSP DEBUG] SYM_TYPE case done\n");
                    break;
                }
                case SYM_GLOBAL:
                case SYM_LOCAL:
                case SYM_MODULE: {
                    int len = 512 + strlen(word) + strlen(type_str);
                    result = (char*)malloc(len);
                    if (result) {
                        snprintf(result, len, "**%s**\n\n"
                                 "```leno\n"
                                 "%s: %s\n"
                                 "```\n\n"
                                 "模块导出的变量",
                                 word, word, type_str);
                    }
                    break;
                }
                default:
                    break;
            }
        } else {
            fprintf(stderr, "[LSP DEBUG] Symbol '%s' not found in module\n", symbol_name);
        }
    } else if (segment_count == 3) {
        // module.Enum.value 或 module.Struct.field
        const char* type_name = segments[1];
        const char* member_name = segments[2];

        // 查找 enum
        Symbol* enum_sym = scope_resolve_tree_bfs(ctx.root_scope, type_name);
        if (enum_sym && enum_sym->kind == SYM_TYPE && enum_sym->type && enum_sym->type->kind == TYPE_ENUM) {
            // 是 enum，显示 enum 值信息
            int len = 512 + strlen(word);
            result = (char*)malloc(len);
            if (result) {
                snprintf(result, len, "**%s**\n\n"
                         "```leno\n"
                         "%s.%s.%s\n"
                         "```\n\n"
                         "%s enum 值 (%s)",
                         word, segments[0], type_name, member_name,
                         type_name, type_name);
            }
        }

        // 查找 struct 字段
        if (!result) {
            Symbol* struct_sym = scope_resolve_tree_bfs(ctx.root_scope, type_name);
            if (struct_sym && struct_sym->kind == SYM_TYPE && struct_sym->type && struct_sym->type->kind == TYPE_STRUCT) {
                fprintf(stderr, "[LSP DEBUG] Found struct '%s', looking for field '%s'\n", type_name, member_name);
                // 在 struct 的字段中查找（使用 Symbol 中的字段信息）
                if (struct_sym->struct_field_names && struct_sym->struct_field_types) {
                    for (int i = 0; i < struct_sym->struct_field_count; i++) {
                        if (strcmp(struct_sym->struct_field_names[i], member_name) == 0) {
                            const char* field_type_str = type_to_string(struct_sym->struct_field_types[i]);
                            int len = 512 + strlen(word) + strlen(field_type_str);
                            result = (char*)malloc(len);
                            if (result) {
                                snprintf(result, len, "**%s**\n\n"
                                         "```leno\n"
                                         "%s: %s\n"
                                         "```\n\n"
                                         "%s 字段 (%s)",
                                         word, word, field_type_str,
                                         type_name, type_name);
                            }
                            fprintf(stderr, "[LSP DEBUG] Found field '%s' with type '%s'\n", member_name, field_type_str);
                            break;
                        }
                    }
                }
                if (!result) {
                    fprintf(stderr, "[LSP DEBUG] Field '%s' not found in struct '%s'\n", member_name, type_name);
                }
            } else {
                fprintf(stderr, "[LSP DEBUG] Struct '%s' not found in module\n", type_name);
            }
        }
    }

    compiler_context_cleanup(&ctx);
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
                        int info_len = 512 + strlen(word) + strlen(enum_name);
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

// 根据变量名获取其类型（使用编译器分析）
static const char* get_variable_type_from_compiler(const char* content, const char* var_name, const char* file_path) {
    if (!content || !var_name) return NULL;
    
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
    bool is_global = false;
    
    if (!compiler_get_symbol_info(&ctx, var_name, &type_str, &is_global)) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }
    
    // 解析类型字符串，提取基本类型
    const char* result = NULL;
    char* struct_result = NULL;
    if (type_str) {
        // 检查类型字符串是否包含特定类型
        if (strstr(type_str, "string") != NULL) {
            result = "string";
        } else if (strstr(type_str, "Array") != NULL || strstr(type_str, "array") != NULL) {
            result = "array";
        } else if (strstr(type_str, "Dict") != NULL || strstr(type_str, "dict") != NULL) {
            result = "dict";
        } else if (strstr(type_str, "struct") != NULL) {
            // 对于 struct 类型，提取 struct 名称
            const char* struct_ptr = strstr(type_str, "struct");
            if (struct_ptr) {
                struct_ptr += 6; // 跳过 "struct"
                while (*struct_ptr && isspace((unsigned char)*struct_ptr)) struct_ptr++;
                if (*struct_ptr) {
                    struct_result = strdup(struct_ptr);
                }
            }
        }
        free(type_str);
    }
    
    // 如果是 struct 类型，返回 struct 名称（需要调用者释放）
    if (struct_result) {
        compiler_context_cleanup(&ctx);
        return struct_result;
    }
    
    compiler_context_cleanup(&ctx);
    return result;
}

// 检查是否是实例方法名（如 add, insert, len 等）
// 如果是，返回对应的类型名（如 "array", "string", "dict" 等）
static const char* is_instance_method(const char* method_name) {
    // 检查数组方法
    int count;
    char** methods = native_get_instance_methods("array", &count);
    if (methods) {
        for (int i = 0; i < count; i++) {
            if (strcmp(methods[i], method_name) == 0) {
                native_free_instance_method_list(methods, count);
                return "array";
            }
        }
        native_free_instance_method_list(methods, count);
    }
    
    // 检查字符串方法
    methods = native_get_instance_methods("string", &count);
    if (methods) {
        for (int i = 0; i < count; i++) {
            if (strcmp(methods[i], method_name) == 0) {
                native_free_instance_method_list(methods, count);
                return "string";
            }
        }
        native_free_instance_method_list(methods, count);
    }
    
    // 检查字典方法
    methods = native_get_instance_methods("dict", &count);
    if (methods) {
        for (int i = 0; i < count; i++) {
            if (strcmp(methods[i], method_name) == 0) {
                native_free_instance_method_list(methods, count);
                return "dict";
            }
        }
        native_free_instance_method_list(methods, count);
    }
    
    return NULL;
}

// 生成实例方法文档
static char* generate_instance_method_doc(const char* type_name, const char* method_name) {
    int arity = native_get_instance_method_arity(type_name, method_name);
    if (arity < 0) return NULL;
    
    TypeKind return_type = native_get_instance_method_return_type(type_name, method_name, NULL);
    const char* return_type_str = type_kind_to_string(return_type);
    
    int len = 512 + strlen(type_name) + strlen(method_name);
    char* info = (char*)malloc(len);
    if (!info) return NULL;
    
    if (arity == 0) {
        snprintf(info, len, "**%s.%s()**\n\n```leno\n%s.%s() -> %s\n```\n\n%s 类型的实例方法",
                 type_name, method_name, type_name, method_name, return_type_str, type_name);
    } else if (arity < 0) {
        snprintf(info, len, "**%s.%s(...)**\n\n```leno\n%s.%s(...) -> %s\n```\n\n%s 类型的实例方法（可变参数）",
                 type_name, method_name, type_name, method_name, return_type_str, type_name);
    } else {
        snprintf(info, len, "**%s.%s(...)**\n\n```leno\n%s.%s(...) -> %s\n```\n\n%s 类型的实例方法（%d 个参数）",
                 type_name, method_name, type_name, method_name, return_type_str, type_name, arity);
    }
    
    return info;
}

// 获取悬停信息
char* lsp_get_hover_info(const char* content, LspPosition pos, const char* file_path) {
    if (!content) return NULL;
    
    char* word = get_word_at_position(content, pos);
    if (!word) return NULL;
    
    // 调试输出
    fprintf(stderr, "[LSP HOVER DEBUG] pos.line=%d, pos.character=%d, word='%s'\n", 
            pos.line, pos.character, word);
    
    char* info = NULL;
    
    // 0. 检查是否是导入模块的导出符号 (如 "color_module.Color.red")
    // 这需要在模块方法检查之前，因为模块方法只支持两层（module.method）
    info = get_module_symbol_hover(content, word, file_path);

    // 0.5 检查是否是当前文件中的 enum 值 (如 "Color.red")
    if (!info) {
        info = get_enum_value_hover(content, word);
    }

    // 1. 检查是否是模块方法调用 (如 "io.print") 或实例方法调用 (如 "s.len")
    char* module = NULL;
    char* method = NULL;
    if (!info && parse_module_method(word, &module, &method)) {
        // 首先尝试作为模块方法
        int arity = native_get_module_method_arity(module, method);
        if (arity >= 0) {
            // 模块方法存在，生成文档
            TypeKind return_type = native_get_module_method_return_type(module, method);
            const char* return_type_str = type_kind_to_string(return_type);

            int len = 256 + strlen(module) + strlen(method);
            info = (char*)malloc(len);
            if (info) {
                if (arity == 0) {
                    snprintf(info, len, "**%s.%s**\n\n```leno\n%s.%s() -> %s\n```",
                             module, method, module, method, return_type_str);
                } else if (arity < 0) {
                    snprintf(info, len, "**%s.%s**\n\n```leno\n%s.%s(...) -> %s\n```",
                             module, method, module, method, return_type_str);
                } else {
                    snprintf(info, len, "**%s.%s**\n\n```leno\n%s.%s(...) -> %s\n```",
                             module, method, module, method, return_type_str);
                }
            }
        } else {
            // 不是模块方法，尝试作为实例方法
            int offset = lsp_position_to_offset(content, pos);
            if (!is_inside_string_literal(content, offset) && !is_inside_comment(content, offset)) {
                // 首先尝试使用编译器确定变量类型
                char* var_type = get_variable_type_from_compiler(content, module, file_path);
                if (var_type) {
                    // 检查该类型是否有这个方法
                    int arity = native_get_instance_method_arity(var_type, method);
                    if (arity >= 0) {
                        info = generate_instance_method_doc(var_type, method);
                    }
                    // 释放类型字符串（对于 struct 类型是动态分配的）
                    // 注意：对于基本类型（string, array, dict），var_type 是常量，不需要释放
                    // 但对于 struct 类型，var_type 是动态分配的，需要释放
                    // 这里我们通过检查 var_type 是否在常量字符串池中来判断
                    if (var_type != "string" && var_type != "array" && var_type != "dict") {
                        free(var_type);
                    }
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
    
    // 2. 尝试获取关键字文档
    if (!info) {
        info = get_keyword_doc(word);
    }
    
    // 3. 尝试获取内置函数文档
    if (!info) {
        const BuiltinFunctionMeta* builtin = find_builtin_function(word);
        if (builtin) {
            info = generate_builtin_doc(builtin);
        }
    }
    
    // 4. 检查是否是实例方法名（如 add, insert, len 等）
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
    
    // 5. 如果不是关键字、内置函数或实例方法，尝试从编译器获取符号信息
    if (!info) {
        info = get_symbol_hover_from_compiler(content, word, pos, file_path);
    }

    free(word);
    return info;
}
