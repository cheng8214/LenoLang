#include "include/lenolang.h"
#include "include/string_table.h"
#include "include/native.h"
#include "include/platform_thread.h"
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
// 字符串方法注册表（哈希表实现 - O(1) 查找）
// ============================================================================

#define STRING_METHOD_TABLE_INITIAL_CAPACITY 32
#define STRING_METHOD_TABLE_MAX_LOAD 0.75

// 计算字符串哈希值（FNV-1a算法）
static uint32_t string_hash_string_local(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= 16777619;
        str++;
    }
    return hash;
}
/* 方法哈希表条目 */
typedef struct StringMethodHashEntry {
    char* name;
    ObjNative* method;
    int arity;
    TypeKind return_type;
    TypeKind return_element_type;
    TypeKind param_types[MAX_METHOD_PARAMS];
    struct StringMethodHashEntry* next;
} StringMethodHashEntry;

typedef struct {
    StringMethodHashEntry** entries;
    int capacity;
    int count;
} StringMethodTable;

static THREAD_LOCAL StringMethodTable stringMethodTable = {NULL, 0, 0};

// 初始化字符串方法表
static void string_method_table_init(void) {
    stringMethodTable.capacity = STRING_METHOD_TABLE_INITIAL_CAPACITY;
    stringMethodTable.count = 0;
    stringMethodTable.entries = (StringMethodHashEntry**)calloc(stringMethodTable.capacity, sizeof(StringMethodHashEntry*));
}

// 释放字符串方法表
static void string_method_table_free(void) {
    if (!stringMethodTable.entries) return;
    
    for (int i = 0; i < stringMethodTable.capacity; i++) {
        StringMethodHashEntry* entry = stringMethodTable.entries[i];
        while (entry) {
            StringMethodHashEntry* next = entry->next;
            free(entry->name);
            free(entry);
            entry = next;
        }
    }
    free(stringMethodTable.entries);
    stringMethodTable.entries = NULL;
    stringMethodTable.capacity = 0;
    stringMethodTable.count = 0;
}

// 扩容字符串方法表
static void string_method_table_resize(void) {
    int old_capacity = stringMethodTable.capacity;
    StringMethodHashEntry** old_entries = stringMethodTable.entries;
    
    int new_capacity = old_capacity * 2;
    StringMethodHashEntry** new_entries = (StringMethodHashEntry**)calloc(new_capacity, sizeof(StringMethodHashEntry*));
    if (!new_entries) return;
    
    for (int i = 0; i < old_capacity; i++) {
        StringMethodHashEntry* entry = old_entries[i];
        while (entry) {
            StringMethodHashEntry* next = entry->next;
            uint32_t hash = string_hash_string_local(entry->name);
            int index = hash & (new_capacity - 1);
            entry->next = new_entries[index];
            new_entries[index] = entry;
            entry = next;
        }
    }
    
    free(old_entries);
    stringMethodTable.entries = new_entries;
    stringMethodTable.capacity = new_capacity;
}

void string_register_method(const char* name, ObjNative* method, int arity, TypeKind return_type) {
    if (!stringMethodTable.entries) {
        string_method_table_init();
    }
    
    if (stringMethodTable.count >= stringMethodTable.capacity * STRING_METHOD_TABLE_MAX_LOAD) {
        string_method_table_resize();
    }
    
    uint32_t hash = string_hash_string_local(name);
    int index = hash & (stringMethodTable.capacity - 1);

    StringMethodHashEntry* entry = stringMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                entry->param_types[i] = TYPE_ANY;
            }
            return;
        }
        entry = entry->next;
    }

    StringMethodHashEntry* new_entry = (StringMethodHashEntry*)malloc(sizeof(StringMethodHashEntry));
    if (!new_entry) {
        native_throw_error("字符串方法注册内存分配失败");
        return;
    }

    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
        new_entry->param_types[i] = TYPE_ANY;
    }

    new_entry->next = stringMethodTable.entries[index];
    stringMethodTable.entries[index] = new_entry;
    stringMethodTable.count++;
}

