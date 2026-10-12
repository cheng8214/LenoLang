# threads 模块

线程模块提供多线程编程支持，包括线程创建、闭包捕获和通道（Channel）通信。

## 模块函数

### `threads.start(func, ...args)`

创建并启动一个新线程，执行指定的函数。

**参数:**
- `func`: 要执行的函数（闭包），支持捕获外部变量
- `...args`: 传递给函数的参数（可选）

**返回值:**
- 返回 Thread 对象

> ⚠ **入口函数的三条规则**（2026-10-07 实测矩阵 / **2026-10-12 第③条翻案**）：
>
> | 入口形态 | 结果 |
> | --- | --- |
> | ① 本文件的**具名全局函数** `threads.start(named, 21)` | ✅ |
> | ② **闭包**（无参 / 有参、有捕获 / 无捕获，四种形态） | ✅ |
> | ③ **别的 `.leno` 模块里的函数** `threads.start(mod.fetch, …)` | ✅ **2026-10-12 起可用**（TH2） |
>
> ③ 原先被硬拦，根因只有一处：协程首帧漏设 `frame->module`（`src/vm/vm.c` 的
> `vm_run_coroutine_with_vm`），而模块字节码全走 `frame->module` 寻址 ⇒ 进函数体后第一条
> 模块符号指令就报「不在模块上下文中」（症状是 `join()` 只给 `Thread error: unknown error`）。
> 补上后模块函数直接可用 ✓ ⇒ **库可以自己封装并发**（模块内 `threads.start(自己的函数)` ✓）。
> ⚠ 唯一仍然会拦的情况：该模块**此刻还没初始化**（不在入口脚本的 import 链上 ⇒ 模块的 `var`
> 全是空的）⇒ 报「入口函数所属的模块尚未初始化」。详见 `docs/threads使用指南.md`
> 的「线程入口：模块函数也能用」与 `docs/线程并发能力修复追踪.md` 的 TH2。
>
> ⚠ **实参个数会被校验**（2026-10-07 起）：`threads.start(f, a, b)` 的实参个数必须**恰好**等于
> `f` 的形参个数 —— 线程入口由 VM 直接调用，**默认参数不会被补齐**。少传 / 多传会在
> `threads.start(...)` 这一行直接报 `入口函数期望 N 个参数，实际传了 M 个`。
> （此前少传只会在 `join()` 时给出「非字符串异常…常见于**进入函数体之前**就失败」这种
> 把人引向"模块没加载"的误导文案；在 GUI 的定时器回调里还会被 try/catch 吞成**静默失败**。）

**闭包捕获:**
- 支持捕获外部变量，捕获的值会深拷贝到子线程
- 每个线程拥有独立的变量副本，互不影响
- 修改捕获变量不会影响主线程或其他线程

**示例:**
```leno
import threads

// 顶层函数
func worker(var x, var y) {
    return x + y
}

// 闭包捕获
var name = "Leno"
var t1 = threads.start(func(){
    print("hello " + name)  // 捕获外部变量
})

// 闭包捕获 + 参数传递
var prefix = "result"
var t2 = threads.start(func(int n){
    return prefix + "_" + n
}, 42)

main() {
    t1.join()
    var r = t2.join()
    print(r)  // 输出: result_42
}
```

### `threads.channel(capacity)`

创建一个通道用于线程间通信。

**参数:**
- `capacity`: 通道缓冲区大小（0 表示无缓冲）

**返回值:**
- 返回 Channel 对象

**示例:**
```leno
import threads

func producer(var ch) {
    ch.send("hello")
    ch.close()
}

main() {
    var ch = threads.channel(10)
    var t = threads.start(producer, ch)
    var msg = ch.receive()
    print(msg)  // 输出: hello
    t.join()
}
```

### `threads.sleep(ms)`

当前线程休眠指定毫秒数。

**参数:**
- `ms`: 休眠时间（毫秒），必须为整数

**示例:**
```leno
import threads

main() {
    print("start")
    threads.sleep(1000)
    print("1 second later")
}
```

### `threads.join_all(Array[Thread])`

等一组线程结束（“WaitGroup”的等价物 —— **没有引入新对象类型**，见 TH3）。

**参数:** 线程数组（非 `Thread` 元素会被跳过）
**返回值:** `int` —— 真正等完的线程数

> 某个线程出错时会**把剩下的也等完**，再让异常浮出来（不会留下没人 join 的活线程 ✓）。

