/* ============================================================================
 * GTextBox 文本框方法注册表（参考 object_button.c）
 * 支持 tb.method() 风格的实例方法调用
 * ============================================================================ */
#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TBOX_METHOD_TABLE_INITIAL_CAPACITY 16
#define TBOX_METHOD_TABLE_MAX_LOAD 0.75

static uint32_t tbox_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) { hash ^= (unsigned char)(*str); hash *= 16777619; str++; }
    return hash;
}

typedef struct TBoxMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct TBoxMethodHashEntry* next;
} TBoxMethodHashEntry;

typedef struct {
    TBoxMethodHashEntry** entries;
    int capacity;
    int count;
} TBoxMethodTable;

static THREAD_LOCAL TBoxMethodTable tboxMethodTable = {NULL, 0, 0};

static void tbox_method_table_init(void) {
    tboxMethodTable.capacity = TBOX_METHOD_TABLE_INITIAL_CAPACITY;
    tboxMethodTable.count = 0;
    tboxMethodTable.entries = (TBoxMethodHashEntry**)calloc(tboxMethodTable.capacity, sizeof(TBoxMethodHashEntry*));
}

static void tbox_method_table_free(void) {
    if (!tboxMethodTable.entries) return;
    for (int i = 0; i < tboxMethodTable.capacity; i++) {
        TBoxMethodHashEntry* entry = tboxMethodTable.entries[i];
        while (entry) {
            TBoxMethodHashEntry* next = entry->next;
            free(entry->name); free(entry);
            entry = next;
        }
    }
    free(tboxMethodTable.entries);
    tboxMethodTable.entries = NULL; tboxMethodTable.capacity = 0; tboxMethodTable.count = 0;
}

static void tbox_method_table_resize(void) {
    int old_cap = tboxMethodTable.capacity;
    TBoxMethodHashEntry** old_entries = tboxMethodTable.entries;
    int new_cap = old_cap * 2;
    TBoxMethodHashEntry** new_entries = (TBoxMethodHashEntry**)calloc(new_cap, sizeof(TBoxMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_cap; i++) {
        TBoxMethodHashEntry* entry = old_entries[i];
        while (entry) {
            TBoxMethodHashEntry* next = entry->next;
            uint32_t hash = tbox_hash_string(entry->name);
            int index = hash & (new_cap - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    tboxMethodTable.entries = new_entries;
    tboxMethodTable.capacity = new_cap;
}

void textbox_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!tboxMethodTable.entries) tbox_method_table_init();
    if (tboxMethodTable.count >= tboxMethodTable.capacity * TBOX_METHOD_TABLE_MAX_LOAD) tbox_method_table_resize();
    uint32_t hash = tbox_hash_string(name);
    int index = hash & (tboxMethodTable.capacity - 1);
    TBoxMethodHashEntry* entry = tboxMethodTable.entries[index];
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
    TBoxMethodHashEntry* ne = (TBoxMethodHashEntry*)malloc(sizeof(TBoxMethodHashEntry));
    if (!ne) return;
    ne->name = strdup(name); ne->method = method; ne->arity = arity;
    ne->return_type = return_type; ne->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int c = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < c; i++) ne->param_types[i] = param_types[i];
        for (int i = c; i < MAX_METHOD_PARAMS; i++) ne->param_types[i] = TYPE_ANY;
    } else { for (int i = 0; i < MAX_METHOD_PARAMS; i++) ne->param_types[i] = TYPE_ANY; }
    ne->next = tboxMethodTable.entries[index];
    tboxMethodTable.entries[index] = ne;
    tboxMethodTable.count++;
    native_register_instance_method_meta_with_params("gtextbox", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* textbox_find_method(const char* name) {
    if (!tboxMethodTable.entries || tboxMethodTable.count == 0) return NULL;
    uint32_t hash = tbox_hash_string(name);
    int index = hash & (tboxMethodTable.capacity - 1);
    TBoxMethodHashEntry* entry = tboxMethodTable.entries[index];
    while (entry) { if (strcmp(entry->name, name) == 0) return entry->method; entry = entry->next; }
    return NULL;
}

void textbox_init_methods(void) { tbox_method_table_free(); tbox_method_table_init(); }

void textbox_mark_methods(void) {
    if (!tboxMethodTable.entries) return;
    for (int i = 0; i < tboxMethodTable.capacity; i++)
        for (TBoxMethodHashEntry* e = tboxMethodTable.entries[i]; e; e = e->next)
            if (e->method) gc_mark_object((Object*)e->method);
}
