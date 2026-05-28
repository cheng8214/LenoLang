#ifndef PLATFORM_THREAD_H
#define PLATFORM_THREAD_H

// ============================================================================
// 跨平台线程抽象层
// 提供统一的线程、互斥锁、条件变量接口
// ============================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 平台相关类型定义
// ============================================================================

#ifdef _WIN32
    #include <windows.h>
    
    // Windows 线程句柄
    typedef HANDLE PlatformThread;
    typedef DWORD PlatformThreadID;
    
    // Windows 同步原语
    typedef CRITICAL_SECTION PlatformMutex;
    typedef CONDITION_VARIABLE PlatformCondVar;
    
#else
    #include <pthread.h>
    
    // POSIX 线程句柄
    typedef pthread_t PlatformThread;
    typedef pthread_t PlatformThreadID;
    
    // POSIX 同步原语
    typedef pthread_mutex_t PlatformMutex;
    typedef pthread_cond_t PlatformCondVar;
    
#endif

// ============================================================================
// 线程函数类型
// ============================================================================

typedef void* (*PlatformThreadFunc)(void* arg);

// ============================================================================
// 线程操作
// ============================================================================

// 创建线程
// 返回 0 成功，非0 失败
int platform_thread_create(PlatformThread* thread, PlatformThreadFunc func, void* arg);

// 等待线程结束
// 返回 0 成功，非0 失败
int platform_thread_join(PlatformThread thread, void** retval);

// 获取当前线程 ID
PlatformThreadID platform_thread_self(void);

// 比较两个线程 ID 是否相等
int platform_thread_equal(PlatformThreadID t1, PlatformThreadID t2);

// 让出 CPU 时间片
void platform_thread_yield(void);

// ============================================================================
// 互斥锁操作
// ============================================================================

// 初始化互斥锁
int platform_mutex_init(PlatformMutex* mutex);

// 销毁互斥锁
int platform_mutex_destroy(PlatformMutex* mutex);

// 加锁
int platform_mutex_lock(PlatformMutex* mutex);

// 尝试加锁（非阻塞）
int platform_mutex_trylock(PlatformMutex* mutex);

// 解锁
int platform_mutex_unlock(PlatformMutex* mutex);

// ============================================================================
// 条件变量操作
// ============================================================================

// 初始化条件变量
int platform_cond_init(PlatformCondVar* cond);

// 销毁条件变量
int platform_cond_destroy(PlatformCondVar* cond);

// 等待条件变量（自动解锁互斥锁，等待时阻塞）
int platform_cond_wait(PlatformCondVar* cond, PlatformMutex* mutex);

// 等待条件变量（带超时）
// timeout_ms: 超时时间（毫秒），-1 表示无限等待
int platform_cond_timedwait(PlatformCondVar* cond, PlatformMutex* mutex, int timeout_ms);

// 唤醒一个等待的线程
int platform_cond_signal(PlatformCondVar* cond);

// 唤醒所有等待的线程
int platform_cond_broadcast(PlatformCondVar* cond);

// ============================================================================
// 辅助函数
// ============================================================================

// 辅助函数

// 休眠指定毫秒（在 platform.h 中已定义，这里不重复声明）
// void platform_sleep_ms(int ms);  // 已在 platform.h 中定义

#ifdef __cplusplus
}
#endif

#endif // PLATFORM_THREAD_H
