# Leno Async/Await 协程系统设计文档

## 1. 设计理念

### 1.1 为什么选择协程

Leno 是个人脚本语言，核心场景是 I/O 密集型任务（网络请求、文件读写、定时器）。协程模型最适合：

- **实现简单** — 单线程，无需锁、线程池、竞争条件
- **VM 改动小** — 在现有栈式 VM 上扩展，不需要多线程调度器
- **学习成本低** — 类似 Python asyncio / JavaScript async，概念通用
- **与 Leno 风格一致** — 简洁、实用、不过度设计

### 1.2 核心概念

```
async 函数 = 可暂停的函数
await      = 暂停点（让出执行权）
协程       = 一个 async 函数的执行实例
调度器     = 管理所有协程的执行顺序
```

### 1.3 与其他语言对比

| 特性 | Leno | Python | JavaScript | Go |
|------|------|--------|------------|-----|
| 关键字 | `async` `await` | `async` `await` | `async` `await` | `go` `chan` |
| 模型 | 协程 | 协程 | 协程 | 绿色线程 |
| 并行 | ❌ 单线程 | ❌ 单线程 | ❌ 单线程 | ✅ 多线程 |
| 通信 | 返回值 | Future/Task | Promise | Channel |
| 启动 | `async` 标记 | `async` 标记 | `async` 标记 | `go` 关键字 |

---

## 2. 语法设计

### 2.1 async 函数定义

```leno
// 基本用法
async func fetch_data(string url):string {
    await sleep(1000)        // 暂停 1 秒
    return $"data from {url}"
}

// async 无返回值
async func log_after(int ms, string msg) {
    await sleep(ms)
    print(msg)
}
```

**规则：**
- `async` 关键字放在 `func` 前面
- async 函数可以有返回值，也可以没有
- async 函数可以有参数，参数规则与普通函数相同
- async 函数内可以使用 `await`

### 2.2 await 表达式

```leno
// await 暂停当前协程，等待操作完成
async func example() {
    var data = await fetch_data("https://api.example.com")
    print(data)
}
```

**规则：**
- `await` 只能在 `async` 函数内使用
- `await` 后面跟一个返回 `Future` 的表达式
- `await` 的值是 `Future` 完成后的结果
- `await` 让出执行权，调度器切换到其他协程

### 2.3 sleep — 最简单的异步操作

```leno
import async

main() {
    async func demo() {
        print("开始")
        await async.sleep(1000)    // 暂停 1000ms
        print("1秒后")
        await async.sleep(500)     // 暂停 500ms
        print("又过了0.5秒")
    }

    demo()    // 启动协程
    async.run()    // 运行事件循环
}

// 输出:
// 开始
// (等待1秒)
// 1秒后
// (等待0.5秒)
// 又过了0.5秒
```

### 2.4 并发执行多个协程

```leno
import async

main() {
    async func task(string name, int ms) {
        await async.sleep(ms)
        print($"{name} 完成 (耗时 {ms}ms)")
    }

    // 启动多个协程（并发执行）
    task("A", 1000)
    task("B", 500)
    task("C", 1500)

    async.run()    // 总耗时约 1500ms（最长的那个），不是 3000ms
}

// 输出:
// B 完成 (耗时 500ms)
// A 完成 (耗时 1000ms)
// C 完成 (耗时 1500ms)
```

### 2.5 async 函数返回值

```leno
import async

async func fetch(string url):string {
    await async.sleep(100)
    return $"response from {url}"
}

main() {
    async func main_task() {
        // await 获取返回值
        var result = await fetch("/api/users")
        print(result)
    }

    main_task()
    async.run()
}
```

### 2.6 并发等待

```leno
import async

main() {
    async func load_user(int id):string {
        await async.sleep(500)
        return $"user_{id}"
    }

    async func load_posts(int user_id):Array[string] {
        await async.sleep(800)
        return ["post1", "post2"]
    }

    async func main_task() {
        // 并发执行，等待全部完成
        var results = await async.all([
            load_user(1),
            load_posts(1)
        ])

        print(results[0])     // "user_1"
        print(results[1])     // ["post1", "post2"]
    }

    main_task()
    async.run()
}
```

### 2.7 超时控制

