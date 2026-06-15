#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

// 辅助函数：获取值的基础类型
static TypeKind get_value_type(Value value) {
    switch (val_get_type(value)) {
        case VAL_INT: return TYPE_INT;
        case VAL_FLOAT: return TYPE_FLOAT;
        case VAL_BOOL: return TYPE_BOOL;
        case VAL_NULL: return TYPE_ANY;
        case VAL_OBJ:
            if (val_as_obj(value)->type == OBJ_STRING) return TYPE_STRING;
            else if (val_as_obj(value)->type == OBJ_DICT) return TYPE_DICT;
            else if (val_as_obj(value)->type == OBJ_ARRAY) return TYPE_ARRAY;
            else if (val_as_obj(value)->type == OBJ_STRUCT) return TYPE_STRUCT;
            else if (val_as_obj(value)->type == OBJ_GUI_WINDOW) return TYPE_WIN;
            else if (val_as_obj(value)->type == OBJ_GUI_RENDERER) return TYPE_DRAW;
            else if (val_as_obj(value)->type == OBJ_GUI_EVENT) return TYPE_EVENT;
            else if (val_as_obj(value)->type == OBJ_GUI_IMAGE) return TYPE_IMAGE;
            else if (val_as_obj(value)->type == OBJ_GUI_FONT) return TYPE_FONT;
            else if (val_as_obj(value)->type == OBJ_BIGINT) return TYPE_BIGINT;
            else if (val_as_obj(value)->type == OBJ_RGB) return TYPE_RGB;
            return TYPE_ANY;
        default: return TYPE_ANY;
    }
}

// 前向声明 infer_dict_type（供 infer_array_type 调用）
static ObjString* infer_dict_type(ObjDict* dict);

