# LenoContainers

栈 / 队列 / 优先队列 / 缓存淘汰，**边界行为固定成契约**。纯 Leno，无原生依赖。

七个类型：`Stack[T]` / `Queue[T]` / `Deque[T]`（双端队列）/ `PriorityQueue[T]` /
`LRU[V]`（严格淘汰但 `get` 是 O(n)）/ `ClockLRU[V]`（O(1) 近似 LRU）/
**`LRUO1[V]`（严格淘汰 + 全 O(1)，2026-11-25 新增）**。

## 用法

```leno
import "Containers" as ct

var s = new ct.Stack[int]()
s.push(1)
s.push(2)
print(s.pop())              // 2（后进先出）
print(s.to_array())         // [1]（快照，非破坏性）

var q = new ct.Queue[string]()
q.push_all(["a", "b", "c"])
print(q.pop())              // a
print(q.pop_n(2))           // [b, c]（一次出队 2 个；不够抛错）

// 双端队列：两端都能进出（撤销栈、日志窗、事件队列）
var dq = new ct.Deque[string]()
dq.push_back("b")
dq.push_front("a")
dq.push_back("c")
print(dq.to_array())        // [a, b, c]
print(dq.pop_front())       // a
print(dq.pop_back())        // c
print(dq.at(0))             // b（逻辑下标，0 = 队首）

// 优先队列：比较器 <0 ⇒ a 先出；返回 0 时**按入队序**先入先出（堆本身不稳定）
var pq = new ct.PriorityQueue[int](cmp = func(int a, int b): int {
    if a > b { return -1 }  // 大顶
    if a < b { return 1 }
    return 0
})
pq.push(3); pq.push(9); pq.push(5)
print(pq.top())             // 9（不出队）
print(pq.pop())             // 9

var l = new ct.LRU[int](cap = 128)
l.put("k", 1)
print(l.get("k", -1))       // 1；缺失返回调用点给的默认值
print(l.peek("k", -1))      // 1；只读，**不提升**访问序

// cap 上千、要严格 O(1) ⇒ 换成 ClockLRU（淘汰顺序只是近似）
var c = new ct.ClockLRU[int](cap = 4096)
c.put("k", 1)
print(c.get("k", -1))       // 1
```

## 直接遍历（`for c to v`，2026-11-25 起）

容器和缓存都能直接 `for` 遍历，**顺序就是各自的逻辑顺序**：

```leno
var st = new ct.Stack[int]()
st.push(1); st.push(2); st.push(3)
for st to v { print(v) }        // 1 2 3（栈底 → 栈顶）

var q = new ct.Queue[string]()
q.push("a"); q.push("b")
for q to s { print(s) }         // a b（队首 → 队尾；已出队的不算）

var l = new ct.LRU[int](cap = 4)
l.put("x", 1); l.put("y", 2)
for l to k { print(k) }         // 缓存遍历的是 **key**
```

| 容器 | 遍历到 | 顺序 |
|---|---|---|
| `Stack` | 元素 | 栈底 → 栈顶 |
| `Queue` / `Deque` | 元素 | 队首 → 队尾（扣掉已出队的） |
| `PriorityQueue` | 元素 | **堆序**（只保证父优于子；要按优先级请用 `drain_sorted()`） |
| `LRU` / `LRUO1` | **key** | 最久未使用 → 最近使用 |
| `ClockLRU` | **key** | 环序（不是访问序） |

- 第二个循环变量仍是**下标**（容器按数组语义）：`for st to v, i { }`；
- 普通 struct（没有 `to_array` / `keys`）**行为不变**，仍是字段遍历；
- 可空容器（`Queue? q`）不接管，保持原来的判空诊断（先判空再遍历）；
- 底层实现：语义期把被遍历对象改写成 `c.to_array()` / `c.keys()` ⇒ **每次遍历会分配一个快照数组**。
  ⚠ `for c to v` **不会**自动变成"遍历元素"（编译器不认方法名、也不猜）——
  要遍历元素请显式写 `for c.raw() to v` / `for c.to_array() to v`，见下一节。
  热路径上想零分配，就自己 `peek` / `at` 按下标走。

## 遍历：`for c to v` 走**声明式协议**（2026-11-25）

