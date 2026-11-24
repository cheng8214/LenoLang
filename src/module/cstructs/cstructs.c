#include "include/native.h"
#include "include/leno_value.h"
#include <string.h>
#include <inttypes.h>

// 前向声明：cstruct 方法支持函数
extern void cstruct_init_methods(void);
extern void cstruct_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);

// ==================== 核心方法实现 ====================
// 注意：receiver 和参数的类型检查由编译期和运行时方法分发机制保证

// cstruct.alloc() - 分配内存
static Value cstruct_method_alloc(int argc, Value* args) {
    (void)argc;
    
    ObjCStructDef* def = (ObjCStructDef*)val_as_obj(args[0]);

    ObjCStruct* instance = cstruct_new(def);
    if (!instance) {
        native_throw_error("分配 cstruct 内存失败");
        return val_null();
    }
    
    return val_obj((Object*)instance);
}

// cstruct.free() - 释放内存
static Value cstruct_method_free(int argc, Value* args) {
    (void)argc;
    
    // receiver 必须是 cstruct 实例
    if (val_as_obj(args[0])->type != OBJ_CSTRUCT) {
        return val_bool(false);
    }

    ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
    
    // 释放内存（如果拥有内存）
    if (instance->owns_memory && instance->data) {
        free(instance->data);
        instance->data = NULL;
        instance->owns_memory = 0;
        return val_bool(true);
    }
    
    // 不拥有内存或已经释放
    return val_bool(false);
}

// cstruct.to_ptr() - 获取指针
static Value cstruct_method_to_ptr(int argc, Value* args) {
    (void)argc;
    
    // 创建 FFI 指针对象
    ObjFFIPointer* ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    ptr->owned = 0;  // cstruct 拥有内存，不是指针拥有
    ptr->freed = 0;
    ptr->element_type = TYPE_PTR;
    
    if (val_as_obj(args[0])->type == OBJ_CSTRUCT) {
        ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
        // ★ 2026-11-16：free() 之后 `instance->data` 为 NULL ⇒ 此前**静默返回一个
        //   悬垂指针**（`ptr->ptr = NULL` 时其实还好，但视图/free_all 之后 data 指针
        //   可能仍是旧值？不 —— 这里统一走存活判定，与字段读写同口径）。
        //   它正是崩溃提示里"Ptr 在 free() 之后继续用"的主要来源 ⇒ 明确报错。
        if (!cstruct_is_alive(instance)) {
            native_throw_error("cstruct 数据已释放（free/free_all 之后不能再 to_ptr()）");
            return val_null();
        }
        ptr->ptr = instance->data;
        ptr->size = instance->def->total_size;
    } else if (val_as_obj(args[0])->type == OBJ_CSTRUCT_ARRAY) {
        ObjCStructArray* array = (ObjCStructArray*)val_as_obj(args[0]);
        if (!array->data) {
            native_throw_error("cstruct 数组已释放（free_all 之后不能再 to_ptr()）");
            return val_null();
        }
        ptr->ptr = array->data;
        ptr->size = array->element_size * array->count;
    } else {
        // 不支持的类型
        ptr->ptr = NULL;
        ptr->size = 0;
    }
    
    return val_obj((Object*)ptr);
}

// cstruct.from_ptr(ptr) - 从指针创建（不拥有内存）
static Value cstruct_method_from_ptr(int argc, Value* args) {
    (void)argc;
    
    ObjCStructDef* def = (ObjCStructDef*)val_as_obj(args[0]);
    ObjFFIPointer* ptr = (ObjFFIPointer*)val_as_obj(args[1]);
    
    if (!ptr->ptr) {
        native_throw_error("from_ptr() 不能从 null 指针创建");
        return val_null();
    }
    
    // 创建 cstruct 实例（不拥有内存）
    ObjCStruct* instance = cstruct_from_ptr(def, ptr->ptr);
    if (!instance) {
        native_throw_error("从指针创建 cstruct 失败");
        return val_null();
    }
    
    return val_obj((Object*)instance);
}

// 辅助函数：将基本类型值转换为字符串
static void value_to_str(Value val, char* buf, size_t buf_size) {
    if (val_is_int(val)) {
        snprintf(buf, buf_size, "%lld", (long long)val_as_num(val));
    } else if (val_is_bigint(val)) {
        char* str = bigint_to_string(val_as_bigint(val));
        snprintf(buf, buf_size, "%s", str);
        free(str);
    } else if (val_is_float(val)) {
        snprintf(buf, buf_size, "%.6g", val_as_num(val));
    } else if (val_is_bool(val)) {
        snprintf(buf, buf_size, "%s", val_as_num(val) ? "true" : "false");
    } else if (val_is_obj(val) && val_as_obj(val)->type == OBJ_FFI_POINTER) {
        snprintf(buf, buf_size, "<ptr %p>", ((ObjFFIPointer*)val_as_obj(val))->ptr);
    } else if (val_is_obj(val) && val_as_obj(val)->type == OBJ_FFI_CALLBACK) {
        snprintf(buf, buf_size, "<callback %p>", ((ObjFFICallback*)val_as_obj(val))->trampoline);
    } else if (val_is_null(val)) {
        snprintf(buf, buf_size, "null");
    } else {
        snprintf(buf, buf_size, "?");
    }
}

