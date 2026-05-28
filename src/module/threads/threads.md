# threads 模块

线程模块提供多线程编程支持，包括线程创建和通道（Channel）通信。

## 模块函数

### `threads.start(func, ...args)`

创建并启动一个新线程，执行指定的函数。

**参数:**
- `func`: 要执行的函数（闭包）
- `...args`: 传递给函数的参数（可选）

**返回值:**
- 返回 Thread 对象

**注意:**
- MVP 版本不支持捕获变量的闭包
- 线程函数不能访问外部变量

**示例:**
```leno
import threads

func worker(var x, var y) {
    return x + y
}

main() {
    var t = threads.start(worker, 10, 20)
    var result = t.join()
    print("结果: " + result)  // 输出: 结果: 30
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
    sleep(100)
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
- 如果通道已关闭且为空，行为未定义

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

## 完整示例

### 基本线程
```leno
import threads

main() {
    var t1 = threads.start(func() {
        return 1 + 2 + 3 + 4 + 5
    })

    var r1 = t1.join()
    print("result: " + r1)  // 输出: result: 15
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

## 限制

1. **MVP 版本不支持闭包捕获变量**
   ```leno
   // 错误示例
   var x = 10
   var t = threads.start(func() {
       print(x)  // 错误：不能捕获外部变量
   })
   ```

2. **线程安全**
   - 多个线程不能同时访问同一个可变对象
   - 使用 Channel 进行线程间通信

3. **错误处理**
   - 线程中的异常不会传播到主线程
   - 使用 `thread.state()` 检查线程是否出错
