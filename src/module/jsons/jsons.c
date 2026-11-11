#include "include/native.h"
#include "include/platform.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <inttypes.h>
#include <errno.h>
#include <math.h>

// 前向声明
extern ObjString* dict_key_to_string(Value key);

// ==================== 嵌套深度上限（2026-11-09，P1 压测发现）====================
// 为什么必须有：`decode` / `encode` 都是**递归**的，而递归深度直接等于数据嵌套深度。
// 实测（本机 1MB 栈）：12000 层正常，**20000 层栈溢出崩溃**（0xC00000FD，
// 属于未处理系统异常，**try/catch 接不住**，整个进程直接死）。
//
// 两个都真实可触发：
//   · `jsons.decode(body)` —— body 来自 HTTP 请求体 / DB 内容 / 文件，**不可信**。
//     一个 `[[[[...` 就能把服务端打崩（Aria 就有这种入口）。
//   · `jsons.encode(node)` —— struct 可以**自引用**（`Node? next = null`），
//     一旦构造成环，编码就是无限递归。
//
// 512 层：真实 JSON 极少超过 20 层，留了 23 倍安全余量（12000 才崩）。
// 超限一律**明确报错**，不静默截断 —— 静默截断正是本项目反复在修的那类问题。
#define JSON_MAX_DEPTH 512

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
    // ★ 2026-10-10（B 组第 11 条）：**整数文本按 int 解析**，不再一律过 double ✗
    //   `is_integer`：文本里**没有** '.' / 'e' / 'E' ✓（即 JSON 意义上的整数 ✓）；
    //   `int_overflow`：是整数文本但**超出 int64** ⇒ 退回 double（见下面的说明 ✓）；
    //   `int_value`：`is_integer && !int_overflow` 时有效 ✓
    int is_integer;
    int int_overflow;
    int64_t int_value;
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

    // ★ 2026-11-11：数字里必须**至少有一位数字** —— 否则 `-`（`-inf` 的前缀）这类
    //   非法 JSON 会被 strtod 静默读成 0.0（实测 `decode("-inf")` ⇒ 0.0，
    //   而 `decode("inf")` ⇒ null；同一个非法输入给出两种结果）。现在一律
    //   JSON_TOKEN_ERROR ⇒ decode 走"解析失败 ⇒ null"这条既有契约。
    int has_digit = 0;
    for (int i = 0; i < token.len; i++) {
        if (isdigit((unsigned char)token.start[i])) { has_digit = 1; break; }
    }
    if (!has_digit) {
        token.type = JSON_TOKEN_ERROR;
        token.len = 0;
        return token;
    }

    char* num_str = malloc(token.len + 1);
    strncpy(num_str, token.start, token.len);
    num_str[token.len] = '\0';
    token.num_value = strtod(num_str, NULL);

    // ★ 2026-10-10（B 组第 11 条）：**整数文本走精确的 int64 路径** ✓
    //   为什么必须补：原先解析器靠 `num == (int64_t)num` 判"是不是整数" ✗ —— 那条对
    //   **超出 2^53** 的值根本不成立 ✓（double 存不下 ⇒ 比较必然错/做溢出转换 ✗），
    //   实测 `jsons.decode("12345678901234567890")` ⇒ 精度丢失 ✗。而现实里最需要它的
    //   正是**雪花 ID / 大时间戳**这类值 ✓（它们 > 2^53 但**在 int64 内** ✓ ⇒ 现在能精确 ✓）。
    //   ⚠ 超出 int64 的整数文本（>19 位）仍然退回 double ✓ —— 想连这个也精确得引
    //     bigint 的"从十进制串构造"入口 ✓ **本轮不做**（先把最常见的区间修对 ✓ 文档写明 ✓）。
    token.is_integer = 1;
    token.int_overflow = 0;
    token.int_value = 0;
    for (int i = 0; i < token.len; i++) {
        if (num_str[i] == '.' || num_str[i] == 'e' || num_str[i] == 'E') {
            token.is_integer = 0;
            break;
        }
    }
    if (token.is_integer) {
        char* end = NULL;
        errno = 0;
        long long n = strtoll(num_str, &end, 10);
        // 整串都吃掉了、且没溢出 ⇒ 用精确值 ✓；否则标记溢出（调用方退回 double ✓）
        if (end && *end == '\0' && errno != ERANGE) {
            token.int_value = (int64_t)n;
        } else {
            token.int_overflow = 1;
        }
    }
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
        return (JsonToken){JSON_TOKEN_EOF, NULL, 0, 0, 0, 0, 0};
    }
    
    char c = json_lexer_peek(lexer);
    
    switch (c) {
        case '{': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_LBRACE, NULL, 0, 0, 0, 0, 0};
        case '}': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_RBRACE, NULL, 0, 0, 0, 0, 0};
        case '[': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_LBRACKET, NULL, 0, 0, 0, 0, 0};
        case ']': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_RBRACKET, NULL, 0, 0, 0, 0, 0};
        case ':': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_COLON, NULL, 0, 0, 0, 0, 0};
        case ',': json_lexer_advance(lexer); return (JsonToken){JSON_TOKEN_COMMA, NULL, 0, 0, 0, 0, 0};
        case '"': return json_lexer_read_string(lexer);
        default:
            if (isdigit(c) || c == '-') {
                return json_lexer_read_number(lexer);
            } else if (isalpha(c)) {
                return json_lexer_read_identifier(lexer);
            } else {
                json_lexer_advance(lexer);
                return (JsonToken){JSON_TOKEN_ERROR, NULL, 0, 0, 0, 0, 0};
            }
    }
}

// JSON Parser

typedef struct {
    JsonLexer lexer;
    JsonToken current;
    bool has_error;
    int depth;                     // 当前嵌套深度（见文件头 JSON_MAX_DEPTH）
} JsonParser;

