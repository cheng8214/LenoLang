/* Leno GUI - 日志系统实现（借鉴 SDL3 Log System）
 * 提供分级日志功能，支持类别和优先级
 */

#include "leno_guis_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

/* ===== 内部状态 ===== */

static int g_log_priorities[LENO_GUI_LOG_CATEGORY_CUSTOM];
static LenoGUILogCallback g_log_callback = NULL;
static void* g_log_userdata = NULL;
static int g_log_initialized = 0;

/* 优先级名称 */
static const char* g_priority_names[] = {
    "INVALID", "TRACE", "VERBOSE", "DEBUG", "INFO", "WARN", "ERROR", "CRITICAL"
};

/* 类别名称 */
static const char* g_category_names[] = {
    "APP", "ERROR", "ASSERT", "SYSTEM", "VIDEO", "RENDER", "INPUT", "EVENT", "WINDOW"
};

/* ===== 获取当前时间字符串 ===== */
static void get_timestamp(char* buf, size_t size) {
    time_t now;
    struct tm* timeinfo;
    time(&now);
    timeinfo = localtime(&now);
    strftime(buf, size, "%H:%M:%S", timeinfo);
}

/* ===== 默认日志输出 ===== */
static void default_log_output(int category, int priority, const char* message) {
    const char* priority_name = (priority >= 0 && priority < LENO_GUI_LOG_PRIORITY_COUNT) 
                                 ? g_priority_names[priority] : "UNKNOWN";
    const char* category_name = (category >= 0 && category < LENO_GUI_LOG_CATEGORY_CUSTOM) 
                                 ? g_category_names[category] : "CUSTOM";
    
    char timestamp[16];
    get_timestamp(timestamp, sizeof(timestamp));
    
    FILE* output = (priority >= LENO_GUI_LOG_PRIORITY_ERROR) ? stderr : stdout;
    
    fprintf(output, "[%s][%s][%s] %s\n", timestamp, category_name, priority_name, message);
    fflush(output);
    
#ifdef _WIN32
    /* Windows 调试输出 */
    if (IsDebuggerPresent()) {
        char debug_msg[1024];
        snprintf(debug_msg, sizeof(debug_msg), "[%.6s][%.8s][%.8s] %.800s\n", 
                 timestamp, category_name, priority_name, message);
        OutputDebugStringA(debug_msg);
    }
#endif
}

/* ===== 初始化日志系统 ===== */
void leno_gui_log_init(void) {
    if (g_log_initialized) return;
    
    /* 设置默认优先级 */
    /* 默认：app=info, error=warn, 其他=error */
    for (int i = 0; i < LENO_GUI_LOG_CATEGORY_CUSTOM; i++) {
        g_log_priorities[i] = LENO_GUI_LOG_PRIORITY_ERROR;
    }
    g_log_priorities[LENO_GUI_LOG_CATEGORY_APPLICATION] = LENO_GUI_LOG_PRIORITY_INFO;
    g_log_priorities[LENO_GUI_LOG_CATEGORY_ERROR] = LENO_GUI_LOG_PRIORITY_WARN;
    
    g_log_callback = NULL;
    g_log_userdata = NULL;
    g_log_initialized = 1;
}

/* ===== 设置所有类别的日志优先级 ===== */
void leno_gui_log_set_all_priority(int priority) {
    if (!g_log_initialized) leno_gui_log_init();
    if (priority <= LENO_GUI_LOG_PRIORITY_INVALID || priority >= LENO_GUI_LOG_PRIORITY_COUNT) return;
    
    for (int i = 0; i < LENO_GUI_LOG_CATEGORY_CUSTOM; i++) {
        g_log_priorities[i] = priority;
    }
}

/* ===== 设置指定类别的日志优先级 ===== */
void leno_gui_log_set_priority(int category, int priority) {
    if (!g_log_initialized) leno_gui_log_init();
    if (category < 0 || category >= LENO_GUI_LOG_CATEGORY_CUSTOM) return;
    if (priority <= LENO_GUI_LOG_PRIORITY_INVALID || priority >= LENO_GUI_LOG_PRIORITY_COUNT) return;
    
    g_log_priorities[category] = priority;
}

/* ===== 获取指定类别的日志优先级 ===== */
int leno_gui_log_get_priority(int category) {
    if (!g_log_initialized) leno_gui_log_init();
    if (category < 0 || category >= LENO_GUI_LOG_CATEGORY_CUSTOM) 
        return LENO_GUI_LOG_PRIORITY_ERROR;
    return g_log_priorities[category];
}

/* ===== 设置日志回调函数 ===== */
void leno_gui_log_set_callback(LenoGUILogCallback callback, void* userdata) {
    if (!g_log_initialized) leno_gui_log_init();
    g_log_callback = callback;
    g_log_userdata = userdata;
}

/* ===== 输出日志（内部实现） ===== */
static void log_message(int category, int priority, const char* fmt, va_list args) {
    if (!g_log_initialized) leno_gui_log_init();
    if (category < 0 || category >= LENO_GUI_LOG_CATEGORY_CUSTOM) category = LENO_GUI_LOG_CATEGORY_APPLICATION;
    if (priority <= LENO_GUI_LOG_PRIORITY_INVALID || priority >= LENO_GUI_LOG_PRIORITY_COUNT) return;
    
    /* 检查优先级 */
    if (priority < g_log_priorities[category]) return;
    
    /* 格式化消息 */
    char message[2048];
    vsnprintf(message, sizeof(message), fmt, args);
    message[sizeof(message) - 1] = '\0';
    
    /* 输出 */
    if (g_log_callback) {
        g_log_callback(g_log_userdata, category, priority, message);
    } else {
        default_log_output(category, priority, message);
    }
}

/* ===== 输出日志（使用 printf 格式） ===== */
void leno_gui_log(int category, int priority, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(category, priority, fmt, args);
    va_end(args);
}

/* ===== 便捷函数 ===== */
void leno_gui_log_trace(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_TRACE, fmt, args);
    va_end(args);
}

void leno_gui_log_debug(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_DEBUG, fmt, args);
    va_end(args);
}

void leno_gui_log_info(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_INFO, fmt, args);
    va_end(args);
}

void leno_gui_log_warn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_WARN, fmt, args);
    va_end(args);
}

void leno_gui_log_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_ERROR, fmt, args);
    va_end(args);
}

void leno_gui_log_critical(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(LENO_GUI_LOG_CATEGORY_APPLICATION, LENO_GUI_LOG_PRIORITY_CRITICAL, fmt, args);
    va_end(args);
}

/* ===== 按类别输出 ===== */
void leno_gui_log_info_category(int category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(category, LENO_GUI_LOG_PRIORITY_INFO, fmt, args);
    va_end(args);
}

void leno_gui_log_error_category(int category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(category, LENO_GUI_LOG_PRIORITY_ERROR, fmt, args);
    va_end(args);
}

void leno_gui_log_debug_category(int category, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    log_message(category, LENO_GUI_LOG_PRIORITY_DEBUG, fmt, args);
    va_end(args);
}

/* ===== 关闭日志系统 ===== */
void leno_gui_log_quit(void) {
    if (!g_log_initialized) return;
    g_log_callback = NULL;
    g_log_userdata = NULL;
    g_log_initialized = 0;
}
