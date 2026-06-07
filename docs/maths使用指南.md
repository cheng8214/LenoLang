# Leno Maths 数学模块使用指南

## 简介

`maths` 模块提供数学运算函数，包括基本运算、取整、三角函数、对数指数、工具函数和数学常量。所有函数返回 `float` 类型，参数接受 `int`、`float` 和 `bigint`。

```leno
import maths
```

---

## API 总览

| 分类 | 模块调用 | 实例方法 | 参数 | 返回 | 错误条件 |
|------|---------|---------|------|------|---------|
| **基本运算** | | | | | |
| 平方根 | `maths.sqrt(x)` | `x.sqrt()` | 1 | `float` | x < 0 报错 |
| 绝对值 | `maths.abs(x)` | `x.abs()` | 1 | `float` | - |
| 幂运算 | `maths.pow(base, exp)` | `base.pow(exp)` | 2 | `float` | - |
| **取整函数** | | | | | |
| 四舍五入 | `maths.round(x)` | `x.round()` | 1 | `float` | - |
| 向上取整 | `maths.ceil(x)` | `x.ceil()` | 1 | `float` | - |
| 向下取整 | `maths.floor(x)` | `x.floor()` | 1 | `float` | - |
| 截断小数 | `maths.trunc(x)` | `x.trunc()` | 1 | `float` | - |
| **三角函数** | | | | | |
| 余弦 | `maths.cos(rad)` | `x.cos()` | 1 | `float` | - |
| 正弦 | `maths.sin(rad)` | `x.sin()` | 1 | `float` | - |
| 正切 | `maths.tan(rad)` | `x.tan()` | 1 | `float` | - |
| 反正弦 | `maths.asin(x)` | `x.asin()` | 1 | `float` | x 不在 [-1,1] 报错 |
| 反余弦 | `maths.acos(x)` | `x.acos()` | 1 | `float` | x 不在 [-1,1] 报错 |
| 反正切 | `maths.atan(x)` | `x.atan()` | 1 | `float` | - |
| 四象限反正切 | `maths.atan2(y, x)` | `y.atan2(x)` | 2 | `float` | - |
| **对数和指数** | | | | | |
| 自然对数 | `maths.log(x)` | `x.log()` | 1 | `float` | x ≤ 0 报错 |
| 常用对数 | `maths.log10(x)` | `x.log10()` | 1 | `float` | x ≤ 0 报错 |
| e 的 x 次方 | `maths.exp(x)` | `x.exp()` | 1 | `float` | - |
| **工具函数** | | | | | |
| 最大值 | `maths.max(a, b, ...)` | - | 可变 | `float` | - |
| 最小值 | `maths.min(a, b, ...)` | - | 可变 | `float` | - |
| 弧度转角度 | `maths.deg(rad)` | `x.deg()` | 1 | `float` | - |
| 角度转弧度 | `maths.rad(deg)` | `x.rad()` | 1 | `float` | - |
| 符号函数 | `maths.sign(x)` | `x.sign()` | 1 | `float` | - |
| **常量** | | | | | |
| 圆周率 | `maths.pi()` | - | 0 | `float` | - |
| 自然常数 | `maths.e()` | - | 0 | `float` | - |

---

## 1. 基本运算

### 1.1 sqrt — 平方根

```leno
maths.sqrt(x)    // 模块调用
x.sqrt()         // 实例方法
```

返回 `x` 的平方根。**x < 0 时抛出运行时错误**。

```leno
import maths

print(maths.sqrt(16))      // 4.0
print(maths.sqrt(2))       // 1.41421...
print(maths.sqrt(0))       // 0.0

// 实例方法
var num = 25
print(num.sqrt())          // 5.0

// 错误：负数
maths.sqrt(-1)             // 运行时错误：sqrt() 参数不能为负数
```

### 1.2 abs — 绝对值

```leno
maths.abs(x)     // 模块调用
x.abs()          // 实例方法
```

返回 `x` 的绝对值。

```leno
print(maths.abs(-5))       // 5.0
print(maths.abs(5))        // 5.0
print(maths.abs(0))        // 0.0
print(maths.abs(-3.14))    // 3.14

// 实例方法
print((-5).abs())          // 5.0
```

