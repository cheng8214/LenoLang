/**
 * LSP 符号缓存实现
 */

#include "lsp_symbol_cache.h"
#include <sys/stat.h>
#include <time.h>

// ==================== 内部数据结构 ====================

#define MAX_MODULE_CACHE 128

typedef struct {
    char* path;                 // 模块绝对路径
    time_t mtime;               // 文件最后修改时间（0 表示无效）
    ModuleSymbolTable* table;   // 符号表（NULL 表示条目空闲）
} ModuleCacheEntry;

static ModuleCacheEntry g_module_cache[MAX_MODULE_CACHE];
static int g_module_cache_count = 0;

// 当前文件分析缓存（单条，因为 LSP 通常聚焦当前编辑的文件）
typedef struct {
    char* path;                 // 文件路径
    unsigned long content_hash; // 内容 hash
    CompilerContext ctx;        // 编译上下文
    int valid;                  // 是否有效
} CurrentFileCache;

static CurrentFileCache g_current_cache = {0};

// ==================== 辅助函数 ====================

// 获取文件修改时间（失败返回 0）
static time_t get_file_mtime(const char* path) {
    if (!path) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return st.st_mtime;
}

// djb2 字符串 hash
static unsigned long djb2_hash(const char* str) {
    unsigned long hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;  // hash * 33 + c
    }
    return hash;
}

// 在模块缓存中查找条目（返回索引，-1 表示未找到）
static int find_module_cache_entry(const char* path) {
    for (int i = 0; i < g_module_cache_count; i++) {
        if (g_module_cache[i].table && g_module_cache[i].path &&
            strcmp(g_module_cache[i].path, path) == 0) {
            return i;
        }
    }
    return -1;
}

// 释放模块缓存条目
static void free_module_cache_entry(int idx) {
    if (idx < 0 || idx >= g_module_cache_count) return;
    if (g_module_cache[idx].table) {
        module_symbol_table_destroy(g_module_cache[idx].table);
        g_module_cache[idx].table = NULL;
    }
    if (g_module_cache[idx].path) {
        free(g_module_cache[idx].path);
        g_module_cache[idx].path = NULL;
    }
    g_module_cache[idx].mtime = 0;
}

// ==================== 模块符号表缓存 ====================

ModuleSymbolTable* lsp_cache_get_module_symtable(const char* module_path, const char* current_file) {
    if (!module_path) return NULL;

    int idx = find_module_cache_entry(module_path);

    if (idx >= 0) {
        // 命中缓存，检查 mtime 是否变化
        time_t cur_mtime = get_file_mtime(module_path);
        if (cur_mtime != 0 && cur_mtime == g_module_cache[idx].mtime) {
            // mtime 未变，直接返回缓存的符号表
            return g_module_cache[idx].table;
        }
        // mtime 变化或无法获取，释放旧条目重新扫描
        free_module_cache_entry(idx);
        // 把该索引留作复用（不缩小 count，直接覆盖）
    } else {
        // 未命中，若缓存已满，回收第一个条目
        if (g_module_cache_count >= MAX_MODULE_CACHE) {
            free_module_cache_entry(0);
            idx = 0;
        } else {
            idx = g_module_cache_count;
            g_module_cache_count++;
        }
    }

    // 创建并扫描符号表
    ModuleSymbolTable* table = module_symbol_table_create(module_path);
    if (!table) return NULL;

    // module_symbol_table_scan 内部走 .lenosymc 磁盘缓存
    if (module_symbol_table_scan(table, current_file) != 0) {
        module_symbol_table_destroy(table);
        return NULL;
    }

    // 存入缓存
    g_module_cache[idx].path = strdup(module_path);
    g_module_cache[idx].mtime = get_file_mtime(module_path);
    g_module_cache[idx].table = table;

    return table;
}

// ==================== 当前文件分析缓存 ====================

CompilerContext* lsp_cache_analyze_current(const char* content, const char* file_path) {
    if (!content) return NULL;

    unsigned long hash = djb2_hash(content);

    // 命中缓存
    if (g_current_cache.valid && g_current_cache.path && file_path &&
        strcmp(g_current_cache.path, file_path) == 0 &&
        g_current_cache.content_hash == hash) {
        return &g_current_cache.ctx;
    }

    // 未命中，清理旧缓存
    if (g_current_cache.valid) {
        compiler_context_cleanup(&g_current_cache.ctx);
        g_current_cache.valid = 0;
    }
    if (g_current_cache.path) {
        free(g_current_cache.path);
        g_current_cache.path = NULL;
    }

    // 重新分析
    compiler_context_init(&g_current_cache.ctx);
    compiler_analyze_with_filename(&g_current_cache.ctx, content, file_path);

    g_current_cache.path = file_path ? strdup(file_path) : NULL;
    g_current_cache.content_hash = hash;
    g_current_cache.valid = 1;

    return &g_current_cache.ctx;
}

// ==================== 缓存管理 ====================

void lsp_cache_invalidate_path(const char* path) {
    if (!path) return;

    // 清理当前文件缓存
    if (g_current_cache.valid && g_current_cache.path &&
        strcmp(g_current_cache.path, path) == 0) {
        compiler_context_cleanup(&g_current_cache.ctx);
        free(g_current_cache.path);
        g_current_cache.path = NULL;
        g_current_cache.valid = 0;
    }

    // 清理模块缓存（路径匹配）
    for (int i = 0; i < g_module_cache_count; i++) {
        if (g_module_cache[i].table && g_module_cache[i].path &&
            strcmp(g_module_cache[i].path, path) == 0) {
            free_module_cache_entry(i);
            break;
        }
    }
}

void lsp_cache_cleanup(void) {
    // 清理当前文件缓存
    if (g_current_cache.valid) {
        compiler_context_cleanup(&g_current_cache.ctx);
        g_current_cache.valid = 0;
    }
    if (g_current_cache.path) {
        free(g_current_cache.path);
        g_current_cache.path = NULL;
    }

    // 清理所有模块缓存
    for (int i = 0; i < g_module_cache_count; i++) {
        free_module_cache_entry(i);
    }
    g_module_cache_count = 0;
}
