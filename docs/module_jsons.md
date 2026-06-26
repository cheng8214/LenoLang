# LenoC JSON 模块 (jsons)

本文档详细说明 `jsons` 模块提供的 JSON 解析和序列化功能。

## 目录

- [使用方式](#使用方式)
- [JSON 解码](#json-解码)
- [JSON 编码](#json-编码)
- [文件操作](#文件操作)
- [数据类型映射](#数据类型映射)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 使用方式

```leno
import jsons
import io

main() {
    // 解析 JSON 字符串
    var obj = jsons.decode('{"name": "Leno", "version": 1.0}')
    io.print(obj.name)  // "Leno"
    
    // 编码为 JSON
    var json_str = jsons.encode(obj)
    io.print(json_str)  // {"name":"Leno","version":1}
}
```

---

## JSON 解码

### `decode(json_string)`

将 JSON 字符串解析为 Leno 对象。

**参数**:
- `json_string` (string): 要解析的 JSON 字符串

**返回**: 
- 解析成功: 对应的 Leno 值（字典、数组、字符串、数字、布尔值或 null）
- 解析失败: `null`

```leno
// 解析对象
var obj = jsons.decode('{"name": "张三", "age": 25}')
io.print(obj.name)   // "张三"
io.print(obj.age)    // 25

// 解析数组
var arr = jsons.decode('[1, 2, 3, "hello"]')
io.print(arr[0])     // 1
io.print(arr[3])     // "hello"

// 解析嵌套结构
var nested = jsons.decode('{"user": {"id": 1, "name": "李四"}, "tags": ["a", "b"]}')
io.print(nested.user.name)   // "李四"
io.print(nested.tags[0])     // "a"

// 解析基本类型
var str = jsons.decode('"hello world"')   // "hello world"
var num = jsons.decode('42')               // 42
var flt = jsons.decode('3.14')             // 3.14
var bool = jsons.decode('true')            // true
var nil = jsons.decode('null')             // null
```

---

## JSON 编码

### `encode(value)`

将 Leno 值编码为紧凑格式的 JSON 字符串。

**参数**:
- `value`: 要编码的值（字典、数组、字符串、数字、布尔值或 null）

**返回**: `string` - JSON 字符串

```leno
// 编码对象
var obj = {name: "Leno", version: 1.0}
var json = jsons.encode(obj)
// json = "{\"name\":\"Leno\",\"version\":1}"

// 编码数组
var arr = [1, 2, 3, "hello"]
var json = jsons.encode(arr)
// json = "[1,2,3,\"hello\"]"

// 编码嵌套结构
var nested = {
    user: {id: 1, name: "李四"},
    tags: ["a", "b"]
}
var json = jsons.encode(nested)
// json = "{\"user\":{\"id\":1,\"name\":\"李四\"},\"tags\":[\"a\",\"b\"]}"
```

---

### `encode_pretty(value)`

将 Leno 值编码为格式化（美化）的 JSON 字符串。

**参数**:
- `value`: 要编码的值

**返回**: `string` - 格式化后的 JSON 字符串

```leno
var obj = {name: "Leno", version: 1.0, tags: ["a", "b"]}
var pretty = jsons.encode_pretty(obj)
io.print(pretty)
```

输出：
```json
{
  "name": "Leno",
  "version": 1,
  "tags": [
    "a",
    "b"
  ]
}
```

---

## 文件操作

### `read_file(path)`

从文件读取 JSON 内容并解析。

**参数**:
- `path` (string): JSON 文件路径

**返回**: 
- 成功: 解析后的 Leno 值
- 失败: `null`（文件不存在或解析错误）

```leno
var data = jsons.read_file("config.json")
if data != null {
    io.print("服务器: " + data.host)
    io.print("端口: " + data.port)
} else {
    io.print("读取配置文件失败")
}
```

---

### `write_file(path, value)`

将 Leno 值编码为 JSON 并写入文件（美化格式）。

**参数**:
- `path` (string): 目标文件路径
- `value`: 要写入的值

**返回**: `bool` - 是否写入成功

```leno
var config = {
    host: "localhost",
    port: 8080,
    debug: true
}

if jsons.write_file("config.json", config) {
    io.print("配置已保存")
} else {
    io.print("保存失败")
}
```

---

## 数据类型映射

### JSON → Leno

| JSON 类型 | Leno 类型 | 示例 |
|-----------|-----------|------|
| object | `dict` | `{"a": 1}` → 字典 |
| array | `array` | `[1, 2, 3]` → 数组 |
| string | `string` | `"hello"` → 字符串 |
| number (整数) | `int` | `42` → 整数 |
| number (小数) | `float` | `3.14` → 浮点数 |
| boolean | `bool` | `true` → 布尔值 |
| null | `null` | `null` → null |

### Leno → JSON

| Leno 类型 | JSON 类型 | 说明 |
|-----------|-----------|------|
| `dict` | object | 字典键必须是字符串 |
| `array` | array | 数组元素可以是任意类型 |
| `string` | string | 自动处理转义字符 |
| `int` | number | 整数 |
| `float` | number | 浮点数 |
| `bool` | boolean | `true` / `false` |
| `null` | null | `null` |

---

## 示例代码

### 1. 配置文件读写

```leno
import jsons
import io

main() {
    // 读取配置
    var config = jsons.read_file("app.json")
    
    if config == null {
        // 使用默认配置
        config = {
            app_name: "MyApp",
            version: "1.0.0",
            debug: false,
            database: {
                host: "localhost",
                port: 3306,
                name: "mydb"
            }
        }
        
        // 保存默认配置
        jsons.write_file("app.json", config)
        io.print("已创建默认配置文件")
    }
    
    // 使用配置
    io.print("应用: " + config.app_name)
    io.print("数据库: " + config.database.host + ":" + config.database.port)
}
```

---

### 2. 处理 JSON API 响应

```leno
import jsons
import io

// 模拟 API 响应处理
func parse_user_list(var json_text) {
    var data = jsons.decode(json_text)
    
    if data == null {
        io.print("解析失败")
        return []
    }
    
    var users = []
    
    for data.users to user {
        users.add({
            id: user.id,
            name: user.name,
            email: user.email
        })
    }
    
    return users
}

main() {
    var api_response = '{
        "total": 2,
        "users": [
            {"id": 1, "name": "张三", "email": "zhangsan@example.com"},
            {"id": 2, "name": "李四", "email": "lisi@example.com"}
        ]
    }'
    
    var users = parse_user_list(api_response)
    
    io.print("共 " + users.len() + " 个用户:")
    for users to u {
        io.print("  - " + u.name + " (" + u.email + ")")
    }
}
```

---

### 3. 数据导出工具

```leno
import jsons
import io

// 导出用户数据为 JSON
func export_users(var users, var filename) {
    var data = {
        export_time: "2026-01-01",
        total: users.len(),
        users: users
    }
    
    if jsons.write_file(filename, data) {
        io.print("导出成功: " + filename)
        return true
    } else {
        io.print("导出失败")
        return false
    }
}

main() {
    var users = [
        {id: 1, name: "张三", age: 25},
        {id: 2, name: "李四", age: 30},
        {id: 3, name: "王五", age: 28}
    ]
    
    export_users(users, "users_export.json")
}
```

---

### 4. JSON 数据转换

```leno
import jsons
import io

// 将 CSV 风格数据转换为 JSON
func csv_to_json(var headers, var rows) {
    var result = []
    
    for rows to row {
        var obj = {}
        for 0:(headers.len() - 1) to i {
            obj[headers[i]] = row[i]
        }
        result.add(obj)
    }
    
    return result
}

main() {
    var headers = ["name", "age", "city"]
    var rows = [
        ["张三", "25", "北京"],
        ["李四", "30", "上海"],
        ["王五", "28", "广州"]
    ]
    
    var json_data = csv_to_json(headers, rows)
    var json_text = jsons.encode_pretty(json_data)
    
    io.print(json_text)
}
```

输出：
```json
[
  {
    "name": "张三",
    "age": "25",
    "city": "北京"
  },
  {
    "name": "李四",
    "age": "30",
    "city": "上海"
  },
  {
    "name": "王五",
    "age": "28",
    "city": "广州"
  }
]
```

---

### 5. 读取唐诗 JSON 文件

```leno
import jsons
import io

main() {
    var data = jsons.read_file("cs.json")
    
    if data == null {
        io.print("无法读取文件")
        return
    }
    
    // 显示诗集信息
    io.print("【" + data.collection.name + "】")
    io.print("描述: " + data.collection.description)
    io.print("共收录 " + data.collection.total + " 首")
    io.print("")
    
    // 遍历所有诗歌
    for data.poems to poem {
        io.print("═══════════════════════")
        io.print("【" + poem.title + "】")
        io.print("作者: " + poem.author.name + " (" + poem.author.dynasty + ")")
        
        // 显示诗句
        io.print("\n原文:")
        for poem.content to line {
            io.print("  " + line.text)
        }
        
        io.print("")
    }
}
```

---

### 6. 验证 JSON 格式

```leno
import jsons
import io

// 检查字符串是否是有效的 JSON
func is_valid_json(var text) -> bool {
    var result = jsons.decode(text)
    return result != null
}

main() {
    var tests = [
        '{"name": "test"}',
        '[1, 2, 3]',
        '"hello"',
        'invalid json',
        '{"unclosed": "string}',
        ''
    ]
    
    for tests to test {
        var valid = is_valid_json(test)
        io.print(test + " -> " + (valid ? "有效" : "无效"))
    }
}
```

---

## 注意事项

1. **编码问题**
   - JSON 文件应使用 UTF-8 编码
   - 支持中文、emoji 等 Unicode 字符

2. **数字精度**
   - 整数支持范围取决于平台（通常为 64 位）
   - 浮点数可能存在精度损失

3. **对象键名**
   - JSON 对象键必须是字符串
   - 使用点号或方括号访问: `obj.key` 或 `obj["key"]`

4. **数组越界**
   - 访问不存在的索引返回 `null`，不会报错
   - 建议先检查数组长度

5. **循环引用**
   - 当前版本不支持处理循环引用
   - 编码包含循环引用的对象可能导致无限递归

6. **文件权限**
   - `read_file` 需要读权限
   - `write_file` 需要写权限，会自动创建父目录

7. **错误处理**
   - 解析错误返回 `null`，不会抛出异常
   - 建议始终检查返回值

---

## 性能提示

| 操作 | 建议 |
|------|------|
| 大文件读取 | 使用 `read_file` 代替手动读取+解析 |
| 频繁编码 | 对静态数据缓存编码结果 |
| 配置存储 | 使用 `write_file` 美化格式便于调试 |
| 网络传输 | 使用 `encode` 紧凑格式减少数据量 |

---

## 完整 API 速查表

| 函数 | 参数 | 返回 | 说明 |
|------|------|------|------|
| `decode(str)` | JSON 字符串 | 解析后的值 | 解析 JSON |
| `encode(val)` | 任意值 | JSON 字符串 | 紧凑编码 |
| `encode_pretty(val)` | 任意值 | JSON 字符串 | 美化编码 |
| `read_file(path)` | 文件路径 | 解析后的值 | 读取并解析 |
| `write_file(path, val)` | 路径, 值 | bool | 写入文件 |

---

*文档版本: 1.0*  
*最后更新: 2026-05-07*
