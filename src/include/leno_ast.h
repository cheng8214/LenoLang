#ifndef LENO_AST_H
#define LENO_AST_H

#include "leno_types.h"
#include "leno_value.h"

// ============================================================================
// AST 节点类型
// ============================================================================

typedef enum {
    AST_NUM,
    AST_STRING,
    AST_BOOL,
    AST_NULL,
    AST_ARRAY,
    AST_DICT,
    AST_RANGE,         // 范围字面量：0:9
    AST_VAR,
    AST_BINOP,
    AST_UNARY,
    AST_CALL,
    AST_INDEX,
    AST_SLICE,         // 数组切片：arr[start:end]
    AST_INDEX_ASSIGN,  // 索引赋值：dict["key"] = value
    AST_BLOCK,
    AST_IF,
    AST_WHILE,
    AST_FOR,
    AST_SWITCH,
    AST_FUNC_DEF,
    AST_RETURN,
    AST_BREAK,
    AST_CONTINUE,
    AST_ASSIGN,
    AST_COMPOUND_ASSIGN,  // 复合赋值: += -= *= /=
    AST_VAR_DECL,
    AST_EXPR_STMT,
    AST_IMPORT,        // import 语句
    AST_EXPORT,        // export 语句
    AST_USE,           // use 语句：use module.Type
    AST_MODULE_CALL,   // 模块方法调用：io.print()
    AST_MODULE_ACCESS, // 模块成员访问：test.PI
    AST_INTERP_STRING, // 插值字符串: $("hello {name}")
    AST_TRY,           // try-catch-finally 语句
    AST_THROW,         // throw 语句
    AST_TYPE_CHECK,    // 类型检查表达式: a is int
    AST_AS_CAST,       // 安全类型转换: a as TypeName
    AST_ALIAS,         // 类型别名: alias Name = Type
    AST_STRUCT_DEF,    // struct 定义
    AST_FACE_DEF,      // face 定义
    AST_CSTRUCT_DEF,   // cstruct 定义（C 布局结构体）
    AST_CLIB_DEF,       // clib 定义（C 库函数签名声明）
    AST_CFUNC_DECL,     // cfunc 声明（C 回调函数签名）
    AST_ENUM_DEF,      // enum 定义
    AST_STRUCT_INIT,   // struct 构造函数调用
    AST_FIELD_ACCESS,  // 字段访问: obj.field
    AST_ADDRESS_OF,    // 取地址: &obj.field（仅 cstruct 字段）
    AST_AWAIT,         // await 表达式
    AST_SAFE_ACCESS,   // 安全访问: expr?.field / expr?.method()
} AstKind;

typedef struct Ast Ast;

// ============================================================================
// AST 列表（用于块、参数列表等）
// ============================================================================

typedef struct {
    Ast** items;
    int count;
    int capacity;
} AstList;

void ast_list_init(AstList* list);
void ast_list_add(AstList* list, Ast* ast);

// ============================================================================
// 符号引用信息（存储在AST节点中，避免悬空指针问题）
// ============================================================================

typedef struct {
    SymKind kind;
    int index;
    char* name;
    TypeKind type_kind;
    char* struct_name;
} SymRef;

// ============================================================================
// AST 节点结构
// ============================================================================

// 字典键值对结构
typedef struct {
    Ast* key;
    Ast* value;
} DictEntry;

typedef struct {
    DictEntry* entries;
    int count;
    int capacity;
} DictEntryList;

// 类型守卫条件结构（使用新的泛型类型系统）
typedef struct {
    char* var_name;        // 被检查的变量名
    char* field_name;      // 被检查的字段名（如 s.age is int 中的 "age"），NULL 表示简单变量守卫
    char* index_key;      // 被检查的索引键字符串（如 arr[0] is int 中的 "0"，d["name"] is int 中的 "name"），NULL 表示非索引守卫
    TypeInfo* guard_type;  // 检查的完整类型信息（支持 Array[int], Dict[string, int] 等）
} TypeGuardCond;

typedef struct {
    TypeGuardCond* items;
    int count;
    int capacity;
} TypeGuardList;

void type_guard_list_init(TypeGuardList* list);
void type_guard_list_add(TypeGuardList* list, TypeGuardCond cond);
void type_guard_list_free(TypeGuardList* list);

