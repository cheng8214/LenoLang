#include "include/lenolang.h"
#include "include/native.h"
#include "include/leno_hash.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 通用方法注册表实现
// ============================================================================

#define METHOD_TABLE_MAX_LOAD 0.75

// 初始化方法表
void method_table_init(MethodTable* table, int initial_capacity) {
    table->capacity = initial_capacity;
    table->count = 0;
    table->entries = (MethodHashEntry**)calloc(table->capacity, sizeof(MethodHashEntry*));
}

// 释放方法表
void method_table_free(MethodTable* table) {
    if (!table->entries) return;

    for (int i = 0; i < table->capacity; i++) {
        MethodHashEntry* entry = table->entries[i];
        while (entry) {
            MethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

// 扩容方法表
void method_table_resize(MethodTable* table) {
    int old_capacity = table->capacity;
    MethodHashEntry** old_entries = table->entries;

    int new_capacity = old_capacity * 2;
    MethodHashEntry** new_entries = (MethodHashEntry**)calloc(new_capacity, sizeof(MethodHashEntry*));
    if (!new_entries) return;

    for (int i = 0; i < old_capacity; i++) {
        MethodHashEntry* entry = old_entries[i];
        while (entry) {
            MethodHashEntry* next = entry->next;
            uint32_t hash = leno_fnv1a(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }

    free(old_entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
}

// 注册方法（含参数类型信息）
void method_table_register_with_params(MethodTable* table, const char* type_name,
                                        const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type,
                                        TypeKind* param_types) {
    if (!table->entries) {
        method_table_init(table, 32);
    }

    if (table->count >= table->capacity * METHOD_TABLE_MAX_LOAD) {
        method_table_resize(table);
    }

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    // 检查是否已存在
    MethodHashEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->min_arity = min_arity;
            entry->max_arity = max_arity;
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
            // 同时注册到编译期元信息表
            native_register_instance_method_meta_with_params(type_name, name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
            return;
        }
        entry = entry->next;
    }

    // 创建新条目
    MethodHashEntry* new_entry = (MethodHashEntry*)malloc(sizeof(MethodHashEntry));
    if (!new_entry) return;

    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->min_arity = min_arity;
    new_entry->max_arity = max_arity;
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

    new_entry->next = table->entries[index];
    table->entries[index] = new_entry;
    table->count++;

    // 同时注册到编译期元信息表
    native_register_instance_method_meta_with_params(type_name, name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找方法（O(1)）
ObjNative* method_table_find(MethodTable* table, const char* name) {
    if (!table->entries || table->count == 0) return NULL;

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 查找方法元信息
MethodEntry method_table_find_meta(MethodTable* table, const char* name) {
    MethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};

    if (!table->entries || table->count == 0) return result;

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
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

// 获取方法参数类型
TypeKind method_table_get_param_type(MethodTable* table, const char* method_name, int param_index) {
    if (!table->entries || table->count == 0) return TYPE_ANY;

    uint32_t hash = leno_fnv1a(method_name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
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

// 初始化方法表（free + init）
void method_table_init_methods(MethodTable* table, int initial_capacity) {
    method_table_free(table);
    method_table_init(table, initial_capacity);
}

// 标记所有方法对象（供 GC 使用）
void method_table_mark(MethodTable* table) {
    if (!table->entries) return;

    for (int i = 0; i < table->capacity; i++) {
        MethodHashEntry* entry = table->entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