### 1.3 pow — 幂运算

```leno
maths.pow(base, exp)    // 模块调用
base.pow(exp)           // 实例方法
```

返回 `base` 的 `exp` 次方。

```leno
print(maths.pow(2, 3))      // 8.0
print(maths.pow(3, 2))      // 9.0
print(maths.pow(4, 0.5))    // 2.0（即 sqrt(4)）

// 实例方法
print(2.pow(10))             // 1024.0
```

---

## 2. 取整函数

### 2.1 round — 四舍五入

```leno
maths.round(x)    // 模块调用
x.round()         // 实例方法
```

返回 `x` 四舍五入到最接近的整数（返回 `float`）。

```leno
print(maths.round(3.4))     // 3.0
print(maths.round(3.6))     // 4.0
print(maths.round(3.5))     // 4.0（.5 向上取整）
print(maths.round(-3.5))    // -4.0

// 实例方法
var num = 3.7
print(num.round())          // 4.0
```

### 2.2 ceil — 向上取整

```leno
maths.ceil(x)     // 模块调用
x.ceil()          // 实例方法
```

返回大于等于 `x` 的最小整数（返回 `float`）。

```leno
print(maths.ceil(3.1))      // 4.0
print(maths.ceil(3.9))      // 4.0
print(maths.ceil(-3.1))     // -3.0
print(maths.ceil(4))        // 4.0
```

### 2.3 floor — 向下取整

```leno
maths.floor(x)    // 模块调用
x.floor()         // 实例方法
```

返回小于等于 `x` 的最大整数（返回 `float`）。

```leno
print(maths.floor(3.9))     // 3.0
print(maths.floor(3.1))     // 3.0
print(maths.floor(-3.1))    // -4.0
print(maths.floor(4))       // 4.0
```

### 2.4 trunc — 截断小数

```leno
maths.trunc(x)    // 模块调用
x.trunc()         // 实例方法
```

返回 `x` 截断小数部分后的值（向零取整，返回 `float`）。

```leno
print(maths.trunc(3.9))     // 3.0
print(maths.trunc(-3.9))    // -3.0
print(maths.trunc(3.1))     // 3.0
```

**trunc 与 floor 的区别：**

| 值 | `trunc` | `floor` |
|----|---------|---------|
| 3.9 | 3.0 | 3.0 |
| -3.9 | -3.0 | -4.0 |

> `trunc` 向零取整，`floor` 向负无穷取整。正数时结果相同，负数时不同。

---

## 3. 三角函数（弧度制）

所有三角函数使用**弧度制**。使用 `maths.rad()` 将角度转为弧度。

### 3.1 cos — 余弦

```leno
maths.cos(rad)    // 模块调用
x.cos()           // 实例方法
```

```leno
print(maths.cos(0))              // 1.0
print(maths.cos(maths.pi()))     // -1.0
print(maths.cos(maths.pi() / 2)) // ≈0.0
```

### 3.2 sin — 正弦

```leno
maths.sin(rad)    // 模块调用
x.sin()           // 实例方法
```

```leno
print(maths.sin(0))              // 0.0
print(maths.sin(maths.pi() / 2)) // 1.0
print(maths.sin(maths.pi()))     // ≈0.0
```

### 3.3 tan — 正切

```leno
maths.tan(rad)    // 模块调用
x.tan()           // 实例方法
```

```leno
print(maths.tan(0))              // 0.0
print(maths.tan(maths.pi() / 4)) // ≈1.0
```

### 3.4 asin — 反正弦

```leno
maths.asin(x)     // 模块调用
x.asin()          // 实例方法
```

返回 `x` 的反正弦值（弧度）。**x 不在 [-1, 1] 范围时抛出运行时错误**。

```leno
print(maths.asin(0))     // 0.0
print(maths.asin(1))     // 1.5707...（π/2）

// 错误：超出范围
maths.asin(2)            // 运行时错误：asin() 参数必须在 -1 到 1 之间
```

### 3.5 acos — 反余弦

```leno
maths.acos(x)     // 模块调用
x.acos()          // 实例方法
```

