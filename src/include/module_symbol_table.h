#ifndef MODULE_SYMBOL_TABLE_H
#define MODULE_SYMBOL_TABLE_H

#include "leno_types.h"

// 模块函数符号
typedef struct {
    char* name;                 // 函数名
    TypeKind return_type;       // 返回类型（基本类型枚举，向后兼容）
    TypeInfo* return_type_info; // 完整返回类型信息（支持 Dict[K,V]/Array[T] 等泛型）
    char* return_struct_name;   // 如果返回类型是 struct，存储 struct 名称
    int type_param_count;       // 泛型类型参数数量（如 identity[T] 的 type_param_count=1）
} ModuleFuncSymbol;

// 模块 struct 字段
typedef struct {
    char* name;                 // 字段名
    TypeKind type;              // 字段类型
    TypeKind element_type;      // Array[T]/Dict[K,V] 中 T/V 的类型（当 type 为 TYPE_ARRAY/TYPE_DICT/TYPE_PTR_GENERIC 时有效）
    char* struct_name;          // 类型名（当 type 为 TYPE_STRUCT/TYPE_CLIB/TYPE_CSTRUCT/TYPE_FACE 时）
    char* element_struct_name;  // Array[T]/Dict[K,V] 中 T/V 的类型名（当 element_type 为 TYPE_STRUCT/TYPE_FACE/TYPE_CSTRUCT/TYPE_CLIB 时）
} ModuleStructField;

// 模块 struct 方法
typedef struct {
    char* name;                 // 方法名
    TypeKind return_type;       // 返回类型（基本类型枚举，向后兼容）
    TypeInfo* return_type_info; // 完整返回类型信息（支持 Dict[K,V]/Array[T] 等泛型）
    char* return_struct_name;   // 如果返回类型是 struct，存储 struct 名称
    char* return_type_param_name; // 如果返回类型是泛型参数（如 T），存储参数名
    int return_generic_count;   // 返回类型的泛型参数数量（如 Holder[K] 的 return_generic_count=1）
    char** return_generic_param_names; // 返回类型的泛型参数名数组（如 ["K"]）
    int param_count;            // 参数数量（不包括 self）
    TypeKind* param_types;      // 参数类型数组（不包括 self）
    char** param_generic_names; // 参数泛型类型参数名（如 "T", "K"），用于泛型方法参数类型检查
} ModuleStructMethod;

// 模块 struct 符号
typedef struct {
    char* name;                 // struct 名称
    int field_count;            // 字段数量
    ModuleStructField* fields;  // 字段数组
    int method_count;           // 方法数量
    ModuleStructMethod* methods; // 方法数组
    int is_cstruct;             // 是否是 cstruct (1 = 是, 0 = 否)
    int impl_count;             // 实现的 face 数量
    char** impl_names;          // 实现的 face 名称数组
    int type_param_count;       // 泛型类型参数数量（如 Box[T] 的 type_param_count=1）
    char** type_param_names;    // 泛型类型参数名称数组（如 ["T"] 或 ["K","V"]）
} ModuleStructSymbol;

// 模块 enum 符号
typedef struct {
    char* name;                 // enum 名称
    int member_count;           // 成员数量
    char** member_names;        // 成员名称数组
    int* member_values;         // 成员值数组
} ModuleEnumSymbol;

// 模块 clib 函数符号
typedef struct {
    char* name;                 // 函数名
    TypeKind return_type;       // 返回类型
    TypeKind return_element_type; // Ptr[T] 中的 T，TYPE_PTR 表示无
    char* return_struct_name;   // 返回 struct 名（为 NULL 则为空字符串兼容）
    int param_count;            // 参数数量
    TypeKind* param_types;      // 参数类型数组
    TypeKind* param_element_types; // 参数 Ptr[T] 中的 T 数组，TYPE_PTR 表示无
    char** param_struct_names;  // 参数 struct 名数组（可为 NULL）
} ModuleClibFuncSymbol;

// 模块 clib 符号
typedef struct {
    char* name;                 // clib 名称
    int func_count;             // 函数数量
    ModuleClibFuncSymbol* funcs; // 函数定义数组
} ModuleClibSymbol;

// 模块 face 方法符号
typedef struct {
    char* name;                 // 方法名
    TypeKind return_type;       // 返回类型
    char* return_struct_name;   // 如果返回类型是 struct，存储 struct 名称
    int param_count;            // 参数数量
} ModuleFaceMethodSymbol;

// 模块 face 符号
typedef struct {
    char* name;                 // face 名称
    int method_count;           // 方法数量
    ModuleFaceMethodSymbol* methods; // 方法数组
    int type_param_count;       // 泛型类型参数数量（如 Comparable[T] 的 type_param_count=1）
} ModuleFaceSymbol;

