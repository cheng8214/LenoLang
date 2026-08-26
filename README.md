# Leno

Leno 是一门带静态类型检查的脚本语言。由 C 语言实现，编译为字节码在虚拟机上运行。

Leno 诞生于对编程语言设计的热爱与探索，虽非完美，但乐在其中。

## 一分钟速览

```leno
// 类型推断 + 静态检查
var name = "Leno"
const PI = 3.14

// 结构体 + 方法
struct Point {
    int x = 0
    int y = 0
    func dist():float { return maths.sqrt(x * x + y * y) }
}
var p = new Point(x = 3, y = 4)
print(p.dist())    // 5.0

// 接口 + 多态
face Shape {
    func area():float
}
struct Circle impl Shape {
    int r = 0
    func area():float { return 3.14 * r * r }
}

// 多返回值 + 解构
func minMax(Array[int] arr): [int, int] {
    return arr[0], arr[-1]
}
var[int, int](lo, hi) = minMax([3, 1, 9, 7])
print(lo, hi)    // 1, 9

// 泛型 struct（类型参数运行时精准传递）
struct Result[T] {
    T data
    bool ok = false
}
func Ok[T](T val): Result[T] { return new Result[T](data=val, ok=true) }
var r = Ok[int](42)
print(r.data)     // 42

// 泛型约束 [T: Face]（编译期检查方法完整性）
face Comparable {
    func compare(Comparable other): int
}
func maxBy[T: Comparable](T a, T b): T {
    if a.compare(b) >= 0 { return a }
    return b
}

// 匿名函数 + 闭包
func make_counter(int start) {
    int count = start
    return func():int { count = count + 1; return count }
}

// 安全访问 + 空值合并
string? maybe_name = get_name()
var display = maybe_name ?? "anonymous"
var len = maybe_name?.len() ?? 0

// 异步协程
async func fetch():string {
    import asyncs
    await asyncs.sleep(100)
    return "done"
}

// 异常处理
try {
    var a = 1 / 0
} catch e {
    print("出错: " + e.msg)
}
```

## 特性

**类型系统**
- **静态类型 + 类型推断** — 显式声明或 `var` 自动推断，类型一旦确定不可更改
- **`const` 常量** — 不可变绑定，声明时必须初始化，支持类型推断和显式类型
- **任意精度整数** — `int` 无溢出，int48 内联存储 + BigInt 自动升级，对外完全透明
- **固定位宽转换** — `_int32()` / `_int64()` / `_uint32()` / `_uint64()` / `_uint8()` / `_byte()` 精确控制位宽
- **`alias` 类型别名** — 支持简单类型、`Array[T]`、`Dict[K,V]`、别名链等复杂组合
- **类型守卫** — `is` 类型检查 + if/switch 块内自动类型收窄
- **`=>` 绑定语法** — `if expr is Type => var` 一次性求值、绑定、类型收窄
- **索引类型收窄** — `arr[0] is int` 后续使用自动收窄，支持连续索引 `a[0]["k"] is Type`
- **`and` 链 + `=>` 绑定** — 多条件守卫组合，收窄传导到局部变量
- **安全类型转换** — `as` 操作符，不匹配返回 `null` 而非崩溃
- **可空类型** — `Type?` 语法，`int?` / `string?` / `Point?` 等

**结构体与面向对象**
- **`struct` 结构体** — 字段、方法、嵌套、自引用，堪比轻量级 class
- **泛型 struct** — `struct Stack[T]`、`struct Result[T]`，类型参数沿调用链精准传递
- **`self` 关键字** — 方法内显式引用当前实例，`self` 可选但冲突时必备
- **`face` 接口** — 名义类型，显式 `impl` 声明，编译期检查方法完整性，支持 `export face`
- **多 face 实现** — `struct Key impl Comparable, Hashable { }` 同时实现多个接口
- **泛型约束** — `func maxBy[T: Comparable](T a, T b): T`，T 必须实现指定 face
- **泛型 face** — `face Container[T] { func get(): T }` 接口级别泛型
- **绑定方法引用** — `return item.format` 返回方法的绑定引用
- **`enum` 枚举** — 自增值或显式值，本质为 `int`

