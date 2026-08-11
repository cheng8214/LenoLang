#ifndef LENO_VM_H
#define LENO_VM_H

#include <stdbool.h>
#include "leno_types.h"
#include "leno_value.h"
#include "platform_thread.h"

// ============================================================================
// 字节码操作码
// ============================================================================

typedef enum {
    OP_CONST,
    OP_NULL,
    OP_TRUE,
    OP_FALSE,
    OP_ZERO,    // 压入 0
    OP_ONE,     // 压入 1
    OP_POP,
    OP_DUP,
    OP_GET_LOCAL,
    OP_SET_LOCAL,
    OP_SET_LOCAL_POP,   // 设置局部变量并弹出栈顶（合并 OP_SET_LOCAL + OP_POP）
    OP_MOVE_LOCAL,      // 局部变量间直接复制：src→dst，并压栈（合并 OP_GET_LOCAL + OP_SET_LOCAL）
    OP_GET_GLOBAL,
    OP_SET_GLOBAL,
    OP_GET_UPVALUE,
    OP_SET_UPVALUE,
    OP_CLOSE_UPVALUE,
    OP_DEFINE_GLOBAL,
    OP_GET_GLOBAL_FUNC,
    OP_DEFINE_GLOBAL_FUNC,
    OP_GET_NATIVE,
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,     // 取模
    OP_BITAND,  // 按位与
    OP_BITOR,   // 按位或
    OP_BITXOR,  // 按位异或
    OP_BITNOT,  // 按位非
    OP_SHL,     // 左移
    OP_SHR,     // 右移（算术右移）
    OP_USHR,    // 逻辑右移（无符号右移 >>>）
    OP_NEG,
    OP_NOT,
    OP_CAST_FLOAT,  // 将 int 转换为 float
    OP_CAST_INT,    // 将 float 转换为 int（截断）
    OP_CAST_STRING, // 将值转换为 string（null 转为空字符串）
    OP_SET_PTR_ELEM_TYPE, // 设置栈顶 FFI 指针的 element_type（Ptr[T] 声明时使用）
    OP_SET_DECLARED_FACE, // 设置栈顶 struct 实例的 declared_face（face 类型变量声明时使用）
    OP_INC,  // ++ (栈顶值)
    OP_DEC,  // -- (栈顶值)
    OP_INC_LOCAL,  // 局部变量++，返回旧值
    OP_DEC_LOCAL,  // 局部变量--，返回旧值
    OP_PRE_INC_LOCAL,  // 局部变量++，返回新值
    OP_PRE_DEC_LOCAL,  // 局部变量--，返回新值
    OP_EQ,
    OP_NEQ,
    OP_IS_NULL,        // 检查栈顶是否为 null：弹出值，压入 bool（修复 float 0.0 == null 的 VM bug）
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,
    OP_IN,             // in 操作符：检查成员是否存在
    OP_RANGE,          // 范围对象：创建 Range
    OP_JUMP,            // 4字节偏移跳转
    OP_JUMP_IF_FALSE,   // 4字节偏移条件跳转
    OP_JUMP_IF_TRUE,    // 4字节偏移条件跳转
    OP_LOOP,            // 4字节偏移循环跳转
    OP_CALL,
    OP_TAIL_CALL,   // 尾调用优化：复用当前调用帧
    OP_CLOSURE,
    OP_RETURN,
    OP_ARRAY,
    OP_ARRAY_GET,
    OP_ARRAY_SET,
    OP_ARRAY_APPEND,        // 数组追加元素（压入新长度，表达式用）
    OP_ARRAY_APPEND_NOPUSH, // 数组追加元素（不压栈，语句用）
    OP_DICT,
    OP_DICT_GET,
    OP_DICT_SET,
    OP_DICT_GET_KEY,   // 按索引获取字典键（用于for循环迭代）
    OP_LOAD_NATIVE_MODULE, // 加载原生模块（import 时执行，运行时调用 native_init_module）
    OP_MODULE_CALL,    // 模块方法调用
    OP_GET_MODULE_CONST, // 获取原生模块常量
    OP_STRING_ADD,     // 字符串拼接（自动转换类型）
    OP_INDEX,          // 通用索引访问（数组或字典）
    OP_INDEX_SET,      // 通用索引赋值（数组或字典）
    OP_SLICE,          // 数组切片：arr[start:end]
    OP_LENGTH,         // 获取长度（字符串、数组、字典）
    OP_ITER_GET,       // 迭代获取元素（运行时检测类型：字符串/数组用索引，字典用键）
    OP_ITER_GET_VALUE, // 迭代获取值（用于字典遍历时获取值）
    OP_TRY,            // 开始 try 块
    OP_CATCH,          // 开始 catch 块
    OP_FINALLY,        // 开始 finally 块
    OP_END_TRY,        // 结束 try-catch-finally
    OP_THROW,          // 抛出异常
    OP_GET_MODULE_VAR, // 获取模块变量
    OP_SET_MODULE_VAR, // 设置模块变量
    OP_GET_MODULE_FUNC,// 获取模块函数
    OP_DEFINE_MODULE_FUNC,// 定义模块函数
    OP_GET_PROPERTY,   // 获取属性或实例方法
    OP_TYPE_CHECK,     // 类型守卫：检查栈顶值是否是指定类型，类型由操作数指定
    OP_AS_CAST,        // 安全类型转换：匹配则保留原值，不匹配则返回 null
    OP_FOR_PREP,       // For循环准备
    OP_FOR_LOOP,       // For循环迭代
    // 类型特化指令（用于编译器已知操作数类型时的优化，int运算溢出时自动提升为bigint）
    OP_ADD_INT,        // int + int（快速路径，溢出时自动转为bigint）
    OP_SUB_INT,        // int - int（快速路径，溢出时自动转为bigint）
    OP_MUL_INT,        // int * int（快速路径，溢出时自动转为bigint）
    OP_DIV_INT,        // int / int -> float（快速路径，无类型检查）
    OP_MOD_INT,        // int % int（快速路径，无类型检查）
    OP_ADD_FLOAT,      // float + float（快速路径，无类型检查）
    OP_SUB_FLOAT,      // float - float（快速路径，无类型检查）
    OP_MUL_FLOAT,      // float * float（快速路径，无类型检查）
    OP_DIV_FLOAT,      // float / float（快速路径，无类型检查）
    OP_NEG_INT,        // -int（快速路径，溢出时自动转为bigint）
    OP_NEG_FLOAT,      // -float（快速路径，无类型检查）
    OP_EQ_INT,         // int == int（快速路径，无类型检查）
    OP_LT_INT,         // int < int（快速路径，无类型检查）
    OP_GT_INT,         // int > int（快速路径，无类型检查）
    OP_LE_INT,         // int <= int（快速路径，无类型检查）
    OP_GE_INT,         // int >= int（快速路径，无类型检查）
    OP_EQ_FLOAT,       // float == float（快速路径，无类型检查）
    OP_LT_FLOAT,       // float < float（快速路径，无类型检查）
    OP_GT_FLOAT,       // float > float（快速路径，无类型检查）
    OP_LE_FLOAT,       // float <= float（快速路径，无类型检查）
    OP_GE_FLOAT,       // float >= float（快速路径，无类型检查）
    // 立即数操作码（避免加载小常量开销，立即数范围为 -128 到 127）
    OP_ADD_INT_IMM,    // int + imm（立即数加法）
    OP_SUB_INT_IMM,    // int - imm（立即数减法）
    OP_MUL_INT_IMM,    // int * imm（立即数乘法）
    OP_LT_INT_IMM,     // int < imm（立即数小于）
    OP_GT_INT_IMM,     // int > imm（立即数大于）
    OP_LE_INT_IMM,     // int <= imm（立即数小于等于）
    OP_GE_INT_IMM,     // int >= imm（立即数大于等于）
    OP_EQ_INT_IMM,     // int == imm（立即数等于）
    OP_SHL_IMM,        // int << imm（立即数左移）
    OP_SHR_IMM,        // int >> imm（立即数算术右移）
    OP_USHR_IMM,       // int >>> imm（立即数逻辑右移）
    // struct 相关指令
    OP_STRUCT_DEF,     // 定义结构体
    OP_STRUCT_INIT,    // 创建结构体实例
    OP_GET_FIELD,      // 获取结构体字段
    OP_SET_FIELD,      // 设置结构体字段
    OP_GET_FIELD_ADDR, // 获取 cstruct 字段地址（返回 ObjFFIPointer）
    OP_GET_METHOD,     // 获取结构体方法（从struct方法表）
    // enum 相关指令
    OP_ENUM_DEF,       // 定义枚举
    OP_FACE_DEF,       // 定义接口
    // cstruct 相关指令
    OP_CSTRUCT_DEF,    // 定义 C 布局结构体
    OP_GET_CSTRUCT_DEF,// 获取 C 布局结构体定义
    // 协程相关指令
    OP_AWAIT,          // 等待 Future 完成，暂停当前协程
    OP_ASYNC_CALL,     // 调用 async 函数，创建新协程并返回 Future
    // 模块初始化指令（运行时执行 .leno 模块的初始化字节码）
    OP_INIT_LENOMODULE, // 初始化 .leno 模块（延迟执行 init_chunk）
    // clib 调用指令（栈上: lib, func_name, args... -> 结果）
    OP_CLIB_CALL,       // 调用 C 库函数（通过 FFI）
    OP_CFUNC_CALLBACK,  // 创建 cfunc 回调（编译期签名，无需字符串）
    // 原生函数调用合并指令（省掉 OP_GET_NATIVE + OP_CALL/TAIL_CALL 配对）
    OP_CALL_NATIVE,      // 直接调用原生函数: name_const(2) arg_count(2)
    OP_TAIL_CALL_NATIVE, // 尾调用原生函数: name_const(2) arg_count(2)
    // C 布局类型转换指令
    OP_U8_TO_F64,        // u8 → f64（0-255 无符号整数转双精度浮点）
    // 泛型函数调用类型参数传递
    OP_PUSH_TYPE_ARGS,   // 将泛型类型参数推入 VM 待处理区: count(1) [const(2)]...
    // 析构函数调用
    OP_DTOR_LOCAL,       // 对局部变量调用析构函数: slot(2)
    // 全局函数调用合并指令（省掉 OP_GET_GLOBAL_FUNC + OP_CALL 配对）
    OP_CALL_GLOBAL_FUNC, // 直接调用全局函数: func_slot(2) arg_count(2)
    // NOPUSH 变体（语句上下文，省掉 OP_POP 分发开销）
    OP_INC_LOCAL_NOPUSH,   // 局部变量++，不压栈（i++ 语句用）
    OP_DEC_LOCAL_NOPUSH,   // 局部变量--，不压栈（i-- 语句用）
    OP_MOVE_LOCAL_POP,     // 局部变量间复制：src→dst，不压栈（a=b 语句用）
    OP_CALL_NATIVE_VOID,   // 调用原生函数，不压栈结果（print() 语句用）
    OP_DICT_SET_NOPUSH,    // 字典赋值，不压栈（dict[k]=v 语句用）
    OP_INDEX_SET_NOPUSH,   // 通用索引赋值，不压栈（arr[i]=v 语句用）
    OP_CLEAR_LOCAL_RANGE,  // 清零局部变量范围 [base, base+count)，用于内联清理
} OpCode;

