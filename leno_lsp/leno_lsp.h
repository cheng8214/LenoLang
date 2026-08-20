/**
 * Leno Language Server Protocol (LSP) Implementation
 * 
 * 复用 LenoC 编译器组件提供 IDE 支持
 */

#ifndef LENO_LSP_H
#define LENO_LSP_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <windows.h>
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

// LSP 版本
#define LSP_VERSION "1.0.0"
#define LSP_PROTOCOL_VERSION "3.17.0"

// LSP 错误码
#define LSP_ERROR_PARSE_ERROR -32700
#define LSP_ERROR_INVALID_REQUEST -32600
#define LSP_ERROR_METHOD_NOT_FOUND -32601
#define LSP_ERROR_INVALID_PARAMS -32602
#define LSP_ERROR_INTERNAL_ERROR -32603
#define LSP_ERROR_SERVER_NOT_INITIALIZED -32002

// 日志级别
typedef enum {
    LSP_LOG_ERROR = 0,
    LSP_LOG_WARN = 1,
    LSP_LOG_INFO = 2,
    LSP_LOG_DEBUG = 3
} LspLogLevel;

// LSP 服务器状态
typedef enum {
    LSP_STATE_UNINITIALIZED = 0,
    LSP_STATE_INITIALIZING = 1,
    LSP_STATE_INITIALIZED = 2,
    LSP_STATE_SHUTDOWN = 3
} LspServerState;

// 位置信息 (0-based)
typedef struct {
    uint32_t line;
    uint32_t character;
} LspPosition;

// 范围信息
typedef struct {
    LspPosition start;
    LspPosition end;
} LspRange;

// 文档位置
typedef struct {
    char* uri;
    LspRange range;
} LspLocation;

// 诊断严重程度
typedef enum {
    LSP_DIAG_ERROR = 1,
    LSP_DIAG_WARNING = 2,
    LSP_DIAG_INFORMATION = 3,
    LSP_DIAG_HINT = 4
} LspDiagnosticSeverity;

// 诊断信息
typedef struct {
    LspRange range;
    LspDiagnosticSeverity severity;
    char* code;
    char* source;
    char* message;
} LspDiagnostic;

// 补全项类型
typedef enum {
	LSP_COMP_TEXT = 1,
	LSP_COMP_METHOD = 2,
	LSP_COMP_FUNCTION = 3,
	LSP_COMP_CONSTRUCTOR = 4,
	LSP_COMP_FIELD = 5,
	LSP_COMP_VARIABLE = 6,
	LSP_COMP_CLASS = 7,
	LSP_COMP_INTERFACE = 8,
	LSP_COMP_MODULE = 9,
	LSP_COMP_PROPERTY = 10,
	LSP_COMP_UNIT = 11,
	LSP_COMP_VALUE = 12,
	LSP_COMP_ENUM = 13,
	LSP_COMP_KEYWORD = 14,
	LSP_COMP_SNIPPET = 15,
	LSP_COMP_COLOR = 16,
	LSP_COMP_FILE = 17,
	LSP_COMP_REFERENCE = 18,
	LSP_COMP_STRUCT = 19,
	LSP_COMP_ENUM_MEMBER = 20,
	LSP_COMP_CONSTANT = 21       // 模块常量
} LspCompletionItemKind;

// 补全项
typedef struct {
    char* label;
    LspCompletionItemKind kind;
    char* detail;
    char* documentation;
    char* insertText;
} LspCompletionItem;

// 打开的文本文档
typedef struct LspTextDocument {
    char* uri;
    char* content;
    int version;
    struct LspTextDocument* next;
} LspTextDocument;

/* LSP 服务器上下文 */
typedef struct {
    LspServerState state;
    int client_pid;
    char* root_path;
    LspTextDocument* documents;
    
    /* 能力配置 */
    bool completion_provider;
    bool hover_provider;
    bool definition_provider;
    bool diagnostic_provider;
    
    /* 日志 */
    LspLogLevel log_level;
    FILE* log_file;
} LspServer;

// ==================== JSON 解析器 ====================

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;
typedef struct JsonMember JsonMember;

struct JsonValue {
    JsonType type;
    union {
        bool bool_val;
        double number_val;
        char* string_val;
        struct {
            JsonValue** items;
            int count;
        } array_val;
        struct {
            JsonMember* members;
            int count;
        } object_val;
    } data;
};

struct JsonMember {
    char* key;
    JsonValue* value;
};

