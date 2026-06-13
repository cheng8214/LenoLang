#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Image 图像方法注册表（哈希表实现 - 参考 object_draw.c）
 * 支持 image.draw() 风格的实例方法调用
 * ============================================================================ */

#define IMAGE_METHOD_TABLE_INITIAL_CAPACITY 32
#define IMAGE_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t image_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct ImageMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct ImageMethodHashEntry* next;
} ImageMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    ImageMethodHashEntry** entries;
    int capacity;
    int count;
} ImageMethodTable;

static THREAD_LOCAL ImageMethodTable imageMethodTable = {NULL, 0, 0};

/* 初始化 Image 方法表 */
static void image_method_table_init(void) {
    imageMethodTable.capacity = IMAGE_METHOD_TABLE_INITIAL_CAPACITY;
    imageMethodTable.count = 0;
    imageMethodTable.entries = (ImageMethodHashEntry**)calloc(imageMethodTable.capacity, sizeof(ImageMethodHashEntry*));
}

/* 释放 Image 方法表 */
static void image_method_table_free(void) {
    if (!imageMethodTable.entries) return;
    for (int i = 0; i < imageMethodTable.capacity; i++) {
        ImageMethodHashEntry* entry = imageMethodTable.entries[i];
        while (entry) {
            ImageMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(imageMethodTable.entries);
    imageMethodTable.entries = NULL;
    imageMethodTable.capacity = 0;
    imageMethodTable.count = 0;
}

/* 扩容 Image 方法表 */
static void image_method_table_resize(void) {
    int old_capacity = imageMethodTable.capacity;
    ImageMethodHashEntry** old_entries = imageMethodTable.entries;
    int new_capacity = old_capacity * 2;
    ImageMethodHashEntry** new_entries = (ImageMethodHashEntry**)calloc(new_capacity, sizeof(ImageMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        ImageMethodHashEntry* entry = old_entries[i];
        while (entry) {
            ImageMethodHashEntry* next = entry->next;
            uint32_t hash = image_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    imageMethodTable.entries = new_entries;
    imageMethodTable.capacity = new_capacity;
}

/* 注册 Image 方法（带参数类型信息，供编译期和运行时使用） */
void image_register_method_with_params(const char* name, ObjNative* method, int arity,
                                       int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!imageMethodTable.entries) {
        image_method_table_init();
    }
    if (imageMethodTable.count >= imageMethodTable.capacity * IMAGE_METHOD_TABLE_MAX_LOAD) {
        image_method_table_resize();
    }
    uint32_t hash = image_hash_string(name);
    int index = hash & (imageMethodTable.capacity - 1);

    /* 检查是否已存在同名方法 */
    ImageMethodHashEntry* entry = imageMethodTable.entries[index];
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
    ImageMethodHashEntry* new_entry = (ImageMethodHashEntry*)malloc(sizeof(ImageMethodHashEntry));
    if (!new_entry) {
        native_throw_error("Image 方法注册内存分配失败");
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
    new_entry->next = imageMethodTable.entries[index];
    imageMethodTable.entries[index] = new_entry;
    imageMethodTable.count++;

    /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("gimage", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 获取 Image 方法的参数类型 */
TypeKind image_get_method_param_type(const char* method_name, int param_index) {
    if (!imageMethodTable.entries || imageMethodTable.count == 0) return TYPE_ANY;
    uint32_t hash = image_hash_string(method_name);
    int index = hash & (imageMethodTable.capacity - 1);
    ImageMethodHashEntry* entry = imageMethodTable.entries[index];
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

/* 查找 Image 方法（运行时 O(1) 查找） */
ObjNative* image_find_method(const char* name) {
    if (!imageMethodTable.entries || imageMethodTable.count == 0) return NULL;
    uint32_t hash = image_hash_string(name);
    int index = hash & (imageMethodTable.capacity - 1);
    ImageMethodHashEntry* entry = imageMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 查找 Image 方法的元信息（编译期类型检查） */
ImageMethodEntry image_find_method_meta(const char* name) {
    ImageMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    if (!imageMethodTable.entries || imageMethodTable.count == 0) return result;
    uint32_t hash = image_hash_string(name);
    int index = hash & (imageMethodTable.capacity - 1);
    ImageMethodHashEntry* entry = imageMethodTable.entries[index];
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

/* 初始化 Image 方法表 */
void image_init_methods(void) {
    image_method_table_free();
    image_method_table_init();
}

/* 标记所有 Image 方法对象（供 GC 使用） */
void image_mark_methods(void) {
    if (!imageMethodTable.entries) return;
    for (int i = 0; i < imageMethodTable.capacity; i++) {
        ImageMethodHashEntry* entry = imageMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
