# Leno 惯用写法（idioms）

> 这份文档只写**实测过**的结论。每条都给「反例 → 正例」和证据，不给口号。
> 起因：四个真实项目里 355 处 `Array[Array[string]]`、90 处 `curNav ==` 魔法比较、
> 四份逐字相同的 `pad2`——**都不是语言不支持，而是没人示范该用什么**。

---

## 1. 数据：用 `Array[struct]`，不要用 `Array[Array[string]]`

**反例**（Aria `downloads.leno:446` 的原话，四个项目共 355 处这类写法）：

```leno
// 一行 = [id, 歌名, 歌手, 状态, 说明, 格式, 已下字节, 总字节, 音源]
// 为什么用行而不是 struct 数组：字段要原地更新（状态 0→1、进度 0→N），字符串行最好改
var job = [id, name, artist, "0", "", fmt, "0", total, src]
job[3] = "1"                                    // 状态 0 → 1
var n = _int(job[6])                            // 每次取值都要转
```

**正例**：

```leno
struct Job {
    int id = 0
    string name = ""
    string artist = ""
    int status = 0          // 用 enum 常量更好，见第 2 节
    string note = ""
    int done = 0
    int total = 0
}

Array[Job] jobs = []
jobs.add(new Job())

jobs[0].status = 1                 // ← 原地改，**不需要**重建数组
jobs[0].done = 1024
var n = jobs[0].done               // ← 已经是 int，不用 _int
```

### 为什么“字段要原地更新”不是理由（实测）

| 操作 | `Array[Job]` 能不能做 | 验证结果 |
|---|---|---|
| `jobs[0].status = 1` | ✅ | 读到 1 |
| 再改一次 `jobs[0].progress = 0.9` | ✅ | 读到 0.9（不是副本）|
| `for jobs to j { j.status = 2 }` | ✅ | 全部变 2 |
| 传进函数 `bump(Job j) { j.status = 77 }` | ✅ | 原元素变 77（**引用语义**）|
| `Job j2 = jobs[1]; j2.status = 99` | ✅ | 原元素变 99 |
| `type(jobs[0])` | ✅ | `Job` |

> struct 是 **GC 托管的引用**，赋给变量、放进数组、传进函数都是**同一个对象**。
> 所以“行数组好改、struct 不好改”这个前提**不成立**。

### 什么时候**必须**用行数组

只在**跨界**的时候（这些边界目前没有自动转换，见第 5 节）：

- `Table` / `ListBox` 等控件按行取数据；
- `jsons.encode` / `decode`（`Array[Dict]` 或 `Array[any]`）；
- 跨线程传（需要可序列化）。

做法：**内存里用 struct，只在边界处转一次**，而不是全程用行。

---

## 2. 状态：用 `enum` 常量，不要魔法整数 / 字符串码

**反例**（Aria 90 处 `curNav ==`，其中 `curNav == -12` 出现 19 次）：

```leno
if curNav == -12 { ... }         // -12 是什么？只能靠注释
if status == "3" { ... }         // 字符串码，拼错不报错
```

**正例**：

```leno
enum Page {
    HOME = 0
    SEARCH = 1
    LYRIC = 2
    DOWNLOADS = 3
}

int curNav = Page.HOME
if curNav == Page.LYRIC { ... }  // 读得懂，改名字编译器跟着改
```

> **带载荷成员**（2026-11-10 起）：`enum Msg { progress(int done, int total) }`，
> 构造 `Msg.progress(3, 10)`、匹配 `case Msg.progress(d, t) { … }`（`_` 表示忽略该位置）。
> 跨模块可用、`==` 按值、`jsons.encode` 用成员名输出。
> 刻意窄的规则：同一 enum 里成员要么全带参数、要么全不带；带载荷成员不是 int，
> 也不能 `new Msg(...)`；`switch` 主题必须是**变量**。
> 纯 int 成员的老写法行为完全不变。降级成同名 struct 的细节见
> [`enum载荷_设计草案.md`](enum载荷_设计草案.md)。

---

## 3. 排序：`sort(比较器)`，不要手写插入/冒泡

