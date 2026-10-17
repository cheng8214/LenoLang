# LenoStats v0.1.0

Leno 的统计与金额工具（**纯 Leno**）。补 `maths` 缺的统计函数，并给 decimal 一个
实用替代：**金额一律用"分"（int）**。

```leno
import "Stats" as st

st.mean([1.0, 2.0, 3.0, 4.0])       // 2.5
st.median([3.0, 1.0, 2.0])          // 2.0
st.percentile(xs, 90.0)             // 线性插值（Excel PERCENTILE.INC 口径）
st.stdevSample(xs)                  // 样本标准差（n-1）
st.stdevPop(xs)                     // 总体标准差（n）

var m = st.money("12.34")           // {ok:true, cents:1234, err:""}
st.formatCents(1234)                // "12.34"
st.centsMul(1234, 7, 100)           // 打 7 折，四舍五入到分
```

## API

| 函数 | 说明 |
|---|---|
| `mean(xs)` / `meanInt(xs)` | 均值（空数组返回 0.0） |
| `median(xs)` | 中位数（不修改入参） |
| `percentile(xs, p)` | p ∈ [0,100]，线性插值 |
| `stdevSample(xs)` / `stdevPop(xs)` | 样本 / 总体标准差 |
| `sumInt/minInt/maxInt(xs)` | 整数聚合 |
| `money(s)` | `"12.34"→1234`、`"-1.5"→-150`、`".5"→50`；**>2 位小数报错**（不静默四舍五入） |
| `formatCents(cents)` | `1234→"12.34"`、`-150→"-1.50"`、`5→"0.05"` |
| `centsMul(cents, num, den)` | 按比例缩放并**四舍五入到分**（负数对称） |

## 为什么不用 float 存金额

`0.1 + 0.2 != 0.3` 是二进制浮点的固有问题；财务场景必须用整数分（或十进制定点）。
本模块的取舍是"**存分、显示时才格式化**"，`money()` 解析时对三位以上小数**报错**
而不是悄悄舍入 —— 账目对不上比报错更难查。

## 边界

- 不做日期/时间统计、不做回归/分布函数（要的话按需加）
- `percentile` 是 `PERCENTILE.INC`（含端点线性插值），与 `EXC` 口径不同
- `centsMul` 的 `den <= 0` 时原样返回（不抛错，避免除零崩）

## 验证

```bat
build\leno.exe build\leno_module\LenoStats\test\test_stats.leno
```

8 组：均值（含空/单点）/ 中位数（奇偶）/ 百分位（p0/p25/p50/p100）/ 标准差
（总体 2.0、样本 2.13809）/ 整数聚合 / 金额解析（6 正例 3 反例）/ 格式化 /
比例缩放（含 50.5 分进位与负数）。
