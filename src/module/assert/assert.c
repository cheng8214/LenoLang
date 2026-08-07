#include "../../include/lenolang.h"
#include "../../include/native.h"

// 递归深度比较两个 Value 是否相等
static int value_deep_equal(Value a, Value b, int depth) {
    // 防止循环引用导致栈溢出
    if (depth > 20) {
        return 0;
    }

    if (val_is_int(a) && val_is_int(b)) {
        return val_as_int(a) == val_as_int(b);
    } else if (val_is_bigint(a) && val_is_bigint(b)) {
        return bigint_compare(val_as_bigint(a), val_as_bigint(b)) == 0;
    } else if (val_is_int(a) && val_is_bigint(b)) {
        return bigint_compare(bigint_from_int64(val_as_int(a)), val_as_bigint(b)) == 0;
    } else if (val_is_bigint(a) && val_is_int(b)) {
        return bigint_compare(val_as_bigint(a), bigint_from_int64(val_as_int(b))) == 0;
    } else if (val_is_float(a) && val_is_float(b)) {
        return val_as_double(a) == val_as_double(b);
    } else if (val_is_int(a) && val_is_float(b)) {
        return (double)val_as_int(a) == val_as_double(b);
    } else if (val_is_float(a) && val_is_int(b)) {
        return val_as_double(a) == (double)val_as_int(b);
    } else if (val_is_bool(a) && val_is_bool(b)) {
        return val_as_bool(a) == val_as_bool(b);
    } else if (val_is_null(a) && val_is_null(b)) {
        return 1;
    } else if (val_is_obj(a) && val_is_obj(b)) {
        Object* obj_a = val_as_obj(a);
        Object* obj_b = val_as_obj(b);

        // 同一对象直接相等
        if (obj_a == obj_b) return 1;

        // 类型不同直接不等
        if (obj_a->type != obj_b->type) return 0;

        switch (obj_a->type) {
            case OBJ_STRING: {
                ObjString* str_a = (ObjString*)obj_a;
                ObjString* str_b = (ObjString*)obj_b;
                return (str_a->len == str_b->len) &&
                       (memcmp(str_a->chars, str_b->chars, str_a->len) == 0);
            }
            case OBJ_ARRAY: {
                ObjArray* arr_a = (ObjArray*)obj_a;
                ObjArray* arr_b = (ObjArray*)obj_b;
                if (arr_a->count != arr_b->count) return 0;
                for (int i = 0; i < arr_a->count; i++) {
                    if (!value_deep_equal(arr_a->elements[i], arr_b->elements[i], depth + 1)) {
                        return 0;
                    }
                }
                return 1;
            }
            case OBJ_DICT: {
                ObjDict* dict_a = (ObjDict*)obj_a;
                ObjDict* dict_b = (ObjDict*)obj_b;
                // 比较键值对数量
                int count_a = dict_a->acount + dict_a->count;
                int count_b = dict_b->acount + dict_b->count;
                if (count_a != count_b) return 0;
                // 遍历 dict_a 的所有键值对，在 dict_b 中查找
                // 数组部分
                for (int i = 0; i < dict_a->asize; i++) {
                    Value va = dict_a->array[i];
                    if (val_is_null(va)) continue;
                    Value vb = dict_b->array && i < dict_b->asize ? dict_b->array[i] : val_null();
                    if (!value_deep_equal(va, vb, depth + 1)) return 0;
                }
                // 哈希部分
                for (int i = 0; i < dict_a->capacity; i++) {
                    ObjDictEntry* entry = &dict_a->entries[i];
                    Value entry_key = entry->key;
                    if (val_is_null(entry_key) || entry_key == DICT_TOMBSTONE_VAL) continue;
                    Value vb = dict_get(dict_b, entry_key);
                    if (!value_deep_equal(entry->value, vb, depth + 1)) return 0;
                }
                return 1;
            }
            default:
                // 其他对象类型（函数、struct 等）用指针比较
                return obj_a == obj_b;
        }
    }

    return 0;
}

