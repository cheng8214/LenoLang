#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
    #include <windows.h>
    #include <process.h>
#else
    #include <unistd.h>
    #include <pwd.h>
#endif

// 外部声明：main.c 中定义的命令行参数
extern int g_argc;
extern char** g_argv;

// _args() - 返回脚本命令行参数数组（不包含解释器路径和脚本路径）
static Value native_args(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    // 找到脚本路径的位置（第一个非选项参数）
    int script_idx = 1;
    for (int i = 1; i < g_argc; i++) {
        if (g_argv[i][0] != '-') {
            script_idx = i;
            break;
        }
    }
    
    // 检查是否是打包版（第一个非选项参数是 .exe 本身）
    int is_packaged = 0;
    if (script_idx < g_argc) {
        const char* first_arg = g_argv[script_idx];
        int len = (int)strlen(first_arg);
        is_packaged = (len > 4 && strcmp(first_arg + len - 4, ".exe") == 0);
    }
    
    // 打包版：跳过 exe 自身即可；普通模式：跳过解释器+脚本
    int start_idx = is_packaged ? 1 : script_idx + 1;
    int real_argc = g_argc - start_idx;
    if (real_argc < 0) real_argc = 0;
    
    ObjArray* arr = arr_new(real_argc);
    for (int i = 0; i < real_argc; i++) {
        arr_write(arr, i, val_obj((Object*)str_copy(
            g_argv[start_idx + i], 
            (int)strlen(g_argv[start_idx + i]))));
    }
    arr->count = real_argc;
    return val_obj((Object*)arr);
}

// _script() - 返回当前脚本路径（第一个非选项参数）
static Value native_script(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    for (int i = 1; i < g_argc; i++) {
        if (g_argv[i][0] != '-') {
            return val_obj((Object*)str_copy(g_argv[i], (int)strlen(g_argv[i])));
        }
    }
    return val_null();
}

// _executable() - 返回可执行文件路径
static Value native_executable(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    if (g_argc > 0 && g_argv[0]) {
        return val_obj((Object*)str_copy(g_argv[0], (int)strlen(g_argv[0])));
    }
    return val_null();
}

// _gc(enabled) - 控制GC开关
static Value native_gc_control(int argCount, Value* args) {
    if (argCount > 0) {
        int enabled = 0;
        if (val_is_bool(args[0])) {
            enabled = val_as_bool(args[0]);
        } else if (val_is_int(args[0])) {
            enabled = (val_as_num(args[0]) != 0);
        }
        gc_set_enabled(enabled);
    }
    return val_bool(gc_get_enabled());
}

// _os() - 返回操作系统名称
static Value native_os(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_obj((Object*)str_copy("windows", 7));
    #elif defined(__APPLE__)
        #include "TargetConditionals.h"
        #if TARGET_OS_MAC
            return val_obj((Object*)str_copy("macos", 5));
        #else
            return val_obj((Object*)str_copy("ios", 3));
        #endif
    #elif defined(__linux__)
        return val_obj((Object*)str_copy("linux", 5));
    #elif defined(__unix__)
        return val_obj((Object*)str_copy("unix", 4));
    #else
        return val_obj((Object*)str_copy("unknown", 7));
    #endif
}

// _clear() - 清屏（跨平台）
static Value native_clear(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        system("cls");
    #else
        system("clear");
    #endif
    return val_null();
}

// _console(show) - 显示/隐藏控制台窗口（Windows）
static Value native_console(int argCount, Value* args) {
    #ifdef _WIN32
        if (argCount > 0) {
            int show = 1;
            if (val_is_bool(args[0])) {
                show = val_as_bool(args[0]);
            } else if (val_is_int(args[0])) {
                show = (val_as_num(args[0]) != 0);
            }
            HWND hwnd = GetConsoleWindow();
            if (hwnd != NULL) {
                ShowWindow(hwnd, show ? SW_SHOW : SW_HIDE);
            }
        }
        // 返回当前控制台窗口是否可见
        HWND hwnd = GetConsoleWindow();
        if (hwnd != NULL) {
            return val_bool(IsWindowVisible(hwnd));
        }
        return val_bool(false);
    #else
        // 非 Windows 平台，控制台隐藏功能不可用
        (void)argCount;
        (void)args;
        return val_bool(true);
    #endif
}