**函数式编程**
- **一等函数** — 函数赋值给变量、作为参数传递、作为返回值
- **匿名函数 / Lambda** — `func(x) { return x * 2 }` 内联定义，支持 IIFE
- **闭包** — 捕获外部变量，状态保持，多实例独立
- **泛型函数** — `func map[T, U](Array[T] arr, func(T):U fn):Array[U]`
- **默认参数** — `func add(int x, int y = 10):int`
- **多返回值** — `func f(): [int, string] { return 42, "ok" }`
- **前向引用** — 函数可以先调用后定义

**解构声明**
- **数组解构** — `var[int, int](a, b) = [10, 20]` 按索引提取
- **字典解构** — `var{"host": string, "port": int}(host, port) = config` 按键名提取
- **多返回值解构** — `var[int, string](id, name) = get_user()`
- **部分解构** — 变量数可少于数据源（多余丢弃），也可多于（补 `null`）
- **`const` 解构** — `const[int, int](a, b) = data` 声明不可变绑定
- **`export` 解构** — 模块中解构变量可导出供其他模块使用

**模块系统**
- **`import` 导入** — 文件模块、内置包、相对路径、中文/空格路径
- **`export` 控制** — 变量/函数/struct/enum/face/alias 显式导出
- **`export alias`** — 模块导出类型别名，支持复杂类型和别名链
- **`use` 类型导入** — `use module.Struct` 导入 struct/face 到当前作用域
- **`use` 链式传导** — D→C→B→A 自动传递类型，无需每层重导出
- **循环依赖支持** — A↔B 相互导入，占位符机制防止无限递归
- **模块缓存 / 单例** — 同模块多次导入共享同一实例，状态全局一致
- **编译缓存** — `.lenocache` 磁盘缓存，模块编译结果跨运行复用，含依赖一致性校验
- **包管理** — `leno.toml` 配置、`leno --init` 创建、`leno --install` 安装

**并发编程**
- **多线程** — `threads` 模块，`start()`/`join()`/通道通信，线程间全局变量隔离
- **异步协程** — `async`/`await` + 事件循环，轻量并发
- **Channel 通道** — 有缓冲/无缓冲，Go 风格 CSP 模型

**底层能力**
- **FFI** — 直接调用 C 动态库和系统 API，无需绑定
- **CStruct** — 声明式定义 C 结构体布局，`packed`/`align` 属性控制对齐
- **`&` 取地址** — 获取变量地址用于 FFI 输出参数
- **FFI 输出参数** — `&var` 引用传递，或手动 `ffi.write_int` 两种写法

**异常处理**
- **`try-catch-finally`** — 完整异常机制，支持嵌套和 `throw` 重抛
- **异常对象** — `e.msg`/`e.file`/`e.stack` 完整诊断信息
- **线程异常传播** — 子线程异常通过 `join()` 传播到调用方

**运算符**
- **安全访问 `?.`** — `obj?.field`、`obj?.method()` 空安全链式调用
- **空值合并 `??`** — `expr ?? default` 左侧为 null 时取右侧默认值
- **成员检查 `in` / `not in`** — 数组、字典、子串检查
- **逻辑右移 `>>>`** — 高位补 0，适合加密算法
- **位复合赋值** — `&=`, `|=`, `^=`, `<<=`, `>>=`, `>>>=`
- **复合赋值** — `+=`, `-=`, `*=`, `/=`, `%=`
- **自增自减** — `++`, `--`

**其他**
- **泛型容器** — `Array[T]`、`Dict[K, V]` 内置泛型数组与字典，支持 `map`/`filter`/`reduce`
- **数组切片** — `arr[2:8]`、`arr[:5]`、`arr[3:]`
- **字符串切片** — `s[2:8]`、`s[:5]`（UTF-8 字符级）
- **字符串插值** — `$"Hello, {name}!"` 内嵌表达式
- **原始字符串** — `@"raw string"` 不转义
- **格式化输出** — `format("%02d: %-10s", i, name)` C 风格格式控制
- **并行赋值** — `a, b = b, a` 交换变量
- **switch 二分查找优化** — 同类型 case ≥ 4 个时自动二分查找（int/float/string），13x 加速
- **正则表达式** — `regexs` 模块
- **JSON** — `jsons` 模块编解码
- **垃圾回收** — 内置 GC，自动内存管理，延迟回收不阻塞事件循环
- **字节码编译** — 源码 → `.lenb` 字节码 → 独立 exe 打包
- **`--debug` 调试** — 编译完成后统一输出全部字节码（主程序 + 所有模块），行号对齐
- **LSP 语言服务器** — 代码补全、定义跳转、悬停提示
- **跨平台** — Windows / Linux / macOS（通过 GitHub Actions 自动构建验证）

