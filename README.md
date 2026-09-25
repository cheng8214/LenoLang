# Leno

Leno 是一门**带静态类型检查的脚本语言** —— 由 C 实现，编译为字节码在虚拟机上运行。
变量与类型一经确定不可更改；内置 GC、协程、多线程、FFI、包管理与字节码打包，
也能直接写跨平台 GUI（自带 SDL3 模块）。

Leno 诞生于对编程语言设计的热爱与探索，虽非完美，但乐在其中。

| | |
| --- | --- |
| 版本 | 1.0.0 |
| 许可 | MIT |
| 构建脚本 | Windows `build.bat` / Linux · macOS `build.sh` |
| 仓库规模 | 内置模块 20 个 · SDL3 源文件 46 个 · 测试 469 文件 · 示例 1016 文件 · 文档 67 篇 |

---

## 一分钟速览

> 下面这段**可以直接运行**（存为 `quick.leno`，执行 `leno quick.leno`）—— 注释里的输出是**实测结果**。

```leno
import maths

// 静态类型 + 类型推断
var name = "Leno"
const PI = 3.14

// 结构体 + 方法
struct Point {
    int x = 0
    int y = 0
    func dist():float { return maths.sqrt(x * x + y * y) }
}
var p = new Point(x = 3, y = 4)
print(p.dist())                       // 5.0

// 接口 + 多态
face Shape {
    func area():float
}
struct Circle impl Shape {
    int r = 0
    func area():float { return 3.14 * r * r }
}
var c = new Circle(r = 2)
print(c.area())                       // 12.56

// 多返回值 + 解构
func minMax(Array[int] arr): [int, int] {
    var lo = arr[0]
    var hi = arr[0]
    for 0 : arr.len() - 1 to i {          // 闭区间（含末项）
        if arr[i] < lo { lo = arr[i] }
        if arr[i] > hi { hi = arr[i] }
    }
    return lo, hi
}
var[int, int](lo, hi) = minMax([3, 1, 9, 7])
print(lo, hi)                         // 1 9

// 泛型 struct（类型参数沿调用链精准传递）
struct Result[T] {
    T data
    bool ok = false
}
func Ok[T](T val): Result[T] { return new Result[T](data = val, ok = true) }
print(Ok[int](42).data)               // 42

// 闭包
func make_counter(int start) {
    int count = start
    return func():int { count = count + 1; return count }
}
var next = make_counter(10)
print(next(), next())                 // 11 12

// 空安全 + 空值合并
string? maybe = null
print(maybe ?? "anonymous")           // anonymous
print(maybe?.len() ?? 0)              // 0

// 异常处理（e.msg 带定位、操作数类型值与调用栈）
try {
    var z = 1 / 0
} catch e {
    print("出错: " + e.msg)
}

// 链式集合操作
var result = [1, 2, 3, 4, 5]
    .filter(func(any x, any i) { return x % 2 == 0 })
    .map(func(any x, any i) { return x * 10 })
print(result)                         // [20, 40]

print("name = " + name)               // name = Leno
```

实际输出：

```text
5.0
12.56
1 9
42
11 12
anonymous
0
出错: 除零错误：除数为 0
  [操作] 除法 (/)
  [内部] DIV (/)
  [位置] quick.leno:62 (func: <main>)
  [操作数a] type=int, value=1
  [操作数b] type=int, value=0
  [调用栈]
    #0 <main> @ quick.leno:62
[20, 40]
name = Leno
```

---

## 快速开始

### 1. 构建

需要 GCC 或 MinGW（C99）。

```bash
build.bat          # Windows
chmod +x build.sh && ./build.sh    # Linux / macOS
```

产物：`build/leno`（编译器 + VM）。只想要 VM 运行时（体积更小、不含前端）：

```bash
build_vm.bat       # Windows → build/leno_vm.exe
```

### 2. Hello World

```leno
main() {
    print("Hello, Leno!")
}
```

```bash
build/leno hello.leno
```

### 3. 跑一个 GUI 应用

