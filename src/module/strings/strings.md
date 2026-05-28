# LenoC 字符串模块 (strings)

本文档详细说明 `strings` 模块提供的所有字符串操作方法。

## 目录

- [使用方式](#使用方式)
- [方法列表](#方法列表)
  - [基础属性](#基础属性)
  - [大小写转换](#大小写转换)
  - [修剪空白](#修剪空白)
  - [包含检查](#包含检查)
  - [查找替换](#查找替换)
  - [子串提取](#子串提取)
  - [字符串变换](#字符串变换)
  - [统计与填充](#统计与填充)
  - [分割与连接](#分割与连接)
  - [格式化](#格式化)
  - [字符编码](#字符编码)

---

## 使用方式

### 模块调用

```leno
import strings

var s = "Hello, World!"
var result = strings.reverse(s)  // "!dlroW ,olleH"
```

### 实例调用

```leno
var s = "Hello, World!"
var result = s.reverse()  // "!dlroW ,olleH"
```

### 全局函数

`format()` 是全局函数，无需导入模块即可使用：

```leno
var msg = format("Hello %s, you have %d messages", "Leno", 5)
print(msg)  // "Hello Leno, you have 5 messages"
```

---

## 方法列表

### 基础属性

#### `len()`

获取字符串长度。

**参数**: 无  
**返回**: `int` - 字符串长度

```leno
"Hello".len()        // 返回 5
"".len()             // 返回 0
strings.len("abc")   // 返回 3
```

---

### 大小写转换

#### `to_upper()`

将字符串转换为大写。

**参数**: 无  
**返回**: `string` - 转换后的字符串

```leno
"hello".to_upper()      // "HELLO"
"Hello World".to_upper() // "HELLO WORLD"
```

#### `to_lower()`

将字符串转换为小写。

**参数**: 无  
**返回**: `string` - 转换后的字符串

```leno
"HELLO".to_lower()      // "hello"
"Hello World".to_lower() // "hello world"
```

---

### 修剪空白

#### `trim()`

去除字符串首尾的所有空白字符（空格、制表符、换行等）。

**参数**: 无  
**返回**: `string` - 修剪后的字符串

```leno
"  hello  ".trim()      // "hello"
"\t hello \n".trim()    // "hello"
```

#### `trim_start()`

仅去除字符串开头的空白字符。

**参数**: 无  
**返回**: `string` - 修剪后的字符串

```leno
"  hello  ".trim_start()  // "hello  "
```

#### `trim_end()`

仅去除字符串结尾的空白字符。

**参数**: 无  
**返回**: `string` - 修剪后的字符串

```leno
"  hello  ".trim_end()    // "  hello"
```

---

### 包含检查

#### `has(substr)`

检查字符串是否包含指定子串。

**参数**:

- `substr` (string): 要查找的子串

**返回**: `bool` - 是否包含该子串

```leno
"Hello, World".has("World")   // true
"Hello, World".has("xyz")     // false
"abc".has("bc")               // true
```

#### `starts_with(prefix)`

检查字符串是否以指定前缀开头。

**参数**:

- `prefix` (string): 要检查的前缀

**返回**: `bool` - 是否以该前缀开头

```leno
"Hello, World".starts_with("Hello")   // true
"Hello, World".starts_with("World")   // false
```

#### `ends_with(suffix)`

检查字符串是否以指定后缀结尾。

**参数**:

- `suffix` (string): 要检查的后缀

**返回**: `bool` - 是否以该后缀结尾

```leno
"Hello, World".ends_with("World")    // true
"Hello, World".ends_with("Hello")    // false
```

---

### 查找替换

#### `replace(old_str, new_str)`

将字符串中所有 `old_str` 替换为 `new_str`。

**参数**:

- `old_str` (string): 要被替换的子串
- `new_str` (string): 用于替换的新子串

**返回**: `string` - 替换后的字符串

```leno
"hello world".replace("world", "Leno")     // "hello Leno"
"a b a".replace("a", "x")                  // "x b x"
```

#### `find(pattern, start?, plain?)`

查找子串在字符串中的位置。

**参数**:

- `pattern` (string): 要查找的子串
- `start` (int, 可选): 开始查找的位置（1-based，默认为1）
- `plain` (bool, 可选): 是否使用纯文本匹配（默认为false）

**返回**: `int | null` - 找到的位置（1-based），未找到返回 null

```leno
"Hello, World! Hello!".find("Hello")        // 1
"Hello, World! Hello!".find("World")        // 8
"Hello, World! Hello!".find("Hello", 2)     // 15 (从位置2开始找)
"Hello, World! Hello!".find("xyz")          // null
```

---

### 子串提取

#### `slice(start, end)`

提取从 `start` 到 `end`（不包含）之间的子串。

**参数**:

- `start` (int): 开始位置（0-based）
- `end` (int): 结束位置（0-based，不包含）

**返回**: `string` - 提取的子串

```leno
"Hello".slice(0, 3)    // "Hel"
"Hello".slice(1, 4)    // "ell"
"Hello".slice(-2, 5)   // "lo" (负数索引)
```

#### `sub_str(start, length)`

从指定位置开始提取指定长度的子串。

**参数**:

- `start` (int): 开始位置（0-based）
- `length` (int): 要提取的长度

**返回**: `string` - 提取的子串

```leno
"Hello".sub_str(0, 3)   // "Hel"
"Hello".sub_str(1, 2)   // "el"
"Hello".sub_str(-2, 2)  // "lo" (负数索引)
```

---

### 字符串变换

#### `reverse()`

将字符串反转。

**参数**: 无  
**返回**: `string` - 反转后的字符串

```leno
"abc".reverse()              // "cba"
"Hello, World!".reverse()    // "!dlroW ,olleH"
"".reverse()                 // ""
```

#### `rep(n)`

将字符串重复 `n` 次。

**参数**:

- `n` (int): 重复次数

**返回**: `string` - 重复后的字符串

```leno
"a".rep(3)      // "aaa"
"ha".rep(3)     // "hahaha"
"abc".rep(0)    // "" (空字符串)
```

#### `count(substr)`

统计子串在字符串中出现的次数（不重叠计数）。

**参数**:

- `substr` (string): 要统计的子串

**返回**: `int` - 出现次数

```leno
"abab".count("ab")         // 2
"hello world".count("l")   // 3
"aaa".count("aa")          // 1 (不重叠)
"abc".count("xyz")         // 0
```

#### `pad_start(target_len, pad_char?)`

在字符串左侧填充字符，使其达到指定长度。

**参数**:

- `target_len` (int): 目标长度
- `pad_char` (string, 可选): 填充字符（默认为空格）

**返回**: `string` - 填充后的字符串

**注意**: 如果原字符串长度已超过目标长度，则返回原字符串。

```leno
"5".pad_start(3, "0")      // "005"
"hi".pad_start(5, " ")     // "   hi"
"hi".pad_start(5, "-")     // "---hi"
"hello".pad_start(3, "0")  // "hello" (不截断)
```

#### `pad_end(target_len, pad_char?)`

在字符串右侧填充字符，使其达到指定长度。

**参数**:

- `target_len` (int): 目标长度
- `pad_char` (string, 可选): 填充字符（默认为空格）

**返回**: `string` - 填充后的字符串

```leno
"hi".pad_end(5, "!")       // "hi!!!"
"test".pad_end(8, " ")     // "test    "
"abc".pad_end(2, "-")      // "abc" (不截断)
```

---

### 分割与连接

#### `split(separator)`

使用分隔符将字符串分割为数组。

**参数**:

- `separator` (string): 分隔符

**返回**: `array` - 分割后的字符串数组

```leno
"a,b,c".split(",")              // ["a", "b", "c"]
"/usr/local/bin".split("/")    // ["", "usr", "local", "bin"]
"one two three".split(" ")     // ["one", "two", "three"]
```

**注意**: 如果分隔符为空字符串，则每个字符作为一个元素。

#### `join(array, separator)`

将字符串数组用分隔符连接成一个字符串。

**参数**:

- `array` (array): 字符串数组
- `separator` (string): 连接用的分隔符

**返回**: `string` - 连接后的字符串

```leno
strings.join(["a", "b", "c"], ",")        // "a,b,c"
strings.join(["Hello", "World"], " ")     // "Hello World"
strings.join(["path", "to", "file"], "/") // "path/to/file"
```

---

### 格式化

#### `format(fmt, ...)`

格式化字符串，支持类似 C 语言 printf 的格式说明符。

**参数**:

- `fmt` (string): 格式字符串，包含格式说明符
- `...`: 可变数量的参数，用于替换格式说明符

**返回**: `string` - 格式化后的字符串

**支持的格式说明符**:

| 说明符 | 类型 | 示例 |
|--------|------|------|
| `%s` | 字符串 | `format("Hello %s", "World")` → `"Hello World"` |
| `%d` 或 `%i` | 整数 | `format("Age: %d", 25)` → `"Age: 25"` |
| `%f` | 浮点数 | `format("Pi = %f", 3.14)` → `"Pi = 3.140000"` |
| `%c` | 字符 | `format("Char: %c", 65)` → `"Char: A"` |
| `%x` | 十六进制(小写) | `format("Hex: %x", 255)` → `"Hex: ff"` |
| `%X` | 十六进制(大写) | `format("Hex: %X", 255)` → `"Hex: FF"` |
| `%%` | 百分号 | `format("100%%")` → `"100%"` |

**使用示例**:

```leno
// 基础用法
format("Name: %s, Age: %d", "Leno", 5)     // "Name: Leno, Age: 5"
format("Pi = %f", 3.14159)                  // "Pi = 3.141590"
format("Hex: %x / %X", 255, 255)            // "Hex: ff / FF"

// 多个参数
format("%s %s %s", "a", "b", "c")          // "a b c"
format("Num: %d, %d, %d", 1, 2, 3)         // "Num: 1, 2, 3"

// 特殊字符
format("100%% complete")                    // "100% complete"
format("Char: %c", 65)                      // "Char: A"
```

**注意**: `format` 既可以作为全局函数使用，也可以通过 `strings.format` 模块调用。

---

### 字符编码

#### `byte(pos?)`

获取指定位置字符的 ASCII 码值。

**参数**:

- `pos` (int, 可选): 字符位置（1-based，默认为1）

**返回**: `int | null` - ASCII 码值，越界返回 null

**说明**: 支持负数索引，-1 表示最后一个字符。

```leno
"Hello".byte(1)     // 72  ('H')
"Hello".byte(2)     // 101 ('e')
"Hello".byte(5)     // 111 ('o')
"Hello".byte(-1)    // 111 ('o', 最后一个字符)
"Hello".byte(10)    // null (越界)
```

#### `char(...)`

将 ASCII 码转换为字符串。

**参数**:

- `...` (int): 一个或多个 ASCII 码值（0-255）

**返回**: `string` - 转换后的字符串

```leno
strings.char(72)                           // "H"
strings.char(72, 101, 108, 108, 111)       // "Hello"
strings.char(65, 66, 67)                   // "ABC"
```

---

## 索引说明

LenoC 字符串操作中的索引分为两种：

### 1-based 索引

以下方法使用 1-based 索引（与 Lua 保持一致）：

- `find()` - 返回的位置是 1-based
- `byte()` - pos 参数是 1-based

### 0-based 索引

以下方法使用 0-based 索引（与大多数编程语言一致）：

- `slice()` - start 和 end 参数
- `sub_str()` - start 参数

### 负数索引

支持负数索引的方法：

- `slice()` - 负数表示从末尾计数
- `sub_str()` - 负数表示从末尾计数
- `byte()` - 负数表示从末尾计数

---

## 性能提示

1. `len()` 操作是 O(1) 复杂度，字符串长度会被缓存
2. `slice()` 和 `sub_str()` 会创建新的字符串对象
3. `rep()` 在大重复次数时注意内存使用

---

## 与 Lua 的对比

| 方法 | LenoC | Lua | 说明 |
|------|-------|-----|------|
| 获取长度 | `s.len()` | `s:len()` 或 `#s` | 相似 |
| 大小写转换 | `s.to_upper()` | `s:upper()` | 命名不同 |
| 子串提取 | `s.slice(s, e)` | `s:sub(i, j)` | 索引不同（0-based vs 1-based） |
| 查找 | `s.find(p, i)` | `s:find(p, i)` | 相似 |
| 重复 | `s.rep(n)` | `s:rep(n)` | 相同 |
| 反转 | `s.reverse()` | 需自定义 | LenoC 特有 |
| 修剪 | `s.trim()` | 需自定义 | LenoC 特有 |
| 包含检查 | `s.has(sub)` | 需自定义 | LenoC 特有 |
| 分割 | `s.split(sep)` | 需自定义 | LenoC 特有 |
| 连接 | `strings.join(arr, sep)` | `table.concat()` | 相似 |
| 格式化 | `format()` | `string.format()` | 相似 |

---

## 示例代码

```leno
import strings

main() {
    var text = "  Hello, Leno!  "
    
    // 链式调用
    var result = text.trim()
                     .to_lower()
                     .replace("leno", "world")
                     .reverse()
    
    print(result)  // 输出: "!dlrow ,olleh"
    
    // 查找和提取
    var pos = text.find("Leno")
    if (pos != null) {
        print("找到位置: " + pos)
        var extracted = text.slice(pos-1, pos+4)  // 注意索引转换
        print("提取内容: " + extracted)
    }
    
    // 分割与连接
    var csv = "apple,banana,orange"
    var fruits = csv.split(",")
    print(fruits)  // ["apple", "banana", "orange"]
    
    var joined = strings.join(fruits, " | ")
    print(joined)  // "apple | banana | orange"
    
    // 包含检查
    if (csv.has("banana")) {
        print("找到 banana!")
    }
    
    // 字符编码操作
    var codes = []
    for 1:text.len() to i {
        codes.add(text.byte(i))
    }
    print(codes)  // 打印所有字符的ASCII码
    
    // 格式化字符串
    var name = "Leno"
    var version = 1.5
    print(format("Welcome to %s v%f!", name, version))
    // 输出: "Welcome to Leno v1.500000!"
}
```

---

*文档版本: 1.3*  
*最后更新: 2026-05-01*
