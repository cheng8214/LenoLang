# Leno Threads 线程使用指南

## 简介

Leno 提供基于 **隔离 VM + Channel** 的多线程支持，参考 Go 语言的 goroutine + channel 设计。每个线程拥有独立的 VM 实例、GC 和内存空间，线程之间通过 Channel 传递消息，**不共享内存，零数据竞争**。

---

## 核心概念

| 概念 | 说明 |
|------|------|
| `Thread` | 线程对象，由 `threads.start()` 创建 |
| `Channel` | 通道对象，线程间安全通讯的桥梁 |
| `threads.start(func, args...)` | 启动新线程，可传递参数 |
| `t.join()` | 等待线程结束，获取返回值 |
| `ch.send(value)` | 向通道发送值（阻塞） |
| `ch.receive()` | 从通道接收值（阻塞） |
| `ch.try_send(value)` | 非阻塞发送，成功返回 true |
| `ch.try_receive()` | 非阻塞接收，成功返回值，失败返回 null |
| `ch.close()` | 关闭通道 |
| `ch.is_closed()` | 检查通道是否已关闭，返回 bool |
| `ch.len()` | 获取缓冲区当前消息数量 |
| `threads.sleep(ms)` | 当前线程休眠指定毫秒数 |

### 1. 创建线程

```leno
import threads

func hello() {
    print("子线程运行")
}

main() {
    var t = threads.start(hello)
    t.join()
}
```

输出：
```
子线程运行
```

### 2. 线程返回值

```leno
import threads

func compute() {
    var sum = 0
    for 100 to var i {
        sum = sum + i + 1
    }
    return sum
}

main() {
    var t = threads.start(compute)
    var result = t.join()
    print("结果: " + result)    // 结果: 5050
}
```

### 3. 多线程并行

```leno
import threads

func task1() {
    for 5 to var i {
        print("任务1: " + (i + 1))
    }
    return "任务1完成"
}

func task2() {
    for 5 to var i {
        print("任务2: " + (i + 1))
    }
    return "任务2完成"
}

main() {
    var t1 = threads.start(task1)
    var t2 = threads.start(task2)

    var r1 = t1.join()
    var r2 = t2.join()
    print(r1)
    print(r2)
}
```

输出（两个线程并行，输出交错）：
```
任务1: 1
任务2: 1
任务1: 2
任务2: 2
...
```

---

## 线程传参

`threads.start(func, arg1, arg2, ...)` 支持向线程函数传递参数，参数会深拷贝到子线程。

```leno
import threads

func greet(var name, var count) {
    for count to var i {
        print("Hello, " + name + "!")
    }
}

main() {
    var t = threads.start(greet, "Leno", 3)
    t.join()
}
```

输出：
```
Hello, Leno!
Hello, Leno!
Hello, Leno!
```

---

## Channel 通讯

Channel 是线程间安全通讯的核心机制，参考 Go 语言的 channel 设计。

### 创建 Channel

```leno
// 有缓冲通道（异步）— 缓冲区大小为 N
var ch = threads.channel(10)

// 无缓冲通道（同步）— send 阻塞直到有人 receive
var ch = threads.channel(0)
```

### 基本发送与接收

```leno
import threads

func sender(var ch) {
    ch.send("hello")
    ch.send(42)
    ch.send(3.14)
    ch.close()
}

main() {
    var ch = threads.channel(10)
    var t = threads.start(sender, ch)

    print(ch.receive())    // hello
    print(ch.receive())    // 42
    print(ch.receive())    // 3.14

    t.join()
}
```

### 非阻塞操作

`try_send()` 和 `try_receive()` 提供非阻塞的 Channel 操作：

```leno
import threads

main() {
    var ch = threads.channel(2)

    // try_send: 成功返回 true，失败（满/关闭）返回 false
    assert_eq(ch.try_send("A"), true)
    assert_eq(ch.try_send("B"), true)
    assert_eq(ch.try_send("C"), false)  // 缓冲区已满

    // try_receive: 成功返回值，失败（空）返回 null
    assert_eq(ch.try_receive(), "A")
    assert_eq(ch.try_receive(), "B")
    assert_eq(ch.try_receive(), null)   // 缓冲区已空

    ch.close()
    assert_eq(ch.try_send("D"), false)  // 已关闭
}
```

