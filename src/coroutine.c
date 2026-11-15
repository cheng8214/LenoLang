#include "include/lenolang.h"
#include "include/leno_vm.h"
#include "include/leno_value.h"
#include "include/leno_ast.h"
#include "include/platform.h"
#include <stdio.h>

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
    // GC 写屏障：Future 可能已晋升老年代，result 可能是年轻代对象
    gc_write_barrier((Object*)future, result);
    
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
    // GC 写屏障：Future 可能已晋升老年代，error 可能是年轻代对象
    gc_write_barrier((Object*)future, error);
    
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
    // 协程 ID：从**所属 VM** 的计数器递增分配（asyncs.current() 用它区分协程）
    extern THREAD_LOCAL VM* current_exec_vm;
    VM* owner_vm = current_exec_vm ? current_exec_vm : &vm;
    co->id = ++owner_vm->next_coroutine_id;
    co->state = COROUTINE_NEW;
    co->saved_sp = 0;
    co->saved_frame_cnt = 0;  // 协程帧之前的基准 frame_cnt
    co->saved_frames = NULL;  // 保存的 frame 副本数组
    co->saved_frame_count = 0;  // 保存的 frame 数量
    co->result = val_null();
    co->closure = closure;
    co->await_count = 0;
    co->await_dst_reg = -1;
    co->waiting_for = NULL;
    co->task_future = NULL;  // task 返回的 Future，由 OP_ASYNC_CALL 设置
    co->error_propagated = 0;  // 初始未传播
    co->next = NULL;
    co->in_ready = 0;
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
    loop->ready_head = 0;
    loop->timer_seq = 0;
    
    loop->running = 0;
    
    // 初始化错误收集
    loop->errors = (Value*)malloc(sizeof(Value) * 16);
    loop->error_count = 0;
    loop->error_capacity = 16;
}

// 添加协程到就绪队列（环形缓冲，O(1) 入队 + O(1) 去重）
//   ★ 2026-11-14 实测：原来是「数组 + 入队时线性扫全表去重 + 出队 memmove 全表」
//     ⇒ N 个协程时 O(N²)：2000 协程 ×100 次让出时每次 4.3us（500 协程时 1.2us），
//     协程数 ×4、单次代价 ×3.7。改成 head 下标 + 每条协程一个 `in_ready` 标志后全 O(1) ✓
void event_loop_add_ready(EventLoop* loop, ObjCoroutine* coroutine) {
    // 不要添加已完成的协程
    if (coroutine->state == COROUTINE_COMPLETED || coroutine->state == COROUTINE_FAILED) {
        return;
    }
    if (coroutine->in_ready) return;      // 已在队列中

    if (loop->ready_count >= loop->ready_capacity) {
        // 扩容：拉直成 head=0（把环形内容按顺序拷到新数组开头）
        int new_capacity = loop->ready_capacity * 2;
        ObjCoroutine** nq = (ObjCoroutine**)malloc(sizeof(ObjCoroutine*) * (size_t)new_capacity);
        if (!nq) return;                  // 分配失败：宁可不调度这一轮，也别破坏队列
        for (int i = 0; i < loop->ready_count; i++) {
            nq[i] = loop->ready_queue[(loop->ready_head + i) % loop->ready_capacity];
        }
        free(loop->ready_queue);
        loop->ready_queue = nq;
        loop->ready_capacity = new_capacity;
        loop->ready_head = 0;
    }
    int idx = (loop->ready_head + loop->ready_count) % loop->ready_capacity;
    loop->ready_queue[idx] = coroutine;
    loop->ready_count++;
    coroutine->in_ready = 1;
}

// 从就绪队列取出一个协程（O(1)：只挪 head 下标）
static ObjCoroutine* event_loop_dequeue_ready(EventLoop* loop) {
    if (loop->ready_count == 0) return NULL;

    ObjCoroutine* co = loop->ready_queue[loop->ready_head];
    loop->ready_head = (loop->ready_head + 1) % loop->ready_capacity;
    loop->ready_count--;
    co->in_ready = 0;
    return co;
}

// 定时器：二叉最小堆（按 (wake_time, seq) 比较）★ 2026-11-14
//   原来是「有序数组插入 + 到期时全表前移」⇒ O(N) 每次：实测 2000 个 `sleep(1)`
//   要 13.5ms（其中 ~12ms 全是搬数组）⇒ 改堆后 O(log N) ✓
//   用 seq 做第二关键字：同到期时间时保持 FIFO（与旧的稳定插入语义一致）
static inline int timer_less(const Timer* a, const Timer* b) {
    if (a->wake_time != b->wake_time) return a->wake_time < b->wake_time;
    return a->seq < b->seq;
}

static void timer_sift_up(EventLoop* loop, int i) {
    Timer t = loop->timers[i];
    while (i > 0) {
        int p = (i - 1) / 2;
        if (!timer_less(&t, &loop->timers[p])) break;
        loop->timers[i] = loop->timers[p];
        i = p;
    }
    loop->timers[i] = t;
}

