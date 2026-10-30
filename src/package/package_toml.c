#include "../include/leno_package.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
/* UTF-8 路径安全打开：Windows 的 fopen 按 ANSI 代码页解释路径字节，
 * 中文目录下的 UTF-8 路径会打不开，必须转宽字符走 _wfopen。
 *
 * ★ 2026-10-21 修（中文目录下 leno.toml 读不到 ⇒ [requires]/依赖/打包配置全失效）：
 *   调用方传进来的路径**编码并不统一** —— 编辑器/LSP 侧是 UTF-8，
 *   而 `argv`（main 走 ANSI 入口）是系统 ANSI 代码页（GBK）✗
 *   ⇒ 只按 CP_UTF8 转宽字符时，GBK 路径会被译成乱码宽字符，_wfopen 必然失败，
 *     而 `printf` 到 GBK 控制台看着还是"正常中文" ⇒ 症状极具欺骗性
 *     （实测：`leno_gui\应用\桌宠\leno.toml` 明明存在，package_config_parse 返回 NULL）。
 *   现在两步：先 CP_UTF8，失败再按 CP_ACP（系统 ANSI）重试 ✓ 两种来源都能开。
 */
static FILE* pack_toml_wfopen(const char* path, const char* mode, UINT cp) {
    int wn = MultiByteToWideChar(cp, 0, path, -1, NULL, 0);
    int mn = MultiByteToWideChar(cp, 0, mode, -1, NULL, 0);
    if (wn <= 0 || mn <= 0) return NULL;
    wchar_t* wpath = (wchar_t*)malloc(wn * sizeof(wchar_t));
    wchar_t* wmode = (wchar_t*)malloc(mn * sizeof(wchar_t));
    if (!wpath || !wmode) {
        free(wpath);
        free(wmode);
        return NULL;
    }
    MultiByteToWideChar(cp, 0, path, -1, wpath, wn);
    MultiByteToWideChar(cp, 0, mode, -1, wmode, mn);
    FILE* fp = _wfopen(wpath, wmode);
    free(wpath);
    free(wmode);
    return fp;
}