### Channel 关闭

- `ch.close()` 关闭通道，不再接受新消息
- `ch.receive()` 在通道关闭且缓冲区为空后返回 `null`
- 向已关闭的通道 `send()` 会抛出异常
- 向已关闭的通道 `try_send()` 返回 `false`（不抛异常）

```leno
import threads

func producer(var ch) {
    for 5 to var i {
        ch.send(i)
    }
    ch.close()
}

main() {
    var ch = threads.channel(10)
    var t = threads.start(producer, ch)

    while true {
        var msg = ch.receive()
        if msg == null {
            print("通道已关闭")
            break
        }
        print("收到: " + msg)
    }

    t.join()
}
```

输出：
```
收到: 0
收到: 1
收到: 2
收到: 3
收到: 4
通道已关闭
```

### 生产者-消费者模式

```leno
import threads

func producer(var ch) {
    for 10 to var i {
        ch.send(i)
        print("生产: " + i)
    }
    ch.close()
}

func consumer(var ch) {
    while true {
        var msg = ch.receive()
        if msg == null {
            print("消费者结束")
            break
        }
        print("消费: " + msg)
    }
}

main() {
    var ch = threads.channel(5)
    var tp = threads.start(producer, ch)
    var tc = threads.start(consumer, ch)
    tp.join()
    tc.join()
}
```

### 双线程通讯

```leno
import threads

func sender(var ch) {
    for 3 to var i {
        ch.send("消息" + (i + 1))
    }
    ch.send("结束")
    ch.close()
}

func receiver(var ch) {
    while true {
        var msg = ch.receive()
        print("接收: " + msg)
        if msg == "结束" {
            break
        }
    }
}

main() {
    var ch = threads.channel(10)
    var ts = threads.start(sender, ch)
    var tr = threads.start(receiver, ch)
    ts.join()
    tr.join()
}
```

---

## 可传递的类型

### Channel 支持的类型

| 类型 | 说明 | 传递方式 |
|------|------|---------|
| `int` | 整数 | 直接复制 |
| `float` | 浮点数 | 直接复制 |
| `bool` | 布尔值 | 直接复制 |
| `string` | 字符串 | 深拷贝 |
| `null` | 空值 | 直接复制 |
| `Array` | 数组 | 深拷贝（递归复制每个元素） |
| `Dict` | 字典 | 深拷贝（递归复制每个键值对） |
| `Channel` | 通道 | 引用计数共享（可传递 channel 本身） |

### 不支持传递的类型

| 类型 | 原因 |
|------|------|
| `Ptr` | 指针不能跨 VM 共享 |
| `File` | 文件句柄不能跨 VM |
| `struct` 实例 | 需要序列化，暂不支持 |
| 闭包/函数 | 需要序列化，暂不支持 |

### cstruct 在线程中的使用

**cstruct 定义**可以在线程中安全使用。子线程会继承主线程的 cstruct 定义，且由于 cstruct 定义是**只读的类型元数据**，多个线程可以安全共享：

```leno
import ffi
import threads

cstruct MEMORYSTATUS {
    u32 dwLength
    u32 dwMemoryLoad
    u64 dwTotalPhys
}

func get_memory_info(var ch) {
    // 在子线程中使用 cstruct
    var kernel32 = ffi.load("kernel32.dll")
    var mem = MEMORYSTATUS.malloc()
    
    mem.dwLength = MEMORYSTATUS.size()
    ffi.call(kernel32, "GlobalMemoryStatus", mem.to_ptr())
    
    ch.send(mem.dwTotalPhys)
    mem.free()
    ffi.free(kernel32)
}

main() {
    var ch = threads.channel(1)
    var t = threads.start(get_memory_info, ch)
    var total_mem = ch.receive()
    print("总物理内存: " + total_mem)
    t.join()
}
```