// 辅助函数：将数组字段值转换为字符串
static void array_field_to_str(ObjCStruct* instance, CStructFieldInfo* field, char* buf, size_t buf_size, int* offset) {
    *offset += snprintf(buf + *offset, buf_size - *offset, "    // 当前值: [");
    
    uint8_t* base_addr = instance->data + field->offset;
    int elements_to_show = field->array_dim > 4 ? 4 : field->array_dim;  // 最多显示4个元素
    
    for (int j = 0; j < elements_to_show; j++) {
        uint8_t* element_addr = base_addr + (j * field->size);
        char val_str[64];
        
        switch (field->type) {
            case TYPE_I8:
                snprintf(val_str, sizeof(val_str), "%d", (int)(*(int8_t*)element_addr));
                break;
            case TYPE_U8:
                snprintf(val_str, sizeof(val_str), "%u", (unsigned)(*(uint8_t*)element_addr));
                break;
            case TYPE_I16:
                snprintf(val_str, sizeof(val_str), "%d", (int)(*(int16_t*)element_addr));
                break;
            case TYPE_U16:
                snprintf(val_str, sizeof(val_str), "%u", (unsigned)(*(uint16_t*)element_addr));
                break;
            case TYPE_I32:
                snprintf(val_str, sizeof(val_str), "%d", *(int32_t*)element_addr);
                break;
            case TYPE_U32:
                snprintf(val_str, sizeof(val_str), "%u", *(uint32_t*)element_addr);
                break;
            case TYPE_I64:
                snprintf(val_str, sizeof(val_str), "%lld", (long long)(*(int64_t*)element_addr));
                break;
            case TYPE_U64:
                snprintf(val_str, sizeof(val_str), "%llu", (unsigned long long)(*(uint64_t*)element_addr));
                break;
            case TYPE_F32:
                snprintf(val_str, sizeof(val_str), "%.6g", (double)(*(float*)element_addr));
                break;
            case TYPE_F64:
                snprintf(val_str, sizeof(val_str), "%.6g", *(double*)element_addr);
                break;
            case TYPE_BOOL:
                snprintf(val_str, sizeof(val_str), "%s", *(uint8_t*)element_addr ? "true" : "false");
                break;
            default:
                snprintf(val_str, sizeof(val_str), "?");
                break;
        }
        
        *offset += snprintf(buf + *offset, buf_size - *offset, "%s%s", 
                           j > 0 ? ", " : "", val_str);
    }
    
    if (field->array_dim > 4) {
        *offset += snprintf(buf + *offset, buf_size - *offset, ", ... (%d more)", field->array_dim - 4);
    }
    
    *offset += snprintf(buf + *offset, buf_size - *offset, "]\n");
}

