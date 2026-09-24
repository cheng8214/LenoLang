/**
 * package_install.c - 全局包缓存管理与包安装
 *
 * 缓存结构：
 *   ~/.leno/pkgs/
 *     <包名>/
 *       lib/          (模块文件，如 <包名>.leno)
 *       src/          (可选)
 *       leno.toml     (包配置)
 *
 * 搜索路径会用：<缓存>/<包名>/lib/
 * 这样 import "包名" 就能解析到 lib/<包名>.leno
 */

#include "../include/leno_package.h"
#include "../include/module_loader.h"
#include "../include/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define PATH_SEP '\\'
#else
#include <unistd.h>
#include <dirent.h>
#define MKDIR(p) mkdir(p, 0755)
#define PATH_SEP '/'
#endif

#ifndef MAX_PATH_LEN
#define MAX_PATH_LEN 1024
#endif

/* ============================================================================
 * 全局缓存目录
 * ============================================================================ */

const char* package_cache_dir(void) {
    static char dir[MAX_PATH_LEN] = {0};
    if (dir[0] != '\0') return dir;

#ifdef _WIN32
    const char* home = getenv("USERPROFILE");
    if (!home) home = getenv("HOMEDRIVE");  /* 回退 */
    if (!home) home = "C:";
    snprintf(dir, sizeof(dir), "%s\\.leno\\pkgs\\", home);
#else
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(dir, sizeof(dir), "%s/.leno/pkgs/", home);
#endif
    return dir;
}

int package_cache_ensure(void) {
    const char* dir = package_cache_dir();
    char buf[MAX_PATH_LEN];

    /* 逐级创建: ~/.leno/, ~/.leno/pkgs/ */
    strncpy(buf, dir, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* p = buf;
    while (*p) {
        if (*p == PATH_SEP || *p == '/') {
            char saved = *p;
            *p = '\0';
            if (buf[0] != '\0') {
#ifdef _WIN32
                /* 跳过驱动器根目录如 C:\ */
                if (strlen(buf) > 2 || (strlen(buf) == 2 && buf[1] != ':'))
                    MKDIR(buf);
#else
                MKDIR(buf);
#endif
            }
            *p = saved;
        }
        p++;
    }
    /* 创建最终目录 */
    size_t len = strlen(dir);
    if (len > 0 && len < MAX_PATH_LEN) {
        char final_dir[MAX_PATH_LEN];
        memcpy(final_dir, dir, len);
        final_dir[len - 1] = '\0';  /* 去掉尾部分隔符 */
        MKDIR(final_dir);
    }

    return 0;
}

/* ============================================================================
 * 搜索路径
 * ============================================================================ */

/**
 * 将指定目录下所有子目录的 lib/ 添加到模块搜索路径。
 * 目录结构: base_dir/<包名>/lib/  → 添加为搜索路径
 * 同时适用于：
 *   - 全局包缓存: ~/.leno/pkgs/<包名>/lib/
 *   - 内置模块:   exe_dir/leno_module/<包名>/lib/
 */
static void add_pkg_lib_search_paths(const char* base_dir) {
#ifdef _WIN32
    /* 枚举目录下的所有子目录 */
    char pattern[MAX_PATH_LEN];
    {
        size_t clen = strlen(base_dir);
        if (clen + 2 >= sizeof(pattern)) return;
        snprintf(pattern, sizeof(pattern), "%s*", base_dir);
    }

    WIN32_FIND_DATAW fd;
    wchar_t wpattern[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, pattern, -1, wpattern, MAX_PATH_LEN);
    HANDLE hFind = FindFirstFileW(wpattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
            continue;

        char name[MAX_PATH_LEN];
        WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1,
                            name, sizeof(name), NULL, NULL);

        char lib_path[MAX_PATH_LEN];
        {
            size_t clen = strlen(base_dir);
            size_t nlen = strlen(name);
            if (clen + nlen + 6 >= sizeof(lib_path)) continue;
            snprintf(lib_path, sizeof(lib_path), "%.*s%.*s%clib%c",
                     (int)clen, base_dir, (int)nlen, name, PATH_SEP, PATH_SEP);
        }

        /* 检查 lib/ 目录存在 */
        struct stat st;
        if (stat(lib_path, &st) == 0 && (st.st_mode & S_IFDIR)) {
            package_search_path_add(lib_path);
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
#else
    /* Linux/macOS: 用 opendir */
    #include <dirent.h>
    DIR* d = opendir(base_dir);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type != DT_DIR) continue;

        char lib_path[MAX_PATH_LEN];
        {
            size_t clen = strlen(base_dir);
            size_t nlen = strlen(ent->d_name);
            if (clen + nlen + 6 >= sizeof(lib_path)) continue;
            snprintf(lib_path, sizeof(lib_path), "%.*s%.*s%clib%c",
                     (int)clen, base_dir, (int)nlen, ent->d_name, PATH_SEP, PATH_SEP);
        }

        struct stat st;
        if (stat(lib_path, &st) == 0 && (st.st_mode & S_IFDIR)) {
            package_search_path_add(lib_path);
        }
    }
    closedir(d);
#endif
}

