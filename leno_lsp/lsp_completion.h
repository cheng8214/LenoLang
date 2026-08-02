/**
 * LSP Completion Engine - 重构版
 * 
 * 架构原则：
 * 1. CompletionSet 统一管理补全项（去重、优先级、排序）
 * 2. 关键字/类型始终可用（修复原有 bug）
 * 3. 上下文检测独立模块化
 * 4. filterText 支持子串匹配（如 "export" 匹配 "export func"）
 */

#ifndef LSP_COMPLETION_H
#define LSP_COMPLETION_H

#include "leno_lsp.h"
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========== 补全优先级（数值越小越靠前） ========== */

typedef enum {
    PRIO_KEYWORD     = 10,   // 关键字 (if, for, func, export...)
    PRIO_TYPE       = 20,   // 内置类型 (int, float, string...)
    PRIO_SNIPPET    = 30,   // 代码片段 (if...else, for...to...)
    PRIO_BUILTIN    = 40,   // 内置函数 (print, len, type...)
    PRIO_LOCAL_VAR  = 50,   // 局部变量 / 参数
    PRIO_FIELD      = 60,   // 结构体字段
    PRIO_METHOD     = 70,   // 方法
    PRIO_MODULE     = 80,   // 模块 / 别名
    PRIO_USER_SYM   = 90,   // 用户定义符号（全局函数/类型等）
} CompletionPriority;

/* ========== 补全上下文类型 ========== */

typedef enum {
    CTX_NORMAL = 0,           // 普通代码位置
    CTX_TYPE_ANNOTATION,      // 类型注解位置 (var x: |)
    CTX_NEW,                  // new 关键字后
    CTX_USE_MODULE,           // use <module>
    CTX_USE_MEMBER,           // use <module>.|
    CTX_DOT_ACCESS,           // expr.
    CTX_STRING_LITERAL,       // "string".
} CompletionContext;

/* ========== Import 别名结构（供上下文检测使用） ========== */

typedef struct {
    char* alias;
    char* module_name;
} ImportAlias;

/* ========== Import 解析函数（由 comp_import.c 实现） ========== */

struct ImportAliasAllocator;  // 前向声明（内部使用）

/* 解析源码中的 import 语句，返回别名数组和数量 */
extern ImportAlias* parse_imports(const char* content, int* count);

/* 释放 parse_imports 返回的别名数组 */
extern void free_import_aliases(ImportAlias* aliases, int count);

/* 根据别名查找模块名（未找到则返回别名本身） */
extern const char* find_module_by_alias(ImportAlias* aliases, int count, const char* alias);

/* 根据别名查找模块路径（仅用于 .leno 文件导入，未找到返回 NULL） */
extern const char* find_module_path_by_alias(ImportAlias* aliases, int count, const char* alias);

/* ========== 查找当前光标所在的 struct 名称 ========== */

/* 返回光标所在 struct/cstruct 方法的名称，调用者需要 free 结果 */
extern char* find_enclosing_struct_name(const char* content, int cursor_offset);

/* ========== 补全项（内部扩展） ========== */

typedef struct {
    char*  label;           // 显示名称
    char*  filterText;      // 过滤文本（用于子串匹配）
    char*  insertText;      // 插入文本（可选）
    char*  detail;          // 右侧描述
    char*  documentation;   // 悬浮文档
    int    kind;            // LspCompletionItemKind
    int    priority;        // CompletionPriority
    char*  sortText;        // 排序字符串
} CompItem;

/* ========== 补全集合 ========== */

typedef struct {
    CompItem*  items;
    int        count;
    int        capacity;
    int        has_dup;     // 内部标记：是否有重复
} CompletionSet;

/* ========== 上下文检测结果 ========== */