// 辅助函数：递归推断数组类型并构建泛型格式字符串
// 返回 Array[ElementType] 格式的类型字符串
static ObjString* infer_array_type(ObjArray* arr) {
    if (arr->count == 0) {
        // 空数组：优先使用 type_info（如果有）
        if (arr->type_info != NULL) {
            const char* type_str = type_to_string(arr->type_info);
            return str_copy(type_str, (int)strlen(type_str));
        }
        return str_copy("Array", 5);  // 空数组返回 Array（未指定元素类型）
    }

    // 检查第一个元素的类型
    Value first = arr->elements[0];

    // 如果元素是数组或字典，优先使用递归推断（而不是 type_info）
    // 因为 type_info 可能不包含嵌套类型的详细信息
    if (val_is_obj(first) && val_as_obj(first)->type == OBJ_ARRAY) {
        ObjString* inner_type = infer_array_type((ObjArray*)val_as_obj(first));

        int canPromote = 1;
        for (int i = 1; i < arr->count; i++) {
            Value elem = arr->elements[i];
            if (!(val_is_obj(elem) && val_as_obj(elem)->type == OBJ_ARRAY)) {
                canPromote = 0;
                break;
            }
            ObjString* check_type = infer_array_type((ObjArray*)val_as_obj(elem));
            // 检查是否可以类型提升
            if (strcmp(inner_type->chars, check_type->chars) != 0) {
                // 尝试类型提升：检查是否是 int/float 混合
                if ((strcmp(inner_type->chars, "Array[int]") == 0 && strcmp(check_type->chars, "Array[float]") == 0) ||
                    (strcmp(inner_type->chars, "Array[float]") == 0 && strcmp(check_type->chars, "Array[int]") == 0)) {
                    // 提升为 Array[Array[float]]
                    inner_type = str_copy("Array[float]", 12);
                } else {
                    canPromote = 0;
                    break;
                }
            }
        }

        char buf[BUFFER_SMALL];
        if (canPromote) {
            snprintf(buf, sizeof(buf), "Array[%s]", inner_type->chars);
        } else {
            snprintf(buf, sizeof(buf), "Array[any]");  // 无法类型提升，返回 Array[any]
        }
        return str_copy(buf, (int)strlen(buf));
    }

    // 如果元素是字典，递归推断
    if (val_is_obj(first) && val_as_obj(first)->type == OBJ_DICT) {
        ObjString* inner_type = infer_dict_type((ObjDict*)val_as_obj(first));

        int canPromote = 1;
        for (int i = 1; i < arr->count; i++) {
            Value elem = arr->elements[i];
            if (!(val_is_obj(elem) && val_as_obj(elem)->type == OBJ_DICT)) {
                canPromote = 0;
                break;
            }
            ObjString* check_type = infer_dict_type((ObjDict*)val_as_obj(elem));
            if (strcmp(inner_type->chars, check_type->chars) != 0) {
                canPromote = 0;
                break;
            }
        }

        char buf[BUFFER_SMALL];
        if (canPromote) {
            snprintf(buf, sizeof(buf), "Array[%s]", inner_type->chars);
        } else {
            snprintf(buf, sizeof(buf), "Array[any]");
        }
        return str_copy(buf, (int)strlen(buf));
    }

    // 基本类型处理 - 支持类型提升
    TypeKind elemType = get_value_type(first);
    // struct 类型：记录 struct 名称和公共 face
    const char* struct_name = NULL;
    const char* first_struct_def_name = NULL;  // 第一个元素的 struct 定义名
    char* common_face_name = NULL;
    int all_same_struct = 1;  // 假设所有元素都是同一个 struct 类型
    if (elemType == TYPE_STRUCT && val_is_obj(first) && val_as_obj(first)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(first);
        if (obj->def) {
            first_struct_def_name = obj->def->name;
            // 如果有 declared_face，说明该变量声明为 face 类型
            if (obj->declared_face) {
                elemType = TYPE_FACE;
                struct_name = obj->declared_face->chars;
                common_face_name = strdup(obj->declared_face->chars);
            } else {
                struct_name = obj->def->name;
                if (obj->def->impl_count > 0) {
                    common_face_name = strdup(obj->def->impl_names[0]);
                }
            }
        }
    }
    
    // 检查所有元素类型，支持类型提升
    for (int i = 1; i < arr->count; i++) {
        Value elem = arr->elements[i];
        TypeKind currType = get_value_type(elem);
        // 如果 struct 实例有 declared_face，视为 face 类型
        const char* elem_face_name = NULL;
        if (currType == TYPE_STRUCT && val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRUCT) {
            ObjStruct* obj = (ObjStruct*)val_as_obj(elem);
            // 检查是否与第一个元素同属一个 struct 定义
            if (obj->def && first_struct_def_name && strcmp(obj->def->name, first_struct_def_name) != 0) {
                all_same_struct = 0;
            }
            if (obj->declared_face) {
                currType = TYPE_FACE;
                elem_face_name = obj->declared_face->chars;
            }
        }
        
        if (currType != elemType) {
            // 尝试类型提升
            // int + float -> float
            if ((elemType == TYPE_INT && currType == TYPE_FLOAT) ||
                (elemType == TYPE_FLOAT && currType == TYPE_INT)) {
                elemType = TYPE_FLOAT;  // 提升为 float
            }
            // int/float + bigint -> bigint
            else if ((elemType == TYPE_INT && currType == TYPE_BIGINT) ||
                     (elemType == TYPE_BIGINT && currType == TYPE_INT) ||
                     (elemType == TYPE_FLOAT && currType == TYPE_BIGINT) ||
                     (elemType == TYPE_BIGINT && currType == TYPE_FLOAT)) {
                elemType = TYPE_BIGINT;  // 提升为 bigint
            }
            // struct + struct：查找公共 face
            else if (elemType == TYPE_STRUCT && currType == TYPE_STRUCT
                     && val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(elem);
                if (obj->def && obj->def->impl_count > 0 && common_face_name) {
                    int found = 0;
                    for (int fi = 0; fi < obj->def->impl_count; fi++) {
                        if (strcmp(obj->def->impl_names[fi], common_face_name) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (found) {
                        // 公共 face 有效，提升为 face 类型
                        elemType = TYPE_FACE;
                        struct_name = common_face_name;
                    } else {
                        free(common_face_name);
                        common_face_name = NULL;
                    }
                } else {
                    free(common_face_name);
                    common_face_name = NULL;
                }
            }
            // face + struct：检查 struct 是否实现该 face
            else if (elemType == TYPE_FACE && currType == TYPE_STRUCT
                     && val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(elem);
                if (obj->def && struct_name) {
                    ObjFaceDef* fdef = face_def_find(struct_name);
                    if (fdef && struct_implements_face(obj->def, fdef)) {
                        // struct 实现了该 face，保持 face 类型
                    } else {
                        free(common_face_name);
                        common_face_name = NULL;
                        elemType = TYPE_ANY;
                    }
                } else {
                    free(common_face_name);
                    common_face_name = NULL;
                    elemType = TYPE_ANY;
                }
            }
            // struct + face：检查 struct 是否实现该 face，提升为 face 类型
            else if (elemType == TYPE_STRUCT && currType == TYPE_FACE
                     && elem_face_name && struct_name) {
                // 第一个元素是 struct，当前元素是 face 变量
                ObjStructDef* first_sdef = struct_def_find(struct_name);
                ObjFaceDef* fdef = face_def_find(elem_face_name);
                if (first_sdef && fdef && struct_implements_face(first_sdef, fdef)) {
                    free(common_face_name);
                    common_face_name = strdup(elem_face_name);
                    struct_name = common_face_name;
                    elemType = TYPE_FACE;
                } else {
                    free(common_face_name);
                    common_face_name = NULL;
                    elemType = TYPE_ANY;
                }
            }
            // face + face：检查是否同一个 face
            else if (elemType == TYPE_FACE && currType == TYPE_FACE
                     && struct_name && elem_face_name) {
                if (strcmp(struct_name, elem_face_name) == 0) {
                    // 同一个 face，保持
                } else {
                    free(common_face_name);
                    common_face_name = NULL;
                    elemType = TYPE_ANY;
                }
            }
            // 无法类型提升
            else {
                free(common_face_name);
                return str_copy("Array[any]", 10);  // 返回 Array[any]
            }
        } else if (elemType == TYPE_STRUCT && val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRUCT) {
            // 相同类型 struct，检查是否同名
            ObjStruct* obj = (ObjStruct*)val_as_obj(elem);
            if (obj->def && struct_name && strcmp(obj->def->name, struct_name) != 0) {
                // 不同名的 struct，查找公共 face
                if (obj->def->impl_count > 0 && common_face_name) {
                    int found = 0;
                    for (int fi = 0; fi < obj->def->impl_count; fi++) {
                        if (strcmp(obj->def->impl_names[fi], common_face_name) == 0) {
                            found = 1;
                            break;
                        }
                    }
                    if (found) {
                        elemType = TYPE_FACE;
                        struct_name = common_face_name;
                    } else {
                        free(common_face_name);
                        common_face_name = NULL;
                        elemType = TYPE_ANY;
                    }
                } else {
                    free(common_face_name);
                    common_face_name = NULL;
                    elemType = TYPE_ANY;
                }
            }
        }
    }
    
    // 如果所有元素都是同一个 struct 定义，优先使用 struct 名而非 face 名
    if (all_same_struct && first_struct_def_name) {
        elemType = TYPE_STRUCT;
        struct_name = first_struct_def_name;
    }
    
    // 根据最终类型构建类型字符串
    char buf[128];
    switch (elemType) {
        case TYPE_INT: snprintf(buf, sizeof(buf), "Array[int]"); break;
        case TYPE_FLOAT: snprintf(buf, sizeof(buf), "Array[float]"); break;
        case TYPE_BOOL: snprintf(buf, sizeof(buf), "Array[bool]"); break;
        case TYPE_STRING: snprintf(buf, sizeof(buf), "Array[string]"); break;
        case TYPE_DICT: snprintf(buf, sizeof(buf), "Array[Dict]"); break;
        case TYPE_BIGINT: snprintf(buf, sizeof(buf), "Array[int]"); break;  // 对外统一为 int
        case TYPE_STRUCT: snprintf(buf, sizeof(buf), "Array[%s]", struct_name ? struct_name : "struct"); break;
        case TYPE_FACE: snprintf(buf, sizeof(buf), "Array[%s]", struct_name ? struct_name : "face"); break;
        default: snprintf(buf, sizeof(buf), "Array[any]"); break;
    }
    free(common_face_name);
    return str_copy(buf, (int)strlen(buf));
}

// 辅助函数：推断字典类型并构建泛型格式字符串
// 返回 Dict[KeyType, ValueType] 格式的类型字符串
static ObjString* infer_dict_type_internal(ObjDict* dict) {
    // 空字典检查
    int total_count = dict->count + dict->acount;
    if (total_count == 0) {
        return str_copy("Dict", 4);  // 空字典返回 Dict
    }

    // 键类型推断：检测字典中是否有整数键
    const char* keyTypeName = "string";
    // 检查数组部分（整数键存储在这里）
    int has_hash_entries = 0;
    for (int i = 0; i < dict->capacity; i++) {
        ObjDictEntry* entry = &dict->entries[i];
        if (!val_is_null(entry->key) && entry->key != DICT_TOMBSTONE_VAL) {
            has_hash_entries = 1;
            break;
        }
    }
    if (dict->acount > 0 && has_hash_entries) {
        keyTypeName = "any";  // 混合键类型（int + string）
    } else if (dict->acount > 0) {
        keyTypeName = "int";  // 仅有整数键
    }

    // 推断值类型
    TypeKind valueType = TYPE_ANY;
    int first = 1;

    // 用于存储嵌套对象的类型字符串（当值是Array或Dict时）
    ObjString* nestedTypeStr = NULL;

    // 检查哈希部分（字符串键存储在这里）
    for (int i = 0; i < dict->capacity; i++) {
        ObjDictEntry* entry = &dict->entries[i];
        if (val_is_null(entry->key) || entry->key == DICT_TOMBSTONE_VAL) continue;  // 空槽或墓碑

        Value val = entry->value;
        TypeKind currType = get_value_type(val);

        // 如果值是数组，递归推断数组类型
        if (currType == TYPE_ARRAY && val_is_obj(val) && val_as_obj(val)->type == OBJ_ARRAY) {
            ObjString* arrType = infer_array_type((ObjArray*)val_as_obj(val));
            if (first) {
                nestedTypeStr = arrType;
                valueType = TYPE_ARRAY;
                first = 0;
            } else {
                // 检查是否与之前的类型一致
                if (valueType != TYPE_ARRAY || strcmp(nestedTypeStr->chars, arrType->chars) != 0) {
                    // 类型不一致，返回 any
                    valueType = TYPE_ANY;
                    if (nestedTypeStr) {
                        // 不需要释放，GC会处理
                    }
                    goto mixed_type;
                }
            }
            continue;
        }

        // 如果值是字典，递归推断字典类型
        if (currType == TYPE_DICT && val_is_obj(val) && val_as_obj(val)->type == OBJ_DICT) {
            ObjString* dictType = infer_dict_type((ObjDict*)val_as_obj(val));
            if (first) {
                nestedTypeStr = dictType;
                valueType = TYPE_DICT;
                first = 0;
            } else {
                // 检查是否与之前的类型一致
                if (valueType != TYPE_DICT || strcmp(nestedTypeStr->chars, dictType->chars) != 0) {
                    // 类型不一致，返回 any
                    valueType = TYPE_ANY;
                    if (nestedTypeStr) {
                        // 不需要释放，GC会处理
                    }
                    goto mixed_type;
                }
            }
            continue;
        }

        // 基本类型处理
        if (first) {
            valueType = currType;
            first = 0;
        } else if (currType != valueType) {
            // 尝试类型提升
            if ((valueType == TYPE_INT && currType == TYPE_FLOAT) ||
                (valueType == TYPE_FLOAT && currType == TYPE_INT)) {
                valueType = TYPE_FLOAT;
            } else if ((valueType == TYPE_INT && currType == TYPE_BIGINT) ||
                       (valueType == TYPE_BIGINT && currType == TYPE_INT) ||
                       (valueType == TYPE_FLOAT && currType == TYPE_BIGINT) ||
                       (valueType == TYPE_BIGINT && currType == TYPE_FLOAT)) {
                valueType = TYPE_BIGINT;
            } else {
                // 无法类型提升，返回 Dict[string, any]
                valueType = TYPE_ANY;
                first = 0;
                goto mixed_type;
            }
        }
    }

    // 检查数组部分（数字键存储在这里）
    if (dict->array != NULL && dict->asize > 0) {
        for (int i = 0; i < dict->asize; i++) {
            // 检查是否是有效的数组元素（非 null）
            if (val_is_null(dict->array[i])) continue;

            Value val = dict->array[i];
            TypeKind currType = get_value_type(val);

            // 如果值是数组，递归推断数组类型
            if (currType == TYPE_ARRAY && val_is_obj(val) && val_as_obj(val)->type == OBJ_ARRAY) {
                ObjString* arrType = infer_array_type((ObjArray*)val_as_obj(val));
                if (first) {
                    nestedTypeStr = arrType;
                    valueType = TYPE_ARRAY;
                    first = 0;
                } else {
                    // 检查是否与之前的类型一致
                    if (valueType != TYPE_ARRAY || strcmp(nestedTypeStr->chars, arrType->chars) != 0) {
                        valueType = TYPE_ANY;
                        goto mixed_type;
                    }
                }
                continue;
            }

            // 如果值是字典，递归推断字典类型
            if (currType == TYPE_DICT && val_is_obj(val) && val_as_obj(val)->type == OBJ_DICT) {
                ObjString* innerDictType = infer_dict_type((ObjDict*)val_as_obj(val));
                if (first) {
                    nestedTypeStr = innerDictType;
                    valueType = TYPE_DICT;
                    first = 0;
                } else {
                    // 检查是否与之前的类型一致
                    if (valueType != TYPE_DICT || strcmp(nestedTypeStr->chars, innerDictType->chars) != 0) {
                        valueType = TYPE_ANY;
                        goto mixed_type;
                    }
                }
                continue;
            }

            // 基本类型处理
            if (first) {
                valueType = currType;
                first = 0;
            } else if (currType != valueType) {
                // 尝试类型提升
                if ((valueType == TYPE_INT && currType == TYPE_FLOAT) ||
                    (valueType == TYPE_FLOAT && currType == TYPE_INT)) {
                    valueType = TYPE_FLOAT;
                } else if ((valueType == TYPE_INT && currType == TYPE_BIGINT) ||
                           (valueType == TYPE_BIGINT && currType == TYPE_INT) ||
                           (valueType == TYPE_FLOAT && currType == TYPE_BIGINT) ||
                           (valueType == TYPE_BIGINT && currType == TYPE_FLOAT)) {
                    valueType = TYPE_BIGINT;
                } else {
                    // 无法类型提升，返回 Dict[string, any]
                    valueType = TYPE_ANY;
                    goto mixed_type;
                }
            }
        }
    }

mixed_type:

    // 根据值类型构建类型字符串
    char buf[BUFFER_MEDIUM];
    if (valueType == TYPE_ARRAY && nestedTypeStr) {
        // 嵌套数组类型
        snprintf(buf, sizeof(buf), "Dict[%s, %s]", keyTypeName, nestedTypeStr->chars);
    } else if (valueType == TYPE_DICT && nestedTypeStr) {
        // 嵌套字典类型
        snprintf(buf, sizeof(buf), "Dict[%s, %s]", keyTypeName, nestedTypeStr->chars);
    } else {
        // 基本类型
        const char* valueTypeName;
        switch (valueType) {
            case TYPE_INT: valueTypeName = "int"; break;
            case TYPE_FLOAT: valueTypeName = "float"; break;
            case TYPE_BOOL: valueTypeName = "bool"; break;
            case TYPE_STRING: valueTypeName = "string"; break;
            case TYPE_BIGINT: valueTypeName = "Bint"; break;
            default: valueTypeName = "any"; break;
        }
        snprintf(buf, sizeof(buf), "Dict[%s, %s]", keyTypeName, valueTypeName);
    }
    return str_copy(buf, (int)strlen(buf));
}

static ObjString* infer_dict_type(ObjDict* dict) {
    return infer_dict_type_internal(dict);
}

// type(value) - 返回值的类型字符串
static Value native_type(int argCount, Value* args) {
    if (argCount < 1) {
        return val_null();
    }
    
    Value value = args[0];
    ObjString* typeStr;
    
    switch (val_get_type(value)) {
        case VAL_NULL:
            typeStr = str_copy("null", 4);
            break;
        case VAL_BOOL:
            typeStr = str_copy("bool", 4);
            break;
        case VAL_INT:
            typeStr = str_copy("int", 3);
            break;
        case VAL_FLOAT:
            typeStr = str_copy("float", 5);
            break;
        case VAL_OBJ:
            if (val_as_obj(value)->type == OBJ_STRING) {
                typeStr = str_copy("string", 6);
            } else if (val_as_obj(value)->type == OBJ_FUNCTION || val_as_obj(value)->type == OBJ_CLOSURE) {
                typeStr = str_copy("func", 4);
            } else if (val_as_obj(value)->type == OBJ_NATIVE) {
                typeStr = str_copy("native", 6);
            } else if (val_as_obj(value)->type == OBJ_BIGINT) {
                typeStr = str_copy("int", 3);  // 对外统一为 int
            } else if (val_as_obj(value)->type == OBJ_ARRAY) {
                typeStr = infer_array_type((ObjArray*)val_as_obj(value));
            } else if (val_as_obj(value)->type == OBJ_DICT) {
                typeStr = infer_dict_type((ObjDict*)val_as_obj(value));
            } else if (val_as_obj(value)->type == OBJ_FILE) {
                typeStr = str_copy("file", 4);
            } else if (val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* struct_obj = (ObjStruct*)val_as_obj(value);
                ObjStructDef* struct_def = struct_obj->def;
                if (struct_def && struct_def->name) {
                    typeStr = str_copy(struct_def->name, (int)strlen(struct_def->name));
                } else {
                    typeStr = str_copy("struct", 6);
                }
            } else if (val_as_obj(value)->type == OBJ_CSTRUCT) {
                ObjCStruct* cstruct_obj = (ObjCStruct*)val_as_obj(value);
                ObjCStructDef* cstruct_def = cstruct_obj->def;
                if (cstruct_def && cstruct_def->name) {
                    typeStr = str_copy(cstruct_def->name, (int)strlen(cstruct_def->name));
                } else {
                    typeStr = str_copy("cstruct", 7);
                }
            } else if (val_as_obj(value)->type == OBJ_GUI_WINDOW) {
                typeStr = str_copy("gwin", 4);
            } else if (val_as_obj(value)->type == OBJ_GUI_RENDERER) {
                typeStr = str_copy("gdraw", 5);
            } else if (val_as_obj(value)->type == OBJ_GUI_EVENT) {
                typeStr = str_copy("gevent", 6);
            } else if (val_as_obj(value)->type == OBJ_GUI_BUTTON) {
                typeStr = str_copy("gbutton", 7);
            } else if (val_as_obj(value)->type == OBJ_RGB) {
                typeStr = str_copy("grgb", 4);
            } else if (val_as_obj(value)->type == OBJ_SOCKET) {
                typeStr = str_copy("socket", 6);
            } else {
                typeStr = str_copy("object", 6);
            }
            break;
        default:
            typeStr = str_copy("unknown", 7);
            break;
    }
    
    return val_obj((Object*)typeStr);
}

// _int(value) - 转换为整数
static Value native_to_int(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }
    
    Value value = args[0];
    
    switch (val_get_type(value)) {
        case VAL_INT:
            return value;
        case VAL_FLOAT:
            return val_int((int)val_as_num(value));
        case VAL_BOOL:
            return val_int(val_as_bool(value) ? 1 : 0);
        case VAL_NULL:
            return val_int(0);
        case VAL_OBJ:
            if (val_as_obj(value)->type == OBJ_STRING) {
                ObjString* str = (ObjString*)val_as_obj(value);
                char* end;
                long num = strtol(str->chars, &end, 10);
                if (*end == '\0') {
                    return val_int((int)num);
                }
                native_throw_error("无法将字符串转换为整数");
                return val_int(0);
            } else if (val_as_obj(value)->type == OBJ_BIGINT) {
                ObjBigInt* big = (ObjBigInt*)val_as_obj(value);
                // 检查是否在 int32 范围内
                if (big->limb_count == 1) {
                    // 单个 limb，检查值是否在 int32 范围内
                    uint32_t val = big->limbs[0];
                    if (!big->is_negative && val <= INT32_MAX) {
                        return val_int((int)val);
                    } else if (big->is_negative && val <= ((uint32_t)INT32_MAX + 1)) {
                        return val_int(-(int)val);
                    }
                }
                // 超出 int32 范围，保持为 bigint 返回
                return value;
            }
            break;
        default:
            break;
    }
    
    // 不支持的类型，报错
    native_throw_error("无法将该类型转换为整数");
    return val_int(0);
}

// _float(value) - 转换为浮点数
static Value native_to_float(int argCount, Value* args) {
    if (argCount < 1) {
        return val_float(0.0);
    }
    
    Value value = args[0];
    
    switch (val_get_type(value)) {
        case VAL_INT:
            return val_float(val_as_num(value));
        case VAL_FLOAT:
            return value;
        case VAL_BOOL:
            return val_float(val_as_bool(value) ? 1.0 : 0.0);
        case VAL_NULL:
            return val_float(0.0);
        case VAL_OBJ:
            if (val_as_obj(value)->type == OBJ_STRING) {
                ObjString* str = (ObjString*)val_as_obj(value);
                char* end;
                double num = strtod(str->chars, &end);
                if (*end == '\0') {
                    return val_float(num);
                }
                native_throw_error("无法将字符串转换为浮点数");
                return val_float(0.0);
            } else if (val_as_obj(value)->type == OBJ_BIGINT) {
                ObjBigInt* bigint = (ObjBigInt*)val_as_obj(value);
                if (bigint->limb_count <= 2) {
                    return val_float((double)bigint_to_int64(bigint));
                }
                // 大数转换
                double result = 0.0;
                double base = 4294967296.0;  // 2^32
                for (int i = bigint->limb_count - 1; i >= 0; i--) {
                    result = result * base + (double)bigint->limbs[i];
                }
                return val_float(bigint->is_negative ? -result : result);
            }
            break;
        default:
            break;
    }
    
    // 不支持的类型，报错
    native_throw_error("无法将该类型转换为浮点数");
    return val_float(0.0);
}

// _bool(value) - 转换为布尔值
static Value native_to_bool(int argCount, Value* args) {
    if (argCount < 1) {
        return val_bool(0);
    }
    
    Value value = args[0];
    
    switch (val_get_type(value)) {
        case VAL_NULL:
            return val_bool(0);
        case VAL_BOOL:
            return value;
        case VAL_INT:
            return val_bool(val_as_num(value) != 0);
        case VAL_FLOAT:
            return val_bool(val_as_num(value) != 0.0);
        case VAL_OBJ: {
            ObjType objType = val_as_obj(value)->type;
            if (objType == OBJ_STRING) {
                ObjString* str = (ObjString*)val_as_obj(value);
                return val_bool(str->len > 0);
            } else if (objType == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(value);
                return val_bool(arr->count > 0);
            } else if (objType == OBJ_DICT) {
                return val_bool(1);
            } else if (objType == OBJ_BIGINT) {
                return val_bool(!bigint_is_zero((ObjBigInt*)val_as_obj(value)));
            }
            return val_bool(1);
        }
        default:
            return val_bool(0);
    }
}

// _str(value) - 转换为字符串
static Value native_to_str(int argCount, Value* args) {
    if (argCount < 1) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    Value value = args[0];
    char buf[BUFFER_MEDIUM];
    ObjString* result;
    
    switch (val_get_type(value)) {
        case VAL_NULL:
            result = str_copy("null", 4);
            break;
        case VAL_BOOL:
            result = str_copy(val_as_bool(value) ? "true" : "false", val_as_bool(value) ? 4 : 5);
            break;
        case VAL_INT:
            snprintf(buf, sizeof(buf), "%.0f", val_as_num(value));
            result = str_copy(buf, (int)strlen(buf));
            break;
        case VAL_FLOAT:
            snprintf(buf, sizeof(buf), "%.6g", val_as_num(value));
            result = str_copy(buf, (int)strlen(buf));
            break;
        case VAL_OBJ:
            if (val_as_obj(value)->type == OBJ_STRING) {
                return value;
            } else if (val_as_obj(value)->type == OBJ_BIGINT) {
                ObjBigInt* big = (ObjBigInt*)val_as_obj(value);
                char* bigStr = bigint_to_string(big);
                result = str_copy(bigStr, (int)strlen(bigStr));
                free(bigStr);
            } else if (val_as_obj(value)->type == OBJ_SOCKET) {
                ObjSocket* sock = (ObjSocket*)val_as_obj(value);
                snprintf(buf, sizeof(buf), "<Socket fd=%d>", (int)sock->fd);
                result = str_copy(buf, (int)strlen(buf));
            } else {
                snprintf(buf, sizeof(buf), "<%s>", "object");
                result = str_copy(buf, (int)strlen(buf));
            }
            break;
        default:
            result = str_copy("", 0);
            break;
    }
    
    return val_obj((Object*)result);
}
// _ptr(value) - 转换为指针
static Value native_to_ptr(int argCount, Value* args) {
    if (argCount < 1) {
        return val_null();
    }
    
    Value value = args[0];
    
    // 如果已经是 FFI 指针，直接返回
    if (val_is_obj(value) && (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                              val_as_obj(value)->type == OBJ_FFI_CALLBACK)) {
        return value;
    }
    
    // 如果是 null，返回 null
    if (val_is_null(value)) {
        return val_null();
    }
    
    // 其他类型无法转换为指针
    native_throw_error("无法将该类型转换为指针");
    return val_null();
}

// _int32(value) - 将整数截断为 32 位有符号整数（环绕语义）
// 用于加密算法、网络协议等需要固定位宽运算的场景
static Value native_to_int32(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }

    Value value = args[0];

    // 获取 int64 值
    int64_t i64_val;
    if (val_is_int(value)) {
        i64_val = (int64_t)val_as_int(value);
    } else if (val_is_bigint(value)) {
        i64_val = bigint_to_int64(val_as_bigint(value));
    } else if (val_is_float(value)) {
        i64_val = (int64_t)val_as_num(value);
    } else if (val_is_bool(value)) {
        i64_val = val_as_bool(value) ? 1 : 0;
    } else if (val_is_null(value)) {
        return val_int(0);
    } else {
        native_throw_error("无法将该类型转换为 int32");
        return val_int(0);
    }

    // 截断为 32 位有符号整数（环绕语义）
    int32_t result = (int32_t)(i64_val & 0xFFFFFFFF);
    return val_int((int)result);
}

