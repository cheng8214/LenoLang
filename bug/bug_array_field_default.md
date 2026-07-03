# Bug: struct 中 Array/Dict 字段未自动初始化为空容器

## 概述

`struct` 中声明 `Array` 或 `Dict` 类型的字段时，`new Struct()` 后字段值为 `null`/any，而非空数组 `[]` 或空字典 `{}`。必须在方法内显式赋值 `_cb = []` 后才能调用 `.add()`、`.len()` 等方法。

## 复现

```leno
struct S {
    Array  a

    func test() {
        a.add(42)      // ❌ 运行时错误: "append 操作需要数组"
    }

    func testFix() {
        a = []          // 必须先手动初始化
        a.add(42)       // ✅ OK
    }
}

main() {
    S s = new S()
    print(s.a.len())    // ❌ 运行时错误: "只能获取数组、字典或模块的属性"
    s.test()
}
```

文件：`examples/测试/repro_array_field.leno`

## 影响

- 所有使用 `Array` / `Dict` 作为 struct 字段的场景都需要在构造时手动 `_field = []` / `_field = {}`，容易遗漏导致运行时崩溃
- 与 int/bool/float 等标量字段的自动初始化行为不一致（`int x` 自动为 0，`bool b` 自动为 false）

## 修复方向

在 `struct` 实例化（`new Struct()` 或 `Struct.malloc()`）时，对 `TYPE_ARRAY` 字段自动赋值为 `[]`，对 `TYPE_DICT` 字段自动赋值为 `{}`。相关代码在 `struct_def` 和 `new` 指令的实现中。

## 根因

`object_struct.c` 的 `struct_instance_new_depth()` 中，字段初始化逻辑：
1. `has_default` → 使用默认值
2. `TYPE_STRUCT` → 递归创建嵌套实例
3. **else → `val_null()`** ← 对 Array/Dict 字段错误地设为 null

## 修复

在 else 链中添加 `TYPE_ARRAY` → `arr_new(0)` 和 `TYPE_DICT` → `dict_new(0)` 分支，
在无显式默认值时自动初始化为空容器。

## 临时规避

在 struct 方法中首次使用 Array/Dict 字段前，先执行：
```leno
_cb = []; _cb.add(...)
// 或
_d = {}; _d["k"] = v
```

## 环境

- 提交: f1b00c04
- 文件: `examples/测试/repro_array_field.leno`

## 状态

- [x] 已修复
