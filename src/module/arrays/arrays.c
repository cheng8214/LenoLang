#include "include/native.h"
#include <string.h>
#include <stdio.h>

// ==================== 辅助函数 ====================

static int values_equal(Value a, Value b) {
    if (val_get_type(a) != val_get_type(b)) return 0;
    switch (val_get_type(a)) {
        case VAL_NULL: return 1;
        case VAL_BOOL: return val_as_bool(a) == val_as_bool(b);
        case VAL_INT:
        case VAL_FLOAT: return val_as_num(a) == val_as_num(b);
        case VAL_OBJ: {
            if (val_as_obj(a)->type != val_as_obj(b)->type) return 0;
            if (val_as_obj(a)->type == OBJ_STRING) {
                ObjString* strA = (ObjString*)val_as_obj(a);
                ObjString* strB = (ObjString*)val_as_obj(b);
                return strA->len == strB->len &&
                       memcmp(strA->chars, strB->chars, strA->len) == 0;
            }
            if (val_as_obj(a)->type == OBJ_BIGINT) {
                return bigint_compare((ObjBigInt*)val_as_obj(a), (ObjBigInt*)val_as_obj(b)) == 0;
            }
            return val_as_obj(a) == val_as_obj(b);
        }
        default: return 0;
    }
}

// ==================== 核心方法实现 ====================

static Value arr_len(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    return val_int(arr->count);
}

static Value arr_add(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) {
            return val_null();
        }
    }
    arr->elements[arr->count++] = args[1];
    gc_write_barrier((Object*)arr, args[1]);
    return val_int(arr->count);
}

// 数组缩容公共函数
// 当使用率低于 25% 时，缩容到 50% 容量
// 成功返回 1，失败返回 0（但不影响操作）
static int array_shrink(ObjArray* arr) {
    // 缩容条件：使用率 < 25% 且容量 > 16
    if (arr->count < arr->capacity / 4 && arr->capacity > 16) {
        int new_capacity = arr->capacity / 2;
        if (new_capacity < 16) new_capacity = 16;
        // 确保至少能容纳当前元素
        if (new_capacity < arr->count) new_capacity = arr->count;
        
        size_t old_size = (size_t)arr->capacity * sizeof(Value);
        size_t new_size = (size_t)new_capacity * sizeof(Value);
        
        Value* new_elements = (Value*)realloc(arr->elements, new_size);
        if (!new_elements) {
            // 缩容失败不是致命错误，继续用原内存
            return 0;
        }
        
        gc_track_memory((Object*)arr, old_size, new_size);
        arr->elements = new_elements;
        arr->capacity = new_capacity;
        return 1;
    }
    return 0;
}

static Value arr_pop(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    if (arr->count == 0) return val_null();
    Value result = arr->elements[--arr->count];
    // 尝试缩容
    array_shrink(arr);
    return result;
}

static Value arr_insert(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    int index = val_is_bigint(args[1]) ?
                (int)bigint_to_int64(val_as_bigint(args[1])) :
                (int)val_as_num(args[1]);
    if (index < 0) index = arr->count + index + 1;
    if (index < 0) index = 0;
    if (index > arr->count) index = arr->count;
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) {
            return val_null();
        }
    }
    for (int i = arr->count; i > index; i--) {
        arr->elements[i] = arr->elements[i - 1];
    }
    arr->elements[index] = args[2];
    gc_write_barrier((Object*)arr, args[2]);
    arr->count++;
    return val_int(arr->count);
}

static Value arr_remove(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    int index = val_is_bigint(args[1]) ?
                (int)bigint_to_int64(val_as_bigint(args[1])) :
                (int)val_as_num(args[1]);
    if (index < 0) index = arr->count + index;
    if (index < 0 || index >= arr->count) {
        native_throw_error("remove() 索引越界");
        return val_null();
    }
    Value removed = arr->elements[index];
    // 使用 memmove 优化内存移动（处理重叠区域）
    if (index < arr->count - 1) {
        memmove(&arr->elements[index], &arr->elements[index + 1],
                (arr->count - index - 1) * sizeof(Value));
    }
    arr->count--;
    // 尝试缩容
    array_shrink(arr);
    return removed;
}

