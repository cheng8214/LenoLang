#ifndef LENO_TYPES_H
#define LENO_TYPES_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

// ============================================================================
// 基础常量定义
// ============================================================================

#define MAX_SYMBOLS 1024      // 最大符号数量（全局变量、函数、局部变量等）
#define MAX_STACK 4096        // 最大栈深度（值栈容量）
#define MAX_CONSTANTS 4096    // 最大常量数量（每个代码块）
#define MAX_UPVALUES 256     // 最大上值数量（闭包捕获变量）
#define MAX_ERRORS 256        // 最大错误记录数量
#define MAX_CODE_SIZE 65536   // 最大字节码大小（每个代码块）
#define MAX_FRAMES 512        // 最大调用帧深度（递归限制）

// ============================================================================
// 编译限制常量
// ============================================================================

#define MAX_ARITY 255         // 函数最大参数数量（字节码限制）
#define MAX_NAME_LEN 255      // 标识符最大长度

// ============================================================================
// 循环控制常量
// ============================================================================

#define MAX_BREAK_JUMPS 256   // 单个循环中最大 break 数量
#define MAX_CONTINUE_JUMPS 256 // 单个循环中最大 continue 数量

// ============================================================================
// 缓冲区大小常量
// ============================================================================

#define BUFFER_SMALL 128      // 小型缓冲区（文件名等）
#define BUFFER_MEDIUM 256     // 中型缓冲区（错误信息等）
#define BUFFER_LARGE 512      // 大型缓冲区（路径等）
#define BUFFER_XLARGE 1024    // 超大缓冲区（行读取等）
#define BUFFER_XXLARGE 4096   // 最大缓冲区（文件读取等）

// ============================================================================
// 路径相关常量
// ============================================================================

#ifdef PATH_MAX
#define MAX_PATH_LEN PATH_MAX
#else
#define MAX_PATH_LEN 4096
#endif

// ============================================================================
// 错误类型
// ============================================================================

typedef enum {
    ERR_NONE,
    ERR_SYNTAX,
    ERR_SEMANTIC,
    ERR_UNDEFINED_VAR,
    ERR_UNDEFINED_FUNC,
    ERR_DUPLICATE_VAR,
    ERR_TYPE_MISMATCH,
    ERR_CLOSURE,
    ERR_RUNTIME,
} ErrorType;

// ============================================================================
// 警告类型（"担心系统"）
// ============================================================================

typedef enum {
    SEV_ERROR,     // 错误：阻断编译/运行
    SEV_WARNING,   // 警告：默认开启，仅提示不阻断
    SEV_HINT,      // 提示（预留）
} Severity;

typedef enum {
    WARN_NONE,
    WARN_FOR_EMPTY_RANGE,  // for A:B 无显式步长且 A>B → 空循环（如 for 5:0）
    WARN_UNUSED_VAR,       // 变量定义未使用
    WARN_SHADOW_VAR,       // 变量遮蔽
    WARN_DEPRECATED,       // 弃用语法/API
    WARN_IMPLICIT_TRUNC,   // 浮点隐式截断为整数
    WARN_UNREACHABLE,      // 不可达代码
    WARN_NULL_FIELD_CHAIN, // null 字段链式访问
    WARN_STRUCT_EQ_NULL,   // struct 值类型与 null 比较（恒为 false/true）
    WARN_OR_TYPE_GUARD,    // or 条件中的类型守卫不会收窄
    WARN_GENERIC_NO_CONSTRAINT, // 泛型参数参与运算但无约束
    WARN_NULLABLE_ARITH,    // nullable 值类型参与算术运算（可能为 null）
} WarnType;

// ============================================================================
// 词法分析 Token 类型
// ============================================================================