// ============================================================================
// 字节码块
// ============================================================================

typedef struct Chunk {
    uint8_t* code;
    int len;
    int code_capacity;
    Value* constants;
    int const_cnt;
    int const_capacity;
    int* lines;
    char* filename;  // 源代码文件名（用于错误报告）
    int local_count; // 主代码块的局部变量数量（用于闭包捕获）
} Chunk;

// Chunk API
void chunk_init(Chunk* chunk);
void chunk_free(Chunk* chunk);
int chunk_add_const(Chunk* chunk, Value value);
void chunk_write(Chunk* chunk, uint8_t byte, int line);

// ============================================================================
// 调用栈帧
// ============================================================================

// 内联 locals 数组大小 - 小函数直接使用栈上空间，避免 malloc/free
// 经验值：大多数函数的局部变量不超过 8 个
#define INLINE_LOCALS_MAX 8

typedef struct CallFrame {
    Chunk* chunk;
    uint8_t* ip;
    int stack_base;      // 栈基址索引（用于操作数栈）
    int slot_count;      // locals数组大小（包括参数和局部变量）
    ObjClosure* closure;
    Value* locals;       // 独立的局部变量数组（可能指向 inline_locals）
    int local_count;     // locals数组大小（与slot_count相同）
    // 内联 locals 数组 - 小函数直接使用，避免内存分配
    Value inline_locals[INLINE_LOCALS_MAX];
    int locals_is_dynamic;  // 1 = 使用 malloc 分配，0 = 使用 inline_locals
    // 异常处理
    uint8_t* catch_ip;   // catch 块的指令指针
    uint8_t* finally_ip; // finally 块的指令指针
    uint8_t* prev_catch_ip;   // 保存之前的 catch_ip（用于嵌套 try-catch）
    uint8_t* prev_finally_ip; // 保存之前的 finally_ip（用于嵌套 try-catch）
    int in_finally;      // 标记是否在 finally 块中（防止无限循环）
    Value try_return_value;  // 保存 try 块中的 return 值
    int has_try_return;      // 标记是否有 try return 值需要处理
    // 模块信息
    void* module;        // 所属模块（如果是模块函数）
    // 优化标志
    int has_captures;    // 1 = 该帧捕获了 upvalue，0 = 无捕获（优化 close_upvalues）
} CallFrame;

