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
TypeInfo* infer_method_return_type(Semantic* s, TypeInfo* obj_type, const char* method_name);
TypeInfo* infer_field_type(Semantic* s, TypeInfo* obj_type, const char* field_name, int* out_field_index);

// 把模块符号表里的一条 struct/cstruct 符号，按完整精度搬进当前作用域的符号
// （字段类型 + 泛型参数）。这是"怎么把模块里的 struct 字段搬进当前作用域"的**唯一实现**：
// 此前 AST_USE 与 import_type_deps 各写一遍，后者丢嵌套泛型（详见 semantic_type_utils.c）。
void semantic_attach_struct_fields(Symbol* sym, const ModuleStructSymbol* ssym);

// 把模块符号表里的一条 struct/cstruct 符号**完整注册**进当前编译：
// 全局 struct_def（含泛型参数/方法名/impl）+ 方法占位符注册到 func_table
// （带 param_types / type_params / 返回泛型实参 —— 泛型替换靠它）。
// 这是这件事的**唯一实现**：此前 AST_USE 与 import_type_deps 各写一份，后者贫到
// 泛型方法经那条路会**静默跳过类型检查**（详见 semantic_type_utils.c 的说明）。
void semantic_register_struct_from_module(Semantic* s, const ModuleStructSymbol* ssym);

// 跨模块 struct 方法的实参检查（**唯一实现**）：方法调用的实参检查在本仓有两份拷贝，
// 两份都只在本编译单元的 root AST 里找 struct 声明 ⇒ 跨模块时整块跳过。
// 本函数改用 semantic_register_struct_from_module 注册的**方法占位符**（key = "Struct::method"）
// 做"参数过多 + 逐参类型"检查（不查参数不足：占位符不记默认值个数）。
// 返回：1 = 按占位符检查过；0 = 没有占位符。
int semantic_check_method_args_from_placeholder(Semantic* s, const char* struct_name,
                                                const char* method_name, AstList* args,
                                                int line, int column);

// face 方法的实参检查（唯一实现；调用点在 visit_module.inc 的 MODULE_CALL 分流处）。
// 同文件用解析器 face AST 的 method_param_types 判逐参类型；跨模块只有 param_count ⇒ 只判个数。
int semantic_check_face_method_args(Semantic* s, const char* face_name, const char* method_name,
                                    AstList* args, int line, int column);

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

// T11：「该变量的值**确定为 null**」⇒ 编译错误（教程：null 不能参与算术运算）。
//   判据是 Symbol.is_null_value —— 声明即 null 且此后没被写过（见 visit_var.inc 的置位/清位点，
//   与 semantic_type.c 的 report_known_null_name 实现）。
void report_known_null_name(Semantic* s, const char* name, int line, int column);

// ============================================================================
// 前置声明 - AST 访问
// ============================================================================
void visit(Semantic* s, Ast* ast);
void visit_list(Semantic* s, AstList* list);
void visit_func(Semantic* s, Ast* ast);
void visit_func_as_struct_method(Semantic* s, Ast* ast);
void infer_generic_bindings(TypeInfo* param_type, TypeInfo* arg_type,
                            char** param_names, TypeInfo** inferred, int count);
void resolve_generic_in_type(TypeInfo* type, char** type_params, char** type_param_constraints, int count);

// ============================================================================
// 前置声明 - struct 方法字段访问转换
// ============================================================================
void transform_method_body(Ast* ast, char** field_names, int field_count, char** method_names, int method_count, const char* struct_name,
    char** param_names, int param_count, char** const_names, int const_count);

// ============================================================================
// 类型工具函数
// ============================================================================
int resolve_alias_in_type(Semantic* s, TypeInfo** type_ptr, int line);
// 递归检查泛型实参/子类型中是否存在未定义类型（不查顶层）——B2 修复用。
// 返回 1 = 报过"未定义"错（调用方应置 declared_type_undefined 跳过级联检查）
int semantic_check_undefined_subtypes(Semantic* s, TypeInfo* type, int line, int column);
// 泛型形参替换：与 type_substitute 相同语义，但额外识别"TYPE_STRUCT 占位形参"
// （struct_name=形参名，如字段类型 Array[T] 里的 T）。返回新类型（调用方 free 旧值）。
TypeInfo* semantic_substitute_generic_param(TypeInfo* type, const char* param_name, TypeInfo* concrete);
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
const char* get_similar_name_hint(Scope* scope, const char* name);

// 生成详细的类型错误信息（包含转换建议）
void format_detailed_type_error(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context);
// 扩展版本：支持变量名
void format_detailed_type_error_ex(char* buf, size_t buf_size,
                                TypeInfo* expected, TypeInfo* actual,
                                const char* context, const char* var_name);

// 数组索引赋值类型检查工具函数
int type_utils_check_array_index_assignment(TypeInfo* obj_type, TypeInfo* value_type, int line, int column);

// 字典索引赋值类型检查工具函数
int type_utils_check_dict_index_assignment(Symbol* dict_sym, TypeInfo* assign_type, int line, int column);

#endif // SEMANTIC_INTERNAL_H