```leno
import async

main() {
    async func slow_operation():string {
        await async.sleep(5000)
        return "完成"
    }

    async func main_task() {
        // 最多等 1 秒
        var result = await async.timeout(slow_operation(), 1000)
        if result is null {
            print("超时了！")
        } else {
            print(result)
        }
    }

    main_task()
    async.run()
}
```

---

## 3. async 模块 API

### 3.1 核心函数

| 函数 | 说明 |
|------|------|
| `async.sleep(int ms)` | 暂停指定毫秒数，返回 Future |
| `async.run()` | 启动事件循环，运行所有协程直到全部完成 |
| `async.all(Array[Future])` | 并发等待多个 Future，全部完成后返回结果数组 |
| `async.timeout(Future, int ms)` | 等待 Future，超时返回 null |
| `async.yield()` | 主动让出执行权（不暂停，只是让其他协程运行） |

### 3.2 事件循环生命周期

```
1. 用户代码启动协程（调用 async 函数）
2. 调用 async.run() 启动事件循环
3. 事件循环：
   a. 检查到期的定时器 → 恢复对应协程
   b. 执行就绪的协程，直到遇到 await 或完成
   c. 重复 a-b 直到所有协程完成
4. async.run() 返回
```

---

## 4. 与现有特性的交互

### 4.1 与普通函数的关系

```leno
// 普通函数不能 await
func normal() {
    // await sleep(100)   // ❌ 编译错误：await 只能在 async 函数中使用
}

// async 函数可以调用普通函数
async func async_fn() {
    print("hello")        // ✅ 调用普通函数
    var x = 10            // ✅ 普通变量操作
    await async.sleep(100)
}

// 普通函数可以启动 async 函数（但不能 await）
func starter() {
    async_fn()            // ✅ 启动协程，但不等待结果
}
```

### 4.2 与 struct 的交互

```leno
struct HttpClient {
    string base_url = ""

    async func get(string path):string {
        await async.sleep(100)
        return $"GET {base_url}{path}"
    }

    async func post(string path, string body):string {
        await async.sleep(200)
        return $"POST {base_url}{path} body={body}"
    }
}

main() {
    var client = HttpClient()
    client.base_url = "https://api.example.com"

    async func main_task() {
        var users = await client.get("/users")
        print(users)
    }

    main_task()
    async.run()
}
```

### 4.3 与闭包的交互

```leno
main() {
    async func counter(int count) {
        for 0:count to i {
            await async.sleep(100)
            print($"第 {i} 次")
        }
    }

    counter(3)
    counter(5)
    async.run()    // 两个协程交替执行
}
```

### 4.4 与 try/catch 的交互

```leno
main() {
    async func safe_fetch(string url):string {
        try {
            var result = await fetch(url)
            return result
        } catch e {
            print($"请求失败: {e}")
            return ""
        }
    }

    safe_fetch("/api/data")
    async.run()
}
```

### 4.5 与 for 循环的交互

```leno
main() {
    async func process_items(Array[string] items) {
        for items to item {
            await async.sleep(100)
            print($"处理: {item}")
        }
    }

    process_items(["a", "b", "c"])
    async.run()
}
```

---

## 5. 编译器实现

### 5.1 AST 节点

```
AsyncFuncDef:
  - 继承 FuncDef
  - is_async: true
  - body: 与普通函数相同

AwaitExpr:
  - expr: ExprNode    // await 的表达式（通常是函数调用）
```

### 5.2 语法解析

```
// async 函数定义
async_func_def := TOK_ASYNC TOK_FUNC name '(' params ')' (':' type)? '{' stmts '}'

// await 表达式
await_expr := TOK_AWAIT expr
```

解析器新增两个 token：
- `TOK_ASYNC` — `async` 关键字
- `TOK_AWAIT` — `await` 关键字

### 5.3 语义分析

**规则检查：**
1. `await` 只能在 `async` 函数体内使用 → 编译错误
2. `async` 函数的返回类型自动包装为 `Future[T]`
3. `await expr` 的类型是 `T`（从 `Future[T]` 中提取）

**类型推断：**
```
async func foo():string { ... }
// foo 的类型是 func():Future[string]

var result = await foo()
// result 的类型是 string
```

### 5.4 字节码指令

