# LenoC For 循环使用指南

## 概述

LenoC 的 `for` 循环支持多种遍历形式，包括数字范围遍历、数组遍历、字符串遍历和字典遍历。

**重要规则**：
- 范围语法中，`:` 表示**包含**结束值
- `for 10` → 0 到 9（不包含10）
- `for :10` → 0 到 10（包含10）

---

## 1. 基本数字循环

### 1.1 简单循环（指定次数）

```leno
// 循环 n 次，i 从 0 开始到 n-1
for 5 {
    print("cs")  // 输出 5 次 "cs"
}
```

### 1.2 带变量名的循环

```leno
// 循环 5 次，num 从 0 到 4
for 5 to num {
    print(num)  // 输出: 0 1 2 3 4
}
```

---

## 2. 范围循环（重要）

### 2.1 不包含结束值（默认）

```leno
// 从 0 到 4（不包含 5）
for 5 to num {
    print(num)  // 输出: 0 1 2 3 4
}
```

### 2.2 包含结束值（使用冒号）

**⚠️ 重要**：在 LenoC 中，`:` 表示范围包含结束值

```leno
// 包含语法：从 0 到 5（包含 5）
for :5 to num {
    print(num)  // 输出: 0 1 2 3 4 5
}

// 完整写法：从 0 到 5（包含 5）
for 0:5 to num {
    print(num)  // 输出: 0 1 2 3 4 5
}

// 从 2 到 5（包含 5）
for 2:5 to num {
    print(num)  // 输出: 2 3 4 5
}
```

### 2.3 使用变量作为范围

```leno
var a = 5
var b = 10

// 从 a 到 b（包含 b）
for a:b to i {
    print(i)  // 输出: 5 6 7 8 9 10
}
```

---

## 3. 带步进值的循环

### 3.1 正序步进

```leno
// 从 0 到 5，步进 2（包含 5）
for 0:5:2 to num {
    print(num)  // 输出: 0 2 4
}
```

### 3.2 小数步进

```leno
// 从 0 到 1，步进 0.1（包含 1）
for 0:1:0.1 to num {
    print(num)
    // 输出: 0.0 0.1 0.2 0.3 0.4 0.5 0.6 0.7 0.8 0.9 1.0
}
```

---

## 4. 倒序遍历

### 4.1 自动倒序（省略步进）

当起始值大于结束值时，自动倒序：

```leno
// 从 5 到 0（包含 0）
for 5:0 to num {
    print(num)  // 输出: 5 4 3 2 1 0
}
```

### 4.2 指定负步进

```leno
// 从 5 到 0，步进 -2（包含 0）
for 5:0:-2 to num {
    print(num)  // 输出: 5 3 1
}
```

---

## 5. 数组遍历

### 5.1 基本遍历（只获取元素）

```leno
var arr = [1, 2, 3, 4, 5, 6]

for arr to a {
    print(a)  // 输出: 1 2 3 4 5 6
}
```

### 5.2 带索引遍历（获取元素和索引）

```leno
var arr = [10, 20, 30]

for arr to item, index {
    print($"{index}: {item}")
    // 输出:
    // 0: 10
    // 1: 20
    // 2: 30
}
```

### 5.3 在排序算法中使用

```leno
func bubble_sort(var arr) {
    var n = arr.len()
    // 遍历 n-1 次
    for n - 1 to var i {
        // 每次比较的范围逐渐缩小
        for n - i - 1 to var j {
            if arr[j] > arr[j + 1] {
                var temp = arr[j]
                arr[j] = arr[j+1]
                arr[j+1] = temp
            }
        }
    }
    return arr
}
```

---

## 6. 字符串遍历

### 6.1 基本遍历（逐字符）

```leno
var str = "cheng8214"

for str to a {
    print(a)
    // 输出: c h e n g 8 2 1 4（每行一个字符）
}
```

### 6.2 带索引遍历

```leno
for "abc" to char, index {
    print($"{index}: {char}")
    // 输出:
    // 0: a
    // 1: b
    // 2: c
}
```

---

## 7. 字典遍历

### 7.1 基本遍历（只获取键）

```leno
var dict = {name: "cheng", age: 25}

for dict to key {
    print(key)
    // 输出: name age（保持插入顺序）
}
```

### 7.2 获取键和值（推荐）

