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
    WARN_BAD_ESCAPE,       // 无效转义序列（如 \q，按原样保留）
    WARN_IMPLICIT_TRUNC,   // 浮点隐式截断为整数
    WARN_UNREACHABLE,      // 不可达代码
    WARN_NULL_FIELD_CHAIN, // null 字段链式访问
    WARN_STRUCT_EQ_NULL,   // struct 值类型与 null 比较（恒为 false/true）
    WARN_OR_TYPE_GUARD,    // or 条件中的类型守卫不会收窄
    WARN_GENERIC_NO_CONSTRAINT, // 泛型参数参与运算但无约束
    WARN_NULLABLE_ARITH,    // nullable 值类型参与算术运算（可能为 null）
    WARN_EMPTY_SOURCE,     // 源文件为空（或只有空白）：会"编译成功"却不做任何事
    WARN_ASSIGN_IN_COND,   // if/while 条件位置出现赋值（可能漏写 ==）
    WARN_FOR_IN_COND,      // for 头写 'x in arr'（in 是成员测试，遍历应写 'for x to arr'）
    WARN_PARTIAL_DECL_INIT, // var 声明列表只有部分变量带初值（Python 多重赋值习惯）
    WARN_FIELD_NO_INIT,     // struct 标量字段未显式初始化（默认是 null 而不是 0，参与运算会报错）
    WARN_AMBIGUOUS_MODULE,  // 裸文件名 import 撞名：多个包目录都有同名 .leno 且内容不同（静默选错文件）
    WARN_NATIVE_TYPE_NAME_CLASH, // 脚本 struct 与已注册的 native 类型同名（字段解析会优先用脚本定义）
    WARN_MISSING_RETURN,     // 声明了返回值的函数存在没有 return 的路径（调用点会静默拿到 null）
    WARN_SYMTAB_INCOMPLETE, // 符号表扫描与源码不一致（诊断 ✓）
                            //   ★ 2026-09-30：这条校验原本用 **error_add** ✗ ⇒ 扫描器一旦被
                            //   畸形输入带偏（实测：struct 收到 0 个方法 ✓），**整个构建**就死在
                            //   这句"请报告编译器 bug"上 ✗，而真正的症状在远处的调用点（
                            //   "类型 'struct Button' 没有方法 'set_enabled'" ✗）⇒ 极难定位 ✓
                            //   ⇒ 降级为**警告**：不中断编译 ✓，消息里直接写清"先删 .lenocache
                            //   重编、仍复现才报告" ✓（诊断该是提示，不该是砖 ✗）
    WARN_FIELD_SHADOW_ASSIGN, // 赋值落在"遮蔽了同名字段"的形参/局部变量上（含自赋值 X = X）
                              //   ★ 2026-09-30：struct 方法里字段与形参/局部同名时，裸名一律
                              //   解析为形参/局部（见 transform_method_body 的遮蔽集合）⇒
                              //   `root = root` 这种写法**静默失效**、字段纹丝不动 ✗。实测踩到：
                              //   pri 去下划线重构把 16 处 `_字段 = 形参` 批量改成 `字段 = 形参`
                              //   ⇒ Window.root 永远是 null ⇒ 整个界面空白，而编译期一声不吭 ✗
                              //   （966/966 "编译通过"完全测不出运行时语义 ✗）⇒ 补编译期诊断 ✓
                              //   正确写法：改字段写 `self.字段 = ...`（读字段写 `self.字段` ✓）
    WARN_SELF_FORWARD,        // 裸调用与所在 struct 方法同名、且实参就是它自己的形参 ⇒ 自己调自己
                              //   ★ 2026-09-30：struct 方法体内的裸调用会被优先解析成"调本 struct
                              //   的同名方法"（见 transform_method_body 的方法名改写）⇒ 一旦方法名
                              //   与**所在文件的模块级函数**撞名，"转发给模块函数"的写法就变成自递归：
                              //   实测 sdl_calendar 的 `func _dow(...) { return dow(y, m, d) }` 在
                              //   去下划线改名成 `dow` 后 ⇒ 数据看板/dashboard 直接「调用栈溢出」，
                              //   编译期一声不吭 ✗（又是"编译通过、运行才炸" ✓）
                              //   正确写法：给方法改名（如 `dowOf`）或直接调用模块级函数 ✓
    WARN_METHOD_NAME_AMBIGUOUS, // 裸调用与**所在方法同名**，而该名字还能按裸名解析到形参个数相同的
                              //   全局/模块函数 ⇒ 两种解释都成立，实际走方法（方法优先）✗
                              //   ★ 2026-09-30：就是这个形状让去下划线重构把"转发给模块函数"变成了
                              //   自递归（`_dow` 改名 `dow` 后 `return dow(y,m,d)` 调自己 ⇒ dashboard 栈溢出）
                              //   ⇒ 补这条提示 ✓。只收"与所在方法同名"（调兄弟方法时作者本意几乎必是
                              //   那个兄弟方法 ⇒ 报了是噪音 ✗）；只收"形参个数相同"（个数不同时编译器
                              //   会当场报参数不足/过多，**响的** ✓ 实测）⇒ 少报不误报 ✓
                              //   正确写法：要调方法写 `self.NAME(...)`；要调那个函数请改名区分 ✓
    WARN_PRINTF_NO_FORMAT,    // `printf` 名字骗人：只做"**不换行打印**"、**不做 % 格式化**（2026-10-10 加）
                              //   ★ 实测（速查 §三 ㉖）：`printf("%d = %s\n", 3, "x")` **不报错** ✗、
                              //     `%d`/`%s` 原样打出来、后面实参按空格拼在末尾 ✓ —— 从 C/Python
                              //     过来**第一眼必踩** ✓，而编译器一声不吭 ✗ ⇒ 补这条 ✓
                              //   只吃一种形状：**首参是含 '%' 的字符串字面量、且实参 > 1** ✓
                              //     （变量里装的格式串无从判断 ⇒ 不猜 ✓ 少报不误报 ✓）
    WARN_NOT_PRECEDENCE,      // `not x is T` / `not x in arr` ⇒ **`not` 先算**（结果常反 ✗，2026-10-10 加）
                              //   ★ 实测：x = 42 时 `not x is string` = **false** ✗
                              //     而 `not (x is string)` = **true** ✓ —— 结论**正好相反** ✓
                              //     同病 `not x in arr` ⇒ `(not x) in arr` ✗（几乎恒 false ✗）
                              //   ★ 为什么值得单独一条：从 Python 过来**必踩** ✓（那边 `not`
                              //     比 `is` / `in` **低**优先级 ⇒ `not x is T` 就是 `not (x is T)` ✓）
                              //   ★ 只查这两处 ✗（`is` 与 `in` / `not in`）：`not a == b` 那种
                              //     偶尔是**有意**写法 ⇒ 不碰 ✓ 少报不误报 ✓
    // 注：原 `WARN_EMPTY_CATCH`（空 catch）与 `WARN_IMPOSSIBLE_CAST`（不可能收窄）已于
    //     2026-09-28 **升为错误** ✗（改用 `error_add_at(ERR_SEMANTIC, ...)`，消息自带
    //     `[类别名]` 前缀 ✓）⇒ 不再需要这两个枚举值 ✓
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
    TOK_PRI,           // pri 关键字（struct 成员私有：默认全公有，只有标了 pri 的才拦 ✓）
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