// ============================================================================
// 模块帧 - 模块执行时的上下文
// ============================================================================

typedef struct ModuleFrame {
    ObjModule* module;    // 所属模块
    Value* globals;       // 模块全局变量数组
    int global_count;     // 全局变量数量
    int global_capacity;  // 全局变量数组容量
    struct ModuleFrame* parent;  // 父模块帧（用于嵌套模块）
} ModuleFrame;

// ============================================================================
// 作用域和符号表（前向声明）
// ============================================================================

typedef struct Symbol Symbol;
typedef struct Scope Scope;

struct Symbol {
    SymKind kind;
    char* name;
    int index;
    int is_captured;
    int is_in_loop;           // 变量是否定义在循环作用域中
    int is_initialized;       // 变量是否已初始化
    int is_const;             // 是否为 const 声明（不可重新赋值）
    Scope* scope;
    Symbol* next;
    TypeInfo* type;
    // 字典键集合（仅当类型为 dict 时使用）
    char** dict_keys;
    int dict_key_count;
    int dict_key_capacity;
    // struct 字段信息（仅当类型为 struct 时使用）
    char** struct_field_names;
    TypeInfo** struct_field_types;
    int struct_field_count;
    char* struct_type_name;     // struct 类型名称（如 "Point"）
    int* struct_field_null_default;  // 字段默认值是否为 null（用于空指针检查）
    // 泛型 struct 类型参数（仅当 struct 有泛型参数时使用）
    char** struct_type_params;       // 类型参数名数组（如 ["T", "U"]）
    int struct_type_param_count;     // 类型参数数量
    // cstruct 布局信息（仅当类型为 cstruct 时使用）
    int cstruct_size;           // cstruct 总大小
    int cstruct_alignment;      // cstruct 对齐要求
    int* cstruct_field_offsets; // cstruct 字段偏移量数组
    // clib 函数签名信息（仅当类型为 clib 时使用）
    char** clib_func_names;         // 函数名称数组
    TypeInfo** clib_func_return_types; // 函数返回类型数组
    int* clib_func_param_counts;    // 函数参数数量数组
    TypeInfo*** clib_func_param_types; // 函数参数类型数组（二维）
    int clib_func_count;            // 函数数量
    // cfunc 回调签名信息（仅当类型为 cfunc 时使用）
    TypeInfo** cfunc_param_types;   // 参数类型数组
    int cfunc_param_count;          // 参数数量
    TypeInfo* cfunc_return_type;    // 返回类型
    // enum 成员信息（仅当类型为 enum 时使用，use 导入的 enum）
    char** enum_value_names;        // 成员名称数组
    int64_t* enum_values;           // 成员值数组（int64_t，支持 >= 2^31 的位标志值）
    int enum_value_count;           // 成员数量
    // 函数别名信息（仅当 kind == SYM_FUNC_ALIAS 时使用）
    char* alias_module_name;        // 源模块名（如 "core"）
    char* alias_func_name;          // 源函数名（如 "loadTexture"）
};