typedef enum {
    TOK_EOF,
    TOK_IDENT,
    TOK_NUM,
    TOK_STRING,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_MOD,  // + - * / %
    TOK_INC, TOK_DEC,  // ++ 和 --
    TOK_PLUSEQ, TOK_MINUSEQ, TOK_STAREQ, TOK_SLASHEQ, TOK_MODEQ,  // += -= *= /= %=
    TOK_EQ, TOK_EQEQ, TOK_NEQ, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_BITAND, TOK_BITOR, TOK_BITXOR, TOK_BITNOT,  // & | ^ ~
    TOK_BITANDEQ, TOK_BITOREQ, TOK_BITXOREQ,  // &= |= ^=
    TOK_SHL, TOK_SHR,  // << >>
    TOK_USHR,  // >>> (逻辑右移/无符号右移)
    TOK_SHLEQ, TOK_SHREQ,  // <<= >>=
    TOK_USHREQ,  // >>>= (逻辑右移复合赋值)
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE,
    TOK_LBRACKET, TOK_RBRACKET,
    TOK_COMMA, TOK_SEMI, TOK_COLON, TOK_DOT,
    TOK_IF, TOK_ELSE, TOK_EIF, TOK_THEN, TOK_FUNC, TOK_RETURN, TOK_WHILE, TOK_FOR, TOK_TO, TOK_BREAK, TOK_CONTINUE,
    TOK_SWITCH, TOK_CASE, TOK_DEFAULT,
    TOK_VAR, TOK_CONST, TOK_TRUE, TOK_FALSE, TOK_NULL,
    TOK_AND, TOK_OR, TOK_NOT, TOK_IS, TOK_IN, TOK_NOT_IN, TOK_QUESTION, TOK_QUESTION_DOT, TOK_NULL_COALESCE, TOK_FAT_ARROW, TOK_IMPORT, TOK_EXPORT, TOK_AS, TOK_USE,
    TOK_TRY, TOK_CATCH, TOK_THROW, TOK_FINALLY,
    TOK_STRUCT,        // struct 关键字
    TOK_ENUM,          // enum 关键字
    TOK_FACE,          // face 关键字
    TOK_IMPL,          // impl 关键字
    TOK_NEW,           // new 关键字（struct 实例化）
    TOK_ALIAS,         // alias 关键字（类型别名）
    // 类型关键字
    TOK_INT_TYPE, TOK_FLOAT_TYPE, TOK_STRING_TYPE, TOK_BOOL_TYPE, TOK_ARRAY_TYPE, TOK_DICT_TYPE, TOK_ANY_TYPE,
    // TOK_BINT 已移除（int 统一对外，Bint 仅作为内部 TYPE_BIGINT 存在）
    TOK_FILE_TYPE, TOK_PTR_TYPE,
    TOK_SOCKET_TYPE,     // Socket 类型
    TOK_CHANNEL_TYPE,    // Channel 类型
    TOK_THREAD_TYPE,     // Thread 类型
    TOK_FUTURE_TYPE,     // Future 类型
    // C 布局类型关键字
    TOK_I8, TOK_U8,           // i8, u8
    TOK_I16, TOK_U16,         // i16, u16
    TOK_I32, TOK_U32,         // i32, u32
    TOK_I64, TOK_U64,         // i64, u64
    TOK_F32, TOK_F64,         // f32, f64
    TOK_C_INT, TOK_C_UINT,    // c_int, c_uint
    TOK_C_LONG, TOK_C_ULONG,  // c_long, c_ulong
    TOK_C_LONGLONG, TOK_C_ULONGLONG,  // c_longlong, c_ulonglong
    TOK_C_SIZE, TOK_C_SSIZE,  // c_size, c_ssize
    // C struct 关键字
    TOK_CSTRUCT,         // cstruct
    TOK_CLIB,            // clib - C 库函数签名声明
    TOK_CFUNC,           // cfunc - C 回调函数签名声明
    // 字符串类型关键字
    TOK_STR8,           // str8 - C char* 字符串指针
    TOK_STR16,          // str16 - UTF-16 字符串数组
    // cstruct 布局属性（保留枚举值，不再作为关键字使用）
    // packed 和 align 现在是"上下文关键字"——在 parser 中通过文本匹配识别
    TOK_PACKED,         // packed - 取消 cstruct 字段间 padding（保留，未使用）
    TOK_ALIGN,          // align(N) - 指定 cstruct 整体对齐边界（保留，未使用）
    // 字符串插值 - 简化设计
    TOK_INTERP_STRING,   // $" 开始
    TOK_INTERP_PART,     // 字符串片段
    TOK_INTERP_END,      // " 结束
    // 原始字符串字面量
    TOK_RAW_STRING,      // @" 开始
    // 协程关键字
    TOK_ASYNC,           // async 关键字
    TOK_AWAIT,           // await 关键字
    // 错误 token
    TOK_ERROR,           // 词法错误，用于错误恢复
} LenoTokenType;

