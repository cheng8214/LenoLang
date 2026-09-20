#include "../include/leno_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ================================================================
 * 内部工具：字符串操作
 * ================================================================ */

static char* str_dup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* d = (char*)malloc(len + 1);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

static char* str_trim(char* s) {
    if (!s) return NULL;
    while (isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char* end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
    return s;
}

/* ================================================================
 * 语义化版本解析
 * ================================================================ */

int semver_parse(const char* str, SemVer* out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(SemVer));

    /* 去除前导空白 */
    while (isspace((unsigned char)*str)) str++;
    if (*str == 'v' || *str == 'V') str++;  /* 跳过可选 v 前缀 */

    if (sscanf(str, "%d.%d.%d", &out->major, &out->minor, &out->patch) < 3) {
        return -1;
    }

    /* 跳过数字和点号部分，定位到 prerelease/build */
    const char* p = str;
    while (*p && (isdigit((unsigned char)*p) || *p == '.')) p++;

    if (*p == '-') {
        const char* start = p + 1;
        p++;
        while (*p && *p != '+') p++;
        size_t len = p - start;
        if (len > 0) {
            out->prerelease = (char*)malloc(len + 1);
            memcpy(out->prerelease, start, len);
            out->prerelease[len] = '\0';
        }
    }
    if (*p == '+') {
        const char* start = p + 1;
        p++;
        while (*p) p++;
        size_t len = p - start;
        if (len > 0) {
            out->build = (char*)malloc(len + 1);
            memcpy(out->build, start, len);
            out->build[len] = '\0';
        }
    }
    return 0;
}

/* ================================================================
 * 版本约束解析
 * ================================================================ */

int version_req_parse(const char* str, VersionReq* out) {
    if (!str || !out) return -1;
    memset(out, 0, sizeof(VersionReq));

    /* 跳过前导空白 */
    while (isspace((unsigned char)*str)) str++;

    if (strcmp(str, "*") == 0 || strlen(str) == 0) {
        out->type = VER_ANY;
        return 0;
    }

    /* ~1.2.3 → COMPAT */
    if (str[0] == '~') {
        out->type = VER_COMPAT;
        return semver_parse(str + 1, &out->version);
    }

    /* >=1.0, <2.0 → RANGE */
    const char* comma = strstr(str, ",");
    if (comma) {
        out->type = VER_RANGE;
        /* 解析第一个部分 */
        char first[64];
        size_t len = comma - str;
        if (len >= sizeof(first)) len = sizeof(first) - 1;
        memcpy(first, str, len);
        first[len] = '\0';
        char* s = str_trim(first);

        if (strncmp(s, ">=", 2) == 0) {
            semver_parse(s + 2, &out->version);
        } else if (s[0] == '>') {
            semver_parse(s + 1, &out->version);
        } else {
            semver_parse(s, &out->version);
        }

        /* 解析第二个部分 */
        const char* s2 = comma + 1;
        while (isspace((unsigned char)*s2)) s2++;
        if (strncmp(s2, "<=", 2) == 0) {
            semver_parse(s2 + 2, &out->version_upper);
        } else if (strncmp(s2, "<", 1) == 0) {
            semver_parse(s2 + 1, &out->version_upper);
        } else {
            semver_parse(s2, &out->version_upper);
        }
        return 0;
    }

    /* >=1.2.3 */
    if (strncmp(str, ">=", 2) == 0) {
        out->type = VER_GTE;
        return semver_parse(str + 2, &out->version);
    }

    /* =1.2.3 或纯粹 1.2.3 → EXACT */
    const char* ver_str = str;
    if (ver_str[0] == '=') ver_str++;
    out->type = VER_EXACT;
    return semver_parse(ver_str, &out->version);
}

/* ================================================================
 * 版本满足性检查
 * ================================================================ */

static int semver_cmp(SemVer* a, SemVer* b) {
    if (a->major != b->major) return a->major - b->major;
    if (a->minor != b->minor) return a->minor - b->minor;
    if (a->patch != b->patch) return a->patch - b->patch;
    return 0;
}

int version_satisfies(SemVer* version, VersionReq* req) {
    if (!version || !req) return 0;

    switch (req->type) {
        case VER_ANY:
            return 1;

        case VER_EXACT:
            return semver_cmp(version, &req->version) == 0;

        case VER_GTE:
            return semver_cmp(version, &req->version) >= 0;

        case VER_COMPAT: {
            /* ~1.2.3 → >=1.2.3 <1.3.0 */
            if (semver_cmp(version, &req->version) < 0) return 0;
            SemVer upper = req->version;
            if (upper.major > 0) {
                upper.minor++;
                upper.patch = 0;
            } else {
                upper.patch++;
            }
            return semver_cmp(version, &upper) < 0;
        }

        case VER_RANGE:
            return semver_cmp(version, &req->version) >= 0 &&
                   semver_cmp(version, &req->version_upper) < 0;
    }
    return 0;
}

