/**
 * 轻量级 JSON 解析器
 * 专为 LSP 协议设计，支持解析和生成 JSON-RPC 消息
 */

#include "leno_lsp.h"
#include <ctype.h>
#include <stdarg.h>

// JSON 解析器状态
typedef struct {
    const char* text;
    int pos;
    int len;
} JsonParser;

// 前向声明
static JsonValue* parse_value(JsonParser* p);
static void skip_whitespace(JsonParser* p);
static char* parse_string(JsonParser* p);

// 跳过空白字符
static void skip_whitespace(JsonParser* p) {
    while (p->pos < p->len && isspace((unsigned char)p->text[p->pos])) {
        p->pos++;
    }
}

// 解析字符串
static char* parse_string(JsonParser* p) {
    if (p->text[p->pos] != '"') return NULL;
    p->pos++; // 跳过开头的 "
    
    int start = p->pos;
    int len = 0;
    
    // 计算长度
    while (p->pos < p->len && p->text[p->pos] != '"') {
        if (p->text[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos += 2;
        } else {
            p->pos++;
        }
        len++;
    }
    
    // 分配内存
    char* str = (char*)malloc(len + 1);
    if (!str) return NULL;
    
    // 复制并处理转义
    p->pos = start;
    int i = 0;
    while (p->pos < p->len && p->text[p->pos] != '"') {
        if (p->text[p->pos] == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            switch (p->text[p->pos]) {
                case '"': str[i++] = '"'; break;
                case '\\': str[i++] = '\\'; break;
                case '/': str[i++] = '/'; break;
                case 'b': str[i++] = '\b'; break;
                case 'f': str[i++] = '\f'; break;
                case 'n': str[i++] = '\n'; break;
                case 'r': str[i++] = '\r'; break;
                case 't': str[i++] = '\t'; break;
                default: str[i++] = p->text[p->pos]; break;
            }
            p->pos++;
        } else {
            str[i++] = p->text[p->pos++];
        }
    }
    str[i] = '\0';
    
    if (p->pos < p->len && p->text[p->pos] == '"') {
        p->pos++; // 跳过结尾的 "
    }
    
    return str;
}

// 解析数字
static JsonValue* parse_number(JsonParser* p) {
    int start = p->pos;

    // 处理负号
    if (p->text[p->pos] == '-') p->pos++;

    // 整数部分
    while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) {
        p->pos++;
    }

    // 小数部分
    if (p->pos < p->len && p->text[p->pos] == '.') {
        p->pos++;
        while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) {
            p->pos++;
        }
    }

    // 指数部分
    if (p->pos < p->len && (p->text[p->pos] == 'e' || p->text[p->pos] == 'E')) {
        p->pos++;
        if (p->pos < p->len && (p->text[p->pos] == '+' || p->text[p->pos] == '-')) {
            p->pos++;
        }
        while (p->pos < p->len && isdigit((unsigned char)p->text[p->pos])) {
            p->pos++;
        }
    }
    
    char* num_str = (char*)malloc(p->pos - start + 1);
    if (!num_str) return NULL;
    memcpy(num_str, p->text + start, p->pos - start);
    num_str[p->pos - start] = '\0';
    
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) {
        free(num_str);
        return NULL;
    }
    
    val->type = JSON_NUMBER;
    val->data.number_val = atof(num_str);
    free(num_str);
    
    return val;
}

// 解析数组
static JsonValue* parse_array(JsonParser* p) {
    p->pos++; // 跳过 [
    skip_whitespace(p);
    
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    
    val->type = JSON_ARRAY;
    val->data.array_val.items = NULL;
    val->data.array_val.count = 0;
    
    if (p->pos < p->len && p->text[p->pos] == ']') {
        p->pos++;
        return val;
    }
    
    int capacity = 8;
    val->data.array_val.items = (JsonValue**)malloc(sizeof(JsonValue*) * capacity);
    if (!val->data.array_val.items) {
        free(val);
        return NULL;
    }
    
    while (p->pos < p->len) {
        JsonValue* item = parse_value(p);
        if (!item) {
            json_free(val);
            return NULL;
        }
        
        if (val->data.array_val.count >= capacity) {
            capacity *= 2;
            JsonValue** new_items = (JsonValue**)realloc(val->data.array_val.items, 
                                                           sizeof(JsonValue*) * capacity);
            if (!new_items) {
                json_free(val);
                return NULL;
            }
            val->data.array_val.items = new_items;
        }
        
        val->data.array_val.items[val->data.array_val.count++] = item;
        
        skip_whitespace(p);
        if (p->pos < p->len && p->text[p->pos] == ',') {
            p->pos++;
            skip_whitespace(p);
        } else if (p->pos < p->len && p->text[p->pos] == ']') {
            p->pos++;
            break;
        } else {
            json_free(val);
            return NULL;
        }
    }
    
    return val;
}

