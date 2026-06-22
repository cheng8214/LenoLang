#include "../include/lenolang.h"
#include "../include/native.h"
#include "../include/platform_thread.h"
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
    return def;
}

void struct_def_set_field(ObjStructDef* def, int index, const char* name, TypeKind type,
                          const char* struct_type_name, Value default_value, int has_default, TypeKind element_type) {
    if (index < 0 || index >= def->field_count) return;

    def->fields[index].name = strdup(name);
    def->fields[index].type = type;
    def->fields[index].struct_type_name = struct_type_name ? strdup(struct_type_name) : NULL;
    def->fields[index].default_value = default_value;
    def->fields[index].has_default = has_default;
    def->fields[index].element_type = element_type;
}

// 注册结构体定义
void struct_def_register(ObjStructDef* def) {
    if (struct_def_count >= MAX_STRUCT_DEFS) {
        error_add(ERR_RUNTIME, 0, "结构体定义数量超过上限");
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

    // 使用默认值初始化字段
    for (int i = 0; i < def->field_count; i++) {
        if (def->fields[i].has_default) {
            // 深拷贝引用类型的默认值（Array、Dict），避免所有实例共享同一对象
            Value dv = def->fields[i].default_value;
            if (val_is_obj(dv)) {
                Object* dobj = val_as_obj(dv);
                if (dobj->type == OBJ_ARRAY || dobj->type == OBJ_DICT || dobj->type == OBJ_STRUCT) {
                    obj->field_values[i] = value_copy(dv);
                    continue;
                }
            }
            obj->field_values[i] = dv;
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
// 结构体方法表（运行时）- 类似 object_dict.c
// ============================================================================

#define STRUCT_METHOD_TABLE_INITIAL_CAPACITY 16
#define STRUCT_METHOD_TABLE_MAX_LOAD 0.75

/* 方法哈希表条目 */
typedef struct StructMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct StructMethodHashEntry* next;
} StructMethodHashEntry;

typedef struct {
    StructMethodHashEntry** entries;
    int capacity;
    int count;
} StructMethodTable;

static THREAD_LOCAL StructMethodTable structMethodTable = {NULL, 0, 0};

// 计算字符串哈希值（FNV-1a算法）
static uint32_t struct_hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}

// 初始化结构体方法表
static void struct_method_table_init(void) {
    structMethodTable.capacity = STRUCT_METHOD_TABLE_INITIAL_CAPACITY;
    structMethodTable.count = 0;
    structMethodTable.entries = (StructMethodHashEntry**)calloc(structMethodTable.capacity, sizeof(StructMethodHashEntry*));
}

// 释放结构体方法表
static void struct_method_table_free(void) {
    if (!structMethodTable.entries) return;
    
    for (int i = 0; i < structMethodTable.capacity; i++) {
        StructMethodHashEntry* entry = structMethodTable.entries[i];
        while (entry) {
            StructMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(structMethodTable.entries);
    structMethodTable.entries = NULL;
    structMethodTable.capacity = 0;
    structMethodTable.count = 0;
}

// 扩容结构体方法表
static void struct_method_table_resize(void) {
    int old_capacity = structMethodTable.capacity;
    StructMethodHashEntry** old_entries = structMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    StructMethodHashEntry** new_entries = (StructMethodHashEntry**)calloc(new_capacity, sizeof(StructMethodHashEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        StructMethodHashEntry* entry = old_entries[i];
        while (entry) {
            StructMethodHashEntry* next = entry->next;
            uint32_t hash = struct_hash_string(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    structMethodTable.entries = new_entries;
    structMethodTable.capacity = new_capacity;
}

// 注册结构体方法（带参数类型）
void struct_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!structMethodTable.entries) {
        struct_method_table_init();
    }
    
    if (structMethodTable.count >= structMethodTable.capacity * STRUCT_METHOD_TABLE_MAX_LOAD) {
        struct_method_table_resize();
    }
    
    uint32_t hash = struct_hash_string(name);
    int index = hash & (structMethodTable.capacity - 1);
    
    StructMethodHashEntry* entry = structMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            }
            return;
        }
        entry = entry->next;
    }
    
    StructMethodHashEntry* new_entry = (StructMethodHashEntry*)malloc(sizeof(StructMethodHashEntry));
    if (!new_entry) {
        native_throw_error("结构体方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    }
    
    new_entry->next = structMethodTable.entries[index];
    structMethodTable.entries[index] = new_entry;
    structMethodTable.count++;
    
    // 同时注册到编译期元信息表，避免重复维护
    native_register_instance_method_meta_with_params("struct", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 查找结构体方法的元信息（用于编译期类型检查）
StructMethodEntry struct_find_method_meta(const char* name) {
    StructMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};
    if (!structMethodTable.entries || structMethodTable.count == 0) return result;
    
    uint32_t hash = struct_hash_string(name);
    int index = hash & (structMethodTable.capacity - 1);
    
    StructMethodHashEntry* entry = structMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            result.name = entry->name;
            result.method = entry->method;
            result.arity = entry->arity;
            result.return_type = entry->return_type;
            result.return_element_type = entry->return_element_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                result.param_types[i] = entry->param_types[i];
            }
            return result;
        }
        entry = entry->next;
    }
    return result;
}

// 查找结构体方法
ObjNative* struct_find_method(const char* name) {
    if (!structMethodTable.entries || structMethodTable.count == 0) return NULL;
    
    uint32_t hash = struct_hash_string(name);
    int index = hash & (structMethodTable.capacity - 1);
    
    StructMethodHashEntry* entry = structMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 标记 struct 方法表中的所有方法（供 GC 使用）
void struct_mark_methods(void) {
    if (!structMethodTable.entries) return;
    for (int i = 0; i < structMethodTable.capacity; i++) {
        StructMethodHashEntry* entry = structMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}

void struct_init_methods(void) {
    struct_method_table_free();
    struct_method_table_init();
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
        error_add(ERR_RUNTIME, 0, "enum 定义数量超过上限");
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