本模块声明了一个普通 face，容器都实现了它：

```leno
export face Iterable {
    func each(): Array          // 唯一方法：无参、返回元素数组
}
export struct Stack[T] impl Iterable { … func each(): Array[T] { return items } … }
```

`for c to v` 的判定规则很简单：**`impl` 列表里有 `Iterable`**。

| struct 的声明 | `for c to v` 的语义 |
|---|---|
| `impl Iterable` | 遍历 `c.<Iterable 声明里的那个方法>()`（元素） |
| 没 impl Iterable（包括实现了别的 face） | **字段遍历**（k = 字段名，本语言原有语义） |

两点要注意：

- `Iterable` 是**语言保留的 face 名**（地位同 `main`：不是关键字，但语言赋予语义）。
  它的形状被钉死为「恰好 1 个方法、无参数、返回 `Array`」，写歪了**编译期报错**；
- 编译器里出现的只有 `"Iterable"` 这一个名字（一处）；方法名（`each`）来自 face 声明，
  不在编译器里。所以自定义别的 face（哪怕形状一样）**不会**被误捕成可迭代。

所以容器直接写就行：

```leno
for st to v { }        // 栈底 → 栈顶（37 ns/op、20 万项 1,271 B 分配 ≈ 0）
for q to v { }         // 队首 → 队尾（Queue 内部有 head 空洞 ⇒ each() 给快照）
for pq to v { }        // 堆序（不保证出队序；要优先级顺序用 drain_sorted）
for lru to k { }       // 缓存：遍历 key（最久 → 最近）
```

想让**自己的类型**也能被 `for` 遍历？声明一个 face 并 impl 它，编译器一行都不用改：

```leno
struct MyRange impl Iterable {
    int n = 0
    func each(): Array[int] { … }      // 返回什么数组就遍历什么
}
for r to v { }                          // 直接可用
```

### 也可以完全显式（不依赖协议）

```leno
for st.raw() to v { }        // 0 复制 + 数组满速（34 ns/op）—— raw 返回**内部数组本身**
for st.to_array() to v { }   // 快照（52 ns/op）—— 循环体里改容器不影响本轮
for lru.keys() to k { }      // 缓存遍历 key
```

元素一次都不复制、也不想快照？用游标（每元素两次方法调用，慢一点，但元素不连续的容器也能用）：

```leno
var it = st.iter()                       // Stack / Queue / PriorityQueue / Deque 都有
while it.has_next() {
    if it.next() is int => v { sink(v) }  // next(): T?（走完给 null）
}
var kit = lru.keys_iter()                 // 缓存：遍历 key
```

| 容器 | 零分配遍历 | 快照遍历 | 游标 |
|---|---|---|---|
| `Stack` | `for st.raw() to v` ✅ | `for st.to_array() to v` | `st.iter()` |
| `PriorityQueue` | `for pq.raw() to v` ✅（堆序） | `.to_array()` | `pq.iter()` |
| `LRU` | `for lru.raw() to k` ✅（顺序 = `keys()`） | `.keys()` | `lru.keys_iter()` |
| `Queue` | ❌（有 head 偏移，内部不连续） | `.to_array()` | `q.iter()` ✅ |
| `Deque` | ❌（f/b 两段） | `.to_array()` | `dq.iter()` ✅ |
| `ClockLRU` | ❌（环里有死槽） | `.keys()` | ❌（用 `keys()`） |
| `LRUO1` | ❌（链表结构） | `.keys()` | ❌（用 `keys()`） |

### 实测（N=1,000,000，本机）

| 写法 | 时间 | 每轮分配 |
|---|---|---|
| `for st to v`（协议 → `each()`） | **37 ns/op** | **~0**（20 万项实测 1,271 B） |
| `for st.raw() to v`（显式） | 34 ns/op | ~0 |
| `for st.to_array() to v`（显式快照） | 52~57 ns/op | 8 MB |
| `iter()` + `has_next()` | 104~121 ns/op | ~0 |

读法：协议版只多一次方法调用（`each()` 返回内部数组，不复制）⇒ 既省掉 8MB 复制、
又保留数组顺序扫的速度，所以它**比快照还快**。游标省的是复制，但每元素两次方法调用，
因此更慢 —— 它是给"元素不连续"的容器（Queue/Deque）用的兜底。

