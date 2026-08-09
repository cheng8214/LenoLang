#include "include/lenolang.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

// 使用 DICT_TOMBSTONE_VAL（定义在 leno_value.h）作为已删除哨兵
static inline int is_tombstone_val(Value v) {
    return v == DICT_TOMBSTONE_VAL;
}

// 将整数键转换为用于 order 数组的标记值
// 使用特殊编码：QNAN | SIGN_BIT | TAG_INT | ((uint64_t)n & 0xFFFF)
// 这样 order 里可以区分 string 键和 int 键
#define INT_KEY_MARKER(n) ((Value)(QNAN | SIGN_BIT | TAG_INT | ((uint64_t)(n) & 0xFFFFFFFFULL)))

// 对 Value 键计算哈希值
static inline uint32_t dict_hash_value(Value key) {
    if (val_is_int(key)) {
        // 整数键：直接使用整数值作为哈希
        int32_t n = (int32_t)val_as_int(key);
        // FNV-1a hash for integers
        uint32_t hash = 2166136261u;
        hash ^= (uint8_t)(n & 0xFF);
        hash *= 16777619;
        hash ^= (uint8_t)((n >> 8) & 0xFF);
        hash *= 16777619;
        hash ^= (uint8_t)((n >> 16) & 0xFF);
        hash *= 16777619;
        hash ^= (uint8_t)((n >> 24) & 0xFF);
        hash *= 16777619;
        return hash;
    }
    if (val_is_float(key)) {
        // 浮点数键：按 bit 哈希
        double d = val_as_double(key);
        Value bits;
        memcpy(&bits, &d, sizeof(double));
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 8; i++) {
            hash ^= (uint8_t)(bits >> (i * 8));
            hash *= 16777619;
        }
        return hash;
    }
    if (val_is_bool(key)) {
        return val_as_bool(key) ? 2166136261u : 0;
    }
    if (val_is_obj(key)) {
        Object* obj = val_as_obj(key);
        if (obj->type == OBJ_STRING) {
            ObjString* str = (ObjString*)obj;
            return str->hash;
        }
    }
    if (val_is_null(key)) {
        return 0;
    }
    // 未知类型，使用指针哈希
    return (uint32_t)(uintptr_t)key;
}

// 比较两个 Value 键是否相等
// 统一使用 value_shallow_equal（定义在 leno_value.h）
// 修复了原先不支持 BigInt 键值比较的问题
static inline int dict_key_equals(Value a, Value b) {
    return value_shallow_equal(a, b);
}

// 查找条目：返回键的位置或应该插入的位置
static int dict_find_slot(ObjDict* dict, Value key) {
    if (dict->capacity == 0) return -1;

    uint32_t hash = dict_hash_value(key);
    uint32_t index = hash & (dict->capacity - 1);
    int first_tombstone = -1;

    while (1) {
        Value current_key = dict->entries[index].key;

        if (val_is_null(current_key) && !is_tombstone_val(current_key)) {
            // 找到空槽
            return first_tombstone != -1 ? first_tombstone : (int)index;
        }

        if (is_tombstone_val(current_key)) {
            if (first_tombstone == -1) {
                first_tombstone = (int)index;
            }
        } else if (dict_key_equals(current_key, key)) {
            return (int)index;
        }

        index = (index + 1) & (dict->capacity - 1);
    }
}

// 查找条目：返回键的位置，如果不存在返回 -1
static int dict_find_entry(ObjDict* dict, Value key) {
    if (dict->capacity == 0) return -1;

    uint32_t hash = dict_hash_value(key);
    uint32_t index = hash & (dict->capacity - 1);

    while (1) {
        Value current_key = dict->entries[index].key;

        if (val_is_null(current_key) && !is_tombstone_val(current_key)) {
            return -1;  // 遇到空槽，键不存在
        }

        if (!is_tombstone_val(current_key) && dict_key_equals(current_key, key)) {
            return (int)index;
        }

        index = (index + 1) & (dict->capacity - 1);
    }
}

