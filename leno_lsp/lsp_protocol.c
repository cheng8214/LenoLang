/**
 * LSP 协议实现
 * 处理 JSON-RPC 消息和 LSP 协议核心方法
 */

#include "leno_lsp.h"
#include <stdarg.h>
#include <time.h>
#include "../src/include/module_loader.h"

// 创建 JSON 对象辅助函数
JsonValue* json_object_new(void) {
    JsonValue* obj = (JsonValue*)malloc(sizeof(JsonValue));
    if (!obj) return NULL;
    obj->type = JSON_OBJECT;
    obj->data.object_val.members = NULL;
    obj->data.object_val.count = 0;
    return obj;
}

JsonValue* json_string_new(const char* str) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_STRING;
    val->data.string_val = strdup(str);
    return val;
}

JsonValue* json_int_new(int n) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_NUMBER;
    val->data.number_val = n;
    return val;
}

JsonValue* json_bool_new(bool b) {
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    val->type = JSON_BOOL;
    val->data.bool_val = b;
    return val;
}

JsonValue* json_array_new(void) {
    JsonValue* arr = (JsonValue*)malloc(sizeof(JsonValue));
    if (!arr) return NULL;
    arr->type = JSON_ARRAY;
    arr->data.array_val.items = NULL;
    arr->data.array_val.count = 0;
    return arr;
}

void json_object_set(JsonValue* obj, const char* key, JsonValue* val) {
    if (!obj || obj->type != JSON_OBJECT) return;
    
    // 检查是否已存在
    for (int i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.members[i].key, key) == 0) {
            json_free(obj->data.object_val.members[i].value);
            obj->data.object_val.members[i].value = val;
            return;
        }
    }
    
    // 添加新成员
    int count = obj->data.object_val.count;
    obj->data.object_val.members = (JsonMember*)realloc(obj->data.object_val.members,
                                                         sizeof(JsonMember) * (count + 1));
    obj->data.object_val.members[count].key = strdup(key);
    obj->data.object_val.members[count].value = val;
    obj->data.object_val.count++;
}

void json_array_add(JsonValue* arr, JsonValue* val) {
    if (!arr || arr->type != JSON_ARRAY) return;
    
    int count = arr->data.array_val.count;
    arr->data.array_val.items = (JsonValue**)realloc(arr->data.array_val.items,
                                                      sizeof(JsonValue*) * (count + 1));
    arr->data.array_val.items[count] = val;
    arr->data.array_val.count++;
}

// 读取 LSP 消息（带 Content-Length 头）
char* lsp_read_message(FILE* stream) {
    char line[1024];
    int content_length = -1;
    
    // 读取头部
    while (fgets(line, sizeof(line), stream)) {
        // 移除行尾的换行符以便比较
        size_t len = strlen(line);
        
        // 空行表示头部结束（可能是 \r\n 或 \n）
        if (len == 0) continue;
        if ((len == 1 && line[0] == '\n') ||
            (len == 2 && line[0] == '\r' && line[1] == '\n')) {
            break;
        }
        
        // 解析 Content-Length
        if (strncmp(line, "Content-Length: ", 16) == 0) {
            content_length = atoi(line + 16);
        }
    }
    
    if (content_length < 0 || content_length > 1024 * 1024) {
        // 无效的长度或超过 1MB
        return NULL;
    }
    
    // 读取内容
    char* content = (char*)malloc(content_length + 1);
    if (!content) return NULL;
    
    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        size_t read = fread(content + total_read, 1, content_length - total_read, stream);
        if (read == 0) {
            if (feof(stream)) break;
            free(content);
            return NULL;
        }
        total_read += read;
    }
    
    if (total_read != (size_t)content_length) {
        free(content);
        return NULL;
    }
    
    content[content_length] = '\0';
    return content;
}

// 写入 LSP 消息
void lsp_write_message(FILE* stream, const char* message) {
    if (!message) return;
    
    int len = strlen(message);
    fprintf(stream, "Content-Length: %d\r\n\r\n%s", len, message);
    fflush(stream);
}

// 创建响应消息
char* lsp_create_response(int id, JsonValue* result) {
	JsonValue* response = json_object_new();
	if (!response) return NULL;
	
	json_object_set(response, "jsonrpc", json_string_new("2.0"));
	json_object_set(response, "id", json_int_new(id));
	if (result) {
		// 使用深拷贝，这样调用者可以安全地释放 result
		json_object_set(response, "result", json_deep_copy(result));
	} else {
		json_object_set(response, "result", json_object_new());
	}
	
	char* result_str = (char*)json_stringify(response);
	json_free(response);
	
	return result_str;
}

