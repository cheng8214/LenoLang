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

// (2026-10-02) 此处原有 `MethodEntry` 结构（find_method_meta 的返回值）—— 随该家族删除 ✓

// 通用方法哈希表条目（内部使用）
// ⚠ (2026-10-02) 这里原有 `param_types[MAX_METHOD_PARAMS]` —— 它曾与**编译期元信息表**
//   （native.c 的 instanceMethodTable，检查器真正读的那份）**平行保存**同一批类型，形成"两处闸"：
//   查表判据各写一遍、修一处漏一处。其消费者（method_table_get_param_type / *_find_method_meta）
//   经查**零调用点** ⇒ 已删除该家族，本字段随之删掉，类型只在编译期元信息表里存一份 ✓
typedef struct MethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    int min_arity;
    int max_arity;
    TypeKind return_type;
    TypeKind return_element_type;
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

// 注册方法（**唯一入口**，2026-10-02 统一；参数规格用 native.h 的 NativeParamSpec + 构造宏：
//   NATIVE_FIXED(types) / NATIVE_FIXED_NONE(n) / NATIVE_VARARG(min,max,cnt,types,tail)）
//   同时登记运行期条目与编译期元信息 —— 规格一次给全，不存在"第二步"可漏 ✓
void method_table_register_method(MethodTable* table, const char* type_name, const char* name,
                                  ObjNative* method, TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);

// 查找方法（O(1)，返回方法对象指针）
ObjNative* method_table_find(MethodTable* table, const char* name);



// 获取方法参数类型
TypeKind method_table_get_param_type(MethodTable* table, const char* method_name, int param_index);

// 初始化方法表（free + init，用于线程初始化）
void method_table_init_methods(MethodTable* table, int initial_capacity);

// 标记所有方法对象（供 GC 使用）
void method_table_mark(MethodTable* table);

// ============================================================================
// (2026-10-02) 此处原有 9 个 `typedef MethodEntry XMethodEntry;` 向后兼容别名
//   —— 它们只服务于 `*_find_method_meta` 家族的返回值，而该家族（含 `MethodEntry`）
//   经查**零调用点** ⇒ 一并删除 ✓
// ============================================================================

#endif // METHOD_TABLE_H
