/**
 * 定义跳转服务
 * 提供跳转到定义位置的功能（支持跨文件跳转）
 */

#include "leno_lsp.h"
#include "../src/include/module_symbol_table.h"
#include "../src/include/module_loader.h"
#include "../src/include/leno_types.h"
#include <ctype.h>

// 获取光标下的单词（复用 lsp_hover.c 中的函数）
extern char* get_word_at_position(const char* content, LspPosition pos);

// 读取模块文件内容（来自 module_loader.c）
extern char* read_module_file(const char* file_path, const char* current_file);

// 最大导入模块数
#define MAX_IMPORTS 32

// 导入条目
typedef struct {
    char alias[64];
    char file_path[MAX_PATH_LEN];
} ImportEntry;

// 处理定义跳转请求
char* lsp_handle_definition(LspServer* server, int id, JsonValue* params) {
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
    
    // 获取定义位置
    int count = 0;
    LspLocation* locations = lsp_get_definition(doc->content, pos, &count, json_string_value(uri));
    
    // 构建响应
    JsonValue* result = NULL;
    
    if (count == 1) {
        // 单个位置
        result = json_object_new();
        json_object_set(result, "uri", json_string_new(locations[0].uri));
        
        JsonValue* range = json_object_new();
        JsonValue* start = json_object_new();
        JsonValue* end = json_object_new();
        json_object_set(start, "line", json_int_new(locations[0].range.start.line));
        json_object_set(start, "character", json_int_new(locations[0].range.start.character));
        json_object_set(end, "line", json_int_new(locations[0].range.end.line));
        json_object_set(end, "character", json_int_new(locations[0].range.end.character));
        json_object_set(range, "start", start);
        json_object_set(range, "end", end);
        
        json_object_set(result, "range", range);
    }
    else if (count > 1) {
        // 多个位置
        result = json_array_new();
        for (int i = 0; i < count; i++) {
            JsonValue* loc = json_object_new();
            json_object_set(loc, "uri", json_string_new(locations[i].uri));
            
            JsonValue* range = json_object_new();
            JsonValue* start = json_object_new();
            JsonValue* end = json_object_new();
            json_object_set(start, "line", json_int_new(locations[i].range.start.line));
            json_object_set(start, "character", json_int_new(locations[i].range.start.character));
            json_object_set(end, "line", json_int_new(locations[i].range.end.line));
            json_object_set(end, "character", json_int_new(locations[i].range.end.character));
            json_object_set(range, "start", start);
            json_object_set(range, "end", end);
            
            json_object_set(loc, "range", range);
            json_array_add(result, loc);
        }
    }
    
    char* response = lsp_create_response(id, result);
    
    if (result) {
        json_free(result);
    }
    
    lsp_free_locations(locations, count);
    
    return response;
}

/**
 * 从源代码中提取 import 语句
 * 支持以下形式:
 *   import "path.leno" as alias     — 字符串路径带别名
 *   import "path.leno"              — 字符串路径无别名（自动从文件名提取）
 *   import "module_name"            — 字符串模块名（通过包搜索路径解析）
 *   import module_name              — 标识符形式（通过包搜索路径解析）
 *   import module_name as alias     — 标识符带别名
 */
