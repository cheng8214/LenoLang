# LenoC IO 模块

本文档详细说明 `io` 模块提供的基础输入输出功能。

## 目录

- [概述](#概述)
- [全局函数](#全局函数)
- [模块方法](#模块方法)
- [输出函数](#输出函数)
- [输入函数](#输入函数)
- [系统函数](#系统函数)
- [输出格式](#输出格式)
- [示例代码](#示例代码)
- [注意事项](#注意事项)

---

## 概述

IO 模块是 Leno 语言中用于基础输入输出的核心模块，提供了控制台打印、用户输入、系统信息查询等功能。

### 特性

- **全局可用**：`print`、`input` 等函数无需导入即可使用
- **模块方式**：也可以通过 `import io` 使用模块方式调用
- **多类型支持**：支持任意类型的输出
- **格式化输出**：自动处理各种数据类型的显示格式

---

## 全局函数

以下函数无需导入，直接在代码中使用：

| 函数 | 说明 | 示例 |
|-----|------|------|
| `print(...)` | 打印并换行 | `print("Hello")` |
| `printf(...)` | 打印不换行 | `printf("Loading...")` |
| `input(prompt)` | 获取用户输入 | `input("Name: ")` |

---

## 模块方法

通过 `import io` 可以使用模块方式调用：

```leno
import io

io.print("Hello World")
var name = io.input("Your name: ")
```

模块方法与全局函数功能完全相同。

---

## 输出函数

### `print(...)`

打印一个或多个值，末尾自动添加换行。

**参数**:
- 可变参数，支持任意类型

**返回**: `null`

```leno
// 打印字符串
print("Hello World")

// 打印数字
print(42)
print(3.14)

// 打印多个值（用空格分隔）
print("Name:", "Alice", "Age:", 25)
// 输出: Name: Alice Age: 25

// 打印变量
var name = "Bob"
var age = 30
print(name, "is", age, "years old")
// 输出: Bob is 30 years old

// 打印数组
var arr = [1, 2, 3]
print(arr)
// 输出: [1, 2, 3]

// 打印字典
var dict = {"a": 1, "b": 2}
print(dict)
// 输出: {a: 1, b: 2}

// 打印结构体
cstruct Point { i32 x, i32 y }
var p = Point.malloc()
p.x = 10; p.y = 20
print(p)
// 输出: cstruct Point{x=10, y=20}
p.free()
```

---

### `printf(...)`

打印一个或多个值，**不**添加换行。

**参数**:
- 可变参数，支持任意类型

**返回**: `null`

```leno
// 不换行输出
printf("Loading")
printf("...")
print(" Done!")
// 输出: Loading... Done!

// 制作进度条效果
for 0 : 10 to i {
    printf("#")
}
print()  // 最后换行
// 输出: ##########

// 同一行输出多个部分
printf("[")
printf("INFO")
printf("] ")
print("Message here")
// 输出: [INFO] Message here
```

---

## 输入函数

### `input(prompt)`

获取用户输入，可选显示提示信息。

**参数**:
- `prompt` (string, 可选): 提示信息

**返回**: `string` - 用户输入的字符串（不含换行符）

```leno
// 简单输入
print("请输入你的名字:")
var name = input()
print("你好, " + name + "!")

// 带提示的输入
var age = input("请输入年龄: ")
print("你输入了: " + age)

// 转换为数字
var num_str = input("输入一个数字: ")
var num = num_str.to_int()
print("平方是: " + (num * num))

// 多行输入示例
print("请输入三行文字:")
var line1 = input("1> ")
var line2 = input("2> ")
var line3 = input("3> ")
print("你输入了:")
print(line1)
print(line2)
print(line3)
```

---

## 输出格式

### 基本类型输出格式

| 类型 | 输出示例 | 说明 |
|-----|---------|------|
| `null` | `null` | 小写 |
| `bool` | `true` / `false` | 小写 |
| `int` | `42` | 十进制整数 |
| `float` | `3.14` / `2.0` | 总是显示小数部分 |
| `string` | `Hello` | 普通输出不加引号 |

### 复合类型输出格式

```leno
// 数组 - 方括号包裹，逗号分隔
var arr = [1, 2, 3]
print(arr)  // [1, 2, 3]

// 嵌套数组
var nested = [[1, 2], [3, 4]]
print(nested)  // [[1, 2], [3, 4]]

// 字典 - 花括号包裹，键值对格式
var dict = {"name": "Alice", "age": 25}
print(dict)  // {name: Alice, age: 25}

// 字符串在数组/字典中加引号
var arr_with_str = ["a", "b"]
print(arr_with_str)  // ["a", "b"]

var dict_with_str = {"key": "value"}
print(dict_with_str)  // {key: "value"}
```

### 特殊对象输出

```leno
// FFI 指针
import ffi
var ptr = ffi.malloc(100)
print(ptr)  // <ptr 0x...>
ffi.free(ptr)

// 文件对象
import files
var f = files.open("test.txt", "w")
print(f)  // <file>
f.close()

// cstruct 定义
cstruct Point { i32 x, i32 y }
print(Point)  // cstruct Point{i32 x, i32 y}

// cstruct 实例
var p = Point.malloc()
print(p)  // cstruct Point{x=0, y=0}
p.free()

// BigInt
var big = 12345678901234567890
print(big)  // 12345678901234567890
```

---

## 示例代码

### 示例1：简单的交互式程序

```leno
main() {
    print("========== 用户注册 ==========")
    
    // 获取用户输入
    var name = input("请输入用户名: ")
    var age_str = input("请输入年龄: ")
    var email = input("请输入邮箱: ")
    
    // 转换年龄为数字
    var age = _int(age_str)
    
    // 显示注册信息
    print("\n========== 注册信息 ==========")
    print("用户名:", name)
    print("年龄:", age)
    print("邮箱:", email)
    
    // 确认
    var confirm = input("\n信息正确吗? (y/n): ")
    if confirm == "y" or confirm == "Y" {
        print("注册成功!")
    } else {
        print("请重新注册")
    }
}
```

### 示例2：简单的计算器

```leno
main() {
    print("========== 简单计算器 ==========")
    
    // 获取第一个数字
    var num1_str = input("输入第一个数字: ")
    var num1 = _float(num1_str)
    
    // 获取运算符
    var op = input("输入运算符 (+, -, *, /): ")
    
    // 获取第二个数字
    var num2_str = input("输入第二个数字: ")
    var num2 = _float(num2_str)
    
    // 计算结果
    var result = 0
    if op == "+" {
        result = num1 + num2
    } else if op == "-" {
        result = num1 - num2
    } else if op == "*" {
        result = num1 * num2
    } else if op == "/" {
        if num2 != 0 {
            result = num1 / num2
        } else {
            print("错误: 不能除以零")
            return
        }
    } else {
        print("错误: 无效的运算符")
        return
    }
    
    // 显示结果
    print("结果:", num1, op, num2, "=", result)
}
```

### 示例3：进度显示

```leno
import times

main() {
    print("========== 处理中 ==========")
    
    var total = 20
    
    for 0 : total to i {
        // 计算进度百分比
        var percent = (i * 100) / total
        
        // 显示进度条
        printf("\r[")
        for 0 : i to j {
            printf("=")
        }
        for i : total - 1 to j {
            printf(" ")
        }
        printf("] %d%%", percent)
        
        // 模拟处理时间
        times.sleep(100)
    }
    
    print("\n处理完成!")
}
```

### 示例4：调试信息输出

```leno
// 调试级别
var DEBUG = true

func debug_print(msg) {
    if DEBUG {
        print("[DEBUG]", msg)
    }
}

func info_print(msg) {
    print("[INFO]", msg)
}

func error_print(msg) {
    print("[ERROR]", msg)
}

main() {
    info_print("程序启动")
    
    debug_print("初始化变量...")
    var data = [1, 2, 3, 4, 5]
    debug_print("数据:", data)
    
    debug_print("处理数据...")
    var sum = 0
    for data to item {
        sum = sum + item
    }
    
    info_print("总和:", sum)
    
    if sum > 100 {
        error_print("总和超出预期")
    } else {
        info_print("结果正常")
    }
    
    info_print("程序结束")
}
```

### 示例5：跨平台路径处理

```leno
import files
import dirs

main() {
    // 获取操作系统
    var os = _os()
    print("操作系统:", os)
    
    // 根据系统选择路径格式
    var path
    if os == "windows" {
        path = "C:\\Users\\Admin\\Documents"
    } else {
        path = "/home/user/documents"
    }
    
    // 更好的方式：使用 dirs 模块
    var better_path = dirs.join("folder", "subfolder", "file.txt")
    print("路径:", better_path)
    
    // 检查文件是否存在
    var test_file = "test.txt"
    if files.exists(test_file) {
        print("文件存在:", test_file)
    } else {
        print("文件不存在:", test_file)
    }
}
```

---

## 注意事项

### 1. print vs printf

- `print` 自动添加换行，适合大多数场景
- `printf` 不添加换行，适合需要连续输出的场景

```leno
// print 自动换行
print("Line 1")
print("Line 2")
// 输出:
// Line 1
// Line 2

// printf 需要手动换行
printf("Line 1\n")
printf("Line 2\n")
```

### 2. 输入处理

`input()` 返回的是字符串，需要进行类型转换：

```leno
// 获取数字输入
var num_str = input("输入数字: ")
var num = _int(num_str)      // 转为整数
var float_num = _float(num_str)  // 转为浮点数

// 安全转换
if num_str.is_numeric() {
    var num = _int(num_str)
} else {
    print("输入不是有效的数字")
}
```

### 3. 输出大对象

输出大型数组或字典可能会产生大量输出：

```leno
// 大数组
var big_arr = []
for 0 : 10000 to i {
    big_arr.add(i)
}
// 不要直接打印大数组
// print(big_arr)  // 会产生10000行输出

// 只打印部分信息
print("数组长度:", big_arr.len())
print("前5个元素:", big_arr[0:4])
```

## 最佳实践

1. **使用 print 进行调试**
   ```leno
   // 在关键位置打印变量值
   print("DEBUG: x =", x, "y =", y)
   ```

2. **格式化输出信息**
   ```leno
   // 使用前缀区分信息类型
   print("[INFO] 操作成功")
   print("[WARN] 参数可能不正确")
   print("[ERROR] 发生错误:", error_msg)
   ```

3. **处理用户输入**
   ```leno
   // 始终验证输入
   var input_str = input("请输入: ")
   if input_str == "" {
       print("输入不能为空")
   }
   ```

4. **避免频繁输出**
   ```leno
   // 批量收集输出
   var logs = []
   for data to item {
       logs.add("处理: " + item)
   }
   // 一次性输出
   for logs to log {
       print(log)
   }
   ```

5. **使用 printf 制作动态效果**
   ```leno
   // 进度显示
   printf("\r进度: %d%%", percent)
   // 注意：最后要换行
   print()  // 结束进度显示
   ```

---

*文档版本: 1.0*  
*最后更新: 2026-05-17*