**注意事项：**
- cstruct **定义**（如 `MEMORYSTATUS`）在线程间共享，是安全的
- cstruct **实例**（如 `mem`）是每个线程独立的，不能跨线程传递
- 每个线程需要独立进行 `malloc()` 和 `free()`

---

## 线程中的功能支持

子线程拥有完整的 VM 实例，支持所有 Leno 语言特性：

### 闭包

```leno
func test_closure() {
    var x = 10
    func inner() {
        print("闭包捕获: x=" + x)
        x = x + 1
        print("闭包修改: x=" + x)
    }
    inner()
}
```

### 异常处理

```leno
func test_exception() {
    try {
        throw "线程中的错误"
    } catch e {
        print("捕获: " + e)
    } finally {
        print("finally 执行")
    }
}
```

### 嵌套函数调用

```leno
func add(var a, var b) { return a + b }
func multiply(var a, var b) { return a * b }

func test() {
    var result = multiply(add(3, 4), 2)
    print(result)    // 14
}
```

### 递归

```leno
func fibonacci(var n) {
    if n <= 1 { return n }
    return fibonacci(n - 1) + fibonacci(n - 2)
}

func test() {
    print(fibonacci(15))    // 610
}
```

### 字典与数组

```leno
func test() {
    var arr = [10, 20, 30]
    arr.add(40)
    print(arr[1:3])    // [20, 30]

    var dict = {name: "Leno", age: 1}
    print(dict.name)   // Leno
}
```

---

## 完整示例：并行排序

```leno
import threads
import times as ti
import rands

func bubble_sort(var arr) {
    var n = arr.len()
    for n - 1 to var i {
        for n - i - 1 to var j {
            if arr[j] > arr[j + 1] {
                var temp = arr[j]
                arr[j] = arr[j + 1]
                arr[j + 1] = temp
            }
        }
    }
    return arr
}

func sort_worker(var ch) {
    var arr = rands.int_array(1, 1000)
    var s = ti.ms()
    var result = bubble_sort(arr)
    var e = ti.ms()
    ch.send(e - s)
}

main() {
    var ch = threads.channel(4)

    // 启动 4 个排序线程
    for 4 to var i {
        threads.start(sort_worker, ch)
    }

    // 收集结果
    var total = 0
    for 4 to var i {
        var ms = ch.receive()
        print("线程" + (i + 1) + ": " + ms + "毫秒")
        total = total + ms
    }
    print("平均: " + (total / 4) + "毫秒")
}
```

---

## 注意事项

### 1. 闭包捕获变量（深拷贝）

`threads.start()` 支持闭包捕获外部变量，捕获的值会深拷贝到子线程，各线程拥有独立副本：

```leno
var x = 10
var t = threads.start(func() {
    print(x)    // ✅ 输出: 10（深拷贝）
})
t.join()
```

多个线程捕获同一变量时，各自拥有独立副本，互不影响：

```leno
var shared = 100
var t1 = threads.start(func(){ return shared })
var t2 = threads.start(func(){ return shared })
print(t1.join())  // 100
print(t2.join())  // 100
```

闭包捕获 + 参数传递：

```leno
var prefix = "hello"
var t = threads.start(func(int n){ return prefix + " " + n }, 42)
print(t.join())  // hello 42
```

### 2. 全局变量是独立的

子线程启动时会深拷贝主线程的全局变量，之后互不影响：

```leno
var count = 0

func worker() {
    count = count + 1
    print("子线程: " + count)    // 子线程: 1
}

main() {
    count = 100
    var t = threads.start(worker)
    t.join()
    print("主线程: " + count)    // 主线程: 100（不受子线程影响）
}
```

### 3. Channel 的缓冲大小

- **有缓冲** `threads.channel(N)`：缓冲区满时 `send()` 阻塞，适合异步通讯
- **无缓冲** `threads.channel(0)`：`send()` 阻塞直到有人 `receive()`，适合同步通讯

### 4. 线程安全

