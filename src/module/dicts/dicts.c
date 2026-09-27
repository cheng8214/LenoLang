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
    // ★ 不能用 memset(0)：全零字节不是 NULL_VAL（0xFFF8000000000000），
    //   会导致 dict_find_entry 的 val_is_null 判断失效，引发无限循环/崩溃。
    //   必须逐个将 key 设为 NULL_VAL。
    dict->count = 0;
    dict->tombstone_count = 0;
    if (dict->entries) {
        for (int i = 0; i < dict->capacity; i++) {
            dict->entries[i].key = NULL_VAL;
            dict->entries[i].value = val_null();
        }
    }

    // 清空插入序
    dict->order_count = 0;

    return val_null();
}

// ==================== 初始化 ====================

void dicts_init_instance_methods(void) {
    dict_init_methods();

    // ---- "类型跟接收者走"的返回规格（2026-09-27）----
    //   keys() 返回 `Array[K]`、values() 返回 `Array[V]`、get(k[,default]) 返回 `V`
    //   （K/V = 接收者 `Dict[K,V]` 的键/值类型）。此前 keys/values 注册成 `TYPE_ARRAY + TYPE_UNKNOWN`
    //   ⇒ 拿到裸 `Array`、元素是 any；get 注册成 TYPE_ANY。用关系型标签声明后，调用点零收窄 ✓
    //   ⚠ set()/remove()/clear() **不**在此列：它们实现上返回 `null`（不是"值"），标成 V 会骗人
    //   ⇒ 它们标的是 `null`（见下方各自的 `return_spec`）。
    //   （两种关系型规格本身已上提为预制 `NATIVE_T_ARG0_KEY` / `NATIVE_T_ARG0_VALUE`，见 native.h；
    //    `Array[...]` 的包装就地组合 —— 只有本模块要这两条，没必要再预制。）
    static const NativeTypeSpec S_ARR_ARG0_K  = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG0_KEY,   NULL };
    static const NativeTypeSpec S_ARR_ARG0_V  = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG0_VALUE, NULL };
    
    TypeKind len_params[] = {};
    dict_register_method_with_params("len", make_native(dict_method_len, 1, "len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

    TypeKind has_params[] = {TYPE_ANY};
    dict_register_method_with_params("has", make_native(dict_method_has, 2, "has"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, has_params);

    TypeKind get_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method_with_params("get", make_native(dict_method_get, 3, "get"), -1, 1, 2, TYPE_ANY, TYPE_UNKNOWN, get_params);
    // ⚠ get **不能**标 `ARG0_VALUE`：它的类型是由**默认值实参**推出来的（`get("y", 0.0)` → float，
    //   见 assert/test_dict_get_infer.leno），语义侧已有更精确的特例。标成 V 会把它盖掉
    //   （实测：混合类型 dict 的值类型是 any ⇒ `d.get(k, 0.0)` 退化成 any，5 个用例回归）。
    //   那是"按默认值实参推返回类型"的另一族标签，需要时再设计（YAGNI）。

    TypeKind set_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method_with_params("set", make_native(dict_method_set, 3, "set"), 2, -1, -1, TYPE_ANY, TYPE_UNKNOWN, set_params);
    // set/remove/clear 的实现都是 `return val_null()`（不是"值"）⇒ 把返回类型从 any 收紧为 `null`。
    //   为什么不标 ARG0_VALUE：那会骗人（`val_obj(d.set(k,v))` 这种写法本来就不成立）。
    //   风险实测：运行期早就返回 null（`d.set(k,v).set(...)` 之类链式写法从来跑不通）
    //   ⇒ 只影响编译期推断，不会让任何**本来能跑**的代码变坏。
    native_register_instance_method_return_spec("Dict", "set", &NATIVE_T_NULL);

    TypeKind remove_params[] = {TYPE_ANY};
    dict_register_method_with_params("remove", make_native(dict_method_remove, 2, "remove"), 1, -1, -1, TYPE_ANY, TYPE_UNKNOWN, remove_params);
    native_register_instance_method_return_spec("Dict", "remove", &NATIVE_T_NULL);

    TypeKind clear_params[] = {};
    dict_register_method_with_params("clear", make_native(dict_method_clear, 1, "clear"), 0, -1, -1, TYPE_ANY, TYPE_UNKNOWN, clear_params);
    native_register_instance_method_return_spec("Dict", "clear", &NATIVE_T_NULL);

    TypeKind keys_params[] = {};
    dict_register_method_with_params("keys", make_native(dict_method_keys, 1, "keys"), 0, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, keys_params);
    native_register_instance_method_return_spec("Dict", "keys", &S_ARR_ARG0_K);

    TypeKind values_params[] = {};
    dict_register_method_with_params("values", make_native(dict_method_values, 1, "values"), 0, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, values_params);
    native_register_instance_method_return_spec("Dict", "values", &S_ARR_ARG0_V);
}
