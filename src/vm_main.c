#include "include/leno_vm_runtime.h"
#include "include/leno_serialize.h"
#include "include/native.h"
#include "include/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>          // GC 用 time()（Windows 下也要，不能只在 POSIX 分支里包）

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#endif

// VM 独立运行时 - 不依赖编译器
// 启动时自动检测 exe 尾部是否嵌入了 lenb 数据：
//   - 有嵌入数据：直接执行嵌入的字节码（打包后的独立 exe 模式）
//   - 无嵌入数据：作为命令行工具，需要传入 .lenb 文件路径
//
// 尾部数据格式: [exe 原始数据] [lenb 数据] [4字节 lenb_size] [4字节 LENB_MAGIC]

extern VM vm;

// 全局变量（VM 运行时需要，原定义在 main.c）
int g_argc = 0;
char** g_argv = NULL;

// lenb 文件魔数
#define LENB_MAGIC 0x424E454C
// 内嵌资源段魔数（"LENR"，小端）
#define RES_MAGIC  0x524E454C

// ============================================================================
// 内嵌资源段（单文件打包 -p --onefile）
// ----------------------------------------------------------------------------
// 尾部布局: [vm][资源段][资源段大小:u32][RES_MAGIC][lenb][lenb大小:u32][LENB_MAGIC]
// 资源段放在 lenb **之前** ⇒ 末尾 8 字节仍是 LENB_MAGIC，旧 VM 读新 exe 依然正常。
// 资源段内部格式（v2；索引在前、内容在后）：
//   "LENOPACK"(8) | version:u32(=2) | payload_hash:u64 | entry_count:u32
//   entry_count × [ rel_len:u16 | size:u64 | hash:u32 | rel_path(rel_len) ]
//   各文件内容按索引顺序紧随其后
//   · payload_hash = FNV-1a 64 over [entry_count .. 末尾]（**打包时算好**，见 main.c）
//     —— 它是**释放目录的键**：不同构建 ⇒ 不同目录（互不覆盖），同构建 ⇒ 同目录（可复用）。
//   · version 只做**格式自检**：exe 与它内嵌的资源段永远出自同一次打包，
//     所以不需要跨版本兼容；版本号对不上就报格式错误（宁可报错，也不要误解析）。
// 释放目标 = **跨平台缓存目录**（2026-09-24 改；此前是 exe 所在目录）：
//   ① 不再把几十个资源文件 / DLL 倒进桌面、下载目录这类"用户看得见"的地方；
//   ② 按 payload_hash 分目录 ⇒ 两个不同构建（哪怕同时运行）互不覆盖；
//   ③ 同构建二次启动走**哨兵快路径** ⇒ 零写入（比原先"逐文件读一遍算哈希"更快）。
//   ⚠ 正因为按 hash 分目录，**这个目录绝不能用来存用户数据** ——
//     每次重新打包 hash 就变、目录就变，写进去的数据会"凭空消失"（且无任何提示）。
//     所以它只服务"读随包资源"（`dirs.res_dir()`）；写数据仍走 `dirs.script_dir()`（exe 目录）。
// 原子性：先释放到 `<hash>.tmp.<pid>`，全部写完（含哨兵）后**原子改名**成 `<hash>`
//   ⇒ 最终目录"要么不存在、要么完整"，不会被并发启动的另一个实例读到半截。
// 清理：启动时惰性 GC —— 同层里非当前 hash 的目录超过 RES_GC_MAX_AGE_SEC 就删，
//   崩溃残留的 `*.tmp.*` 超过 RES_GC_TMP_AGE_SEC 就删。**退出时绝不删**
//   （并发实例 + Windows 下 DLL 还被锁着）。
// 目录的对外出口 = `vm_set_res_dir()`（定义在 core 的 vm.c，见 leno_vm_runtime.h）。
// ============================================================================
#define RES_BLOB_HEADER  "LENOPACK"
#define RES_BLOB_VERSION 2u
// 哨兵：存在且内容 == 本构建的 hash ⇒ 该目录完整可用（快路径只读这一个文件）
#define RES_SENTINEL_NAME ".leno_pack_ok"
// GC 阈值（秒）
#define RES_GC_MAX_AGE_SEC (7 * 24 * 60 * 60)
#define RES_GC_TMP_AGE_SEC (24 * 60 * 60)

// 本机路径分隔符（拼释放目录用；内嵌资源段里的 rel_path 一律用 '/'）
#ifdef _WIN32
  #define RES_SEP '\\'
#else
  #define RES_SEP '/'
#endif