- 每个线程拥有独立的 GC，无需担心 GC 冲突
- Channel 内部使用互斥锁和条件变量，线程安全
- `send()` 和 `receive()` 可以安全地在不同线程中并发调用

### 5. 跨线程能传什么（**传值即深拷贝**）

| 值 | 能传吗 | 行为 |
|---|---|---|
| `int` / `float` / `bool` / `null` / `string` | ✅ | 按值 / 复制 |
| `Array` / `Dict` | ✅ | 深克隆（连字典的键一起） |
| **struct 实例** | ✅ | 深克隆 + 接收侧按定义重建（2026-10-12 起；嵌套 struct / face 声明也一并带上） |
| `Channel` | ✅ | **按引用**共享（同一把句柄） |
| cstruct，或字段里装了 cstruct / `Ptr` / 闭包的 struct | ❌ | 报「不能跨线程传递此类型（来自 结构体.字段）」—— cstruct 是裸 C 内存，错的是给它克隆出**假句柄** |
| **自引用（环形）struct** | ❌ | 报「嵌套层数超过 64」而不是挂死 |

**因为是深拷贝，子线程改了字段主线程看不见** —— 结果要靠 `join()` 的返回值或 `Channel` 带回来：

```leno
struct P { int x = 0 }
func work(P p): int {
    p.x = 100        // 改的是**副本**
    return p.x       // ✓ 结果从返回值走
}
main() {
    var p = new P()
    var t = threads.start(work, p)
    print(t.join())  // 100
    print(p.x)       // 0 —— 主线程的原件没被改到
}
```

详细约束（含 `Ptr` 字段、环形引用这些拒绝面）见 `docs/module_threads_api.md` 的
「跨线程传值的硬约束」，改动与判据见 `docs/线程并发能力修复追踪.md` 的 TH1。

### 6. ⚠ 同一条通道上"边收边发"会把自己发出去的消息收回来

通道语义是"**谁先 `receive` 谁拿**"，不区分发送者。所以"父线程发 N 条 → 子线程收 N 条
→ 子线程再发回 N 条"这种**在同一个通道上**的写法，子线程会先收到**自己刚发出去**的那条：

```leno
// ✗ 同一条通道收发混用：子线程第 2 轮收到的是自己发回的 q，循环次数就对不上了
for 0 : 2 to var i {
    var v = ch.receive()
    if v is Point => q { ch.send(q) }
}
```

**做法**：收发各用一条通道（`in_ch` / `out_ch`），或让方向单一（只发或只收）。
⚠ 附带一条容易踩的：**`for A : B` 是闭区间**（`for 0 : 3` 跑 0、1、2、3 共 **4** 轮）
—— 循环次数与消息条数对不上时，`receive()` 会永久阻塞、`join()` 也就永远等下去
（表现是"程序卡死且无输出"）。

### 7. FFI 回调与原生线程的关系

Leno 提供两种并行机制，它们工作在不同层面：

| 机制 | 实现方式 | 线程安全 | 适用场景 |
|------|---------|---------|---------|
| `threads.start()` | 独立 VM 实例，完全隔离 | ✅ 安全 | Leno 原生多线程计算 |
| `ffi.callback()` | 共享主 VM 栈，外部线程直接回调 | ⚠️ 单线程安全 | C 库单线程回调 Leno |

**关键区别**：

- `threads.start()` 创建的线程拥有**独立的 VM、GC 和内存空间**，通过 Channel 通讯，互不干扰
- `ffi.callback()` 注册的回调运行在**外部线程**（如 Windows 线程池、C 库工作线程），这些线程与主 VM **共享同一个执行环境**

**FFI 回调的并行行为**：

```leno
import ffi

cfunc MyCallback(Ptr arg): void

func my_callback(Ptr arg): void {
    // 此回调可能运行在外部线程上
    // 如果多个外部线程同时触发回调，它们会并发访问主 VM 的栈
}

main() {
    var cb = ffi.callback(my_callback, MyCallback)
    // 将 cb 传给 C 库...
}
```

