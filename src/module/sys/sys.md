# LenoC Sys 系统模块

本文档详细说明 `sys` 模块提供的系统级全局函数。

---

## 概述

Sys 模块提供与运行时环境、命令行参数和系统信息相关的全局函数。这些函数无需导入，直接在代码中使用。

### 特性

- **全局可用**：所有函数无需 `import` 即可使用
- **系统信息**：获取操作系统、命令行参数等
- **运行时控制**：GC 开关控制

---

## 全局函数

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `_args()` | 获取命令行参数数组 | `Array` |
| `_script()` | 获取当前脚本路径 | `String` / `null` |
| `_executable()` | 获取可执行文件路径 | `String` / `null` |
| `_gc(enabled)` | 控制 GC 开关 | `Bool` |
| `_os()` | 获取操作系统名称 | `String` |
| `_clear()` | 清屏（跨平台） | `null` |

---

## 命令行参数

### `_args()`

获取所有命令行参数，包括可执行文件路径。

**参数**: 无  
**返回**: `Array` - 参数数组

```leno
main() {
    var args = _args()
    print("参数个数:", args.len())
    for args to var arg {
        print("  " + arg)
    }
}
```

运行 `leno script.leno --port 8080` 输出：
```
参数个数: 3
  D:\Leno\build\leno.exe
  script.leno
  --port
  8080
```

---

### `_script()`

获取当前运行的脚本路径（第一个非选项参数）。

**参数**: 无  
**返回**: `String` / `null` - 脚本路径，REPL 模式下返回 `null`

```leno
main() {
    var script = _script()
    if script != null {
        print("当前脚本:", script)
    } else {
        print("REPL 模式，无脚本")
    }
}
```

---

### `_executable()`

获取 Leno 可执行文件的完整路径。

**参数**: 无  
**返回**: `String` / `null` - 可执行文件路径

```leno
main() {
    print("可执行文件:", _executable())
}
```

---

## 运行时控制

### `_gc(enabled)`

控制垃圾回收器（GC）的开关状态。

**参数**:
- `enabled` (bool/int, 可选):
  - `true` 或 `1` - 启用 GC
  - `false` 或 `0` - 禁用 GC
  - 不传参数则只查询状态

**返回**: `bool` - 当前 GC 状态

```leno
// 查询 GC 状态
var gc_enabled = _gc()
print("GC enabled:", gc_enabled)

// 禁用 GC（性能关键区域）
_gc(false)
// ... 执行大量内存分配操作
_gc(true)  // 重新启用

// 在循环中临时禁用 GC
_gc(false)
for 0 : 100000 to i {
    var arr = [i, i+1, i+2]  // 大量临时对象
}
_gc(true)
```

**注意**: 禁用 GC 后，内存不会自动回收，长时间禁用可能导致内存耗尽。

---

## 系统信息

### `_clear()`

清屏（跨平台）。在 Windows 上执行 `cls`，在其他系统上执行 `clear`。

**参数**: 无  
**返回**: `null`

```leno
// 清屏
_clear()

// 在循环中清屏并刷新输出
for 0 : 100 to i {
    _clear()
    print("当前进度: " + i + "%")
}
```

---

### `_os()`

获取当前操作系统名称。

**参数**: 无  
**返回**: `String` - 操作系统名称

返回值可能是：
- `"windows"` - Windows
- `"macos"` - macOS
- `"ios"` - iOS
- `"linux"` - Linux
- `"unix"` - 其他 Unix
- `"unknown"` - 未知

```leno
// 获取操作系统
var os = _os()
print("当前操作系统:", os)

// 根据系统执行不同逻辑
if os == "windows" {
    print("Running on Windows")
} else if os == "linux" {
    print("Running on Linux")
} else if os == "macos" {
    print("Running on macOS")
}

// 路径分隔符适配
var separator = (os == "windows") ? "\\" : "/"
var path = "folder" + separator + "file.txt"
```

---

## 示例代码

### 示例1：简单的 CLI 参数解析

```leno
main() {
    var args = _args()

    // 跳过可执行文件和脚本路径
    for 2 : args.len() - 1 to var i {
        if args[i] == "--port" && i + 1 < args.len() {
            print("端口:", args[i + 1])
        } else if args[i] == "--debug" {
            print("调试模式已启用")
        }
    }
}
```

### 示例2：跨平台路径处理

```leno
import dirs

main() {
    var os = _os()
    print("操作系统:", os)

    // 使用 dirs 模块处理路径
    var path = dirs.join("folder", "subfolder", "file.txt")
    print("路径:", path)
}
```

### 示例3：GC 性能测试

```leno
import times

func test_with_gc() {
    print("测试: GC 启用")
    _gc(true)

    var start = times.ms()
    for 0 : 100000 to i {
        var arr = [i, i+1, i+2, i+3, i+4]
    }
    var end = times.ms()
    print("耗时:", end - start, "ms")
}

func test_without_gc() {
    print("测试: GC 禁用")
    _gc(false)

    var start = times.ms()
    for 0 : 100000 to i {
        var arr = [i, i+1, i+2, i+3, i+4]
    }
    var end = times.ms()
    print("耗时:", end - start, "ms")

    _gc(true)
    print("GC 已重新启用")
}

main() {
    print("========== GC 性能测试 ==========")
    test_with_gc()
    print("")
    test_without_gc()
}
```

### 示例4：清屏动画效果

```leno
import times

main() {
    print("========== 清屏演示 ==========")
    for 0 : 10 to i {
        _clear()
        print("倒计时: " + (10 - i))
        times.sleep(1000)
    }
    _clear()
    print("完成!")
}
```

### 示例5：显示运行时信息

```leno
main() {
    print("========== 运行时信息 ==========")
    print("操作系统:", _os())
    print("可执行文件:", _executable())
    print("当前脚本:", _script())
    print("GC 状态:", _gc())
    print("")
    print("命令行参数:")
    for _args() to var arg {
        print("  " + arg)
    }
}
```

---

## 注意事项

### 1. 参数顺序

`_args()` 返回的数组中：
- 索引 0：可执行文件路径
- 索引 1：脚本路径（如果是文件模式）
- 索引 2+：用户传入的参数

### 2. REPL 模式

在 REPL 模式下：
- `_script()` 返回 `null`
- `_args()` 只包含可执行文件路径

### 3. GC 控制

- 一般情况下不需要手动控制 GC
- 在性能关键区域可以临时禁用
- 禁用后记得重新启用

```leno
// 不推荐：长时间禁用 GC
_gc(false)
// ... 运行很长时间
_gc(true)  // 可能内存已经耗尽

// 推荐：短时间禁用
_gc(false)
for 0 : 1000 to i {
    // 快速创建临时对象
}
_gc(true)
```

### 4. 脚本路径解析

`_script()` 返回的是命令行中传入的原始路径，不是绝对路径。如需绝对路径，可以结合 `_args()` 自行处理。

---

*文档版本: 1.1*  
*最后更新: 2026-05-31*
