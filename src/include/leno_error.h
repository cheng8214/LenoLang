#ifndef LENO_ERROR_H
#define LENO_ERROR_H

#include "leno_types.h"

// ============================================================================
// 错误系统（全程不崩溃・可收集）
// ============================================================================

typedef struct {
    ErrorType type;
    int line;
    char filename[BUFFER_SMALL];  // 文件名
    char msg[BUFFER_MEDIUM];
    int repeat_count;             // 重复次数（相同错误合并）
} Error;

typedef struct {
    Error list[MAX_ERRORS];
    int count;
} ErrorCollector;

extern ErrorCollector errors;

void error_set_filename(const char* filename);
const char* error_get_filename(void);
void error_add(ErrorType type, int line, const char* msg);
int error_has_any(void);
void error_clear(void);
void error_print_all(void);

#endif // LENO_ERROR_H
