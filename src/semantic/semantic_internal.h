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

// ★ pri：私有成员的字段访问检查（实现见 semantic_visit_ast.c ✓）
//   非 static：semantic_type.c 的 infer_expr_type 里也要调它 ——
//   因为只有那里跑得够晚，导入模块的符号表才**已经懒加载** ✓（实测早查会拿到 NULL ✗）
void pri_check_field_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* field_name);

// ★ pri：私有**方法**的调用检查（实现见 semantic_visit_ast.c ✓）
//   非 static：semantic_type.c 处理 `obj.method()` 时也要调它 ✓
void pri_check_method_access(Semantic* s, Ast* ast, TypeInfo* obj_type, const char* method_name);

// 递归把类型树里的"名字其实是 face/enum 的 TYPE_STRUCT"纠正过来（含 Array/Dict/Ptr 的实参）。
// **唯一实现**：`is`/`as`、守卫类型、switch case 的匹配类型都调它 —— 因为 v3.2.2 起
// 嵌套实参的名字会被带进运行期做名字校验，漏修就是"拿 struct 名字比 face 实例"⇒ 误判。
void resolve_type_names(Semantic* s, TypeInfo** type_ptr);

// native struct 兜底判据（v3.2.5）：`DirEntry` / `DirInfo` 这类名字由 native 模块用
//   `native_register_struct_spec()` 声明（见 leno_types.h），**不在符号表里**（native 模块
//   没有 sym_table）⇒ 类型名校验会判成"未定义的类型"，于是只能 `var d = dirs.stat(p)`，
//   不能写 `DirInfo d = dirs.stat(p)` / `Array[DirEntry] w = dirs.walk(p)`。
// 本函数回答"这个名字是不是已注册的 native struct"（**唯一来源**：`native_find_struct_spec`，
//   与字段解析、运行期 ObjStructDef 同源）⇒ 是则视作合法 struct 类型（TYPE_STRUCT + 名字）。
// 调用点（四处"查不到就报未定义"的校验）：变量声明（有/无初始化各一处）、函数参数与返回
//   （check_undefined_type，泛型实参也走它）、脚本 struct 的字段类型。
int semantic_native_struct_known(const char* name);

// 把模块符号表里的一条 struct/cstruct 符号，按完整精度搬进当前作用域的符号
// （字段类型 + 泛型参数）。这是"怎么把模块里的 struct 字段搬进当前作用域"的**唯一实现**：
// 此前 AST_USE 与 import_type_deps 各写一遍，后者丢嵌套泛型（详见 semantic_type_utils.c）。
void semantic_attach_struct_fields(Symbol* sym, const ModuleStructSymbol* ssym);

// 把一条 **native 类型规格**导入当前作用域 —— 语义上的 `use <module>.<Type>`（2026-09-27）。
// native 模块没有 sym_table（字段表在 native 类型规格注册表里）⇒ 与 semantic_attach_struct_fields
// 是**两条通道**、但目标一致：当前作用域里出现一个可用的 struct 符号。
// 返回 1 = 已导入（含"同名同类型 ⇒ 静默跳过"）；0 = 同名冲突（文案由调用方决定）。
int semantic_import_native_type(Semantic* s, const NativeStructSpec* spec);

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

// 「未定义的类型」提示（**唯一实现**，2026-10-10 加 / A 组第 4 条）：去**已导入模块**的
//   符号表里按名字找（struct/face/enum/别名/clib ✓）⇒ 命中就写清「它其实是模块 'X' 导出的
//   某类 ⇒ 请补 `use X.名字`」✓；找不到才退回泛泛提示 ✓。
//   为什么要共享：这句话在本仓有**七处拷贝** ✗（实测：visit_var.inc 四处 / visit_type_def.inc 一处 /
//   semantic_visit_func.c 一处 / visit_module.inc 一处）⇒ 各写各的必然改漏 ✓。buf 写成"（…）"（含括号 ✓）
//   ⇒ 调用方直接 `snprintf(msg, …, "未定义的类型: %s%s", type_name, hint)` 即可 ✓。
void semantic_undefined_type_hint(Semantic* s, const char* type_name, char* buf, size_t size);