```
OP_AWAIT
  // 暂停当前协程
  // 栈顶必须是 Future 对象
  // 将当前协程挂起，注册到 Future 的等待列表
  // 切换到调度器

OP_ASYNC_CALL arg_count
  // 调用 async 函数，创建新协程
  // 与 OP_CALL 类似，但不等待结果
  // 返回 Future 对象压栈

OP_RESUME coroutine_ref
  // 恢复一个挂起的协程
  // 由调度器在定时器到期时调用
```

**编译示例：**

```leno
async func fetch(string url):string {
    await async.sleep(100)
    return $"data: {url}"
}
```

编译为：

```
// fetch(self, url):
//   OP_ASYNC_CALL 0, async.sleep, 1    调用 async.sleep(100)
//   OP_AWAIT                               暂停，等待 Future 完成
//   ... 构造返回值 ...
//   OP_RETURN

// 调用方:
//   var data = await fetch("/api")
//   OP_LOAD_CONST "/api"
//   OP_ASYNC_CALL 1, fetch, 1            调用 fetch，返回 Future
//   OP_AWAIT                                暂停，等待 Future 完成
//   OP_STORE_LOCAL "data"                  data = 结果
```

---

## 6. VM 运行时实现

### 6.1 核心数据结构

```c
// 协程状态
typedef enum {
    COROUTINE_NEW,        // 刚创建，未开始
    COROUTINE_RUNNING,    // 正在执行
    COROUTINE_SUSPENDED,  // 被 await 暂停
    COROUTINE_COMPLETED,  // 执行完毕
    COROUTINE_FAILED      // 执行出错
} CoroutineState;

// 协程对象
typedef struct {
    Object header;
    CoroutineState state;
    CallFrame* saved_frame;     // 保存的调用帧
    uint8_t* saved_ip;          // 保存的指令指针
    Value* saved_stack_base;    // 保存的栈基址
    Value result;               // 返回值 / Future 结果
    ObjClosure* closure;        // 关联的闭包
    int await_count;            // await 次数
} ObjCoroutine;

// Future 对象
typedef struct {
    Object header;
    int completed;              // 是否完成
    Value result;               // 结果值
    Value error;                // 错误值
    ObjCoroutine* waiter;       // 等待此 Future 的协程
} ObjFuture;

// 定时器
typedef struct {
    uint64_t wake_time;         // 唤醒时间（毫秒）
    ObjCoroutine* coroutine;    // 关联的协程
} Timer;

// 事件循环
typedef struct {
    Timer timers[MAX_TIMERS];   // 定时器队列
    int timer_count;
    ObjCoroutine* ready_queue[MAX_COROUTINES];  // 就绪队列
    int ready_count;
    int running;                // 是否正在运行
} EventLoop;
```

### 6.2 OP_AWAIT 执行流程

```
OP_AWAIT:
  1. 弹出栈顶的 Future 对象
  2. 如果 Future 已完成：
     a. 将 Future.result 压栈
     b. 继续执行（不暂停）
  3. 如果 Future 未完成：
     a. 保存当前协程状态（ip, frame, stack_base）
     b. 设置协程状态为 SUSPENDED
     c. 将当前协程注册到 Future.waiter
     d. 跳出执行循环，返回调度器
```

### 6.3 OP_ASYNC_CALL 执行流程

```
OP_ASYNC_CALL:
  1. 创建新的 ObjCoroutine
  2. 设置初始调用帧（closure, args）
  3. 创建 ObjFuture 关联此协程
  4. 将协程加入就绪队列
  5. 将 Future 压栈（给调用者）
  6. 继续执行调用者（不暂停）
```

### 6.4 事件循环

```c
void event_loop_run(EventLoop* loop) {
    loop->running = 1;
    while (loop->running) {
        // 1. 检查定时器，将到期的协程移入就绪队列
        check_timers(loop);

        // 2. 如果没有就绪协程，也没有定时器，退出
        if (loop->ready_count == 0 && loop->timer_count == 0) {
            break;
        }

        // 3. 执行一个就绪协程（直到 await 或完成）
        if (loop->ready_count > 0) {
            ObjCoroutine* co = dequeue_ready(loop);
            run_coroutine(co);
        }

        // 4. 如果没有就绪协程但有定时器，休眠到最近的定时器
        if (loop->ready_count == 0 && loop->timer_count > 0) {
            Timer* next = find_earliest_timer(loop);
            sleep_ms(next->wake_time - current_time_ms());
        }
    }
    loop->running = 0;
}
```