// ============================================================================
// 类型系统
// ============================================================================

typedef enum {
    TYPE_UNKNOWN,   // 未知/未指定类型
    TYPE_INFER,     // 类型推断
    TYPE_INT,       // 整数
    TYPE_FLOAT,     // 浮点数
    TYPE_STRING,    // 字符串
    TYPE_BOOL,      // 布尔值
    TYPE_ARRAY,     // 数组
    TYPE_DICT,      // 字典
    TYPE_BIGINT,    // 大整数
    TYPE_NULL,      // null 类型
    TYPE_FILE,      // 文件类型
    TYPE_ANY,       // 任意类型
    TYPE_FUNCTION,  // 函数类型
    TYPE_STRUCT,    // 结构体类型
    TYPE_FACE,      // 接口类型
    TYPE_ENUM,      // 枚举类型
    TYPE_PTR,       // FFI 指针类型
    // C 布局类型
    TYPE_I8, TYPE_U8,           // i8, u8
    TYPE_I16, TYPE_U16,         // i16, u16
    TYPE_I32, TYPE_U32,         // i32, u32
    TYPE_I64, TYPE_U64,         // i64, u64
    TYPE_F32, TYPE_F64,         // f32, f64
    TYPE_C_INT, TYPE_C_UINT,    // c_int, c_uint
    TYPE_C_LONG, TYPE_C_ULONG,  // c_long, c_ulong
    TYPE_C_LONGLONG, TYPE_C_ULONGLONG,  // c_longlong, c_ulonglong
    TYPE_C_SIZE, TYPE_C_SSIZE,  // c_size, c_ssize
    TYPE_CSTRUCT,               // cstruct
    TYPE_CLIB,                  // clib - C 库函数签名类型
    TYPE_CFUNC,                 // cfunc - C 回调函数签名类型
    TYPE_STR8,                 // str8 - C char* 字符串指针
    TYPE_STR16,                 // str16 - UTF-16 字符串数组
    TYPE_PTR_GENERIC,           // 泛型指针 Ptr[T]
    TYPE_THREAD,                // 线程类型
    TYPE_CHANNEL,               // Channel 类型
    TYPE_FUTURE,                // Future 类型（异步结果）
    TYPE_SOCKET,                // Socket 类型
    TYPE_GENERIC_PARAM,         // 泛型类型参数（如 T、U）
    TYPE_MULTI_RET,             // 多返回值类型 [T1, T2, ...] 或 {"k": T1, ...}
    TYPE_KIND_COUNT,            // TypeKind 枚举数量（用于数组边界检查）
} TypeKind;

typedef struct TypeInfo TypeInfo;

