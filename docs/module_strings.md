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
  - [单位与进制](#单位与进制)
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

获取字符串的 Unicode 字符数。

**参数**: 无  
**返回**: `int` - 字符串的字符数

```leno
"Hello".len()        // 返回 5
"".len()             // 返回 0
"你好世界".len()      // 返回 4（4个中文字符）
strings.len("abc")   // 返回 3
```

#### `byte_len()`

获取字符串的 UTF-8 字节长度。

**参数**: 无  
**返回**: `int` - 字符串的字节长度

```leno
"Hello".byte_len()       // 返回 5
"你好世界".byte_len()     // 返回 12（4个中文字 × 3字节）
"hi你好".byte_len()      // 返回 8（2 ASCII + 2中文×3字节）
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
- `start` (int, 可选): 开始查找的位置（0-based，默认为0）
- `plain` (bool, 可选): 是否使用纯文本匹配（默认为false）

**返回**: `int` - 找到的位置（0-based），未找到返回 -1

```leno
"Hello, World! Hello!".find("Hello")        // 0
"Hello, World! Hello!".find("World")        // 7
"Hello, World! Hello!".find("Hello", 1)     // 14 (从位置1开始找)
"Hello, World! Hello!".find("xyz")          // -1
```

#### `byte_find(pattern, start?)`

在字符串中**二进制安全**地搜索字节序列，返回首次匹配的字节偏移量。

与 `find()` 的关键区别：

- 使用 `memcmp` 逐字节比较，**不会在 `\x00` 处截断**
- 返回**字节偏移量**（非字符索引）
- `start` 参数也是字节偏移量
- 不支持模式匹配（`^` / `$`），始终为纯文本匹配

**参数**:

- `pattern` (string): 要查找的字节序列
- `start` (int, 可选): 开始查找的字节偏移（0-based，默认为0）

**返回**: `int` - 找到的字节偏移（0-based），未找到返回 -1

```leno
"Hello, World".byte_find("World")        // 7
"Hello, World".byte_find("o")            // 4
"Hello, World".byte_find("o", 5)         // 8 (从字节偏移5开始找)
"Hello, World".byte_find("xyz")          // -1
```

**二进制数据处理示例**（含 null 字节的搜索）：

```leno
// 在含 null 字节的二进制数据中搜索
var data = strings.char(0x00, 0x01, 0x00, 0x02, 0x00, 0x03)
var pattern = strings.char(0x00, 0x02)
print(data.byte_find(pattern))  // 2 (字节偏移)

// find() 在这个场景下会失败（strstr 遇 \x00 截断）
print(data.find(pattern))       // -1 (找不到)
```

**多次搜索**（找到所有匹配位置）：

```leno
var text = "ababab"
var pos = text.byte_find("ab")
while pos >= 0 {
    print("found at: " + pos)
    pos = text.byte_find("ab", pos + 1)
}
// 输出: found at: 0, found at: 2, found at: 4
```

---

### 子串提取

#### `slice(start, end)`

提取 `start` 到 `end` 之间的子串，**闭区间（包含 `end`）** —— 与切片语法 `s[start:end]`、
数组切片 `arr[start:end]` 完全同一口径（2026-10-10 起统一；此前是"不含 end"）。

**参数**:

- `start` (int): 开始位置（0-based；**负数表示从末尾数**，`-1` = 最后一个字符）
- `end` (int): 结束位置（0-based，**包含**）。超过末尾会被钳到最后一个字符；
  **写负数返回空串**（闭区间下 `-1` 与"最后一个字符"含义冲突 ⇒ 宁空不错，要"到末尾"就写 `x.len() - 1`）

**返回**: `string` - 提取的子串

```leno
"Hello".slice(0, 2)    // "Hel"
"Hello".slice(1, 3)    // "ell"
"Hello".slice(-2, 4)   // "lo"   (负起点：倒数第二个到最后一个)
"Hello".slice(-2, 5)   // "lo"   (end 超界 ⇒ 钳到末尾)
"Hello".slice(1, -1)   // ""     (终点不支持负索引)
"Hello".slice(0, 99)   // "Hello"
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

#### `byte_slice(start, end)`

按 **UTF-8 字节偏移** 提取子串（**闭区间，包含 `end`**，与 `slice` 同口径）。适用于加密、协议解析等二进制数据处理场景。

**参数**:

- `start` (int): 开始字节偏移（0-based；负数表示从末尾数）
- `end` (int): 结束字节偏移（0-based，**包含**）。超过末尾会被钳到最后一个字节；**写负数返回空串**

**返回**: `string` - 提取的子串

**注意**: `start` 和 `end` 是字节偏移量，不是字符索引。如果截断到多字节字符中间，可能产生无效 UTF-8。请确保偏移量对齐字符边界。

```leno
"Hello".byte_slice(0, 2)      // "Hel"（ASCII，字节=字符）
"你好世界".byte_slice(0, 2)     // "你"（前3字节=1个中文字符）
"你好世界".byte_slice(3, 5)     // "好"（第3-5字节=第2个中文字符）
"hi你好".byte_slice(0, 4)      // "hi你"（2 ASCII + 3字节中文）
```

**典型用途**：加密解密后去除 PKCS7 填充：

```leno
var unpadded = decrypted.byte_slice(0, decrypted.byte_len() - pad_len - 1)
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

**返回**: `Array[string]` - 分割后的字符串数组（**元素类型编译期已知**，v3.2.6 起）

```leno
"a,b,c".split(",")              // ["a", "b", "c"]
"/usr/local/bin".split("/")    // ["", "usr", "local", "bin"]
"one two three".split(" ")     // ["one", "two", "three"]

// 元素是 string ⇒ 取出来不必收窄（v3.2.6 起；此前注册成裸 Array，元素是 any）
string first = "a,b,c".split(",")[0]
Array[string] parts = "a,b,c".split(",")
```

> ⚠ v3.2.6 前这里是**裸 `Array`**（元素 any）⇒ `s.split(",")[0]` 拿到 any、要手写 `as string`。
> 现在返回规格是 `Array[string]`（实现两个分支装的都是子串）⇒ 调用点零收窄。

**注意**: 如果分隔符为空字符串，则每个字符作为一个元素。

#### `lines()`

按行拆分，并**去掉行内的 `\r`**（CRLF / 老式 CR 文本都能直接用）。
模块式 `strings.lines(s)` 与实例式 `s.lines()` 等价（两条注册通道，行为一致）。

**参数**:

- `s` (string)：要拆分的字符串（实例式 `s.lines()` 时即接收者）

**返回**: `Array[string]` - 行数组（**元素类型编译期已知** ⇒ 取出来不必收窄）

```leno
"a\r\nb\n".lines()         // ["a", "b", ""]
"a\nb".lines()             // ["a", "b"]
"中文\r\n第二行".lines()     // ["中文", "第二行"]
"a\r\nb\rc".lines()        // ["a", "bc"]  ← 孤立 CR 不是分隔符（只在 \n 处切）
```

> **★ 与 `s.replace("\r", "").split("\n")` 逐字节等价** —— 这不是巧合，而是它的**存在理由**：
> v3.2.9 把 `files.read` / `files.write` 改成二进制通道后行尾不再被翻译 ⇒ CRLF 文本读出来行尾带
> `\r`，于是那个补丁被抄到 **9 处 / 6 个文件**（PvZ 还因此崩在渲染回调里：`_int("12\r")` 抛错）
> ⇒ 按"单一事实来源"收成这一个实现，各调用点换过来行为完全不变 ✓
>
> **⚠ 结尾有换行会多出一个空元素**（`"a\n"` → `["a", ""]`）—— 与 `split` 同语义，**不**像 Python 的
> `splitlines()` 那样丢掉空行。保留原样是为了不改动调用点的循环次数；想去掉空行请自己过滤。
>
> 护栏：`assert/test_strings_lines.leno`（两条判据：两条注册通道一致 + 与旧惯用法逐字节等价）。

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

### 单位与进制

#### `fmt_size(bytes)`

把字节数格式化成人类可读的大小（**1024 进制**：B / KB / MB / GB）。

**参数**:

- `bytes` (int): 字节数

**返回**: `string` - 人类可读的大小

```leno
strings.fmt_size(0)           // "0 B"
strings.fmt_size(1023)        // "1023 B"
strings.fmt_size(1024)        // "1 KB"
strings.fmt_size(1536)        // "1 KB"      （KB/MB 取整，不保留小数）
strings.fmt_size(1048576)     // "1 MB"
strings.fmt_size(1073741824)  // "1 GB"      （整 GB 不显示小数点）
strings.fmt_size(1610612736)  // "1.5 GB"    （GB 保留 1 位小数）
```

> 这是从应用层**四份逐字复制**的实现收编来的标准库函数（文件管理器、PE 分析器、缓存清理工具），
> 口径与其中「GB 带 1 位小数」版一致。
>
> ⚠ **只有模块形态**（没有 `1024.fmt_size()` 这种实例写法）：它收的是**数字**，实例形态只能挂进
> **数字方法表**，而那张表 28 个成员全部返回 `float`、是纯"数值计算"表；本语言里"数字 → 字符串"
> 的既有约定是自由函数（`_str()`、`format()`）⇒ 这俩属于那一族（2026-09-26 决定，理由详见
> `src/module/strings/strings.c` 里的注释）。

#### `hex(value[, width])`

把整数格式化成**大写 16 进制**字符串。

**参数**:

- `value` (int): 整数
- `width` (int, 可选): 输出位数。省略 ⇒ 最少位数、**不补零**；给出 ⇒ 恰好补到 `width` 位

**返回**: `string` - 16 进制字符串（无 `0x` 前缀）

```leno
strings.hex(255)              // "FF"
strings.hex(0x1F, 8)          // "0000001F"
strings.hex(0xDEADBEEF, 8)    // "DEADBEEF"
strings.hex(0x10, 1)          // "0"          （超出 width ⇒ 按补码只留低位）
strings.hex(-1, 8)            // "FFFFFFFF"   （负数按补码）
```

> 这是从应用层 `toHex8` / `toHex4` / `toHex2` 三份复制实现收编来的标准库函数（PE 分析器）。
> `width` 会被夹到 `1..64`；超出 64 位的高位一律补 `0`。同样**只有模块形态**（理由见上）。

---

### 格式化

#### `format(fmt, ...)`

格式化字符串，支持类似 C 语言 printf 的格式说明符，包括宽度、精度和标志位控制。

**参数**:

- `fmt` (string): 格式字符串，包含格式说明符
- `...`: 可变数量的参数，用于替换格式说明符

**返回**: `string` - 格式化后的字符串

**支持的格式说明符**:

| 说明符 | 类型 | 示例 |
|--------|------|------|
| `%s` | 字符串 | `format("Hello %s", "World")` → `"Hello World"` |
| `%d` 或 `%i` | 整数 | `format("Age: %d", 25)` → `"Age: 25"` |
| `%u` | 无符号整数 | `format("%u", -1)` → `"4294967295"` |
| `%f` | 浮点数 | `format("Pi = %f", 3.14)` → `"Pi = 3.140000"` |
| `%e` 或 `%E` | 科学计数法 | `format("%e", 1234.5)` → `"1.234500e+03"` |
| `%g` 或 `%G` | 自动选择 %f/%e | `format("%g", 1234.5)` → `"1234.5"` |
| `%c` | 字符 | `format("Char: %c", 65)` → `"Char: A"` |
| `%x` | 十六进制(小写) | `format("Hex: %x", 255)` → `"Hex: ff"` |
| `%X` | 十六进制(大写) | `format("Hex: %X", 255)` → `"Hex: FF"` |
| `%o` | 八进制 | `format("Oct: %o", 8)` → `"Oct: 10"` |
| `%b` | 二进制 | `format("Bin: %b", 10)` → `"Bin: 1010"` |
| `%t` | 布尔值 | `format("Flag: %t", true)` → `"Flag: true"` |
| `%%` | 百分号 | `format("100%%")` → `"100%"` |

**标志位**:

| 标志 | 含义 | 示例 |
|------|------|------|
| `-` | 左对齐（默认右对齐） | `format("%-10s", "hi")` → `"hi        "` |
| `0` | 零填充（数值类型） | `format("%05d", 42)` → `"00042"` |
| `+` | 显示正号 | `format("%+d", 42)` → `"+42"` |
| ` ` | 正数前加空格 | `format("% d", 42)` → `" 42"` |
| `#` | 替代形式（%#b 加 0b 前缀，%#o 加 0 前缀，%#x 加 0x 前缀） | `format("%#b", 10)` → `"0b1010"` |

**宽度和精度**:

| 语法 | 含义 | 示例 |
|------|------|------|
| `%Nd` | 最小宽度 N | `format("%5d", 42)` → `"   42"` |
| `%-Nd` | 左对齐，最小宽度 N | `format("%-5d", 42)` → `"42   "` |
| `%0Nd` | 零填充，最小宽度 N | `format("%05d", 42)` → `"00042"` |
| `%.Nf` | 小数点后 N 位 | `format("%.2f", 3.14159)` → `"3.14"` |
| `%N.Mf` | 最小宽度 N，小数点后 M 位 | `format("%8.2f", 3.14)` → `"    3.14"` |
| `%.Ns` | 截取前 N 个字符 | `format("%.3s", "Hello")` → `"Hel"` |

**使用示例**:

```leno
// 基础用法
format("Name: %s, Age: %d", "Leno", 5)     // "Name: Leno, Age: 5"
format("Pi = %f", 3.14159)                  // "Pi = 3.141590"
format("Hex: %x / %X", 255, 255)            // "Hex: ff / FF"

// 宽度与对齐
format("%10s", "hi")                         // "        hi"
format("%-10s", "hi")                        // "hi        "
format("%5d", 42)                            // "   42"
format("%-5d", 42)                           // "42   "
format("%05d", 42)                           // "00042"

// 精度控制
format("%.2f", 3.14159)                      // "3.14"
format("%.0f", 3.14159)                      // "3"
format("%8.2f", 3.14)                        // "    3.14"
format("%.3s", "Hello")                      // "Hel"

// 进制转换
format("Oct: %o", 8)                         // "Oct: 10"
format("Bin: %b", 10)                        // "Bin: 1010"
format("%#x", 255)                           // "0xff"
format("%#o", 8)                             // "010"
format("%#b", 10)                            // "0b1010"

// 科学计数法与自动格式
format("%e", 1234.5)                         // "1.234500e+03"
format("%E", 1234.5)                         // "1.234500E+03"
format("%g", 1234.5)                         // "1234.5"
format("%.2e", 1234.5)                       // "1.23e+03"

// 布尔值
format("Flag: %t", true)                     // "Flag: true"
format("Active: %t", false)                  // "Active: false"
format("%10t", true)                         // "      true"

// 无符号整数
format("%u", 42)                             // "42"

// 正号显示
format("%+d", 42)                            // "+42"
format("%+d", -42)                           // "-42"

// 多个参数
format("%s %s %s", "a", "b", "c")          // "a b c"
format("Num: %d, %d, %d", 1, 2, 3)         // "Num: 1, 2, 3"

// 特殊字符
format("100%% complete")                    // "100% complete"
format("Char: %c", 65)                      // "Char: A"
```

**注意**:
- `format` 既可以作为全局函数使用，也可以通过 `strings.format` 模块调用
- `%s` 对非字符串参数会尝试自动转换（int/float/bool），无法转换的显示 `<value>`
- 类型不匹配时（如 `%d` 传入字符串）显示 `<type_error>`
- 参数不足时显示 `<missing>`

---

### 字符编码

#### `byte(pos?)`

获取指定位置字符的 ASCII 码值。

**参数**:

- `pos` (int, 可选): 字符位置（0-based，默认为0）

**返回**: `int | null` - ASCII 码值，越界返回 null

**说明**: 使用 0-based 索引。支持负数索引，-1 表示最后一个字符。

```leno
"Hello".byte(0)     // 72  ('H')
"Hello".byte(1)     // 101 ('e')
"Hello".byte(4)     // 111 ('o')
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

### 字节 / 码点 / 忽略大小写（2026-10-02 新增）

这批 API 的来历是"**数出来的重复实现**"：`strings.char(...)` 全仓 **159 处 / 39 文件**
（crypto / base64 / PE 分析 / web_html 都在逐字节拼串，而循环里 `result += strings.char(b)`
是 **O(n²)**）、"字节串 ↔ hex" **22 处 / 12 文件**、`to_lower(a) == to_lower(b)` **8 处**、
`slice(i, i)` 逐字符扫描 **10 处**（闭区间取单字符）。基准见 `examples/性能测试/strings原生与手写对比.leno`。

#### `to_bytes()` / `strings.from_bytes(arr)`

字符串 ↔ UTF-8 字节数组（元素 0-255）。**逐字节拼串请走 `from_bytes`**（一趟分配）。

```leno
"Hi中文".to_bytes()              // [72, 105, 228, 184, 173, 230, 150, 135]
strings.from_bytes([72, 105])    // "Hi"
```

> ⚠ `from_bytes` 的元素**越界或非 int 会抛错**（不静默截断/回绕 —— 那是"静默错值"的经典来源）。

#### `to_hex(upper?)` / `from_hex()`

字节串 ↔ hex 文本（**默认小写**；`upper=true` 输出大写；`from_hex` 大小写都收）。

```leno
"Hi".to_hex()             // "4869"
"中".to_hex()             // "e4b8ad"   ← 默认小写
"中".to_hex(true)         // "E4B8AD"
strings.from_hex("4869")  // "Hi"
```

> **默认为什么是小写**：收编时数出来的 19 处手写实现（crypto 示例 9 份 `to_hex` +
> `sha*/md5/hmac/pbkdf2` 里的 `byte_to_hex`）**清一色小写**，SHA/MD5 摘要的通行写法也是小写
> ⇒ 只有默认小写才能"换上去以后输出一字不变" ✓（本 API 最早只输出大写，那样会让这 19 处全部改样 ✗）。

> ⚠ 别与 `hex(value, width)` 混：那个是**数字 → hex 文本**，这两个是**字节串 ↔ hex 文本**。
> ⚠ `from_hex` 对**奇数长度**或**非 hex 字符**抛错（并指出第几个字符），不静默跳过。

#### `to_base64(url_safe?)` / `from_base64()`

字节串 ↔ base64 文本（RFC 4648，2026-10-07 新增）。

```leno
"foo".to_base64()             // "Zm9v"
"f".to_base64()               // "Zg=="
strings.from_base64("Zm9v")   // "foo"
strings.from_base64("Zg")     // "f"    ← 缺 padding 也收
"Zm9v\n".from_base64()        // "foo"  ← 换行/空格忽略
```

> **为什么进核心标准库**：本仓原先有**四份**各自独立的实现（`LenoCrypto`、`examples/crypto`、
> `LenoWeb` 的 `web_ws`、以及应用里各一份）——而 `web_ws` 这种"做分帧"的模块不该为了编解码
> 去依赖一个**加密库**。现在 `web_ws` 已改成薄封装（它那 51 行私有实现删掉了 ✓）。

| 项 | 行为 |
| --- | --- |
| 编码 | 标准字母表 + `=` padding；`url_safe=true` ⇒ `+`/`/` 换成 `-`/`_`（JWT / `data:` URL 用得上）|
| 解码宽容度 | 两种字母表都收；**缺 padding** 收；**空白（换行 / 制表 / 空格）忽略** |
| 解码报错 | **非法字符抛错并指出是哪个字符、第几位**（`base64 解码失败：非法字符 '*'（第 5 个字符）`）；余 1 个字符、`=` 多于 2 个、`=` 之后仍有数据同样报错 |
| 静默失败 | **没有** —— 不给空串、也不给半截结果（那正是「青衣」事故的教训：静默比报错难查得多）|

#### `eq_ignore_case(other)`

忽略大小写的相等判断。语义 = `to_lower(a) == to_lower(b)`（同一套逐字节映射），**省两次整串分配**。

```leno
"Hello".eq_ignore_case("hELLO")   // true
"abc".eq_ignore_case("ab")        // false（长度不同直接 false，不越界读）
```

#### `codepoint_at(pos?)` / `to_codepoints()` / `from_codepoint(cp)`

第 `pos` 个**字符**的 Unicode 码点 / 一趟取出全部码点（`Array[int]`）/ **码点 → 单字符字符串**。

```leno
"a中🙂".codepoint_at(0)     // 97
"a中🙂".codepoint_at(1)     // 20013   ('中' = U+4E2D)
"a中🙂".codepoint_at(-1)    // 128578  ('🙂' = U+1F642，4 字节)
"a中🙂".codepoint_at(99)    // null（越界，与 byte 同口径）
"a中🙂".to_codepoints()     // [97, 20013, 128578]
strings.from_codepoint(20013)   // "中"  ← to_codepoints 的**逆操作**（不扫全串 ⇒ O(1)）
"x".from_codepoint(65)          // "A"   ← 实例式也可（接收者被忽略，仅为与 to_codepoints 配对）
```

**逐字符要"字符串"的场景（例如 `measureString(ch)` 逐字测宽）请配对使用**：
`Array[int] cps = s.to_codepoints()`（一趟 O(n)）+ 循环里 `strings.from_codepoint(cps[k])` ——
等价于 `s.slice(k, k)`（闭区间取单字符），但后者每轮都要**从头扫**到第 k 个字符（整段 O(n²)）且每字符造一个临时串 ✗

> **★ 整串逐字符处理请用 `to_codepoints()`，别循环 `codepoint_at(i)`**：
> 后者要从头走到第 i 个字符 ⇒ 循环是 **O(n²)**。基准实测（4000 字符 × 20 趟）：
> 手写 `slice(i,i)` **113,589µs** ｜ 循环 `codepoint_at` **56,307µs（2.0x）**
> ｜ 一趟 `to_codepoints` **863µs（131.6x）** ✓

> **`from_codepoint` 的非法值会抛错**（不静默错值）：负数、大于 `0x10FFFF`、
> 以及 UTF-16 代理区 `0xD800..0xDFFF` 都拒绝。
> ⚠ 一个已知边界：`to_codepoints` 对**非法 UTF-8 字节**（如 `0xFF`）是"原样给首字节值"，
> 而 `from_codepoint(0xFF)` 会按码点正确编成 `C3 BF` 两字节 ⇒ 这条配对**只对合法 UTF-8 严格互逆**
> （实测：合法语料逐字符 0 分歧；含坏字节的串会有差异）。SDL/界面文本恒为合法 UTF-8 ⇒ 不受影响 ✓

> 另一条实测（同一次基准）：逐字节拼串 手写 **700,846µs** vs `from_bytes` **84µs（8343x）**；
> hex 编码 手写 **55,300µs** vs `to_hex` **26µs（2127x）**；忽略大小写 **2.0x**。

---

## 索引说明

LenoC 字符串操作统一使用 **0-based Unicode 字符索引**，与 Python 3、Java 等现代语言保持一致。

### Unicode 字符索引

字符串索引按 **Unicode 字符** 计数，而非 UTF-8 字节：

```leno
var s = "你好cheng"
print(s[0])          // "你" - 第0个字符
print(s[1])          // "好" - 第1个字符
print(s[2])          // "c"  - 第2个字符
print(s.len())       // 6    - 6个字符
print(s.byte_len())  // 9    - 6字节(中文) + 5字节(ASCII) = 9字节
```

### 0-based 索引

以下方法使用 0-based 索引：

- `find()` - 返回的位置和 start 参数都是 0-based 字符索引
- `slice()` - start 和 end 参数为字符索引（**闭区间**：含 end；负终点返回空串）
- `sub_str()` - start 参数为字符索引
- `byte()` - pos 参数为字节偏移
- `byte_slice()` - start 和 end 参数为字节偏移
- `byte_find()` - 返回的字节偏移和 start 参数都是 0-based 字节偏移

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
| 子串提取 | `s.slice(s, e)` | `s:sub(i, j)` | 索引基准不同（0-based vs 1-based）；**末位都是闭区间（含 j / e）** |
| 查找 | `s.find(p, i)` | `s:find(p, i)` | 相似 |
| 二进制查找 | `s.byte_find(p, s)` | 需自定义 | LenoC 特有 |
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
    if (pos != -1) {
        print("找到位置: " + pos)
        var extracted = text.slice(pos, pos + 3)   // "Leno"（闭区间：含 pos+3）
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
    for 0:text.len() - 1 to i {
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

*文档版本: 1.6*  
*最后更新: 2026-08-31*
