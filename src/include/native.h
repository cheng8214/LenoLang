#ifndef NATIVE_H
#define NATIVE_H

#include "lenolang.h"

// Native 函数元信息（编译时和运行时都可用）
typedef struct {
    const char* name;
    int arity;              // 参数个数，-1 表示可变参数
    int min_arity;          // 最小参数个数（仅 arity == -1 时有效，-1 表示不限制）
    int max_arity;          // 最大参数个数（仅 arity == -1 时有效，-1 表示不限制）
    TypeKind return_type;
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS];  // 参数类型数组
} NativeFunctionMeta;

// 模块方法元信息
#define MAX_METHOD_PARAMS 8  // 最大参数数量
typedef struct {
    char module_name[32];
    char method_name[32];
    int arity;              // 参数个数，-1 表示可变参数
    int min_arity;          // 最小参数个数（仅 arity == -1 时有效，-1 表示不限制）
    int max_arity;          // 最大参数个数（仅 arity == -1 时有效，-1 表示不限制）
    TypeKind return_type;
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS];  // 参数类型数组
    NativeFn function;
} ModuleMethodMeta;

// 编译时注册 native 函数元信息（供模块使用）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// return_element_type: 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
void native_register_meta(const char* name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 获取所有注册的 native 函数元信息（编译时使用）
const NativeFunctionMeta* native_get_all_functions(int* count);

// 根据函数名获取返回类型
TypeKind native_get_return_type(const char* name);

// 获取全局函数的返回数组元素类型（编译时调用）
TypeKind native_get_return_element_type(const char* name);

// 获取全局函数的参数类型
TypeKind native_get_global_function_param_type(const char* name, int param_index);

// 运行时注册 native 函数
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
void vm_register_native(const char* name, NativeFn function, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 获取 native 函数的返回类型（运行时使用）
TypeKind vm_get_native_return_type(const char* name);

// 重置 native 函数注册表（编译前调用）
void native_reset_registry(void);

// 注册所有模块的 native 函数元信息（编译时调用）
void native_register_all_module_metas(void);

// 根据名称查找 native 函数对象（运行时使用）
ObjNative* native_find_function(const char* name);

// 标记所有 native 函数对象（供 GC 使用）
void native_mark_all_functions(void);

// ========== 模块方法支持 ==========

// 注册模块方法（带参数类型）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// param_types: 参数类型数组，长度为 arity，如果为 NULL 则所有参数默认为 TYPE_ANY
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_module_method(const char* module_name, const char* method_name,
                                   NativeFn function, int arity, int min_arity, int max_arity,
                                   TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 获取模块方法的参数类型
TypeKind native_get_module_method_param_type(const char* module_name, const char* method_name, int param_index);

// 根据模块名和方法名查找模块方法
ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name);

// 获取模块方法的返回类型
TypeKind native_get_module_method_return_type(const char* module_name, const char* method_name);

// 获取模块方法返回数组时的元素类型（编译时调用）
TypeKind native_get_module_method_return_element_type(const char* module_name, const char* method_name);

// 获取模块方法的参数数量（编译时调用）
int native_get_module_method_arity(const char* module_name, const char* method_name);

// 获取模块的所有方法名（LSP 使用）
// 返回方法名数组，通过 count 返回数量，需要调用者用 free_module_method_list 释放
char** native_get_module_methods(const char* module_name, int* count);

// 释放模块方法名列表
void native_free_module_method_list(char** methods, int count);

// 获取所有模块名称列表（LSP 使用）
// 返回模块名数组，通过 count 返回数量，需要调用者用 native_free_module_list 释放
char** native_get_all_modules(int* count);

// 释放模块名列表
void native_free_module_list(char** modules, int count);

// 获取模块的所有方法元数据（LSP 使用）
// 返回 ModuleMethodMeta 数组的副本，通过 count 返回数量，需要调用者释放
ModuleMethodMeta* native_get_module_method_metas(const char* module_name, int* count);

// 释放模块方法元数据数组
void native_free_module_method_metas(ModuleMethodMeta* metas);

// 初始化 io 模块（import io 时调用）
void io_init_module(void);

// ========== 模块常量支持 ==========

// 注册模块常量（原生模块可导出 int 常量，如 guis.LOGICAL_PRESENTATION_STRETCH）
void native_register_module_const(const char* module_name, const char* const_name, int value);

// 查找模块常量值，未找到返回 0 且 *found 设为 false
int native_find_module_const(const char* module_name, const char* const_name, bool* found);

// 获取模块的所有常量名（LSP 使用）
// 返回常量名数组，通过 count 返回数量，需要调用者用 native_free_module_const_list 释放
char** native_get_module_consts(const char* module_name, int* count);

// 释放模块常量名列表
void native_free_module_const_list(char** consts, int count);

// ========== 模块别名支持 ==========

// 注册模块别名
void native_register_module_alias(const char* alias, const char* module_name);

// 根据别名查找实际模块名
const char* native_resolve_module_alias(const char* alias);

// 重置别名表
void native_reset_module_aliases(void);

// 统一模块初始化
void native_init_module(const char* module_name);

// 注册所有内置 Native 函数（全局函数）
void native_register_globals(void);

// 检查模块名是否是原生模块（如 io, types, times）
int native_is_module(const char* module_name);

// 检查名称是否是内部模块名称（供 parser 使用）
bool native_is_builtin_module(const char* name);

// 创建原生函数对象的辅助函数
ObjNative* make_native(NativeFn fn, int arity, const char* name);

// ========== 实例方法元信息支持（编译期检查用） ==========

// 实例方法元信息
#define MAX_INSTANCE_METHOD_METAS 64
typedef struct {
    char type_name[32];     // 类型名（如 "array", "string", "dict" 等）
    char method_name[32];   // 方法名
    int arity;              // 参数个数（不包括receiver），-1 表示可变参数
    int min_arity;          // 最小参数个数（仅 arity == -1 时有效，-1 表示不限制）
    int max_arity;          // 最大参数个数（仅 arity == -1 时有效，-1 表示不限制）
    TypeKind return_type;   // 返回类型
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS]; // 参数类型数组
} InstanceMethodMeta;

// 注册实例方法元信息（编译时调用）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type);

// 注册实例方法元信息（带参数类型）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta_with_params(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 获取实例方法的参数数量（编译时调用）
int native_get_instance_method_arity(const char* type_name, const char* method_name);

// 获取实例方法的返回类型（编译时调用）
TypeKind native_get_instance_method_return_type(const char* type_name, const char* method_name, int* out_arity);

// 获取实例方法返回数组时的元素类型（编译时调用）
TypeKind native_get_instance_method_return_element_type(const char* type_name, const char* method_name);

// 获取实例方法的参数类型（编译时调用）
TypeKind native_get_instance_method_param_type(const char* type_name, const char* method_name, int param_index);

// 根据类型名和方法名查找实例方法元信息（编译时调用）
// 返回指向实例方法元信息的指针，未找到返回 NULL
const InstanceMethodMeta* native_find_instance_method(const char* type_name, const char* method_name);

// 根据方法名查找实例方法元信息（编译时调用）
// 返回类型名称（如 "array"），如果找到则通过 out_arity 返回参数数量，通过 out_return_type 返回返回类型
const char* native_find_instance_method_type(const char* method_name, int* out_arity, TypeKind* out_return_type);

// 重置实例方法元信息表（编译前调用）
void native_reset_instance_method_metas(void);

// 注册所有内置类型的实例方法元信息（编译时调用）
void native_register_all_instance_method_metas(void);

// 根据 TypeKind 获取类型名称（编译时调用）
// 返回 "array", "string", "dict" 等，如果无法确定返回 NULL
const char* native_get_type_name(TypeKind kind);

// 获取类型的所有实例方法名（LSP 使用）
// 返回方法名数组，通过 count 返回数量，需要调用者用 free_instance_method_list 释放
char** native_get_instance_methods(const char* type_name, int* count);

// 释放实例方法名列表
void native_free_instance_method_list(char** methods, int count);

// 获取当前执行行号（供原生函数使用）
int native_get_current_line(void);

// 抛出运行时错误（供原生函数使用，会自动获取当前行号）
void native_throw_error(const char* msg);

Value ffi_reload_library(const char* path);

#endif
