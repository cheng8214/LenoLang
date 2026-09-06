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
