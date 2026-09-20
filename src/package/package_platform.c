#include "../include/leno_package.h"
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

PlatformType package_get_platform(void) {
#ifdef _WIN32
    /* 检测是否是 64 位 */
#if defined(_WIN64) || defined(__x86_64__) || defined(__amd64__) || defined(_M_AMD64)
    return PLATFORM_WINDOWS_X64;
#elif defined(_M_IX86) || defined(__i386__)
    return PLATFORM_WINDOWS_X86;
#else
    /* 运行时检测 */
    SYSTEM_INFO si;
    GetNativeSystemInfo(&si);
    if (si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64 ||
        si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64) {
#ifdef _M_ARM64
        return PLATFORM_WINDOWS_X64;  /* ARM64 Windows 暂时归类为 x64 兼容 */
#else
        return PLATFORM_WINDOWS_X64;
#endif
    }
    return PLATFORM_WINDOWS_X86;
#endif

#elif defined(__APPLE__)
#if defined(__arm64__) || defined(__aarch64__)
    return PLATFORM_MACOS_ARM64;
#else
    return PLATFORM_MACOS_X64;
#endif

#elif defined(__linux__)
#if defined(__arm64__) || defined(__aarch64__)
    return PLATFORM_LINUX_ARM64;
#else
    return PLATFORM_LINUX_X64;
#endif

#else
    return PLATFORM_UNKNOWN;
#endif
}

const char* package_platform_name(PlatformType platform) {
    switch (platform) {
        case PLATFORM_WINDOWS_X64:  return "windows-x64";
        case PLATFORM_WINDOWS_X86:  return "windows-x86";
        case PLATFORM_LINUX_X64:    return "linux-x64";
        case PLATFORM_LINUX_ARM64:  return "linux-arm64";
        case PLATFORM_MACOS_X64:    return "macos-x64";
        case PLATFORM_MACOS_ARM64:  return "macos-arm64";
        default:                    return "unknown";
    }
}

const char* package_platform_dylib_ext(void) {
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}
