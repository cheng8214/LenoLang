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
