#include "../include/lenolang.h"
#include "../include/native.h"
#include "../include/platform_thread.h"
#include "../include/method_table.h"
#include <string.h>

// ============================================================================
// 结构体定义表（线程局部）
// ============================================================================

#define MAX_STRUCT_DEFS 256

static THREAD_LOCAL ObjStructDef* struct_def_table[MAX_STRUCT_DEFS];
static THREAD_LOCAL int struct_def_count = 0;

// ============================================================================
// 结构体定义操作
// ============================================================================

ObjStructDef* struct_def_new(const char* name, int field_count, int method_count) {
    ObjStructDef* def = (ObjStructDef*)gc_alloc(sizeof(ObjStructDef), OBJ_STRUCT_DEF);
    def->name = strdup(name);
    def->field_count = field_count;
    def->fields = (StructFieldInfo*)calloc(field_count, sizeof(StructFieldInfo));
    def->method_count = method_count;
    def->methods = (StructMethodInfo*)calloc(method_count, sizeof(StructMethodInfo));
    def->impl_names = NULL;
    def->impl_count = 0;
    def->type_param_count = 0;
    def->type_param_names = NULL;
    def->type_param_constraints = NULL;
    def->has_ctor = 0;
    def->ctor_index = -1;
    def->has_dtor = 0;
    def->dtor_index = -1;
    def->const_names = NULL;
    def->const_values = NULL;
    def->const_count = 0;

    // 追踪 fields 和 methods 数组的内存
    gc_track_memory((Object*)def, 0,
        (size_t)field_count * sizeof(StructFieldInfo) +
        (size_t)method_count * sizeof(StructMethodInfo));

    return def;
}

void struct_def_set_field(ObjStructDef* def, int index, const char* name, TypeKind type,
                          const char* struct_type_name, Value default_value, int has_default, TypeKind element_type, int nullable) {
    if (index < 0 || index >= def->field_count) return;

    def->fields[index].name = strdup(name);
    def->fields[index].type = type;
    def->fields[index].struct_type_name = struct_type_name ? strdup(struct_type_name) : NULL;
    def->fields[index].default_value = default_value;
    def->fields[index].has_default = has_default;
    def->fields[index].element_type = element_type;
    def->fields[index].nullable = nullable;
}

// 注册结构体定义
void struct_def_register(ObjStructDef* def) {
    if (struct_def_count >= MAX_STRUCT_DEFS) {
        error_add_at(ERR_RUNTIME, 0, 0, "结构体定义数量超过上限");
        return;
    }

    // 检查是否已存在同名结构体
    for (int i = 0; i < struct_def_count; i++) {
        if (struct_def_table[i]->name && strcmp(struct_def_table[i]->name, def->name) == 0) {
            // 覆盖旧定义：将旧定义的资源指针置 NULL，防止 gc_free_all 时 double-free
            // 旧定义对象仍由 GC 管理，gc_free_all 会调用 free_object_resources
            ObjStructDef* old_def = struct_def_table[i];
            old_def->name = NULL;
            old_def->fields = NULL;
            old_def->field_count = 0;
            old_def->methods = NULL;
            old_def->method_count = 0;
            old_def->impl_names = NULL;
            old_def->impl_count = 0;
            old_def->const_names = NULL;
            old_def->const_values = NULL;
            old_def->const_count = 0;

            struct_def_table[i] = def;
            return;
        }
    }

    struct_def_table[struct_def_count++] = def;
}

// 更新所有结构体方法函数的 module 指针
void struct_def_update_method_modules(ObjModule* old_module, ObjModule* new_module) {
    if (!old_module || !new_module) return;
    for (int i = 0; i < struct_def_count; i++) {
        ObjStructDef* def = struct_def_table[i];
        if (def->methods) {
            for (int j = 0; j < def->method_count; j++) {
                if (def->methods[j].func && def->methods[j].func->module == old_module) {
                    def->methods[j].func->module = new_module;
                }
                if (def->methods[j].closure && def->methods[j].closure->function &&
                    def->methods[j].closure->function->module == old_module) {
                    def->methods[j].closure->function->module = new_module;
                }
            }
        }
    }
}