static Value assert_func(int argc, Value* args) {
    if (!val_is_truthy(args[0])) {
        if (argc >= 2 && val_is_string(args[1])) {
            ObjString* msg = (ObjString*)val_as_obj(args[1]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败: 表达式为假");
        }
    }

    return val_bool(1);
}

static Value assert_eq_func(int argc, Value* args) {
    Value a = args[0];
    Value b = args[1];

    int equal = value_deep_equal(a, b, 0);

    if (!equal) {
        if (argc >= 3 && val_is_string(args[2])) {
            ObjString* msg = (ObjString*)val_as_obj(args[2]);
            native_throw_error(msg->chars);
        } else {
            // val_to_string 使用线程局部静态缓冲区，两次调用会互相覆盖
            // 必须先将第一次结果复制到独立缓冲区
            char a_str[BUFFER_MEDIUM];
            char b_str[BUFFER_MEDIUM];
            snprintf(b_str, sizeof(b_str), "%s", val_to_string(b));
            snprintf(a_str, sizeof(a_str), "%s", val_to_string(a));
            char buf[BUFFER_XLARGE];
            snprintf(buf, sizeof(buf), "断言失败 (assert_eq): 期望 %s, 实际 %s",
                     b_str, a_str);
            native_throw_error(buf);
        }
    }

    return val_bool(1);
}

static Value assert_ne_func(int argc, Value* args) {
    Value a = args[0];
    Value b = args[1];

    int equal = value_deep_equal(a, b, 0);

    if (equal) {
        if (argc >= 3 && val_is_string(args[2])) {
            ObjString* msg = (ObjString*)val_as_obj(args[2]);
            native_throw_error(msg->chars);
        } else {
            char buf[BUFFER_MEDIUM];
            snprintf(buf, sizeof(buf), "断言失败 (assert_ne): 值相等 (都是 %s)",
                     val_to_string(a));
            native_throw_error(buf);
        }
    }

    return val_bool(1);
}

static Value assert_true_func(int argc, Value* args) {
    if (!val_is_truthy(args[0])) {
        if (argc >= 2 && val_is_string(args[1])) {
            ObjString* msg = (ObjString*)val_as_obj(args[1]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败 (assert_true): 值不为真");
        }
    }

    return val_bool(1);
}

static Value assert_false_func(int argc, Value* args) {
    if (val_is_truthy(args[0])) {
        if (argc >= 2 && val_is_string(args[1])) {
            ObjString* msg = (ObjString*)val_as_obj(args[1]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败 (assert_false): 值不为假");
        }
    }

    return val_bool(1);
}

static Value assert_null_func(int argc, Value* args) {
    if (!val_is_null(args[0])) {
        if (argc >= 2 && val_is_string(args[1])) {
            ObjString* msg = (ObjString*)val_as_obj(args[1]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败 (assert_null): 值不为 null");
        }
    }

    return val_bool(1);
}

void assert_init_globals(void) {
    // assert(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert", assert_func, -1, 1, 2, TYPE_BOOL, TYPE_UNKNOWN, NULL);
    // assert_eq(a, b, [msg]) - 2 或 3 个参数
    vm_register_native("assert_eq", assert_eq_func, -1, 2, 3, TYPE_BOOL, TYPE_UNKNOWN, NULL);
    // assert_ne(a, b, [msg]) - 2 或 3 个参数
    vm_register_native("assert_ne", assert_ne_func, -1, 2, 3, TYPE_BOOL, TYPE_UNKNOWN, NULL);
    // assert_true(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_true", assert_true_func, -1, 1, 2, TYPE_BOOL, TYPE_UNKNOWN, NULL);
    // assert_false(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_false", assert_false_func, -1, 1, 2, TYPE_BOOL, TYPE_UNKNOWN, NULL);
    // assert_null(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_null", assert_null_func, -1, 1, 2, TYPE_BOOL, TYPE_UNKNOWN, NULL);
}
