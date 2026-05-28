#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 文件方法注册表（哈希表实现 - O(1) 查找，线程局部）
// ============================================================================

#define FILE_METHOD_TABLE_INITIAL_CAPACITY 16
#define FILE_METHOD_TABLE_MAX_LOAD 0.75

// 计算字符串哈希值（FNV-1a算法）
static uint32_t file_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

typedef struct FileMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct FileMethodHashEntry* next;
} FileMethodHashEntry;

typedef struct {
    FileMethodHashEntry** entries;
    int capacity;
    int count;
} FileMethodTable;

static THREAD_LOCAL FileMethodTable fileMethodTable = {NULL, 0, 0};

// 初始化文件方法表
static void file_method_table_init(void) {
    fileMethodTable.capacity = FILE_METHOD_TABLE_INITIAL_CAPACITY;
    fileMethodTable.count = 0;
    fileMethodTable.entries = (FileMethodHashEntry**)calloc(fileMethodTable.capacity, sizeof(FileMethodHashEntry*));
}

// 释放文件方法表
static void file_method_table_free(void) {
    if (!fileMethodTable.entries) return;
    
    for (int i = 0; i < fileMethodTable.capacity; i++) {
        FileMethodHashEntry* entry = fileMethodTable.entries[i];
        while (entry) {
            FileMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(fileMethodTable.entries);
    fileMethodTable.entries = NULL;
    fileMethodTable.capacity = 0;
    fileMethodTable.count = 0;
}

// 扩容文件方法表
static void file_method_table_resize(void) {
    int old_capacity = fileMethodTable.capacity;
    FileMethodHashEntry** old_entries = fileMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    FileMethodHashEntry** new_entries = (FileMethodHashEntry**)calloc(new_capacity, sizeof(FileMethodHashEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        FileMethodHashEntry* entry = old_entries[i];
        while (entry) {
            FileMethodHashEntry* next = entry->next;
            uint32_t hash = file_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    fileMethodTable.entries = new_entries;
    fileMethodTable.capacity = new_capacity;
}

// 注册文件方法（带参数类型）
void file_register_method_with_params(const char* name, ObjNative* method, int arity,
                                       int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind* param_types) {
    if (!fileMethodTable.entries) {
        file_method_table_init();
    }
    
    if (fileMethodTable.count >= fileMethodTable.capacity * FILE_METHOD_TABLE_MAX_LOAD) {
        file_method_table_resize();
    }
    
    uint32_t hash = file_hash_string(name);
    int index = hash & (fileMethodTable.capacity - 1);
    
    FileMethodHashEntry* entry = fileMethodTable.entries[index];
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
    
    FileMethodHashEntry* new_entry = (FileMethodHashEntry*)malloc(sizeof(FileMethodHashEntry));
    if (!new_entry) {
        native_throw_error("文件方法注册内存分配失败");
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
    
    new_entry->next = fileMethodTable.entries[index];
    fileMethodTable.entries[index] = new_entry;
    fileMethodTable.count++;
    
    native_register_instance_method_meta_with_params("file", name, arity, min_arity, max_arity, return_type, param_types);
}

// 获取文件方法的参数类型
TypeKind file_get_method_param_type(const char* method_name, int param_index) {
    if (!fileMethodTable.entries || fileMethodTable.count == 0) return TYPE_ANY;
    
    uint32_t hash = file_hash_string(method_name);
    int index = hash & (fileMethodTable.capacity - 1);
    
    FileMethodHashEntry* entry = fileMethodTable.entries[index];
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

// 查找文件方法（O(1)）
ObjNative* file_find_method(const char* name) {
    if (!fileMethodTable.entries || fileMethodTable.count == 0) return NULL;
    
    uint32_t hash = file_hash_string(name);
    int index = hash & (fileMethodTable.capacity - 1);
    
    FileMethodHashEntry* entry = fileMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 查找文件方法的元信息（用于编译期类型检查）
FileMethodEntry file_find_method_meta(const char* name) {
    FileMethodEntry result = {NULL, NULL, 0, TYPE_ANY, {TYPE_ANY}};
    
    if (!fileMethodTable.entries || fileMethodTable.count == 0) return result;
    
    uint32_t hash = file_hash_string(name);
    int index = hash & (fileMethodTable.capacity - 1);
    
    FileMethodHashEntry* entry = fileMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            result.name = entry->name;
            result.method = entry->method;
            result.arity = entry->arity;
            result.return_type = entry->return_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                result.param_types[i] = entry->param_types[i];
            }
            return result;
        }
        entry = entry->next;
    }
    return result;
}

void file_init_methods(void) {
    file_method_table_free();
    file_method_table_init();
}

// 标记所有文件方法对象（供 GC 使用）
void file_mark_methods(void) {
    if (!fileMethodTable.entries) return;
    
    for (int i = 0; i < fileMethodTable.capacity; i++) {
        FileMethodHashEntry* entry = fileMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
