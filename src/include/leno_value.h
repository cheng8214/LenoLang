#ifndef LENO_VALUE_H
#define LENO_VALUE_H

#include <stdio.h>
#include "leno_types.h"
#include "platform_thread.h"

// 线程局部存储定义（需要在 GC 声明之前）
#ifndef THREAD_LOCAL
#define THREAD_LOCAL __thread
#endif

// ============================================================================
// GC 对象系统
// ============================================================================

// 对象类型枚举
typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_DICT,
    OBJ_RANGE,      // 范围对象 (start:end)
    OBJ_CLOSURE,
    OBJ_FUNCTION,
    OBJ_NATIVE,
    OBJ_BIGINT,     // 大整数对象
    OBJ_UPVALUE,    // Upvalue 对象
    OBJ_MODULE,     // 模块对象
    OBJ_BOUND_METHOD, // 绑定方法对象
    OBJ_FILE,       // 文件对象
    OBJ_STRUCT_DEF,  // 结构体定义
    OBJ_STRUCT,      // 结构体实例
    OBJ_FACE_DEF,    // face 定义
    OBJ_ENUM_DEF,    // enum 定义
    OBJ_CSTRUCT_DEF,       // C 布局结构体定义
    OBJ_CSTRUCT,           // C 布局结构体实例
    OBJ_CSTRUCT_ARRAY_VIEW, // C 布局结构体数组字段视图
    OBJ_CSTRUCT_ARRAY,     // C 布局结构体数组（批量操作）
    OBJ_COROUTINE,         // 协程对象
    OBJ_FUTURE,     // Future 对象
    OBJ_FFI_LIBRARY, // FFI 库对象
    OBJ_FFI_POINTER, // FFI 指针对象
    OBJ_FFI_CALLBACK, // FFI 回调对象
    OBJ_THREAD,     // 线程对象
    OBJ_CHANNEL,    // Channel 对象
    OBJ_SOCKET,       // Socket 对象
    OBJ_NONE,       // 无效/空类型标记
    OBJ_INT,        // int 类型标记（内联缓存用）
    OBJ_FLOAT,      // float 类型标记（内联缓存用）
} ObjType;

// 对象标志位
#define OBJ_FLAG_INTERNED 0x01
#define GEN_YOUNG 0
#define GEN_OLD   1

typedef struct Object {
    ObjType type;
    struct Object* next;
    int marked;
    uint8_t flags;
    uint8_t generation;
    uint8_t survived;
    size_t size;
} Object;

// 前向声明
struct VM;
struct Chunk;

// FFI 指针对象（提前定义，供 val_is_truthy 使用）
typedef struct ObjFFIPointer {
    Object header;
    void* ptr;                         // 原始指针
    size_t size;                       // 指向的内存大小（字节），0 表示未知
    int owned;                         // 是否由我们分配（1=是，0=否）
    int freed;                         // 是否已释放（防止 double-free 和 use-after-free）
    TypeKind element_type;             // Ptr[T] 中 T 的类型，TYPE_PTR 表示未指定（通用 Ptr）
} ObjFFIPointer;

// ============================================================================
// Value 系统（NaN-tagging 优化：Value = 8 字节）
// ============================================================================
//
// 位编码方案（64 位）：
//   浮点数：直接存储原始 IEEE 754 double
//   标签值：利用 NaN 空间编码类型和载荷
//     Bit 63:     1（符号位，所有标签值都置 1）
//     Bits 62-52: 0x7FF（NaN 指数，全 1）
//     Bit 51:     1（安静 NaN 位）
//     Bits 50-48: 类型标签（3 位）
//     Bits 47-0:  载荷（48 位）
//
// 类型标签：
//   000: NULL   - 载荷 = 0
//   001: FALSE  - 载荷 = 0
//   010: TRUE   - 载荷 = 0
//   011: INT    - 载荷 = 有符号 48 位整数（bits 47-0），溢出回绕
//   100: OBJ    - 载荷 = 48 位对象指针
//
// int 范围：[-2^47, 2^47-1] = [-140737488355328, 140737488355327]
// ============================================================================

typedef enum {
    VAL_NULL,
    VAL_INT,
    VAL_FLOAT,
    VAL_BOOL,
    VAL_OBJ,
} ValueType;

typedef uint64_t Value;

typedef struct ObjFFICallback {
    Object header;
    int callback_id;
    void* sig;
    void* trampoline;
    Value func_val;
} ObjFFICallback;

#define QNAN        ((uint64_t)0x7FF8000000000000ULL)
#define SIGN_BIT    ((uint64_t)0x8000000000000000ULL)
#define TAG_MASK    ((uint64_t)0x0007000000000000ULL)

#define TAG_NULL    ((uint64_t)0x0000000000000000ULL)
#define TAG_FALSE   ((uint64_t)0x0001000000000000ULL)
#define TAG_TRUE    ((uint64_t)0x0002000000000000ULL)
#define TAG_INT     ((uint64_t)0x0003000000000000ULL)
#define TAG_OBJ     ((uint64_t)0x0004000000000000ULL)

#define NULL_VAL    (QNAN | SIGN_BIT | TAG_NULL)
#define FALSE_VAL   (QNAN | SIGN_BIT | TAG_FALSE)
#define TRUE_VAL    (QNAN | SIGN_BIT | TAG_TRUE)

#define PAYLOAD_MASK ((uint64_t)0x0000FFFFFFFFFFFFULL)

static inline double bits_to_double(Value v) {
    double d;
    memcpy(&d, &v, sizeof(double));
    return d;
}

static inline Value double_to_bits(double d) {
    Value v;
    memcpy(&v, &d, sizeof(double));
    return v;
}

static inline int val_is_null(Value v) {
    return v == NULL_VAL;
}

static inline int val_is_bool(Value v) {
    return v == FALSE_VAL || v == TRUE_VAL;
}

static inline int val_is_int(Value v) {
    return (v & (QNAN | SIGN_BIT | TAG_MASK)) == (QNAN | SIGN_BIT | TAG_INT);
}

static inline int val_is_float(Value v) {
    return (v & (QNAN | SIGN_BIT)) != (QNAN | SIGN_BIT);
}

static inline int val_is_obj(Value v) {
    return (v & (QNAN | SIGN_BIT | TAG_MASK)) == (QNAN | SIGN_BIT | TAG_OBJ);
}

