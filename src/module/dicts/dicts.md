# LenoC Dicts 模块

本文档详细说明 `dicts` 模块提供的字典（哈希表）操作功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [字典创建](#字典创建)
- [核心方法](#核心方法)
- [遍历方法](#遍历方法)
- [实例方法 vs 模块方法](#实例方法-vs-模块方法)
- [示例代码](#示例代码)
- [性能说明](#性能说明)
- [注意事项](#注意事项)

---

## 概述

Dicts 模块是 Leno 语言中用于字典（键值对存储）操作的核心模块。字典使用**字符串作为键**，可以存储任意类型的值。

### 特性

- **字符串键**：键必须是字符串类型
- **任意值**：值可以是任意类型（数字、字符串、数组、字典等）
- **动态扩容**：自动处理容量增长
- **哈希实现**：基于哈希表，平均 O(1) 访问速度
- **双模式调用**：实例方法或模块方法

---

## 使用方式

```leno
// 方式1：实例方法（推荐）
var dict = {"name": "Alice", "age": 25}
print(dict.len())
dict.set("city", "Beijing")

// 方式2：模块方法
import dicts
print(dicts.len(dict))
dicts.set(dict, "city", "Beijing")
```

---

## 字典创建

### 字面量创建

```leno
// 空字典
var empty = {}

// 基础字典
var person = {
    "name": "Alice",
    "age": 25,
    "city": "Beijing"
}

// 嵌套字典
var company = {
    "name": "Tech Corp",
    "employees": [
        {"name": "Alice", "role": "Engineer"},
        {"name": "Bob", "role": "Designer"}
    ],
    "address": {
        "city": "Beijing",
        "zip": "100000"
    }
}

// 混合类型值
var mixed = {
    "number": 42,
    "string": "hello",
    "bool": true,
    "null": null,
    "array": [1, 2, 3],
    "dict": {"a": 1}
}
```

### 动态创建

```leno
var dict = {}
dict["key1"] = "value1"
dict["key2"] = 123
```

---

## 核心方法

### `len()`

返回字典中键值对的数量。

**参数**: 无  
**返回**: `int` - 键值对数量

```leno
var dict = {"a": 1, "b": 2, "c": 3}
print(dict.len())  // 3

// 空字典
var empty = {}
print(empty.len())  // 0
```

---

### `has(key)`

检查字典是否包含指定键。

**参数**:
- `key` (string): 要检查的键

**返回**: `bool` - 是否包含该键

```leno
var dict = {"name": "Alice", "age": 25}
print(dict.has("name"))   // true
print(dict.has("email"))  // false

// 嵌套检查
var nested = {"user": {"name": "Bob"}}
print(nested.has("user"))        // true
// 注意：has 不检查嵌套键
// print(nested.has("user.name")) // false
```

---

### `get(key)`

获取指定键的值。

**参数**:
- `key` (string): 要获取的键

**返回**: `any` - 键对应的值，键不存在返回 `null`

```leno
var dict = {"name": "Alice", "age": 25}
print(dict.get("name"))   // Alice
print(dict.get("age"))    // 25
print(dict.get("email"))  // null

// 使用 [] 语法等价
print(dict["name"])       // Alice
```

---

### `set(key, value)`

设置键值对。如果键已存在则更新值，不存在则添加。

**参数**:
- `key` (string): 键
- `value` (any): 值

**返回**: `null`

```leno
var dict = {}

// 添加新键值对
dict.set("name", "Alice")
dict.set("age", 25)
print(dict)  // {name: Alice, age: 25}

// 更新已有键
dict.set("age", 26)
print(dict)  // {name: Alice, age: 26}

// 使用 [] 语法等价
dict["city"] = "Beijing"
print(dict)  // {name: Alice, age: 26, city: Beijing}
```

---

### `remove(key)`

删除指定键值对。

**参数**:
- `key` (string): 要删除的键

**返回**: `null`

**注意**: 键不存在时静默处理，不报错

```leno
var dict = {"a": 1, "b": 2, "c": 3}
dict.remove("b")
print(dict)  // {a: 1, c: 3}

// 删除不存在的键（不报错）
dict.remove("z")
print(dict)  // {a: 1, c: 3}
```

---

## 遍历方法

### `keys()`

获取所有键的数组。

**参数**: 无  
**返回**: `Array` - 键的数组

```leno
var dict = {"name": "Alice", "age": 25, "city": "Beijing"}
var keys = dict.keys()
print(keys)  // [name, age, city]

// 遍历键
for keys to key {
    print("键: " + key)
}
```

---

### `values()`

获取所有值的数组。

**参数**: 无  
**返回**: `Array` - 值的数组

```leno
var dict = {"name": "Alice", "age": 25, "city": "Beijing"}
var values = dict.values()
print(values)  // [Alice, 25, Beijing]

// 遍历值
for values to value {
    print("值: " + value)
}
```

---

### 遍历键值对

使用 `keys()` 和 `get()` 配合遍历：

```leno
var dict = {"name": "Alice", "age": 25, "city": "Beijing"}

// 方式1：遍历键，再获取值
for dict.keys() to key {
    var value = dict.get(key)
    print(key + ": " + value)
}

// 方式2：直接遍历字典（如果语言支持）
// for dict to key, value { ... }
```

---

## 实例方法 vs 模块方法

所有字典方法都支持两种调用方式：

| 实例方法 | 模块方法 | 说明 |
|---------|---------|------|
| `dict.len()` | `dicts.len(dict)` | 获取键值对数量 |
| `dict.has(key)` | `dicts.has(dict, key)` | 检查键是否存在 |
| `dict.get(key)` | `dicts.get(dict, key)` | 获取值 |
| `dict.set(key, val)` | `dicts.set(dict, key, val)` | 设置键值对 |
| `dict.remove(key)` | `dicts.remove(dict, key)` | 删除键值对 |
| `dict.keys()` | `dicts.keys(dict)` | 获取所有键 |
| `dict.values()` | `dicts.values(dict)` | 获取所有值 |

### 选择建议

- **实例方法**：代码更简洁，推荐日常使用
- **模块方法**：需要动态方法名或函数式编程时使用

```leno
// 实例方法（推荐）
var dict = {"a": 1, "b": 2}
dict.set("c", 3)
print(dict.get("a"))

// 模块方法（特殊场景）
import dicts
var method = "get"
print(dicts[method](dict, "a"))  // 动态调用
```

---

## 示例代码

### 示例1：用户信息管理

```leno
main() {
    // 创建用户字典
    var user = {}
    
    // 添加信息
    user.set("name", "张三")
    user.set("age", 28)
    user.set("email", "zhangsan@example.com")
    
    print("用户信息:")
    for user.keys() to key {
        print("  " + key + ": " + user.get(key))
    }
    
    // 更新年龄
    user.set("age", 29)
    print("更新后年龄: " + user.get("age"))
    
    // 检查字段
    if user.has("phone") {
        print("电话: " + user.get("phone"))
    } else {
        print("电话未设置")
    }
}
```

### 示例2：计数器

```leno
main() {
    var words = ["apple", "banana", "apple", "cherry", "banana", "apple"]
    var counter = {}
    
    // 统计词频
    for words to word {
        if counter.has(word) {
            counter.set(word, counter.get(word) + 1)
        } else {
            counter.set(word, 1)
        }
    }
    
    print("词频统计:")
    for counter.keys() to word {
        print("  " + word + ": " + counter.get(word))
    }
}
```

### 示例3：配置管理

```leno
main() {
    // 默认配置
    var config = {
        "host": "localhost",
        "port": 8080,
        "debug": false,
        "database": {
            "type": "sqlite",
            "name": "app.db"
        }
    }
    
    // 读取配置
    print("服务器: " + config.get("host") + ":" + config.get("port"))
    
    // 修改配置
    config.set("port", 3000)
    config.set("debug", true)
    
    // 获取嵌套配置
    var db = config.get("database")
    print("数据库类型: " + db.get("type"))
    
    print("最终配置:")
    for config.keys() to key {
        var value = config.get(key)
        if value.type == "dict" {
            print("  " + key + ": {...}")
        } else {
            print("  " + key + ": " + value)
        }
    }
}
```

### 示例4：缓存实现

```leno
// 简单的内存缓存
var cache = {}
var cache_stats = {"hits": 0, "misses": 0}

func cache_get(key) {
    if cache.has(key) {
        cache_stats.set("hits", cache_stats.get("hits") + 1)
        return cache.get(key)
    }
    cache_stats.set("misses", cache_stats.get("misses") + 1)
    return null
}

func cache_set(key, value) {
    cache.set(key, value)
}

func cache_remove(key) {
    cache.remove(key)
}

func cache_clear() {
    for cache.keys() to key {
        cache.remove(key)
    }
}

main() {
    // 模拟缓存使用
    cache_set("user:1", {"name": "Alice", "age": 25})
    cache_set("user:2", {"name": "Bob", "age": 30})
    
    // 命中
    var user1 = cache_get("user:1")
    print("获取 user:1 = " + user1)
    
    // 命中
    var user1_again = cache_get("user:1")
    
    // 未命中
    var user3 = cache_get("user:3")
    print("获取 user:3 = " + user3)
    
    print("缓存统计:")
    print("  命中: " + cache_stats.get("hits"))
    print("  未命中: " + cache_stats.get("misses"))
    print("  缓存大小: " + cache.len())
}
```

### 示例5：与数组结合使用

```leno
main() {
    // 数组嵌套字典
    var users = [
        {"id": 1, "name": "Alice", "active": true},
        {"id": 2, "name": "Bob", "active": false},
        {"id": 3, "name": "Charlie", "active": true}
    ]
    
    // 过滤活跃用户
    var active_users = []
    for users to user {
        if user.get("active") {
            active_users.add(user)
        }
    }
    
    print("活跃用户:")
    for active_users to user {
        print("  - " + user.get("name"))
    }
    
    // 按 ID 索引
    var user_by_id = {}
    for users to user {
        user_by_id.set(user.get("id").to_str(), user)
    }
    
    print("通过 ID 查找:")
    print("  ID=2: " + user_by_id.get("2").get("name"))
}
```

### 示例6：JSON 数据处理

```leno
import jsons

main() {
    // JSON 字符串
    var json_str = '{"name": "Product", "price": 99.99, "tags": ["new", "hot"]}'
    
    // 解析为字典
    var data = jsons.parse(json_str)
    
    print("产品信息:")
    print("  名称: " + data.get("name"))
    print("  价格: " + data.get("price"))
    
    // 修改数据
    data.set("price", 79.99)
    data.set("stock", 100)
    
    // 转回 JSON
    var new_json = jsons.stringify(data)
    print("更新后 JSON: " + new_json)
}
```

---

## 性能说明

### 时间复杂度

| 方法 | 时间复杂度 | 说明 |
|-----|-----------|------|
| `len()` | O(1) | 直接读取计数器 |
| `has()` | O(1) 平均 | 哈希查找 |
| `get()` | O(1) 平均 | 哈希查找 |
| `set()` | O(1) 平均 | 哈希插入/更新 |
| `remove()` | O(1) 平均 | 哈希删除 |
| `keys()` | O(n) | 遍历所有槽位 |
| `values()` | O(n) | 遍历所有槽位 |

### 空间复杂度

| 操作 | 空间复杂度 | 说明 |
|-----|-----------|------|
| 存储 n 个键值对 | O(n) | 哈希表存储 |
| `keys()` | O(n) | 创建新数组 |
| `values()` | O(n) | 创建新数组 |

### 哈希冲突

- 使用**链地址法**或**开放寻址法**处理冲突
- 负载因子超过阈值时自动扩容
- 扩容时重新哈希所有元素

---

## 注意事项

### 1. 键类型限制

字典的**键必须是字符串**：

```leno
var dict = {}

// 正确
dict["key"] = "value"
dict.set("name", "Alice")

// 错误：键必须是字符串
// dict[123] = "value"  // 错误
// dict.set(123, "value")  // 错误

// 如果需要数字键，转换为字符串
dict["123"] = "value"
```

### 2. 键的唯一性

字典中键是唯一的，重复设置会覆盖：

```leno
var dict = {}
dict.set("key", "value1")
dict.set("key", "value2")
print(dict.get("key"))  // value2
```

### 3. 值类型无限制

值可以是任意类型：

```leno
var dict = {}
dict["number"] = 42
dict["string"] = "hello"
dict["bool"] = true
dict["null"] = null
dict["array"] = [1, 2, 3]
dict["dict"] = {"a": 1}
```

### 4. 遍历顺序

字典遍历顺序**不保证**与插入顺序一致：

```leno
var dict = {}
dict["a"] = 1
dict["b"] = 2
dict["c"] = 3

// 遍历顺序可能是 a,b,c 也可能是其他顺序
for dict.keys() to key {
    print(key)
}
```

### 5. 空值处理

- `get()` 不存在的键返回 `null`
- `remove()` 不存在的键静默处理
- `has()` 可以区分"不存在"和"值为 null"

```leno
var dict = {"a": null}
print(dict.has("a"))      // true（键存在，值为 null）
print(dict.get("a"))      // null
print(dict.has("b"))      // false（键不存在）
print(dict.get("b"))      // null（键不存在）
```

---

## 最佳实践

1. **使用 [] 语法访问**：代码更简洁
   ```leno
   // 推荐
   dict["key"] = value
   var val = dict["key"]
   
   // 不推荐（除非需要动态键名）
   dict.set("key", value)
   var val = dict.get("key")
   ```

2. **检查键存在性后再操作**：
   ```leno
   if dict.has("key") {
       var value = dict["key"]
       // 处理 value
   }
   ```

3. **使用 `keys()` 和 `values()` 遍历**：
   ```leno
   // 遍历键值对
   for dict.keys() to key {
       var value = dict[key]
       print(key + ": " + value)
   }
   ```

4. **嵌套字典安全访问**：
   ```leno
   // 检查每一层
   if dict.has("user") {
       var user = dict["user"]
       if user.has("name") {
           print(user["name"])
       }
   }
   ```

5. **避免在遍历时修改字典**：
   ```leno
   // 错误：遍历时修改可能导致问题
   for dict.keys() to key {
       dict.remove(key)  // 危险！
   }
   
   // 正确：先收集要删除的键
   var to_remove = []
   for dict.keys() to key {
       if should_remove(key) {
           to_remove.add(key)
       }
   }
   for to_remove to key {
       dict.remove(key)
   }
   ```

---

*文档版本: 1.0*  
*最后更新: 2026-05-17*