static int extract_imports_from_content(const char* content, ImportEntry* imports, int max_imports) {
    int count = 0;
    if (!content) return 0;

    // 声明 package_resolve_module_file（在 package_resolve.c 中实现）
    extern int package_resolve_module_file(const char* module_name, char* out_path, int out_len);

    const char* p = content;
    while (*p && count < max_imports) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;

        // 跳过注释行
        if (*p == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        if (strncmp(p, "import", 6) != 0 || (p[6] && isalnum((unsigned char)p[6]))) {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        p += 6;
        while (*p && isspace((unsigned char)*p)) p++;

        if (*p == '"') {
            // 字符串形式: import "path.leno" 或 import "module_name"
            p++;
            const char* path_start = p;
            while (*p && *p != '"') p++;
            if (!*p) break;

            int path_len = (int)(p - path_start);
            if (path_len <= 0 || path_len >= MAX_PATH_LEN) {
                p++;
                while (*p && *p != '\n') p++;
                if (*p) p++;
                continue;
            }
            memcpy(imports[count].file_path, path_start, path_len);
            imports[count].file_path[path_len] = '\0';
            p++;

            // 如果路径不含 .leno，尝试通过包搜索路径解析
            if (strstr(imports[count].file_path, ".leno") == NULL) {
                char resolved[MAX_PATH_LEN];
                if (package_resolve_module_file(imports[count].file_path, resolved, sizeof(resolved)) == 1) {
                    strncpy(imports[count].file_path, resolved, MAX_PATH_LEN - 1);
                    imports[count].file_path[MAX_PATH_LEN - 1] = '\0';
                }
            }
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            // 标识符形式: import module_name 或 import module_name as alias
            const char* name_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int name_len = (int)(p - name_start);
            if (name_len <= 0 || name_len >= MAX_PATH_LEN) {
                while (*p && *p != '\n') p++;
                if (*p) p++;
                continue;
            }

            // 标识符形式导入：先当作模块名，尝试通过包搜索路径解析为文件路径
            char module_name[MAX_PATH_LEN];
            memcpy(module_name, name_start, name_len);
            module_name[name_len] = '\0';

            char resolved[MAX_PATH_LEN];
            if (package_resolve_module_file(module_name, resolved, sizeof(resolved)) == 1) {
                strncpy(imports[count].file_path, resolved, MAX_PATH_LEN - 1);
                imports[count].file_path[MAX_PATH_LEN - 1] = '\0';
            } else {
                // 无法解析为文件路径，跳过（可能是内置模块如 io, maths 等）
                while (*p && *p != '\n') p++;
                if (*p) p++;
                continue;
            }
        } else {
            // 不是字符串也不是标识符，跳过
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

        // 跳过空白，检查 "as alias"
        while (*p && isspace((unsigned char)*p)) p++;
        if (strncmp(p, "as", 2) == 0 && (!p[2] || isspace((unsigned char)p[2]) || p[2] == '\n')) {
            p += 2;
            while (*p && isspace((unsigned char)*p)) p++;
            const char* alias_start = p;
            while (*p && (isalnum((unsigned char)*p) || *p == '_')) p++;
            int alias_len = (int)(p - alias_start);
            if (alias_len > 0 && alias_len < 63) {
                memcpy(imports[count].alias, alias_start, alias_len);
                imports[count].alias[alias_len] = '\0';
            } else {
                imports[count].alias[0] = '\0';
            }
        } else {
            imports[count].alias[0] = '\0';
        }

        // 没有别名则从路径提取
        if (imports[count].alias[0] == '\0') {
            const char* base = strrchr(imports[count].file_path, '/');
            if (!base) base = strrchr(imports[count].file_path, '\\');
            if (base) base++;
            else base = imports[count].file_path;
            const char* dot = strrchr(base, '.');
            if (dot && dot > base) {
                int name_len = (int)(dot - base);
                if (name_len >= 63) name_len = 62;
                memcpy(imports[count].alias, base, name_len);
                imports[count].alias[name_len] = '\0';
            } else {
                strncpy(imports[count].alias, base, 63);
                imports[count].alias[63] = '\0';
            }
        }

        count++;
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }

    return count;
}

/**
 * 解析导入路径为绝对路径（复刻 read_module_file 的逻辑）
 */
static bool resolve_import_path(const char* file_path, const char* current_file,
                                char* out_full_path, int out_len) {
    if (!file_path || !out_full_path || out_len <= 0) return false;

    // 绝对路径直接返回
    if (file_path[0] == '/' || file_path[0] == '\\' ||
        (file_path[1] == ':' && (file_path[2] == '/' || file_path[2] == '\\'))) {
        strncpy(out_full_path, file_path, out_len - 1);
        out_full_path[out_len - 1] = '\0';
        return true;
    }

    if (!current_file) {
        strncpy(out_full_path, file_path, out_len - 1);
        out_full_path[out_len - 1] = '\0';
        return true;
    }

    char normalized_current[MAX_PATH_LEN];
    strncpy(normalized_current, current_file, MAX_PATH_LEN - 1);
    normalized_current[MAX_PATH_LEN - 1] = '\0';
#ifdef _WIN32
    for (int i = 0; normalized_current[i]; i++)
        if (normalized_current[i] == '/') normalized_current[i] = '\\';
    const char* last_slash = strrchr(normalized_current, '\\');
#else
    const char* last_slash = strrchr(normalized_current, '/');
#endif

    if (last_slash) {
        size_t dir_len = last_slash - normalized_current + 1;
        if (dir_len + strlen(file_path) >= (size_t)out_len) return false;
        memcpy(out_full_path, normalized_current, dir_len);
        out_full_path[dir_len] = '\0';
        strcat(out_full_path, file_path);
    } else {
        if (strlen(file_path) >= (size_t)out_len) return false;
        strcpy(out_full_path, file_path);
    }

    return true;
}

// 前向声明
static bool find_definition_in_content(const char* content, const char* word,
LspRange* range);
static bool find_struct_field_definition(const char* content, const char* word,
                                          LspRange* range);

/**
 * 在导入的模块中查找符号定义
 * 1. 解析模块路径 → 2. 建符号表 → 3. 查找 → 4. 读文件定位 → 5. 返回位置
 */
static bool find_definition_in_module(const char* module_path, const char* current_file,
                                      const char* word, LspRange* out_range,
                                      char* out_full_path, int full_path_len) {
    if (!module_path || !word || !out_range || !out_full_path) return false;

    char full_path[MAX_PATH_LEN];
    if (!resolve_import_path(module_path, current_file, full_path, sizeof(full_path)))
        return false;

    // 重置模块扫描栈（防止 LSP 多次请求间残留导致误报循环依赖）
    module_symbol_table_reset_scan_stack();

    ModuleSymbolTable* sym_table = module_symbol_table_create(full_path);
    if (!sym_table) return false;

    if (module_symbol_table_scan(sym_table, current_file) != 0) {
        module_symbol_table_destroy(sym_table);
        return false;
    }

    bool found = false;
    if (module_symbol_table_find_func(sym_table, word)) found = true;
    else if (module_symbol_table_find_struct(sym_table, word)) found = true;
    else if (module_symbol_table_find_enum(sym_table, word)) found = true;
    else if (module_symbol_table_find_face(sym_table, word)) found = true;
    else if (module_symbol_table_find_var(sym_table, word)) found = true;
    // 也查找 clib 函数定义（如 TTF_RenderText_Blended_Wrapped）
    if (!found) {
        for (int i = 0; i < sym_table->clib_count && !found; i++) {
            for (int j = 0; j < sym_table->clibs[i].func_count; j++) {
                if (strcmp(sym_table->clibs[i].funcs[j].name, word) == 0) {
                    found = true;
                    break;
                }
            }
        }
    }
    // 也查找所有 struct 的方法（如 MenuBar::setContextItemEnabled）
    if (!found) {
        for (int i = 0; i < sym_table->struct_count && !found; i++) {
            if (module_symbol_table_find_struct_method(sym_table,
                    sym_table->structs[i].name, word)) {
                found = true;
            }
        }
    }

    module_symbol_table_destroy(sym_table);
    // 符号表查找失败时，仍然尝试读文件定位（方法定义在 struct 内部，符号表可能未收录）

    char* module_content = read_module_file(module_path, current_file);
    if (!module_content) return false;

    bool found_def = find_definition_in_content(module_content, word, out_range);
    free(module_content);

    if (!found_def) return false;

    size_t copy_len = strlen(full_path);
    if (copy_len >= (size_t)full_path_len) copy_len = full_path_len - 1;
    memcpy(out_full_path, full_path, copy_len);
    out_full_path[copy_len] = '\0';
    return true;
}

// 在 clib 定义块内查找函数名定义
// clib 块内格式: <返回类型> FuncName(params)
// 如: Ptr[u8]  TTF_RenderText_Blended_Wrapped(Ptr[u8] font, ...)
static bool find_clib_func_definition(const char* content, const char* word,
                                      LspRange* range) {
    if (!content || !word) return false;

    int word_len = strlen(word);
    const char* p = content;
    int line = 0;
    int col = 0;

    // 先找到 "clib " 或 "export clib " 标记
    while (*p) {
        // 检查是否在行首边界
        bool at_boundary = (p == content ||
                            isspace((unsigned char)p[-1]) ||
                            p[-1] == '{');

        if (at_boundary) {
            const char* clib_kw = NULL;

            // 检查 "export clib "
            if (strncmp(p, "export clib ", 12) == 0) {
                clib_kw = p + 12;
            } else if (strncmp(p, "clib ", 5) == 0) {
                clib_kw = p + 5;
            }

            if (clib_kw) {
                // 找到 clib 块，跳过 clib 名称和 '{'
                const char* block_start = clib_kw;
                // 跳过 clib 名称
                while (*block_start && (isalnum((unsigned char)*block_start) || *block_start == '_')) {
                    block_start++;
                }
                // 查找 '{'
                while (*block_start && *block_start != '{') {
                    if (*block_start == '\n') { line++; col = 0; }
                    else col++;
                    block_start++;
                }
                if (*block_start != '{') continue;  // 没有找到块体
                block_start++;  // 跳过 '{'

                // 在 clib 块内查找函数名
                // 每行格式: <返回类型> FuncName(params)
                const char* line_start = block_start;
                const char* q = block_start;
                int block_line = line;
                int block_col = col;

                while (*q && *q != '}') {
                    // 跳过空白行
                    if (*q == '\n') {
                        block_line++;
                        block_col = 0;
                        line_start = q + 1;
                        q++;
                        continue;
                    }

                    // 跳过行首空白
                    while (*q && (*q == ' ' || *q == '\t')) {
                        q++;
                        block_col++;
                    }
                    if (!*q || *q == '}' || *q == '\n') continue;

                    // 跳过返回类型（可能是多个词，如 "Ptr[u8]" "bool" 等）
                    // 类型后面紧跟函数名，函数名后跟 '('
                    // 策略：在当前行中查找 " word(" 模式
                    const char* line_end = q;
                    while (*line_end && *line_end != '\n' && *line_end != '}') line_end++;

                    // 在当前行中查找函数名
                    const char* search = q;
                    while (search < line_end) {
                        // 检查是否匹配函数名（前面是空白或类型字符，后面是 '('）
                        if (strncmp(search, word, word_len) == 0 &&
                            (search == line_start || isspace((unsigned char)search[-1]) ||
                             search[-1] == ']') &&
                            search[word_len] == '(') {
                            // 计算行号和列号
                            // search 相对于 line_start 的偏移
                            int char_offset = 0;
                            const char* c = line_start;
                            while (c < search) { char_offset++; c++; }

                            range->start.line = block_line;
                            range->start.character = char_offset;
                            range->end.line = block_line;
                            range->end.character = char_offset + word_len;

                            return true;
                        }
                        search++;
                    }

                    // 跳到行尾
                    q = line_end;
                    if (*q == '\n') {
                        block_line++;
                        block_col = 0;
                        line_start = q + 1;
                        q++;
                    }
                }

                // 更新全局 line/col 到 clib 块结束位置
                line = block_line;
                col = block_col;
                p = q;
                continue;
            }
        }

        if (*p == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
        p++;
    }

    return false;
}

// 在内容中查找定义
static bool find_definition_in_content(const char* content, const char* word,
                                       LspRange* range) {
    if (!content || !word) return false;

    // 如果 word 以 '.' 开头，去掉前导点号
    const char* clean_word = word;
    if (word[0] == '.') clean_word = word + 1;
    if (!*clean_word) return false;

    int word_len = strlen(clean_word);
    const char* p = content;
    int line = 0;
    int col = 0;

    static const struct {
        const char* prefix;
        int len;
    } def_patterns[] = {
        {"export cstruct ", 15},
        {"export struct ", 14},
        {"export func ", 12},
        {"export enum ", 12},
        {"export face ", 12},
        {"export var ", 11},
        {"export const ", 13},
        {"cstruct ", 8},
        {"struct ", 7},
        {"func ", 5},
        {"enum ", 5},
        {"face ", 5},
        {"var ", 4},
        {"const ", 6},
    };
    static const int num_patterns = sizeof(def_patterns) / sizeof(def_patterns[0]);

    while (*p) {
        bool at_boundary = (p == content ||
                            isspace((unsigned char)p[-1]) ||
                            p[-1] == '{');

        if (at_boundary) {
            // 先检查标准定义模式
            for (int i = 0; i < num_patterns; i++) {
                int plen = def_patterns[i].len;
                if (strncmp(p, def_patterns[i].prefix, plen) == 0 &&
                    strncmp(p + plen, clean_word, word_len) == 0 &&
                    !isalnum((unsigned char)p[plen + word_len]) &&
                    p[plen + word_len] != '_') {

                    range->start.line = line;
                    range->start.character = col + plen;
                    range->end.line = line;
                    range->end.character = range->start.character + word_len;

                    return true;
                }
            }
            
            // 检查类型前缀变量声明（如 "Array[X] name", "int name", "string name", "Font name" 等）
            // 模式: <Type> <word>  其中 Type 可以是简单类型名或泛型类型 Array[X]
            {
                // 提取从 p 开始的 token 作为类型名
                const char* tp = p;
                // 跳过可能的 "export " 前缀
                if (strncmp(tp, "export ", 7) == 0) tp += 7;
                
                // 提取类型名（字母数字下划线）
                const char* type_start = tp;
                while (*tp && (isalnum((unsigned char)*tp) || *tp == '_')) tp++;
                if (tp > type_start) {
                    int type_len = tp - type_start;
                    // 检查类型名是否是有效的类型关键字
                    // 包括所有编译器内置类型、FFI/系统类型、C布局类型和用户定义类型名
                    bool is_type = false;
                    if (type_len <= 32) {
                        char type_buf[33];
                        memcpy(type_buf, type_start, type_len);
                        type_buf[type_len] = '\0';
                        // 内置类型
                        if (strcmp(type_buf, "int") == 0 ||
                            strcmp(type_buf, "float") == 0 ||
                            strcmp(type_buf, "string") == 0 ||
                            strcmp(type_buf, "bool") == 0 ||
                            strcmp(type_buf, "bigint") == 0 ||
                            strcmp(type_buf, "Array") == 0 ||
                            strcmp(type_buf, "Dict") == 0 ||
                            strcmp(type_buf, "any") == 0 ||
                            strcmp(type_buf, "File") == 0 ||
                            strcmp(type_buf, "var") == 0 ||
                            // FFI / 系统类型
                            strcmp(type_buf, "Ptr") == 0 ||
                            strcmp(type_buf, "Socket") == 0 ||
                            strcmp(type_buf, "Channel") == 0 ||
                            strcmp(type_buf, "Thread") == 0 ||
                            strcmp(type_buf, "Future") == 0 ||
                            // C 布局类型
                            strcmp(type_buf, "i8") == 0 ||
                            strcmp(type_buf, "u8") == 0 ||
                            strcmp(type_buf, "i16") == 0 ||
                            strcmp(type_buf, "u16") == 0 ||
                            strcmp(type_buf, "i32") == 0 ||
                            strcmp(type_buf, "u32") == 0 ||
                            strcmp(type_buf, "i64") == 0 ||
                            strcmp(type_buf, "u64") == 0 ||
                            strcmp(type_buf, "f32") == 0 ||
                            strcmp(type_buf, "f64") == 0 ||
                            strcmp(type_buf, "c_int") == 0 ||
                            strcmp(type_buf, "c_uint") == 0 ||
                            strcmp(type_buf, "c_long") == 0 ||
                            strcmp(type_buf, "c_ulong") == 0 ||
                            strcmp(type_buf, "c_longlong") == 0 ||
                            strcmp(type_buf, "c_ulonglong") == 0 ||
                            strcmp(type_buf, "c_size") == 0 ||
                            strcmp(type_buf, "c_ssize") == 0 ||
                            strcmp(type_buf, "str8") == 0 ||
                            strcmp(type_buf, "str16") == 0 ||
                            // 用户定义类型（首字母大写，如 Font, CachedTex）
                            (isupper((unsigned char)type_buf[0]))) {
                            is_type = true;
                        }
                    }
                    
                    if (is_type) {
                        // 跳过泛型参数部分（如 [CachedTex], [string, int]）
                        const char* after_type = tp;
                        if (*after_type == '[') {
                            // 跳过 [...] 部分
                            int depth = 1;
                            after_type++;
                            while (*after_type && depth > 0) {
                                if (*after_type == '[') depth++;
                                else if (*after_type == ']') depth--;
                                after_type++;
                            }
                        }
                        // 跳过可选类型标记 '?'（如 MenuBar? _ctxMenu）
                        if (*after_type == '?') after_type++;
                        // 跳过空白
                        while (*after_type && isspace((unsigned char)*after_type)) after_type++;
                        // 检查后面是否紧跟目标变量名
                        if (strncmp(after_type, clean_word, word_len) == 0 &&
                            (after_type[word_len] == '\0' ||
                             (!isalnum((unsigned char)after_type[word_len]) && after_type[word_len] != '_'))) {
                            // 确保再后面是 '=', '(', ';', '\n' 等（变量声明的结尾）
                            char after_word = after_type[word_len];
                            if (after_word == '=' || after_word == '\n' || after_word == '\r' ||
                                after_word == ';' || after_word == '\0' || 
                                isspace((unsigned char)after_word)) {
                                // after_type 相对于行首的偏移 = col + (after_type - p)
                                int name_col = col + (int)(after_type - p);
                                range->start.line = line;
                                range->start.character = name_col;
                                range->end.line = line;
                                range->end.character = name_col + word_len;
                                return true;
                            }
                        }

                        // 处理逗号分隔的多变量声明（如 "int a = 1, b = 2, c = 3"）
                        // 在同一行内，从类型声明开始扫描所有逗号后的变量名
                        const char* scan = after_type;
                        while (*scan && *scan != '\n' && *scan != '{' && *scan != '}') {
                            if (*scan == ',') {
                                // 跳过逗号后的空白
                                const char* comma_after = scan + 1;
                                while (*comma_after && isspace((unsigned char)*comma_after)) comma_after++;
                                // 检查是否是目标变量名
                                if (strncmp(comma_after, clean_word, word_len) == 0 &&
                                    (comma_after[word_len] == '\0' ||
                                     (!isalnum((unsigned char)comma_after[word_len]) && comma_after[word_len] != '_'))) {
                                    // 确保再后面是 '=', ';', '\n' 等
                                    char after_word = comma_after[word_len];
                                    if (after_word == '=' || after_word == '\n' || after_word == '\r' ||
                                        after_word == ';' || after_word == '\0' ||
                                        after_word == ',' ||
                                        isspace((unsigned char)after_word)) {
                                        int name_col = col + (int)(comma_after - p);
                                        range->start.line = line;
                                        range->start.character = name_col;
                                        range->end.line = line;
                                        range->end.character = name_col + word_len;
                                        return true;
                                    }
                                }
                            }
                            scan++;
                        }
                    }
                }
            }
        }

        if (*p == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
        p++;
    }

    // 如果标准定义模式没找到，尝试在 clib 块内查找函数定义
    if (find_clib_func_definition(content, clean_word, range)) return true;

    // 尝试在 struct/cstruct 定义块内查找字段定义
    return find_struct_field_definition(content, clean_word, range);
}

// 在 struct/cstruct 定义块内查找字段定义
// struct 块内格式: <Type> fieldName  或  <Type> fieldName;  或  <Type> fieldName = default
// 如: Ptr[u8] texture  或  string key  或  int w; int h; bool ok
static bool find_struct_field_definition(const char* content, const char* word,
                                          LspRange* range) {
    if (!content || !word) return false;

    int word_len = strlen(word);
    const char* p = content;
    int line = 0;
    int col = 0;

    // 查找 "struct " 或 "cstruct " 或 "export struct " 或 "export cstruct " 标记
    while (*p) {
        bool at_boundary = (p == content ||
                            isspace((unsigned char)p[-1]) ||
                            p[-1] == '{' || p[-1] == ';');

        if (at_boundary) {
            const char* struct_kw = NULL;

            if (strncmp(p, "export struct ", 14) == 0) { struct_kw = p + 14; }
            else if (strncmp(p, "export cstruct ", 15) == 0) { struct_kw = p + 15; }
            else if (strncmp(p, "cstruct ", 8) == 0) { struct_kw = p + 8; }
            else if (strncmp(p, "struct ", 7) == 0) { struct_kw = p + 7; }

            if (struct_kw) {
                // 跳过 struct 名称
                const char* block_start = struct_kw;
                while (*block_start && (isalnum((unsigned char)*block_start) || *block_start == '_')) {
                    block_start++;
                }
                // 查找 '{'
                while (*block_start && *block_start != '{') {
                    if (*block_start == '\n') { line++; col = 0; }
                    else col++;
                    block_start++;
                }
                if (*block_start != '{') { p++; continue; }
                block_start++;  // 跳过 '{'

                // 在 struct 块内查找字段
                const char* q = block_start;
                int field_line = line;
                int field_col = col + (int)(block_start - p);

                while (*q && *q != '}') {
                    // 跳过空白和注释
                    while (*q && *q != '}' && isspace((unsigned char)*q)) {
                        if (*q == '\n') { field_line++; field_col = 0; }
                        else field_col++;
                        q++;
                    }
                    if (*q == '}' || !*q) break;

                    // 跳过注释行
                    if (*q == '/' && q[1] == '/') {
                        while (*q && *q != '\n') q++;
                        continue;
                    }

                    // 提取类型名（字母数字下划线）
                    const char* type_start = q;
                    while (*q && (isalnum((unsigned char)*q) || *q == '_')) q++;
                    if (q == type_start) {
                        if (*q == '\n') { field_line++; field_col = 0; }
                        else field_col++;
                        q++;
                        continue;
                    }

                    // 跳过泛型参数 [X] 部分
                    if (*q == '[') {
                        int depth = 1;
                        while (*q && depth > 0) {
                            if (*q == '[') depth++;
                            else if (*q == ']') depth--;
                            if (*q == '\n') { field_line++; field_col = 0; }
                            else field_col++;
                            q++;
                        }
                    }
                    // 跳过可选类型标记 '?'（如 MenuBar? _field）
                    if (*q == '?') { field_col++; q++; }
                    // 跳过空白
                    while (*q && *q != '}' && isspace((unsigned char)*q)) {
                        if (*q == '\n') { field_line++; field_col = 0; }
                        else field_col++;
                        q++;
                    }

                    // 检查是否是目标字段名
                    if (strncmp(q, word, word_len) == 0 &&
                        (q[word_len] == '\0' || q[word_len] == '\n' ||
                         q[word_len] == ';' || q[word_len] == '=' ||
                         isspace((unsigned char)q[word_len]))) {
                        // 计算字段名相对于行首的列偏移
                        // 回溯到行首计算
                        const char* line_start = q;
                        while (line_start > content && line_start[-1] != '\n') line_start--;
                        int name_col = (int)(q - line_start);

                        range->start.line = field_line;
                        range->start.character = name_col;
                        range->end.line = field_line;
                        range->end.character = name_col + word_len;
                        return true;
                    }

                    // 跳过到行尾或 ';' 或下一个字段
                    while (*q && *q != '\n' && *q != ';' && *q != '}') q++;
                    if (*q == ';') q++;
                    if (*q == '\n') { field_line++; field_col = 0; q++; }
                }
            }
        }

        if (*p == '\n') { line++; col = 0; }
        else col++;
        p++;
    }

    return false;
}

// 获取定义位置
LspLocation* lsp_get_definition(const char* content, LspPosition pos, int* count, const char* uri) {
    *count = 0;
    
    if (!content) return NULL;
    
    // 获取光标下的单词
    char* word = get_word_at_position(content, pos);
    if (!word) return NULL;

    // 如果 word 是数字字面量（如 0, 0.0, 123, 3.14 等），不查找定义
    {
        bool is_number = true;
        bool has_dot = false;
        for (const char* p = word; *p; p++) {
            if (*p == '.') {
                if (has_dot) { is_number = false; break; }
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

    // 如果 word 以 '.' 开头（函数调用链，如 .TTF_RenderText_Blended_Wrapped），
    // 去掉前导点号，以便在模块符号表中正确查找
    const char* lookup_word = word;
    if (word[0] == '.') lookup_word = word + 1;
    if (!*lookup_word) { free(word); return NULL; }
    
    // 如果 word 含点号（如 SDL3.PIXELFORMAT_RGBA8888），拆分模块名和符号名
    // 以便在导入的模块中查找符号定义
    char* module_prefix = NULL;   // 如 "SDL3"
    char* symbol_name = NULL;     // 如 "PIXELFORMAT_RGBA8888"
    const char* dot_in_word = strchr(lookup_word, '.');
    if (dot_in_word && dot_in_word != lookup_word && dot_in_word[1]) {
        // 判断光标是在点号前（变量部分）还是点号后（方法部分）
        // 光标在变量部分时，应该用变量名查找定义，而非拆分模块前缀
        int cursor_offset = lsp_position_to_offset(content, pos);
        bool cursor_before_dot = false;
        if (cursor_offset >= 0) {
            int scan = cursor_offset;
            while (scan >= 0) {
                char c = content[scan];
                if (c == '.') {
                    // 光标在点号之后（方法部分），正常拆分模块前缀
                    cursor_before_dot = false;
                    break;
                }
                if (isalnum((unsigned char)c) || c == '_') {
                    scan--;
                    continue;
                }
                // 遇到非单词非点号字符，说明光标在变量部分（点号之前）
                cursor_before_dot = true;
                break;
            }
        }

        if (cursor_before_dot) {
            // 光标在变量部分，用变量名（点号前的部分）查找定义
            // 不拆分模块前缀，直接用变量名在当前文档和导入模块中查找
            int var_len = dot_in_word - lookup_word;
            char* var_name = (char*)malloc(var_len + 1);
            if (var_name) {
                memcpy(var_name, lookup_word, var_len);
                var_name[var_len] = '\0';
                // 用变量名替换 word 和 lookup_word，以便后续查找
                free(word);
                word = var_name;
                lookup_word = var_name;
            }
        } else {
            // 光标在方法部分，正常拆分模块前缀和符号名
            int mod_len = dot_in_word - lookup_word;
            module_prefix = (char*)malloc(mod_len + 1);
            memcpy(module_prefix, lookup_word, mod_len);
            module_prefix[mod_len] = '\0';
            
            const char* sym_start = dot_in_word + 1;
            // 去掉可能的后续点号（如 SDL3.SDL3.PIXELFORMAT...）
            while (sym_start[0] && sym_start[1] == '.') sym_start++;
            symbol_name = strdup(sym_start);
            
            // 用于后续查找的 lookup_word 改为纯符号名
            lookup_word = symbol_name;
        }
    }
    
    // 分配结果数组
    int capacity = 4;
    LspLocation* locations = (LspLocation*)malloc(sizeof(LspLocation) * capacity);
    if (!locations) {
        free(word);
        free(module_prefix);
        free(symbol_name);
        return NULL;
    }
    
    // 在当前文档中查找定义
    LspRange range;
    // 对于带模块前缀的表达式（如 SDL3.PIXELFORMAT_RGBA8888），不在当前文档中查找
    // 而是直接去模块中查找
    if (!module_prefix) {
        if (find_definition_in_content(content, word, &range)) {
            locations[*count].uri = strdup(uri ? uri : "");
            locations[*count].range = range;
            (*count)++;
        }
    }
    
    // 在导入的模块中查找定义（跨文件跳转）
    char file_path[MAX_PATH_LEN] = "";
    // 将 URI 转换为文件路径
    if (uri) {
        char* converted = lsp_uri_to_path(uri);
        if (converted) {
            strncpy(file_path, converted, MAX_PATH_LEN - 1);
            file_path[MAX_PATH_LEN - 1] = '\0';
            free(converted);
        }
    }

    // 检查 module_prefix 是否匹配某个导入别名
    bool module_prefix_matched = false;
    ImportEntry imports[MAX_IMPORTS];
    int import_count = extract_imports_from_content(content, imports, MAX_IMPORTS);
    for (int i = 0; i < import_count && *count < capacity; i++) {
        LspRange mod_range;
        char full_path[MAX_PATH_LEN];
        
        // 如果有模块前缀，只在该模块中查找
        if (module_prefix) {
            // 检查导入别名是否匹配模块前缀
            if (strcmp(imports[i].alias, module_prefix) != 0) continue;
            module_prefix_matched = true;
        }
        
        if (find_definition_in_module(imports[i].file_path,
                                      file_path[0] ? file_path : NULL,
                                      lookup_word, &mod_range, full_path, sizeof(full_path))) {
            // 将文件路径转换为 URI
            char* def_uri = lsp_path_to_uri(full_path);
            locations[*count].uri = def_uri ? def_uri : strdup("");
            locations[*count].range = mod_range;
            (*count)++;
        }
    }

    // 如果 module_prefix 不匹配任何导入别名（说明是实例变量如 _ctxMenu.method），
    // 用方法名（lookup_word）在所有导入模块中查找方法定义
    if (module_prefix && !module_prefix_matched && *count == 0) {
        for (int i = 0; i < import_count && *count < capacity; i++) {
            LspRange mod_range;
            char full_path[MAX_PATH_LEN];
            if (find_definition_in_module(imports[i].file_path,
                                          file_path[0] ? file_path : NULL,
                                          lookup_word, &mod_range, full_path, sizeof(full_path))) {
                char* def_uri = lsp_path_to_uri(full_path);
                locations[*count].uri = def_uri ? def_uri : strdup("");
                locations[*count].range = mod_range;
                (*count)++;
            }
        }
    }
    
    free(word);
    free(module_prefix);
    free(symbol_name);
    
    if (*count == 0) {
        free(locations);
        return NULL;
    }
    
    return locations;
}

// 释放位置信息
void lsp_free_locations(LspLocation* locs, int count) {
    if (!locs) return;
    
    for (int i = 0; i < count; i++) {
        free(locs[i].uri);
    }
    
    free(locs);
}

/**
 * 按指定 word 查找定义位置（供 hover 提示使用）
 * 与 lsp_get_definition 不同，此函数接受外部传入的 word，
 * 而非从 content/pos 中提取（因为 hover 的 word 可能含点号如 "r.method"，
 * 需要取最后一段方法名来查找定义）。
 * 返回的 LspLocation* 需由调用者用 lsp_free_locations 释放。
 */
LspLocation* lsp_find_definition_by_word(const char* content, const char* word,
                                         const char* current_file, int* count) {
    *count = 0;
    if (!content || !word) return NULL;

    // 处理带点号的 word（如 SDL3.PIXELFORMAT_RGBA8888 或 .method）
    // 取最后一个点号后的部分作为查找词
    const char* clean_word = word;
    if (word[0] == '.') clean_word = word + 1;  // 去掉前导点号
    const char* last_dot = strrchr(clean_word, '.');
    if (last_dot && last_dot[1]) clean_word = last_dot + 1;
    if (!*clean_word) return NULL;

    int capacity = 4;
    LspLocation* locations = (LspLocation*)malloc(sizeof(LspLocation) * capacity);
    if (!locations) return NULL;

    // 在当前文档中查找定义
    LspRange range;
    if (find_definition_in_content(content, clean_word, &range)) {
        char* uri = current_file ? lsp_path_to_uri(current_file) : strdup("");
        locations[*count].uri = uri ? uri : strdup("");
        locations[*count].range = range;
        (*count)++;
    }

    // 在导入的模块中查找定义（跨文件）
    ImportEntry imports[MAX_IMPORTS];
    int import_count = extract_imports_from_content(content, imports, MAX_IMPORTS);
    for (int i = 0; i < import_count && *count < capacity; i++) {
        LspRange mod_range;
        char full_path[MAX_PATH_LEN];
        if (find_definition_in_module(imports[i].file_path,
                                      current_file,
                                      clean_word, &mod_range, full_path, sizeof(full_path))) {
            char* def_uri = lsp_path_to_uri(full_path);
            locations[*count].uri = def_uri ? def_uri : strdup("");
            locations[*count].range = mod_range;
            (*count)++;
        }
    }

    if (*count == 0) {
        free(locations);
        return NULL;
    }
    return locations;
}