// 解析对象
static JsonValue* parse_object(JsonParser* p) {
    p->pos++; // 跳过 {
    skip_whitespace(p);
    
    JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
    if (!val) return NULL;
    
    val->type = JSON_OBJECT;
    val->data.object_val.members = NULL;
    val->data.object_val.count = 0;
    
    if (p->pos < p->len && p->text[p->pos] == '}') {
        p->pos++;
        return val;
    }
    
    int capacity = 8;
    val->data.object_val.members = (JsonMember*)malloc(sizeof(JsonMember) * capacity);
    if (!val->data.object_val.members) {
        free(val);
        return NULL;
    }
    
    while (p->pos < p->len) {
        skip_whitespace(p);
        
        // 解析 key
        char* key = parse_string(p);
        if (!key) {
            json_free(val);
            return NULL;
        }
        
        skip_whitespace(p);
        if (p->pos >= p->len || p->text[p->pos] != ':') {
            free(key);
            json_free(val);
            return NULL;
        }
        p->pos++; // 跳过 :
        
        // 解析 value
        JsonValue* value = parse_value(p);
        if (!value) {
            free(key);
            json_free(val);
            return NULL;
        }
        
        if (val->data.object_val.count >= capacity) {
            capacity *= 2;
            JsonMember* new_members = (JsonMember*)realloc(val->data.object_val.members,
                                                            sizeof(JsonMember) * capacity);
            if (!new_members) {
                free(key);
                json_free(value);
                json_free(val);
                return NULL;
            }
            val->data.object_val.members = new_members;
        }
        
        val->data.object_val.members[val->data.object_val.count].key = key;
        val->data.object_val.members[val->data.object_val.count].value = value;
        val->data.object_val.count++;
        
        skip_whitespace(p);
        if (p->pos < p->len && p->text[p->pos] == ',') {
            p->pos++;
            skip_whitespace(p);
        } else if (p->pos < p->len && p->text[p->pos] == '}') {
            p->pos++;
            break;
        } else {
            json_free(val);
            return NULL;
        }
    }
    
    return val;
}

// 解析值
static JsonValue* parse_value(JsonParser* p) {
    skip_whitespace(p);
    
    if (p->pos >= p->len) return NULL;
    
    char c = p->text[p->pos];
    
    if (c == '"') {
        char* str = parse_string(p);
        if (!str) return NULL;
        JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
        if (!val) {
            free(str);
            return NULL;
        }
        val->type = JSON_STRING;
        val->data.string_val = str;
        return val;
    }
    
    if (c == '{') {
        return parse_object(p);
    }
    
    if (c == '[') {
        return parse_array(p);
    }
    
    if (c == 't' && p->pos + 4 <= p->len && strncmp(p->text + p->pos, "true", 4) == 0) {
        p->pos += 4;
        JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
        if (!val) return NULL;
        val->type = JSON_BOOL;
        val->data.bool_val = true;
        return val;
    }
    
    if (c == 'f' && p->pos + 5 <= p->len && strncmp(p->text + p->pos, "false", 5) == 0) {
        p->pos += 5;
        JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
        if (!val) return NULL;
        val->type = JSON_BOOL;
        val->data.bool_val = false;
        return val;
    }
    
    if (c == 'n' && p->pos + 4 <= p->len && strncmp(p->text + p->pos, "null", 4) == 0) {
        p->pos += 4;
        JsonValue* val = (JsonValue*)malloc(sizeof(JsonValue));
        if (!val) return NULL;
        val->type = JSON_NULL;
        return val;
    }
    
    if (c == '-' || isdigit((unsigned char)c)) {
        return parse_number(p);
    }
    
    return NULL;
}

// 公共 API: 解析 JSON
JsonValue* json_parse(const char* text) {
    if (!text) return NULL;
    
    JsonParser parser = {
        .text = text,
        .pos = 0,
        .len = strlen(text)
    };
    
    return parse_value(&parser);
}