static Value arr_has(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    for (int i = 0; i < arr->count; i++) {
        if (values_equal(arr->elements[i], args[1])) {
            return val_bool(1);
        }
    }
    return val_bool(0);
}

// 使用公共的 value_copy 函数（定义在 native.c 中）
// 该函数递归处理数组、字典、结构体等引用类型的深拷贝

static Value arr_copy(int argc, Value* args) {
    (void)argc;
    ObjArray* source = (ObjArray*)val_as_obj(args[0]);
    ObjArray* copy = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!copy) return val_null();
    copy->count = source->count;
    copy->capacity = source->count;
    copy->elements = NULL;
    if (source->count > 0) {
        copy->elements = (Value*)malloc(source->count * sizeof(Value));
        if (!copy->elements) {
            native_throw_error("数组拷贝内存分配失败");
            return val_null();
        }
        // 深拷贝每个元素
        for (int i = 0; i < source->count; i++) {
            copy->elements[i] = value_copy(source->elements[i]);
        }
        // 跟踪非 GC 内存
        gc_track_memory((Object*)copy, 0, source->count * sizeof(Value));
    }
    return val_obj((Object*)copy);
}

static Value arr_clear(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);

    // 计算旧内存大小用于跟踪
    size_t old_size = (size_t)arr->capacity * sizeof(Value);
    
    // 释放元素数组内存
    if (arr->elements) {
        free(arr->elements);
        arr->elements = NULL;
    }
    
    // 重置数组状态
    arr->count = 0;
    arr->capacity = 0;
    
    // 跟踪内存变化
    gc_track_memory((Object*)arr, old_size, 0);
    
    // 返回数组自身，支持链式调用
    return args[0];
}

// ==================== 新增方法 ====================

static Value arr_index_of(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    for (int i = 0; i < arr->count; i++) {
        if (values_equal(arr->elements[i], args[1])) {
            return val_int(i);
        }
    }
    return val_int(-1);
}

static Value arr_last_index_of(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    for (int i = arr->count - 1; i >= 0; i--) {
        if (values_equal(arr->elements[i], args[1])) {
            return val_int(i);
        }
    }
    return val_int(-1);
}

static Value arr_reverse(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    int left = 0;
    int right = arr->count - 1;
    while (left < right) {
        Value temp = arr->elements[left];
        arr->elements[left] = arr->elements[right];
        arr->elements[right] = temp;
        left++;
        right--;
    }
    return args[0];
}

// 排序比较函数
static int value_compare_for_sort(const void* a, const void* b) {
    Value va = *(Value*)a;
    Value vb = *(Value*)b;
    
    // 处理数字类型 (VAL_INT 和 VAL_FLOAT 都使用 as.num)
    if ((val_is_int(va) || val_is_float(va)) &&
        (val_is_int(vb) || val_is_float(vb))) {
        double da = val_as_num(va);
        double db = val_as_num(vb);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }
    
    // 处理字符串类型
    if (val_is_obj(va) && val_is_obj(vb) &&
        val_as_obj(va)->type == OBJ_STRING && val_as_obj(vb)->type == OBJ_STRING) {
        ObjString* sa = (ObjString*)val_as_obj(va);
        ObjString* sb = (ObjString*)val_as_obj(vb);
        return strcmp(sa->chars, sb->chars);
    }
    
    // 不同类型，按类型排序
    if (val_get_type(va) != val_get_type(vb)) {
        return (int)val_get_type(va) - (int)val_get_type(vb);
    }
    
    return 0;
}

static Value arr_sort(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    if (arr->count > 1) {
        qsort(arr->elements, arr->count, sizeof(Value), value_compare_for_sort);
    }
    return args[0];
}

