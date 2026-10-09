#include "include/native.h"
#include "include/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>

// 前向声明
extern ObjString* dict_key_to_string(Value key);

// StringBuilder for dynamic string construction

typedef struct {
    char* data;
    int len;
    int capacity;
} StringBuilder;

static void sb_init(StringBuilder* sb) {
    sb->capacity = 256;
    sb->data = malloc(sb->capacity);
    sb->len = 0;
    if (sb->data) sb->data[0] = '\0';
}

static void sb_free(StringBuilder* sb) {
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->capacity = 0;
}

static void sb_ensure_capacity(StringBuilder* sb, int needed) {
    if (needed > sb->capacity) {
        int new_capacity = sb->capacity * 2;
        while (new_capacity < needed) new_capacity *= 2;
        char* new_data = realloc(sb->data, new_capacity);
        if (new_data) {
            sb->data = new_data;
            sb->capacity = new_capacity;
        }
    }
}

static void sb_append_char(StringBuilder* sb, char c) {
    sb_ensure_capacity(sb, sb->len + 2);
    if (sb->data) {
        sb->data[sb->len++] = c;
        sb->data[sb->len] = '\0';
    }
}

static void sb_append_cstr(StringBuilder* sb, const char* str) {
    int len = strlen(str);
    sb_ensure_capacity(sb, sb->len + len + 1);
    if (sb->data) {
        memcpy(sb->data + sb->len, str, len);
        sb->len += len;
        sb->data[sb->len] = '\0';
    }
}

static void sb_append_int(StringBuilder* sb, int64_t num) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%" PRId64, num);
    sb_append_cstr(sb, buf);
}

static void sb_append_float(StringBuilder* sb, double num) {
    char buf[64];
    // 最短往返：JSON 里的浮点必须能无损反序列化回同一个 double
    // （旧的 "%g" 会把 0.1+0.2 写成 "0.3"、把 1.0 写成 "1" ⇒ 往返后精度/类型都变）
    val_format_float(num, buf, sizeof(buf));
    sb_append_cstr(sb, buf);
}

// JSON Lexer

typedef enum {
    JSON_TOKEN_EOF,
    JSON_TOKEN_LBRACE,
    JSON_TOKEN_RBRACE,
    JSON_TOKEN_LBRACKET,
    JSON_TOKEN_RBRACKET,
    JSON_TOKEN_COLON,
    JSON_TOKEN_COMMA,
    JSON_TOKEN_STRING,
    JSON_TOKEN_NUMBER,
    JSON_TOKEN_TRUE,
    JSON_TOKEN_FALSE,
    JSON_TOKEN_NULL,
    JSON_TOKEN_ERROR
} JsonTokenType;

typedef struct {
    JsonTokenType type;
    const char* start;
    int len;
    double num_value;
} JsonToken;

typedef struct {
    const char* input;
    int pos;
    int len;
} JsonLexer;

static void json_lexer_init(JsonLexer* lexer, const char* input) {
    lexer->input = input;
    lexer->pos = 0;
    lexer->len = strlen(input);
}

static char json_lexer_peek(JsonLexer* lexer) {
    if (lexer->pos >= lexer->len) return '\0';
    return lexer->input[lexer->pos];
}

static char json_lexer_advance(JsonLexer* lexer) {
    if (lexer->pos >= lexer->len) return '\0';
    return lexer->input[lexer->pos++];
}

static void json_lexer_skip_whitespace(JsonLexer* lexer) {
    while (true) {
        char c = json_lexer_peek(lexer);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            json_lexer_advance(lexer);
        } else {
            break;
        }
    }
}

static JsonToken json_lexer_read_string(JsonLexer* lexer) {
    JsonToken token;
    token.type = JSON_TOKEN_STRING;
    
    json_lexer_advance(lexer);  // Skip opening quote
    token.start = lexer->input + lexer->pos;  // Point to content, not opening quote
    
    while (json_lexer_peek(lexer) != '"' && json_lexer_peek(lexer) != '\0') {
        if (json_lexer_peek(lexer) == '\\') {
            json_lexer_advance(lexer);
        }
        json_lexer_advance(lexer);
    }
    
    token.len = (lexer->input + lexer->pos) - token.start;  // Length is just content
    
    if (json_lexer_peek(lexer) == '"') {
        json_lexer_advance(lexer);
    } else {
        token.type = JSON_TOKEN_ERROR;
    }
    
    return token;
}

