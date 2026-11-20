# Leno 随机数模块 (rands)

本文档详细说明 `rands` 模块提供的所有随机数生成方法。

## 目录

- [使用方式](#使用方式)
- [基础随机数](#基础随机数)
- [范围随机数](#范围随机数)
- [布尔与选择](#布尔与选择)
- [数组操作](#数组操作)
- [字符串生成](#字符串生成)
- [统计分布](#统计分布)
- [种子控制](#种子控制)
- [算法说明](#算法说明)

---

## 使用方式

```leno
import rands

main() {
    // 生成 0-1 之间的随机小数
    var num = rands.om()
    print(num)
    
    // 生成 1-100 之间的随机整数
    var dice = rands.ints(1, 100)
    print(dice)
}
```

---

## 基础随机数

### `om()`

生成 0 到 1 之间的随机浮点数（包含 0，不包含 1）。

**参数**: 无  
**返回**: `float` - [0, 1) 范围内的随机数

```leno
rands.om()  // 0.234567890123
rands.om()  // 0.876543210987
```

---

## 范围随机数

### `ints(min, max)`

生成指定范围内的随机整数（包含 min 和 max）。

**参数**:
- `min` (int): 最小值
- `max` (int): 最大值

**返回**: `int` - [min, max] 范围内的随机整数

```leno
rands.ints(1, 6)     // 模拟骰子: 1-6
rands.ints(0, 100)   // 0-100 的随机整数
rands.ints(-10, 10)  // 负数范围也支持
rands.ints(7, 7)     // 恒为 7（min == max 合法）
```

> ⚠ `min > max` ⇒ **报错**（2026-11-16 起；此前**静默对调**两个参数 ——
> `rands.ints(10, 3)` 会"正常"返回 3~10 的数，排查时完全看不出参数写反了）。

### `floats(min, max)`

生成指定范围内的随机浮点数 —— 按全语言的**闭区间**口径写作 `[min, max]`。

**参数**:
- `min` (float): 最小值（含）
- `max` (float): 最大值（含）

**返回**: `float` - `min ≤ v ≤ max` 的随机浮点数

> ⚠ 实现是 `min + r * (max - min)`，其中 `r ∈ [0, 1)`（53 位精度）⇒ 严格来说
> **`max` 恰好被命中的概率是 0**。这在连续分布上与闭区间 `[min, max]` **等价**
> （端点概率本来就为 0），所以口径统一写成闭区间 —— 别据此以为 `max` 会以
> "某个小概率"出现，它是**取不到**的。要"一定能取到端点"的离散语义请用 `ints` ✓

```leno
rands.floats(0.0, 10.0)    // 0.0-10.0 的随机小数
rands.floats(-5.5, 5.5)    // 负数范围也支持
rands.floats(0.0, 1.0)     // 等同于 om()
```

> ⚠ `min > max` ⇒ **报错**（2026-11-16 起）。此前**不检查**：`floats(4.0, 2.0)` 走
> `min + r*(max-min)`，结果落在 [2,4) 却**小于 min** ⇒ 静默给出与参数名相反的值。

---

## 布尔与选择

### `bools(probability)`

生成随机布尔值，按给定概率返回 true。

> ⚠ **`probability` 是必填的**（2026-10-10 实测修正 ✗）：本文档此前写 `bools(probability?)`
> 并给了 `rands.bools()` 的例子 —— 但**实际编译不过**：
> `rands.bools()` ⇒ 编译期 `rands.bools 参数数量不匹配: 期望 1, 实际 0` ✓。
> 想要"50% 概率"就显式写 `0.5` ✓。

**参数**:
- `probability` (float, **必填**): true 的概率，0.0-1.0

**返回**: `bool` - 随机布尔值

```leno
rands.bools(0.5)        // 50% 概率 true
rands.bools(0.7)        // 70% 概率 true
rands.bools(0.1)        // 10% 概率 true
rands.bools(1.0)        // 总是 true
rands.bools(0.0)        // 总是 false
```

### `choice(array)`

从数组中随机选择一个元素。

**参数**:
- `array` (array): 源数组

**返回**: `T` - 数组中的随机一个元素（**T = 入参的元素类型**，v3.2.6 起）

```leno
var fruits = ["apple", "banana", "orange"]
rands.choice(fruits)    // "banana"

var nums = [1, 2, 3, 4, 5]
rands.choice(nums)      // 3（推断类型是 int ⇒ 能直接做运算）

// 元素类型跟着入参走（关系型规格）⇒ 取出来不必收窄
string pick = rands.choice(fruits)
```

> ⚠ v3.2.6 前返回类型是 `any`（`var x = rands.choice(nums)` 推出 any）；
> 现在按"**返回类型跟第 0 个实参走**"的关系型规格声明 ⇒ 与入参元素类型一致。

---

## 数组操作

### `array(n, min, max)`

生成 n 个随机整数的数组（可重复）。

**参数**:
- `n` (int): 数组长度
- `min` (int): 最小值
- `max` (int): 最大值

**返回**: `array` - 随机整数数组

```leno
rands.array(5, 1, 10)       // [3, 7, 2, 9, 5]
rands.array(10, 0, 100)     // 生成 10 个 0-100 的随机数
```

> ⚠ `min > max` ⇒ **报错**（2026-11-16 起；此前**静默对调**两个参数）。

### `int_array(min, max)`

生成 min 到 max 所有整数的随机排列（不重复）。

**参数**:
- `min` (int): 最小值
- `max` (int): 最大值

**返回**: `array` - 不重复的随机排列数组

```leno
rands.int_array(1, 5)       // [3, 1, 4, 2, 5] - 1-5 各出现一次
rands.int_array(1, 52)      // 洗好的 52 张扑克牌
```

**与 array 的区别**:

| 方法 | 结果示例 | 说明 |
|------|----------|------|
| `int_array(1, 5)` | `[3, 1, 4, 2, 5]` | 1-5 各出现一次 |
| `array(5, 1, 5)` | `[5, 2, 3, 1, 4]` | 可能重复，如 `[2, 2, 3, 1, 2]` |

> ⚠ `min > max` ⇒ **报错**（2026-11-16 起；此前**静默对调**）。

### `sample(array, count)`

从数组中随机采样指定数量的不重复元素。

**参数**:
- `array` (array): 源数组
- `count` (int): 采样数量

**返回**: `Array[T]` - 采样的元素数组（**T = 入参的元素类型**，v3.2.6 起；此前是裸 `Array`，元素 any）

```leno
var nums = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10]
rands.sample(nums, 3)       // [7, 2, 9] - 不重复

var cards = rands.int_array(1, 52)
rands.sample(cards, 5)      // 随机抽 5 张牌
```

> ⚠ `count > 数组长度` ⇒ **报错**（2026-11-16 起）。此前**静默截断成全部元素**：
> `sample([1,2,3], 5)` 静默返回 3 个，调用方以为拿到了 5 个样本。
> `count == 0` 合法（空数组）、`count == 数组长度` 合法（就是打乱后的全量）。

### `shuffle(array)`

随机打乱数组，返回新的打乱后的数组（原数组不变）。

**参数**:
- `array` (array): 源数组

**返回**: `Array[T]` - 打乱后的新数组（**T = 入参的元素类型**，v3.2.6 起；此前是裸 `Array`，元素 any）

```leno
var arr = [1, 2, 3, 4, 5]
var shuffled = rands.shuffle(arr)
print(shuffled)             // [3, 1, 5, 2, 4]
print(arr)                  // [1, 2, 3, 4, 5] - 原数组不变
```

---

## 字符串生成

### `str(length, chars)`

生成指定长度的随机字符串。

> ⚠ **`chars` 也是必填的**（2026-10-10 实测修正 ✗）：本文档此前写 `str(length, chars?)`
> 并给了 `rands.str(8)` 的例子 —— 但**实际编译不过**：
> `rands.str(8)` ⇒ 编译期 `rands.str 参数数量不匹配: 期望 2, 实际 1` ✓。
> 想要"默认字母数字"就自己把那个字符集传进去 ✓。

**参数**:
- `length` (int): 字符串长度
- `chars` (string, **必填**): 字符集（**没有**"默认字母数字"这回事 ✗）

**返回**: `string` - 随机字符串

```leno
// 想要字母数字混合就自己传（这就是文档里旧例子想表达的"默认" ✓）
rands.str(8, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789")
rands.str(6, "0123456789")  // "384729" - 纯数字
rands.str(10, "abcdef")     // "bacdfaebfc" - 十六进制字符
rands.str(16, "abcdefghijklmnopqrstuvwxyz")  // 纯小写字母
```

---

## 统计分布

### `gauss(mean, std)`

生成符合正态分布（高斯分布）的随机数。

**参数**:
- `mean` (float): 均值（期望值）
- `std` (float): 标准差

**返回**: `float` - 正态分布随机数

```leno
// 标准正态分布 N(0, 1)
rands.gauss(0, 1)           // -0.543210...
rands.gauss(0, 1)           // 1.234567...

// 模拟身高：均值 170cm，标准差 10cm
rands.gauss(170, 10)        // 173.45...
rands.gauss(170, 10)        // 165.23...

// 模拟考试成绩：均值 75，标准差 15
rands.gauss(75, 15)         // 82.34...
```

**正态分布特性**:
- 约 68% 的值落在 [mean-std, mean+std] 区间
- 约 95% 的值落在 [mean-2*std, mean+2*std] 区间
- 约 99.7% 的值落在 [mean-3*std, mean+3*std] 区间

---

## 种子控制

### `seed(seed)`

设置随机数种子，用于复现随机序列。

> ★ 2026-11-15：RNG 状态是**每线程一份**（`_Thread_local`）⇒ `seed(n)` 只影响**当前线程**。
> 并行程序里要"每个线程各自可复现"，请在该线程入口分别 `rands.seed(n)`。
> 未调 `seed` 时每线程用 `时间 ⊕ 时钟 ⊕ 线程相关地址` 作种子 —— 同瞬启动的线程**不会**
> 拿到同一条序列（此前是普通 static + 只有时间/时钟 ⇒ 多线程撞同种子、序列一模一样 ✗）。

**参数**:
- `seed` (int): 种子值

**返回**: `null`

```leno
// 设置相同的种子，生成相同的序列
rands.seed(12345)
print(rands.ints(1, 100))   // 73
print(rands.ints(1, 100))   // 42

rands.seed(12345)           // 重新设置相同种子
print(rands.ints(1, 100))   // 73 - 与第一次相同
print(rands.ints(1, 100))   // 42 - 与第二次相同
```

**应用场景**:
- 测试代码中的随机行为
- 需要可复现的游戏地图生成
- 科学实验的重复验证

---

## 算法说明

### PCG32 随机数生成器

本模块使用 **PCG32** (Permuted Congruential Generator) 算法，相比传统的 `rand()` 有以下优势：

| 特性 | PCG32 | 传统 rand() |
|------|-------|-------------|
| 周期 | 2^64 | 通常 2^32 |
| 统计质量 | 高 | 低 |
| 支持种子 | 是 | 有限 |
| 线程安全 | 可设计 | 否 |

### Fisher-Yates 洗牌算法

`shuffle` 和 `sample` 使用 Fisher-Yates 算法，保证每个排列的概率完全相等。

### Box-Muller 变换

`gauss` 使用 Box-Muller 算法将均匀分布转换为正态分布。

---

## 示例代码

```leno
import rands

main() {
    // 模拟掷骰子
    print("掷骰子:")
    for 1:5 to i {
        print("  第" + i + "次: " + rands.ints(1, 6))
    }
    
    // 生成随机密码
    print("\n随机密码:")
    var password = rands.str(12, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!@#$%^&*")
    print("  " + password)
    
    // 抽奖程序
    print("\n抽奖结果:")
    var participants = ["张三", "李四", "王五", "赵六", "钱七"]
    var winners = rands.sample(participants, 3)
    print("  获奖者: " + winners)
    
    // 生成符合正态分布的学生成绩
    print("\n模拟成绩:")
    for 1:10 to i {
        var score = rands.gauss(75, 15)
        // 限制在 0-100 之间
        if (score < 0) score = 0
        if (score > 100) score = 100
        print("  学生" + i + ": " + score)
    }
    
    // 可复现的随机序列
    print("\n可复现序列:")
    rands.seed(42)
    print("  " + rands.array(5, 1, 100))
}
```

---

## 注意事项

1. **随机性**: 本模块生成的是伪随机数，通过 `seed()` 可以复现序列
2. **范围**: 全语言的整数 / 下标区间一律**闭区间**（`ints` 含 `max`）；`floats` 按闭区间
   表述，但连续分布下 `max` 的命中概率为 0（见上面的说明）—— 不是两套口径 ✓
3. **性能**: `gauss` 计算开销比普通随机数大，大量生成时需注意性能
4. **线程安全**: 当前实现使用全局状态，多线程环境下需要额外处理

---

*文档版本: 1.0*  
*最后更新: 2026-05-01*
