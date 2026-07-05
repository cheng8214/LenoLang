# Bug: Array 类型在 export func 中的两个问题

## 环境
- 提交: c17a0097 (type-alias-SYM_TYPE 修复后)
- 复现: `examples/测试/repro_array_alias_func.leno`, `examples/测试/repro_export_func_array.leno`

## Bug 1: `Array[alias_func]` 别名丢失，推断为原始类型

### 现象

```leno
export alias MyHandler = func(int):int

MyHandler h = func(int x): int { return x + 1 }
print(h is MyHandler)           // → false  ← 别名丢失!
```

### 根因

VM 的 `OP_TYPE_CHECK` 处理程序缺少 `case TYPE_FUNCTION:` 分支，`is MyHandler` 展开为 `is func(int):int`（TYPE_FUNCTION），落入 `default` 永远返回 false。

### 修复

在 `src/vm/vminc/op_type_check.inc` 添加 `case TYPE_FUNCTION:` 分支，检查值是否为 OBJ_CLOSURE/OBJ_NATIVE/OBJ_FFI_CALLBACK。

- [x] 已修复（`h is MyHandler` 和 `h is func` 现在正确返回 true）

## Bug 2: `export func(Array[struct])` 参数 "未定义的类型"

### 现象

```leno
export struct Point { float x; float y }

export func processOne(Point pt) { ... }          // ✅ 单 struct 参数 OK
export func process(Array[Point] pts) { ... }     // ❌ "未定义的类型: Point"
```

### 状态

- [x] 已不再复现。之前的 type-alias-SYM_TYPE 修复（resolve_alias_in_type + check_undefined_type 递归检查 element_type）已解决了此问题。

## 状态

- [x] 已修复
