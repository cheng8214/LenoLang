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
    int printed;                  // 1 = 已随调用栈即时打印（运行期错误），退出时不再重复
} Error;

typedef struct {
    Error list[MAX_ERRORS];
    int count;
} ErrorCollector;

extern ErrorCollector errors;

void error_set_filename(const char* filename);
void error_set_column(int column);
int error_get_column(void);
const char* error_get_filename(void);
void error_add(ErrorType type, int line, const char* msg);
void error_add_at(ErrorType type, int line, int column, const char* msg);

// ============================================================================
// ★ 2026-10-14：报错 / 警告**文案的统一入口**（格式化 + 安全截断）
// ----------------------------------------------------------------------------
// 为什么要它（单一原则）：此前**每个调用点**各自 `char msg[BUFFER_*]` + `snprintf`
//   ⇒ ① 缓冲尺寸各写各的（全仓几十种写法）✗；
//      ② 长文案被**静默截断**，还可能切在 UTF-8 多字节序列中间 ⇒ 终端打出乱码 ✗
//         （2026-10-14 实测：字典"锁定点回溯"那条第一版 ~220 字节，正被切在中文中间 ✓）；
//      ③ 想把文案放宽一点，得回去改 N 个调用点 ✗。
// ⇒ 现在收到这四个入口里：内部用 `BUFFER_MSG`(1024) 缓冲，**截断时回退到完整字符边界、
//   并以 "…" 收尾**（明确表示"这里还有内容"，而不是静默丢字符 ✓）；最终仍调既有的
//   `error_add_at` / `warning_add_at` ⇒ 去重、上限、入库**只有一份实现** ✓。
// ⚠ 新代码一律用这四个（直接写格式串，别再自己开 msg 缓冲 ✗）。
//   **本仓编译期诊断已全量迁移**（2026-10-14：parser / semantic / optimize / codegen /
//   module_symbol_table 共 262 处 ✓）；仅 4 处保留自开缓冲，各有硬理由 ——
//     · `parser_utils.c` 的 `consume(p, type, msg)`：那个 msg 是**函数参数**（调用方传的文案）✗
//     · `semantic_type.c:2512` / `:3285`：要先用 `error_has_at(.., msg)` **去重**，
//       而它必须拿到**拼好的那句话** ⇒ 走不了 fmt（一调就入库 ✗）
//     · `sym_table_entry.inc` 的循环依赖链：消息是**增量拼接**（循环 append）✗
//   这 4 处只把尺寸并入 `BUFFER_MSG` ✓。
// ⚠ 参数格式同 printf（GCC / Clang 下会做格式串检查 ✓）。
// ============================================================================
void error_add_fmt(ErrorType type, int line, const char* fmt, ...)
    LENO_ATTR_PRINTF(3, 4);
void error_add_at_fmt(ErrorType type, int line, int column, const char* fmt, ...)
    LENO_ATTR_PRINTF(4, 5);
void warning_add_fmt(WarnType type, int line, const char* fmt, ...)
    LENO_ATTR_PRINTF(3, 4);
void warning_add_at_fmt(WarnType type, int line, int column, const char* fmt, ...)
    LENO_ATTR_PRINTF(4, 5);

/* 同一处（行、列）是否已经报过**一字不差**的这句话 —— 2026-09-30 加。
 * 场景：同一处错会被两处检查同时发现 —— visit_field_access.inc 的访问器那条（走 AST 访问 ✓）
 * 与 semantic_type.c 的类型推断那条（走推断 ✓）：两者覆盖的路径不同但会**重叠**（显式 self. 形态 ✓）
 * ⇒ 报第二遍纯属噪音 ✗（实测：统一文案后仍显示 "(重复 2 次)" ✗ —— 那只是计数、不是折叠 ✓）
 * ⇒ 推断侧报之前先问这一句 ✓，同一处同一句话只留一条 ✓ */
int error_has_at(int line, int column, const char* msg);
int error_has_any(void);
int error_count(void);
void error_clear(void);
void error_print_all(void);

// 静默段：期间产生的错误/警告一律丢弃（实现见 error.c）。
// error_silence_end 返回**被丢弃的错误条数**：0 表示这段解析是干净的，>0 表示不可信。
// 用途：符号表扫描器借真解析器求值 enum 成员表达式时，不能把诊断污染到当前文件。
int error_silence_begin(void);
int error_silence_end(void);

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
void warning_add_at(WarnType type, int line, int column, const char* msg);

// ============================================================================
// ★ 2026-10-11：无控制台发布版的**错误可见性**（实现在 error.c 末尾，含完整说明）
// ----------------------------------------------------------------------------
// GUI 打包产物（`_console(false)` ⇒ 无控制台版 VM）双击运行时没有控制台 ⇒ 所有
// `fprintf(stderr, …)`（含带位置的调用栈）**全丢** ⇒ 用户只看到"闪一下就没了" ✗。
// 入口尽早调一次 `leno_error_init()`（幂等 ✓）：
//   ① 有父控制台就接回去（Windows AttachConsole）⇒ 终端里跑也看得到 ✓
//   ② 真·双击（无控制台、且 stdout/stderr 也没被捕获）⇒ **stdout/stderr 一起接到日志文件**
//      （路径三级回退：LENO_LOG → exe 同目录 logs\ → 用户目录 ✓）
//   ③ 已被捕获（管道/重定向/CI/`_exec` 子进程 ✓）⇒ 什么都不动 ✓（否则会破坏别人的捕获 ✗）
// 另装崩溃兜底（`SetUnhandledExceptionFilter` / POSIX `signal`）与致命错误弹窗 ✓
// ============================================================================
void leno_error_init(void);
// 当前日志文件的绝对路径（没开日志则返回 NULL ✓）——弹窗与提示要显示它 ✓
const char* leno_error_log_path(void);
// 进程即将以非零码退出时调用（弹窗只在"输出已被日志接管"时才会真弹 ✓；
// `LENO_NO_DIALOG=1` 可强制关掉 ✓；exit_code == 0 时直接返回 ✓）
void leno_error_report_exit(int exit_code);
int warning_has_any(void);
void warning_clear(void);
void warning_print_all(void);

/* T19：包层扫出的"模块名歧义" → 编译期警告（实现见 error.c；main.c 与 LSP 共用同一份）。
 * 签名与 package_resolve.h 里的 PackageShadowFn 一致 ⇒ 可直接当回调传进去。
 * ⚠ 调用方须在 error_clear()/warning_clear() 之后调用。 */
void error_report_shadowed_module(void* ctx, const char* name, const char* winner, const char* shadowed);

#endif // LENO_ERROR_H