当 FFI 回调被**单个外部线程**顺序调用时，工作正常。但如果被**多个外部线程并发调用**（如 Windows ThreadPool 同时执行多个工作项），由于共享 VM 栈缺乏同步保护，可能出现以下情况：

- 全局变量读写产生竞态条件（结果不确定）
- 触发 GC 时与其他线程冲突（可能导致崩溃）
- VM 栈操作（`sp`、`frame_cnt`）相互覆盖

**这不是 FFI 回调的缺陷，而是两种并行模型的本质差异**：

- `threads.start()` 是 **Leno 管理的协作式/隔离式并行**
- `ffi.callback()` 是 **外部线程对 Leno VM 的侵入式调用**

**建议**：

- 需要 Leno 原生多线程并行 → 使用 `threads.start()` + Channel
- 需要 C 库回调 Leno → 确保回调由**单一线程**顺序触发，或在回调中只进行**无状态/只读操作**
- 不要混用两者：避免在 `ffi.callback()` 中调用 `threads.start()`，或在线程函数中使用 FFI 回调

### 6. print 是线程安全的

单次 `print()` 调用是原子的，不会与其他线程的输出交错。但多次 `print()` 调用之间可能被其他线程插入：

```leno
// 线程1
print("A")  // 原子，不会交错
print("B")  // 原子，但 B 可能在其他线程输出之后

// 线程2
print("C")
```

可能输出：`A C B` 或 `A B C`，但不会出现 `AC` 这样的交错。

### 7. 避免忙等待

使用 `ch.receive()`（阻塞）而非 `ch.try_receive()` + 循环轮询：

```leno
// ❌ 忙等待，浪费 CPU
while not ch.is_closed() or ch.len() > 0 {
    var val = ch.try_receive()
    ...
}

// ✅ 阻塞等待，不浪费 CPU
while true {
    var val = ch.receive()
    if val == null { break }
    ...
}
```

### 8. 线程生命周期

- 线程函数返回后，线程自动结束
- `t.join()` 会阻塞等待线程结束
- 未 `join()` 的线程在主线程退出时可能被强制终止

### 9. 子线程异常与 join() 行为

子线程中的异常**不会自动传播到主线程**，但会在 `join()` 时体现：

- 子线程正常运行：`t.join()` 返回线程函数的返回值
- 子线程抛出异常：`t.state()` 返回 `"error"`，`t.join()` 会在主线程**抛出异常**，错误信息格式为 `"Thread error: <子线程错误信息>"`

```leno
var t = threads.start(func(){
    throw "子线程出错"
})

try {
    t.join()  // 抛出: Thread error: 子线程出错
} catch e {
    print("捕获: " + e)  // 捕获: Thread error: 子线程出错
}
```

**注意：`state()` 是快照，不是锁。** 在 `state()` 和 `join()` 之间存在竞态窗口——线程状态可能在检查后立即改变。因此不建议依赖 `state()` 来决定是否 `join()`，更安全的做法是直接用 `try-catch` 包裹 `join()`：

```leno
// ❌ 依赖 state() 快照，存在竞态窗口
if t.state() == "done" {
    var result = t.join()  // state() 返回后线程可能已变为 "error"
}

// ✅ 直接 try-catch，无竞态问题
try {
    var result = t.join()
} catch e {
    print("线程出错: " + e)
}
```

### 10. threads 与 async 的交互规则

Leno 提供两种并发机制，**不应混用**：

| 机制 | 模型 | 调度方式 | 内存模型 |
|------|------|---------|---------|
| `threads.start()` | OS 线程 + 隔离 VM | 抢占式，真正并行 | 独立内存，Channel 通信 |
| `async func` + `await` | 协程 + 事件循环 | 协作式，单线程内调度 | 共享内存，无数据竞争 |

**交互规则：**

- **不要在子线程中使用 `async`/`await`**：子线程有独立 VM 和事件循环，`asyncs.run()` 不会跨线程调度
- **不要在 `async` 函数中调用 `threads.start()`**：协程是协作式调度，启动 OS 线程会打破协程的执行假设
- **需要 CPU 并行** → 用 `threads.start()` + Channel
- **需要 I/O 并发** → 用 `async func` + `await` + `asyncs.run()`

