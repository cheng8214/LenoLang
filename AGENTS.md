# AGENTS.md —— 写 Leno 代码的默认形态

> 面向在本仓库（及其下游项目）里**生成 Leno 代码**的 AI。
> 只写结论 + 最小可复制片段；每条论证见 [`LenoDocsSrc/site/idioms.html`](LenoDocsSrc/site/idioms.html)。
> 所有条目都在 `build\leno.exe` 上实测过。

---

## 0. 文档：**不在本仓库**，在独立仓库 `LenoDocsSrc`

```
D:\CLeno\LenoDocsSrc\
├── *.md                ← 唯一的源（改文档只改这里）
├── archive\  images\
├── site\               ← 生成好的 HTML 站点（index.html + 138 篇 + 搜索索引，**产物**）
├── build_html.leno     ← 生成器（纯 Leno + LenoMarkdown，不再用 python）
└── _tpl\               ← 页面模板 / CSS / 分类表（site_data.leno）
```

- **本仓库没有 `docs/`**。要看文档：打开 `D:\CLeno\LenoDocsSrc\site\index.html`（带全文搜索）。
- 改/加文档：改 md → `cd D:\CLeno\LenoDocsSrc && d:\CLeno\Leno\build\leno.exe build_html.leno`
  （整目录重建 `site/`；旧版 `build_html.py` 已删除，别照着以前的笔记去 `python build_html.py`）。
- 代码注释里引用文档写 `LenoDocsSrc/site/<名字>.html`（如 `LenoDocsSrc/site/语言陷阱速查.html`）；
  引用**模块自带**文档仍写 `.md`（`build/leno_module/*/docs/*.md` 没转 HTML）。
- 别手改 `site/*.html`：下次生成会覆盖，且 3 MB 搜索索引会与正文不一致。

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

**只有跨界时才转成行**：`Table` 控件、跨线程传递。
内存里一律用 struct，边界处转一次。

### 1.0 造 struct：用**命名参数构造**，不要"先 new 再逐字段赋"

```leno
struct Song { string name = ""; int dur = 0; float score = 0.0 }

Song s = new Song(name = "晴天", dur = 269)   // 没列出的字段取声明时的默认值
Song t = new Song()                            // 全默认
```

- 只给需要的字段即可，**不必给全**；顺序无所谓（按名字匹配）
- 字段类型**编译期校验**：`new Song(dur = "269")` 直接报
  `字段 'dur' 类型不匹配: 期望 'int'，实际 'string'` —— 不是运行期才炸
- 尾逗号、嵌套 struct 字段（`one = inner`）、lambda 里 `return new X(...)`、
  **跨模块**（`new srow.SongRow(id = ...)`）都支持

**正例**（从一行/一个 Dict 造记录，是**一个表达式**）：

```leno
export func from_row(Array[string] row): SongRow {
    return new SongRow(
        id = rowid(row),
        name = rowname(row),
        bytes = rowbytes(row))
}
```

**反例**（多一个可变中间变量，还得自己保证不漏字段）：

```leno
SongRow r = new SongRow()
r.id = rowid(row)
r.name = rowname(row)
r.bytes = rowbytes(row)
return r
```

> 为什么提这条：`AGENTS.md` 之前的示例全是 `new X()` + 逐字段赋值，
> 于是生成出来的代码也是那个形状。实际语言支持命名参数构造，
> 而"从外部数据造记录"本来就该是一个表达式。

### 1.1 struct ↔ JSON：**不要手写解析/拼接**（2026-11-09 起）

```leno
struct Song { string name = ""; int dur = 0; float score = 0.0 }

// 写：struct → JSON（字段名 = 声明名，顺序 = 声明顺序，转义正确）
Song s = new Song(name = "晴天")
jsons.encode(s)                  // {"name":"晴天","dur":0,"score":0.0}

// 读：JSON → struct（按**字段声明类型**填，未知字段忽略，缺席保留默认值）
Song t = new Song()
jsons.fill(t, "{\"name\":\"七里香\",\"dur\":299}")
t.name                           // "七里香"
```

