#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Event 事件方法注册表（哈希表实现 - 参考 object_draw.c / object_file.c）
 * 支持 e.method() 风格的实例方法调用
 * ============================================================================ */

#define EVENT_METHOD_TABLE_INITIAL_CAPACITY 32
#define EVENT_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t event_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct EventMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct EventMethodHashEntry* next;
} EventMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    EventMethodHashEntry** entries;
    int capacity;
    int count;
} EventMethodTable;

static THREAD_LOCAL EventMethodTable eventMethodTable = {NULL, 0, 0};

/* 初始化 Event 方法表 */
static void event_method_table_init(void) {
    eventMethodTable.capacity = EVENT_METHOD_TABLE_INITIAL_CAPACITY;
    eventMethodTable.count = 0;
    eventMethodTable.entries = (EventMethodHashEntry**)calloc(eventMethodTable.capacity, sizeof(EventMethodHashEntry*));
}

/* 释放 Event 方法表 */
static void event_method_table_free(void) {
    if (!eventMethodTable.entries) return;
    for (int i = 0; i < eventMethodTable.capacity; i++) {
        EventMethodHashEntry* entry = eventMethodTable.entries[i];
        while (entry) {
            EventMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(eventMethodTable.entries);
    eventMethodTable.entries = NULL;
    eventMethodTable.capacity = 0;
    eventMethodTable.count = 0;
}

/* 扩容 Event 方法表 */
static void event_method_table_resize(void) {
    int old_capacity = eventMethodTable.capacity;
    EventMethodHashEntry** old_entries = eventMethodTable.entries;
    int new_capacity = old_capacity * 2;
    EventMethodHashEntry** new_entries = (EventMethodHashEntry**)calloc(new_capacity, sizeof(EventMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        EventMethodHashEntry* entry = old_entries[i];
        while (entry) {
            EventMethodHashEntry* next = entry->next;
            uint32_t hash = event_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    eventMethodTable.entries = new_entries;
    eventMethodTable.capacity = new_capacity;
}

/* 注册 Event 方法 */
void event_register_method_with_params(const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind* param_types) {
    if (!eventMethodTable.entries) {
        event_method_table_init();
    }
    if (eventMethodTable.count >= eventMethodTable.capacity * EVENT_METHOD_TABLE_MAX_LOAD) {
        event_method_table_resize();
    }
    uint32_t hash = event_hash_string(name);
    int index = hash & (eventMethodTable.capacity - 1);

    EventMethodHashEntry* entry = eventMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
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

    EventMethodHashEntry* new_entry = (EventMethodHashEntry*)malloc(sizeof(EventMethodHashEntry));
    if (!new_entry) {
        native_throw_error("Event 方法注册内存分配失败");
        return;
    }
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) new_entry->param_types[i] = param_types[i];
        for (int i = count; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) new_entry->param_types[i] = TYPE_ANY;
    }
    new_entry->next = eventMethodTable.entries[index];
    eventMethodTable.entries[index] = new_entry;
    eventMethodTable.count++;

    native_register_instance_method_meta_with_params("event", name, arity, min_arity, max_arity, return_type, param_types);
}

/* 获取 Event 方法的参数类型 */
TypeKind event_get_method_param_type(const char* method_name, int param_index) {
    if (!eventMethodTable.entries || eventMethodTable.count == 0) return TYPE_ANY;
    uint32_t hash = event_hash_string(method_name);
    int index = hash & (eventMethodTable.capacity - 1);
    EventMethodHashEntry* entry = eventMethodTable.entries[index];
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

/* 查找 Event 方法（运行时） */
ObjNative* event_find_method(const char* name) {
    if (!eventMethodTable.entries || eventMethodTable.count == 0) return NULL;
    uint32_t hash = event_hash_string(name);
    int index = hash & (eventMethodTable.capacity - 1);
    EventMethodHashEntry* entry = eventMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 查找 Event 方法的元信息 */
EventMethodEntry event_find_method_meta(const char* name) {
    EventMethodEntry result = {NULL, NULL, 0, TYPE_ANY, {TYPE_ANY}};
    if (!eventMethodTable.entries || eventMethodTable.count == 0) return result;
    uint32_t hash = event_hash_string(name);
    int index = hash & (eventMethodTable.capacity - 1);
    EventMethodHashEntry* entry = eventMethodTable.entries[index];
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

/* 初始化 Event 方法表 */
void event_init_methods(void) {
    event_method_table_free();
    event_method_table_init();
}

/* 标记所有 Event 方法对象（GC） */
void event_mark_methods(void) {
    if (!eventMethodTable.entries) return;
    for (int i = 0; i < eventMethodTable.capacity; i++) {
        EventMethodHashEntry* entry = eventMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