// 调整哈希表大小并重新哈希所有条目
static void dict_resize(ObjDict* dict, int new_capacity) {
    ObjDictEntry* old_entries = dict->entries;
    int old_capacity = dict->capacity;

    dict->entries = (ObjDictEntry*)malloc(new_capacity * sizeof(ObjDictEntry));
    if (!dict->entries) {
        native_throw_error("字典扩容内存分配失败");
        dict->entries = old_entries;
        return;
    }

    for (int i = 0; i < new_capacity; i++) {
        dict->entries[i].key = NULL_VAL;
        dict->entries[i].value = val_null();
    }

    dict->capacity = new_capacity;
    dict->count = 0;
    dict->tombstone_count = 0;

    if (old_entries != NULL) {
        for (int i = 0; i < old_capacity; i++) {
            Value key = old_entries[i].key;
            if (!val_is_null(key) || is_tombstone_val(key)) {
                if (!is_tombstone_val(key) && !val_is_null(key)) {
                    int index = dict_find_slot(dict, key);
                    dict->entries[index].key = key;
                    dict->entries[index].value = old_entries[i].value;
                    dict->count++;
                }
            }
        }
        free(old_entries);
    }
}



// 检查 Value 键是否可以通过数组部分索引
// 只有整数键（非负）才映射到数组部分，字符串键始终走哈希表
// 返回 >= 0 表示数组索引，-1 表示需要走哈希表
static int key_to_array_index(Value key) {
    if (val_is_int(key)) {
        int n = val_as_int(key);
        return n >= 0 ? n : -1;
    }
    return -1;
}

// 数组部分扩容
static int dict_array_resize(ObjDict* dict, int new_size) {
    if (new_size <= dict->asize) return 1;
    int capacity = 4;
    while (capacity < new_size) capacity *= 2;
    Value* new_array = (Value*)realloc(dict->array, capacity * sizeof(Value));
    if (!new_array) return 0;
    for (int i = dict->asize; i < capacity; i++) {
        new_array[i] = val_null();
    }
    dict->array = new_array;
    dict->asize = capacity;
    return 1;
}

ObjDict* dict_new(int capacity) {
    int initial_capacity = 8;
    while (initial_capacity < capacity) {
        initial_capacity *= 2;
    }

    ObjDict* dict = (ObjDict*)gc_alloc(sizeof(ObjDict), OBJ_DICT);
    if (!dict) return NULL;

    dict->array = NULL;
    dict->asize = 0;
    dict->acount = 0;

    dict->entries = NULL;
    dict->count = 0;
    dict->capacity = 0;
    dict->tombstone_count = 0;

    dict_array_resize(dict, 4);

    dict_resize(dict, initial_capacity);

    if (!dict->entries) {
        return NULL;
    }

    dict->type_info = NULL;

    dict->order = NULL;
    dict->order_count = 0;
    dict->order_capacity = 0;

    return dict;
}

// 添加键到插入顺序数组
static void dict_add_to_order(ObjDict* dict, Value key) {
    if (dict->order_count >= dict->order_capacity) {
        int new_capacity = dict->order_capacity < 8 ? 8 : dict->order_capacity * 2;
        Value* new_order = (Value*)realloc(dict->order, new_capacity * sizeof(Value));
        if (!new_order) return;
        dict->order = new_order;
        dict->order_capacity = new_capacity;
    }

    dict->order[dict->order_count++] = key;
    // GC 写屏障：仅当键是对象时才需要
    if (val_is_obj(key)) {
        gc_write_barrier((Object*)dict, key);
    }
}

