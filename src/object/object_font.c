#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Font 字体方法注册表（哈希表实现 - 参考 object_image.c）
 * 支持 font.close() 风格的实例方法调用
 * ============================================================================ */

#define FONT_METHOD_TABLE_INITIAL_CAPACITY 32
#define FONT_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t font_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct FontMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct FontMethodHashEntry* next;
} FontMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    FontMethodHashEntry** entries;
    int capacity;
    int count;
} FontMethodTable;

static THREAD_LOCAL FontMethodTable fontMethodTable = {NULL, 0, 0};

/* 初始化 Font 方法表 */
static void font_method_table_init(void) {
    fontMethodTable.capacity = FONT_METHOD_TABLE_INITIAL_CAPACITY;
    fontMethodTable.count = 0;
    fontMethodTable.entries = (FontMethodHashEntry**)calloc(fontMethodTable.capacity, sizeof(FontMethodHashEntry*));
}

/* 释放 Font 方法表 */
static void font_method_table_free(void) {
    if (!fontMethodTable.entries) return;
    for (int i = 0; i < fontMethodTable.capacity; i++) {
        FontMethodHashEntry* entry = fontMethodTable.entries[i];
        while (entry) {
            FontMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(fontMethodTable.entries);
    fontMethodTable.entries = NULL;
    fontMethodTable.capacity = 0;
    fontMethodTable.count = 0;
}

/* 扩容 Font 方法表 */
static void font_method_table_resize(void) {
    int old_capacity = fontMethodTable.capacity;
    FontMethodHashEntry** old_entries = fontMethodTable.entries;
    int new_capacity = old_capacity * 2;
    FontMethodHashEntry** new_entries = (FontMethodHashEntry**)calloc(new_capacity, sizeof(FontMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        FontMethodHashEntry* entry = old_entries[i];
        while (entry) {
            FontMethodHashEntry* next = entry->next;
            uint32_t hash = font_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    fontMethodTable.entries = new_entries;
    fontMethodTable.capacity = new_capacity;
}

/* 注册 Font 方法（带参数类型信息，供编译期和运行时使用） */
void font_register_method_with_params(const char* name, ObjNative* method, int arity,
                                       int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!fontMethodTable.entries) {
        font_method_table_init();
    }
    if (fontMethodTable.count >= fontMethodTable.capacity * FONT_METHOD_TABLE_MAX_LOAD) {
        font_method_table_resize();
    }
    uint32_t hash = font_hash_string(name);
    int index = hash & (fontMethodTable.capacity - 1);

    /* 检查是否已存在同名方法 */
    FontMethodHashEntry* entry = fontMethodTable.entries[index];
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
    FontMethodHashEntry* new_entry = (FontMethodHashEntry*)malloc(sizeof(FontMethodHashEntry));
    if (!new_entry) {
        native_throw_error("Font 方法注册内存分配失败");
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
    new_entry->next = fontMethodTable.entries[index];
    fontMethodTable.entries[index] = new_entry;
    fontMethodTable.count++;

    /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("font", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 获取 Font 方法的参数类型 */
TypeKind font_get_method_param_type(const char* method_name, int param_index) {
    if (!fontMethodTable.entries || fontMethodTable.count == 0) return TYPE_ANY;
    uint32_t hash = font_hash_string(method_name);
    int index = hash & (fontMethodTable.capacity - 1);
    FontMethodHashEntry* entry = fontMethodTable.entries[index];
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

/* 查找 Font 方法（运行时 O(1) 查找） */
ObjNative* font_find_method(const char* name) {
    if (!fontMethodTable.entries || fontMethodTable.count == 0) return NULL;
    uint32_t hash = font_hash_string(name);
    int index = hash & (fontMethodTable.capacity - 1);
    FontMethodHashEntry* entry = fontMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 查找 Font 方法的元信息（编译期类型检查） */
FontMethodEntry font_find_method_meta(const char* name) {
    FontMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    if (!fontMethodTable.entries || fontMethodTable.count == 0) return result;
    uint32_t hash = font_hash_string(name);
    int index = hash & (fontMethodTable.capacity - 1);
    FontMethodHashEntry* entry = fontMethodTable.entries[index];
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

/* 初始化 Font 方法表 */
void font_init_methods(void) {
    font_method_table_free();
    font_method_table_init();
}

/* 标记所有 Font 方法对象（供 GC 使用） */
void font_mark_methods(void) {
    if (!fontMethodTable.entries) return;
    for (int i = 0; i < fontMethodTable.capacity; i++) {
        FontMethodHashEntry* entry = fontMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
