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
#ifndef MAX_METHOD_PARAMS
#define MAX_METHOD_PARAMS 8  // 最大参数数量
#endif
typedef struct {
    char module_name[32];
    char method_name[32];
    int arity;              // 参数个数，-1 表示可变参数
    int min_arity;          // 最小参数个数（仅 arity == -1 时有效，-1 表示不限制）
    int max_arity;          // 最大参数个数（仅 arity == -1 时有效，-1 表示不限制）
    TypeKind return_type;
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS];  // 参数类型数组
    // 完整返回类型规格（可空）：非 NULL 时**优先于** return_type/return_element_type ——
    // 它才能表达 `Array[DirEntry]` / `Dict[string,string]` 这类参数化、带名字的类型
    // （见 leno_types.h 的 NativeTypeSpec 说明）。为 NULL ⇒ 退回上面两个 Kind 的老路径。
    const NativeTypeSpec* return_spec;
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

// 供语义层"未定义函数"相似名提示遍历（C2）
int native_get_name_count(void);
const char* native_get_name(int index);

// C1：编译期实例方法表中的相似方法名提示（语义分析阶段方法运行表未建，
// 用 native_register_all_instance_method_metas 注册的元信息表遍历）
const char* native_instance_method_hint(const char* type_name, const char* method_name);

// D1：内置模块名相似提示（"未定义的模块或变量"报错时给"是否想用"候选）
const char* native_builtin_module_hint(const char* name);

// 标记所有 native 函数对象（供 GC 使用）
void native_mark_all_functions(void);

// ========== 模块方法支持 ==========

// 注册模块方法（**唯一入口**；v3.2.3 起返回类型用完整规格声明，老的
// `native_register_module_method(...)` 已删除 —— 它只有一个 TypeKind 槽，表达不了
// `Array[DirEntry]` / `Dict[string,string]` 这类类型）。
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// param_types: 参数类型数组，长度为 arity，如果为 NULL 则所有参数默认为 TYPE_ANY
// return_spec: 见 leno_types.h 的 NativeTypeSpec；常用形状直接用下面的 NATIVE_T_* 预制规格
void native_register_module_method_spec(const char* module_name, const char* method_name,
                                        NativeFn function, int arity, int min_arity, int max_arity,
                                        const NativeTypeSpec* return_spec, TypeKind* param_types);

// 获取模块方法的参数类型
TypeKind native_get_module_method_param_type(const char* module_name, const char* method_name, int param_index);

// 根据模块名和方法名查找模块方法
ModuleMethodMeta* native_find_module_method(const char* module_name, const char* method_name);

// 获取模块方法的返回类型
TypeKind native_get_module_method_return_type(const char* module_name, const char* method_name);

