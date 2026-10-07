#ifndef LENO_HASH_H
#define LENO_HASH_H

#include <stdint.h>

// ============================================================================
// FNV-1a 32-bit 哈希算法（统一实现）
// ============================================================================
// 此头文件统一了项目中所有重复的 FNV-1a 字符串哈希实现。
// 相关 Issue: Duplicate FNV-1a generator functions

// FNV-1a 常量
#define LENO_FNV1A_OFFSET_BASIS 2166136261u
#define LENO_FNV1A_PRIME        16777619u

// 对空终止字符串计算 FNV-1a 哈希
static inline uint32_t leno_fnv1a(const char* str) {
    uint32_t hash = LENO_FNV1A_OFFSET_BASIS;
    while (*str) {
        hash ^= (unsigned char)(*str);
        hash *= LENO_FNV1A_PRIME;
        str++;
    }
    return hash;
}

// 对指定长度的字符串计算 FNV-1a 哈希
static inline uint32_t leno_fnv1a_len(const char* str, int len) {
    uint32_t hash = LENO_FNV1A_OFFSET_BASIS;
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)str[i];
        hash *= LENO_FNV1A_PRIME;
    }
    return hash;
}

// FNV-1a 的**增量续算**：从已有 hash 继续吃后面的字节。
// 用途：字符串拼接时 `hash(ab)` 可以由 `hash(a)` 续算 `b` 得到 ⇒ 省掉一次整串扫描。
// FNV-1a 本身是流式算法（h ^= byte; h *= prime），所以续算与"从头算整串"逐位等价 ✓
static inline uint32_t leno_fnv1a_continue(uint32_t hash, const char* str, int len) {
    for (int i = 0; i < len; i++) {
        hash ^= (unsigned char)str[i];
        hash *= LENO_FNV1A_PRIME;
    }
    return hash;
}

#endif // LENO_HASH_H
