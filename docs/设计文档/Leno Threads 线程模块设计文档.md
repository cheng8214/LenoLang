# Leno Threads 线程模块设计文档

> 版本: 1.0  
> 日期: 2026-05-17  
> 状态: 设计阶段

---

## 1. 设计目标

为 Leno 提供简单、稳定的多线程支持，采用 **隔离 VM + Channel** 架构（参考 Go goroutine + channel）。

### 1.1 核心原则

- **不共享内存，只通过消息通信** — 零数据竞争
- **每个线程是独立的 Leno VM** — GC 隔离，崩溃隔离
- **API 简洁** — 只需 `start()` + `channel()` 即可上手
- **跨平台** — Windows (CreateThread) / Linux (pthread)

### 1.2 设计参考

| 语言 | 方案 | Leno 采纳 |
|------|------|-----------|
| Go | goroutine + channel | ✅ API 风格 |
| Lua Lanes | 隔离 VM | ✅ 架构 |
| Erlang | 进程隔离 + 消息 | ✅ 理念 |

---

## 2. 架构设计

### 2.1 整体架构

```
┌──────────────────────────────────────────────────┐
│                  主线程 (Main VM)                  │
│                                                    │
│  var t = threads.start(func() { ... })            │
│  var ch = threads.channel()                       │
│                                                    │
└──────────┬──────────────────────┬─────────────────┘
           │                      │
           │ Channel (消息传递)    │ Channel (消息传递)
           │                      │
┌──────────▼──────────┐  ┌────────▼───────────────┐
│   子线程 VM 1        │  │   子线程 VM 2           │
│   独立 GC            │  │   独立 GC               │
│   独立堆栈           │  │   独立堆栈              │
│   独立全局变量        │  │   独立全局变量           │
└─────────────────────┘  └────────────────────────┘
```

### 2.2 为什么不共享内存

| 共享内存方案的问题 | 隔离 VM 方案的优势 |
|-------------------|-------------------|
| 需要 Mutex / RWLock | 无锁，零竞争 |
| GC 需要全局暂停 | 每个线程独立 GC |
| 数据竞争难以调试 | 不存在数据竞争 |
| 子线程崩溃可能影响主线程 | 崩溃隔离 |
| 实现复杂 | 实现简单 |

### 2.3 内存开销

```
1 个线程:   ~530KB (一个独立 VM)
10 个线程:  ~5.3MB
100 个线程: ~53MB
```

对于脚本语言完全可接受。

---

## 3. API 设计

### 3.1 Thread 类型

`threads.start()` 返回 `Thread` 对象，与 `File`、`Ptr` 风格一致。

```leno
var t = threads.start(func() {
    print("子线程运行")
})
```

#### Thread 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `t.join()` | 等待线程结束，返回函数返回值 | ✅ 阻塞 |

### 3.2 Channel

Go 风格的通道，用于线程间通信。

```leno
// 无缓冲（同步）— send 阻塞直到有人 receive
var ch = threads.channel()

// 有缓冲（异步）— 缓冲区大小为 N
var ch = threads.channel(10)
```

#### Channel 方法

| 方法 | 说明 | 阻塞 |
|------|------|------|
| `ch.send(value)` | 发送值到通道 | 缓冲满时阻塞 |
| `var result = ch.receive()` | 从通道接收值 | 缓冲空时阻塞 |
| `ch.close()` | 关闭通道 | ❌ 不阻塞 |

**`receive()` 返回结构体 `{value: any, ok: bool}`：**

```leno
var result = ch.receive()
if result.ok {
    print("收到: " + result.value)
} else {
    print("通道已关闭")
}
```

**与 Go 的 `(value, ok) := <-ch` 语义一致，避免 `null` 歧义。**

### 3.3 可传递的类型

Channel 可以传递以下类型：

| 类型 | 说明 |
|------|------|
| `int` | 整数 |
| `float` | 浮点数 |
| `bool` | 布尔值 |
| `string` | 字符串（跨 VM 复制） |
| `null` | 空值 |
| `Array` | 数组（深拷贝） |
| `Dict` | 字典（深拷贝） |

**不支持传递的类型：**

| 类型 | 原因 |
|------|------|
| `Ptr` | 指针不能跨 VM 共享 |
| `File` | 文件句柄不能跨 VM |
| `Thread` | 线程对象不能跨 VM |
| `Channel` | 暂不支持跨 VM 传递（未来扩展） |
| `struct` 实例 | 需要序列化，暂不支持 |
| 闭包/函数 | 需要序列化，暂不支持 |

---

## 4. 使用示例

### 4.1 基础用法