// 创建错误响应
char* lsp_create_error(int id, int code, const char* message) {
	JsonValue* response = json_object_new();
	JsonValue* error = json_object_new();
	
	json_object_set(response, "jsonrpc", json_string_new("2.0"));
	json_object_set(response, "id", json_int_new(id));
	
	json_object_set(error, "code", json_int_new(code));
	json_object_set(error, "message", json_string_new(message));
	json_object_set(response, "error", error);
	
	char* result_str = (char*)json_stringify(response);
	json_free(response);
	
	return result_str;
}

// 创建通知消息
char* lsp_create_notification(const char* method, JsonValue* params) {
	JsonValue* notification = json_object_new();
	if (!notification) return NULL;
	
	json_object_set(notification, "jsonrpc", json_string_new("2.0"));
	json_object_set(notification, "method", json_string_new(method));
	if (params) {
		// 使用深拷贝，这样调用者可以安全地释放 params
		json_object_set(notification, "params", json_deep_copy(params));
	}
	
	char* result_str = (char*)json_stringify(notification);
	json_free(notification);
	
	return result_str;
}

// 处理 initialize 请求
char* lsp_handle_initialize(LspServer* server, int id, JsonValue* params) {
    if (server->state != LSP_STATE_UNINITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_INVALID_REQUEST, 
                                "Server already initialized");
    }
    
    server->state = LSP_STATE_INITIALIZING;
    
    // 解析客户端能力（可选）
    JsonValue* client_info = json_object_get(params, "clientInfo");
    if (client_info) {
        JsonValue* name = json_object_get(client_info, "name");
        if (name) {
            lsp_log(server, LSP_LOG_INFO, "Client: %s", json_string_value(name));
        }
    }
    
    // 获取根目录
    JsonValue* root_uri = json_object_get(params, "rootUri");
    if (root_uri && root_uri->type == JSON_STRING) {
        server->root_path = lsp_uri_to_path(json_string_value(root_uri));
    }
    
    // 设置模块符号表缓存目录（与编译器共享 .lenocache/ 目录）
    // 这是 LSP 性能的关键：没有缓存目录，每次分析都要重新扫描所有导入的模块源文件
    if (server->root_path) {
        char cache_dir[1024];
#ifdef _WIN32
        snprintf(cache_dir, sizeof(cache_dir), "%s\\.lenocache\\", server->root_path);
#else
        snprintf(cache_dir, sizeof(cache_dir), "%s/.lenocache/", server->root_path);
#endif
        module_loader_set_cache_enabled(1);
        module_loader_set_cache_dir(cache_dir);
        fprintf(stderr, "[LSP-CACHE] cache_dir=%s\n", cache_dir);
        fflush(stderr);
    } else {
        fprintf(stderr, "[LSP-CACHE] WARNING: root_path is NULL, module cache disabled\n");
        fflush(stderr);
    }
    
    // 构建服务器能力
    JsonValue* capabilities = json_object_new();
    
    // 文本同步 - 增量更新
    JsonValue* text_sync = json_object_new();
    json_object_set(text_sync, "openClose", json_bool_new(true));
    json_object_set(text_sync, "change", json_int_new(1)); // Full
    json_object_set(capabilities, "textDocumentSync", text_sync);
    
    // 补全提供者
    if (server->completion_provider) {
        JsonValue* completion = json_object_new();
        json_object_set(completion, "resolveProvider", json_bool_new(false));
        
        JsonValue* trigger_chars = json_array_new();
        json_array_add(trigger_chars, json_string_new("."));
        json_array_add(trigger_chars, json_string_new(":"));
        json_array_add(trigger_chars, json_string_new("{"));
        json_array_add(trigger_chars, json_string_new(","));
        json_object_set(completion, "triggerCharacters", trigger_chars);
        
        json_object_set(capabilities, "completionProvider", completion);
    }
    
    // 悬停提供者
    if (server->hover_provider) {
        json_object_set(capabilities, "hoverProvider", json_bool_new(true));
    }
    
    // 定义跳转提供者
    if (server->definition_provider) {
        json_object_set(capabilities, "definitionProvider", json_bool_new(true));
    }
    
    // 诊断提供者
    if (server->diagnostic_provider) {
        JsonValue* diagnostic = json_object_new();
        json_object_set(diagnostic, "interFileDependencies", json_bool_new(true));
        json_object_set(diagnostic, "workspaceDiagnostics", json_bool_new(false));
        json_object_set(capabilities, "diagnosticProvider", diagnostic);
    }
    
    // 构建响应
    JsonValue* result = json_object_new();
    json_object_set(result, "capabilities", capabilities);
    
    JsonValue* server_info = json_object_new();
    json_object_set(server_info, "name", json_string_new("LenoLSP"));
    json_object_set(server_info, "version", json_string_new(LSP_VERSION));
    json_object_set(result, "serverInfo", server_info);
    
    char* response = lsp_create_response(id, result);
	
	// 释放 JSON 对象（result 会递归释放所有子对象）
	json_free(result);
	
	server->state = LSP_STATE_INITIALIZED;
	
	lsp_log(server, LSP_LOG_INFO, "Server initialized");
	
	return response;
}