// ========== native 类型规格（完整类型通道，v3.2.3） ==========
// 预制规格：覆盖各模块现有的 19 种返回类型组合（237 个调用点）⇒ 调用点写
//   `&NATIVE_T_STRING` / `&NATIVE_T_ARR_STRING` / `&NATIVE_T_DICT` 即可，不必各自写 static。
// ⚠ 语义与老的 `(return_type, return_element_type)` **逐字对应**：
//   · `NATIVE_T_ARR`  = `TYPE_ARRAY + TYPE_UNKNOWN`（元素未指定 ⇒ 编译器拿裸 Array）
//   · `NATIVE_T_ARR_ANY` = `TYPE_ARRAY + TYPE_ANY`（显式 Array[any]）
//   · `NATIVE_T_DICT` = `TYPE_DICT + TYPE_UNKNOWN`（裸 Dict，K/V 未指定）
//   需要其它形状（如 `Array[DirEntry]`、`Dict[string,string]`）就在模块里自己写 static 规格。
extern const NativeTypeSpec NATIVE_T_ANY;
extern const NativeTypeSpec NATIVE_T_INT;
extern const NativeTypeSpec NATIVE_T_FLOAT;
extern const NativeTypeSpec NATIVE_T_STRING;
extern const NativeTypeSpec NATIVE_T_BOOL;
extern const NativeTypeSpec NATIVE_T_NULL;
extern const NativeTypeSpec NATIVE_T_PTR;
extern const NativeTypeSpec NATIVE_T_FILE;
extern const NativeTypeSpec NATIVE_T_SOCKET;
extern const NativeTypeSpec NATIVE_T_CHANNEL;
extern const NativeTypeSpec NATIVE_T_THREAD;
extern const NativeTypeSpec NATIVE_T_FUTURE;
extern const NativeTypeSpec NATIVE_T_ARR;          // 裸 Array（元素未指定）
extern const NativeTypeSpec NATIVE_T_ARR_ANY;      // Array[any]
extern const NativeTypeSpec NATIVE_T_ARR_INT;      // Array[int]
extern const NativeTypeSpec NATIVE_T_ARR_STRING;   // Array[string]
extern const NativeTypeSpec NATIVE_T_ARR_ARR;      // Array[Array]
extern const NativeTypeSpec NATIVE_T_ARR_DICT;     // Array[Dict]
extern const NativeTypeSpec NATIVE_T_DICT;         // 裸 Dict（K/V 未指定）
// ---- 关系型标签（v3.2.6）：“返回类型跟第 0 个实参走”（实例方法里 receiver 即第 0 个）----
// ⚠ 这是"关系"而不是"类型"：调用点拿得到实参类型才算得出结果，拿不到就退化成 any（保守）。
//   arrays / dicts / rands / sockets 共用这几份 ⇒ 单一来源，免得各模块各抄一份可能漂的副本。
extern const NativeTypeSpec NATIVE_T_ARG0_ELEM;      // 第 0 个实参的元素类型（Array[T] → T）
extern const NativeTypeSpec NATIVE_T_ARG0_KEY;       // 第 0 个实参的键类型（Dict[K,V] → K）
extern const NativeTypeSpec NATIVE_T_ARG0_VALUE;     // 第 0 个实参的值类型（Dict[K,V] → V）
extern const NativeTypeSpec NATIVE_T_ARR_ARG0_ELEM;  // Array[第 0 个实参的元素类型]

// 注册一个 native 结构体规格（**编译期字段表 + 运行期 ObjStructDef 同一来源**）。
// 可在任意 *_init_module() 里反复调用（同名只登记一次）；表满（64）静默忽略。
// ⚠ 规格里的 module_name 必须是**拥有该类型的模块名**（`use <module>.<Type>` 的左侧，
//   见 NativeStructSpec 的说明）；它是 2026-09-27 新增的字段。
void native_register_struct_spec(const NativeStructSpec* spec);

// 按名查 native 结构体规格（编译期字段解析兜底用；未注册 ⇒ NULL）
const NativeStructSpec* native_find_struct_spec(const char* name);

// 按 **(模块名, 类型名)** 查 native 结构体规格 —— `use <module>.<Type>` 通道的唯一来源。
//   module_name 必须已解析过别名（调用方用 native_resolve_module_alias）；未注册 ⇒ NULL。
const NativeStructSpec* native_find_module_struct_spec(const char* module_name, const char* type_name);

// 列出某模块导出的 native 类型名（写进 out_names，最多 max 个），返回写入个数。
//   用途：`use dirs.写错的名字` 的"可用类型"诊断（照 native_builtin_module_hint 的风格）。
int native_list_module_struct_specs(const char* module_name, const char** out_names, int max);

// 取模块方法的返回类型规格（编译期；未声明规格 ⇒ NULL）
const NativeTypeSpec* native_get_module_method_return_spec(const char* module_name, const char* method_name);

// 规格 → TypeInfo（编译期）。**返回新分配、调用方负责 type_free**（struct 名会带进
// TypeInfo->struct_name ⇒ 字段访问能顺着 infer_field_type 的 native 分支解析）。
TypeInfo* native_type_spec_to_info(const NativeTypeSpec* spec);

