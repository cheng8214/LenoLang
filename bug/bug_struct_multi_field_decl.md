# Bug: struct 不支持多字段同行声明

## 概述

`struct` 内部不支持 `int a, b, c` 多字段同行声明，必须拆成单独声明。顶层代码和函数内支持此语法。

## 复现

```leno
struct S {
    int a, b, c          // ❌ 期望字段类型
    int x                // ✅ 单独声明正常
}
```

**错误**：
```
[语法错误] 期望字段类型
```

文件：`examples/测试/test_bug_struct_multi_field.leno`

## 对比（顶层/函数内支持）

```leno
// ✅ 顶层/函数内可以
int a, b, c
```

## 影响

- `float x, y, w, h` 在 struct 内不能简写，必须四行
- `int cr, cg, cb` 必须拆为三行

## 不受影响

- `string x = "hi"` — 单字段带默认值，正常

## 修复方向

`parser.c` 中 struct 字段声明的解析语法需扩展支持逗号分隔。

## 状态

- [ ] 待修复
