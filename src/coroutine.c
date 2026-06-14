#include "include/lenolang.h"
#include "include/leno_vm.h"
#include "include/leno_value.h"
#include "include/leno_ast.h"
#include "include/platform.h"

// 直接使用全局 VM（主程序效率第一）
extern VM vm;

// ============================================================================
// 全局事件循环
// ============================================================================

EventLoop* g_event_loop = NULL;

// ============================================================================
// 时间相关函数
// ============================================================================

// 获取当前时间（毫秒）
uint64_t current_time_ms(void) {
    return platform_current_time_ms();
}

// 休眠指定毫秒
void sleep_ms(uint64_t ms) {
    platform_sleep_ms(ms);
}

// ============================================================================
// Future 操作
// ============================================================================

// 创建 Future 对象
ObjFuture* future_new(void) {
    ObjFuture* future = (ObjFuture*)gc_alloc(sizeof(ObjFuture), OBJ_FUTURE);
    future->completed = 0;
    future->result = val_null();
    future->error = val_null();
    future->waiter = NULL;
    return future;
}

// 完成 Future 并设置结果
void future_complete(ObjFuture* future, Value result) {
    if (future->completed) return;
    
    future->completed = 1;
    future->result = result;
    
    // 如果有协程在等待，将其加入就绪队列
    // 注意：不清除 waiter->waiting_for，让协程恢复时自己处理
    if (future->waiter && g_event_loop) {
        future->waiter->state = COROUTINE_RUNNING;
        event_loop_add_ready(g_event_loop, future->waiter);
        future->waiter = NULL;
    }
}

// 完成 Future 并设置错误
void future_fail(ObjFuture* future, Value error) {
    if (future->completed) return;
    
    future->completed = 1;
    future->error = error;
    
    // 如果有协程在等待，将其加入就绪队列
    // 注意：不清除 waiter->waiting_for，让 vm_run_coroutine 检查错误
    if (future->waiter && g_event_loop) {
        future->waiter->state = COROUTINE_RUNNING;
        event_loop_add_ready(g_event_loop, future->waiter);
        // 标记错误已传播给等待者（通过 task_future 关联的协程）
        // 遍历就绪队列查找关联此 future 的协程，设置 error_propagated
        // 注意：这里我们无法直接获取 task_future 对应的协程
        // 改为在 event_loop_run 中检查
        future->waiter = NULL;
    }
}

// ============================================================================
// 协程操作
// ============================================================================

// 创建协程对象
ObjCoroutine* coroutine_new(ObjClosure* closure) {
    ObjCoroutine* co = (ObjCoroutine*)gc_alloc(sizeof(ObjCoroutine), OBJ_COROUTINE);
    co->state = COROUTINE_NEW;
    co->saved_frame = NULL;
    co->saved_ip = NULL;
    co->saved_stack_base = 0;
    co->saved_frame_cnt = 0;  // 初始化为0，首次执行时会设置
    co->saved_frame_copy = NULL;  // 初始没有 frame 副本
    co->has_saved_frame = 0;  // 初始没有保存 frame 副本
    co->result = val_null();
    co->closure = closure;
    co->await_count = 0;
    co->waiting_for = NULL;
    co->task_future = NULL;  // task 返回的 Future，由 OP_ASYNC_CALL 设置
    co->error_propagated = 0;  // 初始未传播
    co->next = NULL;
    co->initial_args = NULL;
    co->initial_arg_count = 0;
    return co;
}

// ============================================================================
// 事件循环
// ============================================================================

// 初始化事件循环
void event_loop_init(EventLoop* loop) {
    loop->timers = (Timer*)malloc(sizeof(Timer) * MAX_TIMERS);
    loop->timer_count = 0;
    loop->timer_capacity = MAX_TIMERS;
    
    loop->ready_queue = (ObjCoroutine**)malloc(sizeof(ObjCoroutine*) * MAX_COROUTINES);
    loop->ready_count = 0;
    loop->ready_capacity = MAX_COROUTINES;
    
    loop->running = 0;
    
    // 初始化错误收集
    loop->errors = (Value*)malloc(sizeof(Value) * 16);
    loop->error_count = 0;
    loop->error_capacity = 16;
}

// 添加协程到就绪队列
void event_loop_add_ready(EventLoop* loop, ObjCoroutine* coroutine) {
    // 不要添加已完成的协程
    if (coroutine->state == COROUTINE_COMPLETED || coroutine->state == COROUTINE_FAILED) {
        return;
    }
    
    // 检查是否已在队列中（避免重复添加）
    for (int i = 0; i < loop->ready_count; i++) {
        if (loop->ready_queue[i] == coroutine) {
            return;  // 已在队列中
        }
    }
    
    if (loop->ready_count >= loop->ready_capacity) {
        // 就绪队列已满，扩展容量
        int new_capacity = loop->ready_capacity * 2;
        loop->ready_queue = (ObjCoroutine**)realloc(loop->ready_queue, 
                                                     sizeof(ObjCoroutine*) * new_capacity);
        loop->ready_capacity = new_capacity;
    }
    loop->ready_queue[loop->ready_count++] = coroutine;
}

// 从就绪队列取出一个协程
static ObjCoroutine* event_loop_dequeue_ready(EventLoop* loop) {
    if (loop->ready_count == 0) return NULL;
    
    ObjCoroutine* co = loop->ready_queue[0];
    // 移动队列
    for (int i = 0; i < loop->ready_count - 1; i++) {
        loop->ready_queue[i] = loop->ready_queue[i + 1];
    }
    loop->ready_count--;
    return co;
}

