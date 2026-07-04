# Bug: export func 返回 Ptr[T] 时 clib 调用返回值类型不匹配

## 概述

当 `export func` 声明返回类型为 `Ptr[T]`（如 `Ptr[u8]`），且函数体通过 `as Ptr[T]` 转换 clib 调用返回值时，编译器无法识别转换结果，调用方看到的是 `struct` 类型而非 `Ptr[T]`。

## 复现（见 `examples/测试/test_export_ptr_return.leno` + `test_bug_ptr_mod.leno`）

**`test_bug_ptr_mod.leno`**（被调用模块）：
```leno
import ffi

clib kernel32 {
    Ptr GetCurrentProcess()
}
kernel32 _lib = ffi.load("kernel32.dll")

export func getHandle(): Ptr[u8] {
    return _lib.GetCurrentProcess() as Ptr[u8]
}
```

**`test_export_ptr_return.leno`**（调用方）：
```leno
import "test_bug_ptr_mod.leno" as m

main() {
    Ptr[u8] p = m.getHandle()   // ❌ 编译错误
    print("ok: " + p)
}
```

**编译错误**：
```
[类型不匹配] 类型错误：变量 'p' 声明类型与初始化值类型不匹配
  期望类型: Ptr[u8]
  实际类型: struct
```

## 影响范围

- 所有跨模块 `export func` 返回 `Ptr[T]` 且值来自 clib 的场景
- 本次 SDL3 库 Ptr → Ptr[u8] 迁移中，`sdl_image.leno` 的 `loadImage`/`loadTexture`、`SDL3.leno` 的 `loadImageTexture` 等 3 个函数受影响

## 不受影响

- `export func` 的 **参数类型** `Ptr[u8]` — 传入时正常工作
- **struct 方法** 的返回值 `Ptr[u8]` — `sdl_renderer.leno` 的 `createTexture`/`readPixels` 等方法正常
- **cstruct 字段** 的 `Ptr[u8]` — 正常
- **struct 字段** 的 `Ptr[u8]` — `Window.handle`、`Font.handle`、`TextTexture.texture` 正常
- 函数体内部的 `as Ptr[u8]` 转换 — 局部变量正常

## 根因分析（已确认）

三层问题叠加导致此 bug：

### 1. `module_symbol_table.c` — `parse_type_from_string_inner()` 将 `Ptr[T]` 错误解析为 `TYPE_STRUCT`

模块符号表扫描时，`parse_type_from_string_inner()` 只对 `Array[T]` 做了特殊处理（→ `TYPE_ARRAY`），`Ptr[T]` 走入了泛型 struct 路径，返回 `TYPE_STRUCT` + `struct_name="Ptr"`，而非 `TYPE_PTR_GENERIC`。这与正式解析器（`parser_func.c`）对 `Ptr[T]` 返回 `TYPE_PTR_GENERIC` 的行为不一致。

**修复**：在 `Array` 判断之后增加 `Ptr` 判断，调用 `type_ptr_generic(param1)` 生成 `TYPE_PTR_GENERIC`。

### 2. `visit_module.inc` — `cached_type` 仅用 `type_new(return_type)` 丢失泛型参数

语义分析访问 `AST_MODULE_CALL` 时，设置 `ast->cached_type = type_new(func_sym->return_type)`，仅创建 `TYPE_PTR_GENERIC` 的外壳（kind=40），但 `element_type` 为 NULL。后续 `infer_expr_type()` 命中缓存后返回 `Ptr[u8]`（kind=40, element_type=NULL），与声明的 `Ptr[u8]`（kind=40, element_type=TYPE_U8）不兼容。

**修复**：优先使用 `type_copy(func_sym->return_type_info)` 完整拷贝类型信息（含 `element_type`），回退到 `type_new(return_type)` + `struct_name` 的旧逻辑。

### 3. `op_as_cast.inc` — VM 缺少 `TYPE_PTR_GENERIC` case

VM 的 `OP_AS_CAST` 指令只有 `TYPE_PTR` case，没有 `TYPE_PTR_GENERIC` case。当 codegen 为 `as Ptr[u8]` 生成 `OP_AS_CAST TYPE_PTR_GENERIC TYPE_U8` 时，VM 落入 `default` 分支，`matches=0`，转型始终返回 null。

**修复**：添加 `TYPE_PTR_GENERIC` case，与 `TYPE_PTR` 同样检查 FFI 指针对象类型，并将元素类型记录到 `ObjFFIPointer.element_type`。

### 附带问题：错误消息 `struct]` 末尾多余的 `]`

此问题是 #1 的附带效果：`Ptr[u8]` 被错误解析为 `TYPE_STRUCT` 后，类型显示为 "struct" 而非 "Ptr[u8]"。#1 修复后，类型正确显示为 `Ptr[u8]`，此问题自动解决。

## 临时规避

将 `export func` 返回类型保持为 `Ptr`，调用方通过 `as Ptr[u8]` 局部转换：

```leno
// 模块内（避免 Ptr[u8] 返回）
export func getHandle(): Ptr {
    return _lib.GetCurrentProcess()
}

// 调用方（局部转换）
var p = m.getHandle() as Ptr[u8]
```

## 环境

- 提交: 8d2635ec
- 文件: `examples/测试/test_export_ptr_return.leno`、`examples/测试/test_bug_ptr_mod.leno`

## 状态

- [x] 已修复

## 附带问题

错误消息格式化异常：`实际类型: struct]` 末尾多了一个 `]`。推测是 `Ptr[u8]` 的类型名解析时，`]` 被错误纳入 struct 类型名末尾，可能是 `module_compiler.c` 或 `error.c` 中字符串拼接的问题。

