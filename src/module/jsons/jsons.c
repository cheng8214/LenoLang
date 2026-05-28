#include "include/native.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>

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
    snprintf(buf, sizeof(buf), "%lld", num);
    sb_append_cstr(sb, buf);
}

static void sb_append_float(StringBuilder* sb, double num) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", num);
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
                case 'u':
                    if (i + 6 <= token->len) {  // \\uXXXX = 6 chars
                        char hex[5];
                        strncpy(hex, token->start + i + 2, 4);
                        hex[4] = '\0';
                        int code = (int)strtol(hex, NULL, 16);
                        if (code < 128) {
                            decoded[j++] = (char)code;
                        }
                        i += 5;  // skip \\uXXXX (will i++ in for loop)
                    }
                    break;
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
        dict_set(dict, key, value);
        
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

static void json_encode_string(StringBuilder* sb, const char* str) {
    sb_append_char(sb, '"');
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '"': sb_append_cstr(sb, "\\\""); break;
            case '\\': sb_append_cstr(sb, "\\\\"); break;
            case '\b': sb_append_cstr(sb, "\\b"); break;
            case '\f': sb_append_cstr(sb, "\\f"); break;
            case '\n': sb_append_cstr(sb, "\\n"); break;
            case '\r': sb_append_cstr(sb, "\\r"); break;
            case '\t': sb_append_cstr(sb, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)*p);
                    sb_append_cstr(sb, buf);
                } else {
                    sb_append_char(sb, *p);
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
        ObjString* key = dict->order[i];
        if (!key) continue;
        
        Value value = dict_get(dict, key);
        
        if (pretty) {
            json_encode_indent(sb, indent + 2);
        }
        
        json_encode_string(sb, key->chars);
        
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
                    json_encode_string(sb, ((ObjString*)val_as_obj(value))->chars);
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

// jsons.read_file
static Value jsons_read_file_func(int argc, Value* args) {
    (void)argc;
    ObjString* path = (ObjString*)val_as_obj(args[0]);

    // Use binary mode to get accurate file size
    FILE* file = fopen(path->chars, "rb");
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
    FILE* file = fopen(path->chars, "wb");
    if (!file) {
        sb_free(&sb);
        return val_bool(false);
    }
    
    fwrite(sb.data, 1, sb.len, file);
    fclose(file);
    
    sb_free(&sb);
    
    return val_bool(true);
}

// Module initialization
void jsons_init_module(void) {
    TypeKind decode_params[] = {TYPE_STRING};
    native_register_module_method("jsons", "decode", jsons_decode_func, 1, -1, -1, TYPE_ANY, decode_params);

    TypeKind encode_params[] = {TYPE_ANY};
    native_register_module_method("jsons", "encode", jsons_encode_func, 1, -1, -1, TYPE_STRING, encode_params);
    native_register_module_method("jsons", "encode_pretty", jsons_encode_pretty_func, 1, -1, -1, TYPE_STRING, encode_params);

    TypeKind read_file_params[] = {TYPE_STRING};
    native_register_module_method("jsons", "read_file", jsons_read_file_func, 1, -1, -1, TYPE_ANY, read_file_params);

    TypeKind write_file_params[] = {TYPE_STRING, TYPE_ANY};
    native_register_module_method("jsons", "write_file", jsons_write_file_func, 2, -1, -1, TYPE_BOOL, write_file_params);
}