static inline ValueType val_get_type(Value v) {
    if (val_is_null(v)) return VAL_NULL;
    if (val_is_int(v)) return VAL_INT;
    if (val_is_float(v)) return VAL_FLOAT;
    if (val_is_bool(v)) return VAL_BOOL;
    if (val_is_obj(v)) return VAL_OBJ;
    return VAL_NULL;
}

static inline Value val_int(int64_t n) {
    return QNAN | SIGN_BIT | TAG_INT | ((uint64_t)n & PAYLOAD_MASK);
}

static inline Value val_float(double n) {
    Value v = double_to_bits(n);
    if ((v & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT)) {
        v = QNAN;
    }
    return v;
}

static inline Value val_bool(int b) {
    return b ? TRUE_VAL : FALSE_VAL;
}

static inline Value val_null(void) {
    return NULL_VAL;
}

static inline Value val_obj(Object* obj) {
    return QNAN | SIGN_BIT | TAG_OBJ | (uint64_t)(uintptr_t)obj;
}

static inline int val_as_bool(Value v) {
    return v == TRUE_VAL;
}

// 48 位有符号整数，范围 [-2^47, 2^47-1]
#define INT48_SIGN_BIT  ((uint64_t)0x0000800000000000ULL)  // bit 47
#define INT48_MAX   ((int64_t)0x00007FFFFFFFFFFFULL)  // 2^47 - 1
#define INT48_MIN   ((int64_t)0xFFFF800000000000ULL)  // -2^47

static inline int64_t val_as_int(Value v) {
    int64_t val = (int64_t)(v & PAYLOAD_MASK);
    // sign-extend from bit 47
    if (val & INT48_SIGN_BIT) {
        val |= 0xFFFF000000000000ULL;
    }
    return val;
}

static inline double val_as_double(Value v) {
    return bits_to_double(v);
}

static inline Object* val_as_obj(Value v) {
    return (Object*)(uintptr_t)(v & PAYLOAD_MASK);
}

static inline Value val_num(double n) {
    if (n == (int64_t)n && n >= (double)INT48_MIN && n <= (double)INT48_MAX) {
        return val_int((int64_t)n);
    } else {
        return val_float(n);
    }
}

static inline int val_is_truthy(Value v) {
    if (val_is_bool(v)) {
        return v == TRUE_VAL;
    }
    if (val_is_int(v)) {
        return val_as_int(v) != 0;
    }
    if (val_is_float(v)) {
        return val_as_double(v) != 0.0;
    }
    if (val_is_null(v)) {
        return 0;
    }
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_FFI_POINTER) {
        return ((ObjFFIPointer*)val_as_obj(v))->ptr != NULL;
    }
    if (val_is_obj(v) && val_as_obj(v)->type == OBJ_FFI_CALLBACK) {
        return ((ObjFFICallback*)val_as_obj(v))->trampoline != NULL;
    }
    return 1;
}

const char* val_to_string(Value v);
char* value_to_string(Value v);

static inline int val_is_num(Value v) {
    return val_is_int(v) || val_is_float(v) ||
           (val_is_obj(v) && val_as_obj(v)->type == OBJ_BIGINT);
}

static inline double val_as_num(Value v) {
    if (val_is_int(v)) return (double)val_as_int(v);
    if (val_is_float(v)) return val_as_double(v);
    return 0.0;
}

static inline int val_is_string(Value v) {
    return val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING;
}

TypeInfo* type_infer_from_value(Value* v);

// ============================================================================
// 具体对象类型
// ============================================================================

// 字符串对象
typedef struct {
    Object header;
    char* chars;
    int len;          // UTF-8 字节长度
    int char_len;     // Unicode 字符数（缓存，O(1) 访问）
    uint32_t hash;
} ObjString;

// 数组对象
typedef struct {
    Object header;
    Value* elements;
    int count;
    int capacity;
    TypeInfo* type_info;  // 运行时类型信息，使用 TypeInfo 结构体，NULL 表示未设置
} ObjArray;

// 字典条目（使用开放寻址法的哈希表）
typedef struct {
    Value key;        // 键（支持 int/string/float/bool），NULL_VAL 表示空槽
    Value value;
} ObjDictEntry;

// 字典对象
// 使用数组+哈希混合结构，优化连续整数索引的访问性能
#define DICT_MAX_LOAD 0.75  // 哈希表最大负载因子
#define DICT_ARRAY_MAX_LOAD 0.75  // 数组部分最大负载因子

typedef struct {
    Object header;

    // 数组部分：存储连续整数索引 0..(asize-1) 的值
    // 使用数组可以 O(1) 直接访问，无需哈希计算
    Value* array;           // 数组部分
    int asize;              // 数组部分大小（实际分配的容量）
    int acount;             // 数组部分实际元素数量
    int last_index;         // 最后一个使用的数组索引（用于快速判断是否需要扩容）

    // 哈希部分：存储其他键（字符串、非连续整数等）
    ObjDictEntry* entries;  // 哈希表数组
    int count;              // 哈希表实际条目数（不包括 tombstone）
    int capacity;           // 哈希表容量
    int tombstone_count;    // tombstone 数量

    TypeInfo* type_info;    // 运行时类型信息（键值类型），NULL 表示未指定泛型类型

    // 插入顺序维护（用于保持输出顺序）
    Value* order;           // 插入顺序的键数组
    int order_count;        // 当前顺序数组元素数
    int order_capacity;     // 顺序数组容量
} ObjDict;

// 函数对象
typedef struct {
    Object header;
    int arity;
    int upvalue_count;
    int local_count;  // 局部变量数量（包括参数和 callee）
    struct Chunk* chunk;
    char* name;
    void* module;  // 所属模块（如果是模块函数），使用 void* 避免循环依赖
    int has_try;  // 是否包含 try-catch-finally 语句
    TypeKind* param_types;  // 参数类型数组（可选，用于运行时类型检查）
    char** param_generic_names;  // 参数泛型类型参数名（如 "T", "K"），用于泛型方法参数类型检查
    int param_generic_count;     // 有泛型参数名的参数数量
    int type_param_count;        // 函数级泛型类型参数数量（如 func f[T] 的 type_param_count=1）
    char** type_param_names;     // 函数级泛型类型参数名称数组（如 ["T"] 或 ["K","V"]）
    char** type_param_constraints; // 函数级泛型类型参数约束 face 名数组（如 ["Comparable"]，NULL 表示无约束）
    int is_ctor;              // 是否是构造函数（用于 OP_RETURN 返回 self）
} ObjFunction;