**不要**这样：

```leno
// ❌ 手拼：值里有 " 或换行就是坏 JSON，而且不报错
print("{\"name\":\"" + s.name + "\"}")

// ❌ 手取：每个字段都要 _str / _int，字段改名不报错
var d = jsons.decode(text) as Dict[string, any]
Song x = new Song()
x.name = _str(d.get("name", ""))
x.dur = _int(d.get("dur", "0"))
```

**四条规则**（`jsons.fill`）：源里没这个字段 ⇒ 保留默认值；未知字段 ⇒ 忽略；
`null` ⇒ 保留默认值；类型对不上 ⇒ 保留默认值。

> **容器字段按声明类型逐项转换**（2026-11-11 / 11-15 起）：`Array[T]` / `Dict[K,V]` 的元素
> 按上表同口径转（`["1",2.9,true]` 填 `Array[int]` ⇒ `[1,2,1]`）；**转不了的值存 `null`**
> （数组保长度、字典保键），字典的**键**转不了就丢掉该项。`Array[any]` / `Dict[K,any]` 才原样。
> ⚠ **容器套容器也递归**（2026-11-15 起）：`Array[Array[int]]` / `Dict[K, Array[T]]` /
> `Array[Dict[K,V]]` / `Dict[K, Dict[K2,V2]]` 逐层按同口径转换（字节码 v3.2.42 起字段带
> 完整声明类型树；序列化同步带）。内层写 `any` 仍原样（详见 `LenoDocsSrc/site/module_jsons.html` 的表）。

**能用字符串转数字**（`"269"` → `int 269`、`"8.5"` → `8.5`、`"1"` → `true`）——
因为 **SQLite 的 `column_text` / CSV 的列全是字符串**。但反过来**不做**：
`123` 填 `string` 字段不会变成 `"123"`（那样类型错误就被藏起来了）。
数值之间可互转（`8` → `8.0`）；大整数逐位保留。

**第二个参数也可以直接给 `Dict`**（DB 行就是 Dict，不必先转 JSON 字符串）：

```leno
Array[User] out = rows.map(func(Dict r) {
    User u = new User()
    jsons.fill(u, r)
    return u
})
```

> 嵌套 struct 字段必须声明成**可空**（`Song? song = null`），`fill` 会自动 `new` 出来。
> 字段名**必须与源的键名一致**（不做蛇形/驼峰自动映射）；不一致就先改键。

---

## 2. 状态：`enum` 常量

```leno
enum Page { HOME = 0; SEARCH = 1; LYRIC = 2 }    // 成员之间用 `;`（或换行），**不是 `,`**

int cur = Page.HOME
if cur == Page.LYRIC { ... }     // ✅
```

```leno
if curNav == -12 { ... }          // ❌ -12 是什么？只能靠注释
if status == "3" { ... }          // ❌ 字符串码，拼错不报错
```

> **带载荷成员**（2026-11-10 起，P4 最后一项，已收官）：`enum Msg { progress(int done, int total) }`
> 写法是 `Msg.progress(3, 10)`，匹配是 `case Msg.progress(d, t) { … }`；`_` 表示忽略该位置。
> 跨模块可用（`export enum` + `use m.Msg`），`==` **按值**比较，`jsons.encode` 输出
> `{"progress":{"done":3,"total":10}}`（用成员名，不暴露 `tag/p0`）。
> 规则刻意窄：**同一 enum 里成员要么全带参数、要么全不带**；带载荷的成员不是 int
> （`int x = Msg.progress(1,2)` 报错，也不能 `new Msg(...)`）；`switch` 主题必须是**变量**。
> 漏成员又没 `default` ⇒ 穷尽性警告。纯 int 成员的老写法**零变化**（成员仍是 int 常量）。

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

