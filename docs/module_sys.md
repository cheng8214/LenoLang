# Leno Sys 系统模块

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
| `_executable()` | 获取自身可执行文件的绝对路径 | `String` / `null` |
| `_gc(enabled)` | 控制 GC 开关 | `Bool` |
| `_os()` | 获取操作系统名称 | `String` |
| `_arch()` | 获取 CPU 架构 | `String` |
| `_pid()` | 获取当前进程 ID | `Int` |
| `_env(name)` | 获取环境变量 | `String` / `null` |
| `_env(name, value)` | 设置环境变量 | `Bool` |
| `_env_or(name, default)` | 获取环境变量（取不到或空串返回 default） | `String` |
| `_exit(code)` | 以指定退出码终止程序 | 无 |
| `_exec(cmd[, timeout_ms][, stdin_text])` | 执行系统命令（可带超时与 stdin）；返回 `ExecResult{ output, out, err, code }` | `ExecResult` / `null` |
| `_spawn(exe, args)` | **异步**拉起长驻进程并**保留句柄**（失败 -1）| `Int` |
| `_kill(handle)` | 收掉句柄对应的**整棵进程树**（幂等）| `Bool` |
| `_single_instance([name])` | **单实例互斥**：`true` = 我拿到了锁（第一个实例）；`false` = 已有实例在跑（名字省略 ⇒ 用本 exe 路径）| `Bool` |
| `_instance_activate([name])` | 把**持锁那个进程**的窗口顶到前台（`true` = 找到窗口并请求过；找不到 / 平台不支持 ⇒ `false`）| `Bool` |
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

> **边界规则（2026-09-18 起；修的是"解释器把脚本的 `--xxx` 当自己的旗标"这个坑）**：
> **脚本路径之后的一切参数都算脚本参数**（**含 `-` 开头的**）；也可以用 `--` 显式终止解释器
> 自己的选项解析。未知旗标写在**脚本路径之前**仍会报「未知选项」并打印帮助（提示保持有效）。
>
> | 命令行 | `_args()` |
> | --- | --- |
> | `leno.exe app.leno --list status` | `["--list", "status"]` |
> | `leno.exe -- app.leno -x` | `["-x"]` |
> | `leno.exe --nope app.leno` | （报错：未知选项 `--nope`，退出码 64）|
>
> ⚠ **2026-09-26 彻底化（口径 = 内置旗标必须写在脚本路径之前）**：此前与**内置名撞名**的参数
> （`-v/--version/-h/--help/-c/--compile/-p/--pack/-o/--pack-dir/--debug/--pause/--no-cache/
> --onefile/--console/--no-console/--init/--install/--debug-out`）**在任何位置都生效** ⇒
> `leno.exe app.leno -v` 会打印版本、**脚本根本不跑** ✗；`app.leno --no-cache` 则"双重生效"
> （脚本收到 + 解释器也照做）✗。现在**见到脚本路径（或 `--`）之后，解释器不再识别任何内置旗标**：
>
> | 命令行 | 现在的行为 |
> | --- | --- |
> | `leno.exe app.leno -v --no-cache` | `_args()` = `["-v", "--no-cache"]`，脚本照跑 ✓ |
> | `leno.exe -v` | 打印解释器版本（内置旗标**前置**才生效 ✓）|
> | `leno.exe -c app.leno` | 编译为 `.lenb`（同上，前置 ✓）|
>
> ⚠ **2026-10-17 打包版修正**：打包产物（`-p`，含 `--onefile`）的命令行里**根本没有脚本路径**
> —— 脚本已经嵌进 exe 里了 ⇒ 边界变成"**argv[0] 是 exe 自己，`argv[1]` 起全是脚本参数**"。
> 修前这里的判据是"第一个非选项参数有没有 `.exe` 后缀"，两处都错 ✗（那个位置在打包版下就是
> 第一个**用户参数**；且 Linux 打包产物没有 `.exe` 后缀）⇒ 判定恒假、按"解释器 + 脚本"切分
> ⇒ **第一个用户参数被当成脚本吃掉** ✗（真实症状：LenoQR 打包版的 `gen_png URL out.png`
> 把 `out.png` 当成要编码的文本；`decode_png` 报"用法"）。修复由入口**如实上报**
> （`src/vm_main.c` 检测到尾部内嵌 `.lenb` 时置位）✓
>
> | 命令行（打包产物 `app`） | `_args()` |
> | --- | --- |
> | `app.exe a b -c` | `["a", "b", "-c"]`（修前为 `["b", "-c"]` ✗）|
> | `app.exe` | `[]` |

