# LenoC 类型模块 (types)

本文档说明 `types` 模块提供的类型相关工具函数。注意：这些函数为底层类型操作工具，配合 LenoC 的 `is` 类型守卫和类型系统使用。

## 目录

- [快速参考](#快速参考)
- [type() - 获取类型](#type---获取类型)
- [类型转换函数](#类型转换函数)
- [类型推断详解](#类型推断详解)
- [与类型守卫配合使用](#与类型守卫配合使用)
- [常见使用场景](#常见使用场景)

---

## 快速参考

| 函数 | 用途 | 示例 |
|------|------|------|
| `type(value)` | 获取值的类型字符串 | `type(123)` → `"int"` |
| `_int(value)` | 转换为整数 | `_int("123")` → `123` |
| `_float(value)` | 转换为浮点数 | `_float("3.14")` → `3.14` |
| `_bool(value)` | 转换为布尔值 | `_bool(1)` → `true` |
| `_str(value)` | 转换为字符串 | `_str(123)` → `"123"` |
| `_int32(value)` | 截断为 32 位有符号整数 | `_int32(0x1FFFFFFFF)` → `-1` |
| `_int64(value)` | 截断为 64 位有符号整数 | `_int64(0x1FFFFFFFFFFFFFFFF)` → `-1` |
| `_uint32(value)` | 转为 32 位无符号整数 | `_uint32(-1)` → `4294967295` |
| `_uint64(value)` | 转为 64 位无符号整数 | `_uint64(-1)` → `18446744073709551615` |
| `_uint8(value)` | 截断为 8 位无符号整数 | `_uint8(257)` → `1` |
| `_byte(value)` | `_uint8` 的别名 | `_byte(0xFF)` → `255` |
| `Bint` | 大整数类型关键字 | `Bint x = 12345678901234567890` |

**注意**：所有类型转换函数在失败时会抛出运行时错误。

---

## type() - 获取类型

### 基本用法

返回值的类型字符串，支持精确的类型推断（包括泛型）。

```leno
// 基本类型
print(type(123))        // "int"
print(type(3.14))       // "float"
print(type("hello"))    // "string"
print(type(true))       // "bool"
print(type(null))       // "null"

// 数组类型（支持泛型推断）
print(type([1, 2, 3]))              // "Array[int]"
print(type([1.0, 2.0]))             // "Array[float]"
print(type([1, "hello"]))           // "Array[any]"
print(type([]))                     // "Array"

// 字典类型（支持泛型推断）
print(type({"a": 1, "b": 2}))       // "Dict[string, int]"
print(type({"x": 1.0, "y": 2.0}))   // "Dict[string, float]"
print(type({}))                     // "Dict"

// 函数类型
func add(int a, int b):int { return a + b }
print(type(add))        // "func"
```

### 嵌套类型推断

```leno
// 数组包含字典
var users = [{"name": "Alice"}, {"name": "Bob"}]
print(type(users))      // "Array[Dict[string, string]]"

// 字典包含数组
var data = {"scores": [85, 90, 95]}
print(type(data))       // "Dict[string, Array[int]]"

// 深层嵌套
var nested = {
    "level1": {
        "level2": {
            "value": 100
        }
    }
}
print(type(nested))     // "Dict[string, Dict[string, Dict[string, int]]]"
```

---

## 类型转换函数

### _int(value) - 转换为整数

将值转换为 `int` 类型。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| int | `_int(42)` | `42`（直接返回） |
| float | `_int(3.14)` | `3`（截断小数） |
| bool | `_int(true)` | `1` |
| bool | `_int(false)` | `0` |
| string | `_int("123")` | `123` |
| 大整数 | `_int(超大整数值)` | 转换为 int（可能溢出，对外显示为 int） |

```leno
// 基本转换
print(_int(3.14))       // 3
print(_int("100"))      // 100
print(_int(true))       // 1

// 从 any 转换
func getValue():any { return "42" }
var val = getValue()
var num = _int(val)     // 42
```

**错误情况**:

```leno
_int("12b3")            // ❌ 运行时错误：无法将字符串转换为整数
_int("hello")           // ❌ 运行时错误：无法将字符串转换为整数
_int([1, 2, 3])         // ❌ 运行时错误：无法将该类型转换为整数
```

---

### _float(value) - 转换为浮点数

将值转换为 `float` 类型。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| int | `_float(42)` | `42.0` |
| float | `_float(3.14)` | `3.14`（直接返回） |
| bool | `_float(true)` | `1.0` |
| bool | `_float(false)` | `0.0` |
| string | `_float("3.14")` | `3.14` |
| 大整数 | `_float(超大整数值)` | 转换为 float（可能丢失精度，对外显示为 int） |

```leno
print(_float(42))       // 42.0
print(_float("3.14159")) // 3.14159
print(_float(true))     // 1.0
```

**错误情况**:

```leno
_float("abc")           // ❌ 运行时错误
_float("12.34.56")      // ❌ 运行时错误
```

---

### _bool(value) - 转换为布尔值

将值转换为 `bool` 类型。

**转换规则**:

| 原始类型 | 值 | 结果 | 规则 |
|---------|-----|------|------|
| null | `null` | `false` | 空值转 false |
| bool | `true` | `true` | 直接返回 |
| bool | `false` | `false` | 直接返回 |
| int | `0` | `false` | 0 转 false |
| int | 非 0 | `true` | 非 0 转 true |
| float | `0.0` | `false` | 0.0 转 false |
| float | 非 0.0 | `true` | 非 0 转 true |
| string | `""` | `false` | 空字符串转 false |
| string | 非空 | `true` | 非空字符串转 true |
| Array | 空数组 | `false` | 空数组转 false |
| Array | 非空 | `true` | 非空数组转 true |
| Dict | 任意 | `true` | 字典总为 true |

```leno
// 基本转换
print(_bool(1))         // true
print(_bool(0))         // false
print(_bool(""))        // false
print(_bool("hello"))   // true
print(_bool([]))        // false
print(_bool([1, 2]))    // true
```

---

### _str(value) - 转换为字符串

将值转换为 `string` 类型。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| null | `_str(null)` | `"null"` |
| bool | `_str(true)` | `"true"` |
| bool | `_str(false)` | `"false"` |
| int | `_str(123)` | `"123"` |
| float | `_str(3.14159)` | `"3.14159"` |
| string | `_str("hello")` | `"hello"`（直接返回） |
| 大整数 | `_str(超大整数值)` | 转换为字符串（对外显示为 int） |

```leno
print(_str(123))        // "123"
print(_str(3.14159))    // "3.14159"
print(_str(true))       // "true"
print(_str(null))       // "null"
```

**注意**：其他对象类型会返回类型名：

```leno
var arr = [1, 2, 3]
print(_str(arr))        // "<object>"（数组转字符串显示为对象类型）
```

---

### _int64(value) - 截断为 64 位有符号整数

将值截断为 64 位有符号整数（环绕语义），用于 64 位运算场景。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| int | `_int64(42)` | `42`（直接返回） |
| float | `_int64(3.14)` | `3`（截断小数） |
| bool | `_int64(true)` | `1` |
| null | `_int64(null)` | `0` |
| 大整数 | `_int64(超大值)` | 截断低 64 位，解释为有符号 |

```leno
_int64(42)                          // 42
_int64(0x123456789ABCDEF0)          // 1311768467463790320
_int64(0x1FFFFFFFFFFFFFFFF)         // -1（低 64 位全 1）
```

---

### _uint64(value) - 转为 64 位无符号整数

将值转为 64 位无符号整数（0 ~ 18446744073709551615），用于 FFI 交互和 64 位无符号位运算。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| int | `_uint64(42)` | `42` |
| int（负数） | `_uint64(-1)` | `18446744073709551615` |
| float | `_uint64(3.14)` | `3` |
| bool | `_uint64(true)` | `1` |
| null | `_uint64(null)` | `0` |
| 大整数 | `_uint64(超大值)` | 截断低 64 位，解释为无符号 |

```leno
_uint64(42)                         // 42
_uint64(-1)                         // 18446744073709551615（= 0xFFFFFFFFFFFFFFFF）
_uint64(0x8000000000000000)         // 9223372036854775808（超过 INT64_MAX，自动转为 BigInt）
```

> **💡 超过 INT64_MAX 的值会自动转为 BigInt**，确保无符号语义正确。

---

### _uint8(value) - 截断为 8 位无符号整数

将值截断为 8 位无符号整数（0 ~ 255），用于字节操作、协议解析、颜色值等场景。

**支持的转换**:

| 原始类型 | 示例 | 结果 |
|---------|------|------|
| int | `_uint8(42)` | `42` |
| int（超出范围） | `_uint8(257)` | `1`（257 & 0xFF） |
| int（负数） | `_uint8(-1)` | `255` |
| float | `_uint8(3.14)` | `3` |
| bool | `_uint8(true)` | `1` |
| null | `_uint8(null)` | `0` |

```leno
_uint8(0xFF)            // 255
_uint8(257)             // 1（取低 8 位）
_uint8(-1)              // 255

// 提取字节
int value = 0x12345678
var b0 = _uint8(value)           // 0x78
var b1 = _uint8(value >> 8)      // 0x56
```

---

### _byte(value) - _uint8 的别名

`_byte` 与 `_uint8` 完全等价，但在字节操作场景下语义更清晰。

```leno
_byte(0xFF)             // 255
_byte(257)              // 1

// 典型用法：字节提取（比 _uint8 语义更明确）
int value = 0x12345678
var b0 = _byte(value)           // 0x78（最低字节）
var b1 = _byte(value >> 8)      // 0x56
var b2 = _byte(value >> 16)     // 0x34
var b3 = _byte(value >> 24)     // 0x12
```

---

### 固定位宽函数对比

| 函数 | 返回范围 | 用途 |
|------|---------|------|
| `_int32(x)` | -2147483648 ~ 2147483647 | 32 位有符号环绕 |
| `_uint32(x)` | 0 ~ 4294967295 | 32 位无符号位运算 |
| `_int64(x)` | -9223372036854775808 ~ 9223372036854775807 | 64 位有符号运算 |
| `_uint64(x)` | 0 ~ 18446744073709551615 | 64 位无符号位运算、FFI |
| `_uint8(x)` | 0 ~ 255 | 字节操作 |
| `_byte(x)` | 0 ~ 255 | `_uint8` 别名 |

> **💡 与 `_int()` 的区别**：`_int()` 是类型转换，不改变数值；固定位宽函数是位宽截断，会环绕。

---

## 类型推断详解

### 数组类型推断

`type()` 会深入分析数组元素类型：

```leno
// 同类型元素
print(type([1, 2, 3]))              // "Array[int]"
print(type(["a", "b", "c"]))        // "Array[string]"

// 混合类型（类型提升）
print(type([1, 2.0]))               // "Array[float]"（int 提升为 float）
print(type([1, "hello"]))           // "Array[any]"（无法提升，变为 any）

// 嵌套数组
print(type([[1, 2], [3, 4]]))       // "Array[Array[int]]"

// 空数组
print(type([]))                     // "Array"
```

### 字典类型推断

```leno
// 同类型值
print(type({"a": 1, "b": 2}))       // "Dict[string, int]"
print(type({"x": 1.0, "y": 2.0}))   // "Dict[string, float]"

// 混合类型值
print(type({"a": 1, "b": "hello"})) // "Dict[string, any]"

// 嵌套字典
print(type({"data": {"value": 100}}))   // "Dict[string, Dict[string, int]]"

// 空字典
print(type({}))                     // "Dict"
```

---

## 与类型守卫配合使用

类型转换函数经常与 `is` 类型守卫配合使用：

### 场景 1：安全的类型转换

```leno
func process(var x) {
    if x is int {
        // 类型守卫收窄后，可以直接使用
        int n = x
        print("整数: " + n)
    } else if x is string {
        // 尝试将字符串转换为整数
        try {
            int n = _int(x)
            print("从字符串转换: " + n)
        } catch {
            print("无法转换为整数")
        }
    } else {
        // 其他类型，先转字符串再处理
        string s = _str(x)
        print("其他类型: " + s)
    }
}

process(42)         // 整数: 42
process("100")      // 从字符串转换: 100
process("abc")      // 无法转换为整数
process([1, 2])     // 其他类型: <object>
```

### 场景 2：处理 any 参数

```leno
// 不推荐：直接使用 any
func bad_add(var a, var b) {
    return a + b    // 编译通过，但可能运行时错误
}

// 推荐：先检查再转换
func safe_add(var a, var b) {
    if a is int and b is int {
        return a + b                    // ✅ 直接使用（已收窄）
    }
    
    // 尝试转换为数字
    var num_a = a
    var num_b = b
    
    if a is string {
        num_a = _int(a)                 // 转换字符串为 int
    }
    if b is string {
        num_b = _int(b)                 // 转换字符串为 int
    }
    
    return num_a + num_b
}

print(safe_add(1, 2))           // 3
print(safe_add("10", "20"))     // 30
```

---

## 常见使用场景

### 场景 1：调试和日志

```leno
func debug(var value) {
    print("[DEBUG] 类型: " + type(value) + ", 值: " + _str(value))
}

debug(123)              // [DEBUG] 类型: int, 值: 123
debug([1, 2, 3])        // [DEBUG] 类型: Array[int], 值: <object>
debug({"a": 1})         // [DEBUG] 类型: Dict[string, int], 值: <object>
```

### 场景 2：类型检查辅助

```leno
func process_data(var data) {
    var t = type(data)
    
    if t == "Array[int]" {
        print("处理整数数组")
        // ...
    } else if t == "Dict[string, int]" {
        print("处理整数字典")
        // ...
    } else {
        print("未知类型: " + t)
    }
}
```

### 场景 3：配置解析

```leno
func parse_config(Dict[string, string] config) {
    var port = _int(config.get("port", "8080"))
    var timeout = _float(config.get("timeout", "30.0"))
    var debug = _bool(config.get("debug", "false"))
    
    print("端口: " + port)
    print("超时: " + timeout + " 秒")
    print("调试: " + debug)
}

var config = {
    "port": "9090",
    "timeout": "60.5",
    "debug": "true"
}
parse_config(config)
// 端口: 9090
// 超时: 60.5 秒
// 调试: true
```

### 场景 4：用户输入处理

```leno
func get_user_input(string prompt):int {
    while true {
        print(prompt)
        var input = input()         // 获取用户输入（字符串）
        
        if input == "quit" {
            return 0
        }
        
        try {
            return _int(input)      // 尝试转换为整数
        } catch {
            print("无效输入，请输入数字！")
        }
    }
    return 0
}

var age = get_user_input("请输入年龄: ")
print("您的年龄是: " + age)
```

---

## 注意事项

1. **转换失败会报错**：所有 `_xxx()` 函数在转换失败时会抛出运行时错误，建议使用类型守卫先做检查。

2. **与类型守卫的关系**：
   - `is` 类型守卫用于编译时类型收窄（更安全）
   - `_xxx()` 转换函数用于运行时类型转换（更灵活）

3. **优先使用类型守卫**：

```leno
// 推荐：使用类型守卫（编译时检查）
func good(var x) {
    if x is int {
        return x * 2        // ✅ 安全，编译器知道 x 是 int
    }
    return 0
}

// 不推荐：直接转换（可能运行时错误）
func bad(var x) {
    return _int(x) * 2      // ❌ 可能运行时错误
}
```

---

*文档版本: 1.2*  
*最后更新: 2026-06-14*