> `switch` 怎么编译（2026-11-10 起）：**≥16 个互不相同的整数字面量 case 会自动变成
> 按值排序的二叉比较树**（O(log n)），低于 16 个仍是线性链（每条 case 一条融合比较）。
> 实测 100 个 case 命中最后一个：66ms → 21ms（300k 次）。所以：
> **直接用 switch 就行，不用再为"分支多"手写分档**；只有"热路径且永远命中第一个 case"
> 这种极端分布，手写 `if` 才快几毫秒。详见 `LenoDocsSrc/site/archive/性能_分支与枚举.html`。

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

契约：返回 **int**（或 float / bigint），负数 ⇒ a 前、0 ⇒ 相等、正数 ⇒ b 前。排序**稳定**。
⚠ 返回 `bool` / `string` ⇒ **运行时错误**（此前静默当"相等" ⇒ 排序整体不生效）；
"稳定"只对**带比较器**成立，`sort()` 不带比较器走 qsort（相等元素顺序不保证，同值不可区分）。
三元是 `if 条件 then A else B`，**不是** C 的 `?:`。

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

## 6.1 数值：先查 `maths`，别手写

**反例**（四个真实项目里有 **41 处** 这种写法）：

```leno
if x < 0.0 { x = 0.0 }          // 单边夹取 —— maths.max 一行就能做
if alpha > 1.0 { alpha = 1.0 }
if alpha < 0.0 { alpha = 0.0 }  // 两句连写 = saturate
if a == b { ... }               // 浮点用 == 比较（几乎总是 bug）
```

**正例**：

```leno
x = maths.max(x, 0.0)                       // 单边夹取（int 用 maxi）
a = maths.clamp(a, 0.0, 1.0)                // 双边夹取（int 用 clampi）
a = maths.saturate(a)                       // 就是 clamp(v, 0, 1)
ok = maths.approx_eq(0.1 + 0.2, 0.3)        // 浮点比较**用这个**，别用 ==
t = maths.lerp(10.0, 20.0, 0.25)            // 12.5
u = maths.inverse_lerp(10.0, 20.0, 15.0)    // 0.5   （a, b, v）
v = maths.remap(5.0, 0.0, 10.0, 0.0, 100.0) // 50.0
```

**常用的**（全部实测过）：

| 用途 | 函数 |
|---|---|
| 夹取 | `clamp` / `clampi` / `saturate`（= 0..1）|
| 取极值 | `max` / `min` / `maxi` / `mini`（都**可变参**）|
| 插值 | `lerp` / `inverse_lerp` / `remap` |
| 环形 | `wrap(v, lo, hi)` — ⚠ **上界不含**（`wrap(360, 0, 360)` = 0）|
| 角度 | `wrap_angle` / `angle_delta` / `lerp_angle` — ⚠ **全是弧度**，见下 |
| 取整 / 量化 | `roundi` / `trunc` / `floor` / `ceil` / `fract` / `snap(v, step)` |
| 其它 | `sign` / `step` / `hypot` / `dist(x1,y1,x2,y2)` |
| 2 的幂 | `is_pow2` / `next_pow2` |
| 缓动 | `ease_in_quad` / `ease_out_cubic` / `smoothstep` / `smootherstep` …（都只收 **1 个 `t`**，不是 GLSL 那种 `(edge0, edge1, x)`）|
| 模幂 | `pow_mod(base, exp, m)`（RSA / DH / LCG 随机数）|

> ⚠ **三个"快"字辈是近似，别用于精确计算**：
> `rsqrt(4.0)` = **0.49915**（不是 0.5，Quake III 算法，误差 ~0.17%）；
> `sin_fast` / `cos_fast` 同理。要精确就用 `1.0 / maths.sqrt(x)` / `maths.sin(x)`。

**五个实测坑（写错不报错，只是结果无意义）**：

```leno
maths.wrap(360.0, 0.0, 360.0)   // 0.0  ← 上界是开区间，不是 360
maths.wrap_angle(370.0)         // -0.708 弧度 ← 它要**弧度**！
maths.wrap_angle(maths.rad(370.0))      // 0.1745 弧度 = 10° ✓
maths.clamp(5.0, 10.0, 0.0)     // 10.0 ← lo > hi 时收成 lo，不抛错
maths.fract(-2.75)              // 0.25 ← 非负（Python 风格），不是 -0.75
maths.roundi(-2.5)              // -3   ← 远离零，不是银行家舍入
```

