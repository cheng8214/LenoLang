# Bug: export struct 方法内跨模块 cstruct 数组索引赋值崩溃

## 概述

在 `export struct` 的方法内对**跨模块导入的** cstruct 数组进行索引+字段赋值时，**模块加载阶段**即崩溃（0xC0000005）。

## 精确复现条件

必须**同时满足**以下条件：

1. export struct 有多个方法（≥3）
2. 多个 `use` 导入不同 cstruct 类型（≥2）
3. 其中一个方法使用 `cstruct.malloc_array()[i].x = xxx`

单独任一条件都不会触发。

## 复现（见 `examples/测试/test_cstruct_export_bug.leno`）

```
A (export cstruct) → B (export struct + 多方法 + 数组赋值)
  → C (re-export) → main
```

## 排查过程

| # | 操作 | 结果 |
|---|------|------|
| 1 | 单方法 export struct + 数组赋值 | ✅ |
| 2 | 多方法 export struct + 同文件 cstruct | ✅ |
| 3 | 多方法 export struct + 跨模块 cstruct | ❌ 崩溃 |

## 不受影响

- 非 export struct
- 顶层函数
- 单实例 malloc（无索引）
- 同文件定义的 cstruct

## 相关 Bug

- [bug_forward_ref_type_infer.md](./bug_forward_ref_type_infer.md)

## 状态

- [ ] 待修复
