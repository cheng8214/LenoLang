/* Leno GUI - 日志系统（借鉴 SDL3 Log System）
 * 提供分级日志功能，支持类别和优先级
 */

#ifndef LENO_GUIS_LOG_H
#define LENO_GUIS_LOG_H

#include "leno_guis.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 日志类别（借鉴 SDL3）===== */
typedef enum {
    LENO_GUI_LOG_CATEGORY_APPLICATION = 0,  /* 应用程序 */
    LENO_GUI_LOG_CATEGORY_ERROR,            /* 错误 */
    LENO_GUI_LOG_CATEGORY_ASSERT,           /* 断言 */
    LENO_GUI_LOG_CATEGORY_SYSTEM,           /* 系统 */
    LENO_GUI_LOG_CATEGORY_VIDEO,            /* 视频 */
    LENO_GUI_LOG_CATEGORY_RENDER,           /* 渲染 */
    LENO_GUI_LOG_CATEGORY_INPUT,            /* 输入 */
    LENO_GUI_LOG_CATEGORY_EVENT,            /* 事件 */
    LENO_GUI_LOG_CATEGORY_WINDOW,           /* 窗口 */
    
    LENO_GUI_LOG_CATEGORY_CUSTOM            /* 自定义类别起始 */
} LenoGUILogCategory;

/* ===== 日志优先级（借鉴 SDL3）===== */
typedef enum {
    LENO_GUI_LOG_PRIORITY_INVALID = 0,
    LENO_GUI_LOG_PRIORITY_TRACE,      /* 追踪 - 最详细 */
    LENO_GUI_LOG_PRIORITY_VERBOSE,    /* 详细 */
    LENO_GUI_LOG_PRIORITY_DEBUG,      /* 调试 */
    LENO_GUI_LOG_PRIORITY_INFO,       /* 信息 */
    LENO_GUI_LOG_PRIORITY_WARN,       /* 警告 */
    LENO_GUI_LOG_PRIORITY_ERROR,      /* 错误 */
    LENO_GUI_LOG_PRIORITY_CRITICAL,   /* 严重 */
    LENO_GUI_LOG_PRIORITY_COUNT
} LenoGUILogPriority;

/* ===== 日志回调函数类型 ===== */
typedef void (*LenoGUILogCallback)(void* userdata, int category, int priority, const char* message);

/* ===== 日志系统 API ===== */

/* 初始化日志系统 */
void leno_gui_log_init(void);

/* 设置所有类别的日志优先级 */
void leno_gui_log_set_all_priority(int priority);

/* 设置指定类别的日志优先级 */
void leno_gui_log_set_priority(int category, int priority);

/* 获取指定类别的日志优先级 */
int leno_gui_log_get_priority(int category);

/* 设置日志回调函数（NULL 表示使用默认输出） */
void leno_gui_log_set_callback(LenoGUILogCallback callback, void* userdata);

/* 输出日志（使用 printf 格式） */
void leno_gui_log(int category, int priority, const char* fmt, ...);

/* 便捷函数 */
void leno_gui_log_trace(const char* fmt, ...);   /* 追踪 */
void leno_gui_log_debug(const char* fmt, ...);   /* 调试 */
void leno_gui_log_info(const char* fmt, ...);    /* 信息 */
void leno_gui_log_warn(const char* fmt, ...);    /* 警告 */
void leno_gui_log_error(const char* fmt, ...);   /* 错误 */
void leno_gui_log_critical(const char* fmt, ...);/* 严重 */

/* 按类别输出 */
void leno_gui_log_info_category(int category, const char* fmt, ...);
void leno_gui_log_error_category(int category, const char* fmt, ...);
void leno_gui_log_debug_category(int category, const char* fmt, ...);

/* 关闭日志系统 */
void leno_gui_log_quit(void);

#ifdef __cplusplus
}
#endif

#endif /* LENO_GUIS_LOG_H */