// 添加定时器
void event_loop_add_timer(EventLoop* loop, uint64_t wake_time, ObjCoroutine* coroutine) {
    if (loop->timer_count >= loop->timer_capacity) {
        // 定时器队列已满，扩展容量
        int new_capacity = loop->timer_capacity * 2;
        loop->timers = (Timer*)realloc(loop->timers, sizeof(Timer) * new_capacity);
        loop->timer_capacity = new_capacity;
    }
    
    // 插入定时器（保持按 wake_time 排序）
    int i = loop->timer_count;
    while (i > 0 && loop->timers[i - 1].wake_time > wake_time) {
        loop->timers[i] = loop->timers[i - 1];
        i--;
    }
    
    loop->timers[i].wake_time = wake_time;
    loop->timers[i].coroutine = coroutine;
    loop->timer_count++;
}

// 检查并处理到期的定时器
static void event_loop_check_timers(EventLoop* loop) {
    uint64_t now = current_time_ms();
    
    while (loop->timer_count > 0 && loop->timers[0].wake_time <= now) {
        ObjCoroutine* co = loop->timers[0].coroutine;
        
        // 移动定时器队列
        for (int i = 0; i < loop->timer_count - 1; i++) {
            loop->timers[i] = loop->timers[i + 1];
        }
        loop->timer_count--;
        
        // 完成 Future（但不要改变协程状态或加入就绪队列）
        // vm_run_coroutine 会处理 SUSPENDED 状态的协程
        if (co->waiting_for && !co->waiting_for->completed) {
            co->waiting_for->completed = 1;
            co->waiting_for->result = val_null();
        }
        
        // 将协程加入就绪队列（保持 SUSPENDED 状态，让 vm_run_coroutine 恢复）
        event_loop_add_ready(loop, co);
    }
}

// 获取最近的定时器时间（如果没有返回 UINT64_MAX）
static uint64_t event_loop_next_timer_time(EventLoop* loop) {
    if (loop->timer_count == 0) return UINT64_MAX;
    return loop->timers[0].wake_time;
}

// 停止事件循环
void event_loop_stop(EventLoop* loop) {
    loop->running = 0;
}

// 外部声明：执行一个协程直到它挂起或完成
extern int vm_run_coroutine(ObjCoroutine* co);

// 运行事件循环直到所有协程完成
void event_loop_run(EventLoop* loop) {
    loop->running = 1;
    // 重置错误收集
    loop->error_count = 0;
    
    while (loop->running) {
        // 1. 检查并处理到期的定时器
        event_loop_check_timers(loop);
        
        // 2. 执行就绪队列中的协程
        if (loop->ready_count > 0) {
            ObjCoroutine* co = event_loop_dequeue_ready(loop);
            if (co) {
                // 如果协程是因为 yield 而挂起的（waiting_for 未完成且 waiter 是自己），完成它
                // 直接设置完成，不调用 future_complete（避免重复加入就绪队列）
                if (co->waiting_for && !co->waiting_for->completed && co->waiting_for->waiter == co) {
                    co->waiting_for->completed = 1;
                    co->waiting_for->result = val_null();
                }
                extern THREAD_LOCAL VM* current_exec_vm;
                VM* target_vm = current_exec_vm ? current_exec_vm : &vm;
                target_vm->current_coroutine = co;
                vm_run_coroutine(co);
                target_vm->current_coroutine = NULL;
                
                // 收集失败的协程错误
                // 只收集未被 await 传播的错误
                if (co->state == COROUTINE_FAILED && !co->error_propagated) {
                    if (loop->error_count >= loop->error_capacity) {
                        loop->error_capacity *= 2;
                        loop->errors = (Value*)realloc(loop->errors, sizeof(Value) * loop->error_capacity);
                    }
                    loop->errors[loop->error_count++] = co->result;
                }
            }
        }
        
        // 3. 检查是否还有工作要做
        if (loop->ready_count == 0 && loop->timer_count == 0) {
            // 没有就绪协程且没有定时器，退出
            break;
        }
        
        // 4. 如果没有就绪协程但有定时器，休眠到最近的定时器
        if (loop->ready_count == 0 && loop->timer_count > 0) {
            uint64_t next_time = event_loop_next_timer_time(loop);
            uint64_t now = current_time_ms();
            if (next_time > now) {
                uint64_t sleep_time = next_time - now;
                if (sleep_time > 0) {
                    sleep_ms(sleep_time);
                }
            }
        }
    }
    
    loop->running = 0;
}

// ============================================================================
// GC 支持
// ============================================================================

// 标记协程对象（供 GC 使用）
void gc_mark_coroutine(ObjCoroutine* co) {
    if (!co) return;
    gc_mark_object((Object*)co);
    gc_mark_value(co->result);
    gc_mark_object((Object*)co->closure);
    gc_mark_object((Object*)co->waiting_for);
    gc_mark_object((Object*)co->task_future);  // 标记 task_future
    gc_mark_object((Object*)co->next);          // 标记就绪队列中的下一个协程
    // 标记初始参数
    if (co->initial_args) {
        for (int i = 0; i < co->initial_arg_count; i++) {
            gc_mark_value(co->initial_args[i]);
        }
    }
    // 注意：协程的locals保留在vm.frames中，由GC通过遍历frames来标记
    // saved_frame 指向 vm.frames 中的帧，由 GC 遍历 frames 时标记
    // saved_frame_copy 是 frame 的副本，其闭包和 locals 已经在 vm.frames 中标记
}

// 标记 Future 对象（供 GC 使用）
void gc_mark_future(ObjFuture* future) {
    if (!future) return;
    gc_mark_object((Object*)future);
    gc_mark_value(future->result);
    gc_mark_value(future->error);
    gc_mark_object((Object*)future->waiter);
}
