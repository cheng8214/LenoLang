#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Win 窗口方法注册表（哈希表实现 - 参考 object_draw.c）
 * 支持 win.method() 风格的实例方法调用
 * ============================================================================ */

#define WINDOW_METHOD_TABLE_INITIAL_CAPACITY 32
#define WINDOW_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t window_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct WindowMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct WindowMethodHashEntry* next;
} WindowMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    WindowMethodHashEntry** entries;
    int capacity;
    int count;
} WindowMethodTable;

static THREAD_LOCAL WindowMethodTable windowMethodTable = {NULL, 0, 0};

/* 初始化 Win 方法表 */
static void window_method_table_init(void) {
    windowMethodTable.capacity = WINDOW_METHOD_TABLE_INITIAL_CAPACITY;
    windowMethodTable.count = 0;
    windowMethodTable.entries = (WindowMethodHashEntry**)calloc(windowMethodTable.capacity, sizeof(WindowMethodHashEntry*));
}

/* 释放 Win 方法表 */
static void window_method_table_free(void) {
    if (!windowMethodTable.entries) return;
    for (int i = 0; i < windowMethodTable.capacity; i++) {
        WindowMethodHashEntry* entry = windowMethodTable.entries[i];
        while (entry) {
            WindowMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(windowMethodTable.entries);
    windowMethodTable.entries = NULL;
    windowMethodTable.capacity = 0;
    windowMethodTable.count = 0;
}

/* 扩容 Win 方法表 */
static void window_method_table_resize(void) {
    int old_capacity = windowMethodTable.capacity;
    WindowMethodHashEntry** old_entries = windowMethodTable.entries;
    int new_capacity = old_capacity * 2;
    WindowMethodHashEntry** new_entries = (WindowMethodHashEntry**)calloc(new_capacity, sizeof(WindowMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        WindowMethodHashEntry* entry = old_entries[i];
        while (entry) {
            WindowMethodHashEntry* next = entry->next;
            uint32_t hash = window_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    windowMethodTable.entries = new_entries;
    windowMethodTable.capacity = new_capacity;
}

/* 注册 Win 方法（带参数类型信息，供编译期和运行时使用） */
void window_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!windowMethodTable.entries) {
        window_method_table_init();
    }
    if (windowMethodTable.count >= windowMethodTable.capacity * WINDOW_METHOD_TABLE_MAX_LOAD) {
        window_method_table_resize();
    }
    uint32_t hash = window_hash_string(name);
    int index = hash & (windowMethodTable.capacity - 1);

    /* 检查是否已存在同名方法 */
    WindowMethodHashEntry* entry = windowMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) entry->param_types[i] = param_types[i];
                for (int i = count; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            }
            return;
        }
        entry = entry->next;
    }

    /* 创建新条目 */
    WindowMethodHashEntry* new_entry = (WindowMethodHashEntry*)malloc(sizeof(WindowMethodHashEntry));
    if (!new_entry) {
        native_throw_error("Win 方法注册内存分配失败");
        return;
    }
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) new_entry->param_types[i] = param_types[i];
        for (int i = count; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    }
    new_entry->next = windowMethodTable.entries[index];
    windowMethodTable.entries[index] = new_entry;
    windowMethodTable.count++;

    /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("win", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 获取 Win 方法的参数类型 */
TypeKind window_get_method_param_type(const char* method_name, int param_index) {
    if (!windowMethodTable.entries || windowMethodTable.count == 0) return TYPE_ANY;
    uint32_t hash = window_hash_string(method_name);
    int index = hash & (windowMethodTable.capacity - 1);
    WindowMethodHashEntry* entry = windowMethodTable.entries[index];
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

/* 查找 Win 方法（运行时 O(1) 查找） */
ObjNative* window_find_method(const char* name) {
    if (!windowMethodTable.entries || windowMethodTable.count == 0) return NULL;
    uint32_t hash = window_hash_string(name);
    int index = hash & (windowMethodTable.capacity - 1);
    WindowMethodHashEntry* entry = windowMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 查找 Win 方法的元信息（编译期类型检查） */
WindowMethodEntry window_find_method_meta(const char* name) {
    WindowMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    if (!windowMethodTable.entries || windowMethodTable.count == 0) return result;
    uint32_t hash = window_hash_string(name);
    int index = hash & (windowMethodTable.capacity - 1);
    WindowMethodHashEntry* entry = windowMethodTable.entries[index];
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

/* 初始化 Win 方法表 */
void window_init_methods(void) {
    window_method_table_free();
    window_method_table_init();
}

/* 标记所有 Win 方法对象（供 GC 使用） */
void window_mark_methods(void) {
    if (!windowMethodTable.entries) return;
    for (int i = 0; i < windowMethodTable.capacity; i++) {
        WindowMethodHashEntry* entry = windowMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
