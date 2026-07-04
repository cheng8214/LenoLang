#include "include/lenolang.h"
#ifndef LENO_VM_ONLY
#include "include/leno_ast.h"
#endif
#include <stdlib.h>
#include <string.h>

// 创建基本类型
TypeInfo* type_new(TypeKind kind) {
    TypeInfo* type = (TypeInfo*)calloc(1, sizeof(TypeInfo));
    if (!type) {
        error_add(ERR_RUNTIME, 0, "内存分配失败");
        return NULL;
    }
    type->kind = kind;
    type->element_type = NULL;
    type->key_type = NULL;
    type->value_type = NULL;
    type->return_type = NULL;
    type->param_types = NULL;
    type->param_count = 0;
    return type;
}

// 创建数组类型
TypeInfo* type_array(TypeInfo* element_type) {
    TypeInfo* type = type_new(TYPE_ARRAY);
    if (type) {
        type->element_type = element_type;
    }
    return type;
}

// 创建字典类型
TypeInfo* type_dict(TypeInfo* key_type, TypeInfo* value_type) {
    TypeInfo* type = type_new(TYPE_DICT);
    if (type) {
        type->key_type = key_type;
        type->value_type = value_type;
    }
    return type;
}

// 创建泛型指针类型 Ptr[T]
TypeInfo* type_ptr_generic(TypeInfo* element_type) {
    TypeInfo* type = type_new(TYPE_PTR_GENERIC);
    if (type) {
        type->element_type = element_type;
    }
    return type;
}

// 创建函数类型
TypeInfo* type_function(TypeInfo* return_type, TypeInfo** param_types, int param_count) {
    TypeInfo* type = type_new(TYPE_FUNCTION);
    if (type) {
        type->return_type = return_type;
        if (param_count > 0 && param_types) {
            type->param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_count);
            if (type->param_types) {
                for (int i = 0; i < param_count; i++) {
                    type->param_types[i] = type_copy(param_types[i]);
                }
                type->param_count = param_count;
            }
        } else {
            type->param_types = NULL;
            type->param_count = 0;
        }
    }
    return type;
}

// 创建泛型类型参数 (如 T, U)
TypeInfo* type_generic_param(const char* name) {
    TypeInfo* type = type_new(TYPE_GENERIC_PARAM);
    if (type && name) {
        type->type_param_name = strdup(name);
    }
    return type;
}

// 创建带约束的泛型类型参数 (如 T: Comparable)
TypeInfo* type_generic_param_constrained(const char* name, const char* constraint) {
    TypeInfo* type = type_new(TYPE_GENERIC_PARAM);
    if (type) {
        if (name) type->type_param_name = strdup(name);
        if (constraint) type->constraint_name = strdup(constraint);
    }
    return type;
}

// 释放类型信息
void type_free(TypeInfo* type) {
    if (!type) return;
    type_free(type->element_type);
    type_free(type->key_type);
    type_free(type->value_type);
    type_free(type->return_type);
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            type_free(type->param_types[i]);
        }
        free(type->param_types);
    }
    if (type->struct_name) {
        free(type->struct_name);
    }
    if (type->type_param_name) {
        free(type->type_param_name);
    }
    if (type->constraint_name) {
        free(type->constraint_name);
    }
    if (type->generic_args) {
        for (int i = 0; i < type->generic_count; i++) {
            type_free(type->generic_args[i]);
        }
        free(type->generic_args);
    }
    free(type);
}

// 类型相等检查
int type_equals(TypeInfo* a, TypeInfo* b) {
    if (!a || !b) return a == b;
    if (a->kind != b->kind) return 0;
    
    switch (a->kind) {
        case TYPE_ARRAY:
            return type_equals(a->element_type, b->element_type);
        case TYPE_DICT:
            return type_equals(a->key_type, b->key_type) &&
                   type_equals(a->value_type, b->value_type);
        case TYPE_PTR_GENERIC:
            return type_equals(a->element_type, b->element_type);
        case TYPE_FUNCTION: {
            // 处理 return_type：NULL 指针等同于 TYPE_NULL（void 返回）
            // 无显式返回类型的函数 return_type=NULL，func(...):void 的 return_type=TYPE_NULL
            int ret_equal;
            if (!a->return_type && !b->return_type) {
                ret_equal = 1;
            } else if (!a->return_type || !b->return_type) {
                // 一个 NULL 指针一个非 NULL：NULL 指针视为 void（TYPE_NULL）
                TypeInfo* non_null = a->return_type ? a->return_type : b->return_type;
                ret_equal = (non_null->kind == TYPE_NULL || non_null->kind == TYPE_ANY);
            } else {
                ret_equal = type_equals(a->return_type, b->return_type);
            }
            if (!ret_equal) return 0;
            if (a->param_count != b->param_count) return 0;
            for (int i = 0; i < a->param_count; i++) {
                if (!type_equals(a->param_types[i], b->param_types[i])) return 0;
            }
            return 1;
        }
        case TYPE_STRUCT:
        case TYPE_CSTRUCT:
        case TYPE_FACE:
        case TYPE_CLIB:
        case TYPE_CFUNC:
            if (a->struct_name && b->struct_name) {
                if (strcmp(a->struct_name, b->struct_name) != 0) return 0;
            } else if (a->struct_name != b->struct_name) {
                return 0;
            }
            // 比较泛型参数
            if (a->generic_count != b->generic_count) return 0;
            for (int i = 0; i < a->generic_count; i++) {
                if (!type_equals(a->generic_args[i], b->generic_args[i])) return 0;
            }
            return 1;
        case TYPE_GENERIC_PARAM:
            if (a->type_param_name && b->type_param_name) {
                return strcmp(a->type_param_name, b->type_param_name) == 0;
            }
            return a->type_param_name == b->type_param_name;
        default:
            return 1;
    }
}