// ============================================================================
// native 类型规格（NativeTypeSpec / NativeStructSpec）—— native 模块声明类型的**唯一来源**
// ----------------------------------------------------------------------------
// 为什么要有：native 方法元数据（ModuleMethodMeta）过去只有"**一个** TypeKind 槽"
//   （return_type + return_element_type），表达不了 `Array[DirEntry]` / `Dict[string,string]`
//   这类**参数化或带名字**的类型 ⇒ 编译器只能拿到"无名 struct / 裸容器"，字段和泛型实参全丢。
//   模块侧写**一份** static 规格，编译期由 native_type_spec_to_info() 转 TypeInfo、
//   运行期由 native_struct_def_for() 按**同一份规格、同一字段顺序**造 ObjStructDef
//   ⇒ 编译期字段索引与运行期槽位不可能各自漂（历史上就吃过"两边各写一份"的亏）。
// 用法（模块的 xxx_init_module 里各写一次即可）：
//     static const NativeTypeSpec S_STRING   = { NTYPE_STRING, NULL, NULL, NULL, 0, -1 };
//     static const NativeTypeSpec S_STRARR[] = { {NTYPE_ARRAY, NULL, &S_STRING, NULL, 0, -1} };  // 元素
//     static const char* DIRENTRY_FIELDS[] = {"root", "dirs", "files"};
//     static const NativeTypeSpec* DIRENTRY_TYPES[] = { &S_STRING, &S_STRARR[0], &S_STRARR[0] };
//     static const NativeStructSpec DIRENTRY = { "dirs", "DirEntry", 3, DIRENTRY_FIELDS, DIRENTRY_TYPES };
//     static const NativeTypeSpec S_DIRENTRY  = { NTYPE_STRUCT, "DirEntry", NULL, NULL, 0, -1 };
//     static const NativeTypeSpec S_DIRENTRY_ARR[] = { {NTYPE_ARRAY, NULL, &S_DIRENTRY, NULL, 0, -1} };
//     native_register_struct_spec(&DIRENTRY);
//     native_register_module_method("dirs", "walk_entries", fn, &S_DIRENTRY_ARR[0], params);
// ⚠ 被引用的 struct 规格**必须也注册**（native_register_struct_spec），否则编译期字段解析
//   会找不到字段表（运行期同样造不出定义）。
// ⚠ native struct 与脚本 struct 共用**同一个全局名字空间**（按名字索引）⇒ 取名请避免与
//   用户类型撞（如 `DirEntry` 这种模块专属名）。2026-09-27 起两条兜底：
//   ① `use <module>.<Type>` 可显式导入（显式、作用域内、可发现 —— 见 semantic 的 AST_USE）；
//   ② 脚本 struct 与已注册的 native 类型**同名**时给编译期警告（WARN_NATIVE_TYPE_NAME_CLASH），
//      提示"字段解析会优先用脚本定义、而 native 值按 native 规格解释"。
// ============================================================================
typedef enum {
    NTYPE_ANY = 0,      // 无约束（等价 TypeInfo 的 TYPE_ANY）
    NTYPE_INT,
    NTYPE_FLOAT,
    NTYPE_STRING,
    NTYPE_BOOL,
    NTYPE_NULL,
    NTYPE_ARRAY,        // sub  = 元素类型（**sub == NULL = "元素类型未指定"**，与老的
                        //        `TYPE_ARRAY + TYPE_UNKNOWN` 逐字等价 ⇒ 编译器拿到裸 Array）
    NTYPE_DICT,         // sub  = 键 K；sub2 = 值 V（两者皆 NULL = 裸 Dict，同老口径）
    NTYPE_STRUCT,       // name = 结构体名（字段表见同名的 NativeStructSpec）
    NTYPE_PTR_GENERIC,  // sub  = 元素类型（Ptr[T]）
    // ---- 以下为"补齐 237 个调用点"时新增（**追加在末尾**：既有数值不变，避免影响任何
    //      按数值使用的旧二进制）。它们只用于 native 返回类型的顶层声明。 ----
    NTYPE_PTR,          // 裸指针 Ptr（无元素类型）
    NTYPE_FILE,
    NTYPE_SOCKET,
    NTYPE_CHANNEL,
    NTYPE_THREAD,
    NTYPE_FUTURE,
    // ---- "引用第 0 个实参"的**关系型**标签（2026-09-27）----
    //   为什么需要：容器方法常返回"入参里那个容器的元素/键/值" —— `arrays.copy(xs)` 真实类型是
    //   `Array[T]`、`d.keys()` 是 `Array[K]`。写成 `Array[any]` 会把元素类型丢掉；写成具体类型
    //   （如 `Array[int]`）更是错的（会拒绝 `Array[DirEntry]`）。这里把"关系"表达出来，由语义侧
    //   在**调用点**用实参的实际类型替换（见 native_type_spec_to_info_with_args）。
    //   ⚠ 只支持"第 0 个实参"（模块形式的首个实参 / 实例形式的**接收者**）—— 现有两个真实用户
    //     （arrays / dicts）都只需它；将来要"第 N 个实参"再补 index 字段（YAGNI）。
    //   ⚠ 实参缺失或类型不明（裸 `Array` / 裸 `Dict`）⇒ 解析成 TYPE_ANY（自然退化到旧行为）。
    //   ⚠ 只影响**编译期推断**：规格不进字节码 ⇒ 不需要升 LENO_BIN_VERSION。
    NTYPE_ARG0_ELEM,    // 第 0 个实参的元素类型（Array[T] 的 T）
    NTYPE_ARG0_KEY,     // 第 0 个实参的键类型（Dict[K,V] 的 K）
    NTYPE_ARG0_VALUE,   // 第 0 个实参的值类型（Dict[K,V] 的 V）
    // ---- "回调返回类型"标签（2026-09-28，⓪ 族声明化）----
    //   含义：第 arg_index 个实参是**回调** ⇒ 推它的返回类型（内联闭包推函数体 / 命名函数取
    //   声明返回类型）。此前的 map/reduce/threads.start 各自在语义层硬编码 if 链（三份手写
    //   拷贝、精度互不一致）⇒ 收敛后注册即数据，语义侧只有一个通用推断点。
    //   下标约定与 ARG0_* 一致：实例形态接收者 = 0、实参顺延；模块形态 0 = 首实参。
    //   ⚠ 结构解析器（native.c）**解析不了**它 —— 需要语义侧先推好 T，再经
    //     native_type_spec_to_info_with_cb() 代入；推不出 ⇒ 该节点给 NULL（顶层自然回落
    //     Kind 槽旧行为，宁漏勿误报 ✓）。
    //   acc_index >= 0 ⇒ **reduce 语义**：守卫时第 0 参 = 第 acc_index 个实参的类型（累加器）、
    //     第 1 参 = 元素；< 0 ⇒ map 语义（第 0 参 = arg0 的元素）。
    NTYPE_ARG_CB_RET,   // 第 arg_index 个实参（回调）的返回类型
    // ---- "按实参个数选形态"标签（2026-09-28）----
    //   双形态内置函数（同名两种用法、返回类型不同，如 `_env`：1 参读 ⇒ string、
    //   2 参写 ⇒ bool）单一静态类型必谎报其一，Kind 槽也表达不了 arity 分支 ⇒
    //   规格层声明：实参数 < arg_index 取 sub 形态，否则取 sub2 形态。
    //   ⚠ 结构解析器（native.c 的 native_type_spec_to_info*）**解析不了**它 ——
    //     需要调用点实参个数 ⇒ 语义侧先经 native_type_spec_resolve_for_call()
    //     定型成具体子规格，再走正常解析。
    NTYPE_BY_ARITY,     // 按 argc 分支：argc < arg_index ⇒ sub，否则 ⇒ sub2
} NativeTypeTag;