static void json_parser_init(JsonParser* parser, const char* input) {
    json_lexer_init(&parser->lexer, input);
    parser->current = json_lexer_next_token(&parser->lexer);
    parser->has_error = false;
    parser->depth = 0;
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
            // ★ 2026-10-10（B 组第 11 条）：整数文本用**精确的 int64** ✓（超出 int48 会自动
            //   升 bigint ✓）；浮点 / 超 int64 的整数文本照旧走 double ✓。
            //   ⚠ 必须用 `val_int_safe` 而不是 `val_int` ✗：后者是 NaN-boxing 的 **int48** ✗
            //     ⇒ 雪花 ID（~1e18）会被**截断** ✗；而原来那句 `num == (int64_t)num`
            //     对 >2^53 的值**连比较都不成立** ✓（精度早就丢了 ✓ 实测过 ✓）。
            //   ⚠ 这里**先取字段再 advance**（advance 会把 current 换掉 ✓）
            int is_int = parser->current.is_integer && !parser->current.int_overflow;
            int64_t iv = parser->current.int_value;
            double num = parser->current.num_value;
            json_parser_advance(parser);
            if (is_int) {
                return val_int_safe(iv);
            }
            return val_float(num);
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
        // ⚠ 嵌套深度保护：不加上限时，`[[[[...` 会在 ~20000 层时**栈溢出崩溃**
        //   （0xC00000FD，try/catch 接不住）。这里是唯一的递归入口 ⇒ 在这一处计数即可。
        case JSON_TOKEN_LBRACE:
        case JSON_TOKEN_LBRACKET: {
            if (parser->depth >= JSON_MAX_DEPTH) {
                parser->has_error = true;
                return val_null();
            }
            parser->depth++;
            Value nested = (parser->current.type == JSON_TOKEN_LBRACE)
                         ? json_parse_object(parser)
                         : json_parse_array(parser);
            parser->depth--;
            return nested;
        }
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

// ==================== 记录类型：JSON → struct（P1，2026-11-09）====================
// 为什么做成 `fill(实例, json)` 而不是 `decode(Song, json)`：
//   · 调用点保持静态类型 —— `Song s = new Song(); jsons.fill(s, t)`，不需要 `as Song` 强转；
//   · 不吃"按名字找类型"的全局表，同名 struct 跨模块也不会有歧义。

static void json_fill_struct(ObjStruct* target, ObjDict* src);

// 严格解析整串（允许前后空白）。成功返回 1。
//   ⚠ 为什么需要它：**SQLite / CSV 的每一列都是字符串**（Aria 的 `db.all` 用
//     `column_text` 取值），不让字符串转数字的话 `fill` 在 DB 场景完全失效 ——
//     而 DB 行恰恰是"行数组"最大的来源。
static int json_parse_int_text(const char* s, int len, Value* out) {
    if (!s || len <= 0) return 0;
    int b = 0, e = len;
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    if (b >= e) return 0;

    // 全部是数字（可带一个前导 +/-）⇒ 可以按整数解析
    int i = b;
    if (s[i] == '+' || s[i] == '-') i++;
    if (i >= e) return 0;
    for (int k = i; k < e; k++) {
        if (s[k] < '0' || s[k] > '9') return 0;
    }

    if (e - b < 20) {                       // 一定装得进 int64
        char buf[32];
        int n = e - b < (int)sizeof(buf) - 1 ? e - b : (int)sizeof(buf) - 1;
        memcpy(buf, s + b, (size_t)n);
        buf[n] = '\0';
        *out = val_int((int64_t)strtoll(buf, NULL, 10));
        return 1;
    }
    // 超长 ⇒ 走 bigint（大 id 逐位保留，别退化成 double）
    char* tmp = (char*)malloc((size_t)(e - b) + 1);
    if (!tmp) return 0;
    memcpy(tmp, s + b, (size_t)(e - b));
    tmp[e - b] = '\0';
    *out = val_bigint_from_string(tmp);
    free(tmp);
    return 1;
}

static int json_parse_float_text(const char* s, int len, double* out) {
    if (!s || len <= 0 || len >= 64) return 0;
    char buf[64];
    memcpy(buf, s, (size_t)len);
    buf[len] = '\0';
    char* end = NULL;
    double d = strtod(buf, &end);
    if (end == buf) return 0;
    while (*end == ' ' || *end == '\t') end++;
    if (*end != '\0') return 0;             // 后面还有杂字符 ⇒ 不算数字
    *out = d;
    return 1;
}

// ★ 2026-11-11：按**数组元素声明类型**把 JSON 值转成该元素可接受的值（口径同标量字段）。
//   转不了 ⇒ val_null()（读的时候会当场报错，而不是按错误类型读出垃圾）。
static Value json_coerce_to_elem(Value raw, TypeKind elem_type) {
    if (val_is_null(raw)) return raw;
    switch (elem_type) {
        case TYPE_INT: {
            if (val_is_int(raw) || val_is_bigint(raw)) return raw;
            if (val_is_bool(raw)) return val_int(val_as_bool(raw) ? 1 : 0);
            if (val_is_float(raw)) return val_int((int64_t)val_as_num(raw));
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                Value iv = val_null();
                return json_parse_int_text(t->chars, t->len, &iv) ? iv : val_null();
            }
            return val_null();
        }
        case TYPE_FLOAT: {
            if (val_is_float(raw) || val_is_int(raw)) return val_float((double)val_as_num(raw));
            if (val_is_bool(raw)) return val_float(val_as_bool(raw) ? 1.0 : 0.0);
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                double dv = 0.0;
                return json_parse_float_text(t->chars, t->len, &dv) ? val_float(dv) : val_null();
            }
            return val_null();
        }
        case TYPE_BOOL: {
            if (val_is_bool(raw)) return raw;
            if (val_is_int(raw)) return val_bool(val_as_int(raw) != 0);
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                if (t->len == 1 && (t->chars[0] == '1' || t->chars[0] == '0'))
                    return val_bool(t->chars[0] == '1');
                if (t->len == 4 && memcmp(t->chars, "true", 4) == 0) return val_bool(true);
                if (t->len == 5 && memcmp(t->chars, "false", 5) == 0) return val_bool(false);
            }
            return val_null();
        }
        case TYPE_STRING:
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) return raw;
            return val_null();
        case TYPE_DICT:
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_DICT) return raw;
            return val_null();
        case TYPE_ARRAY:
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_ARRAY) return raw;
            return val_null();
        default:
            return raw;   // 未记录/其它元素类型 ⇒ 保持既有行为（原样）
    }
}