// 下面解包要用（定义在后面，保持"读文件"工具与原顺序）
static unsigned char* read_file_binary(const char* path, size_t* out_size);

static uint32_t res_fnv1a32(const unsigned char* data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

// 注：释放目录的键（FNV-1a 64）**运行期不重算** —— 打包侧 main.c 的 pack_fnv1a64
// 已经把它写进 v2 头（payload_hash），这里直接读，省掉启动时对整段资源的哈希开销。

// 取文件内容的 FNV-1a（不命中返回 0），用于判断磁盘上的文件是否已是同一版本
static uint32_t res_hash_existing(const char* path) {
    size_t size = 0;
    unsigned char* data = read_file_binary(path, &size);
    if (!data) return 0;
    uint32_t h = res_fnv1a32(data, size);
    free(data);
    return h ? h : 1;   // 0 保留给"读不到"
}

// 递归创建目录（UTF-8 路径安全）
static void res_mkpath(const char* dir) {
    if (!dir || !dir[0]) return;
    char tmp[MAX_PATH_LEN];
    strncpy(tmp, dir, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    while (len > 1 && (tmp[len - 1] == '\\' || tmp[len - 1] == '/')) tmp[--len] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char save = *p;
            *p = '\0';
#ifdef _WIN32
            wchar_t* w = utf8_to_utf16(tmp);
            if (w) { _wmkdir(w); free(w); }
#else
            mkdir(tmp, 0755);
#endif
            *p = save;
        }
    }
#ifdef _WIN32
    { wchar_t* w = utf8_to_utf16(tmp); if (w) { _wmkdir(w); free(w); } }
#else
    mkdir(tmp, 0755);
#endif
}

static int res_write_file(const char* path, const unsigned char* data, size_t len) {
#ifdef _WIN32
    wchar_t* wp = utf8_to_utf16(path);
    if (!wp) return -1;
    FILE* f = _wfopen(wp, L"wb");
    free(wp);
#else
    FILE* f = fopen(path, "wb");
#endif
    if (!f) return -1;
    int ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

// ----------------------------------------------------------------------------
// 跨平台目录工具（释放目录的原子改名 / 递归删除 / 缓存根定位）
// ----------------------------------------------------------------------------

#ifdef _WIN32
// 宽字符版递归删除（Windows 专用，避免每个子项都做一次 UTF-16↔UTF-8 转换）
static int res_rmtree_w(const wchar_t* wpath) {
    DWORD attr = GetFileAttributesW(wpath);
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;              // 不存在 ⇒ 视为成功
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(wpath) ? 0 : -1;

    wchar_t wpat[4096];
    _snwprintf(wpat, 4096, L"%s\\*", wpath);
    wpat[4095] = L'\0';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t child[4096];
            _snwprintf(child, 4096, L"%s\\%s", wpath, fd.cFileName);
            child[4095] = L'\0';
            res_rmtree_w(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return RemoveDirectoryW(wpath) ? 0 : -1;
}
#endif

// 递归删除目录树（GC / 清理暂存用）。路径不存在视为成功。
static int res_rmtree(const char* path) {
#ifdef _WIN32
    wchar_t* wpath = utf8_to_utf16(path);
    if (!wpath) return -1;
    int rc = res_rmtree_w(wpath);
    free(wpath);
    return rc;
#else
    DIR* d = opendir(path);
    if (!d) return 0;                                           // 不存在 / 不是目录
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[MAX_PATH_LEN * 2];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        struct stat st;
        if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) res_rmtree(child);
        else unlink(child);
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -1;
#endif
}

// 目录的原子改名：成功 ⇒ dst 完整出现；失败（多半是 dst 已存在）⇒ 返回 -1。
//   POSIX: rename() 对"目标是已存在的**非空**目录"报 ENOTEMPTY ⇒ 天然安全；
//   Windows: MoveFileExW 目标已存在即失败（不带 REPLACE_EXISTING）⇒ 同样安全。
static int res_rename_dir(const char* src, const char* dst) {
#ifdef _WIN32
    wchar_t* ws = utf8_to_utf16(src);
    wchar_t* wd = utf8_to_utf16(dst);
    if (!ws || !wd) { free(ws); free(wd); return -1; }
    int ok = MoveFileExW(ws, wd, 0) ? 0 : -1;
    free(ws); free(wd);
    return ok;
#else
    return rename(src, dst) == 0 ? 0 : -1;
#endif
}

// 每用户缓存根目录（UTF-8，含结尾分隔符）。按平台优先级取，全失败则退临时目录。
//   Windows: %LOCALAPPDATA%  → %TEMP%
//   macOS:   $HOME/Library/Caches
//   Linux:   $XDG_CACHE_HOME 或 $HOME/.cache
static int res_cache_root(char* out, size_t n) {
    out[0] = '\0';
#ifdef _WIN32
    wchar_t wbuf[4096];
    DWORD got = GetEnvironmentVariableW(L"LOCALAPPDATA", wbuf, 4096);
    if (got > 0 && got < 4096) {
        char* u = utf16_to_utf8(wbuf);
        if (u && u[0]) {
            snprintf(out, n, "%s", u);
            free(u);
            size_t l = strlen(out);
            if (l + 1 < n && out[l - 1] != '\\' && out[l - 1] != '/') { out[l] = '\\'; out[l + 1] = '\0'; }
            return 0;
        }
        free(u);
    }
    DWORD tl = GetTempPathW(4096, wbuf);
    if (tl > 0 && tl < 4096) {
        char* u = utf16_to_utf8(wbuf);
        if (u && u[0]) {
            snprintf(out, n, "%s", u);
            free(u);
            size_t l = strlen(out);
            if (l + 1 < n && out[l - 1] != '\\' && out[l - 1] != '/') { out[l] = '\\'; out[l + 1] = '\0'; }
            return 0;
        }
        free(u);
    }
    return -1;
#else
    const char* home = getenv("HOME");
#if defined(__APPLE__)
    if (home && home[0]) { snprintf(out, n, "%s/Library/Caches/", home); return 0; }
#else
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0]) {
        snprintf(out, n, "%s", xdg);
        size_t l = strlen(out);
        if (l + 1 < n && out[l - 1] != '/') { out[l] = '/'; out[l + 1] = '\0'; }
        return 0;
    }
    if (home && home[0]) { snprintf(out, n, "%s/.cache/", home); return 0; }