struct TypeInfo {
    TypeKind kind;
    TypeInfo* element_type;  // 数组元素类型
    TypeInfo* key_type;      // 字典键类型
    TypeInfo* value_type;    // 字典值类型
    // 函数类型相关
    TypeInfo* return_type;   // 函数返回类型
    TypeInfo** param_types;  // 函数参数类型数组
    int param_count;         // 函数参数数量
    // 结构体类型相关
    char* struct_name;       // 结构体名称 / face 名称 / cstruct/clib/cfunc 名称
    // 泛型类型参数
    char* type_param_name;   // 泛型类型参数名（如 "T", "U"）
    char* constraint_name;   // 泛型约束 face 名（如 "Comparable"），NULL 表示无约束
    // 泛型 struct 实例化时的具体类型参数（如 Box[int] 的 int）
    TypeInfo** generic_args; // 具体类型参数数组
    int generic_count;       // 类型参数数量
    int nullable;            // 可空类型标记：1=Type?，0=Type
    // 位置信息（用于错误报告）
    int line;                // 类型在源代码中的行号（1-based），0 表示未知
    int column;               // 类型在源代码中的列号（1-based），0 表示未知
    // 驻留标记：1=运行时共享的驻留类型（如 Array[int]），不可被 type_free 释放
    int interned;
};

// 类型系统 API
TypeInfo* type_new(TypeKind kind);
// 获取驻留的数组类型 Array[elem_kind]（每个线程缓存一份，重复调用返回同一实例）
TypeInfo* type_get_array_cached(TypeKind elem_kind);
TypeInfo* type_array(TypeInfo* element_type);
TypeInfo* type_dict(TypeInfo* key_type, TypeInfo* value_type);
TypeInfo* type_ptr_generic(TypeInfo* element_type);
TypeInfo* type_function(TypeInfo* return_type, TypeInfo** param_types, int param_count);
TypeInfo* type_generic_param(const char* name);  // 创建泛型类型参数 T, U 等
TypeInfo* type_generic_param_constrained(const char* name, const char* constraint);  // 创建带约束的泛型类型参数
TypeInfo* type_multi_ret(TypeInfo** ret_types, int count);  // 创建多返回值类型 [T1, T2, ...]
int type_has_generic(TypeInfo* type);             // 类型是否包含泛型参数
int type_has_infer_as_param(TypeInfo* type, TypeKind* out_parent_kind);  // 类型是否将 var 用作了类型参数
TypeInfo* type_substitute(TypeInfo* type, const char* param_name, TypeInfo* concrete);  // 类型替换
void type_free(TypeInfo* type);
int type_equals(TypeInfo* a, TypeInfo* b);
TypeInfo* type_copy(TypeInfo* type);
const char* type_to_string(TypeInfo* type);
const char* type_kind_to_string(TypeKind kind);
int type_is_compatible(TypeInfo* target, TypeInfo* source);
TypeKind token_to_type_kind(LenoTokenType token);
int type_is_nullable(TypeInfo* type);

// C 布局类型 API
int c_layout_type_size(TypeKind kind);
int c_layout_type_align(TypeKind kind);
int c_layout_align_up(int offset, int alignment);
int c_layout_is_valid_field_type(TypeKind kind);
int is_c_layout_type(TypeKind kind);
TypeKind c_layout_type_to_leno(TypeKind kind);

// ============================================================================
// 符号类型
// ============================================================================

typedef enum {
    SYM_GLOBAL,      // 全局变量
    SYM_GLOBAL_FUNC, // 全局函数
    SYM_LOCAL,
    SYM_PARAM,
    SYM_UPVALUE,
    SYM_NATIVE,      // native 函数
    SYM_MODULE,      // 模块级别的变量或函数
    SYM_TYPE,        // 类型定义（enum、struct），不占用运行时索引
    SYM_STRUCT,      // struct 类型定义
    SYM_CSTRUCT,     // cstruct 类型定义
    SYM_CLIB,        // clib 类型定义
    SYM_CFUNC,       // cfunc 回调签名定义
    SYM_ENUM,        // enum 类型定义
    SYM_FUNC_ALIAS,  // use 导入的模块函数别名
} SymKind;

#endif // LENO_TYPES_H