// 查找结构体定义
ObjStructDef* struct_def_find(const char* name) {
    for (int i = 0; i < struct_def_count; i++) {
        if (strcmp(struct_def_table[i]->name, name) == 0) {
            return struct_def_table[i];
        }
    }
    return NULL;
}

// 返回当前线程结构体定义表的数量（供主线程抓取快照传给子线程）
int struct_def_get_count(void) {
    return struct_def_count;
}

// 返回当前线程结构体定义表中第 i 个定义（供主线程抓取快照）
ObjStructDef* struct_def_get(int i) {
    if (i < 0 || i >= struct_def_count) return NULL;
    return struct_def_table[i];
}

// 将主线程定义的结构体导入当前（子）线程的定义表。
// struct 定义是只读类型元数据，与 cstruct 定义一样可跨线程共享。
// 注意：def 可能分配在主线程 GC 堆上，子线程只读不写；
// 主线程通过模块对象持续持有 def，保证其存活，因此子线程引用安全。
void struct_def_import_from_thread(ObjStructDef** defs, int count) {
    for (int i = 0; i < count; i++) {
        if (defs[i]) {
            struct_def_register(defs[i]);
        }
    }
}

// ============================================================================
// 结构体实例操作
// ============================================================================

ObjStruct* struct_instance_new(ObjStructDef* def);

// 最大嵌套实例化深度，防止循环引用导致无限递归
#define STRUCT_INSTANCE_MAX_DEPTH 16

ObjStruct* struct_instance_new_depth(ObjStructDef* def, int depth) {
    if (depth > STRUCT_INSTANCE_MAX_DEPTH) {
        return NULL;
    }

    ObjStruct* obj = (ObjStruct*)gc_alloc(sizeof(ObjStruct), OBJ_STRUCT);
    obj->def = def;
    obj->field_values = (Value*)calloc(def->field_count, sizeof(Value));
    obj->declared_face = NULL;
    obj->generic_type_args = NULL;
    obj->generic_type_arg_count = 0;

    // 追踪 field_values 数组的内存（calloc 分配的额外内存 GC 默认不可见）
    gc_track_memory((Object*)obj, 0, (size_t)def->field_count * sizeof(Value));

    // 使用默认值初始化字段
    for (int i = 0; i < def->field_count; i++) {
        if (def->fields[i].has_default) {
            // 深拷贝引用类型的默认值（Array、Dict、Struct），避免所有实例共享同一对象
            Value dv = def->fields[i].default_value;
            if (val_is_obj(dv)) {
                Object* dobj = val_as_obj(dv);
                if (dobj->type == OBJ_ARRAY || dobj->type == OBJ_DICT || dobj->type == OBJ_STRUCT) {
                    obj->field_values[i] = value_copy(dv);
                    continue;
                }
            }
            obj->field_values[i] = dv;
        } else if (def->fields[i].nullable) {
            // nullable 字段默认为 null（不递归分配/不自动创建空容器）
            obj->field_values[i] = val_null();
        } else if (def->fields[i].type == TYPE_STRUCT && def->fields[i].struct_type_name) {
            // 嵌套 struct 类型：递归创建实例
            ObjStructDef* nested_def = struct_def_find(def->fields[i].struct_type_name);
            if (nested_def) {
                ObjStruct* nested_obj = struct_instance_new_depth(nested_def, depth + 1);
                if (nested_obj) {
                    obj->field_values[i] = val_obj((Object*)nested_obj);
                } else {
                    obj->field_values[i] = val_null();
                }
            } else {
                obj->field_values[i] = val_null();
            }
        } else if (def->fields[i].type == TYPE_ARRAY) {
            // Array 字段：自动初始化为空数组
            ObjArray* arr = arr_new(0);
            obj->field_values[i] = arr ? val_obj((Object*)arr) : val_null();
        } else if (def->fields[i].type == TYPE_DICT) {
            // Dict 字段：自动初始化为空字典
            ObjDict* dict = dict_new(0);
            obj->field_values[i] = dict ? val_obj((Object*)dict) : val_null();
        } else {
            obj->field_values[i] = val_null();
        }
    }

    return obj;
}

ObjStruct* struct_instance_new(ObjStructDef* def) {
    return struct_instance_new_depth(def, 0);
}