> 关于 async 的完整边界定义（返回值类型、不 await 行为、异常传播、事件循环语义），参见 `src/module/asyncs/asyncs.md`。

```leno
// ❌ 错误：混用 threads 和 async
async func bad() {
    var t = threads.start(func(){ ... })  // 不要这样做
    await asyncs.sleep(100)
}

// ✅ 正确：选择一种模型
// 方案1：纯线程
func worker(var ch) { ... }
var t = threads.start(worker, ch)

// 方案2：纯协程
async func worker() { ... }
var f = worker()
asyncs.run()
```

### 11. type() 的语义约定

`type()` 返回的是**运行时值的类型**，而非变量的声明类型：

```leno
int? a = null
print(type(a))  // "null"（不是 "int?"）

var b = 42
print(type(b))  // "int"

b = "hello"
print(type(b))  // "string"（类型随值变化）
```

Leno 的变量是动态类型的，声明类型（如 `int`）只是编译期提示，`type()` 反映的是运行时实际值的类型。这意味着：

- `null` 值的 `type()` 始终返回 `"null"`，无论变量声明为什么类型
- BigInt 和普通 int 的 `type()` 都返回 `"int"`（对外统一）
- 子线程中 `type()` 的行为与主线程一致

---

## 线程入口：模块函数也能用（★ 2026-10-12 起）

**模块里的函数现在可以直接当线程入口** ✓ —— 库可以**自己封装并发**：

```leno
// 模块 side（lib/worker.leno）
import threads
export func do_work(int n): int { return n * 2 }
export func run_parallel(int a, int b): int {
    var t1 = threads.start(do_work, a)          // ✓ 模块开线程跑自己的函数
    var t2 = threads.start(do_work, b)
    return t1.join() + t2.join()
}
```

```leno
// 入口脚本
import "./lib/worker.leno" as w
var t = threads.start(w.do_work, 21)            // ✓ 直接拿模块函数当入口
print(t.join())                                 // 42
print(w.run_parallel(1, 2))                     // 6
```

**以前为什么不行**（历史记录，写在这儿免得再踩）：`threads.start` 会硬拦"入口必须是入口脚本
里的具名全局函数"✗，根因是**协程首帧漏设 `frame->module`**，而模块字节码（读写模块级 `var`、
取模块函数）全部经 `frame->module` 寻址 ⇒ 进函数体后第一条模块符号指令就报
「不在模块上下文中」（症状是 `join()` 只给 `Thread error: unknown error`，连函数体都进不去 ✗）。
补上那一行之后模块函数直接可用 ✓（改动与判据见 `docs/线程并发能力修复追踪.md` 的 TH2 ✓）。

⚠ **两个仍然成立的注意点**：

1. **模块级可变 `var` 是跨线程共享的**（模块对象由进程级缓存共享，不按线程隔离 ⇒ 不这么做是有意的，
   见 `docs/线程并发能力修复追踪.md` 的「明确不做」表）。并发写同一个模块级变量就是数据竞争
   —— 库要么把状态收进实例（`_ParseCtx` 那种写法），要么用 `Channel` 同步。
2. **入口函数的实参个数必须严格对上** ✓ —— 线程入口由 VM 直接调用，**默认参数不会被补齐** ✗。
   另：入口函数所属模块**必须在入口脚本的 import 链上**（否则模块的 `var` 还没初始化，
   会明确报「所属的模块尚未初始化」✓）。

## 并行原语（★ 2026-10-12 起）

手写"起 N 个线程 + 收 N 条结果"的样板以前每个项目都要抄一遍 ⇒ 现在有三个现成的模块函数
（**没有新增对象类型 / 关键字**，就是普通函数 ✓ 见 `docs/线程并发能力修复追踪.md` 的 TH3）：