#endif
    const char* tmp = getenv("TMPDIR");
    if (tmp && tmp[0]) {
        snprintf(out, n, "%s", tmp);
        size_t l = strlen(out);
        if (l + 1 < n && out[l - 1] != '/') { out[l] = '/'; out[l + 1] = '\0'; }
        return 0;
    }
    snprintf(out, n, "/tmp/");
    return 0;
#endif
}

// 取路径的纯文件名（无目录、无扩展名），用作缓存目录里的一级（应用）目录名。
// 例如 "C:\Users\x\Desktop\pvz.exe" → "pvz"。
static void res_exe_stem(const char* exe_path, char* out, size_t n) {
    const char* base = exe_path;
    for (const char* p = exe_path; *p; p++) {
        if (*p == '\\' || *p == '/') base = p + 1;
    }
    size_t l = 0;
    while (base[l] && base[l] != '.' && l + 1 < n) { out[l] = base[l]; l++; }
    out[l] = '\0';
    if (l == 0) snprintf(out, n, "app");   // 兜底（理论上到不了）
}

// 组装本次释放目录：
//   pack_dir = <cache_root><exeStem>/pack/            （GC 扫描这一层）
//   final    = <cache_root><exeStem>/pack/<hash16>/   （含结尾分隔符）
static int res_build_dirs(const char* exe_path, uint64_t hash,
                          char* pack_dir, size_t np, char* final_dir, size_t nf) {
    char root[MAX_PATH_LEN];
    if (res_cache_root(root, sizeof(root)) != 0) return -1;
    char stem[256];
    res_exe_stem(exe_path, stem, sizeof(stem));
    snprintf(pack_dir, np, "%s%s%cpack%c", root, stem, RES_SEP, RES_SEP);
    snprintf(final_dir, nf, "%s%016llx%c", pack_dir, (unsigned long long)hash, RES_SEP);
    return 0;
}

// 哨兵校验：<dir>/.leno_pack_ok 存在且内容 == hash 的 16 位十六进制 ⇒ 目录完整可用。
// 这是**快路径**：命中就完全跳过释放（连逐文件哈希都不用算）。
static int res_sentinel_ok(const char* dir, uint64_t hash) {
    char path[MAX_PATH_LEN * 2];
    snprintf(path, sizeof(path), "%s%s", dir, RES_SENTINEL_NAME);
    size_t size = 0;
    unsigned char* data = read_file_binary(path, &size);
    if (!data) return 0;
    char want[32];
    snprintf(want, sizeof(want), "%016llx", (unsigned long long)hash);
    int ok = (size >= 16 && memcmp(data, want, 16) == 0);
    free(data);
    return ok;
}