> **角度那三个函数一律用弧度**。要处理"度"就先转：
> `maths.deg(maths.angle_delta(maths.rad(a), maths.rad(b)))`。
> 直接把度数传进去**不会报错**，只会静默算出无意义的值 —— 这是这块最容易踩的坑。

> **已知缺口**（不用找了，标准库确实没有）：`gcd` / `lcm` / `is_prime` /
> `factorial` / `cbrt` / `comb`。四个项目里使用量为 **0**，需要时自己写。

## 6.2 字段名 / 字段类型：用 `reflect`，别手抄

```leno
import reflect

reflect.fields(s)             // Array[string]，声明顺序
reflect.field_type(s, "id")   // "int" / "Array[struct Item]" / "i64"
reflect.get(s, "id")          // any，字段值原样
reflect.has_field(s, "x")     // bool，探测用，**不抛**
```

**不要**为每个 struct 手抄一份字段名列表 / 手写 `to_dict`：抄漏一个不会报错，
只是少一行（`Table` 的列、表单标签、文档表格同理，都该从 `fields` 生成）。

`reflect` 是**只读**的 —— 写一律走 `jsons.fill`（按声明类型转换）。它**不做**
enum 值反查（enum 成员运行期就是 `int`，无从知道属于哪个 enum）。`cstruct` 也能用，
`field_type` 给的是 C 宽度：`i32` / `f32` / `u8[4]` / `u32 : 3`。

---

## 6.3 读配置：用 `LenoConfig`，别自己手拼

```leno
import "Config" as config
use config.Config

var r = config.load("app.json", "APP")     // 第二个参数 = ENV 前缀
if r.isErr() { … }
var c = r.get()
var port = c.getInt("server.port", 8080)
```

- 取值顺序：**默认值 < JSON 文件 < ENV**（`APP_SERVER_PORT`，全大写、点转下划线）
- `prefix` 为空 ⇒ **不读 ENV**（否则会捡一堆同名系统变量，"配置自己变了"最难查）
- 类型**不猜**：JSON 里把 `"8080"` 当 int 读会报错；ENV 走严格解析（`res.parseInt`）
- `requireStr/requireInt` 缺（含显式 `null`）⇒ 抛错；`getX(key, default)` 缺 ⇒ 默认值
- 只做 JSON。**别**为配置引入 TOML/YAML 的"够用版"解析器 —— 长尾写不全就是新的静默错源
- ⚠ `Result[X]` 里的 `X` 要在当前文件 `use` 导入；少了它，`r.get()` 会退化成未约束的 `T`
  （报「类型 'T' 没有方法 …」）—— 这是"类型名要导入"的通用规则，不是 `Result` 特有

## 6.4 时间：只有「本地 / UTC」两种口径

```leno
times.now()                              // epoch 秒
times.format(ts, "%Y-%m-%d")             // 本地时区
times.format_utc(ts, "%Y-%m-%d")         // UTC
times.parse("2026-11-24", "%Y-%m-%d")    // 按本地时区解释
times.make(2026, 11, 24, 13, 5, 9)       // 本地时区；不存在的日期会报错
times.datetime(ts)                       // [年,月,日,时,分,秒]；不传参 = 现在
times.utc_offset(ts)                     // 该时刻本地相对 UTC 的偏移秒数（东八 = 28800）
```

- **没有 IANA 时区名**（`Asia/Shanghai` 那种）。要按别的时区算，用 `utc_offset` 自己加减秒
- `parse` 只支持 `%Y %m %d %H %M %S`（Windows 没有 strptime）—— 别拿 `%b` / `%z` 去解析
- `make` / `parse` 会**反查校验**：2 月 30 日不会被静默滚到 3 月 2 日
- `times.ms/us/ns` 是**单调计时**（取差值用），不是挂钟；挂钟走 `now`

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

## 7.1 并发：线程入口用模块级函数 + Channel

