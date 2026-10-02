#include "include/lenolang.h"
#include "include/native.h"
#include "include/leno_hash.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 通用方法注册表实现
// ============================================================================

#define METHOD_TABLE_MAX_LOAD 0.75

// 初始化方法表
void method_table_init(MethodTable* table, int initial_capacity) {
    table->capacity = initial_capacity;
    table->count = 0;
    table->entries = (MethodHashEntry**)calloc(table->capacity, sizeof(MethodHashEntry*));
}

// 释放方法表
void method_table_free(MethodTable* table) {
    if (!table->entries) return;

    for (int i = 0; i < table->capacity; i++) {
        MethodHashEntry* entry = table->entries[i];
        while (entry) {
            MethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(table->entries);
    table->entries = NULL;
    table->capacity = 0;
    table->count = 0;
}

// 扩容方法表
void method_table_resize(MethodTable* table) {
    int old_capacity = table->capacity;
    MethodHashEntry** old_entries = table->entries;

    int new_capacity = old_capacity * 2;
    MethodHashEntry** new_entries = (MethodHashEntry**)calloc(new_capacity, sizeof(MethodHashEntry*));
    if (!new_entries) return;

    for (int i = 0; i < old_capacity; i++) {
        MethodHashEntry* entry = old_entries[i];
        while (entry) {
            MethodHashEntry* next = entry->next;
            uint32_t hash = leno_fnv1a(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }

    free(old_entries);
    table->entries = new_entries;
    table->capacity = new_capacity;
}

// 注册方法（**唯一入口**，2026-10-02 统一）：参数规格见 native.h 的 NativeParamSpec 与三个构造宏。
//   取代了原先两个（`_with_params` 定长 / `_vararg_with_params` 可变）以及更早的
//   "注册 + 事后补声明"两步式 —— 两步式**忘了第二步就静默失去类型检查** ✗
//   ⇒ 现在规格随注册**一次给全**（同一步补齐运行期条目与编译期元信息），漏不掉 ✓
void method_table_register_method(MethodTable* table, const char* type_name, const char* name,
                                  ObjNative* method, TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params) {
    const int arity = params.arity;
    const int min_arity = params.min_arity;
    const int max_arity = params.max_arity;
    const int declared_count = params.declared_count;
    const TypeKind* declared = params.declared;
    const TypeKind tail_type = params.tail_type;

    // 自检（与模块侧同口径：自相矛盾就**响亮失败**）
    if (arity != NATIVE_ARITY_VARARG && arity < 0) {
        fprintf(stderr, "[fatal] native 实例方法注册 %s.%s: arity=%d 非法 —— 可变参数请写 "
                        "NATIVE_ARITY_VARARG(%d)，定长请写具体个数\n",
                type_name, name, arity, NATIVE_ARITY_VARARG);
        abort();
    }
    if (arity != NATIVE_ARITY_VARARG && declared_count != 0 && declared_count != arity) {
        fprintf(stderr, "[fatal] native 实例方法注册 %s.%s: 定长 arity=%d 但 declared_count=%d"
                        "（应相等或传 0）\n", type_name, name, arity, declared_count);
        abort();
    }

    if (!table->entries) {
        method_table_init(table, 32);
    }
    if (table->count >= table->capacity * METHOD_TABLE_MAX_LOAD) {
        method_table_resize(table);
    }

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    // 找条目；没有就建（更新/新建**共用**下面的填写，不再各写一遍 ✓）
    MethodHashEntry* entry = table->entries[index];
    while (entry && strcmp(entry->name, name) != 0) {
        entry = entry->next;
    }
    if (!entry) {
        entry = (MethodHashEntry*)malloc(sizeof(MethodHashEntry));
        if (!entry) return;
        entry->name = strdup(name);
        entry->next = table->entries[index];
        table->entries[index] = entry;
        table->count++;
    }
    entry->method = method;
    entry->arity = arity;
    entry->min_arity = min_arity;
    entry->max_arity = max_arity;
    entry->return_type = return_type;
    entry->return_element_type = return_element_type;
    // 类型填法：先整份 tail_type，再盖上前 declared_count 个（与编译期元信息同一规则 ✓）
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        entry->param_types[i] = tail_type;
    }
    for (int i = 0; i < declared_count && i < MAX_METHOD_PARAMS; i++) {
        entry->param_types[i] = declared ? declared[i] : TYPE_ANY;
    }

    // 编译期元信息（检查器读的就是这份）——与上面同一步完成 ⇒ 调用方漏不掉 ✓
    native_register_instance_method_meta_with_params(type_name, name, arity, min_arity, max_arity,
                                                     return_type, return_element_type, NULL);
    if (declared_count > 0) {
        native_set_instance_method_vararg_params(type_name, name, declared_count, declared, tail_type);
    }
}

// 查找方法（O(1)）
ObjNative* method_table_find(MethodTable* table, const char* name) {
    if (!table->entries || table->count == 0) return NULL;

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 查找方法元信息
MethodEntry method_table_find_meta(MethodTable* table, const char* name) {
    MethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};

    if (!table->entries || table->count == 0) return result;

    uint32_t hash = leno_fnv1a(name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
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

// 获取方法参数类型
TypeKind method_table_get_param_type(MethodTable* table, const char* method_name, int param_index) {
    if (!table->entries || table->count == 0) return TYPE_ANY;

    uint32_t hash = leno_fnv1a(method_name);
    int index = hash & (table->capacity - 1);

    MethodHashEntry* entry = table->entries[index];
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

// 初始化方法表（free + init）
void method_table_init_methods(MethodTable* table, int initial_capacity) {
    method_table_free(table);
    method_table_init(table, initial_capacity);
}

// 标记所有方法对象（供 GC 使用）
void method_table_mark(MethodTable* table) {
    if (!table->entries) return;

    for (int i = 0; i < table->capacity; i++) {
        MethodHashEntry* entry = table->entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