// 注册字符串方法（带参数类型）
void string_register_method_with_params(const char* name, ObjNative* method, int arity, int min_arity, int max_arity,
                                         TypeKind return_type, TypeKind return_element_type, TypeKind* param_types) {
    if (!stringMethodTable.entries) {
        string_method_table_init();
    }
    
    if (stringMethodTable.count >= stringMethodTable.capacity * STRING_METHOD_TABLE_MAX_LOAD) {
        string_method_table_resize();
    }

    uint32_t hash = string_hash_string_local(name);
    int index = hash & (stringMethodTable.capacity - 1);

    StringMethodHashEntry* entry = stringMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            entry->method = method;
            entry->arity = arity;
            entry->return_type = return_type;
            entry->return_element_type = return_element_type;
            if (param_types && arity > 0) {
                int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
                for (int i = 0; i < count; i++) {
                    entry->param_types[i] = param_types[i];
                }
                for (int i = count; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            } else {
                for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                    entry->param_types[i] = TYPE_ANY;
                }
            }
            return;
        }
        entry = entry->next;
    }
    
    StringMethodHashEntry* new_entry = (StringMethodHashEntry*)malloc(sizeof(StringMethodHashEntry));
    if (!new_entry) {
        native_throw_error("字符串方法注册内存分配失败");
        return;
    }
    
    new_entry->name = strdup(name);
    new_entry->method = method;
    new_entry->arity = arity;
    new_entry->return_type = return_type;
    new_entry->return_element_type = return_element_type;
    if (param_types && arity > 0) {
        int count = arity < MAX_METHOD_PARAMS ? arity : MAX_METHOD_PARAMS;
        for (int i = 0; i < count; i++) {
            new_entry->param_types[i] = param_types[i];
        }
        for (int i = count; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    } else {
        for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
            new_entry->param_types[i] = TYPE_ANY;
        }
    }
    
    new_entry->next = stringMethodTable.entries[index];
    stringMethodTable.entries[index] = new_entry;
    stringMethodTable.count++;
   // 同时注册到编译期元信息表，避免重复维护
    native_register_instance_method_meta_with_params("string", name, arity, min_arity, max_arity, return_type, return_element_type, param_types);
}

// 获取字符串方法的参数类型
TypeKind string_get_method_param_type(const char* method_name, int param_index) {
    if (!stringMethodTable.entries || stringMethodTable.count == 0) return TYPE_ANY;

    uint32_t hash = string_hash_string_local(method_name);
    int index = hash & (stringMethodTable.capacity - 1);
    
    StringMethodHashEntry* entry = stringMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, method_name) == 0) {
            if (param_index >= 0 && param_index < entry->arity && param_index < MAX_METHOD_PARAMS) {
                return entry->param_types[param_index];
            }
            break;
        }
        entry = entry->next;
    }
    return TYPE_ANY;
}

// 查找字符串方法（O(1)）
ObjNative* string_find_method(const char* name) {
    if (!stringMethodTable.entries || stringMethodTable.count == 0) return NULL;

    uint32_t hash = string_hash_string_local(name);
    int index = hash & (stringMethodTable.capacity - 1);

    StringMethodHashEntry* entry = stringMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            return entry->method;
        }
        entry = entry->next;
    }
    return NULL;
}

// 查找字符串方法的元信息（用于编译期类型检查）
StringMethodEntry string_find_method_meta(const char* name) {
    StringMethodEntry result = {NULL, NULL, 0, TYPE_ANY, TYPE_UNKNOWN, {TYPE_ANY}};

    if (!stringMethodTable.entries || stringMethodTable.count == 0) return result;

    uint32_t hash = string_hash_string_local(name);
    int index = hash & (stringMethodTable.capacity - 1);
    
    StringMethodHashEntry* entry = stringMethodTable.entries[index];
    while (entry) {
        if (strcmp(entry->name, name) == 0) {
            result.name = entry->name;
            result.method = entry->method;
            result.arity = entry->arity;
            result.return_type = entry->return_type;
            result.return_element_type = entry->return_element_type;
            for (int i = 0; i < MAX_METHOD_PARAMS; i++) {
                result.param_types[i] = entry->param_types[i];
            }
            return result;
        }
        entry = entry->next;
    }
    return result;
}

void string_init_methods(void) {
    string_method_table_free();
    string_method_table_init();
}

// 标记所有字符串方法对象（供 GC 使用）
void string_mark_methods(void) {
    if (!stringMethodTable.entries) return;
    
    for (int i = 0; i < stringMethodTable.capacity; i++) {
        StringMethodHashEntry* entry = stringMethodTable.entries[i];
        while (entry) {
            if (entry->method) {
                gc_mark_object((Object*)entry->method);
            }
            entry = entry->next;
        }
    }
}
