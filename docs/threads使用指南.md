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

---

## 快速开始

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

### 1. 不支持闭包捕获

`threads.start()` 目前只支持顶层函数，不支持带捕获变量的闭包：

```leno
var x = 10
threads.start(func() {
    print(x)    // ❌ 错误：不支持闭包捕获
})
```

替代方案：通过参数传递

```leno
func worker(var x) {
    print(x)    // ✅ 通过参数传递
}
threads.start(worker, 10)
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

### 5. FFI 回调与原生线程的关系

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

### 6. 线程生命周期

- 线程函数返回后，线程自动结束
- `t.join()` 会阻塞等待线程结束
- 未 `join()` 的线程在主线程退出时可能被强制终止

---

## API 参考

### threads 模块

| 方法 | 说明 | 返回值 |
|------|------|--------|
| `threads.start(func, args...)` | 启动新线程 | `Thread` |
| `threads.channel(capacity)` | 创建通道 | `Channel` |

### Thread 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `t.join()` | 等待线程结束，获取返回值 | ✅ 阻塞 |

### Channel 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `ch.send(value)` | 发送值到通道 | 缓冲满时阻塞 |
| `ch.receive()` | 从通道接收值，关闭后返回 null | 缓冲空时阻塞 |
| `ch.try_send(value)` | 非阻塞发送，成功返回 true | ❌ 不阻塞 |
| `ch.try_receive()` | 非阻塞接收，成功返回值，空返回 null | ❌ 不阻塞 |
| `ch.close()` | 关闭通道 | ❌ 不阻塞 |
