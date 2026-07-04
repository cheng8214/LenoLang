# Bug: 函数类型不支持变量声明、struct 字段、匿名闭包传参

## 概述

Leno 已支持 `func(T):U` 函数类型（见 `examples/func/func类型/test_func_type.leno`），但仅限于**返回值类型**和**参数类型**两个位置。以下三种场景不生效：

1. **变量类型声明**：`func():void v = sayHello` → 语法错误
2. **struct 字段类型**：`func(int,int):int op` → 语法错误
3. **匿名闭包传参**：`apply(func(x):int { ... }, 1)` → 语法错误

## 复现

### 场景 1：变量类型声明

```leno
func sayHello() { print("hi") }
func():void fv = sayHello    // ❌ 期望函数名
fv()
```

**错误**：`[语法错误] 期望函数名`

### 场景 2：struct 字段

```leno
struct Calculator {
    func(int, int):int op     // ❌ 期望方法名

    func calc(int a, int b): int {
        return op(a, b)
    }
}
```

**错误**：`[语法错误] 期望方法名`

### 场景 3：匿名闭包作回调参数

```leno
func apply(func(int):int fn, int val): int {
    return fn(val)
}

var r = apply(func(x):int { return x + 100 }, 1)  // ❌ 期望参数名
```

**错误**：`[语法错误] 期望参数名`

## 已验证可用的场景（对比）

| 场景 | 示例 | 状态 |
|------|------|------|
| 返回值类型 | `func foo(): func():int { ... }` | ✅ |
| 参数类型（单参） | `func bar(func(int):int fn)` | ✅ |
| 参数类型（多参） | `func baz(func(int,int):int op)` | ✅ |
| 参数类型（void） | `func run(func():void fn)` | ✅ |
| 闭包返回函数 | `func makeCounter(): func():int` | ✅ |
| 命名函数作实参 | `bar(triple)` | ✅ |
| **变量声明** | `func():void v = sayHello` | ❌ |
| **struct 字段** | `struct S { func():void cb }` | ❌ |
| **匿名闭包实参** | `bar(func(x):int {...}, 1)` | ❌ |

## 影响

- **SDL3 Timer**：无法将 `var cb` 声明为 `func():void`，只能用 `var`（any 类型），丢失编译期类型检查
- **回调存储**：struct 无法声明带类型的回调字段
- **链式调用**：匿名闭包不能直接在调用点传入高阶函数

场景 3 是表达式级匿名函数解析问题，匿名闭包 `func(x):int { ... }` 在函数调用实参位置时解析器未正确识别。

## 环境

- 提交: 3ddff11f
- 测试: `examples/func/func类型/test_func_type.leno`

## 状态

- [ ] 待修复
