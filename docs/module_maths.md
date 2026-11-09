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
| `maths.smoothstep(t)` | `(float) -> float` | 平滑缓动 3t²-2t³（t 自动夹到 [0,1]）；动画"起手慢/收尾慢"用它 |
| `maths.rsqrt(x)` | `(float) -> float` | 快速反平方根 1/√x（参数必须 >0） |
| `maths.hypot(x, y)` | `(float, float) -> float` | 直角三角形斜边 √(x²+y²)，避免溢出 |
| `maths.sign(x)` | `(float) -> float` | 符号函数：正数返回1，负数返回-1，零返回0 |

### 数值实例方法

以上函数也可作为数值的实例方法调用：

```leno
float v = 3.14
float r = v.clamp(0.0, 1.0)   // 等价于 maths.clamp(3.14, 0.0, 1.0)
float h = 3.0.hypot(4.0)      // 等价于 maths.hypot(3.0, 4.0) = 5.0
float e = 0.3.smoothstep()    // 等价于 maths.smoothstep(0.3) = 0.216
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
| `maths.max(a, b, ...)` | 取最大值（可变参数，返回 float） |
| `maths.min(a, b, ...)` | 取最小值（可变参数，返回 float） |
| `maths.clamp(v, lo, hi)` | 夹到 `[lo, hi]`（返回 float） |
| `maths.lerp(a, b, t)` | 线性插值 `a + (b-a)*t`（**不**夹 t） |
| `maths.smoothstep(t)` | 3t²-2t³，t 先夹到 0..1 |
| `maths.fmod(a, b)` | 浮点取模（余数符号跟被除数） |
| `maths.hypot(x, y)` | √(x²+y²)，比手动 sqrt 更安全（防上溢） |
| `maths.deg(rad)` | 弧度转角度 |
| `maths.rad(deg)` | 角度转弧度 |

## 整数版工具（GUI 首选）

控件里的尺寸、行列号、像素坐标全是 `int`，而上面那些返回 `float` ⇒ 调用点还得 `_int(...)`
转回去，比手写 `if` 还长。下面这组是**同语义的 int 版**；小数实参会**四舍五入**取整
（与 `roundi` 一致，符合"像素边界"直觉）。

| 函数 | 说明 |
|------|------|
| `maths.roundi(x)` | 四舍五入到 int（`round` 的 int 版，替代 `_int(x + 0.5)`） |
| `maths.absi(x)` | int 版绝对值（`abs` 返回 float，`abs(pct-50)/5` 这类 int 语义必须要它） |
| `maths.mini(a, b, ...)` / `maths.maxi(...)` | int 版 min/max（可变参数） |
| `maths.clampi(v, lo, hi)` | int 版 clamp；`hi < lo` 时收成 `lo`（不抛异常） |
| `maths.mod(a, b)` | **正取模**：结余符号跟除数走，`mod(-1, 7) == 6` |

> `maths.mod` 存在的理由：`%` 是 C 语义，负数得负余数。日历取星期、环形索引都要自己补
> `+7` 之类的偏移（LenoSDL3 的 `sdl_calendar.leno` 就是 `(firstDow - weekStart + 7) % 7`）。

## 模幂：`pow_mod(base, exp, m)`（2026-11-09 新增）

`base^exp mod m`。RSA / DH / LCG 伪随机 / 哈希校验都要它 —— 之前 `pow`（浮点）与
`mod`（int）各有一个却没有二者的结合，于是每个项目自己抄一份（LenoMusic 那份在
`crypto_math.leno`，用来算网易/QQ 的 `encSecKey`）。

```leno
maths.pow_mod(2, 10, 1000)                 // 24
maths.pow_mod(3, 1000, 1000000007)         // 56888193
maths.pow_mod(-7, 3, 5)                    // 2   ← 负底数先归一化到 [0, m)

