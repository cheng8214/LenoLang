/* ============================================================================
 * GEdit 文本框方法注册表（参考 object_button.c）
 * 支持 tb.method() 风格的实例方法调用
 * ============================================================================ */
#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EDIT_METHOD_TABLE_INITIAL_CAPACITY 16
#define EDIT_METHOD_TABLE_MAX_LOAD 0.75

static uint32_t tbox_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) { hash ^= (unsigned char)(*str); hash *= 16777619; str++; }
    return hash;
}

typedef struct EditMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct EditMethodHashEntry* next;
} EditMethodHashEntry;

typedef struct {
    EditMethodHashEntry** entries;
    int capacity;
    int count;
} EditMethodTable;

static THREAD_LOCAL EditMethodTable editMethodTable = {NULL, 0, 0};

static void EDIT_METHOD_TABLE_init(void) {
    editMethodTable.capacity = EDIT_METHOD_TABLE_INITIAL_CAPACITY;
    editMethodTable.count = 0;
    editMethodTable.entries = (EditMethodHashEntry**)calloc(editMethodTable.capacity, sizeof(EditMethodHashEntry*));
}

static void EDIT_METHOD_TABLE_free(void) {
    if (!editMethodTable.entries) return;
    for (int i = 0; i < editMethodTable.capacity; i++) {
        EditMethodHashEntry* entry = editMethodTable.entries[i];
        while (entry) {
            EditMethodHashEntry* next = entry->next;
            free(entry->name); free(entry);
            entry = next;
        }
    }
    free(editMethodTable.entries);
    editMethodTable.entries = NULL; editMethodTable.capacity = 0; editMethodTable.count = 0;
}

static void EDIT_METHOD_TABLE_resize(void) {
    int old_cap = editMethodTable.capacity;
    EditMethodHashEntry** old_entries = editMethodTable.entries;
    int new_cap = old_cap * 2;
    EditMethodHashEntry** new_entries = (EditMethodHashEntry**)calloc(new_cap, sizeof(EditMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_cap; i++) {
        EditMethodHashEntry* entry = old_entries[i];
        while (entry) {
            EditMethodHashEntry* next = entry->next;
            uint32_t hash = tbox_hash_string(entry->name);
            int index = hash & (new_cap - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    editMethodTable.entries = new_entries;
    editMethodTable.capacity = new_cap;
}

void edit_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!editMethodTable.entries) EDIT_METHOD_TABLE_init();
    if (editMethodTable.count >= editMethodTable.capacity * EDIT_METHOD_TABLE_MAX_LOAD) EDIT_METHOD_TABLE_resize();
    uint32_t hash = tbox_hash_string(name);
    int index = hash & (editMethodTable.capacity - 1);
    EditMethodHashEntry* entry = editMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method; entry->arity = arity;
            entry->return_type = return_type; entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int c = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < c; i++) entry->param_types[i] = param_types[i];
                for (int i = c; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY;
            } else { for (int i = 0; i < MAX_METHOD_PARAMS; i++) entry->param_types[i] = TYPE_ANY; }
            return;
        }
        entry = entry->next;
    }
    EditMethodHashEntry* ne = (EditMethodHashEntry*)malloc(sizeof(EditMethodHashEntry));
    if (!ne) return;
    ne->name = strdup(name); ne->method = method; ne->arity = arity;
    ne->return_type = return_type; ne->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int c = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < c; i++) ne->param_types[i] = param_types[i];
        for (int i = c; i < MAX_METHOD_PARAMS; i++) ne->param_types[i] = TYPE_ANY;
    } else { for (int i = 0; i < MAX_METHOD_PARAMS; i++) ne->param_types[i] = TYPE_ANY; }
    ne->next = editMethodTable.entries[index];
    editMethodTable.entries[index] = ne;
    editMethodTable.count++;
    native_register_instance_method_meta_with_params("GEdit", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* edit_find_method(const char* name) {
    if (!editMethodTable.entries || editMethodTable.count == 0) return NULL;
    uint32_t hash = tbox_hash_string(name);
    int index = hash & (editMethodTable.capacity - 1);
    EditMethodHashEntry* entry = editMethodTable.entries[index];
    while (entry) { if (strcmp(entry->name, name) == 0) return entry->method; entry = entry->next; }
    return NULL;
}

void edit_init_methods(void) { EDIT_METHOD_TABLE_free(); EDIT_METHOD_TABLE_init(); }

/* 标记所有 GEdit 方法对象（供 GC 使用） */
void edit_mark_methods(void) {
    if (!editMethodTable.entries) return;
    for (int i = 0; i < editMethodTable.capacity; i++) {
        EditMethodHashEntry* entry = editMethodTable.entries[i];
        while (entry) {
            if (entry->method) gc_mark_object((Object*)entry->method);
            entry = entry->next;
        }
    }
}
