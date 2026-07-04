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

**实际根因**：语义分析中 `visit_module.inc` 处理 `AST_MODULE_CALL`（如 `t.cb()`）时，只查找 struct 方法定义，未检查 method_name 是否为函数类型字段。当 `cb` 是 `func():void` 类型的字段而非方法时，查找不到方法便报错"类型 'struct Task' 没有方法 'cb'"。

## 修复

在 `visit_module.inc` 的 struct 方法查找逻辑之后、错误报告之前，增加函数类型字段检查：

1. 当 `is_struct_method == 0` 且对象类型为 `TYPE_STRUCT` / `TYPE_CSTRUCT` 时，遍历 struct 的字段列表
2. 检查是否存在名称匹配且类型为 `TYPE_FUNCTION` 的字段
3. 如果找到，标记 `is_func_type_field_call = 1`，跳过"没有方法"错误
4. 后续的 AST 转换（`AST_MODULE_CALL` → `AST_CALL` with `AST_INDEX`）正常进行，`semantic_type.c` 中已有的函数类型字段推断逻辑会正确处理返回类型

修改文件：`src/semantic/visitinc/visit_module.inc`

## 环境

- 发现: 648671a8
- 修复: aba6ee2c

## 状态

- [x] 已修复

## 状态: ✅ 已修复
