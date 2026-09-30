# LenoC 正则表达式模块 (regexs)

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

**返回**: `int` - 匹配的起始位置（0-based），未找到返回 -1

```leno
regexs.find("Hello, World!", "World")     // 7
regexs.find("Hello, World!", "xyz")       // -1
regexs.find("abc123def", "[0-9]+")        // 3
```

#### `find_all(str, pattern)`

查找所有匹配的位置信息。

**参数**:
- `str` (string): 要查找的字符串
- `pattern` (string): 正则表达式模式

**返回**: `Array[RegexMatch]` - 匹配信息数组（**字段类型编译期已知**，v3.2.7 起）

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `start` | `int` | 匹配起始位置（0-based，按**字节**计） |
| `end` | `int` | 匹配结束位置（0-based，**不含**） |
| `text` | `string` | 匹配到的子串 |

```leno
var matches = regexs.find_all("abc123def456", "[0-9]+")
// [RegexMatch{start: 3, end: 6, text: "123"}, RegexMatch{start: 9, end: 12, text: "456"}]

for matches to m {
    io.print(m.text + " @" + m.start)     // 字段类型已知 ⇒ 不做收窄（m.text 是 string、m.start 是 int）
}
```

> ⚠ **v3.2.7 起返回结构体、不再是 `dict`**：字段名与旧字典键**逐字相同** ⇒ `m.text` / `m.start`
> 这类调用点**不用改**；要改的是 `m["text"]` 下标式与 `if m is Dict` 收窄 —— 编译期就会挡住。
> 为什么不用 `Dict[string, ...]`：三个键**类型不齐**（`start`/`end` 是 int、`text` 是 string），
> 同质的 `Dict` 表达不了"这个键 int、那个键 string"（与 `dirs.stat` 改 `DirInfo` 同一判断，见实例十三/二十二）。
> ⚠ `start` / `end` 是**字节**偏移（不是字符索引）—— 含中文时与 `strings.find` 的字符索引不同口径。
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

---

### 分割

#### `split(str, pattern, limit?)`

使用正则表达式分割字符串。

**参数**:
- `str` (string): 要分割的字符串
- `pattern` (string): 正则表达式模式（作为分隔符）
- `limit` (int, 可选): 最大分割次数

**返回**: `array` - 分割后的字符串数组

```leno
regexs.split("a,b,c", ",")              // ["a", "b", "c"]
regexs.split("a1b2c3", "[0-9]")         // ["a", "b", "c", ""]
regexs.split("a,b,c,d", ",", 2)         // ["a", "b", "c,d"] (限制2次)
```

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
`\d`、`{2}`、`[:digit:]` 这类写法**编译期就炸**，不会"看起来没匹配上" ✓

✅ **支持回溯、捕获组、`\d` `\w` `\s` 简写、`\b` 词边界**（2026-10-01 连续几轮补完）：
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
| `.` | 匹配任意单个字符 | `a.c` 匹配 "abc", "a1c" |
| `^` | 匹配行首 | `^hello` 匹配 "hello world" |
| `$` | 匹配行尾 | `world$` 匹配 "hello world" |
| `*` | 匹配前一个字符0次或多次 | `ab*c` 匹配 "ac", "abc", "abbc" |
| `+` | 匹配前一个字符1次或多次 | `ab+c` 匹配 "abc", "abbc" |
| `?` | 匹配前一个字符0次或1次 | `ab?c` 匹配 "ac", "abc" |
| `\|` | 或运算 | `cat\|dog` 匹配 "cat" 或 "dog" |

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
| `{n}` `{n,}` `{n,m}` | ❌ 不支持（`{` `}` 曾当字面量，现报错） | 手写重复，或用 `*` `+` `?` |
| `[:digit:]` 等 POSIX 类 | ❌ 不支持 | `[0-9]` / `[A-Za-z]` |
| 非贪婪 `*?` `+?` `??` | ❌ 不支持（量词一律贪婪） | —— |
| 前后查找 `(?=)` `(?<=)` | ❌ 不支持 | 先 `extract` 再判断 |
| flags（忽略大小写 / 多行） | ❌ 不支持 | 两侧都 `strings.to_lower` 再匹配（`examples\工具\leno-grep.leno` 就是这么做的 ✓） |
| `re.compile` 预编译 | ❌ 每次调用都重新编译 pattern | 循环里尽量复用同一个 pattern 字符串 |

---

## 示例代码

### 验证邮箱格式

```leno
import regexs

func is_valid_email(string email):bool {
    // ⚠ 只有一处别照搬 Python：本引擎**没有 `{n,m}` 量词** ⇒ `[a-zA-Z]{2,}` 会直接报错，
    //   写成 `[a-zA-Z][a-zA-Z]+`（= 至少 2 个字母）✓
    //   （域名段含不含句点都行 —— 2026-10-01 起支持回溯，不再需要躲"贪婪段后接字面量" ✓）
    var pattern = "^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9-]+\\.[a-zA-Z][a-zA-Z]+$"
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
    // ⚠ 没有捕获组 ⇒ 别用 `-([0-9]{2})-` 那套（`{}` 还会**直接报错**）；
    //   用 `extract_all` 把数字段全取出来更直白 ✓
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
    // ⚠ 直接 `print(date)` 只会显示 `<object>`（Dict 的打印就是这样）⇒ 取值打印 ✓
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
    //   ⚠ 量词 `{n}` 不支持 ⇒ 定长段写成 `[0-9][0-9][0-9]` ✓
    return regexs.replace(phone, "([0-9][0-9][0-9])[0-9][0-9][0-9][0-9]([0-9][0-9][0-9][0-9])", "$1****$2")
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

1. **正则表达式编译错误**：模式语法错误 ⇒ **抛运行时错误**（不是静默不匹配 ✓）
   ```leno
   regexs.match("test", "[invalid")      // 抛错：无效的正则表达式（未闭合的字符类）
   regexs.match("test", "\\d+")          // 抛错：无效的正则表达式：不支持的转义 "\d"
   regexs.match("test", "a{2}")          // 抛错：无效的正则表达式：不支持量词 {}
   ```

2. **贪婪匹配**：`*` 和 `+` 默认是贪婪的，尽可能匹配更多字符
   ```leno
   regexs.extract("<div>content</div>", "<.*>")  // "<div>content</div>"
   ```

3. **空匹配**：某些模式可能产生空匹配，函数会正确处理避免无限循环

4. **性能考虑**：复杂的正则表达式在大文本上可能较慢，尽量使用具体的字符类

---

*文档版本: 1.0*  
*最后更新: 2026-05-28*
