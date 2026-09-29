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

/* T20：同"类"判定 —— 类型 + 文件名 + 消息全同 ⇒ 同一根因在不同行的复现。
 * 与 error_add 的即时合并（error.c:64-75）相比只少了"同行号"这一条：
 * 行号一换就不再算重复，所以级联时报出的条数 ≈ 根因数 × 行数。
 * 归并只发生在**打印层**（收集器语义、error_count() 契约都不动）。 */
static int error_same_kind(const Error* a, const Error* b) {
    return a->type == b->type
        && a->filename[0] != '\0'
        && strcmp(a->filename, b->filename) == 0
        && strcmp(a->msg, b->msg) == 0;
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

    // 展示条数 = 归并后的"类"数（T20：同一根因跨行的复现算一类）
    int shown_kinds = 0;
    for (int i = 0; i < errors.count; i++) {
        if (errors.list[i].printed) continue;
        int is_head = 1;
        for (int j = 0; j < i; j++) {
            if (errors.list[j].printed) continue;
            if (error_same_kind(&errors.list[j], &errors.list[i])) { is_head = 0; break; }
        }
        if (is_head) shown_kinds++;
    }

    fprintf(stderr, "\n=== 发现 %d 个错误", total);
    if (total > shown_kinds) {
        fprintf(stderr, "（%d 种，已合并重复）", shown_kinds);
    }
    fprintf(stderr, " ===\n");

    for (int i = 0; i < errors.count; i++) {
        Error* err = &errors.list[i];
        if (err->printed) continue;

        /* T20：同一根因只在**首次出现处**打完整信息，其余位置收进下面那行「同类另有 N 处」。
         * 只看未打印的条目 —— S2 已即时打印过的运行期错误不参与归并。 */
        int is_head = 1;
        for (int j = 0; j < i; j++) {
            if (errors.list[j].printed) continue;
            if (error_same_kind(&errors.list[j], err)) { is_head = 0; break; }
        }
        if (!is_head) continue;

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

        /* T20：同类其余位置并成一行（每行最多 12 个，超了续行）——
         * 级联从"根因数 × 行数"条压到 2 行，且位置一个不丢。 */
        int others = 0;
        for (int j = i + 1; j < errors.count; j++) {
            if (!errors.list[j].printed && error_same_kind(err, &errors.list[j])) others++;
        }
        if (others > 0) {
            int col = 0;
            int sep = 0;
            fprintf(stderr, "    同类另有 %d 处：", others);
            for (int j = i + 1; j < errors.count; j++) {
                const Error* other = &errors.list[j];
                if (other->printed || !error_same_kind(err, other)) continue;
                if (col == 12) {
                    fprintf(stderr, "\n        ");
                    col = 0;
                    sep = 0;
                }
                if (other->repeat_count > 1) {
                    fprintf(stderr, "%s行 %d×%d", sep ? "、" : "", other->line, other->repeat_count);
                } else {
                    fprintf(stderr, "%s行 %d", sep ? "、" : "", other->line);
                }
                sep = 1;
                col++;
            }
            fprintf(stderr, "\n");
        }
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

/* 警告版的同"类"判定（与 error_same_kind 同款；警告没有 printed 边界，全部参与归并） */
static int warning_same_kind(const Warning* a, const Warning* b) {
    return a->type == b->type
        && a->filename[0] != '\0'
        && strcmp(a->filename, b->filename) == 0
        && strcmp(a->msg, b->msg) == 0;
}

void warning_print_all(void) {
    if (warnings.count == 0) return;

    int total = 0;
    for (int i = 0; i < warnings.count; i++) {
        total += warnings.list[i].repeat_count;
    }

    // 展示条数 = 归并后的"类"数（同 error_print_all：同一根因跨行的复现算一类）
    int shown_kinds = 0;
    for (int i = 0; i < warnings.count; i++) {
        int is_head = 1;
        for (int j = 0; j < i; j++) {
            if (warning_same_kind(&warnings.list[j], &warnings.list[i])) { is_head = 0; break; }
        }
        if (is_head) shown_kinds++;
    }

    fprintf(stderr, "\n=== 发现 %d 个警告", total);
    if (total > shown_kinds) {
        fprintf(stderr, "（%d 种，已合并重复）", shown_kinds);
    }
    fprintf(stderr, " ===\n");

    for (int i = 0; i < warnings.count; i++) {
        Warning* w = &warnings.list[i];

        /* 同 error_print_all：同一根因只在**首次出现处**打完整信息，其余位置收进「同类另有 N 处」 */
        int is_head = 1;
        for (int j = 0; j < i; j++) {
            if (warning_same_kind(&warnings.list[j], w)) { is_head = 0; break; }
        }
        if (!is_head) continue;

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
            case WARN_AMBIGUOUS_MODULE: type_str = "模块名歧义"; break;
            case WARN_NATIVE_TYPE_NAME_CLASH: type_str = "与native类型同名"; break;
            case WARN_SYMTAB_INCOMPLETE: type_str = "符号表可能不完整"; break;
            // （原 WARN_EMPTY_CATCH / WARN_IMPOSSIBLE_CAST 已升为错误 ⇒ 类别名移入消息前缀 ✓）
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

        /* 同类其余位置并成一行（每行最多 12 个，超了续行）—— 与 error_print_all 同款 */
        int others = 0;
        for (int j = i + 1; j < warnings.count; j++) {
            if (warning_same_kind(w, &warnings.list[j])) others++;
        }
        if (others > 0) {
            int col = 0;
            int sep = 0;
            fprintf(stderr, "    同类另有 %d 处：", others);
            for (int j = i + 1; j < warnings.count; j++) {
                const Warning* other = &warnings.list[j];
                if (!warning_same_kind(w, other)) continue;
                if (col == 12) {
                    fprintf(stderr, "\n        ");
                    col = 0;
                    sep = 0;
                }
                if (other->repeat_count > 1) {
                    fprintf(stderr, "%s行 %d×%d", sep ? "、" : "", other->line, other->repeat_count);
                } else {
                    fprintf(stderr, "%s行 %d", sep ? "、" : "", other->line);
                }
                sep = 1;
                col++;
            }
            fprintf(stderr, "\n");
        }
    }

    fprintf(stderr, "=====================\n\n");
}

// ============================================================================
// T19：把包层扫出来的"模块名歧义"报成编译期警告
// ----------------------------------------------------------------------------
// 实现放在这里而不是各调用方：编译器（main.c）与 LSP（leno_lsp/leno_compiler_lib.c）
// 都要用，而这两处不共享同一个源文件 —— 只有 error.c 两边都链。
// 位置挂在**实际生效**（搜索路径靠前）那份文件上：终端里可 ctrl+点击直接跳过去，
// 比原来那句"模块 'x' 中没有方法 y"好定位得多。
// ⚠ 调用方必须把它放在 error_clear() / warning_clear() **之后**（否则警告当场被清掉）。
// ============================================================================
void error_report_shadowed_module(void* ctx, const char* name, const char* winner, const char* shadowed) {
    (void)ctx;
    (void)name;
    char msg[BUFFER_MEDIUM];
    snprintf(msg, sizeof(msg),
             "被同名模块遮蔽：%s（内容不同）；消解：改用包入口名或 ./ 相对路径，别用裸文件名",
             shadowed);
    /* error_get_filename() 返回的是内部缓冲区 ⇒ 先拷出来再改，否则会被覆盖 */
    char saved[BUFFER_LARGE];
    saved[0] = '\0';
    const char* cur = error_get_filename();
    if (cur) {
        strncpy(saved, cur, sizeof(saved) - 1);
        saved[sizeof(saved) - 1] = '\0';
    }
    error_set_filename(winner);
    warning_add_at(WARN_AMBIGUOUS_MODULE, 1, 1, msg);
    error_set_filename(saved[0] ? saved : NULL);
}