// 处理 shutdown 请求
char* lsp_handle_shutdown(LspServer* server, int id) {
    if (server->state != LSP_STATE_INITIALIZED) {
        return lsp_create_error(id, LSP_ERROR_INVALID_REQUEST,
                                "Server not initialized");
    }
    
    server->state = LSP_STATE_SHUTDOWN;
    
    lsp_log(server, LSP_LOG_INFO, "Server shutting down");
    
    return lsp_create_response(id, NULL);
}

// 处理 exit 通知
char* lsp_handle_exit(LspServer* server) {
    lsp_log(server, LSP_LOG_INFO, "Server exiting");
    return NULL; // 返回 NULL 表示退出
}

// 处理文档打开通知
char* lsp_handle_did_open(LspServer* server, JsonValue* params) {
    if (!server || !params) return NULL;
    
    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc || text_doc->type != JSON_OBJECT) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* text = json_object_get(text_doc, "text");
    JsonValue* version = json_object_get(text_doc, "version");
    
    if (!uri || uri->type != JSON_STRING) return NULL;
    if (!text || text->type != JSON_STRING) return NULL;
    
    const char* uri_str = json_string_value(uri);
    const char* text_str = json_string_value(text);
    int ver = json_int_value(version);
    
    if (!uri_str || !text_str) return NULL;
    
    // 打开文档
    lsp_document_open(server, uri_str, text_str, ver);
    
    lsp_log(server, LSP_LOG_INFO, "Document opened: %s", uri_str);
    
    // 发布诊断
    if (server->diagnostic_provider) {
        lsp_publish_diagnostics(server, uri_str);
    }
    
    return NULL;
}

// 处理文档修改通知
char* lsp_handle_did_change(LspServer* server, JsonValue* params) {
    JsonValue* text_doc = json_object_get(params, "textDocument");
    JsonValue* content_changes = json_object_get(params, "contentChanges");
    
    if (!text_doc || !content_changes) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    JsonValue* version = json_object_get(text_doc, "version");
    
    if (!uri) return NULL;
    
    fprintf(stderr, "[DID-CHANGE] uri=%s version=%d\n", json_string_value(uri), 
            version ? (int)json_int_value(version) : -1);
    fflush(stderr);
    
    // 获取文档
    LspTextDocument* doc = lsp_document_get(server, json_string_value(uri));
    if (!doc) return NULL;
    
    // 应用内容变更（使用最后一个变更的 text 进行全量替换）
    if (content_changes->type == JSON_ARRAY && 
        content_changes->data.array_val.count > 0) {
        // 获取最后一个变更（最新的内容）
        int last_idx = content_changes->data.array_val.count - 1;
        JsonValue* change = content_changes->data.array_val.items[last_idx];
        
        if (change && change->type == JSON_OBJECT) {
            JsonValue* new_text = json_object_get(change, "text");
            
            if (new_text && new_text->type == JSON_STRING) {
                const char* text = json_string_value(new_text);
                if (text) {
                    lsp_document_update(server, json_string_value(uri),
                                       text,
                                       json_int_value(version));
                }
            }
        }
    }
    
    // 发布诊断
    if (server->diagnostic_provider) {
        lsp_publish_diagnostics(server, json_string_value(uri));
    }
    
    return NULL;
}

// 处理文档关闭通知
char* lsp_handle_did_close(LspServer* server, JsonValue* params) {
    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) return NULL;
    
    JsonValue* uri = json_object_get(text_doc, "uri");
    if (!uri) return NULL;
    
    lsp_document_close(server, json_string_value(uri));
    
    return NULL;
}

