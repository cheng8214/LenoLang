# 修复 clib 类型相关编译器 Bug

## Context

两个编译器 Bug：

1. **clib 方法返回类型推断错误**：调用 clib 实例方法（如 `ttf.TTF_OpenFont()`）时，编译器推断返回类型为 clib 类型本身（如 `sdl3_ttf`），而非 clib 声明的返回类型（如 `Ptr[u8]`）。
2. **clib 专属原始类型可在 clib 外使用**：`u8`、`i32` 等 C 布局类型应该仅在 clib 声明中合法，普通 Leno 代码应使用 `int`、`float`、`string`。但 `Ptr[u8]` 应允许（顶层类型是 `TYPE_PTR_GENERIC`，不是 clib 原始类型）。

## Bug 1 修复：clib 方法返回类型推断

### 根因

`infer_expr_type()` 中 `AST_MODULE_CALL` 路径（semantic_type.c 第1496-1504行）：
- 当 `func->return_struct_name` 存在时，只处理了 `TYPE_FACE` 和 `TYPE_STRUCT`
- 当 `func->return_type == TYPE_CLIB` 时，没有进入任何分支，导致 `struct_name` 未设置
- 返回的 TypeInfo 是 `TYPE_CLIB` 但 `struct_name = NULL`，后续被当作无效类型处理

### 修改文件

**`src/semantic/semantic_type.c`** 第1502行后，添加 `TYPE_CLIB` 和 `TYPE_CSTRUCT` 分支：

```c
} else if (func->return_type == TYPE_CLIB) {
    type->kind = TYPE_CLIB;
    type->struct_name = strdup(func->return_struct_name);
} else if (func->return_type == TYPE_CSTRUCT) {
    type->kind = TYPE_CSTRUCT;
    type->struct_name = strdup(func->return_struct_name);
}
```

## Bug 2 修复：clib 原始类型限制

### 设计

1. 在 `Semantic` 结构体中添加 `in_clib` 标志
2. 添加辅助函数 `is_clib_only_type(TypeKind)` 判断是否为 clib 专属类型
3. 在变量声明、函数参数/返回值中检查是否在 clib 外使用了 clib 专属类型
4. clib 声明处理时设置/重置 `in_clib` 标志
5. `Ptr[u8]` 等指针类型不受限制（顶层类型是 `TYPE_PTR_GENERIC`）

### 修改文件

**1. `src/include/leno_semantic.h`** — Semantic 结构体添加字段：

```c
int in_clib;      // 是否在 clib 声明上下文中（允许使用 u8/i32 等 C 布局类型）
```

**2. `src/semantic/semantic_type.c`** 或 **`src/include/leno_types.h`** — 添加辅助函数：

```c
// 检查是否为 clib 专属原始类型（仅限在 clib 声明中使用）
static bool is_clib_only_type(TypeKind kind) {
    switch (kind) {
        case TYPE_I8: case TYPE_U8:
        case TYPE_I16: case TYPE_U16:
        case TYPE_I32: case TYPE_U32:
        case TYPE_I64: case TYPE_U64:
        case TYPE_F32: case TYPE_F64:
        case TYPE_STR8: case TYPE_STR16:
        case TYPE_C_INT:
            return true;
        default:
            return false;
    }
}
```

**3. `src/semantic/visitinc/visit_var.inc`** — 变量声明检查（在类型解析后、约第59行）：

```c
// 检查变量声明类型是否使用了 clib 专属原始类型（如 i32, u8 等）
if (!s->in_clib && ast->u.var_decl.type && is_clib_only_type(ast->u.var_decl.type->kind)) {
    char msg[BUFFER_MEDIUM];
    snprintf(msg, sizeof(msg), "类型 '%s' 仅限在 clib 声明中使用，请使用 '%s' 代替",
             type_kind_to_string(ast->u.var_decl.type->kind),
             ast->u.var_decl.type->kind == TYPE_F32 || ast->u.var_decl.type->kind == TYPE_F64 ? "float" : "int");
    error_add(ERR_SEMANTIC, ast->line, msg);
}
```

**4. `src/semantic/semantic_visit_func.c`** — 函数参数和返回值检查（在 `visit_func_impl` 中，约第297-323行附近）：

在 `check_undefined_type` 调用之后添加：
```c
// 检查参数类型是否使用了 clib 专属原始类型
if (!s->in_clib) {
    for (int i = 0; i < ast->u.func.pcnt; i++) {
        if (ast->u.func.param_types[i] && is_clib_only_type(ast->u.func.param_types[i]->kind)) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "参数 '%s' 的类型 '%s' 仅限在 clib 声明中使用，请使用 '%s' 代替",
                     ast->u.func.params[i],
                     type_kind_to_string(ast->u.func.param_types[i]->kind),
                     ast->u.func.param_types[i]->kind == TYPE_F32 || ast->u.func.param_types[i]->kind == TYPE_F64 ? "float" : "int");
            error_add(ERR_SEMANTIC, ast->line, msg);
        }
    }
    if (ast->u.func.return_type && is_clib_only_type(ast->u.func.return_type->kind)) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg), "返回类型 '%s' 仅限在 clib 声明中使用，请使用 '%s' 代替",
                 type_kind_to_string(ast->u.func.return_type->kind),
                 ast->u.func.return_type->kind == TYPE_F32 || ast->u.func.return_type->kind == TYPE_F64 ? "float" : "int");
        error_add(ERR_SEMANTIC, ast->line, msg);
    }
}
```

**5. `src/semantic/visitinc/visit_ffi.inc`** — clib 声明处理时设置标志：

在 `case AST_CLIB_DEF:` 入口处设置 `s->in_clib = 1`，在 break 前重置 `s->in_clib = 0`。
同样在 `case AST_CSTRUCT_DEF:` 入口和出口处理。

**6. `src/semantic/semantic_visit_func.c`** — 初始化：确保 `visit_func` 不在 clib 上下文中调用时 `in_clib` 为 0（默认值）。

### 需要确认的 type_kind_to_string 覆盖

需要确保 `type_kind_to_string` 能正确输出 `u8`, `i32`, `f32` 等字符串。检查 `leno_types.h` 中是否有此函数或类似功能。

## 验证

1. 编译 `screenshot_hotkey.leno`，确认 `ttf.TTF_OpenFont()` 返回 `Ptr[u8]` 而非 `sdl3_ttf`
2. 创建测试用例 `i32 x = 10` → 应报错 "类型 'i32' 仅限在 clib 声明中使用"
3. 创建测试用例 `Ptr[u8] p = null` → 应编译通过
4. 在 clib 声明中 `i32 func(Ptr[u8] buf)` → 应编译通过
5. 运行完整测试套件确保无回归
