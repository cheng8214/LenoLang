#include "include/native.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

// 前向声明：字符串对象创建函数
extern ObjString* str_new(const char* chars, int len);
extern ObjString* str_copy(const char* chars, int len);
extern ObjString* str_alloc(int len);

// 前向声明：数组操作
extern ObjArray* arr_new(int capacity);
extern int arr_grow(ObjArray* arr);

// 前向声明：字典操作
extern ObjDict* dict_new(int capacity);
extern void dict_set(ObjDict* dict, Value key, Value value);

// 辅助函数：向数组添加元素
static void arr_push_custom(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        arr_grow(arr);
    }
    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
}

// ==================== 简单正则表达式引擎 ====================
// 支持：. ^ $ * + ? [] () |

typedef enum {
    RE_CHAR,        // 普通字符
    RE_ANY,         // .
    RE_START,       // ^
    RE_END,         // $
    RE_CLASS,       // [...]
    RE_CLASS_NEG,   // [^...]
    RE_GROUP,       // (...)
    RE_OR,          // |
    RE_STAR,        // *
    RE_PLUS,        // +
    RE_QUESTION,    // ?
    RE_END_PATTERN  // 结束标记
} ReOp;

typedef struct ReNode {
    ReOp op;
    char ch;            // RE_CHAR 用
    char* class_chars;  // RE_CLASS/RE_CLASS_NEG 用
    int class_len;
    struct ReNode* left;   // 左子节点 (用于 GROUP, OR)
    struct ReNode* right;  // 右子节点 (用于 OR, *, +, ?)
    struct ReNode* next;   // 下一个节点
} ReNode;

// 简单的内存池
#define RE_POOL_SIZE 256
static ReNode re_pool[RE_POOL_SIZE];
static int re_pool_idx = 0;

static ReNode* re_alloc_node(void) {
    if (re_pool_idx >= RE_POOL_SIZE) return NULL;
    ReNode* node = &re_pool[re_pool_idx++];
    memset(node, 0, sizeof(ReNode));
    return node;
}

static void re_free_all(void) {
    for (int i = 0; i < re_pool_idx; i++) {
        if (re_pool[i].class_chars) {
            free(re_pool[i].class_chars);
        }
    }
    re_pool_idx = 0;
}

// 解析字符类 [abc] 或 [^abc]
static const char* parse_class(const char* p, ReNode* node) {
    bool negated = false;
    if (*p == '^') {
        negated = true;
        p++;
    }
    
    node->op = negated ? RE_CLASS_NEG : RE_CLASS;
    
    // 收集字符
    char chars[256];
    int len = 0;
    
    while (*p && *p != ']' && len < 256) {
        if (*p == '\\' && *(p+1)) {
            p++;
            chars[len++] = *p++;
        } else if (*p == '-' && len > 0 && *(p+1) && *(p+1) != ']') {
            // 范围 a-z
            char start = chars[len-1];
            char end = *(++p);
            for (char c = start + 1; c <= end && len < 256; c++) {
                chars[len++] = c;
            }
            p++;
        } else {
            chars[len++] = *p++;
        }
    }
    
    if (*p == ']') p++;
    
    node->class_chars = (char*)malloc(len + 1);
    if (node->class_chars) {
        memcpy(node->class_chars, chars, len);
        node->class_chars[len] = '\0';
        node->class_len = len;
    }
    
    return p;
}

// 解析正则表达式
static ReNode* parse_regex(const char** pp);
static ReNode* parse_term(const char** pp);
static ReNode* parse_factor(const char** pp);

static ReNode* parse_regex(const char** pp) {
    ReNode* left = parse_term(pp);
    if (!left) return NULL;
    
    while (**pp == '|') {
        (*pp)++;
        ReNode* right = parse_term(pp);
        if (!right) return right;
        
        ReNode* or_node = re_alloc_node();
        if (!or_node) return NULL;
        or_node->op = RE_OR;
        or_node->left = left;
        or_node->right = right;
        left = or_node;
    }
    
    return left;
}

