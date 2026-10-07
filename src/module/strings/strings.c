#include "include/native.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>

// 前向声明：字符串实例方法支持函数（定义在 object_string.c）
extern void string_init_methods(void);
extern void string_register_method(const char* name, ObjNative* method,
                                  TypeKind return_type, TypeKind return_element_type,
                                  NativeParamSpec params);

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

// 10b. 新增：二进制安全的字节序列查找
// 与 find() 的区别：使用 memcmp 而非 strstr，不会在 \x00 处截断
// 返回字节偏移量（非字符索引），start 参数也是字节偏移
static Value str_byte_find(int argc, Value* args) {
    (void)argc;
    ObjString* str = (ObjString*)val_as_obj(args[0]);
    ObjString* pattern = (ObjString*)val_as_obj(args[1]);

    // 起始字节偏移（可选，默认为0）
    int start = 0;
    if (argc >= 3) {
        start = val_as_int(args[2]);
    }

    // 处理负数起始位置（基于字节长度）
    int byte_len = str->len;
    if (start < 0) start = byte_len + start;
    if (start < 0) start = 0;

    if (start > byte_len) {
        return val_int(-1);
    }

    if (pattern->len == 0) {
        // 空模式匹配起始位置
        return val_int(start);
    }

    // 二进制安全搜索：使用 memcmp 逐字节比较
    int max_off = byte_len - pattern->len;
    for (int i = start; i <= max_off; i++) {
        if (memcmp(str->chars + i, pattern->chars, pattern->len) == 0) {
            return val_int(i);
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

// 12b. lines(s) —— 按 '\n' 拆行，并去掉行内的所有 '\r'（CRLF / 老式 CR 文本都能直接用）
//
//   ★ 为什么要有它（2026-10-02，由一次改动的余震引出）：把 `files.read` / `files.write`
//     改成二进制通道后**行尾不再被翻译** ⇒ CRLF 文件读出来每行尾带 `\r`，于是"剥 \r 再 split"
//     这个补丁被**抄到 9 处 / 6 个文件**（sdl_edit ×3、sdl_label、sdl_tooltip、screenshot_hotkey ×2、
//     anim_editor、pvz_art），PvZ 还因此崩在渲染回调里（`_int("12\r")` 抛错 ⇒ 外层 catch 报
//     「渲染异常」直接退出）✗ ⇒ 按"单一事实来源"收成这一个实现：
//     **与 `s.replace("\r", "").split("\n")` 逐字节等价** —— 这不是巧合，而是它的存在理由
//     （各调用点换过来行为完全不变，不必逐处重判 ✓）
//   ★ 语义刻意保留 `str_split` 的原样：结尾有换行会多出一个空串（`"a\n"` → `["a", ""]`）。
//     若改成"丢掉末尾空行"（Python 的 splitlines 那样），各调用点的**循环次数**就变了 ⇒
//     那是偷偷改语义，不是收口 ✗（需要那种语义就在调用点显式处理）
static Value str_lines(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_STRING) {
        native_throw_error("lines 参数必须是字符串");
        return val_null();
    }
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    ObjArray* result = arr_new(8);
    if (!result) return val_null();

    int start = 0;
    for (int i = 0; i <= s->len; i++) {
        if (i < s->len && s->chars[i] != '\n') continue;   // 只在 '\n' 或串尾切

        int seg_len = i - start;
        char* buf = (char*)malloc((size_t)(seg_len > 0 ? seg_len : 1));
        if (!buf) return val_null();
        int n = 0;
        for (int k = 0; k < seg_len; k++) {
            char c = s->chars[start + k];
            if (c != '\r') buf[n++] = c;
        }
        ObjString* line = str_copy(buf, n);
        free(buf);
        if (!line) return val_null();
        arr_push_custom(result, val_obj((Object*)line));
        start = i + 1;
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

// 18. 新增：字节数 → 人类可读大小

// fmt_size(bytes) - 把字节数格式化成人类可读的大小（1024 进制：B / KB / MB / GB）
//   收编自应用层**四份逐字复制**的实现（文件管理器 file_manager/props_dialog、PE分析器、
//   缓存清理工具）⇒ 一处修正全仓受益 ✓
//   · 口径与其中 3 份「GB 带 1 位小数」版一致（另 1 份 props_dialog 是整数 GB 版，
//     切换消费方时那一处显示会从 "1 GB" 变 "1.9 GB" —— 更精确，非回归 ✓）
//   · KB/MB 取整（不保留小数）、GB 保留 1 位小数且为 0 时不显示 ⇒ 替换后界面显示不变 ✓
static Value str_fmt_size(int argc, Value* args) {
    (void)argc;

    // 接受 int（也容错接受 float：应用里常见 _int(...) 已转好，但别因此崩 ✗）
    double b = 0.0;
    if (val_is_int(args[0])) {
        b = (double)val_as_int(args[0]);
    } else if (val_is_float(args[0])) {
        b = val_as_double(args[0]);
    } else {
        return val_null();
    }

    char buf[64];
    if (b < 1024.0) {
        snprintf(buf, sizeof(buf), "%lld B", (long long)b);
    } else if (b < 1048576.0) {
        snprintf(buf, sizeof(buf), "%lld KB", (long long)(b / 1024.0));
    } else if (b < 1073741824.0) {
        snprintf(buf, sizeof(buf), "%lld MB", (long long)(b / 1048576.0));
    } else {
        double gb = b / 1073741824.0;
        long long gb_int = (long long)gb;
        int tenths = (int)((gb - (double)gb_int) * 10.0);
        if (tenths == 0) {
            snprintf(buf, sizeof(buf), "%lld GB", gb_int);
        } else {
            snprintf(buf, sizeof(buf), "%lld.%d GB", gb_int, tenths);
        }
    }

    return val_obj((Object*)str_copy(buf, (int)strlen(buf)));
}

// 19. 新增：16 进制字符串

// hex(value[, width]) - 大写 16 进制字符串
//   收编自应用层**三份逐字复制**的 toHex8 / toHex4 / toHex2（PE分析器 3 个文件）✓
//   · 省略 width ⇒ 最少位数、不补零：hex(0x1F) == "1F" ✓（原来要另写一个函数才行 ✗）
//   · 给出 width ⇒ 恰好 width 位：不足补前导 0，超出按**补码低位**截断：
//       hex(0x10, 1) == "0"（只留低 4 位）· hex(-1, 8) == "FFFFFFFF" ✓
//   · width 夹到 1..64（64 位 = 16 进制 16 位；再宽也只是补前导 0，不报错 ✓）
static Value str_hex(int argc, Value* args) {
    // 值：int 为准，容错接受 float
    int64_t v = 0;
    if (val_is_int(args[0])) {
        v = val_as_int(args[0]);
    } else if (val_is_float(args[0])) {
        v = (int64_t)val_as_double(args[0]);
    } else {
        return val_null();
    }

    // 最少位数（>=1）：本语言的 int 是 48 位有符号，val_as_int 已符号扩展成
    //   64 位补码 ⇒ 这里按 64 位算位数，负数在无 width 时给满 16 位（FFFFFFFFFFFFFFFF ✓）
    int min_digits = 1;
    while (min_digits < 16 && (((uint64_t)v) >> (min_digits * 4)) != 0) {
        min_digits++;
    }

    int width = min_digits;
    if (argc >= 2 && val_is_int(args[1])) {
        width = (int)val_as_int(args[1]);
        if (width < 1) width = 1;
        if (width > 64) width = 64;
    }

    char out[65];
    for (int i = 0; i < width; i++) {
        int idx = width - 1 - i;                       // 高位在前
        int nibble = 0;
        if (idx < 16) {                                // 超出 64 位的高位一律补 0
            nibble = (int)((((uint64_t)v) >> (idx * 4)) & 0xF);
        }
        out[i] = "0123456789ABCDEF"[nibble];
    }
    out[width] = '\0';

    return val_obj((Object*)str_copy(out, width));
}

// ==================== 2026-10-02 新增：字节 ↔ 文本 的桥 + 两个扫描原语 ====================
// 缘起（都是"数出来的重复实现"，见各函数的注释）：
//   · `strings.char(...)` 全仓 **159 处 / 39 文件**，几乎都在逐字节拼串
//     （base64 / AES / vigenere / caesar / PE 分析 / web_html）——而循环里
//     `result += strings.char(b)` 是**平方级**（每次 += 都重新分配整串）⇒ 给"一趟分配"的正路 ✓
//   · "字节串 ↔ hex 文本"在 crypto / PE 里各搓一份（22 处 / 12 文件）
//     ⚠ 与既有 `hex(n, digits)` **分清**：那个是**数字 → hex 文本**；这两个是**字节串 ↔ hex 文本**
//       （同为**大写**，保持模块内一致 ✓）
//   · `to_lower(a) == to_lower(b)` 8 处 / 6 文件（还有只转一侧的写法）⇒ eq_ignore_case
//   · `text.slice(i, i+1)` 逐字符扫描 10 处 / 6 文件（每步分配一个单字符临时串）⇒ codepoint_at

// 单个 hex 字符 → 0-15（大小写都收）；非法 ⇒ -1（由调用方报错，不静默当 0 ✓）
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// to_bytes(s) —— UTF-8 字节序列（元素 0-255）
static Value str_to_bytes(int argc, Value* args) {
    (void)argc;
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    ObjArray* result = arr_new(s->len > 0 ? s->len : 1);
    if (!result) return val_null();
    for (int i = 0; i < s->len; i++) {
        arr_push_custom(result, val_int((unsigned char)s->chars[i]));
    }
    return val_obj((Object*)result);
}

// from_bytes(arr) —— Array[int]（每个 0-255）→ 字节串
//   越界/非 int **响亮报错**（不静默截断、不回绕：那是"静默错值"的经典来源 ✓）
static Value str_from_bytes(int argc, Value* args) {
    (void)argc;
    if (!val_is_obj(args[0]) || val_as_obj(args[0])->type != OBJ_ARRAY) {
        native_throw_error("from_bytes 参数必须是 Array[int]");
        return val_null();
    }
    ObjArray* arr = (ObjArray*)val_as_obj(args[0]);

    char* buf = (char*)malloc((size_t)(arr->count > 0 ? arr->count + 1 : 1));
    if (!buf) { native_throw_error("内存分配失败"); return val_null(); }

    for (int i = 0; i < arr->count; i++) {
        Value v = arr->elements[i];
        if (!val_is_int(v)) {
            char msg[96];
            snprintf(msg, sizeof(msg), "from_bytes 第 %d 个元素不是 int（下标从 0 起）", i);
            free(buf);
            native_throw_error(msg);
            return val_null();
        }
        int64_t b = val_as_int(v);
        if (b < 0 || b > 255) {
            char msg[96];
            snprintf(msg, sizeof(msg), "from_bytes 第 %d 个元素 %lld 超出 0-255",
                     i, (long long)b);
            free(buf);
            native_throw_error(msg);
            return val_null();
        }
        buf[i] = (char)b;
    }
    buf[arr->count] = '\0';

    ObjString* result = str_copy(buf, arr->count);   // str_copy 会算好 len/char_len/hash ✓
    free(buf);
    if (!result) return val_null();
    return val_obj((Object*)result);
}

// to_base64(s, url_safe?) —— 字节串 → base64 文本（RFC 4648）
//
// 为什么进**核心标准库**（2026-10-07）：仓库里原本有**四份各自独立的实现** ——
//   `LenoCrypto/lib/crypto_base64.leno`、`examples/crypto/base64.leno`（前者就是从它搬过去的）、
//   `LenoWeb/lib/web_ws.leno`（WS 握手要 `Sec-WebSocket-Accept` 的编码、截图要解码）、
//   以及音乐下载器 / Trae签到 各一份。而 `web_ws` 这种"做分帧"的模块不该为了编解码去依赖
//   一个**加密库** ⇒ core scalar codec 归核心标准库（与 to_hex/from_hex 同族，正是
//   `docs/单一事实来源与重复实现收敛.md` 的取向）。
//   ★ 同日收口结果：`LenoCrypto/lib/crypto_base64.leno`、`examples/crypto/base64.leno`
//     **已删除**，`LenoWeb/lib/web_ws.leno` 改薄封装，音乐下载器 / Trae签到 直连 `strings.*`
//     ⇒ 四份实现收敛为**这一份** ✓（注释改动，不影响已构建的二进制）
//
// 命名与参数口径**完全照抄 to_hex**：字节串进、ASCII 串出；第二个参数是可选的 `url_safe`
//   （`+`/`/` 换成 `-`/`_`，JWT / data: URL 用得上）。
//   ⚠ 输出**总是**带 `=` padding（最通用）；要 URL-safe 就传 true。
static Value str_to_base64(int argc, Value* args) {
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    int url_safe = 0;
    if (argc >= 2 && !val_is_null(args[1])) {
        url_safe = val_as_bool(args[1]);
    }
    static const char* TBL_STD = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char* TBL_URL = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* tbl = url_safe ? TBL_URL : TBL_STD;

    int n = s->len;
    int out_len = ((n + 2) / 3) * 4;
    char* buf = (char*)malloc((size_t)(out_len > 0 ? out_len : 1) + 1);
    if (!buf) { native_throw_error("内存分配失败"); return val_null(); }

    const unsigned char* p = (const unsigned char*)s->chars;
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        int b0 = p[i];
        int b1 = (i + 1 < n) ? p[i + 1] : 0;
        int b2 = (i + 2 < n) ? p[i + 2] : 0;
        buf[o++] = tbl[b0 >> 2];
        buf[o++] = tbl[((b0 & 0x03) << 4) | (b1 >> 4)];
        buf[o++] = (i + 1 < n) ? tbl[((b1 & 0x0F) << 2) | (b2 >> 6)] : '=';
        buf[o++] = (i + 2 < n) ? tbl[b2 & 0x3F] : '=';
    }
    buf[o] = '\0';

    ObjString* result = str_copy(buf, o);
    free(buf);
    if (!result) return val_null();
    return val_obj((Object*)result);
}

// base64 字符 → 6 位值；不是合法字符 ⇒ -1
//   ⚠ **两种字母表都认**（`+/` 与 `-_`）：JWT / data: URL 用后者，而它们只是同一份数据的
//     不同外壳 —— 让每个调用方自己先做字符替换既啰嗦又容易漏（比如忘了把 `-` 换回去）。
//   用**算的**而不是查表：无需初始化、天然线程安全（本仓有 threads 模块）✓
static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

// from_base64(s) —— base64 文本 → 字节串（RFC 4648）
//
// **宽容**（刻意的取舍，2026-10-07）：
//   · 两种字母表都收（见 b64_val）
//   · **缺 `=` padding** 也收（`"TQ"` 与 `"TQ=="` 等价）
//   · **空白字符忽略**（换行 / 制表 / 空格 —— PEM、MIME、从文件读进来的都带）
// 但**非法字符一律报错、并指出是第几位**（照 B14 的规矩带上坏值）：
//   静默给空串会重演「青衣」那种"看不出为什么没结果"，比报错难查得多。
static Value str_from_base64(int argc, Value* args) {
    (void)argc;
    ObjString* s = (ObjString*)val_as_obj(args[0]);
    const unsigned char* p = (const unsigned char*)s->chars;
    int n = s->len;

    // 输出上界：每 4 个字符最多 3 字节
    unsigned char* out = (unsigned char*)malloc((size_t)(n / 4 + 2) * 3 + 4);
    if (!out) { native_throw_error("内存分配失败"); return val_null(); }

    int o = 0;
    int quad = 0;      // 本组已累积的**有效字符**数（0..3）
    int val = 0;       // 本组累积的位（最多 18 位）
    int pad = 0;       // 见过的 '=' 个数
    for (int i = 0; i < n; i++) {
        unsigned char c = p[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
        if (c == '=') {
            pad++;
            if (pad > 2) {
                free(out);
                native_throw_error("base64 解码失败：'=' 多于 2 个（padding 最多 2 位）");
                return val_null();
            }
            continue;
        }
        if (pad > 0) {
            free(out);
            native_throw_error("base64 解码失败：'=' 之后仍有数据");
            return val_null();
        }
        int v = b64_val(c);
        if (v < 0) {
            char shown[16];
            if (c >= 32 && c < 127) snprintf(shown, sizeof(shown), "'%c'", c);
            else snprintf(shown, sizeof(shown), "\\x%02X", c);
            char msg[128];
            snprintf(msg, sizeof(msg), "base64 解码失败：非法字符 %s（第 %d 个字符）", shown, i + 1);
            free(out);
            native_throw_error(msg);
            return val_null();
        }
        val = (val << 6) | v;
        quad++;
        if (quad == 4) {
            out[o++] = (unsigned char)((val >> 16) & 0xFF);
            out[o++] = (unsigned char)((val >> 8) & 0xFF);
            out[o++] = (unsigned char)(val & 0xFF);
            quad = 0;
            val = 0;
        }
    }

    // 收尾：`quad` 只能是 0 / 2 / 3 —— 余 **1** 个字符表示不出一个字节 ⇒ 长度非法
    if (quad == 1) {
        free(out);
        native_throw_error("base64 解码失败：有效字符数不合法（余 1 个字符无法构成字节）");
        return val_null();
    }
    if (pad > 0 && (quad + pad) % 4 != 0) {
        free(out);
        native_throw_error("base64 解码失败：padding 位数与数据长度不匹配");
        return val_null();
    }
    if (quad == 2) {
        out[o++] = (unsigned char)((val >> 4) & 0xFF);
    } else if (quad == 3) {
        out[o++] = (unsigned char)((val >> 10) & 0xFF);
        out[o++] = (unsigned char)((val >> 2) & 0xFF);
    }

    ObjString* result = str_copy((char*)out, o);
    free(out);
    if (!result) return val_null();
    return val_obj((Object*)result);
}

// to_hex(s, upper?) —— 字节串 → hex 文本（每字节两位）
//   **默认小写**（2026-10-02 收编时定的口径）：仓库里现有的 19 处手写实现
//   （crypto 示例 9 份 `to_hex` + sha/md5/hmac/pbkdf2 里的 `byte_to_hex`）**清一色小写**，
//   且 SHA/MD5 摘要的通行写法也是小写 ⇒ 默认小写才能"换上以后输出一字不变" ✓
//   （原先本函数只输出大写，那样换上去会让 19 处输出全变大写 ✗）
//   `upper=true` 保留大写能力（= 本函数最早的默认行为）。
static Value str_to_hex(int argc, Value* args) {
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    int upper = 0;
    if (argc >= 2 && !val_is_null(args[1])) {
        upper = val_as_bool(args[1]);
    }
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    char* buf = (char*)malloc((size_t)s->len * 2 + 1);
    if (!buf) { native_throw_error("内存分配失败"); return val_null(); }
    for (int i = 0; i < s->len; i++) {
        unsigned char b = (unsigned char)s->chars[i];
        buf[i * 2]     = digits[b >> 4];
        buf[i * 2 + 1] = digits[b & 0xF];
    }
    buf[s->len * 2] = '\0';

    ObjString* result = str_copy(buf, s->len * 2);
    free(buf);
    if (!result) return val_null();
    return val_obj((Object*)result);
}

// from_hex(s) —— hex 文本 → 字节串（大写/小写都收；奇数长度或非 hex 字符 ⇒ 报错并指出位置）
static Value str_from_hex(int argc, Value* args) {
    (void)argc;
    ObjString* s = (ObjString*)val_as_obj(args[0]);
    const char* p = s->chars;

    if (s->len % 2 != 0) {
        native_throw_error("from_hex 要求长度为偶数（每两个字符表示一个字节）");
        return val_null();
    }

    int n = s->len / 2;
    char* buf = (char*)malloc((size_t)(n > 0 ? n + 1 : 1));
    if (!buf) { native_throw_error("内存分配失败"); return val_null(); }

    for (int i = 0; i < n; i++) {
        int hi = hex_val(p[i * 2]);
        int lo = hex_val(p[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            char msg[128];
            snprintf(msg, sizeof(msg), "from_hex 第 %d 个字符 '%c' 不是十六进制数字",
                     (hi < 0 ? i * 2 : i * 2 + 1), (hi < 0 ? p[i * 2] : p[i * 2 + 1]));
            free(buf);
            native_throw_error(msg);
            return val_null();
        }
        buf[i] = (char)((hi << 4) | lo);
    }
    buf[n] = '\0';

    ObjString* result = str_copy(buf, n);
    free(buf);
    if (!result) return val_null();
    return val_obj((Object*)result);
}

// eq_ignore_case(a, b) —— 逐字节 tolower 比较
//   ⚠ 语义 = `to_lower(a) == to_lower(b)`（**同一套映射**：与本文件 str_to_lower 用的
//      `tolower((unsigned char)c)` 逐字节一致 ⇒ 换成它以后行为完全不变，只省两次整串分配 ✓）
//   长度不等直接 false（tolower 不改变字节数 ⇒ 长度不变 ✓）
static Value str_eq_ignore_case(int argc, Value* args) {
    (void)argc;
    ObjString* a = (ObjString*)val_as_obj(args[0]);
    ObjString* b = (ObjString*)val_as_obj(args[1]);
    if (a->len != b->len) return val_bool(false);
    for (int i = 0; i < a->len; i++) {
        int ca = tolower((unsigned char)a->chars[i]);
        int cb = tolower((unsigned char)b->chars[i]);
        if (ca != cb) return val_bool(false);
    }
    return val_bool(true);
}

// codepoint_at(s, i) —— 第 i 个**字符**（0-based，支持负索引）的 Unicode 码点
//   越界 ⇒ null（与 `byte` 同口径 ✓）；返回 int ⇒ 与 ASCII 比较/分类都很便宜、**不分配**
//   ⚠ 现有取字符的两条路都有代价：`s[i]` / `slice(i, i+1)` 都会造一个单字符临时串 ✗
//   （sdl_edit 的词选择、sdl_label 的逐字排版就是这么写的：10 处 / 6 文件）
static Value str_codepoint_at(int argc, Value* args) {
    (void)argc;
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    int ci = 0;
    if (argc >= 2 && val_is_int(args[1])) ci = (int)val_as_int(args[1]);
    int nchars = s->char_len;
    if (ci < 0) ci = nchars + ci;
    if (ci < 0 || ci >= nchars) return val_null();

    int off = utf8_char_offset(s->chars, s->len, ci);
    if (off < 0 || off >= s->len) return val_null();

    const unsigned char* p = (const unsigned char*)(s->chars + off);
    int avail = s->len - off;
    int cp;
    if (p[0] < 0x80) {
        cp = p[0];
    } else if ((p[0] & 0xE0) == 0xC0 && avail >= 2) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    } else if ((p[0] & 0xF0) == 0xE0 && avail >= 3) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    } else if ((p[0] & 0xF8) == 0xF0 && avail >= 4) {
        cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    } else {
        cp = p[0];   // 非法/被截断的序列 ⇒ 原样给首字节值（不崩、也不静默乱猜 ✓）
    }
    return val_int(cp);
}

// to_codepoints(s) —— 一趟扫出全部码点（Array[int]）
//   ★ 为什么有了 codepoint_at 还要它：`codepoint_at(i)` 必须**从头走到第 i 个字符**
//     ⇒ `for i in 0..len { codepoint_at(i) }` 是 **O(n²)**（微基准实测：逐字符扫描只比手写快 ~2x ✗）。
//     要"整串逐字符处理"就用它：一趟 O(n) + 之后按数组索引 ✓
//   （截断/非法序列按单字节推进 ⇒ 既不会死循环，也不静默吃掉后面的字符）
static Value str_to_codepoints(int argc, Value* args) {
    (void)argc;
    ObjString* s = (ObjString*)val_as_obj(args[0]);

    ObjArray* result = arr_new(s->char_len > 0 ? s->char_len : 1);
    if (!result) return val_null();

    const unsigned char* p = (const unsigned char*)s->chars;
    int i = 0;
    while (i < s->len) {
        int cp = 0;
        int adv = 1;
        if (p[i] < 0x80) {
            cp = p[i];
        } else if ((p[i] & 0xE0) == 0xC0 && i + 1 < s->len) {
            cp = ((p[i] & 0x1F) << 6) | (p[i + 1] & 0x3F);
            adv = 2;
        } else if ((p[i] & 0xF0) == 0xE0 && i + 2 < s->len) {
            cp = ((p[i] & 0x0F) << 12) | ((p[i + 1] & 0x3F) << 6) | (p[i + 2] & 0x3F);
            adv = 3;
        } else if ((p[i] & 0xF8) == 0xF0 && i + 3 < s->len) {
            cp = ((p[i] & 0x07) << 18) | ((p[i + 1] & 0x3F) << 12) |
                 ((p[i + 2] & 0x3F) << 6) | (p[i + 3] & 0x3F);
            adv = 4;
        } else {
            cp = p[i];   // 非法/被截断 ⇒ 原样给首字节值（不崩、不乱猜 ✓）
        }
        arr_push_custom(result, val_int(cp));
        i += adv;
    }
    return val_obj((Object*)result);
}

// from_codepoint(cp) —— Unicode 码点 → 单字符字符串（`to_codepoints` 的**逆操作**）
//   为什么补它：`to_codepoints` 能"拆"不能"装"，而 `strings.char` 只收 0-255（ASCII 码值）
//   ⇒ 需要**逐字符字符串**的场景（如逐字测宽 `measureString`）只能退回 `slice(k, k+1)`
//   （每次从头扫到第 k 个字符，整段 O(n²)，且每个字符还造一个临时串 ✗）
//   范围校验（**不静默错值**，与 from_hex / from_bytes 同一口径）：
//     · 负数 / 大于 0x10FFFF ⇒ 抛错（不是合法 Unicode 码点）
//     · 0xD800..0xDFFF（UTF-16 代理区）⇒ 抛错（UTF-8 里不允许出现）
//   合法值按 UTF-8 编成 1..4 字节 ⇒ 与 `to_codepoints` 严格互逆 ✓
//   模块式 strings.from_codepoint(cp)：cp 在 args[0]；
//   实例式 s.from_codepoint(cp)：接收者 args[0]、cp 在 args[1]（接收者被忽略，只为与 to_codepoints 配对/可发现 ✓）
static Value str_from_codepoint(int argc, Value* args) {
    int argi = (argc >= 2) ? 1 : 0;
    if (!val_is_int(args[argi])) {
        native_throw_error("from_codepoint 需要一个 int 码点");
        return val_null();
    }
    int64_t cp = val_as_int(args[argi]);
    if (cp < 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
        native_throw_error("from_codepoint: 非法码点（须在 0..0x10FFFF，且不能是代理区 0xD800..0xDFFF）");
        return val_null();
    }

    char buf[4];
    int n = 0;
    if (cp < 0x80) {
        buf[n++] = (char)cp;
    } else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xF0 | (cp >> 18));
        buf[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }

    ObjString* s = str_alloc(n);
    if (!s) { native_throw_error("内存分配失败"); return val_null(); }
    memcpy(s->chars, buf, (size_t)n);
    s->chars[n] = '\0';
    s->char_len = 1;
    s->hash = hash_string(s->chars, n);
    return val_obj((Object*)s);
}

// ==================== 全局函数适配器层 ====================

// format(fmt, ...) - 全局格式化函数
static Value native_format(int argc, Value* args) {
    return str_format(argc, args);
}

// ==================== 初始化 ====================

void strings_init_module(void) {
    // 注册字符串模块方法（模块名，方法名，函数指针，参数数量，返回类型规格，参数类型数组）
    TypeKind len_params[] = {TYPE_STRING};
    native_register_module_method("strings", "len", str_len, &NATIVE_T_INT, NATIVE_FIXED(len_params));
    native_register_module_method("strings", "byte_len", str_byte_len, &NATIVE_T_INT, NATIVE_FIXED(len_params));

    // 1. 大小写转换
    TypeKind upper_params[] = {TYPE_STRING};
    native_register_module_method("strings", "to_upper", str_to_upper, &NATIVE_T_STRING, NATIVE_FIXED(upper_params));

    TypeKind lower_params[] = {TYPE_STRING};
    native_register_module_method("strings", "to_lower", str_to_lower, &NATIVE_T_STRING, NATIVE_FIXED(lower_params));

    // 2. 修剪空白
    TypeKind trim_params[] = {TYPE_STRING};
    native_register_module_method("strings", "trim", str_trim, &NATIVE_T_STRING, NATIVE_FIXED(trim_params));
    native_register_module_method("strings", "trim_start", str_trim_start, &NATIVE_T_STRING, NATIVE_FIXED(trim_params));
    native_register_module_method("strings", "trim_end", str_trim_end, &NATIVE_T_STRING, NATIVE_FIXED(trim_params));

    // 3. 包含检查
    TypeKind starts_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "starts_with", str_starts_with, &NATIVE_T_BOOL, NATIVE_FIXED(starts_params));

    TypeKind ends_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "ends_with", str_ends_with, &NATIVE_T_BOOL, NATIVE_FIXED(ends_params));

    // 4. 查找替换
    TypeKind replace_params[] = {TYPE_STRING, TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "replace", str_replace, &NATIVE_T_STRING, NATIVE_FIXED(replace_params));

    // 5. 子串提取
    TypeKind slice_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "slice", str_slice, &NATIVE_T_STRING, NATIVE_FIXED(slice_params));

    TypeKind substr_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "sub_str", str_sub_str, &NATIVE_T_STRING, NATIVE_FIXED(substr_params));

    // 5b. 字节级切片
    TypeKind byte_slice_params[] = {TYPE_STRING, TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "byte_slice", str_byte_slice, &NATIVE_T_STRING, NATIVE_FIXED(byte_slice_params));

    // 6. 新增：字符串反转
    TypeKind reverse_params[] = {TYPE_STRING};
    native_register_module_method("strings", "reverse", str_reverse, &NATIVE_T_STRING, NATIVE_FIXED(reverse_params));

    // 7. 新增：重复字符串
    TypeKind rep_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("strings", "rep", str_rep, &NATIVE_T_STRING, NATIVE_FIXED(rep_params));

    // 8. 新增：获取字符的ASCII码值（支持1-2个可变参数）
    TypeKind byte_params[] = {TYPE_STRING, TYPE_INT};
    native_register_module_method("strings", "byte", str_byte, &NATIVE_T_INT, NATIVE_VARARG(1, 2, 2, byte_params, TYPE_ANY));

    // 9. 新增：从ASCII码创建字符串（可变参数）
    // 参数类型 int（v3.2.8）：每个实参都是**码值**（0-255）—— 此前是 any
    //   ⇒ 连 `strings.char("x")` 这种都能编过、运行期才炸（实测）。
    //   (2026-10-02) 改由 `native_register_module_method_vararg` **一次说全**：
    //     前缀 1 个 int、其余（超出部分）也按 int ⇒ 不再有"事后补声明"那一步 ✓
    TypeKind char_params[] = {TYPE_INT};
    native_register_module_method("strings", "char", str_char, &NATIVE_T_STRING, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 1, char_params, TYPE_INT));

    // 10. 新增：查找子串位置（支持2-4个可变参数）
    TypeKind find_params[] = {TYPE_STRING, TYPE_STRING, TYPE_INT, TYPE_BOOL};
    native_register_module_method("strings", "find", str_find, &NATIVE_T_INT, NATIVE_VARARG(2, 4, 4, find_params, TYPE_ANY));

    // 10b. 新增：二进制安全的字节序列查找（支持2-3个可变参数）
    TypeKind byte_find_params[] = {TYPE_STRING, TYPE_STRING, TYPE_INT};
    native_register_module_method("strings", "byte_find", str_byte_find, &NATIVE_T_INT, NATIVE_VARARG(2, 3, 3, byte_find_params, TYPE_ANY));

    // 11. 新增：字符串格式化（可变参数）
    TypeKind format_params[] = {TYPE_STRING};
    native_register_module_method("strings", "format", str_format, &NATIVE_T_STRING, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 1, format_params, TYPE_ANY));

    // 12. 新增：字符串分割
    TypeKind split_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "split", str_split, &NATIVE_T_ARR_STRING, NATIVE_FIXED(split_params));
    // 12b. 按行拆分（剥 \r）—— 收口 "replace(\r).split(\n)" 的 9 处重复（见 str_lines 的注释）
    TypeKind lines_params[] = {TYPE_STRING};
    native_register_module_method("strings", "lines", str_lines, &NATIVE_T_ARR_STRING, NATIVE_FIXED(lines_params));

    // 13. 新增：数组连接
    TypeKind join_params[] = {TYPE_ARRAY, TYPE_STRING};
    native_register_module_method("strings", "join", str_join, &NATIVE_T_STRING, NATIVE_FIXED(join_params));

    // 14. 新增：包含检查
    TypeKind has_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "has", str_has, &NATIVE_T_BOOL, NATIVE_FIXED(has_params));

    // 15. 新增：统计子串出现次数
    TypeKind count_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "count", str_count, &NATIVE_T_INT, NATIVE_FIXED(count_params));

    // 16. 新增：左侧填充
    TypeKind pad_params[] = {TYPE_STRING, TYPE_INT, TYPE_STRING};
    native_register_module_method("strings", "pad_start", str_pad_start, &NATIVE_T_STRING, NATIVE_VARARG(2, 3, 3, pad_params, TYPE_ANY));

    // 17. 新增：右侧填充
    native_register_module_method("strings", "pad_end", str_pad_end, &NATIVE_T_STRING, NATIVE_VARARG(2, 3, 3, pad_params, TYPE_ANY));

    // 18. 新增：字节数 → 人类可读大小（收编应用层 4 份复制实现）
    TypeKind fmt_size_params[] = {TYPE_INT};
    native_register_module_method("strings", "fmt_size", str_fmt_size, &NATIVE_T_STRING, NATIVE_FIXED(fmt_size_params));

    // 19. 新增：16 进制字符串（收编应用层 toHex8/toHex4/toHex2；width 可省略 ⇒ 不补零）
    TypeKind hex_params[] = {TYPE_INT, TYPE_INT};
    native_register_module_method("strings", "hex", str_hex, &NATIVE_T_STRING, NATIVE_VARARG(1, 2, 2, hex_params, TYPE_ANY));

    // 20. 字节 ↔ 文本 的桥 + 扫描原语（2026-10-02；见 str_to_bytes 一族的注释）
    TypeKind bytes_str_params[] = {TYPE_STRING};
    native_register_module_method("strings", "to_bytes", str_to_bytes, &NATIVE_T_ARR_INT, NATIVE_FIXED(bytes_str_params));
    TypeKind bytes_arr_params[] = {TYPE_ARRAY};
    native_register_module_method("strings", "from_bytes", str_from_bytes, &NATIVE_T_STRING, NATIVE_FIXED(bytes_arr_params));
    // `to_hex` 的第二个参数是**可选**的 upper（默认小写；见 str_to_hex 的注释）⇒ 按"可变参数"注册
    //   （个数 1..2；类型随注册**一次说全**：{接收者: string, upper: bool} ✓）
    TypeKind tohex_params[] = {TYPE_STRING, TYPE_BOOL};
    native_register_module_method("strings", "to_hex", str_to_hex, &NATIVE_T_STRING, NATIVE_VARARG(1, 2, 2, tohex_params, TYPE_ANY));
    native_register_module_method("strings", "from_hex", str_from_hex, &NATIVE_T_STRING, NATIVE_FIXED(bytes_str_params));
    // base64（2026-10-07）：与 to_hex 同族 —— 可选第二参数也用"可变参数"注册（个数 1..2）
    TypeKind b64_params[] = {TYPE_STRING, TYPE_BOOL};
    native_register_module_method("strings", "to_base64", str_to_base64, &NATIVE_T_STRING, NATIVE_VARARG(1, 2, 2, b64_params, TYPE_ANY));
    native_register_module_method("strings", "from_base64", str_from_base64, &NATIVE_T_STRING, NATIVE_FIXED(bytes_str_params));
    TypeKind two_str_check_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("strings", "eq_ignore_case", str_eq_ignore_case, &NATIVE_T_BOOL, NATIVE_FIXED(two_str_check_params));
    TypeKind cp_at_params[] = {TYPE_STRING, TYPE_INT};
    // ⚠ `codepoint_at` 原先传了 cp_at_params 却**没配**事后声明 ⇒ 那两个类型一直被静默忽略；
    //   换用一次说全的入口后它们**真正生效**了（{接收者: string, pos: int} ✓ 正是它的签名）✓
    native_register_module_method("strings", "codepoint_at", str_codepoint_at, &NATIVE_T_INT, NATIVE_VARARG(1, 2, 2, cp_at_params, TYPE_ANY));
    native_register_module_method("strings", "to_codepoints", str_to_codepoints, &NATIVE_T_ARR_INT, NATIVE_FIXED(bytes_str_params));
    // from_codepoint(cp)：`to_codepoints` 的**逆操作**（码点 → 单字符 UTF-8 串；不扫全串 ⇒ O(1)）
    //   补它的原因：`to_codepoints` 能拆不能装，导致"逐字符要字符串"的场景只能退回 `slice(k,k+1)`
    //   （O(n) 扫描 ⇒ 整段 O(n²)，见 sdl_label/sdl_edit 的逐字测宽）✗
    TypeKind from_cp_params[] = {TYPE_INT};
    native_register_module_method("strings", "from_codepoint", str_from_codepoint, &NATIVE_T_STRING, NATIVE_FIXED(from_cp_params));

    // (2026-10-02) 这里原有 8 行 `native_set_method_vararg_params(...)` 与一段 v3.2.7 的说明 ——
    //   那是"两步式"里的第二步（先按 `native_register_module_method_spec` 注册，之后补声明类型）。
    //   现已全部并入上面的 `native_register_module_method_vararg(...)`：**个数范围 + 前缀类型 +
    //   尾部类型一次说全** ⇒ 该接口已删除，"忘补声明就静默失去检查"这个失败模式随之消失 ✓
}

