# LenoC Files 模块

本文档详细说明 `files` 模块提供的文件操作功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [文件打开模式](#文件打开模式)
- [模块方法（静态方法）](#模块方法静态方法)
- [文件对象方法](#文件对象方法)
- [读取操作](#读取操作)
- [写入操作](#写入操作)
- [文件定位](#文件定位)
- [文件属性](#文件属性)
- [快捷操作](#快捷操作)
- [示例代码](#示例代码)
- [注意事项](#注意事项)
- [最佳实践](#最佳实践)

---

## 概述

Files 模块是 Leno 语言中用于文件操作的核心模块，提供了完整的文件读写、定位、属性查询等功能。

### 特性

- **双模式操作**：模块快捷方法 + 文件对象精细控制
- **文本/二进制支持**：支持各种文件类型的读写
- **中文路径支持**：完全支持 Unicode 文件名
- **流式操作**：大文件支持分块读写
- **自动资源管理**：文件对象自动跟踪状态

---

## 使用方式

```leno
import files

// 方式1：快捷操作（推荐简单场景）
var content = files.read("data.txt")
files.write("output.txt", "Hello World")

// 方式2：文件对象（推荐复杂操作）
var f = files.open("data.txt", "r")
var line = f.readline()
f.close()
```

---

## 文件打开模式

| 模式 | 说明 | 文件存在 | 文件不存在 |
|------|------|---------|-----------|
| `"r"` | 只读（文本） | 从头读取 | 报错 |
| `"w"` | 只写（文本） | 清空内容 | 创建新文件 |
| `"a"` | 追加（文本） | 追加到末尾 | 创建新文件 |
| `"r+"` | 读写（文本） | 从头读写 | 报错 |
| `"w+"` | 读写（文本） | 清空内容 | 创建新文件 |
| `"a+"` | 读写追加（文本） | 追加到末尾 | 创建新文件 |
| `"rb"` | 只读（二进制） | 从头读取 | 报错 |
| `"wb"` | 只写（二进制） | 清空内容 | 创建新文件 |
| `"ab"` | 追加（二进制） | 追加到末尾 | 创建新文件 |

### 模式选择建议

```leno
// 读取配置文件
var f = files.open("config.ini", "r")

// 写入日志（追加模式）
var log = files.open("app.log", "a")

// 创建新文件或覆盖
files.open("output.txt", "w")

// 读写已有文件
files.open("data.bin", "r+")
```

---

## 模块方法（静态方法）

### `open(path, mode)`

打开文件并返回文件对象。

**参数**:
- `path` (string): 文件路径
- `mode` (string): 打开模式（见上文）

**返回**: `File` - 文件对象

**错误**: 文件不存在或权限不足时抛出错误

```leno
import files

// 打开文件读取
var f = files.open("data.txt", "r")
var content = f.read()
f.close()

// 打开文件写入
var f2 = files.open("output.txt", "w")
f2.write("Hello")
f2.close()
```

---

### `exists(path)`

检查文件是否存在。

**参数**:
- `path` (string): 文件路径

**返回**: `bool` - 是否存在

```leno
import files

if files.exists("data.txt") {
    print("文件存在")
} else {
    print("文件不存在")
}
```

---

### `delete(path)`

删除文件。

**参数**:
- `path` (string): 文件路径

**返回**: `bool` - 是否成功删除

```leno
import files

var success = files.delete("old_file.txt")
if success {
    print("删除成功")
} else {
    print("删除失败")
}
```

---

### `read(path)`

快捷读取文件全部内容。

**参数**:
- `path` (string): 文件路径

**返回**: `string` - 文件内容

```leno
import files

// 读取整个文件
var content = files.read("document.txt")
print(content)

// 处理行
var lines = content.split("\n")
for lines to line {
    print("行: " + line)
}
```

---

### `write(path, content)`

快捷写入内容到文件（覆盖模式）。

**参数**:
- `path` (string): 文件路径
- `content` (string): 要写入的内容

**返回**: `null`

```leno
import files

// 写入字符串
files.write("output.txt", "Hello World")

// 写入变量
var data = "Name: Alice\nAge: 25"
files.write("user.txt", data)

// 写入多行（使用 join）
var lines = ["Line 1", "Line 2", "Line 3"]
files.write("lines.txt", lines.join("\n"))
```

---

## 文件对象方法

### `read()` / `read(n)`

读取文件内容。

**参数**:
- `n` (int, 可选): 读取字节数，不传则读取全部

**返回**: `string` - 读取的内容

```leno
import files

var f = files.open("data.txt", "r")

// 读取全部
var all = f.read()
print("全部内容: " + all)

// 读取前100字节
f.seek(0, "set")  // 回到开头
var part = f.read(100)
print("前100字节: " + part)

f.close()
```

---

### `readline()`

读取一行（不包含换行符）。

**参数**: 无  
**返回**: `string` - 一行内容，EOF 返回空字符串

```leno
import files

var f = files.open("data.txt", "r")

// 逐行读取
var line_num = 1
while true {
    var line = f.readline()
    if line == "" and f.eof() {
        break
    }
    print(line_num + ": " + line)
    line_num = line_num + 1
}

f.close()
```

---

### `readlines()`

读取所有行，返回数组。

**参数**: 无  
**返回**: `Array` - 每行作为一个元素的数组

```leno
import files

var f = files.open("data.txt", "r")
var lines = f.readlines()
f.close()

print("总行数: " + lines.len())
for lines to line {
    print("- " + line)
}
```

---

### `write(string)`

写入字符串到文件。

**参数**:
- `string` (string): 要写入的内容

**返回**: `int` - 写入的字节数

```leno
import files

var f = files.open("output.txt", "w")

// 写入字符串
var written = f.write("Hello World")
print("写入了 " + written + " 字节")

// 写入变量
var name = "Alice"
f.write("Name: " + name)

f.close()
```

---

### `writeln(string)`

写入字符串并添加换行符。

**参数**:
- `string` (string): 要写入的内容

**返回**: `null`

```leno
import files

var f = files.open("log.txt", "w")

// 写入多行
f.writeln("2024-01-15 10:30:00 - 程序启动")
f.writeln("2024-01-15 10:30:01 - 加载配置")
f.writeln("2024-01-15 10:30:02 - 初始化完成")

f.close()
```

---

### `seek(pos, whence)`

移动文件指针位置。

**参数**:
- `pos` (int): 偏移量
- `whence` (string, 可选): 起始位置
  - `"set"` - 从文件开头（默认）
  - `"cur"` - 从当前位置
  - `"end"` - 从文件末尾

**返回**: `int` - 操作结果（0 表示成功）

```leno
import files

var f = files.open("data.bin", "r+")

// 移动到第100字节
f.seek(100, "set")

// 从当前位置向前移动50字节
f.seek(50, "cur")

// 移动到文件末尾前10字节
f.seek(-10, "end")

f.close()
```

---

### `tell()`

获取当前文件指针位置。

**参数**: 无  
**返回**: `int` - 当前位置（字节偏移）

```leno
import files

var f = files.open("data.txt", "r")

// 读取一些内容
var data = f.read(100)

// 获取当前位置
var pos = f.tell()
print("当前位置: " + pos + " 字节")

f.close()
```

---

### `len()`

获取文件大小。

**参数**: 无  
**返回**: `int` - 文件大小（字节）

```leno
import files

var f = files.open("data.bin", "r")
var size = f.len()
print("文件大小: " + size + " 字节")
f.close()
```

---

### `eof()`

检查是否到达文件末尾。

**参数**: 无  
**返回**: `bool` - 是否 EOF

```leno
import files

var f = files.open("data.txt", "r")

while not f.eof() {
    var line = f.readline()
    print(line)
}

f.close()
```

---

### `close()`

关闭文件。

**参数**: 无  
**返回**: `null`

```leno
import files

var f = files.open("data.txt", "r")
// ... 操作文件
f.close()

// 重复关闭是安全的
f.close()  // 不会报错
```

---

## 读取操作

### 读取整个文件

```leno
import files

// 快捷方式
var content = files.read("document.txt")

// 文件对象方式
var f = files.open("document.txt", "r")
var content = f.read()
f.close()
```

### 逐行读取

```leno
import files

// 方式1：readlines
var f = files.open("data.txt", "r")
var lines = f.readlines()
f.close()

for lines to line {
    print(line)
}

// 方式2：readline 循环
var f = files.open("data.txt", "r")
while true {
    var line = f.readline()
    if line == "" and f.eof() {
        break
    }
    process_line(line)
}
f.close()
```

### 分块读取（大文件）

```leno
import files

var f = files.open("large_file.bin", "rb")
var chunk_size = 4096
var total_read = 0

while not f.eof() {
    var chunk = f.read(chunk_size)
    total_read = total_read + chunk.len()
    process_chunk(chunk)
}

print("总共读取: " + total_read + " 字节")
f.close()
```

---

## 写入操作

### 覆盖写入

```leno
import files

// 快捷方式
files.write("output.txt", "新内容")

// 文件对象方式
var f = files.open("output.txt", "w")
f.write("新内容")
f.close()
```

### 追加写入

```leno
import files

// 追加单行
var log = files.open("app.log", "a")
log.writeln("[" + get_time() + "] 用户登录")
log.close()

// 追加多行
var f = files.open("data.txt", "a")
f.write("\n新行1\n新行2")
f.close()
```

### 写入格式化数据

```leno
import files

var f = files.open("report.txt", "w")

// 写入表头
f.writeln("========== 报告 ==========")
f.writeln("")

// 写入数据
var items = [
    {"name": "产品A", "price": 100, "qty": 5},
    {"name": "产品B", "price": 200, "qty": 3}
]

for items to item {
    var line = item["name"] + " | " + item["price"] + "元 | x" + item["qty"]
    f.writeln(line)
}

// 写入总计
f.writeln("")
f.writeln("=========================")

f.close()
```

---

## 文件定位

### 随机访问

```leno
import files

var f = files.open("data.bin", "r+")

// 读取头部信息
var header = f.read(64)

// 跳转到数据区域
f.seek(1024, "set")
var data = f.read(512)

// 跳转到文件末尾追加
f.seek(0, "end")
f.write("新数据")

f.close()
```

### 修改文件特定位置

```leno
import files

var f = files.open("records.bin", "r+")

// 计算第10条记录的位置
var record_size = 128
var record_num = 10
f.seek(record_size * record_num, "set")

// 修改记录
f.write("更新后的记录数据")

f.close()
```

---

## 文件属性

### 获取文件大小

```leno
import files

// 方式1：打开后获取
var f = files.open("data.bin", "r")
var size = f.len()
f.close()

// 方式2：读取后获取内容长度
var content = files.read("data.txt")
var size = content.len()
```

### 检查文件状态

```leno
import files

var f = files.open("data.txt", "r")

// 检查是否到末尾
if f.eof() {
    print("文件已读完")
}

// 获取当前位置
var pos = f.tell()
print("当前位置: " + pos)

f.close()
```

---

## 快捷操作

### 复制文件

```leno
import files

func copy_file(src, dst) {
    var content = files.read(src)
    files.write(dst, content)
}

copy_file("source.txt", "backup.txt")
```

### 大文件复制（分块）

```leno
import files

func copy_large_file(src, dst, chunk_size) {
    var src_f = files.open(src, "rb")
    var dst_f = files.open(dst, "wb")
    
    var total = 0
    while not src_f.eof() {
        var chunk = src_f.read(chunk_size)
        dst_f.write(chunk)
        total = total + chunk.len()
    }
    
    src_f.close()
    dst_f.close()
    
    return total
}

var copied = copy_large_file("large.bin", "large_copy.bin", 8192)
print("复制了 " + copied + " 字节")
```

---

## 示例代码

### 示例1：配置文件读写

```leno
import files
import dicts
import jsons

func load_config(path) {
    if not files.exists(path) {
        return {}
    }
    var content = files.read(path)
    return jsons.parse(content)
}

func save_config(path, config) {
    var json = jsons.stringify(config, true)  // 格式化输出
    files.write(path, json)
}

main() {
    var config_path = "app.config"
    
    // 加载配置
    var config = load_config(config_path)
    
    // 使用配置
    var host = config.get("host", "localhost")
    var port = config.get("port", 8080)
    print("服务器: " + host + ":" + port)
    
    // 修改配置
    config["port"] = 3000
    config["debug"] = true
    
    // 保存配置
    save_config(config_path, config)
    print("配置已保存")
}
```

### 示例2：日志记录器

```leno
import files
import times

var Logger = {
    "file": null,
    "level": "INFO"
}

func logger_init(path) {
    Logger.file = files.open(path, "a")
}

func logger_log(level, message) {
    if Logger.file == null {
        print("日志未初始化")
        return
    }
    
    var time_str = times.now().format("yyyy-MM-dd HH:mm:ss")
    var log_line = "[" + time_str + "] [" + level + "] " + message
    Logger.file.writeln(log_line)
}

func logger_info(message) {
    logger_log("INFO", message)
}

func logger_error(message) {
    logger_log("ERROR", message)
}

func logger_close() {
    if Logger.file != null {
        Logger.file.close()
        Logger.file = null
    }
}

main() {
    logger_init("app.log")
    
    logger_info("程序启动")
    logger_info("加载配置完成")
    logger_error("连接数据库失败")
    
    logger_close()
}
```

### 示例3：CSV 文件处理

```leno
import files
import arrays

func read_csv(path) {
    var f = files.open(path, "r")
    var lines = f.readlines()
    f.close()
    
    if lines.len() == 0 {
        return []
    }
    
    // 解析表头
    var headers = lines[0].split(",")
    
    // 解析数据行
    var records = []
    for 1 : lines.len() - 1 to i {
        var values = lines[i].split(",")
        var record = {}
        for 0 : headers.len() - 1 to j {
            record[headers[j]] = values[j]
        }
        records.add(record)
    }
    
    return records
}

func write_csv(path, headers, records) {
    var f = files.open(path, "w")
    
    // 写入表头
    f.writeln(headers.join(","))
    
    // 写入数据
    for records to record {
        var values = []
        for headers to h {
            values.add(record[h])
        }
        f.writeln(values.join(","))
    }
    
    f.close()
}

main() {
    // 读取 CSV
    var data = read_csv("data.csv")
    for data to row {
        print(row["name"] + ": " + row["age"])
    }
    
    // 写入 CSV
    var headers = ["name", "age", "city"]
    var records = [
        {"name": "Alice", "age": "25", "city": "Beijing"},
        {"name": "Bob", "age": "30", "city": "Shanghai"}
    ]
    write_csv("output.csv", headers, records)
}
```

### 示例4：二进制文件处理

```leno
import files

// 写入二进制数据
func write_binary_data(path) {
    var f = files.open(path, "wb")
    
    // 写入魔数
    f.write("BIN\x00")
    
    // 写入版本
    f.write("\x01\x00")  // 版本 1.0
    
    // 写入数据长度（4字节大端）
    var data_len = 256
    f.write(char((data_len >> 24) & 0xFF))
    f.write(char((data_len >> 16) & 0xFF))
    f.write(char((data_len >> 8) & 0xFF))
    f.write(char(data_len & 0xFF))
    
    // 写入数据
    for 0 : data_len - 1 to i {
        f.write(char(i & 0xFF))
    }
    
    f.close()
}

// 读取二进制数据
func read_binary_data(path) {
    var f = files.open(path, "rb")
    
    // 读取魔数
    var magic = f.read(4)
    if magic != "BIN\x00" {
        print("无效的格式")
        f.close()
        return null
    }
    
    // 读取版本
    var version = f.read(2)
    print("版本: " + version[0] + "." + version[1])
    
    // 读取数据长度
    var len_bytes = f.read(4)
    var data_len = (len_bytes[0] << 24) | (len_bytes[1] << 16) | 
                   (len_bytes[2] << 8) | len_bytes[3]
    print("数据长度: " + data_len)
    
    // 读取数据
    var data = f.read(data_len)
    
    f.close()
    return data
}

main() {
    write_binary_data("data.bin")
    var data = read_binary_data("data.bin")
    print("读取了 " + data.len() + " 字节")
}
```

### 示例5：文件搜索和替换

```leno
import files

func find_and_replace(path, old_str, new_str) {
    // 读取内容
    var content = files.read(path)
    
    // 替换
    var new_content = content.replace(old_str, new_str)
    
    // 写回
    files.write(path, new_content)
    
    return new_content != content  // 是否发生了替换
}

func find_in_file(path, search_str) {
    var f = files.open(path, "r")
    var line_num = 1
    var matches = []
    
    while true {
        var line = f.readline()
        if line == "" and f.eof() {
            break
        }
        
        if line.contains(search_str) {
            matches.add({"line": line_num, "content": line})
        }
        
        line_num = line_num + 1
    }
    
    f.close()
    return matches
}

main() {
    // 查找
    var results = find_in_file("source.txt", "TODO")
    for results to r {
        print("行 " + r["line"] + ": " + r["content"])
    }
    
    // 替换
    var changed = find_and_replace("config.txt", "localhost", "127.0.0.1")
    if changed {
        print("替换成功")
    }
}
```

---

## 注意事项

### 1. 文件编码

- 默认使用系统编码（Windows 通常是 GBK/UTF-8，Linux/macOS 通常是 UTF-8）
- 中文内容建议使用 UTF-8 编码保存

```leno
// 确保正确处理中文
files.write("chinese.txt", "中文内容测试")
var content = files.read("chinese.txt")
print(content)  // 中文内容测试
```

### 2. 路径分隔符

- Windows 支持 `/` 和 `\`
- 建议使用 `/` 或 `dirs.join()` 确保跨平台兼容

```leno
import dirs

// 推荐：使用 dirs.join
var path = dirs.join("folder", "subfolder", "file.txt")

// 也可以直接使用 /
var path2 = "folder/subfolder/file.txt"
```

### 3. 文件关闭

- 文件使用完毕后应调用 `close()`
- 重复关闭是安全的，不会报错
- 程序退出时会自动关闭未关闭的文件

```leno
var f = files.open("data.txt", "r")
// ... 使用文件
f.close()
f.close()  // 安全，不会报错
```

### 4. 大文件处理

- 大文件建议使用分块读取，避免内存不足
- 使用 `"rb"` / `"wb"` 模式进行二进制操作

```leno
// 大文件复制（分块）
var f1 = files.open("large.bin", "rb")
var f2 = files.open("large_copy.bin", "wb")

while not f1.eof() {
    var chunk = f1.read(8192)  // 8KB 分块
    f2.write(chunk)
}

f1.close()
f2.close()
```

### 5. 错误处理

- 文件不存在时 `open()` 会抛出错误
- 使用 `exists()` 预先检查或使用 try-catch

```leno
// 方式1：预先检查
if files.exists("data.txt") {
    var content = files.read("data.txt")
} else {
    print("文件不存在")
}

// 方式2：try-catch（如果语言支持）
try {
    var f = files.open("data.txt", "r")
} catch e {
    print("打开失败: " + e)
}
```

---

## 最佳实践

1. **使用快捷方法处理简单场景**
   ```leno
   // 简单读写用快捷方法
   var content = files.read("data.txt")
   files.write("output.txt", content)
   ```

2. **使用文件对象处理复杂操作**
   ```leno
   // 需要精细控制时用文件对象
   var f = files.open("data.txt", "r")
   while not f.eof() {
       process_line(f.readline())
   }
   f.close()
   ```

3. **及时关闭文件**
   ```leno
   // 使用完立即关闭
   var f = files.open("data.txt", "r")
   var data = f.read()
   f.close()  // 不要遗忘
   ```

4. **处理大文件使用分块**
   ```leno
   var f = files.open("large.bin", "rb")
   while not f.eof() {
       var chunk = f.read(4096)
       process(chunk)
   }
   f.close()
   ```

5. **使用 with 语句（如果语言支持）**
   ```leno
   // 自动关闭
   with files.open("data.txt", "r") to f {
       var content = f.read()
   }  // 自动关闭
   ```

6. **检查操作结果**
   ```leno
   var success = files.delete("old.txt")
   if not success {
       print("删除失败，可能文件不存在或无权限")
   }
   ```

---

*文档版本: 1.0*  
*最后更新: 2026-05-17*
