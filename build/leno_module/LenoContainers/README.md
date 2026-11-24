# LenoContainers

栈 / 队列 / LRU 三个容器，**边界行为固定成契约**。纯 Leno，无原生依赖。

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
| `has(k)` / `remove(k)` / `len()` / `capacity()` | |
| `set_capacity(n)` | 调小立即淘汰到新上限；`n < 1` 抛错 |
| `clear()` | |

## 边界契约（这就是它存在的理由）

1. **空容器取值一律抛错**，不返回 `null` / `0` / 默认值 —— 空栈 `pop()` 是写错了，
   给它一个 0 只会让错误漂到离病因很远的地方。
2. **`len()` 是逻辑元素个数**（队列扣掉已出队但未压缩的空洞）。
3. **LRU 按最近访问淘汰**（`get` 会提升），不是按插入顺序。
4. **键类型固定为 `string`**：LRU 用 `Dict[string, V]` + 顺序数组实现；要别的键类型
   请自己包一层。

## 复杂度

| 容器 | `push` | `pop` | `get` |
|---|---|---|---|
| Stack | 均摊 O(1) | 均摊 O(1) | — |
| Queue | 均摊 O(1) | 均摊 O(1) | — |
| LRU | O(n) | — | O(n) |

LRU 的 O(n) 来自"数组里找 key 再搬位置"。cap 在几千以内没问题；要真正的 O(1)
得上双向链表 + 节点表，在 Leno 里手写那套的复杂度远超收益。

## 跑测试

```
build\leno.exe build\leno_module\LenoContainers\test\test_containers.leno
```