// 辅助函数：构建 cstruct 实例的字符串表示（递归）
static void cstruct_instance_to_str(ObjCStruct* instance, char* buf, size_t buf_size, int* offset, int indent) {
    ObjCStructDef* def = instance->def;
    
    // 添加缩进
    for (int i = 0; i < indent; i++) {
        *offset += snprintf(buf + *offset, buf_size - *offset, "  ");
    }
    
    *offset += snprintf(buf + *offset, buf_size - *offset, "%s {\n", def->name);
    
    // 字段信息
    for (int i = 0; i < def->field_count; i++) {
        CStructFieldInfo* field = &def->fields[i];
        
        // 添加缩进
        for (int j = 0; j < indent + 1; j++) {
            *offset += snprintf(buf + *offset, buf_size - *offset, "  ");
        }
        
        const char* type_name = type_kind_to_string(field->type);
        
        if (field->array_dim > 0) {
            // 数组字段
            *offset += snprintf(buf + *offset, buf_size - *offset,
                               "%s %s[%d];\n", type_name, field->name, field->array_dim);
            
            // 显示数组值
            for (int j = 0; j < indent + 1; j++) {
                *offset += snprintf(buf + *offset, buf_size - *offset, "  ");
            }
            array_field_to_str(instance, field, buf, buf_size, offset);
        } else if (field->type == TYPE_CSTRUCT && field->struct_name) {
            // 嵌套 cstruct 字段 - 递归打印
            if (instance && instance->data) {
                // 查找嵌套 cstruct 的定义
                ObjCStructDef* nested_def = cstruct_def_find(field->struct_name);
                if (nested_def) {
                    // 创建临时 cstruct 实例指向嵌套结构体内存
                    ObjCStruct nested_obj;
                    nested_obj.def = nested_def;
                    nested_obj.data = instance->data + field->offset;
                    nested_obj.owns_memory = 0;
                    
                    // 先打印字段名
                    *offset += snprintf(buf + *offset, buf_size - *offset,
                                       "%s %s = ", field->struct_name, field->name);
                    
                    // 递归打印嵌套结构体（在同一行开始）
                    // 注意：这里我们手动构建格式，而不是递归调用 cstruct_instance_to_str
                    *offset += snprintf(buf + *offset, buf_size - *offset, "{ ");
                    
                    for (int j = 0; j < nested_def->field_count; j++) {
                        CStructFieldInfo* nested_field = &nested_def->fields[j];
                        
                        if (j > 0) {
                            *offset += snprintf(buf + *offset, buf_size - *offset, ", ");
                        }
                        
                        if (nested_field->array_dim > 0) {
                            *offset += snprintf(buf + *offset, buf_size - *offset,
                                               "%s: [...]", nested_field->name);
                        } else if (nested_field->type == TYPE_CSTRUCT) {
                            *offset += snprintf(buf + *offset, buf_size - *offset,
                                               "%s: {...}", nested_field->name);
                        } else {
                            // 基本类型 - 读取值
                            uint8_t* nested_field_addr = nested_obj.data + nested_field->offset;
                            char val_str[64];
                            
                            switch (nested_field->type) {
                                case TYPE_I8:
                                    snprintf(val_str, sizeof(val_str), "%d", (int)(*(int8_t*)nested_field_addr));
                                    break;
                                case TYPE_U8:
                                    snprintf(val_str, sizeof(val_str), "%u", (unsigned)(*(uint8_t*)nested_field_addr));
                                    break;
                                case TYPE_I16:
                                    snprintf(val_str, sizeof(val_str), "%d", (int)(*(int16_t*)nested_field_addr));
                                    break;
                                case TYPE_U16:
                                    snprintf(val_str, sizeof(val_str), "%u", (unsigned)(*(uint16_t*)nested_field_addr));
                                    break;
                                case TYPE_I32:
                                    snprintf(val_str, sizeof(val_str), "%d", *(int32_t*)nested_field_addr);
                                    break;
                                case TYPE_U32:
                                    snprintf(val_str, sizeof(val_str), "%u", *(uint32_t*)nested_field_addr);
                                    break;
                                case TYPE_I64:
                                    snprintf(val_str, sizeof(val_str), "%lld", (long long)(*(int64_t*)nested_field_addr));
                                    break;
                                case TYPE_U64:
                                    snprintf(val_str, sizeof(val_str), "%llu", (unsigned long long)(*(uint64_t*)nested_field_addr));
                                    break;
                                case TYPE_F32:
                                    snprintf(val_str, sizeof(val_str), "%.6g", (double)(*(float*)nested_field_addr));
                                    break;
                                case TYPE_F64:
                                    snprintf(val_str, sizeof(val_str), "%.6g", *(double*)nested_field_addr);
                                    break;
                                case TYPE_BOOL:
                                    snprintf(val_str, sizeof(val_str), "%s", *(uint8_t*)nested_field_addr ? "true" : "false");
                                    break;
                                default:
                                    snprintf(val_str, sizeof(val_str), "?");
                                    break;
                            }
                            
                            *offset += snprintf(buf + *offset, buf_size - *offset,
                                               "%s: %s", nested_field->name, val_str);
                        }
                    }
                    
                    *offset += snprintf(buf + *offset, buf_size - *offset, " }\n");
                }
            } else {
                *offset += snprintf(buf + *offset, buf_size - *offset,
                                   "%s %s;\n", field->struct_name, field->name);
            }
        } else {
            // 基本类型字段
            *offset += snprintf(buf + *offset, buf_size - *offset,
                               "%s %s;", type_name, field->name);
            
            if (instance->data) {
                Value field_val = cstruct_get_field_value(instance, i);
                char val_str[64];
                value_to_str(field_val, val_str, sizeof(val_str));
                *offset += snprintf(buf + *offset, buf_size - *offset,
                                   "  // %s\n", val_str);
            } else {
                *offset += snprintf(buf + *offset, buf_size - *offset, "\n");
            }
        }
    }
    
    // 添加缩进
    for (int i = 0; i < indent; i++) {
        *offset += snprintf(buf + *offset, buf_size - *offset, "  ");
    }
    *offset += snprintf(buf + *offset, buf_size - *offset, "}");
}

// cstruct.to_str() - 打印内存布局信息
static Value cstruct_method_to_str(int argc, Value* args) {
    (void)argc;
    
    // receiver 可以是 cstruct 定义或实例
    ObjCStructDef* def = NULL;
    ObjCStruct* instance = NULL;
    
    if (val_as_obj(args[0])->type == OBJ_CSTRUCT_DEF) {
        def = (ObjCStructDef*)val_as_obj(args[0]);
    } else {
        instance = (ObjCStruct*)val_as_obj(args[0]);
        def = instance->def;
    }

    char buffer[4096];
    int offset = 0;

    if (instance) {
        // ★ 2026-11-16：free() 之后此前会**静默输出空值**（`i32 x;` 没有值），
        //   与"字段读写会报错"的口径不一致 ⇒ 明确报错。
        if (!cstruct_is_alive(instance)) {
            native_throw_error("cstruct 数据已释放（free/free_all 之后不能再 to_str()）");
            return val_null();
        }
        // 实例：使用递归格式
        cstruct_instance_to_str(instance, buffer, sizeof(buffer), &offset, 0);
    } else {
        // 定义：使用简化格式
        if (def->is_packed && def->explicit_align > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "packed align(%d) cstruct %s {\n",
                               def->explicit_align, def->name);
        } else if (def->is_packed) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "packed cstruct %s {\n", def->name);
        } else if (def->explicit_align > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "align(%d) cstruct %s {\n",
                               def->explicit_align, def->name);
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "cstruct %s {\n", def->name);
        }
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  // 总大小: %d 字节, 对齐要求: %d 字节\n",
                           def->total_size, def->alignment);
        
        // 字段信息
        for (int i = 0; i < def->field_count; i++) {
            CStructFieldInfo* field = &def->fields[i];
            const char* type_name = type_kind_to_string(field->type);
            
            if (field->array_dim > 0) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "  %s %s[%d];  // 偏移: %d, 大小: %d\n",
                                   type_name, field->name, field->array_dim,
                                   field->offset, field->size * field->array_dim);
            } else if (field->type == TYPE_CSTRUCT && field->struct_name) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "  %s %s;  // 偏移: %d, 嵌套结构体\n",
                                   field->struct_name, field->name, field->offset);
            } else {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "  %s %s;  // 偏移: %d, 大小: %d\n",
                                   type_name, field->name,
                                   field->offset, field->size);
            }
        }
        
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "}");
    }
    
    // 创建字符串对象
    ObjString* result = str_copy(buffer, offset);
    return val_obj((Object*)result);
}

