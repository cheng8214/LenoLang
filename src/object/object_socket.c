/* ============================================================================
 * Socket 方法注册表（哈希表实现 - 参考 object_image.c）
 * 支持 sock.send()、sock.recv()、sock.close() 等实例方法调用
 * ============================================================================ */

#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SOCKET_METHOD_TABLE_INITIAL_CAPACITY 32
#define SOCKET_METHOD_TABLE_MAX_LOAD 0.75

/* FNV-1a 字符串哈希算法 */
static uint32_t socket_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

/* 方法哈希表条目 */
typedef struct SocketMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct SocketMethodHashEntry* next;
} SocketMethodHashEntry;

/* 方法哈希表 */
typedef struct {
    SocketMethodHashEntry** entries;
    int capacity;
    int count;
} SocketMethodTable;

static THREAD_LOCAL SocketMethodTable socketMethodTable = {NULL, 0, 0};

/* 初始化 Socket 方法表 */
static void socket_method_table_init(void) {
    socketMethodTable.capacity = SOCKET_METHOD_TABLE_INITIAL_CAPACITY;
    socketMethodTable.count = 0;
    socketMethodTable.entries = (SocketMethodHashEntry**)calloc(socketMethodTable.capacity, sizeof(SocketMethodHashEntry*));
}

/* 释放 Socket 方法表 */
static void socket_method_table_free(void) {
    if (!socketMethodTable.entries) return;
    for (int i = 0; i < socketMethodTable.capacity; i++) {
        SocketMethodHashEntry* entry = socketMethodTable.entries[i];
        while (entry) {
            SocketMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(socketMethodTable.entries);
    socketMethodTable.entries = NULL;
    socketMethodTable.capacity = 0;
    socketMethodTable.count = 0;
}

/* 扩容 Socket 方法表 */
static void socket_method_table_resize(void) {
    int old_capacity = socketMethodTable.capacity;
    SocketMethodHashEntry** old_entries = socketMethodTable.entries;
    int new_capacity = old_capacity * 2;
    SocketMethodHashEntry** new_entries = (SocketMethodHashEntry**)calloc(new_capacity, sizeof(SocketMethodHashEntry*));
    if (!new_entries) return;
    for (int i = 0; i < old_capacity; i++) {
        SocketMethodHashEntry* entry = old_entries[i];
        while (entry) {
            SocketMethodHashEntry* next = entry->next;
            uint32_t hash = socket_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    free(old_entries);
    socketMethodTable.entries = new_entries;
    socketMethodTable.capacity = new_capacity;
}

/* 注册 Socket 方法（带参数类型信息，供编译期和运行时使用） */
void socket_register_method_with_params(const char* name, ObjNative* method, int arity,
                                        int min_arity, int max_arity,
                                        TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!socketMethodTable.entries) {
        socket_method_table_init();
    }
    if (socketMethodTable.count >= socketMethodTable.capacity * SOCKET_METHOD_TABLE_MAX_LOAD) {
        socket_method_table_resize();
    }
    uint32_t hash = socket_hash_string(name);
    int index = hash & (socketMethodTable.capacity - 1);

    /* 检查是否已存在同名方法 */
    SocketMethodHashEntry* entry = socketMethodTable.entries[index];
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
    SocketMethodHashEntry* new_entry = (SocketMethodHashEntry*)malloc(sizeof(SocketMethodHashEntry));
    if (!new_entry) return;
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
    new_entry->next = socketMethodTable.entries[index];
    socketMethodTable.entries[index] = new_entry;
    socketMethodTable.count++;

    /* 同时注册编译期元信息 */
    native_register_instance_method_meta_with_params("Socket", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

/* 查找 Socket 方法（运行时 O(1) 查找） */
ObjNative* socket_find_method(const char* name) {
    if (!socketMethodTable.entries || socketMethodTable.count == 0) return NULL;
    uint32_t hash = socket_hash_string(name);
    int index = hash & (socketMethodTable.capacity - 1);
    SocketMethodHashEntry* entry = socketMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

/* 初始化 Socket 方法表 */
void socket_init_methods(void) {
    socket_method_table_free();
    socket_method_table_init();
}

/* 标记所有 Socket 方法对象（供 GC 使用） */
void socket_mark_methods(void) {
    if (!socketMethodTable.entries) return;
    for (int i = 0; i < socketMethodTable.capacity; i++) {
        SocketMethodHashEntry* entry = socketMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
