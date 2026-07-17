#include "include/leno_vm_runtime.h"
#include "include/leno_serialize.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

// VM 独立运行时 - 不依赖编译器
// 启动时自动检测 exe 尾部是否嵌入了 lenb 数据：
//   - 有嵌入数据：直接执行嵌入的字节码（打包后的独立 exe 模式）
//   - 无嵌入数据：作为命令行工具，需要传入 .lenb 文件路径
//
// 尾部数据格式: [exe 原始数据] [lenb 数据] [4字节 lenb_size] [4字节 LENB_MAGIC]

extern VM vm;

// 全局变量（VM 运行时需要，原定义在 main.c）
int debugMode = 0;
int g_argc = 0;
char** g_argv = NULL;

// lenb 文件魔数
#define LENB_MAGIC 0x424E454C

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

// 检测 exe 尾部是否嵌入了 lenb 数据
// 返回: 1=有嵌入数据, 0=无嵌入数据
// 如果有嵌入数据，通过 out_data/out_size 返回提取的 lenb 数据
static int check_embedded_lenb(const char* exe_path, unsigned char** out_data, size_t* out_size) {
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

    memcpy(lenb_data, data + file_size - 8 - lenb_size, lenb_size);
    *out_data = lenb_data;
    *out_size = lenb_size;
    free(data);
    return 1;
}

// 从内存中的 lenb 数据运行（写入临时文件再加载）
static int run_lenb_from_memory(unsigned char* data, size_t size) {
    if (size < 20) {
        fprintf(stderr, "[错误] lenb 数据过小\n");
        return 1;
    }

    // 写入临时文件
    char temp_path[MAX_PATH_LEN];
#ifdef _WIN32
    char temp_dir[MAX_PATH_LEN];
    GetTempPathA(MAX_PATH_LEN, temp_dir);
    temp_dir[MAX_PATH_LEN - 1] = '\0';
    // 使用 %lu 格式匹配 DWORD (unsigned long)
    DWORD pid = GetCurrentProcessId();
    // 检查路径长度，避免截断
    size_t dir_len = strlen(temp_dir);
    // 简化格式: temp_dir + "vm_" + pid(10位) + ".lenb" = dir_len + 3 + 10 + 5 = dir_len + 18
    if (dir_len + 18 < MAX_PATH_LEN) {
        snprintf(temp_path, MAX_PATH_LEN, "%svm_%lu.lenb", temp_dir, (unsigned long)pid);
    } else {
        // 路径太长，使用当前目录
        snprintf(temp_path, MAX_PATH_LEN, "vm_%lu.lenb", (unsigned long)pid);
    }
#else
    snprintf(temp_path, MAX_PATH_LEN, "/tmp/leno_vm_%d.lenb", getpid());
#endif

    FILE* fp = fopen(temp_path, "wb");
    if (!fp) {
        fprintf(stderr, "[错误] 无法创建临时文件: %s\n", temp_path);
        return 1;
    }
    fwrite(data, 1, size, fp);
    fclose(fp);

    int ret = lenolang_run_lenb(temp_path);

    // 清理临时文件
    remove(temp_path);
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
    return ret;
}

int wmain(int argc, wchar_t* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);
#endif

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

    // 自动检测 exe 尾部是否嵌入了 lenb 数据
    char exe_path[MAX_PATH_LEN];
#ifdef _WIN32
    wchar_t wexe_path[MAX_PATH_LEN];
    GetModuleFileNameW(NULL, wexe_path, MAX_PATH_LEN);
    WideCharToMultiByte(CP_UTF8, 0, wexe_path, -1, exe_path, MAX_PATH_LEN, NULL, NULL);
#else
    readlink("/proc/self/exe", exe_path, sizeof(exe_path));
#endif

    unsigned char* embedded_data = NULL;
    size_t embedded_size = 0;
    if (check_embedded_lenb(exe_path, &embedded_data, &embedded_size)) {
        // 有嵌入数据，直接执行
        int ret = run_lenb_from_memory(embedded_data, embedded_size);
        free(embedded_data);
        return ret;
    }

    // 无嵌入数据，作为命令行工具使用
    if (argc < 2) {
        printf("LenoLang VM - 独立运行时\n");
        printf("用法: leno_vm.exe <file.lenb>\n");
        return 0;
    }

    // 转换宽字符参数为 UTF-8
    char arg_utf8[MAX_PATH_LEN];
    size_t arg_len = wcstombs(arg_utf8, argv[1], MAX_PATH_LEN - 1);
    arg_utf8[arg_len] = '\0';

    // 运行 .lenb 文件
    return lenolang_run_lenb(arg_utf8);
}
