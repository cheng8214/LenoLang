/* ============================================================================
 * GLabel 标签方法注册表（参考 object_button.c）
 * 支持 lb.method() 风格的实例方法调用
 * ============================================================================ */
#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LABEL_METHOD_TABLE_INITIAL_CAPACITY 16
#define LABEL_METHOD_TABLE_MAX_LOAD 0.75

static uint32_t label_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) { hash ^= (unsigned char)(*str); hash *= 16777619; str++; }
    return hash;
}

typedef struct LabelMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct LabelMethodHashEntry* next;
} LabelMethodHashEntry;

typedef struct {
    LabelMethodHashEntry** entries;
    int capacity;
    int count;
} LabelMethodTable;

static THREAD_LOCAL LabelMethodTable labelMethodTable = {NULL, 0, 0};

static void label_method_table_init(void) {
    labelMethodTable.capacity = LABEL_METHOD_TABLE_INITIAL_CAPACITY;
    labelMethodTable.count = 0;
    labelMethodTable.entries = (LabelMethodHashEntry**)calloc(labelMethodTable.capacity, sizeof(LabelMethodHashEntry*));
}

static void label_method_table_free(void) {
    if (!labelMethodTable.entries) return;
    for (int i = 0; i < labelMethodTable.capacity; i++) {
        LabelMethodHashEntry* entry = labelMethodTable.entries[i];
        while (entry) {
            LabelMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(labelMethodTable.entries);
    labelMethodTable.entries = NULL;
    labelMethodTable.capacity = 0;
    labelMethodTable.count = 0;
}

static void label_method_table_resize(void) {
    int old_capacity = labelMethodTable.capacity;
    LabelMethodHashEntry** old_entries = labelMethodTable.entries;
    int new_capacity = old_capacity * 2;
    LabelMethodHashEntry** new_entries = (LabelMethodHashEntry**)calloc(new_capacity, sizeof(LabelMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        LabelMethodHashEntry* entry = old_entries[i];
        while (entry) {
            LabelMethodHashEntry* next = entry->next;
            uint32_t hash = label_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    labelMethodTable.entries = new_entries;
    labelMethodTable.capacity = new_capacity;
}

void label_register_method_with_params(const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!labelMethodTable.entries) {
        label_method_table_init();
    }
    if (labelMethodTable.count >= labelMethodTable.capacity * LABEL_METHOD_TABLE_MAX_LOAD) {
        label_method_table_resize();
    }
    uint32_t hash = label_hash_string(name);
    int index = hash & (labelMethodTable.capacity - 1);

    LabelMethodHashEntry* entry = labelMethodTable.entries[index];
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

    LabelMethodHashEntry* new_entry = (LabelMethodHashEntry*)malloc(sizeof(LabelMethodHashEntry));
    if (!new_entry) {
        native_throw_error("GLabel method registration memory allocation failed");
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
    new_entry->next = labelMethodTable.entries[index];
    labelMethodTable.entries[index] = new_entry;
    labelMethodTable.count++;

    native_register_instance_method_meta_with_params("glabel", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* label_find_method(const char* name) {
    if (!labelMethodTable.entries || labelMethodTable.count == 0) return NULL;
    uint32_t hash = label_hash_string(name);
    int index = hash & (labelMethodTable.capacity - 1);
    LabelMethodHashEntry* entry = labelMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

void label_init_methods(void) {
    label_method_table_free();
    label_method_table_init();
}

/* 标记所有 GLabel 方法对象（供 GC 使用） */
void label_mark_methods(void) {
    if (!labelMethodTable.entries) return;
    for (int i = 0; i < labelMethodTable.capacity; i++) {
        LabelMethodHashEntry* entry = labelMethodTable.entries[i];
        while (entry) {
            if (entry->method) gc_mark_object((Object*)entry->method);
            entry = entry->next;
        }
    }
}