struct Scope {
    Scope* parent;
    Symbol** syms;            // 动态数组：符号表
    int sym_cnt;
    int sym_capacity;         // 符号表容量
    int is_func;
    int is_loop;              // 作用域是否是循环作用域
    int depth;
    int local_count;
    int param_count;
    int global_var_index;
    int global_func_index;
    
    // 作用域树结构
    Scope** children;         // 子作用域数组
    int child_count;          // 子作用域数量
    int child_capacity;       // 子作用域数组容量
    int id;                   // 作用域唯一标识（用于调试）
};

// 作用域 API
Scope* scope_new(Scope* parent, int is_func);
void scope_free(Scope* scope);
Symbol* scope_define(Scope* s, const char* name, SymKind kind);
Symbol* scope_resolve(Scope* s, const char* name);
Symbol* scope_resolve_local(Scope* s, const char* name);

// 字典键管理 API
void symbol_add_dict_key(Symbol* sym, const char* key);
int symbol_has_dict_key(Symbol* sym, const char* key);

// ============================================================================
// 作用域树 API
// ============================================================================

// 获取根作用域
Scope* scope_get_root(Scope* s);

// 获取第 n 个子作用域
Scope* scope_get_child(Scope* s, int index);

// 遍历作用域树（前序遍历）
void scope_traverse_preorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata);