// 按字段**声明类型**把 JSON 值转成该字段可接受的值。
//   转不了 ⇒ val_null()（调用方保留字段默认值，而不是塞个错类型进去）。
static Value json_coerce_to_field(Value raw, StructFieldInfo* f) {
    if (val_is_null(raw) || !f) return val_null();

    switch (f->type) {
        case TYPE_INT: {
            // 大整数原样保留（转 double 会丢精度，见 encode 那边 bigint 的同类注释）
            if (val_is_int(raw) || val_is_bigint(raw)) return raw;
            if (val_is_bool(raw)) return val_int(val_as_bool(raw) ? 1 : 0);
            if (val_is_float(raw)) return val_int((int64_t)val_as_num(raw));
            // 字符串 ⇒ 严格解析（DB / CSV 的列都是字符串）
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                Value iv = val_null();
                return json_parse_int_text(t->chars, t->len, &iv) ? iv : val_null();
            }
            return val_null();
        }
        case TYPE_FLOAT: {
            if (val_is_float(raw) || val_is_int(raw)) return val_float((double)val_as_num(raw));
            if (val_is_bool(raw)) return val_float(val_as_bool(raw) ? 1.0 : 0.0);
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                double dv = 0.0;
                return json_parse_float_text(t->chars, t->len, &dv) ? val_float(dv) : val_null();
            }
            return val_null();
        }
        case TYPE_BOOL: {
            if (val_is_bool(raw)) return raw;
            if (val_is_int(raw)) return val_bool(val_as_int(raw) != 0);
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) {
                ObjString* t = (ObjString*)val_as_obj(raw);
                if (t->len == 1 && (t->chars[0] == '1' || t->chars[0] == '0'))
                    return val_bool(t->chars[0] == '1');
                if (t->len == 4 && memcmp(t->chars, "true", 4) == 0) return val_bool(true);
                if (t->len == 5 && memcmp(t->chars, "false", 5) == 0) return val_bool(false);
            }
            return val_null();
        }
        case TYPE_STRING:
            // **不做隐式 _str**：`{"id": 123}` 填 `string id` 时静默变 "123" 会把类型错误藏起来。
            //   （数字→字符串没有"解析"这回事，方向反过来才有意义。）
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_STRING) return raw;
            return val_null();
        case TYPE_ARRAY: {
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_ARRAY) {
                // 元素不是 struct（`Array[int]` / `Array[string]` / `Array[Dict]` …）
                if (!f->element_struct_name || f->element_type != TYPE_STRUCT) {
                    // ★ 2026-11-11：`Array[int]` 这类**标量元素数组**必须逐元素按声明类型转换。
                    //   此前这里直接"原样赋" ⇒ `{"tags":[1,"2"]}` 填 `Array[int]` 后数组里
                    //   真的躺着一个 **string**（静态类型说 int）⇒ 之后 `tags[0]+tags[1]`
                    //   按 int48 读字符串指针 ⇒ 静默错值（实测 1391279286993 ✗）。
                    //   口径与标量字段完全相同（字符串能解析成数字 ⇒ 转；转不了 ⇒ null）。
                    //   `Array[any]` / 裸 `Array`（element_type 未记录）保持原样。
                    TypeKind et = f->element_type;
                    if (et == TYPE_UNKNOWN || et == TYPE_INFER || et == TYPE_ANY ||
                        et == TYPE_PTR || et == TYPE_NULL) {
                        return raw;
                    }
                    ObjArray* srcArr = (ObjArray*)val_as_obj(raw);
                    int need = 0;
                    for (int i = 0; i < srcArr->count; i++) {
                        Value c = json_coerce_to_elem(srcArr->elements[i], et);
                        if (c != srcArr->elements[i]) { need = 1; break; }
                    }
                    if (!need) return raw;   // 类型本来就对 ⇒ 不换对象（保持既有身份语义）

                    ObjArray* outArr = arr_new(srcArr->count > 0 ? srcArr->count : 1);
                    if (!outArr) return val_null();
                    Value outVal = val_obj((Object*)outArr);
                    gc_push_root(&outVal);
                    int n = 0;
                    for (int i = 0; i < srcArr->count; i++) {
                        Value c = json_coerce_to_elem(srcArr->elements[i], et);
                        gc_push_root(&c);
                        while (n >= outArr->capacity) {
                            if (!arr_grow(outArr)) break;
                        }
                        if (n < outArr->capacity) arr_write(outArr, n++, c);
                        gc_pop_root();
                    }
                    outArr->count = n;
                    gc_pop_root();
                    return outVal;
                }

                // `Array[Item]`：逐元素造 struct 再递归填。
                //   ★ 2026-11-09：`element_struct_name` 是**新加的**字段元数据（此前运行时
                //   完全没有数组元素类型，实测读出来是 TYPE_PTR 默认值 ⇒ 只能塞 Array[Dict]，
                //   字段声明类型被静默破坏）。数据来源：AST → 字节码（OP_STRUCT_DEF 元数据）
                //   → StructFieldInfo。改了字节码布局 ⇒ 缓存版本已同步 bump。
                ObjArray* srcArr = (ObjArray*)val_as_obj(raw);
                ObjArray* outArr = arr_new(srcArr->count > 0 ? srcArr->count : 1);
                if (!outArr) return val_null();

                Value outVal = val_obj((Object*)outArr);
                gc_push_root(&outVal);

                int n = 0;
                for (int i = 0; i < srcArr->count; i++) {
                    Value el = srcArr->elements[i];
                    int written = 0;

                    if (val_is_obj(el) && val_as_obj(el)->type == OBJ_DICT) {
                        // ⚠ 先扩容量再建元素：`arr_grow` 会分配，若元素还没挂进数组
                        //   又没被 root 住，会被 GC 回收（use-after-free）。
                        while (n >= outArr->capacity) {
                            if (!arr_grow(outArr)) break;
                        }
                        if (n < outArr->capacity) {
                            ObjStruct* it = native_struct_new(f->element_struct_name);
                            if (it) {
                                Value item = val_obj((Object*)it);
                                gc_push_root(&item);
                                json_fill_struct(it, (ObjDict*)val_as_obj(el));
                                arr_write(outArr, n, item);
                                gc_pop_root();
                                n++;
                                written = 1;
                            }
                        }
                    }

                    if (!written) {
                        // 不是对象 / 造不出来 ⇒ 原样放进去（宁可留个 Dict，也不静默丢数据）
                        while (n >= outArr->capacity) {
                            if (!arr_grow(outArr)) break;
                        }
                        if (n < outArr->capacity) {
                            arr_write(outArr, n, el);
                            n++;
                        }
                    }
                }
                outArr->count = n;
                gc_pop_root();
                return outVal;
            }
            return val_null();
        }
        case TYPE_DICT:
            if (val_is_obj(raw) && val_as_obj(raw)->type == OBJ_DICT) return raw;
            return val_null();
        case TYPE_STRUCT: {
            if (!f->struct_type_name) return val_null();
            if (!val_is_obj(raw) || val_as_obj(raw)->type != OBJ_DICT) return val_null();
            ObjStruct* nested = native_struct_new(f->struct_type_name);
            if (!nested) return val_null();
            Value nv = val_obj((Object*)nested);
            gc_push_root(&nv);                  // 递归里还会 gc_alloc
            json_fill_struct(nested, (ObjDict*)val_as_obj(raw));
            gc_pop_root();
            return nv;
        }
        default:
            return raw;                          // TYPE_ANY / 未标注类型：原样赋
    }
}