static JsonToken json_lexer_read_number(JsonLexer* lexer) {
    JsonToken token;
    token.type = JSON_TOKEN_NUMBER;
    token.start = lexer->input + lexer->pos;
    
    if (json_lexer_peek(lexer) == '-') {
        json_lexer_advance(lexer);
    }
    
    while (isdigit(json_lexer_peek(lexer))) {
        json_lexer_advance(lexer);
    }
    
    if (json_lexer_peek(lexer) == '.') {
        json_lexer_advance(lexer);
        while (isdigit(json_lexer_peek(lexer))) {
            json_lexer_advance(lexer);
        }
    }
    
    if (json_lexer_peek(lexer) == 'e' || json_lexer_peek(lexer) == 'E') {
        json_lexer_advance(lexer);
        if (json_lexer_peek(lexer) == '+' || json_lexer_peek(lexer) == '-') {
            json_lexer_advance(lexer);
        }
        while (isdigit(json_lexer_peek(lexer))) {
            json_lexer_advance(lexer);
        }
    }
    
    token.len = (lexer->input + lexer->pos) - token.start;
    
    char* num_str = malloc(token.len + 1);
    strncpy(num_str, token.start, token.len);
    num_str[token.len] = '\0';
    token.num_value = strtod(num_str, NULL);
    free(num_str);
    
    return token;
}

static JsonToken json_lexer_read_identifier(JsonLexer* lexer) {
    JsonToken token;
    token.start = lexer->input + lexer->pos;
    
    while (isalnum(json_lexer_peek(lexer)) || json_lexer_peek(lexer) == '_') {
        json_lexer_advance(lexer);
    }
    
    token.len = (lexer->input + lexer->pos) - token.start;
    
    if (token.len == 4 && strncmp(token.start, "true", 4) == 0) {
        token.type = JSON_TOKEN_TRUE;
    } else if (token.len == 5 && strncmp(token.start, "false", 5) == 0) {
        token.type = JSON_TOKEN_FALSE;
    } else if (token.len == 4 && strncmp(token.start, "null", 4) == 0) {
        token.type = JSON_TOKEN_NULL;
    } else {
        token.type = JSON_TOKEN_ERROR;
    }
    
    return token;
}

static JsonToken json_lexer_next_token(JsonLexer* lexer) {
    json_lexer_skip_whitespace(lexer);
    
    if (lexer->pos >= lexer->len) {
        return (JsonToken){JSON_TOKEN_EOF, NULL, 0, 0};
    }
    
    char c = json_lexer_peek(lexer);
    
    switch (c) {
        case '{': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_LBRACE, NULL, 0, 0};
        case '}': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_RBRACE, NULL, 0, 0};
        case '[': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_LBRACKET, NULL, 0, 0};
        case ']': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_RBRACKET, NULL, 0, 0};
        case ':': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_COLON, NULL, 0, 0};
        case ',': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_COMMA, NULL, 0, 0};
        case '"': return json_lexer_read_string(lexer);
        default:
            if (isdigit(c) || c == '-') {
                return json_lexer_read_number(lexer);
            } else if (isalpha(c)) {
                return json_lexer_read_identifier(lexer);
            } else {
                json_lexer_advance(lexer);
                return (JsonToken){JSON_TOKEN_ERROR, NULL, 0, 0};
            }
    }
}

// JSON Parser

typedef struct {
    JsonLexer lexer;
    JsonToken current;
    bool has_error;
} JsonParser;

static void json_parser_init(JsonParser* parser, const char* input) {
    json_lexer_init(&parser->lexer, input);
    parser->current = json_lexer_next_token(&parser->lexer);
    parser->has_error = false;
}

static void json_parser_advance(JsonParser* parser) {
    parser->current = json_lexer_next_token(&parser->lexer);
}

static bool json_parser_check(JsonParser* parser, JsonTokenType type) {
    return parser->current.type == type;
}

static bool json_parser_match(JsonParser* parser, JsonTokenType type) {
    if (json_parser_check(parser, type)) {
        json_parser_advance(parser);
        return true;
    }
    return false;
}

static Value json_parse_value(JsonParser* parser);

