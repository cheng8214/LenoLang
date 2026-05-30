#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 外部声明：main.c 中定义的命令行参数
extern int g_argc;
extern char** g_argv;

// _args() - 返回命令行参数数组
static Value native_args(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    ObjArray* arr = arr_new(g_argc);
    for (int i = 0; i < g_argc; i++) {
        arr_write(arr, i, val_obj((Object*)str_copy(g_argv[i], (int)strlen(g_argv[i]))));
    }
    arr->count = g_argc;
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

// ==================== 初始化 ====================

void sys_init_globals(void) {
    // 注册全局 _args 函数（返回 Array，0 个参数）
    vm_register_native("_args", native_args, 0, -1, -1, TYPE_ARRAY, NULL);

    // 注册全局 _script 函数（返回 string，0 个参数）
    vm_register_native("_script", native_script, 0, -1, -1, TYPE_STRING, NULL);

    // 注册全局 _executable 函数（返回 string，0 个参数）
    vm_register_native("_executable", native_executable, 0, -1, -1, TYPE_STRING, NULL);

    // 注册全局 _gc 函数（返回 bool，0 或 1 个参数）
    vm_register_native("_gc", native_gc_control, -1, 0, 1, TYPE_BOOL, NULL);

    // 注册全局 _os 函数（返回 string，0 个参数）
    vm_register_native("_os", native_os, 0, -1, -1, TYPE_STRING, NULL);

    // 注册全局 _clear 函数（清屏，0 个参数）
    vm_register_native("_clear", native_clear, 0, -1, -1, TYPE_NULL, NULL);
}