// cstruct.size() - 返回 cstruct 大小（字节数）
static Value cstruct_method_size(int argc, Value* args) {
    (void)argc;
    
    // receiver 可以是 cstruct 定义或实例
    ObjCStructDef* def = NULL;
    
    if (val_as_obj(args[0])->type == OBJ_CSTRUCT_DEF) {
        def = (ObjCStructDef*)val_as_obj(args[0]);
    } else {
        ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
        def = instance->def;
    }

    return val_int(def->total_size);
}

// cstruct.malloc_array(count) - 创建结构体数组（静态方法）
static Value cstruct_method_malloc_array(int argc, Value* args) {
    (void)argc;

    if (val_as_obj(args[0])->type != OBJ_CSTRUCT_DEF) {
        ObjString* result = str_copy("malloc_array() 只能用于 cstruct 定义", 42);
        return val_obj((Object*)result);
    }
    
    ObjCStructDef* def = (ObjCStructDef*)val_as_obj(args[0]);

    int count = 1;
    if (argc > 1 && val_is_num(args[1])) {
        count = (int)val_as_num(args[1]);
    }
    
    // ★ 2026-11-11（cstruct 压测发现）：原来 `count <= 0` 一律改成 1 —— **静默改写用户意图**：
    //   `X.malloc_array(0)` 拿到的是**一个可写元素**（len() 返回 1、[0] 不越界），
    //   于是"上界算错成 0"的循环不会暴露，反而往合法元素里写脏数据。
    //   现在：0 ⇒ 真空数组（len()==0，任何下标都越界）；负数 ⇒ 直接报错。
    if (count < 0) {
        native_throw_error("malloc_array 的个数不能为负");
        return val_null();
    }
    
    // 创建 cstruct 数组
    ObjCStructArray* array = cstruct_array_new(def, count);
    if (!array) {
        // ★ 2026-11-11（压测）：原来**静默返回 null** —— `V3.malloc_array(2000000000)`
        //   （24GB）拿到的变量是 null，直到后面 `a[0].x = ...` 才报「对象不支持索引」，
        //   完全看不出真因是分配失败。现在直接报分配失败。
        native_throw_error("malloc_array 分配失败（内存不足，或元素个数过大）");
        return val_null();
    }
    
    return val_obj((Object*)array);
}

// cstruct.free_all() - 释放结构体数组（实例方法）
static Value cstruct_method_free_all(int argc, Value* args) {
    (void)argc;
    
    // receiver 必须是 cstruct 数组
    if (val_as_obj(args[0])->type != OBJ_CSTRUCT_ARRAY) {
        return val_bool(false);
    }

    ObjCStructArray* array = (ObjCStructArray*)val_as_obj(args[0]);
    
    // 释放数组内存
    cstruct_array_free(array);
    
    return val_bool(true);
}

// cstruct 数组的 len() 方法 - 返回数组长度
static Value cstruct_array_method_len(int argc, Value* args) {
    (void)argc;

    // ★ 2026-11-11（压测）：**数组字段视图**（`m[0].cols`）也要能 len() ——
    //   原来只认 OBJ_CSTRUCT_ARRAY，视图一律返回 0（实测 `m[0].cols.len()` 得 0，
    //   而它明明是 2 个元素）。视图的元素个数就是 array_dim。
    if (val_is_obj(args[0]) && val_as_obj(args[0])->type == OBJ_CSTRUCT_ARRAY_VIEW) {
        ObjCStructArrayView* view = (ObjCStructArrayView*)val_as_obj(args[0]);
        return val_int(view->array_dim);
    }

    // receiver 必须是 cstruct 数组
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_CSTRUCT_ARRAY) {
        return val_int(0);
    }

    ObjCStructArray* array = (ObjCStructArray*)val_as_obj(args[0]);
    return val_int(array->count);
}

// ==================== 数组按字段批量读（2026-11-24） ====================
// 数据导向真正卡手的地方：手里有连续内存，却只能 `arr[i].x` 一格一格读。
//   sum / min / max       —— 不分配，直接扫
//   column_i / column_f   —— 按字段取一列成 GC 数组
//
// ⚠ 三条边界（一开始就说死，避免"看起来能用"的半个实现）：
//   ① 只支持**非位域标量**（i8..u64 / f32 / f64）；位域（`u32 x : 3`）和数组字段
//      （`u8 data[4]`）**报错**——位域跨字节，批量读的语义最容易写错。
//   ② column_i / column_f **分开**，不做"自动选类型"：混着 int/float 的 `Array[float]`
//      会让下游每次取值都要 `_int`/`_float` 收窄。
//   ③ 空数组：sum 给 0.0（与 arrays.sum 同口径），min/max **报错**（没有定义）。

