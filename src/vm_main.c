#include "include/leno_vm_runtime.h"
#include "include/leno_serialize.h"
#include "include/native.h"
#include "include/platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <unistd.h>
#include <sys/stat.h>
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
// 资源段内部格式（索引在前、内容在后，便于"先比哈希再决定落盘"）：
//   "LENOPACK"(8) | entry_count:u32
//   entry_count × [ rel_len:u16 | size:u64 | hash:u32 | rel_path(rel_len) ]
//   各文件内容按索引顺序紧随其后
// 解包目标 = **exe 所在目录**，理由：
//   ① dirs.script_dir() 在打包 exe 上就是 exe 目录，GUI 应用用它拼资源绝对路径
//      （file_manager.leno 的 imgDir = script_dir()/images）⇒ 还原到这儿语义不变，
//      书签、初始浏览目录这类"exe 旁数据"的行为也一字不改；
//   ② DLL 落回 exe 旁 ⇒ Windows 的 DLL 搜索顺序第一项就是 exe 目录，SDL3_image→SDL3
//      这类兄弟依赖天然成立，无需 SetDllDirectory。
// 幂等：磁盘上已存在且内容哈希一致的文件不重写 ⇒ 二次启动零写入。
// ============================================================================
#define RES_BLOB_HEADER "LENOPACK"

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

// 解包资源段到 exe 目录；返回写入的文件数（-1 表示格式错误）
static int extract_embedded_resources(const char* exe_path,
                                      const unsigned char* blob, size_t blob_len) {
    if (blob_len < 12 || memcmp(blob, RES_BLOB_HEADER, 8) != 0) return -1;

    uint32_t count = 0;
    memcpy(&count, blob + 8, 4);
    if (count == 0 || count > 100000) return -1;

    // exe 所在目录（含结尾分隔符）
    char exe_dir[MAX_PATH_LEN];
    strncpy(exe_dir, exe_path, sizeof(exe_dir) - 1);
    exe_dir[sizeof(exe_dir) - 1] = '\0';
    char* s1 = strrchr(exe_dir, '\\');
    char* s2 = strrchr(exe_dir, '/');
    if (s2 && (!s1 || s2 > s1)) s1 = s2;
    if (!s1) return -1;
    *(s1 + 1) = '\0';

    // 第一遍：读索引，算内容区起点
    const unsigned char* idx = blob + 12;
    const unsigned char* blob_end = blob + blob_len;
    size_t content_off = 12;
    for (uint32_t i = 0; i < count; i++) {
        if (idx + 2 + 8 + 4 > blob_end) return -1;
        uint16_t rlen = 0;
        memcpy(&rlen, idx, 2);
        uint64_t fsize = 0;
        memcpy(&fsize, idx + 2, 8);
        content_off += 2 + 8 + 4 + rlen;
        (void)fsize;
        idx += 2 + 8 + 4 + rlen;
    }
    if (content_off > blob_len) return -1;

    // 第二遍：逐个落盘（哈希一致的跳过）
    idx = blob + 12;
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
            return -1;   // 索引损坏：直接报错，不写半个目录
        }
        char rel_buf[MAX_PATH_LEN];
        size_t copy_len = (rlen < sizeof(rel_buf) - 1) ? rlen : sizeof(rel_buf) - 1;
        memcpy(rel_buf, rel, copy_len);
        rel_buf[copy_len] = '\0';

        char dst[MAX_PATH_LEN * 2];
        snprintf(dst, sizeof(dst), "%s%s", exe_dir, rel_buf);
        // 统一分隔符（内嵌路径一律用 '/'，Windows 下换成盘符风格更好读）
#ifdef _WIN32
        for (char* p = dst + strlen(exe_dir); *p; p++) {
            if (*p == '/') *p = '\\';
        }
#endif

        if (res_hash_existing(dst) == want_hash) {
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

    if (written > 0 || failed > 0) {
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

// VM 主逻辑（平台无关）
static int vm_run_main(int argc, char** argv) {
    // 自动检测 exe 尾部是否嵌入了 lenb 数据
    char exe_path[MAX_PATH_LEN];
#ifdef _WIN32
    wchar_t wexe_path[MAX_PATH_LEN];
    GetModuleFileNameW(NULL, wexe_path, MAX_PATH_LEN);
    WideCharToMultiByte(CP_UTF8, 0, wexe_path, -1, exe_path, MAX_PATH_LEN, NULL, NULL);
#else
    { ssize_t _r = readlink("/proc/self/exe", exe_path, sizeof(exe_path)); (void)_r; }
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
    g_argc = argc;
    g_argv = (char**)malloc(argc * sizeof(char*));
    if (g_argv) {
        for (int i = 0; i < argc; i++) {
            size_t len = wcstombs(NULL, argv[i], 0);
            g_argv[i] = (char*)malloc(len + 1);
            wcstombs(g_argv[i], argv[i], len + 1);
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
