# Leno 语言入门指南

> **Leno 是什么？**
>
> Leno 是一门带静态类型检查的脚本语言，介于 Python（纯动态）和 Go（纯静态）之间。
>
> Leno 诞生于对编程语言设计的热爱与探索，虽非完美，但乐在其中。

## 目录

1. [基础语法](#基础语法)
2. [变量与类型](#变量与类型)
3. [运算符](#运算符)
4. [控制流](#控制流)
5. [函数](#函数)
   - [泛型约束（T: Face）](#泛型约束t-face)
6. [结构体（Struct）](#结构体struct)
   - [泛型结构体](#泛型结构体)
7. [face（接口）](#face接口)
   - [泛型 face](#泛型-face)
8. [枚举（Enum）](#枚举enum)
9. [数组](#数组)
10. [字典](#字典)
11. [字符串](#字符串)
12. [模块系统](#模块系统)
13. [异常处理（try-catch-finally）](#异常处理try-catch-finally)
14. [线程与并发](#线程与并发)
15. [异步编程](#异步编程)
16. [FFI 外部函数接口](#ffi-外部函数接口)
    - [cstruct 线程支持](#cstruct-线程支持)
17. [高级特性](#高级特性)
18. [语法速查表](#语法速查表)

***

## 基础语法

### Hello World

```leno
main() {
    print("Hello, Leno!")
}
```

### 语句结束

- 分号 `;` **可选**，换行即表示语句结束
- 使用 `,` 可以在同一行声明多个变量

```leno
main() {
    print("Hello")      // 无需分号
    print("World")      // 换行分隔

    var a = 1           // 分号可选
    var b = 2;          // 也可以使用分号
}
```

### 代码块

使用 `{}` 定义代码块：

```leno
main() {
    var a = 10
    if a == 10 {
        var a = 100     // 局部变量，遮蔽外部a
        print(a)        // 输出100
    }
    print(a)            // 输出10
}
```

### 注释

```leno
// 这是单行注释

/*
   这是多行注释
   可以跨多行
*/
```

### 输出函数

```leno
main() {
    // print: 输出并换行
    print("Hello")
    print(42)

    // printf: 格式化输出（不自动换行）
    printf("cs ")        // 输出 "cs "（不换行）
    printf("123\n")      // 输出 "123" 并换行
}
```

> **⚠️ 注意：`print`** **自动换行，`printf`** **不换行**
>
> ```leno
> print("a")            // 输出 "a\n"
> printf("a")           // 输出 "a"（无换行）
> printf("a\n")         // 输出 "a\n"（手动换行）
> ```

***

## 变量与类型

### 快速口诀

| 场景 | 推荐 | 口诀 |
| -- | -------------------------- | ----------- |
| 变量 | `var x = 值` | "右边有值用 var" |
| 参数 | `int x` / `Array[int] arr` | "参数写清楚" |
| 默认参数 | `int y = 10` | "默认值放末尾" |
| 返回 | `:int` / `:Array[int]` | "返回要声明" |

**⚠️ 重要：`var` 含义完全不同！**

| 场景 | 写法 | 实际类型 | 说明 |
| -------- | --------------- | ----- | ------------------------- |
| **变量声明** | `var x = 1` | `int` | ✅ 根据右边推断为具体类型 |
| **函数参数** | `func f(var x)` | `any` | ⚠️ 参数 `var` 保持 `any`，不推断！ |
| **函数返回** | `func f()` | `any` | 省略 `:type` 则返回 `any` |

**关键区别**：

- 变量 `var` = **推断模式**：右边是什么，`var` 就是什么
- 参数 `var` = **动态模式**：永远是 `any`，不根据调用推断

```leno
var x = 1              // x 是 int（推断）
func test(var x) {     // x 是 any（不推断！）
    var y = x          // y 也是 any（因为 x 是 any）
}

test(1)                // 调用时 x 仍是 any，不会变成 int
test("hello")          // x 仍然是 any
```

### 基本类型

| 类型       | 说明                | 示例                      |
| -------- | ----------------- | ----------------------- |
| `int`    | 整数（任意精度，自动溢出保护） | `int a = 10`            |
| `float`  | 浮点数（IEEE 754 双精度） | `float b = 3.14`        |
| `string` | 字符串               | `string s = "hello"`    |
| `bool`   | 布尔值               | `bool flag = true`      |
| `null`   | 空值                | `var a = null`          |
| `var`    | 类型推断              | `var x = 10`            |
| `Ptr`    | FFI 指针类型          | `Ptr p = ffi.malloc(8)` |

### 深入理解 int

Leno 的 `int` 是**任意精度**大整数，永远不会溢出。背后有一个巧妙的双层设计：

- **int48（48 位内联存储）**：值在 ±140 万亿（约 ±1.4×10¹⁴）范围内时，直接内联存储在 Value 中，性能极高，零开销。
- **BigInt（堆分配存储）**：值超出 int48 范围时，**自动升级**为堆分配的 BigInt 对象，精度无上限。

对外统一为 `int` 类型，`type()` 始终返回 `"int"`，用户无需关心底层存储形式。

```leno
main() {
    // int48 范围内：轻量级内联存储
    var a = 42                  // type(a) → "int"
    var b = 2000000000          // type(b) → "int"（在 int48 范围内）
    var c = b + b               // c = 4000000000，仍在 int48 范围内
    print(type(c))              // "int"

    // 超出 int48 范围：自动升级为 BigInt，对外仍是 int
    var big = 100000000000000000000  // type(big) → "int"
    print(type(big))                 // "int"

    // 运算溢出时自动升级
    var fib100 = 354224848179261915075  // 第 100 项斐波那契数
    print(type(fib100))                  // "int"（自动升级）

    // 任意精度整数运算完全透明
    var x = 1000000000000000     // 仍在 int48 范围内
    var y = x * x               // 结果超出 int48，自动升级
    print(type(y))              // "int"
    var z = y + 1               // 自动处理
}
```

> **💡 int48 范围有多大？**
>
> int48 的范围是 **-140,737,488,355,327 ~ 140,737,488,355,327**（约 ±140 万亿）。
>
> | 参考值 | 是否在 int48 内 |
> |--------|:---:|
> | 42 | ✅ |
> | INT32_MAX（约 21 亿） | ✅ |
> | 地球人口（约 80 亿） | ✅ |
> | 全球 GDP（约 100 万亿） | ✅ |
> | 100000000000000000000（100 垓） | ❌ → BigInt |
> | fib(100) ≈ 3.5×10²⁰ | ❌ → BigInt |

> **⚠️ 注意：`type()` 始终返回 `"int"`**
>
> ```leno
> int a = 10
> print(type(a))      // "int"（当前是 int48）
>
> a = 100000000000000000000
> print(type(a))      // "int"（底层自动升级，对外无变化）
> ```
>
> 变量声明为 `int`，底层运算自动处理溢出升级，`type()` 始终返回 `"int"`，对用户完全透明。

> **🔧 int 自动升级的运算**
>
> 以下运算在结果超出 int48 范围时会**自动升级**为 BigInt：
>
> ```leno
> // 加法、减法、乘法：自动检测溢出并升级
> var a = 1000000000000000
> var b = a + a                  // 自动升级
> var c = a * a                  // 自动升级
> var d = -140737488355328       // INT48_MIN 取负超出范围，自动升级
>
> // 超过 int64 范围也能正确处理
> var huge = 100000000000000000000 * 100000000000000000000
> print(type(huge))              // "int"
>
> // 除法规则
> int / int → int                // 整数除法
> int / float → float            // 浮点除法
> ```
>
> 从用户角度看，`int` 就是任意精度整数 — 你不需要关心底层细节，语言已经替你处理好了。

### 理解 var 类型推断

`var` 是 Leno 的类型推断关键字。编译器根据**初始值**推断类型：

```leno
main() {
    var x = 10          // 推断为 int
    var y = 3.14        // 推断为 float
    var s = "hello"     // 推断为 string
    var b = true        // 推断为 bool

    // var 推断的 int 同样享受自动溢出保护
    var a = 1000000000000000
    var result = a * a          // 结果超出 int48，自动升级
    print(type(result))         // "int"

    // var 多变量声明（用逗号分隔，每个变量独立赋值）
    var a1 = 10, b1, c1 = 1    // a1=10(int), b1=null, c1=1(int)
    int x1, y1, z1             // 三个 int 变量（均为 null）
    float f1, f2 = 3.14        // f1=null, f2=3.14
}
```

> **⚠️ 注意：`int` 声明 vs `var` 推断 — 行为一致**
>
> 无论你用 `int x = 10` 还是 `var x = 10`，运算溢出保护的行为完全相同：
>
> ```leno
> int a = 2000000000
> var b = 2000000000
> print(a + b)        // 4000000000，正确
> print(type(a + b))  // "int"（4000000000 仍在 int48 范围内）
>
> // 更大的运算
> var huge1 = 10000000000000 * 10000000000000
> print(type(huge1))  // "int"（超出 int48 范围自动升级）
> ```

### 变量声明总结

```leno
main() {
    // 显式类型声明
    int a = 10
    float b = 3.14
    string name = "Leno"
    bool flag = true

    // 类型推断（使用 var）
    var x = 10          // 推断为 int（值为 10）
    var y = 3.14        // 推断为 float
    var s = "hello"     // 推断为 string

    // 大数值的 var 推断
    var big = 100000000000000000000   // 推断为 int（底层自动升级）
    print(type(big))                   // "int"
}
```

> **⚠️ 注意：变量类型一旦确定，终身不变**
>
> ```leno
> var a = 1        // a 永久是 int
> a = 2            // ✅ 值改变，类型不变
> a = "hello"      // ❌ 报错：无法将 string 赋值给 int
> ```

> **⚠️ 注意：多变量声明中每个变量独立赋值，不是并行赋值**
>
> ```leno
> var a = 10, b, c = 1    // a=10, b=null, c=1（各自独立）
> // 不是：a=10, b=10, c=1
> ```

### 类型别名（alias）

使用 `alias` 为复杂类型起简短名字，减少重复书写：

```leno
alias IntList = Array[int]
alias StrDict = Dict[string, string]
alias Callback = func(int):bool

IntList nums = [1, 2, 3]    // 等价于 Array[int] nums
StrDict d = {"a": "hi"}     // 等价于 Dict[string, string] d
```

**别名链**——别名可以引用另一个别名：

```leno
alias A = int
alias B = A                 // B → A → int
func add(A x, B y):B { return x + y }
```

**模块导出别名链**——`export alias` 也支持链式引用：

```leno
// types.leno
export alias A = int
export alias B = A          // B → A → int

// main.leno
import "types.leno" as t
B val = 42                  // B 最终解析为 int
```

**与泛型配合**：

**嵌套别名**——支持任意深度组合：

```leno
alias IntMatrix = Array[IntList]           // Array[Array[int]]
alias NamedMap  = Dict[string, StrDict]    // Dict[string, Dict[string, string]]
```

**函数参数和返回值**：

```leno
alias IntList = Array[int]
func sum(IntList arr):int { ... }     // 参数
func make_list():IntList { ... }      // 返回类型
```

**泛型 struct 别名 + new 实例化**——别名可直接用于 `new`：

```leno
struct Cell[T] { T value }
alias IntCell = Cell[int]

var ic = new IntCell(value=42)
print(ic.value)     // 42

// 多参数泛型别名
struct Pair[K, V] { K key; V val }
alias StrIntPair = Pair[string, int]

var p = new StrIntPair(key="name", val=100)
print(p.key)        // name
```

**局部别名**——在函数体内定义，作用域限于该函数：

```leno
func test() {
    alias Words = Array[string]
    Words w = ["hello", "world"]
}
```

**与泛型配合**：

```leno
alias IntArr = Array[int]
var doubled = map(IntArr nums, double)   // 等价于 map(nums, double)
```

> **⚠️ 注意**：别名在编译期展开为实际类型，`type()` 返回的是展开后的真实类型而非别名。

### int/float 自动升级

`int` 是任意精度大整数，永远不会溢出。`float` 是 IEEE 754 双精度浮点数。

`int` 可以自动升级为 `float`（安全），但不能降级：

```leno
main() {
    float x = 42        // ✅ int → float，自动升级
    int y = 3.14        // ❌ float → int，不允许降级

    var arr = [1, 2.0]  // ✅ 推断为 Array[float]（int 升级为 float）
}
```

> **⚠️ 注意：大整数转 float 可能丢失精度**
>
> ```leno
> int a = 123456789123456789    // int 任意精度，精确存储
> float f = a                    // 转为 float，丢失低位精度
> print(a)                       // 123456789123456789（精确）
> print(f)                       // 1.2345678912345679e+17（约 15-17 位有效数字）
> ```
>
> 这不是 Leno 的 bug，而是 IEEE 754 双精度浮点数的固有特性。
>
> **float 输出规则**：
>
> - float 值始终带小数点或科学计数法，不会和 int 混淆
> - 整数值的 float 追加 `.0`：`50000000.0`
> - 大数/极小数自动使用科学计数法：`5e+64`
> - 保留 17 位有效数字（double 最大精度）

### null 类型

```leno
main() {
    // null 可以赋给任何类型，值保持 null
    int a = null
    string b = null
    print(a is null)     // true
    print(b is null)     // true

    // var 声明 null 后，第一次赋值锁定类型
    var x = null
    x = 42              // 锁定为 int
    // x = "hello"      // ❌ 报错：已锁定为 int

    // 清空变量
    var name = "cheng"
    name = null         // ✅ 允许

    // 函数返回 null
    func findUser(int id):string {
        if id == 0 { return null }
        return "cheng"
    }
}
```

> **⚠️ 注意：null 不能参与算术运算**
>
> ```leno
> int a = null
> a + 1              // ❌ 运行时错误：null 不能参与算术运算
> null == null       // true
> null == 0          // false
> null == ""         // false
> ```

> **⚠️ 注意：`var x = null`** **后类型未锁定，类似空数组**
>
> ```leno
> var x = null        // 类型为 null（未锁定）
> x = 42              // 锁定为 int
> x = "hello"         // ❌ 已锁定为 int
>
> var arr = []        // 类型为 Array（未锁定）
> arr.add(1)          // 锁定为 Array[int]
> arr.add("hello")    // ❌ 已锁定为 Array[int]
> ```

### 泛型类型

Leno 的泛型类型不仅支持内置的 `Array`、`Dict`、`Ptr` 等，还支持自定义 struct 的泛型类型注解：

```leno
main() {
    // 泛型数组（大写 Array）
    Array[int] arr1 = [1, 2, 3]
    Array[float] arr2 = [1.1, 2.2, 3.3]
    Array[string] arr3 = ["a", "b", "c"]

    // 空数组声明（两种写法等价）
    Array[int] arr4        // 空的 Array[int]
    Array[int] arr5 = []   // 等同于上面，也是空的 Array[int]
    var empty = []         // 类型为 Array（未指定，添加元素后锁定）

    // 二维数组
    Array[Array[int]] arr2d = [[1, 2], [3, 4]]

    // 泛型字典（大写 Dict）
    Dict[string, int] dict1 = {"a": 1, "b": 2}
    Dict[int, string] dict2 = {1: "one", 2: "two"}
    Dict[string, int] dict3       // 空的 Dict[string, int]
    Dict[int, string] dict4 = {}  // 等同于上面，也是空的 Dict[int, string]

    // 自定义泛型 struct 类型注解
    // 假设已定义 struct Box[T] { T value }
    Box[int] intBox = new Box[int](value=42)
    Box[string] strBox = new Box[string](value="hello")

    // 嵌套泛型 struct
    Box[Array[int]] nestedBox = new Box[Array[int]](value=[1,2,3])

    // 类型推断
    var inferred_arr = [1, 2, 3]      // 推断为 Array[int]
    var inferred_dict = {"x": 10}     // 推断为 Dict[string, int]
    var inferred_int_dict = {1: "一", 2: "二"}  // 推断为 Dict[int, string]
}
```

> **⚠️ 注意：`var empty = []`** **和** **`Array[int] arr = []`** **含义不同**
>
> ```leno
> var empty = []              // 类型为 Array（未锁定），添加元素后确定类型
> empty.add(1)                // 锁定为 Array[int]
>
> Array[int] arr = []         // 类型已确定为 Array[int]，等同于 Array[int] arr
> arr.add(1)                  // ✅ 直接添加
> ```

> **⚠️ 注意：泛型类型注解与类型擦除**
>
> 泛型参数在编译期完成类型检查，运行时部分信息保留：
> - `type(intBox)` 返回 `"Box[int]"`，包含类型参数信息
> - `Box[int]` 和 `Box[string]` 运行时共享同一个 struct 定义
> - 泛型不会产生代码膨胀，零运行时开销

### 文件级别变量

```leno
var a = 100     // 文件级别变量（在main函数外声明）

main() {
    print(a)    // 可以访问
}
```

### 类型转换

| 函数          | 用途        | 失败时   |
| ----------- | --------- | ----- |
| `_int(x)`   | 转为 int    | 运行时错误 |
| `_float(x)` | 转为 float  | 运行时错误 |
| `_bool(x)`  | 转为 bool   | 运行时错误 |
| `_str(x)`   | 转为 string | 运行时错误 |
| `_int32(x)` | 截断为 32 位有符号整数（环绕） | 运行时错误 |
| `_int64(x)` | 截断为 64 位有符号整数（环绕） | 运行时错误 |
| `_uint32(x)` | 转为 32 位无符号整数 | 运行时错误 |
| `_uint64(x)` | 转为 64 位无符号整数 | 运行时错误 |
| `_uint8(x)` | 转为 8 位无符号整数（0 ~ 255） | 运行时错误 |
| `_byte(x)` | `_uint8` 的别名，语义更清晰 | 运行时错误 |

```leno
main() {
    var s = "123"
    var n = _int(s)       // string → int
    var f = _float("3.14") // string → float

    var x = 3.14
    var i = _int(x)        // float → int（截断小数）
}
```

### int 固定位宽函数 `_int32()` / `_int64()` / `_uint32()` / `_uint64()` / `_uint8()` / `_byte()`

LenoC 的 `int` 是任意精度大整数，永远不会溢出。但在加密算法、网络协议、文件格式等场景中，经常需要固定位宽的环绕运算（overflow wrapping）。`_int32()`、`_int64()`、`_uint32()`、`_uint64()`、`_uint8()`、`_byte()` 提供了这种能力。

**`_int32(x)`** — 截断为 32 位有符号整数（环绕语义）：

```leno
main() {
    // 正常值：不变
    _int32(42)            // 42
    _int32(0x12345678)    // 305419896

    // 超出 32 位：取低 32 位，解释为有符号数
    _int32(0x100000000)   // 0（高 32 位被截断）
    _int32(0x1FFFFFFFF)   // -1（低 32 位全 1 = 无符号 0xFFFFFFFF = 有符号 -1）

    // 加法环绕
    int big = 2147483647 + 1   // int32 最大值 + 1，LenoC 自动提升为大整数
    _int32(big)                // -2147483648（环绕到 int32 最小值）

    // 负数
    _int32(-1)            // -1
}
```

**`_int64(x)`** — 截断为 64 位有符号整数（环绕语义）：

```leno
main() {
    // 正常值：不变
    _int64(42)            // 42
    _int64(0x123456789ABCDEF0)  // 1311768467463790320

    // 超出 64 位：取低 64 位，解释为有符号数
    _int64(0x1FFFFFFFFFFFFFFFF)  // -1（低 64 位全 1）
}
```

**`_uint32(x)`** — 将整数转为 32 位无符号整数（0 ~ 4294967295）：

```leno
main() {
    // 正数：不变
    _uint32(42)           // 42
    _uint32(0x7FFFFFFF)   // 2147483647

    // 负数转无符号（即按 32 位补码解释）
    _uint32(-1)           // 4294967295（= 0xFFFFFFFF）
    _uint32(-2147483648)  // 2147483648（= 0x80000000）

    // 超出范围：取低 32 位
    _uint32(0x100000000)  // 0
    _uint32(0x100000001)  // 1
}
```

**`_uint64(x)`** — 将整数转为 64 位无符号整数（0 ~ 18446744073709551615）：

```leno
main() {
    // 正数：不变
    _uint64(42)           // 42

    // 负数转无符号（即按 64 位补码解释）
    _uint64(-1)           // 18446744073709551615（= 0xFFFFFFFFFFFFFFFF）

    // 超过 INT64_MAX 的值自动转为 BigInt
    _uint64(0x8000000000000000)  // 9223372036854775808（BigInt）
}
```

**`_uint8(x)`** — 将整数截断为 8 位无符号整数（0 ~ 255）：

```leno
main() {
    // 正常值：不变
    _uint8(42)            // 42
    _uint8(0xFF)          // 255

    // 超出范围：取低 8 位
    _uint8(257)           // 1（257 & 0xFF = 1）
    _uint8(256)           // 0
    _uint8(-1)            // 255
}
```

**`_byte(x)`** — `_uint8` 的别名，语义更清晰：

```leno
main() {
    // _byte 与 _uint8 完全等价，但在字节操作场景下语义更明确
    _byte(0xFF)           // 255
    _byte(257)            // 1

    // 典型用法：提取字节
    int value = 0x12345678
    var b0 = _byte(value)           // 0x78（最低字节）
    var b1 = _byte(value >> 8)      // 0x56
    var b2 = _byte(value >> 16)     // 0x34
    var b3 = _byte(value >> 24)     // 0x12
}
```

> **💡 为什么需要 `_uint32()`？**
>
> LenoC 的 `>>` 是算术右移（符号位扩展），位运算在 LenoC 的大整数语义下可能出现非预期的符号扩展。`_uint32()` 将值转为无符号表示，确保位运算和右移按 32 位无符号语义执行。
>
> ```leno
> // ❌ 不用 _uint32：算术右移高位补 1
> int x = 0x80000000    // -2147483648
> x >> 1                // -1073741824（高位补 1，仍是负数）
>
> // ✅ 方法一：用 _uint32 转为无符号值
> _uint32(0x80000000) >> 1    // 1073741824（高位补 0）
>
> // ✅ 方法二：用 >>> 逻辑右移（推荐，语义更清晰）
> (_int32(0x80000000) & 0xFFFFFFFF) >>> 1    // 1073741824（高位补 0）
>
> // _uint32 在加密算法中简化位运算
> _int32((_uint32(x) & _uint32(y)) | (_uint32(~x) & _uint32(z)))  // MD5 F 函数
> ```
>
> `_uint32()` 等效于 `to_unsigned(_int32(x))` 的组合操作，一步完成"截断 32 位 → 转无符号"。

> **⚠️ 注意：固定位宽函数与 `_int()` 的区别**
>
> | 写法 | 语义 | 结果 |
> |------|------|------|
> | `_int(3.14)` | 强制类型转换 | `3`（截断小数） |
> | `_int32(3.14)` | 截断为 32 位有符号整数 | `3` |
> | `_int32(0x1FFFFFFFF)` | 32 位有符号环绕 | `-1`（低 32 位） |
> | `_uint32(-1)` | 转为 32 位无符号整数 | `4294967295`（= 0xFFFFFFFF） |
> | `_uint64(-1)` | 转为 64 位无符号整数 | `18446744073709551615`（= 0xFFFFFFFFFFFFFFFF） |
> | `_uint8(257)` | 截断为 8 位无符号整数 | `1`（257 & 0xFF） |
> | `_byte(-1)` | 等同于 `_uint8(-1)` | `255` |
> | `_int(0x1FFFFFFFF)` | 保持原值 | `8589934591`（不截断） |
>
> `_int()` 是类型转换，不改变数值；固定位宽函数是位宽截断，会环绕。

> **📊 固定位宽函数对比**
>
> | 函数 | 返回范围 | 用途 |
> |------|---------|------|
> | `_int32(x)` | -2147483648 ~ 2147483647 | 32 位有符号运算结果 |
> | `_uint32(x)` | 0 ~ 4294967295 | 32 位无符号位运算（&, \|, ^, >>>） |
> | `_int64(x)` | -9223372036854775808 ~ 9223372036854775807 | 64 位有符号运算 |
> | `_uint64(x)` | 0 ~ 18446744073709551615 | 64 位无符号位运算、FFI 交互 |
> | `_uint8(x)` | 0 ~ 255 | 字节操作、协议解析、颜色值 |
> | `_byte(x)` | 0 ~ 255 | `_uint8` 别名，语义更清晰 |
>
> ```leno
> // 典型加密算法中的配合模式：
> _int32(_uint32(x) ^ _uint32(y))      // 32 位无符号 XOR → 截断回有符号
> _int32(_uint32(x) + _uint32(y))      // 32 位无符号 ADD → 截断回有符号
> _int32(_int32(x) << c | ((_int32(x) & 0xFFFFFFFF) >>> (32 - c)))  // 32 位循环左移（用 >>>）
> ```

**典型应用：加密算法中的 32 位环绕运算**

```leno
// TEA 加密算法需要 32 位环绕运算
int DELTA = -0x61C88647
int sum = 0
for 32 {
    sum = _int32(sum + DELTA)   // 每轮累加后截断到 32 位
}

// 没有 _int32 时需要手动写：sum = sum + DELTA & 0xFFFFFFFF
// _int32 语义更清晰，且自动处理符号位
```

### as 安全类型转换

`as` 操作符提供安全类型转换：**匹配时返回原值，不匹配时返回 `null`**。int ↔ float 之间还会进行数值转换。

```leno
main() {
    // 同类型：返回原值
    var x = 42
    var xi = x as int          // 42

    // int ↔ float 数值转换
    var f = 3.14
    var i = f as int           // 3（float→int 截断小数）
    var a = 10
    var b = a as float         // 10.0（int→float 自动升级）

    // 不同类型：返回 null
    var s = "hello"
    var si = s as int          // null（string 不是 int）
    var n = 100
    var ns = n as string       // null（int 不是 string）

    // null 转换
    var na = null as int       // null
}
```

**struct 和 face 类型：**

```leno
struct Dog {
    string name = ""
    func speak():string { return "Woof" }
}

struct Cat {
    string name = ""
    func speak():string { return "Meow" }
}

var dog = new Dog()
var d = dog as Dog       // Dog 实例（匹配）
var c = dog as Cat       // null（不匹配）

// face 向下转型（最常用场景）
face Shape {
    func area():float
}

func describe(Shape s) {
    var c = s as Circle
    if c != null {
        return "Circle: " + _str(c._radius)
    }
    var r = s as Rect
    if r != null {
        return "Rect: " + _str(r._width)
    }
    return "Unknown"
}
```

> **⚠️ 注意：`as`** **与** **`_int()`/`_float()`** **的区别**
>
> | 写法 | 语义 | 结果 |
> |------|------|------|
> | `42 as float` | 类型检查 + 数值转换 | `42.0` |
> | `_float(42)` | 强制类型转换 | `42.0` |
> | `"abc" as int` | 类型检查 | `null`（不匹配） |
> | `_int("abc")` | 强制类型转换 | 运行时错误（解析失败） |
>
> `as` 是**安全的**：不匹配返回 `null`，不会崩溃。
> `_int()`/`_float()` 是**强制的**：转换失败会抛出运行时错误。

### type() 函数

`type()` 返回**运行时值的类型**（不是编译时声明的类型）：

```leno
main() {
    var x = 10
    print(type(x))          // "int"

    var big = 100000000000000000000
    print(type(big))        // "int"（值超出 int48 范围，底层自动升级为 BigInt）

    var arr = [1, 2, 3]
    print(type(arr))        // "Array[int]"

    var empty = []
    print(type(empty))      // "Array"

    var dict = {"a": 1}
    print(type(dict))       // "Dict[string, int]"

    var id_map = {1: "一"}
    print(type(id_map))     // "Dict[int, string]"

    int b = null
    print(type(b))          // "null"（值是 null，不是 int）
}
```

> **⚠️ 注意：`type()`** **返回的是值的类型，不是变量的编译时类型**
>
> ```leno
> int a = null
> print(type(a))    // "null"（因为当前值是 null）
> a = 10
> print(type(a))    // "int"（值变成了 10）
> a = 100000000000000000000
> print(type(a))    // "int"（值超出 int48，底层自动升级，对外不变）
> ```
>
> **type() 对 int 始终返回 `"int"`**，无论底层是 int48 还是 BigInt 存储，对用户完全透明。

***

## 运算符

### 算术运算符

```leno
+   // 加法
-   // 减法
*   // 乘法
/   // 除法（根据操作数类型返回 int 或 float）
%   // 取模
```

> **⚠️ 注意：`/`** **根据操作数类型决定结果类型**
>
> ```leno
> var a = 7 / 2       // int（3），整数除法
> var b = 7 / 2.0     // float（3.5），浮点除法
> var c = 7.0 / 2     // float（3.5），浮点除法
> // 需要浮点除法：至少一个操作数是 float
> ```
>
> **除法规则：**
>
> | 操作数类型             | 结果类型     | 示例                     |
> | ----------------- | -------- | ---------------------- |
> | `int / int`       | `int`    | `5 / 2 = 2`            |
> | `int / float`     | `float`  | `5 / 2.0 = 2.5`        |
> | `float / int`     | `float`  | `5.0 / 2 = 2.5`        |

### 位运算符

```leno
&   // 按位与
|   // 按位或
^   // 按位异或
~   // 按位非
<<  // 左移
>>  // 右移（算术右移，符号位扩展）
>>> // 逻辑右移（无符号右移，高位补 0）
```

> **⚠️ 注意：`~`** **按位取反的返回值**
>
> `~x` 等价于 `-x - 1`，对任意精度整数都成立：
>
> ```leno
> ~5       // -6    （= -5 - 1）
> ~0       // -1    （= -0 - 1）
> ~(-1)    // 0     （= -(-1) - 1 = 1 - 1）
> ~(-6)    // 5     （= -(-6) - 1 = 6 - 1）
> ```
>
> 对于需要 32 位环绕取反的场景，使用 `_int32(~x)`：
>
> ```leno
> _int32(~0xEFCDAB89)   // 0x10325476（32 位取反）
> // 不用 _int32 时：~0xEFCDAB89 = -4023233418（64 位扩展）
> ```

> **⚠️ 注意：`>>`** **是算术右移（符号位扩展）**
>
> ```leno
> int x = 0x80000000    // 负数（最高位为 1）
> x >> 1                // 算术右移，高位补 1，结果仍为负数
> ```
>
> **`>>>`** **是逻辑右移（高位补 0）**
>
> `>>>` 对 48 位值做无符号右移，高位始终补 0，不考虑符号位：
>
> ```leno
> int x = -1            // 48 位全 1：0xFFFFFFFFFFFF
> x >> 1                // 算术右移 → -1（高位补 1）
> x >>> 1               // 逻辑右移 → 140737488355327（0x7FFFFFFFFFFF，高位补 0）
>
> // 正数时 >>> 和 >> 结果相同
> 256 >>> 8             // 1
> 256 >> 8              // 1
>
> // 负数时 >>> 和 >> 结果不同
> (-256) >>> 8          // 1099511627775（高位补 0）
> (-256) >> 8           // -1（高位补 1）
> ```
>
> **💡 32 位逻辑右移的正确写法**
>
> LenoC 的 `int` 是 48 位有符号整数，`_int32()` 返回的负数会符号扩展到 48 位。
> 要实现 32 位逻辑右移，需要先 `& 0xFFFFFFFF` 截断为 32 位无符号值，再用 `>>>`：
>
> ```leno
> // ❌ 错误：_int32 的负数符号扩展后 >>> 是 48 位逻辑右移
> _int32(0x856d6974) >>> 8     // 结果不对
>
> // ✅ 正确：先截断为 32 位无符号值，再逻辑右移
> (_int32(0x856d6974) & 0xFFFFFFFF) >>> 8   // 8744297
> ```
>
> **💡 32 位循环移位用 `>>>`**
>
> ```leno
> // 循环左移 rotl32
> func rotl32(int x, int n):int {
>     n = _int32(n) & 31
>     if (n == 0) { return _int32(x) }
>     return _int32((_int32(x) << n) | ((_int32(x) & 0xFFFFFFFF) >>> (32 - n)))
> }
>
> // 循环右移 rotr32
> func rotr32(int x, int n):int {
>     return rotl32(x, 32 - (_int32(n) & 31))
> }
> ```
>
> `>>>` 在加密算法（TEA、XXTEA、Speck、RC5 等）中特别有用，替代了之前需要手写 `lshr32` 函数的写法。

### 复合赋值运算符

```leno
+=  -=  *=  /=  %=      // 算术复合赋值
&=  |=  ^=  <<=  >>=  >>>=  // 位运算复合赋值
++  --                  // 自增自减
```

### 比较运算符

```leno
==  !=  <  >  <=  >=    // 比较运算符
```

### 逻辑运算符

`and`、`or`、`not` 是逻辑运算符，具有**短路求值**特性，且**返回实际值**（Python 风格）：

```leno
main() {
    // and: 两个都为真时返回最后一个值，否则返回第一个假值
    1 and 2               // 2
    1 and 0               // 0
    "" and "hello"        // ""（空字符串是假值）
    null and "hello"      // null

    // or: 返回第一个真值，都为假时返回最后一个值
    1 or 0                // 1
    0 or "hello"          // "hello"
    "" or "default"       // "default"
    null or "default"     // "default"

    // not: 返回 bool
    not true              // false
    not false             // true
    not 0                 // true（0 是假值）
    not 1                 // false（1 是真值）
}
```

> **⚠️ 注意：`and`/`or`** **返回的是实际值，不一定是 bool**
>
> ```leno
> var result = 1 and 2    // result 是 2（int），不是 true
> var x = "" or "hi"      // x 是 "hi"（string），不是 true
> ```

> **⚠️ 注意：`and`/`or`** **的返回类型由编译器推断**
>
> - 常量折叠：`1 and 2` → 编译器知道结果是 2，推断为 `int`
> - 常量传播：`var x = 1; x and 2` → 编译器追踪 x=1，推断为 `int`
> - 不确定时：函数参数等无法追踪 → 回退为 `bool`
>
> ```leno
> func test(var x) {
>     var r = x and 2     // r 推断为 bool（编译器不确定 x 的值）
> }
> ```

### 短路求值

```leno
main() {
    func sideEffect(msg) {
        print("执行了: " + msg)
        return true
    }

    // and 短路：第一个为假时不再执行第二个
    false and sideEffect("and")   // 不会执行 sideEffect

    // or 短路：第一个为真时不再执行第二个
    true or sideEffect("or")      // 不会执行 sideEffect
}
```

### 运算符类型规则

> **⚠️ 重要：bool 和 null 不能参与算术运算**
>
> ```leno
> true + 1         // ❌ 编译错误：bool 类型不能参与算术运算
> true - 1         // ❌ 编译错误
> "hello" - "lo"   // ❌ 编译错误：string 类型不能参与减法
> "hello" * 2      // ❌ 编译错误：string 类型不能参与乘法
> null + 1         // ❌ 运行时错误：null 不能参与算术运算
> ```

**`+`** **特殊规则：** 一边是 string 时，自动转为字符串拼接：

```leno
"hello" + " world"   // "hello world"
"hello" + 42         // "hello42"
42 + "hello"         // "42hello"
```

**`==`** **规则：** int 和 float 做数值比较，其他类型严格匹配：

```leno
1 == 1.0          // true（数值比较）
"1" == 1          // false（类型不同）
true == 1         // false（类型不同）
null == 0         // false
null == null      // true
```

**`<`** **`>`** **规则：** 禁止跨类型比较：

```leno
1 < "2"           // ❌ 编译错误：无法比较 int 和 string
true < false      // ❌ 编译错误：bool 类型不能比较大小
1 < 2.0           // ✅ int 和 float 可以比较
"abc" < "def"     // ✅ 字典序比较
```

### 成员检查运算符 (in / not in)

`in` 和 `not in` 用于检查成员关系：

```leno
main() {
    // 数组成员检查
    var arr = [1, 2, 3, 4, 5]
    if 3 in arr {
        print("3 在数组中")
    }
    if 10 not in arr {
        print("10 不在数组中")
    }

    // 字典成员检查（检查 key）
    var dict = {"a": 1, "b": 2}
    if "a" in dict {
        print("键 'a' 存在")
    }
    if "z" not in dict {
        print("键 'z' 不存在")
    }

    // int 键字典成员检查
    var id_map = {1: "张三", 2: "李四"}
    if 1 in id_map {
        print("键 1 存在")
    }

    // 字符串子串检查
    var str = "hello world"
    if "world" in str {
        print("包含 'world'")
    }
    if "xyz" not in str {
        print("不包含 'xyz'")
    }
}
```

### 运算符示例

```leno
main() {
    // 算术运算
    var a = 10
    a += 1;  print(a)     // 11
    a -= 3;  print(a)     // 8
    a *= 2;  print(a)     // 16

    // 位运算
    var b = 5       // 二进制 101
    var c = 3       // 二进制 011
    print(b & c)    // 1  (001)
    print(b | c)    // 7  (111)
    print(b ^ c)    // 6  (110)
    print(~b)       // -6

    // 位移
    var d = 8       // 二进制 1000
    print(d << 2)   // 32 (100000)
    print(d >> 2)   // 2  (10)

    // 逻辑右移 >>>
    var neg = -1
    print(neg >> 1)    // -1（算术右移，高位补 1）
    print(neg >>> 1)   // 140737488355327（逻辑右移，高位补 0）

    // 复合赋值
    var x = 10;  x %= 3;  print(x)  // 1
    var y = 5;   y &= 3;  print(y)  // 1
    var z = 5;   z |= 2;  print(z)  // 7
    var w = -1;  w >>>= 8; print(w) // 1099511627775
}
```

***

## 控制流

### if 条件语句

```leno
main() {
    var score = 85

    if score >= 90 {
        print("优秀")
    } else if score >= 80 {
        print("良好")
    } else if score >= 60 {
        print("及格")
    } else {
        print("不及格")
    }
}
```

### eif 简写（else if 的简写）

`eif` 是 `else if` 的缩写，功能完全相同，可以混用：

```leno
main() {
    var a = 10

    if a == 11 {
        print(11)
    } eif a == 12 {         // eif 等同于 else if
        print(12)
    } else if a == 10 {     // 也可以继续用 else if
        print(10)
    } else {
        print("未知")
    }
}
```

### 类型守卫（Type Guard）

使用 `is` 检查类型，在 if 块内自动收窄：

```leno
main() {
    var a = 100     // a 是 int

    if a is int {
        print("进入 int 分支")
        int result = a + 100
        print("result = " + result)
    }

    // 否定类型守卫
    if a not is string {
        print("a 不是字符串")
    }

    // null 检查
    var b = null
    if b is null {
        print("b 是 null")
    }
}
```

> **⚠️ 注意：类型守卫只在 if 块内有效**
>
> ```leno
> func test(var x) {
>     if x is int {
>         int n = x     // ✅ 守卫内 x 是 int
>     }
>     int m = x        // ❌ 报错：离开 if 后 x 恢复为 any
> }
> ```

> **⚠️ 注意：`or`** **条件不收窄类型**
>
> ```leno
> func test(var x, var y) {
>     if x is int or y is string {
>         int n = x      // ❌ 报错：or 条件下不收窄
>     }
> }
> ```

### 三元表达式

使用 `if ... then ... else`：

```leno
main() {
    var a = 10
    var b = 20

    // 基本用法
    var max = if a > b then a else b
    print("max = " + max)       // max = 20

    // 带括号
    var min = if (a < b) then a else b
    print("min = " + min)       // min = 10

    // 嵌套
    var c = 15
    var middle = if a > b then (if a > c then a else c) else (if b > c then b else c)

    // 在表达式中使用
    var score = 85
    var grade = if score >= 90 then "A" else (if score >= 80 then "B" else "C")
    print("grade: " + grade)    // grade: B
}
```

**类型规则**：

- 如果 `then` 和 `else` 分支类型相同，返回该具体类型
- 如果类型不同，返回 `any`

```leno
var x = if true then 1 else 2          // ✅ int
var y = if true then 1 else "hello"    // ⚠️ any（类型不同）
int z = if flag then 2 else 1          // ✅ 可直接赋值给 int
```

### switch 多分支

```leno
main() {
    var name = "cc"
    var b = 10

    switch name {
        case "ch", "cc" {       // 多值匹配
            print("ok")
            b = 100
        }
        case "c4" {
            print("c4")
        }
        default {
            print("error")
        }
    }
    print(b)        // 输出100
}
```

### switch `case is` 类型匹配

`case is` 可以在 switch 中按类型匹配分支，类似 if 类型守卫但在 switch 中更简洁：

```leno
main() {
    var x = "hello"

    switch x {
        case is int {
            print("是整数: " + x)
        }
        case is string {
            print("是字符串: " + x)    // 匹配此分支
        }
        case is float {
            print("是浮点数: " + x)
        }
        case is bool {
            print("是布尔值")
        }
        default {
            print("未知类型")
        }
    }
}
```

**值匹配和类型匹配可以混合使用：**

```leno
func process(var x) {
    switch x {
        case 0 {
            print("零")
        }
        case 1, 2, 3 {
            print("小数字")
        }
        case is int {
            print("其他整数: " + x)
        }
        case is string {
            print("字符串: " + x)
        }
        default {
            print("其他类型")
        }
    }
}
```

**支持的类型：** `int`、`float`、`string`、`bool`、`null`、struct 类型、face 类型等。

```leno
face Shape {
    func area():float
}

struct Circle impl Shape {
    float _radius = 1.0
    func area():float { return 3.14159 * _radius * _radius }
}

struct Rect impl Shape {
    float _width = 1.0
    float _height = 1.0
    func area():float { return _width * _height }
}

func describe(var s) {
    switch s {
        case is Circle {
            print("圆形，面积: " + s.area())
        }
        case is Rect {
            print("矩形，面积: " + s.area())
        }
        case is Shape {
            print("其他形状，面积: " + s.area())
        }
        default {
            print("不是形状")
        }
    }
}
```

> **⚠️ 注意：`case is` 匹配成功后变量类型会被收窄**
>
> ```leno
> switch x {
>     case is int {
>         int n = x   // ✅ 分支内 x 被收窄为 int
>     }
>     case is string {
>         string s = x  // ✅ 分支内 x 被收窄为 string
>     }
> }
> ```

### while 循环

```leno
main() {
    var i = 0
    while i < 10 {
        i++
        if i == 5 {
            break           // 跳出循环
        }
        print(i)
    }
    print("out:" + i)       // out:5
}
```

### for 循环（多种形式）

Leno 的 `for` 循环支持多种遍历形式，包括数字范围遍历、数组遍历、字符串遍历和字典遍历。

**重要规则**：
- 范围语法中，`:` 表示**包含**结束值
- `for 10` → 0 到 9（不包含10）
- `for :10` → 0 到 10（包含10）

#### 基本数字循环

```leno
main() {
    // 循环 n 次，i 从 0 开始到 n-1
    for 5 {
        print("cs")  // 输出 5 次 "cs"
    }

    // 带变量名：循环 5 次，num 从 0 到 4
    for 5 to num {
        print(num)  // 输出: 0 1 2 3 4
    }
}
```

#### 范围循环

**不包含结束值（默认）**：

```leno
// 从 0 到 4（不包含 5）
for 5 to num {
    print(num)  // 输出: 0 1 2 3 4
}
```

**包含结束值（使用冒号）**：

⚠️ 重要：在 Leno 中，`:` 表示范围包含结束值

```leno
// 包含语法：从 0 到 5（包含 5）
for :5 to num {
    print(num)  // 输出: 0 1 2 3 4 5
}

// 完整写法：从 0 到 5（包含 5）
for 0:5 to num {
    print(num)  // 输出: 0 1 2 3 4 5
}

// 从 2 到 5（包含 5）
for 2:5 to num {
    print(num)  // 输出: 2 3 4 5
}
```

**使用变量作为范围**：

```leno
var a = 5
var b = 10

// 从 a 到 b（包含 b）
for a:b to i {
    print(i)  // 输出: 5 6 7 8 9 10
}
```

#### 带步进值的循环

```leno
// 正序步进：从 0 到 5，步进 2（包含 5）
for 0:5:2 to num {
    print(num)  // 输出: 0 2 4
}

// 小数步进：从 0 到 1，步进 0.1（包含 1）
for 0:1:0.1 to num {
    print(num)
    // 输出: 0.0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0
}
```

#### 倒序遍历

当起始值大于结束值时，自动倒序：

```leno
// 自动倒序：从 5 到 0（包含 0）
for 5:0 to num {
    print(num)  // 输出: 5 4 3 2 1 0
}

// 指定负步进：从 5 到 0，步进 -2（包含 0）
for 5:0:-2 to num {
    print(num)  // 输出: 5 3 1
}
```

#### 遍历数组

```leno
main() {
    var arr = [1, 2, 3, 4, 5]

    // 遍历元素
    for arr to a {
        print(a)
    }

    // 带索引遍历（第二个变量是索引）
    for arr to item, index {
        print($"{index}: {item}")
    }
}
```

在排序算法中使用：

```leno
func bubble_sort(var arr) {
    var n = arr.len()
    for n - 1 to var i {
        for n - i - 1 to var j {
            if arr[j] > arr[j + 1] {
                var temp = arr[j]
                arr[j] = arr[j+1]
                arr[j+1] = temp
            }
        }
    }
    return arr
}
```

#### 遍历字符串

```leno
main() {
    var str = "cheng8214"

    // 遍历字符
    for str to a {
        print(a)
    }

    // 带索引遍历（第二个变量是索引）
    for str to char, index {
        print($"{index}: {char}")
    }

    // Unicode 字符串遍历（按字符，非按字节）
    var cn = "你好世界"
    for cn to ch {
        print(ch)        // 依次输出：你、好、世、界
    }
}
```

#### 遍历字典

```leno
main() {
    var dict = {name: "cheng", age: 25}

    // 遍历 key
    for dict to key {
        print(key)
    }

    // 遍历 key 和 value（第二个变量是 value）
    for dict to key, value {
        print($"{key}: {value}")
    }
}
```

**int 键字典遍历**：

```leno
main() {
    var id_map = {1: "张三", 2: "李四", 3: "王五"}

    for id_map to id, name {
        print($"{id}: {name}")
    }
    // 输出顺序: 1:张三, 2:李四, 3:王五（按插入顺序）
}
```

**带类型注解的字典遍历**：

```leno
Dict[string, string] d = {id: "11", name: "23"}

for d to key, value {
    // key 类型: string
    // value 类型: string
    string s = value  // 类型安全
    print($"{key}: {s}")
}
```

> **⚠️ 注意：双变量遍历时，第二个变量的含义取决于类型**
>
> - 数组/字符串：第二个变量是**索引**（int）
> - 字典：第二个变量是**值**（value）
>
> ```leno
> for arr to item, index    // index 是 int 索引
> for str to char, index    // index 是 int 索引
> for dict to key, value    // value 是键对应的值
> ```

> **⚠️ 注意：遍历 any 类型（常见陷阱）**
>
> 当遍历对象的类型在编译期未知（`any` 类型）时，`for X to y` 可能**静默不迭代**（不报错也不循环）：
>
> ```leno
> // ❌ 可能静默失败：files 是 any 类型
> for files to f { }  // 如果 files 是 any，不会迭代
>
> // ✅ 通用修复：用索引循环替代 for-each
> int n = _int(files.len())
> for 0 : n - 1 to i {
>     var f = _str(files[i])
> }
> ```

#### for 循环快速参考表

| 语法 | 说明 | 包含 | 示例输出 |
|------|------|------|---------|
| `for 5` | 循环5次，i从0到4 | 不含 | 0 1 2 3 4 |
| `for 5 to i` | 循环5次，i从0到4 | 不含 | 0 1 2 3 4 |
| `for :5 to i` | i从0到5 | ✅ 包含 | 0 1 2 3 4 5 |
| `for 2:5 to i` | i从2到5 | ✅ 包含 | 2 3 4 5 |
| `for 0:5:2 to i` | i从0到5，步进2 | ✅ 包含 | 0 2 4 |
| `for 5:0 to i` | i从5到0（倒序） | ✅ 包含 | 5 4 3 2 1 0 |
| `for arr to item` | 遍历数组 | — | 数组元素 |
| `for arr to item, idx` | 遍历数组带索引 | — | 元素+索引 |
| `for str to char` | 遍历字符串 | — | 每个字符 |
| `for str to char, idx` | 遍历字符串带索引 | — | 字符+索引 |
| `for dict to key` | 遍历字典（键） | — | 每个键 |
| `for dict to key, value` | 遍历字典（键值对） | — | 键+值 |

***

## 函数

### 函数定义

```leno
// 基本函数
func add(var a, var b) {
    return a + b
}

// 带返回类型（推荐）
func multiply(int a, int b):int {
    return a * b
}

// 无返回值
func printHello() {
    print("Hello")
}

main() {
    var result = add(1, 2)
    print(result)
}
```

> **⚠️ 注意：`var`** **参数永远是** **`any`，不会根据调用推断类型**
>
> ```leno
> func test(var x) {
>     int a = x    // ❌ 报错：any 不能赋给 int
> }
> test(1)           // x 内部仍然是 any，不是 int
> ```

### 前向引用

函数可以先调用后定义：

```leno
main() {
    var a = 10
    test()      // 先调用

    func test() {
        print(a)
        test1()     // 也可以嵌套调用
        func test1() {
            print(100)
        }
    }
}
```

### 默认参数

```leno
main() {
    // 基本默认参数
    func add(int x, int y = 10):int {
        return x + y
    }
    print(add(1))       // 11
    print(add(1, 20))   // 21

    // 多个默认参数
    func test(int x, int y = 10, string z = "hi"):string {
        return $"x={x} y={y} z={z}"
    }
    print(test(1))            // x=1 y=10 z=hi
    print(test(1, 20))        // x=1 y=20 z=hi
    print(test(1, 20, "hey")) // x=1 y=20 z=hey
}
```

> **⚠️ 默认参数规则：**
>
> 1. 默认参数必须在必选参数**后面**
> 2. 默认值只能是**字面量常量**（int、float、string、bool、null）
> 3. 默认值类型必须**匹配参数类型**（遵循升级不降级规则）
> 4. `var a` 无默认值时类型为 `any`，`var b = 0` 推断为 `int`
>
> ```leno
> func bad(int x = 10, int y) { }      // ❌ 默认参数不能在必选参数前面
> func bad2(int x, int y = 3.14) { }   // ❌ float 不能赋给 int（降级）
>
> func ok(var a, var b = 10) {
>     a         // 编译期 any（无默认值，不推断）
>     b         // 编译期 int（从默认值 10 推断）
> }
> ```

### var 速查：变量 vs 参数

**`var` 在不同场景含义完全不同：**

| 场景 | 写法 | 实际类型 | 说明 |
|------|------|------|------|
| **变量声明** | `var x = 1` | `int` | ✅ 根据右边推断具体类型 |
| **函数参数** | `func f(var x)` | `any` | ⚠️ 参数 `var` 保持 `any`，不推断！ |
| **函数返回** | `func f()` | `any` | 省略 `:type` 则返回 `any` |

**选择口诀：**

| 场景 | 推荐 | 口诀 |
|------|------|------|
| 变量 | `var x = 值` | "右边有值用 var" |
| 参数 | `int x` / `Array[int] arr` | "参数写清楚" |
| 默认参数 | `int y = 10` | "默认值放末尾" |
| 返回 | `:int` / `:Array[int]` | "返回要声明" |

**var 参数 vs 具体类型参数：**

| 特性 | 具体类型 | var |
|------|:---:|:---:|
| 类型检查 | 编译期 | 运行期 |
| 字段访问 | ✅ | ❌ |
| 方法调用 | ✅ | ✅（运行时查找） |
| 索引访问 | ✅ | ✅ |
| 比较运算 | ✅ | ✅ |

> 需要访问 struct 字段 → 具体类型。只需打印/比较 → 可用 var。

**`any` 的编译时 vs 运行时：**

`any` 类型的关键区别——**编译时检查赋值，运行时检查运算**：

| 场景 | 代码 | 结果 | 说明 |
|------|------|:--:|------|
| `any` 直接运算 | `n - 2` | ✅ | 运行时检查，数值可运算 |
| `any` 赋给 `int` | `int a = n` | ❌ | 编译时检查，any 不能隐式转 int |
| `any` 运算后赋给 `int` | `int a = n - 2` | ❌ | 运算结果还是 any |
| `any` 显式转换后赋值 | `int a = _int(n)` | ✅ | 显式转换通过编译 |

```leno
func fib(var n) {        // n 是 any，但运行时实际是 int
    if n <= 1 {          // ✅ 可以比较，运行时检查
        return n
    }
    return fib(n-2) + fib(n-1)  // ✅ 可以算术运算
}
fib(10)  // 正常工作，返回 55
```

### 函数作为值

Leno 支持函数作为一等公民，可以像普通值一样赋值给变量、传递给参数、作为返回值：

```leno
// 函数赋值给变量
func add(int a, int b) { return a + b }
func mul(int a, int b) { return a * b }

var my_add = add       // 函数赋值给变量
print(my_add(3, 4))    // 7

// 函数作为参数（高阶函数）
func apply_operation(int x, int y, var operation) {
    return operation(x, y)
}

print(apply_operation(5, 3, add))   // 8
print(apply_operation(5, 3, mul))   // 15

// 函数作为返回值
func get_operation(var op_name) {
    if op_name == "add" {
        return add
    } else if op_name == "mul" {
        return mul
    }
    return null
}

var op = get_operation("add")
print(op(10, 20))      // 30

// 函数存储在数组中
func func1() { return 1 }
func func2() { return 2 }

var func_array = [func1, func2]
for func_array to f {
    print(f())         // 1, 2
}

// 函数存储在字典中
func greet_en() { return "Hello" }
func greet_cn() { return "你好" }

var greetings = {"en": greet_en, "cn": greet_cn}
print(greetings["cn"]())   // 你好
```

### 匿名函数

Leno 支持匿名函数（Lambda 表达式），可以在需要函数的地方直接定义：

```leno
// 基本匿名函数
var f = func() { return 42 }
print(f())              // 42

// 带参数的匿名函数
var add = func(int a, int b) { return a + b }
print(add(1, 2))        // 3

// 匿名函数作为参数（高阶函数）
func apply(var f, var x) {
    return f(x)
}

print(apply(func(var x) { return x * 2 }, 5))   // 10

// 匿名函数作为返回值
func make_adder(int n) {
    return func(int x) { return x + n }
}

var add5 = make_adder(5)
print(add5(10))         // 15
print(add5(100))        // 105

// 立即执行匿名函数 (IIFE)
var result = func() { return 100 }()
print(result)           // 100

// 匿名函数存储在数组中
var funcs = [
    func() { return 1 },
    func() { return 2 },
    func() { return 3 }
]
print(funcs[0]())       // 1
print(funcs[1]())       // 2

// 匿名函数存储在字典中
var ops = {
    "double": func(int x) { return x * 2 },
    "triple": func(int x) { return x * 3 }
}
print(ops["double"](5)) // 10
```

#### 泛型匿名函数

匿名函数也支持泛型类型参数，与命名函数语法一致：

```leno
// 泛型匿名函数
var id = func[T](T x): T { return x }
print(id[int](42))        // 42
print(id[string]("hi"))   // hi

// 多类型参数
var pair = func[K, V](K k, V v): Pair[K, V] {
    return new Pair[K, V](key=k, val=v)
}
```

> **⚠️ 注意**：泛型匿名函数调用时必须显式指定类型参数（如 `id[int](42)`），编译器不会自动推断匿名函数的类型参数。

### 泛型函数（Generic Functions）

Leno 支持函数级泛型，通过在函数名后添加 `[T, U, ...]` 声明类型参数：

> **💡 类型参数命名**
>
> 类型参数名是**任意标识符**，可以自由命名，没有语法限制。常见的命名习惯：
>
> | 名称 | 含义 | 示例 |
> |------|------|------|
> | `T` | Type（通用类型） | `func id[T](T v): T` |
> | `U` | 第二个通用类型 | `func map[T, U](Array[T] arr, func(T):U fn): Array[U]` |
> | `K` | Key（键类型） | `struct Pair[K, V] { K key; V val }` |
> | `V` | Value（值类型） | `func get[K, V](Dict[K, V] d, K k): V` |
> | `R` | Return/Result（返回类型） | `func cast[T, R](T v): R` |
>
> 你也可以用 `A`、`X`、`Item`、`Element` 等任意名称：
>
> ```leno
> func swap[A, B](A a, B b): Pair[B, A] { return new Pair[B, A](key=b, val=a) }
>
> struct List[Item] { Item data; List next }
>
> func select[Src, Dst](Src input): Dst { ... }
> ```
>
> **类型参数名只在定义作用域内有效**，不同函数/struct 之间互不影响。

#### 自动推断类型参数

编译器会根据调用时的实参**自动推断**具体类型：

```leno
// === 单类型参数 ===
func identity[T](T val):T {
    return val
}

print(identity(42))        // 42（T 推断为 int）
print(identity("hello"))   // hello（T 推断为 string）

// === 多类型参数 ===
func pair[T, U](T a, U b) {
    print(a)
    print(b)
}
pair(42, "hello")  // 42, hello
```

#### 显式指定类型参数

当自动推断不够准确时（如参数为 `null`），可以用 `funcName[类型](args)` 显式指定：

```leno
func identity[T](T v): T {
    return v
}

// 显式指定类型参数
var a = identity[int](42)           // T = int
var b = identity[string]("hello")   // T = string

// 多个类型参数
func first[T, U](T a, U b): T {
    return a
}

var r1 = first[int, string](1, "x")    // T=int, U=string → 返回 1
var r2 = first[string, float]("pi", 3.14)  // T=string, U=float → 返回 "pi"
```

> **💡 何时使用显式类型参数？**
>
> - 参数为 `null` 时，编译器无法推断类型，必须显式指定
> - 需要确保返回值的具体类型时
> - 代码可读性要求高的场景
>
> ```leno
> func coalesce[T](T a, T b): T {
>     if a != null { return a }
>     return b
> }
>
> // ❌ coalesce(null, 42) — 无法推断 T
> // ✅ 必须显式指定：
> var val = coalesce[int](null, 42)    // T = int
> ```

#### 泛型函数接受泛型 struct 参数

泛型函数的参数类型可以是泛型 struct，实现类型安全的容器操作：

```leno
struct Box[T] {
    T value
}

// 从 Box 中提取值
func unbox[T](Box[T] b): T {
    return b.value
}

var intBox = new Box[int](value=42)
var strBox = new Box[string](value="hello")

assert_eq(unbox[int](intBox), 42)
assert_eq(unbox[string](strBox), "hello")

// 创建 Box
func makeBox[T](T v): Box[T] {
    return new Box[T](value=v)
}

var boxed = makeBox[int](100)
assert_eq(boxed.value, 100)
```

#### 泛型函数与比较操作

泛型参数支持 `==`、`!=`、`>=`、`<=`、`>`、`<` 等比较操作：

```leno
func max[T](T a, T b): T {
    if a >= b {
        return a
    }
    return b
}

assert_eq(max[int](3, 5), 5)
assert_eq(max[string]("abc", "xyz"), "xyz")
```

#### 泛型约束（T: Face）

泛型参数可以用 `T: FaceName` 语法约束为必须实现某个 face 的类型，从而在函数体内调用 face 定义的方法：

```leno
face Comparable {
    func compare(Comparable other): int
}

func maxBy[T: Comparable](T a, T b): T {
    if a.compare(b) >= 0 { return a }
    return b
}
```

**struct 实现 face 后即可作为约束类型参数使用：**

```leno
struct IntBox impl Comparable {
    int value
    func compare(Comparable other): int {
        var o = other as IntBox
        if self.value < o.value { return -1 }
        if self.value > o.value { return 1 }
        return 0
    }
}

struct StrBox impl Comparable {
    string value
    func compare(Comparable other): int {
        var o = other as StrBox
        if self.value < o.value { return -1 }
        if self.value > o.value { return 1 }
        return 0
    }
}

main() {
    var a = new IntBox(value=10)
    var b = new IntBox(value=20)
    var r = maxBy(a, b)
    var ri = r as IntBox
    print(ri.value)           // 20

    var s1 = new StrBox(value="apple")
    var s2 = new StrBox(value="banana")
    var rs = maxBy(s1, s2)
    var ris = rs as StrBox
    print(ris.value)          // banana
}
```

**泛型约束也可用于 struct 定义：**

```leno
struct SortedBox[T: Comparable] {
    T value
}

struct Pair[K, V: Comparable] {
    K key
    V val
}
```

> **💡 泛型约束要点**
>
> - `T: Face` 要求传入的类型必须**显式 `impl`** 该 face
> - 约束后可在函数体内调用 face 定义的方法（如 `a.compare(b)`）
> - 多个类型参数可以分别约束：`[K, V: Comparable]`
> - 未约束的泛型参数（如 `K`）仍可接受任意类型

**多 face 实现：**

一个 struct 可以同时实现多个 face，用逗号分隔：

```leno
face Comparable {
    func compare(Comparable other): int
}

face Hashable {
    func hash(): int
}

struct Key impl Comparable, Hashable {
    int id
    func compare(Comparable other): int {
        var o = other as Key
        return self.id - o.id
    }
    func hash(): int { return self.id }
}

func maxBy[T: Comparable](T a, T b): T {
    if a.compare(b) >= 0 { return a }
    return b
}

func getHash[T: Hashable](T item): int {
    return item.hash()
}

main() {
    var k1 = new Key(id=10)
    var k2 = new Key(id=20)
    var r = maxBy(k1, k2)     // Key 实现了 Comparable，可以作为 T: Comparable
    var ri = r as Key
    print(ri.id)               // 20
    print(getHash(k1))         // 10
}
```

**编译期约束校验：**

如果传入的类型没有实现约束 face，编译器会报错：

```leno
struct NotComparable {
    int value
}

main() {
    var a = new NotComparable(value=1)
    var b = new NotComparable(value=2)
    var r = maxBy(a, b)    // ❌ 编译错误：类型 'NotComparable' 不满足约束 'Comparable'
}
```

**约束方法作为值传递（方法引用）：**

泛型约束的方法可以作为值传递，返回绑定方法的函数类型：

```leno
face Printable {
    func format(): string
}

struct Named impl Printable {
    string name
    func format(): string { return "Name: " + self.name }
}

func getFormatter[T: Printable](T item): func():string {
    return item.format    // 方法引用，返回绑定方法
}

main() {
    var n = new Named(name="Leno")
    var fmt = getFormatter(n)
    print(fmt())           // Name: Leno
}
```

> **⚠️ 注意事项**：
> 
> 1. **类型推断在调用时发生**：编译器从实参类型自动推断，无需手动指定 `T=int`
> 2. **显式类型参数优先**：如果调用时写了 `funcName[int](args)`，则使用显式指定的类型，不再推断
> 3. **泛型参数可用于参数和返回类型**：`Array[T]`、`Box[T]`、`Dict[K, V]` 等均可
> 4. **泛型参数是编译期的**：用 `T` 声明的变量在编译期完成类型替换和检查，运行时无额外开销
> 5. **编译期校验**：传入未实现约束 face 的类型会在编译期报错，而非运行时
> 6. **方法引用返回绑定方法**：`item.method` 返回已绑定 `self` 的函数，调用时无需再传 self
> 7. **运算符约束建议**：无约束的泛型参数用于算术/比较运算时，编译器会输出 stderr 警告（不阻塞编译）。建议使用 `T: FaceName` 约束代替直接运算符，或忽略警告（运行时检查）


### 三种参数模式对比

Leno 提供三种参数声明方式，按需选择：

| 模式 | 语法 | 类型检查 | 适用场景 |
|------|------|:---:|------|
| **var** | `func f(var x)` | ❌ 编译期不检查 | 任意类型输入，运行时自行处理 |
| **泛型 [T]** | `func f[T](T x)` | ✅ 调用时推断 | 多类型但需类型安全 |
| **具体类型** | `func f(int x)` | ✅ 编译期严格 | 确定类型，无须多态 |

```leno
// var: 完全动态，接受任何类型
func print_any(var x) {
    print(type(x))           // 运行时才能知道类型
}
print_any(42)                // int
print_any("hello")           // string

// [T]: 类型安全 + 多态
func id[T](T val):T {
    return val               // T 在函数体内保持类型一致
}
print(id(42))                // 42，返回值也是 int
print(id("world"))           // world，返回值也是 string

// 具体类型: 最严格的类型检查
func add(int a, int b):int {
    return a + b
}
add(3, 4)                    // ✅ OK
add("x", "y")                // ❌ 编译错误
```

> **💡 选择口诀**
>
> - **"什么类型都可能"** → 用 `var`
> - **"类型要安全，但不止一种"** → 用 `[T]`
> - **"就这个类型，别的不接受"** → 用具体类型


Leno 支持闭包，内部函数可以访问外部函数的变量：

```leno
// 创建计数器闭包
func make_counter(int start) {
    int count = start
    
    func increment() {
        count = count + 1
        return count
    }
    
    return increment
}

main() {
    var counter = make_counter(10)
    print(counter())   // 11
    print(counter())   // 12
    print(counter())   // 13
    
    // 每个闭包有独立的变量
    var counter2 = make_counter(100)
    print(counter2())  // 101
    print(counter())   // 14 (counter 不受影响)
}
```

> **⚠️ 闭包注意事项：**
>
> - 内部函数可以是命名函数或匿名函数
> - 匿名函数可以捕获外部变量形成闭包
> - 闭包可以修改外部变量（upvalue）
> - 多个闭包可以共享同一个外部变量

> **⚠️ 函数类型规则：**
>
> - 相同类型可以赋值：`func():int → func():int` ✅
> - 具体 → 通用可以赋值：`func(int):int → func` ✅
> - 通用 → 具体不能赋值：`func → func():int` ❌
> - 不同具体类型不能赋值：`func():int → func():string` ❌
> - 非函数值不能赋给函数变量：`a = 10` ❌
> - 函数类型一旦赋值就不能改变：`var f = foo; f = 10` ❌

***

## 结构体（Struct）

Leno 的 struct 不仅是数据结构，还支持面向对象编程：字段、方法、类型守卫等。

**快速入门：**

```leno
// 定义一个 struct
struct Point {
    int x = 0
    int y = 0
}

// 创建实例
main() {
    var p = new Point()    // 创建 Point 实例
    p.x = 10            // 访问字段
    p.y = 20
    print(p)            // 输出: Point{x=10, y=20}
}
```

### 定义结构体

```leno
struct Point {
    int x
    int y
}

// 带默认值
struct Rectangle {
    int x = 0
    int y = 0
    int width
    int height
}
```

> **💡 建议：使用有意义的默认值**
>
> ```leno
> // ✅ 好的设计：合理的默认值
> struct ServerConfig {
>     string host = "localhost"
>     int port = 8080
>     int timeout = 30
>     bool enableLog = true
> }
>
> // ❌ 不推荐：所有字段都是 0/空值
> struct ServerConfig {
>     string host = ""
>     int port = 0
>     int timeout = 0
>     bool enableLog = false
> }
> ```

### 创建结构体实例

```leno
// 命名参数创建（推荐）
var p1 = new Point(x = 10, y = 20)
var rect = new Rectangle(x = 0, y = 0, width = 100, height = 50)

// 空参数创建（字段使用默认值）
var p2 = new Point()        // x = null, y = null
var rect2 = new Rectangle() // x = 0, y = 0, width = null, height = null
```

> **⚠️ 注意：struct 创建必须使用** **`new`** **关键字和命名参数形式**
>
> ```leno
> var p = new Point(10, 20)      // ❌ 错误：必须使用 new Point(x = 10, y = 20)
> var p = new Point(x = 10, y = 20)  // ✅ 正确
> ```

### 访问和修改字段

```leno
var p = new Point(x = 10, y = 20)

// 使用点号访问（推荐，O(1) 直接索引）
print(p.x)      // 10
print(p.y)      // 20

// 修改字段
p.x = 100
p.y = 200
print(p.x)      // 100

// 使用索引访问（等价于点号，但较慢，O(n) 字符串查找）
var x2 = p["x"]
var y2 = p["y"]
```

> **💡 性能提示**：优先使用 `obj.field` 而不是 `obj["field"]`。点号访问在编译期确定字段索引，运行时直接定位；索引访问需要运行时字符串查找。

### null 默认值与编译时检查

**⚠️ 重要：默认值为 null 的字段，访问其成员会有编译时警告**

```leno
struct Container {
    dict data = null      // 默认 null
    array items = null    // 默认 null
}

struct Outer {
    Container inner = null
}

main() {
    var c = new Container()

    // ✅ 获取 null 本身 - 不警告
    var d = c.data

    // ❌ 访问 null 字段的成员 - 编译时警告
    var v = c.data["key"]     // [语义错误] 字段 'c.data' 默认值为 null...
    var item = c.items[0]     // [语义错误] 字段 'c.items' 默认值为 null...

    // ❌ 链式访问 null 字段
    var o = new Outer()
    var x = o.inner.data      // [语义错误] 字段 'o.inner' 默认值为 null...
}
```

**在方法中访问 null 字段**：

```leno
struct LinkedList {
    Node head = null

    func getFirst():int {
        // ❌ 编译时警告：访问可能为 null 的字段
        return head.value
    }

    func safeGetFirst():int {
        // ✅ 先检查再访问
        if (head != null) {
            return head.value
        }
        return 0
    }
}
```

### 结构体方法

在 struct 内部定义 func，实现面向对象编程：

```leno
struct Point {
    int x
    int y
    
    // 打印坐标
    func print_coord() {
        print("Point(" + x + ", " + y + ")")
    }
    
    // 计算到原点的距离
    func distance_to_origin():float {
        return maths.sqrt(x * x + y * y)
    }
    
    // 计算到另一个点的距离
    func distance_to(Point other):float {
        int dx = other.x - x
        int dy = other.y - y
        return maths.sqrt(dx * dx + dy * dy)
    }
    
    // 移动点（修改字段）
    func move(int dx, int dy) {
        x = x + dx
        y = y + dy
    }
}

// 使用方法
var p1 = new Point(x = 0, y = 0)
var p2 = new Point(x = 3, y = 4)

p1.print_coord()                    // Point(0, 0)
print(p1.distance_to_origin())      // 0.0
print(p1.distance_to(p2))           // 5.0

p1.move(10, 20)
p1.print_coord()                    // Point(10, 20)
```

**方法调用其他方法**：

```leno
struct Calculator {
    int value = 0

    func add(int n) {
        value = value + n
    }

    func double() {
        add(value)    // 调用同 struct 的其他方法
    }
}
```

**使用插值字符串输出 struct**：

推荐使用 `$"..."` 来格式化输出，比字符串拼接更简洁：

```leno
struct Point {
    int x = 0
    int y = 0
}

main() {
    var p = new Point()
    p.x = 10
    p.y = 20

    // ❌ 字符串拼接，需要显式转换
    print("(" + _str(p.x) + ", " + _str(p.y) + ")")

    // ✅ 插值字符串，自动转换
    print($"({p.x}, {p.y})")   // 输出: (10, 20)

    // 也可以使用 format 方法
    import strings
    print(strings.format("坐标: ({0}, {1})", p.x, p.y))
}
}
```

### self 关键字

方法内可以使用 `self` 显式引用当前实例：

```leno
struct Counter {
    int count = 0
    
    func increment() {
        self.count = self.count + 1    // 使用 self
    }
    
    func get_count():int {
        return self.count
    }
    
    func reset() {
        self.count = 0
    }
}

var c = new Counter()
c.increment()
c.increment()
print(c.get_count())    // 2
```

> **注意**：也可以不写 `self`，直接访问字段：`count = count + 1`
> 使用 `self` 的情况：
>
> - 参数名和字段名冲突时：`func set_x(int x) { self.x = x }`
> - 调用其他方法时：`self.print_coord()`
> - 想要更清晰的代码时

### 结构体数组与类型守卫

struct 实例添加到数组后，保留完整类型信息：

```leno
// 创建 Point 数组
var points = []
points.add(new Point(x = 0, y = 0))
points.add(new Point(x = 3, y = 4))
points.add(new Point(x = 6, y = 8))

// 使用 Array[Point] 类型守卫
func process_points(var points) {
    if points is Array[Point] {
        // 在类型守卫内部，points[0] 被识别为 Point 类型
        var p = points[0]
        int x_val = p.x        // ✅ 直接访问，无需转换
        int y_val = p.y        // ✅ 直接访问，无需转换
        
        // 调用方法
        p.print_coord()
        
        // 遍历
        for points to p, i {
            print("Point " + i + ": (" + p.x + ", " + p.y + ")")
        }
    }
}

process_points(points)
```

**struct 与类型守卫结合使用**：

当使用 `var` 接收 struct 实例时，类型守卫可以收窄类型，安全访问字段：

```leno
struct Student {
    string name = ""
    int age = 0
}

func process(var obj) {
    // ❌ 直接访问会报错：var 无法识别字段
    // print(obj.name)

    // ✅ 使用类型守卫收窄类型
    if obj is Student {
        // 守卫块内 obj 被视为 Student 类型
        print(obj.name)   // 可以访问字段
        print(obj.age)
    }
}

main() {
    var s = new Student()
    s.name = "张三"
    s.age = 18
    process(s)   // 输出: 张三  18
}
```

### 结构体作为类

Leno 的 struct 具备类的所有特性：

```leno
struct LinkedList {
    Node head = null
    int count = 0
    
    // 添加元素到尾部
    func add(int value) {
        var new_node = new Node()
        new_node.value = value
        
        if head == null {
            head = new_node
        } else {
            var current = head
            while current.next != null {
                current = current.next
            }
            current.next = new_node
        }
        count = count + 1
    }
    
    // 打印链表
    func print_list() {
        print("链表 (共 " + count + " 个): ")
        var current = head
        while current != null {
            print(current.value)
            if current.next != null {
                print(" -> ")
            }
            current = current.next
        }
    }
}

struct Node {
    int value
    Node next = null
}

// 使用
var list = new LinkedList()
list.add(10)
list.add(20)
list.add(30)
list.print_list()   // 链表 (共 3 个): 10 -> 20 -> 30
```

### 嵌套结构体

struct 字段可以是另一个 struct 类型：

```leno
struct Point {
    int x = 0
    int y = 0
}

struct Size {
    int width = 100
    int height = 100
}

struct Rect {
    Point position      // 嵌套 Point
    Size size           // 嵌套 Size
}

main() {
    var r = new Rect()
    // 嵌套对象自动初始化
    print(r)  // Rect{position=Point{x=0, y=0}, size=Size{width=100, height=100}}

    // 访问嵌套字段
    r.position.x = 10
    r.position.y = 20
    r.size.width = 200
    r.size.height = 150

    print(r.position.x)  // 10
    print(r.size.width)  // 200
}
```

**自引用 struct 必须设 null 默认值**：

```leno
// ❌ 错误：会导致栈溢出（无限递归）
struct Node {
    int value
    Node next    // 没有默认值，会无限创建 Node
}

// ✅ 正确：设置 null 默认值
struct Node {
    int value
    Node next = null    // 默认为 null，避免递归
}
```

也可以用另一种嵌套方式：

```leno
struct Address {
    string city
    string street
}

struct Person {
    string name
    int age
    Address addr    // 嵌套结构体
}

var p = new Person(
    name = "张三",
    age = 25,
    addr = new Address(city = "北京", street = "朝阳路")
)

print(p.name)           // 张三
print(p.addr.city)      // 北京
print(p.addr.street)    // 朝阳路
```

### struct vs 字典

虽然 struct 和字典都能表达结构化数据，但 struct 性能远超字典：

| 版本 | 创建时间 | 计算时间 |
|------|----------|----------|
| struct | 16 ms | **2390 ms** |
| 字典 | 15 ms | 6031 ms |

**struct 快 2.5 倍**，因为直接字段访问（编译期偏移量）无需运行时查找。

### 为什么 struct 参数不能用 var？

`var` 参数没有 struct 定义信息，无法访问字段：

| 参数类型 | 能否访问字段 | 说明 |
|---------|:---:|------|
| `LinkedList list` | ✅ | 编译器知道结构，生成字段访问指令 |
| `var list` | ❌ | `any` 类型，没有 struct 定义，无法识别 `.head` |

需要访问 struct 字段 → 用**具体类型**参数。只需要打印、比较、索引 → 可以用 `var`。

#### 链表示例

利用 struct 自引用实现完整数据结构：

```leno
struct Node {
    int value
    Node next = null       // 必须 null，否则无限递归
}

struct LinkedList {
    Node head = null
    int count = 0
}

func list_add(LinkedList list, int value) {
    var new_node = new Node()
    new_node.value = value
    if list.head == null {
        list.head = new_node
    } else {
        var current = list.head
        while current.next != null { current = current.next }
        current.next = new_node
    }
    list.count = list.count + 1
}

// ❌ func list_add(var list, int value) — 不行！list.head 无法识别
```

### 泛型结构体

Leno 支持泛型 struct，可以用类型参数定义通用的数据结构：

#### 定义泛型 struct

在 struct 名称后用 `[T]` 声明类型参数，可以用 `= 类型` 为类型参数设置默认值：

```leno
// 单个类型参数
struct Box[T] {
    T value
}

// 多个类型参数
struct Pair[K, V] {
    K key
    V val
}

// 带默认值的类型参数
struct Box[T = int] {
    T value
}

struct DictWrapper[K = string, V = int] {
    K key
    V val
}
```

> **💡 默认类型参数**
>
> 当**所有类型参数都有默认值**时，实例化可以省略类型参数：
>
> ```leno
> var b1 = new Box[int](value=42)   // 显式指定
> var b2 = new Box(value=100)       // 省略，T 默认 int
> ```
>
> 如果**部分参数无默认值**，仍然必须显式指定：
>
> ```leno
> struct Cache[K, V = int] { K key; V val }
> new Cache[string](key="name", val=1)   // K 必须指定
> ```

#### 实例化泛型 struct

使用 `new StructName[具体类型](...)` 创建实例，**除非所有类型参数都有默认值，否则不可省略**：

```leno
// 用 int 实例化
var intBox = new Box[int](value = 42)
print(intBox.value)     // 42

// 用 string 实例化
var strBox = new Box[string](value = "hello")
print(strBox.value)     // hello

// 多个类型参数
var p = new Pair[string, int](key = "age", val = 25)
print(p.key)            // age
print(p.val)            // 25

// ❌ 省略类型参数会报错
// var bad = new Box(value = 42)    // 编译错误：泛型 struct 'Box' 需要类型参数（如 Box[类型]）
```

> **⚠️ 注意：泛型 struct 必须指定类型参数**
>
> 泛型 struct 实例化时必须提供所有类型参数，编译器不会自动推断。这是因为不同类型参数产生不同的类型，省略会导致歧义。

#### 泛型 struct 带默认值和方法

```leno
struct Container[T] {
    T item = null
    string name = ""
}

var c = new Container[int](item = 100, name = "numbers")
print(c.item)           // 100
print(c.name)           // numbers

// 泛型 struct 带方法
struct Counter[T] {
    T start
    int count = 0

    func increment():int {
        self.count = self.count + 1
        return self.count
    }

    func reset() {
        self.count = 0
    }
}

var counter = new Counter[int](start = 0)
counter.increment()
counter.increment()
print(counter.count)    // 2
counter.reset()
print(counter.count)    // 0
```

#### 泛型 struct 方法的类型推断

泛型 struct 的方法会自动从 `self` 的类型参数推断字段类型，无需额外声明：

```leno
struct Box[T] {
    T value

    // 方法内可以直接使用 self.value，类型自动推断为 T 的具体类型
    func get(): T {
        return self.value
    }

    func set(T v) {
        self.value = v
    }

    // 方法间调用也能正确推断类型
    func unwrapOrDefault(T defaultVal): T {
        if self.value != null {
            return self.value
        }
        return defaultVal
    }

    // 方法互相调用：self.add() 的返回类型也会正确推断
    func add(T v): T {
        return self.value + v
    }

    func double(): T {
        return self.add(self.value)   // ✅ self.add 返回 T，正确推断
    }
}

var intBox = new Box[int](value=42)
intBox.set(100)                         // ✅ 参数类型推断为 int
assert_eq(intBox.get(), 100)
assert_eq(intBox.add(8), 108)           // ✅ 方法调用
assert_eq(intBox.double(), 200)         // ✅ 方法互相调用
assert_eq(intBox.unwrapOrDefault(0), 100)

var strBox = new Box[string](value="hi")
strBox.set("world")                     // ✅ 参数类型推断为 string
assert_eq(strBox.get(), "world")
assert_eq(strBox.unwrapOrDefault("empty"), "world")
```

> **💡 方法类型推断规则**
>
> - `self` 的类型自动携带泛型参数（如 `Box[int]` 的 `self.value` 类型为 `int`）
> - 方法参数中引用泛型参数 `T` 会自动替换为实例化时的具体类型
> - 方法返回类型中的 `T` 也会正确替换
> - 方法间互相调用（如 `self.method()`）也能正确传播类型
> - 链式方法调用（如 `outer.get().get()`）也能正确推断每一步的返回类型

#### 泛型 struct 的泛型方法

泛型 struct 的方法也可以有自己的泛型参数，与 struct 级别的泛型参数独立：

```leno
struct Box[T] {
    T value

    // U 是方法级别的泛型参数，与 struct 的 T 独立
    func map[U](func(T):U fn): Box[U] {
        return new Box[U](value=fn(self.value))
    }

    func set(T v) {
        self.value = v
    }
}

func doubleInt(int n): int { return n * 2 }
func intToStr(int n): string { return n == 0 ? "zero" : "nonzero" }

var intBox = new Box[int](value=42)

// int -> int 映射
var doubled = intBox.map[int](doubleInt)
print(doubled.value)    // 84

// int -> string 映射
var strBox = intBox.map[string](intToStr)
print(strBox.value)     // "nonzero"

// 链式调用
var chained = intBox.map[int](doubleInt).map[int](doubleInt)
print(chained.value)    // 168
```

> **💡 泛型方法的类型参数规则**
>
> - struct 级泛型参数（`T`）由实例化类型决定（如 `Box[int]` 的 `T = int`）
> - 方法级泛型参数（`U`）由调用时的显式指定或推断决定（如 `.map[string](...)` 的 `U = string`）
> - 方法内可以同时使用 `T` 和 `U`，编译器会正确替换两者
> - 链式泛型方法调用也支持：`obj.map[A](f1).map[B](f2)`

#### 泛型注意事项

> **⚠️ 注意 1：方法参数类型不匹配会被编译器报错**
>
> 泛型 struct 的方法参数类型会根据实例化类型进行检查。如果传入类型不匹配，编译器会报错：
>
> ```leno
> struct Box[T] {
>     T value
>     func set(T v) { self.value = v }
> }
>
> var intBox = new Box[int](value=42)
> intBox.set("wrong")    // ❌ 编译错误：set 第 1 个参数类型不匹配: 期望 int, 实际 string
>
> var strBox = new Box[string](value="hi")
> strBox.set(123)        // ❌ 编译错误：set 第 1 个参数类型不匹配: 期望 string, 实际 int
> ```
>
> 多泛型参数时，每个参数都会独立检查：
>
> ```leno
> struct Pair[K, V] {
>     K key
>     V val
>     func setBoth(K k, V v) { self.key = k; self.val = v }
> }
>
> var p = new Pair[string, int](key="x", val=1)
> p.setBoth(1, "x")      // ❌ 两个参数都报错：第1个期望 string 实际 int，第2个期望 int 实际 string
> ```

> **⚠️ 注意 2：Array/Dict 默认值已深拷贝（已修复）**
>
> ~~泛型参数在编译期检查后会被擦除，`Box[int]` 和 `Box[string]` 运行时共享同一个 struct 定义。如果 struct 内部使用了可变的引用类型字段（如数组），不同实例化类型之间可能意外共享数据。~~
>
> **已修复**：`struct_instance_new` 在初始化时对 Array/Dict/Struct 类型的默认值自动深拷贝，每个实例拥有独立副本。
>
> ```leno
> struct Container[T] {
>     Array items = []     // ✅ 每个实例独立的 []
> }
> var c1 = new Container[int]()
> var c2 = new Container[string]()
> c1.items.add(42)
> print(c2.items.len())   // 0（不再共享）
> ```

> **⚠️ 注意 3：泛型函数调用时 `[` 可能被误解析为数组索引**
>
> 当函数名同时也是变量名时，`funcName[Type](args)` 可能被解析为数组索引。Leno 编译器会自动区分这两种情况，但在复杂表达式中可能出现歧义。建议：
>
> - 优先使用类型推断，省略显式类型参数：`identity(42)` 而非 `identity[int](42)`
> - 只在推断失败时才显式指定：`empty[int]()`

> **⚠️ 注意 4：泛型函数参数类型不一致时不会自动转换**
>
> 显式指定类型参数后，编译器会严格检查参数类型，不会做隐式转换：
>
> ```leno
> func strict[T](T a, T b): T { return a }
>
> strict[int](1, 2)          // ✅ OK
> strict[int](1, "x")        // ❌ 编译错误：第 2 个参数类型不匹配
> strict[string]("a", "b")   // ✅ OK
> ```
>
> 如果需要灵活类型，使用 `var` 参数：
> ```leno
> func flexible(var a, var b) { ... }   // 接受任意类型组合
> ```

> **⚠️ 注意 5：方法参数数量不匹配也会报错**
>
> 非 泛型 struct 的方法同样会检查参数数量和类型：
>
> ```leno
> struct Point {
>     int x
>     int y
>     func setXY(int a, int b) { self.x = a; self.y = b }
> }
>
> var pt = new Point(x=1, y=2)
> pt.setXY(1)              // ❌ 方法 'setXY' 参数数量不匹配: 期望 2, 实际 1
> pt.setXY(1, 2, 3)        // ❌ 方法 'setXY' 参数数量不匹配: 期望 2, 实际 3
> pt.setXY("x", 10)        // ❌ setXY 第 1 个参数类型不匹配: 期望 int, 实际 string
> ```

> **⚠️ 注意 6：泛型函数中 `func(T):void` 返回类型的匹配**
>
> 当泛型函数的参数类型是函数类型且返回 `void` 时，传入的具体函数也必须返回 `void`（或不声明返回类型）：
>
> ```leno
> func make_printer[T](T prefix): func(T):void {
>     func inner(T val):void {    // ✅ 正确：显式声明 :void
>         print(prefix)
>         print(val)
>     }
>     return inner
> }
> ```
>
> 注意：`func(T):void` 中的 `void` 是特殊关键字，表示无返回值。不要写成 `func(T):null` 或省略。

> **⚠️ 注意 7：泛型参数参与算术运算时保持泛型类型**
>
> 泛型参数参与算术运算（如 `n - 1`、`n * 2`）时，结果类型仍保持泛型参数类型，不会退化为 `float`：
>
> ```leno
> func factorial[T](T n): T {
>     if n <= 1 { return 1 }
>     return n * factorial(n - 1)   // n-1 的类型仍为 T，不会变成 float
> }
>
> print(factorial[int](5))   // 120
> ```

> **⚠️ 注意 8：泛型 struct 省略类型参数会报错**
>
> 泛型 struct 实例化时必须提供类型参数，省略会导致编译错误：
>
> ```leno
> struct Box[T] {
>     T value
> }
>
> var b = new Box(value=42)       // ❌ 编译错误：泛型 struct 'Box' 需要类型参数（如 Box[类型]）
> var b = new Box[int](value=42)  // ✅ 正确
> ```
>
> 这与内置泛型类型（`Array`、`Dict`）不同——内置类型可以省略类型参数，编译器会从初始值推断。但自定义泛型 struct 必须显式指定。

> **⚠️ 注意 9：泛型函数体内创建泛型 struct — 类型参数如何传递**
>
> 在泛型函数（如 `func Ok[T](T val)`）内部创建泛型 struct（如 `new Result[T]()`）时，类型参数 `T` 会通过调用链自动传递：
>
> ```leno
> func Ok[T](T val): Result[T] {
>     return new Result[T](data=val, ok=true)    // ✅ T 由调用方 Ok[float] 传入，closure 携带
> }
>
> func Err[T](string msg): Result[T] {
>     return new Result[T](ok=false, error=msg)  // ✅ T 同样通过调用链传递，无需 dummy 参数
> }
>
> func reverse[T](Array[T] arr): Array[T] {
>     var s = new Stack[T]()                       // ✅ T 由调用方 reverse[int] 传入
>     ...
> }
> ```
>
> **原理**：调用泛型函数时（如 `Ok[float](...)`），编译器生成 `OP_PUSH_TYPE_ARGS` 指令将类型参数编码为常量。`OP_CALL` 创建 closure 时将这些类型参数存入 `closure.type_param_args`。函数体内创建泛型 struct 时，`OP_STRUCT_INIT` 直接从 closure 读取解析，**不再依赖运行时值推断**。
>
> 这个机制保证了无论函数参数是否携带泛型类型（如 `Err[T](string msg)` 只有 string 参数），都能正确获取 T 的具体类型。

> **⚠️ 注意 10：`int / int = int` 整数除法在泛型上下文中的陷阱**
>
> Leno 中 `int / int` 执行整数除法（向零取整），结果也是 `int`：
>
> ```leno
> var a = 10 / 3      // a = 3（int），而不是 3.333
> var b = 10.0 / 3    // b = 3.333...（float）
> ```
>
> 这在与泛型组合时尤为危险。例如 `Result[float]` 的构造：
>
> ```leno
> func safeDivide(int a, int b): Result[float] {
>     return Ok[float](a / b)     // ❌ a/b = 3（int），泛型推断为 int，与 float 不匹配
> }
> ```
>
> **正确做法**：
> ```leno
> func safeDivide(int a, int b): Result[float] {
>     return Ok[float]((a as float) / (b as float))  // ✅ 显式转为浮点除法
> }
> ```

> **⚠️ 注意 11：face 变量不能直接访问底层 struct 的字段和非 face 方法**
>
> face 类型的变量**只能调用 face 中声明的方法**，不能访问底层 struct 的字段或未声明的方法：
>
> ```leno
> face Sortable {
>     func sortKey(): float
> }
>
> struct Student impl Sortable {
>     string name
>     float score
>     func sortKey(): float { return self.score }
> }
>
> var s = new Student(name="Alice", score=85.0)
> Sortable face_s = s
>
> face_s.sortKey()       // ✅ face 声明的方法
> face_s.name            // ❌ 语义错误：face 'Sortable' 没有字段 'name'
> ```
>
> **解决办法**：使用 `as` 转型访问底层 struct：
> ```leno
> var student = face_s as Student
> print(student.name)    // ✅
> ```
>
> 这是 Leno 的类型安全设计——保证通过 face 变量只能看到接口约定的行为，而非具体实现的内部细节。

> **⚠️ 注意 12：泛型约束语法 `[T: FaceName]`**
>
> 可以用 `: FaceName` 为泛型类型参数添加约束，限制 T 必须实现了某个 face：
>
> ```leno
> face Comparable {
>     func compare(Comparable other): int
> }
>
> // T: Comparable — T 必须实现了 Comparable
> func maxBy[T: Comparable](T a, T b): T {
>     if a.compare(b) >= 0 { return a }
>     return b
> }
>
> struct OrdInt impl Comparable {
>     int value
>     func compare(Comparable other): int {
>         var o = other as OrdInt
>         if self.value < o.value { return -1 }
>         if self.value > o.value { return 1 }
>         return 0
>     }
> }
> ```
>
> 约束也支持跨模块使用：
>
> ```leno
> // constraint_module.leno
> export face Comparable { ... }
> export func maxBy[T: Comparable](T a, T b): T { ... }
>
> // main.leno
> import "constraint_module.leno" as cm
> use cm.Comparable
>
> struct OrdInt impl Comparable { ... }
> var r = cm.maxBy(a, b)  // ✅ 跨模块约束泛型调用
> ```
>
> **完整可运行示例**：
>
> ```leno
> face Comparable {
>     func compare(Comparable other): int
> }
> struct OrdInt impl Comparable {
>     int value
>     func compare(Comparable other): int {
>         var o = other as OrdInt
>         if self.value < o.value { return -1 }
>         if self.value > o.value { return 1 }
>         return 0
>     }
> }
> func id[T: Comparable](T a): T { return a }
>
> main() {
>     var o1 = new OrdInt(value=10)
>     var r1 = id(o1)
>     print(r1.value)     // 10
> }
> ```

> **⚠️ 注意 13：泛型 struct 的方法调用 —— `c.set(99)` 直接生效**
>
> 泛型 struct 的方法参数中 `T` 在编译期被解析为 struct 类型（`TYPE_STRUCT "T"`），但语义检查器会识别出它是 struct 的泛型参数名，跳过编译期类型比对，交给运行时处理：
>
> ```leno
> struct Cell[T] {
>     T value
>     func set(T v) { self.value = v }
> }
>
> var c1 = new Cell[int](value=42)
> c1.set(99)              // ✅ runtime: T=int, 99 是 int，正确
> print(c1.value)          // 99
> ```
>
> 这种设计使得泛型方法在保持编译期解析简单的前提下，在运行时获得正确的类型检查，兼顾了性能和灵活性。

#### 嵌套泛型类型

泛型参数可以是复合类型，如 `Array[int]`、`Dict[string, int]` 等：

```leno
// Box 中存放数组
var arrBox = new Box[Array[int]](value = [1, 2, 3])
print(arrBox.value[0])  // 1

// Pair 中存放字典
var dictPair = new Pair[string, Dict[string, int]](key = "scores", val = {"math": 90})
print(dictPair.val["math"])  // 90
```

#### 泛型 struct 完整示例：Range

```leno
struct Range[T] {
    T lo
    T hi

    func contains(T val): bool {
        return val >= self.lo and val <= self.hi
    }

    func clamp(T val): T {
        if val < self.lo { return self.lo }
        if val > self.hi { return self.hi }
        return val
    }
}

var intRange = new Range[int](lo=1, hi=10)
assert_eq(intRange.contains(5), true)
assert_eq(intRange.contains(0), false)
assert_eq(intRange.clamp(15), 10)
assert_eq(intRange.clamp(-5), 1)
```

> **⚠️ 注意：泛型参数与 type()**
>
> 泛型参数在编译期完成类型检查，运行时 `type()` 会返回带类型参数的名称：
>
> - `type(intBox)` 返回 `"Box[int]"`，包含类型参数信息
> - `type(strBox)` 返回 `"Box[string]"`
> - 嵌套泛型也正确显示：`type(outer)` 返回 `"Box[Box[int]]"`
> - 泛型不会产生代码膨胀，零运行时开销

> **⚠️ 注意：泛型 struct 与普通 struct 的区别**
>
> | 特性 | 普通 struct | 泛型 struct |
> |------|-----------|------------|
> | 定义 | `struct Point { ... }` | `struct Box[T] { ... }` |
> | 实例化 | `new Point(...)` | `new Box[int](...)`（类型参数不可省略） |
> | 类型注解 | `Point p` | `Box[int] p` |
> | 方法类型 | 固定 | 随类型参数变化 |
> | 字段类型 | 固定 | 由类型参数决定 |
> | `type()` 返回 | `"Point"` | `"Box[int]"`（含类型参数） |

### struct 完整示例

#### 学生管理系统

```leno
struct Student {
    string name = ""
    int age = 0
    Array[int] scores = []

    func averageScore():float {
        if (scores.len() == 0) {
            return 0.0
        }
        int total = 0
        for scores to score {
            total = total + score
        }
        return total / scores.len()
    }
}

struct Class {
    string name = ""
    Array[Student] students = []

    func addStudent(Student s) {
        students.add(s)
    }

    func getAverageAge():float {
        if (students.len() == 0) {
            return 0.0
        }
        int total = 0
        for students to s {
            total = total + s.age
        }
        return total / students.len()
    }
}

main() {
    var mathClass = new Class()
    mathClass.name = "数学一班"

    var s1 = new Student()
    s1.name = "张三"
    s1.age = 18
    s1.scores = [85, 90, 78]

    var s2 = new Student()
    s2.name = "李四"
    s2.age = 19
    s2.scores = [92, 88, 95]

    mathClass.addStudent(s1)
    mathClass.addStudent(s2)

    print("班级: " + mathClass.name)
    print("平均年龄: " + mathClass.getAverageAge())
    print("张三平均分: " + s1.averageScore())
}
```

#### 二叉搜索树

```leno
struct TreeNode {
    int value
    TreeNode left = null
    TreeNode right = null
}

struct BST {
    TreeNode root = null

    func insert(int val) {
        root = insertNode(root, val)
    }

    func insertNode(TreeNode node, int val):TreeNode {
        if (node == null) {
            var newNode = new TreeNode()
            newNode.value = val
            return newNode
        }
        if (val < node.value) {
            node.left = insertNode(node.left, val)
        } else if (val > node.value) {
            node.right = insertNode(node.right, val)
        }
        return node
    }

    func inorder() {
        inorderTraversal(root)
    }

    func inorderTraversal(TreeNode node) {
        if (node == null) {
            return
        }
        inorderTraversal(node.left)
        print(node.value)
        print(" ")
        inorderTraversal(node.right)
    }
}

main() {
    var tree = new BST()
    tree.insert(50)
    tree.insert(30)
    tree.insert(70)
    tree.insert(20)
    tree.insert(40)

    print("中序遍历: ")
    tree.inorder()    // 输出: 20 30 40 50 70
}
```

### struct 性能优化

LenoC 对 struct 字段访问进行了优化，编译期确定字段索引，运行时直接访问，避免线性搜索。

**性能数据（100万次访问）**：

| 访问方式 | 耗时 | 说明 |
|---------|------|------|
| `p.y` | ~47ms | O(1) 直接索引访问 |
| `p["y"]` | ~62ms | O(n) 字符串查找 |

**优化建议**：

1. **优先使用字段访问** `obj.field` 而不是 `obj["field"]`
2. **在方法内直接访问字段**，编译器会自动优化为直接索引
3. **字段数量无性能担忧**，即使 struct 有几十个字段，访问速度仍然是 O(1)

**字段较多时的性能对比**：

```leno
struct BigStruct {
    int field0
    int field1
    // ... 更多字段
    int field19
}

main() {
    var obj = new BigStruct(field19 = 100)

    // 访问第 20 个字段：
    // - 字段访问：O(1)，直接定位
    // - 索引访问：O(n)，需要遍历比较字段名
    var val = obj.field19
}
```

### struct 常见问题

**Q: 为什么自引用 struct 必须设 null 默认值？**

A: 否则会无限递归创建实例，导致栈溢出：

```leno
// ❌ 错误
struct Node {
    Node next    // 创建 Node 时要创建 next，创建 next 时要创建 next.next...
}

// ✅ 正确
struct Node {
    Node next = null    // 默认为 null，停止递归
}
```

**Q: 为什么 var 参数不能访问 struct 字段？**

A: `var` 参数类型为 `any`，编译器不知道具体类型，无法生成字段访问指令：

```leno
func test(var list) {
    list.head = null    // ❌ 编译器不知道 list 是什么类型
}

// 应该使用具体类型
func test(LinkedList list) {
    list.head = null    // ✅ 编译器知道是 LinkedList
}
```

**Q: 如何检查 null 字段？**

A: 使用 `!= null` 或 `== null` 检查：

```leno
struct Container {
    dict data = null
}

func safeAccess(Container c) {
    if (c.data != null) {
        // 安全访问
        var v = c.data["key"]
    }
}
```

**Q: 未定义的 struct 类型会报什么错？**

A: 编译器会提示未定义的 struct 类型名：

```leno
UndefinedType x    // [未定义变量] 未定义的 struct 类型: UndefinedType
```

**Q: 类型名拼写错误会怎样？**

A: 编译器会提示未定义的 struct 类型：

```leno
dic13412t a    // [未定义变量] 未定义的 struct 类型: dic13412t
arry b         // [未定义变量] 未定义的 struct 类型: arry

// ✅ 正确的类型名
Dict d
Array arr
```

**struct 特性总结：**

| 特性 | 说明 |
|------|------|
| 定义 | `struct 名称 { 类型 字段 = 默认值 }` |
| 创建 | `var obj = new StructName()` |
| 访问 | `obj.field`（推荐）或 `obj["field"]` |
| 方法 | 在 struct 内定义，直接访问字段 |
| 嵌套 | 支持，自引用必须设 null 默认值 |
| null 检查 | 编译时警告访问可能为 null 的字段 |
| face | `face 名称 { func 签名 }` 定义接口 |
| impl | `struct 名称 impl Face1, Face2` 显式声明（必须，名义类型） |
| impl 检查 | 编译期检查方法缺失、返回类型、参数数量 |
| 多态 | face 参数/变量只接受显式 impl 的 struct |
| 类型守卫 | `if obj is FaceName` 运行时检查是否 impl 了 face |
| 安全转型 | `var c = obj as StructName` 匹配返回原值，不匹配返回 null |
| 数组推断 | 不同 struct 有公共 face 时推断为 Array[FaceName] |
| 跨模块 face | import 模块中的 face 可用于 impl、参数、变量 |
| 性能 | 字段索引编译期确定，O(1) 访问 |
| use 导入 | 支持 `use module.Struct` 和 `use module.Face` 导入 |

> **核心原则**：自引用设 null，访问先检查，参数用具体类型或 face，优先用字段访问！

***

## face（接口）

face 用于定义方法签名契约，让不同 struct 实现相同的行为，实现多态。Leno 采用**名义类型（nominal typing）**——struct 必须**显式声明 `impl`** 才算实现 face。

### 定义 face

```leno
face 名称 {
    func 方法名(参数列表):返回类型
    ...
}
```

face 体只含方法签名，不含字段、不含方法体：

```leno
face Speaker {
    func speak():string
}

face Writer {
    func write(string content)
    func flush()
}
```

### 泛型 face

face 也支持泛型类型参数，在名称后添加 `[T, U, ...]`：

```leno
face Comparable[T] {
    func compareTo(T other): int
}

face Convertible[T, U] {
    func convert(T input): U
}
```

struct 实现泛型 face 时，方法签名中的泛型参数会被替换为具体类型：

```leno
face Comparable[T] {
    func compareTo(T other): int
}

struct IntBox impl Comparable[int] {
    int value

    func compareTo(int other): int {
        if self.value < other { return -1 }
        if self.value > other { return 1 }
        return 0
    }
}

var box = new IntBox(value=42)
print(box.compareTo(10))    // 1
print(box.compareTo(42))    // 0
print(box.compareTo(100))   // -1
```

> **💡 泛型 face 的类型参数**
>
> - 泛型 face 的类型参数在 struct `impl` 时确定具体类型
> - 方法签名中的泛型参数（如 `T other`）会被替换为 impl 声明中的具体类型
> - 同一个 face 可以被不同 struct 以不同类型参数实现（如 `impl Comparable[int]`、`impl Comparable[string]`）
> - **泛型 face 的约束检查完全支持**：`func f[T: Comparable](T a, T b)` 可以约束 T 为实现了 `Comparable` 的类型
>
> ```leno
> face Comparable[T] {
>     func compareTo(T other): int
> }
>
> struct OrdInt impl Comparable[int] {
>     int value
>     func compareTo(int other): int { ... }
> }
>
> func maxBy[T: Comparable](T a, T b): T {
>     // T: Comparable 约束确保 a 和 b 都实现了 Comparable
>     return a
> }
>
> var a = new OrdInt(value=10)
> var r = maxBy(a, a)  // ✅ T=OrdInt, OrdInt impl Comparable[int]
> ```

### struct 实现 face

struct 必须**显式声明 `impl`** 才算实现 face，仅方法签名匹配不算实现：

```leno
face Speaker {
    func speak():string
}

// ✅ 正确：显式声明 impl
struct Dog impl Speaker {
    string name = ""
    func speak():string { return "woof" }
}

// ❌ 错误：没有 impl 声明，即使方法签名匹配也不能作为 Speaker 使用
struct Cat {
    func speak():string { return "meow" }
}
// Cat 有 speak():string 方法，但未声明 impl Speaker，不能传给 Speaker 参数
```

**为什么必须显式 impl？** 防止意外满足接口：

```leno
face Serializable { func serialize():string }

struct Password {
    string value = ""
    func serialize():string { return value }    // 只是碰巧签名相同
}

func save(Serializable s) { ... }

main() {
    var p = new Password()
    save(p)    // ❌ 编译错误：Password 未 impl Serializable
}
```

**多个 face 用逗号分隔：**

```leno
struct FileLogger impl Writer, Speaker {
    func write(string content) { }
    func flush() { }
    func speak():string { return "FileLogger" }
}
```

**impl 编译期检查：**

```leno
// ❌ 编译错误：声明了但缺少方法
struct Fish impl Speaker {
    func swim() { }    // 缺少 speak() 方法
}

// ❌ 编译错误：返回类型不匹配
struct BadSpeaker impl Speaker {
    func speak():int { return 42 }    // face 声明返回 string
}

// ❌ 编译错误：参数数量不匹配
face Adder {
    func add(int a, int b):int
}

struct BadAdder impl Adder {
    func add(int a):int { return a }    // face 声明 2 个参数，实际只有 1 个
}
```

**编译期 impl 检查规则：**

| 检查项 | 说明 | 示例 |
|--------|------|------|
| 方法缺失 | struct 必须实现 face 声明的所有方法 | `impl Speaker` 但没有 `speak()` → 报错 |
| 返回类型匹配 | 方法返回类型必须与 face 声明一致 | face 声明 `:int`，struct 返回 `:string` → 报错 |
| 参数数量匹配 | 方法参数数量（不含 self）必须与 face 一致 | face 声明 2 个参数，struct 只有 1 个 → 报错 |
| 跨模块 face | 导入模块的 face 同样适用以上检查 | `use pm.ProcessManager` 后 `impl ProcessManager` → 检查 |

**未 impl 但传给 face 参数时的错误提示：**

```leno
struct Dog {
    func speak():string { return "woof" }
}

func make_sound(Speaker s) { ... }

main() {
    var d = new Dog()
    make_sound(d)    // ❌ 编译错误：
    // make_sound 第 1 个参数类型不匹配: 期望 face Speaker, 实际 struct Dog
    // (struct 'Dog' 未实现 face 'Speaker'，请添加 impl: struct Dog impl Speaker { ... })
}
```

**空 face（标记接口）：**

face 可以不声明任何方法，作为标记接口使用：

```leno
face Empty {
}

struct Thing impl Empty {
    string name = ""
}

func check(Empty e) {
    print("got empty face")
}

main() {
    var t = new Thing()
    check(t)    // 输出: got empty face
}
```

### face 作为参数类型

```leno
func make_sound(Speaker s) {
    print(s.speak())
}

var d = new Dog()
make_sound(d)    // 输出: woof
```

### face 作为变量类型

```leno
Speaker s = new Dog()
print(s.speak())     // ✅ 可以调用 face 定义的方法

s = new Cat()        // ❌ 编译错误：Cat 未 impl Speaker
```

**face 变量的访问限制**：face 变量只能访问 face 中定义的方法，不能访问底层 struct 的字段或非 face 方法：

```leno
face Speaker {
    func speak():string
}

struct Dog impl Speaker {
    string name
    int age

    func speak():string { return "woof from " + self.name }
    func getAge():int { return self.age }
}

var d = new Dog(name="Rex", age=3)
Speaker s = d

print(s.speak())     // ✅ face 定义的方法
print(s.name)        // ❌ 编译错误：face 'Speaker' 没有字段或方法 'name'
print(s.getAge())    // ❌ 编译错误：face 'Speaker' 没有字段或方法 'getAge'

// 需要通过 as 转型访问底层 struct
var dog = s as Dog
print(dog.name)      // ✅ 转型后可以访问
print(dog.getAge())  // ✅ 转型后可以调用
```

> **💡 设计理念**：face 是编译期契约，不是运行时包装。类型擦除后底层就是 struct，但编译器会确保 face 变量只使用 face 契约中声明的成员。需要访问底层实现时，用 `as` 转型明确意图。

### face 类型守卫

使用 `is` 运算符在运行时检查 struct 是否实现了某个 face（仅检查显式 `impl` 声明）：

```leno
var obj = new Dog()
if obj is Speaker {
    print(obj.speak())     // 守卫内 obj 视为 Speaker 类型
}

// not is 否定检查
if obj not is Speaker {
    print("不是 Speaker")
}
```

**显式 impl vs 无 impl 的区别**：

```leno
face Speaker {
    func speak():string
}

// 显式声明 impl
struct Dog impl Speaker {
    func speak():string { return "woof" }
}

// 没有 impl 声明，即使方法签名匹配也不算实现
struct Duck {
    func speak():string { return "quack" }
}

main() {
    var d = new Dog()
    var duck = new Duck()

    if d is Speaker {
        print("Dog is Speaker")        // ✅ 输出
    }
    if duck is Speaker {
        print("Duck is Speaker")       // ❌ 不会输出（Duck 未 impl Speaker）
    }
}
```

**face 类型守卫速查表**：

| 表达式 | 条件 | 结果 |
|--------|------|------|
| `obj is Face` | struct 显式 `impl Face` | `true` |
| `obj is Face` | struct 未 `impl Face`（即使方法签名匹配） | `false` |
| `obj not is Face` | 上述条件的否定 | 相反结果 |
| `obj as StructName` | obj 是指定 struct | 原值（struct 实例） |
| `obj as StructName` | obj 不是指定 struct | `null` |
| `obj as Face` | obj 实现了指定 face（显式 impl） | 原值（struct 实例） |
| `obj as Face` | obj 未实现指定 face | `null` |

### as 安全类型转换

`as` 运算符用于安全向下转型，匹配返回原值，不匹配返回 `null`：

```leno
face Shape {
    func area():float
}

struct Circle impl Shape {
    float radius = 1.0
    func area():float { return 3.14159 * radius * radius }
}

func describe(Shape s) {
    var c = s as Circle
    if c != null {
        print("Circle radius=" + _str(c.radius))    // ✅ 安全访问 Circle 字段
    }
}
```

**`as` 与 `is` 的区别：**

| 特性 | `is` | `as` |
|------|------|------|
| 返回值 | `bool`（true/false） | 原值或 `null` |
| 用途 | 条件判断 | 安全转型 + null 检查 |
| 典型场景 | `if x is Dog { ... }` | `var d = s as Dog; if d != null { ... }` |

**`as` 与类型转换函数的区别：**

| 写法 | 语义 | 结果 |
|------|------|------|
| `42 as float` | 类型检查 | `null`（42 不是 float 实例） |
| `_float(42)` | 类型转换 | `42.0`（int→float 转换） |
| `"abc" as int` | 类型检查 | `null`（不匹配） |
| `_int("abc")` | 类型转换 | 运行时错误（解析失败） |

> **核心区别**：`as` 是安全类型检查，不做转换，不匹配返回 `null`；`_int()`/`_float()` 等是类型转换，转换失败抛出运行时错误。

### face 数组类型推断

当数组中包含多个实现了同一 face 的 struct 时，编译器自动推断为该 face 类型：

```leno
struct Dog impl Speaker {
    func speak():string { return "woof" }
}

struct Cat impl Speaker {
    func speak():string { return "meow" }
}

main() {
    // 同类型 struct → Array[Dog]
    var dogs = [new Dog(), new Dog()]
    print(type(dogs))    // Array[Dog]

    // 不同 struct，有公共 face → Array[Speaker]
    var animals = [new Dog(), new Cat()]
    print(type(animals))    // Array[Speaker]

    // 遍历时元素类型为 Speaker，可调用 face 方法
    for animals to s {
        print(s.speak())    // woof, meow
    }
}
```

**混合 struct 和 face 变量**：

```leno
main() {
    Speaker a = new Dog()    // face 类型变量
    var d = new Dog()        // struct 类型变量

    // face + struct → Array[Speaker]
    var mixed = [a, d]
    print(type(mixed))    // Array[Speaker]
}
```

**推断规则：**

| 数组元素 | 推断结果 | 说明 |
|---------|---------|------|
| `[Dog, Dog]` | `Array[Dog]` | 同类型，保持原类型 |
| `[Dog, Cat]`（都 impl Speaker） | `Array[Speaker]` | 不同 struct，找公共 face |
| `[Dog, Fish]`（Fish 无 impl） | `Array[any]` | 无公共 face，退化为 any |
| `[Speaker, Cat]` | `Array[Speaker]` | face + impl struct → face |
| `[Dog, Speaker]` | `Array[Speaker]` | struct + face → face |
| `[Speaker, Speaker]` | `Array[Speaker]` | face + face → face |

> **⚠️ 注意：** 数组类型推断基于**显式 impl 声明**。如果 struct 没有 `impl` 声明，即使方法签名匹配，也不会被推断为该 face 类型。

### face 不能实例化

```leno
Speaker s = new Speaker()    // ❌ 错误：face 不能实例化
```

### 完整示例

```leno
face Shape {
    func area():float
    func perimeter():float
}

struct Rectangle impl Shape {
    int width = 0
    int height = 0

    func area():float { return width * height }
    func perimeter():float { return 2 * (width + height) }
}

struct Circle impl Shape {
    int radius = 0

    func area():float { return 3.14159 * radius * radius }
    func perimeter():float { return 2 * 3.14159 * radius }
}

func print_shape_info(Shape s) {
    print($"面积: {s.area()}")
    print($"周长: {s.perimeter()}")
}

main() {
    var r = new Rectangle()
    r.width = 10
    r.height = 5

    var c = new Circle()
    c.radius = 3

    print_shape_info(r)
    print_shape_info(c)

    // 数组推断
    var shapes = [r, c]
    print(type(shapes))    // Array[Shape]
}
```

***

## 枚举（Enum）

Leno 的枚举（Enum）用于定义一组命名的常量值，底层存储为 `int` 类型。

### 定义枚举

```leno
// 基本枚举（自动从 0 开始赋值）
enum Color {
    red     // 0
    green   // 1
    blue    // 2
}

// 带显式值的枚举
enum Status {
    ok = 200
    not_found = 404
    error = 500
}

// 混合枚举（部分显式，部分自动递增）
enum Priority {
    low = 1
    medium  // 自动为 2
    high    // 自动为 3
}
```

### 使用枚举

```leno
main() {
    // 访问枚举值
    print(Color.red)        // 0
    print(Color.green)      // 1
    print(Status.ok)        // 200
    print(Priority.medium)  // 2
    
    // 枚举值比较
    var color = Color.green
    if color == Color.green {
        print("颜色是 green！")
    }
    
    // 枚举值在 switch 中使用
    var status = Status.ok
    switch status {
        case Status.ok {
            print("成功")
        }
        case Status.not_found {
            print("未找到")
        }
        default {
            print("其他错误")
        }
    }
}
```

> **⚠️ 注意：枚举值本质上是 int**
>
> ```leno
> var c = Color.red
> print(type(c))      // "int"
>
> // 可以将枚举值赋给 int 变量
> int color_code = Color.blue
>
> // 也可以直接与 int 比较
> if Color.red == 0 {
>     print("red 等于 0")
> }
> ```

### 枚举作为函数参数

```leno
func handle_status(int status) {
    if status == Status.ok {
        print("处理成功")
    } else if status == Status.error {
        print("处理失败")
    }
}

main() {
    handle_status(Status.ok)
}
```

### 模块中导出枚举

```leno
// color_module.leno
export enum Color {
    red
    green
    blue
}
```

```leno
// main.leno
import color_module

main() {
    print(color_module.Color.red)    // 0
}
```

***

## 数组

### 数组创建

```leno
main() {
    // 基本数组
    var arr1 = [1, 2, 3, 4, 5]

    // 泛型数组
    Array[int] arr2 = [1, 2, 3]
    Array[string] arr3 = ["a", "b", "c"]

    // 空数组（两种写法等价）
    var empty = []             // 类型为 Array（未指定，添加元素后锁定）
    Array[int] arr4            // 空的 Array[int]
    Array[int] arr5 = []       // 等同于上面

    // 二维数组
    Array[Array[int]] matrix = [[1, 2], [3, 4]]
}
```

> **⚠️ 注意：混合类型数组推断为** **`Array[any]`**
>
> ```leno
> var arr = [1, 2, 3]           // Array[int]
> var arr2 = [1, 2.0]           // Array[float]（int 升级为 float）
> var arr3 = [1, "hello"]       // Array[any]（混合类型，可以添加任何类型）
> ```

**数组类型推断规则：**

| 数组形式 | 推断类型 | 说明 |
| -------------------- | ------------ | ----------- |
| `[]` | `Array` | 空数组，元素类型未指定 |
| `[1, 2, 3]` | `Array[int]` | 同类型元素数组 |
| `[1, "hello", true]` | `Array[any]` | 混合类型数组 |

```leno
// 空数组：类型为 Array（未指定）
var arr = []
print(type(arr))    // "Array"
arr.add(1)          // 添加 int 后变为 Array[int]
print(type(arr))    // "Array[int]"
arr.add("hello")    // ❌ 错误：期望 int，传入 string

// 混合类型数组：类型为 Array[any]
var mixed = [1, "hello", true]
print(type(mixed))  // "Array[any]"
mixed.add(42)       // ✅ 可以添加 int
mixed.add("world")  // ✅ 可以添加 string
print(type(mixed))  // "Array[any]"（保持 any 类型）
```

**重要区别**：

- `Array`（空数组）：添加第一个元素后**更新**为具体类型
- `Array[any]`（混合数组）：保持 `any` 类型，**可以添加任何类型**

**建议**：尽量初始化时就明确元素类型：

```leno
var arr = [1,2,3]   // ✅ 明确为 Array[int]

// 或者直接声明空数组类型
Array[int] arr      // ✅ 空的 Array[int]
Array[int] arr2 = []   // ✅ 等同于上面
```

**数组类型检查覆盖：**

所有**修改**数组元素的操作都会进行类型检查：

| 操作 | 示例 | 类型检查 |
| -------- | ------------------ | -------- |
| `add` | `arr.add(1)` | ✅ 检查元素类型 |
| `insert` | `arr.insert(0, 1)` | ✅ 检查元素类型 |
| 索引赋值 | `arr[0] = 1` | ✅ 检查元素类型 |

```leno
var arr = []
arr.add(1)           // ✅ 类型变为 Array[int]
arr.insert(0, 2)     // ✅ 正确
arr[0] = 3           // ✅ 正确
arr.add("bad")       // ❌ 报错：期望 int
arr[0] = "bad"       // ❌ 报错：期望 int
```

**只读操作**（如 `len()`, `pop()`, `remove()`, `has()`, `copy()`）不影响类型检查。

### 数组访问与修改

```leno
main() {
    var arr = [1, 2, 3]

    // 索引访问
    print(arr[0])       // 1
    print(arr[1])       // 2

    // 修改元素
    arr[1] = 25
    print(arr)          // [1, 25, 3]

    // 交换元素（并行赋值）
    var arr2 = [10, 20]
    arr2[0], arr2[1] = arr2[1], arr2[0]
    print(arr2)         // [20, 10]
}
```

> **⚠️ 注意：数组是引用类型，赋值不会复制**
>
> ```leno
> var a = [1, 2, 3]
> var b = a          // b 和 a 指向同一个数组
> b[0] = 99
> print(a[0])        // 99（a 也被修改了）
>
> // 需要独立副本请使用 .copy()
> var c = a.copy()
> c[0] = 0
> print(a[0])        // 99（a 不受影响）
> ```

### 数组方法

```leno
main() {
    var arr = [1, 2, 3]

    // add: 添加元素到末尾
    arr.add(4)
    print(arr)          // [1, 2, 3, 4]

    // insert: 在指定位置插入
    arr.insert(1, 99)   // 在索引1处插入99
    print(arr)          // [1, 99, 2, 3, 4]

    // remove: 删除并返回指定位置的元素
    var removed = arr.remove(1)
    print(removed)      // 99
    print(arr)          // [1, 2, 3, 4]

    // pop: 弹出最后一个元素
    var last = arr.pop()
    print(last)         // 4

    // len: 获取长度
    print(arr.len())    // 3

    // has: 检查是否包含元素
    print(arr.has(2))   // true
    print(arr.has(99))  // false

    // copy: 复制数组（深拷贝）
    var arr2 = arr.copy()
    arr.add(100)
    print(arr)          // [1, 2, 3, 100]
    print(arr2)         // [1, 2, 3]（不受影响）

    // clear: 清空数组
    arr.clear()
    print(arr)          // []

    // map: 映射转换（返回新数组）
    var nums = [1, 2, 3, 4, 5]
    var doubled = nums.map(func(var x, var i) { return x * 2 })
    print(doubled)      // [2, 4, 6, 8, 10]

    // filter: 过滤元素（返回新数组）
    var evens = nums.filter(func(var x, var i) { return x % 2 == 0 })
    print(evens)        // [2, 4]

    // reduce: 累积计算
    var sum = nums.reduce(func(var acc, var x, var i) { return acc + x }, 0)
    print(sum)          // 15

    // 链式调用
    var result = nums.filter(func(var x, var i) { return x > 2 })
                     .map(func(var x, var i) { return x * 10 })
    print(result)       // [30, 40, 50]
}
```

### 数组切片

```leno
main() {
    Array[int] arr = [1, 2, 3, 4, 5, 6, 7, 8]

    // 切片语法: arr[start:end]（包含end，闭区间）
    print(arr[2:8])     // [3, 4, 5, 6, 7, 8]

    // 从开头切片
    print(arr[:3])      // [1, 2, 3]

    // 切到末尾
    print(arr[5:])      // [6, 7, 8]

    // 负数索引（从末尾计数）
    print(arr[-3:])     // [6, 7, 8]
    print(arr[:-3])     // [1, 2, 3, 4, 5]

    // 完整切片（复制数组）
    var copy = arr[:]
}
```

### 数组比较

数组可以使用 `==` 和 `!=` 进行比较：

```leno
main() {
    // 相等比较
    print([1, 2, 3] == [1, 2, 3])     // true
    print(["a", "b"] == ["a", "b"])   // true
    print([] == [])                    // true

    // 不相等比较
    print([1, 2, 3] == [1, 2, 4])     // false（值不同）
    print([1, 2, 3] == [1, 2])        // false（长度不同）
    print([1, 2, 3] != [1, 2, 4])     // true
}
```

> **⚠️ 注意：数组比较会比较每个元素**
>
> ```leno
> var a = [1, 2, 3]
> var b = [1, 2, 3]
> print(a == b)       // true（内容相同）
> print(a == [1, 2])  // false（长度不同）
> ```

### 多维数组

```leno
main() {
    // 二维数组
    Array[Array[int]] matrix = [
        [1, 2, 3],
        [4, 5, 6],
        [7, 8, 9]
    ]

    // 访问元素
    print(matrix[0][0])     // 1
    print(matrix[1][2])     // 6

    // 遍历二维数组
    for matrix to row {
        for row to element {
            print(element)
        }
    }

    // 三维数组
    Array[Array[Array[int]]] cube = [
        [[1, 2], [3, 4]],
        [[5, 6], [7, 8]]
    ]
    print(cube[0][1][0])    // 3
}
```

***

## 字典

### 字典创建

```leno
main() {
    // 基本字典（string 键）
    var dict = {name: "cheng", age: 25}

    // int 键字典
    var id_map = {1: "张三", 2: "李四", 3: "王五"}

    // 复杂字典（嵌套数组和字典）
    var user = {
        id: 1001,
        name: "张三",
        age: 25,
        email: "zhang@test.com",
        vip: true,
        tags: ["活跃", "高价值"],
        address: {
            city: "北京",
            street: "朝阳路"
        }
    }
}
```

**字典类型推断规则：**

| 字典形式 | 推断类型 | 说明 |
| -------- | -------- | ---- |
| `{}` | `Dict` | 空字典，键/值类型未指定 |
| `{"a": 1, "b": 2}` | `Dict[string, int]` | 字符串键，同类型值 |
| `{"a": 1, "b": "hello"}` | `Dict[string, any]` | 字符串键，混合类型值 |
| `{1: "one", 2: "two"}` | `Dict[int, string]` | 整数键，同类型值 |
| `{1: 100, "a": 200}` | `Dict[any, int]` | 混合键，同类型值 |

```leno
// 空字典：类型为 Dict（未指定）
var dict = {}
print(type(dict))    // "Dict"
dict.set("key", 1)   // 添加 string 键后变为 Dict[string, int]
print(type(dict))    // "Dict[string, int]"
dict.set("key2", "hello")  // ❌ 错误：期望 int，传入 string

// 混合类型字典：类型为 Dict[string, any]
var mixed = {"a": 1, "b": "hello", "c": true}
print(type(mixed))   // "Dict[string, any]"
mixed.set("d", 42)   // ✅ 可以添加 int
mixed.set("e", "world")  // ✅ 可以添加 string
print(type(mixed))   // "Dict[string, any]"（保持 any 类型）

// int 键字典：类型为 Dict[int, string]
var int_keys = {1: "one", 2: "two"}
print(type(int_keys))  // "Dict[int, string]"
int_keys.set(3, "three")   // ✅ 可以添加 int 键
int_keys.set("4", "four")  // ❌ 错误：期望 int 键，传入 string
```

**重要区别**：

- `Dict`（空字典）：添加第一个值后**更新**为具体类型
- `Dict[string, any]`（混合字典）：保持 `any` 类型，**可以添加任何类型值**

**建议**：尽量初始化时就明确键和值类型：

```leno
var dict = {"a": 1, "b": 2}      // ✅ 明确为 Dict[string, int]
var id_map = {100: "张三", 200: "李四"}  // ✅ 明确为 Dict[int, string]

// 或者直接声明空字典类型
Dict[string, int] dict           // ✅ 空的 Dict[string, int]
Dict[int, string] id_map2 = {}   // ✅ 空的 Dict[int, string]
```

**字典类型检查覆盖：**

所有**修改**字典值的操作都会进行类型检查：

| 操作 | 示例 | 类型检查 |
| ---- | ---- | -------- |
| `set` | `dict.set("key", 1)` | ✅ 检查值类型 |
| 索引赋值 | `dict["key"] = 1` | ✅ 检查值类型 |
| 点号赋值 | `dict.key = 1` | ✅ 检查值类型 |

```leno
var dict = {}
dict.set("a", 1)     // ✅ 类型变为 Dict[string, int]
dict["b"] = 2        // ✅ 正确
dict.c = 3           // ✅ 正确（点号访问）
dict.set("d", "bad") // ❌ 报错：期望 int
dict["e"] = "bad"    // ❌ 报错：期望 int
```

**只读操作**（如 `len()`, `has()`, `keys()`, `copy()`）不影响类型检查。

### 字典访问与修改

```leno
main() {
    var user = {
        name: "张三",
        age: 25
    }

    // 索引访问
    print(user["name"])             // 张三

    // 点号访问
    print(user.name)                // 张三

    // 索引修改（也可以添加新键）
    user["age"] = 26
    user["email"] = "test@test.com"

    // 点号修改
    user.name = "李四"

    // set 方法
    user.set("city", "上海")
}
```

**int 键字典：**

```leno
main() {
    // 创建 int 键字典
    var id_map = {1: "张三", 2: "李四", 3: "王五"}

    // 索引访问（int 键）
    print(id_map[1])        // 张三
    print(id_map[2])        // 李四

    // 索引修改
    id_map[1] = "张三丰"
    id_map[4] = "赵六"      // 添加新键

    // set 方法
    id_map.set(5, "孙七")

    // has 检查
    print(id_map.has(3))    // true
    print(id_map.has(99))   // false

    // remove 删除
    id_map.remove(2)

    // 遍历 int 键字典
    for id_map to id, name {
        print($"{id}: {name}")
    }
}
```

> **⚠️ 注意：`[]`** **和** **`.`** **都可以添加新键**
>
> ```leno
> var d = {name: "cheng"}
> d["age"] = 25         // ✅ 添加新键
> d.email = "a@b.com"   // ✅ 添加新键
> // 不需要先检查键是否存在，直接赋值即可
> ```

> **⚠️ 注意：字典保持插入顺序**
>
> ```leno
> var dict = {c: 3, a: 1, b: 2}
> for dict to key, value {
>     print($"{key}: {value}")
> }
> // 输出顺序: c:3, a:1, b:2（按插入顺序，不是按字母排序）
> ```

### 字典方法

```leno
main() {
    var dict = {name: "cheng", age: 25}

    // has: 检查键是否存在
    print(dict.has("name"))     // true
    print(dict.has("email"))    // false

    // keys: 获取所有键
    var keys = dict.keys()
    print(keys)                  // [name, age]

    // values: 获取所有值
    var values = dict.values()
    print(values)                // [cheng, 25]

    // len: 获取长度
    print(dict.len())           // 2

    // remove: 删除键值对
    dict.remove("age")
    print(dict)                  // {name: "cheng"}

    // copy: 复制字典
    var dict2 = dict.copy()

    // clear: 清空字典
    dict.clear()
    print(dict)                  // {}
}
```

> **⚠️ 注意：字典也是引用类型**
>
> ```leno
> var a = {name: "cheng"}
> var b = a          // b 和 a 指向同一个字典
> b.name = "li"
> print(a.name)      // "li"（a 也被修改了）
> // 需要独立副本请使用 .copy()
> ```

### `set()` 与 `d[key] = value` 性能对比

| 特性 | `d.set(key, value)` | `d[key] = value` |
|------|:---:|:---:|
| 性能（10万次） | ~150ms | **~100ms**（快 1.5 倍） |
| 推荐 | - | ✅ 更快更直观 |

> **建议**：优先用 `d["key"] = value`，性能更好且直观。`set()` 主要用于方法引用场景。

### 嵌套类型

数组和字典可以互相嵌套，类型精确推断：

```leno
// 数组包含字典
var arrDict = [{"name": "a"}, {"name": "b"}]
print(type(arrDict))  // Array[Dict[string, string]]

// 字典包含数组
var dictArr = {"items": [1, 2, 3]}
print(type(dictArr))  // Dict[string, Array[int]]

// int 键字典包含数组
var id_items = {1: [10, 20], 2: [30, 40]}
print(type(id_items))  // Dict[int, Array[int]]

// 深层嵌套字典
var deep = {"level1": {"level2": {"level3": "value"}}}
print(type(deep))     // Dict[string, Dict[string, Dict[string, string]]]

// 显式声明嵌套类型
Dict[string, Dict[string, int]] nested = {
    level1: {a: 1, b: 2}
}
nested.set("level2", {c: 3})     // ✅ 正常
nested.set("level3", {d: "bad"}) // ❌ 错误：期望 Dict[string, int]
```

***

## 字符串

### 字符串基础

```leno
main() {
    // 字符串声明
    var s1 = "Hello, Leno!"

    // 字符串拼接
    var greeting = "Hello" + " " + "World"

    // 字符串插值（使用 $"..."）
    var name = "Leno"
    var version = 1.0
    print($"Welcome to {name} version {version}!")

    // 访问单个字符（Unicode 字符索引）
    var str = "cheng8214"
    print(str[0])           // "c"
    print(str[4])           // "8"

    // Unicode 字符串索引
    var cn = "你好世界"
    print(cn[0])            // "你"
    print(cn.len())         // 4（4个字符）
    print(cn.byte_len())    // 12（4个中文字×3字节）

    // 遍历字符
    for str to a {
        print(a)        // 逐个输出字符
    }
}
```

> **Unicode 索引说明**：LenoC 字符串按 **Unicode 字符** 索引，而非 UTF-8 字节。`len()` 返回字符数，`byte_len()` 返回字节数。这与 Python 3、Java 等现代语言一致。

### 字符索引 vs 字节索引（重要）

LenoC 字符串内部使用 UTF-8 编码，但对外提供**两套 API**：按字符操作和按字节操作。理解这个区别对正确处理中文和二进制数据至关重要。

#### 字符级 API（日常使用）

以下方法按 **Unicode 字符** 工作，适合处理文本：

| 方法 | 说明 | 示例 |
|------|------|------|
| `len()` | 返回字符数 | `"你好".len()` → 2 |
| `str[i]` | 按字符索引 | `"你好"[0]` → "你" |
| `slice(start, end)` | 按字符位置切片 | `"你好世界".slice(0,2)` → "你好" |
| `sub_str(start, len)` | 按字符位置截取 | `"你好世界".sub_str(1,2)` → "好世" |
| `reverse()` | 按字符反转 | `"你好".reverse()` → "好你" |
| `find(sub)` | 返回字符位置 | `"你好世界".find("世界")` → 2 |

#### 字节级 API（二进制数据处理）

以下方法按 **UTF-8 字节** 工作，适合处理加密、协议等二进制数据：

| 方法 | 说明 | 示例 |
|------|------|------|
| `byte_len()` | 返回字节数 | `"你好".byte_len()` → 6 |
| `byte(i)` | 获取某字节的值(0-255) | `"A".byte(0)` → 65 |
| `byte_slice(start, end)` | 按字节位置切片 | `"你好".byte_slice(0,3)` → "你" |

#### 何时用 `slice` 何时用 `byte_slice`

```leno
main() {
    var text = "你好世界"

    // ✅ 处理文本：用 slice（按字符）
    print(text.slice(0, 2))        // "你好"
    print(text.sub_str(1, 2))      // "好世"

    // ✅ 处理二进制数据：用 byte_slice（按字节）
    // 例如：加密解密后去除 PKCS7 填充
    var padded = some_decrypt_result
    var unpadded = padded.byte_slice(0, padded.byte_len() - pad_len)

    // ❌ 错误示范：对二进制数据用 slice
    // 二进制数据中可能包含多字节字符的片段，用字符索引会出错
    // padded.slice(0, padded.len() - pad_len)  // 可能截断到字符中间！
}
```

#### 注意事项

1. **ASCII 字符串无区别**：纯英文/数字时，`len()` == `byte_len()`，`slice()` == `byte_slice()`，用哪个都行。

2. **中文等多字节字符**：一个中文字符占 3 个 UTF-8 字节，`len()` 和 `byte_len()` 结果不同：
   ```leno
   var s = "你好ab"
   print(s.len())       // 4（4个字符）
   print(s.byte_len())  // 8（2×3 + 2×1 = 8字节）
   print(s[0])          // "你"
   print(s[2])          // "a"
   ```

3. **加密/解密等二进制操作必须用字节级 API**：加密算法操作的是原始字节，填充长度、偏移量都是字节数，必须使用 `byte_len()`、`byte_slice()`、`byte()`。

4. **`for str to ch` 遍历按字符**：遍历字符串时，每次迭代得到一个完整的 Unicode 字符，不是字节。

5. **`strings.char()` 创建单字节字符**：`strings.char(65)` 创建 "A"，`strings.char(0xE4)` 创建单个字节，适合构造二进制数据。

```

### 原始字符串

使用 `@"..."` 创建原始字符串，不处理转义：

```leno
main() {
    var test_code = @"x = 90;
y = 20;
sum = x - y;
print(sum);
print(""Hello, TinyLang!"");"

    print(test_code)
}
```

### 字符串方法

```leno
main() {
    var s = "Hello World"

    // 长度（字符数）
    print(s.len())           // 11（字符数）
    print(s.byte_len())      // 11（字节数，纯ASCII时与len相同）

    // Unicode 字符串长度
    var cn = "你好世界"
    print(cn.len())          // 4（4个字符）
    print(cn.byte_len())     // 12（4个中文字×3字节）

    // 大小写转换
    print(s.to_upper())     // HELLO WORLD
    print(s.to_lower())     // hello world

    // 修剪空白
    var s2 = "  hello world  "
    print(s2.trim())        // 'hello world'
    print(s2.trim_start())  // 'hello world  '
    print(s2.trim_end())    // '  hello world'

    // 包含检查
    var s3 = "hello.txt"
    print(s3.starts_with("hello"))  // true
    print(s3.ends_with(".txt"))     // true

    // 替换
    var s4 = "hello world hello"
    print(s4.replace("hello", "hi"))    // hi world hi

    // 子串提取（字符索引）
    var s5 = "hello world"
    print(s5.slice(0, 5))       // hello
    print(s5.sub_str(0, 5))     // hello
    print(s5.sub_str(6, 5))     // world

    // Unicode 子串提取
    var cn2 = "你好世界hello"
    print(cn2.slice(0, 2))       // "你好"
    print(cn2.sub_str(2, 2))    // "世界"

    // 字节级切片（用于二进制数据处理）
    var bin = "你好"
    print(bin.byte_slice(0, 3))  // "你"（取前3个字节=1个中文字符）
    print(bin.byte_slice(3, 6))  // "好"（取第3-6字节=第2个中文字符）
}
```

***

## 模块系统

### 导入模块

```leno
// 基本导入
import io
import times

// 带别名导入
import times as ti
import rands

main() {
    // 使用别名
    var start = ti.ms()
    // ... 执行代码
    var end = ti.ms()
    print($"耗时: {end - start}ms")

    // 使用随机数模块
    var arr = rands.int_array(1, 100)
}
```

### 创建和导出模块

```leno
// math_utils.leno
export var PI = 3.14156

export func add(var a, var b) {
    return a + b
}

// 未导出的函数（模块私有）
func helper() {
    return "内部使用"
}
```

```leno
// main.leno
import "math_utils.leno" as math

main() {
    print(math.PI)
    print(math.add(1, 2))
}
```

### 导出类型别名（export alias）

可以用 `export alias` 在模块中导出类型别名，调用方导入后可直接用别名声明变量：

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
    Size s = 100              // Size 即 int
    IntList arr = [1, 2, 3]   // IntList 即 Array[int]
    StrDict d = {"k": "v"}    // StrDict 即 Dict[string,string]
}
```

### 使用 use 导入类型

`use` 可以将模块中的 struct/face 类型导入当前作用域，之后直接用类型名声明变量：

```leno
// shape.leno
export struct Point {
    int x = 0
    int y = 0
}

// main.leno
import "shape.leno" as shape
use shape.Point

main() {
    Point p = new shape.Point(x = 3, y = 4)
    print(p.x)    // 3
}
```

`use` 的类型可以**跨模块链式传导**，A→B→C→D 都能用到最底层 D 定义的类型，无需每层手动重导出。

`use` 也支持 **face 类型**导入：
```leno
// shape.leno
export face Shape {
    func area():float
}

// main.leno
import "shape.leno" as sm
use sm.Shape

func print_area(Shape s) {  // use 之后可直接用 face 名
    print(s.area())
}
```

**use 与 import 的区别：**

| 特性 | `import` | `use` |
|------|----------|-------|
| 作用 | 导入模块，通过模块名访问内容 | 将模块中的 struct/face 导入当前作用域 |
| 语法 | `import "路径" as 别名` | `use 别名.名称` |
| 适用范围 | 模块中的所有导出内容 | 仅 struct 和 face 类型 |
| 访问方式 | `别名.func()`、`new 别名.Struct()` | 直接使用类型名 `StructName` |

> **重要说明：**
> - `use` 只能导入 **struct** 和 **face** 类型
> - **func**、**var**、**enum** 必须通过模块名访问（如 `math.distance()`）
> - struct 实例仍需通过模块构造函数创建（如 `new math.Point(x=1, y=2)`）

### 导入泛型 struct 并调用方法

模块中导出的泛型 struct 可以通过 `use` 导入类型，然后直接实例化和调用方法：

```leno
// container.leno
export struct Box[T] {
    T value

    func get(): T {
        return self.value
    }

    func add(T v): T {
        return self.value + v
    }
}
```

```leno
// main.leno
import "container.leno" as cm
use cm.Box

var intBox = new Box[int](value=42)
print(intBox.get())        // 42
print(intBox.add(8))       // 50

// 链式方法调用
var outer = new Box[Box[int]](value=intBox)
print(outer.get().get())   // 42

// type() 正确显示泛型类型
print(type(intBox))        // Box[int]
print(type(outer))         // Box[Box[int]]
```

> **💡 泛型 struct 方法调用要点**
>
> - `use` 导入泛型 struct 后，可以直接用 `new Box[int](...)` 实例化
> - 方法调用时，编译器会根据实例化的类型参数正确推断方法返回类型
> - 链式调用（如 `outer.get().get()`）每一步的返回类型都能正确推断
> - `type()` 返回带类型参数的名称（如 `"Box[int]"`），嵌套泛型也能正确显示

### io 模块使用

```leno
import io

main() {
    print("Hello LenoLang")     // 全局 print
    io.print("通过io模块访问")   // 模块方式访问
}
```

### times 时间模块

```leno
import times
import times as ti  // 常用别名

main() {
    // 获取当前时间戳（毫秒）
    var start = ti.ms()

    // 执行一些代码...
    for 1000000 {}  // 模拟耗时操作

    var end = ti.ms()
    print($"耗时: {end - start}ms")
}
```

### rands 随机数模块

```leno
import rands

main() {
    // 生成 0-1 之间的随机小数
    var r1 = rands.om()

    // 生成指定范围内的随机整数 [min, max]
    var dice = rands.ints(1, 6)       // 掷骰子：1-6

    // 生成指定范围内的随机小数
    var r2 = rands.floats(0.0, 100.0)

    // 随机布尔值（指定概率）
    var coin = rands.bools(0.5)       // 50% 概率为 true

    // 从数组中随机选择元素
    var arr = ["苹果", "香蕉", "橙子"]
    var fruit = rands.choice(arr)

    // 从数组中随机采样 n 个元素
    var numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
    var sample = rands.sample(numbers, 3)  // 随机选3个

    // 随机打乱数组（返回新数组）
    var shuffled = rands.shuffle(numbers)

    // 生成指定长度的随机字符串
    var code = rands.str(8, "abcdef123456")  // 8位随机字符

    // 生成指定范围的随机排列（1到n的随机排列）
    var permutation = rands.int_array(1, 10)  // [1..10] 的随机排列

    // 设置随机种子（用于可重复的随机序列）
    rands.seed(12345)
    var seq1 = rands.ints(1, 100)
    rands.seed(12345)  // 重置种子
    var seq2 = rands.ints(1, 100)  // seq1 == seq2
}
```

### maths 数学模块

```leno
import maths

main() {
    // 基本运算
    print(maths.sqrt(16))       // 4.0 - 平方根
    print(maths.abs(-5))        // 5 - 绝对值
    print(maths.pow(2, 3))      // 8 - 幂运算

    // 取整函数
    print(maths.round(3.7))     // 4 - 四舍五入
    print(maths.ceil(3.2))      // 4 - 向上取整
    print(maths.floor(3.8))     // 3 - 向下取整
    print(maths.trunc(3.9))     // 3 - 截断小数

    // 三角函数（弧度制）
    print(maths.sin(0))         // 0.0
    print(maths.cos(0))         // 1.0
    print(maths.tan(0))         // 0.0
    print(maths.asin(0))        // 0.0 - 反正弦
    print(maths.acos(1))        // 0.0 - 反余弦
    print(maths.atan(0))        // 0.0 - 反正切
    print(maths.atan2(1, 1))    // 0.785... - 四象限反正切

    // 对数和指数
    print(maths.log(2.718))     // 自然对数
    print(maths.log10(100))     // 2 - 常用对数
    print(maths.exp(1))         // 2.718... - e^x

    // 工具函数
    print(maths.max(3, 7, 2))   // 7 - 最大值
    print(maths.min(3, 7, 2))   // 2 - 最小值
    print(maths.deg(3.14159))   // 180 - 弧度转角度
    print(maths.rad(180))       // 3.14159... - 角度转弧度
    print(maths.sign(-5))       // -1 - 符号函数

    // 常量
    print(maths.pi())           // 3.14159...
    print(maths.e())            // 2.71828...

    // 数字实例方法（无需导入 maths）
    var num = 16
    print(num.sqrt())           // 4.0
    print((-5).abs())           // 5
    print(2.pow(3))             // 8
    print(3.7.round())          // 4
    print(3.2.ceil())           // 4
    print(3.8.floor())          // 3
}
```

### files 文件操作模块

```leno
import files

main() {
    // 简单读写（适合小文件）
    files.write("data.txt", "Hello, World!")
    var content = files.read("data.txt")
    print(content)

    // 检查文件是否存在
    if files.exists("data.txt") {
        print("文件存在")
    }

    // 删除文件
    files.delete("data.txt")

    // 打开文件进行复杂操作
    // 模式: "r"读, "w"写, "a"追加, "rb"二进制读, "wb"二进制写
    var f = files.open("data.txt", "w")
    f.write("Line 1\n")
    f.write("Line 2\n")
    f.close()

    // 读取文件
    var f2 = files.open("data.txt", "r")
    var text = f2.read()       // 读取全部内容
    f2.close()

    // 二进制文件操作（适合大文件）
    var src = files.open("source.zip", "rb")
    var dst = files.open("copy.zip", "wb")

    // 获取文件大小
    var size = src.len()

    // 读取并写入（适合小文件）
    var data = src.read()
    dst.write(data)

    src.close()
    dst.close()
}
```

> **⚠️ 注意：文件操作后必须调用 close()**
>
> ```leno
> var f = files.open("data.txt", "w")
> f.write("content")
> f.close()  // 不要忘记关闭
> ```

### jsons JSON 模块

```leno
import jsons

main() {
    // 解析 JSON 字符串
    var obj = jsons.decode('{"name": "Leno", "version": 1.0}')
    print(obj.name)         // Leno
    print(obj.version)      // 1.0
    
    // 解析数组
    var arr = jsons.decode('[1, 2, 3]')
    print(arr[0])           // 1
    
    // 编码为 JSON
    var data = {name: "测试", count: 42}
    var json_str = jsons.encode(data)
    print(json_str)         // {"name":"测试","count":42}
    
    // 美化输出
    var pretty = jsons.encode_pretty(data)
    print(pretty)
    // {
    //     "name": "测试",
    //     "count": 42
    // }
    
    // 文件操作
    jsons.write_file("data.json", data)
    var loaded = jsons.read_file("data.json")
    print(loaded.name)      // 测试
}
```

### sockets 网络模块

```leno
import sockets

main() {
    // TCP 连接到服务器
    var conn = sockets.connect("www.example.com", 80)
    
    if conn != null {
        // 发送 HTTP 请求
        var request = "GET / HTTP/1.0\r\nHost: www.example.com\r\n\r\n"
        sockets.send(conn, request)
        
        // 接收响应
        var response = sockets.recv(conn, 1024)
        print(response)
        
        // 关闭连接
        sockets.close(conn)
    }
}
```

### dirs 目录操作模块

```leno
import dirs

main() {
    // 获取当前工作目录
    var cwd = dirs.cwd()
    print(cwd)
    
    // 检查路径是否存在
    if dirs.exists("data.txt") {
        print("文件存在")
    }
    
    // 判断是文件还是目录
    print(dirs.is_file("data.txt"))     // true/false
    print(dirs.is_dir("/tmp"))          // true/false
    
    // 获取文件信息
    var info = dirs.stat("data.txt")
    print(info["size"])         // 文件大小
    print(info["mtime"])        // 修改时间
    print(info["is_dir"])       // 是否是目录
    
    // 创建和删除目录
    dirs.mkdir("new_folder")
    dirs.rmdir("new_folder")
    
    // 列出目录内容
    var entries = dirs.listdir(".")
    for entries to entry {
        print(entry)
    }
    
    // 路径操作
    print(dirs.sep())                   // 路径分隔符 (\ 或 /)
    print(dirs.join("a", "b", "c"))     // a/b/c
    print(dirs.basename("/home/user/file.txt"))  // file.txt
    print(dirs.dirname("/home/user/file.txt"))   // /home/user
    print(dirs.extname("file.txt"))              // .txt
}
```

### strings 字符串处理模块

```leno
import strings

main() {
    // 字符串反转
    print(strings.reverse("abc"))       // cba
    
    // 重复字符串
    print(strings.rep("ha", 3))         // hahaha
    
    // 获取字符 ASCII 码
    print(strings.byte("Hello", 0))     // 72 (H)
    print(strings.byte("Hello", -1))    // 111 (o, 负数索引)
    
    // 从 ASCII 码创建字符串
    print(strings.char(72, 101, 108, 108, 111))  // Hello
    
    // 查找子串位置（0-indexed 字符索引，未找到返回 -1）
    var text = "Hello, World! Hello!"
    print(strings.find(text, "Hello"))      // 0
    print(strings.find(text, "Hello", 1))   // 14 (从位置1开始找)
    print(strings.find(text, "xyz"))        // -1 (未找到)

    // Unicode 字符串查找
    var cn = "你好世界"
    print(strings.find(cn, "世界"))         // 2（字符索引）
    print(strings.len(cn))                   // 4（字符数）
    print(strings.byte_len(cn))              // 12（字节数）

    // 字节级切片（用于二进制数据处理）
    print(strings.byte_slice(cn, 0, 3))     // "你"（取前3个字节）
    print(strings.byte_slice(cn, 3, 6))     // "好"（取第3-6字节）
}
```

#### format 格式化函数

`format()` 是全局函数，无需导入模块即可使用，支持类似 C 语言 printf 的格式说明符。

**基本语法**：`format(格式字符串, 参数1, 参数2, ...)`

格式字符串中的 `%X` 称为**格式占位符**，每个占位符按顺序对应后面的一个参数。

##### 格式说明符一览

| 说明符 | 说明 | 示例 |
|--------|------|------|
| `%s` | 字符串 | `format("Hello %s", "World")` → `"Hello World"` |
| `%d` / `%i` | 整数 | `format("Age: %d", 25)` → `"Age: 25"` |
| `%u` | 无符号整数 | `format("%u", 42)` → `"42"` |
| `%f` | 浮点数 | `format("Pi = %f", 3.14)` → `"Pi = 3.140000"` |
| `%e` / `%E` | 科学计数法 | `format("%e", 1234.5)` → `"1.234500e+03"` |
| `%g` / `%G` | 自动选择 %f 或 %e | `format("%g", 1234.5)` → `"1234.5"` |
| `%c` | 字符（ASCII码） | `format("Char: %c", 65)` → `"Char: A"` |
| `%x` / `%X` | 十六进制 | `format("Hex: %x", 255)` → `"Hex: ff"` |
| `%o` | 八进制 | `format("Oct: %o", 8)` → `"Oct: 10"` |
| `%b` | 二进制 | `format("Bin: %b", 10)` → `"Bin: 1010"` |
| `%t` | 布尔值（true/false） | `format("Flag: %t", true)` → `"Flag: true"` |
| `%%` | 百分号（转义） | `format("100%%")` → `"100%"` |

##### 宽度与对齐

在 `%` 和格式字母之间插入数字，可以控制输出的最小宽度：

```leno
main() {
    // 右对齐（默认），宽度10
    format("%10s", "hi")      // "        hi"
    
    // 左对齐，用 - 标志
    format("%-10s", "hi")     // "hi        "
    
    // 数值右对齐
    format("%5d", 42)         // "   42"
    
    // 数值左对齐
    format("%-5d", 42)        // "42   "
    
    // 零填充（仅数值类型）
    format("%05d", 42)        // "00042"
}
```

##### 精度控制

用 `.数字` 控制浮点数的小数位数或字符串的截取长度：

```leno
main() {
    // 浮点数保留2位小数
    format("%.2f", 3.14159)   // "3.14"
    
    // 浮点数保留0位小数
    format("%.0f", 3.14159)   // "3"
    
    // 宽度 + 精度组合
    format("%8.2f", 3.14)     // "    3.14"
    
    // 字符串截取前3个字符
    format("%.3s", "Hello")   // "Hel"
    
    // 科学计数法精度
    format("%.2e", 1234.5)    // "1.23e+03"
}
```

##### 标志位

| 标志 | 含义 | 示例 |
|------|------|------|
| `-` | 左对齐（默认右对齐） | `format("%-10s", "hi")` → `"hi        "` |
| `0` | 零填充（数值类型） | `format("%05d", 42)` → `"00042"` |
| `+` | 显示正号 | `format("%+d", 42)` → `"+42"` |
| ` ` | 正数前加空格 | `format("% d", 42)` → `" 42"` |
| `#` | 替代形式 | `format("%#x", 255)` → `"0xff"` |

`#` 标志的替代形式：
- `%#x` → 加 `0x` 前缀（如 `0xff`）
- `%#X` → 加 `0X` 前缀（如 `0xFF`）
- `%#o` → 加 `0` 前缀（如 `010`）
- `%#b` → 加 `0b` 前缀（如 `0b1010`）

##### 完整格式语法

格式占位符的完整语法为：

```
%[标志][宽度][.精度]格式字母
```

例如 `%+08.2f` 表示：显示正号(`+`)、零填充(`0`)、最小宽度8、保留2位小数(`.2`)、浮点数(`f`)。

```leno
main() {
    format("%+08.2f", 3.14)   // "+0003.14"
    format("%-10.3s", "Hello")  // "Hel       "
    format("%#010x", 255)     // "0x000000ff"
}
```

##### 类型自动转换

`%s` 可以接受非字符串参数，会自动转换：

```leno
main() {
    format("Value: %s", 42)      // "Value: 42"（int → string）
    format("Value: %s", 3.14)    // "Value: 3.14"（float → string）
    format("Value: %s", true)    // "Value: true"（bool → string）
}
```

如果类型不匹配（如 `%d` 传入字符串），输出 `<type_error>`；参数不足时输出 `<missing>`。

##### 常见用法示例

```leno
main() {
    // 对齐表格
    print(format("%-10s %5d", "Apple", 5))
    print(format("%-10s %5d", "Banana", 12))
    // Apple         5
    // Banana        12
    
    // 格式化金额
    var price = 9.99
    print(format("Price: $%.2f", price))   // Price: $9.99
    
    // 进制转换
    var num = 255
    print(format("Dec: %d", num))          // Dec: 255
    print(format("Hex: %x", num))          // Hex: ff
    print(format("Oct: %o", num))          // Oct: 377
    print(format("Bin: %b", num))          // Bin: 11111111
    
    // 布尔值
    var active = true
    print(format("Status: %t", active))    // Status: true
    
    // 百分号转义
    print(format("Progress: %d%%", 85))    // Progress: 85%
}
```

### 模块高级用法

#### 循环依赖

Leno 支持模块间的循环依赖：

```leno
// module_a.leno
import "module_b.leno" as b

export var a_value = 10
export func a_func() {
    return "A calling B: " + b.b_func()
}
```

```leno
// module_b.leno
import "module_a.leno" as a

export var b_value = 20
export func b_func() {
    return "B (a_value=" + a.a_value + ")"
}
```

#### 同名不同路径的模块

可以同时导入不同路径的同名模块：

```leno
import "folder1/utils.leno" as u1
import "folder2/utils.leno" as u2

main() {
    print(u1.get_info())   // utils from folder1
    print(u2.get_info())   // utils from folder2
}
```

#### 模块重复导入

同一个模块被多个模块导入时，只会加载一次：

```leno
// shared.leno
export var init_count = 0
export func init() {
    init_count = init_count + 1
    return init_count
}
```

```leno
// main.leno
import "module_a.leno" as ma  // module_a 也导入 shared
import "module_b.leno" as mb  // module_b 也导入 shared
import "shared.leno" as direct

main() {
    print(ma.use_shared())      // count=1
    print(mb.use_shared())      // count=2 (同一个 shared 模块)
    print(direct.get_count())   // 2
}
```

#### 相对路径导入

支持使用相对路径导入上级目录的模块：

```leno
// sub/folder/test.leno
import "../../root_module.leno" as root
import "./sibling.leno" as sib

main() {
    print(root.get_value())
    print(sib.get_value())
}
```

#### 特殊字符路径

支持中文路径、空格路径等：

```leno
import "中文文件夹/模块.leno" as cn
import "folder with spaces/module.leno" as sp
import "folder-with-dashes/module.leno" as dash

main() {
    print(cn.say_hello())
    print(sp.get_value())
    print(dash.get_value())
}
```

#### 导入别名限制

别名不能使用关键字或内部模块名称：

```leno
import "myfile.leno" as io       // ❌ 错误：io 是内部模块
import "myfile.leno" as if       // ❌ 错误：if 是关键字
import "myfile.leno" as mymod    // ✅ 正确
```

***

## 异常处理（try-catch-finally）

Leno 提供了 `try`-`catch`-`finally` 异常处理机制，与 Java/Python 等语言语义一致。

### 基本用法

```leno
main() {
    try {
        print("try block")
        throw "error message"     // 抛出异常（可以是任何类型的值）
    } catch e {
        print($"caught: {e}")     // e 是捕获到的异常值
    } finally {
        print("finally block")    // 无论是否出错都执行
    }
}
```

### try-catch

最基本的异常捕获形式：

```leno
main() {
    try {
        throw "something went wrong"
    } catch e {
        print($"捕获异常: {e}")    // 捕获异常: something went wrong
    }
}
```

### try-finally（无 catch）

没有 `catch` 时，`finally` 仍会执行，但异常会继续向外传播：

```leno
main() {
    try {
        throw "unhandled"
    } finally {
        print("cleanup")         // 始终执行
    }
    // 程序在此处因未捕获异常而终止
}
```

> **⚠️ 注意：try-finally 无 catch 时异常不会被吞掉**
>
> `finally` 块执行完毕后，异常会继续向上传播。如果外层没有 `catch`，程序将因未捕获异常而终止。

### try-catch-finally

`catch` 和 `finally` 可以同时使用：

```leno
main() {
    try {
        throw "error"
    } catch e {
        print($"caught: {e}")     // caught: error
    } finally {
        print("cleanup")         // 始终执行
    }
}
```

执行顺序：
1. `try` 块中抛出异常
2. `catch` 块捕获并处理异常
3. `finally` 块始终执行

> **⚠️ 注意：finally 块只会执行一次**
>
> ```leno
> var count = 0
> try {
>     throw "error"
> } catch e {
>     // 处理异常
> } finally {
>     count = count + 1
> }
> print(count)   // 1（不是 2）
> ```

### throw 语句

`throw` 可以抛出任何类型的值：

```leno
main() {
    // 抛出字符串
    throw "字符串错误"

    // 抛出整数
    throw 404

    // 抛出字典
    throw {code: 500, msg: "server error"}
}
```

### 异常对象属性

当运行时错误（如除零、越界等）被捕获时，`e` 是一个异常对象，包含以下属性：

| 属性 | 类型 | 说明 |
|------|------|------|
| `e.msg` | `string` | 错误消息 |
| `e.file` | `string` | 发生错误的文件路径 |
| `e.stack` | `string` | 调用栈信息 |

```leno
main() {
    try {
        var a = 1 / 0           // 除零错误
    } catch e {
        print(e.msg)            // 除零错误
        print(e.file)           // 文件路径
        print(e.stack)          // 调用栈
    }
}
```

> **⚠️ 注意：throw 字符串和运行时错误的 e 不同**
>
> - `throw "message"` 抛出的是字符串值，`e` 就是字符串本身，没有 `.msg` 等属性
> - 运行时错误（除零、越界等）抛出的是异常对象，有 `.msg`、`.file`、`.stack` 属性
>
> ```leno
> // throw 字符串
> try { throw "error" } catch e { print(e) }        // "error"（字符串）
>
> // 运行时错误
> try { var a = 1/0 } catch e { print(e.msg) }      // "除零错误"（异常对象）
> ```

### 函数内异常传播

函数内部抛出的异常会自动传播到调用者的 `try-catch`：

```leno
func divide(int a, int b):int {
    return a / b               // b=0 时抛出除零异常
}

main() {
    try {
        var result = divide(10, 0)
    } catch e {
        print(e.msg)            // 除零错误
    }
}
```

多层函数调用时，异常会逐层传播直到被捕获：

```leno
func inner() {
    var a = 1 / 0               // 除零异常
}

func middle() {
    inner()                     // 异常传播到 middle
}

func outer() {
    middle()                    // 异常传播到 outer
}

main() {
    try {
        outer()                 // 异常传播到 main 的 catch
    } catch e {
        print(e.msg)            // 除零错误
    }
}
```

### 嵌套 try-catch

`try-catch` 可以嵌套使用，内层 `catch` 处理不了的异常可以重新 `throw`：

```leno
main() {
    try {
        try {
            throw "inner error"
        } catch e {
            print($"内层捕获: {e}")    // 内层捕获: inner error
            throw "rethrown"          // 重新抛出
        }
    } catch e {
        print($"外层捕获: {e}")        // 外层捕获: rethrown
    }
}
```

> **⚠️ 注意：catch 块中 throw 后 finally 仍会执行**
>
> ```leno
> var finally_ran = false
> try {
>     try {
>         throw "first"
>     } catch e {
>         throw "second"         // catch 中重新 throw
>     } finally {
>         finally_ran = true     // ✅ 仍然执行
>     }
> } catch e {
>     print(e)                    // second
> }
> print(finally_ran)              // true
> ```
>
> 执行顺序：try → catch（throw）→ finally → 外层 catch

### 常见运行时异常

以下操作会自动抛出异常，可以被 `try-catch` 捕获：

| 异常场景 | 错误消息示例 |
|---------|-----------|
| 整数除零 | `除零错误` |
| 浮点除零 | `除零错误` |
| 取模零 | `除零错误` |
| 数组越界 | `数组索引越界` |
| 字符串越界 | `字符串索引越界` |
| 字典键不存在 | `键 'xxx' 不存在` |
| null 值操作 | `null 不能参与算术运算` |
| 类型错误 | `类型错误` |

```leno
main() {
    // 数组越界
    try {
        var arr = [1, 2, 3]
        var v = arr[100]
    } catch e {
        print(e.msg)            // 数组索引越界
    }

    // 字符串越界
    try {
        var s = "hello"
        var c = s[100]
    } catch e {
        print(e.msg)            // 字符串索引越界
    }
}
```

### 异常处理最佳实践

1. **只捕获你能处理的异常**：不要用空的 `catch e {}` 吞掉所有异常
2. **finally 用于资源清理**：关闭文件、通道等资源放在 `finally` 中
3. **避免在 finally 中 throw**：`finally` 块中抛出异常会覆盖原始异常
4. **利用异常对象属性调试**：`e.msg`、`e.file`、`e.stack` 提供完整的错误上下文

```leno
// ✅ 推荐：finally 用于清理
import threads

main() {
    var ch = threads.channel(10)
    try {
        ch.send("data")
        // ... 业务逻辑
    } catch e {
        print($"错误: {e.msg}")
    } finally {
        ch.close()              // 确保通道关闭
    }
}
```

***

## 线程与并发

Leno 通过 `threads` 模块提供线程和通道（Channel）支持，实现并发编程。

### 导入模块

```leno
import threads
```

### 创建线程

使用 `threads.start()` 创建新线程，返回线程对象：

```leno
import threads

main() {
    var t = threads.start(func(){
        print("子线程运行")
    })
    t.join()                    // 等待子线程结束
    print("主线程继续")
}
```

### 线程传参

`threads.start()` 的第二个参数开始传递给线程函数的参数：

```leno
import threads

main() {
    var t = threads.start(func(var name, var age){
        print($"name={name}, age={age}")
    }, "Leno", 3)
    t.join()
}
```

### 等待线程结束（join）

`join()` 阻塞当前线程，等待目标线程执行完毕：

```leno
import threads

main() {
    var t = threads.start(func(){
        threads.sleep(100)
        return 42
    })
    var result = t.join()       // result = 42
    print(result)               // 42
}
```

### 线程状态

`state()` 返回线程当前状态：

| 状态 | 说明 |
|------|------|
| `"running"` | 线程正在执行 |
| `"done"` | 线程正常结束 |
| `"error"` | 线程因异常终止 |

```leno
import threads

main() {
    var t = threads.start(func(){
        return 1
    })
    t.join()
    print(t.state())            // "done"
}
```

### 线程异常传播

**子线程中未捕获的异常会通过 `join()` 传播到主线程**：

```leno
import threads

main() {
    var t = threads.start(func(){
        throw "子线程出错"
    })

    try {
        t.join()                // join() 会将子线程异常传播到此处
    } catch e {
        print($"捕获子线程异常: {e}")   // 捕获子线程异常: 子线程出错
    }
}
```

> **⚠️ 注意：子线程异常必须通过 join() 获取**
>
> - 子线程中的异常不会自动传播到主线程，必须调用 `join()` 时才会触发
> - 如果不调用 `join()`，子线程异常会被静默忽略
> - `join()` 传播异常后，线程状态变为 `"error"`

```leno
import threads

main() {
    var t = threads.start(func(){
        throw "子线程出错"
    })
    threads.sleep(100)
    print(t.state())            // "error"

    try {
        t.join()                // 传播异常
    } catch e {
        print(e)                // 子线程出错
    }
}
```

### 子线程运行时异常传播

子线程中的运行时错误（除零、越界等）同样会通过 `join()` 传播：

```leno
import threads

main() {
    var t = threads.start(func(){
        var a = 1 / 0           // 除零错误
    })

    try {
        t.join()
    } catch e {
        print(e.msg)            // 除零错误
        print(e.stack)          // 子线程调用栈
    }
}
```

### 通道（Channel）

通道是线程间通信的核心机制，遵循 Go 语言的 CSP 模型。

#### 创建通道

```leno
import threads

main() {
    // 有缓冲通道（缓冲区大小为 10）
    var ch = threads.channel(10)

    // 无缓冲通道（同步通道，缓冲区大小为 0）
    var sync_ch = threads.channel(0)
}
```

#### 发送和接收

```leno
import threads

main() {
    var ch = threads.channel(10)

    // 发送数据
    ch.send("hello")
    ch.send(42)
    ch.send(3.14)

    // 接收数据
    print(ch.receive())         // hello
    print(ch.receive())         // 42
    print(ch.receive())         // 3.14

    ch.close()
}
```

#### 通道在线程间通信

```leno
import threads

main() {
    var ch = threads.channel(10)

    // 子线程发送数据
    var t = threads.start(func(var ch){
        ch.send("from child")
        ch.close()
    }, ch)

    // 主线程接收数据
    print(ch.receive())         // from child
    t.join()
}
```

#### 无缓冲通道

无缓冲通道（`channel(0)`）是同步通道：发送方必须等待接收方就绪，反之亦然：

```leno
import threads

main() {
    var ch = threads.channel(0)

    // 子线程发送（会阻塞直到主线程接收）
    var t = threads.start(func(var ch){
        ch.send(42)
    }, ch)

    // 主线程接收（会阻塞直到子线程发送）
    var value = _int(ch.receive())
    print(value)                // 42
    t.join()
}
```

#### 非阻塞接收（try_receive）

`try_receive()` 从通道中尝试接收数据，如果通道为空则立即返回 `null`：

```leno
import threads

main() {
    var ch = threads.channel(10)
    ch.send("data")

    var msg = ch.try_receive()  // "data"
    var empty = ch.try_receive() // null（通道已空）
    ch.close()
}
```

#### 通道长度

`len()` 返回通道缓冲区中的消息数量：

```leno
import threads

main() {
    var ch = threads.channel(10)
    ch.send("a")
    ch.send("b")
    print(ch.len())             // 2
    ch.close()
}
```

#### 关闭通道

`close()` 关闭通道，关闭后不能再 `send()`：

```leno
import threads

main() {
    var ch = threads.channel(10)
    ch.send("data")
    ch.close()

    // 向已关闭的通道发送会抛出异常
    try {
        ch.send("should fail")
    } catch e {
        print("通道已关闭")      // 通道已关闭
    }
}
```

> **⚠️ 注意：关闭通道后仍可接收剩余数据**
>
> ```leno
> var ch = threads.channel(10)
> ch.send("a")
> ch.send("b")
> ch.close()
> print(ch.receive())          // "a"（仍可接收缓冲区中的数据）
> print(ch.receive())          // "b"
> // ch.receive()              // 通道为空且已关闭，返回 null
> ```

### 通道传递 null 值

通道可以传递 `null` 值，但需要注意区分"通道返回 null 是因为值为 null 还是因为通道为空"：

```leno
import threads

main() {
    var ch = threads.channel(10)
    ch.send(null)               // 发送 null 值
    ch.send("real_value")

    var m1 = ch.receive()       // null（这是发送的 null 值）
    var m2 = ch.receive()       // "real_value"
    ch.close()
}
```

> **⚠️ 注意：区分 null 值和空通道**
>
> - `receive()` 在通道为空且已关闭时返回 `null`
> - 如果业务需要传递 `null`，建议用字典包装：`ch.send({value: null})`
> - 或者使用 `try_receive()` + `len()` 判断通道状态

### 全局变量隔离

**每个线程拥有独立的全局变量副本**，子线程修改全局变量不影响主线程：

```leno
import threads

var counter = 100

main() {
    var t = threads.start(func(){
        counter = 999           // 修改子线程的全局变量副本
        print($"子线程: {counter}")  // 子线程: 999
    })
    t.join()
    print($"主线程: {counter}")     // 主线程: 100（不受影响）
}
```

> **⚠️ 注意：线程间共享数据请使用通道**
>
> 全局变量是线程隔离的，不能通过全局变量在线程间传递数据。使用通道（Channel）进行线程间通信。

### 线程休眠

`threads.sleep(ms)` 让当前线程休眠指定毫秒数：

```leno
import threads

main() {
    print("开始")
    threads.sleep(1000)         // 休眠 1 秒
    print("1 秒后")
}
```

### 多线程并发写通道

多个线程可以同时向同一个通道发送数据：

```leno
import threads

main() {
    var ch = threads.channel(100)

    var t1 = threads.start(func(var ch){
        for 10 to var i { ch.send("A" + i) }
    }, ch)

    var t2 = threads.start(func(var ch){
        for 10 to var i { ch.send("B" + i) }
    }, ch)

    t1.join()
    t2.join()

    // 通道中有 20 条消息
    print(ch.len())             // 20
    ch.close()
}
```

### 线程与异常处理注意事项

1. **子线程异常不会自动传播**：必须通过 `join()` 获取
2. **join() 传播异常后线程状态为 `"error"`**
3. **子线程中未捕获的运行时异常**（除零、越界等）也会通过 `join()` 传播
4. **通道操作可能抛异常**：向已关闭通道 `send()` 会抛异常，用 `try-catch` 处理
5. **全局变量线程隔离**：不要依赖全局变量在线程间共享状态

```leno
import threads

main() {
    // 完整的线程异常处理模式
    var t = threads.start(func(){
        // 子线程业务逻辑
        var a = 1 / 0
    })

    try {
        t.join()
    } catch e {
        // 处理子线程异常
        print($"线程异常: {e.msg}")
        print($"线程状态: {t.state()}")   // error
    }
}
```

***

## 异步编程

Leno 支持 `async`/`await` 语法进行异步编程，配合 `asyncs` 模块实现非阻塞操作。

### 基本用法

```leno
import asyncs

// 定义异步函数
async func fetch_data():string {
    await asyncs.sleep(100)     // 等待100毫秒
    return "数据获取完成"
}

main() {
    // 调用异步函数
    fetch_data()
    
    // 运行事件循环
    asyncs.run()
}
```

### 等待异步结果

```leno
import asyncs

async func compute(int n):int {
    await asyncs.sleep(50)
    return n * 2
}

async func main_async() {
    print("开始计算...")
    
    // 等待异步结果
    var result = await compute(5)
    print($"结果: {result}")      // 结果: 10
    
    // 并发执行多个异步任务
    var a = await compute(5)
    var b = await compute(10)
    print($"总和: {a + b}")       // 总和: 30
}

main() {
    main_async()
    asyncs.run()
}
```

### 嵌套异步调用

```leno
import asyncs

async func inner(int n):int {
    await asyncs.sleep(100)
    return n * 2
}

async func outer() {
    print("outer 开始")
    var a = await inner(5)
    print($"inner(5) = {a}")
    var b = await inner(10)
    print($"inner(10) = {b}")
    print($"总和: {a + b}")
}

main() {
    outer()
    asyncs.run()
}
```

> **⚠️ 注意：必须调用** **`asyncs.run()`** **启动事件循环**
>
> ```leno
> async func task() {
>     await asyncs.sleep(100)
>     print("完成")
> }
>
> main() {
>     task()
>     asyncs.run()    // 必须调用，否则异步任务不会执行
> }
> ```

***

## FFI 外部函数接口

FFI（Foreign Function Interface）允许 Leno 直接调用 C 语言库函数，实现底层操作。

### 基本用法

```leno
import ffi

main() {
    // 分配内存
    var ptr = ffi.malloc(100)
    print("分配地址: " + ptr)
    
    // 释放内存
    ffi.free(ptr)
    print("内存已释放")
}
```

### clib 声明式调用（推荐）

除了 `ffi.call()` 动态调用，Leno 还支持 `clib` 声明式调用，编译期类型检查，零摩擦参数转换：

```leno
import ffi

// 声明 C 库函数签名
clib kernel32 {
    i32 GetTickCount()
    void Sleep(i32 ms)
}

main() {
    kernel32 lib = ffi.load("kernel32.dll")
    
    // 直接调用，i32 返回值自动转为 int
    int tick = lib.GetTickCount()
    print("运行时间: " + tick + "ms")
    
    lib.Sleep(1000)  // void 返回
    ffi.free(lib)
}
```

### 内存操作

```leno
import ffi

main() {
    // 分配内存
    var ptr = ffi.malloc(1024)
    
    // 写入数据
    ffi.write_int(ptr, 42)
    ffi.write_float(ptr + 8, 3.14)
    
    // 读取数据
    var int_val = ffi.read_int(ptr)
    var float_val = ffi.read_float(ptr + 8)
    
    print(int_val)      // 42
    print(float_val)    // 3.14
    
    // 释放内存
    ffi.free(ptr)
}
```

### 加载动态库和调用函数

```leno
import ffi

main() {
    // 加载动态库
    var lib = ffi.load("kernel32.dll")
    
    // 直接调用函数（传入库句柄、函数名和参数）
    var tick = ffi.call(lib, "GetTickCount")
    print($"系统运行时间: {tick}ms")
    
    // 释放库
    ffi.free(lib)
}
```

### 带参数的函数调用

```leno
import ffi

main() {
    var lib = ffi.load("kernel32.dll")
    
    // 分配缓冲区
    Ptr buf = ffi.malloc(256)
    
    // 调用函数并传递参数
    // ffi.call(库, 函数名, 参数1, 参数2, ...)
    var result = ffi.call(lib, "GetModuleFileNameW", ffi.nullptr(), buf, 256)
    
    // 读取结果
    string path = ffi.utf16_to_utf8(buf)
    print("当前程序路径: " + path)
    
    ffi.free(buf)
    ffi.free(lib)
}
```

### 完整 FFI API 参考

| 函数                                  | 说明             | 示例                                  |
| ----------------------------------- | -------------- | ----------------------------------- |
| `ffi.load(name)`                    | 加载动态库          | `ffi.load("kernel32.dll")`          |
| `ffi.free(lib)`                     | 释放动态库          | `ffi.free(lib)`                     |
| `ffi.call(lib, name, ...)`          | 调用函数           | `ffi.call(lib, "Func", arg1, arg2)` |
| `ffi.malloc(size)`                  | 分配内存           | `ffi.malloc(1024)`                  |
| `ffi.free(ptr)`                     | 释放内存           | `ffi.free(ptr)`                     |
| `ffi.nullptr()`                     | 空指针            | `ffi.nullptr()`                     |
| `ffi.ptr_from_int(n)`               | 整数转指针          | `ffi.ptr_from_int(0x80000000)`      |
| `ffi.is_ptr(val)`                   | 判断是否为指针        | `ffi.is_ptr(x)`                     |
| `ffi.sizeof(ptr)`                   | 获取指针大小         | `ffi.sizeof(ptr)`                   |
| `ffi.assert_size(ptr, min)`         | 断言缓冲区 ≥ min 字节  | `ffi.assert_size(buf, 8192)`        |
| `ffi.offset(ptr, n)`                | 指针偏移           | `ffi.offset(ptr, 8)`                |
| `ffi.read_int(ptr, offset)`         | 读取 int         | `ffi.read_int(ptr, 0)`              |
| `ffi.read_int64(ptr, offset)`       | 读取 int64       | `ffi.read_int64(ptr, 0)`            |
| `ffi.read_float(ptr, offset)`       | 读取 float       | `ffi.read_float(ptr, 0)`            |
| `ffi.read_ptr(ptr, offset)`         | 读取指针           | `ffi.read_ptr(ptr, 0)`              |
| `ffi.read_byte(ptr, offset)`        | 读取字节           | `ffi.read_byte(ptr, 0)`             |
| `ffi.write_int(ptr, offset, val)`   | 写入 int         | `ffi.write_int(ptr, 0, 42)`         |
| `ffi.write_int64(ptr, offset, val)` | 写入 int64       | `ffi.write_int64(ptr, 0, 100)`      |
| `ffi.write_float(ptr, offset, val)` | 写入 float       | `ffi.write_float(ptr, 0, 3.14)`     |
| `ffi.write_byte(ptr, offset, val)`  | 写入字节           | `ffi.write_byte(ptr, 0, 0xFF)`      |
| `ffi.memcpy(dst, src, size)`        | 内存拷贝           | `ffi.memcpy(dst, src, 100)`         |
| `ffi.utf8_to_utf16(str)`            | UTF-8 转 UTF-16 | `ffi.utf8_to_utf16("中文")`           |
| `ffi.utf16_to_utf8(ptr)`            | UTF-16 转 UTF-8 | `ffi.utf16_to_utf8(ptr)`            |

> **⚠️ 注意：FFI 是底层操作，使用不当可能导致程序崩溃**
>
> - 确保分配的内存正确释放，避免内存泄漏
> - **缓冲区大小**：`ffi.malloc(4096)` 不够存 1024 帧×2通道×4字节(f32)=8192 字节。用 `ffi.assert_size(buf, bytes)` 提前验证
> - Leno `write_*`/`memset`/`memcpy` 已内置边界检查，`ffi.free` 有哨兵检测 — 溢出报错而非崩溃
> - 调用外部函数时注意参数类型匹配
> - Windows API 通常使用 UTF-16，需要 `utf8_to_utf16` 转换

### cstruct 线程支持

cstruct 定义可以在多线程环境中安全使用。由于 cstruct 定义是**编译时确定的只读类型元数据**，多个线程可以共享这些定义而不会产生竞争条件。

**在线程中使用 cstruct：**

```leno
import ffi
import threads

cstruct TEST_STRUCT {
    u32 field1
    u32 field2
}

// 在线程中使用 cstruct
func thread_worker(var ch) {
    try {
        var s = TEST_STRUCT.malloc()
        s.field1 = 100
        s.field2 = 200
        ch.send({type: "ok", f1: s.field1, f2: s.field2})
        s.free()
    } catch e {
        ch.send({type: "error", msg: e})
    }
}

main() {
    var ch = threads.channel(1)
    var t = threads.start(thread_worker, ch)
    var result = ch.receive()
    print("field1=" + result.f1 + ", field2=" + result.f2)
    t.join()
}
```

**线程安全说明：**

| 特性 | 说明 |
|------|------|
| cstruct 定义 | 线程安全，只读共享 |
| cstruct 实例 | 每个线程独立，不共享 |
| `malloc()`/`free()` | 每个线程独立管理自己的实例 |
| `from_ptr()` | 可以安全使用，但只在当前线程有效 |

**注意事项：**

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

***

## 自举：Leno 测试自己的代码

Leno 可以用自己写的测试运行器来跑测试——"吃自己的狗粮"：

```leno
// assert/run_tests.leno — Leno 写的测试运行器
import dirs
import strings

func get_arg(int idx, string fallback) {
    var args = _args()
    if idx < args.len() { return args[idx] }
    return fallback
}

main() {
    var leno = get_arg(0, "build\\leno.exe")
    var dir  = get_arg(1, "assert")
    
    int passed = 0
    int failed = 0
    
    var entries = dirs.listdir(dir)
    for entries to entry {
        if entry.starts_with("test_") and entry.ends_with(".leno") {
            var result = _exec(leno + " " + dir + "\\" + entry + " 2>&1")
            if result[1] == 0 {
                print("  [PASS] " + entry)
                passed = passed + 1
            } else {
                print("  [FAIL] " + entry)
                failed = failed + 1
            }
        }
    }
    
    print("Results: " + passed + " passed, " + failed + " failed")
}
```

运行：`leno assert\run_tests.leno build\leno.exe assert`

> **💡 用到的能力**：`dirs` 目录遍历、`strings` 字符串过滤、`_exec()` 执行外部命令、`_args()` 命令行参数

> **⚠️ Windows `_exec` 注意事项**：路径中使用引号 `\"...\"` 可能导致 `_popen` 返回错误，建议直接拼接无空格路径：`_exec(leno + " " + test)` 而非 `_exec("\"" + leno + "\"")`

***

## 高级特性

### 并行赋值

```leno
main() {
    // 基本交换
    var a = 1
    var b = 2
    a, b = b, a
    print($"a={a}, b={b}")    // a=2, b=1

    // 多变量赋值
    var m, n = 100, 200

    // 表达式计算
    var p = 5, q = 3
    p, q = p + q, p - q
    print($"p={p}, q={q}")    // p=8, q=2

    // 数组元素交换
    var arr = [10, 20, 30]
    arr[0], arr[1] = arr[1], arr[0]
    print(arr)                   // [20, 10, 30]
}
```

> **⚠️ 注意：左右两边数量必须相等**
>
> ```leno
> a, b = 1, 2, 3    // ❌ 报错：数量不匹配
> a, b, c = 1, 2    // ❌ 报错：数量不匹配
> ```

> **⚠️ 注意：并行赋值和 var 多变量声明是不同的语法**
>
> ```leno
> var a = 10, b = 20     // 多变量声明：各自独立赋值
> a, b = b, a             // 并行赋值：同时交换
> // var a, b = b, a      // ❌ 这是多变量声明，不是并行赋值
> ```

### 闭包

```leno
main() {
    // 测试1: 基本闭包
    var x = 10
    func test1() {
        print("test1: x=" + x)    // 可以访问外部变量x
    }
    test1()

    // 测试2: 嵌套函数 - 同名变量遮蔽
    var a = 1
    func outer() {
        var a = 2
        func inner() {
            print("inner: a=" + a)    // 2
        }
        inner()
        print("outer: a=" + a)        // 2
    }
    outer()
    print("main: a=" + a)             // 1

    // 测试3: 闭包修改外部变量
    func makeCounter() {
        var count = 0
        func inc() {
            count = count + 1
            return count
        }
        return inc
    }
    var c1 = makeCounter()
    var c2 = makeCounter()
    print(c1())    // 1
    print(c1())    // 2
    print(c2())    // 1（独立的计数器）
    print(c1())    // 3
}
```

***

## 运行程序

```bash
# 编译并运行
lenolang program.leno
```

***

## 语法速查表

### 基础语法

| 功能    | 语法                                  |
| ----- | ----------------------------------- |
| 变量声明  | `var x = 10`, `int a = 10`          |
| 多变量声明 | `var a=10, b, c=1`                  |
| 并行赋值  | `a, b = b, a`                       |
| 函数定义  | `func name(int a):int { return a }` |
| 默认参数  | `func add(int x, int y=10)`         |
| 前向引用  | 函数可以先调用后定义                          |

### 控制流

| 功能        | 语法                                             |
| --------- | ---------------------------------------------- |
| if 语句     | `if cond { } eif cond { } else { }`            |
| 三元表达式     | `if cond then a else b`                        |
| 类型守卫      | `if a is int { }`, `if a not is string { }`    |
| null 检查   | `if a is null { }`                             |
| for 计数循环  | `for 5 to i`, `for 0:5 to i`, `for 0:5:2 to i` |
| for 遍历数组  | `for arr to item`, `for arr to item, index`    |
| for 遍历字典  | `for dict to key`, `for dict to key, value`    |
| for 遍历字符串 | `for str to char`, `for str to char, index`（按 Unicode 字符遍历） |
| while 循环  | `while i < 10 { }`                             |
| switch    | `switch x { case v { } case is Type { } default { } }` |

### 运算符

| 功能    | 语法                                      |
| ----- | --------------------------------------- |
| 算术运算  | `+`, `-`, `*`, `/`, `%`                 |
| 位运算   | `&`, `\|`, `^`, `~`, `<<`, `>>`, `>>>`  |
| 比较运算  | `==`, `!=`, `<`, `>`, `<=`, `>=`        |
| 逻辑运算  | `and`, `or`, `not`                      |
| 成员检查  | `x in arr`, `key in dict`, `sub in str` |
| 非成员检查 | `x not in arr`, `key not in dict`       |
| 复合赋值  | `+=`, `-=`, `*=`, `/=`, `%=`            |
| 位复合赋值 | `&=`, `\|=`, `^=`, `<<=`, `>>=`, `>>>=` |
| 自增自减  | `++`, `--`                              |

### 数据类型

| 功能    | 语法                                            |
| ----- | --------------------------------------------- |
| int 类型 | `int` — 任意精度整数，int48 内联存储，超出自动升级为 BigInt |
| 泛型数组  | `Array[int]`, `Array[Array[int]]`             |
| 泛型字典  | `Dict[string, int]`, `Dict[int, string]`                           |
| 数组切片  | `arr[2:8]`, `arr[:5]`, `arr[3:]`              |
| 数组比较  | `[1,2] == [1,2]`                              |
| 类型转换  | `_int(x)`, `_float(x)`, `_str(x)`, `_bool(x)`, `_int32(x)`, `_int64(x)`, `_uint32(x)`, `_uint64(x)`, `_uint8(x)`, `_byte(x)` |
| 安全转换  | `x as Type`（匹配返回原值，不匹配返回 null；int↔float 数值转换） |
| 类型检查  | `type(x)` — int 返回 `"int"`，泛型 struct 返回 `"Box[int]"` |
| 字符串插值 | `$"Hello {name}"`                             |
| 原始字符串 | `@"raw string"`                               |
| 格式化字符串 | `format("%s %d %.2f", "hi", 42, 3.14)`        |

### 结构体与枚举

| 功能       | 语法                                |
| -------- | --------------------------------- |
| 定义结构体    | `struct Point { int x, int y }`   |
| 定义泛型结构体  | `struct Box[T] { T value }`       |
| 泛型约束     | `func f[T: Comparable](T a, T b)`  |
| 多 face 实现 | `struct Key impl Comparable, Hashable { }` |
| 约束方法引用   | `return item.format`（返回绑定方法）      |
| 创建实例     | `new Point(x=10, y=20)`           |
| 创建泛型实例   | `new Box[int](value=42)`（类型参数不可省略） |
| 访问字段     | `p.x`, `p.y`                      |
| 定义方法     | `func method():type { }`          |
| self 关键字 | `self.field`, `self.method()`     |
| 方法链式调用   | `outer.get().get()`               |
| 定义枚举     | `enum Color { red, green, blue }` |
| 枚举值      | `Color.red`, `Color.green`        |

### 闭包与函数式编程

| 功能        | 语法                                                   |
| --------- | ---------------------------------------------------- |
| 函数赋值      | `var f = add`                                        |
| 函数作为参数    | `apply(x, y, operation)`                             |
| 函数作为返回值   | `return add`                                         |
| 函数存储在数组   | `[func1, func2, func3]`                              |
| 函数存储在字典   | `{"en": greet_en}`                                   |
| 匿名函数      | `var f = func() { return 42 }`                       |
| 匿名函数作为参数  | `apply(func(x) { return x * 2 }, 5)`                 |
| 匿名函数作为返回值 | `return func(x) { return x + n }`                    |
| 立即执行匿名函数  | `var r = func() { return 100 }()`                    |
| 泛型匿名函数    | `var id = func[T](T x): T { return x }`              |
| 闭包（嵌套函数）  | `func outer() { func inner() { ... } return inner }` |
| 闭包（匿名函数）  | `func outer() { return func() { ... } }`             |
| 闭包修改外部变量  | 内部函数可以修改外部函数的变量                                      |

### 异步编程

| 功能     | 语法                           |
| ------ | ---------------------------- |
| 异步函数   | `async func name():type { }` |
| 等待结果   | `await asyncs.sleep(100)`    |
| 启动事件循环 | `asyncs.run()`               |

### FFI 外部函数接口

| 功能             | 语法                                  |
| -------------- | ----------------------------------- |
| 分配内存           | `ffi.malloc(size)`                  |
| 释放内存           | `ffi.free(ptr)`                     |
| 空指针            | `ffi.nullptr()`                     |
| 整数转指针          | `ffi.ptr_from_int(n)`               |
| 判断指针           | `ffi.is_ptr(val)`                   |
| 指针大小           | `ffi.sizeof(ptr)`                   |
| 缓冲区断言         | `ffi.assert_size(ptr, min)`          |
| 指针偏移           | `ffi.offset(ptr, n)`                |
| 读取 int         | `ffi.read_int(ptr, offset)`         |
| 读取 int64       | `ffi.read_int64(ptr, offset)`       |
| 读取 float       | `ffi.read_float(ptr, offset)`       |
| 读取指针           | `ffi.read_ptr(ptr, offset)`         |
| 读取字节           | `ffi.read_byte(ptr, offset)`        |
| 写入 int         | `ffi.write_int(ptr, offset, val)`   |
| 写入 int64       | `ffi.write_int64(ptr, offset, val)` |
| 写入 float       | `ffi.write_float(ptr, offset, val)` |
| 写入字节           | `ffi.write_byte(ptr, offset, val)`  |
| 内存拷贝           | `ffi.memcpy(dst, src, size)`        |
| 加载动态库          | `ffi.load("lib.dll")`               |
| 释放库            | `ffi.free(lib)`                     |
| 调用函数           | `ffi.call(lib, "func", args...)`    |
| UTF-8 转 UTF-16 | `ffi.utf8_to_utf16(str)`            |
| UTF-16 转 UTF-8 | `ffi.utf16_to_utf8(ptr)`            |

### 模块系统

| 功能      | 语法                                                   |
| ------- | ---------------------------------------------------- |
| 导入模块    | `import io`, `import times as ti`                    |
| 文件模块    | `import "file.leno" as mod`                          |
| 相对路径    | `import "../parent.leno"`, `import "./sibling.leno"` |
| 导出变量    | `export var PI = 3.14`                               |
| 导出函数    | `export func name() { }`                             |
| 导出枚举    | `export enum Color { ... }`                          |
| 导出别名    | `export alias Size = int`, `export alias IntList = Array[int]` |
| 别名链     | `export alias B = A` — 别名引用模板内其他别名 |
| 导出 face  | `export face Shape { }` — 导出接口定义 |
| use 类型   | `use module.Point` — 导入 struct/face 类型到当前作用域 |
| use 链式传导 | D→C→B→A 链式传递，无需手动重导出 |
| 循环依赖    | 支持 A→B→A 相互导入                                        |
| 随机数     | `rands.ints(1,6)`, `rands.choice(arr)`               |
| 数学函数    | `maths.sqrt(x)`, `maths.sin(x)`                      |
| 文件操作    | `files.read(path)`, `files.write(path, content)`     |
| JSON 解析 | `jsons.decode(str)`, `jsons.encode(obj)`             |
| 网络连接    | `sockets.connect(host, port)`                        |
| 目录操作    | `dirs.cwd()`, `dirs.listdir(path)`                   |
| 时间戳     | `times.ms()`                                         |

### 异常处理

| 功能 | 语法 |
|------|------|
| try-catch | `try { } catch e { }` |
| try-finally | `try { } finally { }` |
| try-catch-finally | `try { } catch e { } finally { }` |
| 抛出异常 | `throw "error"` |
| 异常对象属性 | `e.msg`, `e.file`, `e.stack` |
| 嵌套 try-catch | 内层 catch 中 `throw` 重新抛出 |

### 线程与并发

| 功能 | 语法 |
|------|------|
| 导入线程模块 | `import threads` |
| 创建线程 | `threads.start(func(){ })` |
| 线程传参 | `threads.start(func(var x){ }, x)` |
| 等待线程 | `t.join()` |
| 线程状态 | `t.state()` → `"running"` / `"done"` / `"error"` |
| 线程休眠 | `threads.sleep(ms)` |
| 创建通道 | `threads.channel(10)` |
| 无缓冲通道 | `threads.channel(0)` |
| 发送数据 | `ch.send(value)` |
| 接收数据 | `ch.receive()` |
| 非阻塞接收 | `ch.try_receive()` |
| 通道长度 | `ch.len()` |
| 关闭通道 | `ch.close()` |
| 线程异常传播 | `join()` 传播子线程异常到主线程 |

### 其他

| 功能 | 语法                      |
| -- | ----------------------- |
| 注释 | `// 单行`, `/* 多行 */`     |
| 输出 | `print(x)`, `printf(x)` |

***

**注意**：本文档中的示例均来自 `d:\CLeno\LenoC\test` 目录下的实际测试文件。