typedef struct NativeTypeSpec {
    NativeTypeTag tag;
    const char* name;                  // NTYPE_STRUCT 的类型名；其余传 NULL
    const struct NativeTypeSpec* sub;  // 容器元素 / Dict 的 K / Ptr 的元素
    const struct NativeTypeSpec* sub2; // Dict 的 V（其余传 NULL）
    // ---- 关系型下标（仅 NTYPE_ARG_CB_RET 用；其余传 0 / -1 即可）----
    int arg_index;                     // 回调所在的实参下标（接收者 = 0，见上）
    int acc_index;                     // reduce 语义的累加器实参下标；非 reduce 传 -1
} NativeTypeSpec;

typedef struct NativeStructSpec {
    // **拥有 / 导出**这个类型的 native 模块名（`use <module>.<Type>` 的左侧）。
    //   2026-09-27 起必须有：`use dirs.DirEntry` 通道与"脚本 struct 撞名"的诊断都靠它定位来源。
    //   （只做纯名字查表的老接口 native_find_struct_spec 不受影响。）
    const char* module_name;
    const char* name;                          // 类型名（Leno 侧写这个）
    int field_count;
    const char* const* field_names;            // 与 field_types **同序** ⇒ 下标即字段号
    const NativeTypeSpec* const* field_types;
} NativeStructSpec;

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

