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
6. [结构体（Struct）](#结构体struct)
7. [face（接口）](#face接口)
8. [枚举（Enum）](#枚举enum)
9. [数组](#数组)
10. [字典](#字典)
11. [字符串](#字符串)
12. [模块系统](#模块系统)
13. [异步编程](#异步编程)
14. [FFI 外部函数接口](#ffi-外部函数接口)
15. [高级特性](#高级特性)
16. [语法速查表](#语法速查表)

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

```leno
main() {
    // 形式1: for n → 循环n次，变量从0到n-1
    for 5 {
        printf("cs ")        // 输出5次 "cs "
    }
    print("")

    // 形式2: for n to var → 迭代0到n-1
    for 5 to num {
        printf(num + " ")   // 输出: 0 1 2 3 4
    }
    print("")

    // 形式3: for :n → 包含n（闭区间）
    for :5 to num {
        printf(num + " ")   // 输出: 0 1 2 3 4 5
    }
    print("")

    // 形式4: for start:end → 闭区间
    for 0:5 to num {
        printf(num + " ")   // 输出: 0 1 2 3 4 5
    }
    print("")

    for 2:5 to num {
        printf(num + " ")   // 输出: 2 3 4 5
    }
    print("")

    // 形式5: for start:end:step → 带步进
    for 0:5:2 to num {
        printf(num + " ")   // 输出: 0 2 4
    }
    print("")

    // 小数步进
    for 0:1:0.1 to num {
        printf(num + " ")   // 输出: 0.0 0.1 0.2 ... 1.0
    }
    print("")

    // 反向迭代
    for 5:0 to num {
        printf(num + " ")   // 输出: 5 4 3 2 1 0
    }
    print("")

    // 反向带步进
    for 5:0:-2 to num {
        printf(num + " ")   // 输出: 5 3 1
    }
    print("")
}
```

### 遍历数组

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

### 遍历字符串

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
}
```

### 遍历字典

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

**int 键字典遍历：**

```leno
main() {
    var id_map = {1: "张三", 2: "李四", 3: "王五"}

    // 遍历 int 键字典
    for id_map to id, name {
        print($"{id}: {name}")
    }
    // 输出顺序: 1:张三, 2:李四, 3:王五（按插入顺序）
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
> 4. `var b = 0` 推断为 `int`（不是 `any`）
>
> ```leno
> func bad(int x = 10, int y) { }      // ❌ 默认参数不能在必选参数前面
> func bad2(int x, int y = 3.14) { }   // ❌ float 不能赋给 int（降级）
> func bad3(int x, var y = 0) { }      // ❌ var 参数不能有默认值
> ```

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

### 闭包（Closure）

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

// 访问字段
print(p.x)      // 10
print(p.y)      // 20

// 修改字段
p.x = 100
p.y = 200
print(p.x)      // 100
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
print(s.speak())     // woof

s = new Cat()        // ❌ 编译错误：Cat 未 impl Speaker
```

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
    var animals = [new Dog(), new Cat()]
    print(type(animals))    // Array[Speaker]

    for animals to s {
        print(s.speak())    // woof, meow
    }
}
```

| 数组元素 | 推断结果 | 说明 |
|---------|---------|------|
| `[Dog, Dog]` | `Array[Dog]` | 同类型，保持原类型 |
| `[Dog, Cat]`（都 impl Speaker） | `Array[Speaker]` | 不同 struct，找公共 face |
| `[Dog, Fish]`（Fish 无 impl） | `Array[any]` | 无公共 face，退化为 any |

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

    // 访问单个字符
    var str = "cheng8214"
    for str to a {
        print(a)        // 逐个输出字符
    }
}
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

    // 子串提取
    var s5 = "hello world"
    print(s5.slice(0, 5))       // hello
    print(s5.sub_str(0, 5))     // hello
    print(s5.sub_str(6, 5))     // world
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
import "test.leno"

main() {
    print(test.PI)
    print(test.add(1, 2))
}
```

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
    
    // 查找子串位置
    var text = "Hello, World! Hello!"
    print(strings.find(text, "Hello"))      // 1
    print(strings.find(text, "Hello", 2))   // 15 (从位置2开始找)
    print(strings.find(text, "xyz"))        // null (未找到)
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
> - 注意内存对齐和类型大小
> - 调用外部函数时注意参数类型匹配
> - Windows API 通常使用 UTF-16，需要 `utf8_to_utf16` 转换

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

### 错误处理

```leno
main() {
    try {
        print("try block")
        throw "error message"     // 抛出错误（可以是任何类型的值）
    } catch e {
        print($"caught: {e}")     // e 是捕获到的错误值
    } finally {
        print("finally block")    // 无论是否出错都执行
    }
}
```

> **⚠️ 注意：`throw`** **可以抛出任何类型的值**
>
> ```leno
> throw "字符串错误"
> throw 404
> throw {code: 500, msg: "server error"}
> ```

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
| for 遍历字符串 | `for str to char`, `for str to char, index`    |
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
| 类型检查  | `type(x)` — int 始终返回 `"int"`              |
| 字符串插值 | `$"Hello {name}"`                             |
| 原始字符串 | `@"raw string"`                               |
| 格式化字符串 | `format("%s %d %.2f", "hi", 42, 3.14)`        |

### 结构体与枚举

| 功能       | 语法                                |
| -------- | --------------------------------- |
| 定义结构体    | `struct Point { int x, int y }`   |
| 创建实例     | `new Point(x=10, y=20)`           |
| 访问字段     | `p.x`, `p.y`                      |
| 定义方法     | `func method():type { }`          |
| self 关键字 | `self.field`, `self.method()`     |
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
| 循环依赖    | 支持 A→B→A 相互导入                                        |
| 随机数     | `rands.ints(1,6)`, `rands.choice(arr)`               |
| 数学函数    | `maths.sqrt(x)`, `maths.sin(x)`                      |
| 文件操作    | `files.read(path)`, `files.write(path, content)`     |
| JSON 解析 | `jsons.decode(str)`, `jsons.encode(obj)`             |
| 网络连接    | `sockets.connect(host, port)`                        |
| 目录操作    | `dirs.cwd()`, `dirs.listdir(path)`                   |
| 时间戳     | `times.ms()`                                         |

### 异常处理

| 功能        | 语法                                |
| --------- | --------------------------------- |
| try-catch | `try { } catch e { }`             |
| finally   | `try { } catch e { } finally { }` |
| 抛出错误      | `throw "error"`                   |

### 其他

| 功能 | 语法                      |
| -- | ----------------------- |
| 注释 | `// 单行`, `/* 多行 */`     |
| 输出 | `print(x)`, `printf(x)` |

***

**注意**：本文档中的示例均来自 `d:\CLeno\LenoC\test` 目录下的实际测试文件。