```leno
var dict = {name: "cheng", age: 25}

for dict to key, value {
    print($"{key}: {value}")
    // 输出:
    // name: cheng
    // age: 25
}
```

### 7.3 带类型注解的字典遍历

```leno
Dict[string, string] d = {id: "11", name: "23"}

for d to key, value {
    // key 类型: string
    // value 类型: string
    string s = value  // 类型安全
    print($"{key}: {s}")
}
```

---

## 8. 快速参考表

| 语法 | 说明 | 包含 | 示例输出 |
|------|------|------|---------|
| `for 5` | 循环5次，i从0到4 | 不含 | 0 1 2 3 4 |
| `for 5 to i` | 循环5次，i从0到4 | 不含 | 0 1 2 3 4 |
| `for :5 to i` | i从0到5 | ✅ 包含 | 0 1 2 3 4 5 |
| `for 2:5 to i` | i从2到5 | ✅ 包含 | 2 3 4 5 |
| `for 0:5:2 to i` | i从0到5，步进2 | ✅ 包含 | 0 2 4 |
| `for 5:0 to i` | i从5到0（倒序） | ✅ 包含 | 5 4 3 2 1 0 |
| `for arr to item` | 遍历数组 | — | 数组元素 |
| `for arr to item, idx` | 遍历数组带索引 | — | 元素+索引 |
| `for str to char` | 遍历字符串 | — | 每个字符 |
| `for str to char, idx` | 遍历字符串带索引 | — | 字符+索引 |
| `for dict to key` | 遍历字典（键） | — | 每个键 |
| `for dict to key, value` | 遍历字典（键值对） | — | 键+值 |

---

## 9. 注意事项

### 9.1 范围语法的包含规则

```leno
// ❌ 不包含 5
for 5 to i      // i: 0,1,2,3,4

// ✅ 包含 5
for :5 to i     // i: 0,1,2,3,4,5
for 0:5 to i    // i: 0,1,2,3,4,5
```

### 9.2 字典遍历顺序

字典遍历保持**插入顺序**：

```leno
var dict = {name: "cheng", age: 25}
for dict to key, value {
    print($"{key}: {value}")
}
// 输出:
// name: cheng
// age: 25
```

### 9.3 遍历 any 类型（常见陷阱）

当遍历对象的类型在编译期未知（`any` 类型）时，`for X to y` 可能**静默不迭代**（不报错也不循环）。通常发生在：

- `dirs.walk()` 返回的元素（已修复，v1.x+ 正确标注为 Array）
- 函数返回未注解类型的数组
- 从模块导入的未类型化变量

```leno
// ❌ 可能静默失败：w[2] 是 any 类型（旧版 dirs.walk）
var walks = dirs.walk("path")
for walks to w {
    var files = w[2]
    for files to f { }  // 如果 files 是 any，不会迭代
}

// ✅ 已修复：dirs.walk 现在返回 Array[Array]
for walks to w {
    var files = w[2]
    for files to f { print(f) }  // 正常迭代
}

// ✅ 通用修复：用 _int() + 索引循环替代 for-each
int n = _int(files.len())
for 0 : n - 1 to i {
    var f = _str(files[i])
}
```

> **提示**：Leno v1.x+ 已修复 `dirs.walk` 等模块的类型标注，大多数场景不再需要手动处理。若仍遇到 `for any to x` 不迭代，用上述索引循环作为 workaround。

### 9.4 类型推断

```leno
// 数组遍历
for arr to item, index {
    // item 类型 = 数组元素类型
    // index 类型 = int
}

// 字典遍历
for dict to key, value {
    // key 类型 = string
    // value 类型 = 字典值类型
}
```

---

## 10. 最佳实践

1. **需要包含结束值时，记得使用冒号**
   ```leno
   // 正确的包含写法
   for 1:10 to i  // 1 到 10（包含）
   ```

2. **字典遍历时，建议使用键值对形式**
   ```leno
   for dict to key, value {
       // 同时获取键和值，更清晰
   }
   ```

3. **需要索引时，使用带索引的遍历**
   ```leno
   for arr to item, index {
       // 既获取元素又获取索引
   }
   ```

4. **倒序遍历时，使用负步进或自动倒序**
   ```leno
   for 10:0 to i      // 自动倒序
   for 10:0:-2 to i   // 指定步进
   ```