// 原生函数类型
typedef Value (*NativeFn)(int argCount, Value* args);

// 原生函数对象
typedef struct {
    Object header;
    NativeFn function;
    int arity;
    char* name;
} ObjNative;

// Upvalue（运行时）
typedef struct Upvalue {
    Object header;
    Value* location;
    Value closed;
    struct Upvalue* next;
} Upvalue;

// 闭包对象
typedef struct ObjClosure {
    Object header;
    ObjFunction* function;
    Upvalue* upvalues[MAX_UPVALUES];
    int upvalue_count;
    int type_param_count;        // 泛型函数调用时传入的类型参数数量
    char** type_param_args;      // 泛型函数调用时传入的类型参数（如 ["int"] 或 ["float"]）
} ObjClosure;

// BigInt 对象（大整数）- 使用 base-1e9 存储，支持任意精度
// 每个 limb 存储 9 位十进制数 (0-999999999)，大幅减少内存占用
typedef struct {
    Object header;
    uint32_t* limbs;   // 数字数组（小端序，limbs[0] 是最低位）
    int limb_count;    // limb 数量
    int is_negative;   // 是否为负数
} ObjBigInt;

// 导出名 -> 全局变量索引映射（用于运行时更新导出值）
typedef struct {
    char* name;
    int global_index;
} ExportGlobalMapping;

// 模块对象 - 包含模块级别的变量和函数
typedef struct {
    Object header;
    char* name;           // 模块名称（别名）
    char* source_path;    // 规范化绝对路径（与 loaded_modules 键一致，用于模块缓存与 MODULE_REF 匹配）
    Value* globals;       // 模块全局变量数组
    int global_count;     // 全局变量数量
    int global_capacity;  // 全局变量数组容量
    ObjDict* exports;     // 导出表（名称 -> 值）
    struct ModuleFrame* frame;  // 模块执行帧（运行时）
    char** native_imports;      // 引用的原生模块名列表
    int native_import_count;    // 原生模块引用数量
    struct Chunk* init_chunk;   // 模块初始化字节码（延迟到运行时执行）
    int initialized;            // 是否已在运行时初始化
    ExportGlobalMapping* export_mappings;  // 导出名 -> 全局变量索引映射
    int export_mapping_count;   // 导出映射数量
    char** use_reexport_names;  // use 导入的需要 re-export 的类型名（运行时添加到 exports）
    int* use_reexport_kinds;    // use re-export 的类型种类（TYPE_ENUM, TYPE_STRUCT 等）
    int use_reexport_count;     // use re-export 数量
} ObjModule;

// 绑定方法对象 - 将 receiver 和 method 绑定在一起
typedef struct {
    Object header;
    Value receiver;       // 接收者（如数组、字符串等）
    ObjNative* method;    // 方法（原生函数），非闭包方法时使用
    ObjClosure* closure;  // 闭包方法（用户自定义 struct 方法），非 NULL 时优先使用
} ObjBoundMethod;

// 范围对象
typedef struct {
    Object header;
    int64_t start;      // 起始值（int48，用 int64_t 容器存储，与 Leno int 模型对齐）
    int64_t end;        // 结束值（int48，用 int64_t 容器存储，与 Leno int 模型对齐）
    int inclusive;      // 是否包含结束值（1=包含，0=不包含）
} ObjRange;

// 文件对象
typedef struct {
    Object header;
    FILE* fp;           // C 标准库文件指针
    ObjString* path;    // 文件路径（使用 ObjString 便于 GC 管理）
    ObjString* mode;    // 打开模式（使用 ObjString 便于 GC 管理）
    int is_closed;      // 是否已关闭
} ObjFile;

// 跨平台 socket 类型（不依赖 winsock2.h，使用通用整数类型）
#ifdef _WIN32
    typedef uintptr_t socket_fd_t;
    #define INVALID_SOCKET_FD ((socket_fd_t)(~((uintptr_t)0)))
#else
    typedef int socket_fd_t;
    #define INVALID_SOCKET_FD (-1)
#endif

#define SOCKET_TYPE_TCP 1
#define SOCKET_TYPE_UDP 2

// Socket 对象
typedef struct {
    Object header;
    socket_fd_t fd;
    int type;           // SOCKET_TYPE_TCP 或 SOCKET_TYPE_UDP
    int is_connected;   // TCP 是否已连接
    int is_listening;   // TCP 是否在监听
    int is_nonblocking; // 是否非阻塞模式
    int last_error;     // 最后一次错误码
} ObjSocket;

// 结构体字段信息
typedef struct {
    char* name;         // 字段名称
    TypeKind type;      // 字段类型
    char* struct_type_name; // 嵌套 struct 类型名称（当 type == TYPE_STRUCT 时有效）
    Value default_value; // 字段默认值
    int has_default;    // 是否有默认值
    TypeKind element_type;  // Ptr[T] 中 T 的类型（仅当 type == TYPE_PTR_GENERIC 时有效）
    int nullable;       // 可空字段标记：1=Type?，0=Type
} StructFieldInfo;

// 结构体方法信息
typedef struct {
    char* name;                    // 方法名称
    ObjFunction* func;             // 方法函数对象
    ObjClosure* closure;           // 预创建的闭包（优化用）
} StructMethodInfo;

// enum 成员信息
typedef struct {
    char* name;         // 成员名称
    int64_t value;      // 成员值（使用int64_t支持大整数）
} EnumMemberInfo;

// enum 定义对象
typedef struct {
    Object header;
    char* name;                    // enum 名称
    EnumMemberInfo* members;       // 成员信息数组
    int member_count;              // 成员数量
} ObjEnumDef;

// C 布局结构体字段信息
typedef struct {
    char* name;         // 字段名称
    TypeKind type;      // 字段类型
    int offset;         // 字段偏移量（字节）
    int size;           // 字段大小（字节）
    int array_dim;      // 数组维度（0 表示非数组，>0 表示数组大小）
    char* struct_name;  // 嵌套结构体类型名称（仅当 type == TYPE_CSTRUCT 时有效）
    TypeKind element_type;  // Ptr[T] 中 T 的类型（仅当 type == TYPE_PTR_GENERIC 时有效）
} CStructFieldInfo;

// C 布局结构体字段哈希表条目
typedef struct CStructFieldHashEntry {
    char* name;                    // 字段名（key）
    int field_index;               // 字段在数组中的索引
    struct CStructFieldHashEntry* next;  // 链表处理冲突
} CStructFieldHashEntry;