**语义差别（必须知道）**：

- `each()`（Stack/PriorityQueue/LRU）= 返回**内部数组** ⇒ **实时视图**：
  循环体里 `push`/`pop` 会被看到（边遍历边改容器本来就该避免）；
- `each()`（Queue/Deque）= 内部不连续 ⇒ 它们返回 `to_array()` ⇒ **快照**；
- `to_array()` / `keys()` 显式写时都是快照；游标 `iter()` 是实时视图。

## API

### `Stack[T]`

| 方法 | 说明 |
|---|---|
| `push(v)` / `push_all(Array[T])` | 入栈 |
| `pop(): T` | 出栈；**空栈抛错** |
| `peek(): T` | 看栈顶；空栈抛错 |
| `len()` / `is_empty()` / `clear()` | |
| `to_array(): Array[T]` | 栈底 → 栈顶的**副本**（改它不影响栈） |
| `at(i): T` | 按逻辑下标看元素（0 = 栈底）；越界抛错 |

也可以直接灌初值：`new ct.Stack[int](items = [1, 2, 3])`（命名参数构造 + 字段默认值）。

### `Queue[T]`

| 方法 | 说明 |
|---|---|
| `push(v)` / `push_all(Array[T])` | 入队（尾部） |
| `pop(): T` | 出队（头部）；**空队列抛错** |
| `pop_n(n): Array[T]` | 一次出队 n 个（按出队顺序）；`n < 1` 或超过剩余 ⇒ 抛错（不静默少给） |
| `peek(): T` | 看队首 |
| `len()` / `is_empty()` / `clear()` | |
| `to_array(): Array[T]` | 队首 → 队尾的副本（扣掉已出队的空洞） |
| `at(i): T` | 按逻辑下标看元素（0 = 队首）；越界抛错 |
| `remove_at(i): T` | 摘掉第 i 个（**O(n)**；按条件取走一条时用，纯 FIFO 出队仍用 `pop()`） |

队列是 **head 指针 + 惰性压缩**：`pop` 只挪指针，积压超过一半且超过 64 个时才整体搬一次，
平摊 O(1) —— 不是 `Array.remove(0)` 那种每次 O(n) 整体搬移。

### `Deque[T]`（双端队列）

| 方法 | 说明 |
|---|---|
| `push_front(v)` / `push_back(v)` | 两端入队，均摊 O(1) |
| `push_front_all(Array[T])` / `push_back_all(Array[T])` | 批量入队（`push_front_all([a,b])` ⇒ 逻辑顺序 b,a,…） |
| `pop_front_n(k): Array[T]` | 一次从队首取走 k 个；`k < 1` 或超过剩余 ⇒ 抛错 |
| `pop_front(): T` / `pop_back(): T` | 两端出队；**空队列抛错** |
| `peek_front(): T` / `peek_back(): T` | 两端看一眼；空队列抛错 |
| `at(i): T` / `set(i, v)` | 逻辑下标（0 = 队首）；越界抛错 |
| `remove_at(i): T` | 摘掉第 i 个（**O(n)**，给"按条件取走一条"的队列用） |
| `len()` / `is_empty()` / `clear()` / `to_array()` | |

实现是**两段式**：`f` 反向存前半段、`b` 正向存后半段（`b` 的头偏移 `bh` 做惰性压缩）。
两端 push 都是 O(1)；`pop_back` 遇到 `b` 空了就把 `f` 整体倒进 `b`（每个元素一辈子
最多搬一次 ⇒ 平摊 O(1)）。为什么不用"单数组 + 头预留空隙"：Leno 的泛型没法造 `T` 的
占位值来预留空隙。

### `PriorityQueue[T]`

| 成员 | 说明 |
|---|---|
| `cmp: func(T, T):int` | 比较器字段，构造时给（`new ct.PriorityQueue[int](cmp = …)`）；`null` ⇒ 全相等 ⇒ 退化成 FIFO |
| `push(v)` / `pop(): T` / `top(): T` | 入堆 / 出堆 / 看堆顶；**空堆 pop/top 抛错** |
| `len()` / `is_empty()` / `clear()` | |
| `to_array(): Array[T]` | **堆序**（只保证父优于子），不是出队顺序 |
| `drain_sorted(): Array[T]` | 依次弹出全部 ⇒ 真实出队顺序（用完堆为空） |