```leno
import threads

// 创建并等待线程
var t = threads.start(func() {
    print("子线程: 开始")
    times.sleep(1000)
    print("子线程: 结束")
    return 42
})

var result = t.join()
print("返回值: " + result)    // 返回值: 42
```

### 4.2 并行计算

```leno
import threads

var ch = threads.channel(2)

// 线程1：计算 0~999999 的和
threads.start(func() {
    var sum = 0
    for 0 : 1000000 to i { sum = sum + i }
    ch.send(sum)
})

// 线程2：计算 1000000~1999999 的和
threads.start(func() {
    var sum = 0
    for 1000000 : 2000000 to i { sum = sum + i }
    ch.send(sum)
})

var r1 = ch.receive().value
var r2 = ch.receive().value
print("总和: " + (r1 + r2))
```

### 4.3 生产者-消费者

```leno
import threads

var ch = threads.channel(5)

// 生产者
threads.start(func() {
    for 0 : 10 to i {
        ch.send(i)
        print("生产: " + i)
    }
    ch.close()
})

// 消费者
threads.start(func() {
    while true {
        var result = ch.receive()
        if not result.ok { break }
        print("消费: " + result.value)
    }
    print("消费者结束")
})
```

### 4.4 多个消费者（工作池模式）

```leno
import threads

var ch = threads.channel(20)

// 生产者：发送 20 个任务
threads.start(func() {
    for 0 : 20 to i {
        ch.send(i)
    }
    ch.close()
})

// 4 个消费者
for 0 : 4 to i {
    threads.start(func() {
        while true {
            var result = ch.receive()
            if not result.ok { break }
            print("工作者 " + i + " 处理任务 " + result.value)
        }
    })
}
```

### 4.5 等待多个线程

```leno
import threads

var workers = []
var ch = threads.channel(3)

for 0 : 3 to i {
    workers.add(threads.start(func() {
        times.sleep(1000 * (i + 1))
        ch.send("线程 " + i + " 完成")
    }))
}

// 接收所有结果
for 0 : 3 to i {
    var result = ch.receive()
    if result.ok {
        print(result.value)
    }
}
```

---

## 5. 实现设计

### 5.1 C 数据结构

```c
// ===== 线程对象 =====
typedef struct ObjThread {
    Object obj;
    LenoVM* vm;              // 独立 VM 实例
    void* os_thread;         // OS 线程句柄 (HANDLE / pthread_t)
    ThreadState state;       // RUNNING, DONE, ERROR
    Value result;            // join() 返回值
    char* error_msg;         // 错误信息
} ObjThread;

// ===== Channel 对象 =====
typedef struct ObjChannel {
    Object obj;
    Value* buffer;           // 环形缓冲区
    int capacity;            // 缓冲区大小 (0 = 无缓冲)
    int head, tail, count;   // 环形队列指针
    int closed;              // 是否已关闭
    void* mutex;             // 互斥锁 (CRITICAL_SECTION / pthread_mutex_t)
    void* not_empty;         // 条件变量 (CONDITION_VARIABLE / pthread_cond_t)
    void* not_full;          // 条件变量
} ObjChannel;

// ===== 线程状态 =====
typedef enum {
    THREAD_RUNNING,
    THREAD_DONE,
    THREAD_ERROR
} ThreadState;
```

### 5.2 线程创建流程

```
threads.start(func)
    │
    ├─ 1. 创建独立 LenoVM 实例
    │     - 独立 GC
    │     - 独立全局变量
    │     - 独立堆栈
    │
    ├─ 2. 将函数序列化后传入子 VM
    │     - 基本类型：直接复制
    │     - 闭包捕获的变量：深拷贝
    │
    ├─ 3. 创建 OS 线程
    │     - Windows: CreateThread()
    │     - Linux: pthread_create()
    │
    ├─ 4. 在子线程中执行函数
    │     - vm_execute(vm, func)
    │     - 捕获返回值
    │
    └─ 5. 返回 Thread 对象
```

### 5.3 Channel 实现

```
ch.send(value)
    │
    ├─ 1. 加锁 (mutex_lock)
    │
    ├─ 2. 如果缓冲区满且未关闭
    │     └─ 等待 not_full 条件变量
    │
    ├─ 3. 将 value 放入缓冲区
    │     - value 需要深拷贝（跨 VM 安全）
    │
    ├─ 4. 通知 not_empty 条件变量
    │
    └─ 5. 解锁 (mutex_unlock)

ch.receive()
    │
    ├─ 1. 加锁 (mutex_lock)
    │
    ├─ 2. 如果缓冲区空且未关闭
    │     └─ 等待 not_empty 条件变量
    │
    ├─ 3. 如果缓冲区空且已关闭
    │     └─ 返回 null
    │
    ├─ 4. 从缓冲区取出 value
    │
    ├─ 5. 通知 not_full 条件变量
    │
    └─ 6. 解锁 (mutex_unlock)
```