// ---- `\uXXXX` 的两块砖：十六进制读数 + 码点写 UTF-8（2026-10-09 修）----
// 为什么必须补：JSON 的 `\uXXXX` 给的是**码点**（不是字节），而本语言内部一律 UTF-8
//   ⇒ 非 ASCII 码点必须按 UTF-8 多字节写出去。旧实现只写 `code < 128`（ASCII ⇒ 单字节）的，
//   中文这种（如 U+5468）**一个字节都不写** ⇒ 该段被静默吞掉（实测：`{"name":"\u5c4b\u9876"}`
//   解码得到空串 ⇒ LenoMusic 的"音乐库"源整列歌名全空 ✗）。
//   ⚠ 这里**不**做转义还原的"工具函数"给调用方用：转义是 JSON 语法的一部分，
//     解码器自己吃掉才对（让每个调用点先手工过一遍，迟早有地方忘 ✗）。
static int json_hex4(const char* p) {
    int v = 0;
    for (int k = 0; k < 4; k++) {
        char c = p[k];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;             // 非法十六进制 ⇒ 调用方原样保留（别写半个字符进去 ✗）
        v = v * 16 + d;
    }
    return v;
}

// 码点 → UTF-8 字节；返回写入的字节数（调用方保证 cp 在 0..0x10FFFF）
static int json_utf8_write(int cp, char* out) {
    if (cp <= 0x7F) { out[0] = (char)cp; return 1; }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static Value json_parse_string_token(JsonToken* token) {
    char* decoded = malloc(token->len + 1);
    int j = 0;
    
    for (int i = 0; i < token->len; i++) {
        if (token->start[i] == '\\' && i + 1 < token->len) {
            char next = token->start[i + 1];
            switch (next) {
                case '"': decoded[j++] = '"'; i++; break;
                case '\\': decoded[j++] = '\\'; i++; break;
                case '/': decoded[j++] = '/'; i++; break;
                case 'b': decoded[j++] = '\b'; i++; break;
                case 'f': decoded[j++] = '\f'; i++; break;
                case 'n': decoded[j++] = '\n'; i++; break;
                case 'r': decoded[j++] = '\r'; i++; break;
                case 't': decoded[j++] = '\t'; i++; break;
                case 'u': {
                    // `\uXXXX` = 6 个字符（'\' 'u' + 4 位十六进制）
                    //   ★ 2026-10-09 修：旧实现只写 `code < 128`（ASCII）⇒ 中文等码点被**静默丢掉**
                    //     （`{"name":"\u5c4b\u9876"}` 解出空串 ✗）。现在一律按 UTF-8 编码写出 ✓
                    if (i + 6 <= token->len) {
                        int code = json_hex4(token->start + i + 2);
                        if (code >= 0) {
                            i += 5;  // 跳过这 6 个字符（for 循环还会 i++ ✓）
                            // 代理对（surrogate pair）：高位 D800-DBFF + 紧邻的 `\uDC00-\uDFFF`
                            //   ⇒ 合成一个码点再编码（emoji 那种；不合成会写出两个非法码点 ✗）
                            if (code >= 0xD800 && code <= 0xDBFF &&
                                i + 6 < token->len &&
                                token->start[i + 1] == '\\' && token->start[i + 2] == 'u') {
                                int lo = json_hex4(token->start + i + 3);
                                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                    code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
                                    i += 6;
                                }
                            }
                            j += json_utf8_write(code, decoded + j);
                        }
                    }
                    break;
                }
                default: decoded[j++] = token->start[i]; break;
            }
        } else {
            decoded[j++] = token->start[i];
        }
    }
    decoded[j] = '\0';
    
    ObjString* str = str_new(decoded, j);
    free(decoded);
    return val_obj((Object*)str);
}

static Value json_parse_object(JsonParser* parser) {
    json_parser_advance(parser);
    
    ObjDict* dict = dict_new(8);
    if (!dict) return val_null();
    
    if (json_parser_match(parser, JSON_TOKEN_RBRACE)) {
        return val_obj((Object*)dict);
    }
    
    while (true) {
        if (!json_parser_check(parser, JSON_TOKEN_STRING)) {
            parser->has_error = true;
            return val_null();
        }
        
        JsonToken key_token = parser->current;
        json_parser_advance(parser);
        Value key_val = json_parse_string_token(&key_token);
        ObjString* key = (ObjString*)val_as_obj(key_val);
        
        if (!json_parser_match(parser, JSON_TOKEN_COLON)) {
            parser->has_error = true;
            return val_null();
        }
        
        Value value = json_parse_value(parser);
        dict_set(dict, val_obj((Object*)key), value);
        
        if (json_parser_match(parser, JSON_TOKEN_COMMA)) {
            continue;
        } else if (json_parser_match(parser, JSON_TOKEN_RBRACE)) {
            break;
        } else {
            parser->has_error = true;
            return val_null();
        }
    }
    
    return val_obj((Object*)dict);
}

