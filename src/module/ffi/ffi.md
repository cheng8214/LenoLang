# LenoC 外部函数接口模块 (ffi)

本文档详细说明 `ffi` 模块提供的 C 动态库调用和内存操作功能。

## 目录

- [使用方式](#使用方式)
- [核心概念](#核心概念)
- [clib 声明式调用](#clib-声明式调用)
- [动态库操作](#动态库操作)
- [函数调用](#函数调用)
- [回调函数](#回调函数)
- [类型守卫](#类型守卫)
- [内存管理](#内存管理)
  - [alloc](#alloctype_name-value)
  - [malloc](#mallocsize)
- [内存拷贝](#内存拷贝)
- [内存填充](#内存填充)
- [指针操作](#指针操作)
- [内存读取](#内存读取)
- [内存写入](#内存写入)
- [字符串工具](#字符串工具)
- [类型信息](#类型信息)
- [类型映射](#类型映射)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import ffi

main() {
    // 加载 C 标准库
    var libc = ffi.load("msvcrt.dll")

    // 调用 strlen（返回 int）
    int len = ffi.call_int(libc, "strlen", "hello")
    print(len)  // 5

    // 用完释放库
    ffi.free(libc)
}
```

---

## 核心概念

### FFI 库对象

通过 `ffi.load()` 加载动态链接库后返回的库对象，用于后续函数调用：

```leno
var libc = ffi.load("msvcrt.dll")      // Windows
var libc = ffi.load("libc.so.6")        // Linux
var libc = ffi.load("libSystem.dylib")  // macOS
```

**注意**: 库对象用完后应调用 `ffi.free()` 释放，否则会泄漏系统资源。

### FFI 指针对象

通过 `ffi.malloc()` 等分配的内存块，包含指针地址和大小信息：

```leno
var ptr = ffi.malloc(100)  // 分配 100 字节
ffi.write_int(ptr, 0, 42)  // 写入
var val = ffi.read_int(ptr, 0)  // 读取
ffi.free(ptr)  // 释放
```

**注意**: `ffi.malloc()` 分配的指针在 GC 回收时会自动释放，但建议手动 `ffi.free()` 以控制生命周期。

### 参数类型自动推断

调用 C 函数时，Leno 值会自动转换为对应的 C 类型：

| Leno 类型 | C 类型 | 说明 |
|-----------|--------|------|
| `int` | `int64_t` | 整数参数 |
| `BigInt` | `int64_t` | 大整数参数（自动转换） |
| `float` | `double` | 浮点参数 |
| `string` | `const char*` | 字符串指针 |
| `bool` | `int` (0/1) | 布尔值转为整数 |
| `null` / FFI 指针 | `void*` | 指针参数 |
| FFI 回调对象 | `void*` | 回调函数指针 |

### clib 声明式调用

除了 `ffi.call_*` 动态调用方式，Leno 还支持 `clib` 声明式调用，具有类型安全、编译期检查等优势：

```leno
import ffi
import io

// 声明 C 库函数签名
clib msvcrt {
    i32 strlen(str8 s)
    i32 strcmp(str8 a, str8 b)
    f64 pow(f64 base, f64 exp)
}

main() {
    msvcrt lib = ffi.load("msvcrt.dll")

    i32 len = lib.strlen("hello")
    io.print("strlen: " + (len as int))
}
```

**clib 声明中允许的 C 布局类型：**

| 类型 | C 对应 | 大小 | 说明 |
|------|--------|------|------|
| `i32` | `int32_t` | 4 | 有符号 32 位整数 |
| `u32` | `uint32_t` | 4 | 无符号 32 位整数 |
| `i64` | `int64_t` | 8 | 有符号 64 位整数 |
| `u64` | `uint64_t` | 8 | 无符号 64 位整数 |
| `f32` | `float` | 4 | 单精度浮点 |
| `f64` | `double` | 8 | 双精度浮点 |
| `str8` | `char*` | 8 | C 字符串指针（UTF-8） |
| `str16` | `wchar_t*` | 8 | 宽字符串指针（UTF-16） |
| `Ptr` | `void*` | 8 | 泛型指针 |
| `bool` | `int` | 4 | 布尔值（0/1） |
| `void` | `void` | - | 无返回值 |

> **注意**：clib 声明中**不允许**使用 Leno 类型（`int`、`float`、`string`），必须使用 C 布局类型。clib 函数返回值需要显式转换（如 `as int`、`as float`）才能赋值给 Leno 变量。

**clib 参数自动转换规则：**

| clib 参数类型 | 可接受的 Leno 实参 | 说明 |
|--------------|-------------------|------|
| `str8` | `string` | Leno 字符串自动转为 C `char*` |
| `str16` | `string` | Leno 字符串自动转为 C `wchar_t*` |
| `i32` / `i64` | `int` | Leno int 自动转为对应 C 整数 |
| `f64` | `float` | Leno float 自动转为 C double |
| `Ptr` | `Ptr`、`null` | 指针和 null |

**两种调用方式对比：**

| 特性 | `ffi.call_*` 动态调用 | `clib` 声明式调用 |
|------|----------------------|-------------------|
| 类型安全 | 无编译期检查 | 编译期参数和返回类型检查 |
| 调用方式 | `ffi.call_int(lib, "strlen", s)` | `lib.strlen(s)` |
| 返回值 | 直接是 Leno 类型 | C 布局类型，需显式转换 |
| 适用场景 | 快速原型、动态调用 | 正式项目、类型安全要求高 |

---

## 动态库操作

### `load(path)`

加载动态链接库。

**参数**:
- `path` (string): 库文件路径

**返回**:
- 成功: FFI 库对象
- 失败: `null`

```leno
// Windows
var user32 = ffi.load("user32.dll")
var libc = ffi.load("msvcrt.dll")

// Linux
var libc = ffi.load("libc.so.6")
var libc = ffi.load("libm.so.6")  // 数学库

// 使用绝对路径
var mylib = ffi.load("C:\\libs\\mylib.dll")
```

---

### `free(x)`

统一释放 FFI 资源（指针/库/回调）。释放后的资源不可再使用。

**参数**:
- `x`: FFI 资源（指针/库对象/回调）

**返回**: `null`

```leno
var libc = ffi.load("msvcrt.dll")
// ... 使用完毕 ...
ffi.free(libc)
// 此后不能再调用 ffi.call(libc, ...)
```

---

## 函数调用

### 设计理念

FFI 模块提供两种调用方式：

1. **明确类型调用**（推荐）：使用 `ffi.call_*` 系列函数，直接获得明确类型的返回值
2. **通用调用** + **类型守卫**：使用 `ffi.call` 返回 `any` 类型，通过类型守卫判断实际类型

### 明确类型调用（推荐）

#### `call_int(lib, name, ...)`

调用 C 函数，返回整数值（`int`）。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: `int` - C 函数返回的整数值

```leno
var libc = ffi.load("msvcrt.dll")
int len = ffi.call_int(libc, "strlen", "hello")   // 5
int cmp = ffi.call_int(libc, "strcmp", "abc", "abd")  // -1
ffi.free(libc)
```

---

#### `call_ptr(lib, name, ...)`

调用 C 函数，返回指针值。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: FFI 指针对象 - C 函数返回的指针

```leno
var kernel32 = ffi.load("kernel32.dll")
Ptr hProcess = ffi.call_ptr(kernel32, "OpenProcess", 0x0400, 0, pid)
ffi.free(kernel32)
```

---

#### `call_double(lib, name, ...)`

调用 C 函数，返回双精度浮点值（`double`）。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: `float` - C 函数返回的 double 值

```leno
var libm = ffi.load("msvcrt.dll")
float s = ffi.call_double(libm, "sin", 1.5707963)  // ~1.0 (sin(π/2))
float p = ffi.call_double(libm, "pow", 2.0, 10.0)   // 1024.0
ffi.free(libm)
```

---

#### `call_bool(lib, name, ...)`

调用 C 函数，返回布尔值。适用于 Windows API 中返回 `BOOL` 类型的函数。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: `bool` - C 函数返回的非零值为 `true`，零值为 `false`

```leno
var user32 = ffi.load("user32.dll")
bool is_win = ffi.call_bool(user32, "IsWindow", 0)  // false
ffi.free(user32)
```

---

#### `call_void(lib, name, ...)`

调用无返回值的 C 函数。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: `null`

```leno
var libc = ffi.load("msvcrt.dll")
ffi.call_void(libc, "srand", 42)  // 设置随机种子，无返回值
ffi.free(libc)
```

---

### 通用调用 + 类型守卫

#### `call(lib, name, ...)`

调用 C 函数，返回 `any` 类型。

**参数**:
- `lib`: FFI 库对象
- `name` (string): 函数名
- `...`: 函数参数（可变，最多 12 个）

**返回**: `any` - C 函数返回的值（实际类型取决于返回值）

```leno
var kernel32 = ffi.load("kernel32.dll")

// 通用调用，返回 any 类型
var result = ffi.call(kernel32, "GetCurrentProcessId")

// 需要通过类型守卫或类型转换来判断实际类型
int pid = _int(result)  // 转换为 int

ffi.free(kernel32)
```

---

## 回调函数

### `callback(func, ret_type, arg_types)`

将 Leno 函数包装为 C 函数指针，使 C 代码能够回调 Leno 函数。

**参数**:
- `func`: Leno 函数或闭包
- `ret_type` (string): 返回值类型字符串
- `arg_types` (数组): 参数类型字符串数组

**返回**: FFI 回调对象 — 可作为指针参数传递给 C 函数

**支持的类型字符串**:

| 类型字符串 | C 类型 | 说明 |
|-----------|--------|------|
| `"int"` | `int` | 整数 |
| `"double"` | `double` | 双精度浮点 |
| `"pointer"` | `void*` | 指针 |
| `"bool"` | `int` | 布尔（0/1） |

```leno
import ffi

// 定义比较函数
func compare(Ptr a, Ptr b): int {
    int va = ffi.read_int(a, 0)
    int vb = ffi.read_int(b, 0)
    if va < vb { return -1 }
    if va > vb { return 1 }
    return 0
}

// 创建回调
var cb = ffi.callback(compare, "int", ["pointer", "pointer"])

// 传递给 C 函数（如 qsort）
var libc = ffi.load("msvcrt.dll")
var arr = ffi.malloc(20)
ffi.write_int(arr, 0, 5)
ffi.write_int(arr, 4, 3)
ffi.write_int(arr, 8, 1)
ffi.write_int(arr, 12, 4)
ffi.write_int(arr, 16, 2)

ffi.call_void(libc, "qsort", arr, 5, 4, cb)

// 释放回调和资源
ffi.free(arr)
ffi.free(cb)
ffi.free(libc)
```

**注意事项**:
- 回调对象用 `ffi.free()` 释放，与指针和库对象使用同一个函数
- 释放回调后，对应的 C 函数指针不再有效
- 最多支持 128 个同时存在的回调
- `ffi.is_ptr(cb)` 对回调对象返回 `true`
- 回调函数中 `Ptr` 类型参数需通过 `ffi.read_*` 读取数据

**平台支持**:

| 平台 | 架构 | 支持状态 | 说明 |
|-----|------|---------|------|
| Windows | x86-64 | ✅ 完全支持 | 使用 Microsoft x64 调用约定 |
| Linux | x86-64 | ✅ 支持 | 使用 System V AMD64 ABI |
| macOS | x86-64 | ✅ 支持 | 使用 System V AMD64 ABI |
| ARM64 (任何平台) | - | ❌ 不支持 | 需要额外实现 |

**调用约定差异**:

| 调用约定 | 整数参数寄存器 | 浮点参数寄存器 | 最大参数数 |
|---------|--------------|--------------|-----------|
| Windows x64 | RCX, RDX, R8, R9 | XMM0-XMM3 | 4 个整数 + 4 个浮点 |
| System V AMD64 | RDI, RSI, RDX, RCX, R8, R9 | XMM0-XMM7 | 6 个整数 + 8 个浮点 |

---

## 类型守卫

当使用 `ffi.call()` 返回 `any` 类型时，可以使用类型守卫来判断实际类型。

### 类型守卫语法

```leno
var result = ffi.call(lib, "some_function", ...)

if result is int {
    // 编译器知道这里 result 是 int 类型
    int value = result
} else if result is Ptr {
    // 编译器知道这里 result 是 Ptr 类型
    Ptr ptr = result
} else {
    // 其他类型
}
```

### 完整示例

```leno
import ffi

func getProcessId() {
    var kernel32 = ffi.load("kernel32.dll")

    // ffi.call 返回 any 类型
    var result = ffi.call(kernel32, "GetCurrentProcessId")

    // 使用类型守卫
    if result is int {
        print("返回值是 int 类型，PID: " + result)
        ffi.free(kernel32)
        return result
    } else if result is Ptr {
        print("返回值是 Ptr 类型")
        ffi.free(kernel32)
        return 0
    } else {
        print("返回值是其他类型，尝试转换")
        ffi.free(kernel32)
        return _int(result)
    }
}

main() {
    int pid = getProcessId()
    print("最终 PID: " + pid)
}
```

### 类型守卫应用场景

| 场景 | 说明 |
|------|------|
| 动态类型判断 | 根据实际类型执行不同逻辑 |
| 类型安全转换 | 确保类型正确后再使用 |
| 错误处理 | 判断返回值是否符合预期类型 |

---

## 内存管理

### `alloc(type_name, value)`

分配指定类型的内存并初始化。等价于 `ffi.malloc(ffi.sizeof_type(type_name))` + 写入初始值。

**参数**:
- `type_name` (string): 类型名称，支持 `"int8"`, `"uint8"`, `"byte"`, `"int16"`, `"uint16"`, `"int"`, `"int32"`, `"uint32"`, `"int64"`, `"uint64"`, `"float"`, `"double"`, `"ptr"`, `"pointer"`, `"bool"`, `"long"`, `"size_t"`
- `value` (可选): 初始值，不传则清零初始化

**返回**: FFI 指针对象

```leno
// 分配 int32 并初始化为 42
var p = ffi.alloc("int", 42)
print(ffi.read_int(p, 0))  // 42

// 分配 double 并初始化
var d = ffi.alloc("double", 3.14)
print(ffi.read_double(d, 0))  // 3.14

// 分配指针，清零（null）
var ptr = ffi.alloc("ptr")

// 分配 bool 并初始化
var b = ffi.alloc("bool", true)
print(ffi.read_bool(b, 0))  // true

// 不传初始值，清零
var z = ffi.alloc("int")
print(ffi.read_int(z, 0))  // 0

ffi.free(p)
ffi.free(d)
ffi.free(ptr)
ffi.free(b)
ffi.free(z)
```

---

### `malloc(size)`

分配指定大小的内存块。

**参数**:
- `size` (int): 字节数

**返回**: FFI 指针对象

```leno
var buf = ffi.malloc(256)  // 分配 256 字节
// ... 使用 ...
ffi.free(buf)
```

---

### `calloc(count, size)`

分配并清零内存。分配 `count * size` 字节，所有位初始化为 0。

**参数**:
- `count` (int): 元素数量
- `size` (int): 每个元素的大小

**返回**: FFI 指针对象

```leno
// 分配 10 个 int 的空间（40 字节），全部清零
var arr = ffi.calloc(10, 4)
ffi.write_int(arr, 0, 100)
print(ffi.read_int(arr, 0))  // 100
ffi.free(arr)
```

---

### `realloc(ptr, size)`

重新分配内存，保留原有数据。

**参数**:
- `ptr`: FFI 指针对象
- `size` (int): 新的字节数

**返回**: FFI 指针对象 - 可能是新的指针地址

```leno
var buf = ffi.malloc(32)
ffi.write_int(buf, 0, 42)

buf = ffi.realloc(buf, 64)  // 扩展到 64 字节
print(ffi.read_int(buf, 0))  // 42，原数据仍保留
ffi.free(buf)
```

---

### `free(ptr)`

释放内存。

**参数**:
- `ptr`: FFI 指针对象

**返回**: `null`

```leno
var ptr = ffi.malloc(100)
ffi.free(ptr)  // 释放后不可再使用
```

---

### `sizeof(ptr)`

获取指针指向的内存大小。

**参数**:
- `ptr`: FFI 指针对象

**返回**: `int` - 内存大小（字节）

```leno
var ptr = ffi.malloc(256)
print(ffi.sizeof(ptr))  // 256
ffi.free(ptr)
```

---

### `nullptr()`

返回一个空指针（`NULL`）。

**参数**: 无
**返回**: FFI 指针对象（值为 NULL）

```leno
var null_ptr = ffi.nullptr()
// 可用于传给需要 NULL 指针的 C 函数
ffi.call_int(libc, "some_func", null_ptr)
```

---

### `memcpy(dest, src, size)`

内存拷贝，将 `size` 字节的数据从 `src` 复制到 `dest`。

**参数**:
- `dest`: FFI 指针对象 - 目标内存地址
- `src`: FFI 指针对象 - 源内存地址
- `size` (int): 拷贝的字节数

**返回**: `null`

**说明**:
- 使用 C 标准库的 `memcpy` 实现，性能高效
- 适用于大块内存的批量复制，比逐字节循环快得多
- 如果 `dest` 或 `src` 为 `null`，会抛出错误
- 如果 `size` 为负数，会抛出错误

```leno
// 分配源和目标缓冲区
var src = ffi.malloc(100)
var dest = ffi.malloc(100)

// 写入源数据
ffi.write_string(src, 0, "Hello, World!")

// 批量拷贝 100 字节
ffi.memcpy(dest, src, 100)

// 验证拷贝结果
print(ffi.read_string(dest, 0))  // "Hello, World!"

ffi.free(src)
ffi.free(dest)
```

**使用场景**: 当需要复制大量数据（如二进制缓冲区、结构体数组）时，使用 `memcpy` 比手动循环更高效。

---

### `memset(ptr, value, size)`

内存填充，将 `size` 字节的内存区域设置为指定的值。

**参数**:
- `ptr`: FFI 指针对象 - 目标内存地址
- `value` (int): 填充值（0-255，按字节填充）
- `size` (int): 填充的字节数

**返回**: `null`

**说明**:
- 使用 C 标准库的 `memset` 实现，性能高效
- 适用于大块内存的清零或填充，比逐字节循环快得多
- 如果 `ptr` 为 `null`，会抛出错误
- 如果 `size` 为负数，会抛出错误

```leno
// 清零缓冲区（最常见的用法）
var buf = ffi.malloc(4096)
ffi.memset(buf, 0, 4096)  // 比 for 循环 write_byte 快得多

// 填充特定值
ffi.memset(buf, 0xFF, 1024)  // 前 1024 字节填充为 0xFF

// 与 memcpy 配合使用
var src = ffi.malloc(100)
ffi.memset(src, 0x42, 100)   // 填充 0x42
var dest = ffi.malloc(100)
ffi.memcpy(dest, src, 100)   // 拷贝到 dest

ffi.free(src)
ffi.free(dest)
ffi.free(buf)
```

**使用场景**: 替代 `for` 循环 + `write_byte` 的低效写法，一次调用完成大块内存的初始化。

---

### `ptr_from_int(address)`

从整数地址创建一个 FFI 指针对象。用于传递 Windows 句柄（如 HKEY）等整数值作为指针参数。

**参数**:
- `address` (int): 整数地址值

**返回**: FFI 指针对象

```leno
// Windows HKEY 根键常量（在 64 位系统上需要作为指针传递）
var HKEY_LOCAL_MACHINE = ffi.ptr_from_int(0x80000002)

// 现在可以作为指针参数传递给 RegOpenKeyExA
ffi.call_int(advapi, "RegOpenKeyExA", HKEY_LOCAL_MACHINE, "SOFTWARE\\...", 0, KEY_READ, hKey)
```

---

### `ptr_to_int(ptr)`

将 FFI 指针的地址值转换为整数。是 `ptr_from_int` 的逆操作，用于比较句柄值、调试打印指针地址等场景。

**参数**:
- `ptr`: FFI 指针对象或回调对象

**返回**: `int` - 指针的地址值

```leno
// 获取 malloc 指针的地址值
var buf = ffi.malloc(256)
int addr = ffi.ptr_to_int(buf)
print("缓冲区地址: " + addr)

// 比较句柄是否等于 INVALID_HANDLE_VALUE (-1)
var hProcess = ffi.call_ptr(kernel32, "OpenProcess", ...)
int handleAddr = ffi.ptr_to_int(hProcess)
if handleAddr == -1 {
    print("无效句柄")
}

// ptr_from_int / ptr_to_int 往返转换
var p = ffi.ptr_from_int(0x80000002)
int addr = ffi.ptr_to_int(p)
// addr == 0x80000002

// nullptr 的地址值为 0
var np = ffi.nullptr()
print(ffi.ptr_to_int(np))  // 0

ffi.free(buf)
```

**注意**: 回调对象也可以传入 `ptr_to_int`，返回的是回调 trampoline 代码的地址。

---

### `is_ptr(value)`

检查值是否是 FFI 指针对象或回调对象。

**参数**:
- `value`: 任意值

**返回**: `bool` - 如果是 FFI 指针对象或回调对象返回 `true`，否则返回 `false`

```leno
var ptr = ffi.malloc(100)
var num = 42

print(ffi.is_ptr(ptr))  // true
print(ffi.is_ptr(num))  // false
print(ffi.is_ptr("hello"))  // false
print(ffi.is_ptr(null))  // false

// 回调对象也返回 true
var cb = ffi.callback(my_func, "int", ["int"])
print(ffi.is_ptr(cb))  // true

ffi.free(ptr)
ffi.free(cb)
```

**用途**: 在需要区分指针和其他类型的场景中使用，例如编写接受多种参数类型的通用函数：

```leno
func process_handle(var h) {
    if ffi.is_ptr(h) {
        // 已经是指针，直接使用
        return h
    } else {
        // 将整数转换为指针
        return ffi.ptr_from_int(h)
    }
}
```

---

## 指针操作

### `offset(ptr, off)`

返回偏移后的新指针，原指针不变。

**参数**:
- `ptr`: FFI 指针对象
- `off` (int): 偏移字节数

**返回**: FFI 指针对象 - 偏移后的新指针

```leno
var buf = ffi.malloc(32)
ffi.write_int(buf, 0, 11111)
ffi.write_int(buf, 4, 22222)

var p2 = ffi.offset(buf, 4)  // 偏移 4 字节
print(ffi.read_int(p2, 0))  // 22222
ffi.free(buf)
```

**注意**: `offset` 返回的新指针不拥有内存，不需要（也不应该）单独 `free`。释放原始指针即可。

---

## 内存读取

所有读取函数的参数格式为 `(ptr, offset)`，`offset` 为字节偏移量。

### 整数读取

| 函数 | 返回类型 | C 类型 | 大小 |
|------|----------|--------|------|
| `read_byte(ptr, off)` | `int` | `uint8_t` | 1 字节 |
| `read_int8(ptr, off)` | `int` | `int8_t` | 1 字节 |
| `read_int16(ptr, off)` | `int` | `int16_t` | 2 字节 |
| `read_uint16(ptr, off)` | `int` | `uint16_t` | 2 字节 |
| `read_int(ptr, off)` | `int` | `int32_t` | 4 字节 |
| `read_uint(ptr, off)` | `int`/`BigInt` | `uint32_t` | 4 字节 |
| `read_int64(ptr, off)` | `int` | `int64_t` | 8 字节 |
| `read_uint64(ptr, off)` | `int`/`BigInt` | `uint64_t` | 8 字节 |

**`read_uint64` 说明**:

`read_uint64` 用于读取无符号 64 位整数。与 `read_int64` 的区别：
- `read_int64` 返回有符号整数，值 `> 0x7FFFFFFFFFFFFFFF` 时显示为负数
- `read_uint64` 返回无符号整数（`int` 或 `BigInt`），支持 `0x8000000000000000` ~ `0xFFFFFFFFFFFFFFFF` 范围的值正确显示为正数

```leno
var buf = ffi.malloc(16)

// 写入 0xFFFFFFFFFFFFFFFF (18446744073709551615)
ffi.write_uint64(buf, 0, 0xFFFFFFFFFFFFFFFF)

// read_int64 将其解释为有符号数
print(ffi.read_int64(buf, 0))    // -1

// read_uint64 将其解释为无符号数
print(ffi.read_uint64(buf, 0))   // 18446744073709551615

ffi.free(buf)
```

```leno
var buf = ffi.malloc(16)
ffi.write_int(buf, 0, 42)

print(ffi.read_byte(buf, 0))   // 读取 1 字节
print(ffi.read_int(buf, 0))    // 读取 4 字节 (int32)
print(ffi.read_int64(buf, 0))  // 读取 8 字节 (int64)
ffi.free(buf)
```

### 浮点读取

| 函数 | 返回类型 | C 类型 | 大小 |
|------|----------|--------|------|
| `read_float(ptr, off)` | `float` | `float` | 4 字节 |
| `read_double(ptr, off)` | `float` | `double` | 8 字节 |

```leno
var buf = ffi.malloc(16)
ffi.write_double(buf, 0, 3.14159)

print(ffi.read_float(buf, 0))   // 读取 4 字节 float
print(ffi.read_double(buf, 0))  // 读取 8 字节 double
ffi.free(buf)
```

### 指针与布尔读取

| 函数 | 返回类型 | C 类型 | 大小 |
|------|----------|--------|------|
| `read_ptr(ptr, off)` | FFI 指针 | `void*` | 8 字节 (x64) |
| `read_bool(ptr, off)` | `bool` | `int` | 4 字节 |

```leno
var pp = ffi.malloc(24)
var inner = ffi.malloc(8)
ffi.write_int(inner, 0, 42)
ffi.write_ptr(pp, 0, inner)       // 写入 inner 指针
var recovered = ffi.read_ptr(pp, 0)  // 读回指针
print(ffi.read_int(recovered, 0)) // 42
ffi.free(inner)
ffi.free(pp)
```

### 字符串读取

#### `read_string(ptr, off)`

从内存读取以 null 结尾的字符串。

**参数**:
- `ptr`: FFI 指针对象
- `off` (int): 字节偏移量

**返回**: `string` - 读取到的字符串

**安全性**: 对于 owned 指针，不会超过内存块边界；对于非 owned 指针，最大读取 4096 字节。

```leno
var buf = ffi.malloc(32)
ffi.write_string(buf, 0, "hello")
print(ffi.read_string(buf, 0))  // "hello"
ffi.free(buf)
```

#### `read_string_n(ptr, off, len)`

从内存读取指定长度的字符串。

**参数**:
- `ptr`: FFI 指针对象
- `off` (int): 字节偏移量
- `len` (int): 读取的字节数

**返回**: `string` - 读取到的字符串

```leno
var buf = ffi.malloc(32)
ffi.write_string(buf, 0, "hello world")
print(ffi.read_string_n(buf, 0, 5))  // "hello"
ffi.free(buf)
```

---

## 内存写入

所有写入函数的参数格式为 `(ptr, offset, value)`，`offset` 为字节偏移量。

### 整数写入

| 函数 | 值类型 | C 类型 | 大小 |
|------|--------|--------|------|
| `write_byte(ptr, off, val)` | `int` | `uint8_t` | 1 字节 |
| `write_int8(ptr, off, val)` | `int` | `int8_t` | 1 字节 |
| `write_int16(ptr, off, val)` | `int` | `int16_t` | 2 字节 |
| `write_int(ptr, off, val)` | `int` | `int32_t` | 4 字节 |
| `write_uint(ptr, off, val)` | `int`/`BigInt` | `uint32_t` | 4 字节 |
| `write_int64(ptr, off, val)` | `int` | `int64_t` | 8 字节 |
| `write_uint64(ptr, off, val)` | `int`/`BigInt` | `uint64_t` | 8 字节 |

**`write_uint64` 说明**:

`write_uint64` 专门用于写入无符号 64 位整数。与 `write_int64` 的区别：
- `write_int64` 接受 `int` 参数，强转为 `int64_t`
- `write_uint64` 接受 `int` 或 `BigInt` 参数，写入 `uint64_t`，支持 `0x8000000000000000` ~ `0xFFFFFFFFFFFFFFFF` 范围的值

```leno
// 写入大范围的 uint64 值
ffi.write_uint64(buf, 0, 0xFFFFFFFFFFFFFFFF)    // 18446744073709551615
ffi.write_uint64(buf, 8, 34128000000)           // 中等大小的值

// 读取时保持无符号语义
print(ffi.read_uint64(buf, 0))   // 18446744073709551615
print(ffi.read_uint64(buf, 8))   // 34128000000
```

```leno
var buf = ffi.malloc(16)
ffi.write_byte(buf, 0, 255)
ffi.write_int(buf, 4, 42)
ffi.write_uint(buf, 8, 0x80000002)  // 写入超过 2^31 的 uint32 值
ffi.write_int64(buf, 12, 999999)
ffi.free(buf)
```

**`write_uint` 说明**:

`write_uint` 专门用于写入无符号 32 位整数。与 `write_int` 的区别：
- `write_int` 接受 `int` 参数，强转为 `int32_t`，值 > `0x7FFFFFFF` 时语义不明确
- `write_uint` 接受 `int` 或 `BigInt` 参数，写入 `uint32_t`，支持 `0x80000000` ~ `0xFFFFFFFF` 范围的值

```leno
// Windows 常量经常超过 2^31
ffi.write_uint(buf, 0, 0x80000002)    // HKEY_LOCAL_MACHINE
ffi.write_uint(buf, 4, 0xFFFFFFFF)    // MAX_UINT32
ffi.write_uint(buf, 8, 0xF003F)       // KEY_ALL_ACCESS
```

### 浮点写入

| 函数 | 值类型 | C 类型 | 大小 |
|------|--------|--------|------|
| `write_float(ptr, off, val)` | `float` | `float` | 4 字节 |
| `write_double(ptr, off, val)` | `float` | `double` | 8 字节 |

```leno
var buf = ffi.malloc(16)
ffi.write_float(buf, 0, 1.5)
ffi.write_double(buf, 8, 2.71828)
ffi.free(buf)
```

### 指针与字符串写入

| 函数 | 值类型 | C 类型 |
|------|--------|--------|
| `write_ptr(ptr, off, val)` | FFI 指针 | `void*` |
| `write_string(ptr, off, str)` | `string` | `const char*` |

```leno
var buf = ffi.malloc(32)
ffi.write_string(buf, 0, "hello")
print(ffi.read_string(buf, 0))  // "hello"
ffi.free(buf)
```

**`write_string` 的两种调用方式**:

```leno
// 标准方式：指定偏移
ffi.write_string(ptr, 0, "hello")

// 简写方式：省略偏移（默认偏移为 0）
ffi.write_string(ptr, "hello")
```

---

### 字符串工具

#### `string_bytes(str)`

获取字符串的字节长度（UTF-8 编码）。

**参数**:
- `str` (string): 字符串

**返回**: `int` - 字符串的字节长度（不是字符数）

```leno
var s = "Hello"
print(ffi.string_bytes(s))  // 5

var chinese = "中文"
print(chinese.len())          // 2 (字符数)
print(ffi.string_bytes(chinese))  // 6 (UTF-8 字节数)
```

---

#### `utf8_to_utf16(str)` (Windows)

将 UTF-8 字符串转换为 UTF-16 宽字符。

**参数**:
- `str` (string): UTF-8 编码的字符串

**返回**: FFI 指针对象 - 指向 UTF-16 宽字符数据的指针

**注意**:
- 仅 Windows 平台可用
- 返回的指针需要用 `ffi.free()` 释放
- 用于调用 Unicode 版本的 Windows API（带 W 后缀的函数）

```leno
// 将 UTF-8 字符串转换为 UTF-16
var wstr = ffi.utf8_to_utf16("Hello 世界")

// 调用 Unicode 版本的 Windows API
ffi.call_int(advapi, "RegSetValueExW", hKey, wstr, ...)

// 释放转换后的内存
ffi.free(wstr)
```

---

#### `utf16_to_utf8(ptr)` (Windows)

将 UTF-16 宽字符指针转换为 UTF-8 字符串。

**参数**:
- `ptr`: FFI 指针对象，指向以 null 结尾的 UTF-16 宽字符数据

**返回**: `string` - UTF-8 编码的字符串

**注意**:
- 仅 Windows 平台可用
- 用于读取 Unicode 版本 Windows API 返回的字符串数据

```leno
// 从 Windows API 获取 UTF-16 数据
var buffer = ffi.malloc(512)
ffi.call_int(advapi, "RegQueryValueExW", hKey, ..., buffer, ...)

// 转换为 UTF-8 字符串
var str = ffi.utf16_to_utf8(buffer)
print(str)  // 正确显示中文

ffi.free(buffer)
```

---

### 类型信息

#### `sizeof_type(type_name)`

获取 C 类型的大小（字节）。

**参数**:
- `type_name` (string): 类型名称

**返回**: `int` - 类型的字节数

**支持的类型名称**:

| 类型名 | 大小 | 说明 |
|--------|------|------|
| `"int8"` | 1 | 有符号 8 位整数 |
| `"uint8"` / `"byte"` | 1 | 无符号 8 位整数 |
| `"int16"` / `"short"` | 2 | 有符号 16 位整数 |
| `"uint16"` / `"ushort"` | 2 | 无符号 16 位整数 |
| `"int32"` / `"int"` | 4 | 有符号 32 位整数 |
| `"uint32"` / `"uint"` | 4 | 无符号 32 位整数 |
| `"int64"` / `"long"` | 8 | 有符号 64 位整数 |
| `"float"` | 4 | 单精度浮点 |
| `"double"` | 8 | 双精度浮点 |
| `"pointer"` / `"ptr"` | 8 | 指针（x64） |
| `"bool"` | 4 | 布尔类型 |
| `"size_t"` | 8 | size_t 类型 |

```leno
print(ffi.sizeof_type("int"))       // 4
print(ffi.sizeof_type("double"))    // 8
print(ffi.sizeof_type("pointer"))   // 8
print(ffi.sizeof_type("int64"))     // 8
```

---

### `alignof(type_name)`

获取 C 类型的对齐要求（字节）。

**参数**:
- `type_name` (string): 类型名称

**返回**: `int` - 对齐字节数

```leno
print(ffi.alignof("int"))      // 4
print(ffi.alignof("double"))   // 8
print(ffi.alignof("pointer"))  // 8
```

---

## 类型映射

### Leno 值到 C 类型的转换

调用 C 函数时，参数自动按以下规则转换：

| Leno 类型 | FFI 参数类型 | 传递方式 |
|-----------|-------------|----------|
| `int` (整数) | `int64_t` | 整数寄存器 |
| `BigInt` (大整数) | `int64_t` | 整数寄存器（自动转换） |
| `float` (浮点) | `double` | 浮点寄存器 |
| `string` (字符串) | `const char*` | 指针寄存器 |
| `bool` (布尔) | `int` (0/1) | 整数寄存器 |
| `null` | `void*` (NULL) | 指针寄存器 |
| FFI 指针对象 | `void*` | 指针寄存器 |

**注意**: `BigInt` 类型在 FFI 调用时会自动转换为 `int64_t`。如果值超出 `int64_t` 范围，转换结果未定义。

### 返回值的转换

| 调用方法 | C 返回类型 | Leno 返回类型 |
|----------|-----------|---------------|
| `call` | 任意类型 | `any` |
| `call_int` | `int` / `long` | `int` |
| `call_double` | `double` | `float` |
| `call_void` | `void` | `null` |
| `call_ptr` | `void*` | FFI 指针对象 |
| `call_bool` | `BOOL` / `int` | `bool` |

---

## 示例代码

### 1. 调用 C 标准库函数

```leno
import ffi

main() {
    var libc = ffi.load("msvcrt.dll")
    if libc == null {
        print("加载库失败")
        return
    }

    // 字符串长度
    int len = ffi.call_int(libc, "strlen", "Hello, Leno!")
    print("字符串长度: " + len)  // 12

    // 数学计算
    float p = ffi.call_double(libc, "pow", 2.0, 10.0)
    print("2^10 = " + p)  // 1024.0

    ffi.free(libc)
}
```

---

### 2. 调用 Windows API

```leno
import ffi

main() {
    var user32 = ffi.load("user32.dll")
    if user32 == null {
        print("加载 user32 失败")
        return
    }

    // 检查窗口句柄是否有效
    bool is_win = ffi.call_bool(user32, "IsWindow", 0)
    print("IsWindow(NULL) = " + is_win)  // false

    ffi.free(user32)
}
```

---

### 3. 使用类型守卫处理返回值

```leno
import ffi

main() {
    var kernel32 = ffi.load("kernel32.dll")

    // 通用调用
    var result = ffi.call(kernel32, "GetCurrentProcessId")

    // 使用类型守卫
    if result is int {
        print("返回 int 类型: " + result)
    } else if result is Ptr {
        print("返回 Ptr 类型")
    }

    ffi.free(kernel32)
}
```

---

### 4. 内存操作

```leno
import ffi

main() {
    // 分配并写入结构体式数据
    var buf = ffi.malloc(32)

    // 写入不同类型
    ffi.write_int(buf, 0, 42)        // 偏移 0: int32
    ffi.write_double(buf, 8, 3.14)   // 偏移 8: double
    ffi.write_string(buf, 16, "Leno") // 偏移 16: 字符串

    // 读取
    print("int: " + ffi.read_int(buf, 0))         // 42
    print("double: " + ffi.read_double(buf, 8))   // 3.14
    print("string: " + ffi.read_string(buf, 16))  // "Leno"

    ffi.free(buf)
}
```

---

### 5. 指针的指针

```leno
import ffi

main() {
    var pp = ffi.malloc(24)
    var inner = ffi.malloc(8)

    ffi.write_int(inner, 0, 42)
    ffi.write_ptr(pp, 0, inner)  // 把 inner 的地址写入 pp

    // 通过二级指针间接访问
    var recovered = ffi.read_ptr(pp, 0)
    print("间接读取: " + ffi.read_int(recovered, 0))  // 42

    ffi.free(inner)
    ffi.free(pp)
}
```

---

### 6. 使用 offset 遍历数组

```leno
import ffi

main() {
    var count = 5
    var arr = ffi.calloc(count, 4)  // 5 个 int，全部为 0

    // 写入数据
    var i = 0
    while i < count {
        ffi.write_int(arr, i * 4, (i + 1) * 10)
        i = i + 1
    }

    // 读取数据
    i = 0
    while i < count {
        var ptr = ffi.offset(arr, i * 4)
        print("arr[" + i + "] = " + ffi.read_int(ptr, 0))
        i = i + 1
    }
    // 输出: arr[0]=10, arr[1]=20, arr[2]=30, arr[3]=40, arr[4]=50

    ffi.free(arr)
}
```

---

### 7. 读取 Windows 结构体

```leno
import ffi

main() {
    var kernel32 = ffi.load("kernel32.dll")
    var psapi = ffi.load("psapi.dll")

    // 获取当前进程 ID
    int pid = ffi.call_int(kernel32, "GetCurrentProcessId")

    // 打开进程
    Ptr hProcess = ffi.call_ptr(kernel32, "OpenProcess", 0x0400 | 0x0010, 0, pid)

    if hProcess != ffi.nullptr() {
        // 分配 PROCESS_MEMORY_COUNTERS 结构体
        Ptr memCounters = ffi.malloc(72)

        // 设置结构体大小
        ffi.write_int(memCounters, 0, 72)

        // 调用 GetProcessMemoryInfo
        int result = ffi.call_int(psapi, "GetProcessMemoryInfo",
            hProcess, memCounters, 72)

        if result != 0 {
            // 读取内存信息
            int workingSet = ffi.read_int64(memCounters, 16) / 1024
            int peakWorkingSet = ffi.read_int64(memCounters, 8) / 1024

            print("WorkingSet: " + workingSet + " KB")
            print("PeakWorkingSet: " + peakWorkingSet + " KB")
        }

        ffi.free(memCounters)
        ffi.call_int(kernel32, "CloseHandle", hProcess)
    }

    ffi.free(psapi)
    ffi.free(kernel32)
}
```

---

## 注意事项

1. **安全性**
   - FFI 调用直接操作内存，没有沙箱保护
   - 错误的参数类型或越界访问可能导致程序崩溃
   - 建议对库对象进行 `null` 检查

2. **内存管理**
   - `ffi.malloc()` 分配的内存可以通过 `ffi.free()` 手动释放
   - 如果忘记释放，GC 最终会回收指针对象并自动释放内存
   - `ffi.offset()` 返回的指针是原始内存的视图，不需要单独释放

3. **边界检查**
   - 对 `ffi.malloc/calloc/realloc` 分配的指针（owned 指针），读写操作会进行越界检查
   - 对非 owned 指针（如 `ffi.call_ptr` 返回的），无法进行边界检查

4. **use-after-free 保护**
   - 已释放的库对象和指针对象带有 `freed` 标志
   - 对已释放的对象进行操作会报错，而不是产生未定义行为

5. **参数数量限制**
   - C 函数调用最多支持 12 个参数（6 寄存器 + 6 栈参数，x64 ABI）

6. **数字类型处理**
   - 十六进制数解析规则：
     - 小于等于 `2^53` 的十六进制数（如 `0xF003F`）解析为 `int`
     - 大于 `2^53` 的十六进制数（如 `0x80000001`）解析为 `BigInt`
   - FFI 调用时数字类型映射：
     - `int` → `int64_t`（整数寄存器）
     - `BigInt` → `int64_t`（整数寄存器，自动转换）
     - `float` → `double`（浮点寄存器）
   - 模块中的数字常量会保持原始类型（int/BigInt/float），不会被强制转为 float

7. **平台差异**
   - Windows 使用 `LoadLibrary` / `GetProcAddress`
   - Linux 使用 `dlopen` / `dlsym`
   - 库文件名和路径格式因平台而异

8. **字符串生命周期**
   - 传给 C 函数的字符串参数是 Leno 内部字符串的指针
   - C 函数不应保存该指针供后续使用（GC 可能回收原字符串）
   - 如需持久化，请使用 `ffi.malloc` + `ffi.write_string` 分配独立内存

9. **返回值类型选择**
   - **推荐使用明确类型调用**：`ffi.call_int`、`ffi.call_ptr` 等，直接获得正确类型
   - **通用调用适用场景**：不确定返回值类型、需要动态处理、或用于演示类型守卫功能

---

## 完整 API 速查表

### 动态库

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `load(path)` | string | lib\|null | 加载动态库 |
| `free(x)` | lib/ptr/callback | null | 统一释放 FFI 资源（指针/库/回调） |

### 函数调用

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `call(lib, name, ...)` | lib, string, ... | any | 通用调用（返回 any） |
| `call_int(lib, name, ...)` | lib, string, ... | int | 调用（返回 int） |
| `call_double(lib, name, ...)` | lib, string, ... | float | 调用（返回 double） |
| `call_void(lib, name, ...)` | lib, string, ... | null | 调用（无返回值） |
| `call_ptr(lib, name, ...)` | lib, string, ... | ptr | 调用（返回指针） |
| `call_bool(lib, name, ...)` | lib, string, ... | bool | 调用（返回布尔） |

### 内存管理

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `alloc(type_name, value?)` | string, any | ptr | 分配指定类型内存并初始化 |
| `malloc(size)` | int | ptr | 分配内存 |
| `calloc(count, size)` | int, int | ptr | 分配并清零 |
| `realloc(ptr, size)` | ptr, int | ptr | 重新分配 |
| `free(x)` | ptr/lib/callback | null | 统一释放 FFI 资源 |
| `sizeof(ptr)` | ptr | int | 获取内存大小 |
| `nullptr()` | 无 | ptr | 返回空指针 |
| `memcpy(dest, src, size)` | ptr, ptr, int | null | 内存拷贝 |
| `memset(ptr, value, size)` | ptr, int, int | null | 内存填充 |

### 指针操作

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `ptr_from_int(address)` | int | ptr | 从整数创建指针 |
| `ptr_to_int(ptr)` | ptr/callback | int | 指针地址转整数 |
| `is_ptr(value)` | any | bool | 检查是否指针或回调对象 |
| `offset(ptr, off)` | ptr, int | ptr | 指针偏移 |

### 回调函数

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `callback(func, ret_type, arg_types)` | 函数, string, 数组 | callback | 创建 FFI 回调 |

### 内存读取

| 函数 | 参数 | 返回 | 大小 |
|------|------|------|------|
| `read_byte(ptr, off)` | ptr, int | int | 1 |
| `read_int8(ptr, off)` | ptr, int | int | 1 |
| `read_int16(ptr, off)` | ptr, int | int | 2 |
| `read_uint16(ptr, off)` | ptr, int | int | 2 |
| `read_int(ptr, off)` | ptr, int | int | 4 |
| `read_uint(ptr, off)` | ptr, int | int/BigInt | 4 |
| `read_int64(ptr, off)` | ptr, int | int | 8 |
| `read_uint64(ptr, off)` | ptr, int | int/BigInt | 8 |
| `read_float(ptr, off)` | ptr, int | float | 4 |
| `read_double(ptr, off)` | ptr, int | float | 8 |
| `read_ptr(ptr, off)` | ptr, int | ptr | 8 |
| `read_bool(ptr, off)` | ptr, int | bool | 4 |
| `read_string(ptr, off)` | ptr, int | string | - |
| `read_string_n(ptr, off, len)` | ptr, int, int | string | - |

### 内存写入

| 函数 | 参数 | 返回 | 大小 |
|------|------|------|------|
| `write_byte(ptr, off, val)` | ptr, int, int | null | 1 |
| `write_int8(ptr, off, val)` | ptr, int, int | null | 1 |
| `write_int16(ptr, off, val)` | ptr, int, int | null | 2 |
| `write_int(ptr, off, val)` | ptr, int, int | null | 4 |
| `write_uint(ptr, off, val)` | ptr, int, int/BigInt | null | 4 |
| `write_int64(ptr, off, val)` | ptr, int, int | null | 8 |
| `write_uint64(ptr, off, val)` | ptr, int, int/BigInt | null | 8 |
| `write_float(ptr, off, val)` | ptr, int, float | null | 4 |
| `write_double(ptr, off, val)` | ptr, int, float | null | 8 |
| `write_ptr(ptr, off, val)` | ptr, int, ptr | null | 8 |
| `write_string(ptr, off, str)` | ptr, int, string | null | - |

### 字符串工具

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `string_bytes(str)` | string | int | 获取字符串的字节长度（UTF-8 编码） |
| `utf8_to_utf16(str)` | string | ptr | 将 UTF-8 字符串转换为 UTF-16 宽字符（Windows） |
| `utf16_to_utf8(ptr)` | ptr | string | 将 UTF-16 宽字符指针转换为 UTF-8 字符串（Windows） |

### clib 声明类型

| clib 类型 | C 对应 | 大小 | 说明 |
|-----------|--------|------|------|
| `i32` | `int32_t` | 4 | 有符号 32 位整数 |
| `u32` | `uint32_t` | 4 | 无符号 32 位整数 |
| `i64` | `int64_t` | 8 | 有符号 64 位整数 |
| `u64` | `uint64_t` | 8 | 无符号 64 位整数 |
| `f32` | `float` | 4 | 单精度浮点 |
| `f64` | `double` | 8 | 双精度浮点 |
| `str8` | `char*` | 8 | C 字符串指针（UTF-8） |
| `str16` | `wchar_t*` | 8 | 宽字符串指针（UTF-16） |
| `Ptr` | `void*` | 8 | 泛型指针 |
| `bool` | `int` | 4 | 布尔值（0/1） |
| `void` | `void` | - | 无返回值 |

---

*文档版本: 2.4*
*最后更新: 2026-06-06*

### 更新记录

- **v2.4** (2026-06-06):
  - 新增 `clib` 声明式调用语法文档
  - 新增 `str8`（C `char*`）和 `str16`（C `wchar_t*`）类型说明
  - 新增 clib 参数自动转换规则（`string` → `str8`/`str16`，`int` → `i32`/`i64`，`float` → `f64`）
  - 修正 `ffi.free()` 返回值为 `null`（非 `true`）

- **v2.3** (2026-05-24):
  - 新增 `ffi.read_uint64(ptr, off)` — 读取 uint64，支持 `0x8000000000000000` ~ `0xFFFFFFFFFFFFFFFF` 范围值正确显示为正数
  - 新增 `ffi.write_uint64(ptr, off, val)` — 写入 uint64，支持 `int` 和 `BigInt` 输入
  - 修复十六进制 bigint 字面量（如 `0xFFFFFFFFFFFFFFFF`）解析为 0 的问题

- **v2.2** (2026-05-23):
  - 新增 `ffi.ptr_to_int(ptr)` — 将指针地址值转换为整数（`ptr_from_int` 的逆操作）
  - 新增 `ffi.memset(ptr, value, size)` — 内存填充，替代低效的 `for` 循环 + `write_byte`
  - 新增 `ffi.write_uint(ptr, off, val)` — 写入 uint32，支持 `int` 和 `BigInt` 输入
  - 修复 `ffi.read_uint` 对 `0x80000000` ~ `0xFFFFFFFF` 范围值的溢出问题（返回正整数而非负数）

- **v2.1** (2026-05-23):
  - 统一 `ffi.free(x)` 释放所有 FFI 资源（指针/库/回调），删除 `ffi.free_lib()`
  - `ffi.callback()` 返回 `ObjFFICallback` 类型，不再伪装为 `ObjFFIPointer`
  - `ffi.is_ptr()` 对回调对象也返回 `true`
  - 新增回调函数文档章节和速查表
  - 参数类型自动推断表新增 FFI 回调对象 → `void*` 映射

- **v2.0** (2026-05-13):
  - 重构函数调用 API 设计
  - `ffi.call` 改为返回 `any` 类型
  - 新增 `ffi.call_int` 明确返回 int 类型
  - 添加类型守卫使用文档和示例
  - 更新返回值的类型映射表格

- **v1.4** (2026-05-09): 添加 `ffi.memcpy` 函数文档，用于高效的内存块拷贝操作

- **v1.3** (2026-05-08): 添加 `ffi.is_ptr` 函数文档，用于检查值是否是指针对象

- **v1.2** (2026-05-08): 添加字符串工具函数文档（`string_bytes`、`utf8_to_utf16`、`utf16_to_utf8`）

- **v1.1** (2026-05-08): 添加数字类型处理说明，包括十六进制数解析规则和 BigInt 类型在 FFI 调用中的行为
