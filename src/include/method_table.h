#ifndef METHOD_TABLE_H
#define METHOD_TABLE_H

// ============================================================================
// 通用方法注册表（哈希表实现 - O(1) 查找）
// 所有内置类型（Array/Dict/String/File/Socket/Struct/CStruct/Thread/Channel/Number）
// 共享此基础设施，消除 ~1200 行重复代码
// ============================================================================

// 注意：此头文件必须在 ObjNative 和 TypeKind 定义之后引入
// （通常由 leno_value.h 或 lenolang.h 提供）

#ifndef MAX_METHOD_PARAMS
#define MAX_METHOD_PARAMS 8
#endif

// 方法元信息（用于编译期类型检查，也作为 find_method_meta 的返回值）
typedef struct {
    const char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
} MethodEntry;

// 通用方法哈希表条目（内部使用）
typedef struct MethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    int min_arity;
    int max_arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct MethodHashEntry* next;
} MethodHashEntry;

// 通用方法哈希表
typedef struct {
    MethodHashEntry** entries;
    int capacity;
    int count;
} MethodTable;

// ============================================================================
// 通用方法表操作函数
// ============================================================================

// 初始化方法表（指定初始容量）
void method_table_init(MethodTable* table, int initial_capacity);

// 释放方法表
void method_table_free(MethodTable* table);

// 扩容方法表（内部使用，负载因子 0.75）
void method_table_resize(MethodTable* table);

// 注册方法（含参数类型信息，同时注册编译期元信息）
void method_table_register_with_params(MethodTable* table, const char* type_name,
                                        const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type,
                                        TypeKind* param_types);

// 查找方法（O(1)，返回方法对象指针）
ObjNative* method_table_find(MethodTable* table, const char* name);

// 查找方法元信息（返回 MethodEntry 结构体）
MethodEntry method_table_find_meta(MethodTable* table, const char* name);

// 获取方法参数类型
TypeKind method_table_get_param_type(MethodTable* table, const char* method_name, int param_index);

// 初始化方法表（free + init，用于线程初始化）
void method_table_init_methods(MethodTable* table, int initial_capacity);

// 标记所有方法对象（供 GC 使用）
void method_table_mark(MethodTable* table);

// ============================================================================
// 类型别名 - 保持向后兼容
// ============================================================================

typedef MethodEntry ArrayMethodEntry;
typedef MethodEntry StringMethodEntry;
typedef MethodEntry FileMethodEntry;
typedef MethodEntry DictMethodEntry;
typedef MethodEntry StructMethodEntry;
typedef MethodEntry CStructMethodEntry;
typedef MethodEntry ThreadMethodEntry;
typedef MethodEntry ChannelMethodEntry;
typedef MethodEntry NumberMethodEntry;

#endif // METHOD_TABLE_H