static Value json_parse_array(JsonParser* parser) {
    json_parser_advance(parser);
    
    ObjArray* arr = arr_new(8);
    if (!arr) return val_null();
    
    if (json_parser_match(parser, JSON_TOKEN_RBRACKET)) {
        return val_obj((Object*)arr);
    }
    
    int index = 0;
    while (true) {
        Value element = json_parse_value(parser);
        
        while (index >= arr->capacity) {
            if (!arr_grow(arr)) {
                parser->has_error = true;
                return val_null();
            }
        }
        
        arr_write(arr, index, element);
        index++;
        arr->count = index;
        
        if (json_parser_match(parser, JSON_TOKEN_COMMA)) {
            continue;
        } else if (json_parser_match(parser, JSON_TOKEN_RBRACKET)) {
            break;
        } else {
            parser->has_error = true;
            return val_null();
        }
    }
    
    return val_obj((Object*)arr);
}

static Value json_parse_value(JsonParser* parser) {
    switch (parser->current.type) {
        case JSON_TOKEN_STRING: {
            JsonToken token = parser->current;
            json_parser_advance(parser);
            return json_parse_string_token(&token);
        }
        case JSON_TOKEN_NUMBER: {
            double num = parser->current.num_value;
            json_parser_advance(parser);
            if (num == (int64_t)num) {
                return val_int((int64_t)num);
            } else {
                return val_float(num);
            }
        }
        case JSON_TOKEN_TRUE:
            json_parser_advance(parser);
            return val_bool(true);
        case JSON_TOKEN_FALSE:
            json_parser_advance(parser);
            return val_bool(false);
        case JSON_TOKEN_NULL:
            json_parser_advance(parser);
            return val_null();
        case JSON_TOKEN_LBRACE:
            return json_parse_object(parser);
        case JSON_TOKEN_LBRACKET:
            return json_parse_array(parser);
        default:
            parser->has_error = true;
            return val_null();
    }
}

// jsons.decode
static Value jsons_decode_func(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    
    JsonParser parser;
    json_parser_init(&parser, str->chars);
    
    Value result = json_parse_value(&parser);
    
    if (parser.has_error) {
        return val_null();
    }
    
    return result;
}

// JSON Encoder

static void json_encode_value(StringBuilder* sb, Value value, int indent, bool pretty);

static void json_encode_string(StringBuilder* sb, const char* str, int len) {
    sb_append_char(sb, '"');
    for (int i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)str[i];
        switch (ch) {
            case '"': sb_append_cstr(sb, "\\\""); break;
            case '\\': sb_append_cstr(sb, "\\\\"); break;
            case '\b': sb_append_cstr(sb, "\\b"); break;
            case '\f': sb_append_cstr(sb, "\\f"); break;
            case '\n': sb_append_cstr(sb, "\\n"); break;
            case '\r': sb_append_cstr(sb, "\\r"); break;
            case '\t': sb_append_cstr(sb, "\\t"); break;
            default:
                if (ch < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    sb_append_cstr(sb, buf);
                } else {
                    sb_append_char(sb, ch);
                }
        }
    }
    sb_append_char(sb, '"');
}

static void json_encode_indent(StringBuilder* sb, int indent) {
    for (int i = 0; i < indent; i++) {
        sb_append_char(sb, ' ');
    }
}

static void json_encode_array(StringBuilder* sb, ObjArray* arr, int indent, bool pretty) {
    sb_append_char(sb, '[');
    
    if (pretty && arr->count > 0) {
        sb_append_char(sb, '\n');
    }
    
    for (int i = 0; i < arr->count; i++) {
        if (pretty) {
            json_encode_indent(sb, indent + 2);
        }
        
        json_encode_value(sb, arr->elements[i], indent + 2, pretty);
        
        if (i < arr->count - 1) {
            sb_append_char(sb, ',');
        }
        
        if (pretty) {
            sb_append_char(sb, '\n');
        }
    }
    
    if (pretty && arr->count > 0) {
        json_encode_indent(sb, indent);
    }
    
    sb_append_char(sb, ']');
}