static int res_write_sentinel(const char* dir, uint64_t hash) {
    char path[MAX_PATH_LEN * 2];
    snprintf(path, sizeof(path), "%s%s", dir, RES_SENTINEL_NAME);
    char body[32];
    int len = snprintf(body, sizeof(body), "%016llx", (unsigned long long)hash);
    return res_write_file(path, (const unsigned char*)body, (size_t)len);
}

// 取文件/目录的最后修改时间（秒）。取不到返回 0（⇒ 不会被 GC 删）。
static long long res_mtime_sec(const char* path) {
#ifdef _WIN32
    wchar_t* w = utf8_to_utf16(path);
    if (!w) return 0;
    WIN32_FILE_ATTRIBUTE_DATA fad;
    long long t = 0;
    if (GetFileAttributesExW(w, GetFileExInfoStandard, &fad)) {
        ULARGE_INTEGER u;
        u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
        u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        // FILETIME(1601-01-01, 100ns) → Unix 秒
        t = (long long)(u.QuadPart / 10000000ULL) - 11644473600LL;
    }
    free(w);
    return t;
#else
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return (long long)st.st_mtime;
#endif
}

// 惰性 GC：扫 pack_dir 一层，删掉
//   · 非当前 hash 的目录（超 RES_GC_MAX_AGE_SEC）
//   · 崩溃残留的暂存目录 `*.tmp.*`（超 RES_GC_TMP_AGE_SEC）
// 绝不动 keep_name（本次在用的那个）。失败一律忽略 —— GC 不该影响启动。
static void res_gc(const char* pack_dir, const char* keep_name) {
#ifdef _WIN32
    long long now = (long long)time(NULL);
    char pat[MAX_PATH_LEN * 2];
    snprintf(pat, sizeof(pat), "%s*", pack_dir);
    wchar_t* wpat = utf8_to_utf16(pat);
    if (!wpat) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(wpat, &fd);
    if (h == INVALID_HANDLE_VALUE) { free(wpat); return; }
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        char* name = utf16_to_utf8(fd.cFileName);
        if (!name) continue;
        if (strcmp(name, keep_name) != 0) {
            char full[MAX_PATH_LEN * 2];
            snprintf(full, sizeof(full), "%s%s", pack_dir, name);
            int is_tmp = (strstr(name, ".tmp.") != NULL);
            long long mt = res_mtime_sec(full);
            if (mt > 0 && now - mt > (is_tmp ? RES_GC_TMP_AGE_SEC : RES_GC_MAX_AGE_SEC)) {
                res_rmtree(full);
            }
        }
        free(name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(wpat);
#else
    long long now = (long long)time(NULL);
    DIR* d = opendir(pack_dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (strcmp(e->d_name, keep_name) == 0) continue;
        char full[MAX_PATH_LEN * 2];
        snprintf(full, sizeof(full), "%s%s", pack_dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        int is_tmp = (strstr(e->d_name, ".tmp.") != NULL);
        long long mt = (long long)st.st_mtime;
        if (mt > 0 && now - mt > (is_tmp ? RES_GC_TMP_AGE_SEC : RES_GC_MAX_AGE_SEC)) {
            res_rmtree(full);
        }
    }
    closedir(d);
#endif
}

// 本次进程的 pid（拼暂存目录名用）
static unsigned long res_getpid(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

// 解包内嵌资源段；返回写入的文件数（-1 表示格式错误）。
// ----------------------------------------------------------------------------
// 目标目录 = 跨平台缓存目录里的 `<cache_root><exeStem>/pack/<hash16>/`（见文件开头说明）。
// 流程（**先建暂存、后原子改名**，保证最终目录要么不存在、要么完整）：
//   ① 解析头，取 version / payload_hash；
//   ② 目标目录已有**匹配的哨兵** ⇒ 快路径直接返回（零写入）；
//   ③ 否则释放到 `<hash16>.tmp.<pid>/`，写完所有文件 + 哨兵；
//   ④ 原子改名成 `<hash16>/`；改名失败（别的实例先到）⇒ 校验对方哨兵，可用就用它；
//   ⑤ 惰性 GC 同层旧目录。
// 兜底：缓存目录不可用/释放失败 ⇒ 退回"释放到 exe 目录"（旧行为），至少还能跑。
// 成功时把最终目录交给 `vm_set_res_dir()`（定义在 core 的 vm.c，供 dirs.res_dir() 读）。
// ----------------------------------------------------------------------------
static int extract_embedded_resources(const char* exe_path,
                                      const unsigned char* blob, size_t blob_len) {
    // 头: "LENOPACK"(8) | version:u32 | payload_hash:u64 | count:u32
    const size_t HDR = 24;
    if (blob_len < HDR || memcmp(blob, RES_BLOB_HEADER, 8) != 0) return -1;

    uint32_t version = 0;
    memcpy(&version, blob + 8, 4);
    if (version != RES_BLOB_VERSION) {
        // exe 与它内嵌的资源段永远出自同一次打包 ⇒ 版本对不上说明文件被改坏了。
        // 宁可响亮报错，也不要按错误布局去解析（会写出乱七八糟的文件）。
        fprintf(stderr, "[pack] 内嵌资源段版本不匹配（exe=%u, blob=%u）—— exe 可能被修改过\n",
                RES_BLOB_VERSION, version);
        return -1;
    }
    uint64_t payload_hash = 0;
    memcpy(&payload_hash, blob + 12, 8);
    uint32_t count = 0;
    memcpy(&count, blob + 20, 4);
    if (count == 0 || count > 100000) return -1;

    // 目标目录（缓存）；拿不到就退 exe 目录
    char pack_dir[MAX_PATH_LEN] = {0};
    char final_dir[MAX_PATH_LEN] = {0};
    int use_cache = (res_build_dirs(exe_path, payload_hash, pack_dir, sizeof(pack_dir),
                                    final_dir, sizeof(final_dir)) == 0);

    // exe 所在目录（含结尾分隔符）—— 兜底路径用
    char exe_dir[MAX_PATH_LEN];
    strncpy(exe_dir, exe_path, sizeof(exe_dir) - 1);
    exe_dir[sizeof(exe_dir) - 1] = '\0';
    char* s1 = strrchr(exe_dir, '\\');
    char* s2 = strrchr(exe_dir, '/');
    if (s2 && (!s1 || s2 > s1)) s1 = s2;
    if (!s1) return -1;
    *(s1 + 1) = '\0';

    // ② 快路径：目标目录的哨兵匹配 ⇒ 直接可用，一个文件都不写
    if (use_cache && res_sentinel_ok(final_dir, payload_hash)) {
        vm_set_res_dir(final_dir);
        return 0;
    }

    // 本次实际写入的目标：缓存暂存目录（或兜底 = exe 目录）
    char target[MAX_PATH_LEN];      // 落盘目标（含结尾分隔符）
    char staging[MAX_PATH_LEN] = {0};
    int atomic_rename = 0;
    if (use_cache) {
        snprintf(staging, sizeof(staging), "%s%016llx.tmp.%lu%c",
                 pack_dir, (unsigned long long)payload_hash, res_getpid(), RES_SEP);
        res_rmtree(staging);        // 同 pid 的陈旧残留（极端情况：pid 复用）
        res_mkpath(staging);
        snprintf(target, sizeof(target), "%s", staging);
        atomic_rename = 1;
    } else {
        snprintf(target, sizeof(target), "%s", exe_dir);
    }

    // 第一遍：读索引，算内容区起点
    const unsigned char* idx = blob + HDR;
    const unsigned char* blob_end = blob + blob_len;
    size_t content_off = HDR;
    for (uint32_t i = 0; i < count; i++) {
        if (idx + 2 + 8 + 4 > blob_end) return -1;
        uint16_t rlen = 0;
        memcpy(&rlen, idx, 2);
        content_off += 2 + 8 + 4 + rlen;
        idx += 2 + 8 + 4 + rlen;
    }
    if (content_off > blob_len) return -1;

    // 第二遍：逐个落盘（兜底路径仍按逐文件哈希跳过；暂存目录是全新的，必然都写）
    idx = blob + HDR;
    const unsigned char* content = blob + content_off;
    const unsigned char* content_end = blob + blob_len;
    int written = 0, skipped = 0, failed = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint16_t rlen = 0;
        memcpy(&rlen, idx, 2);
        uint64_t fsize = 0;
        memcpy(&fsize, idx + 2, 8);
        uint32_t want_hash = 0;
        memcpy(&want_hash, idx + 10, 4);
        const char* rel = (const char*)(idx + 14);

        if (rlen == 0 || rel[0] == '\0' || fsize > (uint64_t)(content_end - content) ||
            memchr(rel, '\0', rlen) != NULL) {
            if (atomic_rename) res_rmtree(staging);   // 索引损坏：别留半个暂存目录
            return -1;
        }
        char rel_buf[MAX_PATH_LEN];
        size_t copy_len = (rlen < sizeof(rel_buf) - 1) ? rlen : sizeof(rel_buf) - 1;
        memcpy(rel_buf, rel, copy_len);
        rel_buf[copy_len] = '\0';

        char dst[MAX_PATH_LEN * 2];
        snprintf(dst, sizeof(dst), "%s%s", target, rel_buf);
        // 统一分隔符（内嵌路径一律用 '/'，Windows 下换成盘符风格更好读）
#ifdef _WIN32
        for (char* p = dst + strlen(target); *p; p++) {
            if (*p == '/') *p = '\\';
        }
#endif

        if (!atomic_rename && res_hash_existing(dst) == want_hash) {
            skipped++;
        } else {
            // 建父目录
            char parent[MAX_PATH_LEN * 2];
            strncpy(parent, dst, sizeof(parent) - 1);
            parent[sizeof(parent) - 1] = '\0';
            char* q1 = strrchr(parent, '\\');
            char* q2 = strrchr(parent, '/');
            if (q2 && (!q1 || q2 > q1)) q1 = q2;
            if (q1) { *q1 = '\0'; res_mkpath(parent); }

            if (res_write_file(dst, content, (size_t)fsize) == 0) written++;
            else {
                failed++;
                fprintf(stderr, "[pack] 解包失败: %s\n", dst);
            }
        }

        content += (size_t)fsize;
        idx += 2 + 8 + 4 + rlen;
    }

    // ③ 哨兵 + ④ 原子改名
    if (atomic_rename) {
        if (failed > 0 || res_write_sentinel(staging, payload_hash) != 0) {
            // 有文件没写成功 ⇒ 整个暂存作废（宁可下次重来，也不要留个"看起来完整"的目录）
            res_rmtree(staging);
            fprintf(stderr, "[pack] 解包内嵌资源失败（%d 个文件未写入）\n", failed);
            return -1;
        }
        if (res_rename_dir(staging, final_dir) == 0) {
            vm_set_res_dir(final_dir);
        } else if (res_sentinel_ok(final_dir, payload_hash)) {
            // 另一个实例先到、且它的目录是完整的 ⇒ 用它，丢掉自己的暂存
            res_rmtree(staging);
            vm_set_res_dir(final_dir);
        } else {
            // 目标存在但不可用（多半是上次崩溃留下的半截目录）：删掉重来一次
            res_rmtree(final_dir);
            if (res_rename_dir(staging, final_dir) == 0) {
                vm_set_res_dir(final_dir);
            } else {
                res_rmtree(staging);
                fprintf(stderr, "[pack] 无法落盘资源目录: %s\n", final_dir);
                return -1;
            }
        }
        // ⑤ 惰性 GC：同层里非当前 hash 的旧目录（含崩溃残留的暂存）
        char keep[64];
        snprintf(keep, sizeof(keep), "%016llx", (unsigned long long)payload_hash);
        res_gc(pack_dir, keep);
    } else {
        // 兜底路径（exe 目录）：语义与旧版一致，用 vm_set_res_dir 告知调用方
        if (failed > 0) {
            fprintf(stderr, "[pack] 解包内嵌资源有 %d 个文件失败\n", failed);
            return -1;
        }
        vm_set_res_dir(exe_dir);
    }

    if (written > 0 || skipped > 0) {
        fprintf(stderr, "[pack] 解包内嵌资源: 新写入 %d 个，已存在 %d 个%s\n",
                written, skipped, failed ? "（有失败）" : "");
    }
    return written;
}

// 从文件读取全部内容到缓冲区
static unsigned char* read_file_binary(const char* path, size_t* out_size) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (wlen <= 0) return NULL;
    wchar_t* wpath = (wchar_t*)malloc(wlen * sizeof(wchar_t));
    if (!wpath) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, wlen);
    FILE* file = _wfopen(wpath, L"rb");
    free(wpath);
#else
    FILE* file = fopen(path, "rb");
#endif
    if (!file) return NULL;

    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    unsigned char* buf = (unsigned char*)malloc(size);
    if (!buf) {
        fclose(file);
        return NULL;
    }

    size_t read = fread(buf, 1, size, file);
    fclose(file);
    *out_size = read;
    return buf;
}

