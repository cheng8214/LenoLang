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
    // ⚠ (2026-10-02) 这里原有把 param_types 填进**运行期条目**的一段 —— 那份数据零读者
    //   （消费者 method_table_get_param_type 已删）⇒ 类型只在**编译期元信息表**里保存一份
    //   （见下面的 native_set_instance_method_vararg_params 调用）✓

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

// (2026-10-02) 此处原有 `method_table_find_meta()` —— 它返回的 `MethodEntry` 里也带一份 param_types，
//   属同一条"平行保存"的链。它与 9 个 per-type 包装（*_find_method_meta）**零调用点** ⇒ 整族删除 ✓

// (2026-10-02) 此处原有 `method_table_get_param_type()` —— 运行期条目的那份 param_types 的读取者。
//   它与**编译期元信息表**（native.c 的 instanceMethodTable，检查器真正读的）平行保存同一批类型，
//   且判据 `param_index < entry->arity` 对可变参数恒假（"闸"写了两份、修一份漏一份）✗。
//   经查它与其 4 个 per-type 包装（array/string/file/dict_get_method_param_type）**零调用点** ⇒ 整族删除，
//   类型只在编译期元信息表保存一份 ✓

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