**反例**（看着能跑，结果静默丢失）：

```leno
// ❌ 想让线程改外面的状态
int progress = 0
var t = threads.start(func() {
    progress = 100          // 改的是**副本**
})
t.join()
print(_str(progress))       // 0   ← 不报错，就是没生效
```

**正例**（入口是模块级函数，结果走 Channel）：

```leno
import threads

func worker(any ch, int k) {
    if ch is Channel {          // any 上不能直接 .send，先收窄
        var c = ch
        c.send(k * 2)
    }
}

main() {
    var ch = threads.channel(1)
    var t = threads.start(worker, ch, 21)
    var got = ch.receive()
    t.join()
    print(_str(_int(got)))      // 42
}
```

**跨线程到底传什么（2026-11-09 在 `leno.exe` 上逐条实测）**

| 传什么 | 结果 |
|---|---|
| `int` / `float` / `bool` / `null` | 直接复制 |
| `string` / `Array` / `Dict` | 深拷贝（递归）|
| **`struct` 实例**、`Array[struct]` | **深拷贝，可用**（2026-10-12 起）。`type()` 仍是 `P`，字段读得到 |
| `Channel` 本身 | 引用共享（可以传给子线程，这是标准做法）|
| 闭包 / 函数 | `type()` 是 `func`，**收到也调不了** |
| `Ptr` / cstruct 实例（或字段里装它们的 struct）| 不能跨线程，**编译期**就报「不能跨线程传递此类型」|
| `File` | 不能跨线程，但**运行期**才炸（见下面第 2 条）|
| 闭包**捕获**的东西（局部变量 / 局部 struct / `self`）| **一律是副本** |

> ⚠ `LenoDocsSrc/site/threads使用指南.html` 的表里原来写着「struct 实例：需要序列化，暂不支持」——
> 那是 **2026-10-12 之前**的状态，已过期（本次已改）。别再据此把 struct 拆成数组数组。
> 权威清单在 `LenoDocsSrc/site/module_threads_api.html` 的「可传递类型」表。

**三条必须记住的**

1. **闭包捕获 = 拷贝，改动不回传。** 子线程里 `self.cnt = 999`，`join` 之后父对象还是原值
   （实测：子线程打印 999，主线程仍是 7）。要回传就用 Channel，别靠闭包改外部状态。
2. **`self` 里含「打开的 File」时，捕获它会让 `threads.start` 抛「未知异常」** ——
   报错信息里完全看不出原因（实测：`File` 字段为 `null` 时正常，真 `files.open` 之后就炸）。
   这就是「线程入口必须是模块级函数」这条规矩的由来。
   （`Ptr` / cstruct 的情况更好：编译器**直接拒绝**，不会等到运行期。）
3. **`any` 两边都要收窄**：发的时候 `if ch is Channel { var c = ch; c.send(x) }`，
   收的时候 `if got is P => p { p.n }`（`receive()` 返回 `any`）。

**每线程状态：用 `thread_local var`（2026-11-15 起），不要用模块级 var 存缓存**

```leno
// lib/crc.leno —— 每个线程各一份（表/缓存/计数器都该这样写）
thread_local var counter = 0
thread_local Array[int] table = []

export func crc_init() {
    if table.len() == 0 { for 0 : 255 to n { table.add(n) } }   // 每个线程建自己那份
}
```

```leno
// ❌ 模块级 var 是全线程**共享的同一份槽**：子线程写进去的对象属于它自己的堆，
//    线程退出/GC 之后别的线程读到的是 185 / -1 / 0 这类**残值**（不报错、不崩，只是值没了）
Array[int] g_table = []
int g_ready = 0
```

- 初值只能是**字面量或空容器**（`0` / `""` / `[]` / `{}`）；要跑代码初始化就
  `thread_local var t = null` + 首次访问时 `if t == null { t = 构建() }`
- 只能在**模块顶层**；**不能 `export`**（每线程一份 ⇒ 导出没有一致值）
- 主线程 import 时构建一次、之后全线程**只读**也是安全的（模块对象在所有 GC 的根里）
- 运行期会检测"子线程把本线程堆的对象写进模块级 var"并打一次警告；
  把 `threads.local_*` 留着给"键是运行期才知道"的场景