// 规格 → TypeInfo，并**用实参类型解析关系型标签**（NTYPE_ARG0_*，2026-09-27）：
//   arg0_type = 第 0 个实参的类型（模块形式 = 首个实参；实例形式 = **接收者**）。
//   ⚠ 解析不出时（实参缺失 / 实参是"元素未指定"的裸 Array / 裸 Dict）⇒ 该处返回 **NULL**
//     （"未指定"），**不是** `any` —— 两者兼容性不同（"未指定"能匹配 `Array[int]`，`any` 不能）。
//     整个规格都解析不出 ⇒ 本函数返回 NULL，调用方回落到原来的 Kind 路径（旧行为逐字不变）。
//   native_type_spec_to_info() 等价于本函数传 NULL。
TypeInfo* native_type_spec_to_info_with_args(const NativeTypeSpec* spec, TypeInfo* arg0_type);

// 规格里是否含 NTYPE_ARG0_*（递归）。调用方据此决定"要不要去推实参类型"——
//   本仓有 230+ 个 native 方法，绝大多数规格与实参无关，不该为它们白推一遍实参。
int native_type_spec_has_arg_ref(const NativeTypeSpec* spec);

// 规格 → 可读类型串（LSP / 诊断）。总是以 '\0' 收尾。
// 风格与 type.c 的 type_to_string **一致**：struct 写成 `struct Name` ⇒ 同一个类型在报错
// 信息与 LSP hover 里是同一种写法（如 `Array[struct DirEntry]`、`Dict[string, string]`）。
void native_type_spec_to_string(const NativeTypeSpec* spec, char* out, int out_size);

// ========== native 结构体的运行期支持 ==========
// 按名取（必要时**创建并注册**）ObjStructDef；无同名规格 ⇒ NULL。
// 字段顺序取自注册的 spec ⇒ 与编译期的字段索引同源。
ObjStructDef* native_struct_def_for(const char* name);

// 按 native struct 名创建实例（字段先置 null，随后用 native_struct_set 填）；失败 ⇒ NULL
ObjStruct* native_struct_new(const char* name);

// 按**字段名**写值（内部查 spec/def 的字段顺序）；返回 1 = 成功，0 = 名字不存在
int native_struct_set(ObjStruct* obj, const char* field_name, Value value);

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

// 注册模块常量（原生模块可导出 int 常量）
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
// 返回 0 = 成功；-1 = 未知模块名。不再静默 no-op —— 字节码里带的模块名与运行时注册表
// 不一致时（例如 leno_vm.exe 版本不同）必须显式失败，否则「模块没被初始化」会一直潜伏到
// 调用其方法时才暴露，且报错位置与真实原因不符。
int native_init_module(const char* module_name);

// 该模块名是否是已注册的原生模块（编译期校验用；权威清单 = native.c 的 module_init_table）
int native_module_is_registered(const char* module_name);

// 已注册原生模块名的逗号分隔列表（编译期报错提示用；返回静态缓冲区，勿释放）
const char* native_module_names_csv(void);

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
    // 返回类型的**完整规格**（可空）。非空时优先于上面的两个 Kind，并支持 `NTYPE_ARG0_*`
    //   这类关系型标签（实例形式下"第 0 个实参"= **接收者**）。
    //   用 native_register_instance_method_return_spec() 在同名注册**之后**补上。
    const NativeTypeSpec* return_spec;
} InstanceMethodMeta;

// 注册实例方法元信息（编译时调用）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type);

// 注册实例方法元信息（带参数类型）
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta_with_params(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 给**已注册**的实例方法补一条"返回类型规格"（2026-09-27）。
//   ⚠ 必须在同名的 native_register_instance_method_meta_with_params /
//     <type>_register_method_with_params **之后**调用（它只改已存在的条目，不创建新条目）。
//   规格里可用 NTYPE_ARG0_*：实例形式下"第 0 个实参"就是**接收者**。
void native_register_instance_method_return_spec(const char* type_name, const char* method_name,
                                                 const NativeTypeSpec* spec);

// 取实例方法的返回类型规格（未声明 ⇒ NULL）
const NativeTypeSpec* native_get_instance_method_return_spec(const char* type_name, const char* method_name);

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
