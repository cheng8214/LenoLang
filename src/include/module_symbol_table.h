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
    // 泛型类型形参**名**（如 ["K","V"]，长度 = type_param_count；v31 补）
    //   为什么需要：只记数量时，调用点无法把返回类型 `Pair[K, V]` 里的**占位符按名**换成
    //   调用点的类型实参（`gm.makePair[string, int]` 的静态类型会停在 `Pair[K, V]` ⇒
    //   `string k = p.getKey()` 编不过，实际类型 `struct K`）。
    //   struct **方法**侧早有这套（`ModuleStructMethod::return_generic_param_names`），
    //   模块**函数**侧一直缺 —— 这里是把它补齐。
    char** type_param_names;
    char* param_text;           // 原始参数文本（如 "Dict opts" 或 "string name, int age"）
    int param_count;            // 参数总数量
    int default_count;          // 有默认值的参数数量
    char** param_default_texts; // 每个参数的默认值文本（NULL 表示该参数无默认值）
    // ---- ⑱ 转正（2026-09-18 补）：参数类型 ----
    // 此前只有 `param_text` ⇒ 跨模块调用的实参**类型**判不了，语义侧只能就地解析文本
    // （那是"参数表 → 类型"的第三份实现）。现在扫描器用唯一实现 `mod_scan_params`
    // （`inc/sym_table_params.inc`）填这两个字段，随 `.lenosymc` v29 往返。
    TypeKind* param_types;      // 长度 = param_count（NULL = 未记录，消费方跳过）
    char** param_struct_names;  // 聚合类型参数（struct/face/cstruct/clib）的名字，按参数下标对齐
                                 // （无名的槽位为 NULL）

    int line;                   // 函数定义所在行号（1-based，0 表示未知）
    int is_async;               // 是否 `async func`（C2：扫描器要认 async，消费者侧据它
                                // 判"调用返回 Future"/发 OP_ASYNC_CALL；随 .lenosymc 往返）
} ModuleFuncSymbol;

// 模块 struct 字段
typedef struct {
    char* name;                 // 字段名
    TypeKind type;              // 字段类型
    TypeKind element_type;      // Array[T]/Dict[K,V] 中 T/V 的类型（当 type 为 TYPE_ARRAY/TYPE_DICT/TYPE_PTR_GENERIC 时有效）
    char* struct_name;          // 类型名（当 type 为 TYPE_STRUCT/TYPE_CLIB/TYPE_CSTRUCT/TYPE_FACE 时）
    char* element_struct_name;  // Array[T]/Dict[K,V] 中 T/V 的类型名（当 element_type 为 TYPE_STRUCT/TYPE_FACE/TYPE_CSTRUCT/TYPE_CLIB 时）
    TypeInfo* type_info;        // 完整类型信息（支持 Array[Array[int]]/Array[Ptr[u8]] 等嵌套泛型类型）
                                // 非 NULL 时优先使用，element_type/element_struct_name 为向后兼容的扁平降级
    int nullable;               // 可空字段标记：1=Type?，0=Type
    int line;                   // 字段定义所在行号（1-based，0 表示未知）
    // `pri` 成员私有（默认全公有 ⇒ 只有显式写 pri 的才是 1）
    //   为什么必须放进模块符号表（只留在 AST 不够）：**跨模块**访问时当前编译单元拿不到
    //   被导入 struct 的定义 AST ⇒ 只有符号表这条路能判私有 ✓
    int is_private;
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
    char** param_struct_names;  // 参数 struct/face/cstruct/clib 名数组（该参数是聚合类型时；否则 NULL）
                                // —— 此前**没有**这个字段（clib 的 ModuleClibFuncSymbol 有）⇒
                                //    扫描器虽然算出了名字（scan_struct.inc 的 param_struct_name[64]）
                                //    却没处可存 ⇒ 跨模块方法的聚合类型参数不可判（只能跳过）。
                                //    补上它，跨模块方法实参检查才能覆盖聚合参数（⑯）。
    char** param_generic_names; // 参数泛型类型参数名（如 "T", "K"），用于泛型方法参数类型检查
    int line;                   // 方法定义所在行号（1-based，0 表示未知）
    int is_async;               // 是否 `async func` 方法（C2，同 ModuleFuncSymbol.is_async）
    // `pri` 成员私有（同 ModuleStructField.is_private 的理由：跨模块只能靠符号表判 ✓）
    int is_private;
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
    // 关联常量
    char** const_names;         // 关联常量名数组
    char** const_value_strs;    // 关联常量值字符串（原始文本，导入时解析）
    int const_count;            // 关联常量数量
    int def_line;               // struct 定义所在行号（1-based，0 表示未知）
} ModuleStructSymbol;

