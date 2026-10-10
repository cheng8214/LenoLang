# Leno JSON 模块 (jsons)

本文档详细说明 `jsons` 模块提供的 JSON 解析和序列化功能。

## 目录

- [使用方式](#使用方式)
- [JSON 解码](#json-解码)
- [取值助手（T21）](#取值助手t21)
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

> ⚠ **类型提示（2026-09-18 补，实测踩过）**：`decode` 的**静态返回类型是 `any`** ⇒ 直接写
> `obj.field` / `arr[0]` 会被类型检查拒绝（「不能在 any 类型上直接访问字段 / 进行索引访问」）。
>
> **最省事的写法（推荐）**：用 `as` —— **安全类型转换**：一步给出目标静态类型，
> **不匹配返回 `null`**（不抛错）；嵌套结构逐层 `as` 即可：
>
> ```leno
> var obj = jsons.decode(text) as Dict[string, any]
> var name = _str(obj["name"])                  // 元素本身仍是 any ⇒ 需 _str 或再 as
> var inner = obj["nested"] as Dict[string, any]  // 嵌套：逐层 as
> ```
>
> 也可以**收窄到带泛型参数的容器**（效果等同，写法稍长）：
>
> ```leno
> var obj = jsons.decode(text)
> if obj is Dict[string, any] {
>     var name = obj["name"]        // 取出来仍是 any ⇒ 嵌套结构要逐层收窄
> }
> ```
>
> 注意：**收窄到裸 `Dict` 是没有用的** —— 取出的元素仍是 `any`（见
> `docs/类型收窄与绑定语法改进.md`）。多层自由取值建议包一层小工具
> （如 `json_get` / `json_obj`，实物见 `leno_gui/应用/Trae签到/trae_core.leno`）。
> **2026-09-26 起：那层小工具已做进标准库** ⇒ 直接用下一节的
> `jsons.get_str/get_int/get_float/get_bool/get_obj/keys`（调用点零收窄 ✓）。

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

## 取值助手（T21）

> **为什么有这一节**：`decode` / `read_file` 的静态返回类型**只能是 `any`**（JSON 顶层可能是
> 对象/数组/标量，硬改成 `Dict` 是错语义）⇒ 每个消费点都要手写收窄
> （`if x is Dict => d and d.has(k)`），于是每个项目都自造一层 `json_get/json_obj/json_keys`
> （实物见 `leno_gui/应用/Trae签到/trae_core.leno`）。这组助手把那一层**做进标准库**：
> 收窄只发生在实现里，**调用点零样板** ✓（2026-09-26 新增；实现见 `src/module/jsons/jsons.c`）。

### `get_str(obj, key, default)` / `get_int` / `get_float` / `get_bool`

从（可能来自 `decode` 的）`any` 里按 key 取一个**标量**；取不到或转不过去 ⇒ `default`。

| 情况 | 结果 |
|------|------|
| `obj` 不是字典 / 没有该键 / 值是 `null` | `default` |
| 标量之间互转 | 字符串数字也能取成 `int`（**整串**都得是数字才认 ✓）；数字/布尔也能取成字符串 |
| 值是字典/数组（容器） | `default`（要容器本身请用 `get_obj` ✓） |
| 字符串转 bool | 只认 `"true"` / `"false"` / `"1"` / `"0"`，其它 ⇒ `default` |

**不抛异常**；`default` 本身也会走一遍转换（所以 `get_int(obj, "h", "21")` 也成立 ✓）。

```leno
var acc = jsons.decode(text)                 // any —— **不需要**先收窄 ✓
var token   = jsons.get_str(acc, "token", "")
var credits = jsons.get_int(acc, "credits", 0)
var ratio   = jsons.get_float(acc, "ratio", 0.0)
var ok      = jsons.get_bool(acc, "ok", false)
```

### `get_obj(obj, key)`

取**原始值**（`any`）继续往下钻；非字典/缺键 ⇒ `null`。

```leno
var user = jsons.get_obj(acc, "user")        // 嵌套对象（any）
var name = jsons.get_str(user, "name", "")   // 再取一层，仍是零收窄 ✓
```

### `keys(obj)`

字典的键列表（**插入序**）；非字符串键转成文本；非字典 ⇒ 空数组。

```leno
for jsons.keys(acc) to k {
    print(k + " = " + jsons.get_str(acc, k, ""))
}
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

### `write_text(path, text)`

**原样**写文本，**不做** JSON 编码 —— 想写"已经编码好的 JSON 文本"就用它 ✓。

**参数**:
- `path` (string): 目标文件路径
- `text` (string): 要原样写入的文本

**返回**: `bool` - 是否写入成功

> ⚠ `write_file(path, "{}")` 会把入参**再编码一次** ⇒ 落盘成带引号的 `"{}"`（读回来是
> `string` 而不是对象 ✗；2026-09-18 实测踩过）。「我已经有 JSON 文本了，只想落盘」
> 一律用 `write_text` ✓（2026-09-26 新增）。

```leno
var text = jsons.encode_pretty(data)   // 已经有文本
jsons.write_text("out.json", text)     // 原样落盘 ✓
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

8. **数组可以直接存、也可以直接收窄**（2026-10-08 实测更正 ✓）
   - 字面量字段里直接放数组：`jsons.write_file(p, {version: 1, arr: ["x","y"]})` ✓ 落盘就是 JSON 数组
   - `Dict[string, any] d = {}` 再 `d["arr"] = ["x","y"]` ✓ **内存与落盘都正常，不会被丢掉**
   - `jsons.decode(...)` 解出来的数组，`is Array[string]` **成立** ✓（库返回的数组如 `jsons.keys` 同样成立）
   - ⇒ 「几个字符串」这类配置项**直接存数组**即可 ✓，不必为了"绕开收窄"把结构拍扁成裸串 ✗
     （本仓例：音乐下载器 `dl_settings.sources` 直接存 `Array[string]` ✓）
   - ⚠ 仍然要在意的一条：`write_file` 会把参数**再编码一次** ⇒ 要写"已编码好的 JSON 文本"用
     `write_text` ✓（见上文 `write_text` ✓）
   - ⚠ 空数组字面量 `var a = []` 的元素类型是 `any` ⇒ 想当 `Array[string]` 用要**显式标注**
     `Array[string] a = []` ✓

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
| `write_file(path, val)` | 路径, 值 | bool | 写入文件（**会再编码一次**）|
| `write_text(path, text)` | 路径, 文本 | bool | **原样**写文本（不编码）|
| `get_str(obj, key, def)` | 任意, 键, 默认 | string | 取字符串（数字也能取成串）|
| `get_int(obj, key, def)` | 任意, 键, 默认 | int | 取整数（字符串数字也认）|
| `get_float(obj, key, def)` | 任意, 键, 默认 | float | 取浮点 |
| `get_bool(obj, key, def)` | 任意, 键, 默认 | bool | 取布尔（认 "1"/"0"/"true"/"false"）|
| `get_obj(obj, key)` | 任意, 键 | 任意 | 取原始值往下钻（缺 ⇒ null）|
| `keys(obj)` | 任意 | Array[string] | 字典键列表（非字典 ⇒ 空数组）|

---

*文档版本: 1.1*  
*最后更新: 2026-09-26（新增 T21 取值助手 + write_text）*