// JSON 解析
JsonValue* json_parse(const char* text);
void json_free(JsonValue* value);
const char* json_stringify(JsonValue* value);
JsonValue* json_deep_copy(JsonValue* value);

// JSON 访问辅助函数
JsonValue* json_object_get(JsonValue* obj, const char* key);
const char* json_string_value(JsonValue* val);
int json_int_value(JsonValue* val);
bool json_bool_value(JsonValue* val);

// JSON 创建辅助函数（在 lsp_protocol.c 中实现）
JsonValue* json_object_new(void);
JsonValue* json_string_new(const char* str);
JsonValue* json_int_new(int n);
JsonValue* json_bool_new(bool b);
JsonValue* json_array_new(void);
void json_object_set(JsonValue* obj, const char* key, JsonValue* val);
void json_array_add(JsonValue* arr, JsonValue* val);

// ==================== LSP 协议 ====================

// 消息处理
char* lsp_handle_message(LspServer* server, const char* message);
char* lsp_create_response(int id, JsonValue* result);
char* lsp_create_notification(const char* method, JsonValue* params);
char* lsp_create_error(int id, int code, const char* message);

// 方法处理
char* lsp_handle_initialize(LspServer* server, int id, JsonValue* params);
char* lsp_handle_shutdown(LspServer* server, int id);
char* lsp_handle_exit(LspServer* server);

// 文档同步
char* lsp_handle_did_open(LspServer* server, JsonValue* params);
char* lsp_handle_did_change(LspServer* server, JsonValue* params);
char* lsp_handle_did_close(LspServer* server, JsonValue* params);

// ==================== 诊断服务 ====================

// 诊断
void lsp_publish_diagnostics(LspServer* server, const char* uri);
LspDiagnostic* lsp_compile_and_get_errors(const char* content, int* count);
LspDiagnostic* lsp_compile_and_get_errors_with_filename(const char* content, int* count, const char* filename);
void lsp_free_diagnostics(LspDiagnostic* diags, int count);
char* lsp_handle_document_diagnostic(LspServer* server, int id, JsonValue* params);

// ==================== 补全服务 ====================

// 补全
char* lsp_handle_completion(LspServer* server, int id, JsonValue* params);
LspCompletionItem* lsp_get_completions(const char* content, LspPosition pos, int* count, const char* file_path);
void lsp_free_completions(LspCompletionItem* items, int count);

// ==================== 悬停服务 ====================

// 悬停
char* lsp_handle_hover(LspServer* server, int id, JsonValue* params);
char* lsp_get_hover_info(const char* content, LspPosition pos, const char* file_path);

// ==================== 定义跳转 ====================

// 跳转定义
char* lsp_handle_definition(LspServer* server, int id, JsonValue* params);
LspLocation* lsp_get_definition(const char* content, LspPosition pos, int* count, const char* uri);
LspLocation* lsp_find_definition_by_word(const char* content, const char* word,
                                         const char* current_file, int* count);
void lsp_free_locations(LspLocation* locs, int count);

// ==================== 文档管理 ====================

// 文本文档管理
LspTextDocument* lsp_document_open(LspServer* server, const char* uri, 
                                   const char* content, int version);
void lsp_document_update(LspServer* server, const char* uri, 
                         const char* content, int version);
void lsp_document_update_incremental(LspServer* server, const char* uri,
                                      int start_line, int start_char,
                                      int end_line, int end_char,
                                      const char* new_text, int version);
void lsp_document_close(LspServer* server, const char* uri);
LspTextDocument* lsp_document_get(LspServer* server, const char* uri);

// 位置转换工具
int lsp_position_to_offset(const char* content, LspPosition pos);
LspPosition lsp_offset_to_position(const char* content, int offset);
char* lsp_get_line_content(const char* content, int line);

// 文本处理工具
char* get_word_at_position(const char* content, LspPosition pos);

// ==================== 服务器管理 ====================

// 服务器生命周期
LspServer* lsp_server_create(void);
void lsp_server_destroy(LspServer* server);
int lsp_server_run(LspServer* server);
void lsp_server_stop(LspServer* server);

// 日志
void lsp_log(LspServer* server, LspLogLevel level, const char* fmt, ...);

// 工具函数
char* lsp_read_message(FILE* stream);
void lsp_write_message(FILE* stream, const char* message);
char* lsp_uri_to_path(const char* uri);
char* lsp_path_to_uri(const char* path);

#endif // LENO_LSP_H