static int csa_is_scalar(TypeKind t) {
    switch (t) {
    case TYPE_I8:  case TYPE_U8:  case TYPE_I16: case TYPE_U16:
    case TYPE_I32: case TYPE_U32: case TYPE_I64: case TYPE_U64:
    case TYPE_F32: case TYPE_F64:
        return 1;
    default:
        return 0;
    }
}

// 从 p 处按 t 读一个标量（memcpy 避免未对齐访问）
static int csa_read_scalar(const uint8_t* p, TypeKind t, double* out) {
    switch (t) {
    case TYPE_I8:  { int8_t   v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_U8:  { uint8_t  v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_I16: { int16_t  v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_U16: { uint16_t v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_I32: { int32_t  v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_U32: { uint32_t v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_I64: { int64_t  v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_U64: { uint64_t v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_F32: { float    v; memcpy(&v, p, sizeof v); *out = (double)v; return 1; }
    case TYPE_F64: { double   v; memcpy(&v, p, sizeof v); *out = v;        return 1; }
    default:
        return 0;
    }
}

// 取字段下标并校验；失败 ⇒ 抛错并返回 0
static int csa_field_of(ObjCStructArray* a, Value fv, const char* api,
                        CStructFieldInfo** out) {
    if (!val_is_obj(fv) || val_as_obj(fv)->type != OBJ_STRING) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 字段名必须是字符串", api);
        native_throw_error(msg);
        return 0;
    }
    const char* fname = ((ObjString*)val_as_obj(fv))->chars;
    int idx = cstruct_get_field_index(a->def, fname);
    if (idx < 0 || idx >= a->def->field_count) {
        char msg[224];
        snprintf(msg, sizeof(msg), "%s: cstruct '%s' 没有字段 '%s'", api,
                 a->def && a->def->name ? a->def->name : "?", fname);
        native_throw_error(msg);
        return 0;
    }
    CStructFieldInfo* f = &a->def->fields[idx];
    if (f->bit_width > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s: 字段 '%s' 是位域（%d 位），批量读不支持 —— 位域跨字节，请逐元素读",
                 api, fname, f->bit_width);
        native_throw_error(msg);
        return 0;
    }
    if (f->array_dim > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s: 字段 '%s' 是数组（%d 个元素），批量读不支持 —— 请逐元素读",
                 api, fname, f->array_dim);
        native_throw_error(msg);
        return 0;
    }
    if (!csa_is_scalar(f->type)) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s: 字段 '%s' 不是数值标量（kind=%d）—— 批量读只支持 i8..u64 / f32 / f64",
                 api, fname, (int)f->type);
        native_throw_error(msg);
        return 0;
    }
    *out = f;
    return 1;
}

// sum / min / max 共用：mode 0=sum, 1=min, 2=max
static Value csa_scan(Value* args, int mode, const char* api) {
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_CSTRUCT_ARRAY) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "%s: 只能对 cstruct 数组调用（`Vec3.malloc_array(n)` 的返回值）", api);
        native_throw_error(msg);
        return val_null();
    }
    ObjCStructArray* a = (ObjCStructArray*)val_as_obj(args[0]);
    CStructFieldInfo* f = NULL;
    if (!csa_field_of(a, args[1], api, &f)) return val_null();

    if (a->count <= 0) {
        if (mode == 0) return val_float(0.0);
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: 空数组没有 %s", api, mode == 1 ? "min" : "max");
        native_throw_error(msg);
        return val_null();
    }

    double acc = 0.0;
    for (int i = 0; i < a->count; i++) {
        const uint8_t* p = a->data + (size_t)i * (size_t)a->element_size + (size_t)f->offset;
        double v = 0.0;
        if (!csa_read_scalar(p, f->type, &v)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s: 读字段 '%s' 失败（类型不支持）", api, f->name);
            native_throw_error(msg);
            return val_null();
        }
        if (i == 0) {
            acc = v;
        } else if (mode == 0) {
            acc += v;
        } else if (mode == 1) {
            if (v < acc) acc = v;
        } else {
            if (v > acc) acc = v;
        }
    }
    return val_float(acc);
}

// column_i / column_f 共用：按字段取一列
static Value csa_column(Value* args, int as_float, const char* api) {
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_CSTRUCT_ARRAY) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "%s: 只能对 cstruct 数组调用（`Vec3.malloc_array(n)` 的返回值）", api);
        native_throw_error(msg);
        return val_null();
    }
    ObjCStructArray* a = (ObjCStructArray*)val_as_obj(args[0]);
    CStructFieldInfo* f = NULL;
    if (!csa_field_of(a, args[1], api, &f)) return val_null();

    ObjArray* out = arr_new(a->count > 0 ? a->count : 1);
    if (!out) {
        native_throw_error("column: 分配结果数组失败");
        return val_null();
    }
    for (int i = 0; i < a->count; i++) {
        const uint8_t* p = a->data + (size_t)i * (size_t)a->element_size + (size_t)f->offset;
        double v = 0.0;
        if (!csa_read_scalar(p, f->type, &v)) {
            char msg[192];
            snprintf(msg, sizeof(msg), "%s: 读字段 '%s' 失败（类型不支持）", api, f->name);
            native_throw_error(msg);
            return val_null();
        }
        out->elements[out->count++] = as_float ? val_float(v) : val_int_safe((int64_t)v);
    }
    return val_obj((Object*)out);
}

