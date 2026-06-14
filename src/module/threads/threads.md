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
- 接收到的值

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