static FILE* pack_toml_fopen(const char* path, const char* mode) {
    FILE* fp = pack_toml_wfopen(path, mode, CP_UTF8);
    if (fp) return fp;
    /* 回退：路径来自 argv（ANSI 代码页）的那些调用点 */
    if (GetACP() != CP_UTF8) {
        fp = pack_toml_wfopen(path, mode, GetACP());
    }
    return fp;
}
#else
#define pack_toml_fopen(path, mode) fopen(path, mode)
#endif

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

    /* 1~3 段数字（`1` / `1.2` 按 1.0.0 / 1.2.0 补齐）—— 用户写 `>=0.3` 很常见，
     * 原来的 sscanf("%d.%d.%d") 对两段版本**直接失败**，而失败结果被调用方忽略 ⇒
     * 整个约束退化成 0.0.0（静默放行/静默拒绝）✗ 实测 2026-10-30。 */
    int comps[3] = { 0, 0, 0 };
    int ncomp = 0;
    const char* p = str;
    if (!isdigit((unsigned char)*p)) return -1;
    while (ncomp < 3) {
        if (!isdigit((unsigned char)*p)) return -1;
        long v = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            if (v > 1000000000L) return -1;   /* 防御溢出 */
            p++;
        }
        comps[ncomp++] = (int)v;
        if (*p != '.') break;
        p++;                                  /* 跳过 '.'，继续下一段（下一段必须仍是数字） */
    }
    /* 段后只允许：结束 / prerelease / build / 空白 */
    if (*p && *p != '-' && *p != '+' && !isspace((unsigned char)*p)) return -1;

    out->major = comps[0];
    out->minor = comps[1];
    out->patch = comps[2];

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

    /* 两端空白都要去掉（否则 `">=1.0.0 "` 这种会落进 EXACT 分支） */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", str);
    char* s = str_trim(buf);

    if (strcmp(s, "*") == 0 || s[0] == '\0') {
        out->type = VER_ANY;
        return 0;
    }

    /* ~1.2.3 → COMPAT */
    if (s[0] == '~') {
        out->type = VER_COMPAT;
        if (semver_parse(s + 1, &out->version) != 0) goto invalid;
        return 0;
    }

    /* >=1.0, <2.0 → RANGE */
    const char* comma = strstr(s, ",");
    if (comma) {
        out->type = VER_RANGE;
        char first[64];
        size_t len = (size_t)(comma - s);
        if (len >= sizeof(first)) len = sizeof(first) - 1;
        memcpy(first, s, len);
        first[len] = '\0';
        char* s1 = str_trim(first);

        if (strncmp(s1, ">=", 2) == 0) {
            if (semver_parse(s1 + 2, &out->version) != 0) goto invalid;
        } else if (s1[0] == '>') {
            /* 严格下界 `>x` 当前不支持：不区分包含性会把它静默当成 >=x ✗ ⇒ 明确报错 */
            goto invalid;
        } else {
            if (semver_parse(s1, &out->version) != 0) goto invalid;
        }

        char second[64];
        const char* s2 = comma + 1;
        while (isspace((unsigned char)*s2)) s2++;
        snprintf(second, sizeof(second), "%s", s2);
        char* s2t = str_trim(second);
        if (strncmp(s2t, "<=", 2) == 0) {
            out->version_upper_inclusive = 1;
            if (semver_parse(s2t + 2, &out->version_upper) != 0) goto invalid;
        } else if (s2t[0] == '<') {
            if (semver_parse(s2t + 1, &out->version_upper) != 0) goto invalid;
        } else {
            /* 范围必须有显式上界比较符，否则 `>=1.0.0, 2.0.0` 这种语义不明 */
            goto invalid;
        }
        return 0;
    }

    /* >=1.2.3 */
    if (strncmp(s, ">=", 2) == 0) {
        out->type = VER_GTE;
        if (semver_parse(s + 2, &out->version) != 0) goto invalid;
        return 0;
    }

    /* =1.2.3 或纯粹 1.2.3 → EXACT */
    const char* ver_str = s;
    if (ver_str[0] == '=') ver_str++;
    /* 其余前缀（`^` / `>` / `<=` 等）**当前不支持** —— 必须明确报错，
     * 不能静默退化成 0.0.0（那会让约束看似生效、实则恒放行/恒拒绝）✗ */
    if (ver_str[0] != '\0' && !isdigit((unsigned char)ver_str[0])) goto invalid;
    out->type = VER_EXACT;
    if (semver_parse(ver_str, &out->version) != 0) goto invalid;
    return 0;

invalid:
    /* 解析失败：标记成 VER_INVALID ⇒ 真正需要它时（[requires] 校验）会给出
     * "无法解析版本要求" 的明确报错，而不是悄悄用 0.0.0 去比较 */
    out->type = VER_INVALID;
    return -1;
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
            /* ~1.2.3 → >=1.2.3 <1.3.0（**总是抬升 minor**）✓
             *   ✗ 原来写成 "major==0 时抬 patch" —— 于是 `~0.2.0` 的上界成了 0.2.1，
             *     0.2.6 直接被判不满足（兼容区间算反了）✗ 实测 2026-10-30。 */
            if (semver_cmp(version, &req->version) < 0) return 0;
            SemVer upper = req->version;
            upper.minor++;
            upper.patch = 0;
            return semver_cmp(version, &upper) < 0;
        }

        case VER_RANGE: {
            int lo_ok = semver_cmp(version, &req->version) >= 0;
            int hi_cmp = semver_cmp(version, &req->version_upper);
            /* `<=` 含端点、`<` 不含 —— 此前一律按 `<` 比较 ⇒ `<=x` 静默错 ✗ */
            int hi_ok = req->version_upper_inclusive ? (hi_cmp <= 0) : (hi_cmp < 0);
            return lo_ok && hi_ok;
        }

        case VER_INVALID:
            /* 约束本身写坏：调用方应先报"无法解析版本要求"，这里只保证不静默放行 */
            return 0;
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

/* 解析字符串数组字面量：`["a", 'b', c]` → 逐项去掉引号后写回 out[]。
 * 只认单行、逗号分隔的形式（解析器本身逐行读取，不支持跨行数组）。 */
