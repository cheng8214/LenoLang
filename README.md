# Leno

Leno 是一门带静态类型检查的脚本语言，介于 Python（纯动态）和 Go（纯静态）之间。由 C 语言实现，编译为字节码在虚拟机上运行。

Leno 诞生于对编程语言设计的热爱与探索，虽非完美，但乐在其中。

## 一分钟速览

```leno
// 类型推断 + 静态检查
var name = "Leno"
int version = 1

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

// 类型别名
alias Size = int
alias IntList = Array[int]
Size s = 100
IntList arr = [1, 2, 3]

// 泛型函数 + 匿名函数
func map_int(Array[int] arr, func(var x):int fn):Array[int] {
    var result = []
    for arr to v { result.add(fn(v)) }
    return result
}
var doubled = map_int([1, 2, 3], func(var x):int { return x * 2 })

// 闭包
func make_counter(int start) {
    int count = start
    return func():int { count = count + 1; return count }
}

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
- **任意精度整数** — `int` 无溢出，int48 内联存储 + BigInt 自动升级，对外完全透明
- **`alias` 类型别名** — 支持简单类型、`Array[T]`、`Dict[K,V]`、别名链等复杂组合
- **类型守卫** — `is` 类型检查 + if/switch 块内自动类型收窄
- **安全类型转换** — `as` 操作符，不匹配返回 `null` 而非崩溃

**结构体与面向对象**
- **`struct` 结构体** — 字段、方法、嵌套、自引用，堪比轻量级 class
- **`self` 关键字** — 方法内显式引用当前实例，参数名冲突时必备
- **`face` 接口** — 名义类型，必须显式 `impl` 声明，编译期检查方法完整性
- **`enum` 枚举** — 自增值或显式值，本质为 `int`

**函数式编程**
- **一等函数** — 函数赋值给变量、作为参数传递、作为返回值
- **匿名函数 / Lambda** — `func(x) { return x * 2 }` 内联定义，支持 IIFE
- **闭包** — 捕获外部变量，状态保持，多实例独立
- **泛型函数** — `func map[T, U](Array[T] arr, func(T):U fn):Array[U]`
- **默认参数** — `func add(int x, int y = 10):int`

**模块系统**
- **`import` 导入** — 文件模块、内置包、相对路径、中文/空格路径
- **`export` 控制** — 变量/函数/struct/enum/face/alias 显式导出
- **`export alias`** — 模块导出类型别名，支持复杂类型和别名链
- **`use` 类型导入** — `use module.Struct` 导入 struct/face 到当前作用域
- **`use` 链式传导** — D→C→B→A 自动传递类型，无需每层重导出
- **循环依赖支持** — A↔B 相互导入，占位符机制防止无限递归
- **模块缓存 / 单例** — 同模块多次导入共享同一实例，状态全局一致
- **包管理** — `leno.toml` 配置、`leno --init` 创建、`leno --install` 安装

**并发编程**
- **多线程** — `threads` 模块，`start()`/`join()`/通道通信，线程间全局变量隔离
- **异步协程** — `async`/`await` + 事件循环，轻量并发
- **Channel 通道** — 有缓冲/无缓冲，Go 风格 CSP 模型

**底层能力**
- **FFI** — 直接调用 C 动态库和系统 API，无需绑定
- **CStruct** — 声明式定义 C 结构体布局，与 FFI 无缝配合

**异常处理**
- **`try-catch-finally`** — 完整异常机制，支持嵌套和 `throw` 重抛
- **异常对象** — `e.msg`/`e.file`/`e.stack` 完整诊断信息
- **线程异常传播** — 子线程异常通过 `join()` 传播到调用方

**其他**
- **泛型容器** — `Array[T]`、`Dict[K, V]` 内置泛型数组与字典，支持 `map`/`filter`/`reduce`
- **字符串插值** — `$"Hello, {name}!"` 内嵌表达式
- **格式化输出** — `format("%02d: %-10s", i, name)` C 风格格式控制
- **`as` 安全转换** — 不匹配返回 `null` 而非崩溃
- **`>>>` 逻辑右移** — 高位补 0，适合加密算法位运算
- **GUI 支持** — 内置窗口、绘图、图片、字体、事件系统
- **正则表达式** — `regexs` 模块
- **JSON** — `jsons` 模块编解码
- **垃圾回收** — 内置 GC，自动内存管理
- **字节码编译** — 源码 → `.lenb` 字节码 → 独立 exe 打包
- **跨平台** — Windows / Linux（guis 在 Linux 下可能有问题）；macOS 暂不支持

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

    // 链式集合操作
    var result = [1, 2, 3, 4, 5]
        .filter(func(var x, var i) { return x % 2 == 0 })
        .map(func(var x, var i) { return x * 10 })
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
```