// C 布局结构体定义对象
typedef struct {
    Object header;
    char* name;                    // 结构体名称
    CStructFieldInfo* fields;      // 字段信息数组
    int field_count;               // 字段数量
    int total_size;                // 总大小（字节）
    int alignment;                 // 对齐要求（字节）
    bool is_packed;                 // 是否 packed（取消所有字段间 padding）
    int  explicit_align;            // 显式对齐要求（0 = 未指定）
    // 字段名哈希表（运行时 O(1) 查找）
    CStructFieldHashEntry** field_hash_table;  // 哈希表数组
    int field_hash_capacity;       // 哈希表容量
} ObjCStructDef;

// C 布局结构体实例对象
typedef struct {
    Object header;
    ObjCStructDef* def;            // 结构体定义
    uint8_t* data;                 // 原始内存数据（按 C 布局）
    int owns_memory;               // 是否拥有内存（需要 free）
} ObjCStruct;

// C 布局结构体数组字段视图对象
typedef struct {
    Object header;
    ObjCStruct* cstruct;           // 所属 cstruct 实例
    int field_index;               // 字段索引
    TypeKind element_type;         // 数组元素类型
    int element_size;              // 元素大小（字节）
    int array_dim;                 // 数组维度
} ObjCStructArrayView;

// C 布局结构体数组对象（批量操作）
typedef struct {
    Object header;
    ObjCStructDef* def;            // 结构体定义
    uint8_t* data;                 // 原始内存数据（连续数组）
    int count;                     // 数组元素个数
    int element_size;              // 单个元素大小
} ObjCStructArray;

// 结构体定义对象（编译期）
typedef struct {
    Object header;
    char* name;                    // 结构体名称
    StructFieldInfo* fields;       // 字段信息数组
    int field_count;               // 字段数量
    StructMethodInfo* methods;     // 方法信息数组
    int method_count;              // 方法数量
    char** impl_names;             // impl 声明的 face 名称数组
    int impl_count;                // impl 声明数量
    int type_param_count;          // 泛型类型参数数量（如 Box[T] 的 type_param_count=1）
    char** type_param_names;       // 泛型类型参数名称数组（如 ["T"] 或 ["K","V"]）
    char** type_param_constraints;  // 泛型类型参数约束 face 名数组
    int has_ctor;                   // 是否有构造函数
    int ctor_index;                 // 构造函数在 methods[] 中的索引（-1 表示无）
    int has_dtor;                   // 是否有析构函数
    int dtor_index;                 // 析构函数在 methods[] 中的索引（-1 表示无）
    // 关联常量（associated constants）
    char** const_names;             // 关联常量名数组
    Value* const_values;            // 关联常量值数组
    int const_count;                // 关联常量数量
} ObjStructDef;

// 结构体实例对象
typedef struct {
    Object header;
    ObjStructDef* def;             // 结构体定义
    Value* field_values;           // 字段值数组（fields_inline=1 时指向对象尾部内联数组）
    ObjString* declared_face;      // 声明时的 face 类型名称（如 "Speaker"），NULL 表示未通过 face 声明
    char** generic_type_args;      // 泛型参数类型名数组（如 ["int"]），NULL 表示非泛型实例
    int generic_type_arg_count;    // 泛型参数数量
    int fields_inline;             // 字段数组是否内联在对象内存尾部（1=随对象一次分配，释放时不可单独 free）
} ObjStruct;

// face 方法签名
typedef struct {
    char* name;                    // 方法名称
    TypeInfo* return_type;         // 返回类型
    TypeInfo** param_types;        // 参数类型数组
    int param_count;               // 参数数量
} FaceMethodInfo;

// face 定义对象
typedef struct {
    Object header;
    char* name;                    // face 名称
    FaceMethodInfo* methods;       // 方法签名数组
    int method_count;              // 方法数量
    int type_param_count;          // 泛型类型参数数量
    char** type_param_names;       // 泛型类型参数名称数组
    char** type_param_constraints;  // 泛型类型参数约束 face 名数组
} ObjFaceDef;

// ============================================================================
// 协程系统
// ============================================================================

// 协程状态
typedef enum {
    COROUTINE_NEW,        // 刚创建，未开始
    COROUTINE_RUNNING,    // 正在执行
    COROUTINE_SUSPENDED,  // 被 await 暂停
    COROUTINE_COMPLETED,  // 执行完毕
    COROUTINE_FAILED      // 执行出错
} CoroutineState;

// 协程对象（前向声明，完整定义在 leno_vm.h）
struct CallFrame;  // 前向声明
typedef struct ObjCoroutine {
    Object header;
    CoroutineState state;
    int saved_sp;                      // 保存的栈指针（恢复时精确还原栈位置）
    int saved_frame_cnt;               // 协程的基准 frame_cnt（协程帧之前的 frame 数）
    struct CallFrame* saved_frames;   // 保存的 frame 副本数组（动态分配）
    int saved_frame_count;             // 保存的 frame 数量（0 表示无保存帧）
    Value result;                      // 返回值 / Future 结果
    ObjClosure* closure;               // 关联的闭包
    int await_count;                   // await 次数
    struct ObjFuture* waiting_for;     // 正在等待的 Future
    struct ObjFuture* task_future;     // task 返回给调用者的 Future（协程完成时需要触发）
    int error_propagated;             // 错误是否已通过 Future 传播给等待者
    struct ObjCoroutine* next;         // 就绪队列链表指针
    // 初始参数（用于协程启动时传递给 locals）
    Value* initial_args;               // 参数数组（动态分配）
    int initial_arg_count;             // 参数数量
} ObjCoroutine;

// Future 对象
typedef struct ObjFuture {
    Object header;
    int completed;                     // 是否完成
    Value result;                      // 结果值
    Value error;                       // 错误值
    ObjCoroutine* waiter;              // 等待此 Future 的协程
} ObjFuture;

// ============================================================================
// 线程系统
// ============================================================================

// 线程状态
typedef enum {
    THREAD_RUNNING,     // 运行中
    THREAD_DONE,        // 已完成
    THREAD_ERROR        // 出错
} ThreadState;

