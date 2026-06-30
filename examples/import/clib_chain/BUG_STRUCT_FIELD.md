# 跨模块 clib 类型已知问题清单

## 问题1: struct 字段 clib 类型跨模块丢失

**现象**: `use A.StructType` 后创建 struct，`struct.field` 是 `any` 而不是 `core_lib`

**文件**: test_multi.leno 第36行 `core_lib h = ctx.lib` → 期望 clib 实际 any

**根因**: module_symbol_table 扫描 struct 字段时，clib 类型未正确解析

---

## 问题2: wrapper 模式返回类型推断失败

**现象**: 三层 use 链中 `export func wrap(): core_lib` 返回 any

**文件**: test_wrapper.leno `core_lib c = sdl.wrapGetCore()` → any

**根因**: wrapper 模块的符号表扫描时，`core_lib` 未在 clib_names 中找到

---

## 问题3: `as` 运算符在表达式中的优先级

**现象**: `"str" + expr as int` 绑定顺序模糊

**绕过**: 使用中间变量 `var v = expr as int; "str" + v`
