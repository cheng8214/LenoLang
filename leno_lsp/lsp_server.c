/**
 * LSP 服务器主程序
 * 管理服务器生命周期和消息循环
 */

#include "leno_lsp.h"
#include "../src/include/native.h"
#include <signal.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

// LenoC 需要的全局变量
int debugMode = 0;
int g_argc = 0;
char** g_argv = NULL;

static volatile int running = 1;

// 信号处理
static void signal_handler(int sig) {
    (void)sig;  // 抑制未使用参数警告
    running = 0;
}

// 创建服务器
LspServer* lsp_server_create(void) {
    LspServer* server = (LspServer*)calloc(1, sizeof(LspServer));
    if (!server) return NULL;
    
    server->state = LSP_STATE_UNINITIALIZED;
    server->log_level = LSP_LOG_INFO;
    server->completion_provider = true;
    server->hover_provider = true;
    server->definition_provider = true;
    server->diagnostic_provider = true;
    
    // 日志文件已禁用
    server->log_file = NULL;
    
    fprintf(stderr, "[LSP] Leno LSP Server v%s starting...\n", LSP_VERSION);

    // 初始化 GC（实例方法注册需要）
    gc_init();
    lsp_log(server, LSP_LOG_INFO, "GC initialized");

    // 初始化实例方法元数据（只需初始化一次）
    native_register_all_instance_method_metas();
    lsp_log(server, LSP_LOG_INFO, "Instance method metas initialized");

    return server;
}

// 销毁服务器
void lsp_server_destroy(LspServer* server) {
    if (!server) return;
    
    lsp_log(server, LSP_LOG_INFO, "Server shutting down...");
    
    // 关闭所有文档
    LspTextDocument* doc = server->documents;
    while (doc) {
        LspTextDocument* next = doc->next;
        free(doc->uri);
        free(doc->content);
        free(doc);
        doc = next;
    }
    
    free(server->root_path);
    
    // 日志文件已禁用，无需关闭
    
    free(server);
}

// 日志记录 - 输出到 stderr
void lsp_log(LspServer* server, LspLogLevel level, const char* fmt, ...) {
    if (!server || level > server->log_level) return;
    
    const char* level_str[] = {"ERROR", "WARN", "INFO", "DEBUG"};
    
    fprintf(stderr, "[%s] ", level_str[level]);
    
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    
    fprintf(stderr, "\n");
}

// 运行服务器主循环
int lsp_server_run(LspServer* server) {
    if (!server) return 1;
    
    // 设置信号处理
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    #ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // 忽略管道破裂信号
    #endif
    
    // 设置 stdin/stdout 为二进制模式（Windows）
    #ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    #endif
    
    lsp_log(server, LSP_LOG_INFO, "Server ready, waiting for connections...");
    fprintf(stderr, "[LSP] Server started, waiting for messages...\n");
    fflush(stderr);
    
    // 主消息循环
    while (running && server->state != LSP_STATE_SHUTDOWN) {
        fprintf(stderr, "[LSP] Waiting for message...\n");
        fflush(stderr);
        
        char* message = lsp_read_message(stdin);
        
        if (!message) {
            fprintf(stderr, "[LSP] lsp_read_message returned NULL\n");
            fflush(stderr);
            if (feof(stdin)) {
                lsp_log(server, LSP_LOG_INFO, "EOF received, exiting...");
                fprintf(stderr, "[LSP] EOF received, exiting...\n");
                break;
            }
            continue;
        }
        
        fprintf(stderr, "[LSP] Received message: %.100s...\n", message);
        fflush(stderr);
        
        lsp_log(server, LSP_LOG_DEBUG, "Received: %s", message);
        
        char* response = lsp_handle_message(server, message);
        free(message);
        
        if (response) {
            fprintf(stderr, "[LSP] Sending response: %.100s...\n", response);
            fflush(stderr);
            lsp_log(server, LSP_LOG_DEBUG, "Sending: %s", response);
            lsp_write_message(stdout, response);
            free(response);
        } else if (server->state == LSP_STATE_SHUTDOWN) {
            // exit 通知
            fprintf(stderr, "[LSP] Shutdown requested\n");
            break;
        }
    }
    
    return 0;
}

// 停止服务器
void lsp_server_stop(LspServer* server) {
    (void)server;  // 抑制未使用参数警告
    running = 0;
}

// 主函数
int main(int argc, char* argv[]) {
    // 检查命令行参数
    bool stdio_mode = true;
    int port = 0;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[i + 1]);
            stdio_mode = false;
            i++;
        }
        else if (strcmp(argv[i], "--help") == 0) {
            printf("Leno Language Server Protocol (LSP) Implementation\n");
            printf("Usage: %s [options]\n", argv[0]);
            printf("Options:\n");
            printf("  --port <port>  Listen on TCP port instead of stdin/stdout\n");
            printf("  --help         Show this help message\n");
            printf("  --version      Show version information\n");
            return 0;
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("Leno LSP Server v%s (Protocol v%s)\n", 
                   LSP_VERSION, LSP_PROTOCOL_VERSION);
            return 0;
        }
    }
    
    // 创建服务器
    LspServer* server = lsp_server_create();
    if (!server) {
        fprintf(stderr, "Failed to create server\n");
        return 1;
    }
    
    int result = 0;
    
    if (stdio_mode) {
        // 标准输入输出模式（VS Code 默认）
        result = lsp_server_run(server);
    } else {
        // TCP 模式（可选）
        fprintf(stderr, "TCP mode not yet implemented (port: %d)\n", port);
        result = 1;
    }
    
    lsp_server_destroy(server);
    return result;
}