// 模块 enum 符号
typedef struct {
    char* name;                 // enum 名称
    int member_count;           // 成员数量
    char** member_names;        // 成员名称数组
    int64_t* member_values;     // 成员值数组（int64_t，支持 >= 2^31 的位标志值）
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

// 模块 cfunc 符号（C 回调函数签名）
typedef struct {
    char* name;                 // cfunc 名称
    int param_count;            // 参数数量
    TypeKind* param_types;      // 参数类型数组（TypeKind，与 clib 一致，支持 i32/f64/str8/Ptr 等）
    TypeKind* param_element_types; // 参数 Ptr[T] 中的 T，TYPE_PTR 表示无（与 clib 一致）
    char** param_struct_names;  // 参数 struct 名数组（可为 NULL，与 clib 一致）
    char** param_names;         // 参数名数组（可为 NULL）
    TypeKind return_type;       // 返回类型
    TypeKind return_element_type; // 返回类型 Ptr[T] 中的 T，TYPE_PTR 表示无
    char* return_struct_name;   // 返回 struct 名（可为 NULL）
} ModuleCfuncSymbol;

// 模块 face 方法符号
typedef struct {
    char* name;                 // 方法名
    TypeKind return_type;       // 返回类型
    char* return_struct_name;   // 如果返回类型是 struct，存储 struct 名称
    int param_count;            // 参数数量
    // ---- ⑰-2（2026-09-18 补）：参数类型与聚合类型参数名 ----
    // 此前只有 param_count ⇒ 跨模块的 face 方法实参**只判得了个数**、判不了类型
    // （同文件能判：解析器的 face AST 有 method_param_types）。补上这两个字段，
    // 跨模块也就能判类型了。与 ModuleStructMethod 的同名字段同义。
    TypeKind* param_types;      // 参数类型数组（长度 = param_count；NULL = 未记录）
    char** param_struct_names;  // 聚合类型参数（struct/face/cstruct/clib）的名字数组，按参数下标对齐
                                 // （无名的槽位为 NULL）
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
    TypeInfo* type_info;        // 完整类型信息（支持 Array[int]/Dict[K,V] 等泛型类型）
                                // 非 NULL 时优先使用，type/struct_name 为向后兼容的扁平降级
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
    ModuleCfuncSymbol* cfuncs;  // cfunc 符号数组
    int cfunc_count;             // cfunc 数量
    int cfunc_capacity;          // cfunc 数组容量
    // ---- v33：导出名清单（收敛 S10 —— 见 docs/待办_单一事实来源与重复实现收敛.md 第二节）----
    // 背景：`src/module_loader.c` 原先自带一个**独立的文本扫描器** `extract_exports()`
    // （`strncmp(p,"export",6)` + 自己跳注释/字符串/反引号 + 循环跳 `const` 后的类型关键字 +
    // "类型在前"兜底分支），与 `module_symbol_table` 的扫描链**各判一遍"什么算 export"**。
    // 那份复刻已删除 ⇒ 导出名只由扫描链这一处判定（唯一实现者，其余都是"读"它）。
    // 依赖模块路径（用于 .lenosymc 缓存失效判定）
    char** dep_paths;           // 依赖模块的绝对路径数组
    int dep_count;              // 依赖模块数量
    int dep_capacity;           // 依赖数组容量
    // 扫描/所有权状态（详见 module_symbol_table_get_shared 的说明）
    int scanned;                // 1 = 已扫描完成：module_symbol_table_scan 幂等，不再重复填充
    int shared;                 // 1 = 由进程内记忆化持有，module_symbol_table_destroy 不得释放
} ModuleSymbolTable;

// 创建模块符号表
ModuleSymbolTable* module_symbol_table_create(const char* module_path);

// 销毁模块符号表
void module_symbol_table_destroy(ModuleSymbolTable* table);

// 扫描模块文件并填充符号表
// current_file: 当前文件路径（用于解析相对路径）
// 返回: 0 成功，-1 失败
// 幂等：table->scanned 已为 1 时直接返回 0，不重复填充
int module_symbol_table_scan(ModuleSymbolTable* table, const char* current_file);

// 取（必要时扫描）某模块的符号表：按解析后的绝对路径做**进程内记忆化**，
// 同一路径只扫描一次，与缓存开关无关。
// 返回的表由内部缓存持有：调用方只读，且不要 destroy（destroy 对它已是 no-op）。
// 扫描失败返回 NULL。
ModuleSymbolTable* module_symbol_table_get_shared(const char* module_path, const char* current_file);

// 查找函数符号
ModuleFuncSymbol* module_symbol_table_find_func(ModuleSymbolTable* table, const char* func_name);

// 查找 struct 符号
ModuleStructSymbol* module_symbol_table_find_struct(ModuleSymbolTable* table, const char* struct_name);

// 添加函数符号
void module_symbol_table_add_func(ModuleSymbolTable* table, const char* name, TypeKind return_type, const char* return_struct_name, int type_param_count, TypeInfo* return_type_info, const char* param_text, int param_count, int default_count, char** param_default_texts, int is_async);

// 添加 struct 符号
void module_symbol_table_add_struct(ModuleSymbolTable* table, const char* name, int field_count, ModuleStructField* fields, int method_count, ModuleStructMethod* methods, int is_cstruct, int type_param_count, char** type_param_names);

// 设置 struct 的 impl 列表（S10：AST 填充器的**建表路径**需要它）
//   为什么单独一个接口：add_struct 的形参里**没有 impl** ⇒ 由 AST 建表时永远填不出
//   "谁实现了哪个 face"（impl_count 恒 0）⇒ 语义无法经 face 解析方法（实测症状：
//   跨模块 `u.fmt()` 报「类型 'struct User' 没有方法 'fmt'」且「struct 'User' 未实现 face 'Printable'」✗）
// 名字内部复制。旧数组不释放（符号表进程内长存活、每模块一份，量小；与其它覆盖路径同一约定 ✓）
void module_symbol_table_set_struct_impls(ModuleSymbolTable* table, const char* struct_name,
                                          int impl_count, char** impl_names);

// 设置 struct 的**关联常量**（S10：AST 填充器的建表路径需要它）
//   同 impl：add_struct 的形参里没有 const ⇒ 由 AST 建表时 const_count 恒 0
//   ⇒ 跨模块引用 `spl.Splitter.HORIZONTAL` 报「struct 'Splitter' 没有字段 'HORIZONTAL'」✗
//   value_strs 是**原始文本**（消费者 visit_module.inc 按文本解析：true/false/null/引号串/数值）
//   ⇒ 调用方负责把 AST 表达式还原成文本。名字与文本都内部复制 ✓
void module_symbol_table_set_struct_consts(ModuleSymbolTable* table, const char* struct_name,
                                           int const_count, char** const_names, char** const_value_strs);

// 查找 struct 方法
ModuleStructMethod* module_symbol_table_find_struct_method(ModuleSymbolTable* table, const char* struct_name, const char* method_name);

// 查找 enum 符号
ModuleEnumSymbol* module_symbol_table_find_enum(ModuleSymbolTable* table, const char* enum_name);

// 添加 enum 符号（member_values 一律原样接收：自动递增已在扫描阶段
// scan_enum.inc 解析完毕。-1 是合法成员值，不表示"无显式值"）
void module_symbol_table_add_enum(ModuleSymbolTable* table, const char* name, int member_count, char** member_names, int64_t* member_values);

// 查找 face 符号
ModuleFaceSymbol* module_symbol_table_find_face(ModuleSymbolTable* table, const char* face_name);

// 添加 face 符号
void module_symbol_table_add_face(ModuleSymbolTable* table, const char* name, int method_count, ModuleFaceMethodSymbol* methods, int type_param_count);

// 查找变量符号
ModuleVarSymbol* module_symbol_table_find_var(ModuleSymbolTable* table, const char* var_name);

// 添加变量符号
void module_symbol_table_add_var(ModuleSymbolTable* table, const char* name, TypeKind type, const char* struct_name, int is_const, TypeInfo* type_info);

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

// 查找 cfunc 符号
ModuleCfuncSymbol* module_symbol_table_find_cfunc(ModuleSymbolTable* table, const char* cfunc_name);

// 添加 cfunc 符号
void module_symbol_table_add_cfunc(ModuleSymbolTable* table, const char* name,
    int param_count, TypeKind* param_types, TypeKind* param_element_types,
    char** param_struct_names, char** param_names,
    TypeKind return_type, TypeKind return_element_type, const char* return_struct_name);

// cfunc 符号数量
int module_symbol_table_cfunc_count(ModuleSymbolTable* table);

// E1：use 符号名拼错时的相似名提示——遍历所有符号名字桶找编辑距离最小的候选。
// 返回"提示串"（\n  提示: 是否想用 'xxx'？），找不到返回空串（非 NULL）。
const char* module_symbol_table_hint(ModuleSymbolTable* table, const char* name);

// 从字符串解析完整类型（支持 int/float/string/bool 及 Array[T]/Dict[K,V]）
TypeInfo* parse_type_from_string(const char* type_str);

// 重置模块扫描栈（用于循环依赖检测，每次编译前调用）
void module_symbol_table_reset_scan_stack(void);

// 清空进程内符号表记忆化（module_symbol_table_get_shared 的进程级缓存，含已失效换下的旧表）。
// 每次编译前调用；LSP 这类常驻宿主应在每次请求前调用 —— 否则可能复用旧符号表。
// 注：记忆化命中前会比对源文件 mtime/size 自动失效，此接口用于主动清空与回收内存。
void module_symbol_table_reset_memo(void);

// 添加依赖模块路径（用于 .lenosymc 缓存失效判定）
void module_symbol_table_add_dep(ModuleSymbolTable* table, const char* path);


// ============================================================================
// S10 迁移：AST 符号填充器（编译期注册；VM-only 不注册 ⇒ 行为与现状一致）
// ============================================================================
// 目的：让"符号信息"逐类从文本扫描链迁到 parser AST，最终删掉那套老解析。
// 机制：扫描链跑完后（`get_shared` 是唯一收口点）调用本填充器，由它**覆盖**
//   自己已接管的那些声明类；未接管的类仍由扫描链产出 ⇒ 可**逐类**替换、
//   每类都能用 `--export-diff` + 断言验收，最后逐段删掉扫描链对应分支 ✓
// 为什么用函数指针：本模块在 `sources_core.txt`（VM-only 也编），不能依赖 parser ✓
// src 由 core 侧读好传入（避免同一份源码读两遍）✓
typedef void (*ModuleSymbolFillProvider)(ModuleSymbolTable* table, const char* src);

void module_symbol_table_set_ast_fill_provider(ModuleSymbolFillProvider provider);

// 扫描链完成后调用填充器（未注册则空操作）；source 为 NULL 时内部自行读盘 ✓
void module_symbol_table_apply_ast_fill(ModuleSymbolTable* table, const char* source,
                                        const char* current_file);

#endif // MODULE_SYMBOL_TABLE_H
