#include "include/lenolang.h"
#include "include/native.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// ============================================================================
// 值操作（注意：val_int, val_float, val_num, val_bool, val_null, val_obj, val_is_truthy 已在 leno_value.h 中内联）
// ============================================================================

const char* val_to_string(Value v) {
    // 使用线程本地存储，确保线程安全
    #ifdef _MSC_VER
        static __declspec(thread) char buffer[BUFFER_MEDIUM];
    #else
        static __thread char buffer[BUFFER_MEDIUM];
    #endif
    
    switch (val_get_type(v)) {
        case VAL_NULL:
            return "null";
        case VAL_BOOL:
            return val_as_bool(v) ? "true" : "false";
        case VAL_INT:
            snprintf(buffer, sizeof(buffer), "%lld", (long long)val_as_int(v));
            return buffer;
        case VAL_FLOAT:
            snprintf(buffer, sizeof(buffer), "%.17g", val_as_num(v));
            if (!strchr(buffer, '.') && !strchr(buffer, 'e') && !strchr(buffer, 'E')) {
                size_t len = strlen(buffer);
                if (len + 2 < sizeof(buffer)) {
                    buffer[len] = '.';
                    buffer[len + 1] = '0';
                    buffer[len + 2] = '\0';
                }
            }
            return buffer;
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                return ((ObjString*)val_as_obj(v))->chars;
            } else if (val_as_obj(v)->type == OBJ_BIGINT) {
                ObjBigInt* bigint = (ObjBigInt*)val_as_obj(v);
                char* str = bigint_to_string(bigint);
                if (str) {
                    // 使用线程本地存储缓冲区（线程安全）
                    #ifdef _MSC_VER
                        static __declspec(thread) char bigint_buffer[BUFFER_XLARGE];
                    #else
                        static __thread char bigint_buffer[BUFFER_XLARGE];
                    #endif
                    strncpy(bigint_buffer, str, sizeof(bigint_buffer) - 1);
                    bigint_buffer[sizeof(bigint_buffer) - 1] = '\0';
                    free(str);
                    return bigint_buffer;
                }
                return "[bigint]";
            } else if (val_as_obj(v)->type == OBJ_STRUCT) {
                return "[struct]";
            } else if (val_as_obj(v)->type == OBJ_ENUM_DEF) {
                return "[enum_def]";
            } else if (val_as_obj(v)->type == OBJ_GUI_WINDOW) {
                return "[win]";
            } else if (val_as_obj(v)->type == OBJ_GUI_RENDERER) {
                return "[draw]";
            } else if (val_as_obj(v)->type == OBJ_GUI_EVENT) {
                return "[event]";
            } else if (val_as_obj(v)->type == OBJ_GUI_IMAGE) {
                return "[image]";
            } else if (val_as_obj(v)->type == OBJ_RGB) {
                return "[rgb]";
            }
            return "[object]";
        default:
            return "unknown";
    }
}

