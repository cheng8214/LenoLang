# Leno FFI 使用指南

FFI（Foreign Function Interface）是 Leno 语言调用 C 动态库和操作系统 API 的接口。本指南从简单到复杂，循序渐进地介绍 FFI 的使用方法。

## 目录

- [第一章：基础入门](#第一章基础入门)
  - [1.1 第一个 FFI 程序](#11-第一个-ffi-程序)
  - [1.2 内存分配与读写](#12-内存分配与读写)
  - [1.3 调用 C 标准库函数](#13-调用-c-标准库函数)
  - [1.4 使用 clib 声明式调用](#14-使用-clib-声明式调用推荐)
- [第二章：动态库操作](#第二章动态库操作)
  - [2.1 加载和释放动态库](#21-加载和释放动态库)
  - [2.2 调用 Windows API](#22-调用-windows-api)
  - [2.3 不同类型返回值](#23-不同类型返回值)
- [第三章：内存操作进阶](#第三章内存操作进阶)
  - [3.1 指针偏移与数组](#31-指针偏移与数组)
  - [3.2 多级指针](#32-多级指针)
  - [3.3 内存拷贝](#33-内存拷贝)
  - [3.4 内存填充](#34-内存填充)
  - [3.5 指针与整数互转](#35-指针与整数互转)
- [第四章：类型安全指针 Ptr[T]](#第四章类型安全指针-ptrt)
  - [4.1 Ptr 与 Ptr[T] 的区别](#41-ptr-与-ptrt-的区别)
  - [4.2 Ptr[u16] 与 Ptr[str16] 的区别](#42-ptru16-与-ptrstr16-的区别)
  - [4.3 使用 Ptr[T]](#43-使用-ptrt)
  - [4.4 cstruct 中的 Ptr[T]](#44-cstruct-中的-ptrt)
- [第五章：字符串处理](#第五章字符串处理)
  - [5.1 字符串读写](#51-字符串读写)
  - [5.2 UTF-8 与 UTF-16 转换](#52-utf-8-与-utf-16-转换)
- [第六章：实战案例](#第六章实战案例)
  - [6.1 进程枚举](#61-进程枚举)
  - [6.2 注册表操作](#62-注册表操作)
  - [6.3 结合 cstruct 使用](#63-结合-cstruct-使用)
- [第七章：回调函数](#第七章回调函数)
  - [7.1 cfunc 声明式回调](#71-cfunc-声明式回调)
  - [7.2 回调与 qsort](#72-回调与-qsort)
  - [7.3 释放回调](#73-释放回调)
- [第八章：最佳实践](#第八章最佳实践)
  - [8.1 错误处理](#81-错误处理)
  - [8.2 内存管理](#82-内存管理)
  - [8.3 类型安全](#83-类型安全)

---

## 第一章：基础入门

### 1.1 第一个 FFI 程序

FFI 模块是 Leno 的内置模块，无需额外安装即可使用。

```leno
import ffi

main() {
    // 分配 100 字节内存
    Ptr ptr = ffi.malloc(100)
    print("分配了内存: " + ptr)
    
    // 释放内存
    ffi.free(ptr)
    print("内存已释放")
}
```

**核心概念：**
- `Ptr` 类型：FFI 指针类型，用于存储内存地址
- `ffi.malloc(size)`：分配指定字节数的内存
- `ffi.free(ptr)`：释放分配的内存

### 1.2 内存分配与读写

FFI 提供了丰富的内存读写函数，支持各种数据类型：

```leno
import ffi

main() {
    // 分配 64 字节内存
    Ptr buf = ffi.malloc(64)
    
    // ===== 整数读写 =====
    // write_int(ptr, offset, value) - 写入 4 字节整数
    ffi.write_int(buf, 0, 42)
    
    // read_int(ptr, offset) - 读取 4 字节整数
    int val = ffi.read_int(buf, 0)
    print("读取 int: " + val)  // 42
    
    // 其他整数类型
    ffi.write_int8(buf, 4, 127)      // 1 字节
    ffi.write_int16(buf, 5, 1000)    // 2 字节
    ffi.write_uint16(buf, 7, 65535)  // 2 字节无符号
    ffi.write_int64(buf, 9, 999999)  // 8 字节

    // ===== 布尔读写 =====
    ffi.write_bool(buf, 17, true)
    bool b = ffi.read_bool(buf, 17)
    print("读取 bool: " + b)  // true

    // ===== 无符号 64 位整数读写 =====
    // write_uint64/read_uint64 支持大范围的 uint64 值
    ffi.write_uint64(buf, 32, 0xFFFFFFFFFFFFFFFF)  // 18446744073709551615
    int bigVal = ffi.read_uint64(buf, 32)
    print("读取 uint64: " + bigVal)  // 18446744073709551615

    // ===== 浮点数读写 =====
    ffi.write_double(buf, 16, 3.14159)
    float d = ffi.read_double(buf, 16)
    print("读取 double: " + d)  // 3.14159
    
    // ===== 字符串读写 =====
    ffi.write_string(buf, 24, "Hello FFI!")
    string s = ffi.read_string(buf, 24)
    print("读取字符串: " + s)  // Hello FFI!
    
    ffi.free(buf)
}
```

**内存布局示意图：**
```
偏移 0:  [42]              <- write_int(buf, 0, 42)
偏移 4:  [127]             <- write_int8(buf, 4, 127)
偏移 5:  [1000]            <- write_int16(buf, 5, 1000)
偏移 7:  [999999]          <- write_int64(buf, 7, 999999)
偏移 16: [3.14159]         <- write_double(buf, 16, 3.14159)
偏移 24: ["Hello FFI!"]    <- write_string(buf, 24, "Hello FFI!")
```

### 1.3 调用 C 标准库函数

```leno
import ffi

main() {
    // 加载 C 标准库（Windows 使用 msvcrt.dll）
    var libc = ffi.load("msvcrt.dll")
    
    if libc == null {
        print("加载库失败")
        return
    }
    
    // ===== 调用 strlen =====
    // ffi.call_int 用于返回 int 的函数
    int len = ffi.call_int(libc, "strlen", "Hello, Leno!")
    print("字符串长度: " + len)  // 13
    
    // ===== 调用 strcmp =====
    int cmp = ffi.call_int(libc, "strcmp", "abc", "abd")
    print("比较结果: " + cmp)  // -1 (小于)
    
    // ===== 调用数学函数 =====
    // ffi.call_double 用于返回 double 的函数
    float p = ffi.call_double(libc, "pow", 2.0, 10.0)
    print("2^10 = " + p)  // 1024.0
    
    float s = ffi.call_double(libc, "sin", 1.5707963)
    print("sin(π/2) ≈ " + s)  // ~1.0
    
    // 释放库
    ffi.free(libc)
}
```

**常用 C 标准库函数：**

| 函数 | 调用方式 | 说明 |
|------|----------|------|
| `strlen` | `call_int` | 字符串长度 |
| `strcmp` | `call_int` | 字符串比较 |
| `atoi` | `call_int` | 字符串转整数 |
| `pow` | `call_double` | 幂运算 |
| `sqrt` | `call_double` | 平方根 |
| `sin/cos/tan` | `call_double` | 三角函数 |

### 1.4 使用 clib 声明式调用（推荐，零摩擦 FFI）

除了 `ffi.call_*` 动态调用方式，Leno 还支持 `clib` 声明式调用，具有**零摩擦**类型转换、编译期检查和运行时安全保障：

```leno
import ffi

// 声明 C 库函数签名
clib msvcrt {
    i32 strlen(str8 s)
    i32 strcmp(str8 a, str8 b)
    f64 pow(f64 base, f64 exp)
    str8 strerror(i32 errnum)
}

main() {
    // 加载库
    msvcrt lib = ffi.load("msvcrt.dll")

    // 返回值自动展开：i32 → int，无需 as 转换
    int len = lib.strlen("hello")
    print("strlen: " + len)

    // str8 → string 零摩擦
    string msg = lib.strerror(2)
    print("strerror(2): " + msg)

    // f64 → float 零摩擦
    float result = lib.pow(2.0, 10.0)
    print("pow(2,10): " + result)
}
```

**核心设计原则：C 的窄类型是 ABI 约束，Leno 的 int 是数学约束。ABI 在调用边界处理，数学在语言内部处理。**

- **返回值方向（C→Leno）**：自动展开，C 窄类型无缝升级为 Leno 宽类型
- **参数方向（Leno→C）**：自动窄化，Leno 宽类型安全塞入 C 窄类型，超范围报错

**clib 声明中允许的 C 布局类型：**

| 类型 | C 对应 | 大小 | 说明 |
|------|--------|------|------|
| `i8` | `int8_t` | 1 | 有符号 8 位整数 |
| `u8` | `uint8_t` | 1 | 无符号 8 位整数 |
| `i16` | `int16_t` | 2 | 有符号 16 位整数 |
| `u16` | `uint16_t` | 2 | 无符号 16 位整数 |
| `i32` | `int32_t` | 4 | 有符号 32 位整数 |
| `u32` | `uint32_t` | 4 | 无符号 32 位整数 |
| `i64` | `int64_t` | 8 | 有符号 64 位整数 |
| `u64` | `uint64_t` | 8 | 无符号 64 位整数 |
| `f32` | `float` | 4 | 单精度浮点 |
| `f64` | `double` | 8 | 双精度浮点 |
| `str8` | `char*` | 8 | C 字符串指针（UTF-8） |
| `str16` | `wchar_t*` | 8 | 宽字符串指针（UTF-16） |
| `Ptr` | `void*` | 8 | 泛型指针 |
| `bool` | `int` | 1 | C 布尔值（0/1），注意 Windows BOOL 是 `i32` |
| `cstruct名` | `struct*` | 8 | cstruct 实例自动传指针（如 `POINT` → `POINT*`） |
| `void` | `void` | - | 无返回值 |

> **注意**：clib 声明中**不允许**使用 Leno 类型（`int`、`float`、`string`），必须使用 C 布局类型。但调用时返回值会自动转换为 Leno 类型，无需手动 `as` 转换。
> **cstruct 参数说明**：clib 声明中可直接使用已定义的 cstruct 名称作为参数类型，调用时 cstruct 实例会自动转换为指针传递给 C 函数，无需手动调用 `to_ptr()`。例如 `i32 GetCursorPos(POINT pt)` 等价于 C 的 `int GetCursorPos(POINT* pt)`。
> **Windows BOOL 说明**：Windows API 中的 `BOOL` 类型实际是 `int`（4字节），不是 C 的 `_Bool`。在 clib/cfunc 中应使用 `i32` 而非 `bool`，例如 `i32 MessageBoxA(...)` 而非 `bool MessageBoxA(...)`。

#### 返回值自动展开（C→Leno 零摩擦）

clib 函数的返回值根据 C 类型自动转换为 Leno 原生类型，无需 `as` 转换：

| clib 返回类型 | Leno 类型 | 说明 |
|--------------|----------|------|
| `i8` / `u8` / `i16` / `u16` / `i32` / `u32` / `i64` | `int` | 所有 C 整数自动展开为 Leno int |
| `u64` | `int` / `BigInt` | 值 ≤ int64 最大值时为 `int`，超出时自动升级为 `BigInt` |
| `f32` / `f64` | `float` | C 浮点数自动展开为 Leno float |
| `str8` | `string` | C `char*` 自动转为 Leno string（深拷贝，安全） |
| `str16` | `string` | C `wchar_t*` 自动 UTF-16→UTF-8 转为 Leno string |
| `bool` | `bool` | C BOOL 自动转为 Leno bool |
| `Ptr` | `Ptr` / `null` | 指针原样返回，NULL 返回 null |
| `void` | `null` | 无返回值 |

```leno
clib msvcrt {
    i32 abs(i32 x)
    u64 _strtoui64(str8 s, Ptr end, i32 base)
    str8 strerror(i32 errnum)
}

msvcrt lib = ffi.load("msvcrt.dll")

// i32 → int，直接赋值，无需 as
int n = lib.abs(-42)

// u64 → int（值在范围内时）
int big = lib._strtoui64("9999999999", null, 10)

// str8 → string，零摩擦
string msg = lib.strerror(2)
```

#### 参数自动窄化（Leno→C 安全转换）

Leno `int` 可以直接传给任何 C 整数类型参数，编译器和运行时会自动处理：

| clib 参数类型 | 可接受的 Leno 实参 | 窄化行为 |
|--------------|-------------------|---------|
| `i8` | `int` | 运行时检查 [-128, 127]，超范围抛异常 |
| `u8` | `int` | 运行时检查 [0, 255]，超范围抛异常 |
| `i16` | `int` | 运行时检查 [-32768, 32767]，超范围抛异常 |
| `u16` | `int` | 运行时检查 [0, 65535]，超范围抛异常 |
| `i32` | `int` | 运行时检查 [-2147483648, 2147483647]，超范围抛异常 |
| `u32` | `int` | 运行时检查 [0, 4294967295]，超范围抛异常 |
| `i64` | `int` | 直接传递（Leno int 就是 int64） |
| `u64` | `int` | 运行时检查非负，超 int64 范围抛异常 |
| `f32` | `float` | double→float 窄化，精度损失不报错 |
| `f64` | `float` | 直接传递 |
| `str8` | `string` | Leno string 自动转为 C `char*`（临时缓冲，调用后释放） |
| `str16` | `string` | Leno string 自动 UTF-8→UTF-16 转换 |
| `Ptr` | `Ptr`、`null` | 指针和 null |
| `bool` | `bool` | true→1, false→0 |
| `cstruct名` | cstruct 实例 | 自动调用 `to_ptr()` 传指针，也兼容 `Ptr` 和 `null` |

```leno
clib msvcrt {
    i32 abs(i32 x)
}

msvcrt lib = ffi.load("msvcrt.dll")

// 正常：42 在 i32 范围内
int result = lib.abs(-42)

// 运行时报错：2147483648 超出 i32 范围
try {
    lib.abs(2147483648)
} catch {
    print("参数超出 i32 范围，正确报错")
}
```

**设计理念**：C 的窄类型是 ABI 约束（决定寄存器/栈布局），Leno 的 int 是数学约束（任意精度）。参数窄化在调用边界做范围检查，比 C 的静默截断更安全，比 Go/Rust 的手动转换更方便。

#### `as` 的使用场景

`as` 仅用于**显式截断**——当你确实想要丢弃高位时：

```leno
int big = 0x123456789ABC
int truncated = big as int32  // 显式截断，丢弃高位
```

普通 clib 调用完全不需要 `as`，返回值和参数都是自动转换的。

> **重要**：`str16` 的自动转换**仅在 clib 声明式调用中生效**。使用 `ffi.call_*` 动态调用时，字符串参数始终按 UTF-8 传递，需要手动调用 `ffi.utf8_to_utf16()` 转换。

**clib 类型转换规则：**

| 源类型 | 目标类型 | 是否允许 | 说明 |
|--------|---------|---------|------|
| `Ptr`（ffi.load 返回） | `clib` 类型 | 允许 | `msvcrt lib = ffi.load("msvcrt.dll")` |
| `clib` 类型 | `Ptr` | 允许 | `Ptr p = lib` |
| 同名 `clib` | 同名 `clib` | 允许 | `msvcrt a = b` |
| 不同名 `clib` | 不同名 `clib` | **禁止** | `msvcrt lib = user32_lib` 会编译报错 |
| `null` | `clib` 类型 | 允许 | ffi.load 失败时返回 null |

> **重要**：不同名的 `clib` 类型之间禁止互转，防止加载错误的库后调用不存在的函数导致运行时崩溃。例如 `msvcrt lib = ffi.load("user32.dll")` 虽然能通过类型检查（Ptr → clib），但后续调用 `lib.strlen()` 会在运行时失败。

**两种调用方式对比：**

| 特性 | `ffi.call_*` 动态调用 | `clib` 声明式调用 |
|------|----------------------|-------------------|
| 类型安全 | 无编译期检查 | 编译期参数和返回类型检查 |
| 调用方式 | `ffi.call_int(lib, "strlen", s)` | `lib.strlen(s)` |
| 返回值 | 直接是 Leno 类型 | 自动展开为 Leno 类型（零摩擦） |
| 参数转换 | 自动推断 | 自动窄化 + 运行时范围检查 |
| str16 自动转换 | **不支持**，需手动 `ffi.utf8_to_utf16()` | **自动转换**，直接传 `string` 即可 |
| 适用场景 | 快速原型、动态调用 | 正式项目、类型安全要求高 |

> **推荐**：涉及 Windows Unicode API（`W` 后缀函数）时，优先使用 `clib` 声明式调用，`str16` 参数会自动完成 UTF-8→UTF-16 转换，无需手动处理。

---

## 第二章：动态库操作

### 2.1 加载和释放动态库

```leno
import ffi

main() {
    // ===== Windows 常用库 =====
    var kernel32 = ffi.load("kernel32.dll")
    var user32 = ffi.load("user32.dll")
    var advapi32 = ffi.load("advapi32.dll")
    
    // ===== Linux 常用库 =====
    // var libc = ffi.load("libc.so.6")
    // var libm = ffi.load("libm.so.6")
    
    // 使用库...
    
    // 释放库（重要！避免资源泄漏）
    ffi.free(kernel32)
    ffi.free(user32)
    ffi.free(advapi32)
}
```

**平台差异：**

| 平台 | 库文件扩展名 | 示例 |
|------|-------------|------|
| Windows | `.dll` | `kernel32.dll`, `user32.dll` |
| Linux | `.so` | `libc.so.6`, `libm.so.6` |
| macOS | `.dylib` | `libSystem.dylib` |

### 2.2 调用 Windows API

```leno
import ffi

main() {
    var user32 = ffi.load("user32.dll")
    var kernel32 = ffi.load("kernel32.dll")
    
    // ===== MessageBoxA =====
    // int MessageBoxA(HWND hWnd, LPCSTR lpText, LPCSTR lpCaption, UINT uType)
    int result = ffi.call_int(user32, "MessageBoxA", 
        0,                    // hWnd: 0 表示无父窗口
        "Hello from Leno!",   // lpText: 消息内容
        "Leno FFI",           // lpCaption: 标题
        0)                    // uType: 0 表示确定按钮
    
    print("MessageBox 返回值: " + result)
    
    // ===== GetCurrentProcessId =====
    int pid = ffi.call_int(kernel32, "GetCurrentProcessId")
    print("当前进程 ID: " + pid)
    
    // ===== Sleep =====
    // void Sleep(DWORD dwMilliseconds)
    print("休眠 1 秒...")
    ffi.call_void(kernel32, "Sleep", 1000)
    print("休眠结束")
    
    ffi.free(user32)
    ffi.free(kernel32)
}
```

**常用 Windows API：**

| API | 库 | 调用方式 | 说明 |
|-----|-----|----------|------|
| `MessageBoxA` | user32 | `call_int` | 消息框 |
| `GetCurrentProcessId` | kernel32 | `call_int` | 获取进程 ID |
| `Sleep` | kernel32 | `call_void` | 休眠 |
| `Beep` | kernel32 | `call_bool` | 蜂鸣器 |

### 2.3 不同类型返回值

FFI 提供了针对不同返回类型的调用函数：

```leno
import ffi

main() {
    var kernel32 = ffi.load("kernel32.dll")
    var libc = ffi.load("msvcrt.dll")
    
    // ===== call_int - 返回整数 =====
    int pid = ffi.call_int(kernel32, "GetCurrentProcessId")
    
    // ===== call_ptr - 返回指针 =====
    // HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId)
    Ptr hProcess = ffi.call_ptr(kernel32, "OpenProcess", 0x0400, 0, pid)
    
    if hProcess != null {
        print("成功打开进程，句柄: " + hProcess)
        
        // 关闭句柄
        ffi.call_bool(kernel32, "CloseHandle", hProcess)
    }
    
    // ===== call_double - 返回浮点数 =====
    float sq = ffi.call_double(libc, "sqrt", 2.0)
    print("sqrt(2) = " + sq)
    
    // ===== call_bool - 返回布尔值 =====
    bool isWin = ffi.call_bool(kernel32, "IsDebuggerPresent")
    print("调试器是否附加: " + isWin)
    
    // ===== call_void - 无返回值 =====
    ffi.call_void(kernel32, "Sleep", 100)
    
    ffi.free(kernel32)
    ffi.free(libc)
}
```

**调用函数对照表：**

| 函数 | 返回值类型 | 适用场景 |
|------|-----------|----------|
| `call_int` | `int` | 整数返回值（strlen, GetLastError） |
| `call_ptr` | `Ptr` | 指针返回值（OpenProcess, malloc） |
| `call_double` | `float` | 浮点返回值（sqrt, sin, pow） |
| `call_bool` | `bool` | 布尔返回值（IsWindow, IsDebuggerPresent） |
| `call_void` | `null` | 无返回值（Sleep, srand） |

---

## 第三章：内存操作进阶

### 3.1 指针偏移与数组

**方法 1：字节偏移（底层方式，适用于裸 `Ptr`）**

```leno
import ffi

main() {
    // 分配能存放 5 个 int 的内存（每个 int 4 字节）
    int count = 5
    Ptr arr = ffi.calloc(count, 4)  // calloc 会清零内存

    // 写入数组元素（需要手动计算字节偏移）
    for 0 : count - 1 to i {
        ffi.write_int(arr, i * 4, (i + 1) * 10)
    }

    // 读取
    for 0 : count - 1 to i {
        int val = ffi.read_int(arr, i * 4)
        print("arr[" + i + "] = " + val)
    }

    ffi.free(arr)
}
```

**方法 2：元素索引（推荐，适用于 `Ptr[T]`）**

使用 `Ptr[T]` + `read_at`/`write_at`，无需手动计算字节偏移：

```leno
import ffi

main() {
    // 声明为 Ptr[u32]，编译器自动知道每个元素 4 字节
    int count = 5
    Ptr[u32] arr = ffi.calloc(count, 4)

    // 按索引写入，自动计算字节偏移
    for 0 : count - 1 to i {
        ffi.write_at(arr, i, (i + 1) * 10)
    }

    // 按索引读取
    for 0 : count - 1 to i {
        int val = ffi.read_at(arr, i) as int
        print("arr[" + i + "] = " + val)
    }

    ffi.free(arr)
}
```

**两种方式对比：**

| 方式 | 写入 | 读取 | 适用场景 |
|------|------|------|----------|
| 字节偏移 | `ffi.write_int(arr, i * 4, val)` | `ffi.read_int(arr, i * 4)` | 裸 `Ptr`，需要精确控制 |
| 元素索引 | `ffi.write_at(arr, i, val)` | `ffi.read_at(arr, i) as int` | `Ptr[T]`，更安全更直观 |

> **注意**：`read_at`/`write_at` 要求指针必须是 `Ptr[T]` 类型（如 `Ptr[u32]`），不支持裸 `Ptr`。返回值需要 `as int` 或 `as float` 转换为 Leno 类型。

### 3.2 多级指针

```leno
import ffi

main() {
    // 模拟二级指针：指向指针的指针
    
    // 分配最内层数据
    Ptr inner = ffi.malloc(8)
    ffi.write_int(inner, 0, 42)
    
    // 分配外层指针（存储 inner 的地址）
    Ptr outer = ffi.malloc(8)
    ffi.write_ptr(outer, 0, inner)
    
    // 通过二级指针访问数据
    // outer -> inner -> 42
    Ptr recovered = ffi.read_ptr(outer, 0)
    int val = ffi.read_int(recovered, 0)
    print("通过二级指针读取: " + val)  // 42
    
    // 释放内存（先释放内层，再释放外层）
    ffi.free(inner)
    ffi.free(outer)
}
```

### 3.3 内存拷贝

```leno
import ffi

main() {
    // 分配源和目标缓冲区
    Ptr src = ffi.malloc(100)
    Ptr dest = ffi.malloc(100)
    
    // 在源缓冲区写入数据
    ffi.write_string(src, 0, "Hello, World!")
    ffi.write_int(src, 20, 42)
    ffi.write_double(src, 24, 3.14159)
    
    // 使用 memcpy 批量拷贝（比逐字节循环快得多）
    ffi.memcpy(dest, src, 100)
    
    // 验证拷贝结果
    print("字符串: " + ffi.read_string(dest, 0))
    print("整数: " + ffi.read_int(dest, 20))
    print("浮点数: " + ffi.read_double(dest, 24))
    
    ffi.free(src)
    ffi.free(dest)
}
```

### 3.4 内存填充

`ffi.memset` 用于大块内存的快速填充，是 `for` 循环 + `write_byte` 的高效替代：

```leno
import ffi

main() {
    Ptr buf = ffi.malloc(4096)
    
    // ===== 清零缓冲区（最常见的用法）=====
    // 旧写法（低效，4096 次 VM 调用）：
    // for 0 : 4095 to i { ffi.write_byte(buf, i, 0) }
    
    // 新写法（高效，1 次 C 调用）：
    ffi.memset(buf, 0, 4096)
    
    // ===== 填充特定值 =====
    ffi.memset(buf, 0xFF, 1024)  // 前 1024 字节填充为 0xFF
    
    // 验证填充结果
    print(ffi.read_byte(buf, 0))    // 255 (0xFF)
    print(ffi.read_byte(buf, 1023)) // 255 (0xFF)
    print(ffi.read_byte(buf, 1024)) // 0
    
    ffi.free(buf)
}
```

**`memset` vs `for` 循环对比：**

| 方式 | 4096 字节清零 | 说明 |
|------|-------------|------|
| `for` + `write_byte` | 4096 次 VM→C 调用 | 极慢 |
| `ffi.memset` | 1 次 C 调用 | 极快 |

### 3.5 指针与整数互转

`ffi.ptr_from_int` 和 `ffi.ptr_to_int` 是一对互逆操作，用于在指针和整数之间转换：

```leno
import ffi

main() {
    // ===== ptr_from_int: 整数 → 指针 =====
    // Windows HKEY 根键常量需要作为指针传递
    var HKEY_LOCAL_MACHINE = ffi.ptr_from_int(0x80000002)
    var HKEY_CURRENT_USER = ffi.ptr_from_int(0x80000001)
    
    // ===== ptr_to_int: 指针 → 整数 =====
    // 获取指针的地址值（用于调试、比较）
    var buf = ffi.malloc(256)
    int addr = ffi.ptr_to_int(buf)
    print("缓冲区地址: " + addr)
    
    // 比较句柄值
    var kernel32 = ffi.load("kernel32.dll")
    Ptr hProcess = ffi.call_ptr(kernel32, "OpenProcess", 0x0400, 0, 1234)
    int handleAddr = ffi.ptr_to_int(hProcess)
    if handleAddr == -1 {
        print("无效句柄 (INVALID_HANDLE_VALUE)")
    }
    
    // ===== 往返转换 =====
    var p = ffi.ptr_from_int(0x12345678)
    int recovered = ffi.ptr_to_int(p)
    // recovered == 0x12345678
    
    // nullptr 的地址值为 0
    var np = ffi.nullptr()
    print(ffi.ptr_to_int(np))  // 0
    
    ffi.free(buf)
    ffi.free(kernel32)
}
```

**常见用途：**

| 场景 | 函数 | 说明 |
|------|------|------|
| 传递 Windows 常量句柄 | `ptr_from_int` | HKEY、INVALID_HANDLE_VALUE 等 |
| 调试打印指针地址 | `ptr_to_int` | `print("地址: " + ffi.ptr_to_int(ptr))` |
| 比较句柄值 | `ptr_to_int` | 判断是否等于特定常量 |
| 获取回调函数地址 | `ptr_to_int` | `ffi.ptr_to_int(callback_obj)` |

---

## 第四章：类型安全指针 Ptr[T]

### 4.1 Ptr 与 Ptr[T] 的区别

Leno 提供两种指针类型：

| 特性 | Ptr | Ptr[T] |
|------|-----|--------|
| 语法 | `Ptr` | `Ptr[T]` |
| 含义 | 无类型指针（类似 C 的 `void*`） | 带元素类型的指针（类似 C 的 `T*`） |
| 用途 | 通用指针，不关心指向的数据类型 | 需要知道元素类型的场景（如数组访问） |
| 大小 | `sizeof(void*)` | `sizeof(void*)` |
| 类型安全 | 低 | 高 |

**示例对比：**

```leno
import ffi

main() {
    // ===== Ptr（无类型指针）=====
    Ptr p1 = ffi.malloc(100)        // 简单的指针
    ffi.free(p1)
    
    // ===== Ptr[T]（类型安全指针）=====
    Ptr[u8] p2 = ffi.malloc(100)    // 明确知道指向的是 u8 数组
    Ptr[u32] p3 = ffi.malloc(100)   // 明确知道指向的是 u32 数组
    Ptr[u64] p4 = ffi.malloc(100)   // 明确知道指向的是 u64 数组
    
    ffi.free(p2)
    ffi.free(p3)
    ffi.free(p4)
}
```

### 4.2 Ptr[u16] 与 Ptr[str16] 的区别

`Ptr[u16]` 和 `Ptr[str16]` 语法都合法，但语义完全不同：

| 类型 | C 等价 | 含义 | 典型用途 |
|------|--------|------|----------|
| `Ptr[u16]` | `uint16_t*` / `wchar_t*` | 指向 u16 数组的指针 | UTF-16 字符串缓冲区 |
| `Ptr[str16]` | `wchar_t**` | 指向 `wchar_t*` 指针的指针（二级指针） | 命令行参数 `argv` 的宽字符版 |

**为什么不同？** `str16` 本身就是指针类型（`wchar_t*`），所以 `Ptr[str16]` 是"指向指针的指针"。而 `u16` 是值类型，`Ptr[u16]` 是"指向 u16 数组的指针"。

```leno
import ffi

main() {
    // Ptr[u16]: 指向 UTF-16 字符数据（最常用）
    // 等价于 C 的 wchar_t*
    Ptr[u16] wstr = ffi.utf8_to_utf16("你好")
    ffi.free(wstr)

    // Ptr[str16]: 指向 wchar_t* 指针（二级指针，很少用）
    // 等价于 C 的 wchar_t**
    // 典型场景: CommandLineToArgvW 的返回值
    Ptr[str16] argv = ffi.malloc(8)   // 存一个 wchar_t* 指针
    ffi.free(argv)
}
```

> **经验法则**：处理 Windows Unicode API 的字符串参数和返回值时，使用 `Ptr[u16]`（或直接在 clib 中声明为 `str16` 让编译器自动转换）。`Ptr[str16]` 仅在需要二级指针时使用。

### 4.3 使用 Ptr[T]

**基本用法：**

```leno
import ffi

main() {
    // 分配 u32 数组（10 个元素，每个 4 字节）
    Ptr[u32] arr = ffi.malloc(40)
    
    // 写入数据
    for 0 : 9 to i {
        ffi.write_int(arr, i * 4, i * 10)
    }
    
    // 读取数据
    for 0 : 9 to i {
        int val = ffi.read_int(arr, i * 4)
        print("arr[" + i + "] = " + val)
    }
    
    ffi.free(arr)
}
```

**与 FFI 函数配合使用：**

```leno
import ffi

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    // ===== 使用 Ptr[u64] 接收句柄 =====
    Ptr[u64] hProcess = ffi.call_ptr(kernel32, "GetCurrentProcess")
    print("进程句柄: " + hProcess)
    
    // ===== 使用 Ptr[u32] 接收进程 ID 列表 =====
    Ptr[u32] pidBuffer = ffi.malloc(1024)
    Ptr[u32] needed = ffi.malloc(4)
    
    var psapi = ffi.load("psapi.dll")
    int result = ffi.call_int(psapi, "EnumProcesses", pidBuffer, 1024, needed)
    
    if result != 0 {
        int count = ffi.read_int(needed, 0) / 4
        print("进程数量: " + count)
        
        for 0 : count - 1 to i {
            int pid = ffi.read_int(pidBuffer, i * 4)
            print("  PID[" + i + "] = " + pid)
        }
    }
    
    ffi.free(pidBuffer)
    ffi.free(needed)
    ffi.free(psapi)
    ffi.free(kernel32)
}
```

**类型转换：**

```leno
import ffi

main() {
    // Ptr[T] 可以自动转换为 Ptr（向上兼容）
    Ptr[u32] typed = ffi.malloc(100)
    Ptr untyped = typed        // 自动转换
    
    // Ptr 也可以自动转换为 Ptr[T]（向下兼容）
    Ptr p = ffi.malloc(100)
    Ptr[u8] p8 = p             // 自动转换
    
    ffi.free(typed)
    ffi.free(p)
}
```

### 4.4 cstruct 中的 Ptr[T]

在 cstruct 中使用 `Ptr[T]` 可以让代码更加类型安全：

```leno
import ffi

// 定义 STARTUPINFO 结构体（使用 Ptr[T]）
cstruct STARTUPINFO {
    u32 cb
    Ptr[u64] lpReserved      // 类型安全的指针
    Ptr[u64] lpDesktop       // 类型安全的指针
    Ptr[u64] lpTitle         // 类型安全的指针
    u32 dwX
    u32 dwY
    u32 dwXSize
    u32 dwYSize
    u32 dwXCountChars
    u32 dwYCountChars
    u32 dwFillAttribute
    u32 dwFlags
    u16 wShowWindow
    u16 cbReserved2
    Ptr[u64] lpReserved2     // 类型安全的指针
    Ptr[u64] hStdInput       // 类型安全的指针
    Ptr[u64] hStdOutput      // 类型安全的指针
    Ptr[u64] hStdError       // 类型安全的指针
}

// 定义 PROCESS_INFORMATION 结构体
cstruct PROCESS_INFORMATION {
    Ptr[u64] hProcess        // 类型安全的句柄
    Ptr[u64] hThread         // 类型安全的句柄
    u32 dwProcessId
    u32 dwThreadId
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    var si = STARTUPINFO.malloc()
    var pi = PROCESS_INFORMATION.malloc()
    
    try {
        si.cb = STARTUPINFO.size()
        
        // 使用 Ptr[u16] 转换命令行
        Ptr[u16] cmdLine = ffi.utf8_to_utf16("notepad.exe")
        
        try {
            int result = ffi.call_int(kernel32, "CreateProcessW",
                ffi.nullptr(), cmdLine, ffi.nullptr(), ffi.nullptr(),
                0, 0x00000010, ffi.nullptr(), ffi.nullptr(),
                si, pi)
            
            if result != 0 {
                print("进程创建成功!")
                print("  进程 ID: " + pi.dwProcessId)
                print("  线程 ID: " + pi.dwThreadId)
                print("  进程句柄: " + pi.hProcess)
                print("  线程句柄: " + pi.hThread)
                
                // 关闭句柄
                ffi.call_int(kernel32, "CloseHandle", pi.hProcess)
                ffi.call_int(kernel32, "CloseHandle", pi.hThread)
            } else {
                print("进程创建失败")
            }
        } finally {
            ffi.free(cmdLine)
        }
    } finally {
        si.free()
        pi.free()
        ffi.free(kernel32)
    }
}
```

**Ptr[T] 在 cstruct 中的优势：**

1. **类型安全**：编译时可以检查类型错误
2. **语义清晰**：一眼就能看出字段的用途（如 `Ptr[u64]` 表示句柄，`Ptr[u16]` 表示 UTF-16 字符串）
3. **FFI 兼容**：`Ptr[T]` 可以自动转换为 FFI 函数期望的 `Ptr` 类型

**支持的元素类型：**

```leno
Ptr[u8]     // 字节指针
Ptr[u16]    // 16位无符号整数指针（常用于 UTF-16 字符串）
Ptr[u32]    // 32位无符号整数指针
Ptr[u64]    // 64位无符号整数指针（常用于句柄）
Ptr[i8]     // 8位有符号整数指针
Ptr[i16]    // 16位有符号整数指针
Ptr[i32]    // 32位有符号整数指针
Ptr[i64]    // 64位有符号整数指针
Ptr[f32]    // 32位浮点数指针
Ptr[f64]    // 64位浮点数指针
```

---

## 第五章：字符串处理

### 5.1 字符串读写

```leno
import ffi

main() {
    Ptr buf = ffi.malloc(256)
    
    // 写入字符串
    ffi.write_string(buf, 0, "Hello")
    
    // 读取整个字符串
    string s = ffi.read_string(buf, 0)
    print("完整字符串: " + s)
    
    // 读取指定长度
    string partial = ffi.read_string_n(buf, 0, 3)
    print("前 3 个字符: " + partial)
    
    // 在偏移处写入另一个字符串
    ffi.write_string(buf, 10, "World")
    print("偏移 10 处: " + ffi.read_string(buf, 10))
    
    ffi.free(buf)
}
```

### 5.2 UTF-8 与 UTF-16 转换

Windows API 通常有 ANSI 版本（A 后缀）和 Unicode 版本（W 后缀）。Unicode 版本需要 UTF-16 字符串。

**方式一：clib 声明式调用（推荐，自动转换）**

```leno
import ffi

// 声明时使用 str16，调用时自动完成 UTF-8→UTF-16 转换
// 返回值 str16 也会自动转为 Leno string（零摩擦）
clib user32 {
    i32 MessageBoxW(Ptr hWnd, str16 lpText, str16 lpCaption, i32 uType)
}

main() {
    user32 lib = ffi.load("user32.dll")
    // 直接传 string，clib 自动转为 UTF-16，调用后自动释放临时内存
    // 返回值 i32 自动展开为 Leno int，无需 as
    int r = lib.MessageBoxW(null, "你好，世界！", "中文标题", 0)
    print("返回: " + r)
    ffi.free(lib)
}
```

**方式二：ffi.call_* 动态调用（手动转换）**

```leno
import ffi

main() {
    var user32 = ffi.load("user32.dll")
    
    // ===== ANSI 版本（MessageBoxA）=====
    ffi.call_int(user32, "MessageBoxA", 0, 
        "Hello World", "ANSI", 0)
    
    // ===== Unicode 版本（MessageBoxW）=====
    // 需要手动将 UTF-8 字符串转换为 UTF-16
    Ptr[u16] wstr = ffi.utf8_to_utf16("你好，世界！")
    Ptr[u16] wtitle = ffi.utf8_to_utf16("Unicode")
    
    ffi.call_int(user32, "MessageBoxW", 0, wstr, wtitle, 0)
    
    // 手动释放转换后的内存
    ffi.free(wstr)
    ffi.free(wtitle)
    
    // ===== 读取 UTF-16 字符串 =====
    // 分配缓冲区接收 UTF-16 数据
    Ptr[u16] buffer = ffi.malloc(512)
    
    // 假设某个 Windows API 返回 UTF-16 数据到 buffer
    // ...
    
    // 转换为 UTF-8 字符串
    string str = ffi.utf16_to_utf8(buffer)
    print("转换后的字符串: " + str)
    
    ffi.free(buffer)
    ffi.free(user32)
}
```

**两种方式对比：**

| 特性 | clib + `str16` | `ffi.call_*` + 手动转换 |
|------|---------------|------------------------|
| 转换方式 | 自动 | 手动 `ffi.utf8_to_utf16()` |
| 内存释放 | 自动 | 手动 `ffi.free()` |
| 代码量 | 少 | 多 |
| 适用场景 | 推荐 | 兼容旧代码 |

---

## 第六章：实战案例

### 6.1 进程枚举

```leno
import ffi

// 进程信息结构（使用 str16 自动 UTF-16 转换）
export cstruct PROCESSENTRY32W {
    u32 dwSize
    u32 cntUsage
    u32 th32ProcessID
    u64 th32DefaultHeapID
    u32 th32ModuleID
    u32 cntThreads
    u32 th32ParentProcessID
    i32 pcPriClassBase
    u32 dwFlags
    str16 szExeFile[260]  // 自动 UTF-16/UTF-8 转换
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    // 创建进程快照
    // HANDLE CreateToolhelp32Snapshot(DWORD dwFlags, DWORD th32ProcessID)
    Ptr[u64] hSnapshot = ffi.call_ptr(kernel32, "CreateToolhelp32Snapshot", 0x00000002, 0)
    
    if not hSnapshot {
        print("创建快照失败")
        return
    }
    
    try {
        // 分配 PROCESSENTRY32W 结构体
        var pe = PROCESSENTRY32W.malloc()
        pe.dwSize = PROCESSENTRY32W.size()
        
        // 获取第一个进程
        int result = ffi.call_int(kernel32, "Process32FirstW", hSnapshot, pe)
        
        print("进程列表:")
        print("=========")
        
        while result != 0 {
            int pid = pe.th32ProcessID
            int parentPid = pe.th32ParentProcessID
            int threads = pe.cntThreads
            
            // 直接读取 str16 字段（自动转换为 UTF-8）
            string name = pe.szExeFile
            
            print(name + " (PID: " + pid + ", Parent: " + parentPid + ", Threads: " + threads + ")")
            
            // 获取下一个进程
            result = ffi.call_int(kernel32, "Process32NextW", hSnapshot, pe)
        }
        
        pe.free()
    } finally {
        ffi.call_bool(kernel32, "CloseHandle", hSnapshot)
        ffi.free(kernel32)
    }
}
```

### 6.2 注册表操作

```leno
import ffi

main() {
    var advapi32 = ffi.load("advapi32.dll")
    
    // HKEY 常量（作为指针传递）
    var HKEY_CURRENT_USER = ffi.ptr_from_int(0x80000001)
    
    // 分配缓冲区接收打开的键句柄
    Ptr[u64] hKeyBuffer = ffi.malloc(8)
    
    try {
        // 打开注册表键
        // RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult)
        int result = ffi.call_int(advapi32, "RegOpenKeyExW",
            HKEY_CURRENT_USER,           // 根键
            ffi.utf8_to_utf16("Software\\Microsoft\\Windows\\CurrentVersion"),  // 子键路径
            0,                           // 选项
            0x20019,                     // KEY_READ 权限
            hKeyBuffer)                  // 输出参数：接收句柄
        
        if result == 0 {  // ERROR_SUCCESS
            Ptr[u64] hKey = ffi.read_ptr(hKeyBuffer, 0)
            print("成功打开注册表键")
            
            // 读取值
            Ptr[u16] dataBuffer = ffi.malloc(512)
            Ptr[u32] dataSize = ffi.malloc(4)
            ffi.write_int(dataSize, 0, 512)
            
            int queryResult = ffi.call_int(advapi32, "RegQueryValueExW",
                hKey,
                ffi.utf8_to_utf16("ProgramFilesDir"),
                0,
                0,
                dataBuffer,
                dataSize)
            
            if queryResult == 0 {
                string value = ffi.utf16_to_utf8(dataBuffer)
                print("ProgramFilesDir: " + value)
            }
            
            ffi.free(dataBuffer)
            ffi.free(dataSize)
            
            // 关闭键
            ffi.call_int(advapi32, "RegCloseKey", hKey)
        } else {
            print("打开注册表键失败，错误码: " + result)
        }
        
    } finally {
        ffi.free(hKeyBuffer)
        ffi.free(advapi32)
    }
}
```

### 6.3 结合 cstruct 使用

#### 方式一：clib 声明式调用（推荐，cstruct 自动传指针）

```leno
import ffi

// 定义 POINT 结构体
cstruct POINT {
    i32 x
    i32 y
}

// 定义 RECT 结构体
cstruct RECT {
    i32 left
    i32 top
    i32 right
    i32 bottom
}

// clib 声明中直接使用 cstruct 名作为参数类型
clib user32 {
    i32 GetCursorPos(POINT lpPoint)
    i32 GetWindowRect(Ptr hWnd, RECT lpRect)
    i32 SetCursorPos(i32 x, i32 y)
}

main() {
    user32 lib = ffi.load("user32.dll")

    // cstruct 实例直接传入，自动转指针，无需手动 to_ptr()
    POINT pt = POINT.malloc()
    int ok1 = lib.GetCursorPos(pt)
    if ok1 != 0 {
        print("鼠标位置: x=" + pt.x + ", y=" + pt.y)
    }

    RECT rc = RECT.malloc()
    int ok2 = lib.GetWindowRect(null, rc)
    if ok2 != 0 {
        print("窗口: left=" + rc.left + " top=" + rc.top)
    }

    // 也可以手动传 Ptr（兼容旧写法）
    int ok3 = lib.GetCursorPos(pt.to_ptr())

    pt.free()
    rc.free()
    ffi.free(lib)
}
```

#### 方式二：ffi.call_* 动态调用（需手动传指针）

```leno
import ffi

cstruct POINT {
    i32 x
    i32 y
}

main() {
    var user32 = ffi.load("user32.dll")

    var pt = POINT.malloc()

    // 动态调用时 cstruct 实例自动作为指针传递
    bool success = ffi.call_bool(user32, "GetCursorPos", pt)

    if success {
        print("鼠标位置: x=" + pt.x + ", y=" + pt.y)
    }

    ffi.free(user32)
}
```

---

## 第七章：回调函数

FFI 回调允许将 Leno 函数作为函数指针传递给 C 代码，使 C 函数能够回调 Leno 函数。

### 7.1 cfunc 声明式回调

Leno 提供 `cfunc` 声明来定义回调函数签名，与 `clib` 风格统一，具有编译期类型检查：

```leno
import ffi

// 声明 cfunc 回调签名（只能使用 C 布局类型）
cfunc Compare(Ptr a, Ptr b): i32

// 实现 Leno 函数（参数和返回值类型与 cfunc 签名匹配）
func my_compare(Ptr a, Ptr b): int {
    int va = ffi.read_int(a, 0)
    int vb = ffi.read_int(b, 0)
    if va < vb { return -1 }
    if va > vb { return 1 }
    return 0
}

main() {
    // 使用 cfunc 签名创建回调：ffi.callback(函数, cfunc名称)
    var cb = ffi.callback(my_compare, Compare)
    print("回调创建成功: " + (cb != null))
}
```

**`ffi.callback(func, cfunc_name)` 参数说明：**

| 参数 | 类型 | 说明 |
|------|------|------|
| `func` | 函数/闭包 | Leno 函数，将被 C 代码回调 |
| `cfunc_name` | cfunc 名称 | cfunc 声明的签名名称，提供编译期类型信息 |

**cfunc 声明中允许的类型（与 clib 相同）：**

| 类型 | C 对应 | 说明 |
|------|--------|------|
| `i32` | `int32_t` | 有符号 32 位整数 |
| `i64` | `int64_t` | 有符号 64 位整数 |
| `u32` | `uint32_t` | 无符号 32 位整数 |
| `u64` | `uint64_t` | 无符号 64 位整数 |
| `f32` | `float` | 单精度浮点 |
| `f64` | `double` | 双精度浮点 |
| `Ptr` | `void*` | 泛型指针 |
| `str8` | `char*` | C 字符串指针 |
| `str16` | `wchar_t*` | 宽字符串指针 |
| `bool` | `_Bool` | C 布尔值，Windows BOOL 用 `i32` |
| `void` | `void` | 无返回值 |

> **注意**：cfunc 声明中**不允许**使用 Leno 类型（`int`、`float`、`string`），必须使用 C 布局类型。例如 `cfunc Foo(int a): int` 会编译报错，应改为 `cfunc Foo(i32 a): i32`。

### 7.2 回调与 qsort

使用 `cfunc` + `clib` 调用 `qsort` 的完整示例：

```leno
import ffi

// 声明 cfunc 回调签名
cfunc Compare(Ptr a, Ptr b): i32

// 实现比较函数
func my_compare(Ptr a, Ptr b): int {
    int va = ffi.read_int(a, 0)
    int vb = ffi.read_int(b, 0)
    if va < vb { return -1 }
    if va > vb { return 1 }
    return 0
}

// 声明 C 库函数签名
clib msvcrt {
    void qsort(Ptr base, i32 nmemb, i32 size, Ptr compar)
}

main() {
    msvcrt lib = ffi.load("msvcrt.dll")

    // 创建 5 个 u32 的数组
    int n = 5
    Ptr[u32] arr = ffi.malloc(n * 4)
    ffi.write_at(arr, 0, 50)
    ffi.write_at(arr, 1, 20)
    ffi.write_at(arr, 2, 40)
    ffi.write_at(arr, 3, 10)
    ffi.write_at(arr, 4, 30)

    print("排序前:")
    for n to i {
        print("  arr[" + i + "] = " + ffi.read_at(arr, i))
    }

    // 创建 cfunc 回调并调用 qsort
    var cb = ffi.callback(my_compare, Compare)
    lib.qsort(arr, n, 4, cb)

    print("排序后:")
    for n to i {
        print("  arr[" + i + "] = " + ffi.read_at(arr, i))
    }

    ffi.free(arr)
}
```

**输出：**
```
排序前:
  arr[0] = 50
  arr[1] = 20
  arr[2] = 40
  arr[3] = 10
  arr[4] = 30
排序后:
  arr[0] = 10
  arr[1] = 20
  arr[2] = 30
  arr[3] = 40
  arr[4] = 50
```

### 7.3 释放回调

回调对象使用 `ffi.free()` 统一释放，与释放指针和库对象的方式相同：

```leno
var cb = ffi.callback(my_func, MyCfunc)
// ... 使用回调 ...
ffi.free(cb)  // 释放回调资源
```

**注意事项：**
- 回调对象在 GC 回收时会自动释放，但建议手动 `ffi.free()` 以控制生命周期
- 释放回调后，对应的 C 函数指针不再有效，继续调用会导致未定义行为
- 最多支持 128 个同时存在的回调（全局注册表限制）
- 回调函数中的参数类型为 `Ptr` 时，需要通过 `ffi.read_*` 读取数据

### 7.5 平台支持

FFI 回调功能在不同平台上的支持情况：

| 平台 | 架构 | 支持状态 | 调用约定 |
|-----|------|---------|---------|
| Windows | x86-64 | ✅ 完全支持 | Microsoft x64 calling convention |
| Linux | x86-64 | ✅ 支持 | System V AMD64 ABI |
| macOS | x86-64 | ✅ 支持 | System V AMD64 ABI |
| ARM64 | - | ❌ 不支持 | 需要额外实现 |

**调用约定差异：**

| 特性 | Windows x64 | System V AMD64 (Linux/macOS) |
|-----|-------------|------------------------------|
| 整数参数寄存器 | RCX, RDX, R8, R9 | RDI, RSI, RDX, RCX, R8, R9 |
| 浮点参数寄存器 | XMM0-XMM3 | XMM0-XMM7 |
| 最大整数参数 | 4 个 | 6 个 |
| 最大浮点参数 | 4 个 | 8 个 |
| Shadow space | 32 bytes | 无 |

---

## 第八章：最佳实践

### 8.1 错误处理

#### 使用 `ffi.last_error()` 获取系统错误码

每次 FFI 调用（`ffi.call_*` 或 `clib` 调用）返回后，Leno 会立即缓存 `GetLastError()`（Windows）或 `errno`（Linux/macOS）的值。通过 `ffi.last_error()` 可以安全地获取该错误码，避免后续 Leno 内部操作覆盖线程错误码。

```leno
import ffi

clib kernel32 {
    Ptr OpenProcess(i32 access, i32 inherit, i32 pid)
    i32 CloseHandle(Ptr h)
    i32 GetCurrentProcessId()
}

main() {
    kernel32 k32 = ffi.load("kernel32.dll")

    // 成功调用后 last_error 通常为 0
    int pid = k32.GetCurrentProcessId()
    int err1 = ffi.last_error()
    print("GetCurrentProcessId 后 last_error: " + err1)

    // 失败调用后 last_error 为非零错误码
    Ptr h = k32.OpenProcess(0x0400, 0, 9999999)
    int err2 = ffi.last_error()
    if h == null {
        print("OpenProcess 失败，错误码: " + err2)  // 如 87 = ERROR_INVALID_PARAMETER
    } else {
        k32.CloseHandle(h)
    }

    ffi.free(k32)
}
```

> **注意**：成功的 API 调用**不一定**重置错误码为 0，这取决于具体 API 的行为。例如 `GetCurrentProcessId` 不会重置 `LastError`。

#### 错误处理最佳实践

```leno
import ffi

// 安全的库加载
func safeLoadLibrary(string name) {
    var lib = ffi.load(name)
    if lib == null {
        print("[错误] 无法加载库: " + name)
    }
    return lib
}

// 安全的指针检查
func safePtr(Ptr p, string context) {
    if not p {
        print("[错误] 空指针: " + context)
        return false
    }
    return true
}

// 检查 Windows API 错误
func checkWinError(int code, string operation) {
    if code != 0 {  // ERROR_SUCCESS = 0
        print("[错误] " + operation + " 失败，错误码: " + code)
        return false
    }
    return true
}

main() {
    var kernel32 = safeLoadLibrary("kernel32.dll")
    if not kernel32 { return }
    
    // 使用 try-finally 确保资源释放
    try {
        Ptr[u64] hProcess = ffi.call_ptr(kernel32, "OpenProcess", 0x0400, 0, 1234)
        
        if not safePtr(hProcess, "OpenProcess") {
            return
        }
        
        try {
            // 使用 hProcess...
            print("成功打开进程")
        } finally {
            ffi.call_bool(kernel32, "CloseHandle", hProcess)
        }
        
    } finally {
        ffi.free(kernel32)
    }
}
```

### 8.2 内存管理

**统一释放：**
- `ffi.free(x)` - 统一释放 FFI 资源（指针/库/回调），一个函数搞定！

```leno
import ffi

// ===== 原则 1: 谁分配谁释放 =====
func processData() {
    Ptr buffer = ffi.malloc(1024)
    
    try {
        // 处理数据...
        ffi.write_string(buffer, 0, "processed data")
        print(ffi.read_string(buffer, 0))
    } finally {
        // 确保释放内存
        ffi.free(buffer)
    }
}

// ===== 动态库释放示例 =====
func useLibrary() {
    var lib = ffi.load("kernel32.dll")
    
    try {
        int pid = ffi.call_int(lib, "GetCurrentProcessId")
        print("PID: " + pid)
    } finally {
        // 释放动态库
        ffi.free(lib)
    }
}

// ===== 原则 2: offset 返回的指针不需要释放 =====
func useOffset() {
    Ptr base = ffi.malloc(100)
    
    // offset 返回的是视图，不拥有内存
    Ptr view = ffi.offset(base, 10)
    
    // 只需要释放 base
    ffi.free(base)
    // 不要: ffi.free(view)  <- 错误！
}

// ===== 原则 3: 使用 calloc 清零内存 =====
func useCalloc() {
    // calloc 分配的内存初始化为 0
    Ptr arr = ffi.calloc(10, 4)  // 10 个 int，共 40 字节，全部清零
    
    // 可以直接使用，无需初始化
    int first = ffi.read_int(arr, 0)  // 0
    print("第一个元素: " + first)
    
    ffi.free(arr)
}

// ===== 原则 4: 及时释放不再使用的内存 =====
func processLargeData() {
    // 阶段 1: 读取数据
    Ptr rawData = ffi.malloc(1000000)
    // ... 填充 rawData ...
    
    // 阶段 2: 处理数据（需要新缓冲区）
    Ptr processedData = ffi.malloc(500000)
    
    // 可以立即释放 rawData，如果不再需要
    ffi.free(rawData)
    rawData = null  // 置空避免误用
    
    // ... 使用 processedData ...
    
    ffi.free(processedData)
}
```

### 8.3 类型安全

```leno
import ffi

// ===== 使用类型守卫 =====
func handleResult(var result) {
    if result is int {
        print("整数结果: " + result)
    } else if result is Ptr {
        print("指针结果: " + result)
        if result == null {
            print("  (空指针)")
        }
    } else if result is float {
        print("浮点结果: " + result)
    } else {
        print("其他类型: " + result)
    }
}

// ===== 使用 Ptr[T] 类型注解 =====
func processHandle(Ptr[u64] hProcess) {
    // 参数类型明确为 Ptr[u64]
    if not hProcess {
        print("无效句柄")
        return
    }
    print("处理句柄: " + hProcess)
}

// ===== 检查指针有效性 =====
func isValidPtr(var value) {
    if value is Ptr {
        Ptr p = value
        return p != null
    }
    return false
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    // 通用调用返回 any 类型
    var result = ffi.call(kernel32, "GetCurrentProcessId")
    handleResult(result)
    
    // 明确类型调用更安全
    int pid = ffi.call_int(kernel32, "GetCurrentProcessId")
    print("明确类型的 PID: " + pid)
    
    // 使用 Ptr[u64] 接收句柄
    Ptr[u64] hProcess = ffi.call_ptr(kernel32, "GetCurrentProcess")
    processHandle(hProcess)
    
    ffi.free(kernel32)
}
```

### 8.4 常见陷阱与注意事项

本章汇总开发 Leno FFI 模块（如 LenoMusic）过程中发现的实际问题，避免踩坑。

#### 陷阱 1：`as int` 在字符串拼接中的优先级问题

`as` 运算符在早期版本中优先级低于 `+`，导致 `"str" + x as int + "%"` 被解析为 `("str" + x) as int + "%"`，结果变成 `null%`。**现已修复**（v1.x+），`as` 优先级高于 `+`。

```leno
// ✅ 现在正确: 先 as int 再拼接
var s = "x=" + (99.9 as int) + "%"   // → "x=99%"

// ✅ 更推荐: 使用 _int() 显式转换，避免歧义
int p = _int(pct * 100)
var bar = "[" + filled + "] " + p + "%"
```

> **建议**：字符串拼接中涉及类型转换时，优先用 `_int()`/`_float()` 先赋值给变量，再参与拼接，代码更清晰。

#### 陷阱 2：clib 类型跨函数传递

`clib` 类型经过函数返回值时，之前会丢失类型信息（被当作 `struct`）。**现已修复**。

```leno
clib kernel32 { i32 GetTickCount() }

// ✅ 现在可用（已修复）
func get_k32(): kernel32 { return ffi.load("kernel32.dll") }
var t = get_k32().GetTickCount()  // 链式调用

// ✅ 开发时推荐: 直接声明，避免不必要的包装
kernel32 k = ffi.load("kernel32.dll")
var t = k.GetTickCount()
```

#### 陷阱 3：buffer 大小计算

`ffi.malloc` 分配的是**字节数**，必须根据元素大小和数量计算。Leno 的 `ffi.malloc` 会**自动追踪**分配大小，可用 `ffi.assert_size()` 在调用前验证：

```leno
// ❌ 错误: 1024 帧 × 2 通道 × 4 字节(f32) = 8192 字节，4096 不够！
var buf = ffi.malloc(4096)
w.read(buf, 1024)  // 堆溢出！ffi.free 崩溃，极难排查

// ✅ 方案 1: 用 ffi.assert_size 提前检测（推荐）
var buf = ffi.malloc(4096)
ffi.assert_size(buf, 1024 * 2 * 4)  // 抛出: "缓冲区溢出！需要 8192 字节，实际只有 4096 字节"

// ✅ 方案 2: 手动计算所需字节数
int frames = 1024; int ch = 2
int bytes = frames * ch * 4  // f32 = 4 字节
var buf = ffi.malloc(bytes)
ffi.assert_size(buf, bytes)  // 不会报错
w.read(buf, frames)

// ✅ 方案 3: 预留足够空间
var buf = ffi.malloc(4800 * 4 * 2)  // 38400 字节
```

> **关键**：`ffi.assert_size(ptr, min)` 在缓冲区不足时立即抛出明确错误，而非等到 `ffi.free` 时崩溃。

#### `ffi.assert_size` 详解

```leno
// 基本用法
ffi.assert_size(ptr, min_bytes)

// 用于 clib 调用前验证
var buf = ffi.malloc(4096)
ffi.assert_size(buf, 1024 * 2 * 4)  // 1024帧×2通道×4字节
w.read(buf, 1024)  // 安全
ffi.free(buf)
```

**行为：**
- 指针大小已知且 ≥ `min_bytes` → 返回 true，继续执行
- 指针大小已知但 < `min_bytes` → **抛出明确错误** "缓冲区溢出！需要 X 字节，实际只有 Y 字节"
- 指针大小未知（非 ffi.malloc/calloc 分配，如 `ffi.ptr_from_int`）→ 抛出 "缓冲区大小未知"
- 指针有符号类型 `Ptr[T]` → `sizeof` 返回分配字节数，不受元素类型影响

#### 陷阱 4：`f32` vs `f64` 精度

clib 声明中使用 `f32` 参数时，Leno `float`（即 f64）到 `f32` 的转换由 C 编译器自动截断，精度损失不报错。但某些平台（如 x64 Windows 混合参数回退策略）可能产生异常值：

```leno
clib miniaudio {
    // ❌ 之前: f32 参数在混合参数回退时精度异常
    // void set_volume(Ptr h, f32 v)

    // ✅ 推荐: 用 f64/f32 均可，但建议直接匹配 DLL 实际类型
    void set_volume(Ptr h, f64 v)
    f64 get_volume(Ptr h)
}
```

> **建议**：DLL 函数签名尽量使用 `double`/`f64`，避免 f32 在不同调用约定下的精度问题。

#### 陷阱 5：`Ptr` 大小写

clib/cfunc 声明中必须使用大写 `Ptr`，小写 `ptr` 会报编译错误：

```leno
// ❌ 编译错误: 未知类型 'ptr'，您是否想使用 'Ptr'？
clib test { ptr func() }

// ✅ 正确
clib test { Ptr func() }
```

#### 陷阱 6：`files.exists()` vs `files.open()`

检测文件是否存在时，用 `files.exists()`。`files.open(path, "r")` 在文件不存在时会**抛出运行时异常**而非返回 null：

```leno
// ❌ 错误: 文件不存在时抛异常，流程中断
var f = files.open("miniaudio.dll", "r")

// ✅ 正确: 安全检测
if files.exists("miniaudio.dll") {
    g_path = "miniaudio.dll"
}
```

#### 陷阱 7：clib 声明中不允许使用 Leno 类型

```leno
// ❌ 编译错误
clib test {
    int func(float x, string s)
}

// ✅ 正确: 使用 C 布局类型
clib test {
    i32 func(f64 x, str8 s)
}
```

#### 陷阱 8：`ffi.free` 统一释放原则

`ffi.free` 统一释放指针、库句柄、回调对象：

```leno
// ✅ 统一用 ffi.free
ffi.free(buf)      // 释放内存
ffi.free(lib)      // 释放动态库
ffi.free(cb)       // 释放回调

// ❌ 错误: offset 返回的视图不需要释放
var view = ffi.offset(base, 10)
// ffi.free(view)  ← 不要！只释放 base
ffi.free(base)
```

---

## 附录：完整 API 速查表

### 动态库操作

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `load(path)` | string | lib\|null | 加载动态库 |
| `free(x)` | lib/ptr/callback | null | 统一释放 FFI 资源（指针/库/回调） |

> ⚠️ **注意**：`ffi.free(x)` 统一释放 FFI 资源（指针/库/回调），一个函数搞定！

### 函数调用

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `call(lib, name, ...)` | lib, string, ... | any | 通用调用（返回 any） |
| `call_int(lib, name, ...)` | lib, string, ... | int | 调用返回 int |
| `call_ptr(lib, name, ...)` | lib, string, ... | Ptr | 调用返回指针 |
| `call_double(lib, name, ...)` | lib, string, ... | float | 调用返回 double |
| `call_bool(lib, name, ...)` | lib, string, ... | bool | 调用返回 bool |
| `call_void(lib, name, ...)` | lib, string, ... | null | 调用无返回值 |
| `last_error()` | 无 | int | 获取最近一次 FFI 调用后的系统错误码 |

### 内存管理

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `alloc(type_name, value?)` | string, any | Ptr | 分配指定类型内存并初始化 |
| `malloc(size)` | int | Ptr | 分配内存 |
| `calloc(count, size)` | int, int | Ptr | 分配并清零 |
| `realloc(ptr, size)` | Ptr, int | Ptr | 重新分配 |
| `free(x)` | ptr/lib/callback | null | 统一释放 FFI 资源 |
| `memcpy(dest, src, size)` | Ptr, Ptr, int | null | 内存拷贝 |
| `memset(ptr, value, size)` | Ptr, int, int | null | 内存填充 |
| `nullptr()` | 无 | Ptr | 空指针 |
| `sizeof(ptr)` | Ptr | int | 返回分配大小（字节） |
| `assert_size(ptr, min)` | Ptr, int | bool | 断言缓冲区 ≥ min 字节（不足抛错） |

### 内存读写

| 读取函数 | 写入函数 | 大小 |
|----------|----------|------|
| `read_bool(ptr, off)` | `write_bool(ptr, off, val)` | 1 字节 |
| `read_byte(ptr, off)` | `write_byte(ptr, off, val)` | 1 字节 |
| `read_int8(ptr, off)` | `write_int8(ptr, off, val)` | 1 字节 |
| `read_int16(ptr, off)` | `write_int16(ptr, off, val)` | 2 字节 |
| `read_uint16(ptr, off)` | `write_uint16(ptr, off, val)` | 2 字节 |
| `read_int(ptr, off)` | `write_int(ptr, off, val)` | 4 字节 |
| `read_uint(ptr, off)` | `write_uint(ptr, off, val)` | 4 字节 |
| `read_int64(ptr, off)` | `write_int64(ptr, off, val)` | 8 字节 |
| `read_uint64(ptr, off)` | `write_uint64(ptr, off, val)` | 8 字节 |
| `read_float(ptr, off)` | `write_float(ptr, off, val)` | 4 字节 |
| `read_double(ptr, off)` | `write_double(ptr, off, val)` | 8 字节 |
| `read_ptr(ptr, off)` | `write_ptr(ptr, off, val)` | 8 字节 |
| `read_string(ptr, off)` | `write_string(ptr, off, str)` | - |
| `read_string_n(ptr, off, len)` | - | - |

### 指针操作

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `offset(ptr, off)` | Ptr, int | Ptr | 指针偏移 |
| `ptr_from_int(addr)` | int | Ptr | 整数转指针 |
| `ptr_to_int(ptr)` | ptr/callback | int | 指针地址转整数 |
| `is_ptr(value)` | any | bool | 检查是否指针或回调对象 |

### 回调函数

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `callback(func, cfunc_name)` | 函数, cfunc名称 | callback | 创建 FFI 回调（需先声明 cfunc） |

### 字符串工具

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `string_bytes(str)` | string | int | 字符串字节长度 |
| `utf8_to_utf16(str)` | string | Ptr[u16] | UTF-8 转 UTF-16 |
| `utf16_to_utf8(ptr)` | Ptr[u16] | string | UTF-16 转 UTF-8 |

### 类型信息

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `sizeof_type(type_name)` | string | int | 获取 C 类型大小（字节） |
| `alignof(type_name)` | string | int | 获取 C 类型对齐（字节） |

### clib 声明类型

| 类型 | 说明 | 示例 |
|------|------|------|
| `Ptr[u8]` | 字节指针 | `Ptr[u8] p = ffi.malloc(100)` |
| `Ptr[u16]` | 16位无符号整数指针 | `Ptr[u16] s = ffi.utf8_to_utf16("text")` |
| `Ptr[u32]` | 32位无符号整数指针 | `Ptr[u32] arr = ffi.malloc(40)` |
| `Ptr[u64]` | 64位无符号整数指针（句柄） | `Ptr[u64] h = ffi.call_ptr(lib, "OpenProcess", ...)` |
| `Ptr[i8]` | 8位有符号整数指针 | `Ptr[i8] p = ffi.malloc(100)` |
| `Ptr[i16]` | 16位有符号整数指针 | `Ptr[i16] p = ffi.malloc(100)` |
| `Ptr[i32]` | 32位有符号整数指针 | `Ptr[i32] p = ffi.malloc(100)` |
| `Ptr[i64]` | 64位有符号整数指针 | `Ptr[i64] p = ffi.malloc(100)` |
| `Ptr[f32]` | 32位浮点数指针 | `Ptr[f32] p = ffi.malloc(100)` |
| `Ptr[f64]` | 64位浮点数指针 | `Ptr[f64] p = ffi.malloc(100)` |

### clib 返回值自动展开

| clib 返回类型 | Leno 类型 | 说明 |
|--------------|----------|------|
| `i8`/`u8`/`i16`/`u16`/`i32`/`u32`/`i64` | `int` | 自动展开 |
| `u64` | `int`/`BigInt` | 超出 int64 范围时自动升级 |
| `f32`/`f64` | `float` | 自动展开 |
| `str8` | `string` | char* → string（深拷贝） |
| `str16` | `string` | wchar_t* → string（UTF-16→UTF-8） |
| `bool` | `bool` | 自动转换 |
| `Ptr` | `Ptr`/`null` | NULL 返回 null |
| `void` | `null` | 无返回值 |

### clib 参数自动窄化

| clib 参数类型 | Leno 实参 | 窄化行为 |
|--------------|----------|---------|
| `i8`/`u8`/`i16`/`u16`/`i32`/`u32` | `int` | 运行时范围检查，超范围抛异常 |
| `i64` | `int` | 直接传递 |
| `u64` | `int` | 运行时检查非负 |
| `f32`/`f64` | `float` | f32 精度窄化，f64 直接传递 |
| `str8` | `string` | 自动转为 char* |
| `str16` | `string` | 自动 UTF-8→UTF-16 转换 |
| `Ptr` | `Ptr`/`null` | 直接传递 |

---

*文档版本: 3.3*
*最后更新: 2026-06-19*

### 更新记录

- **v3.3** (2026-06-19):
  - 新增 **8.4 常见陷阱与注意事项**：汇总 LenoMusic 开发中的 8 个实战踩坑：
    `as int` 优先级、clib 链式调用、buffer 大小计算、f32/f64 精度、Ptr 大小写、files.exists/open 区别、clib 类型限制、ffi.free 统一释放

- **v3.2** (2026-06-07):
  - **clib 支持 cstruct 参数**：clib 声明中可直接使用 cstruct 名称作为参数类型，调用时 cstruct 实例自动转换为指针传递，无需手动 `to_ptr()`
  - 6.3 节更新：新增 clib + cstruct 声明式调用示例（推荐方式）

- **v3.1** (2026-06-07):
  - 新增 `ffi.last_error()` — 获取最近一次 FFI 调用后的系统错误码
  - 新增 `ffi.write_bool()` / `ffi.write_uint16()` — 补全内存写入 API 对称性

- **v3.0** (2026-06-07):
  - **零摩擦 FFI**：clib 返回值自动展开，无需 `as` 转换（i32→int, f64→float, str8→string, str16→string）
  - **参数自动窄化**：Leno int 可直接传给 i8/u8/i16/u16/i32/u32/i64/u64 参数，运行时范围检查
  - **新增 C 布局类型**：`i8`、`u8`、`i16`、`u16`
  - **u64 返回值智能升级**：值 ≤ int64 最大值时返回 `int`，超出时自动升级为 `BigInt`
  - **速查表新增**：clib 返回值自动展开表、clib 参数自动窄化表
  - 移除示例中的 `_gc(false)` / `_gc(true)` 调用
  - 修正 MessageBoxW 示例：返回值无需 `_int()` 转换