/* ================================================================
 * 简单 .toml 解析器
 *
 * 解析策略：逐行读取，支持：
 *   [section]
 *   [section.sub]
 *   key = "value"
 *   key = value
 *   key = 'value'
 *   # 注释
 * ================================================================ */

#define MAX_LINE 1024
#define MAX_SECTION 128

typedef struct {
    char name[MAX_SECTION];    /* 当前段名，如 "package" 或 "dependencies" */
    char parent[MAX_SECTION];  /* 父段名，如 "native-libs.windows-x64" */
    int  table_depth;          /* 段嵌套深度 */
} TomlParser;

static void toml_parser_init(TomlParser* tp) {
    tp->name[0] = '\0';
    tp->parent[0] = '\0';
    tp->table_depth = 0;
}

/* 解析 [section.sub] → parent="section", name="sub", depth=2 */
static int toml_parse_section_header(const char* line, TomlParser* tp) {
    const char* start = line;
    while (*start && isspace((unsigned char)*start)) start++;
    if (*start != '[') return -1;

    const char* end = start + 1;
    /* 检查是否是 [[array]] */
    if (*end == '[') {
        /* 暂不支持数组表，跳过 */
        return -1;
    }

    int depth = 0;
    const char* p = end;
    while (*p && *p != ']') p++;
    if (*p != ']') return -1;

    /* 提取内容 */
    size_t len = p - end;
    if (len >= MAX_SECTION) len = MAX_SECTION - 1;

    char content[MAX_SECTION];
    memcpy(content, end, len);
    content[len] = '\0';

    /* trim */
    char* s = content;
    while (isspace((unsigned char)*s)) s++;
    char* e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) { *e = '\0'; e--; }

    /* 找 '.' 分隔 */
    char* dot = strchr(s, '.');
    if (dot) {
        *dot = '\0';
        strncpy(tp->parent, s, MAX_SECTION - 1);
        tp->parent[MAX_SECTION - 1] = '\0';
        strncpy(tp->name, dot + 1, MAX_SECTION - 1);
        tp->name[MAX_SECTION - 1] = '\0';
        depth = 2;
    } else {
        tp->parent[0] = '\0';
        strncpy(tp->name, s, MAX_SECTION - 1);
        tp->name[MAX_SECTION - 1] = '\0';
        depth = 1;
    }
    tp->table_depth = depth;
    return 0;
}

/* 解析 key = value，返回 key 和 value（动态分配） */
static int toml_parse_kv(const char* line, char** out_key, char** out_val) {
    *out_key = NULL;
    *out_val = NULL;

    const char* eq = strchr(line, '=');
    if (!eq) return -1;

    /* 提取 key */
    const char* kstart = line;
    while (kstart < eq && isspace((unsigned char)*kstart)) kstart++;
    const char* kend = eq - 1;
    while (kend > kstart && isspace((unsigned char)*kend)) kend--;

    size_t klen = kend - kstart + 1;
    if (klen == 0) return -1;

    *out_key = (char*)malloc(klen + 1);
    memcpy(*out_key, kstart, klen);
    (*out_key)[klen] = '\0';

    /* 提取 value（保留原始，之后 trim） */
    const char* vstart = eq + 1;
    while (*vstart && isspace((unsigned char)*vstart)) vstart++;
    const char* vend = vstart + strlen(vstart) - 1;
    while (vend > vstart && isspace((unsigned char)*vend)) vend--;

    size_t vlen = vend - vstart + 1;
    *out_val = (char*)malloc(vlen + 1);
    memcpy(*out_val, vstart, vlen);
    (*out_val)[vlen] = '\0';

    /* 去除引号 */
    if (vlen >= 2 &&
        (((*out_val)[0] == '"' && (*out_val)[vlen - 1] == '"') ||
         ((*out_val)[0] == '\'' && (*out_val)[vlen - 1] == '\''))) {
        memmove(*out_val, *out_val + 1, vlen - 1);
        (*out_val)[vlen - 2] = '\0';
    }

    return 0;
}

/* ================================================================
 * 公开 API：解析 leno.toml
 * ================================================================ */

