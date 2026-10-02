#ifndef NATIVE_H
#define NATIVE_H

#include "lenolang.h"

// Native 函数元信息（编译时和运行时都可用）
typedef struct {
    const char* name;
    int arity;              // 参数个数；可变参数写 NATIVE_ARITY_VARARG（-200，**不是 -1**）
    int min_arity;          // 最小实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    int max_arity;          // 最大实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    TypeKind return_type;
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS];  // 参数类型数组
    // 上面 param_types 里**有效**的条目数（v3.2.8）：定长函数 = arity；可变参数函数 = 0
    //   （老行为：整份被忽略 —— getter 的判据是 `param_index < arity`，对 `arity == -1` 恒假），
    //   除非随注册声明过（NativeParamSpec 的 declared_count > 0 ⇒ 有效长度 = MAX_METHOD_PARAMS）。
    //   ⚠ 与模块方法通道的 `ModuleMethodMeta::param_type_count` 同一口径 —— 模块那边在 v3.2.7
    //     就补上了（实例二十三），内置这边一直漏着（`input("提示")` 的提示文案因而是 any）。
    int param_type_count;
    // 完整返回类型规格（v3.2.8，可空）：非 NULL 时**优先于**上面的 Kind 槽 —— 内置函数通道此前
    //   只有 Kind，表达不了 `ExecResult{...}` 这类带名字/带字段的返回类型（`_exec` 的 `[output, code]`
    //   就只能是 `Array[any]`）。语义与 `native_register_module_method` 的 return_spec 对齐。
    const NativeTypeSpec* return_spec;
} NativeFunctionMeta;

// 模块方法元信息
#ifndef MAX_METHOD_PARAMS
#define MAX_METHOD_PARAMS 8  // 最大参数数量
#endif
typedef struct {
    char module_name[32];
    char method_name[32];
    int arity;              // 参数个数；可变参数写 NATIVE_ARITY_VARARG（-200，**不是 -1**）
    int min_arity;          // 最小实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    int max_arity;          // 最大实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    TypeKind return_type;
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS];  // 参数类型数组
    // 上面 param_types 里**有效**的条目数（v3.2.7）：定长方法 = arity；可变参数方法 = 0
    // （老行为：整份被忽略），除非用 `native_set_method_vararg_params()` 显式声明过（= MAX_METHOD_PARAMS）。
    // ⚠ 为什么需要它：老的参数类型查询判据是 `param_index < arity`，对 `arity == -1` 恒假
    //   ⇒ 可变参数方法的 param_types 永远读不到（`dirs.join(1, 2)` 也编译得过去 ✗）。
    int param_type_count;
    // 完整返回类型规格（可空）：非 NULL 时**优先于** return_type/return_element_type ——
    // 它才能表达 `Array[DirEntry]` / `Dict[string,string]` 这类参数化、带名字的类型
    // （见 leno_types.h 的 NativeTypeSpec 说明）。为 NULL ⇒ 退回上面两个 Kind 的老路径。
    const NativeTypeSpec* return_spec;
    NativeFn function;
} ModuleMethodMeta;

// 编译时注册 native 函数元信息（供模块使用）
// min_arity/max_arity: 当 arity == NATIVE_ARITY_VARARG（可变参数）时，指定最小/最大允许实参个数
//   （NATIVE_ARITY_ANY = 不限）；定长注册忽略这两个字段（用 NATIVE_FIXED* 写即可）
// ⚠ 判据请写哨兵本身或 `< 0`，**不要**写 `== -1` —— 可变参数是 -200，`== -1` 会恒假（2026-10-02 修过这个坑）
// return_element_type: 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
void native_register_meta(const char* name, int arity, int min_arity, int max_arity,
                          TypeKind return_type, TypeKind return_element_type,
                          int declared_count, const TypeKind* declared, TypeKind tail_type);

// 获取所有注册的 native 函数元信息（编译时使用）
const NativeFunctionMeta* native_get_all_functions(int* count);

// 给**全局内置函数**声明完整返回类型规格（v3.2.8）：见 NativeFunctionMeta::return_spec 的说明。
// ⚠ 必须在 `vm_register_native()` 之后调用（那是"查找并更新"；条目不存在时会以最小信息新建）。
void native_register_meta_spec(const char* name, const NativeTypeSpec* return_spec);

// 取内置函数的返回规格（没有则 NULL）
const NativeTypeSpec* native_get_return_spec(const char* name);

// 给**全局内置函数**声明可变参数的参数类型（v3.2.8）：语义与模块方法的
//   `native_set_method_vararg_params()` 逐字一致（先整份填 tail_type，再盖上前 prefix_count 个）。
//   ⚠ 必须在 `vm_register_native()` **之后**调用（同名条目不存在时静默忽略 —— 编译期常量，不该失败）。
//   为什么需要它：`vm_register_native` 的 param_types 实参对 `arity == -1`（如 `input`）是**整份忽略**的。
// (2026-10-02) 此处原有 `native_set_builtin_vararg_params()` —— 全局通道上的"第二步"，
//   已随统一入口（vm_register_native 收 NativeParamSpec）删除 ✓

