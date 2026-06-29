#include "include/native.h"
#include <string.h>

// 前向声明：字典实例方法支持函数（定义在 object_dict.c）
extern void dict_init_methods(void);
extern void dict_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// ==================== 核心方法实现 ====================

static Value dict_method_len(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    // 总数 = 数组部分元素 + 哈希部分条目
    return val_int(dict->acount + dict->count);
}

static Value dict_method_has(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    return val_bool(dict_has(dict, args[1]));
}

static Value dict_method_get(int argc, Value* args) {
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    Value key = args[1];
    // 键存在则返回对应值，否则返回默认值（第2参数）或 null
    if (dict_has(dict, key)) {
        return dict_get(dict, key);
    }
    return argc >= 3 ? args[2] : val_null();
}

static Value dict_method_set(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    dict_set(dict, args[1], args[2]);
    return val_null();
}

static Value dict_method_remove(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    dict_delete(dict, args[1]);
    return val_null();
}

static Value dict_method_keys(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    int total = dict->order_count;
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return val_null();

    arr->count = total;
    arr->capacity = total;
    arr->elements = NULL;

    if (total > 0) {
        arr->elements = (Value*)malloc(total * sizeof(Value));
        if (!arr->elements) {
            native_throw_error("数组内存分配失败");
            return val_null();
        }

        for (int i = 0; i < dict->order_count; i++) {
            Value key = dict->order[i];
            // 尽量保持键的原有类型（int 仍为 int, string 仍为 string）
            arr->elements[i] = key;
        }
    }

    return val_obj((Object*)arr);
}

static Value dict_method_values(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    int total = dict->order_count;
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return val_null();
    
    arr->count = total;
    arr->capacity = total;
    arr->elements = NULL;
    
    if (total > 0) {
        arr->elements = (Value*)malloc(total * sizeof(Value));
        if (!arr->elements) {
            native_throw_error("数组内存分配失败");
            return val_null();
        }
        
        for (int i = 0; i < dict->order_count; i++) {
            arr->elements[i] = dict_get(dict, dict->order[i]);
        }
    }
    
    return val_obj((Object*)arr);
}

static Value dict_method_clear(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    if (!dict) return val_null();

    // 清空数组部分
    dict->acount = 0;
    dict->last_index = 0;

    // 清空哈希部分
    dict->count = 0;
    dict->tombstone_count = 0;
    if (dict->entries) {
        memset(dict->entries, 0, dict->capacity * sizeof(ObjDictEntry));
    }

    // 清空插入序
    dict->order_count = 0;

    return val_null();
}

// ==================== 初始化 ====================

void dicts_init_instance_methods(void) {
    dict_init_methods();
    
    TypeKind len_params[] = {};
    dict_register_method_with_params("len", make_native(dict_method_len, 1, "len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

    TypeKind has_params[] = {TYPE_ANY};
    dict_register_method_with_params("has", make_native(dict_method_has, 2, "has"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, has_params);

    TypeKind get_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method_with_params("get", make_native(dict_method_get, 3, "get"), -1, 1, 2, TYPE_ANY, TYPE_UNKNOWN, get_params);

    TypeKind set_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method_with_params("set", make_native(dict_method_set, 3, "set"), 2, -1, -1, TYPE_ANY, TYPE_UNKNOWN, set_params);

    TypeKind remove_params[] = {TYPE_ANY};
    dict_register_method_with_params("remove", make_native(dict_method_remove, 2, "remove"), 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, remove_params);

    TypeKind clear_params[] = {};
    dict_register_method_with_params("clear", make_native(dict_method_clear, 1, "clear"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, clear_params);

    TypeKind keys_params[] = {};
    dict_register_method_with_params("keys", make_native(dict_method_keys, 1, "keys"), 0, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, keys_params);

    TypeKind values_params[] = {};
    dict_register_method_with_params("values", make_native(dict_method_values, 1, "values"), 0, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, values_params);
}