void dict_set(ObjDict* dict, Value key, Value value) {
    if (!dict) return;

    // 检查是否是整数键或数字字符串键，优先使用数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0) {
        if (array_index >= dict->asize) {
            // 稀疏性保护：当新键远超当前数组大小，或数组已较大且填充率低时，使用哈希表
            // 避免整数键递增导致数组部分无限扩容（如粒子系统 nid 递增场景）
            int sparse = (dict->asize > 0 && array_index > dict->asize * 2 && array_index > 64);
            int low_density = (dict->asize > 1024 && dict->acount < dict->asize / 4);
            if (sparse || low_density) {
                goto use_hash;
            }
            if (!dict_array_resize(dict, array_index + 1)) {
                goto use_hash;
            }
        }
        int is_new = val_is_null(dict->array[array_index]);
        if (is_new) {
            dict->acount++;
            dict_add_to_order(dict, key);
        }
        dict->array[array_index] = value;
        gc_write_barrier((Object*)dict, value);
        return;
    }

use_hash:
    // 非整数键或数组扩容失败：使用哈希部分
    int total_entries = dict->count + dict->tombstone_count;
    if (total_entries + 1 > dict->capacity * DICT_MAX_LOAD) {
        int new_capacity = dict->capacity < 8 ? 8 : dict->capacity * 2;
        dict_resize(dict, new_capacity);
    }

    int index = dict_find_slot(dict, key);

    if (index < 0 || index >= dict->capacity) return;

    Value existing_key = dict->entries[index].key;

    if (val_is_null(existing_key) && !is_tombstone_val(existing_key)) {
        // 新键
        dict->entries[index].key = key;
        dict->entries[index].value = value;
        if (val_is_obj(key)) {
            gc_write_barrier_obj((Object*)dict, val_as_obj(key));
        }
        gc_write_barrier((Object*)dict, value);
        dict->count++;
        dict_add_to_order(dict, key);
    } else if (is_tombstone_val(existing_key)) {
        // 复用 tombstone
        dict->entries[index].key = key;
        dict->entries[index].value = value;
        if (val_is_obj(key)) {
            gc_write_barrier_obj((Object*)dict, val_as_obj(key));
        }
        gc_write_barrier((Object*)dict, value);
        dict->count++;
        dict->tombstone_count--;
        dict_add_to_order(dict, key);
    } else {
        // 更新现有键
        dict->entries[index].value = value;
        gc_write_barrier((Object*)dict, value);
    }
}

static inline int is_valid_obj_value(Value v) {
    if (!val_is_obj(v)) return 1;
    Object* obj = val_as_obj(v);
    return obj->type >= OBJ_STRING && obj->type < OBJ_NONE;
}

Value dict_get(ObjDict* dict, Value key) {
    if (!dict) return val_null();

    // 先检查数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        Value value = dict->array[array_index];
        if (!val_is_null(value)) {
            if (!is_valid_obj_value(value)) return val_null();
            return value;
        }
    }

    // 查哈希部分
    if (dict->capacity == 0) return val_null();
    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        Value value = dict->entries[index].value;
        if (!is_valid_obj_value(value)) return val_null();
        return value;
    }
    return val_null();
}

int dict_has(ObjDict* dict, Value key) {
    if (!dict) return 0;

    // 先检查数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        if (!val_is_null(dict->array[array_index])) {
            return 1;
        }
    }

    // 再检查哈希部分
    if (dict->capacity == 0) return 0;
    return dict_find_entry(dict, key) >= 0;
}

// 从 order 数组移除指定 key
static void dict_remove_from_order(ObjDict* dict, Value key) {
    for (int i = 0; i < dict->order_count; i++) {
        Value entry = dict->order[i];
        if (dict_key_equals(entry, key)) {
            // swap-remove
            dict->order[i] = dict->order[dict->order_count - 1];
            dict->order_count--;
            return;
        }
    }
}

