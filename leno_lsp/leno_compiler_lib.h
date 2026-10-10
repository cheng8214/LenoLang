/**
 * Leno 编译器库接口头文件
 * 供 LSP 服务器使用 - 简化版本
 */

#ifndef LENO_COMPILER_LIB_H
#define LENO_COMPILER_LIB_H

#include "../src/include/lenolang.h"
#include "../src/include/leno_ast.h"
#include "../src/include/leno_lexer.h"
#include "../src/include/leno_parser.h"
#include "../src/include/leno_semantic.h"

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

// ---------------------------------------------------------------------------
// 类型**显示串** → **查表真名**（只挪指针、不拷贝，返回的指针可直接用）
//   为什么需要：compiler_get_struct_field_info / compiler_get_symbol_info 返回的是
//   `type_to_string()` 的**显示**口径（结构体是 `"struct Sound"`、接口是 `"face X"`、
//   C 布局是 `"cstruct X"`），而所有查表 API 要的是**真名**：
//     struct_def_find / module_symbol_table_find_struct / native_get_instance_method_arity …
//   历史上调用方直接把显示串当键 ⇒ 链式成员悬停整条链落空，只剩"是 X 类型变量的成员"兜底：
//     [HOVER-DEBUG] generate_struct_method_doc_from_modules: struct='struct Sound' method='setVolume'
//     [HOVER-DEBUG]     struct 'struct Sound' NOT found in this module
//   ⇒ 拿不到方法签名（用户实测：`g.bgm.setVolume(...)` 悬停没有参数提示）。
//   ⚠ 显示串本身**不改**（它是给人看的）：只在"要拿它去查表"的地方过这个函数 ✓
const char* lsp_type_key(const char* name);

#endif // LENO_COMPILER_LIB_H