// 跨线程传值：**编译期拦截"确定是 struct"的实参**（2026-10-10 加 / A 组第 3 条后半）。
//   `threads.start(f, a1, …)` 的跨线程实参只支持 标量 / string / Array / Dict（深克隆 ✓）
//   与 Channel（按引用 ✓）；struct / cstruct / face 会被运行期拦下（跨堆引用会堆损坏 ✓）。
//   本函数把它**提前到编译期** ✓：只拦"静态类型确定为上述三种"的实参 ✓（`any` 放行 ✓）。
//   返回：报了几处。
int semantic_check_cross_thread_args(Semantic* s, AstList* args, int line, int column);

// 方法不存在时的**分级提示**（**唯一实现**，2026-10-10 加 / A 组第 7 条）：
//   ① `slice*` ⇒ 切片提示（`arr[start:end]` ✓ **有意优先** ✗ 见实现的说明 ✓）；
//   ② 否则给相似名（`startsWith` ⇒ `starts_with` ✓）；③ 都没有 ⇒ 空串。
const char* semantic_no_such_method_hint(TypeInfo* type, const char* method_name);

// `not` 的优先级陷阱（WARN_NOT_PRECEDENCE，2026-10-10 加）：表达式里写 `not x is T` ⇒
//   实际是 `(not x) is T` ✗（结果常相反 ✓ 实测 x = 42：false ✗ vs `not (x is T)` = true ✓）。
//   kind = 0 表示 `is`（类型检查 ✓）、1 表示 `in` / `not in`（成员测试 ✓）。
//   ⚠ 只查这两类 + 只在 **语义 visit** 调用（infer 会重复喷 ✗ 见实现处的说明 ✓）。
void semantic_check_not_precedence(Ast* operand, int line, int column, int kind);

// printf 名字骗人（WARN_PRINTF_NO_FORMAT，2026-10-10 加）：`printf` 只做"**不换行打印**"、
// **不做 % 格式化** ✗（实测：`printf("%d = %s\n", 3, "x")` 不报错、`%d` 原样打出 ✓）
// ⇒ 首参是**含 '%' 的字符串字面量**且**实参 > 1** 时提醒 ✓（其它形状一律不报 ✓ 少报不误报）
// 两条通道共用：全局 `printf(...)`（visit_expr.inc）与 `io.printf(...)`（visit_module.inc）
void semantic_check_printf_style(const char* callee, AstList* args, int line, int column);

// face 方法的实参检查（唯一实现；调用点在 visit_module.inc 的 MODULE_CALL 分流处）。
// 同文件用解析器 face AST 的 method_param_types 判逐参类型；跨模块读符号表的 param_types。
// obj_type（可空，v3.2.8 加）：**接收者的静态类型** —— 泛型 face（`Comparable[int]`）要靠它的
//   类型实参把方法形参里的占位符（`T` ⇒ 解析成 TYPE_STRUCT "T"）代入，否则误报"期望 struct T"。
int semantic_check_face_method_args(Semantic* s, const char* face_name, const char* method_name,
                                    AstList* args, int line, int column, TypeInfo* obj_type);

// ============================================================================
// 前置声明 - 变量解析
// ============================================================================
int allocate_local_index(Semantic* s);

// 槽位回收（寄存器号 8 位上限；实现见 semantic_upvalue.c）:
//   local_index 可回退 ⇒ 语句/作用域边界用 sem_local_mark/sem_local_release 成对回收。
int sem_local_level(Semantic* s);              // 当前函数在 func_max_index/func_pinned 里的下标
int sem_local_mark(Semantic* s);               // 取回收标记（语句/作用域开始处）
void sem_local_release(Semantic* s, int mark); // 回退到 max(mark, 钉住下界)