// _env(name) - 获取环境变量
// _env(name, value) - 设置环境变量，返回是否成功
static Value native_env(int argCount, Value* args) {
    if (argCount < 1 || !val_is_string(args[0])) {
        return val_null();
    }

    ObjString* name = (ObjString*)val_as_obj(args[0]);

    if (argCount >= 2 && val_is_string(args[1])) {
        // 设置环境变量
        ObjString* value = (ObjString*)val_as_obj(args[1]);
        #ifdef _WIN32
            int result = _putenv_s(name->chars, value->chars);
        #else
            int result = setenv(name->chars, value->chars, 1);
        #endif
        return val_bool(result == 0);
    }

    // 获取环境变量
    const char* val = getenv(name->chars);
    if (val == NULL) {
        return val_null();
    }
    return val_obj((Object*)str_copy(val, (int)strlen(val)));
}

// _exit(code) - 以指定退出码终止程序
static Value native_exit(int argCount, Value* args) {
    int code = 0;
    if (argCount > 0) {
        if (val_is_int(args[0])) {
            code = val_as_int(args[0]);
        } else if (val_is_float(args[0])) {
            code = (int)val_as_double(args[0]);
        }
    }
    exit(code);
    return val_null(); // 不会执行
}

// _pid() - 返回当前进程ID
static Value native_pid(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_int((int)GetCurrentProcessId());
    #else
        return val_int((int)getpid());
    #endif
}

// _arch() - 返回CPU架构
static Value native_arch(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #if defined(_M_X64) || defined(__x86_64__) || defined(__amd64__)
        return val_obj((Object*)str_copy("x64", 3));
    #elif defined(_M_IX86) || defined(__i386__) || defined(__i486__) || defined(__i586__) || defined(__i686__)
        return val_obj((Object*)str_copy("x86", 3));
    #elif defined(_M_ARM64) || defined(__aarch64__) || defined(__arm64__)
        return val_obj((Object*)str_copy("arm64", 5));
    #elif defined(_M_ARM) || defined(__arm__)
        return val_obj((Object*)str_copy("arm", 3));
    #elif defined(__riscv) && __riscv_xlen == 64
        return val_obj((Object*)str_copy("riscv64", 7));
    #elif defined(__riscv) && __riscv_xlen == 32
        return val_obj((Object*)str_copy("riscv32", 7));
    #else
        return val_obj((Object*)str_copy("unknown", 7));
    #endif
}

