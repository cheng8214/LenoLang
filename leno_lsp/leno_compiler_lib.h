/**
 * LenoC 编译器库接口头文件
 * 供 LSP 服务器使用 - 简化版本
 */

#ifndef LENO_COMPILER_LIB_H
#define LENO_COMPILER_LIB_H

#include "../src/include/lenolang.h"

// 编译上下文
typedef struct {
    Scope* root_scope;
    Ast* ast_root;
    bool has_errors;
} CompilerContext;

// ==================== 上下文管理 ====================

// 初始化编译上下文
bool compiler_context_init(CompilerContext* ctx);

// 清理编译上下文
void compiler_context_cleanup(CompilerContext* ctx);

// ==================== 编译分析 ====================

// 编译源代码（仅分析和收集信息，不执行）
bool compiler_analyze_with_filename(CompilerContext* ctx, const char* source, const char* filename);

// ==================== 符号查询 ====================

// 获取符号信息（从作用域中查找）
bool compiler_get_symbol_info(CompilerContext* ctx, const char* name, 
                               char** type_str_out, bool* is_global_out);

// 获取所有符号（用于补全）
int compiler_get_all_symbols(CompilerContext* ctx, char*** names_out, char*** types_out);

// 释放符号列表
void compiler_free_symbol_list(char** names, char** types, int count);

// 从指定 struct 中获取字段类型信息
bool compiler_get_struct_field_info(CompilerContext* ctx, const char* struct_name,
                                     const char* field_name, char** type_str_out);

// 获取所有包含指定字段的 struct 名称列表
// 返回找到的 struct 数量，names_out 需要调用者释放
int compiler_find_structs_with_field(CompilerContext* ctx, const char* field_name,
                                      char*** struct_names_out);

#endif // LENO_COMPILER_LIB_H