// _uint32(value) - 将整数截断为 32 位无符号整数
// 等效于 to_unsigned(_int32(x))，用于加密/网络等需要将值解释为 32 位无符号数的场景
static Value native_to_uint32(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }

    Value value = args[0];

    int64_t i64_val;
    if (val_is_int(value)) {
        i64_val = (int64_t)val_as_int(value);
    } else if (val_is_bigint(value)) {
        i64_val = bigint_to_int64(val_as_bigint(value));
    } else if (val_is_float(value)) {
        i64_val = (int64_t)val_as_num(value);
    } else if (val_is_bool(value)) {
        i64_val = val_as_bool(value) ? 1 : 0;
    } else if (val_is_null(value)) {
        return val_int(0);
    } else {
        native_throw_error("无法将该类型转换为 uint32");
        return val_int(0);
    }

    // 截断为 32 位无符号整数
    uint32_t result = (uint32_t)(i64_val & 0xFFFFFFFF);
    // 结果在 0 ~ 4294967295 范围内
    // val_int_safe 自动处理：<= INT32_MAX 返回 VAL_INT，> INT32_MAX 返回 BigInt
    return val_int_safe((int64_t)result);
}

// _int64(value) - 将值截断为 64 位有符号整数
static Value native_to_int64(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }

    Value value = args[0];

    if (val_is_int(value)) {
        return value;  // Leno int 就是 int64，直接返回
    } else if (val_is_bigint(value)) {
        int64_t i64 = bigint_to_int64(val_as_bigint(value));
        return val_int_safe(i64);
    } else if (val_is_float(value)) {
        int64_t i64 = (int64_t)val_as_num(value);
        return val_int_safe(i64);
    } else if (val_is_bool(value)) {
        return val_int(val_as_bool(value) ? 1 : 0);
    } else if (val_is_null(value)) {
        return val_int(0);
    } else {
        native_throw_error("无法将该类型转换为 int64");
        return val_int(0);
    }
}