// _exec(cmd) - 执行系统命令并返回 [stdout, exit_code]
// 使用 system() + 临时文件避免 _popen 在大量并发调用时不稳定的问题
// Windows 版使用 _wsystem + UTF-16 转换以支持中文/Unicode 路径
static Value native_exec(int argCount, Value* args) {
    if (argCount < 1 || !val_is_string(args[0])) {
        return val_null();
    }
    ObjString* cmd = (ObjString*)val_as_obj(args[0]);

#ifdef _WIN32
    // 将 UTF-8 cmd 转为 UTF-16 宽字符
    int wlen = MultiByteToWideChar(CP_UTF8, 0, cmd->chars, -1, NULL, 0);
    if (wlen <= 0) return val_null();
    wchar_t* wcmd = malloc(wlen * sizeof(wchar_t));
    if (!wcmd) return val_null();
    MultiByteToWideChar(CP_UTF8, 0, cmd->chars, -1, wcmd, wlen);

    // 获取临时目录 (宽字符)
    wchar_t wtmp_path[MAX_PATH];
    DWORD tlen = GetTempPathW(MAX_PATH, wtmp_path);
    if (tlen == 0 || tlen >= MAX_PATH - 64) { free(wcmd); return val_null(); }
    swprintf(wtmp_path + tlen, MAX_PATH - tlen, L"leno_exec_%lu.txt", GetCurrentProcessId());

    // 构建宽字符命令: cmd /c "command" > tmp 2>&1
    size_t full_len = wcslen(wcmd) + wcslen(wtmp_path) + 64;
    wchar_t* wfull = malloc(full_len * sizeof(wchar_t));
    if (!wfull) { free(wcmd); return val_null(); }
    swprintf(wfull, full_len, L"cmd /c %s > \"%s\" 2>&1", wcmd, wtmp_path);
    int rc = _wsystem(wfull);
    free(wfull);
    free(wcmd);

    // 读取临时文件 (宽字符路径)
    FILE* f = _wfopen(wtmp_path, L"rb");
    if (!f) { DeleteFileW(wtmp_path); return val_null(); }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* output = malloc(flen + 1);
    if (!output) { fclose(f); DeleteFileW(wtmp_path); return val_null(); }
    fread(output, 1, flen, f);
    output[flen] = '\0';
    fclose(f);
    DeleteFileW(wtmp_path);

    ObjArray* result = arr_new(2);
    arr_write(result, 0, val_obj((Object*)str_copy(output, (int)flen)));
    arr_write(result, 1, val_int(rc));
    result->count = 2;
    free(output);
    return val_obj((Object*)result);
#else
    FILE* fp = popen(cmd->chars, "r");
    if (!fp) return val_null();

    char buffer[256];
    size_t capacity = 1024, len = 0;
    char* output = malloc(capacity);
    if (!output) { pclose(fp); return val_null(); }
    output[0] = '\0';

    while (fgets(buffer, sizeof(buffer), fp)) {
        size_t bl = strlen(buffer);
        if (len + bl + 1 > capacity) {
            capacity = (len + bl + 1) * 2;
            char* newer = realloc(output, capacity);
            if (!newer) { free(output); pclose(fp); return val_null(); }
            output = newer;
        }
        memcpy(output + len, buffer, bl);
        len += bl;
        output[len] = '\0';
    }
    int status = pclose(fp);
    int rc = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    ObjArray* result = arr_new(2);
    arr_write(result, 0, val_obj((Object*)str_copy(output, (int)len)));
    arr_write(result, 1, val_int(rc));
    result->count = 2;
    free(output);
    return val_obj((Object*)result);
#endif
}

// _username() - 返回当前登录用户名
static Value native_username(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        char username[256];
        DWORD size = sizeof(username);
        if (GetUserNameA(username, &size)) {
            return val_obj((Object*)str_copy(username, (int)strlen(username)));
        }
    #else
        struct passwd* pw = getpwuid(getuid());
        if (pw != NULL) {
            return val_obj((Object*)str_copy(pw->pw_name, (int)strlen(pw->pw_name)));
        }
        // 回退：使用环境变量
        const char* user = getenv("USER");
        if (user != NULL) {
            return val_obj((Object*)str_copy(user, (int)strlen(user)));
        }
    #endif
    return val_null();
}

// _homedir() - 返回用户主目录路径
static Value native_homedir(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        // 优先使用 USERPROFILE
        const char* home = getenv("USERPROFILE");
        if (home != NULL) {
            return val_obj((Object*)str_copy(home, (int)strlen(home)));
        }
        // 回退：组合 HOMEDRIVE 和 HOMEPATH
        const char* drive = getenv("HOMEDRIVE");
        const char* path = getenv("HOMEPATH");
        if (drive != NULL && path != NULL) {
            int total_len = (int)(strlen(drive) + strlen(path));
            char* full = (char*)malloc(total_len + 1);
            if (full != NULL) {
                sprintf(full, "%s%s", drive, path);
                ObjString* result = str_copy(full, total_len);
                free(full);
                return val_obj((Object*)result);
            }
        }
    #else
        const char* home = getenv("HOME");
        if (home != NULL) {
            return val_obj((Object*)str_copy(home, (int)strlen(home)));
        }
        struct passwd* pw = getpwuid(getuid());
        if (pw != NULL) {
            return val_obj((Object*)str_copy(pw->pw_dir, (int)strlen(pw->pw_dir)));
        }
    #endif
    return val_null();
}

