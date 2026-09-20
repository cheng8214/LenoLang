#include "include/native.h"
#include "include/leno_value.h"
#include <string.h>

// 前向声明：结构体实例方法支持函数（定义在 object_struct.c）
extern void struct_init_methods(void);
extern void struct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// ==================== 深拷贝实现 ====================

// 递归深拷贝结构体实例
Value struct_copy_recursive(ObjStruct* source) {
    if (!source) return val_null();

    ObjStructDef* def = source->def;

    // 创建新的结构体实例
    ObjStruct* copy = struct_instance_new(def);
    if (!copy) return val_null();

    // 深拷贝每个字段值
    for (int i = 0; i < def->field_count; i++) {
        copy->field_values[i] = value_copy(source->field_values[i]);
    }

    return val_obj((Object*)copy);
}

// ==================== 核心方法实现 ====================

static Value struct_method_copy(int argc, Value* args) {
    (void)argc;
    ObjStruct* source = (ObjStruct*)val_as_obj(args[0]);

    // 使用深拷贝
    return struct_copy_recursive(source);
}

static Value struct_method_len(int argc, Value* args) {
    (void)argc;
    ObjStruct* source = (ObjStruct*)val_as_obj(args[0]);
    return val_int(source->def->field_count);
}

// ==================== 初始化 ====================

void structs_init_instance_methods(void) {
    struct_init_methods();

    // 注册 copy 方法
    TypeKind copy_params[] = {};
    struct_register_method_with_params("copy", make_native(struct_method_copy, 1, "copy"),
                                       0, -1, -1, TYPE_STRUCT, TYPE_UNKNOWN, copy_params);

    // 注册 len 方法（迭代器协议：返回字段数量）
    TypeKind len_params[] = {};
    struct_register_method_with_params("len", make_native(struct_method_len, 1, "len"),
                                       0, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);
}