static ReNode* parse_term(const char** pp) {
    ReNode* first = NULL;
    ReNode* last = NULL;
    
    while (**pp && **pp != ')' && **pp != '|') {
        ReNode* node = parse_factor(pp);
        if (!node) return NULL;
        
        if (!first) {
            first = last = node;
        } else {
            last->next = node;
            last = node;
        }
    }
    
    return first ? first : re_alloc_node(); // 空表达式
}

static ReNode* parse_factor(const char** pp) {
    const char* p = *pp;
    ReNode* node = NULL;
    
    if (*p == '(') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_GROUP;
        node->left = parse_regex(&p);
        if (*p == ')') p++;
    } else if (*p == '.') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_ANY;
    } else if (*p == '^') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_START;
    } else if (*p == '$') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_END;
    } else if (*p == '[') {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        p = parse_class(p, node);
    } else if (*p == '\\' && *(p+1)) {
        p++;
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_CHAR;
        node->ch = *p++;
    } else if (*p && strchr("*+?|)", *p) == NULL) {
        node = re_alloc_node();
        if (!node) return NULL;
        node->op = RE_CHAR;
        node->ch = *p++;
    } else {
        return NULL;
    }
    
    // 处理量词
    if (*p == '*' || *p == '+' || *p == '?') {
        ReNode* quant = re_alloc_node();
        if (!quant) return NULL;
        quant->left = node;
        if (*p == '*') quant->op = RE_STAR;
        else if (*p == '+') quant->op = RE_PLUS;
        else quant->op = RE_QUESTION;
        p++;
        node = quant;
    }
    
    *pp = p;
    return node;
}

// 匹配字符类
static bool match_class(ReNode* node, char c) {
    bool found = false;
    for (int i = 0; i < node->class_len; i++) {
        if (node->class_chars[i] == c) {
            found = true;
            break;
        }
    }
    return (node->op == RE_CLASS) ? found : !found;
}

// 执行匹配
static const char* match_node(ReNode* node, const char* str, bool* matched);

static const char* match_regex(ReNode* node, const char* str, bool* matched) {
    *matched = false;
    if (!node) return str;
    
    // 尝试 OR 的左分支
    if (node->op == RE_OR) {
        bool left_matched = false;
        const char* left_end = match_regex(node->left, str, &left_matched);
        if (left_matched) {
            *matched = true;
            return left_end;
        }
        // 尝试右分支
        return match_regex(node->right, str, matched);
    }
    
    // 顺序匹配所有节点
    const char* pos = str;
    ReNode* cur = node;
    
    while (cur) {
        bool node_matched = false;
        const char* next_pos = match_node(cur, pos, &node_matched);
        
        if (!node_matched) return NULL;
        
        pos = next_pos;
        cur = cur->next;
    }
    
    *matched = true;
    return pos;
}

static const char* match_node(ReNode* node, const char* str, bool* matched) {
    *matched = false;
    if (!node) return str;
    
    switch (node->op) {
        case RE_CHAR:
            if (*str == node->ch) {
                *matched = true;
                return str + 1;
            }
            return NULL;
            
        case RE_ANY:
            if (*str) {
                *matched = true;
                return str + 1;
            }
            return NULL;
            
        case RE_START:
            // ^ 应该在 parse 时处理，这里不应该遇到
            *matched = true;
            return str;
            
        case RE_END:
            if (*str == '\0') {
                *matched = true;
                return str;
            }
            return NULL;
            
        case RE_CLASS:
        case RE_CLASS_NEG:
            if (*str && match_class(node, *str)) {
                *matched = true;
                return str + 1;
            }
            return NULL;
            
        case RE_GROUP:
            return match_regex(node->left, str, matched);
            
        case RE_STAR: {
            // 匹配0次或多次
            const char* best = str;
            const char* pos = str;
            while (true) {
                bool m = false;
                const char* next = match_node(node->left, pos, &m);
                if (!m) break;
                pos = next;
                best = pos;
            }
            *matched = true;
            return best;
        }
            
        case RE_PLUS: {
            // 匹配1次或多次
            const char* pos = str;
            bool first = false;
            const char* first_end = match_node(node->left, pos, &first);
            if (!first) return NULL;
            
            pos = first_end;
            while (true) {
                bool m = false;
                const char* next = match_node(node->left, pos, &m);
                if (!m) break;
                pos = next;
            }
            *matched = true;
            return pos;
        }
            
        case RE_QUESTION: {
            // 匹配0次或1次
            bool m = false;
            const char* next = match_node(node->left, str, &m);
            *matched = true;
            return m ? next : str;
        }
            
        default:
            return NULL;
    }
}