// _uint64(value) - 将值截断为 64 位无符号整数
static Value native_to_uint64(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }

    Value value = args[0];

    int64_t i64_val;
    if (val_is_int(value)) {
        i64_val = val_as_int(value);
    } else if (val_is_bigint(value)) {
        i64_val = bigint_to_int64(val_as_bigint(value));
    } else if (val_is_float(value)) {
        i64_val = (int64_t)val_as_num(value);
    } else if (val_is_bool(value)) {
        i64_val = val_as_bool(value) ? 1 : 0;
    } else if (val_is_null(value)) {
        return val_int(0);
    } else {
        native_throw_error("无法将该类型转换为 uint64");
        return val_int(0);
    }

    // 截断为 64 位无符号整数
    uint64_t result = (uint64_t)i64_val;
    // 如果值在 int64 范围内，用 val_int_safe 自动处理 int48/BigInt
    if (result <= (uint64_t)INT64_MAX) {
        return val_int_safe((int64_t)result);
    }
    // 超过 INT64_MAX，需要 BigInt
    return val_bigint_from_uint64(result);
}

// _uint8(value) - 将值截断为 8 位无符号整数
static Value native_to_uint8(int argCount, Value* args) {
    if (argCount < 1) {
        return val_int(0);
    }

    Value value = args[0];

    int64_t i64_val;
    if (val_is_int(value)) {
        i64_val = val_as_int(value);
    } else if (val_is_bigint(value)) {
        i64_val = bigint_to_int64(val_as_bigint(value));
    } else if (val_is_float(value)) {
        i64_val = (int64_t)val_as_num(value);
    } else if (val_is_bool(value)) {
        i64_val = val_as_bool(value) ? 1 : 0;
    } else if (val_is_null(value)) {
        return val_int(0);
    } else {
        native_throw_error("无法将该类型转换为 uint8");
        return val_int(0);
    }

    // 截断为 8 位无符号整数
    uint8_t result = (uint8_t)(i64_val & 0xFF);
    return val_int((int)result);
}

