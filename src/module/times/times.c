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
//      ±15.6ms。LenoDocsSrc/site/archive/性能测试总结_Leno_vs_Python.html 里成片的 15.6ms 整数倍
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

// 把 struct tm 按格式串渲染成字符串（format 与 format_utc 共用，2026-11-24 抽出）。
//   api_name 只用于报错前缀（"times.format" / "times.format_utc"）——
//   两份实现各自拼一遍格式串扫描是"同一件事两处写"，而这里的报错文案恰恰是最容易漂的部分。
static Value times_format_tm(struct tm* tm_info, const char* fmt, const char* api_name) {
    TimesFmtBuf out = { NULL, 0, 0 };
    if (!tfb_reserve(&out, 64)) {
        native_throw_error("times 格式化: 内存不足");
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
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "%s: 格式串以 '%%' 结尾（要输出百分号请写 '%%%%'）", api_name);
            free(out.data);
            native_throw_error(msg);
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
            char msg[224];
            snprintf(msg, sizeof(msg),
                     "%s: 无法识别的格式说明符 '%s'（支持 C 标准 strftime 的说明符，"
                     "要输出百分号请写 '%%'）", api_name, spec);
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

// 取出参数里的时间戳（int / bigint 都收）
static int64_t times_arg_timestamp(Value v) {
    return val_is_bigint(v) ? bigint_to_int64(val_as_bigint(v)) : (int64_t)val_as_num(v);
}

// 格式化时间戳（**本地时区**）
static Value native_times_format(int argCount, Value* args) {
    (void)argCount;

    int64_t timestamp = times_arg_timestamp(args[0]);
    ObjString* fmt_str = (ObjString*)val_as_obj(args[1]);

    time_t t = (time_t)timestamp;
    struct tm* tm_info;

#ifdef _WIN32
    tm_info = localtime(&t);
#else
    struct tm tm_buf;
    tm_info = localtime_r(&t, &tm_buf);
#endif

    if (!tm_info) {
        // 不再返回空串（那是"看着成功、其实是垃圾"）：说清楚是时间戳不行。
        // ⚠ 2026-11-15：原来只写"不支持 1970 年之前的**负**时间戳"，可本分支同样会接住
        //   **过大**的正数（实测 `times.format(9223372036854775808, "%Y")` 与 int48 上限
        //   140737488355327 都落到这里）—— 那种情况报"负时间戳"只会把人带偏 ✗
        native_throw_error("times.format: 该时间戳无法转换为本地时间"
                           "（可能超出本平台 localtime 的范围：Windows 上只支持 1970 年之后、"
                           "约 3001 年之前；1970 年之前的负时间戳也不支持）");
        return val_null();
    }
    return times_format_tm(tm_info, fmt_str->chars, "times.format");
}

// 格式化时间戳（**UTC**，与 format 对称；2026-11-24 新增）
static Value native_times_format_utc(int argCount, Value* args) {
    (void)argCount;

    int64_t timestamp = times_arg_timestamp(args[0]);
    ObjString* fmt_str = (ObjString*)val_as_obj(args[1]);

    time_t t = (time_t)timestamp;
    struct tm* tm_info;

#ifdef _WIN32
    tm_info = gmtime(&t);
#else
    struct tm tm_buf;
    tm_info = gmtime_r(&t, &tm_buf);
#endif

    if (!tm_info) {
        native_throw_error("times.format_utc: 该时间戳无法转换为 UTC 时间"
                           "（Windows 上的 gmtime 只支持 1970 年之后、约 3001 年之前）");
        return val_null();
    }
    return times_format_tm(tm_info, fmt_str->chars, "times.format_utc");
}

// 辅助函数：向数组添加元素
static void arr_push_value(ObjArray* arr, Value value) {
    if (arr->count >= arr->capacity) {
        arr_grow(arr);
    }
    arr->elements[arr->count++] = value;
}

// 拆时间戳 → [年,月,日,时,分,秒]（**本地时区**）
//   ★ 2026-11-24：加了**可选参数**。原来不收参数、永远拆"现在" ⇒ 手上已有时间戳时
//   只能先 format 再切字符串（或者干脆不拆）。现在：
//     datetime()      = 拆现在（老行为，兼容）
//     datetime(ts)    = 拆任意时间戳
//   没有另起 `parts(ts)` 这个别名 —— 同一能力两个名字是这类库最容易长出来的重复。
static Value native_times_datetime(int argCount, Value* args) {
    time_t t = (argCount >= 1) ? (time_t)times_arg_timestamp(args[0]) : time(NULL);

    struct tm* tm_info;
#ifdef _WIN32
    tm_info = localtime(&t);
#else
    struct tm tm_buf;
    tm_info = localtime_r(&t, &tm_buf);
#endif

    if (!tm_info) {
        if (argCount >= 1) {
            native_throw_error("times.datetime: 该时间戳无法转换为本地时间"
                               "（Windows 上 localtime 只支持 1970 年之后、约 3001 年之前）");
        }
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

// ==================== 时间戳 ↔ 结构体（2026-11-24 补全闭环） ====================
// 之前这组能力是"半个闭环"：能格式化成串、能拆"现在"，但
//   · 不能把字符串**解析**回时间戳（做 TTL / 过期判断只能拿字符串比大小）；
//   · 不能把部件**合成**时间戳（datetime 的反向没有）；
//   · 不能拆**任意**时间戳（datetime 连参数都不收）；
//   · 没有 UTC 侧（format 写死 localtime）。
// 现在补齐：parse <-> format、make <-> datetime、utc_offset / format_utc。
//
// ⚠ 时区口径（先把边界说死，免得被当成"完整时区库"）：
//   只支持 **本地时区** 与 **UTC** 两种 —— 这是 C 标准库（localtime/gmtime）的边界。
//   不支持 IANA 时区名（`Asia/Shanghai` ↔ `America/New_York`）：那要内置 tzdata，
//   或 Windows/POSIX 两套完全不同的 API ⇒ 成本高一个数量级。
//   要"按某个固定偏移算"，自己加减秒（utc_offset 就是为这个给的）。

// 公历日期 → UTC 秒（自带实现，不依赖 `timegm`）。
//   为什么不调 `timegm` / `_mkgmtime`：`-std=c99` 下 glibc **不声明** timegm
//   （`__STRICT_ANSI__` 会把它藏起来），Windows 又没有同名函数 ⇒ 两边都要分支，
//   而这段算法是纯整数运算，反而没有平台差异。算法：Howard Hinnant 的 days_from_civil。
static time_t times_timegm(const struct tm* tm_in) {
    int y = tm_in->tm_year + 1900;
    int m = tm_in->tm_mon + 1;
    int d = tm_in->tm_mday;
    y -= (m <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = (long long)era * 146097 + (long long)doe - 719468;
    return (time_t)(days * 86400LL + (long long)tm_in->tm_hour * 3600LL +
                    (long long)tm_in->tm_min * 60LL + (long long)tm_in->tm_sec);
}

// 校验 → mktime → **反查比对**。
//   为什么必须反查：mktime 会把不存在的日期**自动滚动**（2 月 30 日 ⇒ 3 月 2 日），
//   那正是本项目最忌讳的静默错值（用户写错日期，程序照跑、只是时间不对）。
//   把结果转回本地时间再比一遍，不一致就抛错并说明"这个时间不存在"。
//   返回 1 成功；0 失败（err 里是可读原因）。
static int times_build_epoch(int year, int mon, int mday, int hour, int minute, int sec,
                            time_t* out, char* err, size_t errcap) {
    if (year < 1970) {
        snprintf(err, errcap, "年份 %d 不在支持范围内（Windows 只支持 1970 年之后）", year);
        return 0;
    }
    if (mon < 1 || mon > 12) {
        snprintf(err, errcap, "月份 %d 不合法（1-12）", mon);
        return 0;
    }
    if (mday < 1 || mday > 31) {
        snprintf(err, errcap, "日期 %d 不合法（1-31）", mday);
        return 0;
    }
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || sec < 0 || sec > 59) {
        snprintf(err, errcap, "时间 %02d:%02d:%02d 不合法（时 0-23、分/秒 0-59）",
                 hour, minute, sec);
        return 0;
    }

    struct tm tm_in;
    memset(&tm_in, 0, sizeof(tm_in));
    tm_in.tm_year = year - 1900;
    tm_in.tm_mon = mon - 1;
    tm_in.tm_mday = mday;
    tm_in.tm_hour = hour;
    tm_in.tm_min = minute;
    tm_in.tm_sec = sec;
    tm_in.tm_isdst = -1;                    // 让系统按该时刻判断夏令时

    time_t t = mktime(&tm_in);
    if (t == (time_t)-1) {
        snprintf(err, errcap, "该时间无法转换为时间戳（超出平台 mktime 的范围）");
        return 0;
    }

    struct tm back;
#ifdef _WIN32
    struct tm* bp = localtime(&t);
    if (!bp) {
        snprintf(err, errcap, "无法回查该时间（超出平台 localtime 范围）");
        return 0;
    }
    back = *bp;
#else
    if (!localtime_r(&t, &back)) {
        snprintf(err, errcap, "无法回查该时间（超出平台 localtime 范围）");
        return 0;
    }
#endif

    // 只比**日期与时分秒**：DST 跳变的空档（如某天 02:30 不存在）也会在这里被抓到，
    //   那确实是"不存在的本地时间"，报出来比默默滚到 03:30 好。
    if (back.tm_year + 1900 != year || back.tm_mon + 1 != mon || back.tm_mday != mday ||
        back.tm_hour != hour || back.tm_min != minute || back.tm_sec != sec) {
        snprintf(err, errcap,
                 "%04d-%02d-%02d %02d:%02d:%02d 不是有效的本地时间"
                 "（日期不存在，或落在夏令时跳变的空档里）",
                 year, mon, mday, hour, minute, sec);
        return 0;
    }

    *out = t;
    return 1;
}

// 从 text 当前位置读一个十进制数字串（[min_digits, max_digits] 位）
static int times_read_digits(const char** p, int min_digits, int max_digits, int* out) {
    const char* s = *p;
    int n = 0;
    long v = 0;
    while (n < max_digits && s[n] >= '0' && s[n] <= '9') {
        v = v * 10 + (s[n] - '0');
        n++;
    }
    if (n < min_digits) return 0;
    *out = (int)v;
    *p = s + n;
    return 1;
}

// times.parse(text, fmt) —— 字符串 → 时间戳（按**本地时区**解释，与 format 对称）
//
// 支持的说明符是**明确子集**：%Y %m %d %H %M %S、%%、以及字面量（含中文）。
// 为什么不直接调 strptime：**Windows 上没有 strptime**（MSVC 至今不提供），
//   自带一份"平台不同、支持范围也不同"的实现只会变成新的静默差异源。
//   这里只认能可靠跨平台解析的那几个，别的**明确报错并列出支持范围**。
// ⚠ 不对称提醒：format 支持全部 strftime 说明符，parse 只支持上面这些。
//   `format(parse(s, fmt), fmt)` 只在支持集内承诺往返。
static Value native_times_parse(int argCount, Value* args) {
    (void)argCount;
    ObjString* text_obj = (ObjString*)val_as_obj(args[0]);
    ObjString* fmt_obj = (ObjString*)val_as_obj(args[1]);
    const char* text = text_obj->chars;
    const char* fmt = fmt_obj->chars;

    int year = 1970, mon = 1, mday = 1, hour = 0, minute = 0, sec = 0;
    int seen_year = 0;

    const char* p = fmt;
    const char* q = text;
    while (*p) {
        if (*p != '%') {
            if (*q != *p) {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "times.parse: 第 %d 个字符对不上 —— 格式串要求 '%c'，实际是 '%c'"
                         "（文本: \"%s\"）",
                         (int)(q - text) + 1, *p, *q ? *q : ' ', text);
                native_throw_error(msg);
                return val_null();
            }
            p++;
            q++;
            continue;
        }
        if (p[1] == '\0') {
            native_throw_error("times.parse: 格式串以 '%' 结尾（要匹配百分号请写 '%%'）");
            return val_null();
        }
        if (p[1] == '%') {
            if (*q != '%') {
                char msg[256];
                snprintf(msg, sizeof(msg),
                         "times.parse: 第 %d 个字符应为 '%%'，实际是 '%c'（文本: \"%s\"）",
                         (int)(q - text) + 1, *q ? *q : ' ', text);
                native_throw_error(msg);
                return val_null();
            }
            p += 2;
            q++;
            continue;
        }

        char spec = p[1];
        int ok = 0;
        switch (spec) {
        case 'Y': ok = times_read_digits(&q, 1, 4, &year); seen_year = 1; break;
        case 'm': ok = times_read_digits(&q, 1, 2, &mon);   break;
        case 'd': ok = times_read_digits(&q, 1, 2, &mday);  break;
        case 'H': ok = times_read_digits(&q, 1, 2, &hour);  break;
        case 'M': ok = times_read_digits(&q, 1, 2, &minute); break;
        case 'S': ok = times_read_digits(&q, 1, 2, &sec);   break;
        default: {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "times.parse: 不支持的格式说明符 '%%%c' —— 只支持 %%Y %%m %%d %%H %%M %%S "
                     "（以及 %%%% 和字面量）。%s",
                     spec, spec == 'y' ? "两位年份请改用 %Y（四位）" : "");
            native_throw_error(msg);
            return val_null();
        }
        }
        if (!ok) {
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "times.parse: 在文本第 %d 个字符处读不到 '%%%c' 要求的数字（文本: \"%s\"）",
                     (int)(q - text) + 1, spec, text);
            native_throw_error(msg);
            return val_null();
        }
        p += 2;
    }

    if (*q != '\0') {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "times.parse: 格式串没有覆盖整个文本 —— 还剩 \"%s\"（格式串少给了说明符？）", q);
        native_throw_error(msg);
        return val_null();
    }
    if (!seen_year) {
        native_throw_error("times.parse: 格式串里没有 %Y，算不出完整日期（只有月/日不够）");
        return val_null();
    }

    time_t t = 0;
    char err[192];
    if (!times_build_epoch(year, mon, mday, hour, minute, sec, &t, err, sizeof(err))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "times.parse: %s", err);
        native_throw_error(msg);
        return val_null();
    }
    return val_int_safe((int64_t)t);
}