static Value arr_join(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);

    const char* sep = "";
    int sep_len = 0;
    if (argc > 1 && val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_STRING) {
        ObjString* sep_str = (ObjString*)val_as_obj(args[1]);
        sep = sep_str->chars;
        sep_len = sep_str->len;
    }
    
    // 计算总长度
    int total_len = 0;
    for (int i = 0; i < arr->count; i++) {
        Value v = arr->elements[i];
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            total_len += ((ObjString*)val_as_obj(v))->len;
        } else {
            total_len += 32;
        }
        if (i < arr->count - 1) {
            total_len += sep_len;
        }
    }
    
    // 分配内存并拼接
    char* result = (char*)malloc(total_len + 1);
    if (!result) return val_null();
    
    result[0] = '\0';
    int pos = 0;
    for (int i = 0; i < arr->count; i++) {
        Value v = arr->elements[i];
        if (val_is_obj(v) && val_as_obj(v)->type == OBJ_STRING) {
            ObjString* s = (ObjString*)val_as_obj(v);
            memcpy(result + pos, s->chars, s->len);
            pos += s->len;
        } else {
            // 转换为字符串
            char buf[64];
            int len = 0;
            if (val_is_int(v)) {
                len = snprintf(buf, sizeof(buf), "%lld", (long long)val_as_num(v));
            } else if (val_is_float(v)) {
                len = snprintf(buf, sizeof(buf), "%g", val_as_num(v));
            } else if (val_is_bool(v)) {
                len = snprintf(buf, sizeof(buf), "%s", val_as_bool(v) ? "true" : "false");
            } else if (val_is_null(v)) {
                len = snprintf(buf, sizeof(buf), "null");
            } else {
                len = snprintf(buf, sizeof(buf), "[object]");
            }
            memcpy(result + pos, buf, len);
            pos += len;
        }
        if (i < arr->count - 1 && sep_len > 0) {
            memcpy(result + pos, sep, sep_len);
            pos += sep_len;
        }
    }
    result[pos] = '\0';
    
    ObjString* result_str = str_copy(result, pos);
    free(result);
    return val_obj((Object*)result_str);
}

// 辅助函数：调用 LenoC 函数/闭包
static Value call_closure(Value callee, int arg_count, Value* args) {
    VM* vm_ptr = current_exec_vm ? current_exec_vm : &vm;
    int saved_sp = vm_ptr->sp;
    int saved_frame_cnt = vm_ptr->frame_cnt;

    // 压入参数（从第一个参数开始）
    for (int i = 0; i < arg_count; i++) {
        vm_stack_push(vm_ptr, args[i]);
    }
    // 压入被调用者
    vm_stack_push(vm_ptr, callee);

    int call_result = vm_call_value(callee, arg_count, 0);
    Value ret_val = vm_ptr->last_return_value;

    if (call_result != 1) {
        vm_ptr->has_exception = 0;
        vm_ptr->exception = val_null();
        vm_ptr->frame_cnt = saved_frame_cnt;
        ret_val = val_null();
    }
    vm_ptr->sp = saved_sp;
    return ret_val;
}

static Value arr_map(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    Value fn = args[1];

    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) return val_null();
    result->count = 0;
    result->capacity = arr->count > 0 ? arr->count : 8;
    result->elements = (Value*)malloc(result->capacity * sizeof(Value));
    if (!result->elements) return val_null();
    gc_track_memory((Object*)result, 0, result->capacity * sizeof(Value));

    for (int i = 0; i < arr->count; i++) {
        Value call_args[2];
        call_args[0] = arr->elements[i];
        call_args[1] = val_int(i);
        Value mapped = call_closure(fn, 2, call_args);
        if (result->count >= result->capacity) {
            int new_cap = result->capacity * 2;
            Value* new_elems = (Value*)realloc(result->elements, new_cap * sizeof(Value));
            if (!new_elems) return val_null();
            gc_track_memory((Object*)result, result->capacity * sizeof(Value), new_cap * sizeof(Value));
            result->elements = new_elems;
            result->capacity = new_cap;
        }
        result->elements[result->count++] = mapped;
        gc_write_barrier((Object*)result, mapped);
    }

    return val_obj((Object*)result);
}