// 深拷贝 JSON 值
JsonValue* json_deep_copy(JsonValue* value) {
    if (!value) return NULL;
    
    JsonValue* copy = (JsonValue*)malloc(sizeof(JsonValue));
    if (!copy) return NULL;
    
    copy->type = value->type;
    
    switch (value->type) {
        case JSON_NULL:
            break;
        case JSON_BOOL:
            copy->data.bool_val = value->data.bool_val;
            break;
        case JSON_NUMBER:
            copy->data.number_val = value->data.number_val;
            break;
        case JSON_STRING:
            copy->data.string_val = strdup(value->data.string_val);
            if (!copy->data.string_val) {
                free(copy);
                return NULL;
            }
            break;
        case JSON_ARRAY: {
            copy->data.array_val.count = value->data.array_val.count;
            copy->data.array_val.items = (JsonValue**)malloc(sizeof(JsonValue*) * value->data.array_val.count);
            if (!copy->data.array_val.items) {
                free(copy);
                return NULL;
            }
            for (int i = 0; i < value->data.array_val.count; i++) {
                copy->data.array_val.items[i] = json_deep_copy(value->data.array_val.items[i]);
                if (!copy->data.array_val.items[i]) {
                    for (int j = 0; j < i; j++) {
                        json_free(copy->data.array_val.items[j]);
                    }
                    free(copy->data.array_val.items);
                    free(copy);
                    return NULL;
                }
            }
            break;
        }
        case JSON_OBJECT: {
            copy->data.object_val.count = value->data.object_val.count;
            copy->data.object_val.members = (JsonMember*)malloc(sizeof(JsonMember) * value->data.object_val.count);
            if (!copy->data.object_val.members) {
                free(copy);
                return NULL;
            }
            for (int i = 0; i < value->data.object_val.count; i++) {
                copy->data.object_val.members[i].key = strdup(value->data.object_val.members[i].key);
                if (!copy->data.object_val.members[i].key) {
                    for (int j = 0; j < i; j++) {
                        free(copy->data.object_val.members[j].key);
                        json_free(copy->data.object_val.members[j].value);
                    }
                    free(copy->data.object_val.members);
                    free(copy);
                    return NULL;
                }
                copy->data.object_val.members[i].value = json_deep_copy(value->data.object_val.members[i].value);
                if (!copy->data.object_val.members[i].value) {
                    free(copy->data.object_val.members[i].key);
                    for (int j = 0; j < i; j++) {
                        free(copy->data.object_val.members[j].key);
                        json_free(copy->data.object_val.members[j].value);
                    }
                    free(copy->data.object_val.members);
                    free(copy);
                    return NULL;
                }
            }
            break;
        }
        default:
            free(copy);
            return NULL;
    }
    
    return copy;
}

// 释放 JSON 值
void json_free(JsonValue* value) {
    if (!value) return;
    
    switch (value->type) {
        case JSON_STRING:
            free(value->data.string_val);
            break;
        case JSON_ARRAY:
            for (int i = 0; i < value->data.array_val.count; i++) {
                json_free(value->data.array_val.items[i]);
            }
            free(value->data.array_val.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < value->data.object_val.count; i++) {
                free(value->data.object_val.members[i].key);
                json_free(value->data.object_val.members[i].value);
            }
            free(value->data.object_val.members);
            break;
        default:
            break;
    }
    
    free(value);
}

// 辅助函数: 获取对象成员
JsonValue* json_object_get(JsonValue* obj, const char* key) {
    if (!obj || obj->type != JSON_OBJECT || !key) return NULL;
    
    for (int i = 0; i < obj->data.object_val.count; i++) {
        if (strcmp(obj->data.object_val.members[i].key, key) == 0) {
            return obj->data.object_val.members[i].value;
        }
    }
    return NULL;
}

// 辅助函数: 获取字符串值
const char* json_string_value(JsonValue* val) {
    if (!val || val->type != JSON_STRING) return NULL;
    return val->data.string_val;
}

// 辅助函数: 获取整数值
int json_int_value(JsonValue* val) {
    if (!val) return 0;
    if (val->type == JSON_NUMBER) return (int)val->data.number_val;
    if (val->type == JSON_STRING) return atoi(val->data.string_val);
    return 0;
}

// 辅助函数: 获取布尔值
bool json_bool_value(JsonValue* val) {
    if (!val) return false;
    if (val->type == JSON_BOOL) return val->data.bool_val;
    return false;
}

