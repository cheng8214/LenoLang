# Leno 编译器 Bug 记录

## Bug 1: struct 字段声明语法问题

### 问题描述
在 struct 中使用逗号分隔的同类型字段声明（如 `float x1, y1`）时，编译通过但外部无法访问这些字段。

### 复现代码

```leno
// module_a.leno
export struct Point {
    float x, y    // 使用逗号声明多个同类型字段
}

// module_b.leno
import "module_a.leno" as mod
use mod.Point

func test() {
    Point p = new Point()
    p.x = 1.0  // 编译错误：struct 没有字段 'x'
    p.y = 2.0  // 编译错误：struct 没有字段 'y'
}
```

### 预期行为
`float x, y` 和 `float x; float y` 应该是等价的声明方式，外部都可以正常访问。

### 实际行为
- `float x, y` 语法：编译通过，但外部访问字段报错
- `float x; float y` 语法：正常工作

### 临时解决方案
在 struct 中，每个字段单独一行声明：
```leno
export struct Point {
    float x; float y    // 使用分号或换行分隔
}
```

### 影响版本
Leno 编译器（当前版本）

### 严重程度
高 - 影响模块化开发中的结构体跨模块使用

---

## Bug 2: use 语句无法导入常量，报错信息不准确

### 问题描述
使用 `use` 语句导入模块中的 `export const` 常量时，编译器报错信息不准确，提示"模块中没有类型或函数"，而实际上该常量存在。

### 复现代码

```leno
// constants.leno
export const MAX_SIZE = 100
export const MIN_SIZE = 10

// main.leno
import "constants.leno" as consts
use consts.(MAX_SIZE, MIN_SIZE)  // 报错：模块 'consts' 中没有类型或函数 'MAX_SIZE'

func test() {
    int x = consts.MAX_SIZE  // 这种方式正常工作
}
```

### 编译器报错
```
[语义错误] main.leno 第 X 行: use 语句错误：模块 'consts' 中没有类型或函数 'MAX_SIZE'
```

### 预期行为
1. 如果 `use` 语句支持导入常量，应该正常工作
2. 如果不支持，报错信息应该明确说明"use 语句不支持导入常量，请直接使用 module.CONST 访问"

### 实际行为
- `use` 语句只能导入类型（struct、cstruct、enum）和函数
- 常量需要通过 `模块名.常量名` 的方式访问
- 报错信息误导开发者，让人以为常量不存在或导出有问题

### 临时解决方案
不使用 `use` 导入常量，直接通过模块名访问：
```leno
import "constants.leno" as consts

func test() {
    int x = consts.MAX_SIZE  // 正确方式
}
```

### 影响版本
Leno 编译器（当前版本）

### 严重程度
中 - 影响开发体验，报错信息不准确导致调试困难

---

## 建议改进

### 对于 Bug 1
- 修复 struct 解析器，正确处理逗号分隔的字段声明
- 或者在编译期检测并提示开发者使用正确的语法

### 对于 Bug 2
- 改进报错信息："use 语句只能导入类型和函数，常量 'MAX_SIZE' 请使用 consts.MAX_SIZE 访问"
- 或者支持 `use` 导入常量

---

## 相关文件
- 发现于：`d:\CLeno\LenoC\leno_module\LenoWin32\examples\截图工具\annotation.leno`
- 测试于：`d:\CLeno\LenoC\leno_module\LenoWin32\examples\截图工具\screenshot_hotkey.leno`