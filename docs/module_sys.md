# LenoC Sys 系统模块

本文档详细说明 `sys` 模块提供的系统级全局函数。

---

## 概述

Sys 模块提供与运行时环境、命令行参数和系统信息相关的全局函数。这些函数无需导入，直接在代码中使用。

### 特性

- **全局可用**：所有函数无需 `import` 即可使用
- **系统信息**：获取操作系统、CPU架构、用户名等
- **运行时控制**：GC 开关控制、进程退出
- **环境变量**：获取和设置环境变量
- **路径工具**：主目录、临时目录、路径分隔符
- **命令执行**：执行系统命令并获取输出

---

## 全局函数

| 函数 | 说明 | 返回值 |
|------|------|--------|
| `_args()` | 获取脚本命令行参数数组 | `Array` |
| `_script()` | 获取当前脚本路径 | `String` / `null` |
| `_executable()` | 获取可执行文件路径 | `String` / `null` |
| `_gc(enabled)` | 控制 GC 开关 | `Bool` |
| `_os()` | 获取操作系统名称 | `String` |
| `_arch()` | 获取 CPU 架构 | `String` |
| `_pid()` | 获取当前进程 ID | `Int` |
| `_env(name)` | 获取环境变量 | `String` / `null` |
| `_env(name, value)` | 设置环境变量 | `Bool` |
| `_exit(code)` | 以指定退出码终止程序 | 无 |
| `_exec(cmd)` | 执行系统命令并返回 [输出,退出码] | `[String, Int]` / `null` |
| `_username()` | 获取当前登录用户名 | `String` / `null` |
| `_homedir()` | 获取用户主目录路径 | `String` / `null` |
| `_tmpdir()` | 获取系统临时目录路径 | `String` / `null` |
| `_sep()` | 获取路径分隔符 | `String` |
| `_clear()` | 清屏（跨平台） | `null` |
| `_console(show)` | 显示/隐藏控制台窗口（Windows） | `Bool` |

---

## 命令行参数

### `_args()`

获取脚本自身的命令行参数数组（不包含解释器路径和脚本路径）。

**参数**: 无  
**返回**: `Array` - 参数数组（只包含脚本自身的参数）

```leno
main() {
    var args = _args()
    print("参数个数: ", args.len())
    for args to var arg {
        print("  " + arg)
    }
}
```

运行 `leno script.leno --port 8080` 输出：
```
参数个数: 2
  --port
  8080
```

> ⚠️ `_args()` 只返回脚本参数。如需解释器路径用 `_executable()`，脚本路径用 `_script()`。

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

### `_exit(code)`

以指定退出码终止程序。

**参数**:
- `code` (int, 可选): 退出码，默认为 `0`

**返回**: 无（程序终止）

```leno
// 正常退出
_exit(0)

// 异常退出
_exit(1)

// 根据条件退出
if config == null {
    print("配置文件不存在")
    _exit(1)
}
```

**注意**: `_exit()` 会立即终止程序，不会执行后续代码。

---

## 系统信息

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
```

---

### `_arch()`

获取当前 CPU 架构。

**参数**: 无  
**返回**: `String` - CPU 架构名称

返回值可能是：
- `"x64"` - x86-64 / AMD64
- `"x86"` - x86 (32位)
- `"arm64"` - ARM64 / AArch64
- `"arm"` - ARM (32位)
- `"riscv64"` - RISC-V 64位
- `"riscv32"` - RISC-V 32位
- `"unknown"` - 未知

```leno
// 获取 CPU 架构
var arch = _arch()
print("CPU 架构:", arch)

// 根据架构选择不同的库
if arch == "x64" {
    print("64位 x86 平台")
} else if arch == "arm64" {
    print("64位 ARM 平台")
}
```

---

### `_pid()`

获取当前进程 ID。

**参数**: 无  
**返回**: `Int` - 进程 ID

```leno
var pid = _pid()
print("当前进程 ID:", pid)

// 写入 PID 文件（用于守护进程）
import files
files.write("app.pid", _pid())
```

---

### `_username()`

获取当前登录用户名。

**参数**: 无  
**返回**: `String` / `null` - 用户名

```leno
var user = _username()
print("当前用户:", user)
```

---

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

### `_console(show)`

显示/隐藏控制台窗口（仅 Windows 有效）。

**参数**:
- `show` (bool/int, 可选):
  - `true` 或 `1` - 显示控制台
  - `false` 或 `0` - 隐藏控制台
  - 不传参数则只查询状态

**返回**: `bool` - 当前控制台窗口是否可见

```leno
// 隐藏控制台窗口（GUI 程序）
_console(false)

// 查询控制台状态
if _console() {
    print("控制台可见")
}
```

**注意**: 此功能仅在 Windows 上有效，其他平台始终返回 `true`。

---

## 环境变量

### `_env(name)`

获取指定环境变量的值。

**参数**:
- `name` (String): 环境变量名

**返回**: `String` / `null` - 环境变量值，不存在则返回 `null`

```leno
// 获取 PATH
var path = _env("PATH")
print("PATH:", path)

// 获取 HOME
var home = _env("HOME")
if home != null {
    print("主目录:", home)
}
```

### `_env(name, value)`

设置环境变量的值。

**参数**:
- `name` (String): 环境变量名
- `value` (String): 环境变量值

**返回**: `bool` - 设置是否成功

```leno
// 设置环境变量
var ok = _env("MY_VAR", "hello")
print("设置结果:", ok)

// 读取刚设置的变量
print("MY_VAR:", _env("MY_VAR"))