## 快速开始

### Hello World

创建 `hello.leno`：

```leno
main() {
    print("Hello, Leno!")
}
```

运行：

```bash
leno hello.leno
```

### 更多示例

```leno
main() {
    // 类型推断
    var name = "Leno"
    var features = ["静态类型", "协程", "FFI", "模块系统"]

    // 结构体（命名参数构造）
    var p = new Point(x = 3, y = 4)
    print("距离 = " + p.dist())

    // 多返回值 + 解构
    var[int, int](lo, hi) = minMax([3, 1, 9, 7])
    print(lo, hi)  // 1, 9

    // 安全访问
    string? name2 = get_name()
    var display = name2 ?? "anonymous"

    // 链式集合操作
    var result = [1, 2, 3, 4, 5]
        .filter(func(any x, any i) { return x % 2 == 0 })
        .map(func(any x, any i) { return x * 10 })
    print(result)  // [20, 40]

    // 异常处理
    try {
        var val = 1 / 0
    } catch e {
        print("出错: " + e.msg)
    }
}

struct Point {
    int x = 0
    int y = 0
    func dist():float { return maths.sqrt(x * x + y * y) }
}

func minMax(Array[int] arr): [int, int] {
    return arr[0], arr[-1]
}

func get_name(): string? {
    return null
}
```

## 构建

### 前置要求

- GCC 或 MinGW（C99 支持）

### 使用构建脚本

**Windows：**

```bash
build.bat
```

**Linux / macOS：**

```bash
chmod +x build.sh
./build.sh
```

构建产物 `build/leno`（编译器 + VM）。

### 构建 VM 运行时（仅 Windows）

```bash
build_vm.bat
```

生成 `build/leno_vm.exe`，只包含 VM 运行时，不含编译器前端，体积更小。

## 运行

```bash
# 运行 .leno 源码文件
leno hello.leno

# 运行 .lenb 字节码文件（使用 VM 运行时）
leno_vm hello.lenb

# 调试模式（输出全部字节码 + 行号）
leno --debug hello.leno

# 字节码输出到文件
leno --debug-out bytecode.txt hello.leno

# 不带参数显示帮助信息
leno
```

## 编译为字节码

Leno 支持将源码编译为 `.lenb` 字节码文件，然后使用 `leno_vm` 运行：

```bash
# 编译为字节码
leno -c hello.leno

# 运行字节码
leno_vm hello.lenb

# 打包为独立 exe
leno -p hello.leno
```

## 打包与包管理

### 创建包

```bash
leno --init my-package
```

自动生成项目结构：

```
my-package/
├── leno.toml              # 包配置
├── lib/
│   └── my-package.leno    # 模块代码
├── src/
│   └── main.leno          # 入口文件
├── native/                # 原生库
├── examples/              # 示例
└── test/                  # 测试
```

### 编写模块

```leno
// lib/my-package.leno
export func hello() {
    print("Hello from my-package!")
}

export var VERSION = "0.1.0"

// 导出类型别名
export alias UserID = int
export alias StrList = Array[string]

// 导出 struct
export struct Config {
    string host = "localhost"
    int port = 8080
}

// 不加 export = 私有，外部不可见
func _internal() {
    return "private"
}
```

### 安装包

```bash
# 安装本地包到全局缓存
leno --install my-package

# 从 Git 仓库安装
leno --install gitee:user/my-package

# 安装项目所有依赖（读取 leno.toml）
leno --install
```

### 使用包

```leno
import "my-package"

main() {
    my-package.hello()
    print(my-package.VERSION)
}
```

### leno.toml 配置

