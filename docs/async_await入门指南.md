# Leno Async/Await 入门指南

## 简介

Leno 支持 `async`/`await` 协程，让你能够轻松编写异步代码，实现并发执行而无需处理复杂的线程管理。

---

## 核心概念

| 概念 | 说明 |
|------|------|
| `async` | 标记一个函数为异步函数，可以暂停执行 |
| `await` | 暂停当前协程，等待异步操作完成 |
| `Future` | 异步操作的结果占位符 |
| `asyncs.run()` | 启动事件循环，执行所有协程 |

---

## 快速开始

### 1. 基本的 Sleep

```leno
import io
import asyncs

async func demo() {
    io.print("开始")
    await asyncs.sleep(1000)    // 暂停 1 秒
    io.print("1秒后")
}

main() {
    demo()           // 启动协程
    asyncs.run()     // 运行事件循环
}
```

输出：
```
开始
(等待1秒)
1秒后
```

---

### 2. async 函数返回值

```leno
async func fetch_data(string name):string {
    await asyncs.sleep(500)
    return $"数据: {name}"
}

async func test() {
    var result = await fetch_data("测试")
    io.print(result)    // 输出: 数据: 测试
}

main() {
    test()
    asyncs.run()
}
```

---

### 3. 并发执行

同时启动多个任务，总耗时 = 最慢的那个任务：

```leno
async func task(string name, int ms) {
    await asyncs.sleep(ms)
    io.print($"任务 {name} 完成")
}

main() {
    // 同时启动三个任务
    task("A", 1000)   // 1秒
    task("B", 500)    // 0.5秒
    task("C", 1500)   // 1.5秒
    
    asyncs.run()      // 总耗时约 1.5 秒，不是 3 秒
}
```

输出顺序：
```
任务 B 完成   (0.5秒)
任务 A 完成   (1秒)
任务 C 完成   (1.5秒)
```

---

### 4. 等待多个任务

```leno
async func test() {
    var f1 = task("A", 300)
    var f2 = task("B", 200)
    var f3 = task("C", 400)
    
    // 先等待所有任务完成
    await f1
    await f2
    await f3
    
    // 然后用 all() 收集结果
    var results = asyncs.all([f1, f2, f3])
    io.print(results[0])
    io.print(results[1])
    io.print(results[2])
}
```

---

### 5. 超时控制

```leno
async func slow_task():string {
    await asyncs.sleep(5000)   // 5秒
    return "完成"
}

async func test() {
    // 最多等 1 秒
    var result = await asyncs.timeout(slow_task(), 1000)
    
    if result is null {
        io.print("超时了！")
    } else {
        io.print(result)
    }
}
```

---

### 6. 主动让出执行权

使用 `yield()` 让其他协程先运行：

```leno
async func task_a() {
    io.print("A: 步骤1")
    await asyncs.yield()     // 让出执行权
    io.print("A: 步骤2")
}

async func task_b() {
    io.print("B: 步骤1")
    await asyncs.yield()
    io.print("B: 步骤2")
}

main() {
    task_a()
    task_b()
    asyncs.run()
}
```

输出：
```
A: 步骤1
B: 步骤1
A: 步骤2
B: 步骤2
```

---

## 完整示例

### 并发下载模拟

```leno
import io
import asyncs

async func download(string url, int delay):string {
    io.print($"开始下载: {url}")
    await asyncs.sleep(delay)
    return $"{url} 下载完成"
}

async func main_task() {
    // 同时下载三个文件
    var f1 = download("file1.txt", 1000)
    var f2 = download("file2.txt", 500)
    var f3 = download("file3.txt", 800)
    
    // 等待全部完成
    await f1
    await f2
    await f3
    
    var results = asyncs.all([f1, f2, f3])
    
    io.print("\n所有下载完成:")
    io.print(results[0])
    io.print(results[1])
    io.print(results[2])
}

main() {
    io.print("=== 并发下载测试 ===")
    main_task()
    asyncs.run()
    io.print("=== 完成 ===")
}
```

输出：
```
=== 并发下载测试 ===
开始下载: file1.txt
开始下载: file2.txt
开始下载: file3.txt

所有下载完成:
file1.txt 下载完成
file2.txt 下载完成
file3.txt 下载完成
=== 完成 ===
```

---

## asyncs 模块 API

| 函数 | 说明 | 示例 |
|------|------|------|
| `asyncs.sleep(ms)` | 暂停指定毫秒 | `await asyncs.sleep(1000)` |
| `asyncs.run()` | 启动事件循环 | `asyncs.run()` |
| `asyncs.all([f1,f2])` | 收集已完成 Future 的结果 | `asyncs.all([f1, f2])` |
| `asyncs.timeout(f, ms)` | 超时等待 | `await asyncs.timeout(f, 500)` |
| `asyncs.yield()` | 主动让出执行权 | `await asyncs.yield()` |

---

## 重要规则

### 1. await 只能在 async 函数中使用

```leno
func normal() {
    await asyncs.sleep(100)   // ❌ 错误！
}

async func async_fn() {
    await asyncs.sleep(100)   // ✅ 正确
}
```

### 2. 必须调用 asyncs.run()

```leno
main() {
    async_fn()         // 只创建协程，不会执行
    // 忘记调用 asyncs.run() → 协程不会运行
}

main() {
    async_fn()
    asyncs.run()       // ✅ 启动事件循环
}
```

### 3. async 函数返回 Future

```leno
async func task():string {
    return "结果"
}

main() {
    var future = task()     // 返回 Future，不是 string
    await future            // await 后才能获取 string
}
```

### 4. 普通函数中调用 async 函数

