#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdlib.h>
#include <string.h>

// 数组操作
ObjArray* arr_new(int capacity) {
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return NULL;
    
    arr->elements = (Value*)malloc(capacity * sizeof(Value));
    if (!arr->elements) {
        native_throw_error("数组内存分配失败");
        return NULL;
    }
    
    arr->count = 0;
    arr->capacity = capacity;
    arr->type_info = NULL;  // 运行时类型信息初始为 NULL
    
    // 初始化为 null
    for (int i = 0; i < capacity; i++) {
        arr->elements[i] = val_null();
    }
    
    return arr;
}

// 注意：arr_read, arr_write 已在 leno_value.h 中内联

// 数组扩容 - 使用简单的2倍增长策略
// 返回1成功, 0失败
int arr_grow(ObjArray* arr) {
    if (!arr) return 0;

    int new_capacity = arr->capacity < 8 ? 8 : arr->capacity * 2;

    size_t old_size = (size_t)arr->capacity * sizeof(Value);
    size_t new_size = (size_t)new_capacity * sizeof(Value);

    Value* new_elements = (Value*)realloc(arr->elements, new_size);
    if (!new_elements) {
        native_throw_error("数组内存分配失败");
        return 0;
    }

    gc_track_memory((Object*)arr, old_size, new_size);

    // 初始化新分配的空间
    for (int i = arr->capacity; i < new_capacity; i++) {
        new_elements[i] = val_null();
    }

    arr->elements = new_elements;
    arr->capacity = new_capacity;
    return 1;
}

// ============================================================================
// 数组方法注册表（哈希表实现 - O(1) 查找）
// ============================================================================

#define ARRAY_METHOD_TABLE_INITIAL_CAPACITY 32
#define ARRAY_METHOD_TABLE_MAX_LOAD 0.75

// 计算字符串哈希值（FNV-1a算法）
static uint32_t array_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

typedef struct ArrayMethodHashEntry {
    char* name;                    // 方法名（动态分配）
    ObjNative* method;             // 方法实现
    int arity;                     // 参数个数
    TypeKind return_type;          // 返回类型
    TypeKind return_element_type;  // 返回元素类型
    TypeKind param_types[MAX_METHOD_PARAMS]; // 参数类型数组
    struct ArrayMethodHashEntry* next;  // 链式冲突处理
} ArrayMethodHashEntry;

typedef struct {
    ArrayMethodHashEntry** entries;  // 哈希表数组
    int capacity;                    // 表容量
    int count;                       // 当前方法数量
} ArrayMethodTable;

static THREAD_LOCAL ArrayMethodTable arrayMethodTable = {NULL, 0, 0};

// 初始化数组方法表
static void array_method_table_init(void) {
    arrayMethodTable.capacity = ARRAY_METHOD_TABLE_INITIAL_CAPACITY;
    arrayMethodTable.count = 0;
    arrayMethodTable.entries = (ArrayMethodHashEntry**)calloc(arrayMethodTable.capacity, sizeof(ArrayMethodHashEntry*));
}

