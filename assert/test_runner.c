// ============================================================================
// LenoC 自动化测试运行器
//
// 用法：test_runner.exe <leno_executable> <test_directory>
//
// 测试文件约定：
//   1. 每个测试是一个 .leno 文件
//   2. 测试文件名以 test_ 开头
//   3. 测试使用内置 assert() 函数进行断言
//   4. 退出码 0 = 通过，非 0 = 失败
//   5. 可选：同目录下放 .expected 文件指定期望输出
//
// 输出格式：
//   [PASS] test_name.leno
//   [FAIL] test_name.leno - 原因
//   [ERROR] test_name.leno - 原因
//   ========================================
//   Results: 42 passed, 3 failed, 1 error
// ============================================================================

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#endif

#define MAX_PATH_LEN 512
#define MAX_TESTS 1024
#define MAX_OUTPUT 65536

typedef struct {
    char path[MAX_PATH_LEN];
    char name[256];
    int passed;
    int failed;
    int error;
    char message[4608];
} TestResult;

static TestResult results[MAX_TESTS];
static int test_count = 0;
static int total_passed = 0;
static int total_failed = 0;
static int total_error = 0;

static int ends_with(const char* str, const char* suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    if (suffix_len > str_len) return 0;
    return strcmp(str + str_len - suffix_len, suffix) == 0;
}

static int starts_with(const char* str, const char* prefix) {
    return strncmp(str, prefix, strlen(prefix)) == 0;
}

static const char* get_filename(const char* path) {
    const char* slash = strrchr(path, '\\');
    if (!slash) slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static void run_test(const char* leno_exe, const char* test_path) {
    TestResult* result = &results[test_count++];
    strncpy(result->path, test_path, MAX_PATH_LEN - 1);
    strncpy(result->name, get_filename(test_path), 255);

    char cmd[MAX_PATH_LEN * 3];
    char output[MAX_OUTPUT] = {0};

#ifdef _WIN32
    // 使用临时文件捕获输出，避免管道句柄继承问题
    char temp_file[MAX_PATH_LEN];
    GetTempPathA(MAX_PATH_LEN - 1, temp_file);
    strncat(temp_file, "leno_test_output.txt", MAX_PATH_LEN - strlen(temp_file) - 1);
    
    // 使用 _spawnlp 来正确执行命令
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"", leno_exe, test_path);
    
    // 构建重定向命令
    char cmd_with_redirect[MAX_PATH_LEN * 4];
    snprintf(cmd_with_redirect, sizeof(cmd_with_redirect), "cmd /c \"%s > \"%s\" 2>&1\"", cmd, temp_file);
    
    int ret = system(cmd_with_redirect);
    DWORD exit_code = (DWORD)ret;
    
    // 读取临时文件内容
    FILE* f = fopen(temp_file, "r");
    if (f) {
        size_t n = fread(output, 1, MAX_OUTPUT - 1, f);
        output[n] = '\0';
        fclose(f);
    }
    DeleteFileA(temp_file);
#else
    // POSIX: 使用 popen 捕获输出
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" 2>&1", leno_exe, test_path);
    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        result->error = 1;
        snprintf(result->message, sizeof(result->message),
                 "无法启动进程: %s", cmd);
        return;
    }

    size_t total_read = 0;
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe) && total_read < MAX_OUTPUT - 1) {
        size_t len = strlen(buf);
        if (total_read + len >= MAX_OUTPUT - 1) {
            len = MAX_OUTPUT - 1 - total_read;
        }
        memcpy(output + total_read, buf, len);
        total_read += len;
    }
    output[total_read] = '\0';

    int status = pclose(pipe);
    int exit_code = 1;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    }
#endif

    if (exit_code == 0) {
        char expected_path[MAX_PATH_LEN];
        snprintf(expected_path, sizeof(expected_path), "%.*s.expected",
                 (int)(strlen(test_path) - 5), test_path);

        FILE* ef = fopen(expected_path, "r");
        if (ef) {
            char expected[MAX_OUTPUT] = {0};
            size_t elen = fread(expected, 1, sizeof(expected) - 1, ef);
            expected[elen] = '\0';
            fclose(ef);

            char* e_ptr = expected;
            char* o_ptr = output;
            while (*e_ptr == '\r') e_ptr++;
            while (*o_ptr == '\r') o_ptr++;

            int match = 1;
            while (*e_ptr && *o_ptr) {
                if (*e_ptr == '\r') { e_ptr++; continue; }
                if (*o_ptr == '\r') { o_ptr++; continue; }
                if (*e_ptr != *o_ptr) { match = 0; break; }
                e_ptr++;
                o_ptr++;
            }
            if (*e_ptr != '\0' || *o_ptr != '\0') match = 0;

            if (match) {
                result->passed = 1;
                total_passed++;
            } else {
                result->failed = 1;
                total_failed++;
                snprintf(result->message, sizeof(result->message),
                         "输出不匹配期望\n--- 期望 ---\n%s\n--- 实际 ---\n%s",
                         expected, output);
            }
        } else {
            result->passed = 1;
            total_passed++;
        }
    } else {
        result->failed = 1;
        total_failed++;
        // 截取输出的前300个字符作为错误信息
        size_t msg_len = strlen(output);
        if (msg_len > 300) msg_len = 300;
        if (msg_len == 0) {
            snprintf(result->message, sizeof(result->message),
                     "退出码 %lu (无输出)", (unsigned long)exit_code);
        } else {
            snprintf(result->message, sizeof(result->message),
                     "退出码 %lu: %.*s", (unsigned long)exit_code, (int)msg_len, output);
        }
    }
}

static void scan_directory(const char* leno_exe, const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "无法打开目录: %s\n", dir_path);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        char full_path[MAX_PATH_LEN];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_directory(leno_exe, full_path);
        } else if (ends_with(entry->d_name, ".leno") &&
                   starts_with(entry->d_name, "test_")) {
            run_test(leno_exe, full_path);
        }
    }
    closedir(dir);
}

static int compare_by_name(const void* a, const void* b) {
    return strcmp(((const TestResult*)a)->name, ((const TestResult*)b)->name);
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        fprintf(stderr, "用法: %s <leno_executable> <test_directory>\n", argv[0]);
        return 1;
    }

    const char* leno_exe = argv[1];
    const char* test_dir = argv[2];

    printf("LenoC 自动化测试运行器\n");
    printf("解释器: %s\n", leno_exe);
    printf("测试目录: %s\n", test_dir);
    printf("========================================\n\n");

    scan_directory(leno_exe, test_dir);

    qsort(results, test_count, sizeof(TestResult), compare_by_name);

    for (int i = 0; i < test_count; i++) {
        if (results[i].passed) {
            printf("  [PASS] %s\n", results[i].name);
        } else if (results[i].failed) {
            printf("  [FAIL] %s - %s\n", results[i].name, results[i].message);
        } else if (results[i].error) {
            printf("  [ERROR] %s - %s\n", results[i].name, results[i].message);
        }
    }

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed, %d error (total %d)\n",
           total_passed, total_failed, total_error, test_count);

    return total_failed > 0 || total_error > 0 ? 1 : 0;
}