普通函数调用 async 函数时，返回的是 `Future` 对象而非值。这是完全合法的模式——普通函数充当"中间人"传递 Future，由调用方的 async 函数负责 `await`：

```leno
// 普通函数：创建并返回 Future，不做 await
func make_future(int x) {
    async func worker(int v): int {
        await asyncs.sleep(10)
        return v * 5
    }
    return worker(x)      // 返回 Future，不是 int
}

async func caller(int x): int {
    var f = make_future(x)  // 普通函数调用，得到 Future
    var result = await f    // 在 async 函数中 await
    return result
}
```

> **⚠️ 注意：普通函数不能使用 `await`**
>
> ```leno
> func normal() {
>     var result = await some_async()   // ❌ 编译错误：await 只能在 async 函数中使用
> }
> ```
>
> 如果需要在普通函数中等待异步结果，将其改为 `async func`，或者在 `main()` 中调用 `asyncs.run()` 后通过 Future 获取结果。

### 5. async 函数直接返回另一个 async 调用

当 async 函数直接 `return` 另一个 async 函数的调用时，返回的是**嵌套 Future**（Future 中包含另一个 Future）。需要两次 `await` 才能拿到最终值：

```leno
async func inner(int v): int {
    await asyncs.sleep(5)
    return v + 100
}

async func wrap(): var {
    return inner(42)       // 不 await，直接返回 Future
}

async func test() {
    var f = wrap()          // f 是 Future（其结果是另一个 Future）
    var inner_f = await f    // 第一次 await：得到 inner 的 Future
    var result = await inner_f  // 第二次 await：得到最终值 142
    assert_eq(result, 142)
}
```

> **💡 为什么需要两次 await？**
>
> `wrap()` 返回的是它自己的 `task_future`。当 `wrap` 完成时，`task_future` 的结果是 `inner(42)` 的返回值——但 `inner` 是 async 函数，它的返回值就是另一个 `Future`。因此第一次 `await f` 得到的是 `inner` 的 Future，第二次 `await` 才拿到真正的值。
>
> 如果不希望嵌套，在 `wrap` 中使用 `return await inner(42)` 即可一步返回值。

### 6. async 函数中的局部变量在 await 后保持

协程挂起时，VM 会保存该协程的**所有调用帧**（不只是顶层帧）。这意味着 `await` 前后，局部变量完整保持：

```leno
async func test(): int {
    var a = 100
    var b = 200
    await asyncs.sleep(10)   // 协程挂起，所有帧被保存
    // 恢复后，a 和 b 的值完整保持
    return a + b             // 300
}
```

这一保证也适用于嵌套调用链中的中间帧：

```leno
async func child(int x): int {
    await asyncs.sleep(5)
    return x * 8 + 10
}

async func parent(int x): int {
    var parent_local = x * 3
    var c_result = await child(x)   // parent 的帧被保存
    return parent_local + c_result   // parent_local 保持不变
}
```

以及结构体值类型：

```leno
struct Point3D {
    int x = 0
    int y = 0
    int z = 0
}

async func test(): string {
    var p = new Point3D(x=3, y=4, z=7)
    await asyncs.sleep(5)           // p 存储在帧的 locals 中，被完整保存
    return $"{p.x},{p.y},{p.z}"     // "3,4,7" — 字段完整保持
}
```

---

## 异常处理

async/await 中的异常可以使用 `try-catch` 捕获：

### 捕获异步异常

```leno
import io
import asyncs

async func risky():string {
    await asyncs.sleep(100)
    throw "出错了"
    return "不会执行"
}

async func safe_catch() {
    try {
        var result = await risky()
        io.print($"成功: {result}")
    } catch e {
        io.print($"捕获异常: {e}")
    }
}

main() {
    safe_catch()
    asyncs.run()
}
```

输出：
```
捕获异常: 出错了
```

### 异常传播规则

1. **未捕获的异常会传播**：async 函数中抛出的异常，如果没有被该函数内部的 try-catch 捕获，会通过 Future 传播给调用者
2. **await 会重新抛出异常**：当 await 一个失败的 Future 时，异常会在 await 处重新抛出
3. **非 async 函数调用 async 函数**：如果在普通函数中调用 async 函数（不使用 await），异常会在事件循环中报告

```leno
async func inner() {
    throw "内部错误"
}

async func outer() {
    try {
        await inner()    // 异常在这里重新抛出
    } catch e {
        io.print($"捕获: {e}")
    }
}
```

---

## 常见问题

**Q: 为什么我的协程没有执行？**

A: 确保调用了 `asyncs.run()`。

**Q: asyncs.all() 返回 null？**

A: `all()` 只收集已完成的 Future。先用 `await` 等待每个 Future，再调用 `all()`。

**Q: 如何实现真正的并行？**

A: Leno 的协程是单线程的，通过快速切换实现"伪并行"。对于 CPU 密集型任务，建议使用多进程。

**Q: async 函数中的异常为什么被吞掉了？**

A: 确保使用 `await` 等待 async 函数，并用 `try-catch` 捕获异常。如果不使用 await，异常会在事件循环中报告。

---

## 更多示例

查看 `test/` 目录下的示例文件：
- `test/async await/test_async.leno` - 综合测试
- `test_async_yield.leno` - yield 测试
- `test_async_all.leno` - all 测试
- `test_async_timeout.leno` - timeout 测试
- `examples/sockets/test_async_arecv_aaccept.leno` - async socket 测试
- `examples/sockets/test_async_stress.leno` - async socket 压力测试

## 网络 IO 异步

`sockets` 模块提供了异步方法 `sock.arecv()` 和 `sock.aaccept()`，配合 `await` 使用时让出执行权给事件循环，实现真正的并发网络服务器。详见 [module_sockets.md](../docs/module_sockets.md)。