### `threads.parallel_for(n, f)`

分 n 份并行：`f(index)` 在 n 个线程上各跑一次（`index` = `0..n-1`），等全部结束。

**参数:** `n`（份数，`<= 0` ⇒ 空操作）、`f`（必须是 `func(int):…`，**恰好一个形参**）
**返回值:** `int` —— 真正起来的线程数

```leno
import threads

main() {
    var ch = threads.channel(8)
    // 回调是闭包 ⇒ 可以用捕获的 channel 把结果带回来
    threads.parallel_for(4, func(int i) { ch.send(i * 10) })
    var sum = 0
    for 0 : 3 to var k { sum = sum + _int(ch.receive()) }
    print(sum)   // 60
}
```

> ⚠ `n` 不会自动按核数分（说几份就几份）；回调形参个数是**硬校验**（默认参数不补齐）。

### `threads.map(arr, f)`

并行 map：每个元素起一个线程跑 `f(item, index)`，结果**按输入顺序**收回。

**参数:** `arr`（`Array`）、`f`（必须是 `func(item, index):…`，**恰好两个形参** —— 与 `arrays.map` 同口径）
**返回值:** `Array[回调返回类型]`（由原生返回规格推断 ✓ 可直接当 `Array[int]` 用）

```leno
Array[int] doubled = threads.map([1, 2, 3, 4], func(int x, int idx): int {
    return x * 10 + idx
})
print(doubled[3])   // 43（4*10+3）
```

> ⚠ **线程数 = 元素数**（每个元素一个线程）⇒ 元素多时线程开销会盖过收益；重活请用
> `parallel_for` 自己分块 ✓。元素可以是 `struct`（走 TH1 的深克隆 ✓）。

## Thread 对象方法

### `thread.join()`

等待线程执行完成，并返回线程函数的返回值。

**返回值:**
- 线程函数的返回值

**示例:**
```leno
var t = threads.start(func() {
    return 42
})
var result = t.join()
print(result)  // 输出: 42
```

### `thread.state()`

获取线程当前状态。

**返回值:**
- `"running"`: 线程正在运行
- `"done"`: 线程已完成
- `"error"`: 线程发生错误

**示例:**
```leno
var t = threads.start(func() {
    threads.sleep(100)
})
print(t.state())  // 输出: running
t.join()
print(t.state())  // 输出: done
```

## Channel 对象方法

### `channel.send(value)`

向通道发送一个值。

**参数:**
- `value`: 要发送的值（可以是任意类型）

**注意:**
- 如果通道已关闭，会报错
- 如果通道缓冲区已满，会阻塞等待

### ⚠ 跨线程传值的硬约束（2026-10-09 实测 / 2026-10-12 更新）

上面的"可以是任意类型"要打折扣 ⚠ **子线程有自己的 GC 堆**，跨线程的值必须能被
安全克隆，否则会出现堆损坏 / 静默错误：

| 值 | 能否跨线程 | 说明 |
|---|---|---|
| `int` / `float` / `bool` / `string` | ✅ | 直接克隆 |
| `Array[T]` / `Dict[K,V]` | ✅ | 逐元素克隆（**键也要克隆**，见 `object_thread.c` 的历史坑）|
| **struct 实例** | ✅ | **2026-10-12 起**：深克隆 + 接收侧按定义重建（含嵌套 struct、face 声明、泛型实参）。见下文 |
| **cstruct 实例**（以及字段里装了 cstruct / `Ptr` / 闭包的 struct） | ❌ | 裸 C 内存 / 假句柄 ⇒ 报「不能跨线程传递此类型（来自 结构体.字段）」，**编译期**先拦静态可判定的 cstruct |
| **多返回值 tuple** | ⚠ 能 send，但**收回来用不了** | 见下 |

