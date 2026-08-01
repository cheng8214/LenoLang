# LenoC 异步协程模块 (asyncs)

本文档详细说明 `asyncs` 模块提供的异步编程和协程功能。

## 目录

- [使用方式](#使用方式)
- [核心概念](#核心概念)
- [模块函数](#模块函数)
- [协程控制](#协程控制)
- [并发工具](#并发工具)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import asyncs
import io

// 定义异步函数
async func fetch_data():string {
    await asyncs.sleep(100)  // 模拟异步操作
    return "数据获取完成"
}

main() {
    // 启动异步任务
    var task = fetch_data()
    
    // 运行事件循环，开始执行所有协程
    asyncs.run()
}
```

---

## 核心概念

### async/await

LenoC 使用 `async` 和 `await` 关键字实现协程：

- **`async func`** - 定义异步函数，返回一个 Future 对象
- **`await`** - 等待异步操作完成，让出执行权

```leno
async func my_task():int {
    io.print("任务开始")
    await asyncs.sleep(100)  // 等待100ms，期间其他协程可以运行
    io.print("任务结束")
    return 42
}
```

### Future

Future 是异步函数的返回值，代表一个尚未完成的计算：

```leno
var future = my_task()  // 创建 Future，但任务还未执行

// 使用 await 等待 Future 完成
var result = await future  // 阻塞直到任务完成，返回结果
```

### 事件循环

使用 `asyncs.run()` 启动事件循环，执行所有待处理的协程：

```leno
main() {
    // 创建多个异步任务
    task1()
    task2()
    task3()
    
    // 启动事件循环
    asyncs.run()  // 阻塞直到所有协程完成
}
```

---

## 模块函数

### `sleep(ms)`

暂停当前协程指定毫秒数，让出执行权给其他协程。

**参数**:
- `ms` (int): 休眠时间（毫秒）

**返回**: `null`

```leno
async func task() {
    io.print("开始")
    await asyncs.sleep(500)  // 暂停500ms
    io.print("500ms后继续")
}
```

---

### `yield()`

主动让出执行权，允许其他协程运行，但自身保持可执行状态。

**参数**: 无  
**返回**: `null`

```leno
async func cooperative_task() {
    for 0:5 to i {
        io.print("步骤 " + i)
        await asyncs.yield()  // 让出执行权
    }
}
```

**与 sleep 的区别**:
- `sleep(ms)` - 暂停指定时间后才恢复
- `yield()` - 立即让出，下次调度时恢复

---

### `run()`

启动事件循环，执行所有待处理的协程直到全部完成。

**参数**: 无  
**返回**: `null`

```leno
main() {
    // 创建协程
    async_task1()
    async_task2()
    
    // 启动事件循环
    asyncs.run()
    
    io.print("所有协程完成")
}
```

**注意**: `run()` 会阻塞直到所有协程完成。

---

### `current()`

获取当前正在运行的协程 ID。

**参数**: 无  
**返回**: `int` - 协程 ID

```leno
async func show_id() {
    var id = asyncs.current()
    io.print("当前协程 ID: " + id)
}
```

---

### `is_done(future)`

检查 Future 是否已完成。

**参数**:
- `future`: 要检查的 Future 对象

**返回**: `bool` - 是否已完成

```leno
async func check_status() {
    var task = some_async_task()
    
    // 做一些其他工作...
    await asyncs.yield()
    
    if (asyncs.is_done(task)) {
        io.print("任务已完成")
    } else {
        io.print("任务仍在进行中")
    }
}
```

---

### `get_result(future)`

获取已完成 Future 的结果。如果未完成，返回 `null`。

**参数**:
- `future`: 已完成的 Future 对象

**返回**: 异步函数的返回值，或 `null`（如果未完成）

```leno
async func get_result_demo() {
    var task = fetch_data()
    
    // 等待任务完成
    await task
    
    // 获取结果
    var result = asyncs.get_result(task)
    io.print("结果: " + result)
}
```

**注意**: 必须先使用 `await` 确保 Future 完成，再调用 `get_result()`。

---

## 并发工具

### `all(futures)`

收集多个已完成 Future 的结果。

**参数**:
- `futures` (array): Future 对象数组

**返回**: `array` - 所有 Future 的结果数组

```leno
async func fetch_user(id):dict {
    await asyncs.sleep(100)
    return {id: id, name: "用户" + id}
}

async func fetch_all_users() {
    // 启动多个并发请求
    var f1 = fetch_user(1)
    var f2 = fetch_user(2)
    var f3 = fetch_user(3)
    
    // 等待所有 Future 完成
    await f1
    await f2
    await f3
    
    // 收集所有结果
    var users = asyncs.all([f1, f2, f3])
    
    for users to user {
        io.print(user.name)
    }
}
```

**使用步骤**:
1. 启动多个异步任务，获取 Future
2. 使用 `await` 等待每个 Future 完成
3. 使用 `all()` 收集所有结果

---

### `timeout(future, ms)`

为 Future 设置超时。

**参数**:
- `future`: 要等待的 Future
- `ms` (int): 超时时间（毫秒）

**返回**: 
- 成功: Future 的结果
- 超时: `null`

```leno
async func slow_operation():string {
    await asyncs.sleep(2000)  // 2秒
    return "完成"
}

async func with_timeout() {
    var task = slow_operation()
    
    // 设置1秒超时
    var result = await asyncs.timeout(task, 1000)
    
    if (result != null) {
        io.print("任务成功: " + result)
    } else {
        io.print("任务超时")
    }
}
```

---

## 示例代码

### 1. 基础异步任务

```leno
import asyncs
import io

// 模拟网络请求
async func fetch_url(string url):string {
    io.print("请求: " + url)
    await asyncs.sleep(200)  // 模拟网络延迟
    return "响应: " + url
}

main() {
    // 顺序执行（会阻塞等待）
    var result1 = fetch_url("api/users")
    var result2 = fetch_url("api/posts")
    
    io.print(await result1)
    io.print(await result2)
    
    asyncs.run()
}
```

---

### 2. 并发请求

```leno
import asyncs
import io

async func fetch_user(int id):dict {
    await asyncs.sleep(100)
    return {id: id, name: "用户" + id}
}

async func fetch_concurrent() {
    io.print("开始并发请求...")
    
    // 同时启动多个请求
    var f1 = fetch_user(1)
    var f2 = fetch_user(2)
    var f3 = fetch_user(3)
    
    // 等待所有请求完成
    await f1
    await f2
    await f3
    
    // 收集结果
    var users = asyncs.all([f1, f2, f3])
    
    io.print("获取到 " + users.len() + " 个用户")
    for users to user {
        io.print("  - " + user.name)
    }
}

main() {
    fetch_concurrent()
    asyncs.run()
}
```

---

### 3. 超时处理

```leno
import asyncs
import io

async func unreliable_task():string {
    // 随机延迟 50-300ms
    await asyncs.sleep(200)
    return "任务完成"
}

async func safe_task() {
    var task = unreliable_task()
    
    // 设置100ms超时
    var result = await asyncs.timeout(task, 100)
    
    if (result == null) {
        io.print("操作超时，使用默认值")
        result = "默认值"
    }
    
    return result
}

main() {
    safe_task()
    asyncs.run()
}
```

---

### 4. 协程间协作

```leno
import asyncs
import io

var shared_counter = 0

async func increment(string name) {
    for 0:3 to i {
        shared_counter = shared_counter + 1
        io.print(name + " 增加到: " + shared_counter)
        await asyncs.yield()  // 让出执行权
    }
}

main() {
    // 两个协程同时操作共享数据
    increment("协程A")
    increment("协程B")
    
    asyncs.run()
    
    io.print("最终结果: " + shared_counter)
}
```

---

### 5. 异步递归

```leno
import asyncs
import io

// 异步递归遍历
async func async_walk(var items, int depth) {
    for items to item {
        var indent = "  ".rep(depth)
        io.print(indent + item.name)
        
        if (item.children.len() > 0) {
            await asyncs.yield()
            await async_walk(item.children, depth + 1)
        }
    }
}

main() {
    var tree = [
        {name: "A", children: [
            {name: "A1", children: []},
            {name: "A2", children: [
                {name: "A2-1", children: []}
            ]}
        ]},
        {name: "B", children: [
            {name: "B1", children: []}
        ]}
    ]
    
    async_walk(tree, 0)
    asyncs.run()
}
```

---

### 6. 批量处理带进度

```leno
import asyncs
import io

async func process_item(var item, int index):string {
    // 模拟处理
    await asyncs.sleep(50)
    return "已处理: " + item
}

async func batch_process(var items) {
    var total = items.len()
    var futures = []
    
    io.print("开始批量处理 " + total + " 项...")
    
    // 启动所有任务
    for items to item {
        futures.add(process_item(item, 0))
    }
    
    // 等待所有任务完成
    for futures to f {
        await f
    }
    
    // 收集结果
    var results = asyncs.all(futures)
    
    io.print("完成! 处理了 " + results.len() + " 项")
    return results
}

main() {
    var data = ["任务1", "任务2", "任务3", "任务4", "任务5"]
    batch_process(data)
    asyncs.run()
}
```

---

## 注意事项

1. **必须在 main 中调用 `run()`**
   - 创建的所有协程都等待 `run()` 启动
   - `run()` 会阻塞直到所有协程完成

2. **`await` 只能在 `async func` 中使用**
   ```leno
   async func valid() {
       await asyncs.sleep(100)  // ✅ 正确
   }
   
   func invalid() {
       await asyncs.sleep(100)  // ❌ 错误，普通函数不能用 await
   }
   ```

3. **Future 必须先 await 再获取结果**
   ```leno
   var f = async_task()
   await f                    // 必须先等待
   var result = asyncs.all([f])  // 然后才能收集结果
   ```

4. **协程调度**
   - 协程是协作式调度，不会抢占
   - 长时间运行的任务应主动 `yield()`
   - `sleep()` 会自动让出执行权

5. **异常处理**
   - 协程内异常有三种传播路径（见下方「异常传播路径」）
   - 未被捕获的异常会被 `asyncs.run()` 收集并抛出

6. **内存管理**
   - Future 对象会被自动回收
   - 避免创建过多未完成的协程

7. **与同步代码混用**
   - `asyncs.run()` 是同步阻塞的
   - 可以在普通函数中调用 `run()`

---

## 边界行为定义

### async 函数的返回值类型

`async func` 调用后返回一个 **Future 对象**，不是直接返回函数结果：

```leno
async func compute():int {
    await asyncs.sleep(100)
    return 42
}

var f = compute()   // f 是 Future，不是 42
print(type(f))      // "object"（Future 对象）

var result = await f  // await 获取实际结果
print(result)         // 42
```

- `async func` 的返回类型注解（如 `:int`）描述的是 Future 完成后的值类型
- 不使用 `await` 时，Future 不会自动执行——它只是被注册到事件循环等待调度

### 不 await 时的行为

如果创建了 Future 但不 `await`，协程仍会在 `asyncs.run()` 时执行，但结果会被丢弃：

```leno
async func task() {
    io.print("执行了")
    return 42
}

main() {
    task()          // 创建 Future，不 await
    asyncs.run()    // 协程仍会执行，输出 "执行了"
    // 但返回值 42 无法获取
}
```

- 不 `await` 不会阻止协程执行，只是无法获取结果
- 如果协程有副作用（如修改全局变量），即使不 `await` 也会生效

### 异常传播路径

协程异常有三种传播路径，与线程模块的异常传播设计对齐：

#### 路径1：协程内部 try-catch 捕获

协程内使用 `try-catch` 直接捕获异常，最安全的方式：

```leno
async func safe_task() {
    try {
        await asyncs.sleep(100)
        throw "错误"
    } catch e {
        io.print("协程内捕获: " + e)
        return null  // 返回默认值
    }
}
```

#### 路径2：await 失败的 Future 时传播

当被 await 的协程抛出异常时，异常通过 Future 传播到 await 方：

```leno
async func risky() {
    throw "子协程出错"
}

async func caller() {
    try {
        var result = await risky()  // await 检测到 Future 错误，抛出异常
    } catch e {
        io.print("await 捕获: " + e)  // 输出: await 捕获: 子协程出错
    }
}
```

- 异常通过 `Future.error` 传播，`await` 检测到错误时在当前协程抛出异常
- 已通过 await 传播的异常不会被 `asyncs.run()` 重复收集

#### 路径3：asyncs.run() 收集未捕获异常

没有被任何 try-catch 或 await 捕获的异常，会被 `asyncs.run()` 收集并在主线程抛出：

```leno
async func risky() {
    throw "未捕获异常"
}

main() {
    risky()          // 创建协程，不 await
    try {
        asyncs.run()  // 协程失败，run() 收集异常并抛出
    } catch e {
        io.print("run() 捕获: " + e)  // 输出: run() 捕获: 未捕获异常
    }
}
```

- 单个协程失败：`run()` 抛出该协程的异常
- 多个协程失败：`run()` 抛出汇总异常，格式为 `"N 个协程失败: 错误1; 错误2; ..."`
- 可以用 `try-catch` 包裹 `asyncs.run()` 来处理

#### 异常传播优先级

```
协程内 try-catch > await 传播 > asyncs.run() 收集
```

- 如果协程内部有 try-catch，异常在内部处理，不传播
- 如果协程被 await，异常通过 Future 传播给等待者，`run()` 不重复收集
- 如果异常未被任何方式捕获，`run()` 统一收集并抛出

### asyncs.run() 的事件循环语义

`asyncs.run()` 的行为：

- **单次运行**：调用后阻塞，直到所有协程完成（就绪队列和定时器队列都为空）后返回
- **不能嵌套**：不支持在协程内部再次调用 `asyncs.run()`
- **阻塞主线程**：`run()` 返回前，主线程代码不会继续执行

```leno
main() {
    task1()   // 注册协程
    task2()   // 注册协程
    
    asyncs.run()  // 阻塞，直到 task1 和 task2 都完成
    
    io.print("所有协程完成")  // run() 返回后才执行
}
```

事件循环的调度顺序：
1. 检查并处理到期的定时器（将协程加入就绪队列）
2. 从就绪队列取出协程执行，直到协程挂起（`await`）或完成
3. 如果没有就绪协程但有定时器，休眠到最近的定时器触发时间
4. 就绪队列和定时器队列都为空时，退出循环

### async 与 threads 的交互

`async` 和 `threads` 是两套独立的并发模型，**不应混用**：

| 特性 | async 协程 | threads 线程 |
|------|-----------|-------------|
| 执行模型 | 协作式调度，单线程 | 抢占式调度，OS 线程 |
| 内存模型 | 共享 VM 内存 | 隔离 VM，Channel 通信 |
| 适用场景 | I/O 并发、异步等待 | CPU 并行、真正多线程 |

- **不要在 `async` 函数中调用 `threads.start()`**：协程是协作式调度，启动 OS 线程会打破执行假设
- **不要在子线程中使用 `async`/`await`**：子线程有独立 VM，`asyncs.run()` 不会跨线程调度
- 需要 CPU 并行 → 用 `threads.start()` + Channel
- 网络 I/O → 使用 `socket.arecv()`/`socket.aaccept()` 配合 await，见 [sockets 模块文档](module_sockets.md#异步-io-arecv--aaccept)
需要 I/O 并发 → 用 `async func` + `await` + `asyncs.run()`

---

## 性能提示

| 场景 | 建议 |
|------|------|
| I/O 操作 | 使用 `sleep()` 模拟，适合网络/文件操作 |
| CPU 密集型 | 分割任务，定期 `yield()` |
| 大量并发 | 控制并发数量，避免资源耗尽 |
| 超时控制 | 使用 `timeout()` 防止无限等待 |

---

## 完整 API 速查表

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sleep(ms)` | 毫秒数 | null | 暂停协程 |
| `yield()` | 无 | null | 主动让出 |
| `run()` | 无 | null | 启动事件循环 |
| `current()` | 无 | int | 获取协程 ID |
| `is_done(f)` | Future | bool | 检查是否完成 |
| `get_result(f)` | Future | any | 获取结果 |
| `all(fs)` | Future[] | any[] | 收集结果 |
| `timeout(f, ms)` | Future, int | any | 超时等待 |

---

*文档版本: 1.1*  
*最后更新: 2026-08-01*
