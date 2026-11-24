# LenoContainers

栈 / 队列 / LRU 容器，**边界行为固定成契约**。纯 Leno，无原生依赖。

四个类型：`Stack[T]` / `Queue[T]` / `LRU[V]`（严格按最近访问淘汰）/ `ClockLRU[V]`
（CLOCK 二次机会，**O(1) 近似** LRU，2026-11-24 起）。

## 用法

```leno
import "Containers" as ct

var s = new ct.Stack[int]()
s.push(1)
s.push(2)
print(s.pop())              // 2（后进先出）
print(s.len())              // 1

var q = new ct.Queue[string]()
q.push("a")
q.push("b")
print(q.pop())              // a（先进先出）

var l = new ct.LRU[int](cap = 128)
l.put("k", 1)
print(l.get("k", -1))       // 1；缺失返回调用点给的默认值
print(l.peek("k", -1))      // 1；只读，**不提升**访问序

// cap 上千、要严格 O(1) ⇒ 换成 ClockLRU（淘汰顺序只是近似）
var c = new ct.ClockLRU[int](cap = 4096)
c.put("k", 1)
print(c.get("k", -1))       // 1
```

## API

### `Stack[T]`

| 方法 | 说明 |
|---|---|
| `push(v)` | 入栈 |
| `pop(): T` | 出栈；**空栈抛错** |
| `peek(): T` | 看栈顶；空栈抛错 |
| `len()` / `is_empty()` / `clear()` | |

### `Queue[T]`

| 方法 | 说明 |
|---|---|
| `push(v)` | 入队（尾部） |
| `pop(): T` | 出队（头部）；**空队列抛错** |
| `peek(): T` | 看队首 |
| `len()` / `is_empty()` / `clear()` | |

队列是 **head 指针 + 惰性压缩**：`pop` 只挪指针，积压超过一半且超过 64 个时才整体搬一次，
平摊 O(1) —— 不是 `Array.remove(0)` 那种每次 O(n) 整体搬移。

### `LRU[V]`

| 方法 | 说明 |
|---|---|
| `put(k, v)` | 写入；超容量时淘汰**最久未使用** |
| `get(k, default): V` | 命中会提升为最近使用；缺失返回默认值 |
| `peek(k, default): V` | 命中**不提升**访问序（批量只读用它，省掉搬移） |
| `has(k)` / `remove(k)` / `len()` / `capacity()` | |
| `set_capacity(n)` | 调小立即淘汰到新上限；`n < 1` 抛错 |
| `clear()` | |

### `ClockLRU[V]`

| 方法 | 说明 |
|---|---|
| `put(k, v)` | 写入；满时按 CLOCK 淘汰（近似"最久未使用"） |
| `get(k, default): V` | 命中打一个"最近用过"位（O(1)）；缺失返回默认值 |
| `peek(k, default): V` | 只读，连"最近用过"位都不动 |
| `has(k)` / `remove(k)` / `len()` / `capacity()` / `clear()` | |
| `set_capacity(n)` | **只支持调大**；缩容抛错（定长环要重建，见下） |

实现是 **定长环 + 每格一个 ref 位**：`get` 命中只写一位、不搬数组、不找 key
（key→下标在 `slot` 字典里）；淘汰时 CLOCK 指针走环，`ref=1` 清零放过（给第二次
机会）、`ref=0` 淘汰、`ref=-1`（`remove` 留下的死槽）直接复用。所以 `get` / `put` /
`peek` 都是 **O(1)，与 cap 无关**。

取舍：`ClockLRU` 淘汰的是**近似**最久未使用（不是严格的），且不支持缩容。
要"淘汰的确实是最久未使用"、cap 又不大（≤128 很划算）就用 `LRU`。

## 边界契约（这就是它存在的理由）

1. **空容器取值一律抛错**，不返回 `null` / `0` / 默认值 —— 空栈 `pop()` 是写错了，
   给它一个 0 只会让错误漂到离病因很远的地方。（`LRU.get/peek` 是例外：它是缓存，
   缺失本来就是常态，所以默认值**由调用点显式给**。）
2. **`len()` 是逻辑元素个数**（队列扣掉已出队但未压缩的空洞）。
3. **`LRU` 按最近访问淘汰**（`get` 会提升，`peek` 不会）；**`ClockLRU` 是近似**。
4. **键类型固定为 `string`**：两者都用 `Dict[string, V]` 存值；要别的键类型请自己包一层。

## 复杂度

| 容器 | `push` / `put` | `pop` | `get` | `peek` |
|---|---|---|---|---|
| `Stack` | 均摊 O(1) | 均摊 O(1) | — | — |
| `Queue` | 均摊 O(1) | 均摊 O(1) | — | — |
| `LRU` | O(n) | — | O(n) | **O(1)** |
| `ClockLRU` | **均摊 O(1)** | — | **O(1)** | **O(1)** |

`LRU` 的 O(n) 来自"数组里找 key 再搬位置"（`peek` 不搬，所以是 O(1)）；
`ClockLRU` 换来 O(1)，代价是淘汰顺序只是近似。

## 性能实测（2026-11-24，本机单进程，`bench/bench_containers.leno`）

单位 ns/op，`N=2,000,000`；`pop` 类读数含一个 `sink()` 调用（自由函数约 17 ns/op），
LRU/ClockLRU 读数含 key 拼串。只比同机同时段的趋势，别抠小数。

| 操作 | ns/op |
|---|---|
| 裸 `Array.add`（对照） | 8 |
| `Stack.push` / `pop` | 35 / 68 |
| `Queue.push` / `pop` | 32 / 90 |
| `Queue.pop` 缓存 `len` 前（A/B） | 101 → 94 |
| `Array.remove(0)` 出队（反面教材） | 16 982 |

`get` 命中随 cap 的变化（ns/op）：

| cap | 16 | 128 | 1024 | 4096 |
|---|---|---|---|---|
| `LRU.get` | 379 | 624 | 2 178 | 7 659 |
| `LRU.peek` | — | — | 361 | — |
| `ClockLRU.get` | 320 | 357 | 361 | **369** |
| 裸 `Dict.get`（对照，1024 键） | | | 319 | |

`cap=1024` 的写入：`LRU.put` 新 key 2 825、覆盖 2 253；`ClockLRU.put` 新 key 1 182、覆盖 380。
`LRU.set_capacity(4096 → 128)` 一次裁 3 968 个：**251 µs**（批量裁前缀，不是逐次
`remove(0)`）。

结论：**cap ≤ 128 用 LRU 就够**（`peek` 快的场景也可以一直用 LRU）；**cap 上千、或
`get` 在热路径上，用 `ClockLRU`** —— 它的 `get` 曲线是平的。

## 跑测试

```
build\leno.exe build\leno_module\LenoContainers\test\test_containers.leno
```

## 跑基准

```
build\leno.exe build\leno_module\LenoContainers\bench\bench_containers.leno
```

## 变更记录

- **0.2.0**（2026-11-24）：新增 `ClockLRU`（O(1) 近似 LRU）；`LRU` 加 `peek`（不提升）；
  `Queue.pop` 只取一次 `items.len()`；`LRU.set_capacity` 缩容改批量裁剪（4096→128 从
  几十毫秒降到 0.25 ms）。以上均附实测数字，见「性能实测」。
- **0.1.0**：`Stack` / `Queue` / `LRU` 初版。