static void json_encode_dict(StringBuilder* sb, ObjDict* dict, int indent, bool pretty) {
    sb_append_char(sb, '{');
    
    if (pretty && dict->count > 0) {
        sb_append_char(sb, '\n');
    }
    
    int count = 0;
    int total = dict->count;
    
    for (int i = 0; i < dict->order_count; i++) {
        Value order_key = dict->order[i];
        if (val_is_null(order_key)) continue;
        
        Value value = dict_get(dict, order_key);
        
        if (pretty) {
            json_encode_indent(sb, indent + 2);
        }
        
        // 获取键的字符串表示
        const char* key_chars = NULL;
        int key_len = 0;
        char key_buf[32];
        if (val_is_obj(order_key) && val_as_obj(order_key)->type == OBJ_STRING) {
            ObjString* ks = (ObjString*)val_as_obj(order_key);
            key_chars = ks->chars;
            key_len = ks->len;
        } else {
            ObjString* ks = dict_key_to_string(order_key);
            if (ks) {
                int copy_len = ks->len < (int)sizeof(key_buf) - 1 ? ks->len : (int)sizeof(key_buf) - 1;
                memcpy(key_buf, ks->chars, copy_len);
                key_buf[copy_len] = '\0';
                key_chars = key_buf;
                key_len = copy_len;
            } else {
                key_chars = "";
                key_len = 0;
            }
        }
        json_encode_string(sb, key_chars, key_len);
        
        sb_append_char(sb, ':');
        if (pretty) {
            sb_append_char(sb, ' ');
        }
        
        json_encode_value(sb, value, indent + 2, pretty);
        
        count++;
        if (count < total) {
            sb_append_char(sb, ',');
        }
        
        if (pretty) {
            sb_append_char(sb, '\n');
        }
    }
    
    if (pretty && total > 0) {
        json_encode_indent(sb, indent);
    }
    
    sb_append_char(sb, '}');
}

static void json_encode_value(StringBuilder* sb, Value value, int indent, bool pretty) {
    switch (val_get_type(value)) {
        case VAL_NULL:
            sb_append_cstr(sb, "null");
            break;
        case VAL_BOOL:
            sb_append_cstr(sb, val_as_bool(value) ? "true" : "false");
            break;
        case VAL_INT:
            sb_append_int(sb, val_as_num(value));
            break;
        case VAL_FLOAT:
            sb_append_float(sb, val_as_num(value));
            break;
        case VAL_OBJ:
            switch (val_as_obj(value)->type) {
                case OBJ_STRING:
                    json_encode_string(sb, ((ObjString*)val_as_obj(value))->chars, ((ObjString*)val_as_obj(value))->len);
                    break;
                case OBJ_ARRAY:
                    json_encode_array(sb, (ObjArray*)val_as_obj(value), indent, pretty);
                    break;
                case OBJ_DICT:
                    json_encode_dict(sb, (ObjDict*)val_as_obj(value), indent, pretty);
                    break;
                default:
                    sb_append_cstr(sb, "null");
                    break;
            }
            break;
        default:
            sb_append_cstr(sb, "null");
            break;
    }
}

// jsons.encode
static Value jsons_encode_func(int argc, Value* args) {
    (void)argc;
    
    StringBuilder sb;
    sb_init(&sb);
    
    json_encode_value(&sb, args[0], 0, false);
    
    ObjString* result = str_new(sb.data, sb.len);
    sb_free(&sb);
    
    return val_obj((Object*)result);
}

// jsons.encode_pretty
static Value jsons_encode_pretty_func(int argc, Value* args) {
    (void)argc;
    
    StringBuilder sb;
    sb_init(&sb);
    
    json_encode_value(&sb, args[0], 0, true);
    
    ObjString* result = str_new(sb.data, sb.len);
    sb_free(&sb);
    
    return val_obj((Object*)result);
}

// Helper to open file with UTF-8 path support
static FILE* fopen_utf8(const char* path, const char* mode) {
#ifdef _WIN32
    // On Windows, convert UTF-8 path to UTF-16 and use _wfopen
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) return NULL;
    
    wchar_t wmode[8];
    int i = 0;
    while (mode[i] && i < 7) {
        wmode[i] = (wchar_t)mode[i];
        i++;
    }
    wmode[i] = L'\0';
    
    FILE* file = _wfopen(wpath, wmode);
    free(wpath);
    return file;
#else
    // On Linux/macOS, fopen supports UTF-8 natively
    return fopen(path, mode);
#endif
}

// jsons.read_file
static Value jsons_read_file_func(int argc, Value* args) {
    (void)argc;
    ObjString* path = (ObjString*)val_as_obj(args[0]);

    // Use binary mode to get accurate file size
    FILE* file = fopen_utf8(path->chars, "rb");
    if (!file) {
        return val_null();
    }
    
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* content = malloc(size + 1);
    if (!content) {
        fclose(file);
        return val_null();
    }
    
    size_t read_size = fread(content, 1, size, file);
    content[read_size] = '\0';
    fclose(file);
    
    JsonParser parser;
    json_parser_init(&parser, content);
    Value result = json_parse_value(&parser);
    
    free(content);
    
    if (parser.has_error) {
        return val_null();
    }
    
    return result;
}