---

### `_script()`

获取当前运行的脚本路径（第一个非选项参数）。

- 解释器模式（`leno.exe app.leno`）⇒ 脚本文件路径
- **打包版**（`-p` / `--onefile`，2026-10-17 修正）⇒ **没有脚本文件**（脚本已嵌进 exe）
  ⇒ 返回 exe 自身路径，与 `_executable()` 同值。修前它按"第一个非选项参数"取，在打包版下
  拿到的是**第一个用户参数** ✗（`app a b` ⇒ `_script()` 返回 `'a'`，`dirs.script_dir()`
  也跟着跑到用户参数所在的目录 ✗）

**参数**: 无  
**返回**: `String` / `null` - 脚本路径（打包版为 exe 自身路径），REPL 模式下返回 `null`

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

获取**当前进程自身**可执行文件的绝对路径（跨平台）。

- 未打包运行（`leno.exe app.leno`）⇒ 宿主解释器自己的路径（`leno.exe` / `leno_vm.exe`；**不是脚本**，脚本路径用 `_script()`）
- 单文件打包（`--onefile`）⇒ 打包产物 exe 自身的路径（注册表自启 / 快捷方式要的就是它）
- 与启动方的写法无关：即便以相对写法调用（`build\leno.exe x.leno`），返回的也是绝对路径
  （平台真值：Windows `GetModuleFileNameW` / Linux `/proc/self/exe` / macOS `_NSGetExecutablePath`）

**参数**: 无  
**返回**: `String` / `null` - 可执行文件绝对路径（取不到时为 `null`）

```leno
main() {
    print("可执行文件:", _executable())   // 如 D:\CLeno\Leno\build\leno.exe
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

> ⚠ **非 ASCII 值（2026-10-14 修）**：此前 `_env("X", "中文")` 在 Windows 上会**写失败**（返回
> `false`），且**子进程看不到**这个变量；而纯 ASCII 值、含换行的值、8000 字节的长值全都正常 ——
> 很容易被误判成"多行截断"。
> 根因：老实现走窄口 `_putenv_s`，Windows 上它要按 **ANSI 代码页**把 UTF-8 字节转成宽字符再写进
> 环境块；UTF-8 的"中文"在 GBK（CP936）下不是合法序列 ⇒ 转换失败、变量从没进 Win32 环境块；
> 而 CRT 自己的**窄副本却已被写入** ⇒ 表现为"本进程读得回、子进程读不到"，很有欺骗性。
> 修法：非 ASCII 值走 `SetEnvironmentVariableW`（UTF-8 → UTF-16，**全程不经代码页**）、
> 读走 `GetEnvironmentVariableW`；**纯 ASCII 值仍走原路**（那条路会同步 CRT 窄副本 ——
> 进程内直接用 `getenv` 的 C 库（如 libcurl）只认那个副本）✓。
> 回归用例：`assert/test_env_utf8.leno`（含"子进程必须看到同一个值"的跨进程断言）。
>
> ⚠ 反过来说：**别用 `_env` 传二进制**（值里不能有 NUL、长度也有平台上限）；
> 要传一段数据给子进程请用 `_exec` 的 `stdin_text` ✓。
> 顺带：`_env` 的**返回值要接**——写失败时返回 `false`，忽略它就等于把"没设上"当成"设上了" ✗。

### `_env_or(name, default)`

获取指定环境变量的值；**取不到**或**为空串**时返回 `default`。

**参数**:
- `name` (String): 环境变量名
- `default` (String): 缺省值（**必填**）

**返回**: `String` - 环境变量值，**总是字符串**（不必再判 `null` / `_str()`）

```leno
// 读一个"可能没设置"的变量：省掉 `if x == null` + `_str(x)` 样板
// （`_str(null)` 得到的是字符串 "null" ✗，所以以前必须先判空再转）
var base = _env_or("TRAE_API_BASE", "https://api.trae.cn")
print("接口:", base)