// 检测 exe 尾部是否嵌入了 lenb 数据（以及可选的资源段）
// 返回: 1=有嵌入数据, 0=无嵌入数据
// 尾部布局: [vm][资源段][资源段大小][RES_MAGIC][lenb][lenb大小][LENB_MAGIC]
static int check_embedded_lenb(const char* exe_path, unsigned char** out_data, size_t* out_size,
                               unsigned char** out_res, size_t* out_res_size) {
    *out_data = NULL;
    *out_size = 0;
    *out_res = NULL;
    *out_res_size = 0;

    size_t file_size = 0;
    unsigned char* data = read_file_binary(exe_path, &file_size);
    if (!data || file_size < 8) {
        if (data) free(data);
        return 0;
    }

    // 读取尾部 8 字节: [lenb_size: uint32] [magic: uint32]
    uint32_t tail_magic, lenb_size;
    memcpy(&lenb_size, data + file_size - 8, 4);
    memcpy(&tail_magic, data + file_size - 4, 4);

    if (tail_magic != LENB_MAGIC) {
        free(data);
        return 0;
    }

    if (lenb_size == 0 || lenb_size > file_size - 8) {
        free(data);
        return 0;
    }

    // 提取 lenb 数据
    unsigned char* lenb_data = (unsigned char*)malloc(lenb_size);
    if (!lenb_data) {
        free(data);
        return 0;
    }

    size_t lenb_start = file_size - 8 - lenb_size;
    memcpy(lenb_data, data + lenb_start, lenb_size);

    // lenb 之前若还有 [资源段大小][RES_MAGIC]，就是单文件打包的资源段
    if (lenb_start >= 8) {
        uint32_t res_size = 0, res_magic = 0;
        memcpy(&res_size, data + lenb_start - 8, 4);
        memcpy(&res_magic, data + lenb_start - 4, 4);
        if (res_magic == RES_MAGIC && res_size > 0 && res_size <= lenb_start - 8) {
            unsigned char* res_data = (unsigned char*)malloc(res_size);
            if (res_data) {
                memcpy(res_data, data + lenb_start - 8 - res_size, res_size);
                *out_res = res_data;
                *out_res_size = res_size;
            }
        }
    }

    *out_data = lenb_data;
    *out_size = lenb_size;
    free(data);
    return 1;
}

