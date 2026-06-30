# LenoC Import 使用指南

## 快速入门

### 基本语法

```leno
// 导入模块并使用默认名称（文件名）
import "math.leno"
math.add(1, 2)  // 使用模块名访问

// 导入并指定别名
import "string_utils.leno" as str
str.trim("  hello  ")  // 使用别名访问

// 从子目录导入
import "utils/array_helper.leno" as arr

// 使用 use 导入类型（支持 struct、clib、face）
import "point.leno" as pt
use pt.Point      // 将 Point 类型导入当前作用域
Point p = new pt.Point()  // 现在可以直接使用 Point 类型
```

***

## 1. 导入方式详解

### 1.1 基本导入

```leno
// 同一目录下的模块
import "module.leno"

// 子目录中的模块
import "subfolder/module.leno"

// 上级目录的模块
import "../parent_module.leno"
```

### 1.2 使用别名

当模块名冲突或太长时，使用 `as` 指定别名：

```leno
import "folder1/utils.leno" as utils1
import "folder2/utils.leno" as utils2

main() {
    print(utils1.get_info())  // 使用别名区分
    print(utils2.get_info())
}
```

### 1.3 特殊字符路径

LenoC 支持中文路径、空格路径等特殊字符：

```leno
// 中文路径
import "中文文件夹/中文模块.leno" as cn

// 空格路径（需要引号）
import "folder with spaces/space module.leno" as sp

// 横线路径
import "folder-with-dashes/dash-module.leno" as dash
```

***

## 2. 模块导出规则

### 2.1 显式导出

LenoC 模块使用 `export` 关键字**显式导出**需要暴露的顶层定义：

```leno
// math.leno
export func add(int a, int b):int {
    return a + b
}

export var PI = 3.14159

export struct Point {
    int x = 0
    int y = 0
}
```

导入后可以直接使用：

```leno
import "math.leno" as math

var p = new math.Point()      // 使用导出的 struct
var sum = math.add(1, 2)      // 使用导出的函数
var pi = math.PI              // 使用导出的变量
```

### 2.2 私有成员

**没有 `export` 关键字的标识符不会导出**：

```leno
// module.leno
export var public_var = 42    // ✅ 导出
var _private_var = 100        // ❌ 不导出（无 export）

export func public_func() { } // ✅ 导出
func _helper() { }            // ❌ 不导出（无 export）
```

### 2.3 导出控制建议

```leno
// ✅ 好的做法：显式控制导出
export var public_api = "可用"
var _internal_state = 0       // 内部状态不暴露

export func public_function() {  // 对外接口
    _helper()
}

func _helper() {              // 内部辅助函数
    // ...
}
```

***

## 3. 模块使用模式

### 3.1 命名空间模式

使用别名创建命名空间，避免命名冲突：

```leno
import "core/math.leno" as math
import "core/string.leno" as str
import "core/array.leno" as arr

main() {
    var nums = [1, 2, 3]
    var sum = math.add(nums[0], nums[1])
    var msg = str.format("Sum: {0}", sum)
    arr.add(nums, 4)
}
```

### 3.2 选择性导入

LenoC 目前不支持 ES6 式的选择性导入，但可以通过别名实现类似效果：

```leno
// 只使用需要的功能
import "math.leno" as m

func calculate(int x):int {
    return m.add(x, 10)
}
```

### 3.3 重导出（Re-export）

模块可以聚合其他模块的导出：

```leno
// core.leno - 核心模块聚合
import "math.leno" as math
import "string.leno" as str
import "array.leno" as arr

// 重导出变量
export var math_add = math.add
export var PI = math.PI

// 包装后导出
export func enhanced_add(int a, int b):int {
    print("计算中...")
    return math.add(a, b)
}
```

使用聚合模块：

```leno
import "core.leno" as core
core.enhanced_add(1, 2)
```

### 3.4 导出类型别名（export alias）

模块可以通过 `export alias` 导出类型别名，让调用方直接使用别名类型：

```leno
// types.leno
export alias Size = int
export alias IntList = Array[int]
export alias StrDict = Dict[string, string]
```

