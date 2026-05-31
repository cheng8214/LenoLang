#include "include/native.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>

// 前向声明：字符串实例方法支持函数（定义在 object_string.c）
extern void string_init_methods(void);
extern void string_register_method_with_params(const char* name, ObjNative* method, int arity,
                                                int min_arity, int max_arity,
                                                TypeKind return_type, TypeKind return_element_type, TypeKind* param_types);

// 前向声明：字符串对象创建函数
extern ObjString* str_new(const char* chars, int len);
extern ObjString* str_copy(const char* chars, int len);
extern ObjString* str_alloc(int len);
extern uint32_t hash_string(const char* key, int length);

// ==================== 核心方法实现 ====================

static Value str_len(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    return val_int(str->len);
}

// 1. 大小写转换

static Value str_to_upper(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    ObjString* result = str_alloc(len);
    if (!result) return val_null();
    
    for (int i = 0; i < len; i++) {
        result->chars[i] = (char)toupper((unsigned char)str->chars[i]);
    }
    result->chars[len] = '\0';
    result->hash = hash_string(result->chars, len);
    
    return val_obj((Object*)result);
}

static Value str_to_lower(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    ObjString* result = str_alloc(len);
    if (!result) return val_null();
    
    for (int i = 0; i < len; i++) {
        result->chars[i] = (char)tolower((unsigned char)str->chars[i]);
    }
    result->chars[len] = '\0';
    result->hash = hash_string(result->chars, len);
    
    return val_obj((Object*)result);
}

// 2. 修剪空白

