/**
 * LSP References & Rename - 查找引用和重命名
 *
 * 基于词级文本搜索在工作区中查找符号引用。
 * 支持当前文件和所有打开的文档中的引用查找。
 */

#include "leno_lsp.h"
#include "lsp_completion.h"
#include "leno_compiler_lib.h"
#include "../src/include/leno_types.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ========== 内部辅助 ========== */

// 检查字符是否是标识符的一部分
static int is_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

// 检查字符是否是标识符的起始字符
static int is_ident_start(char c) {
    return isalpha((unsigned char)c) || c == '_';
}

// 在内容中查找所有匹配的单词引用（精确词匹配）
// 返回 LspLocation 数组，使用相同的 uri
typedef struct {
    LspLocation* locs;
    int count;
    int capacity;
} LocationArray;

static void loc_array_init(LocationArray* arr) {
    arr->count = 0;
    arr->capacity = 16;
    arr->locs = (LspLocation*)malloc(sizeof(LspLocation) * arr->capacity);
}

static void loc_array_add(LocationArray* arr, const char* uri, int line, int start_char, int end_char) {
    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        arr->locs = (LspLocation*)realloc(arr->locs, sizeof(LspLocation) * arr->capacity);
    }
    int idx = arr->count++;
    arr->locs[idx].uri = strdup(uri);
    arr->locs[idx].range.start.line = line;
    arr->locs[idx].range.start.character = start_char;
    arr->locs[idx].range.end.line = line;
    arr->locs[idx].range.end.character = end_char;
}

static void loc_array_free(LocationArray* arr) {
    for (int i = 0; i < arr->count; i++) {
        free(arr->locs[i].uri);
    }
    free(arr->locs);
}

// 在单个文档中查找所有精确词匹配
static void find_references_in_content(const char* content, const char* word,
                                        const char* uri, LocationArray* out) {
    int word_len = strlen(word);
    if (word_len == 0) return;

    const char* p = content;
    int line = 0;
    int col = 0;

    while (*p) {
        // 检查是否在注释中
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            if (*p) { p++; line++; col = 0; }
            continue;
        }
        if (*p == '/' && *(p+1) == '*') {
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) {
                if (*p == '\n') { line++; col = 0; }
                else col++;
                p++;
            }
            if (*p) { p += 2; col += 2; }
            continue;
        }
        // 检查是否在字符串中
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            p++;
            col++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1)) { p += 2; col += 2; continue; }
                if (*p == '\n') { line++; col = 0; }
                else col++;
                p++;
            }
            if (*p) { p++; col++; }
            continue;
        }

        // 检查是否匹配单词
        if (is_ident_start(*p) || *p == '_') {
            const char* start = p;
            int start_col = col;
            while (*p && is_ident_char(*p)) { p++; col++; }
            int len = p - start;
            if (len == word_len && strncmp(start, word, len) == 0) {
                loc_array_add(out, uri, line, start_col, start_col + len);
            }
            continue;
        }

        if (*p == '\n') { line++; col = 0; }
        else col++;
        p++;
    }
}

/* ========== References 主入口 ========== */

char* lsp_handle_references(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED, "Server not initialized");
    }

    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* position = json_object_get(params, "position");
    if (!text_doc || !position) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing parameters");
    }

    JsonValue* uri_val = json_object_get(text_doc, "uri");
    if (!uri_val) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing URI");
    }

    const char* uri = json_string_value(uri_val);
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }

    JsonValue* line_val = json_object_get(position, "line");
    JsonValue* char_val = json_object_get(position, "character");
    LspPosition pos = {
        .line = (uint32_t)json_int_value(line_val),
        .character = (uint32_t)json_int_value(char_val)
    };

    // 获取光标处的单词
    char* word = get_word_at_position(doc->content, pos);
    if (!word || !*word) {
        // 返回空数组
        JsonValue* result = json_array_new();
        char* response = lsp_create_response(id, result);
        json_free(result);
        free(word);
        return response;
    }

    // 提取纯名称（如果是 module.method 格式，取最后的 method 名）
    char* dot = strrchr(word, '.');
    const char* search_word = word;
    char* short_word = NULL;
    if (dot && *(dot + 1)) {
        short_word = strdup(dot + 1);
        search_word = short_word;
    }

    LocationArray arr;
    loc_array_init(&arr);

    // 在当前文档中查找
    find_references_in_content(doc->content, search_word, uri, &arr);

    // 也在所有其他打开的文档中查找
    LspTextDocument* other = server->documents;
    while (other) {
        if (other != doc && other->content) {
            find_references_in_content(other->content, search_word, other->uri, &arr);
        }
        other = other->next;
    }

    // 构建 JSON 响应
    JsonValue* result = json_array_new();
    for (int i = 0; i < arr.count; i++) {
        JsonValue* loc = json_object_new();
        json_object_set(loc, "uri", json_string_new(arr.locs[i].uri));

        JsonValue* range = json_object_new();
        JsonValue* start = json_object_new();
        json_object_set(start, "line", json_int_new(arr.locs[i].range.start.line));
        json_object_set(start, "character", json_int_new(arr.locs[i].range.start.character));
        JsonValue* end = json_object_new();
        json_object_set(end, "line", json_int_new(arr.locs[i].range.end.line));
        json_object_set(end, "character", json_int_new(arr.locs[i].range.end.character));
        json_object_set(range, "start", start);
        json_object_set(range, "end", end);
        json_object_set(loc, "range", range);

        json_array_add(result, loc);
    }

    char* response = lsp_create_response(id, result);
    json_free(result);
    loc_array_free(&arr);
    free(short_word);
    free(word);

    return response;
}

