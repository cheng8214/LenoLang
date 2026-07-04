# Bug: func():void / func():int 不能作 struct 字段，func():int 变量声明失败

## 概述

`func()`（无参数）形式在 struct 字段和变量声明两个场景解析失败，而 `func(int)`（有参数）形式正常。

## 复现

### 场景 1：func():void 做 struct 字段

```leno
struct Task {
    func():void cb       // ❌ 期望方法名
}
```

`func(int, int):int` **可以**（已有 Calculator 测试），但 `func():void` / `func():int` 不行。

### 场景 2：func():int 变量声明

```leno
func get42(): int { return 42 }
func():int f = get42    // ❌ 期望函数名
```

`func():void fv = sayHello` ✅ 通过，`func(int):int fi = triple` ✅ 通过，
但 `func():int` 和 `func(int,int):void` ❌ 失败。

### 已验证通过（对比）

| 用例 | 类型 | 状态 |
|------|------|------|
| `func():void fv = sayHello` | 变量声明 | ✅ |
| `func(int):int fi = triple` | 变量声明 | ✅ |
| `func(int, int):int op` | struct 字段 | ✅ |
| `func():void cb` | struct 字段 | ❌ |
| `func():int f = get42` | 变量声明 | ❌ |
| `func(int,int):void f = log` | 变量声明 | ❌ |

## 原因

`parse_struct_stmt` 中看到 `func` 关键字后直接走方法定义路径，未区分 `func` 作为字段类型（`func():void cb`）还是方法定义（`func calc() {...}`）。顶部语句的解析已在 `648671a8` 修复，struct 内部尚未同步修复。

对变量声明，`func():int` 的预读逻辑可能未处理 void 返回量类型 / 无参 + 有返回的特殊组合。

## 环境

- 提交: 648671a8