static void toml_parse_string_array(const char* val, char** out, int max, int* out_count) {
    *out_count = 0;
    if (!val) return;
    const char* p = val;
    while (isspace((unsigned char)*p)) p++;
    if (*p != '[') return;   /* 不是数组字面量：忽略 */
    p++;

    while (*p && *out_count < max) {
        while (isspace((unsigned char)*p) || *p == ',') p++;
        if (*p == ']' || *p == '\0') break;

        char quote = 0;
        if (*p == '"' || *p == '\'') {
            quote = *p;
            p++;
        }
        const char* start = p;
        if (quote) {
            while (*p && *p != quote) p++;
        } else {
            while (*p && *p != ',' && *p != ']') p++;   /* 裸值：到逗号/右括号为止 */
        }
        size_t len = (size_t)(p - start);
        while (len > 0 && isspace((unsigned char)start[len - 1])) len--;   /* 裸值去尾空白 */

        if (len > 0) {
            char* item = (char*)malloc(len + 1);
            if (!item) break;
            memcpy(item, start, len);
            item[len] = '\0';
            out[(*out_count)++] = item;
        }
        if (*p == quote && quote != 0) p++;   /* 跳过收尾引号 */
    }
}

/* ================================================================
 * 公开 API：解析 leno.toml
 * ================================================================ */

