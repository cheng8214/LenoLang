#ifndef LENO_SEMANTIC_H
#define LENO_SEMANTIC_H

#include "leno_types.h"
#include "leno_ast.h"
#include "leno_vm.h"
#include "module_symbol_table.h"

// ============================================================================
// 导入模块信息（用于语义分析时检查模块方法）
// ============================================================================

typedef struct {
    char* alias;                    // 模块别名（如 test）
    char* file_path;                // 模块文件路径（如 test.leno）
    ModuleSymbolTable* sym_table;   // 模块符号表（缓存所有导出符号）
} ImportedModuleInfo;

// ============================================================================
// 函数表哈希表条目
// ============================================================================

typedef struct FuncEntry {
    char* name;         // 函数名
    Ast* func;          // 函数 AST 节点
    struct FuncEntry* next;  // 链式冲突处理
} FuncEntry;

// ============================================================================
// 函数表（哈希表实现 - O(1) 查找）
// ============================================================================

typedef struct {
    FuncEntry** entries;    // 哈希表数组
    int capacity;           // 表容量
    int count;              // 当前函数数量
} FuncTable;

// ============================================================================
// 单遍语义分析（解决前向引用 + 闭包）
// ============================================================================

typedef struct {
    Scope* current;
    Scope* root_scope;
    Ast* root;
    FuncTable func_table;   // 函数表（哈希表，O(1) 查找）
    Ast* current_func;  // 当前正在分析的函数
    Ast* func_stack[64]; // 函数栈，用于处理多层嵌套闭包
    int func_stack_depth; // 函数栈深度
    int local_index;    // 全局局部变量索引计数器（所有作用域共享）
    ImportedModuleInfo imported_modules[64];  // 导入的模块信息表
    int imported_module_count;  // 导入的模块数量
    int is_module;      // 是否为模块模式
    int is_lsp_mode;    // 是否为 LSP 模式（保留所有作用域供符号查询）
} Semantic;

// 函数表 API
void func_table_init(FuncTable* table);
void func_table_free(FuncTable* table);
int func_table_add(FuncTable* table, const char* name, Ast* func);
Ast* func_table_find(FuncTable* table, const char* name);

void semantic_init(Semantic* s, Ast* root);
void semantic_analyze(Semantic* s, Ast* ast);  // 单遍分析
void semantic_analyze_module(Semantic* s, Ast* ast);  // 模块语义分析
void semantic_cleanup(Semantic* s);

#endif // LENO_SEMANTIC_H