PackageConfig* package_config_parse(const char* file_path) {
    FILE* fp = fopen(file_path, "rb");
    if (!fp) return NULL;

    PackageConfig* cfg = (PackageConfig*)calloc(1, sizeof(PackageConfig));
    if (!cfg) {
        fclose(fp);
        return NULL;
    }

    cfg->file_path = str_dup(file_path);

    TomlParser tp;
    toml_parser_init(&tp);

    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), fp)) {
        /* 去除尾换行 */
        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r')) {
            buf[--blen] = '\0';
        }

        char* line = str_trim(buf);
        if (line[0] == '\0' || line[0] == '#') continue;

        /* 段标题 */
        if (line[0] == '[' && line[1] != '[') {
            toml_parse_section_header(line, &tp);
            continue;
        }

        /* key = value */
        char* key = NULL;
        char* val = NULL;
        if (toml_parse_kv(line, &key, &val) != 0) {
            continue;
        }

        /* 根据段名分配 */
        if (strcmp(tp.name, "package") == 0 && tp.table_depth == 1) {
            if (strcmp(key, "name") == 0) {
                cfg->name = str_dup(val);
            } else if (strcmp(key, "version") == 0) {
                cfg->version = str_dup(val);
            } else if (strcmp(key, "description") == 0) {
                cfg->description = str_dup(val);
            } else if (strcmp(key, "authors") == 0) {
                cfg->authors = str_dup(val);
            } else if (strcmp(key, "license") == 0) {
                cfg->license = str_dup(val);
            } else if (strcmp(key, "leno_version") == 0) {
                cfg->leno_version = str_dup(val);
            }
        } else if (strcmp(tp.name, "dependencies") == 0 && tp.table_depth == 1) {
            if (cfg->dep_count < MAX_DEPENDENCIES) {
                int idx = cfg->dep_count++;
                cfg->dependencies[idx].name = str_dup(key);
                cfg->dependencies[idx].version_str = str_dup(val);
                version_req_parse(val, &cfg->dependencies[idx].req);
            }
        } else if (strcmp(tp.name, "dev-dependencies") == 0 && tp.table_depth == 1) {
            if (cfg->dev_dep_count < MAX_DEPENDENCIES) {
                int idx = cfg->dev_dep_count++;
                cfg->dev_dependencies[idx].name = str_dup(key);
                cfg->dev_dependencies[idx].version_str = str_dup(val);
                version_req_parse(val, &cfg->dev_dependencies[idx].req);
            }
        } else if (strcmp(tp.name, "dependency-sources") == 0 && tp.table_depth == 1) {
            /* [dependency-sources] → key=包名, value=git源URL */
            /* 查找对应的依赖条目并设置 source */
            for (int i = 0; i < cfg->dep_count; i++) {
                if (cfg->dependencies[i].name && strcmp(cfg->dependencies[i].name, key) == 0) {
                    cfg->dependencies[i].source = str_dup(val);
                    break;
                }
            }
        } else if (strcmp(tp.parent, "native-libs") == 0 && tp.table_depth == 2) {
            /* [native-libs.sqlite3] → key="win" val="native/sqlite3.dll" */
            /* 查找或创建 NativeLib 条目 */
            int found = -1;
            for (int i = 0; i < cfg->native_count; i++) {
                if (strcmp(cfg->native_libs[i].lib_name, tp.name) == 0) {
                    found = i;
                    break;
                }
            }
            if (found < 0 && cfg->native_count < MAX_NATIVE_LIBS) {
                found = cfg->native_count++;
                cfg->native_libs[found].lib_name = str_dup(tp.name);
            }
            if (found >= 0) {
                if (strcmp(key, "win") == 0) {
                    cfg->native_libs[found].win_path = str_dup(val);
                } else if (strcmp(key, "linux") == 0) {
                    cfg->native_libs[found].linux_path = str_dup(val);
                } else if (strcmp(key, "mac") == 0) {
                    cfg->native_libs[found].mac_path = str_dup(val);
                }
            }
        } else if (strcmp(tp.name, "modules") == 0 && tp.table_depth == 1) {
            if (strcmp(key, "root") == 0) {
                cfg->module_root = str_dup(val);
            }
        }

        free(key);
        free(val);
    }

    fclose(fp);
    return cfg;
}

void package_config_free(PackageConfig* cfg) {
    if (!cfg) return;

    free(cfg->name);
    free(cfg->version);
    free(cfg->description);
    free(cfg->authors);
    free(cfg->license);
    free(cfg->leno_version);
    free(cfg->module_root);
    free(cfg->file_path);

    for (int i = 0; i < cfg->dep_count; i++) {
        free(cfg->dependencies[i].name);
        free(cfg->dependencies[i].version_str);
        free(cfg->dependencies[i].source);
        free(cfg->dependencies[i].req.version.prerelease);
        free(cfg->dependencies[i].req.version.build);
        free(cfg->dependencies[i].req.version_upper.prerelease);
        free(cfg->dependencies[i].req.version_upper.build);
    }
    for (int i = 0; i < cfg->dev_dep_count; i++) {
        free(cfg->dev_dependencies[i].name);
        free(cfg->dev_dependencies[i].version_str);
        free(cfg->dev_dependencies[i].source);
        free(cfg->dev_dependencies[i].req.version.prerelease);
        free(cfg->dev_dependencies[i].req.version.build);
        free(cfg->dev_dependencies[i].req.version_upper.prerelease);
        free(cfg->dev_dependencies[i].req.version_upper.build);
    }
    for (int i = 0; i < cfg->native_count; i++) {
        free(cfg->native_libs[i].lib_name);
        free(cfg->native_libs[i].win_path);
        free(cfg->native_libs[i].linux_path);
        free(cfg->native_libs[i].mac_path);
    }

    free(cfg);
}