// 根据函数名获取返回类型
TypeKind native_get_return_type(const char* name);

// 获取全局函数的返回数组元素类型（编译时调用）
TypeKind native_get_return_element_type(const char* name);

// 获取全局函数的参数类型
TypeKind native_get_global_function_param_type(const char* name, int param_index);

// 运行时注册 native 函数
// min_arity/max_arity: 当 arity == NATIVE_ARITY_VARARG（可变参数）时，指定最小/最大允许实参个数
//   （NATIVE_ARITY_ANY = 不限）；定长注册忽略这两个字段（用 NATIVE_FIXED* 写即可）
// ⚠ 判据请写哨兵本身或 `< 0`，**不要**写 `== -1` —— 可变参数是 -200，`== -1` 会恒假（2026-10-02 修过这个坑）
// 注册全局内置函数（**唯一入口**）：写法与模块/实例族一致 —— 规格用 NATIVE_FIXED /
//   NATIVE_FIXED_NONE / NATIVE_VARARG（见 leno_types.h 的 NativeParamSpec）✓
void vm_register_native(const char* name, NativeFn function, TypeKind return_type,
                        TypeKind return_element_type, NativeParamSpec params);

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

// ========== 参数个数标记（**不要再写裸 -1**）==========
//   为什么要有命名常量：`-1` 过去同时承担两种完全不同的含义 ——
//     · arity 位：**这个方法可变参数**（老写法 `arity == -1`）
//     · min/max 位：**个数不限**
//   一个数字两种意思，读代码只能靠上下文猜 ⇒ 分家：
#define NATIVE_ARITY_VARARG (-200)  // arity 位：可变参数（个数范围看 min_arity..max_arity）
#define NATIVE_ARITY_ANY    (-1)    // min/max 位：不限（只有 max_arity 用得到）

// ========== 参数规格 ==========
// `NativeParamSpec` 与 `NATIVE_FIXED` / `NATIVE_FIXED_NONE` / `NATIVE_VARARG` 三个构造宏
//   定义在 **leno_types.h**（底层头，`leno_value.h` / `method_table.h` 也要用）✓
//   为什么用结构体规格：拆成位置参数时，定长注册必须在调用点写三个"其实恒等"的数字
//   （arity 与 declared_count 必然相同、min/max 对定长毫无意义）⇒ 224 处写法五花八门、
//   且**写错一个就静默漏检查**（实测正是如此）✗ ⇒ 改成"按形状写宏"，全仓库写法一致 ✓

// 注册模块方法（**唯一入口**）。取代了原先两个入口（`..._spec` 定长 / `..._vararg` 可变）
//   以及更早的"注册 + native_set_method_vararg_params 事后补声明"两步式 ——
//   两步式的致命处是**忘了第二步就静默失去类型检查**（实测 25 处里 11 处忘了）✗
// 例：`native_register_module_method("maths", "sqrt", math_sqrt, &NATIVE_T_FLOAT,
//        NATIVE_FIXED(sqrt_params))`                                    ⇒ 定长、全按表查；
//     `native_register_module_method("dirs", "cwd", native_dirs_cwd, &NATIVE_T_STRING,
//        NATIVE_FIXED_NONE(0))`                                         ⇒ 定长、零个实参；
//     `native_register_module_method("strings", "to_hex", str_to_hex, &NATIVE_T_STRING,
//        NATIVE_VARARG(1, 2, 2, tohex_params, TYPE_ANY))`               ⇒ 个数 1..2、前 2 个查表。
void native_register_module_method(const char* module_name, const char* method_name,
                                   NativeFn function, const NativeTypeSpec* return_spec,
                                   NativeParamSpec params);

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
// ---- 回调返回类型族（⓪ 族声明化，2026-09-28）----
extern const NativeTypeSpec NATIVE_T_ARG_CB_RET0;    // 第 0 实参是回调（threads.start ⇒ Thread[T]）
extern const NativeTypeSpec NATIVE_T_ARG_CB_RET1;    // 第 1 实参是回调（实例 map：接收者=0）
extern const NativeTypeSpec NATIVE_T_ARR_CB_RET1;    // Array[第 1 实参回调的返回]（map）
extern const NativeTypeSpec NATIVE_T_ARG_REDUCE12;   // 回调=1、累加器=2（reduce ⇒ T）
extern const NativeTypeSpec NATIVE_T_THREAD_CB_RET0; // Thread[第 0 实参回调的返回]（threads.start）
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
// 带预计算回调返回类型的版本（⓪ 族声明化）：语义侧先推好 T（NTYPE_ARG_CB_RET 节点）再代入；
//   cb_ret = NULL ⇒ CB_RET 节点给 NULL（顶层回落 Kind 槽，宁漏勿误报）
TypeInfo* native_type_spec_to_info_with_cb(const NativeTypeSpec* spec, TypeInfo* arg0_type,
                                           TypeInfo* cb_ret);