static Value str_trim(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    const char* chars = str->chars;
    int len = str->len;
    
    // 跳过开头的空白
    int start = 0;
    while (start < len && isspace((unsigned char)chars[start])) {
        start++;
    }
    
    // 跳过结尾的空白
    int end = len - 1;
    while (end >= start && isspace((unsigned char)chars[end])) {
        end--;
    }
    
    int new_len = end - start + 1;
    if (new_len <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_copy(chars + start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

static Value str_trim_start(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    const char* chars = str->chars;
    int len = str->len;
    
    // 跳过开头的空白
    int start = 0;
    while (start < len && isspace((unsigned char)chars[start])) {
        start++;
    }
    
    int new_len = len - start;
    if (new_len <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_copy(chars + start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

static Value str_trim_end(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    const char* chars = str->chars;
    int len = str->len;
    
    // 跳过结尾的空白
    int end = len - 1;
    while (end >= 0 && isspace((unsigned char)chars[end])) {
        end--;
    }
    
    int new_len = end + 1;
    if (new_len <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_copy(chars, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

// 3. 包含检查

static Value str_starts_with(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* prefix = (ObjString*)val_as_obj(args[1]);
    
    if (prefix->len > str->len) {
        return val_bool(false);
    }
    
    return val_bool(strncmp(str->chars, prefix->chars, prefix->len) == 0);
}

static Value str_ends_with(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* suffix = (ObjString*)val_as_obj(args[1]);
    
    if (suffix->len > str->len) {
        return val_bool(false);
    }
    
    int start = str->len - suffix->len;
    return val_bool(strncmp(str->chars + start, suffix->chars, suffix->len) == 0);
}

// 4. 查找替换

static Value str_replace(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* old_str = (ObjString*)val_as_obj(args[1]);
    ObjString* new_str = (ObjString*)val_as_obj(args[2]);
    
    if (old_str->len == 0) {
        // 如果 old_str 为空，直接返回原字符串
        return val_obj((Object*)str);
    }
    
    // 计算替换后的长度
    int count = 0;
    const char* pos = str->chars;
    while ((pos = strstr(pos, old_str->chars)) != NULL) {
        count++;
        pos += old_str->len;
    }
    
    if (count == 0) {
        // 没有找到，返回原字符串
        return val_obj((Object*)str);
    }
    
    int new_len = str->len + count * (new_str->len - old_str->len);
    char* result = (char*)malloc(new_len + 1);
    if (!result) {
        native_throw_error("内存分配失败");
        return val_null();
    }
    
    char* dst = result;
    const char* src = str->chars;
    const char* match;
    
    while ((match = strstr(src, old_str->chars)) != NULL) {
        int before_len = match - src;
        memcpy(dst, src, before_len);
        dst += before_len;
        memcpy(dst, new_str->chars, new_str->len);
        dst += new_str->len;
        src = match + old_str->len;
    }
    
    // 复制剩余部分
    int remaining = str->len - (src - str->chars);
    memcpy(dst, src, remaining);
    dst += remaining;
    *dst = '\0';
    
    ObjString* result_str = str_new(result, new_len);
    free(result);
    if (!result_str) return val_null();
    
    return val_obj((Object*)result_str);
}

// 5. 子串提取

static Value str_slice(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    int start = val_as_int(args[1]);
    int end = val_as_int(args[2]);
    
    // 处理负数索引
    if (start < 0) start = len + start;
    if (end < 0) end = len + end;
    
    // 边界检查
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start > end) start = end;
    
    int new_len = end - start;
    if (new_len <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_copy(str->chars + start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

static Value str_sub_str(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    int start = val_as_int(args[1]);
    int length = val_as_int(args[2]);
    
    // 处理负数索引
    if (start < 0) start = len + start;
    
    // 边界检查
    if (start < 0) start = 0;
    if (start > len) start = len;
    if (length < 0) length = 0;
    if (start + length > len) length = len - start;
    
    if (length <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_copy(str->chars + start, length);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

// 6. 新增：字符串反转

static Value str_reverse(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    if (len == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_alloc(len);
    if (!result) return val_null();
    
    for (int i = 0; i < len; i++) {
        result->chars[i] = str->chars[len - i - 1];
    }
    result->chars[len] = '\0';
    result->hash = hash_string(result->chars, len);
    
    return val_obj((Object*)result);
}

// 7. 新增：重复字符串

static Value str_rep(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int n = val_as_int(args[1]);
    
    if (n <= 0 || str->len == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    long total_len = (long)str->len * n;
    if (total_len > INT_MAX) {
        native_throw_error("结果字符串太长");
        return val_null();
    }
    
    ObjString* result = str_alloc((int)total_len);
    if (!result) return val_null();
    
    for (int i = 0; i < n; i++) {
        memcpy(result->chars + i * str->len, str->chars, str->len);
    }
    result->chars[(int)total_len] = '\0';
    result->hash = hash_string(result->chars, (int)total_len);
    
    return val_obj((Object*)result);
}

// 8. 新增：获取字符的ASCII码值

static Value str_byte(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    // 默认获取第1个字符
    int pos = 1;
    if (argc >= 2) {
        pos = val_as_int(args[1]);
    }
    
    // 处理负数索引（-1表示最后一个字符）
    if (pos < 0) pos = len + pos + 1;
    
    // 转换为0-based索引
    int idx = pos - 1;
    
    // 边界检查
    if (idx < 0 || idx >= len) {
        return val_null();  // 越界返回null
    }
    
    return val_int((unsigned char)str->chars[idx]);
}

// 9. 新增：从ASCII码创建字符串

static Value str_char(int argc, Value* args) {
    // 支持一个或多个ASCII码
    if (argc == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_alloc(argc);
    if (!result) return val_null();
    
    for (int i = 0; i < argc; i++) {
        int code = val_as_int(args[i]);
        if (code < 0 || code > 255) {
            native_throw_error("ASCII码必须在0-255之间");
            return val_null();
        }
        result->chars[i] = (char)code;
    }
    result->chars[argc] = '\0';
    result->hash = hash_string(result->chars, argc);
    
    return val_obj((Object*)result);
}

// 10. 新增：查找子串位置

static Value str_find(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern = (ObjString*)val_as_obj(args[1]);
    
    // 起始位置（可选，默认为1）
    int start = 1;
    if (argc >= 3) {
        start = val_as_int(args[2]);
    }
    
    // 是否使用纯文本匹配（可选，默认为false）
    bool plain = false;
    if (argc >= 4 && val_is_bool(args[3])) {
        plain = val_as_bool(args[3]);
    }
    
    // 处理负数起始位置
    int len = str->len;
    if (start < 0) start = len + start + 1;
    if (start < 1) start = 1;
    
    // 转换为0-based索引
    int pos = start - 1;
    if (pos >= len) {
        return val_null();
    }
    
    if (pattern->len == 0) {
        // 空模式匹配起始位置
        return val_int(start);
    }
    
    if (plain) {
        // 纯文本查找（不使用模式匹配）
        const char* found = strstr(str->chars + pos, pattern->chars);
        if (found) {
            return val_int((int)(found - str->chars) + 1);
        }
    } else {
        // 简单模式匹配（支持 ^ 开头和 $ 结尾）
        if (pattern->len == 2 && pattern->chars[0] == '^') {
            // 匹配开头
            char target = pattern->chars[1];
            if (pos < len && str->chars[pos] == target) {
                return val_int(pos + 1);
            }
        } else if (pattern->len == 2 && pattern->chars[1] == '$') {
            // 匹配结尾
            char target = pattern->chars[0];
            if (len > 0 && str->chars[len - 1] == target) {
                return val_int(len);
            }
        } else {
            // 普通子串查找
            const char* found = strstr(str->chars + pos, pattern->chars);
            if (found) {
                return val_int((int)(found - str->chars) + 1);
            }
        }
    }
    
    return val_null();  // 未找到
}

// 11. 新增：字符串格式化

static Value str_format(int argc, Value* args) {
    if (argc == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    // 第一个参数必须是格式字符串
    ObjString* fmt = (ObjString*)val_as_obj(args[0]);
    const char* fmt_str = fmt->chars;
    int fmt_len = fmt->len;
    
    // 分配结果缓冲区（初始大小）
    int buf_size = fmt_len * 2 + 64;
    char* result = (char*)malloc(buf_size);
    if (!result) return val_null();
    
    int result_len = 0;
    int arg_idx = 1;  // 从第二个参数开始
    
    for (int i = 0; i < fmt_len; i++) {
        if (fmt_str[i] != '%') {
            // 普通字符，直接复制
            if (result_len + 1 >= buf_size) {
                buf_size *= 2;
                char* new_buf = (char*)realloc(result, buf_size);
                if (!new_buf) {
                    free(result);
                    return val_null();
                }
                result = new_buf;
            }
            result[result_len++] = fmt_str[i];
        } else {
            // 遇到格式符
            i++;
            if (i >= fmt_len) break;
            
            char spec = fmt_str[i];
            char temp_buf[256];
            int temp_len = 0;
            
            switch (spec) {
                case 's': {
                    // 字符串
                    if (arg_idx < argc && val_is_obj(args[arg_idx]) &&
                        val_as_obj(args[arg_idx])->type == OBJ_STRING) {
                        ObjString* s = (ObjString*)val_as_obj(args[arg_idx]);
                        temp_len = s->len;
                        if (temp_len > 255) temp_len = 255;
                        memcpy(temp_buf, s->chars, temp_len);
                    } else if (arg_idx < argc) {
                        // 非字符串参数，转为字符串表示
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<value>");
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    }
                    arg_idx++;
                    break;
                }
                
                case 'd':
                case 'i': {
                    // 整数
                    if (arg_idx < argc) {
                        int val = val_as_int(args[arg_idx]);
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "%d", val);
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    }
                    arg_idx++;
                    break;
                }
                
                case 'f': {
                    // 浮点数（支持 int、float 和 BigInt）
                    if (arg_idx < argc) {
                        double val;
                        if (val_is_int(args[arg_idx])) {
                            val = (double)val_as_int(args[arg_idx]);
                        } else if (val_is_float(args[arg_idx])) {
                            val = val_as_num(args[arg_idx]);
                        } else if (val_is_bigint(args[arg_idx])) {
                            val = bigint_to_double(val_as_bigint(args[arg_idx]));
                        } else {
                            val = 0;
                        }
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "%f", val);
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    }
                    arg_idx++;
                    break;
                }
                
                case 'c': {
                    // 字符
                    if (arg_idx < argc) {
                        int val = val_as_int(args[arg_idx]);
                        temp_buf[0] = (char)val;
                        temp_len = 1;
                    } else {
                        temp_buf[0] = '?';
                        temp_len = 1;
                    }
                    arg_idx++;
                    break;
                }
                
                case 'x':
                case 'X': {
                    // 十六进制
                    if (arg_idx < argc) {
                        int val = val_as_int(args[arg_idx]);
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), 
                                          spec == 'x' ? "%x" : "%X", val);
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    }
                    arg_idx++;
                    break;
                }
                
                case '%': {
                    // 转义的百分号
                    temp_buf[0] = '%';
                    temp_len = 1;
                    break;
                }
                
                default: {
                    // 未知的格式符，原样输出
                    temp_buf[0] = '%';
                    temp_buf[1] = spec;
                    temp_len = 2;
                    break;
                }
            }
            
            // 确保缓冲区足够
            while (result_len + temp_len >= buf_size) {
                buf_size *= 2;
                char* new_buf = (char*)realloc(result, buf_size);
                if (!new_buf) {
                    free(result);
                    return val_null();
                }
                result = new_buf;
            }
            
            memcpy(result + result_len, temp_buf, temp_len);
            result_len += temp_len;
        }
    }
    
    result[result_len] = '\0';
    
    ObjString* result_str = str_new(result, result_len);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 辅助函数：向数组添加元素
static void arr_push_custom(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        arr_grow(arr);
    }
    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
}

// 12. 新增：字符串分割

static Value str_split(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* sep = (ObjString*)val_as_obj(args[1]);
    
    // 创建数组
    ObjArray* result = arr_new(8);
    if (!result) return val_null();
    
    const char* str_chars = str->chars;
    int str_len = str->len;
    const char* sep_chars = sep->chars;
    int sep_len = sep->len;
    
    // 空分隔符，每个字符分割
    if (sep_len == 0) {
        for (int i = 0; i < str_len; i++) {
            ObjString* ch_str = str_copy(str_chars + i, 1);
            if (!ch_str) return val_null();
            arr_push_custom(result, val_obj((Object*)ch_str));
        }
        return val_obj((Object*)result);
    }
    
    int start = 0;
    int i = 0;
    
    while (i <= str_len - sep_len) {
        if (strncmp(str_chars + i, sep_chars, sep_len) == 0) {
            // 找到分隔符
            int part_len = i - start;
            if (part_len >= 0) {
                ObjString* part = str_copy(str_chars + start, part_len);
                if (!part) return val_null();
                arr_push_custom(result, val_obj((Object*)part));
            }
            start = i + sep_len;
            i = start;
        } else {
            i++;
        }
    }
    
    // 添加最后一部分
    int part_len = str_len - start;
    if (part_len >= 0) {
        ObjString* part = str_copy(str_chars + start, part_len);
        if (!part) return val_null();
        arr_push_custom(result, val_obj((Object*)part));
    }
    
    return val_obj((Object*)result);
}

// 13. 新增：数组连接为字符串

static Value str_join(int argc, Value* args) {
    (void)argc;
    
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);
    ObjString* sep = (ObjString*)val_as_obj(args[1]);
    
    // 计算总长度
    int total_len = 0;
    for (int i = 0; i < arr->count; i++) {
        Value elem = arr->elements[i];
        if (val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRING) {
            total_len += ((ObjString*)val_as_obj(elem))->len;
        }
        if (i < arr->count - 1) {
            total_len += sep->len;
        }
    }
    
    // 分配结果缓冲区
    char* result = (char*)malloc(total_len + 1);
    if (!result) return val_null();
    
    int pos = 0;
    for (int i = 0; i < arr->count; i++) {
        Value elem = arr->elements[i];
        if (val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRING) {
            ObjString* s = (ObjString*)val_as_obj(elem);
            memcpy(result + pos, s->chars, s->len);
            pos += s->len;
        }
        if (i < arr->count - 1 && sep->len > 0) {
            memcpy(result + pos, sep->chars, sep->len);
            pos += sep->len;
        }
    }
    
    result[pos] = '\0';
    ObjString* result_str = str_new(result, pos);
    free(result);
    
    if (!result_str) return val_null();
    return val_obj((Object*)result_str);
}

// 14. 新增：包含检查

static Value str_has(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* substr = (ObjString*)val_as_obj(args[1]);
    
    if (substr->len == 0) {
        return val_bool(true);
    }
    if (substr->len > str->len) {
        return val_bool(false);
    }
    
    const char* found = strstr(str->chars, substr->chars);
    return val_bool(found != NULL);
}

// 15. 新增：统计子串出现次数

static Value str_count(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* substr = (ObjString*)val_as_obj(args[1]);
    
    if (substr->len == 0) {
        return val_int(0);  // 空字符串计数为0
    }
    if (substr->len > str->len) {
        return val_int(0);
    }
    
    int count = 0;
    const char* pos = str->chars;
    
    while ((pos = strstr(pos, substr->chars)) != NULL) {
        count++;
        pos += substr->len;  // 跳过已匹配的部分，避免重叠计数
    }
    
    return val_int(count);
}

// 16. 新增：左侧填充

static Value str_pad_start(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int target_len = val_as_int(args[1]);
    
    // 获取填充字符，默认为空格
    char pad_char = ' ';
    if (argc >= 3) {
        ObjString* pad_str = (ObjString*)val_as_obj(args[2]);
        if (pad_str->len > 0) {
            pad_char = pad_str->chars[0];
        }
    }
    
    // 如果目标长度小于等于当前长度，返回原字符串
    if (target_len <= str->len) {
        return val_obj((Object*)str);
    }
    
    int pad_len = target_len - str->len;
    ObjString* result = str_alloc(target_len);
    if (!result) return val_null();
    
    // 填充字符
    for (int i = 0; i < pad_len; i++) {
        result->chars[i] = pad_char;
    }
    
    // 复制原字符串
    memcpy(result->chars + pad_len, str->chars, str->len);
    result->chars[target_len] = '\0';
    result->hash = hash_string(result->chars, target_len);
    
    return val_obj((Object*)result);
}

// 17. 新增：右侧填充

static Value str_pad_end(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int target_len = val_as_int(args[1]);
    
    // 获取填充字符，默认为空格
    char pad_char = ' ';
    if (argc >= 3) {
        ObjString* pad_str = (ObjString*)val_as_obj(args[2]);
        if (pad_str->len > 0) {
            pad_char = pad_str->chars[0];
        }
    }
    
    // 如果目标长度小于等于当前长度，返回原字符串
    if (target_len <= str->len) {
        return val_obj((Object*)str);
    }
    
    int pad_len = target_len - str->len;
    ObjString* result = str_alloc(target_len);
    if (!result) return val_null();
    
    // 复制原字符串
    memcpy(result->chars, str->chars, str->len);
    
    // 填充字符
    for (int i = 0; i < pad_len; i++) {
        result->chars[str->len + i] = pad_char;
    }
    
    result->chars[target_len] = '\0';
    result->hash = hash_string(result->chars, target_len);
    
    return val_obj((Object*)result);
}

// ==================== 全局函数适配器层 ====================

// format(fmt, ...) - 全局格式化函数
static Value native_format(int argc, Value* args) {
    return str_format(argc, args);
}

// ==================== 初始化 ====================

void strings_init_module(void) {
    // 注册字符串模块方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    TypeKind len_params[] = {TYPE_STRING};
    native_register_module_method("strings", "len", str_len, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

    // 1. 大小写转换
    TypeKind upper_params[] = {TYPE_STRING};
    native_register_module_method("strings", "to_upper", str_to_upper, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, upper_params);

    TypeKind lower_params[] = {TYPE_STRING};
    native_register_module_method("strings", "to_lower", str_to_lower, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, lower_params);

    // 2. 修剪空白
    TypeKind trim_params[] = {TYPE_STRING};
    native_register_module_method("strings", "trim", str_trim, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, trim_params);
    native_register_module_method("strings", "trim_start", str_trim_start, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, trim_params);
    native_register_module_method("strings", "trim_end", str_trim_end, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, trim_params);

    // 3. 包含检查
    TypeKind starts_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "starts_with", str_starts_with, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, starts_params);

    TypeKind ends_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "ends_with", str_ends_with, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, ends_params);

    // 4. 查找替换
    TypeKind replace_params[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "replace", str_replace, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, replace_params);

    // 5. 子串提取
    TypeKind slice_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "slice", str_slice, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, slice_params);

    TypeKind substr_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "sub_str", str_sub_str, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, substr_params);

    // 6. 新增：字符串反转
    TypeKind reverse_params[] = {TYPE_STRING};
    native_register_module_method("strings", "reverse", str_reverse, 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, reverse_params);

    // 7. 新增：重复字符串
    TypeKind rep_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("strings", "rep", str_rep, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, rep_params);

    // 8. 新增：获取字符的ASCII码值（支持1-2个可变参数）
    TypeKind byte_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("strings", "byte", str_byte, -1, 1, 2, TYPE_INT, TYPE_UNKNOWN, byte_params);

    // 9. 新增：从ASCII码创建字符串（可变参数）
    native_register_module_method("strings", "char", str_char, -1, 1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 10. 新增：查找子串位置（支持2-4个可变参数）
    TypeKind find_params[] = {TYPE_STRING, TYPE_STRING, TYPE_INT, TYPE_BOOL};
    native_register_module_method("strings", "find", str_find, -1, 2, 4, TYPE_INT, TYPE_UNKNOWN, find_params);

    // 11. 新增：字符串格式化（可变参数）
    TypeKind format_params[] = {TYPE_STRING};
    native_register_module_method("strings", "format", str_format, -1, 1, -1, TYPE_STRING, TYPE_UNKNOWN, format_params);

    // 12. 新增：字符串分割
    TypeKind split_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "split", str_split, 2, -1, -1, TYPE_ARRAY, TYPE_STRING, split_params);

    // 13. 新增：数组连接
    TypeKind join_params[] = {TYPE_ARRAY, TYPE_STRING};
    native_register_module_method("strings", "join", str_join, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, join_params);

    // 14. 新增：包含检查
    TypeKind has_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "has", str_has, 2, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, has_params);

    // 15. 新增：统计子串出现次数
    TypeKind count_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "count", str_count, 2, -1, -1, TYPE_INT, TYPE_UNKNOWN, count_params);

    // 16. 新增：左侧填充
    TypeKind pad_params[] = {TYPE_STRING, TYPE_INT, TYPE_STRING};
    native_register_module_method("strings", "pad_start", str_pad_start, -1, 2, 3, TYPE_STRING, TYPE_UNKNOWN, pad_params);

    // 17. 新增：右侧填充
    native_register_module_method("strings", "pad_end", str_pad_end, -1, 2, 3, TYPE_STRING, TYPE_UNKNOWN, pad_params);
}

// 初始化全局函数（程序启动时调用）
void strings_init_globals(void) {
    // 注册全局 format 函数（可变参数，至少 1 个，无上限）
    vm_register_native("format", native_format, -1, 1, -1, TYPE_STRING, NULL);
}

void strings_init_instance_methods(void) {
    string_init_methods();
    // 注册实例方法：方法名, 方法对象, 参数个数(不含receiver), 返回类型, 参数类型
    TypeKind len_params[] = {};
    string_register_method_with_params("len", make_native(str_len, 1, "len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

    // 1. 大小写转换
    TypeKind empty_params[] = {};
    string_register_method_with_params("to_upper", make_native(str_to_upper, 1, "to_upper"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);
    string_register_method_with_params("to_lower", make_native(str_to_lower, 1, "to_lower"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);

    // 2. 修剪空白
    string_register_method_with_params("trim", make_native(str_trim, 1, "trim"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);
    string_register_method_with_params("trim_start", make_native(str_trim_start, 1, "trim_start"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);
    string_register_method_with_params("trim_end", make_native(str_trim_end, 1, "trim_end"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);

    // 3. 包含检查
    TypeKind str_params[] = {TYPE_STRING};
    string_register_method_with_params("starts_with", make_native(str_starts_with, 2, "starts_with"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);
    string_register_method_with_params("ends_with", make_native(str_ends_with, 2, "ends_with"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, str_params);

    // 4. 查找替换
    TypeKind replace2_params[] = {TYPE_STRING, TYPE_STRING};
    string_register_method_with_params("replace", make_native(str_replace, 3, "replace"), 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, replace2_params);

    // 5. 子串提取
    TypeKind int2_params[] = {TYPE_INT, TYPE_INT};
    string_register_method_with_params("slice", make_native(str_slice, 3, "slice"), 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, int2_params);
    string_register_method_with_params("sub_str", make_native(str_sub_str, 3, "sub_str"), 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, int2_params);

    // 6. 新增：字符串反转（无参数实例方法）
    string_register_method_with_params("reverse", make_native(str_reverse, 1, "reverse"), 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, empty_params);

    // 7. 新增：重复字符串
    TypeKind int_params[] = {TYPE_INT};
    string_register_method_with_params("rep", make_native(str_rep, 2, "rep"), 1, -1, -1, TYPE_STRING, TYPE_UNKNOWN, int_params);

    // 8. 新增：获取字符的ASCII码值
    string_register_method_with_params("byte", make_native(str_byte, 2, "byte"), 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, int_params);
    // byte() 无参数版本使用默认值0（第1个字符）

    // 10. 新增：查找子串位置
    TypeKind str_int_bool_params[] = {TYPE_STRING, TYPE_INT, TYPE_BOOL};
    string_register_method_with_params("find", make_native(str_find, 4, "find"), 3, -1, -1, TYPE_INT, TYPE_UNKNOWN, str_int_bool_params);

    // 12. 新增：字符串分割（实例方法）
    TypeKind split_sep_params[] = {TYPE_STRING};
    string_register_method_with_params("split", make_native(str_split, 2, "split"), 1, -1, -1, TYPE_ARRAY, TYPE_UNKNOWN, split_sep_params);

    // 14. 新增：包含检查（实例方法）
    TypeKind has_substr_params[] = {TYPE_STRING};
    string_register_method_with_params("has", make_native(str_has, 2, "has"), 1, -1, -1, TYPE_BOOL, TYPE_UNKNOWN, has_substr_params);

    // 15. 新增：统计子串出现次数（实例方法）
    string_register_method_with_params("count", make_native(str_count, 2, "count"), 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, has_substr_params);

    // 16. 新增：左侧填充（实例方法）
    TypeKind int_str_params[] = {TYPE_INT, TYPE_STRING};
    string_register_method_with_params("pad_start", make_native(str_pad_start, -1, "pad_start"), 2, 2, 3, TYPE_STRING, TYPE_UNKNOWN, int_str_params);

    // 17. 新增：右侧填充（实例方法）
    string_register_method_with_params("pad_end", make_native(str_pad_end, -1, "pad_end"), 2, 2, 3, TYPE_STRING, TYPE_UNKNOWN, int_str_params);
}
