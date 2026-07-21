#ifndef LENO_PACKAGE_H
#define LENO_PACKAGE_H

#include <stdint.h>

/* ============================================================================
 * 平台标识
 * ============================================================================ */
typedef enum {
    PLATFORM_WINDOWS_X64,
    PLATFORM_WINDOWS_X86,
    PLATFORM_LINUX_X64,
    PLATFORM_LINUX_ARM64,
    PLATFORM_MACOS_X64,
    PLATFORM_MACOS_ARM64,
    PLATFORM_UNKNOWN
} PlatformType;

/* ============================================================================
 * 依赖版本约束
 * ============================================================================ */
typedef enum {
    VER_EXACT,     /* =1.2.3  */
    VER_COMPAT,    /*  ~1.2.3 → >=1.2.3 <1.3.0 */
    VER_RANGE,     /* >=1.0 <2.0 */
    VER_GTE,       /* >=1.2.3  */
    VER_ANY        /* *  */
} VersionConstraint;

/* 语义化版本 */
typedef struct {
    int major;
    int minor;
    int patch;
    char* prerelease;  /* -alpha, -beta.1, NULL */
    char* build;       /* +build001, NULL */
} SemVer;

/* 版本约束条目 */
typedef struct {
    VersionConstraint type;
    SemVer version;        /* 用于 EXACT, COMPAT, GTE */
    SemVer version_upper;  /* 用于 RANGE 的上界 */
} VersionReq;

/* ============================================================================
 * 原生库映射
 * ============================================================================ */
typedef struct {
    char* lib_name;      /* 别名: "sqlite3" */
    char* win_path;      /* native/sqlite3.dll */
    char* linux_path;    /* native/libsqlite3.so */
    char* mac_path;      /* native/libsqlite3.dylib */
} NativeLib;

/* ============================================================================
 * 包配置（对应 leno.toml）
 * ============================================================================ */
#define MAX_DEPENDENCIES 64
#define MAX_NATIVE_LIBS  32

typedef struct {
    /* [package] */
    char* name;
    char* version;        /* 原始字符串 "1.2.0" */
    char* description;
    char* authors;
    char* license;
    char* leno_version;   /* 要求的 Leno 编译器版本 */

    /* [dependencies] */
    int dep_count;
    struct {
        char* name;
        char* version_str;  /* 原始版本约束字符串 */
        VersionReq req;     /* 解析后的约束 */
        char* source;       /* git 源 URL（如 gitee:user/repo） */
    } dependencies[MAX_DEPENDENCIES];

    /* [dev-dependencies] */
    int dev_dep_count;
    struct {
        char* name;
        char* version_str;
        VersionReq req;
        char* source;       /* git 源 URL */
    } dev_dependencies[MAX_DEPENDENCIES];

    /* [native-libs] */
    int native_count;
    NativeLib native_libs[MAX_NATIVE_LIBS];

    /* [modules] */
    char* module_root;    /* 自定义模块加载根目录 */

    /* 元数据 */
    char* file_path;      /* leno.toml 的完整路径 */
} PackageConfig;

/* ============================================================================
 * 平台检测
 * ============================================================================ */

/** 返回当前平台类型 */
PlatformType package_get_platform(void);

/** 返回平台名称字符串 */
const char* package_platform_name(PlatformType platform);

/** 返回平台原生库后缀 */
const char* package_platform_dylib_ext(void);

/* ============================================================================
 * TOML 解析
 * ============================================================================ */

/** 解析 leno.toml 文件 */
PackageConfig* package_config_parse(const char* file_path);

/** 释放包配置 */
void package_config_free(PackageConfig* config);

/** 解析语义化版本字符串 */
int semver_parse(const char* str, SemVer* out);

/** 解析版本约束字符串 */
int version_req_parse(const char* str, VersionReq* out);

/** 检查版本是否满足约束 */
int version_satisfies(SemVer* version, VersionReq* req);

/* ============================================================================
 * init 命令
 * ============================================================================ */

/** 在当前目录创建 leno.toml + 项目骨架 */
int package_init(const char* dir_path, const char* package_name);

/* ============================================================================
 * 锁文件
 * ============================================================================ */

#define MAX_LOCK_PACKAGES 256

typedef struct {
    char* name;
    SemVer version;
    char* source;     /* git URL */
    char* commit;     /* git commit hash */
    char* sha256;     /* 校验和 */
} LockEntry;

typedef struct {
    char* leno_version;
    int package_count;
    LockEntry packages[MAX_LOCK_PACKAGES];
} PkgLockFile;