// 查找第一个匹配
static const char* find_match(ReNode* pattern, const char* str, const char** start, const char** end) {
    for (const char* pos = str; *pos; pos++) {
        bool matched = false;
        const char* match_end = match_regex(pattern, pos, &matched);
        if (matched) {
            *start = pos;
            *end = match_end;
            return match_end;
        }
    }
    return NULL;
}

// ==================== 核心方法实现 ====================

// 1. 检查字符串是否匹配正则表达式
static Value regex_match(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    bool matched = false;
    match_regex(pattern, str->chars, &matched);
    
    re_free_all();
    return val_bool(matched);
}

// 2. 查找第一个匹配位置
static Value regex_find(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, &start, &end);
    
    re_free_all();
    
    if (start) {
        // 返回 0-based 位置
        return val_int((int)(start - str->chars));
    }
    return val_int(-1);
}

// ==================== RegexMatch 的类型规格（find_all 的返回元素，v3.2.7） ====================
// 为什么不用 Dict（这是全仓**最后一个**返回裸 Dict 的地方）：
//   三个键**类型不齐** —— `start` / `end` 是 int、`text` 是 string ⇒ 同质的 `Dict[K,V]`
//   表达不了"这个键 int、那个键 string"；退一步用裸 `Dict` 则让调用点拿到 any
//   （`m["text"]` 是 any，还得手动收窄）。
// 改结构体后字段类型**编译期已知**、零收窄 —— 与 `DirEntry`（dirs.walk）/ `DirInfo`（dirs.stat）
//   同一套做法：**编译期字段表 + 运行期 ObjStructDef 是同一份声明** ⇒ 字段顺序不可能漂。
// 字段名与旧的字典键**逐字相同**（start / end / text）⇒ 只是取值方式从 `m["x"]` 变成 `m.x`。
// 顺带：不再每次匹配都 `str_copy` 三个键名（那是 3 次分配 → 0 次）。
static const NativeTypeSpec S_RX_STRING          = { NTYPE_STRING, NULL, NULL, NULL };
static const NativeTypeSpec S_RX_INT             = { NTYPE_INT,    NULL, NULL, NULL };
static const NativeTypeSpec S_REGEXMATCH_SPEC    = { NTYPE_STRUCT, "RegexMatch", NULL, NULL };
static const NativeTypeSpec S_REGEXMATCH_ARR_SPEC = { NTYPE_ARRAY, NULL, &S_REGEXMATCH_SPEC, NULL };

static const char* REGEXMATCH_FIELD_NAMES[] = { "start", "end", "text" };
static const NativeTypeSpec* REGEXMATCH_FIELD_TYPES[] = { &S_RX_INT, &S_RX_INT, &S_RX_STRING };
static const NativeStructSpec REGEXMATCH_STRUCT_SPEC = {
    "regexs", "RegexMatch", 3, REGEXMATCH_FIELD_NAMES, REGEXMATCH_FIELD_TYPES
};

// 3. 查找所有匹配位置（返回 `Array[RegexMatch]`，v3.2.7 起）
static Value regex_find_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }

    // ★ result 自己也要 root：循环里每次 `native_struct_new` / `str_copy` 都会 gc_alloc
    //   ⇒ 它是还没交出去的中间对象（同 dirs.walk 的说明）。
    Value result_val = val_obj((Object*)result);
    gc_push_root(&result_val);
    
    const char* pos = str->chars;
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        
        if (!start) break;
        
        // 创建 `RegexMatch{ start, end, text }`（字段顺序 = REGEXMATCH_STRUCT_SPEC 的声明顺序）
        ObjStruct* m = native_struct_new("RegexMatch");
        if (!m) break;
        Value m_val = val_obj((Object*)m);
        gc_push_root(&m_val);   // 填字段期间它还没进 result ⇒ 必须自己护住

        native_struct_set(m, "start", val_int((int)(start - str->chars)));   // 0-based
        native_struct_set(m, "end",   val_int((int)(end - str->chars)));     // 0-based，不含
        native_struct_set(m, "text",  val_obj((Object*)str_copy(start, (int)(end - start))));

        arr_push_custom(result, m_val);
        gc_pop_root();   // m_val
        
        if (end == start) {
            pos++; // 避免空匹配无限循环
        } else {
            pos = end;
        }
    }
    
    gc_pop_root();   // result_val
    re_free_all();
    return val_obj((Object*)result);
}

