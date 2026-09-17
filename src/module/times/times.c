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

// ============================================================================
// 高精度单调计时：ms() / us() / ns() 三者共用同一个时钟源
//
// 历史问题（2026-09-16 修）：
//   1) ms() 走 GetTickCount64 —— 那是分辨率 ≈15.625ms 的"刻度计数器"，且返回 int；
//      于是"1000 万次函数调用 16ms"这种测量其实只有 1 个刻度的分辨率，单次误差可达
//      ±15.6ms。docs/性能测试总结_Leno_vs_Python.md 里成片的 15.6ms 整数倍
//      （16/31/47/62/78/94/109/125/140…）就是这么来的。
//   2) QPC 取不到时，us()/ns() 用 GetTickCount()*1000 / *1000000 冒充——
//      分辨率仍是 15.6ms（还有 32 位 49.7 天回绕），只是看起来精确，属"静默给假精度"。
//
// 现在：三者统一走 QPC（Windows）/ CLOCK_MONOTONIC（POSIX），只是换算单位不同。
// **精度由时钟源决定，不由单位决定** —— 所以 ms() 换算成 float 后精度与 us 版一致。
// ms() 返回值类型随之由 int 改为 float（int 会把毫秒截断成 1ms 粒度）。
//
// 语义：ms/us/ns 都是**单调计时**（用于取差值），不是挂钟时间；挂钟走 now()/format()。
// 取不到时钟时返回 0：宁可给 0，也不再拿 15.6ms 粒度的计数器冒充高精度。
// ============================================================================

// 取单调计数器的原始计数与频率；成功返回 1
static int times_monotonic_counter(long long* counter, long long* freq) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    if (!QueryPerformanceFrequency(&f) || !QueryPerformanceCounter(&c)) return 0;
    *freq = (long long)f.QuadPart;
    *counter = (long long)c.QuadPart;
    return 1;
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    *freq = 1000000000LL;   // 纳秒分辨率
    *counter = (long long)ts.tv_sec * 1000000000LL + (long long)ts.tv_nsec;
    return 1;
#endif
}

// 获取当前时间（毫秒，float：带小数，分辨率与 us()/ns() 相同，只是单位不同）
static Value native_times_ms(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    long long counter = 0, freq = 0;
    if (!times_monotonic_counter(&counter, &freq) || freq <= 0) return val_num(0.0);
    return val_num((double)counter * 1000.0 / (double)freq);
}

// 获取当前时间（微秒）
static Value native_times_us(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    long long counter = 0, freq = 0;
    if (!times_monotonic_counter(&counter, &freq) || freq <= 0) return val_num(0.0);
    return val_num((double)counter * 1000000.0 / (double)freq);
}

// 获取当前时间（纳秒）
static Value native_times_ns(int argCount, Value* args) {
    (void)argCount;
    (void)args;

    long long counter = 0, freq = 0;
    if (!times_monotonic_counter(&counter, &freq) || freq <= 0) return val_num(0.0);
    return val_num((double)counter * 1000000000.0 / (double)freq);
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
    vm_register_native("sleep", native_sleep, 1, -1, -1, TYPE_NULL, TYPE_UNKNOWN, sleep_params);
}

// 初始化 times 模块（import times 时调用）
void times_init_module(void) {
    // 注册 times.ms 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    // 返回类型为 float：毫秒也要带小数（int 会把毫秒截断成 1ms 粒度，白白丢掉精度）
    native_register_module_method("times", "ms", native_times_ms, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, NULL);

    // 注册 times.us 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    native_register_module_method("times", "us", native_times_us, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, NULL);

    // 注册 times.ns 方法（模块名，方法名，函数指针，参数数量，返回类型，参数类型数组）
    native_register_module_method("times", "ns", native_times_ns, 0, -1, -1, TYPE_FLOAT, TYPE_UNKNOWN, NULL);

    // 新增：获取当前时间戳（秒）
    native_register_module_method("times", "now", native_times_now, 0, -1, -1, TYPE_INT, TYPE_UNKNOWN, NULL);

    // 新增：格式化时间戳
    TypeKind format_params[] = {TYPE_INT, TYPE_STRING};
    native_register_module_method("times", "format", native_times_format, 2, -1, -1, TYPE_STRING, TYPE_UNKNOWN, format_params);

    // 新增：获取当前日期时间数组
    native_register_module_method("times", "datetime", native_times_datetime, 0, -1, -1, TYPE_ARRAY, TYPE_INT, NULL);
}

// times 模块不需要单独的 register_meta 函数
// sleep 函数的元信息通过 vm_register_native 自动注册