两条契约：
- **同优先级 FIFO**：比较器返回 0 时按入队序号先入先出（堆本身不稳定，这是手写堆最常错的地方）；
- **空堆抛错**，不返回 null。

### `LRU[V]`

| 成员 | 说明 |
|---|---|
| `put(k, v): string?` | 写入；超容量时淘汰**最久未使用**，返回被淘汰的 key（`null` = 没淘汰） |
| `get(k, default): V` | 命中会提升为最近使用；缺失返回默认值 |
| `peek(k, default): V` | 命中**不提升**访问序（批量只读用它，省掉搬移） |
| `keys(): Array[string]` | 现存 key，最久未使用 → 最近使用 |
| `has(k)` / `remove(k)` / `len()` / `capacity()` | |
| `set_capacity(n): Array[string]` | 调小立即淘汰到新上限，返回被淘汰的 key；`n < 1` 抛错 |
| `on_evict: func(string, V):void` | 淘汰回调（见下） |
| `clear()` | 清空；挂了 `on_evict` 就逐条通知 |

### `ClockLRU[V]`

| 成员 | 说明 |
|---|---|
| `put(k, v): string?` | 写入；满时按 CLOCK 淘汰（近似"最久未使用"），返回被淘汰的 key |
| `get(k, default): V` | 命中打一个"最近用过"位（O(1)）；缺失返回默认值 |
| `peek(k, default): V` | 只读，连"最近用过"位都不动 |
| `keys(): Array[string]` | 现存 key（**环序**，不是访问序） |
| `has(k)` / `remove(k)` / `len()` / `capacity()` / `clear()` | |
| `set_capacity(n): Array[string]` | **只支持调大**；缩容抛错（定长环要重建，见下） |
| `on_evict: func(string, V):void` | 淘汰回调（见下） |

实现是 **定长环 + 每格一个 ref 位**：`get` 命中只写一位、不搬数组、不找 key
（key→下标在 `slot` 字典里）；淘汰时 CLOCK 指针走环，`ref=1` 清零放过（给第二次
机会）、`ref=0` 淘汰、`ref=-1`（`remove` 留下的死槽）直接复用。所以 `get` / `put` /
`peek` 都是 **O(1)，与 cap 无关**。

取舍：`ClockLRU` 淘汰的是**近似**最久未使用（不是严格的），且不支持缩容。
要"淘汰的确实是最久未使用"、cap 又不大（≤128 很划算）就用 `LRU`。

### `LRUO1[V]`（严格最近访问淘汰 + get/put 全 O(1)）

API 与 `LRU` **完全一致**（`put / get / peek / has / remove / keys / len / capacity /
set_capacity / clear / on_evict`），区别只是内部结构：

| | `LRU` | `ClockLRU` | **`LRUO1`** |
|---|---|---|---|
| 淘汰顺序 | **严格**最久未使用 | 近似（CLOCK 二次机会） | **严格**最久未使用 |
| `get` | O(n) | O(1) | **O(1)** |
| `put` | O(n) | 均摊 O(1) | **O(1)** |
| 额外内存 | 1 条 key 数组 | 1 条环 + 1 条 ref | 每键两条 int + 1 条 key |
| 支持缩容 | ✅ | ❌（抛错） | ✅ |

实现是**整数下标双向链表**（`prev/next` 两个 `Array[int]` + `keyAt` + `slot` 字典），
不是 `Ptr` 链表；被 remove/淘汰的下标进**空闲栈**复用 ⇒ 数组不会无限长。
容量可在 4096 这种量级上仍然 `get`/`put` 全 O(1)，且**淘汰顺序严格**。

### `on_evict`：淘汰时交还资源（三个缓存容器都有）

缓存里装的常常是**有副作用的资源**（SDL 纹理 `Ptr`、打开的文件、socket），淘汰那一刻必须释放：

