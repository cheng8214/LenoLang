/**
 * LSP Folding Range - 代码折叠区域
 *
 * 基于语法结构生成折叠区域：
 * - 函数体 大括号块
 * - struct/cstruct/enum/face 块
 * - import 连续行
 * - 多行注释块
 */

#include "leno_lsp.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    int start_line;
    int end_line;
    const char* kind;  // "comment", "region", "imperial", "section"
} FoldingRange;

typedef struct {
    FoldingRange* ranges;
    int count;
    int capacity;
} FoldingArray;

static void fold_init(FoldingArray* arr) {
    arr->count = 0;
    arr->capacity = 16;
    arr->ranges = (FoldingRange*)malloc(sizeof(FoldingRange) * arr->capacity);
}

static void fold_add(FoldingArray* arr, int start, int end, const char* kind) {
    if (start >= end) return;  // 单行不需要折叠
    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        arr->ranges = (FoldingRange*)realloc(arr->ranges, sizeof(FoldingRange) * arr->capacity);
    }
    arr->ranges[arr->count].start_line = start;
    arr->ranges[arr->count].end_line = end;
    arr->ranges[arr->count].kind = kind;
    arr->count++;
}

static void fold_free(FoldingArray* arr) {
    free(arr->ranges);
}

/* ========== 主入口 ========== */

char* lsp_handle_folding_range(LspServer* server, int id, JsonValue* params) {
    if (!server || server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_SERVER_NOT_INITIALIZED, "Server not initialized");
    }

    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing textDocument");
    }

    JsonValue* uri_val = json_object_get(text_doc, "uri");
    if (!uri_val) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing URI");
    }

    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri_val));
    if (!doc || !doc->content) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }

    const char* content = doc->content;
    FoldingArray arr;
    fold_init(&arr);

    int line = 0;
    const char* p = content;

    // 用栈跟踪 { ... } 块
    int brace_stack[256];
    int brace_top = 0;

    while (*p) {
        // 跳过字符串
        if (*p == '"' || *p == '\'') {
            char quote = *p;
            p++;
            while (*p && *p != quote) {
                if (*p == '\\' && *(p+1)) { p += 2; continue; }
                if (*p == '\n') line++;
                p++;
            }
            if (*p) p++;
            continue;
        }

        // 块注释
        if (*p == '/' && *(p+1) == '*') {
            int start_line = line;
            p += 2;
            while (*p && !(*p == '*' && *(p+1) == '/')) {
                if (*p == '\n') line++;
                p++;
            }
            if (*p) { p += 2; }
            fold_add(&arr, start_line, line, "comment");
            continue;
        }

        // 行注释
        if (*p == '/' && *(p+1) == '/') {
            while (*p && *p != '\n') p++;
            if (*p) { p++; line++; }
            continue;
        }

        // 跟踪 { ... } 块
        if (*p == '{') {
            if (brace_top < 256) {
                brace_stack[brace_top++] = line;
            }
            p++;
            continue;
        }

        if (*p == '}') {
            if (brace_top > 0) {
                int start_line = brace_stack[--brace_top];
                fold_add(&arr, start_line, line, "region");
            }
            p++;
            continue;
        }

        if (*p == '\n') line++;
        p++;
    }

    // 构建 JSON 响应
    JsonValue* result = json_array_new();
    for (int i = 0; i < arr.count; i++) {
        JsonValue* range = json_object_new();
        json_object_set(range, "startLine", json_int_new(arr.ranges[i].start_line));
        json_object_set(range, "endLine", json_int_new(arr.ranges[i].end_line));
        if (arr.ranges[i].kind) {
            json_object_set(range, "kind", json_string_new(arr.ranges[i].kind));
        }
        json_array_add(result, range);
    }

    char* response = lsp_create_response(id, result);
    json_free(result);
    fold_free(&arr);

    return response;
}