### 6.5 async.sleep 实现

```c
// async.sleep(ms) 的原生函数实现
Value native_async_sleep(int arg_count, Value* args) {
    int ms = val_as_int(args[0]);

    // 1. 创建 Future
    ObjFuture* future = future_new();

    // 2. 获取当前协程
    ObjCoroutine* current = vm_current_coroutine();

    // 3. 创建定时器
    Timer timer;
    timer.wake_time = current_time_ms() + ms;
    timer.coroutine = current;

    // 4. 将定时器注册到事件循环
    event_loop_add_timer(&vm.event_loop, timer);

    // 5. 将当前协程设为等待此 Future
    future->waiter = current;

    return val_obj((Object*)future);
}
```

### 6.6 协程恢复流程

```
定时器到期时：
  1. 从定时器队列移除
  2. 将关联协程的状态设为 RUNNING
  3. 恢复协程的 ip, frame, stack_base
  4. 将 Future.result 压栈（await 的返回值）
  5. 将协程加入就绪队列
  6. 调度器下次循环会执行此协程
```

---

## 7. GC 集成

### 7.1 标记

```c
void gc_mark_coroutine(ObjCoroutine* co) {
    gc_mark_header(&co->header);
    gc_mark_value(co->result);
    gc_mark_obj((Object*)co->closure);
    // saved_frame 中的值通过栈扫描标记
}

void gc_mark_future(ObjFuture* future) {
    gc_mark_header(&future->header);
    gc_mark_value(future->result);
    gc_mark_value(future->error);
    gc_mark_obj((Object*)future->waiter);
}
```

### 7.2 内存大小

```c
size_t get_object_size(Object* obj) {
    switch (obj->type) {
        case OBJ_COROUTINE: return sizeof(ObjCoroutine);
        case OBJ_FUTURE:    return sizeof(ObjFuture);
        // ...
    }
}
```

### 7.3 资源释放

```c
void free_coroutine(ObjCoroutine* co) {
    // 释放保存的调用帧（如果是动态分配的）
    if (co->saved_frame && co->saved_frame->locals_is_dynamic) {
        free(co->saved_frame->locals);
    }
    free(co->saved_frame);
}

void free_future(ObjFuture* future) {
    // Future 本身没有额外堆内存
}
```

---

## 8. 错误处理

### 8.1 编译错误

```
❌ await 在非 async 函数中使用
   func normal() {
       await sleep(100)
   }
   → "await 只能在 async 函数中使用"

❌ await 无效表达式
   async func foo() {
       await 42
   }
   → "await 的表达式必须返回 Future 类型"

❌ async main 不允许
   async main() { }
   → "main 函数不能是 async 函数，请在 main 中启动协程并调用 async.run()"
```

### 8.2 运行时错误

```
❌ 事件循环未启动
   async func foo() {
       await async.sleep(100)
   }
   foo()    // 启动了协程但没有调用 async.run()
   → 程序正常退出，协程未执行（警告："协程未运行，请调用 async.run()"）
```

---

## 9. 完整示例

### 9.1 并发下载模拟

```leno
import async

async func download(string url, int size):string {
    // 模拟下载：每 100KB 暂停一次
    int downloaded = 0
    while downloaded < size {
        await async.sleep(100)
        downloaded = downloaded + 100
        printf("\r${url}: ")
        printf(_str(downloaded))
        printf("KB / ")
        printf(_str(size))
        printf("KB")
    }
    print("")
    return $"下载完成: {url} ({size}KB)"
}

main() {
    async func main_task() {
        print("开始并发下载...")

        var results = await async.all([
            download("/file1", 300),
            download("/file2", 200),
            download("/file3", 500)
        ])

        for results to r {
            print(r)
        }
    }

    main_task()
    async.run()
    print("全部完成!")
}
```

### 9.2 超时 + 重试

```leno
import async

async func request(string url):string {
    await async.sleep(500)
    return $"OK: {url}"
}

async func fetch_with_retry(string url, int max_retries):string {
    for 0:max_retries to i {
        var result = await async.timeout(request(url), 1000)
        if result != null {
            return result
        }
        print($"第 {i + 1} 次超时，重试中...")
    }
    return "失败"
}

main() {
    async func main_task() {
        var result = await fetch_with_retry("/api/data", 3)
        print(result)
    }

    main_task()
    async.run()
}
```

