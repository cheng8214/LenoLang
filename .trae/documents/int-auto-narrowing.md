# 整数类型自动收窄实施方案

## Context

从 SDL3 封装实践中发现：cstruct 字段类型（i32/u8 等）每次都需要 `as int` 转换，SDL3 封装中出现了 50+ 次 `as int`。这些转换本质是无损操作却要显式写，属于语言摩擦。

**根因**：cstruct 字段访问的 `infer_expr_type` 返回原始 C 布局 TypeKind（如 `TYPE_I32`），而非 Leno 等价类型（`TYPE_INT`）。虽然 `type_is_compatible()` 已处理 `TYPE_INT ↔ TYPE_I32` 等，但 5 处手动 `bool type_compatible` 检查块未覆盖 C 布局类型。

## 修改文件清单

| 文件 | 改动 |
|------|------|
| `src/type.c` | 新增 `c_layout_type_to_leno()` 辅助函数；补充 `type_is_compatible` 遗漏规则 |
| `src/include/leno_types.h` | 声明 `c_layout_type_to_leno()` |
| `src/semantic/semantic_type.c` | cstruct 字段推断 4 处 + clib 返回类型 2 处使用 `c_layout_type_to_leno` |
| `src/semantic/visitinc/visit_expr.inc` | 3 处手动 `type_compatible` 添加 C 布局支持；清理 clib 参数冗余检查 |
| `src/semantic/visitinc/visit_module.inc` | 2 处手动 `type_compatible` 添加 C 布局支持；清理 clib 参数冗余检查 |

## 步骤

### 1. 新增辅助函数 `c_layout_type_to_leno` — `src/type.c` + `src/include/leno_types.h`

将 C 布局 TypeKind 映射为 Leno 等价 TypeKind：

```
i8/u8/i16/u16/i32/u32/i64/u64/c_int/c_uint/c_long/c_ulong/c_longlong/c_ulonglong/c_size/c_ssize → TYPE_INT
f32/f64 → TYPE_FLOAT
str8/str16 → TYPE_STRING
其他 → 原样返回
```

### 2. 修改 cstruct 字段类型推断 — `src/semantic/semantic_type.c`（核心修复）

4 处 `infer_expr_type` 中 cstruct 字段推断返回 C 布局原始 TypeKind 的地方，改用 `c_layout_type_to_leno` 映射：

- ~L1760: `result = type_copy(cstruct_def_sym->struct_field_types[i])` → 转换 result->kind
- ~L1775: `result = type_copy(var_sym->struct_field_types[i])` → 转换 result->kind
- L1789: `result = type_new(cdef->fields[idx].type)` → `type_new(c_layout_type_to_leno(cdef->fields[idx].type))`
- L1801: `result = type_new(ssym->fields[fi].type)` → `type_new(c_layout_type_to_leno(ssym->fields[fi].type))`

**注意**：2a/2b 的 `type_copy` 返回完整 TypeInfo，对 Ptr[T] 等 element_type 也要递归转换。

### 3. 修改 clib 返回类型推断 — `src/semantic/semantic_type.c`

2 处 clib 返回类型推断的手动 switch-case（仅覆盖 TYPE_I8~TYPE_U64），改用 `c_layout_type_to_leno`（同时覆盖平台相关类型 TYPE_C_INT 等）：

- ~L74-79: AST_MODULE_CALL clib 缓存命中
- ~L1827-1846: AST_FIELD_ACCESS clib 返回类型

### 4. 补充 `type_is_compatible` 遗漏规则 — `src/type.c`

在 `return 0` 之前添加：

```c
// cstruct ↔ Ptr 交叉兼容（cstruct 实例可传给期望 Ptr 的 clib 参数）
if (target->kind == TYPE_PTR && source->kind == TYPE_CSTRUCT) return 1;
if (target->kind == TYPE_PTR_GENERIC && source->kind == TYPE_CSTRUCT) return 1;
if (target->kind == TYPE_CSTRUCT && source->kind == TYPE_PTR) return 1;
// null 可赋值给 Ptr/str8/str16/cstruct（指针可为 NULL）
if (target->kind == TYPE_PTR && source->kind == TYPE_NULL) return 1;
if ((target->kind == TYPE_STR8 || target->kind == TYPE_STR16) && source->kind == TYPE_NULL) return 1;
if (target->kind == TYPE_CSTRUCT && source->kind == TYPE_NULL) return 1;
```

### 5. 给 5 处手动 `bool type_compatible` 检查块添加 C 布局支持

在每处 `// 允许 Ptr[T] <-> Ptr` 块之后添加：

```c
// C 布局类型隐式转换
if ((expected_param_type == TYPE_INT && arg_type->kind >= TYPE_I8 && arg_type->kind <= TYPE_C_SSIZE) ||
    (arg_type->kind == TYPE_INT && expected_param_type >= TYPE_I8 && expected_param_type <= TYPE_C_SSIZE))
    type_compatible = true;
if ((expected_param_type == TYPE_FLOAT && (arg_type->kind == TYPE_F32 || arg_type->kind == TYPE_F64)) ||
    (arg_type->kind == TYPE_FLOAT && (expected_param_type == TYPE_F32 || expected_param_type == TYPE_F64)))
    type_compatible = true;
if ((expected_param_type == TYPE_STRING && (arg_type->kind == TYPE_STR8 || arg_type->kind == TYPE_STR16)) ||
    (arg_type->kind == TYPE_STRING && (expected_param_type == TYPE_STR8 || expected_param_type == TYPE_STR16)))
    type_compatible = true;
```

5 处位置：
- `visit_expr.inc:460` — 内置实例方法参数检查
- `visit_expr.inc:550` — 内置方法参数检查（仅 TypeKind）
- `visit_expr.inc:845` — 内置方法参数检查
- `visit_module.inc:563` — 模块级内置方法参数检查
- `visit_module.inc:1282` — 模块级内置方法参数检查（仅 TypeKind）

### 6. 清理 clib 参数检查中的冗余手动异常 — `visit_expr.inc` + `visit_module.inc`

步骤 4 将遗漏规则补充到 `type_is_compatible` 后，clib 参数检查中 `type_is_compatible` 返回 false 时的手动 `!(...)` 条件块可以简化——删除已被 `type_is_compatible` 覆盖的条件，只保留 `type_is_compatible` 仍不覆盖的（步骤 4 添加后应该全部覆盖了，可以大幅简化）。

位置：
- `visit_expr.inc:236-258`
- `visit_module.inc:701-724`

### 7. 更新类型转换提示 — `src/semantic/semantic_type_utils.c`

删除过时的 "i32 转 int 请使用 as int" 等提示（C 布局→Leno 现在是隐式的）。保留 float→int 的显式转换提示。

## 验证

1. 编译：`build.bat`
2. 运行全部 138 个断言测试：`build\leno.exe assert\run_tests.leno build\leno.exe assert`
3. 创建专项测试：cstruct i32/u8 等字段赋值给 int 变量、作为函数参数、赋值给 struct int 字段——不需要 `as int`
4. 验证 float→int 仍需显式 `as`