static Value cstruct_array_method_sum(int argc, Value* args) {
    (void)argc;
    return csa_scan(args, 0, "sum");
}

static Value cstruct_array_method_min(int argc, Value* args) {
    (void)argc;
    return csa_scan(args, 1, "min");
}

static Value cstruct_array_method_max(int argc, Value* args) {
    (void)argc;
    return csa_scan(args, 2, "max");
}

static Value cstruct_array_method_column_f(int argc, Value* args) {
    (void)argc;
    return csa_column(args, 1, "column_f");
}

static Value cstruct_array_method_column_i(int argc, Value* args) {
    (void)argc;
    return csa_column(args, 0, "column_i");
}

// cstruct.alignment() - 返回 cstruct 对齐要求（字节数）
static Value cstruct_method_alignment(int argc, Value* args) {
    (void)argc;
    
    // receiver 可以是 cstruct 定义或实例
    ObjCStructDef* def = NULL;
    
    if (val_as_obj(args[0])->type == OBJ_CSTRUCT_DEF) {
        def = (ObjCStructDef*)val_as_obj(args[0]);
    } else {
        ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
        def = instance->def;
    }

    return val_int(def->alignment);
}

// cstruct.offset_of(field_name) - 返回字段的字节偏移量
static Value cstruct_method_offset_of(int argc, Value* args) {
    (void)argc;
    
    // receiver 可以是 cstruct 定义或实例
    ObjCStructDef* def = NULL;
    
    if (val_as_obj(args[0])->type == OBJ_CSTRUCT_DEF) {
        def = (ObjCStructDef*)val_as_obj(args[0]);
    } else if (val_as_obj(args[0])->type == OBJ_CSTRUCT) {
        ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
        def = instance->def;
    } else {
        native_throw_error("offset_of: receiver 必须是 cstruct 定义或实例");
        return val_null();
    }
    
    // 参数是字段名
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_STRING) {
        native_throw_error("offset_of: 参数必须是字段名字符串");
        return val_null();
    }
    const char* field_name = ((ObjString*)val_as_obj(args[1]))->chars;
    
    // 查找字段
    for (int i = 0; i < def->field_count; i++) {
        if (strcmp(def->fields[i].name, field_name) == 0) {
            return val_int(def->fields[i].offset);
        }
    }
    
    char msg[256];
    snprintf(msg, sizeof(msg), "offset_of: 字段 '%s' 不存在于结构体 '%s' 中", field_name, def->name ? def->name : "?");
    native_throw_error(msg);
    return val_null();
}