```leno
func destroyTex(string path, Ptr[u8] t) {
    if t != null { core.lib().SDL_DestroyTexture(t) }
}

var tex = new ct.ClockLRU[Ptr[u8]](cap = 64, on_evict = destroyTex)
tex.put("a.png", SDL3.loadImageTexture(r.handle, "a.png"))   // 满了淘汰 ⇒ 自动销毁
tex.clear()                                                  // 逐条销毁剩余
```

- 触发时机：**自动淘汰**（超容量 / `set_capacity` 缩容）与 `clear()`；
- 显式 `remove(k)` **不触发** —— 那是调用方主动拿走的，由它自己收尾；
- 回调签名是 `(key, value)`：大多数场景只用 value，key 用来记账/打印。

## 边界契约（这就是它存在的理由）

1. **空容器取值一律抛错**，不返回 `null` / `0` / 默认值 —— 空栈 `pop()` 是写错了，
   给它一个 0 只会让错误漂到离病因很远的地方。错误串统一前缀
   `Containers.<类型>.<方法>:`，要按字符串分支也有得抓。
   （`LRU.get/peek` 是例外：缓存命中缺失本来就是常态，默认值**由调用点显式给**。）
2. **`len()` 是逻辑元素个数**（队列扣掉已出队但未压缩的空洞）。
3. **`LRU` 按最近访问淘汰**（`get` 会提升，`peek` 不会）；**`ClockLRU` 是近似**；
   **`PriorityQueue` 同优先级 FIFO**。
4. **键类型固定为 `string`**：两个缓存都用 `Dict[string, V]` 存值；要别的键类型请自己包一层。
5. **不带锁**。Leno 的并发模型是 Channel 传值 + `thread_local`，容器不做加锁版本；
   要跨线程就整份传，或者把容器留在一个线程里用 Channel 收发。

## 复杂度

| 容器 | `push` / `put` | `pop` | `get` / `at` | `peek` |
|---|---|---|---|---|
| `Stack` | 均摊 O(1) | 均摊 O(1) | — | — |
| `Queue` | 均摊 O(1) | 均摊 O(1) | — | — |
| `Deque` | 两端均摊 O(1) | 两端均摊 O(1) | `at` O(1)（`remove_at` O(n)） | — |
| `PriorityQueue` | O(log n) | O(log n) | — | O(1) |
| `LRU` | O(n) | — | O(n) | **O(1)** |
| `ClockLRU` | **均摊 O(1)** | — | **O(1)** | **O(1)**（近似淘汰） |
| `LRUO1` | **O(1)** | — | **O(1)** | **O(1)**（严格淘汰） |

`LRU` 的 O(n) 来自"数组里找 key 再搬位置"（`peek` 不搬，所以是 O(1)）；
`ClockLRU` 换来 O(1)，代价是淘汰顺序只是近似。

## 性能实测（2026-11-24，本机单进程，`bench/bench_containers.leno`）

单位 ns/op，`N=2,000,000`；`pop` 类读数含一个 `sink()` 调用（自由函数约 17 ns/op），
LRU/ClockLRU 读数含 key 拼串。只比同机同时段的趋势，别抠小数。

| 操作 | ns/op |
|---|---|
| 裸 `Array.add`（对照） | 8 |
| `Stack.push` / `pop` | 35 / 68 |
| `Queue.push` / `pop` | 36 / 84 |
| `Queue.pop` 缓存 `len` 前（A/B） | 101 → 94 |
| `Deque.push_back` / `push_front` | 39 / 47 |
| `Deque.pop_front` / `pop_back`（含 sink） | 113 / 124 |
| `Deque` push_front+pop_back 交替 | 124 |
| `Array.remove(0)` 出队（反面教材） | 16 982 |
| `PriorityQueue.push`（N=500k 随机优先级） | 430 |
| `PriorityQueue.pop`（N=500k，含 sink） | 4 120 |
| 对照：`Array.add` 500k + 全量 `sort` | 13 + 1 528 |

`get` 命中随 cap 的变化（ns/op）：

| cap | 16 | 128 | 1024 | 4096 |
|---|---|---|---|---|
| `LRU.get`（严格，O(n)） | 399 | 656 | 2 313 | 7 342 |
| `LRU.peek` | — | — | 351 | — |
| `ClockLRU.get`（近似，O(1)） | 414 | 404 | 369 | **371** |
| **`LRUO1.get`（严格，O(1)）** | **493** | **507** | **502** | **513** |
| 裸 `Dict.get`（对照，1024 键） | | | 312 | |