// 复制类型信息（深拷贝）
TypeInfo* type_copy(TypeInfo* type) {
    if (!type) return NULL;
    
    TypeInfo* copy = type_new(type->kind);
    if (!copy) return NULL;
    
    switch (type->kind) {
        case TYPE_ARRAY:
            copy->element_type = type_copy(type->element_type);
            break;
        case TYPE_PTR_GENERIC:
            copy->element_type = type_copy(type->element_type);
            break;
        case TYPE_DICT:
            copy->key_type = type_copy(type->key_type);
            copy->value_type = type_copy(type->value_type);
            break;
        case TYPE_FUNCTION:
            copy->return_type = type_copy(type->return_type);
            if (type->param_count > 0 && type->param_types) {
                copy->param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * type->param_count);
                if (copy->param_types) {
                    for (int i = 0; i < type->param_count; i++) {
                        copy->param_types[i] = type_copy(type->param_types[i]);
                    }
                    copy->param_count = type->param_count;
                }
            }
            break;
        case TYPE_STRUCT:
        case TYPE_FACE:
        case TYPE_CSTRUCT:
        case TYPE_CLIB:
        case TYPE_CFUNC:
        case TYPE_ENUM:
            if (type->struct_name) {
                copy->struct_name = strdup(type->struct_name);
            }
            // 深拷贝泛型参数
            if (type->generic_count > 0 && type->generic_args) {
                copy->generic_args = (TypeInfo**)malloc(sizeof(TypeInfo*) * type->generic_count);
                if (copy->generic_args) {
                    for (int i = 0; i < type->generic_count; i++) {
                        copy->generic_args[i] = type_copy(type->generic_args[i]);
                    }
                    copy->generic_count = type->generic_count;
                }
            }
            break;
        case TYPE_GENERIC_PARAM:
            if (type->type_param_name) {
                copy->type_param_name = strdup(type->type_param_name);
            }
            if (type->constraint_name) {
                copy->constraint_name = strdup(type->constraint_name);
            }
            break;
        default:
            break;
    }
    
    return copy;
}

