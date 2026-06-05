#include "include/string_table.h"
#include "include/native.h"
#include <stdlib.h>
#include <string.h>

// ============================================================================
// 线程局部字符串表和缓存 - 每个线程有自己的字符串表
// ============================================================================

THREAD_LOCAL StringTable string_table = {0};
THREAD_LOCAL StringCacheBucket string_cache[STRCACHE_SIZE] = {0};

// 内存统计（用于调试）
static size_t intern_saved_memory = 0;  // 通过内化节省的内存
static size_t intern_shared_count = 0;  // 共享字符串的次数

// ============================================================================
// 内部辅助函数
// ============================================================================

// FNV-1a 哈希算法（与原来兼容）
uint32_t intern_hash_string(const char* str, int len) {
    uint32_t hash = 2166136261u;
    for (int i = 0; i < len; i++) {
        hash ^= (uint8_t)str[i];
        hash *= 16777619;
    }
    return hash;
}

// 字符串比较
inline int intern_string_equal(ObjString* a, const char* chars, int len) {
    if (a->len != len) return 0;
    return memcmp(a->chars, chars, len) == 0;
}

// 计算缓存索引
static inline int cache_index(const char* str, int len) {
    // 使用哈希值的低几位作为索引
    uint32_t hash = intern_hash_string(str, len);
    return hash & (STRCACHE_SIZE - 1);
}

// ============================================================================
// 字符串表操作
// ============================================================================

void intern_table_init(void) {
    string_table.capacity = INTERN_MIN_TABLE_SIZE;
    string_table.count = 0;
    string_table.entries = (InternEntry**)malloc(
        sizeof(InternEntry*) * string_table.capacity);
    
    if (!string_table.entries) {
        native_throw_error("字符串表初始化失败：内存分配错误");
        return;
    }
    
    // 初始化为 NULL
    for (int i = 0; i < string_table.capacity; i++) {
        string_table.entries[i] = NULL;
    }
    
    // 初始化缓存
    for (int i = 0; i < STRCACHE_SIZE; i++) {
        for (int j = 0; j < STRCACHE_BUCKET_SIZE; j++) {
            string_cache[i].entries[j] = NULL;
        }
    }
    
    intern_saved_memory = 0;
    intern_shared_count = 0;
}

void intern_table_free(void) {
    if (!string_table.entries) return;
    
    // 释放所有条目（字符串本身由 GC 管理，不需要在这里释放）
    for (int i = 0; i < string_table.capacity; i++) {
        InternEntry* entry = string_table.entries[i];
        while (entry) {
            InternEntry* next = entry->next;
            free(entry);
            entry = next;
        }
    }
    
    free(string_table.entries);
    string_table.entries = NULL;
    string_table.capacity = 0;
    string_table.count = 0;
}

static void intern_table_resize(int new_capacity) {
    // 分配新的条目数组
    InternEntry** new_entries = (InternEntry**)malloc(
        sizeof(InternEntry*) * new_capacity);
    if (!new_entries) {
        native_throw_error("字符串表扩容失败");
        return;
    }
    
    // 初始化新数组
    for (int i = 0; i < new_capacity; i++) {
        new_entries[i] = NULL;
    }
    
    // 重新哈希所有现有条目
    for (int i = 0; i < string_table.capacity; i++) {
        InternEntry* entry = string_table.entries[i];
        while (entry) {
            InternEntry* next = entry->next;
            
            // 计算新位置
            uint32_t hash = entry->str->hash;
            int index = hash & (new_capacity - 1);
            
            // 插入到新数组头部
            entry->next = new_entries[index];
            new_entries[index] = entry;
            
            entry = next;
        }
    }
    
    // 释放旧数组，使用新数组
    free(string_table.entries);
    string_table.entries = new_entries;
    string_table.capacity = new_capacity;
}

// 更新缓存
static void update_cache(ObjString* str) {
    int idx = cache_index(str->chars, str->len);
    StringCacheBucket* bucket = &string_cache[idx];
    
    // 检查是否已在缓存中
    for (int i = 0; i < STRCACHE_BUCKET_SIZE; i++) {
        if (bucket->entries[i] == str) {
            // 已在缓存中，移动到最前面（LRU）
            if (i > 0) {
                ObjString* temp = bucket->entries[0];
                bucket->entries[0] = str;
                bucket->entries[i] = temp;
            }
            return;
        }
    }
    
    // 不在缓存中，添加到最前面，其他后移
    for (int i = STRCACHE_BUCKET_SIZE - 1; i > 0; i--) {
        bucket->entries[i] = bucket->entries[i - 1];
    }
    bucket->entries[0] = str;
}

// 从缓存中查找
static ObjString* find_in_cache(const char* chars, int len) {
    int idx = cache_index(chars, len);
    StringCacheBucket* bucket = &string_cache[idx];
    
    for (int i = 0; i < STRCACHE_BUCKET_SIZE; i++) {
        ObjString* str = bucket->entries[i];
        if (str && intern_string_equal(str, chars, len)) {
            return str;
        }
    }
    return NULL;
}

ObjString* intern_find(const char* chars, int len) {
    // 检查字符串表是否已初始化
    if (!string_table.entries || string_table.capacity == 0) {
        return NULL;
    }
    
    // 首先检查缓存
    ObjString* cached = find_in_cache(chars, len);
    if (cached) return cached;
    
    // 计算哈希
    uint32_t hash = intern_hash_string(chars, len);
    int index = hash & (string_table.capacity - 1);
    
    // 在表中查找
    InternEntry* entry = string_table.entries[index];
    while (entry) {
        ObjString* str = entry->str;
        if (str->hash == hash && intern_string_equal(str, chars, len)) {
            // 找到，更新缓存并返回
            update_cache(str);
            return str;
        }
        entry = entry->next;
    }
    
    return NULL;
}