内置 SDL3 模块，仓库里就有可直接运行的应用（数据看板、文件管理器、扫雷、俄罗斯方块……）：

```bash
# 从仓库根目录
build\leno.exe leno_gui\应用\数据看板\dashboard.leno          # 多特性组合样板（布局/控件/Canvas 自绘 ✓）
build\leno.exe leno_gui\游戏\俄罗斯方块\tetris.leno
```

---

## 语言特性

**类型系统**
- **静态类型 + 类型推断** —— 显式声明或 `var` 自动推断，类型一旦确定不可更改
- **`const` 常量** —— 不可变绑定，声明时必须初始化
- **任意精度整数** —— `int` 无溢出，int48 内联 + BigInt 自动升级，对外透明
- **固定位宽转换** —— `_int32()` / `_int64()` / `_uint32()` / `_uint64()` / `_uint8()` / `_byte()`
- **`alias` 类型别名** —— 简单类型、`Array[T]`、`Dict[K,V]`、别名链
- **类型守卫与收窄** —— `is` 检查 + `if`/`switch` 块内自动收窄；索引收窄 `arr[0] is int`
- **`=>` 绑定语法** —— `if expr is Type => var` 一次求值、绑定、收窄；可 `and` 链组合
- **安全转换 `as`** —— 不匹配返回 `null` 而非崩溃
- **可空类型 `Type?`**

**结构体与面向对象**
- **`struct`** —— 字段、方法、嵌套、自引用
- **泛型 struct** —— `struct Stack[T]`、`struct Result[T]`
- **`self`** —— 方法内显式引用实例（`self` 可选，命名冲突时必备）
- **`face` 接口** —— 名义类型 + 显式 `impl`，编译期检查方法完整性，支持 `export face`
- **多 face 实现** —— `struct Key impl Comparable, Hashable { }`
- **泛型约束与泛型 face** —— `func maxBy[T: Comparable](...)`、`face Container[T] { }`
- **`enum`** —— 自增值或显式值，本质为 `int`
- **绑定方法引用** —— `return item.format`

**函数式**
- **一等函数 / 匿名函数 / IIFE / 闭包**（捕获外部变量，多实例独立状态）
- **泛型函数** —— `func map[T, U](Array[T] arr, func(T):U fn):Array[U]`
- **默认参数**、**多返回值**、**前向引用**

**解构声明**
- 数组 `var[int, int](a, b) = [10, 20]`、字典 `var{"host": string}(host) = config`
- 多返回值解构、部分解构（多余丢弃 / 不足补 `null`）、`const` 解构、`export` 解构

**模块与包**
- **`import`** —— 文件模块、内置模块、相对 / 绝对路径、中文与空格路径
- **`export`** —— 变量 / 函数 / struct / enum / face / alias 显式导出
- **`use` 类型导入与链式传导** —— `use module.Struct`，跨层自动传递
- **循环依赖支持**、**模块缓存（单例）**、**`.lenocache` 编译缓存**（含依赖一致性校验）
- **包管理** —— `leno.toml`、`leno --init`、`leno --install`

**并发**
- **多线程** —— `threads` 模块，`start()` / `join()` / Channel，线程间全局变量隔离
- **异步协程** —— `async` / `await` + 事件循环
- **Channel** —— 有缓冲 / 无缓冲，Go 风格 CSP

**底层能力**
- **FFI** —— 直接调用 C 动态库与系统 API，无需绑定层
- **CStruct** —— 声明式 C 结构体布局，`packed` / `align` 控制对齐
- **`&` 取地址**、**FFI 输出参数**（`&var` 或 `ffi.write_int`）

**异常**
- **`try-catch-finally`**，可嵌套、可 `throw` 重抛
- 异常对象带 `msg` / `file` / `stack` 完整诊断（含操作数类型与实际值）
- 子线程异常通过 `join()` 传播到调用方

**运算符**
- 安全访问 `?.`、空值合并 `??`、成员检查 `in` / `not in`
- 逻辑右移 `>>>`；位复合赋值 `&=` `|=` `^=` `<<=` `>>=` `>>>=`；复合赋值与 `++` / `--`
- 并行赋值 `a, b = b, a`

