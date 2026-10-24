#include "include/native.h"
#include <string.h>

// 前向声明：字典实例方法支持函数（定义在 object_dict.c）
extern void dict_init_methods(void);
extern void dict_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);

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

// ★ 2026-10-24：const 深保护的运行期那一半（与 arrays.c 的 ARR_REJECT_READONLY 同口径）
//   const D = {...}; tryMut(D) 里函数体拿到的是形参 ⇒ 编译期拦不住 ⇒ 只读标记在这里兜住。
//   ⚠ 只放 mutator：len/has/get/keys/values/items/copy 都是只读操作 ✓
#define DICT_REJECT_READONLY()                                                         \
    do {                                                                              \
        if (obj_is_readonly(args[0])) {                                               \
            native_throw_error("const 字典不可修改（const 是**整体不可变**：需要可变字典请声明为 var）"); \
            return val_null();                                                        \
        }                                                                             \
    } while (0)
static Value dict_method_set(int argc, Value* args) {
    (void)argc;
    DICT_REJECT_READONLY();
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    dict_set(dict, args[1], args[2]);
    return val_null();
}

// remove(key)：删除并**返回被删掉的值**（键不存在 ⇒ null）。
//   ★ 2026-10-24（round7 Q9）：原来一律 `return val_null()` ⇒ "删成功" 与
//     "键本来就不存在" 返回值完全相同，调用方没法区分（`d.remove("a")` 和
//     `d.remove("zz")` 都是 null，只能另外写一次 `has()` ✗）。
//   与 `array.remove(i)` 对齐：那个一直返回被删掉的**元素**。
//   ⚠ 键存在但值是 null ⇒ 同样拿到 null ⇒ 要区分"存在但与删"请先 `has()`（文档里写明）。
//   兼容性：调用方忽略返回值（语句位置）时行为完全不变 ✓；没有任何既有代码用它的返回值
//   （全仓扫过：只有 `arr.remove(...)` 的结果被接过）。
static Value dict_method_remove(int argc, Value* args) {
    (void)argc;
    DICT_REJECT_READONLY();
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    // dict_take：删除 + 带出旧值，**一次查找**（原来 has+get+delete 是三次）
    return dict_take(dict, args[1]);
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

// setdefault(key, default)：键在 ⇒ 返回现值；键不在 ⇒ **写入** default 并返回它
//   （语义与 Python `dict.setdefault` 一致 ✓；参数个数由编译期把关 ⇒ 函数体不再判长度 ✓）
static Value dict_method_setdefault(int argc, Value* args) {
    (void)argc;
    DICT_REJECT_READONLY();
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    Value key = args[1];
    if (dict_has(dict, key)) {
        return dict_get(dict, key);
    }
    Value def = args[2];
    dict_set(dict, key, def);
    return def;
}

// items()：把字典摊平成 `Array[DictEntry{key, value}]`（**按插入序** ✓）
//   为什么不用"每项一个 `[k, v]` 小数组"：位置取值不可读、顺序一漂就静默错位 ✗
//   —— 与 `DirEntry`（dirs.walk）/ `RegexMatch`（regexs.find_all）同一判断：
//   编译期字段表与运行期 ObjStructDef **同源** ⇒ 字段顺序不可能各自漂 ✓
static Value dict_method_items(int argc, Value* args) {
    (void)argc;
    ObjDict* dict = (ObjDict*)val_as_obj(args[0]);
    int total = dict->order_count;

    ObjArray* arr = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!arr) return val_null();
    arr->count = 0;
    arr->capacity = total > 0 ? total : 1;
    arr->elements = (Value*)malloc(arr->capacity * sizeof(Value));
    if (!arr->elements) {
        native_throw_error("数组内存分配失败");
        return val_null();
    }

    Value arr_val = val_obj((Object*)arr);
    gc_push_root(&arr_val);        // 循环里 native_struct_new 会 gc_alloc ⇒ 还没交出去的它要护住 ✓

    for (int i = 0; i < total; i++) {
        Value k = dict->order[i];
        ObjStruct* e = native_struct_new("DictEntry");
        if (!e) break;
        Value ev = val_obj((Object*)e);
        gc_push_root(&ev);
        native_struct_set(e, "key", k);
        native_struct_set(e, "value", dict_get(dict, k));
        gc_pop_root();
        arr->elements[arr->count++] = ev;
        gc_write_barrier((Object*)arr, ev);
    }

    gc_pop_root();
    return arr_val;
}

