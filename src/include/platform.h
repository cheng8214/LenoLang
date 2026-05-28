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