| 原语 | 作用 | 回调形态 | 返回 |
|---|---|---|---|
| `threads.join_all(Array[Thread])` | 等一组线程结束（“WaitGroup”的等价物） | — | `int`（真正等完的个数） |
| `threads.parallel_for(n, f)` | 分 n 份并行：`f(index)` 在 n 个线程上各跑一次，等全部结束 | `f(int index)` | `int`（真正起来的线程数） |
| `threads.map(arr, f)` | 并行 map，结果**按输入顺序**收回 | `f(item, index)` | `Array[回调返回类型]` |

```leno
import threads

// 分 4 份并行；回调是闭包 ⇒ 可以用捕获的 channel 把结果带回来
var ch = threads.channel(8)
var started = threads.parallel_for(4, func(int i) {
    ch.send(i * 10)
})
var sum = 0
for 0 : 3 to var k { sum = sum + _int(ch.receive()) }
print(sum)          // 60（0+10+20+30）

// 并行 map：结果顺序与输入一致，静态类型直接是 Array[int]
var doubled = threads.map([1, 2, 3, 4], func(int x, int idx): int {
    return x * 10 + idx
})
print(doubled[3])   // 43（4*10+3）

// 手工起的线程可以一次等完
var ts = []
ts.add(threads.start(work, 1))
ts.add(threads.start(work, 2))
print(threads.join_all(ts))   // 2
```

⚠ 三条注意：
1. **`n` 要自己控**（`parallel_for` 不会自动按核数分）—— 说几份就几份。
2. **`map` 的线程数 = 元素数**（每个元素一个线程）⇒ 元素多时线程开销会盖过收益；
   重活请用 `parallel_for` 自己分块 ✓
3. **回调的形参个数是硬校验**（线程入口由 VM 直接调用，默认参数不会被补齐 ✓）：
   `parallel_for` 要 `f(int)`，`map` 要 `f(item, index)`（与 `arrays.map` 同口径 ✓）。

## 通道元素类型（`Channel[T]`，★ 2026-10-12 起）

给通道写上元素类型，`receive()` 就**直接是那个类型**，不用再 `_int(...)` / `is` 收窄：

```leno
import threads

struct Pt { int x = 0  string tag = "" }

main() {
    Channel[int] ch = threads.channel(4)   // ← 工厂返回裸 Channel，靠标注拿到元素类型
    ch.send(7)
    int v = ch.receive()                   // ✓ 直接是 int

    Channel[Pt] pc = threads.channel(4)
    var p = new Pt()
    p.x = 9
    pc.send(p)
    Pt got = pc.receive()                  // ✓ 直接是 Pt（跨线程的 struct 是深拷贝）
    print(got.x + v)
}
```

| 写法 | `receive()` 的静态类型 |
|---|---|
| `Channel[int] ch` | `int` ✓ |
| `Channel[Pt] ch` | `Pt` ✓（可再取字段） |
| `var ch = threads.channel(1)`（裸 `Channel`） | `any`（**回落**，仍可用但需 `_int(...)` / 收窄） |
| `ch.try_receive()` | 恒为 `any` ✓ **这是有意的**：空通道它给 `null`，说它是 `T` 就是撒谎（要精确得先有"可空元素"的表示） |

⚠ 两条口径：
1. **元素未知的一侧一律放行** —— 裸 `Channel` 能赋给 `Channel[int]`，`Channel[int]` 也能传给
   形参写成 `Channel` 的函数（运行期本来就不携带元素类型 ⇒ 写窄了只会误报）；
2. `threads.channel(n)` **推不出** T（工厂没有元素信息）⇒ 想看精确类型就**写标注**。
   顺带：`Thread[T]` 标注现在也能写了（`Thread[int] t = threads.start(f, 1)` ✓）。

## 线程局部存储（`threads.local_*`，★ 2026-10-12 起）

**模块级可变 `var` 是所有线程同一份**（故意不隔离 ⇒ 并发写就是数据竞争）。库想"每个线程一份
状态"时，用这三个函数 —— 它们按**线程**隔离，不需要锁：