// 从内存中的 lenb 数据运行（直接内存反序列化，不写临时文件）
static int run_lenb_from_memory(unsigned char* data, size_t size) {
    if (size < 20) {
        fprintf(stderr, "[错误] lenb 数据过小\n");
        return 1;
    }

    Chunk chunk;
    chunk_init(&chunk);
    Scope* scope = NULL;

    SerializeResult result = chunk_deserialize_from_memory(data, size, &chunk, &scope);
    if (result != SERIALIZE_OK) {
        fprintf(stderr, "[错误] 内存反序列化失败: %d\n", result);
        chunk_free(&chunk);
        return 1;
    }

    // 注册 struct/face/enum 定义到全局表
    register_defs_from_chunk(&chunk);

    // 初始化 GC
    gc_init();

    // 修复模块函数指针
    fix_module_function_ptrs(&chunk);

    // 初始化 VM 并执行
    vm_init_with_scope(scope);
    int ret = vm_run_chunk(&chunk);
    chunk_free(&chunk);
    // scope 已被 vm_init_with_scope 设为 vm.global_scope，由 gc_free_all 释放
    gc_free_all();
    // main 的返回值作为进程退出码
    if (ret == 0) ret = vm_get_exit_code();
    return ret;
}

// 运行 .lenb 文件
int lenolang_run_lenb(const char* filename) {
    if (!serialize_is_binary_file(filename)) {
        fprintf(stderr, "[错误] 不是有效的 .lenb 文件: %s\n", filename);
        return 1;
    }

    Chunk chunk;
    chunk_init(&chunk);
    Scope* scope = NULL;

    SerializeResult result = chunk_deserialize(filename, &chunk, &scope);
    if (result != SERIALIZE_OK) {
        fprintf(stderr, "[错误] 反序列化失败: %d\n", result);
        chunk_free(&chunk);
        return 1;
    }

    // 注册 struct/face/enum 定义到全局表
    register_defs_from_chunk(&chunk);

    // 初始化 GC
    gc_init();

    // 修复模块函数指针
    fix_module_function_ptrs(&chunk);

    // 初始化 VM 并执行
    vm_init_with_scope(scope);

    int ret = vm_run_chunk(&chunk);
    chunk_free(&chunk);
    gc_free_all();
    // main 的返回值作为进程退出码
    if (ret == 0) ret = vm_get_exit_code();
    return ret;
}

