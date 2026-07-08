#include "include/lenolang.h"

// 全局错误收集器
ErrorCollector errors = {0};
// 全局警告收集器（"担心系统"）
WarningCollector warnings = {0};

// 当前文件名（用于错误报告）
static char current_filename[BUFFER_SMALL] = "";
// 当前列号（词法分析时更新，-1 表示未知）
static int current_column = -1;

void error_set_filename(const char* filename) {
    if (filename) {
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        current_filename[sizeof(current_filename) - 1] = '\0';
    } else {
        current_filename[0] = '\0';
    }
}

void error_set_column(int column) {
    current_column = column;
}

const char* error_get_filename(void) {
    return current_filename[0] ? current_filename : NULL;
}

void error_add(ErrorType type, int line, const char* msg) {
    // 检查是否与最后一条错误相同（同类型、同文件、同行、同消息），相同则合并
    if (errors.count > 0) {
        Error* last = &errors.list[errors.count - 1];
        if (last->type == type && last->line == line &&
            strcmp(last->msg, msg) == 0 && last->filename[0] != '\0') {
            // 比较文件名
            const char* fname = current_filename[0] ? current_filename : "";
            if (strcmp(last->filename, fname) == 0) {
                last->repeat_count++;
                return;
            }
        }
    }

    if (errors.count >= MAX_ERRORS) {
        fprintf(stderr, "错误收集器已满，无法添加更多错误\n");
        return;
    }
    
    Error* err = &errors.list[errors.count++];
    err->type = type;
    err->line = line;
    err->column = current_column;
    err->repeat_count = 1;
    strncpy(err->msg, msg, sizeof(err->msg) - 1);
    err->msg[sizeof(err->msg) - 1] = '\0';
    
    // 保存当前文件名
    if (current_filename[0]) {
        size_t len = strlen(current_filename);
        if (len > sizeof(err->filename) - 1) {
            len = sizeof(err->filename) - 1;
        }
        memcpy(err->filename, current_filename, len);
        err->filename[len] = '\0';
    } else {
        err->filename[0] = '\0';
    }
}

int error_has_any(void) {
    return errors.count > 0;
}

void error_clear(void) {
    errors.count = 0;
}

void error_print_all(void) {
    if (errors.count == 0) return;
    
    // 计算实际错误总数（含重复）
    int total = 0;
    for (int i = 0; i < errors.count; i++) {
        total += errors.list[i].repeat_count;
    }
    
    fprintf(stderr, "\n=== 发现 %d 个错误", total);
    if (total > errors.count) {
        fprintf(stderr, "（%d 种，已合并重复）", errors.count);
    }
    fprintf(stderr, " ===\n");
    
    for (int i = 0; i < errors.count; i++) {
        Error* err = &errors.list[i];
        const char* type_str = "未知";
        
        switch (err->type) {
            case ERR_SYNTAX:        type_str = "语法错误"; break;
            case ERR_SEMANTIC:      type_str = "语义错误"; break;
            case ERR_UNDEFINED_VAR: type_str = "未定义变量"; break;
            case ERR_UNDEFINED_FUNC:type_str = "未定义函数"; break;
            case ERR_DUPLICATE_VAR: type_str = "重复定义"; break;
            case ERR_RUNTIME:       type_str = "运行时错误"; break;
            case ERR_TYPE_MISMATCH: type_str = "类型不匹配"; break;
            default: break;
        }
        
        // 如果有文件名，显示文件名；如果有列号，显示列号
        if (err->filename[0] && err->column > 0) {
            fprintf(stderr, "[%s] %s 第 %d 行第 %d 列: %s",
                    type_str, err->filename, err->line, err->column, err->msg);
        } else if (err->filename[0]) {
            fprintf(stderr, "[%s] %s 第 %d 行: %s",
                    type_str, err->filename, err->line, err->msg);
        } else if (err->column > 0) {
            fprintf(stderr, "[%s] 第 %d 行第 %d 列: %s",
                    type_str, err->line, err->column, err->msg);
        } else {
            fprintf(stderr, "[%s] 第 %d 行: %s", type_str, err->line, err->msg);
        }
        
        // 如果有重复，显示重复次数
        if (err->repeat_count > 1) {
            fprintf(stderr, " (重复 %d 次)", err->repeat_count);
        }
        fprintf(stderr, "\n");
    }
    
    fprintf(stderr, "===================\n\n");
}

// ============================================================================
// 警告系统（"担心系统"）
// ============================================================================

void warning_add(WarnType type, int line, const char* msg) {
    // 合并相同警告（同类型、同文件、同行、同消息）
    if (warnings.count > 0) {
        Warning* last = &warnings.list[warnings.count - 1];
        if (last->type == type && last->line == line &&
            strcmp(last->msg, msg) == 0 && last->filename[0] != '\0') {
            const char* fname = current_filename[0] ? current_filename : "";
            if (strcmp(last->filename, fname) == 0) {
                last->repeat_count++;
                return;
            }
        }
    }

    if (warnings.count >= MAX_WARNINGS) {
        return;
    }

    Warning* w = &warnings.list[warnings.count++];
    w->type = type;
    w->line = line;
    w->column = current_column;
    w->repeat_count = 1;
    strncpy(w->msg, msg, sizeof(w->msg) - 1);
    w->msg[sizeof(w->msg) - 1] = '\0';

    if (current_filename[0]) {
        size_t len = strlen(current_filename);
        if (len > sizeof(w->filename) - 1) {
            len = sizeof(w->filename) - 1;
        }
        memcpy(w->filename, current_filename, len);
        w->filename[len] = '\0';
    } else {
        w->filename[0] = '\0';
    }
}

int warning_has_any(void) {
    return warnings.count > 0;
}

void warning_clear(void) {
    warnings.count = 0;
}

void warning_print_all(void) {
    if (warnings.count == 0) return;

    int total = 0;
    for (int i = 0; i < warnings.count; i++) {
        total += warnings.list[i].repeat_count;
    }

    fprintf(stderr, "\n=== 发现 %d 个警告", total);
    if (total > warnings.count) {
        fprintf(stderr, "（%d 种，已合并重复）", warnings.count);
    }
    fprintf(stderr, " ===\n");

    for (int i = 0; i < warnings.count; i++) {
        Warning* w = &warnings.list[i];
        const char* type_str = "警告";

        switch (w->type) {
            case WARN_FOR_EMPTY_RANGE: type_str = "空循环"; break;
            case WARN_UNUSED_VAR:      type_str = "未使用变量"; break;
            case WARN_SHADOW_VAR:      type_str = "变量遮蔽"; break;
            case WARN_DEPRECATED:      type_str = "弃用"; break;
            case WARN_IMPLICIT_TRUNC:  type_str = "隐式截断"; break;
            case WARN_UNREACHABLE:     type_str = "不可达代码"; break;
            default: break;
        }

        if (w->filename[0] && w->column > 0) {
            fprintf(stderr, "[%s] %s 第 %d 行第 %d 列: %s",
                    type_str, w->filename, w->line, w->column, w->msg);
        } else if (w->filename[0]) {
            fprintf(stderr, "[%s] %s 第 %d 行: %s",
                    type_str, w->filename, w->line, w->msg);
        } else if (w->column > 0) {
            fprintf(stderr, "[%s] 第 %d 行第 %d 列: %s",
                    type_str, w->line, w->column, w->msg);
        } else {
            fprintf(stderr, "[%s] 第 %d 行: %s", type_str, w->line, w->msg);
        }

        if (w->repeat_count > 1) {
            fprintf(stderr, " (重复 %d 次)", w->repeat_count);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "=====================\n\n");
}