**内置容器与字符串**
- `Array[T]` / `Dict[K, V]`，`map` / `filter` / `reduce`
- 数组切片 `arr[2:8]`、`arr[:5]`、`arr[3:]`；字符串切片（UTF-8 字符级）
- 字符串插值 `$"Hello, {name}!"`、原始字符串 `@"..."`、`format("%02d: %-10s", i, name)`
- ⚠ **索引必须非负** —— `arr[-1]` 会抛索引越界（这是**有意设计**，见 `assert/test_string_edge.leno`）

**工程化**
- **字节码** —— 源码 → `.lenb` → 独立 exe（可 `--onefile` 全内嵌）
- **`--debug`** —— 输出全部字节码（主程序 + 所有模块）并带行号
- **GC** —— 内置垃圾回收，延迟回收不阻塞事件循环
- **LSP** —— `leno_lsp/` 语言服务器（补全 / 定义跳转 / 悬停）
- **`switch` 二分查找优化** —— 同类型 case ≥ 4 个时自动二分（int/float/string）

---

## GUI 生态（LenoSDL3）

`build/leno_module/LenoSDL3/` 是一套完整的跨平台 GUI 框架 —— 46 个 `sdl_*.leno`：
约 36 个控件（按钮 / 编辑框 / 表格 / 树 / 图表 / 标签页 / 折叠面板 / 滑块 / 日期…）
＋ 10 个基础设施（窗口 / 事件 / 批量渲染 / 字体与文本 / 主题 / 布局 / 拖拽 / 托盘…）。

```leno
import "SDL3" as SDL3
use SDL3.(Event, Renderer)

main() {
    // 窗口用 Dict 配置：title / w / h / flags…
    var win = SDL3.createWindow({title: "Hello LenoSDL3", w: 800, h: 600})
    if not win.ok { return }

    win.run(
        func(Event e): bool {                       // 返回 false 结束主循环
            if e.isQuit() or e.isWindowClose() { return false }
            return true
        },
        func(Renderer r) {                          // 每帧绘制（在控件之下）
            r.setColor(#141E32FF)
            r.clear()
        }
    )
}
```

完整可运行版本见 `build/leno_module/LenoSDL3/examples/基础示例/simple_window.leno`。

`leno_gui/` 里是跑起来的成品：

| 目录 | 内容 |
| --- | --- |
| `控件/` | 14 类控件演示：按钮 / 编辑框 / 标签 / 表格 / 布局 / 复合控件 / 画布 / 数值控件 / 图表 / 选择控件 / 总览 / **设置面板** / 系统（数据看板的两代旧版原型也在该目录 ✓） |
| `应用/` | 9 个完整应用：**数据看板**（布局 / 控件 / Canvas 自绘的组合样板 ✓）、模拟时钟、文件管理器、文件搜索、缓存清理工具、桌宠、IDE 布局、PE 分析器、Trae 签到 |
| `游戏/` | 6 个游戏：俄罗斯方块 / 飞机大战 / 扫雷 / 塔防 / 五子棋 / 植物大战僵尸 |
| `特效/` | 烟花、代码雨、水波涟漪（含 Python 基准对照） |
| `系统/` | 截图工具 + 全局热键 |

渲染上有一处细节值得说明：SDL 的 2D 图元管线**不做抗锯齿**，所以圆角矩形与斜线
改为「CPU 按覆盖率算好 alpha 写进纹理，GPU 只做采样」——平滑来自像素内容，不受光栅算法限制。

---

## 命令行

```bash
build/leno <file.leno>                # 运行源码
build/leno <file.lenb>                # 运行字节码
build/leno <file.leno> --list         # 文件之后的参数原样传给脚本（脚本内用 _args() 取）
build/leno -- <file>                  # 终止选项解析（文件名以 - 开头时用）
build/leno -c <file.leno>             # 编译为 .lenb（不执行）
build/leno -p <file.leno>             # 打包为独立可执行文件（嵌入 leno_vm）
build/leno -p -o release <file.leno>  # 指定打包输出目录（默认 <源码目录>/dist）
build/leno -p --onefile <file.leno>   # 单文件打包（原生库与 resources 内嵌）
build/leno --debug <file.leno>        # 输出全部字节码 + 行号
build/leno --init my-package          # 创建新包
build/leno --install                  # 安装当前项目依赖（leno.toml）
```

