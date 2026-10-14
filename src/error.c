#include "include/lenolang.h"
// ★ 2026-10-11：无控制台发布版的错误可见性要用到平台层（exe 路径 / 宽字符转换 / mkdir ✓）
//   platform.h 两侧都兜住了：Windows 给 utf16_to_utf8（内联 ✓）、POSIX 声明
//   platform_self_exe_path ✓
#include "include/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// ★ 2026-10-14：报错/警告文案统一入口要用 vsnprintf（见 error_add_fmt 一族 ✓）
#include <stdarg.h>
// ★ signal / raise / SIGSEGV / SIGABRT / SIGBUS / SIGFPE / SIGILL / SIG_DFL 全在这里 ✓
//   ⚠ 2026-10-11 补：Windows 侧被 <windows.h> **间接**带进来了 ⇒ Linux 首编当场报
//     “‘SIGSEGV’ undeclared / implicit declaration of ‘signal’” ✗（实测 ✓）。
//   ⇒ 标准头一律**显式**写，别靠别人间接带 ✓（这条教训适用于本仓所有 C 文件 ✓）
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif

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

// ============================================================================
// ★ 2026-10-14：报错 / 警告文案的**统一入口**（单一原则 —— 改长度只改这里 ✓）
// ----------------------------------------------------------------------------
// 完整动机见 leno_error.h 的说明。要点：
//   · 内部用 `BUFFER_MSG`(1024)：文案"说清原因 + 给怎么办"要长度，中文 3 字节/字 ✓
//   · **截断要安全**：`vsnprintf` 在 cap 处**硬切**，可能切在 UTF-8 多字节序列中间
//     ⇒ 尾部留下孤立首字节 ⇒ 终端打出乱码 ✗（实测切中文必现 ✓）
//   · 最终仍调**既有**的 error_add_at / warning_add_at ⇒ 去重、上限、入库不另起一份 ✓
// ============================================================================

// 截断收尾：把"被 vsnprintf 硬切"的尾巴修成"完整字符 + …"
//   want：vsnprintf 的返回值（<0 = 编码出错；<cap = 没截断）⇒ 只有真截断才动手 ✓
//   为什么补 "…"：静默丢字符会让读者以为"文案就这么长" ✗；一个省略号明确表示"还有内容"
static void error_tidy_truncated(char* buf, size_t cap, int want) {
    if (!buf || cap == 0) return;
    if (want < 0 || (size_t)want < cap) return;       // 没截断（或编码出错）⇒ 原样 ✓
    size_t n = strlen(buf);
    // ① 退掉尾部的**续字节**（UTF-8 续字节形如 0b10xxxxxx）
    while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
    // ② 退掉那个**孤立的首字节**（0b11xxxxxx —— 它的续字节在切点之后，已经没了）
    if (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0xC0) n--;
    // ③ 用 "…"（U+2026，3 字节）收尾；放不下就只做截断（不强塞）
    if (n + 4 <= cap) {
        memcpy(buf + n, "\xE2\x80\xA6", 3);
        buf[n + 3] = '\0';
    } else {
        buf[n] = '\0';
    }
}

// 四个入口共用的样板：展开 va_list 进 buf 并做截断收尾 ✓
static void error_format_v(char* buf, size_t cap, const char* fmt, va_list ap) {
    int want = vsnprintf(buf, cap, fmt, ap);
    error_tidy_truncated(buf, cap, want);
}

void error_add_fmt(ErrorType type, int line, const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUFFER_MSG];
    va_list ap;
    va_start(ap, fmt);
    error_format_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error_add(type, line, buf);
}

void error_add_at_fmt(ErrorType type, int line, int column, const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUFFER_MSG];
    va_list ap;
    va_start(ap, fmt);
    error_format_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    error_add_at(type, line, column, buf);
}

void warning_add_fmt(WarnType type, int line, const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUFFER_MSG];
    va_list ap;
    va_start(ap, fmt);
    error_format_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    warning_add(type, line, buf);
}

void warning_add_at_fmt(WarnType type, int line, int column, const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUFFER_MSG];
    va_list ap;
    va_start(ap, fmt);
    error_format_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    warning_add_at(type, line, column, buf);
}