// 查找或创建字符串（所有字符串由 GC 管理）
// 如果字符串已存在于表中，返回已有字符串；否则创建新字符串并加入表中
ObjString* intern_string(const char* chars, int len) {
    // 长字符串不内化
    if (!intern_should_intern(len)) {
        return str_new_nointern(chars, len);
    }
    
    // 检查字符串表是否已初始化
    if (!string_table.entries || string_table.capacity == 0) {
        return str_new_nointern(chars, len);
    }
    
    // 首先尝试查找
    ObjString* existing = intern_find(chars, len);
    if (existing) {
        intern_shared_count++;
        intern_saved_memory += len + sizeof(ObjString);
        return existing;
    }
    
    // 创建新字符串，由 GC 管理
    ObjString* str = (ObjString*)gc_alloc(sizeof(ObjString), OBJ_STRING);
    if (!str) {
        native_throw_error("字符串创建失败：内存分配错误");
        return NULL;
    }
    
    // 分配字符数组
    str->chars = (char*)malloc(len + 1);
    if (!str->chars) {
        native_throw_error("字符串创建失败：字符数组分配错误");
        return NULL;
    }
    
    // 初始化字符串
    memcpy(str->chars, chars, len);
    str->chars[len] = '\0';
    str->len = len;
    str->hash = intern_hash_string(chars, len);
    
    // 注册到字符串表
    intern_register(str);
    
    return str;
}

void intern_remove(ObjString* str) {
    if (!str || !string_table.entries) return;
    
    int index = str->hash & (string_table.capacity - 1);
    InternEntry** current = &string_table.entries[index];
    
    while (*current) {
        InternEntry* entry = *current;
        if (entry->str == str) {
            // 找到，移除条目
            *current = entry->next;
            free(entry);
            string_table.count--;
            
            // 从缓存中移除
            int cache_idx = cache_index(str->chars, str->len);
            StringCacheBucket* bucket = &string_cache[cache_idx];
            for (int i = 0; i < STRCACHE_BUCKET_SIZE; i++) {
                if (bucket->entries[i] == str) {
                    bucket->entries[i] = NULL;
                    break;
                }
            }
            return;
        }
        current = &entry->next;
    }
}

// 将 GC 管理的字符串注册到字符串表（弱引用，不阻止 GC 回收）
// 当 GC 回收字符串时，需调用 intern_remove 清理条目
void intern_register(ObjString* str) {
    if (!str || !string_table.entries || string_table.capacity == 0) return;
    
    // 检查是否需要扩容
    if (string_table.count + 1 > string_table.capacity * INTERN_MAX_LOAD_FACTOR) {
        intern_table_resize(string_table.capacity * 2);
    }
    
    int index = str->hash & (string_table.capacity - 1);
    InternEntry* new_entry = (InternEntry*)malloc(sizeof(InternEntry));
    if (!new_entry) return;
    
    new_entry->str = str;
    new_entry->next = string_table.entries[index];
    string_table.entries[index] = new_entry;
    string_table.count++;
    
    update_cache(str);
}

// 标记字符串表中的所有字符串（供 GC 使用）
// 注意：现在字符串由 GC 管理，不再需要 intern_mark_all
// 保留此函数为空操作以兼容现有调用
void intern_mark_all(void) {
    // 不再标记所有内化字符串，字符串由 GC 正常管理
}

// GC 后清理字符串表中指向已回收对象的条目
// 字符串现在由 GC 管理，GC 回收字符串时通过 free_object_resources -> intern_remove 清理
// 此函数作为安全网，清理可能遗漏的条目（如字符串被 GC 回收但 intern_remove 未调用）
void intern_sweep_unmarked(void) {
    if (!string_table.entries) return;

    for (int i = 0; i < string_table.capacity; i++) {
        InternEntry** current = &string_table.entries[i];
        while (*current) {
            InternEntry* entry = *current;
            if (entry->str && !entry->str->header.marked) {
                // 字符串未被标记（即将被 GC 回收），清理条目
                *current = entry->next;
                // 清除缓存
                int cache_idx = cache_index(entry->str->chars, entry->str->len);
                StringCacheBucket* bucket = &string_cache[cache_idx];
                for (int j = 0; j < STRCACHE_BUCKET_SIZE; j++) {
                    if (bucket->entries[j] == entry->str) {
                        bucket->entries[j] = NULL;
                        break;
                    }
                }
                // 只释放 InternEntry，字符串由 GC 的 sweep 释放
                free(entry);
                string_table.count--;
            } else {
                current = &entry->next;
            }
        }
    }
}

int intern_get_count(void) {
    return string_table.count;
}

int intern_get_capacity(void) {
    return string_table.capacity;
}

// 调试函数：打印统计信息
void intern_print_stats(void) {
    printf("=== 字符串内化统计 ===\n");
    printf("表大小: %d/%d (%.1f%%)\n", 
           string_table.count, string_table.capacity,
           100.0 * string_table.count / string_table.capacity);
    printf("共享次数: %zu\n", intern_shared_count);
    printf("节省内存: %zu bytes\n", intern_saved_memory);
}
