# Leno 异步协程模块 (asyncs)

本文档详细说明 `asyncs` 模块提供的异步编程和协程功能。

## 目录

- [使用方式](#使用方式)
- [核心概念](#核心概念)
- [模块函数](#模块函数)
- [协程控制](#协程控制)
- [并发工具](#并发工具)
- [示例代码](#示例代码)
- [注意事项](#注意事项)
- [性能提示](#性能提示)
- [实战：并发网络客户端](#实战并发网络客户端2026-11-14)

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

Leno 使用 `async` 和 `await` 关键字实现协程：

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

> ⚠ **不能在协程内部调用**（2026-11-16 起**明确拦下**）。此前只是"跑坏"：
> 在 `async func` 里调 `run()` 实测两种表现 —— ① 0xC0000005 访问违例崩溃；
> ② 不崩，但 `run()` 返回后 `current_coroutine` 被清空 ⇒ 后续 `await` 报
> 「await 只能在 async 函数中使用」（**指错真因**，代码明明在 async 函数里）。
> 现在 `run()` / `run_for()` 都会抛
> `asyncs.run() 不能在协程内部调用：事件循环不能嵌套…`。
> 正确做法：让协程先返回（需要结果就 `await`），再在 `main` 里驱动一次。

---

> **实现状态**：下面这三个查询 API（`current()` / `is_done()` / `get_result()`）
> 此前**两个引擎都没实现**（`asyncs` 注册表里只有 `sleep / run / yield / all / timeout`）。
> 寄存器式已按本文档补齐（含协程 ID）；栈式侧尚未，详见
> `docs/寄存式与栈式的差异清单.md` 的 **A6**。

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

收集多个**已完成** Future 的结果。

**参数**:
- `futures` (array): Future 对象数组 —— ⚠ 里面**非 Future 的元素原样带过**（有意用法 ✓）

**返回**: `array` - 所有 Future 的结果数组

> **★ 失败与未完成：当场抛错，不给 `null`**（2026-10-12 收口，见 `待办与路线图.md` §T4）
>
> | 情形 | 行为 |
> |---|---|
> | 某个 Future **还没完成** | **抛错**（C10，2026-10-10 起）—— 文案里给两条正确做法 ✓ |
> | 某个 Future **失败** | **抛错**（C8，2026-10-12 起）—— 不再是 `[null]` ✗ |
> | **1 个**失败 | 抛**原错误值**（try-catch 拿到的就是原来那个值/类型 ✓，与 `asyncs.run()` 的单错分支同口径 ✓） |
> | **多个**失败 | 抛**汇总串**：`asyncs.all：N 个 Future 失败 ⇒ 第 2 个: …; 第 4 个: …` ✓ |
> | 想**逐个**处理失败 | 改用 `is_done(f)` + `get_result(f)` 自己遍历 ✓ |
>
> 为什么（C8 的由来）：失败的 Future 也是"已完成"（`is_done` 为 true）、`result` 是 `null`
> ⇒ 过去 `all([失败f])` 返回 `[null]`，**与"成功但值是 null"无法区分** ✗，而错误要拖到
> `asyncs.run()` 才炸（甚至跑到别处才炸 ✗）—— 典型的静默错值 ✓ 已消灭 ✓
>
> ⚠ **`all` 本身不会等待**（native 调用没有协程挂起能力）：它只收**已完成**的结果 ⇒
> 要等齐请先 `await` 或让 `asyncs.run()` 跑完（这也是"未完成就抛错"那条存在的原因 ✓）。
> 判据：`assert/test_asyncs_all_errors.leno` ✓
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

> ⚠ **2026-10-10 起**：数组里还有**没完成**的 Future ⇒ `all()` **抛错** ✓（不再静默给 `null` ✗）。
> 原文案：`asyncs.all：第 N 个 Future **还没完成** ⇒ 拿到的会是 null，不是你要的值 ✗ …`
> （给出两种正确做法：先 `await` / 先 `asyncs.run()` 跑完；或改用
> `asyncs.is_done(f)` + `asyncs.get_result(f)` 逐个查 ✓）
> **为什么改**：原来那个 `null` 会被读成"协程没跑"✗，而真因是"没等它"✓ ——
> 静默错值比报错难查得多 ✓。回归钉在 `assert/test_semantics_pins.leno` 的 **P30** ✓。

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

### 2.5 并发上限与超时（2026-10-20 实战修正）

**并发上限**：异步 I/O 表已从"固定 64 项"改为**动态扩容**；每轮 `select` 最多装
`FD_SETSIZE`（已放宽到 **1024**）个 socket，超出的用**环形游标**排到下一轮
（不会因为前 1024 个长期不可读而饿死后面的）。
⇒ 单进程同时挂起的 `arecv`/`aaccept` 实测 **70 个并发正常**（`assert\test_async_io_limit.leno`）。

**异步 connect**：`sockets.aconnect(host, port)`（2026-10-20 起）—— 非阻塞 connect，
在 async 函数里 `await` 得到 `Socket|null`。`sockets.connect` 是**阻塞**的，在协程里会卡住
整个事件循环，高并发客户端请用它：

```leno
var raw = await sockets.aconnect("127.0.0.1", 8080)
if raw != null {
    var c = raw as Socket        // ⚠ 返回的 Future 未标注元素类型 ⇒ await 出来是 any
    c.send("hello")
    var r = await c.arecv(1024)
}
```

**全局超时**：`asyncs.run_for(ms) -> bool`（2026-10-20 起）—— `run()` 会等**所有**协程结束，
一个挂在永不完成 Future 上的协程就能让它永不返回。`run_for` 最多跑 `ms` 毫秒，
超时返回 `false`，协程 / 定时器 / IO 状态**全部保留**（可再次 run），`ms <= 0` 等价 `run()`。

**`timeout(future, ms)` 会取消底层 IO**：超时后不仅 Future 变成 `null`，
`sockets` 里对应的 `arecv`/`aaccept` 请求也会被取消并释放缓冲区。
（修复前只是"假装完成"，请求还挂在事件循环里 ⇒ `run()` 永不返回，整个程序卡死。）

**两条实战纪律**：

1. `async func` 的函数体要等 `asyncs.run()` 才开始执行 ⇒ 想在主线程里用到它的结果
   （比如"先 listen 再起客户端"），**监听器要在主线程同步建好**，再作为参数传进协程。
   在主线程里 `ch.receive()` 等协程发信号会**先死锁**。
2. 协程里调用**阻塞 API**（`sockets.connect` / `sockets.recv` / 文件 IO）会卡住整个事件循环
   —— 网络收发要么用 `aaccept`/`arecv`，要么放线程池里做。

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
- ★ **时序也要成立**（2026-11-16 修）：协程如果是在"没人等的窗口"里失败的
  （失败那一刻 `Future.waiter` 还是 NULL），错误会**立刻**进 `asyncs.run()` 的待抛列表；
  之后调用方才 `await` 这个已完成的 Future ⇒ 那条错误已被消费，现在会从列表里**移除**
  ⇒ `run()` 结尾不再重复抛同一错误。`asyncs.all` 抛出的错误同样会被消费。
  （修前实测：`var bad = boom(); await ok; try { await bad } catch { }` 之后 `run()`
  仍抛一次 `炸` ✗）

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

- **单次运行**：调用后阻塞，直到以下条件全部满足后返回：
  1. 就绪队列为空（无待执行的协程）
  2. 定时器队列为空（无待触发的定时器）
  3. 无待处理的异步 I/O 请求（无 `arecv()`/`aaccept()` 在等待）
- **不能嵌套**：在协程内部再次调用 `asyncs.run()` / `run_for()` ⇒ **抛错**
  （2026-11-16 起拦下；此前是崩溃或错位报错，见上方的「注意」）
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
3. 如果没有就绪协程但有定时器或待处理的异步 I/O，通过 `select()` 等待 I/O 就绪或定时器触发
4. 就绪队列、定时器队列都为空且无待处理异步 I/O 时，退出循环

> **说明**: 使用 `sock.arecv()` 或 `sock.aaccept()` 等异步 I/O 时，无需额外的"保活"协程。事件循环会自动检测待处理的 I/O 请求并保持活跃直到完成。

### async 与 threads 的交互

`async` 和 `threads` 是两套**各自独立的调度器**，但可以**分工协作**：协程负责"等"，
线程负责"算"，中间用 `Channel` 交接。

| 特性 | async 协程 | threads 线程 |
|------|-----------|-------------|
| 执行模型 | 协作式调度，单线程 | 抢占式调度，OS 线程 |
| 内存模型 | 共享 VM 内存 | 隔离 VM，Channel 通信 |
| 适用场景 | I/O 并发、异步等待 | CPU 并行、真正多线程 |

- **子线程里不能用 `async`/`await`**：`current_exec_vm` 为空，`asyncs.run()` 会直接报错
  （「事件循环是每进程一个」）——这是硬约束。
- **协程里可以 `threads.start()`**（实测可用），但线程的**阻塞操作**（`join()` / 空的
  `receive()`）会卡住整条事件循环 ⇒ 用 `try_receive()` + `await asyncs.sleep(1)` 轮询，
  拿到结果再 `join()`：

```leno
func worker(any ch, int n) {
    threads.sleep(30)                       // 模拟 CPU 活
    if ch is Channel { ch.send(n * 2) }
}

async func await_channel(Channel ch) {
    var got = null
    while got == null {
        got = ch.try_receive()              // 不阻塞
        if got == null { await asyncs.sleep(1) }
    }
    return got
}

async func task(int n): int {
    var ch = threads.channel(1)
    var t = threads.start(worker, ch, n)
    var v = await await_channel(ch)
    t.join()                                 // 结果已到 ⇒ 立即返回
    return _int(v)
}
```

实测：10 个协程各开一个 30ms 的线程，总耗时 37ms（真并行）。轮询粒度 1ms，
不适合微秒级同步。
- 网络 I/O → 用 `socket.aconnect()` / `socket.arecv()` / `socket.aaccept()` 配合 await，
  见 [sockets 模块文档](module_sockets.md#异步-io-arecv--aaccept) 与
  [`examples/async await/async_fetch.leno`](../examples/async%20await/async_fetch.leno)。

---

## 性能提示

| 场景 | 建议 |
|------|------|
| I/O 操作 | 使用 `sleep()` 模拟，适合网络/文件操作 |
| CPU 密集型 | 分割任务，定期 `yield()` |
| 大量并发 | 控制并发数量，避免资源耗尽 |
| 超时控制 | 使用 `timeout()` 防止无限等待 |

### 实测成本（2026-11-14，本机；复测：`tools\async_bench.leno`）

| 项 | 成本 |
|---|---|
| 同步函数调用（基线） | ~22 ns/次 |
| 串行 `await`（含协程/Future 创建 + 调度 + 恢复） | **~400 ns/次** |
| `yield()` 让出（500~2000 个协程同时跑） | **~210~240 ns/次** |
| `sleep(1)` × 2000 个协程 | **~2.0 ms 总耗时**（线性：500 个 ≈ 1.2 ms） |
| `asyncs.all()` 收集 2000 个 Future | ~0.15 ms |

结论：`await` 的固定成本约 **0.4 µs** ⇒ 一行一 await 的细粒度写法（一次 await 只做几十 ns 的活）
不划算，批量/分块再 await 更合适；而并发量本身已经**不随协程数二次增长**（见下）。

### 事件循环的数据结构（2026-11-14 改）

| 结构 | 原来 | 现在 |
|---|---|---|
| 就绪队列 | 数组：入队线性扫去重 + 出队 `memmove` 全表 ⇒ N 个协程 O(N²) | **环形缓冲 + 每条协程一个 `in_ready` 标志** ⇒ 入队/出队/去重全 O(1) |
| 定时器 | 有序数组：插入/到期都要搬数组 ⇒ O(N) | **二叉最小堆**（按 `(wake_time, seq)` 比较，同到期时间保 FIFO）⇒ O(log N) |

实测（同机、改前 → 改后）：2000 协程 ×100 次让出 **854 ms → 45 ms**（单次 4269 → 226 ns）、
2000 个 `sleep(1)` **13.5 ms → 2.0 ms**、`all()` ×2000 **3.56 ms → 0.13 ms**。
回归用例：`assert\test_async_fanout.leno`（1500 协程让出 + 1500 定时器，覆盖超过初始容量
`MAX_COROUTINES/MAX_TIMERS = 1024` 后的扩容路径）。

---

## 实战：并发网络客户端（2026-11-14）

[`examples/async await/async_fetch.leno`](../examples/async%20await/async_fetch.leno) 演示了
async 最有价值的用法：**一个线程、一个事件循环**里同时跑「1 个 `aaccept` 协程 + N 个
连接处理协程 + N 个 `aconnect` 请求协程」。每个连接 99% 的时间在等，所以不需要 N 个线程。

实测（Windows 10，本机回环）：

| 并发连接 | 每个请求"处理" | 实际耗时 | 逐个等 |
|---|---|---|---|
| 150 | 30ms | **65ms** | 4500ms |

哨兵用例：`assert/test_async_sockets.leno` —— 40 路 `aconnect` 扇出，并断言"总耗时远小于
串行下界"（正确性用例全绿时，只有它能发现"被悄悄串行化"的退化）。

**三个真实上限**（是边界，不是 bug —— 文档别再按"上千长连接"宣传）：

| 上限 | 数值 / 原因 |
|---|---|
| 单轮 `select` 的 socket 数 | `FD_SETSIZE = 1024`；更多靠 `g_async_scan_start` 游标轮转，正确但延迟上升。换 IOCP/epoll 见 `待办与路线图.md`（触发条件：常驻 >2000 连接 / 挂起 IO 经常 >1024） |
| 发送 | 只有阻塞的 `sock.send`（无 `asend`）：非阻塞 socket 上会自旋重试（上限 10 万次）⇒ 大 body 上传放线程，别放协程 |
| 本机回环握手 | Windows 的 listen 背压约 200：同一瞬间 >200 个 SYN 会触发 TCP 重传（+~500ms）。实测 n=200 → 19ms、n=205 → 519ms（其中 5 个 connect 用了 508ms）——这是 TCP 行为，与 async 无关 |

---

## 完整 API 速查表

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sleep(ms)` | 毫秒数 | null | 暂停协程 |
| `yield()` | 无 | null | 主动让出 |
| `run()` | 无 | null | 启动事件循环 |
| `run_for(ms)` | 毫秒数 | bool | 事件循环最多跑 ms：true=全部完成，false=超时（状态保留） |
| `current()` | 无 | int | 获取协程 ID |
| `is_done(f)` | Future | bool | 检查是否完成 |
| `get_result(f)` | Future | any | 获取结果 |
| `all(fs)` | Future[] | any[] | 收集结果 |
| `timeout(f, ms)` | Future, int | any | 超时等待 |

---

*文档版本: 1.2*  
*最后更新: 2026-11-14*
