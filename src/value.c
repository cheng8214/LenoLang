#include "include/lenolang.h"
#include "include/native.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

// ============================================================================
// 值操作（注意：val_int, val_float, val_num, val_bool, val_null, val_obj, val_is_truthy 已在 leno_value.h 中内联）
// ============================================================================

// 浮点 -> 字符串：最短往返表示。
//
// 旧实现直接 "%.17g"：double 的 9.7 真实值是 9.699999999999999289...，
// 于是 `print(9.7)` / `"x=" + 9.7` 会显示 9.6999999999999993（实测：爬虫算出的
// 平均分 9.068 打印成 9.0680000000000032）。而 CSV/JSON 两条路径用的是 "%g"，
// 同一个值在不同出口长得不一样 —— 这是缺陷。
//
// 现在按 "%.15g → %.16g → %.17g" 依次尝试，取第一个 **strtod 能精确回读**
// 的精度：9.7 在 15 位就往返成功 ⇒ 输出 "9.7"；而 0.1+0.2 这类真的需要
// 更多位才能无损的值，仍会输出 "0.30000000000000004"（不丢信息）。
void val_format_float(double v, char* buf, size_t size) {
    if (size == 0) return;
    if (isnan(v)) {
        snprintf(buf, size, "nan");
        return;
    }
    if (isinf(v)) {
        snprintf(buf, size, v > 0 ? "inf" : "-inf");
        return;
    }
    for (int prec = 15; prec <= 17; prec++) {
        snprintf(buf, size, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    // 整数值的浮点必须保留浮点特征：1 写成 "1.0"（保持旧行为）
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) {
        size_t len = strlen(buf);
        if (len + 2 < size) {
            buf[len] = '.';
            buf[len + 1] = '0';
            buf[len + 2] = '\0';
        }
    }
}

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
            val_format_float(val_as_num(v), buffer, sizeof(buffer));
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
            } else {
                return "[object]";
            }
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
            val_format_float(val_as_num(v), buffer, sizeof(buffer));
            result = strdup(buffer);
            break;
        }
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                ObjString* str = (ObjString*)val_as_obj(v);
                result = (char*)malloc(str->len + 1);
                if (result) {
                    memcpy(result, str->chars, str->len);
                    result[str->len] = '\0';
                }
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

                // 检测是否为异常对象（同时含 "msg" 字符串键和 "stack" 数组键）
                ObjString* msg_lookup = str_copy("msg", 3);
                Value msg_val = dict_get(dict, val_obj((Object*)msg_lookup));
                ObjString* stack_lookup = str_copy("stack", 5);
                Value stack_val = dict_get(dict, val_obj((Object*)stack_lookup));
                if (val_is_string(msg_val) &&
                    val_is_obj(stack_val) && val_as_obj(stack_val)->type == OBJ_ARRAY) {
                    // 异常对象：只返回 msg 字符串内容
                    ObjString* msg_str = (ObjString*)val_as_obj(msg_val);
                    result = (char*)malloc(msg_str->len + 1);
                    if (result) {
                        memcpy(result, msg_str->chars, msg_str->len);
                        result[msg_str->len] = '\0';
                    }
                    break;
                }

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

ObjRange* range_new(int64_t start, int64_t end, int inclusive) {
    ObjRange* range = (ObjRange*)gc_alloc(sizeof(ObjRange), OBJ_RANGE);
    if (!range) return NULL;
    range->start = start;
    range->end = end;
    range->inclusive = inclusive;
    return range;
}

int range_contains(ObjRange* range, int64_t value) {
    if (range->inclusive) {
        return value >= range->start && value <= range->end;
    } else {
        return value >= range->start && value < range->end;
    }
}