// 释放数组方法表
static void array_method_table_free(void) {
    if (!arrayMethodTable.entries) return;
    
    for (int i = 0; i < arrayMethodTable.capacity; i++) {
        ArrayMethodHashEntry* entry = arrayMethodTable.entries[i];
        while (entry) {
            ArrayMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(arrayMethodTable.entries);
    arrayMethodTable.entries = NULL;
    arrayMethodTable.capacity = 0;
    arrayMethodTable.count = 0;
}

// 扩容数组方法表
static void array_method_table_resize(void) {
    int old_capacity = arrayMethodTable.capacity;
    ArrayMethodHashEntry** old_entries = arrayMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    ArrayMethodHashEntry** new_entries = (ArrayMethodHashEntry**)calloc(new_capacity, sizeof(ArrayMethodHashEntry*));
    if (!new_entries) return;
    
    // 重新哈希所有条目
    for (int i = 0; i < old_capacity; i++) {
        ArrayMethodHashEntry* entry = old_entries[i];
        while (entry) {
            ArrayMethodHashEntry* next = entry->next;
            uint32_t hash = array_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    arrayMethodTable.entries = new_entries;
    arrayMethodTable.capacity = new_capacity;
}

// 注册数组方法
void array_register_method(const char* name, ObjNative* method, int arity, TypeKind return_type) {
    if (!arrayMethodTable.entries) {
        array_method_table_init();
    }
    
    // 检查是否需要扩容
    if (arrayMethodTable.count >= arrayMethodTable.capacity * ARRAY_METHOD_TABLE_MAX_LOAD) {
        array_method_table_resize();
    }
    
    // 计算哈希索引
    uint32_t hash = array_hash_string(name);
    int index = hash & (arrayMethodTable.capacity - 1);
    
    // 检查是否已存在
    ArrayMethodHashEntry* entry = arrayMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // 已存在，更新
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                entry->param_types[i] = TYPE_ANY;
            }
            return;
        }
        entry = entry->next;
    }
    
    // 创建新条目
    ArrayMethodHashEntry* new_entry = (ArrayMethodHashEntry*)malloc(sizeof(ArrayMethodHashEntry));
    if (!new_entry) {
        native_throw_error("数组方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        new_entry->param_types[i] = TYPE_ANY;
    }
    
    // 插入到哈希表
    new_entry->next = arrayMethodTable.entries[index];
    arrayMethodTable.entries[index] = new_entry;
    arrayMethodTable.count++;
}

// 注册数组方法（带参数类型）
void array_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!arrayMethodTable.entries) {
        array_method_table_init();
    }
    
    // 检查是否需要扩容
    if (arrayMethodTable.count >= arrayMethodTable.capacity * ARRAY_METHOD_TABLE_MAX_LOAD) {
        array_method_table_resize();
    }
    
    // 计算哈希索引
    uint32_t hash = array_hash_string(name);
    int index = hash & (arrayMethodTable.capacity - 1);

    // 检查是否已存在
    ArrayMethodHashEntry* entry = arrayMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            // 已存在，更新
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            }
            return;
        }
        entry = entry->next;
    }
    
    // 创建新条目
    ArrayMethodHashEntry* new_entry = (ArrayMethodHashEntry*)malloc(sizeof(ArrayMethodHashEntry));
    if (!new_entry) {
        native_throw_error("数组方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    
    // 复制参数类型
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    }
    
    // 插入到哈希表
    new_entry->next = arrayMethodTable.entries[index];
    arrayMethodTable.entries[index] = new_entry;
    arrayMethodTable.count++;
    
    // 同时注册到编译期元信息表，避免重复维护
    native_register_instance_method_meta_with_params("array", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 获取数组方法的参数类型
TypeKind array_get_method_param_type(const char* method_name, int param_index) {
    if (!arrayMethodTable.entries || arrayMethodTable.count == 0) return TYPE_ANY;
    
    uint32_t hash = array_hash_string(method_name);
    int index = hash & (arrayMethodTable.capacity - 1);
    
    ArrayMethodHashEntry* entry = arrayMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, method_name) == 0) {
            if (param_index >= 0 && param_index < entry->arity && param_index < MAX_METHOD_PARAMS) {
                return entry->param_types[param_index];
            }
            break;
        }
        entry = entry->next;
    }
    return TYPE_ANY;
}

// 查找数组方法（O(1)）
ObjNative* array_find_method(const char* name) {
    if (!arrayMethodTable.entries || arrayMethodTable.count == 0) return NULL;
    
    uint32_t hash = array_hash_string(name);
    int index = hash & (arrayMethodTable.capacity - 1);
    
    ArrayMethodHashEntry* entry = arrayMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 查找数组方法的元信息（用于编译期类型检查）
ArrayMethodEntry array_find_method_meta(const char* name) {
    ArrayMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    
    if (!arrayMethodTable.entries || arrayMethodTable.count == 0) return result;
    
    uint32_t hash = array_hash_string(name);
    int index = hash & (arrayMethodTable.capacity - 1);
    
    ArrayMethodHashEntry* entry = arrayMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            result.name = entry->name;
            result.method = entry->method;
            result.arity = entry->arity;
            result.return_type = entry->return_type;
            result.return_element_type = entry->return_element_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                result.param_types[i] = entry->param_types[i];
            }
            return result;
        }
        entry = entry->next;
    }
    return result;
}

// 初始化数组方法表
void array_init_methods(void) {
    array_method_table_free();
    array_method_table_init();
}

// 标记所有数组方法对象（供 GC 使用）
void array_mark_methods(void) {
    if (!arrayMethodTable.entries) return;
    
    for (int i = 0; i < arrayMethodTable.capacity; i++) {
        ArrayMethodHashEntry* entry = arrayMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