struct Ast {
    AstKind kind;
    int line;
    int column;             // 列号（1-based，用于错误报告，-1 表示未知）
    TypeInfo* cached_type;  // 类型推断缓存，避免重复推断同一表达式
    union {
        struct { double value; int is_bigint; char* bigint_str; int is_float; } num;
        struct { char* value; int len; } string;
        int boolean;
        AstList array;
        DictEntryList dict;
        struct { Ast* start; Ast* end; int inclusive; } range;  // 范围：0:9
        struct { char* name; SymRef ref; } var;
        struct { Ast* l; Ast* r; LenoTokenType op; } binop;
        struct { Ast* operand; LenoTokenType op; int is_postfix; } unary;
        struct { Ast* callee; AstList args; int is_tail_call;
                 char** generic_type_names; TypeInfo** generic_type_args; int generic_type_count;
                 int callee_is_async; } call;
        struct { Ast* obj; Ast* index; } index;
        struct { Ast* obj; Ast* start; Ast* end; } slice;  // 切片：arr[start:end]
        struct { Ast* obj; Ast* index; Ast* value; int field_index; } index_assign;
        AstList block;
        struct {
            Ast* cond;
            Ast* then;
            Ast* else_;
            // 类型守卫相关字段（使用新的泛型类型系统）
            char* guard_var;       // 被检查的变量名（如 "a"）
            TypeInfo* guard_type;  // 检查的完整类型信息（支持 Array[int] 等）
            SymRef guard_var_ref;  // 被检查变量的符号引用（用于代码生成）
            // 多条件类型守卫支持
            TypeGuardList guard_conds; // 所有类型守卫条件列表
            // 绑定变量（=> var）：当使用 if expr is Type => var 语法时，
            // expr 求值一次，类型匹配后赋值给新局部变量 var
            char* guard_bind_var;       // 绑定的变量名（NULL 表示无绑定）
            int guard_bind_index;       // 绑定变量的局部变量索引
            Ast* guard_bind_expr;      // 被绑定的表达式（codegen 使用）
        } if_;
        struct { Ast* cond; Ast* body; } while_;
        struct {
            Ast* start;      // 起始值（可为NULL，表示从0开始）
            Ast* end;        // 结束值
            Ast* step;       // 步进值（可为NULL，表示根据方向自动判断）
            int inclusive;   // 是否包含结束值（有:表示包含）
            char* var_name;  // 循环变量名（NULL表示无变量）
            char* index_var_name;  // 索引变量名（NULL表示无索引变量）
            Ast* body;
            int loop_var_index;  // 循环变量的局部变量索引
            int end_index;       // 结束值的局部变量索引
            int step_index;      // 步进值的局部变量索引
            int start_index;     // 起始值的局部变量索引（用于浮点数循环）
            int counter_index;   // 整数计数器的局部变量索引（用于浮点数循环）
            int index_var_index; // 索引变量的局部变量索引（用于迭代循环）
        } for_;
        struct { 
            Ast* expr;           // switch 表达式
            struct SwitchCase {
                AstList values;  // case 值列表（支持多个值）
                Ast* body;       // case 体
                int is_type_match;    // 1 = case is Type 模式
                TypeInfo* match_type; // 匹配的首类型（= match_types[0]，用于类型窄化和解构）
                TypeInfo** match_types;  // 匹配类型数组（逗号合并：case is A, B, C）
                int match_type_count;    // 匹配类型数量（1=单类型，>1=逗号合并）
                char* guard_var;      // switch 表达式变量名（用于类型收窄）
                SymRef guard_var_ref; // switch 表达式变量的符号引用
                char** destructure_vars;      // 解构变量名数组（如 case is Point(x, y) → ["x", "y"]）
                int destructure_count;        // 解构变量数量（0 = 无解构）
                int* destructure_indices;     // 解构变量的局部变量索引数组（语义分析时填充）
                char** destructure_field_names; // 对应的 struct 字段名数组（语义分析时填充，如 ["x", "y"]）
            }* cases;            // case 数组
            int case_count;      // case 数量
            Ast* default_body;   // default 体
        } switch_;
        struct {
            char* name;
            char** params;
            TypeInfo** param_types;    // 参数完整类型信息
            int pcnt;
            TypeInfo* return_type;     // 返回完整类型信息
            Ast* body;
            SymRef ref;
            int local_count;
            // 闭包信息
            char** upvalue_names;      // upvalue 名称数组
            int* upvalue_indices;      // upvalue 在外层函数的索引
            int* upvalue_is_local;     // upvalue 是否是外层函数的局部变量（1=局部，0=upvalue）
            int* upvalue_is_value_capture; // upvalue 是否使用值捕获（1=值捕获，0=引用捕获）
            int upvalue_count;         // upvalue 数量
            int is_in_loop;            // 函数是否在循环体内定义（1=是，0=否）
            // 默认参数信息
            Ast** param_defaults;      // 参数默认值表达式数组（NULL 表示没有默认值）
            int default_count;         // 有默认值的参数数量
            // 协程信息
            int is_async;              // 是否是 async 函数（1=是，0=否）
            int is_ctor;               // 是否是构造函数（func StructName()）
            int is_dtor;               // 是否是析构函数（func ~StructName()）
            // 泛型类型参数
            char** type_params;        // 类型参数名数组（如 ["T", "U"]）
            char** type_param_constraints; // 类型参数约束 face 名（如 ["Comparable", NULL]）
            char** type_param_defaults;    // 类型参数默认值（如 ["int", NULL]），NULL=无默认
            int type_param_count;      // 类型参数数量
        } func;
        Ast* ret;
        struct {
            char** names;         // 目标变量名数组（并行赋值）
            int name_count;       // 目标变量数量
            Ast** targets;        // 目标表达式数组（支持复杂赋值目标）
            Ast* value;           // 右侧表达式（可能是元组/列表）
            SymRef* refs;         // 符号引用数组
        } assign;
        struct { char* name; Ast* value; SymRef ref; LenoTokenType op; } compound_assign;  // 复合赋值
        struct { char* name; Ast* init; SymRef ref; TypeInfo* type; int is_const; } var_decl;
        struct { Ast* expr; } expr_stmt;
        struct { char* module_name; char* alias; char* file_path; } import;
        struct { Ast* decl; } export;
        struct { char* module_name; char* symbol_name; } use;
        struct { char* module_name; char* method_name; AstList args; SymRef lib_ref; TypeInfo** generic_type_args; int generic_type_count; char** generic_type_names; TypeInfo* clib_return_type; } module_call;
        struct { char* module_name; char* member_name; SymRef ref; } module_access;
        struct {
            char** parts;      // 字符串片段数组
            Ast** exprs;       // 表达式数组
            int count;         // 片段/表达式数量
        } interp_string;
        struct {
            Ast* try_body;     // try 代码块
            char* catch_var;   // catch 变量名（可为 NULL）
            Ast* catch_body;   // catch 代码块（可为 NULL）
            Ast* finally_body; // finally 代码块（可为 NULL）
            SymRef catch_var_ref; // catch 变量的引用信息
        } try_;
        struct {
            Ast* expr;         // throw 的表达式
        } throw_;
        struct {
            Ast* expr;         // 被检查的表达式
            TypeInfo* type;    // 检查的类型
        } type_check;
        struct {
            char* name;        // 别名名称
            TypeInfo* type;    // 别名指向的类型（类型别名时非NULL）
            Ast* expr;         // 值表达式（值别名时非NULL，如 alias X = Signal.mid）
            char** type_params;        // 泛型类型参数名（如 ["T"]），NULL 表示非泛型别名
            int type_param_count;      // 泛型类型参数数量
            SymRef ref;        // 符号引用信息（值别名时使用）
        } alias;
        struct {
            char* name;        // struct 名称
            char** field_names; // 字段名称数组
            TypeInfo** field_types; // 字段类型数组
            Ast** field_defaults;   // 字段默认值表达式数组（可为 NULL）
            int field_count;    // 字段数量
            Ast** methods;      // 方法定义数组（AST_FUNC_DEF）
            int method_count;   // 方法数量
            char** impl_names;  // impl 声明的 face 名称数组
            int impl_count;     // impl 声明数量
            TypeInfo*** impl_type_args;  // impl 声明的 face 泛型参数（如 impl Comparable[int] 中的 [int]）
            int* impl_type_arg_counts;   // 每个 impl 的泛型参数数量
            // 泛型类型参数
            char** type_params;        // 类型参数名数组（如 ["T", "U"]）
            char** type_param_constraints; // 类型参数约束 face 名
            char** type_param_defaults;    // 类型参数默认值（如 ["int", NULL]）
            int type_param_count;      // 类型参数数量
            // 关联常量（associated constants）
            char** const_names;       // 关联常量名数组
            Ast** const_values;       // 关联常量值表达式数组
            int const_count;          // 关联常量数量
        } struct_def;
        struct {
            char* name;              // face 名称
            char** method_names;     // 方法名称数组
            TypeInfo** method_return_types; // 方法返回类型数组
            TypeInfo*** method_param_types; // 方法参数类型数组
            int* method_param_counts;      // 方法参数数量数组
            int method_count;        // 方法数量
            char** type_params;      // 类型参数名数组（如 ["T"]）
            char** type_param_constraints; // 类型参数约束 face 名
            int type_param_count;    // 类型参数数量
        } face_def;
        struct {
            char* name;        // cstruct 名称
            char** field_names; // 字段名称数组
            TypeInfo** field_types; // 字段类型数组（必须是 C 布局类型）
            int field_count;    // 字段数量
            int total_size;     // 总大小（编译期计算）
            int alignment;      // 对齐要求（编译期计算）
            int* field_offsets; // 字段偏移量数组（编译期计算）
            int* field_array_dims; // 字段数组维度（0 表示非数组，>0 表示数组大小）
            bool is_packed;     // 是否 packed（取消所有字段间 padding）
            int  explicit_align; // 显式对齐要求（0 表示未指定，否则必须是 2 的幂）
            SymRef ref;        // 符号引用信息
        } cstruct_def;
        struct {
            char* name;        // clib 名称
            char** func_names; // 函数名称数组
            TypeInfo** func_return_types; // 函数返回类型数组
            TypeInfo*** func_param_types; // 函数参数类型数组（二维）
            int* func_param_counts;       // 函数参数数量数组
            int func_count;     // 函数数量
            SymRef ref;        // 符号引用信息
        } clib_def;
        struct {
            char* name;           // cfunc 名称
            TypeInfo** param_types; // 参数类型数组
            char** param_names;     // 参数名数组
            int param_count;        // 参数数量
            TypeInfo* return_type;  // 返回类型
            SymRef ref;           // 符号引用信息
        } cfunc_decl;
        struct {
            char* name;        // enum 名称
            char** member_names; // 成员名称数组
            int64_t* member_values;  // 成员值数组（显式指定或自动分配）
            int member_count;    // 成员数量
            SymRef ref;        // 符号引用信息（用于模块变量索引）
        } enum_def;
        struct {
            char* struct_name;  // struct 名称
            char** field_names; // 字段名称数组（命名参数）
            Ast** field_values; // 字段值表达式数组
            int field_count;    // 字段数量
            // 泛型类型参数（实例化时提供，如 new Box[int]{value: 42}）
            TypeInfo** generic_type_args;   // 具体类型参数数组（如 [TYPE_INT]）
            int generic_type_count;         // 类型参数数量
            int has_dtor;                   // 该 struct 是否有析构函数（语义分析阶段设置）
        } struct_init;
        struct {
            Ast* obj;          // 对象表达式
            char* field_name;  // 字段名称
            int field_index;   // 字段索引（编译期确定，-1 表示未确定）
        } field_access;
        struct {
            Ast* operand;      // 取地址的操作数（必须是 AST_FIELD_ACCESS）
        } address_of;
        struct {
            Ast* expr;         // await 的表达式
        } await;
        struct {
            Ast* obj;          // 对象表达式（?. 之前）
            char* name;        // 字段名或方法名（?. 之后）
            int is_call;       // 1=方法调用，0=字段访问
            AstList args;      // 方法参数（is_call=1时使用）
            int field_index;   // 字段索引（语义分析填充，-1=未确定）
            SymRef ref;        // 对象符号引用（语义分析填充）
            int callee_is_async; // 方法是否是 async
            TypeInfo** generic_type_args;
            int generic_type_count;
            char** generic_type_names;
        } safe_access;
    } u;
};

// AST API
Ast* ast_new(AstKind kind, int line);
void ast_free(Ast* ast);

#endif // LENO_AST_H