返回 `x` 的反余弦值（弧度）。**x 不在 [-1, 1] 范围时抛出运行时错误**。

```leno
print(maths.acos(1))     // 0.0
print(maths.acos(0))     // 1.5707...（π/2）

// 错误：超出范围
maths.acos(2)            // 运行时错误：acos() 参数必须在 -1 到 1 之间
```

### 3.6 atan — 反正切

```leno
maths.atan(x)     // 模块调用
x.atan()          // 实例方法
```

返回 `x` 的反正切值（弧度），范围 [-π/2, π/2]。

```leno
print(maths.atan(0))     // 0.0
print(maths.atan(1))     // 0.7853...（π/4）
```

### 3.7 atan2 — 四象限反正切

```leno
maths.atan2(y, x)    // 模块调用
y.atan2(x)           // 实例方法
```

返回点 (x, y) 的角度（弧度），范围 [-π, π]。能正确判断象限。

```leno
print(maths.atan2(1, 1))     // 0.7853...（π/4，第一象限）
print(maths.atan2(1, -1))    // 2.3561...（3π/4，第二象限）
print(maths.atan2(-1, -1))   // -2.3561...（-3π/4，第三象限）
print(maths.atan2(-1, 1))    // -0.7853...（-π/4，第四象限）
```

---

## 4. 对数和指数

### 4.1 log — 自然对数

```leno
maths.log(x)      // 模块调用
x.log()           // 实例方法
```

返回 `x` 的自然对数（ln x）。**x ≤ 0 时抛出运行时错误**。

```leno
print(maths.log(1))           // 0.0
print(maths.log(maths.e()))   // 1.0
print(maths.log(10))          // 2.3025...

// 错误：非正数
maths.log(0)                  // 运行时错误：log() 参数必须大于 0
maths.log(-1)                 // 运行时错误：log() 参数必须大于 0
```

### 4.2 log10 — 常用对数

```leno
maths.log10(x)    // 模块调用
x.log10()         // 实例方法
```

返回 `x` 的常用对数（lg x，以 10 为底）。**x ≤ 0 时抛出运行时错误**。

```leno
print(maths.log10(100))     // 2.0
print(maths.log10(1000))    // 3.0
print(maths.log10(1))       // 0.0

// 错误：非正数
maths.log10(0)              // 运行时错误：log10() 参数必须大于 0
```

### 4.3 exp — e 的 x 次方

```leno
maths.exp(x)      // 模块调用
x.exp()           // 实例方法
```

返回 e^x。

```leno
print(maths.exp(0))     // 1.0
print(maths.exp(1))     // 2.71828...
print(maths.exp(2))     // 7.38905...
```

---

## 5. 工具函数

### 5.1 max — 最大值

```leno
maths.max(a, b, ...)    // 仅模块调用，可变参数
```

返回所有参数中的最大值。

```leno
print(maths.max(3, 7))        // 7.0
print(maths.max(3, 7, 2))     // 7.0
print(maths.max(-1, -5, 0))   // 0.0
print(maths.max(1.5, 2))      // 2.0
```

### 5.2 min — 最小值

```leno
maths.min(a, b, ...)    // 仅模块调用，可变参数
```

返回所有参数中的最小值。

```leno
print(maths.min(3, 7))        // 3.0
print(maths.min(3, 7, 2))     // 2.0
print(maths.min(-1, -5, 0))   // -5.0
```

### 5.3 deg — 弧度转角度

```leno
maths.deg(rad)    // 模块调用
x.deg()           // 实例方法
```

将弧度转换为角度。

```leno
print(maths.deg(maths.pi()))     // 180.0
print(maths.deg(maths.pi() / 2)) // 90.0
print(maths.deg(0))              // 0.0

// 实例方法
var rad = maths.pi() / 4
print(rad.deg())                 // 45.0
```

### 5.4 rad — 角度转弧度

```leno
maths.rad(deg)    // 模块调用
x.rad()           // 实例方法
```

将角度转换为弧度。

