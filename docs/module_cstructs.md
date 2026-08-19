# LenoC CStruct 模块 (cstructs)

本文档详细说明 `cstructs` 模块提供的 C 布局结构体操作功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [定义 cstruct](#定义-cstruct)
- [创建实例](#创建实例)
- [批量操作](#批量操作)
- [字段访问](#字段访问)
- [数组字段](#数组字段)
- [数组字段切片](#数组字段切片)
- [嵌套 cstruct](#嵌套-cstruct)
- [方法参考](#方法参考)
- [类型安全指针](#类型安全指针)
- [调试工具](#调试工具)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 概述

CStruct 是 Leno 语言中用于与 C 语言库和操作系统 API 交互的结构体类型。它与普通 struct 不同，采用 C 语言的内存布局规则：

- **紧凑布局**：字段按声明顺序排列
- **对齐要求**：遵循 C 语言的对齐规则
- **原始内存**：直接操作内存，无额外元数据
- **FFI 兼容**：可直接传递给 C 函数

---

## 使用方式

```leno
// 定义 cstruct
cstruct Point {
    i32 x
    i32 y
}

main() {
    // 创建实例
    var p = Point.malloc()
    
    // 访问字段
    p.x = 10
    p.y = 20
    
    print("Point: (" + p.x + ", " + p.y + ")")
    
    // 释放内存
    p.free()
}
```

---

## 定义 cstruct

### 基本语法

```leno
cstruct 结构体名 {
    类型 字段名
    类型 字段名2
    ...
}
```

### 支持的字段类型

| 类型 | 大小 | 说明 |
|------|------|------|
| `i8` | 1 字节 | 有符号 8 位整数 |
| `u8` | 1 字节 | 无符号 8 位整数 |
| `i16` | 2 字节 | 有符号 16 位整数 |
| `u16` | 2 字节 | 无符号 16 位整数 |
| `i32` | 4 字节 | 有符号 32 位整数 |
| `u32` | 4 字节 | 无符号 32 位整数 |
| `i64` | 8 字节 | 有符号 64 位整数 |
| `u64` | 8 字节 | 无符号 64 位整数 |
| `f32` | 4 字节 | 32 位浮点数 |
| `f64` | 8 字节 | 64 位浮点数 |
| `bool` | 1 字节 | 布尔值 |
| `c_int` | 平台相关 | C int 类型 |
| `c_uint` | 平台相关 | C unsigned int 类型 |
| `c_long` | 平台相关 | C long 类型 |
| `c_ulong` | 平台相关 | C unsigned long 类型 |
| `c_longlong` | 8 字节 | C long long 类型 |
| `c_ulonglong` | 8 字节 | C unsigned long long 类型 |
| `c_size` | 平台相关 | C size_t 类型 |
| `c_ssize` | 平台相关 | C ssize_t 类型 |
| `Ptr` | 平台相关 | 无类型指针 |
| `Ptr[T]` | 平台相关 | 类型安全指针 |
| `str16` | 2 字节/字符 | UTF-16 字符串数组 |

### packed — 取消字段间 padding

默认情况下，cstruct 遵循 C 语言的对齐规则：每个字段按其类型的自然对齐要求排列，中间可能插入填充字节。使用 `packed` 关键字可以取消所有 padding，使字段紧挨排列。

```leno
// 默认布局（有 padding）
cstruct Normal {
    u8  a       // offset 0, size 1
    // 3 字节 padding
    i32 b       // offset 4, size 4
    u8  c       // offset 8, size 1
    // 3 字节 padding
}               // total=12, alignment=4

// packed 布局（无 padding）
packed cstruct PackedDemo {
    u8  a       // offset 0, size 1
    i32 b       // offset 1, size 4  ← 无 padding！
    u8  c       // offset 5, size 1
}               // total=6, alignment=1
```

`packed` 等价于 C 语言的 `#pragma pack(1)` 或 `__attribute__((packed))`，常用于：
- 网络协议头（字段紧挨，无填充）
- 文件格式解析（二进制结构体精确匹配）
- 嵌入式系统的紧凑数据结构

### align(N) — 指定结构体整体对齐

`align(N)` 指定结构体的整体对齐要求，N 必须是 2 的幂（1, 2, 4, 8, 16, 32, 64）。字段仍按自然对齐排列，但结构体总大小和起始地址按 N 对齐。

```leno
// 对齐到 16 字节边界（如缓存行、SIMD）
align(16) cstruct CacheLine {
    i64 data    // offset 0, size 8
    // 8 字节 padding（对齐到 16）
}               // total=16, alignment=16
```

`align(N)` 等价于 C 语言的 `alignas(N)` 或 `__declspec(align(N))`。

### packed + align(N) 组合

`packed` 和 `align(N)` 可以组合使用，顺序可互换：

```leno
// packed 使字段紧挨，align(16) 使结构体整体对齐 16
packed align(16) cstruct NetworkPacket {
    u8  magic    // offset 0
    u16 length   // offset 1  ← packed 使字段紧挨
    u8  flag     // offset 3
}                // packed 后 3 字节, align(16) 后 total=16, alignment=16
```

### 数组字段

```leno
cstruct Buffer {
    u8 data[1024]      // 1024 字节的缓冲区
    u32 length         // 长度字段
}
```

### 嵌套 cstruct

```leno
cstruct Point {
    i32 x
    i32 y
}

cstruct Rect {
    Point top_left      // 嵌套结构体
    Point bottom_right
}
```

---

## 创建实例

### `malloc()`

分配内存并创建 cstruct 实例。

**参数**: 无  
**返回**: `cstruct` - 新实例

```leno
var p = Point.malloc()
// 使用 p...
p.free()
```

### `from_ptr(ptr)`

从现有指针创建 cstruct 实例（不复制内存）。

**参数**:
- `ptr` (Ptr): 指向现有内存的指针

**返回**: `cstruct` - 包装该指针的实例

```leno
import ffi

main() {
    // 分配内存
    var mem = ffi.malloc(8)
    
    // 从指针创建 cstruct
    var p = Point.from_ptr(mem)
    p.x = 10
    p.y = 20
    
    // 注意：from_ptr 创建的实例不拥有内存
    // 需要手动释放原始内存
    ffi.free(mem)
}
```

---

## 批量操作

cstruct 支持批量操作，可以一次性创建多个结构体实例的数组。

### `malloc_array(count)`

创建指定数量的 cstruct 实例数组。

**参数**:
- `count` (int): 数组元素个数

**返回**: `cstruct_array` - 结构体数组

```leno
cstruct Point {
    i32 x
    i32 y
}

main() {
    // 创建 100 个 Point 的数组
    var points = Point.malloc_array(100)
    
    // 初始化数组元素
    for 0 : 99 to i {
        points[i].x = i * 10
        points[i].y = i * 20
    }
    
    // 访问数组元素
    print("points[5] = (" + points[5].x + ", " + points[5].y + ")")
    // 输出: points[5] = (50, 100)
    
    // 获取数组长度
    print("数组长度: " + points.len())  // 100
    
    // 获取数组指针（用于 FFI）
    var ptr = points.to_ptr()
    
    // 释放整个数组
    points.free_all()
}
```

### 批量操作方法

#### `len()`

返回数组元素个数。

**参数**: 无  
**返回**: `int` - 数组长度

```leno
var arr = Point.malloc_array(50)
print(arr.len())  // 50
arr.free_all()
```

#### `free_all()`

释放整个结构体数组。

**参数**: 无  
**返回**: `bool` - 是否成功释放

```leno
var arr = Point.malloc_array(100)
// 使用 arr...
var result = arr.free_all()
print("释放结果: " + result)  // true
```

#### `to_ptr()`

获取数组首地址指针（用于 FFI 调用）。

**参数**: 无  
**返回**: `Ptr` - 数组首地址指针

```leno
import ffi

cstruct ProcessInfo {
    u32 pid
    u32 ppid
}

main() {
    var processes = ProcessInfo.malloc_array(100)
    
    // 获取指针，传递给 Windows API
    var ptr = processes.to_ptr()
    var count = ffi.call_int(psapi, "EnumProcesses", ptr, 400, ...)
    
    // 访问返回的数据
    for 0 : count - 1 to i {
        print("PID: " + processes[i].pid)
    }
    
    processes.free_all()
}
```

### 批量操作特性

- **连续内存**：数组元素在内存中连续存储，符合 C 语言数组布局
- **索引访问**：支持 `array[i]` 语法访问第 i 个元素
- **边界检查**：访问时自动进行边界检查
- **批量释放**：一次调用释放整个数组

---

## 字段访问

### 点号访问

```leno
var p = Point.malloc()
p.x = 10        // 设置字段
var y = p.y     // 读取字段
```

### 索引访问（字符串键）

```leno
var p = Point.malloc()
p["x"] = 10     // 设置字段
var y = p["y"]  // 读取字段
```

### 自动类型转换

```leno
cstruct Test {
    f32 value
}

main() {
    var t = Test.malloc()
    
    // int 自动转换为 float
    t.value = 42        // 自动转换为 42.0
    
    t.free()
}
```

---

## 数组字段

### 访问数组元素

```leno
cstruct Buffer {
    u8 data[10]
}

main() {
    var buf = Buffer.malloc()
    
    // 访问单个元素
    buf.data[0] = 65    // 'A'
    buf.data[1] = 66    // 'B'
    
    // 读取元素
    var first = buf.data[0]
    print(first)        // 65
    
    buf.free()
}
```

### str16 字符串数组

```leno
cstruct ProcessInfo {
    u32 pid
    str16 name[260]     // UTF-16 字符串数组
}

main() {
    var info = ProcessInfo.malloc()
    
    // 直接赋值字符串（自动转换为 UTF-16）
    info.name = "notepad.exe"
    
    // 读取时自动转换为 UTF-8
    print(info.name)    // "notepad.exe"
    
    info.free()
}
```

---

## 数组字段切片

cstruct 数组字段支持切片操作，语法与普通数组一致。

### 切片语法

```leno
cstruct Data {
    u32 values[10]
}

main() {
    var d = Data.malloc()
    
    // 初始化数据
    for 0 : 9 to i {
        d.values[i] = (i + 1) * 10
    }
    
    // 切片操作
    var slice1 = d.values[2:5]      // 索引 2 到 5（包含）
    var slice2 = d.values[:3]       // 从开始到 3（包含）
    var slice3 = d.values[7:]       // 从 7 到结尾
    
    print(slice1)   // [30, 40, 50, 60]
    print(slice2)   // [10, 20, 30, 40]
    print(slice3)   // [80, 90, 100]
    
    d.free()
}
```

### 切片特性

- 返回新的 Array 对象（独立副本）
- 修改切片不会影响原数组
- 支持所有数值类型

---

## 嵌套 cstruct

### 访问嵌套字段

```leno
cstruct Point {
    i32 x
    i32 y
}

cstruct Rect {
    Point top_left
    Point bottom_right
}

main() {
    var r = Rect.malloc()
    
    // 访问嵌套字段
    r.top_left.x = 0
    r.top_left.y = 0
    r.bottom_right.x = 100
    r.bottom_right.y = 100
    
    print("Rect: (" + r.top_left.x + ", " + r.top_left.y + ") - (" +
          r.bottom_right.x + ", " + r.bottom_right.y + ")")
    
    r.free()
}
```

---

## 方法参考

### 实例方法

#### `free()`

释放 cstruct 实例占用的内存。

**参数**: 无  
**返回**: `bool` - 是否成功释放（拥有内存并成功释放返回 true，否则返回 false）

```leno
var p = Point.malloc()
// 使用 p...
var result = p.free()    // 释放内存
print("释放结果: " + result)  // true

// 重复释放会返回 false
var result2 = p.free()
print("重复释放: " + result2)  // false
```

#### `to_ptr()`

获取指向 cstruct 内存的指针。

**参数**: 无  
**返回**: `Ptr` - 内存指针

```leno
import ffi

main() {
    var p = Point.malloc()
    p.x = 10
    p.y = 20
    
    // 获取指针，传递给 C 函数
    var ptr = p.to_ptr()
    
    // 示例：使用 ffi 读取内存
    var x = ffi.read_int(ptr, 0)
    print(x)    // 10
    
    p.free()
}
```

#### `to_str()`

获取 cstruct 的字符串表示。

**参数**: 无  
**返回**: `string` - 字符串表示

```leno
var p = Point.malloc()
p.x = 10
p.y = 20

print(p.to_str())
// 输出:
// Point {
//   i32 x;  // 10
//   i32 y;  // 20
// }

p.free()
```

#### `size()`

获取 cstruct 的总大小（字节数）。

**参数**: 无  
**返回**: `int` - 大小（字节）

```leno
print(Point.size())     // 8 (两个 i32)
print(Buffer.size())    // 1024 + 4 = 1028
```

#### `alignment()`

获取 cstruct 的对齐要求（字节数）。

**参数**: 无  
**返回**: `int` - 对齐要求

```leno
print(Point.alignment())    // 4
```

---

#### `offset_of(field_name)`

获取指定字段的字节偏移量。可用于编程式访问结构体字段，而不依赖 `debug()` 的文本输出。

**参数**:
- `field_name` (string): 字段名称

**返回**: `int` - 字段偏移量（字节）

**说明**:
- receiver 可以是 cstruct 定义或实例
- 字段不存在时抛出错误

```leno
cstruct POINT {
    i32 x
    i32 y
}

// 对定义调用
print(POINT.offset_of("x"))  // 0
print(POINT.offset_of("y"))  // 4

// 对实例调用
var p = POINT.malloc()
print(p.offset_of("x"))      // 0
print(p.offset_of("y"))      // 4
p.free()
```

---

## 类型安全指针

### Ptr[T] 语法

cstruct 支持类型安全的指针字段：

```leno
cstruct STARTUPINFO {
    u32 cb
    Ptr[u64] lpReserved      // 指向 u64 的指针
    Ptr[u64] lpDesktop       // 指向 u64 的指针
    Ptr[u16] lpTitle         // 指向 UTF-16 字符串的指针
}
```

### 支持的元素类型

- `Ptr[u8]`, `Ptr[u16]`, `Ptr[u32]`, `Ptr[u64]`
- `Ptr[i8]`, `Ptr[i16]`, `Ptr[i32]`, `Ptr[i64]`
- `Ptr[f32]`, `Ptr[f64]`

### 与 FFI 配合

```leno
import ffi

cstruct PROCESS_INFORMATION {
    Ptr[u64] hProcess
    Ptr[u64] hThread
    u32 dwProcessId
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    var pi = PROCESS_INFORMATION.malloc()
    
    // 直接使用 Ptr[u64] 接收句柄
    // ...
    
    // 关闭句柄
    ffi.call_bool(kernel32, "CloseHandle", pi.hProcess)
    ffi.call_bool(kernel32, "CloseHandle", pi.hThread)
    
    pi.free()
    ffi.free(kernel32)
}
```

---

## 调试工具

### `debug()`

显示详细的内存布局、偏移、对齐信息。

**参数**: 无  
**返回**: `string` - 调试信息

```leno
var p = Point.malloc()
p.x = 0x12345678
p.y = 0xABCDEF00

print(p.debug())
// 输出:
// === cstruct Point 调试信息 ===
//
// [基本信息]
//   总大小: 8 字节 (0x8)
//   对齐要求: 4 字节
//   字段数量: 2
//
// [字段布局]
//   索引 偏移       类型   名称  大小  数组维度
//   ...
//
// [当前值]
//   x = 305419896 (i32)
//   y = 2882400000 (i32)

p.free()
```

### `hex()`

以十六进制格式显示原始字节。

**参数**: 无  
**返回**: `string` - 十六进制表示

```leno
var p = Point.malloc()
p.x = 0x12345678
p.y = 0xABCDEF00

print(p.hex())
// 输出:
// === cstruct Point 原始字节 (共 8 字节) ===
//
//   偏移      00 01 02 03 04 05 06 07 ...
//   00000000  78 56 34 12 00 EF CD AB  |  xV4.....
//
// [字段标记]
//   0x0000-0x0003  x (i32)
//   0x0004-0x0007  y (i32)

p.free()
```

---

## 示例代码

### 1. Windows API 进程信息

```leno
import ffi

// PROCESSENTRY32W 结构体
cstruct PROCESSENTRY32W {
    u32 dwSize
    u32 cntUsage
    u32 th32ProcessID
    u64 th32DefaultHeapID
    u32 th32ModuleID
    u32 cntThreads
    u32 th32ParentProcessID
    i32 pcPriClassBase
    u32 dwFlags
    str16 szExeFile[260]
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    // 创建进程快照
    var hSnapshot = ffi.call_ptr(kernel32, "CreateToolhelp32Snapshot", 0x00000002, 0)
    
    var pe = PROCESSENTRY32W.malloc()
    pe.dwSize = PROCESSENTRY32W.size()
    
    // 获取第一个进程
    var result = ffi.call_int(kernel32, "Process32FirstW", hSnapshot, pe)
    
    while result != 0 {
        print("PID: " + pe.th32ProcessID + ", 名称: " + pe.szExeFile)
        result = ffi.call_int(kernel32, "Process32NextW", hSnapshot, pe)
    }
    
    pe.free()
    ffi.call_bool(kernel32, "CloseHandle", hSnapshot)
    ffi.free(kernel32)
}
```

### 2. 文件时间结构体

```leno
import ffi

cstruct FILETIME {
    u32 dwLowDateTime
    u32 dwHighDateTime
}

cstruct WIN32_FIND_DATA {
    u32 dwFileAttributes
    FILETIME ftCreationTime
    FILETIME ftLastAccessTime
    FILETIME ftLastWriteTime
    u32 nFileSizeHigh
    u32 nFileSizeLow
    u32 dwReserved0
    u32 dwReserved1
    str16 cFileName[260]
    str16 cAlternateFileName[14]
}

main() {
    var kernel32 = ffi.load("kernel32.dll")
    
    var findData = WIN32_FIND_DATA.malloc()
    
    // 查找文件
    var hFind = ffi.call_ptr(kernel32, "FindFirstFileW",
        ffi.utf8_to_utf16("*.txt"), findData)
    
    if hFind != ffi.nullptr() {
        print("文件: " + findData.cFileName)
        print("大小: " + (findData.nFileSizeHigh * 4294967296 + findData.nFileSizeLow))
        
        ffi.call_bool(kernel32, "FindClose", hFind)
    }
    
    findData.free()
    ffi.free(kernel32)
}
```

### 3. 网络地址结构体

```leno
import ffi

// sockaddr_in 结构体
cstruct in_addr {
    u32 s_addr
}

cstruct sockaddr_in {
    u16 sin_family
    u16 sin_port
    in_addr sin_addr
    u8 sin_zero[8]
}

main() {
    var addr = sockaddr_in.malloc()
    
    addr.sin_family = 2      // AF_INET
    addr.sin_port = 0x5000   // 80 (网络字节序)
    addr.sin_addr.s_addr = 0x0100007F  // 127.0.0.1
    
    // 查看内存布局
    print(addr.debug())
    print(addr.hex())
    
    addr.free()
}
```

### 4. 使用切片处理数组字段

```leno
cstruct Packet {
    u32 header
    u8 payload[256]
    u32 checksum
}

main() {
    var pkt = Packet.malloc()
    
    // 填充 payload
    for 0 : 255 to i {
        pkt.payload[i] = i
    }
    
    // 获取前 16 字节
    var header_data = pkt.payload[0:15]
    print("Header bytes: " + header_data)
    
    // 计算校验和（简化示例）
    var sum = 0
    for header_data to byte {
        sum = sum + byte
    }
    pkt.checksum = sum
    
    print(pkt.debug())
    
    pkt.free()
}
```

### 5. 批量操作 - 进程枚举

```leno
import ffi

// 简化的进程信息结构体
cstruct ProcessInfo {
    u32 pid
    u32 ppid
    str16 name[64]
}

main() {
    // 创建 100 个进程信息结构体的数组
    var processes = ProcessInfo.malloc_array(100)
    
    // 模拟填充数据（实际中会从 Windows API 获取）
    for 0 : 4 to i {
        processes[i].pid = 1000 + i
        processes[i].ppid = 500
        processes[i].name = "process_" + i + ".exe"
    }
    
    // 批量处理
    print("进程列表:")
    for 0 : 4 to i {
        print("  PID: " + processes[i].pid + 
              ", PPID: " + processes[i].ppid +
              ", 名称: " + processes[i].name)
    }
    
    print("\n总进程数: " + processes.len())
    
    // 批量释放
    processes.free_all()
}
```

### 6. 批量操作 - 批量文件处理

```leno
import ffi

cstruct FileInfo {
    str16 path[260]
    u32 size
    u32 attributes
}

main() {
    // 创建文件信息数组
    var files = FileInfo.malloc_array(50)
    
    // 模拟文件列表
    var file_names = ["doc1.txt", "doc2.txt", "image.png"]
    
    for file_names to name {
        var idx = file_names.index_of(name)
        files[idx].path = name
        files[idx].size = 1024 * (idx + 1)
        files[idx].attributes = 0
    }
    
    // 批量处理
    var total_size = 0
    for 0 : file_names.len() - 1 to i {
        total_size = total_size + files[i].size
        print(files[i].path + ": " + files[i].size + " 字节")
    }
    
    print("\n总大小: " + total_size + " 字节")
    
    files.free_all()
}

---

## 线程支持

cstruct 定义可以在多线程环境中安全使用。由于 cstruct 定义是**编译时确定的只读类型元数据**，多个线程可以共享这些定义而不会产生竞争条件。

### 在线程中使用 cstruct

```leno
import ffi
import threads

cstruct MEMORYSTATUS {
    u32 dwLength
    u32 dwMemoryLoad
    u64 dwTotalPhys
}

func get_memory_info(var ch) {
    // 在子线程中使用 cstruct
    var kernel32 = ffi.load("kernel32.dll")
    var mem = MEMORYSTATUS.malloc()
    
    mem.dwLength = MEMORYSTATUS.size()
    ffi.call(kernel32, "GlobalMemoryStatus", mem.to_ptr())
    
    ch.send(mem.dwTotalPhys)
    mem.free()
    ffi.free(kernel32)
}

main() {
    var ch = threads.channel(1)
    var t = threads.start(get_memory_info, ch)
    var total_mem = ch.receive()
    print("总物理内存: " + total_mem)
    t.join()
}
```

### 线程安全说明

| 特性 | 说明 |
|------|------|
| cstruct 定义 | 线程安全，只读共享 |
| cstruct 实例 | 每个线程独立，不共享 |
| `malloc()`/`free()` | 每个线程独立管理自己的实例 |
| `from_ptr()` | 可以安全使用，但只在当前线程有效 |

### 注意事项

1. **不要在线程间传递 cstruct 实例**：实例不能跨线程共享，每个线程应独立 `malloc()`
2. **FFI 资源独立管理**：每个线程加载的 DLL、分配的内存都是独立的
3. **通过 Channel 传递数据**：使用基本类型（int、float、string 等）或 Dict/Array 在线程间传递数据

```leno
// ✅ 正确：每个线程独立使用 cstruct
func worker(var ch) {
    var s = TEST_STRUCT.malloc()  // 子线程独立分配
    s.field1 = 123
    ch.send(s.field1)  // 发送数据，不是实例
    s.free()
}

// ❌ 错误：不要尝试共享实例
func bad_worker(var s, var ch) {
    s.field1 = 123  // 不要传递实例到线程
}
```

---

## 注意事项

### 1. 内存管理

- **必须手动释放**：cstruct 实例需要调用 `free()` 释放内存
- **内存泄漏风险**：忘记释放会导致内存泄漏
- **不要重复释放**：重复释放会导致未定义行为

```leno
var p = Point.malloc()
// 使用 p...
p.free()
// p.free()  // 错误！不要重复释放
```

### 2. 指针安全

- **空指针检查**：使用指针字段前检查是否为 null
- **生命周期管理**：确保指针指向的内存有效

```leno
if p.hProcess != ffi.nullptr() {
    ffi.call_bool(kernel32, "CloseHandle", p.hProcess)
}
```

### 3. 字节序

- **网络字节序**：网络相关结构体需要注意字节序转换
- **主机字节序**：本地操作通常使用主机字节序

### 4. 对齐和填充

- **自动对齐**：cstruct 自动处理字段对齐
- **平台差异**：不同平台可能有不同的对齐要求

```leno
// 查看对齐信息
print(MyStruct.alignment())
print(MyStruct.debug())
```

### 5. 数组越界

- **运行时检查**：数组访问会进行边界检查
- **越界错误**：越界访问会抛出运行时错误

```leno
var buf = Buffer.malloc()
buf.data[0] = 1       // OK
buf.data[1023] = 1    // OK
// buf.data[1024] = 1 // 错误！越界
buf.free()
```

---

## 与普通 struct 的区别

| 特性 | struct | cstruct |
|------|--------|---------|
| 内存布局 | 灵活（GC 管理） | C 兼容（原始内存） |
| 字段类型 | 任意 Leno 类型 | 仅 C 类型 |
| 内存管理 | 自动 GC | 手动 malloc/free |
| FFI 兼容 | 否 | 是 |
| 性能 | 一般 | 高效（无 GC 开销） |
| 适用场景 | 通用编程 | 系统编程、FFI |

---

## 最佳实践

1. **使用 try-finally 确保释放**
   ```leno
   var p = Point.malloc()
   try {
       // 使用 p...
   } finally {
       p.free()
   }
   ```

2. **使用 debug() 检查布局**
   ```leno
   // 定义后检查内存布局
   print(MyStruct.debug())
   ```

3. **使用 Ptr[T] 增强类型安全**
   ```leno
   // 推荐
   Ptr[u64] hProcess
   
   // 不推荐
   Ptr hProcess
   ```

4. **使用 str16 处理 Windows 字符串**
   ```leno
   str16 name[260]  // 自动 UTF-16 转换
   ```

---

*文档版本: 1.2*  
*最后更新: 2026-08-19*

## 更新日志

### v1.2 (2026-08-19)
- 新增 `offset_of(field_name)` 方法，编程式获取字段偏移量

### v1.1 (2026-05-17)
- 新增 **批量操作** 章节
- 添加 `malloc_array()`、`free_all()`、`len()` 方法文档
- 添加批量操作示例代码

### v1.0 (2026-05-17)
- 初始版本
- 包含基础 cstruct 定义、字段访问、数组字段、切片、调试工具等