> **struct 的语义是深拷贝**：接收线程拿到的是**独立副本**，改它不影响发送方
> （与 `Array` / `Dict` 同口径 ✓）。所以 `threads.start(work, p)` 里子线程改 `p` 的字段，
> 主线程看不见 —— 想把结果带回来请用 `join()` 的返回值或 `Channel` ✓
> （改动与判据见 `docs/线程并发能力修复追踪.md` 的 TH1 ✓，用例 `assert\test_channel_struct.leno`）。
>
> ⚠ **自引用（环形）struct 会被拒**：克隆上限 64 层，超限报「嵌套层数超过 64」而不是挂死 ✓
>
> **为什么 cstruct 不行**：cstruct 是**裸 C 内存**（`Ptr` / `ffi` 句柄 / SDL 纹理 / socket fd /
> 文件句柄），拷出来是**假句柄**（比当场报错难查一百倍）；子线程分配的对象还挂在子线程的堆上，
> 子线程退出后该堆被遗弃，主线程 GC 若扫到它就会访问已释放内存。要跨线程就传**纯数据**
> （标量 / `string` / `Array` / `Dict` / **struct**），在接收方重建 cstruct —— 这也是很多框架
> 约定"对外一律传字符串数组行"的原因 ✓

> ⚠ **多返回值（tuple）跨线程的两个坑**（实测）：
> ① `ch.send(多返回值函数())` **能发出去**（不报错），但接收端拿到的静态类型是 `any`
>   ⇒ `var[int,int](a,b) = ch.try_receive()` 被编译器拒绝：
>   「解构声明槽位 0 类型 int 与数组元素类型 any 不兼容」；
> ② 想 `as Array[int]` 再解构也不行 —— **tuple 的运行期表示不是 Array**，
>   `as` 之后索引会抛「下标访问: 对象不支持索引」。
> ⇒ **跨线程要传结构化数据，用 `Array[T]`（元素类型一致的那种）** ✓

**示例:**
```leno
var ch = threads.channel(10)
ch.send("hello")
ch.send(42)
ch.send([1, 2, 3])
```

### `channel.receive()`

从通道接收一个值。

**返回值:**
- 接收到的值 —— **静态类型 = 接收者的元素类型** ✓（★ 2026-10-12 起，见下面的 `Channel[T]`）

**注意:**
- 如果通道为空且未关闭，会阻塞等待
- 如果通道已关闭且为空，返回 `null`

**示例:**
```leno
var ch = threads.channel(10)
ch.send("hello")
var msg = ch.receive()
print(msg)  // 输出: hello
```

### `Channel[T]` —— 通道的元素类型（★ 2026-10-12 起）

写 `Channel[T]` 就把"元素类型"告诉编译器，`receive()` 直接得到 T，不用 `_int(...)` / 收窄：

```leno
Channel[int] ch = threads.channel(4)     // 工厂返回裸 Channel ⇒ 靠标注拿到元素类型
ch.send(7)
int v = ch.receive()                     // ✓ 直接是 int

Channel[Pt] pc = threads.channel(4)      // Pt 是 struct
Pt got = pc.receive()                    // ✓ 直接是 Pt（跨线程的 struct 是深拷贝）
```

**规则**（与 `Ptr[T]` / `Thread[T]` 同口径 ✓）：

| 情形 | 结果 |
|---|---|
| `Channel[int]` 的 `receive()` | `int` |
| 裸 `Channel` 的 `receive()` | `any`（回落 —— 仍可用，但要 `_int(...)` / 收窄） |
| `try_receive()` | 恒为 `any` ✓（**有意**：空通道给 `null`，说它是 `T` 就是撒谎） |
| 裸 `Channel` 赋给 `Channel[int]` | ✅ 允许（元素未知的一侧放行） |
| `Channel[int]` 传给形参 `Channel` | ✅ 允许（同上） |
| `Channel[int]` 赋给 `Channel[string]` | ❌ 报「期望类型: Channel[string]，实际类型: Channel[int]」 |
| `threads.channel(n)` | 返回**裸 `Channel`**（工厂推不出 T）⇒ 要精确就写标注 |

顺带：`Thread[T]` 标注现在也写得出来了 ✓（`Thread[int] t = threads.start(f, 1)`）。

### `channel.try_send(value)`

非阻塞地向通道发送一个值。

**参数:**
- `value`: 要发送的值

**返回值:**
- `true`: 发送成功
- `false`: 发送失败（通道已满或已关闭）

**注意:**
- 与 `send()` 不同，`try_send()` 不会阻塞，也不会在通道关闭时抛异常

**示例:**
```leno
var ch = threads.channel(2)
assert_eq(ch.try_send("A"), true)
assert_eq(ch.try_send("B"), true)
assert_eq(ch.try_send("C"), false)  // 缓冲区已满
ch.close()
assert_eq(ch.try_send("D"), false)  // 已关闭
```

### `channel.try_receive()`

非阻塞地从通道接收一个值。

**返回值:**
- 接收到的值（通道非空时）
- `null`（通道为空时）