// --check-bin <file>：只校验「本 VM 能不能读出这个 .lenb」（不注册定义、不初始化 VM、不执行）
// ----------------------------------------------------------------------------
// 打包器（main.c 的 pack_vm_check_lenb）用它做**打包前握手**：编译器侧改过序列化
// 格式 / LENO_BIN_VERSION 而 VM 二进制没重建时，产物会被内嵌进 exe 却读不出来
// （双击即「内存反序列化失败: 5」= SERIALIZE_ERR_VERSION 闪退）⇒ 打包必须先失败。
// 退出码：0 = 可读；2 = 不可读（报错文本与运行期同源）；其它由调用方按"非 0 即失败"处理。
static int check_lenb_only(const char* filename) {
    if (!serialize_is_binary_file(filename)) {
        fprintf(stderr, "[错误] 不是有效的 .lenb 文件: %s\n", filename);
        return 2;
    }

    Chunk chunk;
    chunk_init(&chunk);
    Scope* scope = NULL;

    SerializeResult result = chunk_deserialize(filename, &chunk, &scope);
    if (result != SERIALIZE_OK) {
        fprintf(stderr, "[错误] 反序列化失败: %d\n", result);
        chunk_free(&chunk);
        return 2;
    }

    // 只校验：不 register_defs_from_chunk / 不 gc_init / 不 vm_run_chunk。
    // 反序列化出的对象由进程退出交给 OS 回收（一次性短命进程，不做 gc_free_all）。
    chunk_free(&chunk);
    return 0;
}

