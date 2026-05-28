#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <string.h>

// ============================================================================
// 数字方法注册表（哈希表实现 - O(1) 查找，线程局部）
// ============================================================================

#define NUMBER_METHOD_TABLE_INITIAL_CAPACITY 16
#define NUMBER_METHOD_TABLE_MAX_LOAD 0.75

// 计算字符串哈希值（FNV-1a算法）
static uint32_t number_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

typedef struct NumberMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct NumberMethodHashEntry* next;
} NumberMethodHashEntry;

typedef struct {
    NumberMethodHashEntry** entries;
    int capacity;
    int count;
} NumberMethodTable;

static THREAD_LOCAL NumberMethodTable numberMethodTable = {NULL, 0, 0};

// 初始化数字方法表
static void number_method_table_init(void) {
    numberMethodTable.capacity = NUMBER_METHOD_TABLE_INITIAL_CAPACITY;
    numberMethodTable.count = 0;
    numberMethodTable.entries = (NumberMethodHashEntry**)calloc(numberMethodTable.capacity, sizeof(NumberMethodHashEntry*));
}

// 释放数字方法表
static void number_method_table_free(void) {
    if (!numberMethodTable.entries) return;
    
    for (int i = 0; i < numberMethodTable.capacity; i++) {
        NumberMethodHashEntry* entry = numberMethodTable.entries[i];
        while (entry) {
            NumberMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(numberMethodTable.entries);
    numberMethodTable.entries = NULL;
    numberMethodTable.capacity = 0;
    numberMethodTable.count = 0;
}

// 扩容数字方法表
static void number_method_table_resize(void) {
    int old_capacity = numberMethodTable.capacity;
    NumberMethodHashEntry** old_entries = numberMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    NumberMethodHashEntry** new_entries = (NumberMethodHashEntry**)calloc(new_capacity, sizeof(NumberMethodHashEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        NumberMethodHashEntry* entry = old_entries[i];
        while (entry) {
            NumberMethodHashEntry* next = entry->next;
            uint32_t hash = number_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    numberMethodTable.entries = new_entries;
    numberMethodTable.capacity = new_capacity;
}

void number_register_method(const char* name, ObjNative* method, int arity, TypeKind return_type) {
    if (!numberMethodTable.entries) {
        number_method_table_init();
    }
    
    if (numberMethodTable.count >= numberMethodTable.capacity * NUMBER_METHOD_TABLE_MAX_LOAD) {
        number_method_table_resize();
    }
    
    uint32_t hash = number_hash_string(name);
    int index = hash & (numberMethodTable.capacity - 1);
    
    NumberMethodHashEntry* entry = numberMethodTable.entries[index];
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
    
    NumberMethodHashEntry* new_entry = (NumberMethodHashEntry*)malloc(sizeof(NumberMethodHashEntry));
    if (!new_entry) {
        native_throw_error("数字方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        new_entry->param_types[i] = TYPE_ANY;
    }
    
    new_entry->next = numberMethodTable.entries[index];
    numberMethodTable.entries[index] = new_entry;
    numberMethodTable.count++;
}

void number_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind* param_types) {
    if (!numberMethodTable.entries) {
        number_method_table_init();
    }
    
    if (numberMethodTable.count >= numberMethodTable.capacity * NUMBER_METHOD_TABLE_MAX_LOAD) {
        number_method_table_resize();
    }
    
    uint32_t hash = number_hash_string(name);
    int index = hash & (numberMethodTable.capacity - 1);
    
    NumberMethodHashEntry* entry = numberMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
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
    
    NumberMethodHashEntry* new_entry = (NumberMethodHashEntry*)malloc(sizeof(NumberMethodHashEntry));
    if (!new_entry) {
        native_throw_error("数字方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    
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
    
    new_entry->next = numberMethodTable.entries[index];
    numberMethodTable.entries[index] = new_entry;
    numberMethodTable.count++;
    
    native_register_instance_method_meta_with_params("number", name, arity, min_arity, max_arity, return_type, param_types);
}

// 查找数字方法（O(1)）
ObjNative* number_find_method(const char* name) {
    if (!numberMethodTable.entries || numberMethodTable.count == 0) return NULL;
    
    uint32_t hash = number_hash_string(name);
    int index = hash & (numberMethodTable.capacity - 1);
    
    NumberMethodHashEntry* entry = numberMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

void number_init_methods(void) {
    number_method_table_free();
    number_method_table_init();
}

void number_mark_methods(void) {
    if (!numberMethodTable.entries) return;
    
    for (int i = 0; i < numberMethodTable.capacity; i++) {
        NumberMethodHashEntry* entry = numberMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