```leno
// main.leno
import "types.leno" as t

main() {
    Size w = 100              // Size 被识别为 int
    IntList arr = [1, 2, 3]   // IntList 被识别为 Array[int]
    StrDict d = {"a": "hi"}   // StrDict 被识别为 Dict[string,string]
}
```

支持的复杂类型：`Array[T]`、`Dict[K,V]`、自定义 struct 名等。

**别名链**——`export alias` 可以引用同模块内的其他别名：

```leno
// types.leno
export alias A = int
export alias B = A          // B → A → int

// main.leno
import "types.leno" as t
B val = 42                  // B 最终解析为 int
```

### 3.5 导出 face 类型

模块可以导出 `face` 定义，供调用方实现或作为参数类型：

```leno
// shape.leno
export face Shape {
    func area():float
    func name():string
}

export struct Circle impl Shape {
    int r = 0
    func area():float { return 3.14 * r * r }
    func name():string { return "Circle" }
}
```

```leno
// main.leno
import "shape.leno" as sm
use sm.Shape

func print_info(Shape s) {
    print(s.name() + " 面积=" + s.area())
}

main() {
    var c = new sm.Circle(r = 3)
    print_info(c)
}
```

***

## 4. 循环依赖处理

### 4.1 循环依赖支持

LenoC **支持模块间的循环依赖**：

```leno
// module_a.leno
import "module_b.leno" as mb

var a_value = 100

func a_func():string {
    return "A calls B: " + mb.b_value
}
```

```leno
// module_b.leno
import "module_a.leno" as ma

var b_value = 200

func b_func():string {
    return "B calls A: " + ma.a_value
}
```

### 4.2 循环依赖注意事项

```leno
// ⚠️ 避免在模块初始化时相互调用

// module_a.leno - 危险！
import "module_b.leno" as mb
var a_value = mb.b_value  // 可能导致未定义行为

// ✅ 安全的做法：通过函数延迟访问
import "module_b.leno" as mb

var a_value = 100

func get_b_value():int {
    return mb.b_value  // 运行时调用，安全
}
```

### 4.3 循环依赖最佳实践

```leno
// ✅ 好的设计：单向依赖优先
// 如果可能，重构为单向依赖

// utils.leno - 基础工具（无依赖）
func helper() { }

// module_a.leno - 依赖 utils
import "utils.leno"

// module_b.leno - 依赖 utils 和 module_a
import "utils.leno"
import "module_a.leno"
```

***

## 5. 模块缓存机制

### 5.1 单例模式

LenoC 模块是**单例**的，多次导入返回同一实例：

```leno
// counter.leno
var count = 0

func increment():int {
    count = count + 1
    return count
}

func get_count():int {
    return count
}
```

```leno
// main.leno
import "counter.leno" as c1
import "counter.leno" as c2  // 同一个模块！

main() {
    c1.increment()  // count = 1
    c1.increment()  // count = 2
    print(c2.get_count())  // 输出: 2（共享状态）
}
```

### 5.2 利用缓存实现状态共享

```leno
// config.leno - 全局配置
var settings = {
    debug: false,
    timeout: 30
}

func set_debug(bool value) {
    settings["debug"] = value
}

func get_config():Dict {
    return settings
}
```

```leno
// module_a.leno
import "config.leno" as cfg
cfg.set_debug(true)

// module_b.leno
import "config.leno" as cfg
// 可以看到 module_a 的修改
print(cfg.get_config()["debug"])  // true
```

***

## 6. 错误处理

### 6.1 导入错误类型

| 错误类型 | 示例 | 处理方式 |
|---------|------|---------|
| 模块不存在 | `import "not_exist.leno"` | 编译错误 |
| 语法错误 | 模块内有语法错误 | 编译错误，指出位置 |
| 运行时错误 | 模块初始化失败 | 运行时异常 |
| 循环依赖错误 | 初始化时循环调用 | 可能导致栈溢出 |

### 6.2 防御性导入