```toml
[package]
name = "my-package"
version = "0.1.0"
description = "A Leno package"
license = "MIT"

[dependencies]
http_client = "~1.2.0"

[dependency-sources]
http_client = "gitee:user/http-client"

[modules]
root = "lib"
```

## 运行测试

```bash
# 运行断言测试
build/leno assert/run_tests.leno build/leno assert
```

## 命令参考

| 命令 | 说明 |
|------|------|
| `leno` | 显示帮助信息 |
| `leno <file.leno>` | 运行 Leno 源码 |
| `leno --debug <file.leno>` | 调试模式（输出全部字节码 + 行号） |
| `leno --debug-out <file> <file.leno>` | 字节码输出到指定文件 |
| `leno --no-cache <file.leno>` | 禁用模块编译缓存 |
| `leno -c <file.leno>` | 编译为 `.lenb` 字节码 |
| `leno -p <file.leno>` | 打包为独立 exe |
| `leno --init [name]` | 创建包项目 |
| `leno --install [path]` | 安装包到全局缓存 |
| `leno --install` | 安装当前项目所有依赖 |
| `leno_vm <file.lenb>` | 运行字节码文件 |

## 内置模块

| 模块 | 说明 |
|------|------|
| `io` | 输入输出 |
| `maths` | 数学函数 |
| `arrays` | 数组工具 |
| `strings` | 字符串工具 |
| `dicts` | 字典工具 |
| `types` | 类型操作 |
| `times` | 时间日期 |
| `rands` | 随机数 |
| `files` | 文件操作 |
| `dirs` | 目录操作 |
| `jsons` | JSON 编解码 |
| `sockets` | 网络套接字 |
| `ffi` | 外部函数接口 |
| `cstructs` | C 结构体 |
| `threads` | 多线程 |
| `asyncs` | 异步协程 |
| `regexs` | 正则表达式 |
| `sys` | 系统信息 |
| `assert` | 断言测试 |

## 项目结构

```
LenoC/
├── src/
│   ├── main.c                  # 编译器入口
│   ├── module_compiler.c       # 模块编译器
│   ├── module_loader.c         # 模块加载器（含缓存）
│   ├── module_dispatch.c       # 模块分发
│   ├── parser/                 # 语法解析
│   ├── semantic/               # 语义分析
│   ├── codegen/                # 字节码生成
│   ├── vm/                     # 虚拟机
│   ├── optimize/               # 优化器
│   ├── serialize/              # 序列化/反序列化
│   ├── gc.c                    # 垃圾回收
│   ├── debug.c                 # 字节码反汇编
│   ├── module/                 # 内置模块
│   ├── module_symbol_table/    # 模块符号表
│   ├── package/                # 包管理
│   ├── object/                 # 内置对象类型
│   └── platform/               # 平台相关
├── leno_lsp/                   # LSP 语言服务器
├── leno_module/                # 内置扩展模块
│   ├── LenoSDL3/               # SDL3 GUI 框架
│   ├── LenoWeb/                # Web 模块
│   ├── LenoSqlite/             # SQLite 模块
│   ├── LenoWin32/              # Win32 API 模块
│   └── LenoMusic/              # 音乐模块
├── assert/                     # 测试用例
├── examples/                   # 示例代码
├── docs/                       # 文档
├── build.bat / build.sh        # 构建脚本
└── build_vm.bat / build_vm.sh  # VM 构建脚本
```

## 文档

- [Leno 入门教程](docs/Leno入门教程.md) — 语言完整语法参考（含类型系统详解）
- [Import 使用指南](docs/import使用指南.md) — 模块导入详解
- [Async/Await 入门指南](docs/async_await入门指南.md) — 异步编程
- [FFI 使用指南](docs/FFI使用指南.md) — 外部函数接口
- [Threads 使用指南](docs/threads使用指南.md) — 多线程
- [包管理与安装使用指南](docs/包管理与安装使用指南.md) — 包管理详解
- [加密算法示例指南](docs/加密算法示例指南.md) — Base64 / AES / RSA / SHA 等纯 LenoC 实现
- [性能优化记录](docs/性能优化记录.md) — 性能优化历史

## 许可证

[MIT](LICENSE)
