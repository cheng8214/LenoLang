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

int error_get_column(void) {
    return current_column;
}

const char* error_get_filename(void) {
    return current_filename[0] ? current_filename : NULL;
}

// ============================================================================
// 静默段（error_silence_begin / error_silence_end）
// ----------------------------------------------------------------------------
// 用途：符号表扫描器在**扫描别人的模块**时要借真解析器求值一段表达式（见 docs 的 Phase 1）。
// 那段解析若失败，诊断绝不能进当前编译的错误列表：一是归因错（错的不是当前文件），
// 二是扫描器对求值失败本来就是宽松处理（当作"无显式值"），该报的错留给模块自身编译时。
// 进入静默段后 error_add / warning_add 一律丢弃并计数；error_silence_end 返回**被丢弃的
// 错误条数**，调用方据此判断"这段解析是否干净"（>0 ⇒ 结果不可信）。支持嵌套，最外层归零。
// ============================================================================
static int silence_depth = 0;
static int silenced_errors = 0;
static int silenced_warnings = 0;

int error_silence_begin(void) {
    silence_depth++;
    return 0;
}

int error_silence_end(void) {
    if (silence_depth > 0) silence_depth--;
    int silenced = silenced_errors;
    if (silence_depth == 0) {
        silenced_errors = 0;
        silenced_warnings = 0;
    }
    return silenced;
}

void error_add(ErrorType type, int line, const char* msg) {
    if (silence_depth > 0) { silenced_errors++; return; }
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
    err->printed = 0;
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

void error_add_at(ErrorType type, int line, int column, const char* msg) {
    int saved_column = current_column;
    current_column = column;
    error_add(type, line, msg);
    current_column = saved_column;
}

int error_has_any(void) {
    return errors.count > 0;
}

int error_count(void) {
    return errors.count;
}

void error_clear(void) {
    errors.count = 0;
}

void error_print_all(void) {
    if (errors.count == 0) return;

    // 计算实际错误总数（含重复）；已即时打印过的运行期错误不重复展示（S2）
    int total = 0;
    for (int i = 0; i < errors.count; i++) {
        if (!errors.list[i].printed) {
            total += errors.list[i].repeat_count;
        }
    }
    if (total == 0) return;

    int shown_kinds = 0;
    for (int i = 0; i < errors.count; i++) {
        if (!errors.list[i].printed) shown_kinds++;
    }

    fprintf(stderr, "\n=== 发现 %d 个错误", total);
    if (total > shown_kinds) {
        fprintf(stderr, "（%d 种，已合并重复）", shown_kinds);
    }
    fprintf(stderr, " ===\n");

    for (int i = 0; i < errors.count; i++) {
        Error* err = &errors.list[i];
        if (err->printed) continue;
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
        
        // 输出格式兼容 VSCode/GCC 终端链接（ctrl+点击跳转）
        // 标准格式: filename(line,column): error: message
        // 无列号时: filename(line): error: message
        // 无文件名时退化为简单格式
        if (err->filename[0] && err->column > 0) {
            fprintf(stderr, "%s(%d,%d): error: [%s] %s",
                    err->filename, err->line, err->column, type_str, err->msg);
        } else if (err->filename[0]) {
            fprintf(stderr, "%s(%d): error: [%s] %s",
                    err->filename, err->line, type_str, err->msg);
        } else if (err->column > 0) {
            fprintf(stderr, "<stdin>:%d:%d: error: [%s] %s",
                    err->line, err->column, type_str, err->msg);
        } else {
            fprintf(stderr, "<stdin>:%d: error: [%s] %s", err->line, type_str, err->msg);
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
    if (silence_depth > 0) { silenced_warnings++; return; }
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

void warning_add_at(WarnType type, int line, int column, const char* msg) {
    int saved_column = current_column;
    current_column = column;
    warning_add(type, line, msg);
    current_column = saved_column;
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
            case WARN_BAD_ESCAPE:      type_str = "无效转义"; break;
            case WARN_IMPLICIT_TRUNC:  type_str = "隐式截断"; break;
            case WARN_UNREACHABLE:     type_str = "不可达代码"; break;
            case WARN_NULL_FIELD_CHAIN: type_str = "空值链式访问"; break;
            case WARN_STRUCT_EQ_NULL:  type_str = "struct与null比较"; break;
            case WARN_OR_TYPE_GUARD:   type_str = "or类型守卫"; break;
            case WARN_GENERIC_NO_CONSTRAINT: type_str = "泛型无约束"; break;
            case WARN_NULLABLE_ARITH:  type_str = "可空类型运算"; break;
            case WARN_EMPTY_SOURCE:    type_str = "空源文件"; break;
            case WARN_ASSIGN_IN_COND:  type_str = "条件中的赋值"; break;
            case WARN_FOR_IN_COND:     type_str = "for头成员测试"; break;
            case WARN_PARTIAL_DECL_INIT: type_str = "部分初值声明"; break;
            case WARN_FIELD_NO_INIT:   type_str = "字段未初始化"; break;
            default: break;
        }

        // 输出格式兼容 VSCode/GCC 终端链接（ctrl+点击跳转）
        // 标准格式: filename(line,column): warning: message
        if (w->filename[0] && w->column > 0) {
            fprintf(stderr, "%s(%d,%d): warning: [%s] %s",
                    w->filename, w->line, w->column, type_str, w->msg);
        } else if (w->filename[0]) {
            fprintf(stderr, "%s(%d): warning: [%s] %s",
                    w->filename, w->line, type_str, w->msg);
        } else if (w->column > 0) {
            fprintf(stderr, "<stdin>:%d:%d: warning: [%s] %s",
                    w->line, w->column, type_str, w->msg);
        } else {
            fprintf(stderr, "<stdin>:%d: warning: [%s] %s", w->line, type_str, w->msg);
        }

        if (w->repeat_count > 1) {
            fprintf(stderr, " (重复 %d 次)", w->repeat_count);
        }
        fprintf(stderr, "\n");
    }

    fprintf(stderr, "=====================\n\n");
}
