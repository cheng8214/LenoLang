#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// 用于标记已删除的哨兵键（线程局部存储，支持多线程）
_Thread_local ObjString* tombstone = NULL;

// 获取字符串的哈希值（使用 ObjString 中缓存的哈希）
static inline uint32_t dict_get_hash(ObjString* key) {
    return key->hash;
}

// 查找条目：返回键的位置或应该插入的位置
// 返回的位置保证：如果是已存在的键，返回该键的位置；如果是新键，返回空槽或 tombstone 位置
static int dict_find_slot(ObjDict* dict, ObjString* key) {
    if (dict->capacity == 0) return -1;
    
    uint32_t hash = dict_get_hash(key);
    uint32_t index = hash & (dict->capacity - 1);  // 快速取模（容量必须是 2 的幂）
    int first_tombstone = -1;
    
    while (1) {
        ObjString* current_key = dict->entries[index].key;
        
        if (current_key == NULL) {
            // 找到空槽：如果是之前遇到 tombstone，使用 tombstone 位置；否则使用空槽
            return first_tombstone != -1 ? first_tombstone : (int)index;
        }
        
        if (current_key == tombstone) {
            // 遇到 tombstone，记录第一个 tombstone 位置
            if (first_tombstone == -1) {
                first_tombstone = (int)index;
            }
        } else if (current_key->len == key->len && 
                   current_key->hash == key->hash &&
                   memcmp(current_key->chars, key->chars, key->len) == 0) {
            // 找到匹配的键
            return (int)index;
        }
        
        // 线性探测下一个槽
        index = (index + 1) & (dict->capacity - 1);
    }
}

// 查找条目：返回键的位置，如果不存在返回 -1
static int dict_find_entry(ObjDict* dict, ObjString* key) {
    if (dict->capacity == 0) return -1;
    
    uint32_t hash = dict_get_hash(key);
    uint32_t index = hash & (dict->capacity - 1);
    
    while (1) {
        ObjString* current_key = dict->entries[index].key;
        
        if (current_key == NULL) {
            return -1;  // 遇到空槽，键不存在
        }
        
        if (current_key != tombstone &&
            current_key->len == key->len && 
            current_key->hash == key->hash &&
            memcmp(current_key->chars, key->chars, key->len) == 0) {
            return (int)index;
        }
        
        index = (index + 1) & (dict->capacity - 1);
    }
}

// 调整哈希表大小并重新哈希所有条目
static void dict_resize(ObjDict* dict, int new_capacity) {
    // 初始化 tombstone 哨兵（如果还没有）
    if (tombstone == NULL) {
        tombstone = str_new("", 0);
    }
    
    ObjDictEntry* old_entries = dict->entries;
    int old_capacity = dict->capacity;
    
    // 分配新表
    dict->entries = (ObjDictEntry*)malloc(new_capacity * sizeof(ObjDictEntry));
    if (!dict->entries) {
        native_throw_error("字典扩容内存分配失败");
        dict->entries = old_entries;  // 恢复原表
        return;
    }
    
    // 初始化新表
    for (int i = 0; i < new_capacity; i++) {
        dict->entries[i].key = NULL;
        dict->entries[i].value = val_null();
    }
    
    dict->capacity = new_capacity;
    dict->count = 0;
    dict->tombstone_count = 0;
    
    // 重新插入所有旧条目
    if (old_entries != NULL) {
        for (int i = 0; i < old_capacity; i++) {
            ObjString* key = old_entries[i].key;
            if (key != NULL && key != tombstone) {
                int index = dict_find_slot(dict, key);
                dict->entries[index].key = key;
                dict->entries[index].value = old_entries[i].value;
                dict->count++;
            }
        }
        free(old_entries);
    }
}

// 检查字符串键是否是纯数字，返回整数索引或 -1
static int key_to_array_index(ObjString* key) {
    if (!key || key->len == 0) return -1;
    for (int i = 0; i < key->len; i++) {
        if (key->chars[i] < '0' || key->chars[i] > '9') return -1;
    }
    int index = 0;
    for (int i = 0; i < key->len; i++) {
        index = index * 10 + (key->chars[i] - '0');
    }
    return index;
}

