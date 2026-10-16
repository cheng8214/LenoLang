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
// ★ 2026-10-15：`error_add` / `error_add_at` / `warning_add` / `warning_add_at` **已删除声明**
//   —— 它们是「入库」的唯一实现，但**只能由本文件内的 `_fmt` 入口调用**（现已是 error.c 内的
//   static ✓）。全仓（含运行期）调用点已 100% 迁到 `_fmt` 入口，所以外部再写
//   "自开缓冲 + 传 msg" 会在**编译期**直接撞墙 ✓ —— 这正是"统一"的落地方式。
//   ⇒ 报错/警告一律用下面四个（外加带去重的 `error_add_at_fmt_dedup`）。

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
// ⚠ 新代码一律用这些入口（直接写格式串，别再自己开 msg 缓冲 ✗）。
//   **本仓编译期诊断已全量迁移**（2026-10-14~15：parser / semantic / optimize / codegen /
//   module_symbol_table）——
//     · 262 处「自开 msg 缓冲 + snprintf / format_xxx + 传 msg」逐处手改 ✓
//     · 另有 207 处「直接传字面量」由**带严格跳过条件**的脚本改（末参必须纯字面量、
//       不含任何 %、≤1000 字节、不在 # 行 ⇒ 全程零语义变化，并由编译 + 套件 + 逐字节对拍验证）✓
//     · 随后把 20 余处「末参是变量（单条 snprintf 紧邻报错）」也逐处收进来 ✓
//   ⇒ **编译期诊断里的旧写法已 100% 清零**（2026-10-15 复查：parser / semantic / optimize /
//     codegen / module_symbol_table 里**没有一处**直调 `error_add` / `error_add_at` /
//     `warning_add` / `warning_add_at` ✓ —— 全仓唯一的字符串匹配是 semantic_type_utils.c
//     注释里的**举例文字** ✗，不是代码）。
//   收尾三件事（2026-10-15）：
//     · `consume(p, type, msg)` 内部改走 `error_add_at_fmt(.., "%s", msg)` ✓（msg 是整句话）
//     · 给 `format_type_error` 加**第 5 槽 `%s5`** ⇒ 原先那两处"先出模板、再 snprintf 填 %d"
//       的 clib 参数检查也一行收尾了 ✓
//     · 三个 **buf 输出型**辅助（`format_type_error` / `format_detailed_type_error(_ex)`）
//       已收为 `semantic_type_utils.c` 内部的 static ⇒ **对外只剩 `*_text` 返回串版** ✓
//   ⚠ 下面这四个底层 API **保留**（不是"旧实现残留"）：它们是"入库"的唯一实现
//     （去重 / 上限 / 计数都在这儿），统一入口内部就调它们 ✓；运行期（vm / object / 顶层）
//     也还有约 55 处在用 ⇒ 编译期**一律不得**直调，走 `_fmt` 入口 ✓
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

// ★ 2026-10-15：**带去重**的统一入口（格式化 → `error_has_at` 判重 → 入库）
//   用途：**两条路径会命中同一处**的检查点 —— 必须先用拼好的那句话判重，否则同一句话报两遍
//   （实测不去重会显示 "(重复 2 次)" ✗，那只是计数不是折叠 ✓）。
//   例：semantic_type.c 的 AST_INDEX 分支（裸字段名走索引路 = 访问器那条已报过 ✓）。
//   ⇒ 这类调用点从此不必自己开缓冲（原来要四步：开缓冲 → snprintf → error_has_at → error_add_at ✗）
void error_add_at_fmt_dedup(ErrorType type, int line, int column, const char* fmt, ...)
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

// ★ 2026-10-15：`warning_add` / `warning_add_at` 同样**已删声明**（收为 error.c 内 static，
//   理由见上面 `error_add` 那段 ✓）。警告一律走 `warning_add_fmt` / `warning_add_at_fmt`。

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
// ★ 2026-10-16：**延迟创建**日志（用户实测两轮后的最终口径）
//   成功运行 ⇒ **全程连 `logs\` 目录都不出现** ✓（上一版"启动就建、成功再删"被用户一眼看穿 ✗）
//   只有"真要写"的那一刻才建：① 运行期错误打印前（vm_exception.inc ✓）；
//     ② 非零退出时（`leno_error_report_exit` ✓）；③ 崩溃兜底里现建（见 error.c ④ ✓）；
//     ④ `LENO_LOG=<路径>`（用户显式要 = 立刻接管，含成功运行 ✓）
//   幂等 ✓；建不出来（只读目录等）⇒ 放弃并清 pending ✓
void leno_log_ensure_open(void);
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