```leno
print(maths.rad(180))     // 3.14159...
print(maths.rad(90))      // 1.5707...
print(maths.rad(0))       // 0.0

// 实例方法
var deg = 45
print(deg.rad())          // 0.7853...
```

### 5.5 sign — 符号函数

```leno
maths.sign(x)     // 模块调用
x.sign()          // 实例方法
```

返回 `x` 的符号：正数返回 `1.0`，负数返回 `-1.0`，零返回 `0.0`。

```leno
print(maths.sign(5))      // 1.0
print(maths.sign(-5))     // -1.0
print(maths.sign(0))      // 0.0
print(maths.sign(3.14))   // 1.0

// 实例方法
print((-10).sign())       // -1.0
```

---

## 6. 常量

### 6.1 pi — 圆周率

```leno
maths.pi()    // 仅模块调用
```

返回圆周率 π ≈ 3.141592653589793。

```leno
print(maths.pi())     // 3.141592653589793
```

### 6.2 e — 自然常数

```leno
maths.e()     // 仅模块调用
```

返回自然常数 e ≈ 2.718281828459045。

```leno
print(maths.e())      // 2.718281828459045
```

---

## 7. 模块调用 vs 实例方法

每个数学函数都有两种调用方式：

| 方式 | 语法 | 示例 |
|------|------|------|
| 模块调用 | `maths.函数(参数)` | `maths.sqrt(16)` |
| 实例方法 | `数值.方法(参数)` | `16.sqrt()` |

**实例方法无需 `import maths`**，任何数字值都可以直接调用：

```leno
// 模块调用（需要 import）
import maths
print(maths.sqrt(16))       // 4.0
print(maths.pow(2, 3))      // 8.0

// 实例方法（无需 import）
print(16.sqrt())            // 4.0
print(2.pow(3))             // 8.0
print(3.7.round())          // 4.0
print((-5).abs())           // 5.0
```

**注意**：`max`、`min`、`pi`、`e` 仅支持模块调用，不支持实例方法。

---

## 8. 错误条件汇总

| 函数 | 错误条件 | 错误信息 |
|------|---------|---------|
| `sqrt(x)` | x < 0 | `sqrt() 参数不能为负数` |
| `asin(x)` | x < -1 或 x > 1 | `asin() 参数必须在 -1 到 1 之间` |
| `acos(x)` | x < -1 或 x > 1 | `acos() 参数必须在 -1 到 1 之间` |
| `log(x)` | x ≤ 0 | `log() 参数必须大于 0` |
| `log10(x)` | x ≤ 0 | `log10() 参数必须大于 0` |

---

## 9. 完整示例

```leno
import maths

main() {
    // 基本运算
    print(maths.sqrt(16))       // 4.0
    print(maths.abs(-5))        // 5.0
    print(maths.pow(2, 3))      // 8.0

    // 取整函数
    print(maths.round(3.7))     // 4.0
    print(maths.ceil(3.2))      // 4.0
    print(maths.floor(3.8))     // 3.0
    print(maths.trunc(3.9))     // 3.0

    // 三角函数（弧度制）
    var pi = maths.pi()
    print(maths.sin(0))         // 0.0
    print(maths.cos(0))         // 1.0
    print(maths.tan(0))         // 0.0
    print(maths.asin(0))        // 0.0
    print(maths.acos(1))        // 0.0
    print(maths.atan(0))        // 0.0
    print(maths.atan2(1, 1))    // 0.7853...

    // 对数和指数
    print(maths.log(maths.e())) // 1.0
    print(maths.log10(100))     // 2.0
    print(maths.exp(1))         // 2.71828...

    // 工具函数
    print(maths.max(3, 7, 2))   // 7.0
    print(maths.min(3, 7, 2))   // 2.0
    print(maths.deg(pi))        // 180.0
    print(maths.rad(180))       // 3.14159...
    print(maths.sign(-5))       // -1.0

    // 常量
    print(maths.pi())           // 3.14159...
    print(maths.e())            // 2.71828...

    // 实例方法（无需 import）
    print(16.sqrt())            // 4.0
    print((-5).abs())           // 5.0
    print(2.pow(10))            // 1024.0
    print(3.7.round())          // 4.0
}
```