// 判"是否存在"、以及"设置变量"仍用 `_env`
if _env("TRAE_CHECKIN_DEBUG") != null { print("调试模式已开") }
```

**注意**: `_env_or` 与 `_env` 并存、**不是替换** —— 设置变量、判存在（`!= null`）继续用 `_env` ✓。
"没设置"与"值为空串"都回默认（Windows 的 `cmd` 里 `set VAR=` 本身就是**删除**该变量；
想"特意设成空串"请用 `_env` ✓）。

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

### `_exec(cmd[, timeout_ms][, stdin_text])`

执行系统命令并返回结构体 `ExecResult{ string output, string out, string err, int code }`
（**字段类型编译期已知**，v3.2.8 起；`out` / `err` 与 `stdin_text` 于 **2026-10-14** 补）。

**参数**:
- `cmd` (String): 要执行的命令
- `timeout_ms` (Int, 可选): **超时毫秒数**。> 0 时超时即**杀掉**子进程（整棵树），退出码返回 **`124`**
  （GNU `timeout` 的惯例）；不传 / 传 `0` = 不限制（旧行为）
- `stdin_text` (String, 可选): 喂给子进程 **stdin** 的内容（按 UTF-8**字节**原样写入，二进制安全）

> 第 2 个实参**按类型分流**：`Int` ⇒ 超时、`String` ⇒ stdin ⇒ 以下四种写法都合法：
> `_exec(cmd)` · `_exec(cmd, 5000)` · `_exec(cmd, "输入")` · `_exec(cmd, 5000, "输入")`

**返回**: `ExecResult` / `null` - 执行失败返回 `null`

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `output` | `string` | **合并文本** = `out` 后接 `err`（老调用点零改动） |
| `out` | `string` | **仅 stdout** |
| `err` | `string` | **仅 stderr** |
| `code` | `int` | 退出码；**超时是 124** |

> ⚠ **合并文本的行序不是"真实交错"**：`out` / `err` 各自重定向到独立的临时文件；
> "哪一行属于哪个流"是**确定**信息，行序不是（共享一个文件时行序由 OS 写入时机决定、跨平台还不一样）
> ⇒ 要分流的场合请直接用 `out` / `err`，别拿 `output` 猜。`output` 保留只为兼容既有调用点 ✓。

> ⚠ **v3.2.8 起返回结构体、不再是 `[output, code]` 数组**：旧写法 `r[0]` / `r[1]` 要改成
> `r.output` / `r.code`（数组下标会落到"struct 字段名必须是字符串"的运行期错误）。
> 为什么要改：两个元素**类型不齐**（string + int）⇒ 同质的 `Array[T]` 表达不了，只能注册成
> `Array[any]`，调用点位置取值拿到的是 any（与 `dirs.stat` 改 `DirInfo` 同一判断）。

> ⚠ **建议总是给个 `timeout_ms`**（工具 / 测试尤其）：不限制时子进程一挂住，会把父进程一起拖死 ✗
> （写 GUI 测试时只能从外部 `Start-Process` + `WaitForExit` 兜）。超时判定就是 `r.code == 124`：
>
> ```leno
> var r = _exec("some_maybe_hanging_tool", 30000)   // 30 s
> if r.code == 124 {
>     print("超时了（子进程已被杀掉）")
> }
> ```
>
> ✅ **命令自己拉起的后台进程会活下来**（只有"超时"才收整棵树）：`_exec("cmd /c start \"\" \"a.txt\"")`
> 这种**异步**打开是正当用法 —— `cmd` 立刻退出、被拉起的应用继续跑 ✓。
> 超时给得及时，则连同孙子一起收（`TerminateJobObject`），不会留下握着临时文件的孤儿。
>
> 实现：Windows 走 `CreateProcessW` + `WaitForSingleObject` + `TerminateProcess`；POSIX 用 coreutils
> 的 `timeout` 包一层（系统没有该命令时会以 `127` **响亮**失败，不会静默忽略超时）。

```leno
// 执行命令并获取输出和退出码（v3.2.8 起是字段取值）
var r = _exec("echo hello")
print("输出:", r.output)          // 合并文本（= out 后接 err）
print("退出码:", r.code)