// cstruct.debug() - 显示详细的内存布局、偏移、对齐信息
static Value cstruct_method_debug(int argc, Value* args) {
    (void)argc;

    ObjCStructDef* def = NULL;
    ObjCStruct* instance = NULL;

    if (val_as_obj(args[0])->type == OBJ_CSTRUCT_DEF) {
        def = (ObjCStructDef*)val_as_obj(args[0]);
    } else {
        instance = (ObjCStruct*)val_as_obj(args[0]);
        def = instance->def;
        // ★ 2026-11-16：debug() 会打印每个字段当前值 ⇒ free() 之后读 data 直接
        //   0xC0000005 崩溃（实测）。与 to_str / hex / to_ptr 统一口径。
        if (!cstruct_is_alive(instance)) {
            native_throw_error("cstruct 数据已释放（free/free_all 之后不能再 debug()）");
            return val_null();
        }
    }
    
    // 构建详细的调试信息字符串
    char buffer[8192];
    int offset = 0;
    
    // 标题
    if (def->is_packed && def->explicit_align > 0) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "=== packed align(%d) cstruct %s 调试信息 ===\n\n",
                           def->explicit_align, def->name);
    } else if (def->is_packed) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "=== packed cstruct %s 调试信息 ===\n\n", def->name);
    } else if (def->explicit_align > 0) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "=== align(%d) cstruct %s 调试信息 ===\n\n",
                           def->explicit_align, def->name);
    } else {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "=== cstruct %s 调试信息 ===\n\n", def->name);
    }
    
    // 基本信息
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "[基本信息]\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  总大小: %d 字节 (0x%X)\n", def->total_size, def->total_size);
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  对齐要求: %d 字节\n", def->alignment);
    if (def->is_packed) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  packed: 是 (字段间无 padding)\n");
    }
    if (def->explicit_align > 0) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  显式对齐: %d 字节\n", def->explicit_align);
    }
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  字段数量: %d\n\n", def->field_count);
    
    // 字段布局表头
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "[字段布局]\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  %-6s %-12s %-20s %-10s %-10s %-10s\n",
                       "索引", "偏移", "类型", "名称", "大小", "数组维度");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  %-6s %-12s %-20s %-10s %-10s %-10s\n",
                       "------", "------------", "--------------------", "----------", "----------", "----------");
    
    // 字段信息
    for (int i = 0; i < def->field_count; i++) {
        CStructFieldInfo* field = &def->fields[i];
        const char* type_name = type_kind_to_string(field->type);
        
        if (field->array_dim > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  %-6d 0x%08X %-20s %-10s %-10d %-10d\n",
                               i, field->offset, type_name, field->name,
                               field->size * field->array_dim, field->array_dim);
        } else if (field->type == TYPE_CSTRUCT && field->struct_name) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  %-6d 0x%08X %-20s %-10s %-10d %-10s\n",
                               i, field->offset, field->struct_name, field->name,
                               field->size, "-");
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  %-6d 0x%08X %-20s %-10s %-10d %-10s\n",
                               i, field->offset, type_name, field->name,
                               field->size, "-");
        }
    }
    
    // 内存布局图
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n[内存布局图]\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "  地址偏移   字段\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "  ---------  -------------------------\n");
    
    for (int i = 0; i < def->field_count; i++) {
        CStructFieldInfo* field = &def->fields[i];
        int field_end = field->offset + (field->array_dim > 0 ? field->size * field->array_dim : field->size) - 1;
        const char* display_type = (field->type == TYPE_CSTRUCT && field->struct_name) ? field->struct_name : type_kind_to_string(field->type);
        
        if (field->array_dim > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  0x%04X-0x%04X  %s %s[%d]\n",
                               field->offset, field_end, display_type,
                               field->name, field->array_dim);
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  0x%04X-0x%04X  %s %s\n",
                               field->offset, field_end, display_type,
                               field->name);
        }
    }
    
    // 如果是实例，显示当前值
    if (instance) {
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n[当前值]\n");
        for (int i = 0; i < def->field_count; i++) {
            CStructFieldInfo* field = &def->fields[i];
            uint8_t* field_addr = instance->data + field->offset;
            
            if (field->array_dim > 0) {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                   "  %s = [数组, 维度=%d]\n", field->name, field->array_dim);
            } else {
                // 根据类型显示值
                switch (field->type) {
                    case TYPE_I8:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %d (i8)\n", field->name, *(int8_t*)field_addr);
                        break;
                    case TYPE_U8:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %u (u8)\n", field->name, *(uint8_t*)field_addr);
                        break;
                    case TYPE_I16:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %d (i16)\n", field->name, *(int16_t*)field_addr);
                        break;
                    case TYPE_U16:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %u (u16)\n", field->name, *(uint16_t*)field_addr);
                        break;
                    case TYPE_I32:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %d (i32)\n", field->name, *(int32_t*)field_addr);
                        break;
                    case TYPE_U32:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %u (u32)\n", field->name, *(uint32_t*)field_addr);
                        break;
                    case TYPE_I64:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %" PRId64 " (i64)\n", field->name, *(int64_t*)field_addr);
                        break;
                    case TYPE_U64:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %" PRIu64 " (u64)\n", field->name, *(uint64_t*)field_addr);
                        break;
                    case TYPE_F32:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %f (f32)\n", field->name, *(float*)field_addr);
                        break;
                    case TYPE_F64:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %lf (f64)\n", field->name, *(double*)field_addr);
                        break;
                    case TYPE_BOOL:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %s (bool)\n", field->name, *(uint8_t*)field_addr ? "true" : "false");
                        break;
                    case TYPE_PTR:
                    case TYPE_PTR_GENERIC:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %p (ptr)\n", field->name, *(void**)field_addr);
                        break;
                    default: {
                        const char* dt = (field->type == TYPE_CSTRUCT && field->struct_name) ? field->struct_name : type_kind_to_string(field->type);
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = [...] (%s)\n", field->name, dt);
                        break;
                    }
                }
            }
        }
    }
    
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n=== 结束 ===");
    
    // 创建字符串对象
    ObjString* result = str_copy(buffer, offset);
    return val_obj((Object*)result);
}