// 4. 提取匹配的子串
static Value regex_extract(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, &start, &end);
    
    re_free_all();
    
    if (start) {
        int match_len = (int)(end - start);
        ObjString* result = str_copy(start, match_len);
        return val_obj((Object*)result);
    }
    return val_null();
}

// 5. 提取所有匹配的子串
static Value regex_extract_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }
    
    const char* pos = str->chars;
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        
        if (!start) break;
        
        int match_len = (int)(end - start);
        ObjString* matched_str = str_copy(start, match_len);
        arr_push_custom(result, val_obj((Object*)matched_str));
        
        if (end == start) {
            pos++;
        } else {
            pos = end;
        }
    }
    
    re_free_all();
    return val_obj((Object*)result);
}

// 6. 替换第一个匹配
static Value regex_replace(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    ObjString* replacement = (ObjString*)val_as_obj(args[2]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, &start, &end);
    
    re_free_all();
    
    if (!start) {
        return val_obj((Object*)str);  // 未找到，返回原字符串
    }
    
    // 计算新字符串长度
    int before_len = (int)(start - str->chars);
    int match_len = (int)(end - start);
    int after_len = str->len - before_len - match_len;
    int new_len = before_len + replacement->len + after_len;
    
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    memcpy(result, str->chars, before_len);
    memcpy(result + before_len, replacement->chars, replacement->len);
    memcpy(result + before_len + replacement->len, end, after_len);
    result[new_len] = '\0';
    
    ObjString* result_str = str_new(result, new_len);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 7. 替换所有匹配
static Value regex_replace_all(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    ObjString* replacement = (ObjString*)val_as_obj(args[2]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    // 计算结果长度
    const char* pos = str->chars;
    int match_count = 0;
    int total_match_len = 0;
    
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        if (!start) break;
        match_count++;
        total_match_len += (int)(end - start);
        if (end == start) pos++;
        else pos = end;
    }
    
    if (match_count == 0) {
        re_free_all();
        return val_obj((Object*)str);
    }
    
    int new_len = str->len - total_match_len + match_count * replacement->len;
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        re_free_all();
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    // 构建结果字符串
    pos = str->chars;
    char* dst = result;
    
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        if (!start) break;
        
        // 复制匹配前的内容
        int before = (int)(start - pos);
        memcpy(dst, pos, before);
        dst += before;
        
        // 复制替换内容
        memcpy(dst, replacement->chars, replacement->len);
        dst += replacement->len;
        
        pos = end;
        if (end == start) {
            *dst++ = *pos++;
        }
    }
    
    // 复制剩余内容
    strcpy(dst, pos);
    
    re_free_all();
    
    ObjString* result_str = str_new(result, new_len);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 8. 分割字符串
static Value regex_split(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    // 限制分割次数（可选）
    int limit = -1;
    if (argc >= 3) {
        limit = val_as_int(args[2]);
    }
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    ObjArray* result = arr_new(8);
    if (!result) {
        re_free_all();
        return val_null();
    }
    
    const char* pos = str->chars;
    const char* last_pos = pos;
    int count = 0;
    
    while (*pos && (limit < 0 || count < limit - 1)) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        if (!start) break;
        
        int part_len = (int)(start - last_pos);
        ObjString* part = str_copy(last_pos, part_len);
        arr_push_custom(result, val_obj((Object*)part));
        
        pos = end;
        last_pos = pos;
        count++;
    }
    
    // 添加最后一部分
    if (*last_pos || result->count == 0) {
        ObjString* part = str_copy(last_pos, strlen(last_pos));
        arr_push_custom(result, val_obj((Object*)part));
    }
    
    re_free_all();
    return val_obj((Object*)result);
}