static Value arr_filter(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    Value fn = args[1];

    ObjArray* result = (ObjArray*)gc_alloc(sizeof(ObjArray), OBJ_ARRAY);
    if (!result) return val_null();
    result->count = 0;
    result->capacity = arr->count > 0 ? arr->count : 8;
    result->elements = (Value*)malloc(result->capacity * sizeof(Value));
    if (!result->elements) return val_null();
    gc_track_memory((Object*)result, 0, result->capacity * sizeof(Value));

    for (int i = 0; i < arr->count; i++) {
        Value call_args[2];
        call_args[0] = arr->elements[i];
        call_args[1] = val_int(i);
        Value pred = call_closure(fn, 2, call_args);
        if (val_is_bool(pred) && val_as_bool(pred)) {
            if (result->count >= result->capacity) {
                int new_cap = result->capacity * 2;
                Value* new_elems = (Value*)realloc(result->elements, new_cap * sizeof(Value));
                if (!new_elems) return val_null();
                gc_track_memory((Object*)result, result->capacity * sizeof(Value), new_cap * sizeof(Value));
                result->elements = new_elems;
                result->capacity = new_cap;
            }
            result->elements[result->count++] = arr->elements[i];
            gc_write_barrier((Object*)result, arr->elements[i]);
        }
    }

    return val_obj((Object*)result);
}

static Value arr_reduce(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    Value fn = args[1];

    if (arr->count == 0) {
        native_throw_error("reduce() 不能在空数组上调用");
        return val_null();
    }

    Value acc = arr->elements[0];
    int start_idx = 1;

    // 如果有初始值
    if (argc > 2) {
        acc = args[2];
        start_idx = 0;
    }

    for (int i = start_idx; i < arr->count; i++) {
        Value call_args[3];
        call_args[0] = acc;
        call_args[1] = arr->elements[i];
        call_args[2] = val_int(i);
        acc = call_closure(fn, 3, call_args);
    }

    return acc;
}

// ==================== 初始化 ====================

void arrays_init_module(void) {
    TypeKind len_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "len", arr_len, 1, -1, -1, TYPE_INT, len_params);

    TypeKind add_params[] = {TYPE_ARRAY, TYPE_ANY};
    native_register_module_method("arrays", "add", arr_add, 2, -1, -1, TYPE_INT, add_params);

    TypeKind pop_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "pop", arr_pop, 1, -1, -1, TYPE_ANY, pop_params);

    TypeKind insert_params[] = {TYPE_ARRAY, TYPE_INT, TYPE_ANY};
    native_register_module_method("arrays", "insert", arr_insert, 3, -1, -1, TYPE_INT, insert_params);

    TypeKind remove_params[] = {TYPE_ARRAY, TYPE_INT};
    native_register_module_method("arrays", "remove", arr_remove, 2, -1, -1, TYPE_ANY, remove_params);

    TypeKind has_params[] = {TYPE_ARRAY, TYPE_ANY};
    native_register_module_method("arrays", "has", arr_has, 2, -1, -1, TYPE_BOOL, has_params);

    TypeKind copy_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "copy", arr_copy, 1, -1, -1, TYPE_ARRAY, copy_params);

    TypeKind clear_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "clear", arr_clear, 1, -1, -1, TYPE_ARRAY, clear_params);

    // 新增方法注册
    TypeKind index_of_params[] = {TYPE_ARRAY, TYPE_ANY};
    native_register_module_method("arrays", "index_of", arr_index_of, 2, -1, -1, TYPE_INT, index_of_params);

    TypeKind last_index_of_params[] = {TYPE_ARRAY, TYPE_ANY};
    native_register_module_method("arrays", "last_index_of", arr_last_index_of, 2, -1, -1, TYPE_INT, last_index_of_params);

    TypeKind reverse_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "reverse", arr_reverse, 1, -1, -1, TYPE_ARRAY, reverse_params);

    TypeKind sort_params[] = {TYPE_ARRAY};
    native_register_module_method("arrays", "sort", arr_sort, 1, -1, -1, TYPE_ARRAY, sort_params);

    TypeKind join_params[] = {TYPE_ARRAY, TYPE_STRING};
    native_register_module_method("arrays", "join", arr_join, 2, -1, -1, TYPE_STRING, join_params);

    // 函数式方法
    TypeKind map_params[] = {TYPE_ARRAY, TYPE_FUNCTION};
    native_register_module_method("arrays", "map", arr_map, 2, -1, -1, TYPE_ARRAY, map_params);

    TypeKind filter_params[] = {TYPE_ARRAY, TYPE_FUNCTION};
    native_register_module_method("arrays", "filter", arr_filter, 2, -1, -1, TYPE_ARRAY, filter_params);

    TypeKind reduce_params[] = {TYPE_ARRAY, TYPE_FUNCTION, TYPE_ANY};
    native_register_module_method("arrays", "reduce", arr_reduce, 3, -1, -1, TYPE_ANY, reduce_params);
}