// 数组部分扩容
static int dict_array_resize(ObjDict* dict, int new_size) {
    if (new_size <= dict->asize) return 1;
    int capacity = 4;
    while (capacity < new_size) capacity *= 2;
    Value* new_array = (Value*)realloc(dict->array, capacity * sizeof(Value));
    if (!new_array) return 0;
    for (int i = dict->asize; i < capacity; i++) {
        new_array[i] = val_null();
    }
    dict->array = new_array;
    dict->asize = capacity;
    return 1;
}

ObjDict* dict_new(int capacity) {
    // 确保容量是 2 的幂
    int initial_capacity = 8;
    while (initial_capacity < capacity) {
        initial_capacity *= 2;
    }
    
    ObjDict* dict = (ObjDict*)gc_alloc(sizeof(ObjDict), OBJ_DICT);
    if (!dict) return NULL;
    
    // 初始化数组部分
    dict->array = NULL;
    dict->asize = 0;
    dict->acount = 0;
    
    // 初始化哈希部分
    dict->entries = NULL;
    dict->count = 0;
    dict->capacity = 0;
    dict->tombstone_count = 0;
    
    // 默认分配数组空间
    dict_array_resize(dict, 4);
    
    dict_resize(dict, initial_capacity);

    if (!dict->entries) {
        return NULL;
    }

    // 初始化类型信息为 NULL（表示未指定泛型类型）
    dict->type_info = NULL;

    // 初始化插入顺序数组
    dict->order = NULL;
    dict->order_count = 0;
    dict->order_capacity = 0;

    return dict;
}

// 添加键到插入顺序数组（仅用于新键）
// 调用者必须确保键是新插入的（不存在于字典中）
static void dict_add_to_order(ObjDict* dict, ObjString* key) {
    // 扩容
    if (dict->order_count >= dict->order_capacity) {
        int new_capacity = dict->order_capacity < 8 ? 8 : dict->order_capacity * 2;
        ObjString** new_order = (ObjString**)realloc(dict->order, new_capacity * sizeof(ObjString*));
        if (!new_order) return;
        dict->order = new_order;
        dict->order_capacity = new_capacity;
    }

    // 直接追加到末尾 O(1)
    dict->order[dict->order_count++] = key;
}

void dict_set(ObjDict* dict, ObjString* key, Value value) {
    if (!dict || !key) return;

    // 检查是否是纯数字键，如果是优先使用数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0) {
        // 纯数字键，尝试存储在数组部分
        if (array_index >= dict->asize) {
            // 需要扩容数组
            // 如果新索引太大（稀疏），直接存哈希表
            if (dict->asize > 0 && array_index > dict->asize * 2 && array_index > 64) {
                goto use_hash;  // 太稀疏，使用哈希表
            }
            if (!dict_array_resize(dict, array_index + 1)) {
                goto use_hash;  // 扩容失败，回退到哈希表
            }
        }
        // 存储到数组
        int is_new = val_is_null(dict->array[array_index]);
        if (is_new) {
            dict->acount++;
            // 数字键也需要记录插入顺序
            dict_add_to_order(dict, key);
        }
        dict->array[array_index] = value;
        gc_write_barrier((Object*)dict, value);
        return;
    }

use_hash:
    // 非数字键或数组扩容失败：使用哈希部分

    // 检查是否需要扩容
    int total_entries = dict->count + dict->tombstone_count;
    if (total_entries + 1 > dict->capacity * DICT_MAX_LOAD) {
        int new_capacity = dict->capacity < 8 ? 8 : dict->capacity * 2;
        dict_resize(dict, new_capacity);
    }

    int index = dict_find_slot(dict, key);

    if (index < 0 || index >= dict->capacity) return;

    ObjString* existing_key = dict->entries[index].key;

    if (existing_key == NULL) {
        // 新键
        dict->entries[index].key = key;
        dict->entries[index].value = value;
        gc_write_barrier((Object*)dict, value);
        dict->count++;
        dict_add_to_order(dict, key);  // 添加到插入顺序
    } else if (existing_key == tombstone) {
        // 复用 tombstone
        dict->entries[index].key = key;
        dict->entries[index].value = value;
        gc_write_barrier((Object*)dict, value);
        dict->count++;
        dict->tombstone_count--;
        dict_add_to_order(dict, key);  // 添加到插入顺序
    } else {
        // 更新现有键
        dict->entries[index].value = value;
        gc_write_barrier((Object*)dict, value);
    }
}

