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
 * 支持: import "path" 和 import "path" as alias
 */
static int extract_imports_from_content(const char* content, ImportEntry* imports, int max_imports) {
    int count = 0;
    if (!content) return 0;

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
        if (*p != '"') {
            while (*p && *p != '\n') p++;
            if (*p) p++;
            continue;
        }

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

    module_symbol_table_destroy(sym_table);
    if (!found) return false;

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

// 在内容中查找定义
static bool find_definition_in_content(const char* content, const char* word,
                                       LspRange* range) {
    if (!content || !word) return false;
    
    int word_len = strlen(word);
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
        {"cstruct ", 8},
        {"struct ", 7},
        {"func ", 5},
        {"enum ", 5},
        {"face ", 5},
        {"var ", 4},
    };
    static const int num_patterns = sizeof(def_patterns) / sizeof(def_patterns[0]);
    
    while (*p) {
        bool at_boundary = (p == content ||
                            isspace((unsigned char)p[-1]) ||
                            p[-1] == '{');
        
        if (at_boundary) {
            for (int i = 0; i < num_patterns; i++) {
                int plen = def_patterns[i].len;
                if (strncmp(p, def_patterns[i].prefix, plen) == 0 &&
                    strncmp(p + plen, word, word_len) == 0 &&
                    !isalnum((unsigned char)p[plen + word_len]) &&
                    p[plen + word_len] != '_') {
                    
                    range->start.line = line;
                    range->start.character = col + plen;
                    range->end.line = line;
                    range->end.character = range->start.character + word_len;
                    
                    return true;
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
    
    return false;
}

// 获取定义位置
LspLocation* lsp_get_definition(const char* content, LspPosition pos, int* count, const char* uri) {
    *count = 0;
    
    if (!content) return NULL;
    
    // 获取光标下的单词
    char* word = get_word_at_position(content, pos);
    if (!word) return NULL;
    
    // 分配结果数组
    int capacity = 4;
    LspLocation* locations = (LspLocation*)malloc(sizeof(LspLocation) * capacity);
    if (!locations) {
        free(word);
        return NULL;
    }
    
    // 在当前文档中查找定义
    LspRange range;
    if (find_definition_in_content(content, word, &range)) {
        locations[*count].uri = strdup(uri ? uri : "");
        locations[*count].range = range;
        (*count)++;
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

    ImportEntry imports[MAX_IMPORTS];
    int import_count = extract_imports_from_content(content, imports, MAX_IMPORTS);
    for (int i = 0; i < import_count && *count < capacity; i++) {
        LspRange mod_range;
        char full_path[MAX_PATH_LEN];
        if (find_definition_in_module(imports[i].file_path,
                                      file_path[0] ? file_path : NULL,
                                      word, &mod_range, full_path, sizeof(full_path))) {
            // 将文件路径转换为 URI
            char* def_uri = lsp_path_to_uri(full_path);
            locations[*count].uri = def_uri ? def_uri : strdup("");
            locations[*count].range = mod_range;
            (*count)++;
        }
    }
    
    free(word);
    
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

    int capacity = 4;
    LspLocation* locations = (LspLocation*)malloc(sizeof(LspLocation) * capacity);
    if (!locations) return NULL;

    // 在当前文档中查找定义
    LspRange range;
    if (find_definition_in_content(content, word, &range)) {
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
                                      word, &mod_range, full_path, sizeof(full_path))) {
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
