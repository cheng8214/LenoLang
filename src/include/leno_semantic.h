#ifndef LENO_SEMANTIC_H
#define LENO_SEMANTIC_H

#include "leno_types.h"
#include "leno_ast.h"
#include "leno_vm.h"
#include "module_symbol_table.h"

// ============================================================================
// 导入模块信息（用于语义分析时检查模块方法）
// ============================================================================

typedef struct {
    char* alias;                    // 模块别名（如 test）
    char* file_path;                // 模块文件路径（如 test.leno）
    ModuleSymbolTable* sym_table;   // 模块符号表（缓存所有导出符号）
    int load_failed;                // 模块加载是否失败（文件不存在或读取失败）
} ImportedModuleInfo;

// ============================================================================
// 函数表哈希表条目
// ============================================================================

typedef struct FuncEntry {
    char* name;         // 函数名
    Ast* func;          // 函数 AST 节点
    struct FuncEntry* next;  // 链式冲突处理
} FuncEntry;

// ============================================================================
// 函数表（哈希表实现 - O(1) 查找）
// ============================================================================

typedef struct {
    FuncEntry** entries;    // 哈希表数组
    int capacity;           // 表容量
    int count;              // 当前函数数量
} FuncTable;

// ============================================================================
// 泛型参数"需求推断"（B 方案，2026-10-02）
// ----------------------------------------------------------------------------
// 要解决的问题：编译器**不做实例化后复查**（泛型在运行期是擦除的 —— `generic_args` 只存在于
//   parser/符号表，codegen 的"特化"是指令级 int/float 快路径），而 **native 边界**是 C 侧
//   `val_as_obj / val_as_int` 无校验盲转 ⇒ 函数体里 `strings.to_hex(x)`（x: T）在**定义处**
//   无法判定对错：`hexit("中")` 合法、`hexit(42)` 把 int 当指针解引用（实测 exit=0xC0000005）✗
// 做法（两段式）：
//   ① **收集**：检查泛型函数体时，若把 T 用在了 native 的**具体类型形参**上 ⇒ 记一条需求
//      「T 需要与 C 兼容」（此刻不报错，因为 T 未知）
//   ② **校验**：在该泛型函数的**调用点**（那里已能推断或显式给出类型实参）逐条校验需求
//      ⇒ `hexit(42)` 编译期报错；`hexit("中")` 照常通过 ✓（比"定义处一刀切"既准又不误伤）
// 注：只对 **native 形参**收集 —— 用户层赋值/实参/返回会插**运行期转换**（实测 `to_str(42)` → "42"、
//   `int y = "中"` 是可捕获抛错），不构成 UB，收进来只会误伤（实测误伤 4 个用例）✓
// ============================================================================

typedef struct {
    char* func_name;    // 需求属于哪个泛型函数 / **方法**（定义处函数名）
    char* owner_struct; // 方法需求时的 struct 名；**函数需求为 NULL**（用于区分两类所有者 ✓）
    char* param_name;   // 类型参数名（如 "T"）
    TypeKind expected;  // native 形参要求的具体类型（如 TYPE_STRING）
    int line;           // 需求来源行（函数体内那一行，供调用点报错时指路）
    char* callee;       // 需求来源描述（如 "strings.to_hex"）
} GenericRequirement;

// 待复查的调用点（**顺序无关**的关键，2026-10-02）：
//   需求是在"访问定义体"时记录的，而调用点可能在定义体**之前**被访问（main 在前、
//   struct 前向引用等）⇒ 那一刻需求表还是空的 ⇒ 当场判会**漏过** ✗
//   做法：调用点只**登记**，等所有定义体都访问完（semantic_analyze / _module 的收尾）
//   再统一复查 ⇒ 与源码顺序无关 ✓
typedef struct {
    Ast* call_ast;      // 调用点（报错位置就取它的 line/column ✓；AST 活得比本表久 ✓）
    char* owner_struct; // NULL = 自由函数
    char* func_name;    // 函数名 / 方法名
    char* param_name;   // 类型参数名（re_infer 时为 NULL ✓）
    TypeInfo* actual;   // 调用点推断出的类型实参（副本，flush 后释放 ✓；re_infer 时为 NULL ✓）
    // **收尾重新推断**标记（2026-10-03）：调用点那一刻函数定义还没解析完 ⇒ 推断结果退化成
    //   `any`（典型场景：main 在前、被调函数在后）⇒ 只登记"这个泛型调用要在收尾重判" ✓
    int re_infer;
} PendingReqCheck;

// ============================================================================
// 单遍语义分析（解决前向引用 + 闭包）
// ============================================================================

typedef struct {
    Scope* current;
    Scope* root_scope;
    Ast* root;
    FuncTable func_table;   // 函数表（哈希表，O(1) 查找）
    Ast* current_func;  // 当前正在分析的函数
    Ast* func_stack[64]; // 函数栈，用于处理多层嵌套闭包
    int func_stack_depth; // 函数栈深度
    int local_index;    // 全局局部变量索引计数器（所有作用域共享）
    ImportedModuleInfo imported_modules[64];  // 导入的模块信息表
    int imported_module_count;  // 导入的模块数量
    int is_module;      // 是否为模块模式
    int is_lsp_mode;    // 是否为 LSP 模式（保留所有作用域供符号查询）
    int in_clib;        // 是否在 clib/cfunc 声明上下文中（C 布局类型允许）
    int has_module_load_failure; // 是否有模块加载失败（用于抑制下游级联 any 错误）
    int in_main_func;   // 是否正在分析入口函数 main 的函数体（返回值必须为 int，作为进程退出码）

    // ---- 泛型需求推断（见上面 GenericRequirement 的说明块）----
    Ast* cur_generic_func;      // 当前正在检查的**泛型函数定义**（非泛型函数体内为 NULL）
    Ast* cur_generic_struct;    // 当前正在检查其**方法体**的泛型 struct 定义（否则为 NULL）
    char* cur_struct_method_name; // 上述方法体所属的**方法名**（需求要按 struct+方法 记 ✓；否则 NULL）
    GenericRequirement* reqs;   // 需求表（动态数组）
    int req_count;
    int req_capacity;
    PendingReqCheck* pending;   // 待复查的调用点（收尾统一判 ⇒ 顺序无关 ✓）
    int pending_count;
    int pending_capacity;
} Semantic;

// 函数表 API
void func_table_init(FuncTable* table);
void func_table_free(FuncTable* table);
int func_table_add(FuncTable* table, const char* name, Ast* func);
Ast* func_table_find(FuncTable* table, const char* name);

void semantic_init(Semantic* s, Ast* root);
void semantic_analyze(Semantic* s, Ast* ast);  // 单遍分析
void semantic_analyze_module(Semantic* s, Ast* ast);  // 模块语义分析
void semantic_cleanup(Semantic* s);

#endif // LENO_SEMANTIC_H