// 验证返回的对象是否为野指针（type 字段不在有效范围内）
static inline int is_valid_obj_value(Value v) {
    if (!val_is_obj(v)) return 1;
    Object* obj = val_as_obj(v);
    return obj->type >= OBJ_STRING && obj->type < OBJ_NONE;
}

Value dict_get(ObjDict* dict, ObjString* key) {
    if (!dict || !key) return val_null();
    
    // 先检查数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        Value value = dict->array[array_index];
        if (!val_is_null(value)) {
            if (!is_valid_obj_value(value)) return val_null();
            return value;
        }
        // 如果是 null，可能实际存的是 null，也可能在哈希表中
        // 继续查哈希表确认
    }
    
    // 查哈希部分
    if (dict->capacity == 0) return val_null();
    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        Value value = dict->entries[index].value;
        if (!is_valid_obj_value(value)) return val_null();
        return value;
    }
    return val_null();
}

int dict_has(ObjDict* dict, ObjString* key) {
    if (!dict || !key) return 0;
    
    // 先检查数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        if (!val_is_null(dict->array[array_index])) {
            return 1;
        }
    }
    
    // 再检查哈希部分
    if (dict->capacity == 0) return 0;
    return dict_find_entry(dict, key) >= 0;
}

// 从 order 数组移除指定 key（用字符串内容比较，避免指针比较被 GC 野指针坑）
static void dict_remove_from_order(ObjDict* dict, ObjString* key) {
    for (int i = 0; i < dict->order_count; i++) {
        ObjString* entry = dict->order[i];
        if (entry != NULL && entry->len == key->len &&
            entry->hash == key->hash &&
            memcmp(entry->chars, key->chars, key->len) == 0) {
            // swap-remove: 用最后一个元素替换当前位置
            dict->order[i] = dict->order[dict->order_count - 1];
            dict->order_count--;
            return;
        }
    }
}

void dict_delete(ObjDict* dict, ObjString* key) {
    if (!dict || !key) return;
    
    // 先尝试从数组部分删除
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        if (!val_is_null(dict->array[array_index])) {
            dict->array[array_index] = val_null();
            dict->acount--;
            dict_remove_from_order(dict, key);
            return;
        }
    }
    
    // 从哈希部分删除
    if (dict->capacity == 0) return;
    
    // 初始化 tombstone 哨兵
    if (tombstone == NULL) {
        tombstone = str_new("", 0);
    }
    
    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        dict->entries[index].key = tombstone;
        dict->entries[index].value = val_null();
        dict->count--;
        dict->tombstone_count++;
        dict_remove_from_order(dict, key);
    }
}

// 注意：dict_get_key_by_index 已在 leno_value.h 中内联

// 按索引获取字典值（用于for循环迭代时同时获取值）
// 使用 order 数组保持插入顺序
Value dict_get_value_by_index(ObjDict* dict, int index) {
    if (!dict || index < 0) return val_null();
    
    // 使用 order 数组获取键，然后通过键获取值
    if (dict->order && index < dict->order_count) {
        ObjString* key = dict->order[index];
        return dict_get(dict, key);
    }
    
    return val_null();
}

// ============================================================================
// 字典实例方法支持（哈希表实现 - O(1) 查找）
// ============================================================================

#define DICT_METHOD_TABLE_INITIAL_CAPACITY 32
#define DICT_METHOD_TABLE_MAX_LOAD 0.75

// 计算字符串哈希值（FNV-1a算法）
static uint32_t dict_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}
/* 方法哈希表条目 */
typedef struct DictMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct DictMethodHashEntry* next;
} DictMethodHashEntry;

typedef struct {
    DictMethodHashEntry** entries;
    int capacity;
    int count;
} DictMethodTable;

static THREAD_LOCAL DictMethodTable dictMethodTable = {NULL, 0, 0};