// ★ 2026-10-15：**带去重**的统一入口 —— 格式化 → 问一句"这句话在该行该列报过没" → 再入库。
//   为什么需要它（把最后两处"自开缓冲"也收进统一入口）：语义层有**两条路径命中同一处**
//   （裸字段名的成员访问走索引路 ✗ + 访问器那条 ✓，见 semantic_type.c 的 AST_INDEX 分支），
//   必须先拿**拼好的那句话**去问 `error_has_at` 才能避免同一句话报两遍
//   （实测：不去重会显示 "(重复 2 次)" ✗ —— 那只是计数、不是折叠 ✓）。
//   原来只能四步走（自开 msg 缓冲 → snprintf → error_has_at → error_add_at）✗，现在一行 ✓；
//   尺寸仍是 `BUFFER_MSG`、截断仍走同一份安全收尾（error_format_v ✓）⇒ **改长度只改一处** ✓。
void error_add_at_fmt_dedup(ErrorType type, int line, int column, const char* fmt, ...) {
    if (!fmt) return;
    char buf[BUFFER_MSG];
    va_list ap;
    va_start(ap, fmt);
    error_format_v(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (error_has_at(line, column, buf)) return;   // 同一处、同一句话 ⇒ 不重复入库 ✓
    error_add_at(type, line, column, buf);
}

int error_has_any(void) {
    return errors.count > 0;
}

int error_count(void) {
    return errors.count;
}

/* 同一处（行、列）是否已报过一字不差的这句话 —— 见 leno_error.h 的说明 ✓
 * ⚠ 不比文件名：调用它的两处检查都在**同一次编译**里、同一文件 ✓（跨模块也由 error_get_filename
 *   统一成"当前文件" ✓），所以按 (行, 列, 文案) 判定就够 ✓ */
int error_has_at(int line, int column, const char* msg) {
    if (!msg) return 0;
    for (int i = 0; i < errors.count; i++) {
        Error* e = &errors.list[i];
        if (e->line == line && e->column == column && strcmp(e->msg, msg) == 0) {
            return 1;
        }
    }
    return 0;
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
            case WARN_FIELD_SHADOW_ASSIGN: type_str = "遮蔽字段的赋值"; break;
            case WARN_SELF_FORWARD:          type_str = "疑似无限递归"; break;
            case WARN_METHOD_NAME_AMBIGUOUS: type_str = "同名歧义（可能走错目标）"; break;
            case WARN_MISSING_RETURN:  type_str = "漏写 return"; break;
            case WARN_PRINTF_NO_FORMAT: type_str = "printf不做格式化"; break;
            case WARN_NOT_PRECEDENCE: type_str = "not的优先级陷阱"; break;
            case WARN_MODULE_ALIAS_SHADOWED: type_str = "模块别名被遮蔽"; break;
            case WARN_STRUCT_NAME_CLASH:     type_str = "struct跨模块撞名"; break;
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

// ============================================================================
// ★ 2026-10-11：无控制台发布版的**错误可见性**（唯一实现；说明见 include/leno_error.h）
// ----------------------------------------------------------------------------
// 症状（用户实录 ✓）：GUI 打包产物（`_console(false)` ⇒ 无控制台版 VM）**双击**运行 ⇒
//   Windows GUI 子系统进程没有控制台 ⇒ 所有 `fprintf(stderr, …)`（含已带位置的调用栈）
//   **全部消失** ✗ ⇒ 用户只看到"闪一下就没了"，作者一行线索都拿不到 ✓。
//   （Linux 从 .desktop 启动、macOS 从 Finder 启动同理 ✓）
//
// 分层对策：
//   ① 有父控制台 ⇒ **接回去**（Windows AttachConsole(ATTACH_PARENT_PROCESS) + 重开 CONOUT$）
//      ⇒ 在终端里跑打包产物也能看到全部输出 ✓
//   ② 真·双击（无控制台 ✓）⇒ **stdout/stderr 一起接到日志文件** ⇒ 现有所有 fprintf/print
//      一行不改就全落盘 ✓（含调用栈 ✓）；路径三级回退：LENO_LOG → exe 同目录 logs\ → 用户目录
//   ③ stderr 若**已被捕获**（管道/文件/CI/`_exec` 子进程/无头用例 ✓）⇒ **什么都不动** ✓
//      —— 动了会把别人的捕获搞坏 ✗，也会让套件假失败 ✗（这条是"只认真·双击"的判据 ✓）
//   ④ 崩溃兜底与弹窗见下方两个小节 ✓
// ============================================================================

static char g_leno_log_path[MAX_PATH_LEN] = "";   // 日志绝对路径（空 = 没开日志 ✓）
static int  g_leno_log_fd = -1;                   // 崩溃兜底要用的**异步安全** fd（见 ④ ✓）
static int  g_leno_err_inited = 0;                // 幂等（多个入口调用只生效一次 ✓）
static int  g_leno_logging_active = 0;            // 输出是否已被日志接管（= 允许弹框的唯一前提 ✓）

static void leno_install_crash_handlers(void);    // ④ 的实现在本节之后 ✓

const char* leno_error_log_path(void) {
    return g_leno_log_path[0] ? g_leno_log_path : NULL;
}

// stderr 现在**有没有人接**（= 输出不会白丢 ✓）。
// 判据（保守方向 ✓：拿不准就当"有人接" ⇒ 我们不动 ✓ —— 少写一份日志没关系，
// 但绝不能把别人的捕获搞坏 ✗，也绝不能把本该显示的输出抢走 ✗）：
//   ① 管道 / 普通文件 ⇒ **有人接** ✓（终端重定向、CI、`_exec` 子进程、无头用例 ✓）
//   ② 真控制台 ⇒ 有人接 ✓（人看得见 ✓）
//   ③ 其余（句柄无效 / NUL / POSIX 上非 tty 非管道非文件 ✓）⇒ **没人接** ⇒ 才落日志 ✓
//      —— Windows 双击 GUI 打包产物就是 ③ ✓
static int leno_one_fd_has_reader(int which) {
#ifdef _WIN32
    // which: 1 = stdout ✓ 2 = stderr ✓
    // ★ 判据只认**"句柄真的没有"**（NULL / INVALID / FILE_TYPE_UNKNOWN ✓）⇒ 才认为"没人接" ✓。
    //   \u26a0 为什么不再用"CHAR + 没有控制台窗口"当判据（实测教训 ✗）：
    //     打包产物（`leno_vm_gui.exe` ✓）在**重定向到文件**时 `GetFileType` 也报 **CHAR(0x2)** ✗
    //     （同条件下普通 GUI 探针报 DISK(0x1) ✓ ⇒ 两者在句柄层就分不开 ✗），而 `GetConsoleWindow()`
    //     在 IDE / 无控制台环境里恒为 NULL ✗ ⇒ 旧判据会把"**文件重定向**"也当成没人接 ✗
    //     ⇒ 接管后把用户的重定向冲掉、文件 0 行 ✗✗（实测：`app.exe > out.txt` 空文件、exit=0 ✓）。
    //   ⇒ 取**保守方向**：宁可少接管一次（输出照旧打屏/进管道 ✓），也绝不动别人接好的输出 ✓。
    //   ⇒ 想要日志的场合用**显式**开关 `LENO_LOG=<路径>`（见 leno_error_init ✓ 它强制接管 ✓）。
    HANDLE h = GetStdHandle(which == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return 0;
    return GetFileType(h) != FILE_TYPE_UNKNOWN ? 1 : 0;
#else
    int fd = (which == 1) ? 1 : 2;
    struct stat st;
    if (fstat(fd, &st) != 0) return 0;          // fd 无效 ⇒ 没人接 ✓
    return 1;                                   // tty / 管道 / 文件 / /dev/null 一律当"有去处" ✓
#endif
}

// ★ 2026-10-11（实测抓到的真 bug ✗）：**stdout 与 stderr 要分别判** ✓。
//   只判 stderr 会漏掉最常见的一类用法 —— `app.exe | grep x` / PowerShell 的
//   `& app.exe 2>&1 | Out-String` / CI 里只接 stdout ⇒ **stdout 是管道、stderr 不是** ✗
//   ⇒ 旧代码把 stdout 也一起抢进日志 ✗ ⇒ **被捕获的那一路反而什么都收不到** ✗✗
//   （实测：无头跑游戏，屏幕上 0 行输出 ✓ 就是它 ✓）。
//   判据改成"**两路都有去处才算有人接**" ✓：任一路没人接 ⇒ 不接管 ✓（绝不抢 ✓）；
//   两路都没人接（真·双击 ✓）⇒ 才接管 ✓。
static int leno_output_has_reader(void) {
    return leno_one_fd_has_reader(1) && leno_one_fd_has_reader(2);
}

#ifdef _WIN32
// 这一路（1 = stdout ✓ 2 = stderr ✓）现在**能不能用**（句柄有效、有去处 ✓）。
// 与 has_reader 的区别：usable 只问"句柄有效吗"（管道 / 文件 / 控制台都算能用 ✓），
//   它专门用来决定"AttachConsole 之后要不要把它重开成 CONOUT$" ✓ ——
//   **已经能用的绝不碰** ✓（碰了就把用户的重定向冲掉 ✗ 见 leno_error_init 里的实测说明 ✓）
static int leno_one_fd_usable(int which) {
    HANDLE h = GetStdHandle(which == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE);
    if (h == NULL || h == INVALID_HANDLE_VALUE) return 0;
    return GetFileType(h) != FILE_TYPE_UNKNOWN ? 1 : 0;
}
#endif

// 给日志文件建父目录（只建一层 ✓ —— logs\ 不存在是唯一现实情况 ✓）
static void leno_ensure_parent_dir(const char* file) {
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", file);
    char* slash = strrchr(dir, '/');
    char* bslash = strrchr(dir, '\\');
    char* cut = (bslash && (!slash || bslash > slash)) ? bslash : slash;
    if (!cut || cut == dir) return;
    *cut = '\0';
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
}

// 取"本进程 exe 的绝对路径"（取不到返回 0 ✓）
static int leno_self_exe_path(char* out, size_t n) {
    out[0] = '\0';
#ifdef _WIN32
    // W 版 + utf16_to_utf8：路径里有中文（本仓的发布目录就是中文名 ✓）也不乱码 ✓
    wchar_t wbuf[MAX_PATH_LEN];
    DWORD len = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (len == 0 || len >= (DWORD)(sizeof(wbuf) / sizeof(wbuf[0]))) return 0;
    wbuf[len] = L'\0';
    char* u8 = utf16_to_utf8(wbuf);
    if (!u8) return 0;
    snprintf(out, n, "%s", u8);
    free(u8);
    return out[0] != '\0';
#else
    return platform_self_exe_path(out, n);
#endif
}

// 取"本 exe 所在目录"（取不到返回 0 ✓ —— 调用方降级到用户目录 ✓）
static int leno_exe_dir(char* out, size_t n) {
    if (!leno_self_exe_path(out, n)) return 0;
    char* slash = strrchr(out, '/');
    char* bslash = strrchr(out, '\\');
    char* cut = (bslash && (!slash || bslash > slash)) ? bslash : slash;
    if (!cut) { out[0] = '\0'; return 0; }
    *cut = '\0';
    return out[0] != '\0';
}

#ifdef _WIN32
// ★ 2026-10-11：本二进制是 **GUI 子系统** 还是 **控制台子系统**（读自身 PE 头 ✓ 不改构建脚本 ✓）。
//   为什么必须分：Windows 上"没有控制台窗口"有两种情形 ——
//     ① **GUI 子系统**（`leno_vm_gui.exe` / 打包产物 ✓）被双击 ⇒ 输出真的没人接 ⇒ **该接管** ✓；
//     ② **控制台子系统**（`leno.exe` / `leno_vm.exe` ✓）在一个"没有可见窗口"的环境里被启动
//        （IDE 里的 PowerShell ✓ 实测就是 `GetConsoleWindow()==NULL` ✗）⇒ 用户**其实看得见**
//        输出（终端在 ✓）⇒ 接管的唯一效果是**在 `build\logs\` 里攒垃圾日志** ✗（实测撞到 ✓）。
//   ⇒ 判据：只有 GUI 子系统才做这次接管 ✓；控制台子系统永不接管 ✓。
//   PE 头：e_lfanew(0x3C) → PE 签名 4 字节 → COFF 头 20 字节 → OptionalHeader 里的
//   Subsystem 字段（PE32/PE32+ 都在 OptionalHeader 偏移 68 ✓）= 2 表示 IMAGE_SUBSYSTEM_WINDOWS_GUI ✓
static int leno_is_gui_subsystem(void) {
    char exe[MAX_PATH_LEN];
    if (!leno_self_exe_path(exe, sizeof(exe))) return 0;
    FILE* f = fopen(exe, "rb");
    if (!f) return 0;
    unsigned char dos[64];
    if (fread(dos, 1, sizeof(dos), f) != sizeof(dos) || dos[0] != 'M' || dos[1] != 'Z') {
        fclose(f); return 0;
    }
    long pe_off = (long)dos[0x3C] | ((long)dos[0x3D] << 8) | ((long)dos[0x3E] << 16) | ((long)dos[0x3F] << 24);
    if (pe_off <= 0 || fseek(f, pe_off, SEEK_SET) != 0) { fclose(f); return 0; }
    // ⚠ 缓冲要覆盖到 OptionalHeader 偏移 68+2 处（= 相对 pe_off 的 92/93 字节 ✓）：
    //   PE 签名 4 + COFF 20 + OptionalHeader 里 Subsystem 的偏移 68 ⇒ 至少 94 字节 ✓
    //   （第一版给了 72 ✗ ⇒ gcc `-Warray-bounds` 当场抓住 ✓ 这就是"构建 0 警告"的用处 ✓）
    unsigned char hdr[96];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return 0; }
    fclose(f);
    if (hdr[0] != 'P' || hdr[1] != 'E' || hdr[2] != 0 || hdr[3] != 0) return 0;
    unsigned subsystem = hdr[24 + 68] | ((unsigned)hdr[24 + 69] << 8);
    return subsystem == 2 ? 1 : 0;   // IMAGE_SUBSYSTEM_WINDOWS_GUI
}
#endif

// 选日志路径（三级回退 ✓）：① LENO_LOG ② exe 同目录 logs\ ③ 用户目录
static void leno_pick_log_path(char* out, size_t n) {
    const char* env = getenv("LENO_LOG");
    if (env && env[0]) { snprintf(out, n, "%s", env); return; }

    char dir[MAX_PATH_LEN];
    if (leno_exe_dir(dir, sizeof(dir))) {
#ifdef _WIN32
        snprintf(out, n, "%s\\logs\\leno_error.log", dir);
#else
        snprintf(out, n, "%s/logs/leno_error.log", dir);
#endif
        return;
    }
#ifdef _WIN32
    const char* base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("TEMP");
    if (!base || !base[0]) base = ".";
    snprintf(out, n, "%s\\Leno\\logs\\leno_error.log", base);
#else
    const char* base = getenv("XDG_STATE_HOME");
    if (base && base[0]) {
        snprintf(out, n, "%s/leno/logs/leno_error.log", base);
    } else {
        const char* home = getenv("HOME");
        if (!home || !home[0]) home = ".";
        snprintf(out, n, "%s/.local/state/leno/logs/leno_error.log", home);
    }
#endif
}

void leno_error_init(void) {
    if (g_leno_err_inited) return;
    g_leno_err_inited = 1;

#ifdef _WIN32
    // ① 终端里跑（父进程有控制台 ✓）：接回去，这样 GUI 打包产物在 cmd/PowerShell 里
    //    也能看到全部输出 ✓（双击时这句返回 FALSE ⇒ 落到 ② ✓）
    // ⚠ 实测抓到的真 bug ✗：**只把"当前不可用"的那一路接回来** ✓ —— 无条件 freopen("CONOUT$")
    //   会把用户已有的**重定向**（`app.exe > out.txt` / `app.exe | grep x` ✓）直接冲掉 ✗✗
    //   ⇒ 表现是"输出既不在文件里也不在屏上"（实测：重定向文件 0 行 ✓ 而 exit=0 ✓）。
    //   AttachConsole 只是"让控制台存在" ✓，**不该顺手改掉已经能用的标准流** ✓。
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f;
        if (!leno_one_fd_usable(1)) { f = freopen("CONOUT$", "w", stdout); (void)f; }
        if (!leno_one_fd_usable(2)) { f = freopen("CONOUT$", "w", stderr); (void)f; }
        {
            HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
            if (h == NULL || h == INVALID_HANDLE_VALUE || GetFileType(h) == FILE_TYPE_UNKNOWN) {
                f = freopen("CONIN$", "r", stdin); (void)f;
            }
        }
        // 接回来的控制台也要 UTF-8（VM 侧本来没有这句 ✓ 顺手补上，中文错误信息才不乱码 ✓）
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
#endif

    // 输出有人接（管道 / 文件 / 控制台 / 甚至是"说不清但句柄有效" ✓）⇒ 绝不插手 ✓
    // ★ `LENO_LOG=<路径>` 是**显式要求**日志 ⇒ 跳过这条判据强制接管 ✓（但**仍**只对 GUI 子系统 ✓）
    {
        const char* envLog = getenv("LENO_LOG");
        int forced = (envLog && envLog[0]) ? 1 : 0;
        if (!forced && leno_output_has_reader()) return;
    }
#ifdef _WIN32
    // ★ 只有 **GUI 子系统**的二进制才做这次接管 ✓（判据见 leno_is_gui_subsystem 的说明）：
    //   控制台子系统（leno.exe / leno_vm.exe ✓）的 stdout/stderr 永远有去处（终端/父进程 ✓），
    //   接管的唯一效果是在 `build\logs\` 攒垃圾日志 ✗ ⇒ 直接返回 ✓
    if (!leno_is_gui_subsystem()) return;
#endif

    // ② 真·双击：stdout/stderr 一起接到日志（**无缓冲** ✓ —— 崩溃时缓冲里的内容会丢 ✗）
    char path[MAX_PATH_LEN];
    leno_pick_log_path(path, sizeof(path));
    if (!path[0]) return;
    leno_ensure_parent_dir(path);
    // ⚠ 日志是**追加**的 ✓ ⇒ 不加约束会无上限长大 ✗（实测：同一程序反复启动会一段段堆头 ✓）：
    //   · 只在**空文件**（或刚重开 ✓）时写那段说明头 ✓；
    //   · 超过 256KB 就**重开一份** ✓（上次的错已经没意义了 ✓ 本次的错误一定在后面 ✓）
    long old_size = 0;
    {
        FILE* probe = fopen(path, "rb");
        if (probe) {
            if (fseek(probe, 0, SEEK_END) == 0) old_size = ftell(probe);
            fclose(probe);
        }
    }
    int fresh = (old_size <= 0);
    if (old_size > 256 * 1024) {
        remove(path);
        fresh = 1;
    }
    FILE* f = freopen(path, "a", stdout);
    if (!f) return;
    // ★ 让 stderr **共用 stdout 那个文件位置** ✓ —— 直接再 freopen(stderr) 同一路径会得到
    //   两个**独立**位置 ⇒ 两路输出在日志里互相插队、一行被另一行劈成两半 ✗
    //   （实测撞到过：print 的整行被 [崩溃] 行从中间切开 ✓）
#ifdef _WIN32
    _dup2(_fileno(stdout), _fileno(stderr));
#else
    dup2(fileno(stdout), fileno(stderr));
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_leno_log_fd = fileno(stdout);
    snprintf(g_leno_log_path, sizeof(g_leno_log_path), "%s", path);

    if (fresh) {
        fprintf(stderr,
                "\n===== Leno 运行时日志（本文件是无控制台发布版的**唯一**输出通道 ✓）=====\n"
                "启动时间：程序启动即写；出错时会带上位置与调用栈 ✓\n"
                "提示：把本文件整个发给作者即可定位 ✓（路径：%s）\n\n", path);
    } else {
        // 追加场景：只留一行分隔，别再堆一遍说明头 ✗（同一程序反复启动会把日志堆满 ✓）
        fprintf(stderr, "\n--- 新的启动 ---\n");
    }
    fflush(stderr);

    g_leno_logging_active = 1;   // 弹窗判据：只有"接管了输出"时才允许弹 ✓（见下 ✓）
    leno_install_crash_handlers();
}

// ============================================================================
// ④ 崩溃兜底（`SetUnhandledExceptionFilter` / POSIX `signal`）
// ----------------------------------------------------------------------------
// 受控错误（Leno 的 throw / 运行时错误）走的是 ③ 的日志路径 ✓；但**真正的崩溃**
//   （访问违例 / 除零 / abort ✓）根本不经过 Leno 代码 ✗ —— 不挂这个钩子，用户看到的
//   只有系统的"程序已停止工作"（一个字不提 Leno ✓，也不指日志 ✓）。
// ⚠ 这种上下文里只有**异步信号安全**的调用能用 ✗ ⇒ 不能 fprintf / snprintf / malloc ✗
//   ⇒ 这里手写十六进制拼接 ✓ 只用 write(2) ✓（否则会在崩溃时二次死掉 ✓）
// ============================================================================
static void leno_crash_write(const char* kind, unsigned long code) {
    char buf[160];
    int n = 0;
    const char* p = "\n[崩溃] ";
    while (*p && n < 140) buf[n++] = *p++;
    p = kind;
    while (*p && n < 140) buf[n++] = *p++;
    p = "，异常码 0x";
    while (*p && n < 140) buf[n++] = *p++;
    {
        static const char hexd[] = "0123456789ABCDEF";
        int shift;
        for (shift = 28; shift >= 0; shift -= 4) {
            unsigned long d = (code >> shift) & 0xFUL;
            buf[n++] = hexd[d];
        }
    }
    buf[n++] = '\n';
    buf[n++] = '\0';
#ifdef _WIN32
    {
        HANDLE h = (HANDLE)_get_osfhandle(g_leno_log_fd >= 0 ? g_leno_log_fd : 2);
        DWORD wrote = 0;
        if (h != INVALID_HANDLE_VALUE && h != NULL) WriteFile(h, buf, (DWORD)(n - 1), &wrote, NULL);
    }
#else
    {
        ssize_t r = write(g_leno_log_fd >= 0 ? g_leno_log_fd : 2, buf, (size_t)(n - 1));
        (void)r;
        if (g_leno_log_fd >= 0) {
            ssize_t r2 = write(2, buf, (size_t)(n - 1));
            (void)r2;
        }
    }
#endif
}

#ifdef _WIN32
static LONG WINAPI leno_unhandled_filter(EXCEPTION_POINTERS* info) {
    unsigned long code = 0;
    if (info && info->ExceptionRecord) code = (unsigned long)info->ExceptionRecord->ExceptionCode;
    leno_crash_write("未处理的系统异常", code);
    return EXCEPTION_EXECUTE_HANDLER;   // 交给系统收尾（**不拦** ✓ 别冒充"已处理" ✗）
}
#else
static void leno_signal_handler(int sig) {
    leno_crash_write("收到信号", (unsigned long)sig);
    // 复原默认处理再重发 ⇒ 保留标准行为（core dump ✓）同时日志里已经留下痕迹 ✓
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

static void leno_install_crash_handlers(void) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(leno_unhandled_filter);
#else
    signal(SIGSEGV, leno_signal_handler);
    signal(SIGABRT, leno_signal_handler);
    signal(SIGBUS,  leno_signal_handler);
    signal(SIGFPE,  leno_signal_handler);
    signal(SIGILL,  leno_signal_handler);
#endif
}

// ============================================================================
// ⑤ 致命错误弹窗（只在 **真·双击** 时弹 ✓ —— 即"输出已经被日志接管"那种场合 ✓）
// ----------------------------------------------------------------------------
// 判据用 `g_leno_logging_active`：管道/文件/真控制台 ⇒ 从没置位 ⇒ **永不弹框** ✓
//   ⇒ CI、无头用例、终端里跑，都不会被弹框卡住 ✗✓（这是本设计最要紧的一条 ✓）
// 内容用**日志尾部**当摘要（不依赖任何内部错误结构 ✓ 信息和日志完全一致 ✓）；
// Windows 加一个"是否打开日志"（`explorer /select` ✓ 用户一步就能把文件发出来 ✓）。
// `LENO_NO_DIALOG=1` 可强制关掉（无人值守 ✓）。
// ============================================================================
static void leno_read_log_tail(char* out, size_t n) {
    out[0] = '\0';
    if (!g_leno_log_path[0]) return;
    FILE* f = fopen(g_leno_log_path, "rb");
    if (!f) return;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long size = ftell(f);
    if (size <= 0) { fclose(f); return; }
    long want = (long)(n - 1);
    long from = size > want ? size - want : 0;
    if (fseek(f, from, SEEK_SET) != 0) { fclose(f); return; }
    size_t got = fread(out, 1, (size_t)want, f);
    out[got] = '\0';
    fclose(f);
    // 用二进制方式读，行尾可能是 \r\n ✓ 显示无妨 ✓；截断处可能切到多字节字符 ⇒ 允许 ✓
}

void leno_error_report_exit(int exit_code) {
    if (exit_code == 0) return;
    if (!g_leno_logging_active) return;              // 输出有人接 ⇒ 绝不弹框 ✓
    if (!g_leno_log_path[0]) return;
    const char* noDlg = getenv("LENO_NO_DIALOG");
    if (noDlg && noDlg[0] && strcmp(noDlg, "0") != 0) {
        fprintf(stderr, "[提示] 已按 LENO_NO_DIALOG 跳过错误提示框 ✓\n");
        fflush(stderr);
        return;
    }

    char tail[1800];
    leno_read_log_tail(tail, sizeof(tail));

    // ⚠ 缓冲必须容得下**最坏情况**（否则 Linux 侧 gcc 会报 -Wformat-truncation ✗ 实测 ✓）：
    //   tail ≤ 1799 ✓ + g_leno_log_path ≤ MAX_PATH_LEN-1 ✓ —— Linux 上 MAX_PATH_LEN 是 **4096**
    //   （Windows 只有 260 ✓ 所以我本地 gcc 15 一直没报 ✗）⇒ 原来给 2400 是不够的 ✗
    //   ⇒ 取 8192（1800 + 4096 + 字面量 ≈ 6.1KB ✓ 留足余量 ✓）
    char body[8192];
    /* 手写拼接（避免在可能的异常上下文里依赖 snprintf 的长度语义 ✓ 这里其实安全 ✓） */
    snprintf(body, sizeof(body),
             "程序出错了（退出码 %d）\n\n"
             "最近日志：\n%s\n\n"
             "完整日志（含调用栈）在：\n%s\n\n"
             "把这个文件发给作者即可定位 ✓（Ctrl+C 可复制本框）",
             exit_code, tail, g_leno_log_path);

#ifdef _WIN32
    {
        wchar_t wbody[2400];
        if (MultiByteToWideChar(CP_UTF8, 0, body, -1, wbody, 2400) > 0) {
            // 是/否 = "是否打开日志所在文件夹" ✓（用户下一步就是把文件发出去 ✓）
            int r = MessageBoxW(NULL, wbody, L"Leno 程序出错",
                                MB_YESNO | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
            fprintf(stderr, "[提示] 已弹出错误提示框（MessageBoxW ✓）\n");
            fflush(stderr);
            if (r == IDYES) {
                // 用 explorer /select 打开并选中日志 ✓（走 system 免去额外链接 shell32 ✓）
                char cmd[MAX_PATH_LEN + 32];
                snprintf(cmd, sizeof(cmd), "explorer /select,\"%s\"", g_leno_log_path);
                int rc = system(cmd);
                (void)rc;
            }
        }
    }
#else
    {
        /* Linux/macOS：优先 zenity（能显示长文本 ✓），退 xmessage ✓；
           都没有就只剩日志文件 ✓（绝不死等 ✗） */
        // 这条格式串里出现**两次** g_leno_log_path ⇒ 最坏 2 × (MAX_PATH_LEN-1) + 字面量 ✓
        // （Linux 的 MAX_PATH_LEN = 4096 ⇒ 原来的 +256 明显不够 ✗ 实测被 -Wformat-truncation 抓住 ✓）
        char cmd[MAX_PATH_LEN * 2 + 512];
        snprintf(cmd, sizeof(cmd),
                 "if command -v zenity >/dev/null 2>&1; then "
                 "zenity --error --no-wrap --title='Leno 程序出错' --text='日志文件：%s'; "
                 "elif command -v xmessage >/dev/null 2>&1; then "
                 "xmessage -title 'Leno 程序出错' '日志文件：%s'; fi",
                 g_leno_log_path, g_leno_log_path);
        int rc = system(cmd);
        (void)rc;
        fprintf(stderr, "[提示] 已尝试弹出错误提示（zenity / xmessage ✓）\n");
        fflush(stderr);
    }
#endif
}