// VM 主逻辑（平台无关）
static int vm_run_main(int argc, char** argv) {
    // --check-bin <file>：只校验不执行 —— 必须排在"自检内嵌数据"之前，
    //   否则带内嵌数据的 VM 会先走自运行分支，命令行参数就被丢掉了。
    if (argc >= 3 && argv[1] && strcmp(argv[1], "--check-bin") == 0) {
        return check_lenb_only(argv[2]);
    }

    // 自动检测 exe 尾部是否嵌入了 lenb 数据
    char exe_path[MAX_PATH_LEN];
    exe_path[0] = '\0';
#ifdef _WIN32
    wchar_t wexe_path[MAX_PATH_LEN];
    GetModuleFileNameW(NULL, wexe_path, MAX_PATH_LEN);
    WideCharToMultiByte(CP_UTF8, 0, wexe_path, -1, exe_path, MAX_PATH_LEN, NULL, NULL);
#else
    /* Linux/macOS 取自身路径统一走 platform_self_exe_path。
     * 取不到就留空串 ⇒ 走"无嵌入数据"分支（fail-closed，与 serialize.c 一致）。
     * 实现与那两个坑（readlink 不补 '\0'、macOS 没有 /proc）见 src/platform/platform_path.c */
    platform_self_exe_path(exe_path, sizeof(exe_path));
#endif

    unsigned char* embedded_data = NULL;
    size_t embedded_size = 0;
    unsigned char* embedded_res = NULL;
    size_t embedded_res_size = 0;
    if (check_embedded_lenb(exe_path, &embedded_data, &embedded_size,
                            &embedded_res, &embedded_res_size)) {
        // 有内嵌资源段：先解包到 exe 目录（幂等），再执行
        if (embedded_res) {
            extract_embedded_resources(exe_path, embedded_res, embedded_res_size);
            free(embedded_res);
        }
        // 有嵌入数据，直接执行
        int ret = run_lenb_from_memory(embedded_data, embedded_size);
        free(embedded_data);
        return ret;
    }

    // 无嵌入数据，作为命令行工具使用
    if (argc < 2) {
        printf("LenoLang VM - 独立运行时\n");
        printf("用法: leno_vm <file.lenb>\n");
        printf("      leno_vm --check-bin <file.lenb>   只校验能否读出，不执行（打包前握手用）\n");
        return 0;
    }

    // 运行 .lenb 文件
    return lenolang_run_lenb(argv[1]);
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    // 设置全局参数
    //   ⚠ 必须走 WideCharToMultiByte(CP_UTF8)：原来的 wcstombs 按 C locale 转换，
    //   中文/Unicode 路径会被吞成 '?'（`leno_vm <中文路径>.lenb` 打不开；打包前的
    //   --check-bin 握手也靠它）。编译器侧 main.c 的 wmain 是同一口径。
    g_argc = argc;
    g_argv = (char**)malloc(argc * sizeof(char*));
    if (g_argv) {
        for (int i = 0; i < argc; i++) {
            int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
            if (len <= 0) { g_argv[i] = NULL; continue; }
            g_argv[i] = (char*)malloc(len);
            if (g_argv[i]) {
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, g_argv[i], len, NULL, NULL);
            }
        }
    }
    return vm_run_main(argc, g_argv);
}
#else
int main(int argc, char* argv[]) {
    g_argc = argc;
    g_argv = argv;
    return vm_run_main(argc, argv);
}
#endif