**批量并行**（不用自己起线程）：

```leno
var ch = threads.channel(8)
int n = threads.parallel_for(4, func(int i) { ch.send(i * 10) })   // 这里用闭包是安全的
int sum = 0
for 0 : n - 1 to k { sum = sum + _int(ch.receive()) }
print(_str(sum))                                                   // 60
```

> `parallel_for` 的闭包**只**捕获 `ch`，而 Channel 是引用共享 ⇒ 安全。
> 它捕获别的可变状态一样是副本 —— 规则没变。等一组线程用 `threads.join_all(Array[Thread])`。

**大只读数据反复跨线程 → `_freeze`（零拷贝）**：默认传值都是深拷贝，同一份数据每个任务拷一遍最亏。

```leno
Frozen[Array[int]] snap = _freeze(big)    // 一次性 O(n)；之后只读
ch.send(snap)                             // O(1)，不再拷贝
var hits = threads.map(idx, func(int i, int no) { return snap[i % snap.len()] })
Array[int] copy = _thaw(snap)             // 要改就解冻出副本（O(n)）
```

- 写 frozen 是**编译错误**（`Frozen[T]` 与 `T` 双向不兼容）；形参写成 `Frozen[Array[int]]` 才能接收。
- 判"是不是冻结"用 `_is_frozen(x)`；`x is Frozen[...]` / `as Frozen[...]` 被**禁用**（运行期只有对象标签）。
- 只冻**数据**：`Ptr` / `cstruct` / `File` / `Channel` / `Atomic` / 闭包编译期就拒；含环的数据运行期拒。
- 用法与坑：`LenoDocsSrc/site/frozen使用指南.html`（原理：`大对象零拷贝共享设计.html`）。

---

## 8. 写之前先跑一遍

```bat
:: 编译检查（不产出文件，最快）
build\leno.exe --check 你的文件.leno

:: 语言断言套件
build\leno.exe assert\run_tests.leno build\leno.exe assert

:: 仓库内 examples 全量
build\leno.exe --check <file>

:: 改了语言/模块之后要打包（-p）时：先重建 VM 基底
::   build.bat 只重建 leno.exe；-p 内嵌的是 leno_vm.exe（另有 GUI 版）
::   漏了这步 ⇒ 打包产物是旧运行时，运行时报「未找到 native 函数」
build_vm.bat
```

**提交前必须全绿**：断言 685/685、`examples` 812/812。

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
| `{n:y}` | 插值说明符 `y` 不存在（**2026-11-02 起编译期报错**，并列出可用转换符） | `{n:d}` / `{n:.2f}` |
| `d["a"]["b"]`（`d` 是 `any`） | 不能在 `any` 上直接下标 | 先 `as Dict`，或 `jsons.dig(d, "a.b")` |
| `new Route(method: "GET", ...)` | `new` 的命名实参用 **`=`**，不是 `:`（`:` 是 Dict 字面量 / 插值） | `new Route(method="GET", path="/x")` |
| `f(..., func(x) {` 换行 `...` `})` 内联多行 lambda 当实参 | 解析器在多行 lambda 处报「期望 ')' / 期望表达式」 | 把 handler 写成**单独的具名函数**再传：`f(..., hPing)` |
| `import "Result" as res` 而本文件有局部/字段叫 `res` | 模块别名被**遮蔽**（`WARN_SHADOW_MODULE_ALIAS`），`res.ok(...)` 报「在该对象上找不到方法」 | 别名叫 `rstd`（本仓惯例），或给那个局部量改名 |
| 表格行写成 `rows.add([s.title, s.artist, s.album])` | **列顺序即契约**：插一列要改所有构造点，漏一处就**串列**（不报错、看着像数据错） | 用 `tb.set_struct_keys([...])` + `tb.add_struct(s)`（2026-11-10 起）：映射只写一次，写在表头旁边 |
| `S s = S`（把**类型名**当值） | 2026-11-10 前**静默编过**，运行期 `_str(s)` 得到字符串 `"S"`（实测） | 现在编译期报「'S' 是类型名，不能当值用」；要实例化写 `new S()` |
| `switch a { case B.x … }`（case 用**另一个 enum** 的成员） | 两个 int 枚举的成员都是整数，数值撞上就**静默命中错误分支**（实测 A.x=0/B.x=0 走了 B.x 那条） | 现在编译期报错；要用同一 enum 的成员，或改 `case is` 做类型分支 |