// jsons.write_file
static Value jsons_write_file_func(int argc, Value* args) {
    (void)argc;
    ObjString* path = (ObjString*)val_as_obj(args[0]);
    
    StringBuilder sb;
    sb_init(&sb);
    json_encode_value(&sb, args[1], 0, true);
    
    // Use binary mode to write exact bytes
    FILE* file = fopen_utf8(path->chars, "wb");
    if (!file) {
        sb_free(&sb);
        return val_bool(false);
    }
    
    fwrite(sb.data, 1, sb.len, file);
    fclose(file);
    
    sb_free(&sb);
    
    return val_bool(true);
}

/* ============================================================================
 * T21：类型化取值助手 —— 把 "any 收窄" 收进标准库
 * ----------------------------------------------------------------------------
 * 背景：`jsons.decode/read_file` 的返回类型**只能是 any**（JSON 顶层可能是对象/数组/
 * 标量，硬改成 Dict 是错语义）⇒ 此前每个消费点都要手写 `if x is Dict => d and d.has(k)`，
 * 于是每个项目都自造一层 json_get/json_obj/json_keys（TraeSign 的 `trae_core.leno` 就是）。
 * 这里把这一层做成标准库：**收窄只发生在这几个函数体里，调用点零样板** ✓。
 *
 * 语义约定（全部**不抛异常**：坏输入 ⇒ 默认值/空；唯一"失败"是 write_text 落盘失败返 false）：
 *   · get_str/get_int/get_float/get_bool(obj, key, default)
 *       - obj 不是字典 / 缺键 / 值转不过去 ⇒ default（default 本身也走一遍转换 ✓）
 *       - 标量之间**互相可转**：字符串数字也能取成 int（**整串**都得是数字才认 ✓）；
 *         数字/布尔也能取成字符串（复用 value_to_string 的打印口径 ✓）
 *       - 字典/数组这类容器**不**参与转换（要它们请用 get_obj ✓）
 *   · get_obj(obj, key)：拿原始值继续往下钻（非字典/缺键 ⇒ null）
 *   · keys(obj)：字典的键列表（插入序；非字符串键转文本 ✓）；非字典 ⇒ 空数组
 *   · write_text(path, s)：**原样**写文本、不做 JSON 编码 —— 想写"已经编码好的 JSON 文本"
 *     必须用它：`write_file` 会把入参**再编码一次**，传 "{}" 会落盘成带引号的 `"{}"` ✗
 * ============================================================================ */

// 取 obj[key]；obj 不是字典 / key 不是字符串 / 缺键 ⇒ null
static Value json_pick(Value obj, Value key) {
    if (!val_is_obj(obj) || val_as_obj(obj)->type != OBJ_DICT) return val_null();
    if (!val_is_obj(key) || val_as_obj(key)->type != OBJ_STRING) return val_null();
    ObjDict* d = (ObjDict*)val_as_obj(obj);
    if (!dict_has(d, key)) return val_null();
    return dict_get(d, key);
}

// 标量 → 文本（字符串/整数/浮点/布尔/BigInt）；容器与 null ⇒ NULL
static ObjString* json_scalar_text(Value v) {
    if (val_is_obj(v)) {
        ObjType t = val_as_obj(v)->type;
        if (t == OBJ_STRING) return (ObjString*)val_as_obj(v);
        if (t != OBJ_BIGINT) return NULL;   // 字典/数组等容器不给文本形态（要它们请用 get_obj）
    } else if (!val_is_int(v) && !val_is_float(v) && !val_is_bool(v)) {
        return NULL;                        // null / 其它 ⇒ 无文本形态
    }
    char* s = value_to_string(v);           // 复用统一打印口径（bigint 也覆盖 ✓）
    if (!s) return NULL;
    ObjString* out = str_new(s, (int)strlen(s));
    free(s);
    return out;
}

