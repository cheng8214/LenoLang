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

## 根因

`transform_method_body_ex()` 中 `AST_ASSIGN` 处理存在两个 bug：

1. **NULL 名称未跳过**：当赋值目标为复杂表达式（如 `v[0].x`）时，`ast->u.assign.names[i]` 为 NULL，但代码直接调用 `strcmp(NULL, field_names[j])`，导致 0xC0000005 崩溃
2. **targets 未递归处理**：`ast->u.assign.targets[]` 数组未被递归处理，复杂赋值目标中的字段/方法访问不会被转换

## 修复

在 `semantic_visit_method.c` 的 `AST_ASSIGN` case 中：
- 添加 `if (!ast->u.assign.names[i]) continue;` 跳过 NULL 名称
- 添加对 `targets[]` 数组的递归 `transform_method_body_ex` 调用

## 不受影响

- 非 export struct
- 顶层函数
- 单实例 malloc（无索引）
- 同文件定义的 cstruct

## 相关 Bug

- [bug_forward_ref_type_infer.md](./bug_forward_ref_type_infer.md)

## 状态

- [x] 已修复