int struct_get_field_index(ObjStructDef* def, const char* name) {
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

Value struct_get_field(ObjStruct* obj, int index) {
    if (index < 0 || index >= obj->def->field_count) {
        return val_null();
    }
    return obj->field_values[index];
}

void struct_set_field(ObjStruct* obj, int index, Value value) {
    if (index < 0 || index >= obj->def->field_count) {
        return;
    }
    obj->field_values[index] = value;
    gc_write_barrier((Object*)obj, value);
}

// ============================================================================
// 结构体方法表（运行时 - 使用通用 MethodTable）
// ============================================================================

#define STRUCT_METHOD_TABLE_INITIAL_CAPACITY 16

static THREAD_LOCAL MethodTable structMethodTable = {NULL, 0, 0};

// 注册结构体方法（带参数类型）
void struct_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&structMethodTable, "struct", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找结构体方法的元信息（用于编译期类型检查）
StructMethodEntry struct_find_method_meta(const char* name) {
    return method_table_find_meta(&structMethodTable, name);
}

// 查找结构体方法
ObjNative* struct_find_method(const char* name) {
    return method_table_find(&structMethodTable, name);
}

// 标记 struct 方法表中的所有方法（供 GC 使用）
void struct_mark_methods(void) {
    method_table_mark(&structMethodTable);
}

void struct_init_methods(void) {
    method_table_init_methods(&structMethodTable, STRUCT_METHOD_TABLE_INITIAL_CAPACITY);
}

// 标记所有结构体定义（供 GC 使用）
void struct_def_mark_all(void) {
    extern void gc_mark_object(Object* obj);
    for (int i = 0; i < struct_def_count; i++) {
        gc_mark_object((Object*)struct_def_table[i]);
    }
}

// ============================================================================
// enum 定义表（运行时全局）
// ============================================================================

#define MAX_ENUM_DEFS 256

static THREAD_LOCAL ObjEnumDef* enum_def_table[MAX_ENUM_DEFS];
static THREAD_LOCAL int enum_def_count = 0;

// ============================================================================
// enum 定义操作
// ============================================================================

ObjEnumDef* enum_def_new(const char* name, int member_count) {
    ObjEnumDef* def = (ObjEnumDef*)gc_alloc(sizeof(ObjEnumDef), OBJ_ENUM_DEF);
    def->name = strdup(name);
    def->member_count = member_count;
    def->members = (EnumMemberInfo*)calloc(member_count, sizeof(EnumMemberInfo));

    // 追踪 members 数组的内存
    gc_track_memory((Object*)def, 0, (size_t)member_count * sizeof(EnumMemberInfo));

    return def;
}

void enum_def_set_member(ObjEnumDef* def, int index, const char* name, int64_t value) {
    if (index < 0 || index >= def->member_count) return;

    def->members[index].name = strdup(name);
    def->members[index].value = value;
}

int64_t enum_def_get_member_value(ObjEnumDef* def, const char* name) {
    for (int i = 0; i < def->member_count; i++) {
        if (strcmp(def->members[i].name, name) == 0) {
            return def->members[i].value;
        }
    }
    return -1; // 未找到
}

// 注册 enum 定义
void enum_def_register(ObjEnumDef* def) {
    if (enum_def_count >= MAX_ENUM_DEFS) {
        error_add_at(ERR_RUNTIME, 0, 0, "enum 定义数量超过上限");
        return;
    }

    // 检查是否已存在同名 enum
    for (int i = 0; i < enum_def_count; i++) {
        if (strcmp(enum_def_table[i]->name, def->name) == 0) {
            // 覆盖旧定义：将旧定义的资源指针置 NULL，防止 gc_free_all 时 double-free
            ObjEnumDef* old_def = enum_def_table[i];
            old_def->name = NULL;
            old_def->members = NULL;
            old_def->member_count = 0;

            enum_def_table[i] = def;
            return;
        }
    }

    enum_def_table[enum_def_count++] = def;
}

// 查找 enum 定义
ObjEnumDef* enum_def_find(const char* name) {
    for (int i = 0; i < enum_def_count; i++) {
        if (strcmp(enum_def_table[i]->name, name) == 0) {
            return enum_def_table[i];
        }
    }
    return NULL;
}