`cap=1024` 的写入：`LRU.put` 新 key 2 929、覆盖 2 449；`ClockLRU` 1 162 / 399；
**`LRUO1` 1 322 / 约 500**。
`LRU.set_capacity(4096 → 128)` 一次裁 3 968 个：**473 µs**（批量裁前缀，不是逐次 `remove(0)`）。

一句话选型：**cap ≤128 用 `LRU`**（最省事）；**要 O(1) 又要严格淘汰 ⇒ `LRUO1`**
（每次比 `ClockLRU` 慢约 130 ns，换来严格顺序，4096 时比 `LRU` 快 14 倍且仍严格）；
**"够用的近似"就 `ClockLRU`**（不用维护链表）。

几条读数的解读（都是实测，不是估算）：

- **`Stack.push` 35 ns vs 裸 `Array.add` 9 ns**：差的 26 ns 就是**一次 struct 方法调用**
  （工程内实测：自由函数 17 ns、非泛型方法 38 ns、泛型方法 37 ns —— **泛型没有额外惩罚**）。
  这段没法在容器层消掉，只能靠编译器内联。
- **`PriorityQueue` 每元素 ~4 µs**：log₂(500k) ≈ 19 层，每层 1~2 次比较，而**每次比较要付
  一次 `cmp` 函数调用（约 55 ns）** + 两次 `Array[T]` 下标（泛型下标比 `Array[int]` 贵 ~12 ns）。
  所以：**元素多、又要边插边取**才用堆；纯"攒完一次性排序"直接 `sort` 更快（1 530 ns/元素，
  但只排一次）。爬虫这种"队列上万、每次只取一个"的场景是对的。
- **`ClockLRU` / `LRUO1` 的 `get` 曲线是平的**（4096 也只到 371 / 513），`LRU.get` 是斜的（7 342）。

## 在仓库里的真实使用

| 位置 | 用的什么 | 为什么 |
|---|---|---|
| `LenoWeb/lib/web_crawler.leno` | `PriorityQueue[CrawlItem]` | 爬虫的 URL 前沿本来手写了一份二叉堆（`_higher/_swap/_siftUp/_siftDown` 共 60 行）；收编后 `seq`（同优先级 FIFO）与断点续爬都靠容器的契约保住 |
| `LenoSDL3/lib/sdl_menu.leno` | `ClockLRU[Ptr[u8]]` + `on_evict` | 菜单图标缓存原来两条平行数组 + 线性扫描、**没有容量上限**（长跑应用持续占显存）；现在 O(1) 命中、容量封顶、淘汰即 `SDL_DestroyTexture`。单槽 MRU 快路径保留（同一图标相邻行连续绘制时不碰字典） |
| `LenoSDL3/lib/sdl_popmenu.leno` | 同上 | 弹出菜单图标缓存，形态与 sdl_menu 逐行同构（双数组 + MRU + 线性扫 + 无上限） |
| `LenoSDL3/lib/sdl_treeview.leno` | 同上 | 树节点图标缓存：同样无上限 + 线性扫描。`dispose()` **保持原语义**（只丢引用、不销毁，交给渲染器），避免 double-free |
| `LenoSDL3/lib/sdl_table.leno` | 同上 | 表格图标缓存：原来是"环形覆盖 + 上限"，**每个单元格要 O(imgCacheMax) 次字符串比较** ⇒ 现在 O(1)；`image_cache_max == 0`（不限制）映射成一个大 cap |
| `LenoSDL3/lib/sdl_combobox.leno` | 同上 | 下拉项图标缓存原来是 `Dict`（O(1) 但**只增不减**）⇒ 换容器补上容量封顶与淘汰销毁 |
| `LenoSDL3/lib/sdl_listbox.leno` | 同上（+ 日志窗借范式） | 行图标缓存：无上限 + 线性扫描；`dispose()` 同样只丢引用。另外日志窗的 `append_item/append_bottom` 原来是**每来一条就 `remove(0)` 搬一遍整表**，改成本模块 `Queue` 同款"批量裁剪"思路（一次丢 1/4 上限，用一次切片完成） |
| `LenoSDL3/lib/sdl_edit.leno` | `Deque[UndoFrame]` | 撤销/重做原来是 **4 条平行数组**（行栈 + 光标栈 ×2），超 200 用 `remove(0)` 丢最旧（每次保存都搬一遍整表）。换成双端队列后：新快照尾进、撤销尾出、丢最旧头出，全部 O(1)，并把"行 + 光标"合成一帧（不再有两数组必须同步摘的隐患） |
| `LenoWeb/lib/web_cdp.leno` | `Deque[string]` | 未领取事件队列：原来"按 method 领走一条"要**重建整个数组**（O(n) 拷贝）、超上限也是 `remove(0)`；现在入队/丢最旧 O(1)，领取走 `remove_at(i)`（只搬这一条） |