// 标量 → int64（不抛异常）：字符串要**整串**都是数字才认；溢出/容器 ⇒ 0（调用方回退默认值）
static int json_scalar_int(Value v, int64_t* out) {
    switch (val_get_type(v)) {
        case VAL_INT:
            *out = val_as_int(v);
            return 1;
        case VAL_FLOAT: {
            double d = val_as_num(v);
            // 同 native_to_int 的边界处理：超出 int64 的浮点先挡掉（别做 UB 转换 ✗）
            if (d >= 9.223372036854775e18 || d <= -9.223372036854775e18) return 0;
            *out = (int64_t)d;
            return 1;
        }
        case VAL_BOOL:
            *out = val_as_bool(v) ? 1 : 0;
            return 1;
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                ObjString* s = (ObjString*)val_as_obj(v);
                char* end;
                errno = 0;
                long long n = strtoll(s->chars, &end, 10);
                // `end == s->chars` 挡掉空串；整串都得是数字；ERANGE ⇒ 溢出 int64（回退默认值 ✓）
                if (end == s->chars || *end != '\0' || errno == ERANGE) return 0;
                *out = (int64_t)n;
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

// 标量 → double（同款约定）
static int json_scalar_float(Value v, double* out) {
    switch (val_get_type(v)) {
        case VAL_INT:   *out = (double)val_as_int(v);      return 1;
        case VAL_FLOAT: *out = val_as_num(v);              return 1;
        case VAL_BOOL:  *out = val_as_bool(v) ? 1.0 : 0.0; return 1;
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                ObjString* s = (ObjString*)val_as_obj(v);
                char* end;
                errno = 0;
                double d = strtod(s->chars, &end);
                if (end == s->chars || *end != '\0' || errno == ERANGE) return 0;
                *out = d;
                return 1;
            }
            return 0;
        default:
            return 0;
    }
}

// 标量 → bool（同款约定）：字符串只认 "true"/"false"/"1"/"0"（与 TraeSign 的 _flag 同口径 ✓）
static int json_scalar_bool(Value v, int* out) {
    switch (val_get_type(v)) {
        case VAL_BOOL:  *out = val_as_bool(v);       return 1;
        case VAL_INT:   *out = val_as_int(v) != 0;   return 1;
        case VAL_FLOAT: *out = val_as_num(v) != 0.0; return 1;
        case VAL_OBJ:
            if (val_as_obj(v)->type == OBJ_STRING) {
                ObjString* s = (ObjString*)val_as_obj(v);
                if (s->len == 4 && strncmp(s->chars, "true", 4) == 0)  { *out = 1; return 1; }
                if (s->len == 5 && strncmp(s->chars, "false", 5) == 0) { *out = 0; return 1; }
                if (s->len == 1 && s->chars[0] == '1') { *out = 1; return 1; }
                if (s->len == 1 && s->chars[0] == '0') { *out = 0; return 1; }
            }
            return 0;
        default:
            return 0;
    }
}

// jsons.get_str(obj, key, default)
static Value jsons_get_str_func(int argc, Value* args) {
    Value def = val_obj((Object*)str_new("", 0));
    if (argc >= 3) {
        ObjString* t = json_scalar_text(args[2]);
        if (t) def = val_obj((Object*)t);
    }
    Value v = json_pick(args[0], args[1]);
    if (!val_is_null(v)) {
        ObjString* t = json_scalar_text(v);
        if (t) return val_obj((Object*)t);
    }
    return def;
}

// jsons.get_int(obj, key, default)
static Value jsons_get_int_func(int argc, Value* args) {
    Value def = val_int(0);
    if (argc >= 3) {
        int64_t d;
        if (json_scalar_int(args[2], &d)) def = val_int_safe(d);
    }
    int64_t n;
    Value v = json_pick(args[0], args[1]);
    if (!val_is_null(v) && json_scalar_int(v, &n)) return val_int_safe(n);
    return def;
}

// jsons.get_float(obj, key, default)
static Value jsons_get_float_func(int argc, Value* args) {
    Value def = val_float(0.0);
    if (argc >= 3) {
        double d;
        if (json_scalar_float(args[2], &d)) def = val_float(d);
    }
    double f;
    Value v = json_pick(args[0], args[1]);
    if (!val_is_null(v) && json_scalar_float(v, &f)) return val_float(f);
    return def;
}

// jsons.get_bool(obj, key, default)
static Value jsons_get_bool_func(int argc, Value* args) {
    Value def = val_bool(0);
    if (argc >= 3) {
        int d;
        if (json_scalar_bool(args[2], &d)) def = val_bool(d);
    }
    int b;
    Value v = json_pick(args[0], args[1]);
    if (!val_is_null(v) && json_scalar_bool(v, &b)) return val_bool(b);
    return def;
}

// jsons.get_obj(obj, key)：原始值（继续往下钻用）；非字典/缺键 ⇒ null
static Value jsons_get_obj_func(int argc, Value* args) {
    (void)argc;
    return json_pick(args[0], args[1]);
}

// jsons.keys(obj)：字典键列表（插入序；非字符串键转文本 ✓）；非字典 ⇒ 空数组
static Value jsons_keys_func(int argc, Value* args) {
    (void)argc;
    ObjArray* arr = arr_new(8);
    if (!arr) return val_null();
    Value o = args[0];
    if (val_is_obj(o) && val_as_obj(o)->type == OBJ_DICT) {
        ObjDict* d = (ObjDict*)val_as_obj(o);
        int n = 0;
        for (int i = 0; i < d->order_count; i++) {
            Value k = d->order[i];
            if (val_is_null(k)) continue;
            ObjString* ks = dict_key_to_string(k);
            if (!ks) continue;
            while (n >= arr->capacity) {
                if (!arr_grow(arr)) { arr->count = n; return val_obj((Object*)arr); }
            }
            arr_write(arr, n, val_obj((Object*)ks));
            n++;
        }
        arr->count = n;
    }
    return val_obj((Object*)arr);
}

// jsons.write_text(path, s)：**原样**写文本（不 JSON 编码 ✓）；失败 ⇒ false
static Value jsons_write_text_func(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_STRING) return val_bool(false);
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_STRING) return val_bool(false);
    ObjString* path = (ObjString*)val_as_obj(args[0]);
    ObjString* text = (ObjString*)val_as_obj(args[1]);
    FILE* file = fopen_utf8(path->chars, "wb");
    if (!file) return val_bool(false);
    size_t written = text->len > 0 ? fwrite(text->chars, 1, (size_t)text->len, file) : 0;
    fclose(file);
    return val_bool(written == (size_t)text->len);
}