## 构建

### 前置要求

- GCC 或 MinGW（C99 支持）
- CMake 3.16+（可选，推荐）

### 方式一：使用构建脚本

**Windows：**

```bash
build.bat
```

**Linux / macOS：**

```bash
chmod +x build.sh
./build.sh
```

构建产物在 `build/` 目录下：

| 文件 | 说明 |
|------|------|
| `build/leno` | 编译器 + VM（完整版） |
| `build/leno_vm` | 纯 VM 运行时（仅运行 `.lenb` 字节码） |
| `build/test_runner` | 测试运行器 |

### 方式二：使用 CMake

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows 上如果使用 MinGW：

```bash
cmake -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

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
# 使用测试运行器（当前 95 个测试全部通过）
build/test_runner build/leno assert

# 使用 CMake
cmake --build build --target test
```

## 命令参考

| 命令 | 说明 |
|------|------|
| `leno` | 显示帮助信息 |
| `leno <file.leno>` | 运行 Leno 源码 |
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
| `guis` | 图形界面 |
| `sys` | 系统信息 |
| `assert` | 断言测试 |

## 项目结构

```
LenoC/
├── src/
│   ├── main.c                  # 编译器入口
│   ├── lexer.c                 # 词法分析
│   ├── ast.c                   # AST 定义
│   ├── parser/                 # 语法解析
│   ├── semantic/               # 语义分析
│   ├── codegen/                # 字节码生成
│   ├── vm/                     # 虚拟机
│   ├── optimize/               # 优化器
│   ├── gc.c                    # 垃圾回收
│   ├── bigint.c                # 任意精度整数
│   ├── module/                 # 内置模块
│   ├── package/                # 包管理
│   ├── object/                 # 内置对象类型
│   └── platform/               # 平台相关
├── assert/                     # 测试用例
├── examples/                   # 示例代码
├── docs/                       # 文档
├── build.bat / build.sh        # 构建脚本
├── build_vm.bat / build_vm.sh  # VM 构建脚本
└── CMakeLists.txt              # CMake 配置
```

## 文档

- [Leno 入门教程](docs/Leno入门教程.md) — 语言完整语法参考
- [类型使用指南](docs/类型使用指南.md) — 类型系统详解
- [Struct 使用指南](docs/struct使用指南.md) — 结构体详解
- [Import 使用指南](docs/import使用指南.md) — 模块导入详解
- [For 循环使用指南](docs/for循环使用指南.md) — 循环语法
- [Async/Await 入门指南](docs/async_await入门指南.md) — 异步编程
- [FFI 使用指南](docs/FFI使用指南.md) — 外部函数接口
- [Maths 使用指南](docs/maths使用指南.md) — 数学模块
- [Threads 使用指南](docs/threads使用指南.md) — 多线程
- [包管理与安装使用指南](docs/包管理与安装使用指南.md) — 包管理详解
- [加密算法示例指南](docs/加密算法示例指南.md) — Base64 / AES / RSA / SHA 等纯 LenoC 实现

## 许可证

[MIT](LICENSE)