// JSON 字符串转义
static void json_escape_string(char** out, int* out_len, int* out_cap, const char* str) {
    for (const char* p = str; *p; p++) {
        char buf[8];
        int len = 0;
        
        switch (*p) {
            case '"':  strcpy(buf, "\\\""); len = 2; break;
            case '\\': strcpy(buf, "\\\\"); len = 2; break;
            case '\b': strcpy(buf, "\\b");  len = 2; break;
            case '\f': strcpy(buf, "\\f");  len = 2; break;
            case '\n': strcpy(buf, "\\n");  len = 2; break;
            case '\r': strcpy(buf, "\\r");  len = 2; break;
            case '\t': strcpy(buf, "\\t");  len = 2; break;
            default:
                if ((unsigned char)*p < 0x20) {
                    sprintf(buf, "\\u%04x", (unsigned char)*p);
                    len = 6;
                } else {
                    buf[0] = *p;
                    len = 1;
                }
        }
        
        if (*out_len + len >= *out_cap) {
            *out_cap *= 2;
            *out = (char*)realloc(*out, *out_cap);
        }
        memcpy(*out + *out_len, buf, len);
        *out_len += len;
    }
}

// 将 JSON 值序列化为字符串
static void json_stringify_value(JsonValue* val, char** out, int* out_len, int* out_cap);

static void json_stringify_value(JsonValue* val, char** out, int* out_len, int* out_cap) {
    if (!val) return;
    
    char buf[256];
    int len = 0;
    
    switch (val->type) {
        case JSON_NULL:
            len = 4;
            if (*out_len + len >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            memcpy(*out + *out_len, "null", len);
            *out_len += len;
            break;
            
        case JSON_BOOL:
            len = val->data.bool_val ? 4 : 5;
            if (*out_len + len >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            memcpy(*out + *out_len, val->data.bool_val ? "true" : "false", len);
            *out_len += len;
            break;
            
        case JSON_NUMBER:
            sprintf(buf, "%.15g", val->data.number_val);
            len = strlen(buf);
            if (*out_len + len >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            memcpy(*out + *out_len, buf, len);
            *out_len += len;
            break;
            
        case JSON_STRING:
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = '"';
            json_escape_string(out, out_len, out_cap, val->data.string_val);
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = '"';
            break;
            
        case JSON_ARRAY:
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = '[';
            for (int i = 0; i < val->data.array_val.count; i++) {
                if (i > 0) {
                    if (*out_len + 1 >= *out_cap) {
                        *out_cap *= 2;
                        *out = (char*)realloc(*out, *out_cap);
                    }
                    (*out)[(*out_len)++] = ',';
                }
                json_stringify_value(val->data.array_val.items[i], out, out_len, out_cap);
            }
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = ']';
            break;
            
        case JSON_OBJECT:
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = '{';
            for (int i = 0; i < val->data.object_val.count; i++) {
                if (i > 0) {
                    if (*out_len + 1 >= *out_cap) {
                        *out_cap *= 2;
                        *out = (char*)realloc(*out, *out_cap);
                    }
                    (*out)[(*out_len)++] = ',';
                }
                // key
                if (*out_len + 1 >= *out_cap) {
                    *out_cap *= 2;
                    *out = (char*)realloc(*out, *out_cap);
                }
                (*out)[(*out_len)++] = '"';
                json_escape_string(out, out_len, out_cap, val->data.object_val.members[i].key);
                if (*out_len + 2 >= *out_cap) {
                    *out_cap *= 2;
                    *out = (char*)realloc(*out, *out_cap);
                }
                (*out)[(*out_len)++] = '"';
                (*out)[(*out_len)++] = ':';
                // value
                json_stringify_value(val->data.object_val.members[i].value, out, out_len, out_cap);
            }
            if (*out_len + 1 >= *out_cap) {
                *out_cap *= 2;
                *out = (char*)realloc(*out, *out_cap);
            }
            (*out)[(*out_len)++] = '}';
            break;
    }
}

// 公共 API: 序列化 JSON
const char* json_stringify(JsonValue* value) {
    if (!value) return NULL;
    
    int cap = 256;
    char* out = (char*)malloc(cap);
    if (!out) return NULL;
    
    int len = 0;
    json_stringify_value(value, &out, &len, &cap);
    out[len] = '\0';
    
    return out;
}