### 5.4 值序列化（跨 VM 传递）

```c
// 将值从一个 VM 复制到另一个 VM
Value value_clone(LenoVM* src_vm, LenoVM* dst_vm, Value val) {
    switch (val.type) {
        case VAL_INT:
        case VAL_FLOAT:
        case VAL_BOOL:
        case VAL_NULL:
            return val;  // 基本类型直接复制

        case VAL_STRING:
            // 复制字符串到目标 VM
            return val_string(dst_vm, val.as.obj->as.string.chars, val.as.obj->as.string.len);

        case VAL_ARRAY: {
            // 深拷贝数组
            ObjArray* src = val.as.obj->as.array;
            ObjArray* dst = array_new(dst_vm, src->count);
            for (int i = 0; i < src->count; i++) {
                dst->elements[i] = value_clone(src_vm, dst_vm, src->elements[i]);
            }
            return val_obj(dst);
        }

        case VAL_DICT: {
            // 深拷贝字典
            ObjDict* src = val.as.obj->as.dict;
            ObjDict* dst = dict_new(dst_vm, src->capacity);
            // ... 遍历并深拷贝每个键值对
            return val_obj(dst);
        }

        default:
            // Ptr, File, Thread, Channel, struct, 闭包 → 不支持
            runtime_error("不能跨线程传递此类型");
            return val_null();
    }
}
```

---

## 6. 跨平台实现

### 6.1 线程封装

```c
// platform_thread.h

#ifdef _WIN32
    #include <windows.h>
    typedef HANDLE ThreadHandle;
    typedef CRITICAL_SECTION Mutex;
    typedef CONDITION_VARIABLE CondVar;
#else
    #include <pthread.h>
    typedef pthread_t ThreadHandle;
    typedef pthread_mutex_t Mutex;
    typedef pthread_cond_t CondVar;
#endif

// 统一接口
ThreadHandle platform_thread_create(void (*func)(void*), void* arg);
void platform_thread_join(ThreadHandle handle);
void platform_mutex_init(Mutex* m);
void platform_mutex_lock(Mutex* m);
void platform_mutex_unlock(Mutex* m);
void platform_cond_init(CondVar* cv);
void platform_cond_wait(CondVar* cv, Mutex* m);
void platform_cond_signal(CondVar* cv);
```

### 6.2 平台差异处理

| 功能 | Windows | Linux |
|------|---------|-------|
| 创建线程 | `CreateThread()` | `pthread_create()` |
| 等待线程 | `WaitForSingleObject()` | `pthread_join()` |
| 互斥锁 | `CRITICAL_SECTION` | `pthread_mutex_t` |
| 条件变量 | `CONDITION_VARIABLE` | `pthread_cond_t` |
| 线程局部存储 | `TlsAlloc()` | `pthread_key_create()` |

---

## 7. 错误处理

### 7.1 线程错误

| 错误 | 处理方式 |
|------|---------|
| 子线程运行时错误 | 捕获错误信息，`join()` 时重新抛出 |
| 子线程崩溃 | 不影响主线程，`join()` 返回错误信息 |
| VM 创建失败 | `threads.start()` 直接抛出异常 |
| 线程资源耗尽 | `threads.start()` 直接抛出异常 |

### 7.2 Channel 错误

| 错误 | 处理方式 |
|------|---------|
| 向已关闭的 Channel 发送 | 抛出异常 |
| 不支持的类型传递 | 抛出异常 |
| Channel 创建失败 | 抛出异常 |

---

## 8. GC 集成

### 8.1 GC 安全

```
主线程 GC                    子线程 GC
    │                            │
    ├─ 只追踪主线程 VM 的对象     ├─ 只追踪子线程 VM 的对象
    ├─ 不扫描子线程 VM            ├─ 不扫描主线程 VM
    ├─ 不扫描 Channel 缓冲区      ├─ 不扫描 Channel 缓冲区
    │                            │
    └─ 完全独立，无需同步          └─ 完全独立，无需同步
```

### 8.2 Channel 缓冲区 GC

Channel 缓冲区中的值**不被任何 VM 的 GC 追踪**，由 Channel 自身管理生命周期：

```c
// Channel 持有值的引用计数
void channel_send(ObjChannel* ch, Value val) {
    // 深拷贝值到 Channel 自己管理的内存
    // 不依赖任何 VM 的 GC
}

void channel_close(ObjChannel* ch) {
    // 释放缓冲区中所有值
    for (int i = 0; i < ch->count; i++) {
        value_free(ch->buffer[(ch->head + i) % ch->capacity]);
    }
    ch->count = 0;
}
```