---

## 10. 已知写不出来的（别浪费时间绕）

| 需求 | 现状 |
|---|---|
| `enum` 里混用"带载荷 / 不带载荷"成员 | 不支持（有意）：值是 int 与值是对象两种表示并存会让 switch / == / 穷尽性分叉 |
| `jsons.fill(枚举值, json)` 反向填充 | 没有（载荷"属于哪个成员"是运行期概念）⇒ 用 `switch` 分派 |
| struct ↔ JSON 自动转换 | 没有，手写 `as` 收窄 + 字段赋值 |
| `switch` 穷尽性检查 | 没有，用 `default` 兜底 |
| 函数局部量 > 255 | **编译期报错**（不会静默），需拆函数。寄存器号保持 8 位是**量过之后的决定**（四个项目 1.6 万个函数最高只用 134，见 `LenoDocsSrc/site/待办与路线图.html` 的 P3 后半）；高水位 ≥ 200 时会先给 `WARN_REG_PRESSURE` 预警 |
| struct 字段 > 65535 | 编译期报错；**255 不再是上限**（2026-11-10 起字段索引走宽索引指令 `OP_*_FIELD_W`） |
| 自定义可遍历对象（`for` 吃 `has_next/next`） | **未实现**，先 `toArray()` 或手动 `while` |

---

## 11. 错误与空值：`Result` / `Option`（2026-11-09 起）

```leno
import "Result" as res
use res.(Result, Option)

Result[int] n = res.parseInt(txt)          // 严格解析：不 trim、不认 nan/全角/十六进制
if n.isErr() { print(n.errText()) }        // "PARSE: 不是整数（含非数字字符）：12a"
int v = n.orElse(0)                        // 默认值必须**显式**给（不吃"静默 0"）
Result[string] s = res.mapResult(n, func(int x) { return _str(x) })   // 失败短路，code/msg 带过去
Option[string] o = res.none[string]()      // T 只在返回值里 ⇒ 调用点写 [T]
```

| 场景 | 用 |
|---|---|
| 可能失败（IO / 网络 / 解析 / DB / 鉴权） | `Result[T]` —— `ok` / `code` / `msg` / `val` |
| 可能"没有值"但不算失败 | `Option[T]` —— `some` / `val` |
| 函数内部临时判空 | 继续用 `T?`（跨**模块边界**才换 Option） |

**五条不要**：
- 不要再新增 `Dict{ok,err}` / `bool` / `int -1` / `error` 字段这套（同时存在等于每次都要重学）
- 不要用 `code == 0` 判成功 —— 判据只有 `ok`
- 不要读失败结果的 `val`（0/""/null 没有意义），要默认值用 `orElse`
- 不要把 SQL 的 NULL 压成 `""` / `0`：`stmt.column_value(i)` 给 `null`、`stmt.column_is_null(i)` 单独判
- 不要在能恢复的路径上用 `unwrap()`（它会抛）

**HTTP 服务端：优先结构化的 `Req` / `Res`**（2026-11-10 起，LenoHTTP）

```leno
use http.(Req, Res, Route)

func hSearch(Req r): Res {                     // 字段是真类型，不用再 `req["query"] as Dict`
    return http.resJson({"q": r.q("q"), "user": r.user}).header("X-Req-Id", "abc")
}

http.mount(app, [ new Route(method="GET", path="/api/search", auth=true, handler=hSearch) ])
// 老写法 `http.get(app, path, func(Dict): Dict {...})` 仍然可用，两条路进同一张路由表
```