// 删除环境变量（设为空字符串）
_env("MY_VAR", "")
```

**注意**: 设置的环境变量仅在当前进程及其子进程中有效，不会影响父进程或系统级环境变量。

---

## 路径工具

### `_homedir()`

获取当前用户的主目录路径。

**参数**: 无  
**返回**: `String` / `null` - 主目录路径

```leno
var home = _homedir()
print("主目录:", home)

// 拼接配置文件路径
var config_path = _homedir() + _sep() + ".config" + _sep() + "app.conf"
```

---

### `_tmpdir()`

获取系统临时目录路径。

**参数**: 无  
**返回**: `String` / `null` - 临时目录路径

```leno
var tmp = _tmpdir()
print("临时目录:", tmp)

// 创建临时文件
var tmp_file = _tmpdir() + _sep() + "output.tmp"
```

---

### `_sep()`

获取当前系统的路径分隔符。

**参数**: 无  
**返回**: `String` - 路径分隔符（Windows 为 `\`，其他为 `/`）

```leno
var sep = _sep()
print("路径分隔符:", sep)

// 跨平台路径拼接
var path = "folder" + _sep() + "subfolder" + _sep() + "file.txt"
```

---

## 命令执行

### `_exec(cmd)`

执行系统命令并返回 `[标准输出, 退出码]` 数组。

**参数**:
- `cmd` (String): 要执行的命令

**返回**: `[String, Int]` / `null` - 索引 0 是标准输出内容，索引 1 是退出码；执行失败返回 `null`

```leno
// 执行命令并获取输出和退出码
var r = _exec("echo hello")
print("输出:", r[0])
print("退出码:", r[1])

// 检查命令是否成功
var result = _exec("leno.exe test.leno")
if result[1] == 0 {
    print("测试通过")
} else {
    print("测试失败: " + result[0])
}
```

**注意**:
- `_exec()` 会等待命令执行完毕后返回
- 返回值是 `[stdout_string, exit_code]` 数组，可同时获取输出和退出码
- 如需获取 stderr，在命令中加 `2>&1` 重定向：`_exec("mycmd 2>&1")`
- **Windows `_popen` 路径问题**：如果命令路径中包含引号 `"`，`_popen` 可能返回错误。建议不使用引号包裹路径，直接拼接：`_exec(leno + " " + test_file)` 而非 `_exec("\"" + leno + "\" \"" + test_file + "\"")`

---

## 示例代码

### 示例1：简单的 CLI 参数解析

```leno
main() {
    var args = _args()

    for args to var arg {
        if arg == "--port" {
            // 解析方式取决于实际参数排列
            print("检测到 --port")
        } else if arg == "--debug" {
            print("调试模式已启用")
        }
    }
}
```

### 示例2：跨平台路径处理

```leno
main() {
    var os = _os()
    print("操作系统:", os)
    print("CPU 架构:", _arch())

    // 使用 _sep() 适配路径分隔符
    var path = "folder" + _sep() + "subfolder" + _sep() + "file.txt"
    print("路径:", path)

    // 也可以使用 dirs 模块
    import dirs
    var path2 = dirs.join("folder", "subfolder", "file.txt")
    print("路径:", path2)
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
    print("CPU 架构:", _arch())
    print("进程 ID:", _pid())
    print("可执行文件:", _executable())
    print("当前脚本:", _script())
    print("GC 状态:", _gc())
    print("用户名:", _username())
    print("主目录:", _homedir())
    print("临时目录:", _tmpdir())
    print("路径分隔符:", _sep())
    print("")
    print("命令行参数:")
    for _args() to var arg {
        print("  " + arg)
    }
}
```

### 示例6：环境变量操作

```leno
main() {
    // 读取环境变量
    var path = _env("PATH")
    print("PATH:", path)

    // 设置自定义环境变量
    _env("APP_MODE", "production")
    _env("APP_PORT", "8080")

    // 读取自定义变量
    print("APP_MODE:", _env("APP_MODE"))
    print("APP_PORT:", _env("APP_PORT"))

    // 清除自定义变量
    _env("APP_MODE", "")
    _env("APP_PORT", "")
}
```

### 示例7：执行系统命令

```leno
main() {
    // 获取当前目录
    var cwd = ""
    if _os() == "windows" {
        cwd = _exec("cd")
    } else {
        cwd = _exec("pwd")
    }
    print("当前目录:", cwd)

    // 获取磁盘使用情况（Linux）
    if _os() == "linux" {
        var df = _exec("df -h /")
        print("磁盘信息:", df)
    }
}
```

---

## 注意事项

### 1. 参数顺序

`_args()` 只返回脚本自身的命令行参数：
- 不包含解释器路径（用 `_executable()` 获取）
- 不包含脚本路径（用 `_script()` 获取）

运行 `leno.exe --flag script.leno arg1 arg2` 时：
- `_executable()` = `"leno.exe"`
- `_script()` = `"script.leno"`
- `_args()` = `["arg1", "arg2"]`

### 2. REPL 模式

在 REPL 模式下：
- `_script()` 返回 `null`
- `_args()` 返回空数组

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

### 5. 环境变量作用域

通过 `_env(name, value)` 设置的环境变量仅在当前进程及其子进程中有效，不会修改系统级环境变量或影响父进程。

### 6. 命令执行安全

`_exec()` 直接执行传入的命令字符串，请避免将不受信任的用户输入直接传入，以防止命令注入攻击。

### 7. 进程退出

`_exit()` 会立即终止程序，不会执行任何清理操作（如 GC 等）。如需在退出前执行清理逻辑，请手动处理后再调用 `_exit()`。

---

*文档版本: 1.3*  
*最后更新: 2026-06-19*
*变更: `_exec()` 返回 `[stdout, exit_code]` 数组；`_args()` 只返回脚本参数*