// ★ 2026-11-10（P4 enum 载荷 / fill 往返）：`{"成员":{"参数":值,…}}` 填回 `tag + 载荷槽`。
//   与 `json_encode_enum_payload` 共用同一份 `enum_sig`（`a(x);b(s)`），保证编码/解码对称。
//   ⚠ 此前 fill 走通用路径：顶层键（成员名）**一个都匹配不上隐藏字段** ⇒ 静默得到
//     `Msg{tag=null,p0=null,…}`（实测），既不报错也不是默认值，属于"静默错值"。
//   找不到匹配的成员键 ⇒ 返回 0，让调用方退回通用"按字段名填"（兼容 `{"tag":1,"p0":3}` 这类老 JSON）。
static int json_fill_enum_payload(ObjStruct* target, ObjDict* src) {
    if (!target || !target->def || !src) return 0;
    ObjStructDef* def = target->def;
    if (!def->enum_sig || def->field_count < 1) return 0;

    // 取 src 里第一个"值是对象"的键（encode 只会写一个成员键）
    const char* key = NULL;
    int key_len = 0;
    ObjDict* inner = NULL;
    for (int i = 0; i < src->order_count; i++) {
        Value k = src->order[i];
        if (!val_is_obj(k) || val_as_obj(k)->type != OBJ_STRING) continue;
        Value v = dict_get(src, k);
        if (!val_is_obj(v) || val_as_obj(v)->type != OBJ_DICT) continue;
        ObjString* ks = (ObjString*)val_as_obj(k);
        key = ks->chars;
        key_len = ks->len;
        inner = (ObjDict*)val_as_obj(v);
        break;
    }
    if (!key || !inner) return 0;

    // 在签名里定位同名成员，同时累计载荷基址（与 encode 同一个公式）
    const char* p = def->enum_sig;
    int idx = 0;
    int base = 0;
    for (;;) {
        if (!p || !*p) return 0;
        const char* nend = strchr(p, '(');
        const char* semi = strchr(p, ';');
        if (!nend || (semi && semi < nend)) nend = semi;
        if (!nend) nend = p + strlen(p);

        int pc = 0;
        const char* ps = NULL;
        const char* pe = NULL;
        if (*nend == '(') {
            pe = strchr(nend, ')');
            if (!pe) return 0;
            ps = nend + 1;
            pc = (pe > ps) ? 1 : 0;
            for (const char* q = ps; q < pe; q++) {
                if (*q == ',') pc++;
            }
        }

        int name_len = (int)(nend - p);
        if (name_len == key_len && memcmp(p, key, (size_t)key_len) == 0) {
            // ① tag
            struct_set_field(target, 0, val_int(idx));
            // ② 该成员的载荷槽：按**参数名**去内层对象里找
            int avail = def->field_count - 1 - base;
            if (pc > avail) pc = avail > 0 ? avail : 0;
            int k = 0;
            const char* q = ps;
            while (q && pe && q < pe && k < pc) {
                const char* comma = memchr(q, ',', (size_t)(pe - q));
                const char* name_end = comma ? comma : pe;
                int nlen = (int)(name_end - q);
                for (int i = 0; i < inner->order_count; i++) {
                    Value ik = inner->order[i];
                    if (!val_is_obj(ik) || val_as_obj(ik)->type != OBJ_STRING) continue;
                    ObjString* iks = (ObjString*)val_as_obj(ik);
                    if ((int)iks->len != nlen || memcmp(iks->chars, q, (size_t)nlen) != 0) continue;
                    Value conv = json_coerce_to_field(dict_get(inner, ik),
                                                      &def->fields[1 + base + k]);
                    if (!val_is_null(conv)) struct_set_field(target, 1 + base + k, conv);
                    break;
                }
                k++;
                if (!comma) break;
                q = comma + 1;
            }
            return 1;
        }

        if (!semi) return 0;
        base += pc;
        idx++;
        p = strchr(nend, ';');
        if (!p) return 0;
        p++;
    }
}