/* 全局包缓存搜索路径 */
void package_cache_add_to_search_paths(void) {
    const char* cache = package_cache_dir();
    add_pkg_lib_search_paths(cache);
}

/* 内置模块搜索路径: exe_dir/leno_module/<包名>/lib/ */
void package_builtin_add_to_search_paths(void) {
#ifdef _WIN32
    wchar_t wexe[MAX_PATH_LEN];
    GetModuleFileNameW(NULL, wexe, MAX_PATH_LEN);
    /* 转为 UTF-8 并提取目录 */
    char exe_dir[MAX_PATH_LEN];
    int conv = WideCharToMultiByte(CP_UTF8, 0, wexe, -1, exe_dir, MAX_PATH_LEN, NULL, NULL);
    if (conv <= 0) return;
    char* last_sep = strrchr(exe_dir, '\\');
    if (!last_sep) last_sep = strrchr(exe_dir, '/');
    if (last_sep) *(last_sep + 1) = '\0'; else exe_dir[0] = '\0';
#else
    char exe_dir[MAX_PATH_LEN];
    ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
    if (len <= 0) return;
    exe_dir[len] = '\0';
    char* last_sep = strrchr(exe_dir, '/');
    if (last_sep) *(last_sep + 1) = '\0'; else exe_dir[0] = '\0';
#endif

    if (exe_dir[0] == '\0') return;

    /* 拼接 exe_dir + "leno_module/"，提前检查长度 */
    size_t elen = strlen(exe_dir);
    if (elen + 12 >= (size_t)MAX_PATH_LEN) return;  /* "leno_module" + sep + '\0' */
    char builtin_dir[MAX_PATH_LEN];
    snprintf(builtin_dir, sizeof(builtin_dir), "%sleno_module%c", exe_dir, PATH_SEP);

    /* 检查内置模块目录存在 */
    struct stat st;
    if (stat(builtin_dir, &st) == 0 && (st.st_mode & S_IFDIR)) {
        add_pkg_lib_search_paths(builtin_dir);
    }
}

/* 内置模块搜索路径（从指定 exe 路径）: leno_exe_dir/leno_module/<包名>/lib/ */
void package_builtin_add_to_search_paths_from(const char* leno_exe_path) {
    if (!leno_exe_path || !leno_exe_path[0]) return;

    /* 提取目录部分（去掉文件名） */
    char exe_dir[MAX_PATH_LEN];
    strncpy(exe_dir, leno_exe_path, MAX_PATH_LEN - 1);
    exe_dir[MAX_PATH_LEN - 1] = '\0';

    char* last_sep = strrchr(exe_dir, '\\');
    if (!last_sep) last_sep = strrchr(exe_dir, '/');
    if (last_sep) {
        *(last_sep + 1) = '\0';
    } else {
        /* 没有路径分隔符，可能本身就是目录 */
        size_t dlen = strlen(exe_dir);
        if (dlen > 0 && exe_dir[dlen - 1] != '\\' && exe_dir[dlen - 1] != '/') {
            if (dlen + 1 < (size_t)MAX_PATH_LEN) {
                exe_dir[dlen] = PATH_SEP;
                exe_dir[dlen + 1] = '\0';
            }
        }
    }

    if (exe_dir[0] == '\0') return;

    /* 拼接 exe_dir + "leno_module/" */
    size_t elen = strlen(exe_dir);
    if (elen + 12 >= (size_t)MAX_PATH_LEN) return;
    char builtin_dir[MAX_PATH_LEN];
    snprintf(builtin_dir, sizeof(builtin_dir), "%sleno_module%c", exe_dir, PATH_SEP);

    /* 检查内置模块目录存在 */
    struct stat st;
    if (stat(builtin_dir, &st) == 0 && (st.st_mode & S_IFDIR)) {
        add_pkg_lib_search_paths(builtin_dir);
    }
}

/* ============================================================================
 * 文件/目录操作工具
 * ============================================================================ */

static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int dir_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