> 六个 SDL3 控件的图标缓存现在共用一套实现（`ClockLRU[Ptr[u8]]` + `on_evict` 销毁），
> 每处都保留各自的**单槽 MRU 快路径**（命中时不碰字典），所以热路径没变慢、只把
> "无上限"和"线性扫描"两件事一起消掉了。
>
> 刻意**没换**的：`sdl_font.leno` 的 `texCache/measCache`（MRU 单槽 + 尺寸表 + `texCap` 是
> "装得下整屏文字就不淘汰"的刻意选择）、`sdl_texcache.leno`（带 refcount 与按渲染器失效，
> 容器没有引用计数语义）、`sdl_renderer.leno` 的裁剪栈/图集（按下标池化、不是 KV）。

> ⚠ **LenoWeb 的 `web_generic.Stack` 现在是本模块 `Stack` 的别名**（2026-11-24 先删掉、
> 2026-11-26 用别名补回）：手写那份与本模块**同名**，而编译器类型表全局按名字索引 ——
> 同时 `import "Web"` 与 `import "Containers"` 会直接报
> 「跨模块同名struct 'Stack' … 运行期只能保留一份 … 请给其中一个改名」。
> 现在 `export alias Stack[T] = ct.Stack[T]`：`use web.Stack` 照旧可用、与 `ct.Stack` 是同一个类型，
> 但实现只有一份 ⇒ 不再撞名字。
> ⚠ 语义随本模块：长度是 `len()`（没有字段 `size`）、判空 `is_empty()`、空取**抛错**、没有 `init()`。
> （这条别名之所以写得出来，是同日补齐了"跨模块泛型别名"能力，见 LenoDocsSrc M6。）

## 跑测试

```
build\leno.exe build\leno_module\LenoContainers\test\test_containers.leno
```

## 跑基准

```
build\leno.exe build\leno_module\LenoContainers\bench\bench_containers.leno
```

## 变更记录

- **0.8.1**（2026-11-26）：`Iterable` 定为**语言保留的 face 名**，判定从"看形状"改成
  "看 impl 列表"。修掉一个真隐患：旧规则会把任何「单 face＋单方法＋返回 Array」的类型
  误判成可迭代（实测 `face Snapshot { func bytes(): Array }` ⇒ `for b to k` 遍历 bytes
  而不是字段）。现在形状在 face 声明处校验（1 个无参方法、返回 Array），写歪编译期报错。
- **0.8.0**（2026-11-25）：新增 `face Iterable { func each(): Array }`，七个类型全部 `impl Iterable`
  ⇒ **`for st to v` 直接遍历元素/键**（37 ns/op、20 万项 ≈ 0 分配）。
  显式写法（`.raw()` / `.to_array()` / `.keys()` / `iter()`）继续保留。

- **0.7.0**（2026-11-25）：新增 `raw()`（返回**内部数组本身**）—— `Stack` / `PriorityQueue` / `LRU`。
  用法是**显式**的 `for st.raw() to v`：实测 1M 项 **34 ns/op、~0 分配**（对照
  `to_array()` 57 ns/op + 8 MB）。⚠ 同一天内曾有一版让语义期按方法名自动改写 `for c to v`，
  **已删除**（编译器不该猜方法名：同名 `raw()` 会被静默劫持）；`for <struct> to k, v`
  回到唯一的"字段遍历"语义。测试第 ⑯ 组改为显式写法，并加"撞名不被劫持"的反面断言。