**注意:**
- 与 `receive()` 不同，`try_receive()` 不会阻塞等待
- 通道关闭且为空时同样返回 `null`

**示例:**
```leno
var ch = threads.channel(2)
ch.send("hello")
assert_eq(ch.try_receive(), "hello")
assert_eq(ch.try_receive(), null)   // 缓冲区已空
```

### `channel.close()`

关闭通道。

**注意:**
- 关闭后不能再发送数据
- 已缓冲的数据仍然可以接收

**示例:**
```leno
var ch = threads.channel(10)
ch.send("data")
ch.close()
// ch.send("more")  // 错误：不能向已关闭的通道发送
```

### `channel.is_closed()`

检查通道是否已关闭。

**返回值:**
- `true`: 通道已关闭
- `false`: 通道未关闭

**示例:**
```leno
var ch = threads.channel(5)
print(ch.is_closed())  // 输出: false
ch.close()
print(ch.is_closed())  // 输出: true
```

### `channel.len()`

获取通道缓冲区中当前的消息数量（线程安全）。

**返回值:**
- 缓冲区中的消息数量（整数）

**示例:**
```leno
var ch = threads.channel(10)
print(ch.len())  // 输出: 0
ch.send(1)
ch.send(2)
print(ch.len())  // 输出: 2
ch.receive()
print(ch.len())  // 输出: 1
```

## 完整示例

### 闭包捕获 + Channel 通信
```leno
import threads

main() {
    var ch = threads.channel(10)
    var msg = "hello"

    var producer = threads.start(func(){
        for 5 to i {
            ch.send(msg + "_" + i)
        }
        ch.close()
    })

    var consumer = threads.start(func(){
        while true {
            var val = ch.receive()
            if val == null { break }
            print("received: " + val)
        }
    })

    producer.join()
    consumer.join()
}
```

### 生产者-消费者模式
```leno
import threads

func producer(var ch) {
    for 5 to var i {
        ch.send(i * i)
    }
    ch.close()
}

func consumer(var ch) {
    while true {
        var msg = ch.receive()
        if msg == null { break }
        print("收到: " + msg)
    }
}

main() {
    var ch = threads.channel(10)
    var t1 = threads.start(producer, ch)
    var t2 = threads.start(consumer, ch)
    t1.join()
    t2.join()
}
```

## 注意事项

1. **闭包捕获变量是深拷贝**
   - 捕获的变量值会深拷贝到子线程，各线程拥有独立副本
   - 修改捕获变量不会影响主线程或其他线程
   ```leno
   var x = 10
   var t = threads.start(func(){
       print(x)  // ✅ 输出: 10（深拷贝）
   })
   ```

2. **线程安全**
   - 多个线程不能同时访问同一个可变对象
   - 使用 Channel 进行线程间通信

3. **错误处理**
   - 线程中的异常不会传播到主线程
   - 使用 `thread.state()` 检查线程是否出错

4. **print 是线程安全的**
   - 单次 `print()` 调用是原子的，不会与其他线程的输出交错
   - 但多次 `print()` 调用之间可能被其他线程插入

5. **避免忙等待**
   - 使用 `ch.receive()`（阻塞）而非 `ch.try_receive()` + 循环轮询
   - `is_closed()` + `len()` + `try_receive()` 的组合会产生忙等待，浪费 CPU
   - 推荐模式：`while true { var val = ch.receive(); if val == null { break } }`

6. **子线程异常与 join() 行为**
   - 子线程抛出异常时，`t.state()` 返回 `"error"`
   - 调用 `t.join()` 会在主线程抛出异常，格式为 `"Thread error: <子线程错误信息>"`
   - 可用 `try-catch` 包裹 `join()`，或先检查 `t.state()` 再 join

7. **threads 与 async 不应混用**
   - `threads.start()` 是 OS 线程 + 隔离 VM，真正并行
   - `async func` 是协程 + 事件循环，协作式调度
   - 不要在子线程中使用 `async`/`await`，也不要在 `async` 函数中调用 `threads.start()`
   - CPU 并行用 threads，I/O 并发用 async

8. **type() 返回运行时值的类型**
   - `type()` 反映运行时实际值的类型，而非变量声明类型
   - `int? a = null` 时 `type(a)` 返回 `"null"`，不是 `"int?"`
   - BigInt 和普通 int 的 `type()` 都返回 `"int"`