// 辅助函数：构建泛型类型字符串（递归）
static void build_generic_type_string(TypeInfo* type, char* buf, size_t buf_size, size_t* offset) {
    if (!type) {
        const char* str = "unknown";
        size_t len = strlen(str);
        if (*offset + len < buf_size - 1) {
            memcpy(buf + *offset, str, len);
            *offset += len;
            buf[*offset] = '\0';
        }
        return;
    }
    
    switch (type->kind) {
        case TYPE_ARRAY: {
            // Array[ElementType] 或 Array（元素类型未指定）
            const char* prefix = "Array";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            // 如果有元素类型，输出 [ElementType]
            if (type->element_type) {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = '[';
                    (*offset)++;
                }
                // 递归输出元素类型
                build_generic_type_string(type->element_type, buf, buf_size, offset);
                // 闭合 ]
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ']';
                    (*offset)++;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_DICT: {
            // Dict[KeyType, ValueType]
            const char* prefix = "Dict[";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            // 键类型
            build_generic_type_string(type->key_type, buf, buf_size, offset);
            // 逗号和空格
            if (*offset + 2 < buf_size) {
                buf[*offset] = ',';
                buf[*offset + 1] = ' ';
                *offset += 2;
            }
            // 值类型
            build_generic_type_string(type->value_type, buf, buf_size, offset);
            // 闭合 ]
            if (*offset + 1 < buf_size) {
                buf[*offset] = ']';
                (*offset)++;
                buf[*offset] = '\0';
            }
            break;
        }
        case TYPE_PTR_GENERIC: {
            // Ptr[ElementType]
            const char* prefix = "Ptr";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            if (type->element_type) {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = '[';
                    (*offset)++;
                }
                build_generic_type_string(type->element_type, buf, buf_size, offset);
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ']';
                    (*offset)++;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_FUNCTION: {
            // func():ReturnType 或 func(ParamType1, ParamType2):ReturnType
            const char* prefix = "func(";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            // 参数类型
            for (int i = 0; i < type->param_count; i++) {
                if (i > 0) {
                    if (*offset + 2 < buf_size) {
                        buf[*offset] = ',';
                        buf[*offset + 1] = ' ';
                        *offset += 2;
                    }
                }
                build_generic_type_string(type->param_types[i], buf, buf_size, offset);
            }
            // 闭合参数列表和返回类型
            if (type->return_type) {
                const char* sep = "):";
                size_t sep_len = strlen(sep);
                if (*offset + sep_len < buf_size - 1) {
                    memcpy(buf + *offset, sep, sep_len);
                    *offset += sep_len;
                }
                // void 返回类型（TYPE_NULL）显示为 "void"
                if (type->return_type->kind == TYPE_NULL) {
                    const char* void_str = "void";
                    size_t void_len = strlen(void_str);
                    if (*offset + void_len < buf_size - 1) {
                        memcpy(buf + *offset, void_str, void_len);
                        *offset += void_len;
                    }
                } else {
                    build_generic_type_string(type->return_type, buf, buf_size, offset);
                }
            } else {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ')';
                    (*offset)++;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_STRUCT: {
            // struct TypeName 或 struct（无名称）或 TypeName[int]（有泛型参数时省略 struct 前缀）
            if (type->generic_count > 0 && type->struct_name) {
                // 有泛型参数时直接输出 TypeName[int, string]
                size_t name_len = strlen(type->struct_name);
                if (*offset + name_len < buf_size - 1) {
                    memcpy(buf + *offset, type->struct_name, name_len);
                    *offset += name_len;
                }
                if (*offset + 1 < buf_size) {
                    buf[*offset] = '[';
                    (*offset)++;
                }
                for (int i = 0; i < type->generic_count; i++) {
                    if (i > 0) {
                        if (*offset + 2 < buf_size) {
                            buf[*offset] = ',';
                            buf[*offset + 1] = ' ';
                            *offset += 2;
                        }
                    }
                    build_generic_type_string(type->generic_args[i], buf, buf_size, offset);
                }
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ']';
                    (*offset)++;
                    buf[*offset] = '\0';
                }
            } else {
                const char* prefix = "struct";
                size_t prefix_len = strlen(prefix);
                if (*offset + prefix_len < buf_size - 1) {
                    memcpy(buf + *offset, prefix, prefix_len);
                    *offset += prefix_len;
                }
                if (type->struct_name) {
                    if (*offset + 1 < buf_size) {
                        buf[*offset] = ' ';
                        (*offset)++;
                    }
                    size_t name_len = strlen(type->struct_name);
                    if (*offset + name_len < buf_size - 1) {
                        memcpy(buf + *offset, type->struct_name, name_len);
                        *offset += name_len;
                        buf[*offset] = '\0';
                    }
                }
            }
            break;
        }
        case TYPE_FACE: {
            const char* prefix = "face";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            if (type->struct_name) {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ' ';
                    (*offset)++;
                }
                size_t name_len = strlen(type->struct_name);
                if (*offset + name_len < buf_size - 1) {
                    memcpy(buf + *offset, type->struct_name, name_len);
                    *offset += name_len;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_CSTRUCT: {
            // cstruct TypeName 或 cstruct（无名称）
            const char* prefix = "cstruct";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            // 如果有 struct 名称，输出名称
            if (type->struct_name) {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ' ';
                    (*offset)++;
                }
                size_t name_len = strlen(type->struct_name);
                if (*offset + name_len < buf_size - 1) {
                    memcpy(buf + *offset, type->struct_name, name_len);
                    *offset += name_len;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_CLIB:
        case TYPE_CFUNC: {
            // clib/cfunc TypeName
            const char* prefix = (type->kind == TYPE_CFUNC) ? "cfunc" : "clib";
            size_t prefix_len = strlen(prefix);
            if (*offset + prefix_len < buf_size - 1) {
                memcpy(buf + *offset, prefix, prefix_len);
                *offset += prefix_len;
            }
            if (type->struct_name) {
                if (*offset + 1 < buf_size) {
                    buf[*offset] = ' ';
                    (*offset)++;
                }
                size_t name_len = strlen(type->struct_name);
                if (*offset + name_len < buf_size - 1) {
                    memcpy(buf + *offset, type->struct_name, name_len);
                    *offset += name_len;
                    buf[*offset] = '\0';
                }
            }
            break;
        }
        case TYPE_GENERIC_PARAM: {
            // 输出泛型参数名 (T, U 等)
            const char* name = type->type_param_name ? type->type_param_name : "T";
            size_t len = strlen(name);
            if (*offset + len < buf_size - 1) {
                memcpy(buf + *offset, name, len);
                *offset += len;
                buf[*offset] = '\0';
            }
            break;
        }
        default: {
            // 基础类型
            const char* type_str = type_kind_to_string(type->kind);
            size_t len = strlen(type_str);
            if (*offset + len < buf_size - 1) {
                memcpy(buf + *offset, type_str, len);
                *offset += len;
                buf[*offset] = '\0';
            }
            break;
        }
    }
}

// 类型转字符串（使用线程本地存储，确保线程安全）
const char* type_to_string(TypeInfo* type) {
    if (!type) return "unknown";

    // 使用线程本地存储缓冲区，每个线程有独立的缓冲区
    #ifdef _MSC_VER
        static __declspec(thread) char buf[BUFFER_MEDIUM];
    #else
        static __thread char buf[BUFFER_MEDIUM];
    #endif

    size_t offset = 0;
    buf[0] = '\0';
    build_generic_type_string(type, buf, BUFFER_MEDIUM, &offset);
    return buf;
}

// TypeKind 转字符串
const char* type_kind_to_string(TypeKind kind) {
    switch (kind) {
        case TYPE_INFER:    return "any";  // 推断类型显示为 any
        case TYPE_INT:      return "int";
        case TYPE_FLOAT:    return "float";
        case TYPE_STRING:   return "string";
        case TYPE_BOOL:     return "bool";
        case TYPE_ARRAY:    return "Array";
        case TYPE_DICT:     return "Dict";
        case TYPE_BIGINT:   return "int";  // 对外统一为 int
        case TYPE_NULL:     return "null";
        case TYPE_FILE:     return "File";
        case TYPE_SOCKET:   return "Socket";
        case TYPE_CHANNEL:  return "Channel";
        case TYPE_THREAD:   return "Thread";
        case TYPE_PTR:      return "Ptr";
        case TYPE_PTR_GENERIC: return "Ptr";
        case TYPE_ANY:      return "any";
        case TYPE_FUNCTION: return "func";
        case TYPE_STRUCT:   return "struct";
        case TYPE_FACE:     return "face";
        // C 布局类型
        case TYPE_I8:       return "i8";
        case TYPE_U8:       return "u8";
        case TYPE_I16:      return "i16";
        case TYPE_U16:      return "u16";
        case TYPE_I32:      return "i32";
        case TYPE_U32:      return "u32";
        case TYPE_I64:      return "i64";
        case TYPE_U64:      return "u64";
        case TYPE_F32:      return "f32";
        case TYPE_F64:      return "f64";
        case TYPE_C_INT:    return "c_int";
        case TYPE_C_UINT:   return "c_uint";
        case TYPE_C_LONG:   return "c_long";
        case TYPE_C_ULONG:  return "c_ulong";
        case TYPE_C_LONGLONG:  return "c_longlong";
        case TYPE_C_ULONGLONG: return "c_ulonglong";
        case TYPE_C_SIZE:   return "c_size";
        case TYPE_C_SSIZE:  return "c_ssize";
        case TYPE_CSTRUCT:  return "cstruct";
        case TYPE_CLIB:     return "clib";
        case TYPE_CFUNC:    return "cfunc";
        case TYPE_STR8:     return "str8";
        case TYPE_STR16:    return "str16";
        case TYPE_GENERIC_PARAM: return "generic";
        default:            return "unknown";
    }
}

// 从Value推断类型
TypeInfo* type_infer_from_value(Value* v) {
    if (!v) return type_new(TYPE_ANY);

    switch (val_get_type(*v)) {
        case VAL_INT:
            return type_new(TYPE_INT);
        case VAL_FLOAT:
            return type_new(TYPE_FLOAT);
        case VAL_BOOL:
            return type_new(TYPE_BOOL);
        case VAL_NULL:
            return type_new(TYPE_NULL);
        case VAL_OBJ:
            if (val_as_obj(*v)->type == OBJ_STRING) {
                return type_new(TYPE_STRING);
            } else if (val_as_obj(*v)->type == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(*v);
                if (arr->count == 0) {
                    return type_array(type_new(TYPE_ANY));
                }
                TypeInfo* elem_type = type_infer_from_value(&arr->elements[0]);
                for (int i = 1; i < arr->count; i++) {
                    TypeInfo* curr = type_infer_from_value(&arr->elements[i]);
                    if (!type_equals(elem_type, curr)) {
                        type_free(elem_type);
                        type_free(curr);
                        return type_array(type_new(TYPE_ANY));
                    }
                    type_free(curr);
                }
                return type_array(elem_type);
            } else if (val_as_obj(*v)->type == OBJ_DICT) {
                return type_new(TYPE_DICT);
            } else if (val_as_obj(*v)->type == OBJ_STRUCT) {
                ObjStruct* struct_obj = (ObjStruct*)val_as_obj(*v);
                TypeInfo* type = type_new(TYPE_STRUCT);
                if (type && struct_obj->def && struct_obj->def->name) {
                    type->struct_name = strdup(struct_obj->def->name);
                }
                return type;
            } else if (val_as_obj(*v)->type == OBJ_CSTRUCT) {
                ObjCStruct* cstruct_obj = (ObjCStruct*)val_as_obj(*v);
                TypeInfo* type = type_new(TYPE_CSTRUCT);
                if (type && cstruct_obj->def && cstruct_obj->def->name) {
                    type->struct_name = strdup(cstruct_obj->def->name);
                }
                return type;
            } else if (val_as_obj(*v)->type == OBJ_FILE) {
                return type_new(TYPE_FILE);
            } else if (val_as_obj(*v)->type == OBJ_DICT) {
                return type_new(TYPE_DICT);
            } else if (val_as_obj(*v)->type == OBJ_FFI_POINTER) {
                ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)val_as_obj(*v);
                if (ffi_ptr->element_type != TYPE_PTR && ffi_ptr->element_type != TYPE_INFER) {
                    // Ptr[T] - 带有具体元素类型的指针
                    return type_ptr_generic(type_new(ffi_ptr->element_type));
                }
                return type_new(TYPE_PTR);
            } else if (val_as_obj(*v)->type == OBJ_FFI_LIBRARY) {
                return type_new(TYPE_PTR);
            } else if (val_as_obj(*v)->type == OBJ_FFI_CALLBACK) {
                return type_new(TYPE_PTR);
            } else if (val_as_obj(*v)->type == OBJ_SOCKET) {
                return type_new(TYPE_SOCKET);
            }
            return type_new(TYPE_ANY);
        default:
            return type_new(TYPE_ANY);
    }
}

#ifndef LENO_VM_ONLY
// 从AST推断类型
TypeInfo* type_infer_from_ast(Ast* ast) {
    if (!ast) return type_new(TYPE_ANY);

    switch (ast->kind) {
        case AST_NUM:
            // 检查是否是整数
            if (ast->u.num.is_bigint) {
                return type_new(TYPE_BIGINT);
            }
            if (ast->u.num.value == (int)ast->u.num.value) {
                return type_new(TYPE_INT);
            }
            return type_new(TYPE_FLOAT);
        case AST_STRING:
            return type_new(TYPE_STRING);
        case AST_BOOL:
            return type_new(TYPE_BOOL);
        case AST_NULL:
            return type_new(TYPE_NULL);
        default:
            return type_new(TYPE_ANY);
    }
}

#endif // LENO_VM_ONLY

// 检查类型是否兼容（用于赋值）
int type_is_compatible(TypeInfo* target, TypeInfo* source) {
    if (!target || !source) return 1;  // 任意类型都可以
    if (target->kind == TYPE_ANY) return 1;  // any 可以接受任何类型
    if (target->kind == TYPE_INFER) return 1;  // 推断类型接受任何值
    // 泛型类型参数：兼容任何类型（用于泛型函数体内部和类型推断）
    if (target->kind == TYPE_GENERIC_PARAM) return 1;
    if (source->kind == TYPE_GENERIC_PARAM) return 1;

    // null 可以赋值给任何类型（允许 int b = null, string c = null）
    if (source->kind == TYPE_NULL) return 1;

    // 相同类型肯定兼容
    if (type_equals(target, source)) return 1;

    // int 可以隐式转换为 float（类型提升）
    if (target->kind == TYPE_FLOAT && source->kind == TYPE_INT) {
        return 1;
    }

    // int 和 bigint 之间允许双向隐式转换
    // int48 与 Bint 互相兼容（int 内联值 → Bint 堆值）
    if ((target->kind == TYPE_BIGINT && source->kind == TYPE_INT) ||
        (target->kind == TYPE_INT && source->kind == TYPE_BIGINT)) {
        return 1;
    }

    // Bint 可以隐式转换为 float
    if (target->kind == TYPE_FLOAT && source->kind == TYPE_BIGINT) {
        return 1;
    }

    // clib 类型与 Ptr 的兼容性
    // ffi.load() 返回 Ptr，可以赋值给 clib 变量（Ptr → clib）
    // clib 变量也可以赋值给 Ptr（clib → Ptr，向下兼容）
    // 不同名的 clib 类型之间禁止互转，防止加载错误的库后调用不存在的函数
    if (target->kind == TYPE_CLIB && source->kind == TYPE_PTR) {
        return 1;
    }
    if (target->kind == TYPE_PTR && source->kind == TYPE_CLIB) {
        return 1;
    }
    if (target->kind == TYPE_CLIB && source->kind == TYPE_CLIB) {
        // 同名 clib 兼容，不同名不兼容
        if (target->struct_name && source->struct_name) {
            return strcmp(target->struct_name, source->struct_name) == 0;
        }
        return 1;
    }

    // 数组类型兼容性检查
    if (target->kind == TYPE_ARRAY && source->kind == TYPE_ARRAY) {
        // 如果源是空数组（元素类型未指定），可以接受任何目标类型
        // 这允许 Array[int] arr = [] 这样的初始化
        if (!source->element_type) {
            return 1;
        }
        
        // 如果源是 any[]（元素类型为 any），只能赋值给 any[] 目标
        // 不能赋值给具体类型的数组如 int[]
        if (source->element_type->kind == TYPE_ANY) {
            // 源是 any[]，只有当目标也是 any[] 时才兼容
            return (target->element_type && target->element_type->kind == TYPE_ANY);
        }
        
        // 如果目标是 any[]，禁止接受具体元素类型的数组
        // 防止类型约束丢失（如 Array[int] 赋值给 Array[any] 后添加字符串）
        if (target->element_type && target->element_type->kind == TYPE_ANY) {
            return 0;
        }
        
        // 如果目标没有指定元素类型（如 Array），接受任何源数组类型
        if (!target->element_type) {
            return 1;
        }
        
        // 否则需要元素类型具体兼容
        return type_is_compatible(target->element_type, source->element_type);
    }
    
    // 字典类型兼容性检查
    if (target->kind == TYPE_DICT && source->kind == TYPE_DICT) {
        // 如果目标没有指定键/值类型（如 Dict），接受任何 Dict 赋值
        if (!target->key_type && !target->value_type) {
            return 1;
        }
        
        // 如果源是空字典（没有指定键/值类型），可以接受任何目标类型
        // 这允许 Dict[string, int] d = {} 这样的初始化
        if (!source->key_type && !source->value_type) {
            return 1;
        }
        
        // 如果源有部分类型信息，检查兼容性
        if (!source->key_type || !source->value_type) {
            return 0;
        }
        
        // 检查键类型兼容性
        if (target->key_type && !type_is_compatible(target->key_type, source->key_type)) {
            return 0;
        }
        
        // 检查值类型兼容性
        if (target->value_type && !type_is_compatible(target->value_type, source->value_type)) {
            return 0;
        }
        
        return 1;
    }

    // 函数类型兼容性检查
    // 函数类型只能赋值给函数类型，不能赋值给其他类型
    if (source->kind == TYPE_FUNCTION) {
        // 源是函数类型，目标必须是函数类型或 any
        if (target->kind == TYPE_FUNCTION) {
            // 允许"具体 → 通用"的单向赋值
            // 即 func(int):int 可以赋值给 func（无签名）
            // 但 func(int):int 不能赋值给 func(string):int
            
            // 如果目标是无签名的 func（无参数类型、无返回类型），接受任何函数
            if (!target->return_type && !target->param_types && target->param_count == 0) {
                return 1;  // 具体 → 通用，安全
            }
            
            // 如果目标有签名，要求完全匹配
            return type_equals(target, source);
        }
        // 函数类型不能赋值给非函数类型（如 int）
        return 0;
    }

    // struct 类型兼容性检查
    if (target->kind == TYPE_STRUCT && source->kind == TYPE_STRUCT) {
        if (target->struct_name && source->struct_name) {
            return strcmp(target->struct_name, source->struct_name) == 0;
        }
        return 0;
    }

    // cstruct 类型兼容性检查
    if (target->kind == TYPE_CSTRUCT && source->kind == TYPE_CSTRUCT) {
        if (target->struct_name && source->struct_name) {
            return strcmp(target->struct_name, source->struct_name) == 0;
        }
        return 0;
    }

    // struct 和 cstruct 之间的交叉兼容性检查
    // 当类型名称相同时，允许 TYPE_STRUCT 和 TYPE_CSTRUCT 互相兼容
    if ((target->kind == TYPE_STRUCT && source->kind == TYPE_CSTRUCT) ||
        (target->kind == TYPE_CSTRUCT && source->kind == TYPE_STRUCT)) {
        if (target->struct_name && source->struct_name) {
            return strcmp(target->struct_name, source->struct_name) == 0;
        }
        return 0;
    }

    // struct 目标 + face 源同名时兼容（用于泛型约束方法引用类型推断，如 func(Comparable):int 中的 Comparable 是 struct 类型）
    if (target->kind == TYPE_STRUCT && source->kind == TYPE_FACE) {
        if (target->struct_name && source->struct_name) {
            return strcmp(target->struct_name, source->struct_name) == 0;
        }
        return 0;
    }

    // face 类型兼容性检查
    if (target->kind == TYPE_FACE) {
        if (source->kind == TYPE_FACE) {
            if (target->struct_name && source->struct_name) {
                return strcmp(target->struct_name, source->struct_name) == 0;
            }
            return 0;
        }
        if (source->kind == TYPE_STRUCT && target->struct_name) {
            ObjFaceDef* fdef = face_def_find(target->struct_name);
            if (fdef) {
                ObjStructDef* sdef = struct_def_find(source->struct_name);
                if (sdef) {
                    return struct_implements_face(sdef, fdef);
                }
            }
            return 0;
        }
        return 0;
    }

    // Ptr 类型兼容性检查
    // FFI 指针和 FFI 库对象都可以赋值给 Ptr 类型
    if (target->kind == TYPE_PTR) {
        if (source->kind == TYPE_PTR) {
            return 1;
        }
        // Ptr[u8] 可以赋值给 Ptr（向上兼容）
        if (source->kind == TYPE_PTR_GENERIC) {
            return 1;
        }
        return 0;
    }

    // Ptr[T] 类型兼容性检查
    if (target->kind == TYPE_PTR_GENERIC) {
        if (source->kind == TYPE_PTR) {
            // Ptr 可以赋值给 Ptr[T]（向下兼容，运行时不检查元素类型）
            return 1;
        }
        if (source->kind == TYPE_PTR_GENERIC) {
            // Ptr[T] 之间：如果目标元素类型未指定，接受任何源
            if (!target->element_type) return 1;
            // 如果源元素类型未指定，只能赋值给未指定的目标
            if (!source->element_type) return 0;
            // 两个都有元素类型，检查兼容性
            return type_is_compatible(target->element_type, source->element_type);
        }
        return 0;
    }

    // float 不能直接赋值给 int（需要显式转换）
    // if (target->kind == TYPE_INT && source->kind == TYPE_FLOAT) {
    //     return 0;  // 不允许隐式转换
    // }

    // Leno 基本类型与 C 布局类型的兼容性
    // int 与 C 整数类型互通
    if (target->kind == TYPE_INT && source->kind >= TYPE_I8 && source->kind <= TYPE_C_SSIZE) return 1;
    if (source->kind == TYPE_INT && target->kind >= TYPE_I8 && target->kind <= TYPE_C_SSIZE) return 1;
    // float 与 C 浮点类型互通
    if (target->kind == TYPE_FLOAT && (source->kind == TYPE_F32 || source->kind == TYPE_F64)) return 1;
    if (source->kind == TYPE_FLOAT && (target->kind == TYPE_F32 || target->kind == TYPE_F64)) return 1;
    // string 与 C 字符串类型互通
    if (target->kind == TYPE_STRING && (source->kind == TYPE_STR8 || source->kind == TYPE_STR16)) return 1;
    if (source->kind == TYPE_STRING && (target->kind == TYPE_STR8 || target->kind == TYPE_STR16)) return 1;
    // C 整数类型之间互通（i32 ↔ u32, i64 ↔ u64 等）
    if (target->kind >= TYPE_I8 && target->kind <= TYPE_C_SSIZE &&
        source->kind >= TYPE_I8 && source->kind <= TYPE_C_SSIZE) return 1;
    // C 浮点类型之间互通（f32 ↔ f64）
    if ((target->kind == TYPE_F32 || target->kind == TYPE_F64) &&
        (source->kind == TYPE_F32 || source->kind == TYPE_F64)) return 1;
    // C 字符串类型之间互通（str8 ↔ str16）
    if ((target->kind == TYPE_STR8 || target->kind == TYPE_STR16) &&
        (source->kind == TYPE_STR8 || source->kind == TYPE_STR16)) return 1;

    // cstruct ↔ Ptr 交叉兼容（cstruct 实例可传给期望 Ptr 的 clib 参数）
    if (target->kind == TYPE_PTR && source->kind == TYPE_CSTRUCT) return 1;
    if (target->kind == TYPE_PTR_GENERIC && source->kind == TYPE_CSTRUCT) return 1;
    if (target->kind == TYPE_CSTRUCT && source->kind == TYPE_PTR) return 1;
    // null 可赋值给 Ptr/str8/str16/cstruct（指针可为 NULL）
    if (target->kind == TYPE_PTR && source->kind == TYPE_NULL) return 1;
    if ((target->kind == TYPE_STR8 || target->kind == TYPE_STR16) && source->kind == TYPE_NULL) return 1;
    if (target->kind == TYPE_CSTRUCT && source->kind == TYPE_NULL) return 1;

    return 0;  // 默认不兼容
}

#ifndef LENO_VM_ONLY
// 从token类型获取TypeKind
TypeKind token_to_type_kind(LenoTokenType token) {
    switch (token) {
        case TOK_INT_TYPE:      return TYPE_INT;
        case TOK_FLOAT_TYPE:    return TYPE_FLOAT;
        case TOK_STRING_TYPE:   return TYPE_STRING;
        case TOK_BOOL_TYPE:     return TYPE_BOOL;
        case TOK_ARRAY_TYPE:    return TYPE_ARRAY;
        case TOK_DICT_TYPE:     return TYPE_DICT;
        // case TOK_BINT:          return TYPE_BIGINT;  // 已移除：对外统一用 int
        case TOK_ANY_TYPE:      return TYPE_ANY;
        case TOK_FILE_TYPE:     return TYPE_FILE;
        case TOK_SOCKET_TYPE:   return TYPE_SOCKET;
        case TOK_CHANNEL_TYPE:  return TYPE_CHANNEL;
        case TOK_THREAD_TYPE:   return TYPE_THREAD;
        case TOK_PTR_TYPE:      return TYPE_PTR;
        case TOK_VAR:           return TYPE_INFER;
        // C 布局类型
        case TOK_I8:            return TYPE_I8;
        case TOK_U8:            return TYPE_U8;
        case TOK_I16:           return TYPE_I16;
        case TOK_U16:           return TYPE_U16;
        case TOK_I32:           return TYPE_I32;
        case TOK_U32:           return TYPE_U32;
        case TOK_I64:           return TYPE_I64;
        case TOK_U64:           return TYPE_U64;
        case TOK_F32:           return TYPE_F32;
        case TOK_F64:           return TYPE_F64;
        case TOK_C_INT:         return TYPE_C_INT;
        case TOK_C_UINT:        return TYPE_C_UINT;
        case TOK_C_LONG:        return TYPE_C_LONG;
        case TOK_C_ULONG:       return TYPE_C_ULONG;
        case TOK_C_LONGLONG:    return TYPE_C_LONGLONG;
        case TOK_C_ULONGLONG:   return TYPE_C_ULONGLONG;
        case TOK_C_SIZE:        return TYPE_C_SIZE;
        case TOK_C_SSIZE:       return TYPE_C_SSIZE;
        case TOK_STR8:          return TYPE_STR8;
        case TOK_STR16:         return TYPE_STR16;
        default:                return TYPE_INFER;
    }
}

// 检查类型是否包含泛型参数（递归）
int type_has_generic(TypeInfo* type) {
    if (!type) return 0;
    if (type->kind == TYPE_GENERIC_PARAM) return 1;
    if (type_has_generic(type->element_type)) return 1;
    if (type_has_generic(type->key_type)) return 1;
    if (type_has_generic(type->value_type)) return 1;
    if (type_has_generic(type->return_type)) return 1;
    if (type->param_types) {
        for (int i = 0; i < type->param_count; i++) {
            if (type_has_generic(type->param_types[i])) return 1;
        }
    }
    return 0;
}

// 类型替换：将泛型参数替换为具体类型（深拷贝后替换，不修改原类型）
TypeInfo* type_substitute(TypeInfo* type, const char* param_name, TypeInfo* concrete) {
    if (!type) return NULL;
    
    // 如果是泛型参数且名字匹配，返回具体类型的拷贝
    if (type->kind == TYPE_GENERIC_PARAM && type->type_param_name &&
        strcmp(type->type_param_name, param_name) == 0) {
        return type_copy(concrete);
    }
    
    // 递归替换子类型
    TypeInfo* result = type_copy(type);
    if (result->element_type) {
        TypeInfo* substituted = type_substitute(result->element_type, param_name, concrete);
        type_free(result->element_type);
        result->element_type = substituted;
    }
    if (result->key_type) {
        TypeInfo* substituted = type_substitute(result->key_type, param_name, concrete);
        type_free(result->key_type);
        result->key_type = substituted;
    }
    if (result->value_type) {
        TypeInfo* substituted = type_substitute(result->value_type, param_name, concrete);
        type_free(result->value_type);
        result->value_type = substituted;
    }
    if (result->return_type) {
        TypeInfo* substituted = type_substitute(result->return_type, param_name, concrete);
        type_free(result->return_type);
        result->return_type = substituted;
    }
    if (result->param_types) {
        for (int i = 0; i < result->param_count; i++) {
            TypeInfo* substituted = type_substitute(result->param_types[i], param_name, concrete);
            type_free(result->param_types[i]);
            result->param_types[i] = substituted;
        }
    }
    // 替换泛型参数中的类型（如 Box[T] 的 T 被替换后，generic_args 中的 T 也需要替换）
    if (result->generic_args) {
        for (int i = 0; i < result->generic_count; i++) {
            TypeInfo* substituted = type_substitute(result->generic_args[i], param_name, concrete);
            type_free(result->generic_args[i]);
            result->generic_args[i] = substituted;
        }
    }
    return result;
}

#endif // LENO_VM_ONLY

// ============================================================================
// C 布局类型大小和对齐查询
// ============================================================================

// 获取 C 布局类型的大小（字节）
int c_layout_type_size(TypeKind kind) {
    switch (kind) {
        // 固定大小类型
        case TYPE_I8:       return 1;
        case TYPE_U8:       return 1;
        case TYPE_I16:      return 2;
        case TYPE_U16:      return 2;
        case TYPE_I32:      return 4;
        case TYPE_U32:      return 4;
        case TYPE_I64:      return 8;
        case TYPE_U64:      return 8;
        case TYPE_F32:      return 4;
        case TYPE_F64:      return 8;
        case TYPE_BOOL:     return 1;
        // C 平台相关类型
        case TYPE_C_INT:    return 4;
        case TYPE_C_UINT:   return 4;
        case TYPE_C_LONG:   return sizeof(long);        // Windows: 4, Linux/macOS: 8
        case TYPE_C_ULONG:  return sizeof(unsigned long);
        case TYPE_C_LONGLONG:   return 8;
        case TYPE_C_ULONGLONG:  return 8;
        case TYPE_C_SIZE:   return sizeof(size_t);      // 32位: 4, 64位: 8
        case TYPE_C_SSIZE:  return sizeof(ssize_t);
        case TYPE_PTR:      return sizeof(void*);       // 32位: 4, 64位: 8
        case TYPE_PTR_GENERIC: return sizeof(void*);    // 与 TYPE_PTR 相同
        case TYPE_STR8:     return sizeof(void*);       // str8 是 char* 指针
        case TYPE_STR16:    return 2;                   // str16 是 u16 数组，基本元素大小为 2
        default:            return 0;  // 未知类型
    }
}

// 获取 C 布局类型的对齐要求（字节）
int c_layout_type_align(TypeKind kind) {
    // 对齐要求通常等于类型大小（对于基本类型）
    return c_layout_type_size(kind);
}

// 向上对齐到指定边界
int c_layout_align_up(int offset, int alignment) {
    if (alignment <= 0) return offset;
    return (offset + alignment - 1) & ~(alignment - 1);
}

// 检查类型是否是有效的 C 布局字段类型
int c_layout_is_valid_field_type(TypeKind kind) {
    switch (kind) {
        case TYPE_I8: case TYPE_U8:
        case TYPE_I16: case TYPE_U16:
        case TYPE_I32: case TYPE_U32:
        case TYPE_I64: case TYPE_U64:
        case TYPE_F32: case TYPE_F64:
        case TYPE_BOOL:
        case TYPE_C_INT: case TYPE_C_UINT:
        case TYPE_C_LONG: case TYPE_C_ULONG:
        case TYPE_C_LONGLONG: case TYPE_C_ULONGLONG:
        case TYPE_C_SIZE: case TYPE_C_SSIZE:
        case TYPE_PTR:
        case TYPE_PTR_GENERIC:
        case TYPE_CSTRUCT:
        case TYPE_STR8:            // str8 - C char* 字符串指针
        case TYPE_STR16:           // str16 - UTF-16 字符串数组
            return 1;
        default:
            return 0;
    }
}

// 将 C 布局 TypeKind 映射为 Leno 等价 TypeKind
// cstruct 字段类型推断使用此函数，使 i32/u8 等自动映射为 int，无需 as int
TypeKind c_layout_type_to_leno(TypeKind kind) {
    switch (kind) {
        // C 整数类型 → int
        case TYPE_I8: case TYPE_U8:
        case TYPE_I16: case TYPE_U16:
        case TYPE_I32: case TYPE_U32:
        case TYPE_I64: case TYPE_U64:
        case TYPE_C_INT: case TYPE_C_UINT:
        case TYPE_C_LONG: case TYPE_C_ULONG:
        case TYPE_C_LONGLONG: case TYPE_C_ULONGLONG:
        case TYPE_C_SIZE: case TYPE_C_SSIZE:
            return TYPE_INT;
        // C 浮点类型 → float
        case TYPE_F32: case TYPE_F64:
            return TYPE_FLOAT;
        // C 字符串类型 → string
        case TYPE_STR8: case TYPE_STR16:
            return TYPE_STRING;
        // 其他类型保持不变
        default:
            return kind;
    }
}
