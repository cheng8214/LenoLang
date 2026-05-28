/**
 * 诊断服务
 * 复用 LenoC 编译器提供实时错误检查
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"

// 发布诊断信息
// 从 URI 提取文件路径（去掉 file:// 前缀）
static void uri_to_filepath(const char* uri, char* out, size_t out_size) {
    if (!uri || !out || out_size == 0) return;
    
    // 检查是否是 file:// 协议
    if (strncmp(uri, "file:///", 8) == 0) {
        // Windows 路径: file:///d%3A/... -> d:/...
        const char* path = uri + 8;
        
        // URL 解码（简单的 %XX 处理）
        size_t j = 0;
        for (size_t i = 0; path[i] && j < out_size - 1; i++) {
            if (path[i] == '%' && path[i+1] && path[i+2]) {
                // 解码 %XX
                char hex[3] = {path[i+1], path[i+2], '\0'};
                int val = (int)strtol(hex, NULL, 16);
                out[j++] = (char)val;
                i += 2;
            } else if (path[i] == '/') {
                out[j++] = '\\';  // 转换为 Windows 路径分隔符
            } else {
                out[j++] = path[i];
            }
        }
        out[j] = '\0';
    } else {
        // 不是 file:// 协议，直接复制
        strncpy(out, uri, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

void lsp_publish_diagnostics(LspServer* server, const char* uri) {
    if (!server || !uri) return;
    
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) return;
    
    // 从 URI 提取文件路径
    char filepath[512];
    uri_to_filepath(uri, filepath, sizeof(filepath));
    
    // 获取诊断信息，传入文件名用于模块路径解析
    int count = 0;
    LspDiagnostic* diags = lsp_compile_and_get_errors_with_filename(doc->content, &count, filepath);
    
    // 构建诊断通知
    JsonValue* params = json_object_new();
    json_object_set(params, "uri", json_string_new(uri));
    
    JsonValue* diagnostics = json_array_new();
    for (int i = 0; i < count; i++) {
        JsonValue* diag = json_object_new();
        
        // 范围
        JsonValue* range = json_object_new();
        JsonValue* start = json_object_new();
        JsonValue* end = json_object_new();
        json_object_set(start, "line", json_int_new(diags[i].range.start.line));
        json_object_set(start, "character", json_int_new(diags[i].range.start.character));
        json_object_set(end, "line", json_int_new(diags[i].range.end.line));
        json_object_set(end, "character", json_int_new(diags[i].range.end.character));
        json_object_set(range, "start", start);
        json_object_set(range, "end", end);
        json_object_set(diag, "range", range);
        
        // 严重程度
        json_object_set(diag, "severity", json_int_new(diags[i].severity));
        
        // 代码
        if (diags[i].code) {
            json_object_set(diag, "code", json_string_new(diags[i].code));
        }
        
        // 来源
        json_object_set(diag, "source", json_string_new("leno"));
        
        // 消息
        json_object_set(diag, "message", json_string_new(diags[i].message));
        
        json_array_add(diagnostics, diag);
    }
    
    json_object_set(params, "diagnostics", diagnostics);
    
    // 发送通知
    char* notification = lsp_create_notification("textDocument/publishDiagnostics", params);
    if (notification) {
        lsp_write_message(stdout, notification);
        free(notification);
    }
    
    // 清理
    json_free(params);
    lsp_free_diagnostics(diags, count);
}

// 将 LenoC 错误类型转换为 LSP 严重程度
static int error_type_to_severity(ErrorType type) {
    switch (type) {
        case ERR_SYNTAX:
        case ERR_UNDEFINED_VAR:
        case ERR_UNDEFINED_FUNC:
        case ERR_DUPLICATE_VAR:
        case ERR_TYPE_MISMATCH:
        case ERR_CLOSURE:
            return LSP_DIAG_ERROR;
        case ERR_SEMANTIC:
            return LSP_DIAG_WARNING;
        default:
            return LSP_DIAG_INFORMATION;
    }
}

// 编译并获取错误（复用 LenoC 编译器）
LspDiagnostic* lsp_compile_and_get_errors_with_filename(const char* content, int* count, const char* filename) {
    *count = 0;
    
    if (!content) return NULL;
    
    // 使用 LenoC 编译器分析代码
    CompilerContext ctx;
    compiler_context_init(&ctx);
    
    // 编译分析（不执行代码生成），传入文件名用于模块路径解析
    compiler_analyze_with_filename(&ctx, content, filename);
    
    // 获取错误信息
    int error_count = errors.count;
    if (error_count == 0) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }
    
    // 分配诊断数组
    LspDiagnostic* diags = (LspDiagnostic*)malloc(sizeof(LspDiagnostic) * error_count);
    if (!diags) {
        compiler_context_cleanup(&ctx);
        return NULL;
    }
    
    // 转换错误为 LSP 诊断格式
    for (int i = 0; i < error_count; i++) {
        Error* err = &errors.list[i];
        
        // 行号转换为 0-based
        diags[i].range.start.line = err->line > 0 ? err->line - 1 : 0;
        diags[i].range.start.character = 0;
        diags[i].range.end.line = diags[i].range.start.line;
        diags[i].range.end.character = 100; // 整行高亮
        
        diags[i].severity = error_type_to_severity(err->type);
        
        // 错误代码
        char code_buf[16];
        snprintf(code_buf, sizeof(code_buf), "E%03d", err->type);
        diags[i].code = strdup(code_buf);
        
        diags[i].source = strdup("leno");
        diags[i].message = strdup(err->msg);
    }
    
    *count = error_count;
    
    compiler_context_cleanup(&ctx);
    return diags;
}

LspDiagnostic* lsp_compile_and_get_errors(const char* content, int* count) {
    return lsp_compile_and_get_errors_with_filename(content, count, NULL);
}

// 释放诊断信息
void lsp_free_diagnostics(LspDiagnostic* diags, int count) {
    if (!diags) return;
    
    for (int i = 0; i < count; i++) {
        free(diags[i].code);
        free(diags[i].source);
        free(diags[i].message);
    }
    
    free(diags);
}

// 处理 textDocument/diagnostic 请求 (LSP 3.17+)
char* lsp_handle_document_diagnostic(LspServer* server, int id, JsonValue* params) {
    if (!server || !params) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing parameters");
    }
    
    // 获取文档 URI
    JsonValue* text_doc = json_object_get(params, "textDocument");
    if (!text_doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing textDocument");
    }
    
    JsonValue* uri_val = json_object_get(text_doc, "uri");
    if (!uri_val || uri_val->type != JSON_STRING) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Missing document URI");
    }
    
    const char* uri = json_string_value(uri_val);
    LspTextDocument* doc = lsp_document_get(server, uri);
    if (!doc) {
        return lsp_create_error(id, LSP_ERROR_INVALID_PARAMS, "Document not found");
    }
    
    // 从 URI 提取文件路径
    char filepath[512];
    uri_to_filepath(uri, filepath, sizeof(filepath));
    
    // 获取诊断信息，传入文件名用于模块路径解析
    int count = 0;
    LspDiagnostic* diags = lsp_compile_and_get_errors_with_filename(doc->content, &count, filepath);
    
    // 构建响应
    JsonValue* result = json_object_new();
    JsonValue* items = json_array_new();
    
    for (int i = 0; i < count; i++) {
        JsonValue* diag = json_object_new();
        
        // 范围
        JsonValue* range = json_object_new();
        JsonValue* start = json_object_new();
        JsonValue* end = json_object_new();
        json_object_set(start, "line", json_int_new(diags[i].range.start.line));
        json_object_set(start, "character", json_int_new(diags[i].range.start.character));
        json_object_set(end, "line", json_int_new(diags[i].range.end.line));
        json_object_set(end, "character", json_int_new(diags[i].range.end.character));
        json_object_set(range, "start", start);
        json_object_set(range, "end", end);
        json_object_set(diag, "range", range);
        
        // 严重程度
        json_object_set(diag, "severity", json_int_new(diags[i].severity));
        
        // 代码
        if (diags[i].code) {
            json_object_set(diag, "code", json_string_new(diags[i].code));
        }
        
        // 来源
        json_object_set(diag, "source", json_string_new("leno"));
        
        // 消息
        json_object_set(diag, "message", json_string_new(diags[i].message));
        
        json_array_add(items, diag);
    }
    
    json_object_set(result, "items", items);
    
    // 版本信息 - 使用 document version
    json_object_set(result, "version", json_int_new(doc->version));
    
    // 完整文档标识
    json_object_set(result, "kind", json_string_new("full"));
    
    lsp_free_diagnostics(diags, count);
    
    return lsp_create_response(id, result);
}
