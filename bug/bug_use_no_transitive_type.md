# Bug: `use` 不携带类型依赖

## 现象

```leno
// sdl_window.leno:  export alias EventHandler = func(Event):bool
// sdl_dialog.leno:
use wnd.EventHandler    // 拿到了 func(Event):bool，但 Event 不可见!
// 必须额外 import sdl_event.leno 才能用 EventHandler
```

`use` 导入类型别名时，别名引用的底层类型（如 `EventHandler → func(Event):bool` 中的 `Event`）不会自动带入当前作用域。

## 影响

- `sdl_dialog.leno` 需要显式 import `sdl_event.leno` + `sdl_renderer.leno`
- 所有依赖别名类型的模块都需要手动补链

## 根因

两个层面缺失类型依赖的递归传导：

1. **语义分析层面**（`visit_module.inc:437-447`）：`use` 处理 alias 时，只注册 alias 自身为 `SYM_TYPE`，不递归扫描其 `TypeInfo` 中引用的类型（如 `Event`）并自动导入
2. **符号表层面**（`module_symbol_table.c:1360-1375`）：`module_symbol_table_scan` 处理 use alias 传导时，只传导 alias 的 `type_info`，不传导其底层类型依赖的 struct/enum/face/alias

另外，变量声明类型中的嵌套 alias（如 `Array[EventHandler]` 中的 `EventHandler`）没有被 `resolve_alias_in_type` 递归解析，导致类型检查时 `EventHandler` 仍被视为 `TYPE_STRUCT` 而非 `TYPE_FUNCTION`。

## 修复

### 1. 语义分析层面 — `import_type_deps()` 函数

在 `src/semantic/semantic_visit_ast.c` 中新增 `import_type_deps()` 函数，递归扫描 `TypeInfo` 中引用的所有 `TYPE_STRUCT`/`TYPE_CSTRUCT`/`TYPE_FACE`/`TYPE_ENUM` 类型，自动从源模块导入到当前作用域。处理 struct 时完整复制字段信息、方法、impl 信息等。

在 `visit_module.inc` 的 alias use 处理中调用：
```c
Symbol* sym = scope_define(s->current, symbol_name, SYM_TYPE);
if (sym && alias_sym->type_info) {
    sym->type = type_copy(alias_sym->type_info);
    // 递归导入 alias 底层类型依赖
    import_type_deps(s, module_info, alias_sym->type_info);
}
```

### 2. 符号表层面 — `import_alias_type_deps_to_sym_table()` 函数

在 `src/module_symbol_table.c` 中新增 `import_alias_type_deps_to_sym_table()` 函数，在 `module_symbol_table_scan` 的 use alias 传导时调用，递归传导底层类型依赖。

### 3. 变量声明类型中的 alias 解析

在 `visit_var.inc` 的类型检查前，添加 `resolve_alias_in_type()` 调用：
```c
resolve_alias_in_type(s, &ast->u.var_decl.type, ast->line);
```

将 `resolve_alias_in_type` 从 `static` 改为非 static，并在 `semantic_internal.h` 中声明。

### 4. 重复 use 导入容错

在 `visit_module.inc` 的 use struct 处理中，当 `scope_define` 返回 NULL 时，如果已有同名且类型名相同的符号（来自 `import_type_deps` 的隐式导入），允许静默跳过而非报错。

## 测试

- `test_use_alias_dep.leno` / `test_use_alias_dep_mod.leno` — 验证 use 导入 alias 时底层类型依赖自动可见
- `test_arr_alias.leno` / `test_arr_alias_mod.leno` — 验证手动 use + 自动导入不冲突
- 152个测试全部通过

## 状态

- [x] 已修复
