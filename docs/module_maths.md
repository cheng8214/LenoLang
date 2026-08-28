# maths 模块

数学运算模块，提供基本运算、三角函数、对数指数、工具函数和常量。

## 基本运算

| 函数 | 签名 | 说明 |
|------|------|------|
| `maths.sqrt(x)` | `(float) -> float` | 平方根，参数不能为负 |
| `maths.abs(x)` | `(float) -> float` | 绝对值 |
| `maths.pow(base, exp)` | `(float, float) -> float` | 幂运算 base^exp |
| `maths.fmod(a, b)` | `(float, float) -> float` | 浮点取模 a mod b（除数不能为0） |
| `maths.clamp(v, lo, hi)` | `(float, float, float) -> float` | 限幅：将 v 限制在 [lo, hi] 范围内 |
| `maths.lerp(a, b, t)` | `(float, float, float) -> float` | 线性插值：a + (b-a)*t |
| `maths.rsqrt(x)` | `(float) -> float` | 快速反平方根 1/√x（参数必须 >0） |
| `maths.hypot(x, y)` | `(float, float) -> float` | 直角三角形斜边 √(x²+y²)，避免溢出 |
| `maths.sign(x)` | `(float) -> float` | 符号函数：正数返回1，负数返回-1，零返回0 |

### 数值实例方法

以上函数也可作为数值的实例方法调用：

```leno
float v = 3.14
float r = v.clamp(0.0, 1.0)   // 等价于 maths.clamp(3.14, 0.0, 1.0)
float h = 3.0.hypot(4.0)      // 等价于 maths.hypot(3.0, 4.0) = 5.0
```

## 取整函数

| 函数 | 说明 |
|------|------|
| `maths.round(x)` | 四舍五入到最近整数 |
| `maths.ceil(x)` | 向上取整 |
| `maths.floor(x)` | 向下取整 |
| `maths.trunc(x)` | 截断小数部分（向零取整） |

## 三角函数（弧度制）

| 函数 | 说明 |
|------|------|
| `maths.sin(x)` | 正弦（标准精度） |
| `maths.cos(x)` | 余弦（标准精度） |
| `maths.tan(x)` | 正切 |
| `maths.asin(x)` | 反正弦，参数 [-1, 1] |
| `maths.acos(x)` | 反余弦，参数 [-1, 1] |
| `maths.atan(x)` | 反正切 |
| `maths.atan2(y, x)` | 双参数反正切，可确定象限 |

### 快速近似三角函数

| 函数 | 说明 |
|------|------|
| `maths.sin_fast(x)` | 快速正弦近似（Bhaskara I 公式，精度 <0.001，比标准 sin 快约 3-5x） |
| `maths.cos_fast(x)` | 快速余弦近似（同上） |

> **使用建议**：在逐像素渲染、物理模拟等高性能场景中使用 `sin_fast`/`cos_fast`，
> 在需要精确计算的场景（如金融、科学计算）中使用标准 `sin`/`cos`。

## 对数和指数

| 函数 | 说明 |
|------|------|
| `maths.log(x)` | 自然对数 ln(x)（x > 0） |
| `maths.log2(x)` | 以2为底的对数（x > 0） |
| `maths.log10(x)` | 以10为底的对数（x > 0） |
| `maths.exp(x)` | 自然指数 e^x |

## 工具函数

| 函数 | 说明 |
|------|------|
| `maths.max(a, b, ...)` | 取最大值（可变参数） |
| `maths.min(a, b, ...)` | 取最小值（可变参数） |
| `maths.deg(rad)` | 弧度转角度 |
| `maths.rad(deg)` | 角度转弧度 |

## 常量

| 常量 | 值 | 说明 |
|------|----|------|
| `maths.pi` | 3.14159265358979... | 圆周率 π |
| `maths.e` | 2.71828182845904... | 自然底数 e |

## 性能场景使用示例

```leno
import maths

// 逐像素距离计算（高性能场景）
float dx = fx - cx
float dy = fy - cy
float dist = maths.hypot(dx, dy)        // 比 sqrt(dx*dx+dy*dy) 更安全
float invDist = maths.rsqrt(dx*dx + dy*dy) // 一步得到 1/dist，省掉除法

// 快速正弦波
float wave = maths.sin_fast(t * 6.28)

// 像素值限幅
int r = maths.clamp(rawR, 0, 255)
int g = maths.clamp(rawG, 0, 255)
int b = maths.clamp(rawB, 0, 255)

// 浮点取模（周期性动画）
float phase = maths.fmod(t, 2.0)  // 0.0 ~ 2.0 循环

// 线性插值（颜色渐变、动画过渡）
float val = maths.lerp(startVal, endVal, progress)
```
