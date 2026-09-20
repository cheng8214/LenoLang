#ifndef STRING_TABLE_H
#define STRING_TABLE_H

#include "leno_value.h"
#include "platform_thread.h"

// ============================================================================
// 字符串内化 (String Interning) 系统
// ============================================================================
// 
// 字符串内化是一种优化技术，让所有相同内容的字符串共享同一个对象。
// 优点：
//   1. 节省内存 - 相同字符串只存储一份
//   2. 快速比较 - 字符串比较变成 O(1) 的指针比较
//   3. 快速哈希 - 哈希值只需计算一次，可缓存用于字典键

// 短字符串最大长度（超过此长度的字符串不内化）
// Lua 使用 40，这里设为 40 以适应大多数标识符和关键字
#define INTERN_MAX_SHORT_LEN 40

// 字符串表初始大小（必须是 2 的幂）
#define INTERN_MIN_TABLE_SIZE 64

// 字符串表最大负载因子
#define INTERN_MAX_LOAD_FACTOR 0.75

// 字符串缓存大小（用于缓存最近使用的字符串，加速查找）
// 使用类似 Lua 的字符串缓存：N 个桶，每个桶 M 个条目
#define STRCACHE_SIZE 32  // 桶数量
#define STRCACHE_BUCKET_SIZE 2  // 每个桶的条目数

// 字符串表条目（使用开链法处理冲突）
typedef struct InternEntry {
    ObjString* str;           // 字符串对象
    struct InternEntry* next; // 下一个条目（链式结构）
} InternEntry;

// 字符串表结构
typedef struct {
    InternEntry** entries;    // 哈希表数组
    int capacity;             // 表容量
    int count;                // 当前字符串数量
} StringTable;

// 字符串缓存条目
typedef struct {
    ObjString* entries[STRCACHE_BUCKET_SIZE];
} StringCacheBucket;

// 线程局部字符串表和缓存 - 每个线程有自己的字符串表
extern THREAD_LOCAL StringTable string_table;
extern THREAD_LOCAL StringCacheBucket string_cache[STRCACHE_SIZE];

// ============================================================================
// API 函数
// ============================================================================

// 初始化字符串表
void intern_table_init(void);

// 释放字符串表（通常在程序结束时调用）
void intern_table_free(void);

// 查找或创建内化字符串
// 如果字符串已存在于表中，返回已有字符串；否则创建新字符串并加入表中
ObjString* intern_string(const char* chars, int len);

// 查找内化字符串（不创建新字符串）
// 如果找到返回字符串，否则返回 NULL
ObjString* intern_find(const char* chars, int len);

// 检查字符串是否需要内化（短字符串）
static inline int intern_should_intern(int len) {
    return len <= INTERN_MAX_SHORT_LEN;
}

// 计算字符串哈希值（FNV-1a 算法，与原来相同以保持兼容）
uint32_t intern_hash_string(const char* str, int len);

// 移除字符串表中的字符串（当字符串被 GC 回收时调用）
void intern_remove(ObjString* str);

// 将 GC 管理的字符串注册到字符串表（弱引用，不阻止 GC 回收）
void intern_register(ObjString* str);

// 标记字符串表中的所有字符串（供 GC 使用）
void intern_mark_all(void);
void intern_sweep_unmarked(void);

// 获取字符串表统计信息
int intern_get_count(void);
int intern_get_capacity(void);

// 字符串比较（用于查找时比较）
int intern_string_equal(ObjString* a, const char* chars, int len);

#endif // STRING_TABLE_H
