#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "../include/module_symbol_table.h"
#include "../include/module_loader.h"
#include "../include/leno_serialize.h"
#include "../include/leno_parser.h"   // parser_eval_const_expr_text（扫描阶段的 enum 成员求值）
#include "../include/leno_error.h"
#include "../include/platform.h"
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#endif

// 来自 module_loader.c
extern int normalize_path(char* path, int max_len);

// 初始容量
#define INITIAL_CAPACITY 16

// ---- 模块符号表容量限制（命名常量，替代硬编码） ----
// 注：数量增长型上限（类型数/方法数/字段数/枚举成员数/clib 函数数等）
// 已全部改为动态数组，不再使用固定上限（修复符号被静默截断的 bug）。
// 以下仅保留"语言设计型"限制：超限时通过 error_add 报编译错误（而非静默丢弃）。
#define MOD_MAX_NAMES         64   // 标识符/名称缓冲区长度
#define MOD_MAX_TYPE_STR     256   // 类型字符串缓冲区长度
#define MOD_MAX_PARAMS        64   // 单函数最大参数数
#define MOD_MAX_TYPE_PARAMS   16   // 单类型最大泛型参数数
#define MOD_MAX_GENERIC_RET   16   // 返回类型最大泛型参数数

// ---- 动态名称数组辅助（模块符号表扫描用） ----
// 历史教训：固定上限数组（如 methods[128]）超限时静默丢弃符号，
// 错误却爆发在远处的调用点（"类型 'struct X' 没有方法 'Y'"），极难排查。
// 数量型收集一律使用以下按需翻倍增长的动态数组。
// 返回 0 成功，-1 内存不足（调用方跳过该名称继续扫描）

// 追加名称（源码区间 [name_start, name_start+name_len)）
static int mod_names_push(char*** names, int* count, int* capacity,
                          const char* name_start, int name_len) {
    if (name_len <= 0) return -1;
    if (*count >= *capacity) {
        int new_cap = *capacity == 0 ? 16 : *capacity * 2;
        char** grown = (char**)realloc(*names, sizeof(char*) * new_cap);
        if (!grown) return -1;
        *names = grown;
        *capacity = new_cap;
    }
    char* s = (char*)malloc(name_len + 1);
    if (!s) return -1;
    memcpy(s, name_start, name_len);
    s[name_len] = '\0';
    (*names)[(*count)++] = s;
    return 0;
}

// 追加名称（NUL 结尾字符串，内部复制）
static int mod_names_push_z(char*** names, int* count, int* capacity,
                            const char* name) {
    return mod_names_push(names, count, capacity, name, (int)strlen(name));
}

// 追加本地别名（名称 + 类型信息成对存储，type_info 所有权转移给数组）
static int mod_alias_push(char*** names, TypeInfo*** types, int* count, int* capacity,
                          const char* name, TypeInfo* type_info) {
    if (*count >= *capacity) {
        int new_cap = *capacity == 0 ? 16 : *capacity * 2;
        char** grown_names = (char**)realloc(*names, sizeof(char*) * new_cap);
        if (!grown_names) return -1;
        *names = grown_names;
        TypeInfo** grown_types = (TypeInfo**)realloc(*types, sizeof(TypeInfo*) * new_cap);
        if (!grown_types) return -1;
        *types = grown_types;
        *capacity = new_cap;
    }
    char* s = strdup(name);
    if (!s) return -1;
    (*names)[*count] = s;
    (*types)[*count] = type_info;
    (*count)++;
    return 0;
}

// 计算源码位置 pos 所在行号（1-based，用于错误报告）
static int mod_source_line(const char* source, const char* pos) {
    int line = 1;
    for (const char* c = source; c < pos && *c; c++) {
        if (*c == '\n') line++;
    }
    return line;
}

// 创建/销毁
#include "inc/sym_table_create.inc"

// 添加/查找操作
#include "inc/sym_table_add.inc"

// 类型字符串解析器
#include "inc/sym_table_type_parse.inc"

// 参数表 → 类型 的唯一实现（⑰-2 起 face 用它；scan_struct 的方法参数段仍是内联版）
// ⚠ 必须在这里（**文件作用域**）包含：`scan/*.inc` 那批是在扫描**函数体内**包含的代码片段，
//   在里面定义函数会得到 "invalid storage class for function"（实测踩过）。
#include "inc/sym_table_params.inc"

// 导入别名类型依赖传导
#include "inc/sym_table_import_alias.inc"

// 枚举成员常量表达式求值：**不再有本模块自己的求值器**。
// 扫描阶段需要值时调 parser_eval_const_expr_text（parser_func.c），词法/语法/求值全是语言本身
// 那一套 —— 原先复刻在这里的 inc/sym_table_enum_expr.inc 已删除（Phase 1 的收敛）。
//
// ⚠ VM-only 构建（build_vm.bat，不带编译器）：parser 整个目录都不在清单里，而扫描链
// （sym_table_scan.inc → scan/*.inc → scan_enum.inc）在 VM 里只是被**编译进来**、
// 运行时**够不到** —— module_symbol_table_scan 的唯一外部调用方是语义分析
// （src/semantic/visitinc/visit_module.inc:39），属编译器阶段；VM 运行时靠**反序列化**
// 符号表 + 吃 .lenb（module_loader.c / vm.c / serialize.c 都不碰 module_symbol_table_*）。
// ⇒ VM-only 下给个占位实现即可，不必把 lexer / AST / parser 拖进"无编译器"的产物
// （那正是 build_vm.bat 存在的意义）。若真被调到 ⇒ 说明 VM 走了源码扫描路径（本不该发生）
// 就**立即硬失败**：宁可炸，也不要静默给出错误的枚举值（Phase 1 收敛就是为消灭静默错值）。
#ifdef LENO_VM_ONLY
int parser_eval_const_expr_text(const char* text, char** names, int64_t* values,
                                int count, int64_t* out) {
    (void)text; (void)names; (void)values; (void)count; (void)out;
    fprintf(stderr, "[fatal] VM-only 构建里调用了 parser_eval_const_expr_text："
                    "VM 运行时不扫描源码符号表，这条路径不该出现\n");
    abort();
}
#endif

// 前向声明（定义在 sym_table_entry.inc，但 scan 阶段需要使用）
static void resolve_module_full_path(char* full_path, int max_len,
                                       const char* module_path, const char* current_file);

// 符号表扫描（两遍扫描）
#include "inc/sym_table_scan.inc"

// 符号表缓存（.lenosymc 序列化/反序列化）
#include "inc/sym_table_cache.inc"

// 公开入口
#include "inc/sym_table_entry.inc"
