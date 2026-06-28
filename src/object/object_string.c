#include "include/lenolang.h"
#include "include/string_table.h"
#include "include/native.h"
#include "include/platform_thread.h"
#include "include/method_table.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// 计算 UTF-8 字符串的 Unicode 字符数
int utf8_char_len(const char* chars, int byte_len) {
    int count = 0;
    int i = 0;
    while (i < byte_len) {
        unsigned char c = (unsigned char)chars[i];
        if (c < 0x80) {
            i += 1;      // ASCII: 1 字节
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;      // 2 字节 UTF-8
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;      // 3 字节 UTF-8（中文在此范围）
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;      // 4 字节 UTF-8（emoji 等）
        } else {
            i += 1;      // 无效 UTF-8，跳过1字节
        }
        count++;
    }
    return count;
}

// 获取 UTF-8 字符串中第 char_index 个字符的字节偏移
// 返回字节偏移，如果越界返回 byte_len
int utf8_char_offset(const char* chars, int byte_len, int char_index) {
    int count = 0;
    int i = 0;
    while (i < byte_len && count < char_index) {
        unsigned char c = (unsigned char)chars[i];
        if (c < 0x80) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
        count++;
    }
    return i;
}

// 获取 UTF-8 字符串中从字节偏移 offset 开始的一个字符的字节长度
int utf8_char_byte_len(const char* chars, int byte_len, int offset) {
    if (offset >= byte_len) return 0;
    unsigned char c = (unsigned char)chars[offset];
    if (c < 0x80) return 1;
    else if ((c & 0xE0) == 0xC0) return 2;
    else if ((c & 0xF0) == 0xE0) return 3;
    else if ((c & 0xF8) == 0xF0) return 4;
    return 1; // 无效 UTF-8
}

// 计算字符串哈希值（保持与 string_table.c 兼容）
uint32_t hash_string(const char* key, int length) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < length; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 16777619;
    }
    return hash;
}

// 分配指定长度的字符串对象（不初始化内容）
// 注意：此函数创建非内化字符串，不检查字符串表
ObjString* str_alloc(int len) {
    ObjString* str = (ObjString*)gc_alloc(sizeof(ObjString), OBJ_STRING);
    if (!str) return NULL;

    str->chars = (char*)malloc(len + 1);
    if (!str->chars) {
        native_throw_error("内存分配失败");
        return NULL;
    }

    str->len = len;
    str->char_len = 0;  // 调用者需在填充内容后设置
    str->hash = 0;
    str->chars[0] = '\0';
    return str;
}

// 创建非内化字符串（用于长字符串或特殊情况）
// 长字符串（> INTERN_MAX_SHORT_LEN）使用此函数
ObjString* str_new_nointern(const char* chars, int len) {
    ObjString* str = (ObjString*)gc_alloc(sizeof(ObjString), OBJ_STRING);
    if (!str) return NULL;
    
    str->chars = (char*)malloc(len + 1);
    if (!str->chars) {
        native_throw_error("内存分配失败");
        return NULL;
    }
    
    memcpy(str->chars, chars, len);
    str->chars[len] = '\0';
    str->len = len;
    str->char_len = utf8_char_len(chars, len);
    str->hash = hash_string(chars, len);
    
    return str;
}

// 创建字符串（所有字符串由 GC 管理）
ObjString* str_new(const char* chars, int len) {
    // 先尝试在字符串表中查找已有的等价字符串（去重）
    if (string_table.entries && string_table.capacity > 0) {
        ObjString* existing = intern_find(chars, len);
        if (existing) return existing;
    }
    
    // 创建新字符串，由 GC 管理
    ObjString* str = (ObjString*)gc_alloc(sizeof(ObjString), OBJ_STRING);
    if (!str) return NULL;
    
    str->chars = (char*)malloc(len + 1);
    if (!str->chars) {
        native_throw_error("内存分配失败");
        return NULL;
    }
    
    memcpy(str->chars, chars, len);
    str->chars[len] = '\0';
    str->len = len;
    str->char_len = utf8_char_len(chars, len);
    str->hash = hash_string(chars, len);
    
    // 将短字符串注册到字符串表（弱引用，不阻止 GC 回收）
    if (intern_should_intern(len)) {
        intern_register(str);
    }
    
    return str;
}

// 复制字符串（现在直接使用 str_new，会自动处理内化）
// 注意：str_copy 已在 leno_value.h 中内联

// 字符串拼接
// 结果字符串如果短则内化，否则不内化
ObjString* str_concat(ObjString* a, ObjString* b) {
    int len = a->len + b->len;
    
    // 创建字符缓冲区
    char* buffer = (char*)malloc(len + 1);
    if (!buffer) {
        native_throw_error("内存分配失败");
        return NULL;
    }
    
    memcpy(buffer, a->chars, a->len);
    memcpy(buffer + a->len, b->chars, b->len);
    buffer[len] = '\0';
    
    // 使用 str_new 自动处理内化
    ObjString* result = str_new(buffer, len);
    
    free(buffer);
    return result;
}

// 注意：str_concat 使用 str_new，char_len 已在 str_new 中自动计算

// ============================================================================
// 字符串方法注册表（使用通用 MethodTable）
// ============================================================================

#define STRING_METHOD_TABLE_INITIAL_CAPACITY 32

static THREAD_LOCAL MethodTable stringMethodTable = {NULL, 0, 0};

// 注册字符串方法（带参数类型）
void string_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    method_table_register_with_params(&stringMethodTable, "string", name, method, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 获取字符串方法的参数类型
TypeKind string_get_method_param_type(const char* method_name, int param_index) {
    return method_table_get_param_type(&stringMethodTable, method_name, param_index);
}

// 查找字符串方法（O(1)）
ObjNative* string_find_method(const char* name) {
    return method_table_find(&stringMethodTable, name);
}

// 查找字符串方法的元信息（用于编译期类型检查）
StringMethodEntry string_find_method_meta(const char* name) {
    return method_table_find_meta(&stringMethodTable, name);
}

void string_init_methods(void) {
    method_table_init_methods(&stringMethodTable, STRING_METHOD_TABLE_INITIAL_CAPACITY);
}

// 标记所有字符串方法对象（供 GC 使用）
void string_mark_methods(void) {
    method_table_mark(&stringMethodTable);
}