static void json_fill_struct(ObjStruct* target, ObjDict* src) {
    if (!target || !target->def || !src) return;
    ObjStructDef* def = target->def;

    // ★ 带载荷 enum：先按「成员 + 参数名」解码（形状对上了就直接返回）
    if (def->enum_sig && json_fill_enum_payload(target, src)) return;

    for (int i = 0; i < src->order_count; i++) {
        Value k = src->order[i];
        if (!val_is_obj(k) || val_as_obj(k)->type != OBJ_STRING) continue;
        ObjString* ks = (ObjString*)val_as_obj(k);

        // 按字段名定位（字段数通常十几，线性比建哈希划算）
        int fi = -1;
        for (int j = 0; j < def->field_count; j++) {
            const char* fn = def->fields[j].name;
            if (fn && (int)strlen(fn) == ks->len && memcmp(fn, ks->chars, (size_t)ks->len) == 0) {
                fi = j;
                break;
            }
        }
        if (fi < 0) continue;                    // 未知字段：忽略（向前兼容）

        Value conv = json_coerce_to_field(dict_get(src, k), &def->fields[fi]);
        if (val_is_null(conv)) continue;         // 转不了：保留字段默认值
        struct_set_field(target, fi, conv);      // 带 GC 写屏障
    }
}

// jsons.fill(target, json)：把 JSON 对象填进**已有的** struct 实例；返回是否成功
static Value jsons_fill_func(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_STRUCT) {
        native_throw_error("fill() 第一个参数必须是 struct 实例");
        return val_null();
    }
    ObjStruct* target = (ObjStruct*)val_as_obj(args[0]);

    // 第二个参数：JSON 字符串 **或** 已解析好的 Dict
    //   （DB 行 / CSV 行本来就已经是 Dict，再拼成字符串再解析一遍是浪费）
    if (val_is_obj(args[1]) && val_as_obj(args[1])->type == OBJ_DICT) {
        json_fill_struct(target, (ObjDict*)val_as_obj(args[1]));
        return val_bool(true);
    }
    if (!val_is_obj(args[1]) || val_as_obj(args[1])->type != OBJ_STRING) {
        native_throw_error("fill() 第二个参数必须是 JSON 字符串或 Dict");
        return val_null();
    }

    ObjString* text = (ObjString*)val_as_obj(args[1]);

    JsonParser parser;
    json_parser_init(&parser, text->chars);
    Value parsed = json_parse_value(&parser);

    if (parser.has_error) return val_bool(false);
    if (!val_is_obj(parsed) || val_as_obj(parsed)->type != OBJ_DICT) return val_bool(false);

    gc_push_root(&parsed);
    json_fill_struct(target, (ObjDict*)val_as_obj(parsed));
    gc_pop_root();
    return val_bool(true);
}

// JSON Encoder

// `depth` = 当前嵌套层数（超 JSON_MAX_DEPTH 就置 *err 并停止递归）；
// `err`   = 错误标志：0 正常 / 1 深度溢出 / 2 浮点非有限（NaN、±Infinity），
//           由顶层统一检查后抛错（2 是 2026-11-11 加的，见 VAL_FLOAT 分支）。
//   ⚠ 不能在这里直接 `native_throw_error` —— 它**不跳转**（只登记异常，见 native.h:446），
//     调用后递归会继续走下去 ⇒ 照样爆栈。
static void json_encode_value(StringBuilder* sb, Value value, int indent, bool pretty,
                              int depth, int* err);

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

