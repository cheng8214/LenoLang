#include "include/lenolang.h"

// 全局错误收集器
ErrorCollector errors = {0};

// 当前文件名（用于错误报告）
static char current_filename[BUFFER_SMALL] = "";

void error_set_filename(const char* filename) {
    if (filename) {
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        current_filename[sizeof(current_filename) - 1] = '\0';
    } else {
        current_filename[0] = '\0';
    }
}

const char* error_get_filename(void) {
    return current_filename[0] ? current_filename : NULL;
}

void error_add(ErrorType type, int line, const char* msg) {
    if (errors.count >= MAX_ERRORS) {
        fprintf(stderr, "错误收集器已满，无法添加更多错误\n");
        return;
    }
    
    Error* err = &errors.list[errors.count++];
    err->type = type;
    err->line = line;
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
    
    fprintf(stderr, "\n=== 发现 %d 个错误 ===\n", errors.count);
    
    for (int i = 0; i < errors.count; i++) {
        Error* err = &errors.list[i];
        const char* type_str = "未知";
        
        switch (err->type) {
            case ERR_SYNTAX:        type_str = "语法错误"; break;
            case ERR_SEMANTIC:      type_str = "语义错误"; break;
            case ERR_UNDEFINED_VAR: type_str = "未定义变量"; break;
            case ERR_DUPLICATE_VAR: type_str = "重复定义"; break;
            case ERR_CLOSURE:       type_str = "闭包错误"; break;
            case ERR_RUNTIME:       type_str = "运行时错误"; break;
            case ERR_TYPE_MISMATCH: type_str = "类型不匹配"; break;
            default: break;
        }
        
        // 如果有文件名，显示文件名
        if (err->filename[0]) {
            fprintf(stderr, "[%s] %s 第 %d 行: %s\n", type_str, err->filename, err->line, err->msg);
        } else {
            fprintf(stderr, "[%s] 第 %d 行: %s\n", type_str, err->line, err->msg);
        }
    }
    
    fprintf(stderr, "===================\n\n");
}