// _byte(value) - _uint8 的别名，语义更清晰
static Value native_to_byte(int argCount, Value* args) {
    return native_to_uint8(argCount, args);
}

// ==================== 初始化 ====================

void types_init_globals(void) {
    // 注册全局 type 函数（返回 string，1 个参数）
    TypeKind type_params[] = {TYPE_ANY};
    vm_register_native("type", native_type, 1, -1, -1, TYPE_STRING, type_params);

    // 注册类型转换函数
    TypeKind convert_params[] = {TYPE_ANY};
    vm_register_native("_int", native_to_int, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_float", native_to_float, 1, -1, -1, TYPE_FLOAT, convert_params);
    vm_register_native("_bool", native_to_bool, 1, -1, -1, TYPE_BOOL, convert_params);
    vm_register_native("_str", native_to_str, 1, -1, -1, TYPE_STRING, convert_params);
    vm_register_native("_ptr", native_to_ptr, 1, -1, -1, TYPE_PTR, convert_params);
    vm_register_native("_int32", native_to_int32, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_uint32", native_to_uint32, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_int64", native_to_int64, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_uint64", native_to_uint64, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_uint8", native_to_uint8, 1, -1, -1, TYPE_INT, convert_params);
    vm_register_native("_byte", native_to_byte, 1, -1, -1, TYPE_INT, convert_params);
}

// types 模块通过全局 type() 函数提供服务
// 不需要单独的模块初始化，保留此文件以维持模块结构一致性
