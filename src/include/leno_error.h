#ifndef LENO_ERROR_H
#define LENO_ERROR_H

#include "leno_types.h"

// ============================================================================
// 错误系统（全程不崩溃・可收集）
// ============================================================================

typedef struct {
    ErrorType type;
    int line;
    int column;                   // 列号（-1 表示未知）
    char filename[BUFFER_SMALL];  // 文件名
    char msg[BUFFER_XXLARGE];  // 扩大缓冲区以支持详细报错信息
    int repeat_count;             // 重复次数（相同错误合并）
} Error;

typedef struct {
    Error list[MAX_ERRORS];
    int count;
} ErrorCollector;

extern ErrorCollector errors;

void error_set_filename(const char* filename);
void error_set_column(int column);
const char* error_get_filename(void);
void error_add(ErrorType type, int line, const char* msg);
int error_has_any(void);
void error_clear(void);
void error_print_all(void);

// ============================================================================
// 警告系统（"担心系统"）：默认开启，收集后打印，但不阻断编译
// ============================================================================

#ifndef MAX_WARNINGS
#define MAX_WARNINGS 256
#endif

typedef struct {
    WarnType type;
    int line;
    int column;                   // 列号（-1 表示未知）
    char filename[BUFFER_SMALL];  // 文件名
    char msg[BUFFER_XXLARGE];  // 扩大缓冲区以支持详细警告信息
    int repeat_count;             // 重复次数（相同警告合并）
} Warning;

typedef struct {
    Warning list[MAX_WARNINGS];
    int count;
} WarningCollector;

extern WarningCollector warnings;

void warning_add(WarnType type, int line, const char* msg);
int warning_has_any(void);
void warning_clear(void);
void warning_print_all(void);

#endif // LENO_ERROR_H