static void timer_sift_down(EventLoop* loop, int i) {
    Timer t = loop->timers[i];
    int n = loop->timer_count;
    while (true) {
        int l = i * 2 + 1;
        if (l >= n) break;
        int m = l;
        int r = l + 1;
        if (r < n && timer_less(&loop->timers[r], &loop->timers[l])) m = r;
        if (!timer_less(&loop->timers[m], &t)) break;
        loop->timers[i] = loop->timers[m];
        i = m;
    }
    loop->timers[i] = t;
}

// 添加定时器（入堆 + 上浮）
void event_loop_add_timer(EventLoop* loop, uint64_t wake_time, ObjCoroutine* coroutine) {
    if (loop->timer_count >= loop->timer_capacity) {
        // 定时器队列已满，扩展容量
        int new_capacity = loop->timer_capacity * 2;
        loop->timers = (Timer*)realloc(loop->timers, sizeof(Timer) * new_capacity);
        loop->timer_capacity = new_capacity;
    }

    int i = loop->timer_count;
    loop->timers[i].wake_time = wake_time;
    loop->timers[i].seq = loop->timer_seq++;
    loop->timers[i].coroutine = coroutine;
    loop->timer_count++;
    timer_sift_up(loop, i);
}

extern void sockets_cancel_async(ObjCoroutine* co);   // TH15：超时后取消底层 async IO