| 函数 | 说明 |
|---|---|
| `threads.local_get(key, default)` | 本线程存过 `key` ⇒ 给它；没存过 ⇒ 给 `default`（**不写**） |
| `threads.local_has(key)` | 本线程是否存过 `key`（⇒ `"存过 null"` 与 `"没存过"` 是两回事 ✓） |
| `threads.local_set(key, value)` | 只写**本线程**那份 |

```leno
import threads

func worker(int id): string {
    threads.local_set("hits", id * 100)                 // 每个线程各写各的
    var n = _int(threads.local_get("hits", -1))
    threads.local_set("hits", n + 1)
    return _str(_int(threads.local_get("hits", -1)))    // 101
}

main() {
    threads.local_set("hits", 7)
    print(threads.start(worker, 1).join())              // 101
    print(threads.start(worker, 2).join())              // 201
    print(_int(threads.local_get("hits", -1)))          // 7 —— 子线程动不到主线程那份
    print(threads.local_has("nope"))                    // false
}
```

⚠ 三条口径**别混**（各有不同语义）：

| 状态放在哪 | 主/子线程之间 | 并发写 |
|---|---|---|
| **模块级 `var`**（`lib/xxx.leno` 顶层） | **同一份**（共享） | 数据竞争 ✗ ⇒ 要么收进实例，要么用 `threads.local_*` |
| **入口脚本顶层标量**（`var count = 0`） | **各一份**（子线程里重新赋值主线程看不见） | 安全 ✓（但要靠 `join()` 返回值汇总） |
| **入口脚本顶层的对象**（Array / Dict / struct） | 槽各一份，但**对象本身共享** | 改内容会互相看见 ✗ |
| **`threads.local_*`** | **每线程一份**（子线程读不到主线程写的） | 安全 ✓ |

## API 参考

### threads 模块

| 方法 | 说明 | 返回值 |
|------|------|--------|
| `threads.start(func, args...)` | 启动新线程 | `Thread[回调返回]` |
| `threads.channel(capacity)` | 创建通道（裸 `Channel`；要元素类型请写标注） | `Channel` |
| `threads.sleep(ms)` | 当前线程休眠 | `null` |
| `threads.join_all(Array[Thread])` | 等一组线程结束 | `int` |
| `threads.parallel_for(n, f)` | 分 n 份并行（`f(index)`） | `int` |
| `threads.map(arr, f)` | 并行 map（`f(item, index)`，按输入顺序） | `Array[回调返回]` |
| `threads.local_get(key, default)` | 读本线程存的值（没存过给 `default`） | `any` |
| `threads.local_has(key)` | 本线程是否存过该键 | `bool` |
| `threads.local_set(key, value)` | 写本线程的值（其它线程看不见） | `null` |

### Channel 方法（返回类型与元素挂钩）

| 方法 | 说明 | 返回值 |
|------|------|--------|
| `ch.send(v)` | 发送（满则阻塞） | `null` |
| `ch.receive()` | 接收（空则阻塞） | **接收者的元素类型**（`Channel[int]` ⇒ `int`；裸 `Channel` ⇒ `any`） |
| `ch.try_receive()` | 非阻塞接收 | `any`（空通道给 `null` —— 见上表说明） |

### Thread 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `t.join()` | 等待线程结束，获取返回值；子线程出错时抛异常 | ✅ 阻塞 |
| `t.state()` | 获取线程状态：`"running"` / `"done"` / `"error"` | ❌ 不阻塞 |

### Channel 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `ch.send(value)` | 发送值到通道 | 缓冲满时阻塞 |
| `ch.receive()` | 从通道接收值，关闭后返回 null | 缓冲空时阻塞 |
| `ch.try_send(value)` | 非阻塞发送，成功返回 true | ❌ 不阻塞 |
| `ch.try_receive()` | 非阻塞接收，成功返回值，空返回 null | ❌ 不阻塞 |
| `ch.close()` | 关闭通道 | ❌ 不阻塞 |
| `ch.is_closed()` | 检查通道是否已关闭 | ❌ 不阻塞 |
| `ch.len()` | 获取缓冲区消息数量 | ❌ 不阻塞 |