// cstruct.hex() - 以十六进制格式显示原始字节
static Value cstruct_method_hex(int argc, Value* args) {
    (void)argc;
    
    // receiver 必须是 cstruct 实例
    if (val_as_obj(args[0])->type != OBJ_CSTRUCT) {
        ObjString* result = str_copy("hex() 只能用于 cstruct 实例", 30);
        return val_obj((Object*)result);
    }

    ObjCStruct* instance = (ObjCStruct*)val_as_obj(args[0]);
    ObjCStructDef* def = instance->def;

    // ★ 2026-11-16：free() 之后 `instance->data` 是 NULL ⇒ 下面按 def->total_size 逐字节读
    //   会直接 0xC0000005 崩溃（实测）。与字段读写的口径一致：明确报错。
    if (!cstruct_is_alive(instance)) {
        native_throw_error("cstruct 数据已释放（free/free_all 之后不能再 hex()）");
        return val_null();
    }
    
    // 构建十六进制输出
    char buffer[8192];
    int offset = 0;
    
    // 标题
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== cstruct %s 原始字节 (共 %d 字节) ===\n\n",
                       def->name, def->total_size);
    
    // 十六进制输出（类似 hexdump -C 格式）
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  偏移      00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F  |  可打印字符\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  --------  -----------------------------------------------  |  ----------------\n");
    
    for (int row = 0; row < def->total_size; row += 16) {
        // 偏移
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, "  %08X  ", row);
        
        // 十六进制字节
        char ascii[17];
        for (int col = 0; col < 16; col++) {
            int byte_idx = row + col;
            if (byte_idx < def->total_size) {
                uint8_t byte = instance->data[byte_idx];
                offset += snprintf(buffer + offset, sizeof(buffer) - offset, "%02X ", byte);
                // 可打印字符
                if (byte >= 32 && byte < 127) {
                    ascii[col] = (char)byte;
                } else {
                    ascii[col] = '.';
                }
            } else {
                offset += snprintf(buffer + offset, sizeof(buffer) - offset, "   ");
                ascii[col] = ' ';
            }
        }
        ascii[16] = '\0';
        
        // 可打印字符
        offset += snprintf(buffer + offset, sizeof(buffer) - offset, " |  %s\n", ascii);
    }
    
    // 添加字段标记
    offset += snprintf(buffer + offset, sizeof(buffer) - offset, "\n[字段标记]\n");
    for (int i = 0; i < def->field_count; i++) {
        CStructFieldInfo* field = &def->fields[i];
        int field_size = field->array_dim > 0 ? field->size * field->array_dim : field->size;
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "  0x%04X-0x%04X  %s (%s)\n",
                           field->offset, field->offset + field_size - 1,
                           field->name, type_kind_to_string(field->type));
    }
    
    // 创建字符串对象
    ObjString* result = str_copy(buffer, offset);
    return val_obj((Object*)result);
}

// ==================== 初始化 ====================

void cstructs_init_methods(void) {
    // 先清空 cstruct 方法表
    cstruct_init_methods();
    
    // 注册 cstruct 定义的方法（静态方法）
    // malloc()
    cstruct_register_method("malloc", make_native(cstruct_method_alloc, 1, "malloc"), TYPE_CSTRUCT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // from_ptr(ptr)
    TypeKind from_ptr_params[] = {TYPE_PTR};
    cstruct_register_method("from_ptr", make_native(cstruct_method_from_ptr, 2, "from_ptr"), TYPE_CSTRUCT, TYPE_UNKNOWN, NATIVE_FIXED(from_ptr_params));

    // malloc_array(count) - 创建结构体数组
    TypeKind malloc_array_params[] = {TYPE_INT};
    cstruct_register_method("malloc_array", make_native(cstruct_method_malloc_array, 2, "malloc_array"), TYPE_CSTRUCT, TYPE_UNKNOWN, NATIVE_FIXED(malloc_array_params));

    // 注册 cstruct 实例的方法
    // free()
    cstruct_register_method("free", make_native(cstruct_method_free, 1, "free"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // to_ptr()
    cstruct_register_method("to_ptr", make_native(cstruct_method_to_ptr, 1, "to_ptr"), TYPE_PTR, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // to_str() - 打印内存布局
    cstruct_register_method("to_str", make_native(cstruct_method_to_str, 1, "to_str"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // size() - 返回 cstruct 大小
    cstruct_register_method("size", make_native(cstruct_method_size, 1, "size"), TYPE_I32, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // alignment() - 返回 cstruct 对齐要求
    cstruct_register_method("alignment", make_native(cstruct_method_alignment, 1, "alignment"), TYPE_I32, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // offset_of(field_name) - 返回字段偏移量
    TypeKind offset_of_params[] = {TYPE_STRING};
    cstruct_register_method("offset_of", make_native(cstruct_method_offset_of, 2, "offset_of"), TYPE_I32, TYPE_UNKNOWN, NATIVE_FIXED(offset_of_params));

    // debug() - 显示详细的内存布局、偏移、对齐信息
    cstruct_register_method("debug", make_native(cstruct_method_debug, 1, "debug"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // hex() - 以十六进制格式显示原始字节
    cstruct_register_method("hex", make_native(cstruct_method_hex, 1, "hex"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // free_all() - 释放结构体数组
    cstruct_register_method("free_all", make_native(cstruct_method_free_all, 1, "free_all"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 注册 cstruct 数组的方法（OBJ_CSTRUCT_ARRAY 实例方法）
    // len() - 返回数组长度
    cstruct_register_method("len", make_native(cstruct_array_method_len, 1, "len"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // ---- 按字段批量读（2026-11-24）----
    // 参数统一是 (field: string)；返回 Array[float]/Array[int] 用 (TYPE_ARRAY, 元素kind) 表达。
    TypeKind field_name_params[] = {TYPE_STRING};
    cstruct_register_method("sum", make_native(cstruct_array_method_sum, 2, "sum"),
                            TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(field_name_params));
    cstruct_register_method("min", make_native(cstruct_array_method_min, 2, "min"),
                            TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(field_name_params));
    cstruct_register_method("max", make_native(cstruct_array_method_max, 2, "max"),
                            TYPE_FLOAT, TYPE_UNKNOWN, NATIVE_FIXED(field_name_params));
    cstruct_register_method("column_f", make_native(cstruct_array_method_column_f, 2, "column_f"),
                            TYPE_ARRAY, TYPE_FLOAT, NATIVE_FIXED(field_name_params));
    cstruct_register_method("column_i", make_native(cstruct_array_method_column_i, 2, "column_i"),
                            TYPE_ARRAY, TYPE_INT, NATIVE_FIXED(field_name_params));
}
