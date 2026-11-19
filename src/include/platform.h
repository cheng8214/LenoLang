#ifndef PLATFORM_H
#define PLATFORM_H

// ============================================================================
// 平台抽象层 - 封装跨平台差异
// ============================================================================

#ifdef _WIN32
    #include <windows.h>
#else
    #include <sys/time.h>
    #include <unistd.h>
    #include <stdint.h>
#endif

#include <stdint.h>
#include <stddef.h>

#ifndef _WIN32
// 取当前进程可执行文件的**绝对路径**（UTF-8）。成功返回 1 并写入 out；失败返回 0（out 置空串）。
//   · Linux: /proc/self/exe     · macOS: _NSGetExecutablePath + realpath
//   · Windows: 本函数**不提供实现** —— 调用点继续直接用 GetModuleFileNameW
//     （那是已验证工作的代码，不因"新增 macOS 支持"而改动）；请只在 #ifndef _WIN32 分支里调用。
// 实现见 src/platform/platform_path.c（原先 5 处各自为政的 readlink 已收敛到这里）。
int platform_self_exe_path(char* out, size_t n);
#endif

// 获取当前时间（毫秒）
static inline uint64_t platform_current_time_ms(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;
    // 转换为毫秒（FILETIME 是 100 纳秒为单位，从 1601 年开始）
    return uli.QuadPart / 10000ULL;
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000;
#endif
}

// 休眠指定毫秒
static inline void platform_sleep_ms(uint64_t ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    usleep((useconds_t)(ms * 1000));
#endif
}

// ============================================================================
// UTF-8 <-> UTF-16 转换（跨平台实现）
// ============================================================================

#ifdef _WIN32
#include <stdlib.h>

// Windows 使用 WideCharToMultiByte/MultiByteToWideChar
static inline char* utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, NULL, 0, NULL, NULL);
    if (size_needed <= 0) return NULL;
    
    char* str = (char*)malloc(size_needed);
    if (!str) return NULL;
    
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, str, size_needed, NULL, NULL);
    return str;
}

static inline wchar_t* utf8_to_utf16(const char* str) {
    if (!str) return NULL;
    
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
    if (size_needed <= 0) return NULL;
    
    wchar_t* wstr = (wchar_t*)malloc(size_needed * sizeof(wchar_t));
    if (!wstr) return NULL;
    
    MultiByteToWideChar(CP_UTF8, 0, str, -1, wstr, size_needed);
    return wstr;
}

#elif defined(__ANDROID__)
// ---------------------------------------------------------------------------
// Android (bionic)：**不用 iconv** —— bionic 的 iconv 直到 API 28 才提供，且只支持
//   UTF-8 / UTF-16 / UTF-32(UCS-4) / US-ASCII 这几族；引 <iconv.h> 在 API < 28 上
//   直接是空声明 ⇒ `iconv_open` 未声明（C99 下是 error，实测 r27c + android21 报
//   "call to undeclared function 'iconv_open'" 共 120 处）。
// 这里用纯 C 做 UTF-8 ↔ UCS-4：Android 上 `wchar_t` 恒为 4 字节（与 Linux glibc 同），
//   语义与 iconv 的 "WCHAR_T" 一致，但不依赖 API 级别、少一次 dlopen/表查找，
//   而且**非法序列不会整体失败**（iconv 会返回 -1 让整串丢掉，这里逐字符跳过）。
// ---------------------------------------------------------------------------
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// UCS-4(wchar_t) → UTF-8：每个码点最多 4 字节，含结尾 NUL ⇒ n*4+1
static inline char* utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    size_t n = wcslen(wstr);
    char* out = (char*)malloc(n * 4 + 1);
    if (!out) return NULL;
    char* p = out;
    for (size_t i = 0; i < n; i++) {
        unsigned int cp = (unsigned int)wstr[i];
        if (cp < 0x80u) {
            *p++ = (char)cp;
        } else if (cp < 0x800u) {
            *p++ = (char)(0xC0u | (cp >> 6));
            *p++ = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
            *p++ = (char)(0xE0u | (cp >> 12));
            *p++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            *p++ = (char)(0x80u | (cp & 0x3Fu));
        } else if (cp <= 0x10FFFFu) {
            *p++ = (char)(0xF0u | (cp >> 18));
            *p++ = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            *p++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            *p++ = (char)(0x80u | (cp & 0x3Fu));
        } else {
            *p++ = '?';   // 非法码点（代理对 / 越界）⇒ 单个 '?'，绝不整串失败
        }
    }
    *p = '\0';
    return out;
}