```leno
// 使用 try-catch 处理导入错误
try {
    import "optional_module.leno" as opt
    opt.do_something()
} catch e {
    print("可选模块加载失败，使用默认行为")
    // 使用默认实现
}
```

### 6.3 模块版本检查

```leno
// version.leno
var VERSION = "1.2.3"

func check_version(string required):bool {
    // 简单的版本比较
    return VERSION == required
}
```

```leno
import "version.leno" as ver

main() {
    if not ver.check_version("1.2.3") {
        print("警告：版本不匹配")
    }
}
```

***

## 7. 高级用法

### 7.1 动态模块选择

```leno
// 根据条件选择不同实现
func get_database_module(bool use_mysql) {
    if use_mysql {
        import "db/mysql.leno" as db
        return db
    } else {
        import "db/sqlite.leno" as db
        return db
    }
}
```

### 7.2 插件系统实现

```leno
// plugin_manager.leno
var plugins = []

func register_plugin(string path) {
    // 动态加载插件
    import path as plugin
    plugins.add(plugin)
}

func run_all_plugins() {
    for plugins to p {
        p.run()
    }
}
```

### 7.3 模块装饰器模式

```leno
// logging_decorator.leno
import "target_module.leno" as target

func logged_add(int a, int b):int {
    print("调用 add({0}, {1})".format(a, b))
    var result = target.add(a, b)
    print("返回: " + result)
    return result
}

// 导出增强版本
var add = logged_add
```

***

## 8. 跨模块类型使用

### 8.1 导出 Struct

```leno
// point.leno
struct Point {
    int x = 0
    int y = 0
    
    func move(int dx, int dy) {
        x = x + dx
        y = y + dy
    }
}
```

```leno
// main.leno
import "point.leno" as pt

main() {
    var p = new pt.Point()  // 使用导入的 struct
    p.move(10, 20)
    print(p.x)  // 10
}
```

### 8.2 使用 use 语句导入类型

当需要在当前作用域中使用模块的 struct 类型进行变量声明时，可以使用 `use` 语句：

```leno
// shape.leno
export struct Point {
    int x = 0
    int y = 0
}

export struct Circle {
    float radius = 1.0
}
```

```leno
// main.leno
import "shape.leno" as shape
use shape.Point      // 将 Point 类型导入当前作用域

main() {
    // ✅ 现在可以直接使用 Point 类型
    Point p = new shape.Point()
    print(p.x)
    
    // ❌ 没有 use Circle，不能直接使用 Circle 类型
    // Circle c = new shape.Circle()  // 错误：Circle 未定义
    
    // ✅ 但可以通过模块名访问
    var c = new shape.Circle()
}
```

**use 语句规则：**

| 用法 | 支持 | 说明 |
|------|------|------|
| `use module.Struct` | ✅ | 导入 struct 类型到当前作用域 |
| `use module.Clib` | ✅ | 导入 clib 类型到当前作用域（需模块有函数返回该 clib 类型） |
| `use module.Face` | ✅ | 导入 face 类型到当前作用域 |
| `use module.func` | ❌ | 函数必须通过模块名访问 |
| `use module.var` | ❌ | 变量必须通过模块名访问 |
| `use module.enum` | ❌ | enum 成员通过模块名访问（如 `module.enum.member`） |

**为什么 use 只支持 struct/clib/face？**

- **struct**：编译时类型，需要用于变量声明（如 `Point p`）
- **clib**：FFI 库类型，需要用于变量声明（如 `core_lib c = module.loadCore()`）
- **face**：编译时类型，需要用于函数参数（如 `func printArea(Shape s)`）
- **func/var**：运行时实体，必须通过模块名访问（如 `module.func()`）
- **enum**：成员访问本身就是通过模块名（如 `module.enum.member`）

### 8.3 use 类型链式传导

`use` 的类型可以通过模块链自动传递，无需手动重导出：

```leno
// d.leno - 最底层定义类型
export struct Vec3 {
    int x = 0
    int y = 0
    int z = 0
}

// c.leno - use d 的类型
import "d.leno" as d
use d.Vec3

// b.leno - use c 中传递的类型
import "c.leno" as c
use c.Vec3

// a.leno - use b 中传递的类型
import "b.leno" as b
use b.Vec3
```