static void json_encode_array(StringBuilder* sb, ObjArray* arr, int indent, bool pretty,
                              int depth, int* err) {
    sb_append_char(sb, '[');
    
    if (pretty && arr->count > 0) {
        sb_append_char(sb, '\n');
    }
    
    for (int i = 0; i < arr->count; i++) {
        if (pretty) {
            json_encode_indent(sb, indent + 2);
        }
        
        json_encode_value(sb, arr->elements[i], indent + 2, pretty, depth + 1, err);
        
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

static void json_encode_dict(StringBuilder* sb, ObjDict* dict, int indent, bool pretty,
                             int depth, int* err) {
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
        
        json_encode_value(sb, value, indent + 2, pretty, depth + 1, err);
        
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

// ★ 2026-11-10（P4 enum 载荷）：带载荷 enum 的降级 struct → `{"progress":{"done":3,"total":10}}`
//   为什么不能直接用下面的通用实现：那里会输出隐藏字段 `{"tag":0,"p0":3,"p1":10}` ——
//   用户看到的是编译器实现细节，而且换个成员就整串变样 ✗
//   成员/参数名来自 `def->enum_sig`（`progress(done,total);finished(path)`，见 leno_ast.h）。
//   签名缺失/解析不出来 ⇒ 返回 0，让调用方退回通用实现（宁可多暴露字段，也不要输出坏 JSON）。
static int json_encode_enum_payload(StringBuilder* sb, ObjStruct* st, int indent, bool pretty,
                                    int depth, int* err) {
    ObjStructDef* def = st->def;
    if (!def || !def->enum_sig || def->field_count < 1) return 0;

    Value tagv = st->field_values[0];
    if (val_get_type(tagv) != VAL_INT) return 0;
    int tag = (int)val_as_num(tagv);
    if (tag < 0) return 0;

    // ── 走到第 tag 个成员，同时累计载荷基址 ────────────────────────────────
    const char* p = def->enum_sig;
    int idx = 0, base = 0;
    const char* mname = NULL;
    int mname_len = 0;
    int pcount = 0;
    for (;;) {
        if (!p || !*p) return 0;                  // 签名比 tag 短 ⇒ 对不上
        const char* nend = strchr(p, '(');
        const char* semi = strchr(p, ';');
        if (!nend || (semi && semi < nend)) nend = semi;
        if (!nend) nend = p + strlen(p);
        int pc = 0;
        const char* pstart = NULL;
        if (*nend == '(') {
            const char* pend = strchr(nend, ')');
            if (!pend) return 0;
            pstart = nend + 1;
            // 数逗号 + 1（空括号 = 0 个参数）
            pc = (pend > pstart) ? 1 : 0;
            for (const char* q = pstart; q < pend; q++) {
                if (*q == ',') pc++;
            }
            semi = strchr(pend, ';');
        }
        if (idx == tag) {
            mname = p;
            mname_len = (int)(nend - p);
            pcount = pc;
            break;
        }
        if (!semi) return 0;                     // 没有更多成员了
        base += pc;
        idx++;
        p = semi + 1;
    }

    // ── 输出 `{"成员":{ "参数": 值 … }}` ───────────────────────────────────
    int avail = def->field_count - 1 - base;
    if (pcount > avail) pcount = avail > 0 ? avail : 0;

    sb_append_char(sb, '{');
    if (pretty) sb_append_char(sb, '\n');
    if (pretty) json_encode_indent(sb, indent + 2);
    json_encode_string(sb, mname, mname_len);
    sb_append_char(sb, ':');
    if (pretty) sb_append_char(sb, ' ');
    sb_append_char(sb, '{');
    if (pretty && pcount > 0) sb_append_char(sb, '\n');

    // 参数名：与成员名同一段签名里，`name(a,b)` 的括号内
    const char* q = strchr(mname, '(');
    const char* qend = q ? strchr(q, ')') : NULL;
    const char* pn = q ? q + 1 : NULL;
    for (int k = 0; k < pcount; k++) {
        const char* pn_end = NULL;
        if (pn && qend && pn <= qend) {
            const char* comma = pn;
            while (comma < qend && *comma != ',') comma++;
            pn_end = comma;
        }
        if (pretty) json_encode_indent(sb, indent + 4);
        if (pn && pn_end) {
            json_encode_string(sb, pn, (int)(pn_end - pn));
        } else {
            char fb[16];
            snprintf(fb, sizeof(fb), "p%d", base + k);
            json_encode_string(sb, fb, (int)strlen(fb));
        }
        sb_append_char(sb, ':');
        if (pretty) sb_append_char(sb, ' ');
        json_encode_value(sb, st->field_values[1 + base + k], indent + 4, pretty, depth + 1, err);
        if (k + 1 < pcount) sb_append_char(sb, ',');
        if (pretty) sb_append_char(sb, '\n');
        if (pn && qend && pn_end && pn_end < qend) {
            pn = pn_end + 1;
        } else {
            pn = NULL;
        }
    }

    if (pretty && pcount > 0) json_encode_indent(sb, indent + 2);
    sb_append_char(sb, '}');
    if (pretty) sb_append_char(sb, '\n');
    if (pretty) json_encode_indent(sb, indent);
    sb_append_char(sb, '}');
    return 1;
}

// ★ 2026-11-09（P1 记录类型）：struct 实例 → JSON 对象
//   字段名取 `def->fields[i].name`，值取 `st->field_values[i]`（字段顺序 = 声明顺序）。
//   嵌套 struct / struct 数组靠 `json_encode_value` 递归 ⇒ 自动支持。
static void json_encode_struct(StringBuilder* sb, ObjStruct* st, int indent, bool pretty,
                               int depth, int* err) {
    if (!st || !st->def) {
        sb_append_cstr(sb, "null");
        return;
    }
    ObjStructDef* def = st->def;
    // 带载荷 enum：先按成员式输出（见 json_encode_enum_payload 的说明）
    if (def->enum_sig && json_encode_enum_payload(sb, st, indent, pretty, depth, err)) return;
    int n = def->field_count;

    sb_append_char(sb, '{');
    if (pretty && n > 0) sb_append_char(sb, '\n');

    for (int i = 0; i < n; i++) {
        const char* fname = def->fields[i].name;
        if (!fname) fname = "";

        if (pretty) json_encode_indent(sb, indent + 2);
        json_encode_string(sb, fname, (int)strlen(fname));
        sb_append_char(sb, ':');
        if (pretty) sb_append_char(sb, ' ');
        json_encode_value(sb, st->field_values[i], indent + 2, pretty, depth + 1, err);

        if (i + 1 < n) sb_append_char(sb, ',');
        if (pretty) sb_append_char(sb, '\n');
    }

    if (pretty && n > 0) json_encode_indent(sb, indent);
    sb_append_char(sb, '}');
}

static void json_encode_value(StringBuilder* sb, Value value, int indent, bool pretty,
                              int depth, int* err) {
    // 深度保护：struct 可以自引用（`Node? next = null`），成环时这里是无限递归。
    //   实测未加保护时 `a.next = b; b.next = a; encode(a)` ⇒ 栈溢出崩溃（0xC00000FD）。
    if (depth > JSON_MAX_DEPTH) {
        *err = 1;
        sb_append_cstr(sb, "null");          // 占位；顶层会据此抛错，不会把它交出去
        return;
    }
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
        case VAL_FLOAT: {
            // ★ 2026-11-11：NaN / ±Infinity **不是合法 JSON**。此前直接走
            //   val_format_float ⇒ 输出裸 `nan` / `inf` / `-inf`（实测），
            //   连本库自己的 decode 都读不回来（inf ⇒ null、-inf ⇒ 0.0）——
            //   又是"静默交出坏数据"。这里置 2，由顶层抛错（与深度溢出同一收尾路径）。
            double num = val_as_num(value);
            if (isnan(num) || isinf(num)) {
                *err = 2;
                sb_append_cstr(sb, "null");   // 占位；顶层会据此抛错，不会把它交出去
                break;
            }
            sb_append_float(sb, num);
            break;
        }
        case VAL_OBJ:
            switch (val_as_obj(value)->type) {
                case OBJ_BIGINT: {
                    // ★ 2026-10-10（B 组第 11 条）：**大整数按十进制原样写出** ✓
                    //   ⚠ 不补这条就会**静默写成 null** ✗ —— 实测：`encode(decode("{\"id\":153282510146394112}"))`
                    //     ⇒ `{"id":null}` ✗（那比原先的"精度丢"更糟 ✓；本轮要消灭的正是静默错值 ✓）
                    char* s = bigint_to_string((ObjBigInt*)val_as_obj(value));
                    if (s) {
                        sb_append_cstr(sb, s);
                        free(s);
                    } else {
                        sb_append_cstr(sb, "0");   // 极端兜底：拿不到十进制就写 0（不写 null ✗）
                    }
                    break;
                }
                case OBJ_STRING:
                    json_encode_string(sb, ((ObjString*)val_as_obj(value))->chars, ((ObjString*)val_as_obj(value))->len);
                    break;
                case OBJ_ARRAY:
                    json_encode_array(sb, (ObjArray*)val_as_obj(value), indent, pretty, depth + 1, err);
                    break;
                case OBJ_DICT:
                    json_encode_dict(sb, (ObjDict*)val_as_obj(value), indent, pretty, depth + 1, err);
                    break;
                case OBJ_STRUCT:
                    // ★ 2026-11-09（P1）：**struct 按字段名写成 JSON 对象**。
                    //   此前这里落到 default ⇒ 静默输出 `"null"` ✗（实测：`encode(Song)`
                    //   返回字符串 "null"，调用方以为成功）。这与第 706 行 bigint 那条是
                    //   同一类 bug —— "不认识的类型静默变 null"，而它比"报错"更糟：
                    //   数据无声丢失，跑测试也发现不了。
                    //   字段顺序 = 声明顺序（`def->fields[]`），与结构体源码一致。
                    json_encode_struct(sb, (ObjStruct*)val_as_obj(value), indent, pretty, depth + 1, err);
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

// 编码溢出时的统一收尾（见 JSON_MAX_DEPTH）
static Value json_encode_overflow(void) {
    native_throw_error("编码失败：嵌套超过 512 层（结构体可能存在循环引用）");
    return val_null();
}

// 浮点非有限（NaN / ±Infinity）时的统一收尾：JSON 没有这三个字面量，
// 直接写出去就是**非法 JSON**（且本库 decode 也读不回来）。宁可明确报错。
static Value json_encode_nonfinite(void) {
    native_throw_error("编码失败：浮点值是 NaN 或 Infinity —— JSON 不支持这三个值；"
                       "请在编码前替换成有限数或 null");
    return val_null();
}

// 顶层统一收尾：err 为 1 / 2 时分别抛对应错误（由调用方先 sb_free）
static Value json_encode_fail(int err) {
    return err == 2 ? json_encode_nonfinite() : json_encode_overflow();
}

// jsons.encode
static Value jsons_encode_func(int argc, Value* args) {
    (void)argc;
    
    StringBuilder sb;
    sb_init(&sb);
    
    int enc_err = 0;
    json_encode_value(&sb, args[0], 0, false, 0, &enc_err);
    if (enc_err) { sb_free(&sb); return json_encode_fail(enc_err); }
    
    ObjString* result = str_new(sb.data, sb.len);
    sb_free(&sb);
    
    return val_obj((Object*)result);
}

// jsons.encode_pretty
static Value jsons_encode_pretty_func(int argc, Value* args) {
    (void)argc;
    
    StringBuilder sb;
    sb_init(&sb);
    
    int enc_err = 0;
    json_encode_value(&sb, args[0], 0, true, 0, &enc_err);
    if (enc_err) { sb_free(&sb); return json_encode_fail(enc_err); }
    
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
    int enc_err = 0;
    json_encode_value(&sb, args[1], 0, true, 0, &enc_err);
    if (enc_err) { sb_free(&sb); return json_encode_overflow(); }

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

// dig 的一步：字典按字符串键取；**数组**按下标取（2026-11-03 补）
// ----------------------------------------------------------------------------
// 为什么单独一个而不是直接改 json_pick：get_str/get_obj 那几个参数的语义是"字典取键"，
//   在数组上返回 null 是它们既定的口径（不该因为 dig 需要遍历数组就改变 get_* 的行为）。
// 下标来源两种：
//   · 数组路径 `["a", 1]` —— 元素本身是 int；
//   · 点分路径 `"a.1.b"` —— 段是**整串数字**才当下标（"1x" 之类仍视为普通键 ⇒ null）。
// 越界 / 非数字 / 中途不是容器 ⇒ null（不抛异常，与 dig 其它分支同口径 ✓）
static Value json_dig_step(Value cur, Value key) {
    if (!val_is_obj(cur)) return val_null();
    ObjType t = val_as_obj(cur)->type;

    if (t == OBJ_DICT) return json_pick(cur, key);

    if (t == OBJ_ARRAY) {
        int idx = -1;
        if (val_is_int(key)) {
            idx = (int)val_as_int(key);
        } else if (val_is_obj(key) && val_as_obj(key)->type == OBJ_STRING) {
            const char* s = ((ObjString*)val_as_obj(key))->chars;
            if (!s || !*s) return val_null();
            int v = 0;
            for (const char* q = s; *q; q++) {
                if (*q < '0' || *q > '9') return val_null();
                if (v > 200000000) return val_null();   // 防溢出（也远超任何真实数组长度）
                v = v * 10 + (*q - '0');
            }
            idx = v;
        }
        ObjArray* arr = (ObjArray*)val_as_obj(cur);
        if (idx < 0 || idx >= arr->count) return val_null();
        return arr_read(arr, idx);
    }

    return val_null();
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
    // ★ 2026-10-10（B 组第 11 条）：JSON 里的大整数现在可能落在 **bigint** 上 ✓
    //   （雪花 ID 那种 >int48 的值 ✓）⇒ 能装进 int64 就照常给值 ✓；
    //   装不进 ⇒ 0 = 调用方回退默认值 ✓（与"超范围给默认值"的既有口径一致 ✓）。
    //   ⚠ 必须放在 switch **之前**用 `val_is_bigint` 判 ✗ —— 大整数在 `val_get_type()`
    //     眼里不是独立的 VAL_* 值（实测编译报 `VAL_BIGINT undeclared` ✗），它走的是对象路径 ✓。
    if (val_is_bigint(v)) {
        ObjBigInt* bi = val_as_bigint(v);
        if (!bigint_fits_in_int64(bi)) return 0;
        *out = bigint_to_int64(bi);
        return 1;
    }
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
    // ★ 2026-10-10（B 组第 11 条）：大整数也要能当浮点取 ✓
    //   ⚠ 不补这条就是**静默回默认值** ✗（实测 `get_float(snow, "id", -1.0)` ⇒ -1.0 ✗
    //     而调用方以为自己拿的是那个 ID ✓）—— 同 `json_scalar_int` 的说明 ✓
    if (val_is_bigint(v)) { *out = bigint_to_double(val_as_bigint(v)); return 1; }
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
// jsons.dig(obj, path)：按路径**逐层**取值（2026-11-03）
//
// 为什么需要它：`jsons.decode` 返回 `any`，而 Leno 不允许在 any 上做下标/字段访问
//   （编译期就拦，这是有意的安全设计）⇒ 取嵌套字段得写
//     var u = data["user"] as Dict[string, any]
//     var name = u["name"]
//   每一层来一遍。dig 把这个套路收成一行，且**不放松任何类型检查**（它返回 any，
//   调用点照旧 `_str()` / `_int()` 收窄）。
//
// path 两种形态：
//   · "a.b.c"（点分字符串）—— 键名不含点时的简写；
//   · ["a", "b", "c"]（数组）—— 键名本身含点、或需要按下标穿过 JSON 数组时用它。
// 每层按当前值的类型分派：字典查字符串键、数组查下标（int，或点分段里的整串数字）。
// 任一层不是容器 / 键或下标不存在 / 中途为 null ⇒ 返回 null（与 get_* 同口径）。
static Value jsons_dig_func(int argc, Value* args) {
    if (argc < 2) return val_null();
    Value cur = args[0];
    Value path = args[1];
    if (!val_is_obj(path)) return val_null();

    // ① 数组形态：逐元素当键取
    if (val_as_obj(path)->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)val_as_obj(path);
        if (arr->count == 0) return val_null();     // 空路径 ⇒ null（同字符串形态）
        for (int i = 0; i < arr->count; i++) {
            cur = json_dig_step(cur, arr_read(arr, i));
            if (val_is_null(cur)) return val_null();
        }
        return cur;
    }

    // ② 字符串形态：按 '.' 拆
    if (val_as_obj(path)->type != OBJ_STRING) return val_null();
    ObjString* sp = (ObjString*)val_as_obj(path);
    const char* p = sp->chars;
    char buf[256];
    int segments = 0;
    while (p && *p) {
        int n = 0;
        while (*p && *p != '.' && n < (int)sizeof(buf) - 1) buf[n++] = *p++;
        if (*p == '.') p++;
        if (n == 0) continue;              // 连续的 '.' 直接跳过
        buf[n] = '\0';
        ObjString* ks = str_new(buf, n);
        if (!ks) return val_null();
        cur = json_dig_step(cur, val_obj((Object*)ks));
        if (val_is_null(cur)) return val_null();
        segments++;
    }
    // 空路径（""、".." 之类）⇒ null，**不要**"原样返回整个对象" ——
    //   否则"路径写错"会静默拿到整个 data，比报错更难查。
    if (segments == 0) return val_null();
    return cur;
}

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

    // ★ 2026-11-09（P1）：`fill(target, src)` —— 把 JSON 对象 / Dict 按**字段声明类型**
    //   填进 struct 实例。
    //   两个形参都用 TYPE_ANY，原因各不相同：
    //     · 第一个：形参类型只到 TYPE_STRUCT 这一档、写不出具体结构体名；
    //     · 第二个：要同时接受 JSON 字符串**和**已解析好的 Dict（DB 行 / CSV 行就是 Dict），
    //       而原生形参没有"联合类型"。代价是编译期不再拦 `fill(s, 123)`，
    //       但运行期会抛明确错误（"必须是 JSON 字符串或 Dict"），不是静默失败。
    TypeKind fill_params[] = {TYPE_ANY, TYPE_ANY};
    native_register_module_method("jsons", "fill", jsons_fill_func, &NATIVE_T_BOOL, NATIVE_FIXED(fill_params));

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

    // dig(obj, path)：按路径逐层取值（any 场景专用，见函数头的说明）
    TypeKind dig_params[] = {TYPE_ANY, TYPE_ANY};
    native_register_module_method("jsons", "dig", jsons_dig_func, &NATIVE_T_ANY, NATIVE_FIXED(dig_params));
}