// ============================================================================
// 前置声明 - upvalue 管理
// ============================================================================
ImportedModuleInfo* find_imported_module(Semantic* s, const char* alias);
int add_upvalue(Ast* func_ast, const char* name, int index, int is_local, int is_value_capture);
Symbol* resolve_variable_with_upvalue(Semantic* s, const char* name, SymRef* ref);
// 上面那个的**原实现**（2026-10-10 拆出来给 WARN_UNUSED_VAR 用）：它只做解析，
// 而公开名 `resolve_variable_with_upvalue` 现在还会顺手把「该变量被读过」置位 ✓
// （见 semantic_upvalue.c 里的说明：函数式变量被调用时走的就是这条路 ✓ 漏了会误报 ✗）。
Symbol* resolve_variable_with_upvalue_raw(Semantic* s, const char* name, SymRef* ref);

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
    char** param_names, int param_count, char** const_names, int const_count, const char* method_name,
    char** amb_names, int* amb_pcnts, int amb_count,
    int* method_pcounts, int* amb_meth_pcnts);

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
// 从函数体推断返回类型（唯一实现，定义在 semantic_type.c）。语义：看 return 语句的实际类型；
//   推不出（空体 / 全 any）返回 NULL 或 TYPE_ANY。返回**新类型**（调用方 type_free）。
//   v3.2.8 起导出：face impl 的返回类型兼容性检查要用它处理"impl 方法没标返回类型"的情况。
TypeInfo* infer_return_type_from_body(Semantic* s, Ast* body);
// 泛型参数 → native 形参：**两段式需求推断**（B 方案，2026-10-02；实现与理由见 semantic_type_utils.c）
//   ① semantic_native_arg_generic()：native 实参检查点调用。若实参是**当前泛型函数**声明的类型参数
//      ⇒ 记一条需求（不报错 —— 此刻 T 未知）；否则兜底就地报错。返回 1 = 已报错。
//   ② semantic_check_generic_requirements()：泛型函数的**调用点**调用（那里已能推断/显式给出类型实参）
//      ⇒ 逐条校验需求 ⇒ `hexit(42)` 编译期报错、`hexit("中")` 合法 ✓
//   ⚠ 只对 native 调用点使用 —— 用户函数/字段/赋值会插运行期转换，不构成 UB，收进来只会误伤 ✓
//   `callee_desc` 形如 "strings.to_hex" / "方法 'pad_start'"
//   同时也覆盖**可空值**（`int?`/`string?` 直传非可空 native 形参 = 盲转遇 null ⇒ 段错误 ✗）
int semantic_check_native_arg(Semantic* s, Ast* ast, TypeInfo* arg_type, TypeKind expected,
                              const char* callee_desc, int arg_index);
// 约束名 → 内建具体类型 kind（`[T: string]` 这类；不是具体类型名 ⇒ TYPE_UNKNOWN，走 face 那条路 ✓）
TypeKind semantic_constraint_builtin_kind(const char* name);
void semantic_record_generic_requirement(Semantic* s, const char* param_name, TypeKind expected,
                                         int line, const char* callee);
//   `owner_struct`：需求属于 struct 方法时传 struct 名、否则传 NULL（与记录侧对称 ✓）
void semantic_check_generic_requirements(Semantic* s, const char* owner_struct, const char* func_name,
                                         const char* param_name, TypeInfo* actual, Ast* call_ast);
// **顺序无关**的两个入口（2026-10-02）：调用点先 semantic_record_pending_req_check() 登记，
//   分析收尾由 semantic_flush_pending_req_checks() 统一复查（否则调用点早于定义体时会漏过 ✗）
void semantic_record_pending_req_check(Semantic* s, const char* owner_struct, const char* func_name,
                                       const char* param_name, TypeInfo* actual, Ast* call_ast);
// 调用点那一刻**推不出**类型实参时登记它（如"调用在前、定义在后"）⇒ 收尾用 func_table 里
//   已完整的定义**重新推断**再判 ✓（否则该形态会漏过 ✗）
void semantic_record_pending_generic_call(Semantic* s, const char* callee_name, Ast* call_ast);
void semantic_flush_pending_req_checks(Semantic* s);
void semantic_free_generic_requirements(Semantic* s);

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
// C2：未定义函数相似名提示（函数表 + 内置 native + 作用域变量）
const char* get_undefined_func_hint(Semantic* s, const char* name);
// C1：内置类型方法相似名提示（委托 method_table_similar_hint）
const char* semantic_method_hint(TypeInfo* type, const char* method_name);
// C1：在给定名字集合中找最相似的（struct 方法提示用）
const char* get_similar_in_names(const char** names, int count, const char* name);
// C1：用户 struct 方法相似名提示（扫函数表 "Struct::method" 占位符）
const char* get_similar_struct_method_hint(Semantic* s, const char* struct_name, const char* method_name);
// E5：未定义的 struct 类型若是已导入模块的导出类型，提示先 use 导入
const char* get_module_with_struct_hint(Semantic* s, const char* struct_name);

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