// Module initialization
void jsons_init_module(void) {
    TypeKind decode_params[] = {TYPE_STRING};
    native_register_module_method("jsons", "decode", jsons_decode_func, &NATIVE_T_ANY, NATIVE_FIXED(decode_params));

    TypeKind encode_params[] = {TYPE_ANY};
    native_register_module_method("jsons", "encode", jsons_encode_func, &NATIVE_T_STRING, NATIVE_FIXED(encode_params));
    native_register_module_method("jsons", "encode_pretty", jsons_encode_pretty_func, &NATIVE_T_STRING, NATIVE_FIXED(encode_params));

    TypeKind read_file_params[] = {TYPE_STRING};
    native_register_module_method("jsons", "read_file", jsons_read_file_func, &NATIVE_T_ANY, NATIVE_FIXED(read_file_params));

    TypeKind write_file_params[] = {TYPE_STRING, TYPE_ANY};
    native_register_module_method("jsons", "write_file", jsons_write_file_func, &NATIVE_T_BOOL, NATIVE_FIXED(write_file_params));

    // ---- T21：类型化取值助手（收窄收在标准库里 ⇒ 调用点零样板 ✓）----
    TypeKind get_str_params[] = {TYPE_ANY, TYPE_STRING, TYPE_STRING};
    native_register_module_method("jsons", "get_str", jsons_get_str_func, &NATIVE_T_STRING, NATIVE_FIXED(get_str_params));

    TypeKind get_int_params[] = {TYPE_ANY, TYPE_STRING, TYPE_INT};
    native_register_module_method("jsons", "get_int", jsons_get_int_func, &NATIVE_T_INT, NATIVE_FIXED(get_int_params));

    TypeKind get_float_params[] = {TYPE_ANY, TYPE_STRING, TYPE_FLOAT};
    native_register_module_method("jsons", "get_float", jsons_get_float_func, &NATIVE_T_FLOAT, NATIVE_FIXED(get_float_params));

    TypeKind get_bool_params[] = {TYPE_ANY, TYPE_STRING, TYPE_BOOL};
    native_register_module_method("jsons", "get_bool", jsons_get_bool_func, &NATIVE_T_BOOL, NATIVE_FIXED(get_bool_params));

    TypeKind get_obj_params[] = {TYPE_ANY, TYPE_STRING};
    native_register_module_method("jsons", "get_obj", jsons_get_obj_func, &NATIVE_T_ANY, NATIVE_FIXED(get_obj_params));

    TypeKind keys_params[] = {TYPE_ANY};
    native_register_module_method("jsons", "keys", jsons_keys_func, &NATIVE_T_ARR_STRING, NATIVE_FIXED(keys_params));

    TypeKind write_text_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("jsons", "write_text", jsons_write_text_func, &NATIVE_T_BOOL, NATIVE_FIXED(write_text_params));
}