/* 递归复制目录 */
static int copy_dir(const char* src, const char* dst) {
#ifdef _WIN32
    /* 创建目标目录 */
    char cmd_buf[MAX_PATH_LEN * 4];
    snprintf(cmd_buf, sizeof(cmd_buf), "xcopy /E /I /Y /Q \"%s\" \"%s\" > nul", src, dst);
    int ret = system(cmd_buf);
    return (ret == 0) ? 0 : -1;
#else
    char cmd_buf[MAX_PATH_LEN * 4];
    snprintf(cmd_buf, sizeof(cmd_buf),
             "cp -r \"%s\" \"%s\" 2>/dev/null", src, dst);
    int ret = system(cmd_buf);
    return (ret == 0) ? 0 : -1;
#endif
}

/* ============================================================================
 * 文件复制
 * ============================================================================ */

/* 复制单个文件（Windows 走 CopyFileW 以支持 UTF-8/中文路径）。
 * 返回 0 成功，-1 失败（源不存在 / 目标不可写）。目标已存在则覆盖。 */
int package_copy_file(const char* src, const char* dst) {
    if (!src || !dst) return -1;
#ifdef _WIN32
    wchar_t* wsrc = utf8_to_utf16(src);
    if (!wsrc) return -1;
    wchar_t* wdst = utf8_to_utf16(dst);
    if (!wdst) { free(wsrc); return -1; }
    BOOL ok = CopyFileW(wsrc, wdst, FALSE);   /* FALSE = 允许覆盖 */
    free(wsrc);
    free(wdst);
    return ok ? 0 : -1;
#else
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[65536];
    size_t n;
    int rc = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { rc = -1; break; }
    }
    if (ferror(in)) rc = -1;
    fclose(in);
    if (fclose(out) != 0) rc = -1;
    if (rc != 0) remove(dst);
    return rc;
#endif
}

/* ============================================================================
 * 打包（-p）：原生库收集
 * ----------------------------------------------------------------------------
 * leno.toml 的 [native-libs.<名>] + win/linux/mac 三键（见 package_toml.c）从
 * 2026-06 起就在 schema 里、各模块也都填了，但一直**没有消费者** —— 解析出来
 * 存在 cfg->native_libs 里，全仓库只有 parse 和 free。这里补上打包侧唯一的消费者。
 *
 * 清单来源是"**实际 import 的模块**"，不是应用自己手抄一遍：
 *   每个已加载模块的源路径向上找最近的 leno.toml（复用 package_find_project_root）
 *   ⇒ 那就是它所属的包 ⇒ 取该包的 [native-libs] ⇒ 展开 ⇒ 去重。
 * 为什么必须按包推导：原生库是**包**的属性，且一个包可以需要多个库 —— LenoSDL3
 * 的 toml 原先只写了 SDL3.dll，但 sdl_image.leno / sdl_font.leno 还各自
 * ffi.load("SDL3_image.dll"/"SDL3_ttf.dll")，手抄必漏。
 * 应用自己的 leno.toml 也在枚举范围内（应用根就是它自己所属的"包"），
 * 所以"零散第三方 DLL 挂在应用根声明"这条路径天然可用，无需额外逻辑。
 * ============================================================================ */

#define MAX_PACK_ROOTS 64

/* 当前平台在 [native-libs] 里对应的键名；未知平台返回 NULL */
static const char* pack_platform_key(PlatformType p) {
    switch (p) {
        case PLATFORM_WINDOWS_X64:
        case PLATFORM_WINDOWS_X86:   return "win";
        case PLATFORM_LINUX_X64:
        case PLATFORM_LINUX_ARM64:   return "linux";
        case PLATFORM_MACOS_X64:
        case PLATFORM_MACOS_ARM64:   return "mac";
        default:                     return NULL;
    }
}

/* 取该平台声明的库路径；未声明返回 NULL */
static const char* native_lib_path_for(const NativeLib* nl, const char* key) {
    if (strcmp(key, "win") == 0) return nl->win_path;
    if (strcmp(key, "linux") == 0) return nl->linux_path;
    return nl->mac_path;
}

static const char* path_basename(const char* path) {
    const char* a = strrchr(path, '/');
    const char* b = strrchr(path, '\\');
    const char* sep = a;
    if (b && (!a || b > a)) sep = b;
    return sep ? sep + 1 : path;
}

/* 目录 + 相对路径 拼接（目录缺结尾分隔符时自动补） */
static void path_join(char* out, size_t out_size, const char* dir, const char* rel) {
    size_t dlen = strlen(dir);
    int need_sep = (dlen > 0 && dir[dlen - 1] != '/' && dir[dlen - 1] != '\\');
    if (need_sep) snprintf(out, out_size, "%s%c%s", dir, PATH_SEP, rel);
    else          snprintf(out, out_size, "%s%s", dir, rel);
}