// 将 Value 转换为动态分配的字符串（调用者需要 free）
char* value_to_string(Value v) {
    char* result = NULL;

    switch (val_get_type(v)) {
        case VAL_NULL:
            result = strdup("null");
            break;
        case VAL_BOOL:
            result = strdup(val_as_bool(v) ? "true" : "false");
            break;
        case VAL_INT: {
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%lld", (long long)val_as_int(v));
            result = strdup(buffer);
            break;
        }
        case VAL_FLOAT: {
            char buffer[64];
            snprintf(buffer, sizeof(buffer), "%.17g", val_as_num(v));
            if (!strchr(buffer, '.') && !strchr(buffer, 'e') && !strchr(buffer, 'E')) {
                size_t len = strlen(buffer);
                if (len + 2 < sizeof(buffer)) {
                    buffer[len] = '.';
                    buffer[len + 1] = '0';
                    buffer[len + 2] = '\0';
                }
            }
            result = strdup(buffer);
            break;
        }
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                ObjString* str = (ObjString*)val_as_obj(v);
                result = strdup(str->chars);
            } else if (val_as_obj(v)->type == OBJ_BIGINT) {
                ObjBigInt* bigint = (ObjBigInt*)val_as_obj(v);
                result = bigint_to_string(bigint);
            } else if (val_as_obj(v)->type == OBJ_ARRAY) {
                ObjArray* arr = (ObjArray*)val_as_obj(v);
                int total_len = 2; // "["
                char** element_strs = (char**)malloc(arr->count * sizeof(char*));
                for (int i = 0; i < arr->count; i++) {
                    element_strs[i] = value_to_string(arr->elements[i]);
                    total_len += (int)strlen(element_strs[i]);
                    if (i < arr->count - 1) total_len += 2; // ", "
                }
                total_len += 1; // "]"
                result = (char*)malloc(total_len + 1);
                result[0] = '[';
                int pos = 1;
                for (int i = 0; i < arr->count; i++) {
                    int len = (int)strlen(element_strs[i]);
                    memcpy(result + pos, element_strs[i], len);
                    pos += len;
                    free(element_strs[i]);
                    if (i < arr->count - 1) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                }
                result[pos++] = ']';
                result[pos] = '\0';
                free(element_strs);
            } else if (val_as_obj(v)->type == OBJ_DICT) {
                ObjDict* dict = (ObjDict*)val_as_obj(v);
                // 先计算需要的总长度
                int total_len = 2; // "{}"
                int entry_count = 0;
                // 使用插入顺序数组
                for (int i = 0; i < dict->order_count; i++) {
                    Value key = dict->order[i];
                    if (!dict_has(dict, key)) continue;
                    // 跳过纯数字键（整数键）
                    if (val_is_int(key)) continue;
                    entry_count++;
                    ObjString* key_str = dict_key_to_string(key);
                    total_len += key_str->len + 2; // key + ": "
                    Value val = dict_get(dict, key);
                    char* val_str = value_to_string(val);
                    total_len += (int)strlen(val_str);
                    free(val_str);
                    if (entry_count > 1) total_len += 2; // ", "
                }
                // 数组部分的数字键
                for (int i = 0; i < dict->asize; i++) {
                    if (!val_is_null(dict->array[i])) {
                        entry_count++;
                        char key_buf[32];
                        snprintf(key_buf, sizeof(key_buf), "%d", i);
                        total_len += (int)strlen(key_buf) + 2;
                        char* val_str = value_to_string(dict->array[i]);
                        total_len += (int)strlen(val_str);
                        free(val_str);
                        if (entry_count > 1) total_len += 2;
                    }
                }
                result = (char*)malloc(total_len + 1);
                result[0] = '{';
                int pos = 1;
                int printed = 0;
                for (int i = 0; i < dict->order_count; i++) {
                    Value key = dict->order[i];
                    if (!dict_has(dict, key)) continue;
                    // 跳过纯数字键（整数键）
                    if (val_is_int(key)) continue;
                    if (printed > 0) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                    ObjString* key_str = dict_key_to_string(key);
                    memcpy(result + pos, key_str->chars, key_str->len);
                    pos += key_str->len;
                    result[pos++] = ':';
                    result[pos++] = ' ';
                    Value val = dict_get(dict, key);
                    char* val_str = value_to_string(val);
                    int val_len = (int)strlen(val_str);
                    memcpy(result + pos, val_str, val_len);
                    pos += val_len;
                    free(val_str);
                    printed++;
                }
                for (int i = 0; i < dict->asize; i++) {
                    if (!val_is_null(dict->array[i])) {
                        if (printed > 0) {
                            result[pos++] = ',';
                            result[pos++] = ' ';
                        }
                        char key_buf[32];
                        snprintf(key_buf, sizeof(key_buf), "%d", i);
                        int key_len = (int)strlen(key_buf);
                        memcpy(result + pos, key_buf, key_len);
                        pos += key_len;
                        result[pos++] = ':';
                        result[pos++] = ' ';
                        char* val_str = value_to_string(dict->array[i]);
                        int val_len = (int)strlen(val_str);
                        memcpy(result + pos, val_str, val_len);
                        pos += val_len;
                        free(val_str);
                        printed++;
                    }
                }
                result[pos++] = '}';
                result[pos] = '\0';
            } else if (val_as_obj(v)->type == OBJ_STRUCT) {
                ObjStruct* struct_obj = (ObjStruct*)val_as_obj(v);
                ObjStructDef* struct_def = struct_obj->def;
                
                // 计算需要的总长度
                int total_len = (int)strlen(struct_def->name) + 1; // "Point{"
                for (int i = 0; i < struct_def->field_count; i++) {
                    total_len += (int)strlen(struct_def->fields[i].name) + 1; // "field="
                    char* val_str = value_to_string(struct_obj->field_values[i]);
                    total_len += (int)strlen(val_str);
                    free(val_str);
                    if (i < struct_def->field_count - 1) total_len += 2; // ", "
                }
                total_len += 1; // "}"
                
                result = (char*)malloc(total_len + 1);
                int pos = 0;
                
                // struct 名称
                memcpy(result + pos, struct_def->name, strlen(struct_def->name));
                pos += (int)strlen(struct_def->name);
                result[pos++] = '{';
                
                // 字段
                for (int i = 0; i < struct_def->field_count; i++) {
                    if (i > 0) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                    // 字段名
                    memcpy(result + pos, struct_def->fields[i].name, strlen(struct_def->fields[i].name));
                    pos += (int)strlen(struct_def->fields[i].name);
                    result[pos++] = '=';
                    // 字段值
                    char* val_str = value_to_string(struct_obj->field_values[i]);
                    int val_len = (int)strlen(val_str);
                    memcpy(result + pos, val_str, val_len);
                    pos += val_len;
                    free(val_str);
                }
                
                result[pos++] = '}';
                result[pos] = '\0';
            } else if (val_as_obj(v)->type == OBJ_ENUM_DEF) {
                ObjEnumDef* enum_def = (ObjEnumDef*)val_as_obj(v);
                
                // 计算需要的总长度
                int total_len = 2; // "{}"
                for (int i = 0; i < enum_def->member_count; i++) {
                    total_len += (int)strlen(enum_def->members[i].name) + 2; // "name: "
                    char val_buf[32];
                    snprintf(val_buf, sizeof(val_buf), "%lld", (long long)enum_def->members[i].value);
                    total_len += (int)strlen(val_buf);
                    if (i < enum_def->member_count - 1) total_len += 2; // ", "
                }
                
                result = (char*)malloc(total_len + 1);
                int pos = 0;
                result[pos++] = '{';
                
                for (int i = 0; i < enum_def->member_count; i++) {
                    if (i > 0) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                    // 成员名
                    memcpy(result + pos, enum_def->members[i].name, strlen(enum_def->members[i].name));
                    pos += (int)strlen(enum_def->members[i].name);
                    result[pos++] = ':';
                    result[pos++] = ' ';
                    // 成员值
                    char val_buf[32];
                    snprintf(val_buf, sizeof(val_buf), "%lld", (long long)enum_def->members[i].value);
                    int val_len = (int)strlen(val_buf);
                    memcpy(result + pos, val_buf, val_len);
                    pos += val_len;
                }
                
                result[pos++] = '}';
                result[pos] = '\0';
            } else if (val_as_obj(v)->type == OBJ_FFI_POINTER) {
                ObjFFIPointer* ptr = (ObjFFIPointer*)val_as_obj(v);
                char buf[64];
                snprintf(buf, sizeof(buf), "<ptr %p>", ptr->ptr);
                result = strdup(buf);
            } else if (val_as_obj(v)->type == OBJ_FFI_CALLBACK) {
                ObjFFICallback* cb = (ObjFFICallback*)val_as_obj(v);
                char buf[64];
                snprintf(buf, sizeof(buf), "<callback %p>", cb->trampoline);
                result = strdup(buf);
            } else if (val_as_obj(v)->type == OBJ_FFI_LIBRARY) {
                result = strdup("<library>");
            } else if (val_as_obj(v)->type == OBJ_CSTRUCT_DEF) {
                ObjCStructDef* def = (ObjCStructDef*)val_as_obj(v);

                // 计算需要的总长度
                int total_len = 8; // "cstruct "
                total_len += (int)strlen(def->name);
                total_len += 1; // "{"
                for (int i = 0; i < def->field_count; i++) {
                    total_len += (int)strlen(type_kind_to_string(def->fields[i].type)) + 1; // "type "
                    total_len += (int)strlen(def->fields[i].name);
                    if (i < def->field_count - 1) total_len += 2; // ", "
                }
                total_len += 1; // "}"

                result = (char*)malloc(total_len + 1);
                int pos = 0;

                // "cstruct "
                memcpy(result + pos, "cstruct ", 8);
                pos += 8;

                // cstruct 名称
                memcpy(result + pos, def->name, strlen(def->name));
                pos += (int)strlen(def->name);
                result[pos++] = '{';

                // 字段
                for (int i = 0; i < def->field_count; i++) {
                    if (i > 0) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                    // 类型名
                    const char* type_name = type_kind_to_string(def->fields[i].type);
                    memcpy(result + pos, type_name, strlen(type_name));
                    pos += (int)strlen(type_name);
                    result[pos++] = ' ';
                    // 字段名
                    memcpy(result + pos, def->fields[i].name, strlen(def->fields[i].name));
                    pos += (int)strlen(def->fields[i].name);
                }

                result[pos++] = '}';
                result[pos] = '\0';
            } else if (val_as_obj(v)->type == OBJ_CSTRUCT) {
                ObjCStruct* cst = (ObjCStruct*)val_as_obj(v);
                ObjCStructDef* def = cst->def;

                // 计算需要的总长度
                int total_len = 8; // "cstruct "
                total_len += (int)strlen(def->name);
                total_len += 1; // "{"
                for (int i = 0; i < def->field_count; i++) {
                    total_len += (int)strlen(def->fields[i].name) + 1; // "field="
                    Value field_val = cstruct_get_field_value(cst, i);
                    char* val_str = value_to_string(field_val);
                    total_len += (int)strlen(val_str);
                    free(val_str);
                    if (i < def->field_count - 1) total_len += 2; // ", "
                }
                total_len += 1; // "}"

                result = (char*)malloc(total_len + 1);
                int pos = 0;

                // "cstruct "
                memcpy(result + pos, "cstruct ", 8);
                pos += 8;

                // cstruct 名称
                memcpy(result + pos, def->name, strlen(def->name));
                pos += (int)strlen(def->name);
                result[pos++] = '{';

                // 字段
                for (int i = 0; i < def->field_count; i++) {
                    if (i > 0) {
                        result[pos++] = ',';
                        result[pos++] = ' ';
                    }
                    // 字段名
                    memcpy(result + pos, def->fields[i].name, strlen(def->fields[i].name));
                    pos += (int)strlen(def->fields[i].name);
                    result[pos++] = '=';
                    // 字段值
                    Value field_val = cstruct_get_field_value(cst, i);
                    char* val_str = value_to_string(field_val);
                    int val_len = (int)strlen(val_str);
                    memcpy(result + pos, val_str, val_len);
                    pos += val_len;
                    free(val_str);
                }

                result[pos++] = '}';
                result[pos] = '\0';
            } else if (val_as_obj(v)->type == OBJ_GUI_WINDOW) {
                result = strdup("<win>");
            } else if (val_as_obj(v)->type == OBJ_GUI_RENDERER) {
                result = strdup("<draw>");
            } else if (val_as_obj(v)->type == OBJ_GUI_EVENT) {
                result = strdup("<event>");
            } else if (val_as_obj(v)->type == OBJ_GUI_IMAGE) {
                result = strdup("<image>");
            } else if (val_as_obj(v)->type == OBJ_RGB) {
                ObjRgb* rgb = (ObjRgb*)val_as_obj(v);
                char buf[64];
                snprintf(buf, sizeof(buf), "Rgb(%d,%d,%d,%d)", rgb->r, rgb->g, rgb->b, rgb->a);
                result = strdup(buf);
            } else {
                result = strdup("[object]");
            }
            break;
        default:
            result = strdup("unknown");
            break;
    }

    return result ? result : strdup("");
}

// ============================================================================
// Range 操作
// ============================================================================

ObjRange* range_new(int start, int end, int inclusive) {
    ObjRange* range = (ObjRange*)gc_alloc(sizeof(ObjRange), OBJ_RANGE);
    if (!range) return NULL;
    range->start = start;
    range->end = end;
    range->inclusive = inclusive;
    return range;
}

int range_contains(ObjRange* range, int value) {
    if (range->inclusive) {
        return value >= range->start && value <= range->end;
    } else {
        return value >= range->start && value < range->end;
    }
}