// ========== native 注册的**参数规格**（2026-10-02；模块式与实例式共用）==========
//   为什么放这个底层头：`leno_value.h`（各类型的包装声明）与 `method_table.h` 都要用到它，
//   放这里两边都能拿到，也不会造成头文件互相包含 ✓
//   ── 参数个数标记（**不要再写裸 -1**）──
//   `-1` 过去同时承担两种意思（"这个方法可变参数" / "个数不限"）⇒ 一个数字两种含义，读代码只能猜 ✗
#define NATIVE_ARITY_VARARG (-200)  // arity 位：可变参数（个数范围看 min_arity..max_arity）
#define NATIVE_ARITY_ANY    (-1)    // min/max 位：不限（只有 max_arity 用得到）

typedef struct {
    int arity;                // 定长个数；或 NATIVE_ARITY_VARARG
    int min_arity;            // 仅可变：最少实参（定长时无意义）
    int max_arity;            // 仅可变：最多实参（NATIVE_ARITY_ANY = 不限）
    int declared_count;       // declared 的有效长度（0 = 不声明任何类型）
    const TypeKind* declared; // 前 declared_count 个实参的类型
    TypeKind tail_type;       // 其余实参的类型（TYPE_ANY = 不查）
} NativeParamSpec;

// 三个构造宏：同一形状在全仓库**写法完全一致**，且 count 由 sizeof 推出 ⇒ 不可能与个数打架 ✓
//   ⚠ NATIVE_FIXED(types) 的 types 必须是**真正的数组**（不能是数组指针，否则 sizeof 退化）
#define NATIVE_FIXED(types) \
    (NativeParamSpec){ (int)(sizeof(types) / sizeof(TypeKind)), NATIVE_ARITY_ANY, NATIVE_ARITY_ANY, \
                       (int)(sizeof(types) / sizeof(TypeKind)), (types), TYPE_ANY }
#define NATIVE_FIXED_NONE(n) \
    (NativeParamSpec){ (n), NATIVE_ARITY_ANY, NATIVE_ARITY_ANY, 0, NULL, TYPE_ANY }
#define NATIVE_VARARG(min, max, cnt, types, tail) \
    (NativeParamSpec){ NATIVE_ARITY_VARARG, (min), (max), (cnt), (types), (tail) }

#endif // LENO_TYPES_H