/* ========== Rename 主入口 ========== */

char* lsp_handle_rename(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED, "Server not initialized");
    }

    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* position = json_object_get(params, "position");
    JsonValue* new_name_val = json_object_get(params, "newName");
    if (!text_doc || !position || !new_name_val) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing parameters");
    }

    const char* new_name = json_string_value(new_name_val);
    // 验证新名称是合法标识符
    if (!new_name || !is_ident_start(new_name[0])) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Invalid new name");
    }
    for (int i = 1; new_name[i]; i++) {
        if (!is_ident_char(new_name[i])) {
            return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Invalid new name");
        }
    }

    JsonValue* uri_val = json_object_get(text_doc, "uri");
    if (!uri_val) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing URI");
    }

    const char* uri = json_string_value(uri_val);
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }

    JsonValue* line_val = json_object_get(position, "line");
    JsonValue* char_val = json_object_get(position, "character");
    LspPosition pos = {
        .line = (uint32_t)json_int_value(line_val),
        .character = (uint32_t)json_int_value(char_val)
    };

    char* word = get_word_at_position(doc->content, pos);
    if (!word || !*word) {
        free(word);
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "No symbol at position");
    }

    // 提取纯名称
    char* dot = strrchr(word, '.');
    const char* search_word = word;
    char* short_word = NULL;
    if (dot && *(dot + 1)) {
        short_word = strdup(dot + 1);
        search_word = short_word;
    }

    LocationArray arr;
    loc_array_init(&arr);

    find_references_in_content(doc->content, search_word, uri, &arr);

    // 在所有其他打开的文档中查找
    LspTextDocument* other = server->documents;
    while (other) {
        if (other != doc && other->content) {
            find_references_in_content(other->content, search_word, other->uri, &arr);
        }
        other = other->next;
    }

    // 构建 WorkspaceEdit 响应
    // { "changes": { "uri": [TextEdit, ...], ... } }
    JsonValue* result = json_object_new();
    JsonValue* changes = json_object_new();

    // 按 URI 分组 TextEdits
    LspTextDocument* cur = server->documents;
    while (cur) {
        // 收集此文档的所有引用
        JsonValue* edits = json_array_new();
        for (int i = 0; i < arr.count; i++) {
            if (strcmp(arr.locs[i].uri, cur->uri) == 0) {
                JsonValue* edit = json_object_new();

                JsonValue* range = json_object_new();
                JsonValue* start = json_object_new();
                json_object_set(start, "line", json_int_new(arr.locs[i].range.start.line));
                json_object_set(start, "character", json_int_new(arr.locs[i].range.start.character));
                JsonValue* end = json_object_new();
                json_object_set(end, "line", json_int_new(arr.locs[i].range.end.line));
                json_object_set(end, "character", json_int_new(arr.locs[i].range.end.character));
                json_object_set(range, "start", start);
                json_object_set(range, "end", end);
                json_object_set(edit, "range", range);

                json_object_set(edit, "newText", json_string_new(new_name));
                json_array_add(edits, edit);
            }
        }

        json_object_set(changes, cur->uri, edits);
        cur = cur->next;
    }

    json_object_set(result, "changes", changes);

    char* response = lsp_create_response(id, result);
    json_free(result);
    loc_array_free(&arr);
    free(short_word);
    free(word);

    return response;
}
