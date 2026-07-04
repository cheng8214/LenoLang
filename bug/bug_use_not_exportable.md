# Bug: `use` 导入的类型不能通过链式 `import` 访问

## 环境
- 发现: `b87e055c`
- 复现文件: `assert/test_use_reexport_*.leno`

## 现象

模块 B 用 `use` 导入的类型（enum/struct 等），外部 C 通过 `import B as b` 无法访达，`b.TypeName` 为 `null`：

```leno
// A.leno: export enum Flag { ON = 1; OFF = 0 }
// B.leno: import "A.leno" as a; use a.Flag  ← B 自己能用 Flag.ON
// C.leno: import "B.leno" as b
main() {
    print(b.Flag)       // → null
    print(b.Flag.ON)    // → 崩溃: "索引操作需要对象类型，但实际类型为 'null'"
}
```

## 根因

`b.Flag` 运行时通过 `dict_get(module->exports, "Flag")` 查找。B 的 `exports` 字典中没有 Flag 键，因为：

1. `extract_exports()` 只扫描 `export` 关键字声明，不扫描 `use` 语句
2. `export_mappings` 中没有 Flag 的条目
3. `use` 是编译期操作，不在模块的运行时 exports 中注册被导入的类型

## 修复

在模块对象 `ObjModule` 中添加 `use_reexport_names/kinds/count` 字段，记录 `use` 导入的需要 re-export 的类型（enum/struct）。模块初始化时，通过 `enum_def_find` / `struct_def_find` 获取定义并添加到 exports 字典。

### 修改的文件

| 文件 | 修改内容 |
|------|---------|
| `src/include/leno_value.h` | `ObjModule` 添加 `use_reexport_names/kinds/count` |
| `src/module.c` | `module_new()` 初始化新字段 |
| `src/module_compiler.c` | 编译时收集 `use` 导入的 enum/struct 到 re-export 列表 |
| `src/module_loader.c` | 将 re-export 信息从编译模块复制到占位符模块 |
| `src/vm/vminc/op_init_lenomodule.inc` | 模块初始化后将 re-export 类型添加到 exports 字典 |

### 数据流

```
B 源码: use a.Flag
    ↓ module_compiler.c 编译时扫描 AST
ObjModule.use_reexport_names = ["Flag"]
ObjModule.use_reexport_kinds = [TYPE_ENUM]
    ↓ module_loader.c 复制到 placeholder_module
    ↓ C import B 时触发 OP_INIT_LENOMODULE
    ↓ op_init_lenomodule.inc:
      enum_def_find("Flag") → ObjEnumDef
      dict_set(module->exports, "Flag", ObjEnumDef)
    ↓ C 运行时: b.Flag → dict_get(exports, "Flag") → ObjEnumDef ✅
```

## 回归测试

- `assert/test_use_reexport_a.leno` — 最底层：export enum + struct
- `assert/test_use_reexport_b.leno` — 中间层：use a.Flag + a.Counter
- `assert/test_use_reexport_c.leno` — 顶层：import b + b.Flag.ON + use b.Counter

## 状态

- [x] 已修复
