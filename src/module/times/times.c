#include "include/lenolang.h"
#include "include/native.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
//      ±15.6ms。docs/archive/性能测试总结_Leno_vs_Python.md 里成片的 15.6ms 整数倍
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

// ==================== times.format 的实现（2026-11-11 重写）====================
//
// 旧实现是"一次 strftime 渲到底"，有三个**静默错值**（都不是报错，而是给空串 / 坏字节）：
//   ① 格式串里含**非 ASCII 字面量**（文档示例 `"%Y年%m月%d日"`）⇒ 整串返回 ""。
//      MSVC 的 strftime 会校验格式串的编码，UTF-8 多字节序列在 C locale 下非法 ⇒ 返回 0。
//   ② 输出超过 256 字节 ⇒ strftime 返回 0 ⇒ 同样返回 ""（长模板静默变空）。
//   ③ `%Z`（时区名）拿到的是**本地代码页**字节（实测中文系统是 GBK），而 Leno 的 string
//      一律 UTF-8 ⇒ 字符串里出现非法 UTF-8（打印成 `�й���׼ʱ��`）。
//
// 现在：自己逐字节扫描格式串 —— 字面量原样拷贝（不动 UTF-8），只有 `%X` 才交给
// strftime 渲（单条说明符不可能超长），Windows 上再把该条结果从 ACP 转成 UTF-8；
// 输出用可增长缓冲。任何真没法渲的情况（未知说明符 / 结尾裸 `%` / 1970 年之前）都
// **明确报错**，不再静默返回空串。
typedef struct {
    char* data;
    int len;
    int cap;
} TimesFmtBuf;

static int tfb_reserve(TimesFmtBuf* b, int extra) {
    if (!b) return 0;
    int need = b->len + extra + 1;
    if (need <= b->cap) return 1;
    int ncap = b->cap > 0 ? b->cap : 64;
    while (ncap < need) ncap *= 2;
    char* nd = (char*)realloc(b->data, (size_t)ncap);
    if (!nd) return 0;
    b->data = nd;
    b->cap = ncap;
    return 1;
}

static void tfb_append(TimesFmtBuf* b, const char* s, int n) {
    if (!b || !s || n <= 0) return;
    if (!tfb_reserve(b, n)) return;
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    b->data[b->len] = '\0';
}

// strftime 的**单条说明符**产出：Windows 上是本地代码页字节（%Z / %A / %B / %p …）⇒ 转 UTF-8。
// 纯数字/ASCII 的说明符转完与原样一致（恒等），所以不必区分是哪一条。
static void tfb_append_locale(TimesFmtBuf* b, const char* s, int n) {
#ifdef _WIN32
    if (n <= 0) return;
    int wlen = MultiByteToWideChar(CP_ACP, 0, s, n, NULL, 0);
    if (wlen <= 0) { tfb_append(b, s, n); return; }        // 转不了就原样给（不丢内容）
    wchar_t* w = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wlen);
    if (!w) { tfb_append(b, s, n); return; }
    MultiByteToWideChar(CP_ACP, 0, s, n, w, wlen);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, w, wlen, NULL, 0, NULL, NULL);
    if (ulen <= 0) {
        free(w);
        tfb_append(b, s, n);
        return;
    }
    char* u8 = (char*)malloc((size_t)ulen);
    if (!u8) {
        free(w);
        tfb_append(b, s, n);
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, u8, ulen, NULL, NULL);
    tfb_append(b, u8, ulen);
    free(u8);
    free(w);
#else
    tfb_append(b, s, n);
#endif
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
        // 不再返回空串（那是"看着成功、其实是垃圾"）：说清楚是时间戳不行
        native_throw_error("times.format: 该时间戳无法转换为本地时间"
                           "（Windows 的 localtime 不支持 1970 年之前的负时间戳）");
        return val_null();
    }
    
    TimesFmtBuf out = { NULL, 0, 0 };
    if (!tfb_reserve(&out, 64)) {
        native_throw_error("times.format: 内存不足");
        return val_null();
    }
    out.data[0] = '\0';

    const char* p = fmt;
    while (*p) {
        if (*p != '%') {
            // 字面量（含 UTF-8 多字节）原样拷贝：UTF-8 的续字节都 >= 0x80，
            // 不可能等于 '%' ⇒ 逐字节扫描对 UTF-8 是安全的
            tfb_append(&out, p, 1);
            p++;
            continue;
        }
        if (p[1] == '\0') {
            free(out.data);
            native_throw_error("times.format: 格式串以 '%' 结尾（要输出百分号请写 '%%'）");
            return val_null();
        }
        if (p[1] == '%') {
            tfb_append(&out, "%", 1);
            p += 2;
            continue;
        }
        // `%Ec` / `%Ow` 这类带修饰符的也算一条（strftime 支持，别把它拆坏）
        int spec_len = ((p[1] == 'E' || p[1] == 'O') && p[2] != '\0') ? 3 : 2;
        char spec[4];
        spec[0] = '%';
        spec[1] = p[1];
        spec[2] = (spec_len == 3) ? p[2] : '\0';
        spec[3] = '\0';

        char tmp[256];
        size_t n = strftime(tmp, sizeof(tmp), spec, tm_info);
        if (n == 0) {
            char msg[128];
            snprintf(msg, sizeof(msg),
                     "times.format: 无法识别的格式说明符 '%s'（支持 C 标准 strftime 的说明符，"
                     "要输出百分号请写 '%%'）", spec);
            free(out.data);
            native_throw_error(msg);
            return val_null();
        }
        tfb_append_locale(&out, tmp, (int)n);
        p += spec_len;
    }
    
    ObjString* result = str_new(out.data ? out.data : "", out.len);
    free(out.data);
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
    vm_register_native("sleep", native_sleep, TYPE_NULL, TYPE_UNKNOWN, NATIVE_FIXED(sleep_params));
}

// 初始化 times 模块（import times 时调用）
void times_init_module(void) {
    // 注册 times.ms 方法（模块名，方法名，函数指针，参数数量，返回类型规格，参数类型数组）
    // 返回类型为 float：毫秒也要带小数（int 会把毫秒截断成 1ms 粒度，白白丢掉精度）
    native_register_module_method("times", "ms", native_times_ms, &NATIVE_T_FLOAT, NATIVE_FIXED_NONE(0));

    // 注册 times.us 方法（模块名，方法名，函数指针，参数数量，返回类型规格，参数类型数组）
    native_register_module_method("times", "us", native_times_us, &NATIVE_T_FLOAT, NATIVE_FIXED_NONE(0));

    // 注册 times.ns 方法（模块名，方法名，函数指针，参数数量，返回类型规格，参数类型数组）
    native_register_module_method("times", "ns", native_times_ns, &NATIVE_T_FLOAT, NATIVE_FIXED_NONE(0));

    // 新增：获取当前时间戳（秒）
    native_register_module_method("times", "now", native_times_now, &NATIVE_T_INT, NATIVE_FIXED_NONE(0));

    // 新增：格式化时间戳
    TypeKind format_params[] = {TYPE_INT, TYPE_STRING};
    native_register_module_method("times", "format", native_times_format, &NATIVE_T_STRING, NATIVE_FIXED(format_params));

    // 新增：获取当前日期时间数组
    native_register_module_method("times", "datetime", native_times_datetime, &NATIVE_T_ARR_INT, NATIVE_FIXED_NONE(0));
}

// times 模块不需要单独的 register_meta 函数
// sleep 函数的元信息通过 vm_register_native 自动注册