// _tmpdir() - 返回系统临时目录路径
static Value native_tmpdir(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        char buf[MAX_PATH];
        UINT len = GetTempPathA(MAX_PATH, buf);
        if (len > 0) {
            return val_obj((Object*)str_copy(buf, (int)len));
        }
    #else
        const char* tmp = getenv("TMPDIR");
        if (tmp != NULL) {
            return val_obj((Object*)str_copy(tmp, (int)strlen(tmp)));
        }
        return val_obj((Object*)str_copy("/tmp", 4));
    #endif
    return val_null();
}

// _sep() - 返回路径分隔符
static Value native_sep(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    #ifdef _WIN32
        return val_obj((Object*)str_copy("\\", 1));
    #else
        return val_obj((Object*)str_copy("/", 1));
    #endif
}

// ==================== 初始化 ====================

void sys_init_globals(void) {
    // 注册全局 _args 函数（返回 Array[string]，0 个参数）
    vm_register_native("_args", native_args, 0, -1, -1, TYPE_ARRAY, TYPE_STRING, NULL);

    // 注册全局 _script 函数（返回 string，0 个参数）
    vm_register_native("_script", native_script, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _executable 函数（返回 string，0 个参数）
    vm_register_native("_executable", native_executable, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _gc 函数（返回 bool，0 或 1 个参数）
    vm_register_native("_gc", native_gc_control, -1, 0, 1, TYPE_BOOL, TYPE_UNKNOWN, NULL);

    // 注册全局 _os 函数（返回 string，0 个参数）
    vm_register_native("_os", native_os, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _clear 函数（清屏，0 个参数）
    vm_register_native("_clear", native_clear, 0, -1, -1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 _console 函数（控制台显示控制，0 或 1 个参数）
    vm_register_native("_console", native_console, -1, 0, 1, TYPE_BOOL, TYPE_UNKNOWN, NULL);

    // 注册全局 _env 函数（环境变量，1 或 2 个参数）
    vm_register_native("_env", native_env, -1, 1, 2, TYPE_ANY, TYPE_UNKNOWN, NULL);

    // 注册全局 _exit 函数（退出程序，0 或 1 个参数）
    vm_register_native("_exit", native_exit, -1, 0, 1, TYPE_NULL, TYPE_UNKNOWN, NULL);

    // 注册全局 _pid 函数（进程ID，0 个参数）
    vm_register_native("_pid", native_pid, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, NULL);

    // 注册全局 _arch 函数（CPU架构，0 个参数）
    vm_register_native("_arch", native_arch, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _exec 函数（返回 [stdout_string, exit_code] 数组，混合类型用 TYPE_ANY）
    vm_register_native("_exec", native_exec, 1, -1, -1, TYPE_ARRAY, TYPE_ANY, NULL);

    // 注册全局 _username 函数（用户名，0 个参数）
    vm_register_native("_username", native_username, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _homedir 函数（主目录，0 个参数）
    vm_register_native("_homedir", native_homedir, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _tmpdir 函数（临时目录，0 个参数）
    vm_register_native("_tmpdir", native_tmpdir, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);

    // 注册全局 _sep 函数（路径分隔符，0 个参数）
    vm_register_native("_sep", native_sep, 0, -1, -1, TYPE_STRING, TYPE_UNKNOWN, NULL);
}
