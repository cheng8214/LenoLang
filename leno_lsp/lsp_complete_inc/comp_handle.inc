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
