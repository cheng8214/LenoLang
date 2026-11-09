# AGENTS.md —— 写 Leno 代码的默认形态

> 面向在本仓库（及其下游项目）里**生成 Leno 代码**的 AI。
> 只写结论 + 最小可复制片段；每条论证见 [`docs/idioms.md`](docs/idioms.md)。
> 所有条目都在 `build\leno.exe` 上实测过。

---

## 1. 数据：`Array[struct]`，不要 `Array[Array[string]]`

```leno
struct Job {
    int id = 0
    string name = ""
    int status = 0          // 状态用 enum 常量更好，见 #2
    int done = 0
    int total = 0
}

Array[Job] jobs = []
jobs.add(new Job())
jobs[0].status = 1          // ✅ 原地改，不需要重建数组
jobs[0].done = 1024         // ✅ 传进函数改、取出再改，都是同一个对象
var n = jobs[0].done        // ✅ 已经是 int，不用 _int()
```

**不要**这样：

```leno
// ❌ 行协议：每次取值都要 _int/_str，字段顺序改了不报错
var job = [id, name, "0", "0", total]
job[2] = "1"
var n = _int(job[3])
```

**只有跨界时才转成行**：`Table` 控件、`jsons.encode`、跨线程传递。
内存里一律用 struct，边界处转一次。

---

## 2. 状态：`enum` 常量

```leno
enum Page { HOME = 0, SEARCH = 1, LYRIC = 2 }

int cur = Page.HOME
if cur == Page.LYRIC { ... }     // ✅
```

```leno
if curNav == -12 { ... }          // ❌ -12 是什么？只能靠注释
if status == "3" { ... }          // ❌ 字符串码，拼错不报错
```

> `enum` 只能存 int 常量，**不能带载荷**。需要 `Progress(done, total)` 这类
> 带数据的消息时，用 struct + int tag 字段。

---

## 3. 分支：≥3 个同值比较用 `switch`

```leno
switch cmd {
    case "add"    { add() }
    case "remove" { remove() }
    case is Point(p) { use(p) }       // 类型分支
    case 0:59 { young() }             // 区间分支（闭区间）
    default { usage() }
}
```

> `switch` 会展开成**线性比较链**（没有跳转表），所以：
> **冷路径优先 switch（可读），热路径（每帧跑几千次）才手写分档。**

---

## 4. 集合变换：`map` / `filter` / `reduce`

```leno
Array[int] doubled = nums.map(func(int x) { return x * 2 })
Array[int] big     = nums.filter(func(int x) { return x > 10 })
int total          = nums.reduce(func(int acc, int x) { return acc + x }, 0)
```

- 回调可以收 `(item)` 或 `(item, index)`，`reduce` 还可以 `(acc, item, index)`
- **返回类型跟着元素类型走**：`Array[int].reduce(...)` 是 `int`，不是 `any`
- 不要手写索引循环只为了 `add(...)`

---

## 5. 排序：`sort(比较器)`

```leno
ord.sort(func(int a, int b) {
    if a < b { return -1 }
    if a > b { return 1 }
    return 0
})
```

契约：返回 **int**，负数 ⇒ a 前、0 ⇒ 相等、正数 ⇒ b 前。排序**稳定**。

**两个不要**：
- 不要改「往已排序数组插一个」（二分 + 一次移动，O(log n)）—— 全量 sort 会更慢
- 不要改「取前 N 名」的选择排序（O(n×lim) 优于 O(n log n)）
  **例外**：如果每轮还配着 `arr.remove(bi)`（那又是 O(n) 的搬移），就改全量 sort
  ——`Aria/server/api_notice.leno` 原来就是这种，改成 `sort` + 切片后更快更短。

### 4.1 回调 / 切片的两条硬性事实（实测，写错直接编译失败）

```leno
nums.map(func(int x) { return x * 2 })     // ✅ 参数**必须带类型**
nums.map(func(x) { return x * 2 })         // ❌ 「期望参数名」
```

```leno
var top = arr[0 : lim - 1]                 // ✅ Array 用 [a:b] 切片（闭区间）
var top = arr.slice(0, lim - 1)            // ❌ 「类型 'Array[T]' 没有方法 'slice'」
```