- **0.6.0**（2026-11-25）：新增**零分配遍历**——`Iter[T]` / `DequeIter[T]` 游标 +
  `Stack/Queue/Deque/PriorityQueue.iter()`、`LRU.keys_iter()`。实测遍历 20 万项：
  年轻代增量 **1,601,327 B → 808 B**（快照 vs 游标）；单趟 1M 遍历的耗时是 55 → 106 ns/op
  （游标每元素两次方法调用），所以 README 里同时给了 `for st.items to v` 这条
  "零分配 + 全速"的 Stack 专用写法。测试新增第 ⑮ 组（顺序对齐 `to_array()`、
  Queue 跳空洞、Deque 跨两段、`keys_iter` 与 `keys()` 一致、10 万项零分配）。

- **0.5.0**（2026-11-25）：新增 **`LRUO1[V]`** —— 严格最久未使用淘汰 + `get`/`put` 全 O(1)
  （整数下标双向链表 + 空闲下标复用）。实测 `get` 命中 493 / 507 / 502 / 513 ns（cap 16/128/1024/4096），
  曲线平且淘汰严格；同 cap 下 `LRU` 是 399 / 656 / 2 313 / 7 342。测试里加了与 `LRU` 的
  2000 步**随机对拍**（每一步 `keys()` 必须逐项一致），用来钉死链表摘/接的正确性。
  bench 增 `LRUO1` 段与三者在 cap 扫描里的对比。
- **0.4.0**（2026-11-24）：新增 **`Deque[T]`**（双端队列，两端均摊 O(1)，含 `at/set/remove_at`）。
  三处"缺双端"的真实代码因此收编：`sdl_edit` 撤销/重做（4 条平行数组 → `Deque[UndoFrame]`）、
  `web_cdp` 事件队列（按 method 领取不再重建数组）、`sdl_listbox` 日志窗（借"批量裁剪"范式）。
  下游同步：`LenoCleaner.top_items` 的 top-K 换 `PriorityQueue`（N=200k 实测 limit=100
  435 ms → 19 ms）。
- **0.3.1**（2026-11-24）：把 SDL3 其余五处图标缓存（popmenu / treeview / table / combobox / listbox）
  一并换成 `ClockLRU` + `on_evict`（原来有的无上限、有的每格 O(n) 线性扫）；顺带给
  `LenoWeb` 的 CDP 未领取事件加了上限（`CDP_EVENTS_MAX = 256`，原来只增不减）。
  组件行为不变，接口零改动。
- **0.3.0**（2026-11-24）：
  - 新增 `PriorityQueue[T]`（二叉堆；同优先级 FIFO；空堆抛错）。
  - 新增 `Stack/Queue.to_array()`、`Queue.pop_n()`、`Stack/Queue.push_all()`、`LRU/ClockLRU.keys()`。
  - 两个缓存容器新增 **`on_evict` 淘汰回调** 与 `put` 返回被淘汰的 key（用来释放纹理/文件句柄）。
  - 错误串统一前缀 `Containers.<类型>.<方法>:`；文档明确"不带锁"。
  - 真实替换：LenoWeb 爬虫的二叉堆、LenoSDL3 菜单图标缓存（见上表）。
  - 顺带修了编译器一个 bug：**跨模块泛型 struct 的 func 类型字段**命名构造时泛型参数没替换
    （`new ct.PriorityQueue[int](cmp = func(int,int):int {...})` 报类型不匹配）。
    根因是替换时靠"遍历本单元 AST 找 struct 定义"，跨模块那个 AST 根本不在本单元；现改为从
    符号表 `struct_type_params` 取。回归用例：`assert/test_struct_init_func_field_cross.leno`。
- **0.2.0**（2026-11-24）：新增 `ClockLRU`；`LRU.peek`；`Queue.pop` 只取一次 `items.len()`；
  `LRU.set_capacity` 缩容改批量裁剪（4096→128 从几十毫秒降到 0.25 ms）。
- **0.1.0**：`Stack` / `Queue` / `LRU` 初版。