**统计/`Stats`**：空数组不再返回 `0.0`，一律 `Option[T]`（`st.mean([]).isNone()`）；
`money("12.34")` 是 `Result[int]`（分）。

**enum 的 `switch` 要写全，或者写 `default`**：主题是 enum 且漏 case、又没有 `default` 时，
编译器给 `WARN_SWITCH_NOT_EXHAUSTIVE` 并列出缺的成员（漏掉的值会**穿过去什么都不做**）。
只是警告，不打断编译。⚠ `var m = Mode.on` 这种**变量**写法也覆盖了（2026-11-23 修）：
成员在语义期被折成纯数字，靠 `Symbol.enum_origin` 找回来源枚举 —— 类型仍是 `int`。

## 12. 工作区卫生：`build/_*` 用完就删（2026-11-23 的教训）

`build/` 是**允许入库**的目录（`leno_module/` 在里面），所以 `.gitignore` 只忽略
`build/_*` —— 临时探针/命令输出按约定都以 `_` 开头放在 `build/` 下，
**正因如此它们不会提醒你"该删了"**（git 永远看不到）。实测一次压测攒下 20+ 个
`_*.txt` 与 3 个目录（`_stress` / `_bigbench` / `_doctest`，合计约 9.6MB），最后由用户来问。

**约定**：

1. 临时文件一律用 `build/_` 前缀（`_probe_x.leno`、`_out.txt`、`_run.ps1`…）；
2. **每轮结束前**执行一次收尾（在 `build/` 下）：
   ```
   del /q _*.txt _*.py _*.ps1 _*.leno
   rmdir /s /q _stress _bigbench _doctest
   ```
3. **保留**：`leno.exe` / `leno_vm.exe` / `leno_vm_gui.exe` / `leno_icon.*` /
   `leno_module/` / `.lenocache`（缓存，删了只是下次编译慢一点）；
4. 需要长期保留的脚本放 `bench/`（入库），不要留在 `build/_`。

---

## 13. 结构体命名：运行期**按名字**索引，通用名会撞（2026-11-27 的教训）

四张类型表（struct / face / enum / 别名）在运行期**全局按名字索引**。两个模块各定义一个同名
`export struct` 时，编译器只能证明"等价"才放行，否则报：

```
跨模块同名 struct 'X'：a.leno 与 b.leno 各定义了一份且无法证明等价
  ⇒ 四张类型表全局只按名字索引、运行期只能保留一份。
    字段索引、方法体在编译期都按各自模块的定义确定，混用会**静默读到同序号字段**或跑到另一份方法体
```

它不是"重名了改一下"那么轻：报错只在**同时加载两者**时出现，很容易先跑通一半测试、上线后
在某个组合路径上炸，而且症状是"读到错字段"，不是崩溃。

真实案例：新写的过渡动画内核叫 `Anim`，和框架原有的帧动画播放器（`sdl_anim.leno`，
`SDL3.loadAnim()` 返回它）撞了。改叫 `Tween` 才解决。

**约定**：

1. `export struct` 一律带**模块语义的前缀/后缀**，别用通用词 ——
   `Anim` / `Node` / `State` / `Item` / `Config` / `Info` / `Result` 这类都会撞；
   写 `ScrollPhys` / `Tween` / `TreeViewNode` / `Vec2` 这种能看出归属的名字 ✓
2. 新增 `export struct` 前先跑一次全量重名检查（在 `build/leno_module/LenoSDL3` 下）：

   ```
   Select-String -Path lib\*.leno -Pattern "^export struct (\w+)" |
     ForEach-Object { $_.Matches[0].Groups[1].Value + "|" + $_.Filename } |
     Group-Object { $_.Split("|")[0] } | Where-Object { $_.Count -gt 1 }
   ```

   有输出就是撞了，先改名。当前 68 个 `export struct`，无重名。
3. 只在**本文件内部**用的结构体不要 `export`（不导出就不进全局限定，随便叫 `Item` 都行）。
4. 同理适用于 `face` / `enum` 的名字。