// 检查并处理到期的定时器（出堆 + 下沉）
static void event_loop_check_timers(EventLoop* loop) {
    uint64_t now = current_time_ms();
    
    while (loop->timer_count > 0 && loop->timers[0].wake_time <= now) {
        ObjCoroutine* co = loop->timers[0].coroutine;

        // 出堆：末尾元素补到根，再下沉（O(log N)）
        loop->timer_count--;
        if (loop->timer_count > 0) {
            loop->timers[0] = loop->timers[loop->timer_count];
            timer_sift_down(loop, 0);
        }
        
        // 完成 Future（但不要改变协程状态或加入就绪队列）
        // vm_run_coroutine 会处理 SUSPENDED 状态的协程
        if (co->waiting_for && !co->waiting_for->completed) {
            co->waiting_for->completed = 1;
            co->waiting_for->result = val_null();
            // ★ TH15（2026-10-20）：定时器到期 = 这次等待超时（`asyncs.timeout` 或 `sleep`）。
            //   如果它是挂在 socket 上的异步 IO（arecv / aaccept），**必须把请求一并取消** ——
            //   否则 g_async_requests 里那一项仍 active ⇒ `sockets_has_async_io() > 0`
            //   ⇒ `asyncs.run()` 的退出条件永远不满足 ⇒ **整个事件循环卡死** ✗
            //   （实测：一个不回包的连接 + `asyncs.timeout(f, 1000)` 就能让 run() 永不返回）
            sockets_cancel_async(co);
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

// 从"待抛错误"列表里移除一条**同一个错误值**（2026-11-16）
//
// 问题：协程失败时若**当刻没有等待者**（`task_future->waiter == NULL`），错误会被立刻
//   收进 `loop->errors`（见 event_loop_run_for）。之后调用方才 `await` 这个已完成的
//   Future（或在 `asyncs.all` 里拿到它）⇒ 那条错误其实**已被消费**，但列表里还留着
//   ⇒ `asyncs.run()` 结尾**再抛一次同样的错误**。
//   实测：`var bad = boom(); await ok; try { await bad } catch { }` 之后 run() 仍抛
//   "炸"（boom 内部先 await sleep 再 throw ⇒ 失败发生在没人等的窗口里）。
//   文档（module_asyncs.md 路径2）写的是"已通过 await 传播的异常不会被 run() 重复收集"✗
//
// 判据：同一个 Value（NaN-boxing 8 字节完全相同 ⇒ 同一个错误对象 / 同一个数字）。
// `await` 与 `asyncs.all` 在把错误抛给调用方时各调一次 ⇒ 两处消费口径一致。
void event_loop_consume_error(EventLoop* loop, Value err) {
    if (!loop || loop->error_count <= 0) return;
    for (int i = 0; i < loop->error_count; i++) {
        if (memcmp(&loop->errors[i], &err, sizeof(Value)) == 0) {
            for (int j = i + 1; j < loop->error_count; j++) {
                loop->errors[j - 1] = loop->errors[j];
            }
            loop->error_count--;
            return;
        }
    }
}

// 外部声明：执行一个协程直到它挂起或完成
extern int vm_run_coroutine(ObjCoroutine* co);

// 外部声明：sockets 模块提供的 async I/O poll 接口
extern int sockets_poll_async_io(uint64_t timeout_ms);

// 外部声明：查询是否有待处理的异步 I/O 请求
extern int sockets_has_async_io(void);

// ★ 2026-11-14：**程序结束时还有协程没跑/没跑完 ⇒ 运行期警告**（不静默）
//
// 由来：`async func` 被调用只是把协程塞进就绪队列，**不调 asyncs.run() 就永远不执行**
//   （实测 `main(){ t(); print("END") }` 只打印 END、exit=0、零诊断 ✗）。
//   编译期判"有没有 run"不稳（方法调用 / 跨模块 / 运行期才判定 async），
//   但**所有**协程最终都挂在事件循环上 ⇒ 出口数一数就够。
//
// 为什么只警告不报错：先建协程、稍后再 run 是合法用法；`run_for(ms)` 超时留下
//   没跑完的协程也是设计内行为 ⇒ 提醒即可。
//
// 计数口径：
//   · 有事件循环（import 过 asyncs）⇒ ready + 定时器 + 挂起的 socket IO
//   · 没有事件循环（压根没 import asyncs）⇒ 创建的协程一个都没排队，用创建计数
static int g_async_created = 0;

void coroutine_note_async_call(void) {
    g_async_created++;
}

void vm_warn_pending_async(void) {
    int pending;
    if (g_event_loop) {
        pending = g_event_loop->ready_count + g_event_loop->timer_count + sockets_has_async_io();
    } else {
        pending = g_async_created;
    }
    if (pending <= 0) return;

    fprintf(stderr,
        "[警告] 程序结束时还有 %d 个协程/异步 IO 未完成 —— 协程体没有执行（或没跑完）。\n"
        "       创建协程本身不会执行它：需要调用 asyncs.run()（或 run_for(ms)）驱动事件循环。\n",
        pending);
    fflush(stderr);
}

// 运行事件循环直到所有协程完成；timeout_ms > 0 时**最多**跑这么久（TH17）
//   返回 1 = 全部完成；0 = 超时停下（协程 / 定时器 / 未完成的 IO 状态**全部保留**，可再次 run）
//   为什么要它：`asyncs.run()` 会等所有协程结束 ⇒ 只要有一个协程挂在永不完成的 Future 上
//   就永远不返回（没有全局超时只能 kill 进程）。有了它就能"跑一会儿，没完成先出来"。
int event_loop_run_for(EventLoop* loop, uint64_t timeout_ms) {
    loop->running = 1;
    // 重置错误收集
    loop->error_count = 0;

    uint64_t deadline = (timeout_ms > 0) ? (current_time_ms() + timeout_ms) : 0;
    int timed_out = 0;

    while (loop->running) {
        if (deadline != 0 && current_time_ms() >= deadline) {
            timed_out = 1;
            break;
        }
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
        // 关键修复：必须同时检查待处理的异步 I/O 请求（如 arecv/aaccept）
        // 否则在网络 I/O 等待期间事件循环会过早退出
        int has_async_io = sockets_has_async_io();
        if (loop->ready_count == 0 && loop->timer_count == 0 && has_async_io == 0) {
            // 没有就绪协程、没有定时器、没有异步 I/O，退出
            break;
        }

        // 4. 如果没有就绪协程但有定时器或有待处理的 async I/O，等待
        if (loop->ready_count == 0) {
            uint64_t timeout = event_loop_next_timer_time(loop);
            uint64_t now = current_time_ms();
            if (timeout != UINT64_MAX && timeout > now) {
                timeout = timeout - now;
            } else if (timeout == UINT64_MAX) {
                // 无定时器但有异步 I/O 时使用 100ms 轮询超时
                timeout = has_async_io ? 100 : 0;
            } else {
                timeout = 0;
            }

            // 有 deadline 时，单次等待不能越过它（否则超时精度会差一整个 poll 周期）
            if (deadline != 0) {
                uint64_t now2 = current_time_ms();
                uint64_t remain = (deadline > now2) ? (deadline - now2) : 0;
                if (remain < timeout) timeout = remain;
            }

            // 用 sockets_poll_async_io 等待 I/O 就绪或定时器到期
            sockets_poll_async_io(timeout);
        }
    }

    loop->running = 0;
    return timed_out ? 0 : 1;
}

// 无限期运行（旧行为，保留给 internal 调用）
void event_loop_run(EventLoop* loop) {
    (void)event_loop_run_for(loop, 0);
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
    // 标记保存的调用帧副本数组
    if (co->saved_frames && co->saved_frame_count > 0) {
        for (int fi = 0; fi < co->saved_frame_count; fi++) {
            CallFrame* frame_copy = &co->saved_frames[fi];
            gc_mark_object((Object*)frame_copy->closure);
            if (frame_copy->chunk) {
                for (int j = 0; j < frame_copy->chunk->const_cnt; j++) {
                    gc_mark_value(frame_copy->chunk->constants[j]);
                }
            }
            if (frame_copy->locals) {
                for (int i = 0; i < frame_copy->local_count; i++) {
                    gc_mark_value(frame_copy->locals[i]);
                }
            }
            // FRAME_TRY_VALID：无 try 函数的帧 has_try_return 为脏值（帧复用优化）
            if (FRAME_TRY_VALID(frame_copy) && frame_copy->has_try_return) {
                gc_mark_value(frame_copy->try_return_value);
            }
            if (frame_copy->module) {
                gc_mark_object((Object*)frame_copy->module);
            }
        }
    }
}

// 标记 Future 对象（供 GC 使用）
void gc_mark_future(ObjFuture* future) {
    if (!future) return;
    gc_mark_object((Object*)future);
    gc_mark_value(future->result);
    gc_mark_value(future->error);
    gc_mark_object((Object*)future->waiter);
}
