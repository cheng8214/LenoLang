/*
 * jit_mem.h - Executable memory management for JIT
 *
 * Windows: VirtualAlloc with PAGE_EXECUTE_READWRITE
 * Linux/Mac: mmap with PROT_READ|WRITE|EXEC
 */
#ifndef LENO_JIT_MEM_H
#define LENO_JIT_MEM_H

#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

/* Allocate executable memory. Returns NULL on failure. */
static inline void* jit_mem_alloc(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        PAGE_EXECUTE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

/* Flush the instruction cache after writing or modifying executable memory.
 *
 * Windows 的契约：生成/改写可执行代码后**必须**调用 FlushInstructionCache，
 * 然后才能执行。它同时也是序列化点，会清掉 CPU 的 icache 与解码流缓冲
 * （DSB / uop cache —— 后者按虚拟地址索引）。
 *
 * 缺了它会出「机器码内存里明明正确、但执行起来某条指令像没生效」的症状：
 * 若目标虚拟地址此前被别处执行过，DSB/icache 里可能还留着旧 uops，新写入的
 * 代码就有机会执行到陈旧指令流。表现为**与代码长度/对齐有关的、确定性的算错**
 * （本次实测：加 4 条指令后 while 循环条件恒被判成 false，去掉后恢复）。
 *
 * x86 硬件通常能靠缓存一致性自动处理同核自改代码，所以这个 bug 平时不显形；
 * 但 DSB 陈旧 uops 与跨核/预取场景下必须显式刷新，不能依赖运气。 */
static inline void jit_mem_flush(void* ptr, size_t size) {
    if (!ptr) return;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), ptr, size);
#else
    __builtin___clear_cache((char*)ptr, (char*)ptr + size);
#endif
}

/* Free executable memory. */
static inline void jit_mem_free(void* ptr, size_t size) {
    if (!ptr) return;
#ifdef _WIN32
    (void)size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

#endif /* LENO_JIT_MEM_H */
