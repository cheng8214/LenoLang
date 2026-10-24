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

    size_t old_size = (size_t)old_capacity * sizeof(ObjDictEntry);
    size_t new_size = (size_t)new_capacity * sizeof(ObjDictEntry);

    dict->entries = (ObjDictEntry*)malloc(new_capacity * sizeof(ObjDictEntry));
    if (!dict->entries) {
        native_throw_error("字典扩容内存分配失败");
        dict->entries = old_entries;
        return;
    }

    // 追踪哈希表内存变化，确保 GC 正确计算字典的内存占用
    gc_track_memory((Object*)dict, old_size, new_size);

    for (int i = 0; i < new_capacity; i++) {
        dict->entries[i].key = NULL_VAL;
        dict->entries[i].value = val_null();
        dict->entries[i].order_index = -1;
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
                    dict->entries[index].order_index = old_entries[i].order_index;  // 顺序下标跟着搬 ✓
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

// 供 VM 的字典读写内联缓存用（见 leno_vm.h 的 InlineDictCacheEntry）：
// 返回该键在**哈希部分**的槽位号；找不到、或该键走数组部分 ⇒ -1（不缓存）。
// 只认哈希部分：整数键走数组部分时槽位语义完全不同，拿去当 entries 下标会写坏字典。
int dict_slot_for(ObjDict* dict, Value key) {
    if (!dict || dict->capacity == 0) return -1;
    if (key_to_array_index(key) >= 0) return -1;
    return dict_find_entry(dict, key);
}

// 数组部分扩容
static int dict_array_resize(ObjDict* dict, int new_size) {
    if (new_size <= dict->asize) return 1;
    int capacity = 4;
    while (capacity < new_size) capacity *= 2;

    size_t old_size = (size_t)dict->asize * sizeof(Value);
    size_t new_cap_size = (size_t)capacity * sizeof(Value);

    Value* new_array = (Value*)realloc(dict->array, new_cap_size);
    if (!new_array) return 0;
    for (int i = dict->asize; i < capacity; i++) {
        new_array[i] = val_null();
    }
    // ★ 2026-10-19：数组部分的 order 下标（与 array 同长）
    int* new_aorder = (int*)realloc(dict->aorder, (size_t)capacity * sizeof(int));
    if (!new_aorder) { dict->array = new_array; return 0; }   // 回滚价值不大：至少别越界
    for (int i = dict->asize; i < capacity; i++) { new_aorder[i] = -1; }
    // 追踪数组部分内存变化，确保 GC 正确计算字典的内存占用
    gc_track_memory((Object*)dict, old_size, new_cap_size);
    dict->array = new_array;
    dict->aorder = new_aorder;
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
    dict->aorder = NULL;
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

// 前向声明（dict_set 里"数组部分优先 ⇒ 摘掉哈希里那份"要用 ✓）
static void dict_order_remove_at(ObjDict* dict, int pos);

// 添加键到插入顺序数组；返回它在 order 里的下标（失败 -1）
static int dict_add_to_order(ObjDict* dict, Value key) {
    if (dict->order_count >= dict->order_capacity) {
        int old_capacity = dict->order_capacity;
        int new_capacity = old_capacity < 8 ? 8 : old_capacity * 2;
        size_t old_size = (size_t)old_capacity * sizeof(Value);
        size_t new_size = (size_t)new_capacity * sizeof(Value);
        Value* new_order = (Value*)realloc(dict->order, new_size);
        if (!new_order) return -1;
        // 追踪 order 数组内存变化，确保 GC 正确计算字典的内存占用
        gc_track_memory((Object*)dict, old_size, new_size);
        dict->order = new_order;
        dict->order_capacity = new_capacity;
    }

    int pos = dict->order_count;
    dict->order[dict->order_count++] = key;
    // GC 写屏障：仅当键是对象时才需要
    if (val_is_obj(key)) {
        gc_write_barrier((Object*)dict, key);
    }
    return pos;
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
        // ★★ 2026-10-19（fuzz 抓到）：同一个整数键可能**先走哈希**（稀疏保护：下标远超 asize），
        //   之后又落进数组部分 ⇒ 两处各存一份 ⇒ `len()` 多算 1、`keys()` 出现重复键 ✗
        //   （实测：随机增删 5000 次后 keys() 里出现重复的 108 / 148）
        //   口径：数组部分优先 ⇒ 写数组前先把哈希里那份摘掉（连它的顺序表项一起 ✓ O(1)）
        if (dict->count > 0) {
            int hidx = dict_find_entry(dict, key);
            if (hidx >= 0) {
                int hpos = dict->entries[hidx].order_index;
                dict->entries[hidx].key = DICT_TOMBSTONE_VAL;
                dict->entries[hidx].value = val_null();
                dict->entries[hidx].order_index = -1;
                dict->count--;
                dict->tombstone_count++;
                dict_order_remove_at(dict, hpos);
            }
        }

        int is_new = val_is_null(dict->array[array_index]);
        if (is_new) {
            dict->acount++;
            dict->aorder[array_index] = dict_add_to_order(dict, key);
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
        dict->entries[index].order_index = dict_add_to_order(dict, key);
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
        dict->entries[index].order_index = dict_add_to_order(dict, key);
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
    return dict_get_slot(dict, key, NULL);
}

// ★ 2026-10-03：一次查找同时给值与哈希槽位（唯一实现，dict_get 是它的包装 ✓）
//   为什么要合并：VM 的字典内联缓存未命中后还要回填槽位，原先实现是
//   `dict_get(...)`（查一遍）+ `dict_slot_for(...)`（再查一遍）⇒ 同一次访问**两次查找** ✗
//   （实测最贵的是 str 键轮转访问：IC 单槽缓存必然 miss ⇒ 每次都吃两遍查找）
//   语义与 dict_get 完全一致：数组部分优先（越界/洞 ⇒ 落哈希），值合法性过滤同口径 ✓
//   out_slot 只在"命中哈希部分"时写槽位；数组部分/不存在 ⇒ 写 -1（与 dict_slot_for 同约定 ✓）
Value dict_get_slot(ObjDict* dict, Value key, int* out_slot) {
    if (out_slot) *out_slot = -1;
    if (!dict) return val_null();

    // 先检查数组部分
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        Value value = dict->array[array_index];
        if (!val_is_null(value)) {
            if (!is_valid_obj_value(value)) return val_null();
            return value;
        }
        // 洞 ⇒ 落到哈希（不变量：数组部分的键不会同时在哈希里 ⇒ 结果必为 null ✓）
        //   ⚠ 但**不能**提前 return：下面统一 return val_null() 也行，这里保留原流程以免语义漂移
    }

    // 查哈希部分
    if (dict->capacity == 0) return val_null();
    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        Value value = dict->entries[index].value;
        if (!is_valid_obj_value(value)) return val_null();
        if (out_slot) *out_slot = index;   // 命中哈希部分 ⇒ 可回填 IC ✓
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

// 从 order 数组移除**指定下标**（swap-remove）+ 回填被搬动那条的索引 ⇒ **O(1)**
//   ★ 2026-10-19：原实现按 key **线性扫描** order 数组 ⇒ 删除 O(n)：
//     10 万键删 1k/10k/50k = 116ms/1053ms/**4022ms** ✗（实测）。
//   现在下标存在两处：哈希条目 `entries[i].order_index`、数组部分 `aorder[idx]` ✓
static void dict_order_remove_at(ObjDict* dict, int pos) {
    if (pos < 0 || pos >= dict->order_count) return;
    int last = dict->order_count - 1;
    Value moved = dict->order[last];
    dict->order_count = last;
    if (pos == last) return;
    dict->order[pos] = moved;
    // 被搬动的那条：把它的下标从 last 改成 pos
    int ai = key_to_array_index(moved);
    if (ai >= 0 && ai < dict->asize && dict->aorder && !val_is_null(dict->array[ai])) {
        dict->aorder[ai] = pos;
        return;
    }
    int slot = dict_find_entry(dict, moved);
    if (slot >= 0) dict->entries[slot].order_index = pos;
}

void dict_try_shrink(ObjDict* dict) {
    if (dict->capacity > 16 && dict->count < dict->capacity * 0.25) {
        int new_capacity = dict->capacity / 2;
        if (new_capacity < 8) new_capacity = 8;
        dict_resize(dict, new_capacity);
    }

    if (dict->order_capacity > 16 && dict->order_count < dict->order_capacity / 4) {
        int old_cap = dict->order_capacity;
        int new_cap = old_cap / 2;
        if (new_cap < 8) new_cap = 8;
        Value* new_order = (Value*)realloc(dict->order, new_cap * sizeof(Value));
        if (new_order) {
            gc_track_memory((Object*)dict, (size_t)old_cap * sizeof(Value), (size_t)new_cap * sizeof(Value));
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
                gc_track_memory((Object*)dict, (size_t)dict->asize * sizeof(Value), (size_t)new_asize * sizeof(Value));
                dict->array = new_array;
                // aorder 与 array 同长（收缩时一起缩；失败的旧指针仍然有效，只是多占一点 ✓）
                int* new_aorder = (int*)realloc(dict->aorder, (size_t)new_asize * sizeof(int));
                if (new_aorder) dict->aorder = new_aorder;
                dict->asize = new_asize;
            }
        }
    }
}

// ★ 2026-10-24：删除并返回被删掉的值（键不存在 ⇒ null），**一次查找**完成。
//   为什么加：`Dict.remove` 现在要返回值（round7 Q9），若用
//     `dict_has` + `dict_get` + `dict_delete` 拼就是**三次查找** ✗
//   ⇒ 把删除本身提成"顺手带出旧值"的形态，`dict_delete` 成为它的包装（唯一实现）。
//   ⚠ 返回值的合法性口径与 dict_get 一致：数组部分的**洞**不返回值（落到哈希继续找），
//     哈希命中的值原样返回（删除前取出，删除后那格被清空 ✓）。
Value dict_take(ObjDict* dict, Value key) {
    if (!dict) return val_null();

    // 先尝试从数组部分删除
    int array_index = key_to_array_index(key);
    if (array_index >= 0 && array_index < dict->asize) {
        Value old = dict->array[array_index];
        if (!val_is_null(old)) {
            int pos = (dict->aorder && array_index < dict->asize) ? dict->aorder[array_index] : -1;
            dict->array[array_index] = val_null();
            if (dict->aorder && array_index < dict->asize) dict->aorder[array_index] = -1;
            dict->acount--;
            dict_order_remove_at(dict, pos);
            dict_try_shrink(dict);
            return old;
        }
    }

    // 从哈希部分删除
    if (dict->capacity == 0) return val_null();

    int index = dict_find_entry(dict, key);
    if (index >= 0) {
        Value old = dict->entries[index].value;
        int pos = dict->entries[index].order_index;
        dict->entries[index].key = DICT_TOMBSTONE_VAL;
        dict->entries[index].value = val_null();
        dict->entries[index].order_index = -1;
        dict->count--;
        dict->tombstone_count++;
        dict_order_remove_at(dict, pos);
        dict_try_shrink(dict);
        return old;
    }
    return val_null();
}

void dict_delete(ObjDict* dict, Value key) {
    (void)dict_take(dict, key);   // 只要副作用（见 dict_take 的说明）
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
        val_format_float(val_as_double(key), buf, sizeof(buf));
        return str_new(buf, (int)strlen(buf));
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

// 2026-10-19：线程退出清理（见 object_array.c 的同名函数说明）
void dict_method_table_cleanup(void) { method_table_free(&dictMethodTable); }

void dict_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params) {
    method_table_register_method(&dictMethodTable, "Dict", name, method, return_type, return_element_type, params);
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