// RSA：c = m^e mod n（1024 位模数照算，见下）
int c = maths.pow_mod(m, 65537, mod)
```

**语义**

| 情况 | 行为 |
|---|---|
| `m <= 0` | 抛错（`mod()` 对 0 也是抛错；负模数没有意义）|
| `exp < 0` | 抛错（没有模逆，不做隐式求逆）|
| `base < 0` | 先归一化到 `[0, m)`（数学惯例；C 的 `%` 会给负余数）|
| 任意精度 | `int` 是 bignum ⇒ 1024 位模数也能算（实测金标对齐 Python）|

**两条实现路径**（性能差别很大，值得知道）

| 场景 | 走哪条 | 为什么 |
|---|---|---|
| `base` / `exp` / `m` **都 ≤ int64** | **int64 快路径**（`unsigned __int128` 做 mulmod）| 全程零分配、纯 C 循环 |
| 任一参数超出 int64（如 1024 位 RSA 模数）| **bignum 路径**（`bigint_mul` + `bigint_mod`）| 大数乘除本身是瓶颈 |

**实测（2026-11-09，本机，与 `crypto_math.leno` 的手写版对比）**

| 场景 | 原生 | 手写 Leno | 加速 |
|---|---|---|---|
| 全 int64，`3^1000000 mod 1e9+7` | 0.23 µs/次 | 2.67 µs/次 | **11.6×** |
| 全 int64，`7^65537 mod 1e9+7` | 0.18 µs/次 | 1.61 µs/次 | **8.6×** |
| 64 位模数 + 128 位底数 | 234 µs/次 | 246 µs/次 | 1.05× |
| 1024 位模数（RSA 真场景）| 4.9 ms/次 | 5.2 ms/次 | 1.05× |

> **结论**：小模数（什么都装得进 int64）快 8–12 倍；真要算 1024 位时，瓶颈是
> 大数乘除本身，两边几乎一样——但至少现在不用每个项目自己写一份，也不会写错。

## 归一化与区间映射

| 函数 | 说明 |
|------|------|
| `maths.saturate(v)` | 夹到 `0..1`（透明度、进度、归一化值的最高频操作） |
| `maths.inverse_lerp(a, b, v)` | `(v-a)/(b-a)`；`a == b` 返回 0，不抛 |
| `maths.remap(v, inLo, inHi, outLo, outHi)` | 把 v 在 `[inLo,inHi]` 的比例（**先夹到 0..1**）映射到 `[outLo,outHi]` |
| `maths.wrap(v, lo, hi)` | 环形夹取到 `[lo, hi)`（轮播、色相环、环形索引） |

`remap` 会夹输入，因为 UI 侧几乎都是"内容长度 → 滑块长度"，越界时必须夹住，
否则滑块会长出轨道。典型用法：

```leno
// 内容 5000px、视口 300px ⇒ 滑块长度（最少 20px）
float thumb = maths.remap(300.0, 0.0, 5000.0, 0.0, 300.0)   // → 18.0
thumb = maths.max(20.0, thumb)
```

## 缓动曲线（动画）

入口 `t` 一律**先夹到 0..1**（动画进度越界是常态，不该由调用方每次自己夹）。

| 函数 | 曲线 | 典型用途 |
|------|------|----------|
| `maths.smootherstep(t)` | 5t⁴-6t⁵ | 比 smoothstep 更平滑的起收 |
| `maths.ease_in_quad(t)` | t² | 退场（越走越快） |
| `maths.ease_out_quad(t)` | 1-(1-t)² | 进场（冲进来再减速） |
| `maths.ease_in_out_quad(t)` | 两头慢 | 展开/切换 |
| `maths.ease_in_cubic(t)` | t³ | 同上，更陡 |
| `maths.ease_out_cubic(t)` | 1-(1-t)³ | **UI 最常用**的进场曲线 |
| `maths.ease_in_out_cubic(t)` | 两头慢 | 页面切换 |
| `maths.ease_out_back(t)` / `(t, k)` | 越过 1 再回弹 | 弹窗、卡片出现；`k` 是回弹幅度（默认 1.70158） |

## 角度工具

角度的"最短路径"不是普通减法：341° → 19° 应该走 +38°，直接相减是 -322°，按后者插值会转一大圈。

| 函数 | 说明 |
|------|------|
| `maths.angle_delta(from, to)` | 最短角差，落在 `[-π, π)` |
| `maths.lerp_angle(from, to, t)` | 按最短路径插值（from/to 无需先归一化） |
| `maths.wrap_angle(a)` | 任意弧度绕回 `[-π, π)` |

## 数值小工具

| 函数 | 说明 |
|------|------|
| `maths.fract(v)` | 小数部分，负数也落在 `[0,1)` |
| `maths.step(edge, x)` | GLSL 语义：`x < edge` ⇒ 0，否则 1 |
| `maths.snap(v, step)` | 吸附到步长网格；`step <= 0` 原样返回 |
| `maths.approx_eq(a, b[, eps])` | 浮点近似相等（默认 eps = 1e-6），布局/动画断言别用 `==` |
| `maths.dist(x1, y1, x2, y2)` | 两点距离 |
| `maths.is_pow2(n)` / `maths.next_pow2(n)` | 2 的幂判断 / 向上取到 2 的幂（扩容、网格分块） |

## 手写 vs 调用 maths：实测开销

口径：`tools/bench_maths_native.leno`，300 万次 × 3 轮取最小值，再减掉空转基线
（取值 + 累加的固定开销），得到**单次调用的净开销**。**负数 = maths 更快**。

| 用例 | 内联前 | 内联后（现状） |
|------|--------|----------------|
| `clamp` | +2.1 ns | **−5.4 ns** |
| `abs` | +0.6 ns | **−4.3 ns** |
| `max` | +3.9 ns | **−0.3 ns** |
| `lerp` | +4.4 ns | **−0.9 ns** |
| `smoothstep` | −13.1 ns | **−19.0 ns** |
| `clampi` / `roundi`（int 版） | +6 ~ +10 ns | 同左（未内联前的那批数据） |

### 为什么现在反而更快

`maths.abs/clamp/min/max/lerp/smoothstep/saturate/fract/roundi/clampi/mini/maxi/mod`
这 13 个函数被**编译期内联**成一条 `OP_MATH_FAST` 指令（见 `leno_vm.h` 的说明）：
不再需要「取模块对象 → 常量装方法名 → `OP_INDEX` 查表 → native 调用协议」四步，
实参也仍然只求值一次。所以现在**调用 maths 比自己手写还快**，代码却更好读。

几条结论：

1. **这些函数可以无脑用**：全部 ≤ 手写开销，`clamp` 快 5 ns、`smoothstep` 快 19 ns。
2. 表达式越复杂，收益越大（`smoothstep` 手写要先夹取再算多项式）。
3. 「封个 Leno 函数复用」仍然最贵（+16~23 ns/次，见基准脚本里的"函数"档）——
   想复用就直接用 maths，不要自己包一层。
4. 没内联的那批（`ease_*` / `remap` / `inverse_lerp` / `approx_eq` / `dist` / 角度工具）
   仍走 native 路径，单次比手写贵 1~5 ns；GUI 一帧几百次 ⇒ 每帧 1 µs 级，可以忽略。
5. **每像素内循环**（`sdl_renderer` 的采样循环，每帧几十万次）继续手写——那里的瓶颈
   不是调用开销而是算法本身。
6. ⚠ 内联只认 `import maths`（别名保持 `maths`）。写成 `import maths as m` 后调
   `m.abs(x)` 走的是常规 native 路径——**结果一样正确**，只是没被内联（少一次优化而已）。
   实参个数与内联表不匹配时同理，自动回落到常规路径。

## 常量

| 常量 | 值 | 说明 |
|------|----|------|
| `maths.pi()` | 3.14159265358979... | 圆周率 π（**函数**，要写括号） |
| `maths.e()` | 2.71828182845904... | 自然底数 e（**函数**，要写括号） |

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