// 初始化字典方法表
static void dict_method_table_init(void) {
    dictMethodTable.capacity = DICT_METHOD_TABLE_INITIAL_CAPACITY;
    dictMethodTable.count = 0;
    dictMethodTable.entries = (DictMethodHashEntry**)calloc(dictMethodTable.capacity, sizeof(DictMethodHashEntry*));
}

// 释放字典方法表
static void dict_method_table_free(void) {
    if (!dictMethodTable.entries) return;
    
    for (int i = 0; i < dictMethodTable.capacity; i++) {
        DictMethodHashEntry* entry = dictMethodTable.entries[i];
        while (entry) {
            DictMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(dictMethodTable.entries);
    dictMethodTable.entries = NULL;
    dictMethodTable.capacity = 0;
    dictMethodTable.count = 0;
}

// 扩容字典方法表
static void dict_method_table_resize(void) {
    int old_capacity = dictMethodTable.capacity;
    DictMethodHashEntry** old_entries = dictMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    DictMethodHashEntry** new_entries = (DictMethodHashEntry**)calloc(new_capacity, sizeof(DictMethodHashEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        DictMethodHashEntry* entry = old_entries[i];
        while (entry) {
            DictMethodHashEntry* next = entry->next;
            uint32_t hash = dict_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    dictMethodTable.entries = new_entries;
    dictMethodTable.capacity = new_capacity;
}

void dict_register_method(const char* name, ObjNative* method, int arity, TypeKind return_type) {
    if (!dictMethodTable.entries) {
        dict_method_table_init();
    }
    
    if (dictMethodTable.count >= dictMethodTable.capacity * DICT_METHOD_TABLE_MAX_LOAD) {
        dict_method_table_resize();
    }
    
    uint32_t hash = dict_hash_string(name);
    int index = hash & (dictMethodTable.capacity - 1);
    
    DictMethodHashEntry* entry = dictMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
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
    
    DictMethodHashEntry* new_entry = (DictMethodHashEntry*)malloc(sizeof(DictMethodHashEntry));
    if (!new_entry) {
        native_throw_error("字典方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        new_entry->param_types[i] = TYPE_ANY;
    }
    
    new_entry->next = dictMethodTable.entries[index];
    dictMethodTable.entries[index] = new_entry;
    dictMethodTable.count++;
}

void dict_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!dictMethodTable.entries) {
        dict_method_table_init();
    }
    
    if (dictMethodTable.count >= dictMethodTable.capacity * DICT_METHOD_TABLE_MAX_LOAD) {
        dict_method_table_resize();
    }
    
    uint32_t hash = dict_hash_string(name);
    int index = hash & (dictMethodTable.capacity - 1);
    
    DictMethodHashEntry* entry = dictMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
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
            // 同时更新编译期元信息表
            native_register_instance_method_meta_with_params("dict", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
            return;
        }
        entry = entry->next;
    }
    
    DictMethodHashEntry* new_entry = (DictMethodHashEntry*)malloc(sizeof(DictMethodHashEntry));
    if (!new_entry) {
        native_throw_error("字典方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
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
    
    new_entry->next = dictMethodTable.entries[index];
    dictMethodTable.entries[index] = new_entry;
    dictMethodTable.count++;
  /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("dict", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找字典方法（O(1)）
ObjNative* dict_find_method(const char* name) {
    if (!dictMethodTable.entries || dictMethodTable.count == 0) return NULL;
    
    uint32_t hash = dict_hash_string(name);
    int index = hash & (dictMethodTable.capacity - 1);
    
    DictMethodHashEntry* entry = dictMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

void dict_init_methods(void) {
    dict_method_table_free();
    dict_method_table_init();
}

void dict_mark_methods(void) {
    if (!dictMethodTable.entries) return;
    
    for (int i = 0; i < dictMethodTable.capacity; i++) {
        DictMethodHashEntry* entry = dictMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}

/* 查找 Dict 方法的元信息（编译期类型检查） */
DictMethodEntry dict_find_method_meta(const char* name) {
    DictMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    
    if (!dictMethodTable.entries || dictMethodTable.count == 0) return result;
    
    uint32_t hash = dict_hash_string(name);
    int index = hash & (dictMethodTable.capacity - 1);
    
    DictMethodHashEntry* entry = dictMethodTable.entries[index];
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