```leno
// main.leno - 最终使用端
import "a.leno" as a
use a.Vec3

main() {
    Vec3 v = new a.Vec3(x = 1, y = 2, z = 3)
    print(v.x)    // 1
}
```

**多个模块的类型可以同时 use**：

```leno
import "point_mod.leno" as pm
import "color_mod.leno" as cm
use pm.Point
use cm.Color

main() {
    Point p = new pm.Point(x = 10, y = 20)
    Color c = new cm.Color(r = 255, g = 128, b = 0)
}
```

### 8.4 use 类型作为 export func 返回类型

`use` 导入的 struct/face 类型可以直接作为 `export func` 的返回类型，调用方能正确推断返回值类型：

```leno
// item.leno - 底层模块定义 struct
export struct Item {
    int id = 0
    string label = ""

    func describe(): string {
        return "Item(" + id + ":" + label + ")"
    }
}

export func makeItem(int id, string label): Item {
    return new Item(id = id, label = label)
}
```

```leno
// wrapper.leno - 中间层通过 use 导入 Item 并作为返回类型
import "item.leno" as item
use item.Item

export func createItem(int id, string label): Item {
    return item.makeItem(id, label)
}
```

```leno
// main.leno - 调用方正确推断返回类型为 Item
import "wrapper.leno" as wrapper

main() {
    var it = wrapper.createItem(1, "hello")   // it 被推断为 Item，不是 any
    print(it.describe())                       // ✅ 可以直接调用 Item 的方法
}
```

**face 类型同样支持**：

```leno
// base.leno
export face Describable {
    func describe(): string
}

export struct Item impl Describable {
    int id = 0
    func describe(): string { return "Item(" + id + ")" }
}

export func makeItem(int id): Item {
    return new Item(id = id)
}
```

```leno
// mid.leno - use 导入 face 并作为返回类型
import "base.leno" as base
use base.Describable

export func getDescribable(): Describable {
    return base.makeItem(42)
}
```

```leno
// main.leno
import "mid.leno" as mid
use mid.Describable

main() {
    var d = mid.getDescribable()    // d 被推断为 Describable（face 类型）
    print(d.describe())             // ✅ 可以调用 face 定义的方法
}
```

### 8.5 use face 类型

```leno
// shape_mod.leno
export face Shape {
    func area():float
    func name():string
}

export struct Circle impl Shape {
    int r = 0
    func area():float { return 3.14 * r * r }
    func name():string { return "Circle" }
}
```

```leno
// main.leno
import "shape_mod.leno" as sm
use sm.Shape

// use 之后 face 可直接作为参数类型
func print_info(Shape s) {
    print(s.name() + " 面积=" + s.area())
}

main() {
    var c = new sm.Circle(r = 2)
    print_info(c)
}
```

### 8.6 clib 类型跨模块使用

`use` 也支持导入其他模块定义的 clib 类型：

```leno
// sdl_base.leno - 底层模块定义 clib
import ffi

clib renderer_lib {
    i32 do_something(i32 x)
}

renderer_lib g_renderer = null

export func loadRenderer(): renderer_lib {
    if g_renderer == null {
        g_renderer = ffi.load("mylib.dll")
    }
    return g_renderer
}
```

```leno
// sdl_wrapper.leno - 中间层 use 底层 clib 并重新导出
import "sdl_base.leno" as base
use base.renderer_lib

export func getRenderer(): renderer_lib {
    return base.loadRenderer()
}
```

```leno
// main.leno - 最终使用端，只需感知中间层
import "sdl_wrapper.leno" as sdl
use sdl.renderer_lib

main() {
    // ✅ clib 类型正确推断
    renderer_lib r = sdl.getRenderer()
    var result = r.do_something(42) as int  // ⚠️ C返回类型需 as int 转换
}

```

> **⚠️ 重要**：clib 函数返回的是 C 类型（`i32`、`i64`、`f32` 等），参与 Leno 运算时**必须用 `as int` / `as float` 显式转换**。

