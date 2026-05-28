/**
 * 定义跳转服务
 * 提供跳转到定义位置的功能
 */

#include "leno_lsp.h"
#include <ctype.h>

// 获取光标下的单词（复用 lsp_hover.c 中的函数）
extern char* get_word_at_position(const char* content, LspPosition pos);

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

// 在内容中查找定义
static bool find_definition_in_content(const char* content, const char* word,
                                       LspRange* range) {
    if (!content || !word) return false;
    
    int word_len = strlen(word);
    const char* p = content;
    int line = 0;
    int col = 0;
    int offset = 0;
    
    while (*p) {
        // 检查是否是定义
        // 简单实现：查找 "func word"、"var word"、"struct word" 模式
        if ((strncmp(p, "func ", 5) == 0 && strncmp(p + 5, word, word_len) == 0 &&
             !isalnum((unsigned char)p[5 + word_len]) && p[5 + word_len] != '_') ||
            (strncmp(p, "var ", 4) == 0 && strncmp(p + 4, word, word_len) == 0 &&
             !isalnum((unsigned char)p[4 + word_len]) && p[4 + word_len] != '_') ||
            (strncmp(p, "struct ", 7) == 0 && strncmp(p + 7, word, word_len) == 0 &&
             !isalnum((unsigned char)p[7 + word_len]) && p[7 + word_len] != '_')) {
            
            // 找到定义
            range->start.line = line;
            range->start.character = col + (p[0] == 'v' ? 4 : (p[0] == 'f' ? 5 : 7));
            range->end.line = line;
            range->end.character = range->start.character + word_len;
            
            return true;
        }
        
        if (*p == '\n') {
            line++;
            col = 0;
        } else {
            col++;
        }
        p++;
        offset++;
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
    
    // TODO: 从导入的模块中查找定义
    // TODO: 复用 LenoC 的 scope.c 和 module_loader.c
    // TODO: 处理跨文件跳转
    
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