| 选项 | 说明 |
| --- | --- |
| `-h, --help` / `-v, --version` | 帮助 / 版本 |
| `--pause` | 执行完毕后暂停（双击运行时看输出） |
| `--debug` / `--debug-out <file>` | 输出字节码 / 输出到指定文件 |
| `-c, --compile` | 编译为 `.lenb`，不执行 |
| `-p, --pack` | 编译并打包（exe + 依赖原生库复制到同一目录） |
| `-o, --pack-dir <目录>` | 打包输出目录，默认 `<源码目录>/dist` |
| `--onefile` | 原生库与 `resource.toml [pack] resources` 声明的资源内嵌进 exe |
| `--console` / `--no-console` | 打包时强制控制台版 / 无控制台版 `leno_vm`（Windows；二者互斥） |
| `--init [路径]` | 创建新包项目 |
| `--install [路径\|git源]` | 安装包或依赖到全局缓存（如 `gitee:user/repo/pkg-a`） |
| `--` | 终止选项解析，其后参数都按位置参数处理 |
| `--no-cache` | 禁用模块编译缓存（⚠ 未列在 `--help` 输出里，但确实可用） |

## 运行测试

```bash
build\leno.exe assert\run_tests.leno              # 默认用 build\leno.exe 跑 assert/
build\leno.exe assert\run_tests.leno build\leno.exe assert   # 显式指定解释器与测试目录
```

## 包管理

```bash
leno --init my-package        # 生成 leno.toml / lib/ src/ native/ examples/ test/
leno --install my-package     # 安装本地包到全局缓存
leno --install gitee:user/my-package
leno --install                # 按 leno.toml 安装全部依赖
```

模块写法（`lib/my-package.leno`）：

```leno
export func hello() { print("Hello from my-package!") }
export var VERSION = "0.1.0"
export alias StrList = Array[string]
export struct Config { string host = "localhost"; int port = 8080 }

func _internal() { return "private" }   // 不加 export = 私有
```

```leno
import "my-package"

main() {
    my-package.hello()
    print(my-package.VERSION)
}
```

---

## 项目结构

```
Leno/
├── src/                        # 编译器 + VM（C 实现，89 个 .c）
│   ├── parser/ semantic/ codegen/ optimize/ serialize/
│   ├── vm/                     # 虚拟机
│   ├── module/                 # 20 个内置模块（io / maths / strings / ffi / threads / asyncs …）
│   ├── package/ platform/ object/ module_symbol_table/
│   └── gc.c  debug.c  module_compiler.c  module_loader.c  module_dispatch.c
├── build/                      # 构建产物
│   ├── leno.exe  leno_vm.exe  leno_vm_gui.exe
│   └── leno_module/            # ★ 扩展模块源码在这里（已入库）
│       ├── LenoSDL3/           #   跨平台 GUI（46 个 sdl_*.leno + docs/ + examples/）
│       ├── LenoWeb/  LenoSqlite/  LenoWin32/
│       └── LenoMusic/  LenoCrypto/  LenoHack/
├── leno_gui/                   # GUI 演示与应用（控件 14 类 / 应用 9 个 / 游戏 6 个 / 特效）
├── leno_lsp/                   # LSP 语言服务器（C 实现 + .vsix 插件）
├── assert/                     # 断言测试（469 个文件，入口 run_tests.leno）
├── examples/                   # 示例代码（1016 个文件）
├── docs/                       # 文档（67 篇）
├── resources/leno.rc           # Windows 资源（图标 / 版本信息）
├── sources_core.txt  sources_vm.txt  sources_compiler.txt   # 构建源文件清单
└── build.sh  build.bat  build_vm.sh  build_vm.bat
```

## 内置模块