**clib 类型可以用于 struct 字段**：

```leno
import "sdl_base.leno" as base
use base.renderer_lib

struct RendererHolder {
    renderer_lib renderer = null
    string       name     = ""
}

main() {
    var h = new RendererHolder(renderer = base.loadRenderer(), name = "main")
    var v = h.renderer.do_something(10) as int
}
```

### 8.7 类型守卫与跨模块类型

```leno
// shape.leno
struct Circle {
    float radius = 1.0
}

struct Rectangle {
    float width = 1.0
    float height = 1.0
}
```

```leno
// main.leno
import "shape.leno" as shape

func process(var obj) {
    if obj is shape.Circle {
        print("圆，半径: " + obj.radius)
    } else if obj is shape.Rectangle {
        print("矩形，面积: " + (obj.width * obj.height))
    }
}
```

***

## 9. 性能考虑

### 9.1 导入开销

- **首次导入**：编译并执行模块代码
- **后续导入**：从缓存读取，开销极小
- **建议**：大胆使用模块，不必担心性能

### 9.2 大型项目组织

```
project/
├── main.leno           # 入口文件
├── core/               # 核心模块
│   ├── config.leno
│   ├── logger.leno
│   └── utils.leno
├── modules/            # 功能模块
│   ├── user/
│   │   ├── model.leno
│   │   └── service.leno
│   └── order/
│       ├── model.leno
│       └── service.leno
└── vendor/             # 第三方模块
    └── third_party.leno
```

### 9.3 避免过度模块化

```leno
// ❌ 过度拆分
import "math/add.leno"
import "math/sub.leno"
import "math/mul.leno"

// ✅ 合理聚合
import "math.leno"
```

***

## 10. 最佳实践总结

### ✅ 推荐做法

1. **使用有意义的别名**
   ```leno
   import "string_utilities.leno" as str  // 简洁明了
   ```

2. **控制导出范围**
   ```leno
   var _internal = 0  // 不导出内部状态
   var public_api = {}  // 只导出必要的
   ```

3. **避免循环依赖**
   ```leno
   // 优先单向依赖，循环依赖作为最后手段
   ```

4. **利用模块缓存**
   ```leno
   // 需要共享状态时使用模块级变量
   ```

5. **版本管理**
   ```leno
   // 在模块中导出版本信息
   var VERSION = "1.0.0"
   ```

### ❌ 避免的做法

1. **运行时动态构造导入路径**
   ```leno
   // 不推荐
   import ("module_" + suffix + ".leno")
   ```

2. **模块间紧密耦合**
   ```leno
   // 避免模块 A 依赖 B，B 依赖 C，C 又依赖 A
   ```

3. **在模块初始化时执行复杂逻辑**
   ```leno
   // 避免在模块加载时做大量计算
   var result = heavy_computation()  // 不推荐
   ```

***

## 11. 常见问题

### Q: 如何查看模块导出了什么？

A: 目前 LenoC 没有内省功能，建议查看模块源码或文档。

### Q: 模块可以热重载吗？

A: 目前不支持，模块在程序启动时加载并缓存。

### Q: 如何处理模块版本冲突？

A: 使用不同别名导入不同版本：
```leno
import "lib/v1/api.leno" as api_v1
import "lib/v2/api.leno" as api_v2
```

### Q: 相对路径的基准是什么？

A: 相对于当前文件所在的目录。

***

## 12. 示例项目结构

```leno
// main.leno
import "config.leno" as cfg
import "routes.leno" as routes
import "db/connection.leno" as db

main() {
    cfg.load()
    db.connect()
    routes.start()
}
```

```leno
// config.leno
var _settings = {}

func load() {
    _settings = {
        port: 8080,
        debug: true
    }
}

func get(string key) {
    return _settings[key]
}
```

```leno
// routes.leno
import "config.leno" as cfg

func start() {
    var port = cfg.get("port")
    print("服务器启动在端口: " + port)
}
```

***

> **核心原则**：模块是组织代码的基本单元，合理使用可以让代码更清晰、更可维护。
