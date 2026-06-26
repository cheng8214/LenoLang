#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * GButton 按钮方法注册表（哈希表实现 - 参考 object_window.c）
 * 支持 btn.method() 风格的实例方法调用
 * ============================================================================ */

#define BUTTON_METHOD_TABLE_INITIAL_CAPACITY 16
#define BUTTON_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t button_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct ButtonMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct ButtonMethodHashEntry* next;
} ButtonMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    ButtonMethodHashEntry** entries;
    int capacity;
    int count;
} ButtonMethodTable;

static THREAD_LOCAL ButtonMethodTable buttonMethodTable = {NULL, 0, 0};

/* 初始化 GButton 方法表 */
static void button_method_table_init(void) {
    buttonMethodTable.capacity = BUTTON_METHOD_TABLE_INITIAL_CAPACITY;
    buttonMethodTable.count = 0;
    buttonMethodTable.entries = (ButtonMethodHashEntry**)calloc(buttonMethodTable.capacity, sizeof(ButtonMethodHashEntry*));
}

/* 释放 GButton 方法表 */
static void button_method_table_free(void) {
    if (!buttonMethodTable.entries) return;
    for (int i = 0; i < buttonMethodTable.capacity; i++) {
        ButtonMethodHashEntry* entry = buttonMethodTable.entries[i];
        while (entry) {
            ButtonMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(buttonMethodTable.entries);
    buttonMethodTable.entries = NULL;
    buttonMethodTable.capacity = 0;
    buttonMethodTable.count = 0;
}

/* 扩容 GButton 方法表 */
static void button_method_table_resize(void) {
    int old_capacity = buttonMethodTable.capacity;
    ButtonMethodHashEntry** old_entries = buttonMethodTable.entries;
    int new_capacity = old_capacity * 2;
    ButtonMethodHashEntry** new_entries = (ButtonMethodHashEntry**)calloc(new_capacity, sizeof(ButtonMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        ButtonMethodHashEntry* entry = old_entries[i];
        while (entry) {
            ButtonMethodHashEntry* next = entry->next;
            uint32_t hash = button_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    buttonMethodTable.entries = new_entries;
    buttonMethodTable.capacity = new_capacity;
}

/* 注册 GButton 方法（带参数类型信息） */
void button_register_method_with_params(const char* name, ObjNative* method, int arity,
                                         int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!buttonMethodTable.entries) {
        button_method_table_init();
    }
    if (buttonMethodTable.count >= buttonMethodTable.capacity * BUTTON_METHOD_TABLE_MAX_LOAD) {
        button_method_table_resize();
    }
    uint32_t hash = button_hash_string(name);
    int index = hash & (buttonMethodTable.capacity - 1);

    /* 检查是否已存在同名方法 */
    ButtonMethodHashEntry* entry = buttonMethodTable.entries[index];
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
    ButtonMethodHashEntry* new_entry = (ButtonMethodHashEntry*)malloc(sizeof(ButtonMethodHashEntry));
    if (!new_entry) {
        native_throw_error("GButton 方法注册内存分配失败");
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
    new_entry->next = buttonMethodTable.entries[index];
    buttonMethodTable.entries[index] = new_entry;
    buttonMethodTable.count++;

    /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("gbutton", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 获取 GButton 方法的参数类型 */
TypeKind button_get_method_param_type(const char* method_name, int param_index) {
    if (!buttonMethodTable.entries || buttonMethodTable.count == 0) return TYPE_ANY;
    uint32_t hash = button_hash_string(method_name);
    int index = hash & (buttonMethodTable.capacity - 1);
    ButtonMethodHashEntry* entry = buttonMethodTable.entries[index];
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

/* 查找 GButton 方法（运行时 O(1) 查找） */
ObjNative* button_find_method(const char* name) {
    if (!buttonMethodTable.entries || buttonMethodTable.count == 0) return NULL;
    uint32_t hash = button_hash_string(name);
    int index = hash & (buttonMethodTable.capacity - 1);
    ButtonMethodHashEntry* entry = buttonMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 查找 GButton 方法的元信息（编译期类型检查） */
ButtonMethodEntry button_find_method_meta(const char* name) {
    ButtonMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    if (!buttonMethodTable.entries || buttonMethodTable.count == 0) return result;
    uint32_t hash = button_hash_string(name);
    int index = hash & (buttonMethodTable.capacity - 1);
    ButtonMethodHashEntry* entry = buttonMethodTable.entries[index];
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

/* 初始化 GButton 方法表 */
void button_init_methods(void) {
    button_method_table_free();
    button_method_table_init();
}