// UTF-8 → UCS-4(wchar_t)：输出最多 n+1 个 wchar_t
static inline wchar_t* utf8_to_utf16(const char* str) {
    if (!str) return NULL;
    size_t n = strlen(str);
    wchar_t* out = (wchar_t*)malloc((n + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    const unsigned char* s = (const unsigned char*)str;
    size_t o = 0;
    while (*s) {
        unsigned int cp;
        int extra;
        if (s[0] < 0x80u)                          { cp = s[0];          extra = 0; }
        else if ((s[0] & 0xE0u) == 0xC0u)          { cp = s[0] & 0x1Fu;  extra = 1; }
        else if ((s[0] & 0xF0u) == 0xE0u)          { cp = s[0] & 0x0Fu;  extra = 2; }
        else if ((s[0] & 0xF8u) == 0xF0u)          { cp = s[0] & 0x07u;  extra = 3; }
        else { s++; continue; }                    // 非法首字节：跳过，不放弃整串
        s++;
        int ok = 1;
        for (int k = 0; k < extra; k++) {
            if ((*s & 0xC0u) != 0x80u) { ok = 0; break; }
            cp = (cp << 6) | (unsigned int)(*s & 0x3Fu);
            s++;
        }
        if (!ok) continue;                          // 截断序列：跳过
        out[o++] = (wchar_t)cp;                     // Android 的 wchar_t 是 4 字节，直接存码点
    }
    out[o] = L'\0';
    return out;
}

#else
// Linux/macOS 使用 iconv
#include <iconv.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <locale.h>

// 将 UTF-16 宽字符转换为 UTF-8 字符串
static inline char* utf16_to_utf8(const wchar_t* wstr) {
    if (!wstr) return NULL;
    
    // 获取输入长度（以字节为单位）
    size_t wlen = wcslen(wstr);
    size_t inlen = wlen * sizeof(wchar_t);
    
    // 分配输出缓冲区（UTF-8 最多 4 字节/字符）
    size_t outlen = wlen * 4 + 1;
    char* outbuf = (char*)malloc(outlen);
    if (!outbuf) return NULL;
    
    // 打开 iconv 转换描述符
    iconv_t cd = iconv_open("UTF-8", "WCHAR_T");
    if (cd == (iconv_t)-1) {
        free(outbuf);
        return NULL;
    }
    
    // 执行转换
    char* inptr = (char*)wstr;
    char* outptr = outbuf;
    size_t inleft = inlen;
    size_t outleft = outlen - 1;  // 留一个字节给 null
    
    if (iconv(cd, &inptr, &inleft, &outptr, &outleft) == (size_t)-1) {
        iconv_close(cd);
        free(outbuf);
        return NULL;
    }
    
    *outptr = '\0';  // null 终止
    iconv_close(cd);
    
    return outbuf;
}

// 将 UTF-8 字符串转换为 UTF-16 宽字符
static inline wchar_t* utf8_to_utf16(const char* str) {
    if (!str) return NULL;
    
    size_t inlen = strlen(str);
    
    // 分配输出缓冲区
    size_t outlen = (inlen + 1) * sizeof(wchar_t);
    wchar_t* outbuf = (wchar_t*)malloc(outlen);
    if (!outbuf) return NULL;
    
    // 打开 iconv 转换描述符
    iconv_t cd = iconv_open("WCHAR_T", "UTF-8");
    if (cd == (iconv_t)-1) {
        free(outbuf);
        return NULL;
    }
    
    // 执行转换
    char* inptr = (char*)str;
    char* outptr = (char*)outbuf;
    size_t inleft = inlen;
    size_t outleft = outlen - sizeof(wchar_t);  // 留空间给 null
    
    if (iconv(cd, &inptr, &inleft, &outptr, &outleft) == (size_t)-1) {
        iconv_close(cd);
        free(outbuf);
        return NULL;
    }
    
    // null 终止
    wchar_t* wptr = (wchar_t*)outptr;
    *wptr = L'\0';
    
    iconv_close(cd);
    return outbuf;
}
#endif // _WIN32

#endif // PLATFORM_H
