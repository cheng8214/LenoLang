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
 * 缓存搜索路径
 * ============================================================================ */

void package_cache_add_to_search_paths(void) {
    const char* cache = package_cache_dir();

#ifdef _WIN32
    /* 枚举缓存目录下的所有子目录 */
    char pattern[MAX_PATH_LEN];
    {
        size_t clen = strlen(cache);
        if (clen + 2 >= sizeof(pattern)) return;
        snprintf(pattern, sizeof(pattern), "%s*", cache);
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
            size_t clen = strlen(cache);
            size_t nlen = strlen(name);
            if (clen + nlen + 6 >= sizeof(lib_path)) continue;
            snprintf(lib_path, sizeof(lib_path), "%.*s%.*s%clib%c",
                     (int)clen, cache, (int)nlen, name, PATH_SEP, PATH_SEP);
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
    DIR* d = opendir(cache);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        if (ent->d_type != DT_DIR) continue;

        char lib_path[MAX_PATH_LEN];
        {
            size_t clen = strlen(cache);
            size_t nlen = strlen(ent->d_name);
            if (clen + nlen + 6 >= sizeof(lib_path)) continue;
            snprintf(lib_path, sizeof(lib_path), "%.*s%.*s%clib%c",
                     (int)clen, cache, (int)nlen, ent->d_name, PATH_SEP, PATH_SEP);
        }

        struct stat st;
        if (stat(lib_path, &st) == 0 && (st.st_mode & S_IFDIR)) {
            package_search_path_add(lib_path);
        }
    }
    closedir(d);
#endif
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

/* 简单文件复制（当前未使用，保留用于未来扩展） */
#if 0
static int copy_file(const char* src, const char* dst) {
    FILE* fsrc = fopen(src, "rb");
    if (!fsrc) return -1;
    FILE* fdst = fopen(dst, "wb");
    if (!fdst) { fclose(fsrc); return -1; }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fsrc)) > 0) {
        fwrite(buf, 1, n, fdst);
    }
    fclose(fsrc);
    fclose(fdst);
    return 0;
}
#endif

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
    system(rm_cmd);
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
    system(rm_cmd);

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
        system(rm_cmd);
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
