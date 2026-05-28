#include "include/native.h"
#include <string.h>

// tombstone 哨兵（线程局部存储，前向声明）
extern _Thread_local ObjString* tombstone;

// 前向声明：字典实例方法支持函数（定义在 object_dict.c）
extern void dict_init_methods(void);
extern void dict_register_method_with_params(const char* name, ObjNative* method, int arity,
                                              int min_arity, int max_arity,
                                              TypeKind return_type, TypeKind* param_types);

// ==================== 核心方法实现 ====================

static Value dict_method_len(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    return val_int(dict->count);
}

static Value dict_method_has(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjString* key = (ObjString*)val_as_obj(args[1]);
    extern int dict_has(ObjDict* dict, ObjString* key);
    return val_bool(dict_has(dict, key));
}

static Value dict_method_get(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjString* key = (ObjString*)val_as_obj(args[1]);
    extern Value dict_get(ObjDict* dict, ObjString* key);
    return dict_get(dict, key);
}

static Value dict_method_set(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjString* key = (ObjString*)val_as_obj(args[1]);
    extern void dict_set(ObjDict* dict, ObjString* key, Value value);
    dict_set(dict, key, args[2]);
    return val_null();
}

static Value dict_method_remove(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjString* key = (ObjString*)val_as_obj(args[1]);
    extern void dict_delete(ObjDict* dict, ObjString* key);
    dict_delete(dict, key);
    return val_null();
}

static Value dict_method_keys(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return val_null();

    arr->count = dict->count;
    arr->capacity = dict->count;
    arr->elements = NULL;

    if (dict->count > 0) {
        arr->elements = (Value*)malloc(dict->count * sizeof(Value));
        if (!arr->elements) {
            native_throw_error("数组内存分配失败");
            return val_null();
        }

        int idx = 0;
        for (int i = 0; i < dict->capacity; i++) {
            ObjString* key = dict->entries[i].key;
            if (key != NULL) {
                if (key != tombstone) {
                    arr->elements[idx++] = val_obj((Object*)key);
                }
            }
        }
    }

    return val_obj((Object*)arr);
}

static Value dict_method_values(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return val_null();
    
    arr->count = dict->count;
    arr->capacity = dict->count;
    arr->elements = NULL;
    
    if (dict->count > 0) {
        arr->elements = (Value*)malloc(dict->count * sizeof(Value));
        if (!arr->elements) {
            native_throw_error("数组内存分配失败");
            return val_null();
        }
        
        int idx = 0;
        for (int i = 0; i < dict->capacity; i++) {
            ObjString* key = dict->entries[i].key;
            if (key != NULL) {
                if (key != tombstone) {
                    arr->elements[idx++] = dict->entries[i].value;
                }
            }
        }
    }
    
    return val_obj((Object*)arr);
}

// ==================== 初始化 ====================

void dicts_init_instance_methods(void) {
    dict_init_methods();
    
    TypeKind len_params[] = {};
    dict_register_method_with_params("len", make_native(dict_method_len, 1, "len"), 0, -1, -1, TYPE_INT, len_params);

    TypeKind has_params[] = {TYPE_STRING};
    dict_register_method_with_params("has", make_native(dict_method_has, 2, "has"), 1, -1, -1, TYPE_BOOL, has_params);

    TypeKind get_params[] = {TYPE_STRING};
    dict_register_method_with_params("get", make_native(dict_method_get, 2, "get"), 1, -1, -1, TYPE_ANY, get_params);

    TypeKind set_params[] = {TYPE_STRING, TYPE_ANY};
    dict_register_method_with_params("set", make_native(dict_method_set, 3, "set"), 2, -1, -1, TYPE_ANY, set_params);

    TypeKind remove_params[] = {TYPE_STRING};
    dict_register_method_with_params("remove", make_native(dict_method_remove, 2, "remove"), 1, -1, -1, TYPE_ANY, remove_params);

    TypeKind keys_params[] = {};
    dict_register_method_with_params("keys", make_native(dict_method_keys, 1, "keys"), 0, -1, -1, TYPE_ARRAY, keys_params);

    TypeKind values_params[] = {};
    dict_register_method_with_params("values", make_native(dict_method_values, 1, "values"), 0, -1, -1, TYPE_ARRAY, values_params);
}