// 线程对象（前向声明 VM）
struct VM;
typedef struct ObjThread {
    Object header;
    struct VM* vm;
    PlatformThread os_thread;
    PlatformMutex mutex;
    PlatformCondVar done_cond;
    ThreadState state;
    Value result;
    char* error_msg;
    int has_result;
    int joined;
    // ★ 线程启动参数的 GC 保护锚点
    // 在 thread_new_with_args 中深拷贝到主线程 GC 堆，子线程在 thread_entry_point
    // 中读取并 re-clone 到自己的 GC 堆后清 NULL。
    // GC 通过 gc_scan_children(OBJ_THREAD) 扫描这些值，防止主线程 GC 在子线程
    // 读取前回收它们（这是多线程崩溃的主要根因）。
    Value* pending_args;
    int pending_arg_count;
} ObjThread;

// Channel 对象
typedef struct ObjChannel {
    Object header;
    int ref_count;
    Value* buffer;
    int capacity;
    int head;
    int tail;
    int count;
    int closed;
    PlatformMutex mutex;
    PlatformCondVar not_empty;
    PlatformCondVar not_full;
} ObjChannel;

// 定时器
typedef struct {
    uint64_t wake_time;                // 唤醒时间（毫秒）
    ObjCoroutine* coroutine;           // 关联的协程
} Timer;

// 事件循环
typedef struct {
    Timer* timers;                     // 定时器队列（动态数组）
    int timer_count;
    int timer_capacity;
    ObjCoroutine** ready_queue;        // 就绪队列（动态数组）
    int ready_count;
    int ready_capacity;
    int running;                       // 是否正在运行
    // 失败协程收集
    Value* errors;                     // 失败协程的错误信息数组
    int error_count;                   // 失败协程数量
    int error_capacity;                // 错误数组容量
} EventLoop;

// 最大协程数量
#define MAX_COROUTINES 1024
#define MAX_TIMERS 1024

// ============================================================================
// GC 系统（分代收集：年轻代 + 老年代 + 写屏障）
// ============================================================================

#define GC_MODE_FULL    0
#define GC_MODE_MINOR   1

#define GC_YOUNG_THRESHOLD   (1024 * 1024 * 8)   // 8MB
#define GC_OLD_THRESHOLD     (1024 * 1024 * 32)  // 32MB
#define GC_OLD_FLUSH_THRESHOLD (1024 * 1024 * 2)  // 2MB：Minor GC 时老年代超此阈值顺带清扫
#define GC_PROMOTE_AGE       2
#define GC_REMEMBERED_INIT   256

typedef struct {
    Object* young_heap;
    Object* old_heap;
    size_t young_allocated;
    size_t old_allocated;
    size_t young_threshold;
    size_t old_threshold;
    int running;
    int mode;
    struct VM* vm;
    int enabled;
    int deferred_gc;              // 延迟 GC 标志：gc_alloc 置位，由事件循环在帧间执行
    Object** remembered_set;
    int remembered_count;
    int remembered_capacity;
    int promote_age;
    size_t minor_gc_count;
    Value** extra_roots;
    int extra_root_count;
    int extra_root_capacity;
} LenoGC;

extern THREAD_LOCAL LenoGC gc;

void gc_init(void);
void gc_mark_value(Value v);
void gc_mark_object(Object* obj);
void gc_collect(void);
void gc_minor_collect(void);
void gc_major_collect(void);
Object* gc_alloc(size_t size, ObjType type);
void gc_free_all(void);
void gc_track_memory(Object* obj, size_t old_size, size_t new_size);
void gc_write_barrier(Object* holder, Value value);
void gc_write_barrier_obj(Object* holder, Object* value_obj);
// 将老年代对象加入 remembered set（供批量字段写入后保守调用）
void gc_remember_object(Object* holder);

void gc_set_enabled(int enabled);
int gc_get_enabled(void);
void gc_try_collect_deferred(void);
void gc_check_safe_point(void);
void gc_push_root(Value* ptr);
void gc_pop_root(void);

// ============================================================================
// 对象操作 API
// ============================================================================

// 数组操作
ObjArray* arr_new(int capacity);
int arr_grow(ObjArray* arr);  // 数组扩容，成功返回1，失败返回0

// 数组读写（static inline 供编译器内联优化）
static inline void arr_write(ObjArray* arr, int index, Value value) {
    if (index < 0 || index >= arr->capacity) {
        extern void native_throw_error(const char* msg);
        native_throw_error("数组索引越界");
        return;
    }
    arr->elements[index] = value;
    gc_write_barrier((Object*)arr, value);
    if (index >= arr->count) {
        arr->count = index + 1;
    }
}

static inline Value arr_read(ObjArray* arr, int index) {
    if (index < 0 || index >= arr->count) {
        extern void native_throw_error(const char* msg);
        native_throw_error("数组索引越界");
        return val_null();
    }
    return arr->elements[index];
}

// 字典操作
// 用于标记已删除哈希表条目的 Value 哨兵值
#define DICT_TOMBSTONE_VAL ((Value)(QNAN | SIGN_BIT | TAG_TRUE | 0x1))

ObjDict* dict_new(int capacity);
void dict_set(ObjDict* dict, Value key, Value value);
Value dict_get(ObjDict* dict, Value key);
int dict_has(ObjDict* dict, Value key);
void dict_delete(ObjDict* dict, Value key);
void dict_try_shrink(ObjDict* dict);
int dict_get_array_size(ObjDict* dict);  // 获取数组部分大小（用于迭代）
int dict_get_hash_count(ObjDict* dict);  // 获取哈希部分条目数（用于迭代）
ObjString* dict_key_to_string(Value key); // 将键转为字符串表示

// 按索引获取字典键（static inline 供编译器内联优化）
static inline Value dict_get_key_by_index(ObjDict* dict, int index) {
    if (!dict || index < 0) return NULL_VAL;
    if (dict->order && index < dict->order_count) {
        return dict->order[index];
    }
    return NULL_VAL;
}

Value dict_get_value_by_index(ObjDict* dict, int index);  // 按索引获取值（用于for循环同时获取键值）

// 字符串操作
uint32_t hash_string(const char* key, int length);
int utf8_char_len(const char* chars, int byte_len);
int utf8_char_offset(const char* chars, int byte_len, int char_index);
int utf8_char_byte_len(const char* chars, int byte_len, int offset);
ObjString* str_alloc(int len);
ObjString* str_new(const char* chars, int len);
ObjString* str_new_nointern(const char* chars, int len);  // 创建非内化字符串（用于长字符串）

// 字符串复制（static inline 供编译器内联优化）
static inline ObjString* str_copy(const char* chars, int len) {
    return str_new(chars, len);
}