void dict_try_shrink(ObjDict* dict) {
    if (dict->capacity > 16 && dict->count < dict->capacity * 0.25) {
        int new_capacity = dict->capacity / 2;
        if (new_capacity < 8) new_capacity = 8;
        dict_resize(dict, new_capacity);
    }

    if (dict->order_capacity > 16 && dict->order_count < dict->order_capacity / 4) {
        int new_cap = dict->order_capacity / 2;
        if (new_cap < 8) new_cap = 8;
        Value* new_order = (Value*)realloc(dict->order, new_cap * sizeof(Value));
        if (new_order) {
            dict->order = new_order;
            dict->order_capacity = new_cap;
        }
    }

    // 数组部分缩容：当实际元素数远小于数组容量时，缩小数组部分
    // 找到数组部分最后一个非空元素的位置，将 asize 缩至该位置
    if (dict->asize > 16 && dict->acount < dict->asize / 4) {
        // 从尾部向前扫描，找到最后一个非空元素
        int last_non_null = -1;
        for (int i = dict->asize - 1; i >= 0; i--) {
            if (!val_is_null(dict->array[i])) {
                last_non_null = i;
                break;
            }
        }
        int target_size = last_non_null + 1;
        // 向上取整到 2 的幂次
        int new_asize = 4;
        while (new_asize < target_size) new_asize *= 2;
        if (new_asize < 4) new_asize = 4;
        if (new_asize < dict->asize) {
            Value* new_array = (Value*)realloc(dict->array, new_asize * sizeof(Value));
            if (new_array) {
                dict->array = new_array;
                dict->asize = new_asize;
            }
        }
    }
}

void dict_delete(ObjDict* dict, Value key) {
    if (!dict) return;

    // 先尝试从数组部分删除
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        if (!val_is_null(dict->array[array_index])) {
            dict->array[array_index] = val_null();
            dict->acount--;
            dict_remove_from_order(dict, key);
            dict_try_shrink(dict);
            return;
        }
    }

    // 从哈希部分删除
    if (dict->capacity == 0) return;

    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        dict->entries[index].key = DICT_TOMBSTONE_VAL;
        dict->entries[index].value = val_null();
        dict->count--;
        dict->tombstone_count++;
        dict_remove_from_order(dict, key);
        dict_try_shrink(dict);
    }
}

// 获取数组部分大小（用于迭代）
int dict_get_array_size(ObjDict* dict) {
    if (!dict) return 0;
    return dict->asize;
}

// 获取哈希部分条目数
int dict_get_hash_count(ObjDict* dict) {
    if (!dict) return 0;
    return dict->count;
}

// 将键转为字符串表示（主要用于 keys() 方法返回字符串键）
ObjString* dict_key_to_string(Value key) {
    if (val_is_obj(key) && val_as_obj(key)->type == OBJ_STRING) {
        return (ObjString*)val_as_obj(key);
    }
    if (val_is_int(key)) {
        char buf[32];
        int n = val_as_int(key);
        int len = snprintf(buf, sizeof(buf), "%d", n);
        return str_new(buf, len);
    }
    if (val_is_float(key)) {
        char buf[64];
        double d = val_as_double(key);
        int len = snprintf(buf, sizeof(buf), "%g", d);
        return str_new(buf, len);
    }
    if (val_is_bool(key)) {
        return str_new(val_as_bool(key) ? "true" : "false", val_as_bool(key) ? 4 : 5);
    }
    if (val_is_null(key)) {
        return str_new("null", 4);
    }
    return str_new("[object]", 8);
}

// 按索引获取字典值
Value dict_get_value_by_index(ObjDict* dict, int index) {
    if (!dict || index < 0) return val_null();

    if (dict->order && index < dict->order_count) {
        Value key = dict->order[index];
        return dict_get(dict, key);
    }

    return val_null();
}

// ============================================================================
// 字典实例方法支持（使用通用 MethodTable）
// ============================================================================

#define DICT_METHOD_TABLE_INITIAL_CAPACITY 32

static THREAD_LOCAL MethodTable dictMethodTable = {NULL, 0, 0};

void dict_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                       TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&dictMethodTable, "Dict", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

ObjNative* dict_find_method(const char* name) {
    return method_table_find(&dictMethodTable, name);
}

void dict_init_methods(void) {
    method_table_init_methods(&dictMethodTable, DICT_METHOD_TABLE_INITIAL_CAPACITY);
}

void dict_mark_methods(void) {
    method_table_mark(&dictMethodTable);
}

DictMethodEntry dict_find_method_meta(const char* name) {
    return method_table_find_meta(&dictMethodTable, name);
}
