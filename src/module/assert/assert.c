#include "../../include/lenolang.h"
#include "../../include/native.h"

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

    int equal = 0;
    if (val_is_int(a) && val_is_int(b)) {
        equal = val_as_int(a) == val_as_int(b);
    } else if (val_is_bigint(a) && val_is_bigint(b)) {
        equal = bigint_compare(val_as_bigint(a), val_as_bigint(b)) == 0;
    } else if (val_is_int(a) && val_is_bigint(b)) {
        equal = bigint_compare(bigint_from_int64(val_as_int(a)), val_as_bigint(b)) == 0;
    } else if (val_is_bigint(a) && val_is_int(b)) {
        equal = bigint_compare(val_as_bigint(a), bigint_from_int64(val_as_int(b))) == 0;
    } else if (val_is_float(a) && val_is_float(b)) {
        equal = val_as_double(a) == val_as_double(b);
    } else if (val_is_int(a) && val_is_float(b)) {
        equal = (double)val_as_int(a) == val_as_double(b);
    } else if (val_is_float(a) && val_is_int(b)) {
        equal = val_as_double(a) == (double)val_as_int(b);
    } else if (val_is_bool(a) && val_is_bool(b)) {
        equal = val_as_bool(a) == val_as_bool(b);
    } else if (val_is_null(a) && val_is_null(b)) {
        equal = 1;
    } else if (val_is_obj(a) && val_is_obj(b)) {
        Object* obj_a = val_as_obj(a);
        Object* obj_b = val_as_obj(b);

        // 字符串需要比较内容
        if (obj_a->type == OBJ_STRING && obj_b->type == OBJ_STRING) {
            ObjString* str_a = (ObjString*)obj_a;
            ObjString* str_b = (ObjString*)obj_b;
            equal = (str_a->len == str_b->len) &&
                    (memcmp(str_a->chars, str_b->chars, str_a->len) == 0);
        } else {
            equal = obj_a == obj_b;
        }
    } else {
        equal = 0;
    }

    if (!equal) {
        if (argc >= 3 && val_is_string(args[2])) {
            ObjString* msg = (ObjString*)val_as_obj(args[2]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败 (assert_eq): 值不相等");
        }
    }

    return val_bool(1);
}

static Value assert_ne_func(int argc, Value* args) {
    Value a = args[0];
    Value b = args[1];

    int equal = 0;
    if (val_is_int(a) && val_is_int(b)) {
        equal = val_as_int(a) == val_as_int(b);
    } else if (val_is_bigint(a) && val_is_bigint(b)) {
        equal = bigint_compare(val_as_bigint(a), val_as_bigint(b)) == 0;
    } else if (val_is_int(a) && val_is_bigint(b)) {
        equal = bigint_compare(bigint_from_int64(val_as_int(a)), val_as_bigint(b)) == 0;
    } else if (val_is_bigint(a) && val_is_int(b)) {
        equal = bigint_compare(val_as_bigint(a), bigint_from_int64(val_as_int(b))) == 0;
    } else if (val_is_float(a) && val_is_float(b)) {
        equal = val_as_double(a) == val_as_double(b);
    } else if (val_is_bool(a) && val_is_bool(b)) {
        equal = val_as_bool(a) == val_as_bool(b);
    } else if (val_is_null(a) && val_is_null(b)) {
        equal = 1;
    } else if (val_is_obj(a) && val_is_obj(b)) {
        equal = val_as_obj(a) == val_as_obj(b);
    } else {
        equal = 0;
    }

    if (equal) {
        if (argc >= 3 && val_is_string(args[2])) {
            ObjString* msg = (ObjString*)val_as_obj(args[2]);
            native_throw_error(msg->chars);
        } else {
            native_throw_error("断言失败 (assert_ne): 值相等");
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
    vm_register_native("assert", assert_func, -1, 1, 2, TYPE_BOOL, NULL);
    // assert_eq(a, b, [msg]) - 2 或 3 个参数
    vm_register_native("assert_eq", assert_eq_func, -1, 2, 3, TYPE_BOOL, NULL);
    // assert_ne(a, b, [msg]) - 2 或 3 个参数
    vm_register_native("assert_ne", assert_ne_func, -1, 2, 3, TYPE_BOOL, NULL);
    // assert_true(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_true", assert_true_func, -1, 1, 2, TYPE_BOOL, NULL);
    // assert_false(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_false", assert_false_func, -1, 1, 2, TYPE_BOOL, NULL);
    // assert_null(value, [msg]) - 1 或 2 个参数
    vm_register_native("assert_null", assert_null_func, -1, 1, 2, TYPE_BOOL, NULL);
}