ObjString* str_concat(ObjString* a, ObjString* b);

// Range 操作
ObjRange* range_new(int64_t start, int64_t end, int inclusive);
int range_contains(ObjRange* range, int64_t value);

// BigInt 操作
ObjBigInt* bigint_new(const uint32_t* limbs, int limb_count, int is_negative);
ObjBigInt* bigint_from_int64(int64_t value);
ObjBigInt* bigint_from_string(const char* str);
int bigint_is_int(ObjBigInt* bigint);
int bigint_to_int(ObjBigInt* bigint);
// BigInt 简单操作（static inline 供编译器内联优化）
static inline int val_is_bigint(Value v) {
    return val_is_obj(v) && val_as_obj(v)->type == OBJ_BIGINT;
}

static inline ObjBigInt* val_as_bigint(Value v) {
    if (val_is_bigint(v)) {
        return (ObjBigInt*)val_as_obj(v);
    }
    return NULL;
}

// 检查 BigInt 是否适合 int64 范围（简单检查，可内联）
static inline int bigint_fits_in_int64(ObjBigInt* bigint) {
    // 如果 limb 数量超过 2，肯定超过 int64 范围
    if (bigint->limb_count > 2) return 0;
    if (bigint->limb_count < 2) return 1;
    // 2个 limb：最大值为 BASE^2 - 1 < INT64_MAX
    return 1;
}

// 需要复杂计算的函数保持声明
int64_t bigint_to_int64(ObjBigInt* bigint);
Value val_bigint_from_int64(int64_t value);
Value val_bigint_from_uint64(uint64_t value);
Value val_bigint_from_string(const char* str);
Value val_bigint_from_limbs(const uint32_t* limbs, int limb_count, int is_negative);
// 检查 int 是否溢出，溢出时返回 bigint Value
Value val_int_safe(int64_t value);

// 如果 bigint 的值在 int48 范围内，将其压缩为 int Value
static inline Value bigint_compact_to_int(Value v) {
    if (val_is_bigint(v)) {
        ObjBigInt* bi = val_as_bigint(v);
        if (bigint_fits_in_int64(bi)) {
            int64_t i64 = bigint_to_int64(bi);
            if (i64 >= INT48_MIN && i64 <= INT48_MAX) {
                return val_int(i64);
            }
        }
    }
    return v;
}

// BigInt 运算
Value bigint_add(ObjBigInt* a, ObjBigInt* b);
Value bigint_sub(ObjBigInt* a, ObjBigInt* b);
Value bigint_mul(ObjBigInt* a, ObjBigInt* b);
Value bigint_div(ObjBigInt* a, ObjBigInt* b);
Value bigint_neg(ObjBigInt* a);
Value bigint_mod(ObjBigInt* a, ObjBigInt* b);
Value bigint_and(ObjBigInt* a, ObjBigInt* b);
Value bigint_or(ObjBigInt* a, ObjBigInt* b);
Value bigint_xor(ObjBigInt* a, ObjBigInt* b);
Value bigint_not(ObjBigInt* a);
Value bigint_shl(ObjBigInt* a, int shift);
Value bigint_shr(ObjBigInt* a, int shift);
Value bigint_ushr(ObjBigInt* a, int shift);
int bigint_compare(ObjBigInt* a, ObjBigInt* b);

// BigInt 工具函数
char* bigint_to_string(ObjBigInt* bigint);
int bigint_is_zero(ObjBigInt* bigint);

// ============================================================================
// 内联辅助函数（供VM外部模块使用）
// ============================================================================

// 将 BigInt 转换为 double（正确处理大数）- 内联版本供外部使用
// 使用 likely 提示编译器：绝大多数 BigInt 都在 int64 范围内
static inline double bigint_to_double(ObjBigInt* bigint) {
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(bigint->limb_count <= 2, 1)) {
        // 可以用 int64 表示，直接转换
        return (double)bigint_to_int64(bigint);
    }
#else
    if (bigint->limb_count <= 2) {
        return (double)bigint_to_int64(bigint);
    }
#endif
    // 大数：使用 limb 计算 double 值
    // double 有 53 位有效位数，我们需要累加所有 limb
    double result = 0.0;
    double base = 4294967296.0;  // 2^32 = BASE
    // 从高位到低位计算
    for (int i = bigint->limb_count - 1; i >= 0; i--) {
        result = result * base + (double)bigint->limbs[i];
    }
    return bigint->is_negative ? -result : result;
}

// 将 Value 转换为 double（支持 int/float/bigint）- 内联版本供外部使用
// 使用 likely/unlikely 提示编译器优化分支预测
static inline double value_to_double(Value val) {
#if defined(__GNUC__) || defined(__clang__)
    if (__builtin_expect(val_is_int(val), 1)) {
        return (double)val_as_int(val);
    } else if (__builtin_expect(val_is_float(val), 0)) {
        return val_as_double(val);
    } else if (__builtin_expect(val_is_bigint(val), 0)) {
        return bigint_to_double(val_as_bigint(val));
    }
#else
    if (val_is_int(val)) {
        return (double)val_as_int(val);
    } else if (val_is_float(val)) {
        return val_as_double(val);
    } else if (val_is_bigint(val)) {
        return bigint_to_double(val_as_bigint(val));
    }
#endif
    return 0.0;
}

// ============================================================================
// 支持 BigInt 的 Value 辅助函数（在 BigInt 定义之后）
// ============================================================================

// 辅助函数：检查是否是整数类型（包含 VAL_INT 和 BigInt）
static inline int val_is_int_ex(Value v) {
    return val_is_int(v) || val_is_bigint(v);
}

static inline int val_is_num_ex(Value v) {
    return val_is_int(v) || val_is_float(v) || val_is_bigint(v);
}

static inline int64_t val_as_int_ex(Value v) {
    if (val_is_int(v)) {
        return val_as_int(v);
    }
    if (val_is_bigint(v)) {
        ObjBigInt* bigint = val_as_bigint(v);
        return bigint_to_int64(bigint);
    }
    return 0;
}

static inline double val_as_num_ex(Value v) {
    if (val_is_int(v) || val_is_float(v)) {
        return val_as_num(v);
    }
    if (val_is_bigint(v)) {
        return bigint_to_double(val_as_bigint(v));
    }
    return 0.0;
}

