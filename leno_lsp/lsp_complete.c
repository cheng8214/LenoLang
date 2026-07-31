/**
 * 补全服务
 * 提供代码自动补全功能 - 使用 LenoC 编译器符号表
 */

#include "leno_lsp.h"
#include "leno_compiler_lib.h"
#include "leno_builtins.h"
#include "../src/include/native.h"
#include <ctype.h>
#include <stdlib.h>

// 从模块加载器读取模块文件
extern char* read_module_file(const char* file_path, const char* current_file);

// === 拆分的子模块 ===

// 静态数据表（关键字、类型、cstruct 方法）
#include "lsp_complete_inc/comp_data.inc"

// 导入解析（ImportAlias 结构及解析/查找/释放）
#include "lsp_complete_inc/comp_import.inc"

// 补全项管理（add_completion_item）
#include "lsp_complete_inc/comp_item.inc"

// enum 补全
#include "lsp_complete_inc/comp_enum.inc"

// struct 补全（find_enclosing_struct_name / add_symbol_table_struct_completions / add_current_file_struct_completions）
#include "lsp_complete_inc/comp_struct.inc"

// face 补全（find_face_methods_in_content）
#include "lsp_complete_inc/comp_face.inc"

// 模块 struct 方法补全（add_module_struct_methods）
#include "lsp_complete_inc/comp_module_struct.inc"

// 模块符号补全（add_module_symbol_completions）
#include "lsp_complete_inc/comp_module_symbol.inc"

// 工具函数（is_string_literal_before_dot / is_type_annotation_context / get_word_before_cursor / parse_module_prefix / detect_use_context）
#include "lsp_complete_inc/comp_utils.inc"

// 变量类型推断与编译器符号表补全（get_variable_type / add_symbols_from_compiler）
#include "lsp_complete_inc/comp_var_type.inc"

// LSP 补全请求处理（lsp_handle_completion）
#include "lsp_complete_inc/comp_handle.inc"

// 主补全逻辑（lsp_get_completions / lsp_free_completions）
#include "lsp_complete_inc/comp_main.inc"