/* UTF-8 路径安全的"是文件吗"检查（stat 在 Windows 上对中文路径会失败） */
static int pack_file_exists(const char* path) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return 0;
    DWORD attr = GetFileAttributesW(wp);
    free(wp);
    return (attr != INVALID_FILE_ATTRIBUTES) && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

#ifndef _WIN32
/* 通配匹配（支持 * 与 ?；POSIX 侧展开用。Windows 交给 FindFirstFileW 自己匹配）*/
static int wildcard_match(const char* pat, const char* str) {
    while (*pat) {
        if (*pat == '*') {
            pat++;
            if (!*pat) return 1;
            for (; *str; str++) {
                if (wildcard_match(pat, str)) return 1;
            }
            return 0;
        }
        if (*pat == '?') {
            if (!*str) return 0;
            pat++; str++;
            continue;
        }
        if (*pat != *str) return 0;
        pat++; str++;
    }
    return *str == '\0';
}
#endif /* !_WIN32 */

typedef struct {
    PackLib* items;
    int count;
    int cap;
    char roots[MAX_PACK_ROOTS][MAX_PATH_LEN];
    int root_count;
} PackCollector;

/* 收一条库（按**目标文件名**去重：同名视为同一个库，后到者忽略） */
static void collector_add(PackCollector* c, const char* src, const char* from_pkg) {
    const char* name = path_basename(src);
    for (int i = 0; i < c->count; i++) {
        if (strcmp(c->items[i].file_name, name) == 0) return;
    }
    if (c->count == c->cap) {
        int nc = c->cap ? c->cap * 2 : 8;
        PackLib* ni = (PackLib*)realloc(c->items, sizeof(PackLib) * (size_t)nc);
        if (!ni) return;
        c->items = ni;
        c->cap = nc;
    }
    c->items[c->count].src_path = strdup(src);
    c->items[c->count].file_name = strdup(name);
    c->items[c->count].from_pkg = strdup(from_pkg ? from_pkg : "<应用>");
    if (!c->items[c->count].src_path || !c->items[c->count].file_name ||
        !c->items[c->count].from_pkg) {
        free(c->items[c->count].src_path);
        free(c->items[c->count].file_name);
        free(c->items[c->count].from_pkg);
        return;
    }
    c->count++;
}

/* 由"某个源文件"登记其所属包根（去重） */
static void collector_add_root(PackCollector* c, const char* source_file) {
    if (!source_file || !source_file[0]) return;
    char* root = package_find_project_root(source_file);
    if (!root) return;
    for (int i = 0; i < c->root_count; i++) {
        if (strcmp(c->roots[i], root) == 0) { free(root); return; }
    }
    if (c->root_count < MAX_PACK_ROOTS) {
        strncpy(c->roots[c->root_count], root, MAX_PATH_LEN - 1);
        c->roots[c->root_count][MAX_PATH_LEN - 1] = '\0';
        c->root_count++;
    }
    free(root);
}

/* 展开一个含通配的条目：abs_dir 为绝对目录，pattern 为最后一段（含 * 或 ?）。
 * 返回匹配到并收录的个数；-1 表示目录打不开。 */
static int collector_expand_pattern(PackCollector* c, const char* abs_dir,
                                    const char* pattern, const char* from_pkg) {
    int matched = 0;
#ifdef _WIN32
    /* Windows 用系统自带通配（FindFirstFileW 直接接受 *.dll 这类模式） */
    char spec[MAX_PATH_LEN];
    path_join(spec, sizeof(spec), abs_dir, pattern);
    wchar_t* wspec = utf8_to_utf16(spec);
    if (!wspec) return -1;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wspec, &fd);
    free(wspec);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char name[MAX_PATH_LEN];
        if (WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof(name), NULL, NULL) <= 0)
            continue;
        char full[MAX_PATH_LEN];
        path_join(full, sizeof(full), abs_dir, name);
        collector_add(c, full, from_pkg);
        matched++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(abs_dir);
    if (!d) return -1;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.' && ent->d_name[1] == '\0') continue;
        if (!wildcard_match(pattern, ent->d_name)) continue;
        char full[MAX_PATH_LEN];
        path_join(full, sizeof(full), abs_dir, ent->d_name);
        if (!pack_file_exists(full)) continue;   /* 只收文件，跳过同名目录 */
        collector_add(c, full, from_pkg);
        matched++;
    }
    closedir(d);
#endif
    return matched;
}

