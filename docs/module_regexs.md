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
regexs.match("hello123", "\\d+")          // false (不以数字开头)
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

**返回**: `array` - 匹配信息数组，每个元素是包含 `start`, `end`, `text` 的字典

```leno
var matches = regexs.find_all("abc123def456", "[0-9]+")
// [
//   {start: 3, end: 6, text: "123"},
//   {start: 9, end: 12, text: "456"}
// ]
```

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

获取匹配的所有分组。

**参数**:
- `str` (string): 要匹配的字符串
- `pattern` (string): 包含分组的正则表达式模式

**返回**: `array` - 分组数组，第0个元素是完整匹配，后续是捕获分组

```leno
regexs.groups("2024-05-28", "([0-9]+)-([0-9]+)-([0-9]+)")
// ["2024-05-28", "2024", "05", "28"]

regexs.groups("user@example.com", "([a-z]+)@([a-z.]+)")
// ["user@example.com", "user", "example.com"]
```

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

LenoC 使用 POSIX 扩展正则表达式（ERE）语法。

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

### 预定义字符类

| 类 | 说明 |
|----|------|
| `[:alnum:]` | 字母数字 [a-zA-Z0-9] |
| `[:alpha:]` | 字母 [a-zA-Z] |
| `[:digit:]` | 数字 [0-9] |
| `[:space:]` | 空白字符 |
| `[:lower:]` | 小写字母 |
| `[:upper:]` | 大写字母 |

### 分组与引用

| 语法 | 说明 | 示例 |
|------|------|------|
| `(...)` | 捕获分组 | `([a-z]+)@([a-z]+)` |

### 量词

| 量词 | 说明 |
|------|------|
| `{n}` | 恰好 n 次 |
| `{n,}` | 至少 n 次 |
| `{n,m}` | n 到 m 次 |

---

## 示例代码

### 验证邮箱格式

```leno
import regexs

func is_valid_email(string email):bool {
    var pattern = "^[a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\\.[a-zA-Z]{2,}$"
    return regexs.match(email, pattern)
}

main() {
    print(is_valid_email("user@example.com"))   // true
    print(is_valid_email("invalid.email"))      // false
}
```

### 提取 URL 中的域名

```leno
import regexs

func extract_domain(string url):string {
    var pattern = "https?://([^/]+)"
    var groups = regexs.groups(url, pattern)
    if (groups.len() >= 2) {
        return groups[1]
    }
    return ""
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
    var pattern = "([0-9]{4})-([0-9]{2})-([0-9]{2})"
    var groups = regexs.groups(date_str, pattern)

    if (groups.len() >= 4) {
        return {
            year: _int(groups[1]),
            month: _int(groups[2]),
            day: _int(groups[3])
        }
    }
    return {}
}

main() {
    var date = parse_date("2024-05-28")
    print(date)  // {year: 2024, month: 5, day: 28}
}
```

### 清理文本

```leno
import regexs

func clean_text(string text):string {
    // 移除多余空白
    var result = regexs.replace_all(text, "[[:space:]]+", " ")
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
    // 将手机号中间4位替换为 ****
    return regexs.replace(phone, "([0-9]{3})[0-9]{4}([0-9]{4})", "$1****$2")
}

func mask_email(string email):string {
    // 将邮箱用户名部分脱敏
    return regexs.replace(email, "(^.)[^@]*(@.*)$", "$1***$2")
}

main() {
    print(mask_phone("13812345678"))        // "138****5678"
    print(mask_email("zhangsan@qq.com"))    // "z***@qq.com"
}
```

---

## 注意事项

1. **正则表达式编译错误**：如果模式语法错误，会抛出运行时错误
   ```leno
   regexs.match("test", "[invalid")  // 抛出错误
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