void arrays_init_instance_methods(void) {
    array_init_methods();
    TypeKind len_params[] = {};
    array_register_method_with_params("len",    make_native(arr_len,    1, "len"),    0, -1, -1, TYPE_INT,  len_params);

    TypeKind add_params[] = {TYPE_ANY};
    array_register_method_with_params("add",    make_native(arr_add,    2, "add"),    1, -1, -1, TYPE_INT,  add_params);

    TypeKind pop_params[] = {};
    array_register_method_with_params("pop",    make_native(arr_pop,    1, "pop"),    0, -1, -1, TYPE_ANY,  pop_params);

    TypeKind insert_params[] = {TYPE_INT, TYPE_ANY};
    array_register_method_with_params("insert", make_native(arr_insert, 3, "insert"), 2, -1, -1, TYPE_INT,  insert_params);

    TypeKind remove_params[] = {TYPE_INT};
    array_register_method_with_params("remove", make_native(arr_remove, 2, "remove"), 1, -1, -1, TYPE_ANY,  remove_params);

    TypeKind has_params[] = {TYPE_ANY};
    array_register_method_with_params("has",    make_native(arr_has,    2, "has"),    1, -1, -1, TYPE_BOOL, has_params);

    TypeKind copy_params[] = {};
    array_register_method_with_params("copy",   make_native(arr_copy,   1, "copy"),   0, -1, -1, TYPE_ARRAY,  copy_params);

    TypeKind clear_params[] = {};
    array_register_method_with_params("clear",  make_native(arr_clear,  1, "clear"),  0, -1, -1, TYPE_ARRAY,  clear_params);

    // 新增实例方法注册
    TypeKind index_of_params[] = {TYPE_ANY};
    array_register_method_with_params("index_of",     make_native(arr_index_of,     2, "index_of"),     1, -1, -1, TYPE_INT,    index_of_params);

    TypeKind last_index_of_params[] = {TYPE_ANY};
    array_register_method_with_params("last_index_of", make_native(arr_last_index_of, 2, "last_index_of"), 1, -1, -1, TYPE_INT, last_index_of_params);

    TypeKind reverse_params[] = {};
    array_register_method_with_params("reverse",      make_native(arr_reverse,      1, "reverse"),      0, -1, -1, TYPE_ARRAY,  reverse_params);

    TypeKind sort_params[] = {};
    array_register_method_with_params("sort",         make_native(arr_sort,         1, "sort"),         0, -1, -1, TYPE_ARRAY,  sort_params);

    TypeKind join_params[] = {TYPE_STRING};
    array_register_method_with_params("join",         make_native(arr_join,         2, "join"),         1, -1, -1, TYPE_STRING, join_params);

    // 函数式实例方法
    TypeKind map_params[] = {TYPE_FUNCTION};
    array_register_method_with_params("map",          make_native(arr_map,          2, "map"),          1, -1, -1, TYPE_ARRAY,  map_params);

    TypeKind filter_params[] = {TYPE_FUNCTION};
    array_register_method_with_params("filter",       make_native(arr_filter,       2, "filter"),       1, -1, -1, TYPE_ARRAY,  filter_params);

    TypeKind reduce_params[] = {TYPE_FUNCTION, TYPE_ANY};
    array_register_method_with_params("reduce",       make_native(arr_reduce,       3, "reduce"),       2, -1, -1, TYPE_ANY,    reduce_params);
}