// 遍历作用域树（后序遍历）
void scope_traverse_postorder(Scope* s, void (*callback)(Scope* scope, void* userdata), void* userdata);

// 在作用域树中查找符号（广度优先）
Symbol* scope_resolve_tree_bfs(Scope* root, const char* name);

// 获取作用域的完整路径（用于调试）
char* scope_get_path(Scope* s);

// 打印作用域树（用于调试）
void scope_print_tree(Scope* s, int indent);

// 从父作用域中移除子作用域（不释放子作用域）
// 用于临时作用域，避免递归释放时重复释放
void scope_detach_child(Scope* parent, Scope* child);

// ============================================================================
// 内联缓存 (Inline Caching) - 优化属性访问和方法调用
// ============================================================================
//
// 原理：缓存 receiver 类型到方法的映射，避免每次查找
// 效果：对频繁访问同一类型对象的属性/方法，性能提升 2-5 倍
//

typedef struct {
    int valid;              // 缓存是否有效
    ObjType receiver_type;  // receiver 的对象类型
    uint32_t name_hash;     // 方法名的哈希值（防止不同方法碰撞到同一缓存槽）
    ObjNative* method;      // 缓存的方法指针
} InlineCacheEntry;

// 每个 OP_GET_PROPERTY 指令使用一个缓存槽
// 简单实现：使用固定大小的缓存表，以指令地址的哈希值为索引
#define IC_CACHE_SIZE 4096  // 缓存表大小（必须是 2 的幂），增大以减少冲突

// ============================================================================
// 内联缓存：全局 Native 函数 (OP_GET_NATIVE)
// 缓存 name_hash → ObjNative* 的映射，避免每次调用都线性扫描 native 函数表
// ============================================================================
typedef struct {
    int valid;
    uint32_t name_hash;     // native 函数名的 FNV-1a 哈希
    ObjNative* fn;          // 缓存的 native 函数对象
} InlineNativeCacheEntry;

#define IC_NATIVE_CACHE_SIZE 2048  // 必须是 2 的幂