// 分流：正常结果与诊断分开判断（2026-10-14 起有 out / err）
var b = _exec("mytool --build")
if b.code != 0 {
    print("失败原因（stderr）: " + b.err)
} else {
    print("产物信息（stdout）: " + b.out)
}

// stdin：把一段数据直接喂给子进程（不必再自己写临时文件 / 塞环境变量）
var rr = _exec("node -e \"let d='';process.stdin.on('data',c=>d+=c).on('end',()=>console.log(d))\"",
               20000, "喂给 node stdin 的内容")

// 检查命令是否成功
var result = _exec("leno.exe test.leno")
if result.code == 0 {
    print("测试通过")
} else {
    print("测试失败: " + result.output)
}
```

**注意**:
- `_exec()` 会等待命令执行完毕后返回
- 返回值是 `ExecResult{ output, out, err, code }` 结构体（v3.2.8 起；此前是 `[output, code]` 数组）
- **不再需要 `2>&1`**：两个流本来就分开采集；老代码里写的 `2>&1` 照旧可用，只是多余
- ⚠ **命令里的 `&` / `|` 也整组受重定向约束**（2026-10-14 修）：实现把整条命令行包进 `( )` 再重定向
  （`cmd /c ( <cmd> ) > out 2> err`）。此前 `_exec("echo A & echo B")` 里 `A` 的输出会**漏到父进程
  控制台** ✗（重定向只绑在 `&` 的后半句上），现在不会了 ✓
- ⚠ **Windows 引号坑**：`_exec` 走 `cmd /c`，把带空格的路径用引号包起来再拼进命令时，引号可能被
  `cmd` 吞掉 ⇒ 更稳的是 `_spawn(exe, args)`（CreateProcessW，**不经 `cmd`**），
  或先把命令写进 `.bat` 再 `_exec(那个文件)`
- ℹ **`%TEMP%` 里可能出现 `leno_exec_<pid>_<序号>.out/.err/.in`**：这是本次执行的重定向文件。
  它在命令跑完后**总是尝试删除**，但若**子进程自己拉起的后台进程**（`cmd /c start …` 那类）继承了
  重定向句柄并还活着，删除会失败 ⇒ 留下文件。文件名每次唯一 ⇒ 不影响后续调用（这是"临时文件"方案的
  固有取舍）；**别去手工清理正在跑的**，跑完再删无妨

---

### `_spawn(exe, args)` / `_kill(handle)`

**异步**拉起一个长驻进程并**保留句柄**，之后用句柄收尾（2026-10-07 新增）。

```leno
var h = _spawn("msedge.exe", "--headless=new --remote-debugging-port=0 --user-data-dir=…")
if h > 0 {
    // … 用它 …
    _kill(h)                 // 收掉整棵进程树
}
```

| 函数 | 参数 | 返回 |
| --- | --- | --- |
| `_spawn(exe, args)` | `exe` = 可执行文件；`args` = 参数串（原样拼在其后）| `Int` 句柄（**> 0**；失败 **-1**）|
| `_kill(handle)` | `_spawn` 返回的句柄 | `Bool`（true = 确实收了一个有效句柄）|

- `_spawn` **不等待**（拉起就返回）；`exe` 与 `args` 分成两个参数，是为了让"哪个是可执行文件"
  没有歧义（Job / 进程组都要认准那**一个**进程，再从命令行里解析一遍就多一次出错机会）。
- `_kill` 收的是**整棵进程树**（Windows `TerminateJobObject` / POSIX `kill(-pgid)`）；
  重复调用返回 `false`（幂等）。最多 256 个句柄，已退出的槽位会被自动回收。
- 句柄是不透明的**正整数**（0 / 负数一律无效）⇒ 结构体字段默认 `0` **不会**误伤某个槽位。

> ★ **父进程退出 ⇒ 子进程一并消失**（这是它与 `_exec("cmd /c start …")` 的本质区别）：
> Windows 侧子进程被放进带 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job Object ⇒ Leno 进程
> **正常退出 / 崩溃 / 被 `taskkill`**，内核都会把 job 内的进程收掉；Linux 侧用 `PR_SET_PDEATHSIG`。
> 实测（`assert/test_spawn.leno` 的 ③ 与一次 /F 强杀实验）：父进程被强杀后，"被拉起的进程"计数 1 → 0。

> ⚠ **`_spawn` 还是 `_exec`？**
> - 要**等结果**（跑条命令拿输出）⇒ `_exec`；
> - 拉起**长驻进程**（浏览器 / 后台服务）⇒ `_spawn` + `_kill`。
> 两者的 Job Object 语义**故意相反**：`_exec` **不**设那个标志（`cmd /c start` 拉起的应用必须活下来，
> 见 `assert/test_exec_timeout.leno` ④），`_spawn` **必须**设 —— 否则"孤儿进程"就是这么来的。

> ⚠ **POSIX 说明**：`_spawn` 经 `/bin/sh -c` 拉起（参数仍按 shell 规则解析）；"父死子死"是 Linux 的
> `PR_SET_PDEATHSIG`，macOS 没有对应机制 ⇒ 那边要**显式 `_kill`**（与 Playwright 改用 CDP 管道
> 解决同一问题的原因相同：管道一断，浏览器自己退）。

---

### `_single_instance([name])`

**单实例互斥**（2026-10-15 新增）：挡住"双击两次起了两个程序"。

**由来**：`leno -p` 打出的**单文件 exe**，双击两次会起两个 —— 托盘应用 / 清理工具这类当场打架
（两边同写一份配置与日志、托盘两个图标、两边抢同一台设备）。**任何系统都不会替我们判重**：
macOS 只对 **.app bundle** 由 Finder 保证单实例（双击第二次只是激活已运行的那个），而我们产出的
是**裸二进制** ⇒ 一样开两个；Linux 双击 .desktop 同理。所以要自己做。

```leno
main() {
    // 零配置：名字默认 = 本进程 exe 的绝对路径 ⇒ "同一个程序只许一个实例"
    if not _single_instance() {
        // 已经有实例在跑：把它的窗口顶到前台，然后自己退出
        //   ⚠ 这一句**不能省**：只有 `_single_instance` 的话，"双击第二次"会变成
        //     "闪一下就没"（比多开一个更让人困惑 ✗）
        _instance_activate()
        _exit(0)
    }
    // …真正的业务…
}
```

配套的 `_instance_activate([name])`（同一个 `name`，默认同样是本 exe 路径）：

| 返回 | 含义 |
| --- | --- |
| `true` | **找到了**持锁进程的可见顶层窗口，并请求过激活（`ShowWindow(SW_RESTORE)` + `SetForegroundWindow`）|
| `false` | 没找到 / 拿不到持锁进程的 pid / 平台做不到 ⇒ **不假装**（调用方可以就此静默退出 ✓）|

- Windows：pid 取自 pid 文件（`_single_instance` 拿到锁时写的）⇒ 用 `EnumWindows` **按 pid** 找窗口
  （**不**按窗口标题/类名找：标题会变、类名会撞 ✗）。
- POSIX：`wmctrl -lp` 按 pid 找窗口、`wmctrl -i -a <id>` 激活；**`wmctrl` 不在 ⇒ `false`**
  （X11 要连 X 服务器才能 `XRaiseWindow`，Wayland 下没有跨 WM 的做法 ⇒ 不装工具就只能如实说做不到）。
- ⚠ **自己持有锁**时调用它一律 `false`（不会有人需要"激活自己"）。
- ⚠ Windows 的前台切换受"前台窗口锁"限制 ⇒ 我们只能**请求**：返回 `true` 的含义是"找到了窗口并且
  请求过了"，**不代表**它一定成了前台。
- ⚠ 验证方式是**机器判定**（`assert/test_single_instance.leno` ⑤）：拿 notepad 当"有窗口的持锁进程"，
  调完再问 `Win32.foregroundWindowPid()` 是不是它 —— 不靠肉眼 ✓。
  （直接起一个"持锁的 leno 进程"验不了：控制台窗口属于 **conhost**，不在 leno 进程名下 ✗）

| 调用 | 含义 |
| --- | --- |
| `_single_instance()` | 名字 = **本进程 exe 的绝对路径**（同一个程序只许一个实例，零配置 ✓） |
| `_single_instance("LenoDumate")` | 显式名字（同一个 exe 想分多个身份时用：正式版 / 便携版各一份 ✓） |
| 返回 `true` | **本进程拿到了锁** ⇒ 它是第一个实例，继续跑 |
| 返回 `false` | **已经有实例在跑**（含**同一进程内**重复调同一个名字）⇒ 调用方该做的是"激活已有窗口 + 自己退出" |

- ★ **锁由内核持有**：Windows 是命名互斥体（`Local\...` ⇒ **每个登录会话一份**，多用户机器互不影响）；
  Linux/macOS 是 `flock` 锁住运行时目录里的一个文件（`$XDG_RUNTIME_DIR` 优先，否则 `$TMPDIR` 或 `/tmp`，
  文件名带 uid ⇒ 同样是"每个用户一份"）。
  ⇒ 进程**正常退出 / 崩溃 / 被 `taskkill`**，内核都会自动放锁，**不存在"上次崩了留下死锁、以后永远
  起不来"** —— 这正是选"内核锁"而不是"锁文件存不存在"那种做法的唯一理由（后者崩溃后要人工删文件，
  而且"谁先创建"没有原子性）。
- ⚠ **名字会净化**：Windows 的互斥体名不许含 `\`（那是 `Global\` / `Local\` 的命名空间分隔符）、
  POSIX 的文件名不许含 `/` ⇒ 其余非法字符一律换成 `_`；超过 120 字符会**截断并接一段哈希后缀**
  （否则两个长名字截断后会撞成同一把锁 ✗）。
  ⇒ 净化后同名的两个原始名字就是**同一把锁**（如 `A*B` 与 `A?B`）。
- ⚠ **大小写**：显式名字两端都**原样用**（`MyApp` 与 `myapp` 是两把锁 ✓ 跨平台一致）；
  只有"默认名（exe 路径）"在 Windows 侧折叠大小写（NTFS 路径大小写不敏感 ⇒ `D:\App\a.exe` 与
  `d:\app\A.EXE` 必须是同一把锁 ✓）。
- ⚠ **失败即放行**：句柄 / 锁文件建不出来（极少见）⇒ 返回 `true` 并往 stderr 打一行警告 ——
  宁可"可能多起一个"，也不要"用户双击了、程序却起不来"（单实例是锦上添花，不是运行前提）。
- 用例 `assert/test_single_instance.leno`（同进程重复 / 跨进程 / **持锁进程被杀后锁自动释放** ✓）。
  ⚠ 它就是个**全局内置函数**（与 `_pid` / `_executable` 同一族）⇒ 不要在模块里再包一层
  `Sys.xxx()` 的转发壳（纯转发没有信息量，只会多一处要同步的地方 ✗）。

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

    // 读取"可能未设置"的变量：取不到 / 空串 ⇒ 默认值（**总是字符串** ✓）
    print("APP_MODE(已清除):", _env_or("APP_MODE", "development"))
}
```

### 示例7：执行系统命令

```leno
main() {
    // 获取当前目录
    var cwd = ""
    if _os() == "windows" {
        cwd = _exec("cd").output
    } else {
        cwd = _exec("pwd").output
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
- `_executable()` = 解释器的**绝对路径**（如 `D:\CLeno\Leno\build\leno.exe`）
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
*变更: **v3.2.8 起 `_exec()` 返回 `ExecResult{ output, code }` 结构体**（此前是 `[stdout, exit_code]` 数组；
`r[0]`/`r[1]` ⇒ `r.output`/`r.code`）；`_args()` 只返回脚本参数*