// ============================================================================
// 统一值比较（浅比较）— Single Source of Truth
// ============================================================================
// 相关 Issue: 统一与优化 values_equal, dict_key_equals 与 value_deep_equal 的比较逻辑
// 此函数是所有值比较的核心，其他比较函数（values_equal, dict_key_equals,
// value_deep_equal）均基于它构建。

// 检查 BigInt 是否等于 int64（无内存分配，避免临时对象）
static inline int bigint_equals_int64(ObjBigInt* a, int64_t b) {
    // 零值快速路径
    if (a->limb_count == 0 || (a->limb_count == 1 && a->limbs[0] == 0)) {
        return b == 0;
    }
    // 3+ limbs: 值超过 2^64，不可能等于 int64
    if (a->limb_count > 2) return 0;

    // 提取绝对值（base-2^32 小端序）
    uint64_t abs_a;
    if (a->limb_count == 1) {
        abs_a = (uint64_t)a->limbs[0];
    } else {
        abs_a = ((uint64_t)a->limbs[1] << 32) | (uint64_t)a->limbs[0];
    }

    if (a->is_negative) {
        if (b >= 0) return 0;
        uint64_t abs_b = (uint64_t)(-(b + 1)) + 1;  // 安全取绝对值
        return abs_a == abs_b;
    } else {
        if (b < 0) return 0;
        return abs_a == (uint64_t)b;
    }
}

// 核心浅比较：处理基本类型、String 内容、BigInt 值、对象指针
// 快速路径覆盖 null/bool（bit-exact）、同指针对象
// 跨类型支持 int<->float、int<->BigInt
static inline int value_shallow_equal(Value a, Value b) {
    // 1. 快速路径：bit-exact 相等（涵盖 null, bool, 同指针对象, 相同 int/float）
    if (a == b) return 1;

    // 2. 跨类型比较：int <-> float
    if (val_is_int(a) && val_is_float(b)) {
        return (double)val_as_int(a) == val_as_double(b);
    }
    if (val_is_float(a) && val_is_int(b)) {
        return val_as_double(a) == (double)val_as_int(b);
    }

    // 3. 跨类型比较：int <-> BigInt（无内存分配）
    if (val_is_int(a) && val_is_bigint(b)) {
        return bigint_equals_int64(val_as_bigint(b), val_as_int(a));
    }
    if (val_is_bigint(a) && val_is_int(b)) {
        return bigint_equals_int64(val_as_bigint(a), val_as_int(b));
    }

    // 4. 同类型：int
    if (val_is_int(a) && val_is_int(b)) {
        return val_as_int(a) == val_as_int(b);
    }
    // 5. 同类型：float
    if (val_is_float(a) && val_is_float(b)) {
        return val_as_double(a) == val_as_double(b);
    }

    // 6. 对象比较
    if (val_is_obj(a) && val_is_obj(b)) {
        Object* oa = val_as_obj(a);
        Object* ob = val_as_obj(b);
        // a == b 已检查，此处指针不同
        if (oa->type != ob->type) return 0;
        if (oa->type == OBJ_STRING) {
            ObjString* sa = (ObjString*)oa;
            ObjString* sb = (ObjString*)ob;
            if (sa->len != sb->len) return 0;
            if (sa->hash != sb->hash) return 0;
            for (int i = 0; i < sa->len; i++) {
                if (sa->chars[i] != sb->chars[i]) return 0;
            }
            return 1;
        }
        if (oa->type == OBJ_BIGINT) {
            return bigint_compare((ObjBigInt*)oa, (ObjBigInt*)ob) == 0;
        }
        // 其他对象类型：指针不同即不等
        return 0;
    }

    return 0;
}

// ============================================================================
// 深拷贝 API
// ============================================================================

// 深拷贝一个 Value（递归处理数组、字典、结构体等引用类型）
Value value_copy(Value v);

// ============================================================================
// 结构体操作 API
// ============================================================================

// 创建结构体定义
ObjStructDef* struct_def_new(const char* name, int field_count, int method_count);

// 设置结构体字段
void struct_def_set_field(ObjStructDef* def, int index, const char* name, TypeKind type,
                          const char* struct_type_name, Value default_value, int has_default, TypeKind element_type, int nullable);

// 创建结构体实例
ObjStruct* struct_instance_new(ObjStructDef* def);

// 获取结构体字段索引
int struct_get_field_index(ObjStructDef* def, const char* name);

// 获取结构体字段值
Value struct_get_field(ObjStruct* obj, int index);

// 设置结构体字段值
void struct_set_field(ObjStruct* obj, int index, Value value);

// 结构体定义查找（运行时）
ObjStructDef* struct_def_find(const char* name);

// 注册结构体定义
void struct_def_register(ObjStructDef* def);

// 当前结构体定义表代数（每次注册/覆盖递增），供 VM 内联缓存校验 def 是否被重定义
uint32_t struct_def_generation(void);

// 跨线程：返回当前线程结构体定义表数量（主线程抓取快照）
int struct_def_get_count(void);
// 跨线程：返回当前线程结构体定义表第 i 个定义
ObjStructDef* struct_def_get(int i);
// 跨线程：将主线程的定义导入当前（子）线程的定义表
void struct_def_import_from_thread(ObjStructDef** defs, int count);

// 更新所有结构体方法函数的 module 指针
void struct_def_update_method_modules(ObjModule* old_module, ObjModule* new_module);

// ============================================================================
// face 定义管理
// ============================================================================

ObjFaceDef* face_def_new(const char* name, int method_count);
void face_def_set_type_params(ObjFaceDef* def, int count, char** names, char** constraints);
void face_def_register(ObjFaceDef* def);
ObjFaceDef* face_def_find(const char* name);
int struct_implements_face(ObjStructDef* struct_def, ObjFaceDef* face_def);
int struct_def_has_face_method(const char* struct_name, const char* method_name, int* out_param_count, TypeKind* out_return_type);

// ============================================================================
// enum 定义管理
// ============================================================================

// 创建 enum 定义
ObjEnumDef* enum_def_new(const char* name, int member_count);

// 设置 enum 成员
void enum_def_set_member(ObjEnumDef* def, int index, const char* name, int64_t value);

// 查找 enum 成员值
int64_t enum_def_get_member_value(ObjEnumDef* def, const char* name);

// enum 定义查找（运行时）
ObjEnumDef* enum_def_find(const char* name);

// 注册 enum 定义
void enum_def_register(ObjEnumDef* def);

// C 布局结构体定义创建
ObjCStructDef* cstruct_def_new(const char* name, int field_count, int total_size, int alignment);