// times.make(year, mon, day[, hour, min, sec]) —— 部件 → 时间戳（本地时区）
//   后三个参数可省略（默认 0）—— "只要日期"是最常见的用法。
static Value native_times_make(int argCount, Value* args) {
    int64_t v[6] = {1970, 1, 1, 0, 0, 0};
    for (int i = 0; i < argCount && i < 6; i++) {
        v[i] = times_arg_timestamp(args[i]);
    }
    time_t t = 0;
    char err[192];
    if (!times_build_epoch((int)v[0], (int)v[1], (int)v[2], (int)v[3], (int)v[4], (int)v[5],
                           &t, err, sizeof(err))) {
        char msg[256];
        snprintf(msg, sizeof(msg), "times.make: %s", err);
        native_throw_error(msg);
        return val_null();
    }
    return val_int_safe((int64_t)t);
}

// times.utc_offset(ts) —— 该时刻本地时区相对 UTC 的偏移（**秒**；东八区 = 28800）
//   实现：把本地拆出来的 tm 按"UTC"重新解释，与真实时间戳的差就是偏移。
//   夏令时下同一地区不同日期的偏移不同 ⇒ 必须传时间戳，不能做成无参常量。
static Value native_times_utc_offset(int argCount, Value* args) {
    (void)argCount;
    time_t t = (time_t)times_arg_timestamp(args[0]);

    struct tm lt;
#ifdef _WIN32
    struct tm* lp = localtime(&t);
    if (!lp) {
        native_throw_error("times.utc_offset: 该时间戳无法转换为本地时间");
        return val_null();
    }
    lt = *lp;
#else
    if (!localtime_r(&t, &lt)) {
        native_throw_error("times.utc_offset: 该时间戳无法转换为本地时间");
        return val_null();
    }
#endif
    lt.tm_isdst = 0;                        // 按"不带夏令时"的 UTC 解释
    time_t as_utc = times_timegm(&lt);
    // 东八区：本地钟面比 UTC 大 8 小时 ⇒ as_utc = t + 28800 ⇒ 偏移 = as_utc - t
    return val_int_safe((int64_t)(as_utc - t));
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

    // 格式化时间戳（**本地时区**）
    TypeKind format_params[] = {TYPE_INT, TYPE_STRING};
    native_register_module_method("times", "format", native_times_format, &NATIVE_T_STRING, NATIVE_FIXED(format_params));

    // 格式化时间戳（**UTC**）—— 与 format 同一个渲染器，只换 gmtime（2026-11-24）
    native_register_module_method("times", "format_utc", native_times_format_utc, &NATIVE_T_STRING,
                                  NATIVE_FIXED(format_params));

    // 拆时间戳：datetime() = 现在，datetime(ts) = 任意时间戳（**本地时区**）
    //   ★ 2026-11-24 之前是 FIXED_NONE(0)，只能拆"现在"（见函数头注释）
    native_register_module_method("times", "datetime", native_times_datetime, &NATIVE_T_ARR_INT,
                                  NATIVE_VARARG(0, 1, 0, NULL, TYPE_INT));

    // 字符串 → 时间戳（按本地时区解释；支持的说明符是**明确子集**，见函数头）
    TypeKind parse_params[] = {TYPE_STRING, TYPE_STRING};
    native_register_module_method("times", "parse", native_times_parse, &NATIVE_T_INT,
                                  NATIVE_FIXED(parse_params));

    // 部件 → 时间戳（本地时区）：make(y, m, d[, h, mi, s])，后三个可省略
    TypeKind make_params[] = {TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT, TYPE_INT};
    native_register_module_method("times", "make", native_times_make, &NATIVE_T_INT,
                                  NATIVE_VARARG(3, 6, 6, make_params, TYPE_INT));

    // 本地相对 UTC 的偏移（秒；东八区 = 28800）
    TypeKind int_param[] = {TYPE_INT};
    native_register_module_method("times", "utc_offset", native_times_utc_offset, &NATIVE_T_INT,
                                  NATIVE_FIXED(int_param));
}

// times 模块不需要单独的 register_meta 函数
// sleep 函数的元信息通过 vm_register_native 自动注册