// 模块变量符号
typedef struct {
    char* name;                 // 变量名
    TypeKind type;              // 变量类型
    char* struct_name;          // 如果类型是 struct，存储 struct 名称
    int is_const;               // 是否为 const 声明
} ModuleVarSymbol;

// 模块类型别名符号
typedef struct {
    char* name;                 // 别名
    TypeInfo* type_info;        // 完整类型信息（支持 Array[T]/Dict[K,V] 等复杂类型）
} ModuleAliasSymbol;

// 模块符号表
typedef struct {
    char* module_path;          // 模块文件路径
    ModuleFuncSymbol* funcs;    // 函数符号数组
    int func_count;             // 函数数量
    int func_capacity;          // 函数数组容量
    ModuleStructSymbol* structs; // struct 符号数组
    int struct_count;           // struct 数量
    int struct_capacity;        // struct 数组容量
    ModuleEnumSymbol* enums;    // enum 符号数组
    int enum_count;             // enum 数量
    int enum_capacity;          // enum 数组容量
    ModuleFaceSymbol* faces;    // face 符号数组
    int face_count;             // face 数量
    int face_capacity;          // face 数组容量
    ModuleVarSymbol* vars;      // 变量符号数组
    int var_count;              // 变量数量
    int var_capacity;           // 变量数组容量
    ModuleAliasSymbol* aliases; // 类型别名符号数组
    int alias_count;            // 别名数量
    int alias_capacity;         // 别名数组容量
    ModuleClibSymbol* clibs;    // clib 符号数组
    int clib_count;             // clib 数量
    int clib_capacity;          // clib 数组容量
} ModuleSymbolTable;

// 创建模块符号表
ModuleSymbolTable* module_symbol_table_create(const char* module_path);

// 销毁模块符号表
void module_symbol_table_destroy(ModuleSymbolTable* table);

// 扫描模块文件并填充符号表
// current_file: 当前文件路径（用于解析相对路径）
// 返回: 0 成功，-1 失败
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file);

// 查找函数符号
ModuleFuncSymbol* module_symbol_table_find_func(ModuleSymbolTable* table, const char* func_name);

// 查找 struct 符号
ModuleStructSymbol* module_symbol_table_find_struct(ModuleSymbolTable* table, const char* struct_name);

// 添加函数符号
void module_symbol_table_add_func(ModuleSymbolTable* table, const char* name, TypeKind return_type, const char* return_struct_name, int type_param_count, TypeInfo* return_type_info);

// 添加 struct 符号
void module_symbol_table_add_struct(ModuleSymbolTable* table, const char* name, int field_count, ModuleStructField* fields, int method_count, ModuleStructMethod* methods, int is_cstruct, int type_param_count, char** type_param_names);

// 查找 struct 方法
ModuleStructMethod* module_symbol_table_find_struct_method(ModuleSymbolTable* table, const char* struct_name, const char* method_name);

// 查找 enum 符号
ModuleEnumSymbol* module_symbol_table_find_enum(ModuleSymbolTable* table, const char* enum_name);

// 添加 enum 符号（member_values[i] = -1 表示无显式值，使用自动递增）
void module_symbol_table_add_enum(ModuleSymbolTable* table, const char* name, int member_count, char** member_names, int* member_values);

// 查找 face 符号
ModuleFaceSymbol* module_symbol_table_find_face(ModuleSymbolTable* table, const char* face_name);

// 添加 face 符号
void module_symbol_table_add_face(ModuleSymbolTable* table, const char* name, int method_count, ModuleFaceMethodSymbol* methods, int type_param_count);

// 查找变量符号
ModuleVarSymbol* module_symbol_table_find_var(ModuleSymbolTable* table, const char* var_name);

// 添加变量符号
void module_symbol_table_add_var(ModuleSymbolTable* table, const char* name, TypeKind type, const char* struct_name, int is_const);

// 查找别名符号
ModuleAliasSymbol* module_symbol_table_find_alias(ModuleSymbolTable* table, const char* alias_name);

// 添加别名符号
void module_symbol_table_add_alias(ModuleSymbolTable* table, const char* name, TypeInfo* type_info);

// 查找 clib 符号
ModuleClibSymbol* module_symbol_table_find_clib(ModuleSymbolTable* table, const char* clib_name);

// 添加 clib 符号
void module_symbol_table_add_clib(ModuleSymbolTable* table, const char* name, int func_count, ModuleClibFuncSymbol* funcs);

// clib 符号数量
int module_symbol_table_clib_count(ModuleSymbolTable* table);

// 从字符串解析完整类型（支持 int/float/string/bool 及 Array[T]/Dict[K,V]）
TypeInfo* parse_type_from_string(const char* type_str);

#endif // MODULE_SYMBOL_TABLE_H
