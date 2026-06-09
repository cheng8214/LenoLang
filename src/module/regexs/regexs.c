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
        // 返回 1-based 位置
        return val_int((int)(start - str->chars + 1));
    }
    return val_null();
}

// 3. 查找所有匹配位置
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
    
    const char* pos = str->chars;
    while (*pos) {
        const char* start = NULL;
        const char* end = NULL;
        find_match(pattern, pos, &start, &end);
        
        if (!start) break;
        
        // 创建匹配信息字典
        ObjDict* match_info = dict_new(4);
        if (!match_info) break;
        
        // 起始位置（1-based）
        ObjString* key_start = str_copy("start", 5);
        dict_set(match_info, val_obj((Object*)key_start), val_int((int)(start - str->chars + 1)));
        
        // 结束位置（1-based，不包含）
        ObjString* key_end = str_copy("end", 3);
        dict_set(match_info, val_obj((Object*)key_end), val_int((int)(end - str->chars + 1)));
        
        // 匹配内容
        int match_len = (int)(end - start);
        ObjString* matched_str = str_copy(start, match_len);
        ObjString* key_text = str_copy("text", 4);
        dict_set(match_info, val_obj((Object*)key_text), val_obj((Object*)matched_str));
        
        arr_push_custom(result, val_obj((Object*)match_info));
        
        if (end == start) {
            pos++; // 避免空匹配无限循环
        } else {
            pos = end;
        }
    }
    
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

    native_register_module_method("regexs", "match", regex_match, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);
    native_register_module_method("regexs", "find", regex_find, 2, -1, -1, TYPE_INT, TYPE_UNKNOWN, str_params);
    native_register_module_method("regexs", "extract", regex_extract, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str_params);
    native_register_module_method("regexs", "replace", regex_replace, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str3_params);
    native_register_module_method("regexs", "split", regex_split, -1, 2, 3, TYPE_ARRAY, TYPE_STRING, str_params);
    native_register_module_method("regexs", "groups", regex_groups, 2, -1, -1, TYPE_ARRAY, TYPE_STRING, str_params);

    // 返回数组的方法
    native_register_module_method("regexs", "find_all", regex_find_all, 2, -1, -1, TYPE_ARRAY, TYPE_STRING, str_params);
    native_register_module_method("regexs", "extract_all", regex_extract_all, 2, -1, -1, TYPE_ARRAY, TYPE_STRING, str_params);
    native_register_module_method("regexs", "replace_all", regex_replace_all, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, str3_params);

    // 单参数方法
    TypeKind single_str_params[] = {TYPE_STRING};
    native_register_module_method("regexs", "escape", regex_escape, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, single_str_params);
}