// 带**调用上下文**的规格定型（2026-09-28）：NTYPE_BY_ARITY 节点按实参个数选形态
//   （双形态内置如 `_env`：读/写返回类型不同）。纯"规格 → 规格"替换 —— BY_ARITY ⇒
//   选中的子规格（argc < arg_index 取 sub，否则 sub2），其余原样返回。
//   语义侧先调本函数定型，再走 native_type_spec_to_info* 正常解析。
const NativeTypeSpec* native_type_spec_resolve_for_call(const NativeTypeSpec* spec, int argc);

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
    int arity;              // 参数个数（不包括 receiver）；可变参数写 NATIVE_ARITY_VARARG（-200，**不是 -1**）
    int min_arity;          // 最小实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    int max_arity;          // 最大实参个数（仅可变参数 arity == NATIVE_ARITY_VARARG 时有效；NATIVE_ARITY_ANY = 不限）
    TypeKind return_type;   // 返回类型
    TypeKind return_element_type; // 返回数组时的元素类型（TYPE_UNKNOWN 表示未指定）
    TypeKind param_types[MAX_METHOD_PARAMS]; // 参数类型数组
    // **有效参数类型个数**（2026-10-02 补；与模块式的 ModuleMethodMeta 同字段同语义）。
    //   为什么要这个字段：查表原先用 `param_index < meta->arity` 当闸门，而**可变参数**方法
    //   （arity == -1）下这句恒假 ⇒ 那些方法无论怎么声明参数类型都会退回 TYPE_ANY（类型检查形同虚设 ✗）。
    //   模块式 v3.2.7 已由该字段 + native_set_method_vararg_params() 修掉；实例式这次补齐 ✓
    //   取值：定长注册 = arity（封顶 MAX_METHOD_PARAMS）；其余 = 0（可变参数未声明前不检查）。
    int param_type_count;
    // 返回类型的**完整规格**（可空）。非空时优先于上面的两个 Kind，并支持 `NTYPE_ARG0_*`
    //   这类关系型标签（实例形式下"第 0 个实参"= **接收者**）。
    //   用 native_register_instance_method_return_spec() 在同名注册**之后**补上。
    const NativeTypeSpec* return_spec;
} InstanceMethodMeta;

// 注册实例方法元信息（编译时调用）
// min_arity/max_arity: 当 arity == NATIVE_ARITY_VARARG（可变参数）时，指定最小/最大允许实参个数
//   （NATIVE_ARITY_ANY = 不限）；定长注册忽略这两个字段（用 NATIVE_FIXED* 写即可）
// ⚠ 判据请写哨兵本身或 `< 0`，**不要**写 `== -1` —— 可变参数是 -200，`== -1` 会恒假（2026-10-02 修过这个坑）
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type);

// 注册实例方法元信息（带参数类型）
// min_arity/max_arity: 当 arity == NATIVE_ARITY_VARARG（可变参数）时，指定最小/最大允许实参个数
//   （NATIVE_ARITY_ANY = 不限）；定长注册忽略这两个字段（用 NATIVE_FIXED* 写即可）
// ⚠ 判据请写哨兵本身或 `< 0`，**不要**写 `== -1` —— 可变参数是 -200，`== -1` 会恒假（2026-10-02 修过这个坑）
// return_element_type: 返回数组时的元素类型，非数组返回类型时传 TYPE_UNKNOWN
void native_register_instance_method_meta_with_params(const char* type_name, const char* method_name, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// ⚠ **内部接口**（2026-10-02 起）：只应由 `method_table_register_method()` 调用一次，
//   把类型规格写进编译期元信息表；**不要在注册调用点自己调**（那正是被删掉的"两步式"✗）。
//   语义：前 prefix_count 个实参按 prefix[i] 检查，其余按 tail_type（TYPE_ANY = 不检查）；
//   只改已存在的条目，不创建新条目。
void native_set_instance_method_vararg_params(const char* type_name, const char* method_name,
                                              int prefix_count, const TypeKind* prefix, TypeKind tail_type);

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
//
// ⚠ 本函数**不跳转**：它只把异常登记进当前 VM（`exception` / `has_exception`，见 native.c），
//   真正的抛出发生在 native 调用**返回之后**（VM 检查 `has_exception`）⇒ 调用点永远拿不到
//   紧随其后的返回值。所以 `native_throw_error(...); return val_null();` 里的 `return` 只是
//   C 语法的兜底，**不构成"失败时返回 null"的契约**——曾据此误判 `files.read` 的失败语义
//   （2026-09-30 实测：无 `try` 时程序直接终止，有 `try` 时由 `catch` 接走，消息见 `e.msg`）。
void native_throw_error(const char* msg);

Value ffi_reload_library(const char* path);

#endif