> `Array` 的方法只有这些：`add / clear / copy / filter / flatten / has / index_of /
> insert / join / last_index_of / len / map / pop / reduce / remove / reverse / sort / sum`。
> **没有** `slice` / `contains` / `first` / `sort` 的变体之类——切片走 `[a:b]` 语法。
> （`strings.slice` 是字符串的，存在；别和 `Array` 混。）

---

## 6. 字符串：先查标准库，别自己写

```leno
strings.pad_start(_str(v), 2, "0")    // "07"（按字符算，"你好" 是 2 个字符）
strings.pad_end(s, 5)                 // 右侧补空格到 5 个字符
strings.trim_unicode(s)               // 认全角空格 / NBSP（trim 只认 ASCII）
strings.to_lower(s)
strings.split(s, ",")                 // 已有！别手写 split
strings.fmt_size(1536)                // "1 KB"（字节数 → 人话，已有）
strings.lines(s)                      // 按行切（认 \r\n）
```

> **已知高频重复**：`pad2` 曾在四个项目里各抄一份。写补零/补空格前先想
> `pad_start`（`pad2(v)` 就是 `pad_start(_str(v), 2, "0")`）。
> 同理，`split` / `fmt_size` / `lines` / `format` 都**已经在标准库里**，
> 写之前先 `strings.` 看一眼可用列表（编译器报"未找到方法"时会直接列出全部可用项）。

---

## 7. 可空：`T?` + `is` 收窄，不要 `any` + 双检

```leno
int? n = try_get()
if n is int => v {
    print(_str(v))          // v 已收窄成 int
}

var x = maybe ?? 0          // 空值合并
acc ??= 1                   // 空值合并且赋值
obj?.field                  // 安全访问
```

- `?` 系列（`??` / `??=` / `?.`）只对**可空**生效，别对非空变量用
- `as` 只用于**收窄 `any`**，失败返回 null（不会抛）

---

## 8. 写之前先跑一遍

```bat
:: 编译检查（不产出文件，最快）
build\leno.exe --check 你的文件.leno

:: 语言断言套件
build\leno.exe assert\run_tests.leno build\leno.exe assert

:: 仓库内 examples 全量
build\leno.exe --check <file>
```

**提交前必须全绿**：断言 573/573、`examples` 812/812。

---

## 9. 常见误写（会产生定向报错，别以为是 bug）

| 写法 | 为什么错 | 正确写法 |
|---|---|---|
| `x => { return 1 }` | 箭头函数体只能是**单个表达式** | `x => 1` 或 `func(x) { return 1 }` |
| `(int x): int => x` | 箭头函数**不能写返回类型** | `func(int x): int { return x }` |
| `for key in dict` | `in` 是成员测试，`for` 是区间循环 ⇒ **循环体 0 次**（编译器给 2 条警告，但代码照跑） | `for dict to key`（集合在前） |
| `for arr as x` | 遍历要用 `to` | `for arr to x` |
| `arr[1:5:2]` | 切片**不支持步长** | 用循环或 `filter` |
| `new 5` | `new` 只能造 struct | 直接用 `5` |
| `{n:y}` | 插值说明符 `y` 不存在（会静默输出 `%y`） | `{n:d}` / `{n:.2f}` |
| `d["a"]["b"]`（`d` 是 `any`） | 不能在 `any` 上直接下标 | 先 `as Dict`，或 `jsons.dig(d, "a.b")` |

---

## 10. 已知写不出来的（别浪费时间绕）

| 需求 | 现状 |
|---|---|
| `enum` 带载荷（`Progress(done, total)`） | 不支持，用 struct + tag |
| 标准 `Result` / `Option` | 没有，项目内自己约定一种 |
| struct ↔ JSON 自动转换 | 没有，手写 `as` 收窄 + 字段赋值 |
| `switch` 穷尽性检查 | 没有，用 `default` 兜底 |
| struct 字段 > 255 / 函数局部量 > 255 | **编译期报错**（不会静默），需拆类型/拆函数 |
| 自定义可遍历对象（`for` 吃 `has_next/next`） | **未实现**，先 `toArray()` 或手动 `while` |