// 9. 获取匹配分组 - 简化版，返回所有匹配
static Value regex_groups(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern_str = (ObjString*)val_as_obj(args[1]);
    
    re_pool_idx = 0;
    memset(re_pool, 0, sizeof(re_pool));
    
    const char* p = pattern_str->chars;
    ReNode* pattern = parse_regex(&p);
    
    if (!pattern) {
        re_free_all();
        native_throw_error("无效的正则表达式");
        return val_null();
    }
    
    const char* start = NULL;
    const char* end = NULL;
    find_match(pattern, str->chars, &start, &end);
    
    re_free_all();
    
    if (!start) {
        return val_null();
    }
    
    // 返回包含完整匹配的数组
    ObjArray* result = arr_new(1);
    if (!result) return val_null();
    
    int match_len = (int)(end - start);
    ObjString* matched_str = str_copy(start, match_len);
    arr_push_custom(result, val_obj((Object*)matched_str));
    
    return val_obj((Object*)result);
}

// 10. 转义正则特殊字符
static Value regex_escape(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    
    // 正则特殊字符：. ^ $ * + ? { } [ ] \ | ( )
    const char* special = ".^$*+?{}[]\\|()";
    
    // 计算转义后的长度
    int new_len = 0;
    for (int i = 0; i < str->len; i++) {
        if (strchr(special, str->chars[i])) {
            new_len += 2;
        } else {
            new_len += 1;
        }
    }
    
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    int j = 0;
    for (int i = 0; i < str->len; i++) {
        if (strchr(special, str->chars[i])) {
            result[j++] = '\\';
        }
        result[j++] = str->chars[i];
    }
    result[j] = '\0';
    
    ObjString* result_str = str_new(result, j);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// ==================== 初始化 ====================

void regexs_init_module(void) {
    // 注册正则模块方法
    TypeKind str_params[] = {TYPE_STRING, TYPE_STRING};
    TypeKind str3_params[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING};

    native_register_module_method_spec("regexs", "match", regex_match, 2, -1, -1, &NATIVE_T_BOOL, str_params);
    native_register_module_method_spec("regexs", "find", regex_find, 2, -1, -1, &NATIVE_T_INT, str_params);
    native_register_module_method_spec("regexs", "extract", regex_extract, 2, -1, -1, &NATIVE_T_STRING, str_params);
    native_register_module_method_spec("regexs", "replace", regex_replace, 3, -1, -1, &NATIVE_T_STRING, str3_params);
    native_register_module_method_spec("regexs", "split", regex_split, -1, 2, 3, &NATIVE_T_ARR_STRING, str_params);
    // split(str, pattern[, limit])：前两位必须是 string（v3.2.7 显式声明可变参数的前缀类型 ——
    //   此前 `arity == -1` ⇒ param_types 被整份忽略，`regexs.split(1, 2)` 编译期不报错 ✗）
    native_set_method_vararg_params("regexs", "split", 2, str_params, TYPE_ANY);
    native_register_module_method_spec("regexs", "groups", regex_groups, 2, -1, -1, &NATIVE_T_ARR_STRING, str_params);

    // 返回数组的方法
    // find_all：返回 `Array[RegexMatch]`（**字段类型编译期已知**，v3.2.7）——
    //   原来是 `Array[Dict]`（裸 Dict）⇒ `m["text"]` 是 any、取值要手动收窄；
    //   改结构体后 `m.text` 直接是 string、`m.start` 直接是 int ✓
    native_register_struct_spec(&REGEXMATCH_STRUCT_SPEC);
    native_register_module_method_spec("regexs", "find_all", regex_find_all, 2, -1, -1, &S_REGEXMATCH_ARR_SPEC, str_params);
    native_register_module_method_spec("regexs", "extract_all", regex_extract_all, 2, -1, -1, &NATIVE_T_ARR_STRING, str_params);
    native_register_module_method_spec("regexs", "replace_all", regex_replace_all, 3, -1, -1, &NATIVE_T_STRING, str3_params);

    // 单参数方法
    TypeKind single_str_params[] = {TYPE_STRING};
    native_register_module_method_spec("regexs", "escape", regex_escape, 1, -1, -1, &NATIVE_T_STRING, single_str_params);
}