// 设置 C 布局结构体字段
void cstruct_def_set_field(ObjCStructDef* def, int index, const char* name, TypeKind type, int offset, int size, int array_dim, const char* struct_name, TypeKind element_type);

// 查找 C 布局结构体字段
CStructFieldInfo* cstruct_def_find_field(ObjCStructDef* def, const char* name);

// 获取 C 布局结构体字段索引
int cstruct_get_field_index(ObjCStructDef* def, const char* name);

// C 布局结构体实例创建
ObjCStruct* cstruct_new(ObjCStructDef* def);

// 从指针创建 C 布局结构体实例（不拥有内存）
ObjCStruct* cstruct_from_ptr(ObjCStructDef* def, void* ptr);

// 注册 C 布局结构体定义
void cstruct_def_register(ObjCStructDef* def);

// 查找 C 布局结构体定义
ObjCStructDef* cstruct_def_find(const char* name);

// 获取字段值
Value cstruct_get_field_value(ObjCStruct* obj, int field_index);

// 设置字段值
void cstruct_set_field_value(ObjCStruct* obj, int field_index, Value value);

// C 布局结构体数组创建
ObjCStructArray* cstruct_array_new(ObjCStructDef* def, int count);

// 从 cstruct 数组获取指定索引的元素（返回临时 cstruct 实例，不拥有内存）
ObjCStruct* cstruct_array_get(ObjCStructArray* array, int index);

// 释放 C 布局结构体数组
void cstruct_array_free(ObjCStructArray* array);

// cstruct 方法支持
void cstruct_init_methods(void);
void cstruct_mark_methods(void);
void cstruct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
ObjNative* cstruct_find_method(const char* name);

// 查找结构体方法
ObjNative* struct_find_method(const char* name);
void struct_mark_methods(void);

// ============================================================================
// 绑定方法支持
// ============================================================================

// 创建绑定方法
ObjBoundMethod* bound_method_new(Value receiver, ObjNative* method);
ObjBoundMethod* bound_closure_method_new(Value receiver, ObjClosure* closure);

// 查找数组方法
ObjNative* array_find_method(const char* name);

// 查找字符串方法
ObjNative* string_find_method(const char* name);

// 查找数字方法
ObjNative* number_find_method(const char* name);

// 查找字典方法
ObjNative* dict_find_method(const char* name);

// 查找文件方法
ObjNative* file_find_method(const char* name);

ObjNative* thread_find_method(const char* name);
ObjNative* channel_find_method(const char* name);

void thread_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
void channel_register_method_with_params(const char* name, ObjNative* method, int arity,
                                          int min_arity, int max_arity,
                                          TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

void thread_init_methods(void);
void channel_init_methods(void);
void thread_mark_methods(void);
void channel_mark_methods(void);

void threads_init_instance_methods(void);

// 查找结构体方法
ObjNative* struct_find_method(const char* name);

// 初始化数组方法表
void array_init_methods(void);

// 标记所有数组方法对象（供 GC 使用）
void array_mark_methods(void);

// 初始化文件方法表
void file_init_methods(void);

// 标记所有文件方法对象（供 GC 使用）
void file_mark_methods(void);

// ============================================================================
// 通用方法注册表（所有内置类型共享）
// ============================================================================

#include "method_table.h"

// 方法注册/查找 API（各类型保留独立函数名，内部调用通用 method_table_* 函数）

// Array
void array_register_method_with_params(const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
ArrayMethodEntry array_find_method_meta(const char* name);
TypeKind array_get_method_param_type(const char* method_name, int param_index);

// String
void string_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
StringMethodEntry string_find_method_meta(const char* name);
TypeKind string_get_method_param_type(const char* method_name, int param_index);

// File
void file_register_method_with_params(const char* name, ObjNative* method, int arity,
                                       int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
FileMethodEntry file_find_method_meta(const char* name);
TypeKind file_get_method_param_type(const char* method_name, int param_index);

// Socket
void socket_init_methods(void);
void socket_mark_methods(void);
void socket_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
ObjNative* socket_find_method(const char* name);

// Dict
void dict_init_methods(void);
void dict_mark_methods(void);
void dict_register_method_with_params(const char* name, ObjNative* method, int arity,
                                       int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
DictMethodEntry dict_find_method_meta(const char* name);
TypeKind dict_get_method_param_type(const char* method_name, int param_index);

// Struct
void struct_init_methods(void);
void struct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
StructMethodEntry struct_find_method_meta(const char* name);

// CStruct
void cstruct_init_methods(void);
void cstruct_mark_methods(void);
void cstruct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                          int min_arity, int max_arity,
                                          TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
CStructMethodEntry cstruct_find_method_meta(const char* name);

// Thread
void thread_init_methods(void);
void thread_mark_methods(void);
void thread_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
ThreadMethodEntry thread_find_method_meta(const char* name);

// Channel
void channel_init_methods(void);
void channel_mark_methods(void);
void channel_register_method_with_params(const char* name, ObjNative* method, int arity,
                                          int min_arity, int max_arity,
                                          TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
ChannelMethodEntry channel_find_method_meta(const char* name);

// Number
void number_init_methods(void);
void number_mark_methods(void);
void number_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);
NumberMethodEntry number_find_method_meta(const char* name);

// ============================================================================
// 协程系统 API
// ============================================================================

// 创建协程对象
ObjCoroutine* coroutine_new(ObjClosure* closure);

// 创建 Future 对象
ObjFuture* future_new(void);

// 完成 Future 并设置结果
void future_complete(ObjFuture* future, Value result);

// 完成 Future 并设置错误
void future_fail(ObjFuture* future, Value error);

// 初始化事件循环
void event_loop_init(EventLoop* loop);

// 运行事件循环直到所有协程完成
void event_loop_run(EventLoop* loop);

// 停止事件循环
void event_loop_stop(EventLoop* loop);

// 添加定时器
void event_loop_add_timer(EventLoop* loop, uint64_t wake_time, ObjCoroutine* coroutine);

// 添加协程到就绪队列
void event_loop_add_ready(EventLoop* loop, ObjCoroutine* coroutine);

// 获取当前时间（毫秒）
uint64_t current_time_ms(void);

// 休眠指定毫秒
void sleep_ms(uint64_t ms);

// 全局事件循环（在 VM 中使用）
extern EventLoop* g_event_loop;

#endif // LENO_VALUE_H