int package_collect_pack_libs(const char* entry_file, PackLib** out, int* out_count) {
    if (!out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;

    const char* key = pack_platform_key(package_get_platform());
    if (!key) {
        /* 未知平台：不拦打包，只是收集不到（打包本身是当前平台自举的） */
        fprintf(stderr, "[pack] 提示: 未识别的平台，跳过原生库收集\n");
        return 0;
    }

    PackCollector c;
    memset(&c, 0, sizeof(c));

    /* 1) 入口文件所属"包" + 所有实际加载过的模块所属"包" */
    collector_add_root(&c, entry_file);
    int mod_count = loaded_modules_get_count();
    for (int i = 0; i < mod_count; i++) {
        collector_add_root(&c, loaded_modules_get_path(i));
    }

    /* 2) 逐包读 [native-libs]，取当前平台条目 */
    int rc = 0;
    for (int r = 0; r < c.root_count; r++) {
        char toml_path[MAX_PATH_LEN];
        path_join(toml_path, sizeof(toml_path), c.roots[r], "leno.toml");
        PackageConfig* cfg = package_config_parse(toml_path);
        if (!cfg) continue;
        const char* pkg = cfg->name ? cfg->name : c.roots[r];

        for (int k = 0; k < cfg->native_count; k++) {
            const char* rel = native_lib_path_for(&cfg->native_libs[k], key);
            if (!rel || !rel[0]) continue;   /* 该平台未声明这条 */

            /* 拆成 目录 + 末段（末段可能含通配） */
            char rel_buf[MAX_PATH_LEN];
            strncpy(rel_buf, rel, MAX_PATH_LEN - 1);
            rel_buf[MAX_PATH_LEN - 1] = '\0';
            char* slash = strrchr(rel_buf, '/');
            char* bslash = strrchr(rel_buf, '\\');
            if (bslash && (!slash || bslash > slash)) slash = bslash;

            char dir_abs[MAX_PATH_LEN];
            const char* last = rel_buf;
            if (slash) {
                *slash = '\0';
                path_join(dir_abs, sizeof(dir_abs), c.roots[r], rel_buf);
                last = slash + 1;
            } else {
                strncpy(dir_abs, c.roots[r], sizeof(dir_abs) - 1);
                dir_abs[sizeof(dir_abs) - 1] = '\0';
            }
            if (!last[0]) continue;

            if (strpbrk(last, "*?") == NULL) {
                /* 精确路径 */
                char full[MAX_PATH_LEN];
                path_join(full, sizeof(full), dir_abs, last);
                if (!pack_file_exists(full)) {
                    fprintf(stderr, "[pack] 错误: 包 '%s' 声明的原生库不存在: %s\n", pkg, full);
                    rc = -1;
                    continue;
                }
                collector_add(&c, full, pkg);
            } else {
                /* 通配：匹配不到也算声明落空 */
                int n = collector_expand_pattern(&c, dir_abs, last, pkg);
                if (n <= 0) {
                    fprintf(stderr, "[pack] 错误: 包 '%s' 的通配 '%s' 未匹配到任何文件（目录: %s）\n",
                            pkg, last, dir_abs);
                    rc = -1;
                }
            }
        }
        package_config_free(cfg);
    }

    if (rc != 0) {
        package_pack_libs_free(c.items, c.count);
        return -1;
    }

    *out = c.items;
    *out_count = c.count;
    return 0;
}

void package_pack_libs_free(PackLib* libs, int count) {
    if (!libs) return;
    for (int i = 0; i < count; i++) {
        free(libs[i].src_path);
        free(libs[i].file_name);
        free(libs[i].from_pkg);
    }
    free(libs);
}

/* ============================================================================
 * Git 源 URL 解析
 * ============================================================================ */

int package_parse_git_source(const char* source, char* out_url, int out_len,
                             char* out_subdir, int subdir_len) {
    if (!source || !out_url || out_len <= 0) return -1;

    /* 初始化 subdir */
    if (out_subdir && subdir_len > 0) out_subdir[0] = '\0';

    /* 检查是否是简写: platform:user/repo[/subdir...] */
    const char* colon = strchr(source, ':');
    if (colon && (strncmp(source, "http://", 7) != 0) && (strncmp(source, "https://", 8) != 0)) {
        size_t platform_len = colon - source;
        const char* path = colon + 1;

        /* 分离 user/repo 和 subdir（monorepo 支持） */
        char repo_path[MAX_PATH_LEN];
        repo_path[0] = '\0';
        const char* subdir = NULL;

        const char* first_slash = strchr(path, '/');
        const char* second_slash = first_slash ? strchr(first_slash + 1, '/') : NULL;

        if (second_slash) {
            /* 有子目录: user/repo/subdir... */
            size_t repo_len = second_slash - path;
            if (repo_len >= MAX_PATH_LEN) repo_len = MAX_PATH_LEN - 1;
            memcpy(repo_path, path, repo_len);
            repo_path[repo_len] = '\0';
            subdir = second_slash + 1;
            /* 跳过开头的 / */
            while (*subdir == '/') subdir++;
        } else {
            /* 没有子目录: user/repo */
            strncpy(repo_path, path, sizeof(repo_path) - 1);
            repo_path[sizeof(repo_path) - 1] = '\0';
        }

        if (strncmp(source, "gitee", platform_len) == 0) {
            snprintf(out_url, out_len, "https://gitee.com/%s.git", repo_path);
        } else if (strncmp(source, "github", platform_len) == 0) {
            snprintf(out_url, out_len, "https://github.com/%s.git", repo_path);
        } else if (strncmp(source, "gitlab", platform_len) == 0) {
            snprintf(out_url, out_len, "https://gitlab.com/%s.git", repo_path);
        } else if (strncmp(source, "git:", 4) == 0 && platform_len == 3) {
            snprintf(out_url, out_len, "https://github.com/%s.git", repo_path);
        } else {
            /* 未知平台前缀，原样返回（不拆分子目录） */
            strncpy(out_url, source, out_len - 1);
            out_url[out_len - 1] = '\0';
            return 0;
        }

        /* 输出子目录 */
        if (out_subdir && subdir_len > 0 && subdir && subdir[0] != '\0') {
            strncpy(out_subdir, subdir, subdir_len - 1);
            out_subdir[subdir_len - 1] = '\0';
        }
        return 0;
    }

    /* 完整 URL，不支持子目录提取，原样复制 */
    if (strncmp(source, "http://", 7) == 0 || strncmp(source, "https://", 8) == 0 ||
        source[0] == '/' || strstr(source, "git@")) {
        strncpy(out_url, source, out_len - 1);
        out_url[out_len - 1] = '\0';
        return 0;
    }

    /* 可能是完整 git URL */
    strncpy(out_url, source, out_len - 1);
    out_url[out_len - 1] = '\0';
    return 0;
}

/* ============================================================================
 * Git 远程安装
 * ============================================================================ */

int package_install_from_git(const char* git_url) {
    if (!git_url) return -1;

    /* 1. 解析出完整 URL 和子目录 */
    char full_url[MAX_PATH_LEN];
    char subdir[MAX_PATH_LEN];
    subdir[0] = '\0';
    if (package_parse_git_source(git_url, full_url, sizeof(full_url),
                                 subdir, sizeof(subdir)) != 0) {
        fprintf(stderr, "[leno install] 错误: 无法解析 git 源: '%s'\n", git_url);
        return -1;
    }

    printf("[leno install] 从 Git 安装: %s", full_url);
    if (subdir[0]) printf(" (子目录: %s)", subdir);
    printf("\n");

    /* 2. 检查 git 是否可用 */
    int git_ok = (system("git --version > nul 2>&1") == 0);
#ifndef _WIN32
    git_ok = (system("git --version > /dev/null 2>&1") == 0);
#endif
    if (!git_ok) {
        fprintf(stderr, "[leno install] 错误: 未找到 git，请先安装 Git\n");
        fprintf(stderr, "  下载: https://git-scm.com/downloads\n");
        return -1;
    }

    /* 3. 创建临时目录 */
    const char* cache = package_cache_dir();
    char tmp_dir[MAX_PATH_LEN];
    {
        size_t clen = strlen(cache);
        if (clen + 10 >= sizeof(tmp_dir)) {
            fprintf(stderr, "[leno install] 错误: 缓存路径过长\n");
            return -1;
        }
        snprintf(tmp_dir, sizeof(tmp_dir), "%s.leno-tmp", cache);
    }

#ifdef _WIN32
    char rm_cmd[MAX_PATH_LEN * 2];
    snprintf(rm_cmd, sizeof(rm_cmd), "rmdir /S /Q \"%s\" > nul 2>&1", tmp_dir);
    system(rm_cmd);
    MKDIR(tmp_dir);
#else
    char rm_cmd[MAX_PATH_LEN * 2];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", tmp_dir);
    { int _r = system(rm_cmd); (void)_r; }
    MKDIR(tmp_dir);
#endif

    /* 4. git clone --depth 1 */
    char clone_cmd[MAX_PATH_LEN * 4];
    snprintf(clone_cmd, sizeof(clone_cmd),
             "git clone --depth 1 \"%s\" \"%s\" > nul 2>&1",
             full_url, tmp_dir);
#ifndef _WIN32
    snprintf(clone_cmd, sizeof(clone_cmd),
             "git clone --depth 1 \"%s\" \"%s\" > /dev/null 2>&1",
             full_url, tmp_dir);
#endif

    printf("[leno install] git clone --depth 1...\n");
    int clone_ret = system(clone_cmd);
    if (clone_ret != 0) {
        fprintf(stderr, "[leno install] 错误: git clone 失败 (返回码: %d)\n", clone_ret);
        fprintf(stderr, "  URL: %s\n", full_url);
        fprintf(stderr, "  目标: %s\n", tmp_dir);
        return -1;
    }

    /* 5. 定位包目录：monorepo 则定位到子目录，否则用仓库根目录 */
    char pkg_dir[MAX_PATH_LEN];
    if (subdir[0]) {
        size_t tlen = strlen(tmp_dir);
        size_t slen = strlen(subdir);
        if (tlen + slen + 2 >= sizeof(pkg_dir)) {
            fprintf(stderr, "[leno install] 错误: 包路径过长\n");
            return -1;
        }
        snprintf(pkg_dir, sizeof(pkg_dir), "%.*s%c%.*s",
                 (int)tlen, tmp_dir, PATH_SEP, (int)slen, subdir);
    } else {
        strncpy(pkg_dir, tmp_dir, sizeof(pkg_dir) - 1);
        pkg_dir[sizeof(pkg_dir) - 1] = '\0';
    }

    /* 5.1 检查包目录中是否有 leno.toml */
    char cloned_toml[MAX_PATH_LEN];
    {
        size_t plen = strlen(pkg_dir);
        if (plen + 11 >= sizeof(cloned_toml)) return -1;
        snprintf(cloned_toml, sizeof(cloned_toml), "%s%cleno.toml", pkg_dir, PATH_SEP);
    }
    if (!file_exists(cloned_toml)) {
        fprintf(stderr, "[leno install] 错误: 克隆的仓库中缺少 leno.toml\n");
        fprintf(stderr, "  路径: %s\n", cloned_toml);
        if (subdir[0])
            fprintf(stderr, "  提示: 子目录 '%s' 中未找到 leno.toml\n", subdir);
        return -1;
    }

    /* 6. 解析 leno.toml 获取包名 */
    PackageConfig* cfg = package_config_parse(cloned_toml);
    if (!cfg || !cfg->name) {
        fprintf(stderr, "[leno install] 错误: 无法解析包配置\n");
        if (cfg) package_config_free(cfg);
        return -1;
    }

    /* 7. 检查 lib/ 目录 */
    char cloned_lib[MAX_PATH_LEN];
    {
        size_t plen = strlen(pkg_dir);
        if (plen + 5 >= sizeof(cloned_lib)) return -1;
        snprintf(cloned_lib, sizeof(cloned_lib), "%s%clib", pkg_dir, PATH_SEP);
    }
    if (!dir_exists(cloned_lib)) {
        fprintf(stderr, "[leno install] 警告: 包 '%s' 没有 lib/ 目录，可能无法正常导入\n", cfg->name);
    }

    /* 8. 安装到缓存（从子目录安装） */
    int ret = package_install_from_dir(pkg_dir);

    /* 9. 清理临时目录 */
#ifdef _WIN32
    snprintf(rm_cmd, sizeof(rm_cmd), "rmdir /S /Q \"%s\" > nul 2>&1", tmp_dir);
#else
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", tmp_dir);
#endif
    { int _r = system(rm_cmd); (void)_r; }

    package_config_free(cfg);
    return ret;
}

/* ============================================================================
 * 包安装
 * ============================================================================ */

int package_install_from_dir(const char* pkg_path) {
    if (!pkg_path) return -1;

    /* 1. 检查 leno.toml 是否存在 */
    char toml_path[MAX_PATH_LEN];
    {
        size_t plen = strlen(pkg_path);
        if (plen + 11 >= sizeof(toml_path)) return -1;
        snprintf(toml_path, sizeof(toml_path), "%s%cleno.toml", pkg_path, PATH_SEP);
    }
    if (!file_exists(toml_path)) {
        fprintf(stderr, "[leno install] 错误: 在 '%s' 中找不到 leno.toml\n", pkg_path);
        return -1;
    }

    /* 2. 解析 leno.toml 获取包名 */
    PackageConfig* cfg = package_config_parse(toml_path);
    if (!cfg || !cfg->name) {
        fprintf(stderr, "[leno install] 错误: 无法解析 '%s'\n", toml_path);
        if (cfg) package_config_free(cfg);
        return -1;
    }

    /* 3. 确保全局缓存目录存在 */
    package_cache_ensure();

    /* 4. 计算目标路径 */
    const char* cache = package_cache_dir();
    char dst_dir[MAX_PATH_LEN];
    {
        size_t clen = strlen(cache);
        size_t nlen = strlen(cfg->name);
        if (clen + nlen + 2 >= sizeof(dst_dir)) return -1;
        snprintf(dst_dir, sizeof(dst_dir), "%.*s%.*s%c",
                 (int)clen, cache, (int)nlen, cfg->name, PATH_SEP);
    }

    /* 5. 如果已存在，先删除再覆盖 */
    if (dir_exists(dst_dir)) {
        fprintf(stderr, "[leno install] 包 '%s' 已安装，正在覆盖...\n", cfg->name);
#ifdef _WIN32
        char rm_cmd[MAX_PATH_LEN * 2];
        snprintf(rm_cmd, sizeof(rm_cmd), "rmdir /S /Q \"%s\" > nul 2>&1", dst_dir);
        system(rm_cmd);
#else
        char rm_cmd[MAX_PATH_LEN * 2];
        snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf \"%s\"", dst_dir);
        { int _r = system(rm_cmd); (void)_r; }
#endif
    }

    /* 6. 复制整个包目录到缓存 */
    if (copy_dir(pkg_path, dst_dir) != 0) {
        fprintf(stderr, "[leno install] 错误: 复制 '%s' 到 '%s' 失败\n", pkg_path, dst_dir);
        package_config_free(cfg);
        return -1;
    }

    printf("[leno install] 包 '%s' (%s) 安装成功!\n", cfg->name,
           cfg->version ? cfg->version : "unknown");
    printf("  安装到: %s\n", dst_dir);

    /* 7. 检查是否有依赖需要安装 */
    if (cfg->dep_count > 0) {
        printf("  依赖: ");
        for (int i = 0; i < cfg->dep_count; i++) {
            printf("%s%s%s", i > 0 ? ", " : "",
                   cfg->dependencies[i].name,
                   cfg->dependencies[i].version_str ? " " : "");
            if (cfg->dependencies[i].version_str)
                printf("%s", cfg->dependencies[i].version_str);
        }
        printf("\n");
        printf("[leno install] 提示: 运行 'leno --install' 来安装依赖\n");
    }

    package_config_free(cfg);
    return 0;
}