---

## 9. 限制与未来扩展

### 9.1 当前限制

| 限制 | 原因 | 未来 |
|------|------|------|
| 不能传递 Ptr | 指针不能跨 VM | 可设计共享内存区域 |
| 不能传递 struct 实例 | 需要序列化 | 可实现 struct 序列化 |
| 不能传递闭包/函数 | 需要序列化 | 可实现函数序列化 |
| 不能传递 Channel | 跨 VM 引用复杂 | 可实现 Channel 转移 |
| 不能取消线程 | 需要协作式取消 | 可添加 `t.cancel()` |

### 9.2 未来扩展

| 功能 | 优先级 | 说明 |
|------|--------|------|
| `select` 语句 | 中 | 同时监听多个 Channel（Go 风格） |
| `t.cancel()` | 中 | 协作式取消线程 |
| Channel 泛型 | 低 | `Channel[int]` 类型约束 |
| 共享原子变量 | 低 | `threads.atomic_int` |
| 线程池 | 低 | `threads.pool(4)` 固定大小线程池 |
| struct 序列化 | 中 | 支持跨线程传递 struct |
| Channel 转移 | 低 | 将 Channel 作为值传递给子线程 |

---

## 10. 实现计划

### 阶段 1：跨平台线程封装（1 天）

- [ ] `platform_thread.h` — 统一线程接口
- [ ] Windows 实现 (CreateThread, CRITICAL_SECTION, CONDITION_VARIABLE)
- [ ] Linux 实现 (pthread)

### 阶段 2：threads.start() + join()（2 天）

- [ ] `ObjThread` 运行时对象
- [ ] 子 VM 创建与初始化
- [ ] 函数序列化与反序列化
  - **MVP：仅支持顶层函数（无闭包捕获）**
- [ ] `join()` 等待与返回值
- [ ] 错误捕获与传播

### 阶段 3：threads.channel()（2 天）

- [ ] `ObjChannel` 运行时对象
- [ ] 无缓冲 Channel 实现
- [ ] 有缓冲 Channel 实现
- [ ] `send()` / `receive()` / `close()`
- [ ] `receive()` 返回 `{value, ok}` 结构体
- [ ] 值深拷贝（跨 VM 安全）
  - **MVP：仅支持 int, float, bool, string, null**

### 阶段 4：GC 集成（1 天）

- [ ] Thread 对象 GC 追踪
- [ ] Channel 对象 GC 追踪
- [ ] Channel 缓冲区生命周期管理

### 阶段 5：测试（1 天）

- [ ] 基础创建与 join
- [ ] 并行计算
- [ ] 生产者-消费者
- [ ] 多消费者工作池
- [ ] Channel 关闭行为
- [ ] 错误处理
- [ ] 跨平台测试

**总计：约 7 天**

---

## 11. MVP 范围

### 最小可用版本

```leno
// ✅ MVP 支持
var t = threads.start(func() {        // 顶层函数
    return 42
})
print(t.join())                        // 42

var ch = threads.channel()             // 无缓冲
ch.send("hello")
var result = ch.receive()
print(result.value)                    // "hello"
print(result.ok)                       // true
```

### MVP 限制

| 限制 | 说明 |
|------|------|
| 函数类型 | 仅顶层函数，不支持闭包 |
| Channel 类型 | 仅 int, float, bool, string, null |
| Channel 缓冲 | 仅无缓冲 |

### 后续扩展

| 阶段 | 功能 |
|------|------|
| v1.1 | 有缓冲 Channel |
| v1.2 | Array, Dict 深拷贝 |
| v1.3 | 闭包捕获基本类型 |
| v1.4 | 完整闭包支持 |
| v1.5 | struct 序列化 |

---

## 12. 与现有模块的关系

| 模块 | 关系 |
|------|------|
| `times` | `times.sleep()` 用于线程内休眠 |
| `ffi` | FFI 指针不能跨线程传递 |
| `asyncs` | 协程是单线程并发，threads 是多线程并行，两者互补 |
| `cstruct` | cstruct 实例不能跨线程传递（未来可扩展） |

---

## 13. 总结

```
Leno 类型系统:
  int  float  bool  string  Array  Dict  struct  enum  null  Ptr  File  Thread  Channel

Leno 模块系统:
  io    maths   jsons   dirs    sockets   ffi    asyncs   threads   times
```

`Thread` 和 `Channel` 作为新的内置类型，`threads` 作为新的内置模块，与现有系统完全一致。