### 9.3 生产者-消费者模式

```leno
import async

// 简单的任务队列（用数组和协程模拟）
struct TaskQueue {
    Array[string] tasks = []
    int consumers = 0
}

main() {
    var queue = TaskQueue()

    async func producer(string name) {
        for 0:5 to i {
            await async.sleep(200)
            var task = $"{name}_task_{i}"
            queue.tasks.add(task)
            print($"生产: {task}")
        }
    }

    async func consumer(string name) {
        for 0:5 to i {
            while queue.tasks.len() == 0 {
                await async.yield()    // 队列为空，让出执行权
            }
            var task = queue.tasks.remove(0)
            print($"  {name} 消费: {task}")
            await async.sleep(100)
        }
    }

    producer("P1")
    producer("P2")
    consumer("C1")
    consumer("C2")

    async.run()
}
```

---

## 10. 实现优先级

### P0 - 基础协程（约 5 天）

| 任务 | 时间 | 说明 |
|------|------|------|
| 词法/语法 | 0.5 天 | `async` `await` 关键字，async 函数解析 |
| AST + 语义 | 1 天 | AsyncFuncDef, AwaitExpr, await 位置检查 |
| ObjCoroutine | 1 天 | 协程对象，状态保存/恢复 |
| ObjFuture | 0.5 天 | Future 对象，完成/等待机制 |
| OP_AWAIT | 1 天 | 暂停协程，保存状态 |
| OP_ASYNC_CALL | 0.5 天 | 创建协程，返回 Future |
| 事件循环 | 0.5 天 | 就绪队列 + 定时器 |
| async.sleep | 0.5 天 | 定时器实现 |
| async.run | 0.5 天 | 事件循环入口 |
| 基础测试 | 0.5 天 | sleep, 并发, 返回值 |

### P1 - 进阶功能（约 4 天）

| 任务 | 时间 | 说明 |
|------|------|------|
| async.all | 1 天 | 并发等待多个 Future |
| async.timeout | 0.5 天 | 超时控制 |
| async.yield | 0.5 天 | 主动让出 |
| GC 集成 | 1 天 | 标记、释放、内存统计 |
| struct 方法 | 0.5 天 | async 方法在 struct 中 |
| 错误处理 | 0.5 天 | try/catch 与 await 的交互 |

### P2 - 扩展功能（约 3 天）

| 任务 | 时间 | 说明 |
|------|------|------|
| async.http_get | 1 天 | HTTP 请求（需要 socket 支持） |
| async.file_read | 1 天 | 异步文件读取 |
| async.channel | 1 天 | 协程间通信（可选） |

---

## 11. 设计决策总结

| 决策 | 选择 | 理由 |
|------|------|------|
| 并发模型 | 单线程协程 | 最简单，VM 改动最小 |
| 启动方式 | 调用即启动 + async.run() | 类似 JS，无需 go 关键字 |
| 返回值 | Future 对象 | 显式表示异步结果 |
| 事件循环 | 显式 async.run() | 用户控制何时开始调度 |
| 通信机制 | 暂不实现 channel | 先做基础，按需添加 |
| 错误传播 | try/catch + Future.error | 与现有异常处理一致 |
| 协程调度 | FIFO 就绪队列 | 公平简单 |
| 定时器 | 最小堆 | 高效查找最近到期 |

---

## 12. 未来扩展

### 12.1 Channel（协程通信）

```leno
// 未来可能支持
var ch = async.channel<string>()

async func producer() {
    ch.send("hello")
    ch.send("world")
    ch.close()
}

async func consumer() {
    var msg = await ch.receive()
    while msg != null {
        print(msg)
        msg = await ch.receive()
    }
}
```

### 12.2 select（多路复用）

```leno
// 未来可能支持
var result = await async.select({
    ch1: ch1.receive(),
    ch2: ch2.receive(),
    timeout: async.sleep(1000)
})
```

### 12.3 取消协程

```leno
// 未来可能支持
var handle = async.spawn(slow_task())
await async.sleep(100)
handle.cancel()
```