**反例**（LenoMusic `player.leno` / Aria `lyric.leno`，同一份代码复制了两处）：

```leno
// 插入排序 10 行
i = 1
while i < n {
    int cur = ord[i]
    ...
}
```

**正例**：

```leno
ord.sort(func(int a, int b) {
    float ta = out.secs[a]
    float tb = out.secs[b]
    if ta < tb { return -1 }
    if ta > tb { return 1 }
    return 0
})
```

契约：`cmp(a, b)` 返回 int —— 负数 ⇒ a 前、0 ⇒ 相等、正数 ⇒ b 前。
排序是**稳定**的（相等元素保持原序）。

> **不要**把「往已排序数组插一个」（二分定位 + 一次移动，O(log n)）改成全量 `sort`
> —— 那会退化成 O(n² log n)。也不要改「取前 N 名」的选择排序（O(n×lim) 更优）。

---

## 4. 字符串：先查标准库，别自己写

**反例 1**（`pad2` 在四个项目里各抄一份，注释写着“两处各一份，改动要一起改 ⚠”）：

```leno
func pad2(int v): string {
    if v < 10 { return "0" + _str(v) }
    return _str(v)
}
```

**正例 1**：

```leno
strings.pad_start(_str(v), 2, "0")       // "07"
strings.pad_start("你好", 4, "*")         // "**你好"（按字符算，不是字节）
strings.pad_end("hi", 5)                 // "hi   "
```

**反例 2**（手写 split / 字节格式化 / 按行切 —— 这些标准库**都已经有**）：

```leno
Array[string] out = []
int i = 0
while i < s.len() { /* 找分隔符、切片、add */ }
```

**正例 2**：

```leno
strings.split("a,b,,c", ",")     // [a][b][][c]（空段保留）
strings.fmt_size(1536)           // "1 KB"
strings.lines("a\nb\r\nc")       // 按行切，认 \r\n
```

> **2026-11-09 的自纠**：我为"消掉四份 `pad2`"曾加过一个 `strings.pad_left`，
> 后来发现 `pad_start` **早就在**（`c07a9f6` 就有），语义相同且实现更好
> ⇒ 删掉 `pad_left`，四份 `pad2` 改用 `pad_start`。
> **教训：加 API 之前先看同一个模块里已有的完整方法表** ——
> 编译器报「未找到方法」时会把全部可用项列出来，那就是最权威的清单。

---

## 5. 已知缺口（写不出来，不是“不想写”）

| 缺口 | 表现 | 绕过 |
|---|---|---|
| **struct ↔ JSON 无自动转换** | `jsons.decode` 实测返回 `Dict[string, any]`（值保留运行期类型：`d["list"]` 收窄后就是 `Array[int]`），但**变不成你自己的 struct**，每层仍要 `as` 收窄 + `_str`/`_int` | 收窄成 `Dict[string, any]` 后手取；或 `jsons.dig(data, "user.name")` |
| ~~**enum 不能带载荷**~~ | ✅ 2026-11-10 已补：`enum Msg { progress(int done, int total) }` + `case Msg.progress(d, t)`；跨模块 / `==` 按值 / `jsons.encode` 都支持 | 带数据的消息优先用它，别再手写 struct + int tag |
| ~~**没有 `Result`**~~ | ✅ 2026-11-09 已补：`import "Result"` 提供 `Result[T]` / `Option[T]`（`ok`/`code`/`msg`/`val`，见 AGENTS.md §11） | 新代码一律用它，别再自造 `Dict{ok,err}` / `bool` / `int -1` |
| **8 位索引上限** | struct 字段 > 255 / 函数局部量 > 255 编译报错 | 拆类型 / 拆函数（现在是**编译期报错**，不会静默） |
| **`switch` 无穷尽性检查** | 漏 case 不报错 | 用 `default` 兜底 + 注释 |

---

## 6. 一句话

四个项目里被统计出来的那些“样板”，绝大多数**不是语言写不出来**，而是：

1. 没有一份“默认形态”可参照（本文档）；
2. 编译器/LSP 不提示（还没做）；
3. 前人用了行数组，后人照着抄。

**先解决第 1 条，成本最低。**