// ============================================================================
// 内联缓存：模块方法调用 (OP_MODULE_CALL)
// 缓存 hash(module, method) → ModuleMethodMeta* 的映射
// 避免每次调用都做哈希表查找
// 注意：ModuleMethodMeta 在 native.h 中定义，此处用 void* 避免循环依赖
// ============================================================================
typedef struct {
    int valid;
    uint32_t combined_hash;         // hash(module_name, method_name) 组合哈希
    void* meta;                     // 实际类型为 ModuleMethodMeta*（在 native.h 中定义）
} InlineModuleCallCacheEntry;

#define IC_MODULE_CACHE_SIZE 2048   // 必须是 2 的幂

// ============================================================================
// 虚拟机
// ============================================================================

typedef struct VM {
    Chunk* chunk;
    uint8_t* ip;
    Value* stack;
    int sp;
    int stack_capacity;
    CallFrame* frames;
    int frame_cnt;
    int frame_capacity;
    Scope* global_scope;
    Upvalue* open_upvalues;
    Value* globals;           // 动态数组：全局变量
    int global_count;
    int global_capacity;      // 全局变量数组容量
    Value* global_funcs;      // 动态数组：全局函数
    int global_func_count;
    int global_func_capacity; // 全局函数数组容量
    // 模块系统
    ModuleFrame* current_module_frame;  // 当前模块帧
    // 异常处理
    Value exception;          // 当前异常值
    int has_exception;        // 是否有未处理的异常
    uint8_t* catch_ip;        // catch 块的指令指针
    int catch_frame;          // catch 块的帧索引
    int pending_exception;    // finally 后需要传播的异常
    int exception_line;       // 异常发生时的行号
    int current_line;         // 当前执行行号（供原生函数使用）
    // 内联缓存
    InlineCacheEntry ic_cache[IC_CACHE_SIZE];           // 属性访问缓存表 (OP_GET_PROPERTY)
    InlineNativeCacheEntry ic_native_cache[IC_NATIVE_CACHE_SIZE];   // 全局 Native 函数缓存 (OP_GET_NATIVE)
    InlineModuleCallCacheEntry ic_module_cache[IC_MODULE_CACHE_SIZE]; // 模块方法缓存 (OP_MODULE_CALL)
    int ic_hits;               // 缓存命中次数（统计用）
    int ic_misses;             // 缓存未命中次数（统计用）
    // 协程系统
    EventLoop* event_loop;     // 事件循环
    ObjCoroutine* current_coroutine;
    ObjCoroutine* all_coroutines;
    Value last_return_value;
    // 线程系统
    ObjThread** active_threads;  // 活动线程列表
    int active_thread_count;
    int active_thread_capacity;
    int stop_frame_cnt;          // 回调执行时：帧数降到此值时停止
    // 泛型类型参数传递：OP_PUSH_TYPE_ARGS → OP_CALL 间的桥接
    char** pending_type_args;    // 待处理的泛型类型参数
    int pending_type_arg_count;  // 待处理数量
    int gc_return_counter;       // OP_RETURN 的 GC 检查计数器（放在 VM 结构体中避免 THREAD_LOCAL 开销）
} VM;

// 主线程使用全局 VM（效率第一）
extern VM vm;
extern int vm_initialized;

// 当前执行线程使用的 VM 指针（子线程使用独立的 VM）
extern THREAD_LOCAL VM* current_exec_vm;

// VM API
void vm_init(void);
void vm_init_with_scope(Scope* global_scope);
void vm_load(Chunk* c);
int vm_run(void);
int vm_run_chunk(Chunk* chunk);
int vm_run_with_vm(VM* vm_ptr);
void vm_reset_stack(void);
void vm_free(void);
int vm_call_value(Value callee, int arg_count, int line);
bool vm_get_global(ObjString* name, Value* out);

// ============================================================================
// 栈操作（static inline 供编译器内联优化）
// ============================================================================

// 动态扩容辅助函数（内部使用）
static inline int vm_grow_capacity(int old_capacity) {
    return old_capacity < 8 ? 8 : old_capacity * 2;
}