// 主消息处理函数
char* lsp_handle_message(LspServer* server, const char* message) {
    JsonValue* req = json_parse(message);
    if (!req) {
        return lsp_create_error(0, LSP_ERROR_PARSE_ERROR, "Invalid JSON");
    }
    
    // 检查 jsonrpc 版本
    JsonValue* jsonrpc = json_object_get(req, "jsonrpc");
    if (!jsonrpc || strcmp(json_string_value(jsonrpc), "2.0") != 0) {
        json_free(req);
        return lsp_create_error(0, LSP_ERROR_INVALID_REQUEST, 
                                "Unsupported JSON-RPC version");
    }
    
    // 获取方法名
    JsonValue* method = json_object_get(req, "method");
    if (!method || method->type != JSON_STRING) {
        json_free(req);
        return lsp_create_error(0, LSP_ERROR_INVALID_REQUEST, 
                                "Missing or invalid method");
    }
    
    const char* method_name = json_string_value(method);
    JsonValue* params = json_object_get(req, "params");
    JsonValue* id_val = json_object_get(req, "id");
    int id = id_val ? json_int_value(id_val) : -1;
    
    char* response = NULL;
    
    // 分发处理
    if (strcmp(method_name, "initialize") == 0) {
        response = lsp_handle_initialize(server, id, params);
    }
    else if (strcmp(method_name, "shutdown") == 0) {
        response = lsp_handle_shutdown(server, id);
    }
    else if (strcmp(method_name, "exit") == 0) {
        response = lsp_handle_exit(server);
    }
    else if (strcmp(method_name, "textDocument/didOpen") == 0) {
        response = lsp_handle_did_open(server, params);
    }
    else if (strcmp(method_name, "textDocument/didChange") == 0) {
        response = lsp_handle_did_change(server, params);
    }
    else if (strcmp(method_name, "textDocument/didClose") == 0) {
        response = lsp_handle_did_close(server, params);
    }
    else if (strcmp(method_name, "textDocument/completion") == 0) {
        response = lsp_handle_completion(server, id, params);
    }
    else if (strcmp(method_name, "textDocument/hover") == 0) {
        response = lsp_handle_hover(server, id, params);
    }
    else if (strcmp(method_name, "textDocument/definition") == 0) {
		response = lsp_handle_definition(server, id, params);
	}
	else if (strcmp(method_name, "textDocument/diagnostic") == 0) {
		response = lsp_handle_document_diagnostic(server, id, params);
	}
	else {
        // 未知方法
        if (id >= 0) {
            response = lsp_create_error(id, LSP_ERROR_METHOD_NOT_FOUND,
                                        "Method not found");
        }
    }
    
    json_free(req);
    return response;
}

// URI 转换工具
char* lsp_uri_to_path(const char* uri) {
    if (!uri) return NULL;
    
    const char* path_start = uri;
    
    // file:// 协议
    if (strncmp(uri, "file://", 7) == 0) {
        // Windows: file:///C:/path -> C:/path  或 file:///d%3A/path -> d:/path
        // Unix: file:///path -> /path
        if (uri[7] == '/' && strlen(uri) > 9 &&
            (uri[9] == ':' || (uri[9] == '%' && uri[10] == '3' && (uri[11] == 'A' || uri[11] == 'a')))) {
            // Windows 绝对路径
            path_start = uri + 8;
        } else {
            path_start = uri + 7;
        }
    }
    
    // URL 解码（处理 %3A -> : 等编码）
    const char* src = path_start;
    int len = strlen(src);
    char* result = (char*)malloc(len + 1);
    if (!result) return NULL;
    
    int dst = 0;
    while (*src) {
        if (*src == '%' && src[1] && src[2]) {
            // 解析两位十六进制
            char hex[3] = {src[1], src[2], 0};
            char* endptr;
            long val = strtol(hex, &endptr, 16);
            if (endptr == hex + 2) {
                result[dst++] = (char)val;
                src += 3;
                continue;
            }
        }
        result[dst++] = *src++;
    }
    result[dst] = '\0';
    
    return result;
}

char* lsp_path_to_uri(const char* path) {
    if (!path) return NULL;
    
    // 检查是否已经是 URI
    if (strncmp(path, "file://", 7) == 0) {
        return strdup(path);
    }
    
    // 构建 file:// URI
    char* uri = (char*)malloc(strlen(path) + 8);
    if (!uri) return NULL;
    
    #ifdef _WIN32
    sprintf(uri, "file:///%s", path);
    #else
    sprintf(uri, "file://%s", path);
    #endif
    
    return uri;
}