/** 解析 leno.lock */
PkgLockFile* lockfile_parse(const char* file_path);

/** 写入 leno.lock */
int lockfile_write(const char* file_path, PkgLockFile* lock);

/** 释放锁文件 */
void lockfile_free(PkgLockFile* lock);

/* ============================================================================
 * 模块搜索路径
 * ============================================================================ */

#define MAX_SEARCH_PATHS 32

/**
 * 根据源文件路径向上查找 leno.toml，返回项目根目录的绝对路径。
 * 返回的字符串由调用者 free()。
 * @param source_file  当前编译的 .leno 文件绝对路径
 * @return 项目根目录（含结尾分隔符），未找到则返回 NULL
 */
char* package_find_project_root(const char* source_file);

/**
 * 向搜索路径列表尾部追加一个路径。
 * @return 0 成功，-1 路径列表已满
 */
int package_search_path_add(const char* path);

/** 清空所有搜索路径 */
void package_search_path_clear(void);

/** 获取当前搜索路径数量 */
int package_search_path_count(void);

/** 获取第 i 条搜索路径 */
const char* package_search_path_get(int index);

/**
 * 在搜索路径中查找模块文件。
 * 尝试在每个搜索路径下查找 <module_name>.leno
 * @param module_name  模块名（如 "test_pkg"）
 * @param out_path     输出缓冲区，成功时写入找到的完整路径（相对或绝对均可）
 * @param out_len      输出缓冲区大小
 * @return 1 找到，-1 未找到
 */
int package_resolve_module_file(const char* module_name, char* out_path, int out_len);

/* ============================================================================
 * 全局包缓存
 * ============================================================================ */

/**
 * 获取全局包缓存目录。
 * Windows: %USERPROFILE%\.leno\pkgs\
 * 其他:    ~/.leno/pkgs/
 * 返回静态缓冲区，不需要释放。
 */
const char* package_cache_dir(void);

/**
 * 确保全局缓存目录存在。
 * @return 0 成功，-1 创建失败
 */
int package_cache_ensure(void);

/**
 * 将缓存目录（及其下各包的 lib/ 子目录）添加到模块搜索路径。
 * 这样 import "包名" 就能直接在缓存中查找。
 */
void package_cache_add_to_search_paths(void);

/**
 * 将内置模块目录（exe_dir/leno_module/<包名>/lib/）添加到模块搜索路径。
 * 结构与全局缓存一致，便于随 exe 分发内置包模块（如 LenoSDL3）。
 */
void package_builtin_add_to_search_paths(void);

/* ============================================================================
 * 包安装
 * ============================================================================ */

/**
 * 安装一个包到全局缓存。
 * 目前支持从本地目录安装（复制到缓存）。
 * @param pkg_path  包目录路径（包含 leno.toml 的那个目录）
 * @return 0 成功，-1 失败
 */
int package_install_from_dir(const char* pkg_path);

/**
 * 根据 leno.toml 安装所有依赖。
 * @param toml_path  leno.toml 文件路径
 * @return 0 成功，-1 有依赖安装失败
 */
int package_install_deps(const char* toml_path);

/**
 * 从 Git 远程仓库安装包到全局缓存。
 * 支持完整 URL 和简写：gitee:user/repo → https://gitee.com/user/repo.git
 * @param git_url  Git 仓库地址（完整 URL 或简写）
 * @return 0 成功，-1 失败
 */
int package_install_from_git(const char* git_url);

/**
 * 将简写 git 源转换为完整 URL，同时解出子目录路径（monorepo 支持）。
 *   gitee:user/repo          → https://gitee.com/user/repo.git
 *   gitee:user/repo/pkg-a    → https://gitee.com/user/repo.git  + subdir="pkg-a"
 *   gitee:user/repo/a/b/c    → https://gitee.com/user/repo.git  + subdir="a/b/c"
 *   github:user/repo         → https://github.com/user/repo.git
 *   gitlab:user/repo         → https://gitlab.com/user/repo.git
 *   完整 URL 原样返回（不支持带子目录的完整 URL，请用简写）
 * @param source      输入简写/URL
 * @param out_url     输出完整 git clone URL
 * @param out_len     out_url 缓冲区大小
 * @param out_subdir  输出子目录路径（可为 NULL 表示不需要）。无子目录时返回空串。
 * @param subdir_len  out_subdir 缓冲区大小
 * @return 0 成功，-1 格式无效
 */
int package_parse_git_source(const char* source, char* out_url, int out_len,
                             char* out_subdir, int subdir_len);

#endif /* LENO_PACKAGE_H */
