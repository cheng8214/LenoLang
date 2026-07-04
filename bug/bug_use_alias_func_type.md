# Bug: use 导入的 export alias func 类型被当成 TYPE_STRUCT

## 概述

模块 A 中 `export alias MyCB = func():void`，模块 B 中 `use A.MyCB` 后，`MyCB` 在函数参数中解析为 `TYPE_STRUCT "func"` 而非 `TYPE_FUNCTION`。

## 复现

**bug_repro_alias_mod.leno**（模块 A）：
```leno
export alias MyCB = func():void
```

**bug_repro_alias_use.leno**（模块 B）：
```leno
import "bug_repro_alias_mod.leno" as m
use m.MyCB

// ❌ BUG: 普通函数参数
func callCB(MyCB cb) { cb() }
// → "未定义的类型: func", "struct 'cb' 需要使用 new 关键字"

func hi() { print("hi") }
callCB(hi)  // → "期望 struct func, 实际 func()"

// ❌ BUG: struct 方法参数
struct Local {
    func test(MyCB cb) { cb() }
}
// → 同上错误
```

## 对比

| 方式 | 结果 |
|------|------|
| 同文件 `alias MyCB = func():void` → 直接用 | ✅ |
| `use m.MyCB` → 用 `MyCB` 在变量声明 | ? |
| `use m.MyCB` → 用 `MyCB` 在函数参数 | ❌ TYPE_STRUCT |
| `use m.MyCB` → 用 `MyCB` 在 struct 方法参数 | ❌ TYPE_STRUCT |
| 无 use，用 `m.MyCB` 全路径 | ❌ 语法错误 "期望参数名" |


## 环境

- 提交: aba6ee2c
- 复现: `examples/func/func类型/bug_repro_alias_use.leno`
- 模块: `examples/func/func类型/bug_repro_alias_mod.leno`

## 实际根因

`module_symbol_table.c` 中的 `parse_type_from_string_inner()` 不支持 `func(ParamTypes):ReturnType` 语法。当扫描到 `alias MyCB = func():void` 时，`parse_type_from_string("func():void")` 只读了 `func` 标识符就调用 `parse_simple_type_str`，将 `func` 当成自定义类型名创建 `TYPE_STRUCT` with `struct_name="func"`，而非 `TYPE_FUNCTION`。

## 修复

在 `parse_type_from_string_inner()` 中增加 `func` 标识符的特殊处理：
1. 读到 `func` 后，检查 `(` 解析参数类型列表（递归调用自身）
2. 解析 `:` 后的返回类型，`void` 映射为 `TYPE_NULL`
3. 构建 `TYPE_FUNCTION` 类型的 TypeInfo

同时在 `parse_simple_type_str()` 中增加 `void` → `TYPE_NULL` 的映射。

## 状态

- [x] 已修复
