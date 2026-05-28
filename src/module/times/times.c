#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>

#ifdef _WIN32
    #include <windows.h>
#endif
#include <time.h>
#ifndef _WIN32
    #include <sys/time.h>
#endif

// 获取当前时间（毫秒）
static Value native_times_ms(int argCount, Value* args) {
    (void)argCount;
    (void)args;

#ifdef _WIN32
    ULONGLONG ms = GetTickCount64();
    return val_int_safe((int64_t)ms);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long ms = (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
    return val_int_safe((int64_t)ms);
#endif
}

// 获取当前时间（微秒）
static Value native_times_us(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count)) {
        double us = (double)count.QuadPart * 1000000.0 / (double)freq.QuadPart;
        return val_num(us);
    }
    return val_num((double)GetTickCount() * 1000.0);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    long long us = (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
    return val_num((double)us);
#endif
}

// 获取当前时间（纳秒）
static Value native_times_ns(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    if (QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&count)) {
        double ns = (double)count.QuadPart * 1000000000.0 / (double)freq.QuadPart;
        return val_num(ns);
    }
    return val_num((double)GetTickCount() * 1000000.0);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return val_num((double)ns);
#endif
}

// 全局 sleep(ms) - 休眠指定毫秒
static Value native_sleep(int argCount, Value* args) {
    (void)argCount;
    int ms = val_is_bigint(args[0]) ? (int)bigint_to_int64(val_as_bigint(args[0])) : (int)val_as_num(args[0]);
    
    if (ms < 0) ms = 0;
    
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
    
    return val_null();
}

// 获取当前 Unix 时间戳（秒）
static Value native_times_now(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    
    time_t t = time(NULL);
    return val_int_safe((int64_t)t);
}

// 格式化时间戳
static Value native_times_format(int argCount, Value* args) {
    (void)argCount;
    
    int64_t timestamp = val_is_bigint(args[0]) ? bigint_to_int64(val_as_bigint(args[0])) : (int64_t)val_as_num(args[0]);
    ObjString* fmt_str = (ObjString*)val_as_obj(args[1]);
    const char* fmt = fmt_str->chars;
    
    time_t t = (time_t)timestamp;
    struct tm* tm_info;
    
#ifdef _WIN32
    tm_info = localtime(&t);
#else
    struct tm tm_buf;
    tm_info = localtime_r(&t, &tm_buf);
#endif
    
    if (!tm_info) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    char buffer[256];
    int result_len = strftime(buffer, sizeof(buffer), fmt, tm_info);
    
    if (result_len == 0) {
        return val_obj((Object*)str_copy("", 0));
    }
    
    ObjString* result = str_new(buffer, result_len);
    return val_obj((Object*)result);
}

// 辅助函数：向数组添加元素
static void arr_push_value(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        arr_grow(arr);
    }
    arr->elements[arr->count++] = value;
}

// 获取当前日期时间的数组表示 [年,月,日,时,分,秒]
static Value native_times_datetime(int argCount, Value* args) {
    (void)argCount;
    (void)args;
    
    time_t t = time(NULL);
    
    struct tm* tm_info;
#ifdef _WIN32
    tm_info = localtime(&t);
#else
    struct tm tm_buf;
    tm_info = localtime_r(&t, &tm_buf);
#endif
    
    if (!tm_info) {
        return val_null();
    }
    
    // 创建数组 [年,月,日,时,分,秒]
    ObjArray* arr = arr_new(6);
    if (!arr) return val_null();
    
    arr_push_value(arr, val_int(tm_info->tm_year + 1900));  // 年
    arr_push_value(arr, val_int(tm_info->tm_mon + 1));      // 月 (1-12)
    arr_push_value(arr, val_int(tm_info->tm_mday));         // 日
    arr_push_value(arr, val_int(tm_info->tm_hour));         // 时
    arr_push_value(arr, val_int(tm_info->tm_min));          // 分
    arr_push_value(arr, val_int(tm_info->tm_sec));          // 秒
    
    return val_obj((Object*)arr);
}

// ==================== 初始化 ====================

void times_init_globals(void) {
    // 注册全局 sleep 函数（1 个参数）
    TypeKind sleep_params[] = {TYPE_INT};
    vm_register_native("sleep", native_sleep, 1, -1, -1, TYPE_NULL, sleep_params);
}

// 初始化 times 模块（import times 时调用）
void times_init_module(void) {
    // 注册 times.ms 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    native_register_module_method("times", "ms", native_times_ms, 0, -1, -1, TYPE_INT, NULL);

    // 注册 times.us 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    native_register_module_method("times", "us", native_times_us, 0, -1, -1, TYPE_FLOAT, NULL);

    // 注册 times.ns 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    native_register_module_method("times", "ns", native_times_ns, 0, -1, -1, TYPE_FLOAT, NULL);

    // 新增：获取当前时间戳（秒）
    native_register_module_method("times", "now", native_times_now, 0, -1, -1, TYPE_INT, NULL);

    // 新增：格式化时间戳
    TypeKind format_params[] = {TYPE_INT, TYPE_STRING};
    native_register_module_method("times", "format", native_times_format, 2, -1, -1, TYPE_STRING, format_params);

    // 新增：获取当前日期时间数组
    native_register_module_method("times", "datetime", native_times_datetime, 0, -1, -1, TYPE_ARRAY, NULL);
}

// times 模块不需要单独的 register_meta 函数
// sleep 函数的元信息通过 vm_register_native 自动注册
