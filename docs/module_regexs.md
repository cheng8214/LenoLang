# Leno 正则表达式模块 (regexs)

本文档详细说明 `regexs` 模块提供的正则表达式操作方法。

## 目录

- [使用方式](#使用方式)
- [方法列表](#方法列表)
  - [匹配检查](#匹配检查)
  - [查找与提取](#查找与提取)
  - [替换](#替换)
  - [分割](#分割)
  - [分组](#分组)
  - [工具](#工具)
- [正则表达式语法](#正则表达式语法)
- [示例代码](#示例代码)

---

## 使用方式

### 模块调用

```leno
import regexs

var text = "Hello, World!"
var result = regexs.match(text, "Hello.*")  // true
```

---

## 方法列表

### 匹配检查

#### `match(str, pattern)`

**锚定匹配**：从字符串开头开始匹配，等价于 `^pattern`。不搜索子串。

> ⚠️ **注意**：`match` 不是全文搜索！要搜索字符串中任意位置的匹配，请用 `find()`。例如：
> - `match("abc set_volume", "set_")` → `false`（不以 "set_" 开头）
> - `find("abc set_volume", "set_") >= 0` → `true`（搜索到子串）

**参数**:
- `str` (string): 要检查的字符串
- `pattern` (string): 正则表达式模式

**返回**: `bool` - 是否从开头匹配

```leno
regexs.match("hello123", "[a-z]+")        // true (以字母开头)
regexs.match("hello123", "[0-9]+")        // false (不以数字开头)
regexs.match("hello123", "\\d+")          // false (同上 —— `\d` 已等价 `[0-9]` ✓)
// ⚠ 这个 `\d` 有段历史：2026-10-01 之前它被当**字面字母 d**（静默失配 ✗），
//    中间一版改为报错挡住，现在**已展开成数字类** ✓（详见「正则表达式语法」的转义表）
regexs.match("hello", "^[a-z]+$")         // true (全字匹配)
regexs.match("Hello123", "^[a-z]+$")      // false (大写H不匹配)
```

---

#### `match` vs `find` 对比

| 函数 | 行为 | 等价于 | 适用场景 |
|------|------|--------|---------|
| `match` | 从开头匹配 | `^pattern` | 验证字符串格式、前缀匹配 |
| `find >= 0` | 搜索子串 | 全文搜索 | grep 搜索、提取子串 |

---

### 查找与提取

#### `find(str, pattern)`

查找第一个匹配的位置。

**参数**:
- `str` (string): 要查找的字符串
- `pattern` (string): 正则表达式模式

**返回**: `int` - 匹配的起始位置（0-based，按**字符**计 ✓），未找到返回 -1

```leno
regexs.find("Hello, World!", "World")     // 7
regexs.find("Hello, World!", "xyz")       // -1
regexs.find("abc123def", "[0-9]+")        // 3
regexs.find("中文字abc", "abc")            // 3 ✓（按字符；按字节会是 9 ✗）
```

> ⚠ **2026-10-10 起改为按**字符**计** ✓（原来按字节 ✗）。理由：本语言的切片 / `.len()` /
> `strings.find` 全是**字符**口径 ✓ ⇒ 字节偏移交到手上，拿去 `s[start:end]` 必然错位 ✗。
> （注：本文档此前写"`find` 按字符 ✓、`find_all` 按字节 ✗"是**错的** ✗ —— 那一版用的例子
> `"abc中文def"` 前三个字符是 ASCII ⇒ 字节与字符**数值相同** ✗ 根本区分不出来 ✓；
> 重新实测 `"中文字abc"` 找 `"abc"` ⇒ 旧值 **9** ✗ ⇒ 两者其实**一直一致**，都是字节 ✗）

#### `find_all(str, pattern)`

查找所有匹配的位置信息。

**参数**:
- `str` (string): 要查找的字符串
- `pattern` (string): 正则表达式模式

**返回**: `Array[RegexMatch]` - 匹配信息数组（**字段类型编译期已知**，v3.2.7 起）

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `start` | `int` | 匹配起始位置（0-based，按**字符**计 ✓，2026-10-10 起；原按字节 ✗） |
| `last` | `int` | 匹配**末位**位置（0-based，按字符，**含** ✓ —— 与切片 / `for a:b` 同一闭区间口径） |
| `text` | `string` | 匹配到的子串 |

```leno
var matches = regexs.find_all("abc123def456", "[0-9]+")
// [RegexMatch{start: 3, last: 5, text: "123"}, RegexMatch{start: 9, last: 11, text: "456"}]

for matches to m {
    io.print(m.text + " @" + m.start)     // 字段类型已知 ⇒ 不做收窄（m.text 是 string、m.start 是 int）
    var seg = "abc123def456"[m.start : m.last]   // ⇒ "123"：直接切，不用 -1 ✓
}
```

> ★ **2026-11-20：`end` 已删除，改名 `last`（闭区间末位）** —— 全语言的下标/整数区间统一成
> **闭区间**（切片 / `for a:b` / `switch case a:b` / `strings.slice` 同款），`RegexMatch` 是当时
> 唯一一处"叫 `end` 却**不含**末尾"的例外。删除而不是改语义，是为了让旧代码**编译期报错**：
> `m.end` ⇒ 「struct 'RegexMatch' 没有字段 'end' —— 它改名了…」（不会静默差一格 ✗）。
> 换算：旧 `end` = 新 `last + 1`；空匹配时 `last = start - 1`（切片得空串 ✓）。
> ⚠ 字节码版本 v3.2.43 ⇒ **v3.2.44**、模块缓存 v58 ⇒ **v59**（字段号含义变了 ⇒ 旧产物整体失效重编 ✓）。
> ⚠ `start` / `last` **2026-10-10 起是**字符**下标** ✓（与 `strings.find` / 切片 / `.len()` 同口径 ✓；
> 原来按字节 ✗ ⇒ 含中文时拿 `start` 去切片必然错位 ✗）；实测 `"a中b中"` 找 `"中"` ⇒ `start` = 1, 3 ✓。
> ⚠ native struct 名（`RegexMatch`）**不能当类型标注**（不在符号表里）；让编译器推断
> （`var m = matches[0]`）再按字段用即可。

#### `extract(str, pattern)`

提取第一个匹配的子串。

**参数**:
- `str` (string): 要提取的字符串
- `pattern` (string): 正则表达式模式

**返回**: `string | null` - 匹配的子串，未找到返回 null

```leno
regexs.extract("Email: user@example.com", "[a-z]+@[a-z.]+")  // "user@example.com"
regexs.extract("abc123def", "[0-9]+")                       // "123"
```

#### `extract_all(str, pattern)`

提取所有匹配的子串。

**参数**:
- `str` (string): 要提取的字符串
- `pattern` (string): 正则表达式模式

**返回**: `array` - 匹配的子串数组

```leno
regexs.extract_all("abc123def456ghi", "[0-9]+")  // ["123", "456"]
regexs.extract_all("hello world", "[0-9]+")      // []
```

---

### 替换

#### `replace(str, pattern, replacement)`

替换第一个匹配的子串。

**参数**:
- `str` (string): 原字符串
- `pattern` (string): 正则表达式模式
- `replacement` (string): 替换内容

**返回**: `string` - 替换后的字符串

```leno
regexs.replace("hello world", "world", "Leno")     // "hello Leno"
regexs.replace("abc123def", "[0-9]+", "XXX")       // "abcXXXdef"
regexs.replace("hello", "xyz", "abc")              // "hello" (未找到，原样返回)
```

#### `replace_all(str, pattern, replacement)`

替换所有匹配的子串。

**参数**:
- `str` (string): 原字符串
- `pattern` (string): 正则表达式模式
- `replacement` (string): 替换内容

**返回**: `string` - 替换后的字符串

```leno
regexs.replace_all("a1b2c3", "[0-9]", "X")         // "aXbXcX"
regexs.replace_all("hello world hello", "hello", "hi")  // "hi world hi"
```

> **替换串支持反向引用**（`replace` / `replace_all` 同一套展开，每次匹配按**当次**捕获）：
>
> - `$0` = 整个匹配；`$1`…`$9` = 第 N 个捕获组
> - `$$` = 字面 `$`（要输出 `$100` 就写 `$$100`）；`$` 后面不是数字/`$`（`$x`、行尾 `$`）⇒ 原样
> - **只认单个数字**：`$10` 是 `$1` 后面跟字面 `"0"` ⇒ 捕获组上限虽是 16，但 `$10`–`$16` 引用不到
> - 组存在但**本次没参与匹配**（可选组没匹配上）⇒ 展开成空串
> - ⚠ **引用不存在的组 ⇒ 运行时错误**（2026-11-15 起；此前静默展开成空串 ——
>   实测 `replace("cost: b", "b", "$100")` 得到 `"cost: 00"`：想写字面价格却被当成 `$1`+"00"，
>   数据被静默吃掉 ✗）
>
> ```leno
> regexs.replace("abc123", "[0-9]+", "<$0>")     // "abc<123>"
> regexs.replace("abc", "(b)", "[$1]")           // "a[b]c"
> regexs.replace("2026-10-01", "(\\d+)-(\\d+)-(\\d+)", "$3/$2/$1")  // "01/10/2026"
> regexs.replace("x", "x", "$$100")              // "$100"   （字面 $）
> regexs.replace("x", "x", "$100")               // ✗ 运行时错误：模式里没有第 1 个组
> ```

---

### 分割

#### `split(str, pattern, limit?)`

使用正则表达式分割字符串。

**参数**:
- `str` (string): 要分割的字符串
- `pattern` (string): 正则表达式模式（作为分隔符）
- `limit` (int, 可选): 最多切成 `limit` **段**（= 最多分割 `limit-1` 次）；负数 = 不限

**返回**: `array` - 分割后的字符串数组

```leno
regexs.split("a,b,c", ",")              // ["a", "b", "c"]
regexs.split("a1b2c3", "[0-9]")         // ["a", "b", "c", ""]   ← 尾部空段保留
regexs.split("a,b,c,d", ",", 2)         // ["a", "b,c,d"]        ← limit=2 ⇒ 只切 1 次、共 2 段
```

> ★ 2026-11-15 两条修正（都与 `strings.split` 对齐）：
> - **尾部空段保留**：`"a,b,"` ⇒ `["a","b",""]`（旧实现静默丢掉最后那段 ⇒ 同一文本
>   `strings.split` 给 3 段、`regexs.split` 给 2 段）
> - **零宽模式不再崩溃**：`regexs.split("ab", "")` 此前会无限追空串、把数组撑爆后
>   **0xC0000005**；现在跳过零宽匹配 ⇒ 返回 `["ab"]`（原串原样）
> - 空输入 ⇒ `[]`（同 `strings.split("", ",")`）

---

### 分组

#### `groups(str, pattern)`

获取匹配结果数组：`[整体匹配, 组1, 组2, ...]` —— **2026-10-01 起真的含捕获组** ✓

**参数**:
- `str` (string): 要匹配的字符串
- `pattern` (string): 含 `(...)` 的正则

**返回**: `Array[string]` - 长度 = `1 + 组数`；**无匹配 ⇒ `null`**
- 第 0 个元素是整体匹配；
- **未参与**本次匹配的组（如 `(a)|(b)` 命中 "b" 时的组 1）⇒ **空串** `""`（元素类型是 string，不用 null ✓）；
- 重复组（如 `(ab)+` 命中 "abab"）取**最后一次**匹配 ⇒ `"ab"`（与 Python 一致 ✓）。

```leno
var g = regexs.groups("2026-10-01", "([0-9]+)-([0-9]+)-([0-9]+)")
// ["2026-10-01", "2026", "10", "01"]

var g2 = regexs.groups("user@example.com", "([a-z]+)@([a-z.]+)")
// ["user@example.com", "user", "example.com"]

var g3 = regexs.groups("b", "(a)|(b)")
// ["b", "", "b"]        ← 组 1 未参与 ⇒ 空串
```

> ⚠ 捕获要正确，必须让**失败的分支撤掉已记录的组** —— 本引擎用"捕获日志 + 回滚水位"实现 ✓。
> 反向引用（替换串里的 `$1` / `$0` / `$$`）自 2026-10-01 起**也已支持**（见 `replace` 一节 ✓）。

---

### 工具

#### `escape(str)`

转义字符串中的正则特殊字符。

**参数**:
- `str` (string): 要转义的字符串

**返回**: `string` - 转义后的字符串

```leno
regexs.escape("hello.world")     // "hello\\.world"
regexs.escape("a+b*c?")          // "a\\+b\\*c\\?"
regexs.escape("[test]")          // "\\[test\\]"
```

**用途**: 当你需要将用户输入作为字面量匹配时，先使用 escape 转义。

```leno
var user_input = "hello.world"
var pattern = regexs.escape(user_input)  // "hello\\.world"
regexs.match("hello.world", pattern)     // true
regexs.match("helloXworld", pattern)     // false (点号不再匹配任意字符)
```

---

## 正则表达式语法

**本引擎是自研的简易实现**（`src/module/regexs/regexs.c`）—— **不是 POSIX ERE**，也不等价于
Python 的 `re`，只支持下面这些。

⚠ **不支持的语法不会静默当字面量，而是直接报错**（`无效的正则表达式：…`，2026-10-01 起）：
`\q`、`[:digit:]`、`a{3,2}` 这类写法**编译期就炸**，不会"看起来没匹配上" ✓

✅ **支持回溯、捕获组、`\d` `\w` `\s` 简写、`\b` 词边界、`{n,m}` 量词**：
- 贪婪量词会**回退** —— `[a-z.]+\.[a-z]+` 匹配 `ab.c.def` 得到 `true`（旧实现静默给 `false` ✗）；
- `(...)` **真的产生捕获** —— `groups()` 返回 `[整体, 组1, 组2, ...]`（见「分组」一节 ✓）；
- 替换串里可用**反向引用** —— `$1` / `$0` / `$$`（见 `replace` 一节 ✓）；
- **转义简写**：`\d` = `[0-9]`、`\w` = `[A-Za-z0-9_]`、`\s` = 空白，取反 `\D` `\W` `\S` ✓
  （字符类**内**只认小写：`[\d]` ✓；`[\D]` 语义含糊 ⇒ 报错 ✓）；
- **词边界** `\b` / `\B`（零宽断言 ✓）、**控制字符** `\t` `\n` `\r` `\f` `\v` ✓
  —— 于是"只替换整词"这种常见需求可以直接写：`replace_all(s, "\\bcat\\b", "DOG")` ✓；
- 代价：病态 pattern（如 `(a+)+b` 匹配一长串 a 而**没有** b）会触发"灾难回溯"——**慢但不会错**，改用更具体的 pattern 即可 ✓

### 基本元字符

| 元字符 | 说明 | 示例 |
|--------|------|------|
| `.` | 匹配任意**单个字符**（按 UTF-8 字符走，不是按字节） | `a.c` 匹配 "abc"；`extract("中文", ".")` ⇒ `"中"` |
| `^` | 匹配行首 | `^hello` 匹配 "hello world" |
| `$` | 匹配行尾 | `world$` 匹配 "hello world" |
| `*` | 匹配前一个字符0次或多次 | `ab*c` 匹配 "ac", "abc", "abbc" |
| `+` | 匹配前一个字符1次或多次 | `ab+c` 匹配 "abc", "abbc" |
| `?` | 匹配前一个字符0次或1次 | `ab?c` 匹配 "ac", "abc" |
| `{n}` | 恰好 n 次（**2026-11-19 起支持**） | `a{3}` 匹配 "aaa" |
| `{n,}` | 至少 n 次 | `a{2,}` 匹配 "aa"/"aaaa" |
| `{n,m}` | n~m 次（闭区间） | `[0-9]{2,4}` 匹配 2~4 位数字 |
| `\|` | 或运算 | `cat\|dog` 匹配 "cat" 或 "dog" |

> **`{n,m}` 的四条规则**：① `n`、`m` 是 **0~1000** 的十进制整数（超了编译期报错，防止
> `a{100000}` 把匹配器磨死）；② **`n` 必须写**（`{,3}` 不认）；③ `m < n` 直接报错
> （`a{3,2}` 不会被悄悄当成 `{3}`）；④ 和 `*` `+` 一样**贪婪**，`{n}?` 这种非贪婪写法**不支持**。
> 要匹配字面花括号仍然转义：`\{` `\}` ✓

### 字符类

| 字符类 | 说明 | 示例 |
|--------|------|------|
| `[abc]` | 匹配 a, b 或 c | `[aeiou]` 匹配元音 |
| `[^abc]` | 匹配非 a, b, c 的字符 | `[^0-9]` 匹配非数字 |
| `[a-z]` | 匹配 a 到 z | `[a-zA-Z]` 匹配所有字母 |
| `[0-9]` | 匹配数字 | 等同于 `[0123456789]` |
| `[\-]` | 类内转义 `-`（阻止被当范围） | 匹配字面减号 |

### 转义

- **元字符转义**：`\. \* \+ \? \( \) \[ \] \{ \} \| \\ \$ \^ \/ \-` ✓
- **简写（2026-10-01 起支持）**：`\d` = `[0-9]`、`\w` = `[A-Za-z0-9_]`、`\s` = 空白（空格 / tab / 换行），
  取反 `\D` `\W` `\S` ✓；字符类**内**只认小写（`[\d]` ✓、`[\D]` 报错 ✓）
- **其余转义一律报错**（而不是"当字面量"）：

| 想写 | 结果 | 替代 |
|------|------|------|
| `\d` `\w` `\s` `\D` `\W` `\S` | ✅ 支持（展开成等价字符类） | —— |
| `\b` `\B`（词边界 / 非边界） | ✅ 支持（**零宽断言** ✓） | —— |
| `\t` `\n` `\r` `\f` `\v` | ✅ 支持（展开成真控制字符 ✓） | —— |
| `\q` 等任意其他字母 | ❌ 报错（**消息指出是哪个转义**） | —— |

> ⚠ 为什么"不支持的"必须报错、不能当字面量：以前 `\d` 被当**字面字母 d** ——
> `regexs.match("123", "\d+")` 返回 `false` 却毫无提示（而 `match("d", "\d")` 返回 `true`），
> 从 Python 搬过来的 `r"\d+"` 正是这个下场，**且编译器不给任何警告** ✗ ⇒ 现在编译期就炸 ✓

### 分组

| 语法 | 说明 | 示例 |
|------|------|------|
| `(...)` | 捕获分组 | `([a-z]+)@([a-z]+)` |

### ⚠ 不支持清单（写了会报错，别照搬 Python / POSIX）

| 语法 | 现状 | 替代写法 |
|------|------|---------|
| `{,3}`（不写下界） | ❌ 不支持 | 写全 `{0,3}` |
| `{1001}` 及更大的次数 | ❌ 不支持（上限 1000） | 拆成多段或改用 `*` `+` |
| `[:digit:]` 等 POSIX 类 | ❌ 不支持 | `[0-9]` / `[A-Za-z]` |
| 非贪婪 `*?` `+?` `??` `{n}?` | ❌ 不支持（量词一律贪婪） | —— |
| 命名组 `(?P<name>...)` | ❌ 不支持 | 普通捕获组 `(...)` + `groups()` 按**序号**取 |
| 前后查找 `(?=)` `(?<=)` | ❌ 不支持 | 先 `extract` 再判断 |
| flags（忽略大小写 / 多行） | ❌ 不支持 | 两侧都 `strings.to_lower` 再匹配（`examples\工具\leno-grep.leno` 就是这么做的 ✓） |
| `re.compile` 预编译 | ❌ 每次调用都重新编译 pattern | 循环里尽量复用同一个 pattern 字符串 |

> ★ **报错会指真因**（2026-11-16）：非贪婪 / 内联标志 / 命名组这三类此前都落到
> 「量词 '?' 前面没有可重复的元素」⇒ 用户照着去找量词，而真因是"语法没实现"。
> 现在分别报
> `不支持非贪婪量词 '+?'…`、`不支持内联标志 (?...)…`、`不支持命名组 (?P<name>...)…`。
> 普通捕获组 `(...)`、转义的 `\?`、可选量词 `a?` 都不受影响。

---

## 示例代码

### 验证邮箱格式

```leno
import regexs

func is_valid_email(string email):bool {
    // 域名段用 `{2,}`（至少 2 个字母）—— 2026-11-19 起花括号量词已支持 ✓
    //   （不支持 `{n,m}` 那会儿只能写成 `[a-zA-Z][a-zA-Z]+`）
    //   （域名段含不含句点都行 —— 2026-10-01 起支持回溯，不再需要躲"贪婪段后接字面量" ✓）
    var pattern = "^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9-]+\\.[a-zA-Z]{2,}$"
    return regexs.match(email, pattern)
}

main() {
    print(is_valid_email("user@example.com"))   // true
    print(is_valid_email("invalid.email"))      // false
    print(is_valid_email("a@b.c.com"))          // false（本例只认单级域名 —— 见上面 ②）
}
```

### 提取 URL 中的域名

```leno
import strings

func extract_domain(string url):string {
    // 这条用字符串分段（更直观、也更快 ✓）。现在用正则也行：
    //   `regexs.extract(url, "[a-zA-Z0-9.]+\\.com")` ⇒ `"www.example.com"` ✓
    //   —— 2026-10-01 起支持回溯，"贪婪段后接字面量"不再静默失配 ✓
    var by_scheme = strings.split(url, "://")
    if by_scheme.len() < 2 { return "" }
    var by_path = strings.split(by_scheme[1], "/")
    return by_path[0]
}

main() {
    print(extract_domain("https://www.example.com/path"))  // "www.example.com"
    print(extract_domain("http://api.test.com/v1"))        // "api.test.com"
}
```

### 解析日期

```leno
import regexs

func parse_date(string date_str):Dict {
    // 也可以直接 `match(date_str, "^[0-9]{4}-[0-9]{2}-[0-9]{2}$")` 先校验（2026-11-19 起 `{n}` ✓）；
    //   这里用 `extract_all` 把数字段全取出来更直白
    var nums = regexs.extract_all(date_str, "[0-9]+")

    if nums.len() >= 3 {
        return {
            year: _int(nums[0]),
            month: _int(nums[1]),
            day: _int(nums[2])
        }
    }
    return {}
}

main() {
    var date = parse_date("2024-05-28")
    // `print(date)` 现在会完整展开（`{year: 2024, month: 5, day: 28}`）；
    // 单取字段仍更直观 ⇒ 按需取值打印 ✓
    print(date.get("year"))    // 2024
    print(date.get("month"))   // 5
    print(date.get("day"))     // 28
}
```

### 清理文本

```leno
import regexs

func clean_text(string text):string {
    // ⚠ POSIX 类 `[[:space:]]` **不支持**（会报错）⇒ 把**字面**空白写进字符类：
    //   字符串里的 `\t` 由编译器转成真正的制表符 ✓
    var result = regexs.replace_all(text, "[\t ]+", " ")
    // 移除首尾空白
    result = result.trim()
    return result
}

main() {
    var messy = "  hello    world   \n\t  "
    print(clean_text(messy))  // "hello world"
}
```

### 统计单词数

```leno
import regexs

func count_words(string text):int {
    var words = regexs.extract_all(text, "[a-zA-Z]+")
    return words.len()
}

main() {
    print(count_words("Hello, World! This is Leno."))  // 5
}
```

### 敏感信息脱敏

```leno
import regexs

func mask_phone(string phone):string {
    // 反向引用（2026-10-01 起支持）：`$1` / `$2` 就是两个捕获组 ✓
    //   定长段用 `{n}`（2026-11-19 起支持）⇒ 11 位手机号一眼看得出位数 ✓
    return regexs.replace(phone, "^([0-9]{3})[0-9]{4}([0-9]{4})$", "$1****$2")
}

func mask_email(string email):string {
    // `^(.)[^@]*(@.*)$`：组 1 = 首字符、组 2 = `@` 之后的部分 ✓
    return regexs.replace(email, "^(.)[^@]*(@.*)$", "$1***$2")
}

main() {
    print(mask_phone("13812345678"))        // "138****5678"
    print(mask_email("zhangsan@qq.com"))    // "z***@qq.com"
}
```

---

## 注意事项

0. **线程安全（2026-11-15 起）**：解析用的节点池与组号计数器是**每线程一份**
   （`_Thread_local`）⇒ 多个线程同时调 `regexs.*` 互不干扰。此前是普通 static，
   实测 4 线程各 500 次 `match` 会**直接 0xC0000005 崩溃**（A 线程释放了 B 线程正在用的节点）。
   ⚠ 同一线程内**重入**仍不支持（本模块不会回调进 Leno，正常调用不会重入）。

1. **正则表达式编译错误**：模式语法错误 ⇒ **抛运行时错误**（不是静默不匹配 ✓）
   ```leno
   regexs.match("test", "[invalid")      // 抛错：无效的正则表达式（未闭合的字符类）
   regexs.match("test", "\\q")           // 抛错：无效的正则表达式：不支持的转义 "\q"
   regexs.match("test", "a{3,2}")        // 抛错：无效的量词 'a{3,2}' —— 写法只有 {n} / {n,} / {n,m}
   regexs.match("test", "a{2}")          // ✅ 合法（2026-11-19 起支持），不抛错
   ```

2. **贪婪匹配**：`*`、`+`、`{n,m}` 默认都是贪婪的，尽可能匹配更多字符（匹配不上再回退）
   ```leno
   regexs.extract("<div>content</div>", "<.*>")       // "<div>content</div>"
   regexs.extract("aaaa", "a{2,3}")                  // "aaa"（先吃满 3 个）
   regexs.match("aaab", "^a{2,3}b$")                 // true（回退到 2 个才对得上 b）
   ```

3. **空匹配**：某些模式可能产生空匹配，函数会正确处理避免无限循环

3.1 **`.` 按字符、字符类按 ASCII 字节**（2026-11-11 起）：`.` 会吃掉**一整个 UTF-8 字符**
（`extract("中文", ".")` ⇒ `"中"`，以前给半个汉字 = 非法 UTF-8 ✗）。而 `[...]`、`\w`、`\d`
这类字符类仍是**逐字节 + 只认 ASCII** 的（这是引擎的固有实现）：`\w` 不匹配汉字，
`[^0-9]` 在中文上会**按字节**前进 —— 但连续匹配（如 `[^,]+`）在 ASCII 分隔符处停下，
取出的整段仍是合法 UTF-8 ✓。要按字符处理中文，用 `.` / `strings` 的切片，或先 `to_codepoints()`。

3.2 **`{n,m}` 重复"零宽单元"不会成功**（有意）：`(){2}` / `(a|){2}` 这类单元一次都不消耗字符，
匹配器数到 0 次就停（防死循环），于是 `n > 0` 时判失败 —— Python 在这里会给"空匹配成功"。
正常写法（`a{2}`、`[0-9]{11}`、`(ab){3}`）不受影响 ✓

4. **性能考虑**：复杂的正则表达式在大文本上可能较慢，尽量使用具体的字符类
   （`{n,m}` 的上界限制在 1000、回退是 O(重复次数²)，所以别拿它当"大段重复"用）

---

*文档版本: 1.1*  
*最后更新: 2026-11-19（新增 `{n}` / `{n,}` / `{n,m}` 量词）*