/* ================================================================
 * LockFile 实现
 * ================================================================ */

PkgLockFile* lockfile_parse(const char* file_path) {
    FILE* fp = fopen(file_path, "rb");
    if (!fp) return NULL;

    PkgLockFile* lock = (PkgLockFile*)calloc(1, sizeof(PkgLockFile));
    if (!lock) { fclose(fp); return NULL; }

    TomlParser tp;
    toml_parser_init(&tp);

    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r'))
            buf[--blen] = '\0';

        char* line = str_trim(buf);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (line[0] == '[') {
            toml_parse_section_header(line, &tp);
            continue;
        }

        char* key = NULL;
        char* val = NULL;
        if (toml_parse_kv(line, &key, &val) != 0) continue;

        if (strcmp(tp.name, "lock") == 0) {
            if (strcmp(key, "leno_version") == 0)
                lock->leno_version = str_dup(val);
        } else if (strcmp(tp.parent, "packages") == 0 && tp.table_depth == 2) {
            /* [[packages]] 被解析为 parent="packages", name="包名" */
            int idx = lock->package_count;
            if (idx < MAX_LOCK_PACKAGES) {
                lock->packages[idx].name = str_dup(tp.name);
                if (strcmp(key, "version") == 0) {
                    lock->packages[idx].version = (SemVer){0};
                    semver_parse(val, &lock->packages[idx].version);
                }
                if (strcmp(key, "source") == 0) {
                    lock->packages[idx].source = str_dup(val);
                }
                if (strcmp(key, "commit") == 0) {
                    lock->packages[idx].commit = str_dup(val);
                }
                if (strcmp(key, "sha256") == 0) {
                    lock->packages[idx].sha256 = str_dup(val);
                }
                lock->package_count = idx + 1;
            }
        } else if (strcmp(tp.parent, "packages") != 0 && tp.table_depth == 1 &&
                   lock->package_count > 0) {
            /* 顶层包条目 */
            int last = lock->package_count - 1;
            if (strcmp(key, "version") == 0) {
                semver_parse(val, &lock->packages[last].version);
            } else if (strcmp(key, "source") == 0) {
                lock->packages[last].source = str_dup(val);
            } else if (strcmp(key, "commit") == 0) {
                lock->packages[last].commit = str_dup(val);
            } else if (strcmp(key, "sha256") == 0) {
                lock->packages[last].sha256 = str_dup(val);
            }
        }

        free(key);
        free(val);
    }

    fclose(fp);
    return lock;
}

int lockfile_write(const char* file_path, PkgLockFile* lock) {
    if (!lock || !file_path) return -1;

    FILE* fp = fopen(file_path, "w");
    if (!fp) return -1;

    fprintf(fp, "# Leno lock file - auto-generated, do not edit\n");
    fprintf(fp, "[lock]\n");
    fprintf(fp, "leno_version = \"%s\"\n\n", lock->leno_version ? lock->leno_version : "1.0.0");

    for (int i = 0; i < lock->package_count; i++) {
        fprintf(fp, "[[packages]]\n");
        fprintf(fp, "name = \"%s\"\n", lock->packages[i].name);
        fprintf(fp, "version = \"%d.%d.%d\"\n",
                lock->packages[i].version.major,
                lock->packages[i].version.minor,
                lock->packages[i].version.patch);
        if (lock->packages[i].source)
            fprintf(fp, "source = \"%s\"\n", lock->packages[i].source);
        if (lock->packages[i].commit)
            fprintf(fp, "commit = \"%s\"\n", lock->packages[i].commit);
        if (lock->packages[i].sha256)
            fprintf(fp, "sha256 = \"%s\"\n", lock->packages[i].sha256);
        fprintf(fp, "\n");
    }

    fclose(fp);
    return 0;
}

void lockfile_free(PkgLockFile* lock) {
    if (!lock) return;
    free(lock->leno_version);
    for (int i = 0; i < lock->package_count; i++) {
        free(lock->packages[i].name);
        free(lock->packages[i].source);
        free(lock->packages[i].commit);
        free(lock->packages[i].sha256);
    }
    free(lock);
}
