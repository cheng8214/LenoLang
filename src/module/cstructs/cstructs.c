#include "include/native.h"
#include "include/leno_value.h"
#include <string.h>

// 前向声明：cstruct 方法支持函数
extern void cstruct_init_methods(void);
extern void cstruct_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

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
        ptr->ptr = instance->data;
        ptr->size = instance->def->total_size;
    } else if (val_as_obj(args[0])->type == OBJ_CSTRUCT_ARRAY) {
        ObjCStructArray* array = (ObjCStructArray*)val_as_obj(args[0]);
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
        // 实例：使用递归格式
        cstruct_instance_to_str(instance, buffer, sizeof(buffer), &offset, 0);
    } else {
        // 定义：使用简化格式
        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                           "cstruct %s {\n", def->name);
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
    
    if (count <= 0) {
        count = 1;
    }
    
    // 创建 cstruct 数组
    ObjCStructArray* array = cstruct_array_new(def, count);
    if (!array) {
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
    
    // receiver 必须是 cstruct 数组
    if (val_as_obj(args[0])->type != OBJ_CSTRUCT_ARRAY) {
        return val_int(0);
    }

    ObjCStructArray* array = (ObjCStructArray*)val_as_obj(args[0]);
    return val_int(array->count);
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
    }
    
    // 构建详细的调试信息字符串
    char buffer[8192];
    int offset = 0;
    
    // 标题
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "=== cstruct %s 调试信息 ===\n\n", def->name);
    
    // 基本信息
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "[基本信息]\n");
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  总大小: %d 字节 (0x%X)\n", def->total_size, def->total_size);
    offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                       "  对齐要求: %d 字节\n", def->alignment);
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
                               "  %-6d 0x%08X %-20s %-10s %-10s %-10s\n",
                               i, field->offset, field->struct_name, field->name,
                               "嵌套", "-");
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
        
        if (field->array_dim > 0) {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  0x%04X-0x%04X  %s %s[%d]\n",
                               field->offset, field_end, type_kind_to_string(field->type),
                               field->name, field->array_dim);
        } else {
            offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                               "  0x%04X-0x%04X  %s %s\n",
                               field->offset, field_end, type_kind_to_string(field->type),
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
                                           "  %s = %lld (i64)\n", field->name, *(int64_t*)field_addr);
                        break;
                    case TYPE_U64:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = %llu (u64)\n", field->name, *(uint64_t*)field_addr);
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
                    default:
                        offset += snprintf(buffer + offset, sizeof(buffer) - offset,
                                           "  %s = [...] (%s)\n", field->name, type_kind_to_string(field->type));
                        break;
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
    TypeKind malloc_params[] = {};
    cstruct_register_method_with_params("malloc", make_native(cstruct_method_alloc, 1, "malloc"),
                                       0, -1, -1, TYPE_CSTRUCT, TYPE_UNKNOWN, malloc_params);

    // from_ptr(ptr)
    TypeKind from_ptr_params[] = {TYPE_PTR};
    cstruct_register_method_with_params("from_ptr", make_native(cstruct_method_from_ptr, 2, "from_ptr"),
                                       1, -1, -1, TYPE_CSTRUCT, TYPE_UNKNOWN, from_ptr_params);

    // malloc_array(count) - 创建结构体数组
    TypeKind malloc_array_params[] = {TYPE_INT};
    cstruct_register_method_with_params("malloc_array", make_native(cstruct_method_malloc_array, 2, "malloc_array"),
                                       1, -1, -1, TYPE_CSTRUCT, TYPE_UNKNOWN, malloc_array_params);

    // 注册 cstruct 实例的方法
    // free()
    TypeKind free_params[] = {};
    cstruct_register_method_with_params("free", make_native(cstruct_method_free, 1, "free"),
                                       0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, free_params);

    // to_ptr()
    TypeKind to_ptr_params[] = {};
    cstruct_register_method_with_params("to_ptr", make_native(cstruct_method_to_ptr, 1, "to_ptr"),
                                       0, -1, -1, TYPE_PTR, TYPE_UNKNOWN, to_ptr_params);

    // to_str() - 打印内存布局
    TypeKind to_str_params[] = {};
    cstruct_register_method_with_params("to_str", make_native(cstruct_method_to_str, 1, "to_str"),
                                       0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, to_str_params);

    // size() - 返回 cstruct 大小
    TypeKind size_params[] = {};
    cstruct_register_method_with_params("size", make_native(cstruct_method_size, 1, "size"),
                                       0, -1, -1, TYPE_I32, TYPE_UNKNOWN, size_params);

    // alignment() - 返回 cstruct 对齐要求
    TypeKind alignment_params[] = {};
    cstruct_register_method_with_params("alignment", make_native(cstruct_method_alignment, 1, "alignment"),
                                       0, -1, -1, TYPE_I32, TYPE_UNKNOWN, alignment_params);

    // debug() - 显示详细的内存布局、偏移、对齐信息
    TypeKind debug_params[] = {};
    cstruct_register_method_with_params("debug", make_native(cstruct_method_debug, 1, "debug"),
                                       0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, debug_params);

    // hex() - 以十六进制格式显示原始字节
    TypeKind hex_params[] = {};
    cstruct_register_method_with_params("hex", make_native(cstruct_method_hex, 1, "hex"),
                                       0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, hex_params);

    // free_all() - 释放结构体数组
    TypeKind free_all_params[] = {};
    cstruct_register_method_with_params("free_all", make_native(cstruct_method_free_all, 1, "free_all"),
                                       0, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, free_all_params);

    // 注册 cstruct 数组的方法（OBJ_CSTRUCT_ARRAY 实例方法）
    // len() - 返回数组长度
    TypeKind array_len_params[] = {};
    cstruct_register_method_with_params("len", make_native(cstruct_array_method_len, 1, "len"),
                                       0, -1, -1, TYPE_INT, TYPE_UNKNOWN, array_len_params);
}