typedef struct {
    CompletionContext type;
    
    // CTX_DOT_ACCESS / CTX_USE_MEMBER
    char*  module_alias;    // 模块别名（如 "maths"）
    char*  member_prefix;   // 成员前缀（如 "Co" 在 "maths.Co|"）
    
    // CTX_NEW / CTX_USE_MODULE / CTX_USE_MEMBER
    char*  prefix;          // 用户已输入的前缀部分
    
    // CTX_DOT_ACCESS（变量成员访问）
    char*  var_type;        // 变量类型（推断结果）
    
    // CTX_STRING_LITERAL
    // （无额外字段）
} CompletionContextInfo;

/* ========== CompletionSet API ========== */

// 创建补全集合
CompletionSet* comp_set_create(void);

// 销毁补全集合
void comp_set_destroy(CompletionSet* set);

// 添加补全项（返回 1=成功, 0=重复被跳过, -1=内存失败）
int comp_set_add(CompletionSet* set,
                 const char* label,
                 int kind,
                 int priority,
                 const char* detail,
                 const char* documentation,
                 const char* insertText,
                 const char* filterText);

// 批量添加（仅当 label 不在 set 中时添加）
int comp_set_add_unique(CompletionSet* set,
                        const char* label,
                        int kind,
                        int priority,
                        const char* detail,
                        const char* documentation,
                        const char* insertText,
                        const char* filterText);

// 排序并转换为 LspCompletionItem 数组
// 返回的数组需要用 lsp_free_completions 释放
// out_count 输出项数
LspCompletionItem* comp_set_to_lsp_array(CompletionSet* set, int* out_count);

/* ========== 上下文检测 ========== */

// 检测补全上下文
CompletionContextInfo comp_detect_context(
    const char* content,
    LspPosition pos,
    const char* file_path,
    int import_count,
    ImportAlias* import_aliases
);

// 释放上下文检测结果
void comp_context_free(CompletionContextInfo* ctx);

/* ========== 内置提供者 ========== */

// 添加所有关键字
void comp_provider_add_keywords(CompletionSet* set, const char* filter);

// 添加所有内置类型
void comp_provider_add_types(CompletionSet* set, const char* filter);

// 添加代码片段
void comp_provider_add_snippets(CompletionSet* set, const char* filter);

// 添加内置函数
void comp_provider_add_builtins(CompletionSet* set, const char* filter);

/* ========== 符号提供者 ========== */

// 添加原生模块方法（含参数类型）
void comp_provider_add_native_modules(
    CompletionSet* set,
    const char* filter
);

// 从编译器符号表添加用户定义符号
void comp_provider_add_user_symbols(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* filter
);

// 添加 self 关键字（结构体方法体内）
void comp_provider_add_self_keyword(
    CompletionSet* set,
    const char* content,
    LspPosition pos
);

/* ========== 上下文特定补全 ========== */

// CTX_NEW: struct 名称 + 模块别名
void comp_provider_add_new_structs(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    int import_count,
    ImportAlias* import_aliases
);

// CTX_USE_MODULE: 模块别名列表
void comp_provider_add_use_modules(
    CompletionSet* set,
    int import_count,
    ImportAlias* import_aliases,
    const char* filter
);

// CTX_USE_MEMBER / CTX_DOT_ACCESS: 模块符号
void comp_provider_add_module_symbols(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* module_alias,
    const char* member_prefix,
    int import_count,
    ImportAlias* import_aliases
);

// CTX_DOT_ACCESS: 变量类型推断 → 成员补全
void comp_provider_add_variable_members(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    const char* var_name,
    int import_count,
    ImportAlias* import_aliases
);

// CTX_STRING_LITERAL: 字符串实例方法
void comp_provider_add_string_methods(CompletionSet* set);

// CTX_TYPE_ANNOTATION: 类型 + 模块类型
void comp_provider_add_type_annotation_types(
    CompletionSet* set,
    const char* content,
    const char* file_path,
    int import_count,
    ImportAlias* import_aliases
);

#ifdef __cplusplus
}
#endif

#endif /* LSP_COMPLETION_H */