| 模块 | 说明 | 模块 | 说明 |
| --- | --- | --- | --- |
| `io` | 输入输出 | `files` | 文件操作 |
| `maths` | 数学函数 | `dirs` | 目录操作 |
| `strings` | 字符串工具 | `jsons` | JSON 编解码 |
| `arrays` | 数组工具 | `sockets` | 网络套接字 |
| `dicts` | 字典工具 | `regexs` | 正则表达式 |
| `types` | 类型操作 | `sys` | 系统信息 |
| `times` | 时间日期 | `ffi` | 外部函数接口 |
| `rands` | 随机数 | `cstructs` | C 结构体 |
| `threads` | 多线程 | `asyncs` | 异步协程 |

（另有 `assert`（测试框架）与 `structs`（内部）两个模块。）

## 文档

**入门与指南**
- [Leno 入门教程](docs/Leno入门教程.md) —— 完整语法参考（含类型系统详解）
- [FAQ](docs/FAQ.md) · [Leno 语言规范草稿](docs/Leno_规范草稿.md) · [语言改进建议](docs/Leno语言改进建议.md)
- [Import 使用指南](docs/import使用指南.md) · [Async/Await 入门指南](docs/async_await入门指南.md)
- [FFI 使用指南](docs/FFI使用指南.md) · [Threads 使用指南](docs/threads使用指南.md) · [并发选择指引](docs/并发选择指引.md)
- [包管理与安装使用指南](docs/包管理与安装使用指南.md) · [单文件打包使用指南](docs/单文件打包使用指南.md)
- [加密算法示例指南](docs/加密算法示例指南.md)

**模块 API 参考**（`docs/module_*.md`）
- [io](docs/module_io.md) · [maths](docs/module_maths.md) · [strings](docs/module_strings.md) · [arrays](docs/module_arrays.md) · [dicts](docs/module_dicts.md)
- [types](docs/module_types.md) · [times](docs/module_times.md) · [rands](docs/module_rands.md) · [files](docs/module_files.md) · [dirs](docs/module_dirs.md)
- [jsons](docs/module_jsons.md) · [sockets](docs/module_sockets.md) · [regexs](docs/module_regexs.md) · [sys](docs/module_sys.md)
- [ffi](docs/module_ffi_api.md) · [cstructs](docs/module_cstructs.md) · [threads](docs/module_threads_api.md) · [asyncs](docs/module_asyncs.md)

**GUI（LenoSDL3）**
- [常用 API 与易踩坑速查](build/leno_module/LenoSDL3/docs/SDL3常用API与易踩坑速查.md) —— 布局 `{basis, grow}` 语义、绘制层先后、抗锯齿范围
- [控件优化清单](build/leno_module/LenoSDL3/docs/SDL3控件优化清单.md)
- [像素直写渲染优化](docs/LenoSDL3像素直写渲染优化.md) · [裁剪过多导致闪烁排查](docs/LenoSDL3_裁剪过多导致闪烁排查记录.md) · [Table 方法表断裂编译器 bug](docs/LenoSDL3_Table方法表断裂编译器bug排查记录.md)
- 回归用例：`build/leno_module/LenoSDL3/examples/图形绘制/`（抗锯齿、时钟）+ `其他测试/`（绘制顺序、布局 dump）

**性能与实现记录**
- [性能优化记录](docs/性能优化记录.md) · [性能测试总结（Leno vs Python）](docs/性能测试总结_Leno_vs_Python.md) · [字节码优化分析](docs/字节码优化分析.md)
- [寄存式与栈式的差异清单](docs/寄存式与栈式的差异清单.md) · [JIT 实现与调试记录](docs/JIT实现与调试记录.md) · [GC 与分配优化（待办）](docs/待办_GC与分配优化.md)

**路线图**
- [待办与路线图](docs/待办与路线图.md) · [易用性痛点（TraeSign 移植实录）](docs/待办_易用性痛点（TraeSign 移植实录）.md) · [单一事实来源与重复实现收敛](docs/待办_单一事实来源与重复实现收敛.md)

## 许可证

[MIT](LICENSE)