PackageConfig* package_config_parse(const char* file_path) {
    // ★ 2026-10-21 修：这里原来是**裸 fopen** ⇒ 中文目录下的 leno.toml 读不到
    //   （`leno_gui/应用/桌宠/leno.toml` 明明存在，函数返回 NULL ⇒ [requires]/[dependencies]
    //    /[modules] 全部静默失效）。resource.toml 那边早已用 pack_toml_fopen，
    //   只有 leno.toml 这条漏了 ⇒ 统一走 UTF-8 安全的那个 ✓
    FILE* fp = pack_toml_fopen(file_path, "rb");
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
        } else if (strcmp(tp.name, "requires") == 0 && tp.table_depth == 1) {
            /* [requires] → 运行时模块包版本要求（不参与安装，只做 import 期校验） */
            if (cfg->require_count < MAX_DEPENDENCIES) {
                int idx = cfg->require_count++;
                cfg->requires[idx].name = str_dup(key);
                cfg->requires[idx].version_str = str_dup(val);
                version_req_parse(val, &cfg->requires[idx].req);
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
                } else if (strcmp(key, "optional") == 0) {
                    /* 2026-10-21：optional = true ⇒ 默认不随打包，应用点名才带 */
                    cfg->native_libs[found].optional =
                        (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
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
    for (int i = 0; i < cfg->require_count; i++) {
        free(cfg->requires[i].name);
        free(cfg->requires[i].version_str);
        free(cfg->requires[i].req.version.prerelease);
        free(cfg->requires[i].req.version.build);
        free(cfg->requires[i].req.version_upper.prerelease);
        free(cfg->requires[i].req.version_upper.build);
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
 * [requires]：运行时模块版本要求
 * ----------------------------------------------------------------
 * 与 [dependencies] 不同：这些包由运行时提供（leno_module/ 或全局缓存），
 * 本段只做 import 期的版本校验，不参与 `leno --install` ✓
 * ================================================================ */
#define REQ_PATH_MAX 4096

#ifdef _WIN32
#define REQ_SEP '\\'
#else
#define REQ_SEP '/'
#endif

static struct {
    char* name;
    char* version_str;
    VersionReq req;
} g_module_reqs[MAX_DEPENDENCIES];
static int g_module_req_count = 0;

/* 同一次编译内已校验过的 "包名@版本" ⇒ 同一包的多个 import 只报一次 */
static char g_req_checked[MAX_DEPENDENCIES][256];
static int g_req_checked_count = 0;

void package_requirements_clear(void) {
    for (int i = 0; i < g_module_req_count; i++) {
        free(g_module_reqs[i].name);
        free(g_module_reqs[i].version_str);
        free(g_module_reqs[i].req.version.prerelease);
        free(g_module_reqs[i].req.version.build);
        free(g_module_reqs[i].req.version_upper.prerelease);
        free(g_module_reqs[i].req.version_upper.build);
    }
    g_module_req_count = 0;
    g_req_checked_count = 0;
}

void package_requirements_set(PackageConfig* cfg) {
    package_requirements_clear();
    if (!cfg) return;
    for (int i = 0; i < cfg->require_count && g_module_req_count < MAX_DEPENDENCIES; i++) {
        const char* n = cfg->requires[i].name;
        if (!n) continue;
        int idx = g_module_req_count++;
        g_module_reqs[idx].name = str_dup(n);
        g_module_reqs[idx].version_str = str_dup(cfg->requires[i].version_str);
        /* 重新解析 ⇒ req 内部的 prerelease/build 归本表自己所有（与 cfg 解耦 ✓） */
        version_req_parse(cfg->requires[i].version_str, &g_module_reqs[idx].req);
    }
}

static int req_file_exists(const char* path) {
    FILE* fp = pack_toml_fopen(path, "rb");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

/* 把 path 就地改成父目录；返回 0 = 已经没有父目录 */
static int req_parent_dir(char* path) {
    size_t n = strlen(path);
    while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '\\')) path[--n] = '\0';
    if (n == 0) return 0;
    char* s1 = strrchr(path, '/');
    char* s2 = strrchr(path, '\\');
    char* sep = s1;
    if (!sep || (s2 && s2 > sep)) sep = s2;
    if (!sep) return 0;
    if (sep == path) {              /* "/foo" → "/" */
        sep[1] = '\0';
        return 1;
    }
    if (sep == path + 2 && path[1] == ':') {   /* "C:\foo" → "C:\" */
        sep[1] = '\0';
        return 1;
    }
    *sep = '\0';
    return 1;
}

/* 从模块文件向上找最近的 leno.toml（内置/缓存包的包根） */
static int req_find_package_toml(const char* resolved_path, char* out_toml, int out_len) {
    if (!resolved_path || !out_toml || out_len <= 0) return 0;
    char dir[REQ_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", resolved_path);
    if (!req_parent_dir(dir)) return 0;     /* 先到文件所在目录 */

    for (int depth = 0; depth < 5; depth++) {
        char cand[REQ_PATH_MAX + 16];
        snprintf(cand, sizeof(cand), "%s%cleno.toml", dir, REQ_SEP);
        if (req_file_exists(cand)) {
            snprintf(out_toml, (size_t)out_len, "%s", cand);
            return 1;
        }
        if (!req_parent_dir(dir)) break;
    }
    return 0;
}

int package_check_module_version(const char* resolved_path, char* out_msg, int out_len) {
    if (!out_msg || out_len <= 0) return 1;
    out_msg[0] = '\0';
    if (!resolved_path || g_module_req_count == 0) return 1;

    char toml[REQ_PATH_MAX + 16];
    if (!req_find_package_toml(resolved_path, toml, (int)sizeof(toml))) return 1;

    PackageConfig* pkg = package_config_parse(toml);
    if (!pkg) return 1;

    int ri = -1;
    for (int i = 0; i < g_module_req_count; i++) {
        if (pkg->name && g_module_reqs[i].name &&
            strcmp(pkg->name, g_module_reqs[i].name) == 0) {
            ri = i;
            break;
        }
    }
    if (ri < 0) {                       /* 没有对该包的版本要求 */
        package_config_free(pkg);
        return 1;
    }

    /* 同一包版本只校验一次：错误已经报过就不再重复刷屏 */
    char key[256];
    snprintf(key, sizeof(key), "%s@%s", pkg->name, pkg->version ? pkg->version : "?");
    for (int i = 0; i < g_req_checked_count; i++) {
        if (strcmp(g_req_checked[i], key) == 0) {
            package_config_free(pkg);
            return 1;
        }
    }
    if (g_req_checked_count < MAX_DEPENDENCIES) {
        snprintf(g_req_checked[g_req_checked_count],
                 sizeof(g_req_checked[0]), "%s", key);
        g_req_checked_count++;
    }

    const char* req_str = g_module_reqs[ri].version_str ? g_module_reqs[ri].version_str : "*";
    if (!pkg->version) {
        snprintf(out_msg, (size_t)out_len,
                 "模块包 '%s' 未声明可用的 version（缺 leno.toml 的 [package].version）"
                 "—— 无法满足项目要求 '%s'\n"
                 "  提示: 升级该模块包，或调整 leno.toml 的 [requires]",
                 pkg->name, req_str);
        package_config_free(pkg);
        return 0;
    }

    SemVer ver;
    if (semver_parse(pkg->version, &ver) != 0) {
        snprintf(out_msg, (size_t)out_len,
                 "模块包 '%s' 的版本 '%s' 无法解析 —— 无法满足项目要求 '%s'\n"
                 "  提示: 检查该模块包 leno.toml 的 [package].version",
                 pkg->name, pkg->version, req_str);
        package_config_free(pkg);
        return 0;
    }

    int ok = version_satisfies(&ver, &g_module_reqs[ri].req);
    if (g_module_reqs[ri].req.type == VER_INVALID) {
        /* 约束本身写错了：明确报出来，而不是拿 0.0.0 去比较（那会静默放行/静默拒绝） */
        snprintf(out_msg, (size_t)out_len,
                 "无法解析版本要求 '%s'（leno.toml 的 [requires] %s）\n"
                 "  支持: \"1.2.3\" / \"~1.2.3\" / \">=1.2.3\" / \">=1.2.3, <2.0.0\" / \"*\""
                 "（版本可只写 1 段或 2 段，如 \"~1.2\" / \">=0.3\"）",
                 req_str, pkg->name);
        free(ver.prerelease);
        free(ver.build);
        package_config_free(pkg);
        return 0;
    }
    free(ver.prerelease);
    free(ver.build);

    if (!ok) {
        snprintf(out_msg, (size_t)out_len,
                 "模块包 '%s' 版本 %s 不满足项目要求 '%s'\n"
                 "  提示: 升级 LenoLang 运行时/模块包，或调整 leno.toml 的 [requires]",
                 pkg->name, pkg->version, req_str);
        package_config_free(pkg);
        return 0;
    }

    package_config_free(pkg);
    return 1;
}

/* ================================================================
 * 打包资源配置：解析 resource.toml（与 leno.toml 同在包根，可选）
 * ----------------------------------------------------------------
 * 目前识别 [pack] 段：
 *   onefile   = true
 *   resources = 通配模式数组（images 目录递归全部、fonts 目录下的 ttf 等）
 *   icon      = "app.ico"  单文件/目录模式打包产物要换成的图标（相对包根）
 * ================================================================ */

PackConfig* package_pack_config_parse(const char* file_path) {
    FILE* fp = pack_toml_fopen(file_path, "rb");
    if (!fp) return NULL;   /* 文件不存在：可选配置，不算错误 */

    PackConfig* cfg = (PackConfig*)calloc(1, sizeof(PackConfig));
    if (!cfg) {
        fclose(fp);
        return NULL;
    }

    cfg->file_path = str_dup(file_path);

    TomlParser tp;
    toml_parser_init(&tp);

    char buf[MAX_LINE];
    while (fgets(buf, sizeof(buf), fp)) {
        size_t blen = strlen(buf);
        while (blen > 0 && (buf[blen - 1] == '\n' || buf[blen - 1] == '\r')) {
            buf[--blen] = '\0';
        }

        char* line = str_trim(buf);
        if (line[0] == '\0' || line[0] == '#') continue;

        if (line[0] == '[' && line[1] != '[') {
            toml_parse_section_header(line, &tp);
            continue;
        }

        char* key = NULL;
        char* val = NULL;
        if (toml_parse_kv(line, &key, &val) != 0) continue;

        if (strcmp(tp.name, "pack") == 0 && tp.table_depth == 1) {
            if (strcmp(key, "onefile") == 0) {
                cfg->onefile = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
            } else if (strcmp(key, "resources") == 0) {
                toml_parse_string_array(val, cfg->resources, MAX_PACK_RESOURCES,
                                        &cfg->resource_count);
            } else if (strcmp(key, "native") == 0) {
                /* native = ["libwebp", ...]：启用模块声明的 optional 原生库（2026-10-21） */
                toml_parse_string_array(val, cfg->native, MAX_PACK_RESOURCES,
                                        &cfg->native_count);
            } else if (strcmp(key, "icon") == 0) {
                /* 单个字符串（toml_parse_kv 已去引号）；重复出现时后者覆盖前者 */
                free(cfg->icon);
                cfg->icon = (val[0] != '\0') ? str_dup(val) : NULL;
            }
        }
        /* 未知段/未知键：静默忽略（其它打包相关配置以后也放这里） */

        free(key);
        free(val);
    }

    fclose(fp);
    return cfg;
}

void package_pack_config_free(PackConfig* cfg) {
    if (!cfg) return;

    for (int i = 0; i < cfg->resource_count; i++) {
        free(cfg->resources[i]);
    }
    for (int i = 0; i < cfg->native_count; i++) {
        free(cfg->native[i]);
    }
    free(cfg->icon);
    free(cfg->file_path);
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
    fprintf(fp, "leno_version = \"%s\"\n\n", lock->leno_version ? lock->leno_version : "0.1.0");

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