// 栈操作：压入值（带 VM* 参数）
static inline void vm_stack_push(VM* vm, Value v) {
    if (__builtin_expect(vm->sp >= vm->stack_capacity, 0)) {
        int new_capacity = vm_grow_capacity(vm->stack_capacity);
        Value* new_stack = (Value*)realloc(vm->stack, new_capacity * sizeof(Value));
        if (!new_stack) return;
        vm->stack = new_stack;
        vm->stack_capacity = new_capacity;
    }
    vm->stack[vm->sp++] = v;
}

// 栈操作：压入值（不检查溢出，用于性能关键路径）
// 警告：只在确定栈有空间时使用，否则会导致未定义行为
static inline void vm_stack_push_fast(VM* vm, Value v) {
    vm->stack[vm->sp++] = v;
}

// 栈操作：弹出值（带 VM* 参数）
static inline Value vm_stack_pop(VM* vm) {
    if (__builtin_expect(vm->sp <= 0, 0)) {
        return val_null();
    }
    return vm->stack[--vm->sp];
}

// 栈操作：弹出值（不检查下溢，用于性能关键路径）
// 警告：只在确定栈有值时使用，否则会导致未定义行为
static inline Value vm_stack_pop_fast(VM* vm) {
    return vm->stack[--vm->sp];
}

// 栈操作：查看栈顶值（带 VM* 参数）
static inline Value vm_stack_peek(VM* vm, int distance) {
    if (__builtin_expect(vm->sp - 1 - distance < 0, 0)) {
        return val_null();
    }
    return vm->stack[vm->sp - 1 - distance];
}

// 栈操作：查看栈顶值（不检查边界，用于性能关键路径）
// 警告：只在确定栈有足够值时使用，否则会导致未定义行为
static inline Value vm_stack_peek_fast(VM* vm, int distance) {
    return vm->stack[vm->sp - 1 - distance];
}

// Native 函数注册
// min_arity/max_arity: 当 arity == -1（可变参数）时，指定最小/最大允许参数个数；其他情况传 -1
void vm_register_native(const char* name, NativeFn function, int arity, int min_arity, int max_arity, TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
TypeKind vm_get_native_return_type(const char* name);

// IO 模块初始化
void io_init_globals(void);

// ============================================================================
// 模块系统 API
// ============================================================================

// 创建模块对象
ObjModule* module_new(const char* name);

// 创建模块帧
ModuleFrame* module_frame_new(ObjModule* module);

// 进入模块帧
void module_frame_enter(ModuleFrame* frame);

// 退出模块帧
void module_frame_exit(void);

// 获取当前模块帧中的变量
Value module_get_var(int index);

// 设置当前模块帧中的变量
void module_set_var(int index, Value value);

// 确保模块变量数组容量
int module_ensure_var_capacity(int slot);

// 模块函数定义
void module_define_func(int index, Value func);

// 获取模块函数
Value module_get_func(int index);

// ============================================================================
// 调试功能（VM 独立运行时不需要，通过 LENO_VM_ONLY 条件编译排除）
// ============================================================================

#ifndef LENO_VM_ONLY
int disassembleInstruction(Chunk* chunk, int offset);
void disassembleChunk(Chunk* chunk, const char* name);
void debugPrintStack(Value* stack, int sp);
#endif // LENO_VM_ONLY

// ============================================================================
// 协程系统 API
// ============================================================================

// 获取当前正在执行的协程（NULL 表示主线程）
ObjCoroutine* vm_current_coroutine(void);

// 检查当前是否在 async 函数中
int vm_in_async_context(void);

// 挂起当前协程，等待 Future
void vm_suspend_coroutine(ObjFuture* future);

// 恢复协程执行
void vm_resume_coroutine(ObjCoroutine* co, Value result);

// 使用指定的 VM 执行协程（供子线程使用）
int vm_run_coroutine_with_vm(ObjCoroutine* co, VM* vm_ptr);

// 包装函数：主线程使用全局 vm
int vm_run_coroutine(ObjCoroutine* co);

#endif // LENO_VM_H
