#ifndef SEMANTIC_INTERNAL_H
#define SEMANTIC_INTERNAL_H

#include "include/lenolang.h"
#include "include/leno_ast.h"
#include "include/leno_semantic.h"
#include "include/native.h"
#include "include/module_loader.h"
#include "include/module_symbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// 前置声明 - 类型推断
// ============================================================================
TypeInfo* infer_expr_type(Semantic* s, Ast* ast);

// ============================================================================
// 前置声明 - 变量解析
// ============================================================================
int allocate_local_index(Semantic* s);

// ============================================================================
// 前置声明 - upvalue 管理
// ============================================================================
ImportedModuleInfo* find_imported_module(Semantic* s, const char* alias);
int add_upvalue(Ast* func_ast, const char* name, int index, int is_local, int is_value_capture);
Symbol* resolve_variable_with_upvalue(Semantic* s, const char* name, SymRef* ref);

// ============================================================================
// 前置声明 - AST 访问
// ============================================================================
void visit(Semantic* s, Ast* ast);
void visit_list(Semantic* s, AstList* list);
void visit_func(Semantic* s, Ast* ast);
void visit_func_as_struct_method(Semantic* s, Ast* ast);
void infer_generic_bindings(TypeInfo* param_type, TypeInfo* arg_type,
                            char** param_names, TypeInfo** inferred, int count);

// ============================================================================
// 前置声明 - struct 方法字段访问转换
// ============================================================================
void transform_method_body(Ast* ast, char** field_names, int field_count, char** method_names, int method_count, const char* struct_name);

// ============================================================================
// 类型工具函数
// ============================================================================
int type_utils_is_array_element_mutator(const char* method_name);
int type_utils_get_array_element_param_index(const char* method_name, int is_module_call);
int type_utils_try_update_array_element_type(Symbol* arr_sym, TypeInfo* elem_type);
int type_utils_try_update_nested_array_element_type(Symbol* arr_sym, TypeInfo* elem_type);
int type_utils_try_update_nested_array_element_type_ex(Symbol* arr_sym, Ast* index_ast, TypeInfo* elem_type);
Symbol* type_utils_resolve_var_symbol(Semantic* s, Ast* ast);

// 字典类型检查工具函数
int type_utils_is_dict_element_mutator(const char* method_name);
int type_utils_get_dict_element_param_index(const char* method_name);
int type_utils_try_update_dict_value_type(Symbol* dict_sym, TypeInfo* value_type);


// 安全格式化类型错误信息（避免 type_to_string 缓冲区覆盖）
void format_type_error(char* buf, size_t buf_size, const char* fmt,
                       TypeInfo* type1, TypeInfo* type2,
                       const char* str1, const char* str2);

// 获取类型转换建议
const char* get_type_conversion_hint(TypeKind expected, TypeKind actual);

// 生成详细的类型错误信息（包含转换建议）
void format_detailed_type_error(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context);

// 数组索引赋值类型检查工具函数
int type_utils_check_array_index_assignment(TypeInfo* obj_type, TypeInfo* value_type, int line);

// 字典索引赋值类型检查工具函数
int type_utils_check_dict_index_assignment(Symbol* dict_sym, TypeInfo* assign_type, int line);

#endif // SEMANTIC_INTERNAL_H
