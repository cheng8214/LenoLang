#include "include/native.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

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
    return val_int(str->char_len);
}

static Value str_byte_len(int argc, Value* args) {
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
    result->char_len = str->char_len;
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
    result->char_len = str->char_len;
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
        // 空字符串替换：在每个字符之间插入 new_str
        // "你好".replace("", "-") → "-你-好-"
        if (str->len == 0) {
            return val_obj((Object*)new_str);
        }
        int char_len = str->char_len;
        int new_len = str->len + (char_len + 1) * new_str->len;
        char* result = (char*)malloc(new_len + 1);
        if (!result) {
            native_throw_error("内存分配失败");
            return val_null();
        }
        char* p = result;
        // 前缀
        memcpy(p, new_str->chars, new_str->len);
        p += new_str->len;
        // 按字符遍历
        for (int i = 0; i < char_len; i++) {
            int byte_offset = utf8_char_offset(str->chars, str->len, i);
            int char_bytes = utf8_char_byte_len(str->chars, str->len, byte_offset);
            memcpy(p, str->chars + byte_offset, char_bytes);
            p += char_bytes;
            memcpy(p, new_str->chars, new_str->len);
            p += new_str->len;
        }
        *p = '\0';
        ObjString* res = str_copy(result, (int)(p - result));
        free(result);
        return val_obj((Object*)res);
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
    int char_len = str->char_len;
    
    int start = val_as_int(args[1]);
    int end = val_as_int(args[2]);
    
    // 处理负数索引
    if (start < 0) start = char_len + start;
    if (end < 0) end = char_len + end;
    
    // 边界检查
    if (start < 0) start = 0;
    if (end > char_len) end = char_len;
    if (start > end) start = end;
    
    if (start >= end) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    int byte_start = utf8_char_offset(str->chars, str->len, start);
    int byte_end = utf8_char_offset(str->chars, str->len, end);
    int new_len = byte_end - byte_start;
    
    ObjString* result = str_copy(str->chars + byte_start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

static Value str_sub_str(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int char_len = str->char_len;
    
    int start = val_as_int(args[1]);
    int length = val_as_int(args[2]);
    
    // 处理负数索引
    if (start < 0) start = char_len + start;
    
    // 边界检查
    if (start < 0) start = 0;
    if (start > char_len) start = char_len;
    if (length < 0) length = 0;
    if (start + length > char_len) length = char_len - start;
    
    if (length <= 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    int byte_start = utf8_char_offset(str->chars, str->len, start);
    int byte_end = utf8_char_offset(str->chars, str->len, start + length);
    int new_len = byte_end - byte_start;
    
    ObjString* result = str_copy(str->chars + byte_start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

// 5b. 字节级切片：按字节偏移量截取子串（用于二进制数据处理）
static Value str_byte_slice(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int byte_len = str->len;
    
    int start = val_as_int(args[1]);
    int end = val_as_int(args[2]);
    
    // 处理负数索引（基于字节长度）
    if (start < 0) start = byte_len + start;
    if (end < 0) end = byte_len + end;
    
    // 边界检查
    if (start < 0) start = 0;
    if (end > byte_len) end = byte_len;
    if (start > end) start = end;
    
    if (start >= end) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    int new_len = end - start;
    ObjString* result = str_copy(str->chars + start, new_len);
    if (!result) return val_null();
    
    return val_obj((Object*)result);
}

// 6. 新增：字符串反转

static Value str_reverse(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int byte_len = str->len;
    int char_len = str->char_len;
    
    if (byte_len == 0 || char_len == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_alloc(byte_len);
    if (!result) return val_null();
    
    // 按字符反转：从后往前遍历每个 Unicode 字符
    char* dst = result->chars;
    for (int i = char_len - 1; i >= 0; i--) {
        int byte_offset = utf8_char_offset(str->chars, byte_len, i);
        int char_bytes = utf8_char_byte_len(str->chars, byte_len, byte_offset);
        memcpy(dst, str->chars + byte_offset, char_bytes);
        dst += char_bytes;
    }
    *dst = '\0';
    result->char_len = char_len;
    result->hash = hash_string(result->chars, byte_len);
    
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
    result->char_len = str->char_len * n;
    result->hash = hash_string(result->chars, (int)total_len);
    
    return val_obj((Object*)result);
}

// 8. 新增：获取字符的ASCII码值

static Value str_byte(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    int len = str->len;
    
    // 默认获取第1个字符（0-based索引）
    int pos = 0;
    if (argc >= 2) {
        pos = val_as_int(args[1]);
    }
    
    // 处理负数索引（-1表示最后一个字符）
    if (pos < 0) pos = len + pos;
    
    // 边界检查
    if (pos < 0 || pos >= len) {
        return val_null();  // 越界返回null
    }
    
    return val_int((unsigned char)str->chars[pos]);
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
    result->char_len = argc;  // ASCII字符数 = 字节数
    result->hash = hash_string(result->chars, argc);
    
    return val_obj((Object*)result);
}

// 10. 新增：查找子串位置

static Value str_find(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern = (ObjString*)val_as_obj(args[1]);
    
    // 起始位置（可选，默认为0，0-indexed，字符索引）
    int start = 0;
    if (argc >= 3) {
        start = val_as_int(args[2]);
    }
    
    // 是否使用纯文本匹配（可选，默认为false）
    bool plain = false;
    if (argc >= 4 && val_is_bool(args[3])) {
        plain = val_as_bool(args[3]);
    }
    
    // 处理负数起始位置（字符索引）
    int char_len = str->char_len;
    if (start < 0) start = char_len + start;
    if (start < 0) start = 0;
    
    if (start > char_len) {
        return val_int(-1);
    }
    
    if (pattern->len == 0) {
        // 空模式匹配起始位置
        return val_int(start);
    }
    
    // 将字符起始位置转换为字节偏移
    int byte_start = utf8_char_offset(str->chars, str->len, start);
    
    if (plain) {
        // 纯文本查找（不使用模式匹配）
        const char* found = strstr(str->chars + byte_start, pattern->chars);
        if (found) {
            int byte_pos = (int)(found - str->chars);
            // 将字节偏移转换为字符索引
            return val_int(utf8_char_len(str->chars, byte_pos));
        }
    } else {
        // 简单模式匹配（支持 ^ 开头和 $ 结尾）
        if (pattern->len == 2 && pattern->chars[0] == '^') {
            // 匹配开头
            char target = pattern->chars[1];
            if (byte_start < str->len && str->chars[byte_start] == target) {
                return val_int(start);
            }
        } else if (pattern->len == 2 && pattern->chars[1] == '$') {
            // 匹配结尾
            char target = pattern->chars[0];
            if (str->len > 0 && str->chars[str->len - 1] == target) {
                return val_int(char_len - 1);
            }
        } else {
            // 普通子串查找
            const char* found = strstr(str->chars + byte_start, pattern->chars);
            if (found) {
                int byte_pos = (int)(found - str->chars);
                // 将字节偏移转换为字符索引
                return val_int(utf8_char_len(str->chars, byte_pos));
            }
        }
    }
    
    return val_int(-1);  // 未找到返回 -1
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
    
    // 辅助宏：确保 result 缓冲区有足够空间
    #define ENSURE_BUF(need) do { \
        while (result_len + (need) >= buf_size) { \
            buf_size *= 2; \
            char* _nb = (char*)realloc(result, buf_size); \
            if (!_nb) { free(result); return val_null(); } \
            result = _nb; \
        } \
    } while(0)
    
    // 辅助宏：将 temp_buf 内容追加到 result
    #define APPEND_TEMP() do { \
        ENSURE_BUF(temp_len); \
        memcpy(result + result_len, temp_buf, temp_len); \
        result_len += temp_len; \
    } while(0)
    
    // 辅助函数：将字符串直接写入 result（不经过 temp_buf，避免截断）
    // 追加一段已知长度的字符串到 result
    #define APPEND_STR(s, slen) do { \
        ENSURE_BUF(slen); \
        memcpy(result + result_len, (s), slen); \
        result_len += slen; \
    } while(0)
    
    for (int i = 0; i < fmt_len; i++) {
        if (fmt_str[i] != '%') {
            // 普通字符，直接复制
            ENSURE_BUF(1);
            result[result_len++] = fmt_str[i];
            continue;
        }
        
        // 遇到 %，解析格式说明符
        i++;
        if (i >= fmt_len) break;
        
        // 解析标志位：'-'(左对齐), '0'(零填充), '+'(显示正号), ' '(空格替代正号), '#'(替代形式)
        bool flag_minus = false;
        bool flag_zero = false;
        bool flag_plus = false;
        bool flag_space = false;
        bool flag_hash = false;
        
        while (i < fmt_len) {
            switch (fmt_str[i]) {
                case '-': flag_minus = true; i++; continue;
                case '0': flag_zero = true; i++; continue;
                case '+': flag_plus = true; i++; continue;
                case ' ': flag_space = true; i++; continue;
                case '#': flag_hash = true; i++; continue;
                default: break;
            }
            break;
        }
        if (i >= fmt_len) break;
        
        // 左对齐时忽略零填充
        if (flag_minus) flag_zero = false;
        
        // 解析宽度
        int width = 0;
        bool has_width = false;
        while (i < fmt_len && fmt_str[i] >= '0' && fmt_str[i] <= '9') {
            has_width = true;
            width = width * 10 + (fmt_str[i] - '0');
            i++;
        }
        if (i >= fmt_len) break;
        
        // 解析精度
        int precision = -1;  // -1 表示未指定
        if (fmt_str[i] == '.') {
            i++;
            precision = 0;
            while (i < fmt_len && fmt_str[i] >= '0' && fmt_str[i] <= '9') {
                precision = precision * 10 + (fmt_str[i] - '0');
                i++;
            }
        }
        if (i >= fmt_len) break;
        
        char spec = fmt_str[i];
        
        // 构造 printf 风格的格式字符串
        char printf_fmt[64];
        int pf_len = 0;
        printf_fmt[pf_len++] = '%';
        if (flag_minus) printf_fmt[pf_len++] = '-';
        if (flag_plus) printf_fmt[pf_len++] = '+';
        if (flag_space) printf_fmt[pf_len++] = ' ';
        if (flag_hash) printf_fmt[pf_len++] = '#';
        if (flag_zero) printf_fmt[pf_len++] = '0';
        if (has_width) {
            pf_len += snprintf(printf_fmt + pf_len, sizeof(printf_fmt) - pf_len, "%d", width);
        }
        if (precision >= 0) {
            pf_len += snprintf(printf_fmt + pf_len, sizeof(printf_fmt) - pf_len, ".%d", precision);
        }
        // 预留1字节给 spec + '\0'
        if (pf_len + 2 > (int)sizeof(printf_fmt)) {
            // 格式串太长，回退简单输出
            ENSURE_BUF(2);
            result[result_len++] = '%';
            result[result_len++] = spec;
            continue;
        }
        
        // 用于格式化输出的临时缓冲区（足够大以容纳宽格式化结果）
        char temp_buf[1024];
        int temp_len = 0;
        
        switch (spec) {
            case 's': {
                // 字符串
                if (arg_idx < argc && val_is_obj(args[arg_idx]) &&
                    val_as_obj(args[arg_idx])->type == OBJ_STRING) {
                    ObjString* s = (ObjString*)val_as_obj(args[arg_idx]);
                    int slen = s->len;
                    const char* schars = s->chars;
                    
                    // 精度限制字符串长度
                    if (precision >= 0 && slen > precision) {
                        slen = precision;
                    }
                    
                    // 宽度填充
                    if (has_width && slen < width) {
                        int pad = width - slen;
                        if (flag_minus) {
                            // 左对齐：先字符串后空格
                            APPEND_STR(schars, slen);
                            for (int p = 0; p < pad; p++) {
                                ENSURE_BUF(1);
                                result[result_len++] = ' ';
                            }
                        } else {
                            // 右对齐：先空格后字符串
                            for (int p = 0; p < pad; p++) {
                                ENSURE_BUF(1);
                                result[result_len++] = ' ';
                            }
                            APPEND_STR(schars, slen);
                        }
                    } else {
                        APPEND_STR(schars, slen);
                    }
                } else if (arg_idx < argc) {
                    // 非字符串参数，尝试数值转字符串
                    if (val_is_int(args[arg_idx])) {
                        printf_fmt[pf_len++] = 'd';
                        printf_fmt[pf_len] = '\0';
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_int(args[arg_idx]));
                        APPEND_TEMP();
                    } else if (val_is_float(args[arg_idx])) {
                        printf_fmt[pf_len++] = 'f';
                        printf_fmt[pf_len] = '\0';
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_num(args[arg_idx]));
                        APPEND_TEMP();
                    } else if (val_is_bool(args[arg_idx])) {
                        const char* bs = val_as_bool(args[arg_idx]) ? "true" : "false";
                        int blen = val_as_bool(args[arg_idx]) ? 4 : 5;
                        if (precision >= 0 && blen > precision) blen = precision;
                        if (has_width && blen < width) {
                            int pad = width - blen;
                            if (flag_minus) {
                                APPEND_STR(bs, blen);
                                for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = ' '; }
                            } else {
                                for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = ' '; }
                                APPEND_STR(bs, blen);
                            }
                        } else {
                            APPEND_STR(bs, blen);
                        }
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<value>");
                        APPEND_TEMP();
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    APPEND_TEMP();
                }
                arg_idx++;
                break;
            }
            
            case 'd':
            case 'i': {
                // 整数
                printf_fmt[pf_len++] = 'd';
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    if (val_is_int(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_int(args[arg_idx]));
                    } else if (val_is_float(args[arg_idx])) {
                        // float → int 截断
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, (int)val_as_num(args[arg_idx]));
                    } else if (val_is_bool(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_bool(args[arg_idx]) ? 1 : 0);
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<type_error>");
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'u': {
                // 无符号整数
                printf_fmt[pf_len++] = 'u';
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    if (val_is_int(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, (unsigned int)val_as_int(args[arg_idx]));
                    } else if (val_is_float(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, (unsigned int)val_as_num(args[arg_idx]));
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<type_error>");
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'f': {
                // 浮点数（支持 int、float 和 BigInt）
                printf_fmt[pf_len++] = 'f';
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    double val;
                    if (val_is_int(args[arg_idx])) {
                        val = (double)val_as_int(args[arg_idx]);
                    } else if (val_is_float(args[arg_idx])) {
                        val = val_as_num(args[arg_idx]);
                    } else if (val_is_bigint(args[arg_idx])) {
                        val = bigint_to_double(val_as_bigint(args[arg_idx]));
                    } else if (val_is_bool(args[arg_idx])) {
                        val = val_as_bool(args[arg_idx]) ? 1.0 : 0.0;
                    } else {
                        val = 0;
                    }
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val);
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'e':
            case 'E': {
                // 科学计数法
                printf_fmt[pf_len++] = spec;
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    double val;
                    if (val_is_int(args[arg_idx])) {
                        val = (double)val_as_int(args[arg_idx]);
                    } else if (val_is_float(args[arg_idx])) {
                        val = val_as_num(args[arg_idx]);
                    } else if (val_is_bigint(args[arg_idx])) {
                        val = bigint_to_double(val_as_bigint(args[arg_idx]));
                    } else if (val_is_bool(args[arg_idx])) {
                        val = val_as_bool(args[arg_idx]) ? 1.0 : 0.0;
                    } else {
                        val = 0;
                    }
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val);
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'g':
            case 'G': {
                // 自动选择 %f 或 %e
                printf_fmt[pf_len++] = spec;
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    double val;
                    if (val_is_int(args[arg_idx])) {
                        val = (double)val_as_int(args[arg_idx]);
                    } else if (val_is_float(args[arg_idx])) {
                        val = val_as_num(args[arg_idx]);
                    } else if (val_is_bigint(args[arg_idx])) {
                        val = bigint_to_double(val_as_bigint(args[arg_idx]));
                    } else if (val_is_bool(args[arg_idx])) {
                        val = val_as_bool(args[arg_idx]) ? 1.0 : 0.0;
                    } else {
                        val = 0;
                    }
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val);
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'c': {
                // 字符
                if (arg_idx < argc) {
                    int val = 0;
                    if (val_is_int(args[arg_idx])) {
                        val = val_as_int(args[arg_idx]);
                    } else if (val_is_float(args[arg_idx])) {
                        val = (int)val_as_num(args[arg_idx]);
                    }
                    ENSURE_BUF(1);
                    result[result_len++] = (char)val;
                } else {
                    ENSURE_BUF(1);
                    result[result_len++] = '?';
                }
                arg_idx++;
                break;
            }
            
            case 'x':
            case 'X': {
                // 十六进制
                printf_fmt[pf_len++] = spec;
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    if (val_is_int(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_int(args[arg_idx]));
                    } else if (val_is_float(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, (int)val_as_num(args[arg_idx]));
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<type_error>");
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'o': {
                // 八进制
                printf_fmt[pf_len++] = 'o';
                printf_fmt[pf_len] = '\0';
                if (arg_idx < argc) {
                    if (val_is_int(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, val_as_int(args[arg_idx]));
                    } else if (val_is_float(args[arg_idx])) {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), printf_fmt, (int)val_as_num(args[arg_idx]));
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<type_error>");
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                }
                APPEND_TEMP();
                arg_idx++;
                break;
            }
            
            case 'b': {
                // 二进制
                if (arg_idx < argc) {
                    unsigned int val = 0;
                    if (val_is_int(args[arg_idx])) {
                        val = (unsigned int)val_as_int(args[arg_idx]);
                    } else if (val_is_float(args[arg_idx])) {
                        val = (unsigned int)val_as_num(args[arg_idx]);
                    } else {
                        temp_len = snprintf(temp_buf, sizeof(temp_buf), "<type_error>");
                        APPEND_TEMP();
                        arg_idx++;
                        break;
                    }
                    
                    // 计算二进制位数
                    if (val == 0) {
                        temp_buf[0] = '0';
                        temp_len = 1;
                    } else {
                        temp_len = 0;
                        unsigned int v = val;
                        while (v > 0) { temp_len++; v >>= 1; }
                        // 写入二进制（从高位到低位）
                        for (int bit = temp_len - 1; bit >= 0; bit--) {
                            temp_buf[bit] = (val & 1) ? '1' : '0';
                            val >>= 1;
                        }
                    }
                    
                    // # 标志：添加 0b 前缀
                    int prefix_len = flag_hash ? 2 : 0;
                    // 宽度填充
                    if (has_width && temp_len + prefix_len < width) {
                        int pad = width - temp_len - prefix_len;
                        if (flag_minus) {
                            // 左对齐
                            if (flag_hash) { APPEND_STR("0b", 2); }
                            APPEND_TEMP();
                            for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = ' '; }
                        } else {
                            // 右对齐
                            char fill_char = flag_zero ? '0' : ' ';
                            if (flag_zero && flag_hash) {
                                APPEND_STR("0b", 2);
                            }
                            for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = fill_char; }
                            if (!flag_zero && flag_hash) {
                                APPEND_STR("0b", 2);
                            }
                            APPEND_TEMP();
                        }
                    } else {
                        if (flag_hash) { APPEND_STR("0b", 2); }
                        APPEND_TEMP();
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    APPEND_TEMP();
                }
                arg_idx++;
                break;
            }
            
            case 't': {
                // 布尔值
                if (arg_idx < argc) {
                    const char* bs;
                    int blen;
                    if (val_is_bool(args[arg_idx])) {
                        bs = val_as_bool(args[arg_idx]) ? "true" : "false";
                        blen = val_as_bool(args[arg_idx]) ? 4 : 5;
                    } else {
                        // 非布尔值，按真值判断
                        bool truthy = false;
                        if (val_is_int(args[arg_idx])) truthy = (val_as_int(args[arg_idx]) != 0);
                        else if (val_is_float(args[arg_idx])) truthy = (val_as_num(args[arg_idx]) != 0.0);
                        else truthy = true;
                        bs = truthy ? "true" : "false";
                        blen = truthy ? 4 : 5;
                    }
                    
                    // 精度限制
                    if (precision >= 0 && blen > precision) blen = precision;
                    
                    // 宽度填充
                    if (has_width && blen < width) {
                        int pad = width - blen;
                        if (flag_minus) {
                            APPEND_STR(bs, blen);
                            for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = ' '; }
                        } else {
                            for (int p = 0; p < pad; p++) { ENSURE_BUF(1); result[result_len++] = ' '; }
                            APPEND_STR(bs, blen);
                        }
                    } else {
                        APPEND_STR(bs, blen);
                    }
                } else {
                    temp_len = snprintf(temp_buf, sizeof(temp_buf), "<missing>");
                    APPEND_TEMP();
                }
                arg_idx++;
                break;
            }
            
            case '%': {
                // 转义的百分号
                ENSURE_BUF(1);
                result[result_len++] = '%';
                break;
            }
            
            default: {
                // 未知的格式符，原样输出
                ENSURE_BUF(2);
                result[result_len++] = '%';
                result[result_len++] = spec;
                break;
            }
        }
    }
    
    #undef ENSURE_BUF
    #undef APPEND_TEMP
    #undef APPEND_STR
    
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
    result->char_len = utf8_char_len(result->chars, target_len);
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
    result->char_len = utf8_char_len(result->chars, target_len);
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
    native_register_module_method("strings", "byte_len", str_byte_len, 1, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

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

    // 5b. 字节级切片
    TypeKind byte_slice_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "byte_slice", str_byte_slice, 3, -1, -1, TYPE_STRING, TYPE_UNKNOWN, byte_slice_params);

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
    string_register_method_with_params("byte_len", make_native(str_byte_len, 1, "byte_len"), 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, len_params);

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
    string_register_method_with_params("byte_slice", make_native(str_byte_slice, 3, "byte_slice"), 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, int2_params);

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
    string_register_method_with_params("find", make_native(str_find, 4, "find"), -1, 1, 3, TYPE_INT, TYPE_UNKNOWN, str_int_bool_params);

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
