#include "../include/platform_thread.h"
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32

// Windows 线程函数包装器，避免类型转换警告
typedef struct {
    PlatformThreadFunc func;
    void* arg;
} ThreadWrapperArgs;

static DWORD WINAPI thread_wrapper(LPVOID param) {
    ThreadWrapperArgs* args = (ThreadWrapperArgs*)param;
    PlatformThreadFunc func = args->func;
    void* arg = args->arg;
    free(args);
    return (DWORD)(uintptr_t)func(arg);
}

int platform_thread_create(PlatformThread* thread, PlatformThreadFunc func, void* arg) {
    ThreadWrapperArgs* wrapper_args = (ThreadWrapperArgs*)malloc(sizeof(ThreadWrapperArgs));
    if (wrapper_args == NULL) return -1;
    wrapper_args->func = func;
    wrapper_args->arg = arg;
    *thread = CreateThread(NULL, 0, thread_wrapper, wrapper_args, 0, NULL);
    if (*thread == NULL) {
        free(wrapper_args);
        return -1;
    }
    return 0;
}

int platform_thread_join(PlatformThread thread, void** retval) {
    (void)retval;
    DWORD result = WaitForSingleObject(thread, INFINITE);
    if (result != WAIT_OBJECT_0) return -1;
    CloseHandle(thread);
    return 0;
}

PlatformThreadID platform_thread_self(void) {
    return GetCurrentThreadId();
}

int platform_thread_equal(PlatformThreadID t1, PlatformThreadID t2) {
    return t1 == t2;
}

void platform_thread_yield(void) {
    SwitchToThread();
}

int platform_mutex_init(PlatformMutex* mutex) {
    InitializeCriticalSection(mutex);
    return 0;
}

int platform_mutex_destroy(PlatformMutex* mutex) {
    DeleteCriticalSection(mutex);
    return 0;
}

int platform_mutex_lock(PlatformMutex* mutex) {
    EnterCriticalSection(mutex);
    return 0;
}

int platform_mutex_trylock(PlatformMutex* mutex) {
    return TryEnterCriticalSection(mutex) ? 0 : -1;
}

int platform_mutex_unlock(PlatformMutex* mutex) {
    LeaveCriticalSection(mutex);
    return 0;
}

int platform_cond_init(PlatformCondVar* cond) {
    InitializeConditionVariable(cond);
    return 0;
}

int platform_cond_destroy(PlatformCondVar* cond) {
    (void)cond;
    return 0;
}

int platform_cond_wait(PlatformCondVar* cond, PlatformMutex* mutex) {
    return SleepConditionVariableCS(cond, mutex, INFINITE) ? 0 : -1;
}

int platform_cond_timedwait(PlatformCondVar* cond, PlatformMutex* mutex, int timeout_ms) {
    DWORD ms = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    return SleepConditionVariableCS(cond, mutex, ms) ? 0 : -1;
}

int platform_cond_signal(PlatformCondVar* cond) {
    WakeConditionVariable(cond);
    return 0;
}

int platform_cond_broadcast(PlatformCondVar* cond) {
    WakeAllConditionVariable(cond);
    return 0;
}

#else

int platform_thread_create(PlatformThread* thread, PlatformThreadFunc func, void* arg) {
    return pthread_create(thread, NULL, func, arg);
}

int platform_thread_join(PlatformThread thread, void** retval) {
    return pthread_join(thread, retval);
}

PlatformThreadID platform_thread_self(void) {
    return pthread_self();
}

int platform_thread_equal(PlatformThreadID t1, PlatformThreadID t2) {
    return pthread_equal(t1, t2);
}

void platform_thread_yield(void) {
    sched_yield();
}

int platform_mutex_init(PlatformMutex* mutex) {
    return pthread_mutex_init(mutex, NULL);
}

int platform_mutex_destroy(PlatformMutex* mutex) {
    return pthread_mutex_destroy(mutex);
}

int platform_mutex_lock(PlatformMutex* mutex) {
    return pthread_mutex_lock(mutex);
}

int platform_mutex_trylock(PlatformMutex* mutex) {
    return pthread_mutex_trylock(mutex);
}

int platform_mutex_unlock(PlatformMutex* mutex) {
    return pthread_mutex_unlock(mutex);
}

int platform_cond_init(PlatformCondVar* cond) {
    return pthread_cond_init(cond, NULL);
}

int platform_cond_destroy(PlatformCondVar* cond) {
    return pthread_cond_destroy(cond);
}

int platform_cond_wait(PlatformCondVar* cond, PlatformMutex* mutex) {
    return pthread_cond_wait(cond, mutex);
}

int platform_cond_timedwait(PlatformCondVar* cond, PlatformMutex* mutex, int timeout_ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(cond, mutex, &ts);
}

int platform_cond_signal(PlatformCondVar* cond) {
    return pthread_cond_signal(cond);
}

int platform_cond_broadcast(PlatformCondVar* cond) {
    return pthread_cond_broadcast(cond);
}

#endif
