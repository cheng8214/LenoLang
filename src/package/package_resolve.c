/**
 * package_resolve.c - 模块搜索路径管理与模块文件查找
 *
 * 核心功能：
 * 1. 从源文件向上查找 leno.toml，确定项目根目录
 * 2. 管理模块搜索路径列表（项目 lib/ + 全局缓存）
 * 3. 在搜索路径中查找模块文件
 */

#include "../include/leno_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 1024
#endif
#else
#include <unistd.h>
#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 1024
#endif
#endif

/* ============================================================================
 * 搜索路径列表
 * ============================================================================ */

static char search_paths[MAX_SEARCH_PATHS][MAX_PATH_LEN];
static int search_path_count = 0;

int package_search_path_add(const char* path) {
    if (search_path_count >= MAX_SEARCH_PATHS || !path) return -1;

    size_t len = strlen(path);
    if (len >= MAX_PATH_LEN) len = MAX_PATH_LEN - 1;

    memcpy(search_paths[search_path_count], path, len);
    search_paths[search_path_count][len] = '\0';

    /* 统一路径分隔符 */
#ifdef _WIN32
    for (int i = 0; search_paths[search_path_count][i]; i++) {
        if (search_paths[search_path_count][i] == '/')
            search_paths[search_path_count][i] = '\\';
    }
#endif

    search_path_count++;
    return 0;
}

void package_search_path_clear(void) {
    search_path_count = 0;
}

int package_search_path_count(void) {
    return search_path_count;
}

const char* package_search_path_get(int index) {
    if (index < 0 || index >= search_path_count) return NULL;
    return search_paths[index];
}

/* ============================================================================
 * 项目根目录查找
 * ============================================================================ */

/**
 * 检查文件是否存在
 */
static int file_exists_internal(const char* path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return 0;
    wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wpath) return 0;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return (attr != INVALID_FILE_ATTRIBUTES);
#else
    FILE* fp = fopen(path, "r");
    if (fp) {
        fclose(fp);
        return 1;
    }
    return 0;
#endif
}

char* package_find_project_root(const char* source_file) {
    if (!source_file || source_file[0] == '\0') return NULL;

    char current[MAX_PATH_LEN];
    strncpy(current, source_file, MAX_PATH_LEN - 1);
    current[MAX_PATH_LEN - 1] = '\0';

#ifdef _WIN32
    for (int i = 0; current[i]; i++)
        if (current[i] == '/') current[i] = '\\';
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    /* 从文件目录开始向上查找 leno.toml */
    char* last_sep = strrchr(current, sep);
    if (last_sep) *last_sep = '\0';  /* 去掉文件名 */

    /* 防止无限循环：最多向上查找 32 层 */
    for (int depth = 0; depth < 32; depth++) {
        char toml_path[MAX_PATH_LEN];
        /* 确保不会超出缓冲区: current + sep + "leno.toml" */
        size_t clen = strlen(current);
        if (clen + 10 >= (size_t)MAX_PATH_LEN) return NULL;
        snprintf(toml_path, sizeof(toml_path), "%s%cleno.toml", current, sep);

        if (file_exists_internal(toml_path)) {
            /* 找到了，返回项目根目录（带结尾分隔符） */
            char* result = (char*)malloc(strlen(current) + 2);
            if (result) {
                strcpy(result, current);
                size_t rlen = strlen(result);
                result[rlen] = sep;
                result[rlen + 1] = '\0';
            }
            return result;
        }

        /* 向上一层 */
        last_sep = strrchr(current, sep);
        if (!last_sep) break;
        *last_sep = '\0';
    }

    return NULL;
}

/* ============================================================================
 * 模块文件查找
 * ============================================================================ */

int package_resolve_module_file(const char* module_name, char* out_path, int out_len) {
    if (!module_name || !out_path || out_len <= 0) return -1;

    char candidate[MAX_PATH_LEN];

    for (int i = 0; i < search_path_count; i++) {
        size_t plen = strlen(search_paths[i]);
        size_t mlen = strlen(module_name);
        if (plen + mlen + 6 >= (size_t)MAX_PATH_LEN) continue;
        snprintf(candidate, sizeof(candidate), "%.*s%.*s.leno",
                 (int)plen, search_paths[i], (int)mlen, module_name);

        if (file_exists_internal(candidate)) {
            strncpy(out_path, candidate, out_len - 1);
            out_path[out_len - 1] = '\0';
            return 1;
        }
    }

    return -1;
}

/* ============================================================================
 * "什么算包名写法" —— 唯一实现（S9 收敛）
 * ----------------------------------------------------------------------------
 * 规则：写法里**不含 ".leno"** 才按包名处理（在各搜索路径下找 <写法>.leno）；
 *       含 ".leno" 的一律返回 -1（那是文件路径写法，交给调用方按相对/绝对路径解析）。
 *
 * 为什么需要这个包装：这条"包名 vs 文件路径"的判断此前在**多处各写一遍**
 * （parser_module.c 两处、module_symbol_table 的 resolve_module_full_path 一处；
 * 加载器 load_module_file 只做"相对路径拼当前目录 + normalize"，不做包搜索）。
 * 任一处漏改或写歪，就是"同一个 import 在不同阶段解析到不同文件"——S9 那类
 * 静默降级（裸名导入时跨模块字段类型变 any）正是这么来的。现在两边都只调这里。
 * ============================================================================ */
int package_resolve_import_spec(const char* spec, char* out_path, int out_len) {
    if (!spec || !out_path || out_len <= 0) return -1;
    if (strstr(spec, ".leno") != NULL) return -1;   /* 文件路径写法，不是包名 */
    return package_resolve_module_file(spec, out_path, out_len);
}