// 初始化全局函数（程序启动时调用）
void strings_init_globals(void) {
    // 注册全局 format 函数（可变参数，至少 1 个，无上限）
    vm_register_native("format", native_format, TYPE_STRING, TYPE_UNKNOWN, NATIVE_VARARG(1, NATIVE_ARITY_ANY, 0, NULL, TYPE_ANY));
}

void strings_init_instance_methods(void) {
    string_init_methods();
    // 注册实例方法：方法名, 方法对象, 参数个数(不含receiver), 返回类型, 参数类型
    string_register_method("len", make_native(str_len, 1, "len"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    string_register_method("byte_len", make_native(str_byte_len, 1, "byte_len"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 1. 大小写转换
    string_register_method("to_upper", make_native(str_to_upper, 1, "to_upper"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    string_register_method("to_lower", make_native(str_to_lower, 1, "to_lower"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 2. 修剪空白
    string_register_method("trim", make_native(str_trim, 1, "trim"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    string_register_method("trim_start", make_native(str_trim_start, 1, "trim_start"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    string_register_method("trim_end", make_native(str_trim_end, 1, "trim_end"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 3. 包含检查
    TypeKind str_params[] = {TYPE_STRING};
    string_register_method("starts_with", make_native(str_starts_with, 2, "starts_with"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(str_params));
    string_register_method("ends_with", make_native(str_ends_with, 2, "ends_with"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(str_params));

    // 4. 查找替换
    TypeKind replace2_params[] = {TYPE_STRING, TYPE_STRING};
    string_register_method("replace", make_native(str_replace, 3, "replace"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(replace2_params));

    // 5. 子串提取
    TypeKind int2_params[] = {TYPE_INT, TYPE_INT};
    string_register_method("slice", make_native(str_slice, 3, "slice"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(int2_params));
    string_register_method("sub_str", make_native(str_sub_str, 3, "sub_str"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(int2_params));
    string_register_method("byte_slice", make_native(str_byte_slice, 3, "byte_slice"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(int2_params));

    // 6. 新增：字符串反转（无参数实例方法）
    string_register_method("reverse", make_native(str_reverse, 1, "reverse"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));

    // 7. 新增：重复字符串
    TypeKind int_params[] = {TYPE_INT};
    string_register_method("rep", make_native(str_rep, 2, "rep"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(int_params));

    // 8. 新增：获取字符的ASCII码值
    string_register_method("byte", make_native(str_byte, 2, "byte"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED(int_params));
    // byte() 无参数版本使用默认值0（第1个字符）

    // 10. 新增：查找子串位置
    TypeKind str_int_bool_params[] = {TYPE_STRING, TYPE_INT, TYPE_BOOL};
    string_register_method("find", make_native(str_find, 4, "find"), TYPE_INT, TYPE_UNKNOWN, NATIVE_VARARG(1, 3, 3, str_int_bool_params, TYPE_ANY));

    // 10b. 新增：二进制安全的字节序列查找（实例方法）
    TypeKind str_int_find_params[] = {TYPE_STRING, TYPE_INT};
    string_register_method("byte_find", make_native(str_byte_find, 3, "byte_find"), TYPE_INT, TYPE_UNKNOWN, NATIVE_VARARG(1, 2, 2, str_int_find_params, TYPE_ANY));

    // 12. 新增：字符串分割（实例方法）
    TypeKind split_sep_params[] = {TYPE_STRING};
    string_register_method("split", make_native(str_split, 2, "split"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED(split_sep_params));
    // `s.split(sep)` 返回 `Array[string]`（v3.2.6）：两个分支（按分隔符 / 无分隔符逐字符）
    //   装的都是 `str_copy` 出来的**子串** ⇒ 元素类型是 string，不是 any ⇒ 取出来不必收窄。
    //   原来注册成 `TYPE_ARRAY + TYPE_UNKNOWN`（裸 Array）⇒ `s.split(",")[0]` 是 any。
    native_register_instance_method_return_spec("string", "split", &NATIVE_T_ARR_STRING);

    // 12b. 按行拆分（实例方法）—— 与 `strings.lines` 共用同一个实现（见 str_lines 的注释）
    //   注册元素类型 `Array[string]`（同 split 的理由：装的都是 str_copy 出来的子串 ✓）
    string_register_method("lines", make_native(str_lines, 1, "lines"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("string", "lines", &NATIVE_T_ARR_STRING);

    // 20. 字节 ↔ 文本 的桥 + 扫描原语（实例方法，2026-10-02；与模块式**共用同一个实现**）
    //   `s.to_bytes()` / `s.to_hex()` / `s.from_hex()` / `a.eq_ignore_case(b)` / `s.codepoint_at(i)`
    //   （from_bytes 只做模块式：它的接收者是数组，不是字符串）
    string_register_method("to_bytes", make_native(str_to_bytes, 1, "to_bytes"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("string", "to_bytes", &NATIVE_T_ARR_INT);
    // arity 必须传 **-1**：实例方法的检查器里 `arity >= 0` 表示**精确匹配**（min/max 被忽略 ✗，
    //   见 visit_expr.inc:892），只有可变参数（arity == NATIVE_ARITY_VARARG）才按 min/max 放行可选参数 ✓
    TypeKind tohex_bool_param[] = {TYPE_BOOL};
    // (2026-10-02) 改用"一次说全"的入口：个数 0..1 + 前缀 {bool} + 尾部 ANY
    //   （原先写成两步：注册 + 事后 `native_set_instance_method_vararg_params` ⇒ 现已被
    //   `string_register_method_vararg_with_params` 吸收，第二步不存在了 ✓）
    string_register_method("to_hex", make_native(str_to_hex, -1, "to_hex"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_VARARG(0, 1, 1, tohex_bool_param, TYPE_ANY));
    string_register_method("from_hex", make_native(str_from_hex, 1, "from_hex"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    // base64 实例方法：与 `to_hex` 同形（arity 必须 -1 ⇒ 可选参数才放行，见上面的说明）
    string_register_method("to_base64", make_native(str_to_base64, -1, "to_base64"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_VARARG(0, 1, 1, tohex_bool_param, TYPE_ANY));
    string_register_method("from_base64", make_native(str_from_base64, 1, "from_base64"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    TypeKind one_str_params[] = {TYPE_STRING};
    string_register_method("eq_ignore_case", make_native(str_eq_ignore_case, 2, "eq_ignore_case"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(one_str_params));
    string_register_method("codepoint_at", make_native(str_codepoint_at, 2, "codepoint_at"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED(int_params));
    string_register_method("to_codepoints", make_native(str_to_codepoints, 1, "to_codepoints"), TYPE_ARRAY, TYPE_UNKNOWN, NATIVE_FIXED_NONE(0));
    native_register_instance_method_return_spec("string", "to_codepoints", &NATIVE_T_ARR_INT);
    // 实例式 from_codepoint：与 to_codepoints 配对（可发现性）。⚠ 接收者本身用不上、被忽略 ——
    //   码点来自实参（`s.from_codepoint(20013)` 与 `strings.from_codepoint(20013)` 结果相同 ✓）
    string_register_method("from_codepoint", make_native(str_from_codepoint, 2, "from_codepoint"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_FIXED(int_params));

    // 14. 新增：包含检查（实例方法）
    TypeKind has_substr_params[] = {TYPE_STRING};
    string_register_method("has", make_native(str_has, 2, "has"), TYPE_BOOL, TYPE_UNKNOWN, NATIVE_FIXED(has_substr_params));

    // 15. 新增：统计子串出现次数（实例方法）
    string_register_method("count", make_native(str_count, 2, "count"), TYPE_INT, TYPE_UNKNOWN, NATIVE_FIXED(has_substr_params));

    // 16. 新增：左侧填充（实例方法）
    TypeKind int_str_params[] = {TYPE_INT, TYPE_STRING};
    // ⚠ 实例式原先写成 arity=2/min=2/max=3 ⇒ 检查器把 arity 当"**精确**个数"⇒ `s.pad_start(5)`
    //   被挡（"期望 2, 实际 1"），而模块式 `strings.pad_start(s, 5)` 却是合法的 —— 同一个
    //   `arity` 被当"必需个数"用造成的语义分叉。改用可变参数入口（min=1/max=2）后两边一致 ✓
    string_register_method("pad_start", make_native(str_pad_start, -1, "pad_start"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_VARARG(1, 2, 2, int_str_params, TYPE_ANY));

    // 17. 新增：右侧填充（实例方法）
    string_register_method("pad_end", make_native(str_pad_end, -1, "pad_end"), TYPE_STRING, TYPE_UNKNOWN, NATIVE_VARARG(1, 2, 2, int_str_params, TYPE_ANY));
}

// 为什么 18/19 两位（fmt_size / hex）**只有模块形态**、没进数字实例方法表：
//   它们收的是**数字**（不是字符串），所以实例形态只能挂到**数字方法表**上 ——
//   而那张表（maths 拥有）目前 28 个成员**全部返回 float**，是一张纯"数值计算"表；
//   数字 → 字符串在本语言的既有约定是**自由函数**（`_str()`、`format()`）⇒ 这俩属于那一族 ✓
//   代价也不划算：数字表的拥有者 maths 第一步 number_init_methods() 会 free + 重建整张表
//   （见 method_table_init_methods），而初始化顺序里 strings 在 maths 之前 ⇒
//   要挂上去就得让 maths 末尾反过来调 strings，凭空多一条**隐性的初始化顺序约束** ✗
//   （实测过那版：放错位置会表现为"编译照过、运行时才炸"，得靠额外守卫断言兜着）
//   ⇒ 撤掉，只留 `strings.fmt_size(...)` / `strings.hex(...)`（2026-09-26 决定）