// copy()：**深拷贝**（与 `Array.copy()` 同一口径 —— 逐元素递归，走 native.c 的 value_copy ✓）
//   ★ 2026-10-14 补：此前 Array / struct 都有 copy()，只有 Dict 没有 ✗（写 LenoWordBuddy 时撞到，
//     用户原话"Dict.copy() 没有（Array 有）"✓）。语义按"与 Array 对齐"定 ⇒ **深**（不是浅引用）：
//     `d.copy()` 之后改副本的嵌套数组/字典不会影响原字典 ✓。
//   ⚠ 代价要说清：值里若装着大块二进制（如 `Array[int]` 的信封，每字节一个 8 字节 Value）
//     深拷贝不便宜 ⇒ **只读场景直接传引用即可**，别为"试试看"拷贝 ✓。
//   ⚠ 为什么不是一行 `value_copy(args[0])` 就完事：字典那一支原先**丢插入序 + 丢 type_info**
//     ⇒ 已在 native.c 的 OBJ_DICT 分支一并修好（否则 `keys()` / `items()` 的顺序会被打乱）。
static Value dict_method_copy(int argc, Value* args) {
    (void)argc;
    return value_copy(args[0]);
}

static Value dict_method_clear(int argc, Value* args) {
    (void)argc;
    DICT_REJECT_READONLY();
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
    //   ⚠ set()/clear() **不**在此列：它们实现上返回 `null`（不是"值"），标成 V 会骗人
    //   ⇒ 它们标的是 `null`（见下方各自的 `return_spec`）。
    //   ⚠ remove() 2026-10-24 起**返回被删掉的值**（round7 Q9）⇒ 也不再标 null（见下方说明）。
    //   （两种关系型规格本身已上提为预制 `NATIVE_T_ARG0_KEY` / `NATIVE_T_ARG0_VALUE`，见 native.h；
    //    `Array[...]` 的包装就地组合 —— 只有本模块要这两条，没必要再预制。）
    static const NativeTypeSpec S_ARR_ARG0_K  = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG0_KEY,   NULL, 0, -1 };
    static const NativeTypeSpec S_ARR_ARG0_V  = { NTYPE_ARRAY, NULL, &NATIVE_T_ARG0_VALUE, NULL, 0, -1 };
    
    dict_register_method("len", make_native(dict_method_len, 1, "len"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    TypeKind has_params[] = {TYPE_ANY};
    dict_register_method("has", make_native(dict_method_has, 2, "has"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(has_params));

    TypeKind get_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method("get", make_native(dict_method_get, 3, "get"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_VARARG(1, 2, 2, get_params, TYPE_ANY));
    // ⚠ get **不能**标 `ARG0_VALUE`：它的类型是由**默认值实参**推出来的（`get("y", 0.0)` → float，
    //   见 assert/test_dict_get_infer.leno），语义侧已有更精确的特例。标成 V 会把它盖掉
    //   （实测：混合类型 dict 的值类型是 any ⇒ `d.get(k, 0.0)` 退化成 any，5 个用例回归）。
    //   那是"按默认值实参推返回类型"的另一族标签，需要时再设计（YAGNI）。

    TypeKind set_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method("set", make_native(dict_method_set, 3, "set"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED(set_params));
    // set/remove/clear 的实现都是 `return val_null()`（不是"值"）⇒ 把返回类型从 any 收紧为 `null`。
    //   为什么不标 ARG0_VALUE：那会骗人（`val_obj(d.set(k,v))` 这种写法本来就不成立）。
    //   风险实测：运行期早就返回 null（`d.set(k,v).set(...)` 之类链式写法从来跑不通）
    //   ⇒ 只影响编译期推断，不会让任何**本来能跑**的代码变坏。
    native_register_instance_method_return_spec("Dict", "set", &NATIVE_T_NULL);

    TypeKind remove_params[] = {TYPE_ANY};
    dict_register_method("remove", make_native(dict_method_remove, 2, "remove"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED(remove_params));
    // ⚠ remove **不再**标 `NATIVE_T_NULL`（2026-10-24 / round7 Q9）：它现在返回**被删掉的值**，
    //   键不存在才是 null ⇒ 标成 null 会撒谎（`var old = d.remove(k)` 被推成 null 类型）。
    //   也不标 ARG0_VALUE：键缺失时运行期仍是 null，标 V 等于承诺"一定非空"✓（同 `get` 的取舍：
    //   返回 any，调用点自己 `_int()` / `_str()` 收窄）。

    dict_register_method("clear", make_native(dict_method_clear, 1, "clear"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Dict", "clear", &NATIVE_T_NULL);

    dict_register_method("keys", make_native(dict_method_keys, 1, "keys"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Dict", "keys", &S_ARR_ARG0_K);

    dict_register_method("values", make_native(dict_method_values, 1, "values"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Dict", "values", &S_ARR_ARG0_V);

    // setdefault(key, default)：返回类型标 ANY —— 它"按默认值实参推"的那套标签还没设计，
    //   与 get 同一处境（见上面 get 的说明 ✓）
    TypeKind setdefault_params[] = {TYPE_ANY, TYPE_ANY};
    dict_register_method("setdefault", make_native(dict_method_setdefault, 3, "setdefault"), TYPE_ANY, TYPE_UNKNOWN, NATIVE_FIXED(setdefault_params));

    // items()：返回 `Array[DictEntry{key, value}]`（**字段类型编译期已知** ✓）
    //   key/value 都标 ANY —— 字典的键值类型本来就可能不齐 ⇒ 收窄交给调用点 ✓
    static const NativeTypeSpec S_DI_ANY        = { NTYPE_ANY,    NULL, NULL, NULL, 0, -1 };
    static const NativeTypeSpec S_DICTENTRY     = { NTYPE_STRUCT, "DictEntry", NULL, NULL, 0, -1 };
    static const NativeTypeSpec S_DICTENTRY_ARR = { NTYPE_ARRAY, NULL, &S_DICTENTRY, NULL, 0, -1 };
    static const char* DICTENTRY_FIELDS[] = { "key", "value" };
    static const NativeTypeSpec* DICTENTRY_TYPES[] = { &S_DI_ANY, &S_DI_ANY };
    static const NativeStructSpec DICTENTRY_SPEC = { "dicts", "DictEntry", 2, DICTENTRY_FIELDS, DICTENTRY_TYPES };
    native_register_struct_spec(&DICTENTRY_SPEC);

    dict_register_method("items", make_native(dict_method_items, 1, "items"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Dict", "items", &S_DICTENTRY_ARR);

    // copy()：深拷贝，返回**与接收者同型**的字典（2026-10-14 追加）
    //   返回类型用关系型标签就地组合 `Dict[ARG0_KEY, ARG0_VALUE]` ⇒ `Dict[string,int].copy()`
    //   仍是 `Dict[string,int]` ✓（若标成预制 `NATIVE_T_DICT`（裸 Dict）⇒ 拷完就丢泛型、
    //   取值退化成 any ⇒ 正是"裸容器 → any 传染"那类问题，不能这么标 ✗）。
    //   规格解析器对 `NTYPE_DICT` 是**递归**处理 sub/sub2 的（native.c：
    //   `key_type = impl(sub, …)` / `value_type = impl(sub2, …)`），且
    //   `native_type_spec_has_arg_ref` 也是递归判定 ⇒ 嵌套的关系标签能生效 ✓。
    //   裸 `Dict` 接收者 ⇒ 两个子标签都解析不出 ⇒ 自然退化成裸 Dict（与旧行为一致 ✓）。
    static const NativeTypeSpec S_DICT_ARG0 = { NTYPE_DICT, NULL, &NATIVE_T_ARG0_KEY, &NATIVE_T_ARG0_VALUE, 0, -1 };
    dict_register_method("copy", make_native(dict_method_copy, 1, "copy"), TYPE_DICT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("Dict", "copy", &S_DICT_ARG0);
}
