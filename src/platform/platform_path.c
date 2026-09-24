// ============================================================================
// 自身可执行文件路径（POSIX 侧实现）
// ----------------------------------------------------------------------------
// 为什么单独成一个文件：取"当前进程 exe 的路径"这件事，仓库里有 5 个消费者
//   · src/vm_main.c                    解包内嵌资源段（要 exe 路径 + 所在目录）
//   · src/main.c                       打包时定位 leno_vm 运行时
//   · src/package/package_install.c    内置模块搜索路径 exe_dir/leno_module/
//   · src/serialize/serialize.c        字节码缓存的 key（按自身 exe 失效）
//   · src/module/ffi/ffi.c             DLL 搜索链的最后一站（exe 目录）
// 此前每个地方各写一份 readlink("/proc/self/exe")，结果就是**各自踩各自的坑** ——
// 最典型的一例：readlink **不写 '\0'**，其中一处漏了补 NUL ⇒ 路径尾部是栈垃圾
// ⇒ 后续 fopen 必然失败（表现为"检测不到内嵌数据"，单文件 exe 直接退化成命令行工具）。
// 收敛到这一处，跨平台差异只在这里处理。
//
// Windows：**本文件不提供实现** —— 各调用点继续直接用 GetModuleFileNameW。
//   那是已验证工作的代码，不因"新增 macOS 支持"去改它（项目约定）。
//   调用方请在 #ifndef _WIN32 分支里调用本函数（声明见 include/platform.h）。
// ============================================================================
#ifndef _WIN32

#include "include/platform.h"
#include "include/leno_types.h"   // MAX_PATH_LEN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>          // _NSGetExecutablePath：macOS 没有 /proc/self/exe
#endif

int platform_self_exe_path(char* out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = '\0';

#ifdef __APPLE__
    // macOS：dyld 的 _NSGetExecutablePath。
    // 它可能返回相对路径、或含符号链接的路径 ⇒ 用 realpath 规范化
    // （realpath 失败就用原值，总比取不到强）。
    char raw[MAX_PATH_LEN];
    uint32_t sz = (uint32_t)sizeof(raw);
    if (_NSGetExecutablePath(raw, &sz) != 0) return 0;   // 缓冲区不够/取不到
    char resolved[MAX_PATH_LEN];
    if (realpath(raw, resolved)) {
        snprintf(out, n, "%s", resolved);
    } else {
        snprintf(out, n, "%s", raw);
    }
    return out[0] != '\0';
#else
    // Linux：/proc/self/exe。
    // ⚠ readlink **不写结束符** ⇒ 必须自己补 '\0'；且必须用 n-1 限制长度，
    //   否则缓冲区尾部的垃圾会被当成路径的一部分（后续 fopen 必失败）。
    // 取不到就留空串并返回 0，让调用方走"拿不到自身路径"的兜底分支（fail-closed）。
    ssize_t rl = readlink("/proc/self/exe", out, n - 1);
    if (rl <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[rl] = '\0';
    return 1;
#endif
}

#endif // !_WIN32