int package_install_deps(const char* toml_path) {
    if (!toml_path) return -1;

    PackageConfig* cfg = package_config_parse(toml_path);
    if (!cfg) {
        fprintf(stderr, "[leno install] 错误: 无法解析 '%s'\n", toml_path);
        return -1;
    }

    if (cfg->dep_count == 0) {
        printf("[leno install] 没有依赖需要安装\n");
        package_config_free(cfg);
        return 0;
    }

    printf("[leno install] 从 '%s' 安装 %d 个依赖...\n",
           cfg->name ? cfg->name : "leno.toml", cfg->dep_count);

    int fail_count = 0;
    int skip_count = 0;
    int ok_count = 0;

    for (int i = 0; i < cfg->dep_count; i++) {
        const char* dep_name = cfg->dependencies[i].name;
        if (!dep_name) continue;

        /* 检查是否已在缓存中 */
        const char* cache = package_cache_dir();
        char cached_lib[MAX_PATH_LEN];
        {
            size_t clen = strlen(cache);
            size_t nlen = strlen(dep_name);
            if (clen + nlen * 2 + 11 >= sizeof(cached_lib)) {
                fprintf(stderr, "  [失败] %s - 路径过长\n", dep_name);
                fail_count++;
                continue;
            }
            snprintf(cached_lib, sizeof(cached_lib), "%.*s%.*s%clib%c%.*s.leno",
                     (int)clen, cache, (int)nlen, dep_name, PATH_SEP, PATH_SEP,
                     (int)nlen, dep_name);
        }

        if (file_exists(cached_lib)) {
            printf("  [跳过] %s (已安装)\n", dep_name);
            skip_count++;
            continue;
        }

        /* 尝试从 Git 源安装 */
        if (cfg->dependencies[i].source) {
            printf("  [安装] %s ← %s\n", dep_name, cfg->dependencies[i].source);
            if (package_install_from_git(cfg->dependencies[i].source) == 0) {
                ok_count++;
            } else {
                fail_count++;
            }
            continue;
        }

        /* 没有 source，无法安装 */
        fprintf(stderr, "  [失败] %s - 没有指定 git 源\n", dep_name);
        fprintf(stderr, "         提示: 在 leno.toml 中添加 [dependency-sources]\n");
        fprintf(stderr, "         %s = \"gitee:user/%s\"\n", dep_name, dep_name);
        fprintf(stderr, "         或用 'leno --install <git-url>' 手动安装\n");
        fail_count++;
    }

    printf("[leno install] 完成: %d 成功, %d 跳过, %d 失败\n",
           ok_count, skip_count, fail_count);

    package_config_free(cfg);
    return (fail_count > 0) ? -1 : 0;
}
