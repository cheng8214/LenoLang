# 修复：alias 类型支持跨模块 use 导入

## Context

Bug 文档 `docs/bug_alias_cross_module.md` 描述：当模块 A 定义了 `export alias MySize = Dict[string, int]`，模块 B 通过 `use A.MySize` 导入时报错"没有 struct、clib 或 face 类型"。原因是 `use` 语句的处理逻辑只查找 struct/clib/enum/face，不查找 alias。

## 修复方案

### 缺陷 1：`use` 语句语义分析不识别 alias

**文件**: `src/semantic/visitinc/visit_module.inc`（AST_USE 处理，约 143-446 行）

在 face 查找之后、报错之前，增加 alias 查找逻辑：
- 调用 `module_symbol_table_find_alias` 查找模块符号表中的 alias
- 找到后注册为 `SYM_TYPE` 符号到当前作用域，设置 `sym->type = type_copy(alias_sym->type_info)`
- 同时将 alias 注册到解析器别名表（`add_alias`），让后续类型解析能识别

### 缺陷 2：模块符号表扫描器中 `use` 不传播 alias

**文件**: `src/module_symbol_table.c`（use 处理，约 1060-1224 行）

在 struct/face/clib/enum 查找之后，增加 alias 查找逻辑：
- 调用 `module_symbol_table_find_alias` 查找依赖模块的 alias
- 找到后调用 `module_symbol_table_add_alias` 将 alias 传播到当前模块符号表
- 这样当其他模块 import 当前模块时，也能看到这个 alias

## 关键修改文件

1. `src/semantic/visitinc/visit_module.inc` — AST_USE 处理增加 alias 查找
2. `src/module_symbol_table.c` — use 语句扫描器增加 alias 传播

## 验证

1. 编译通过：`build.bat`
2. 全部 124 测试通过
3. 创建测试用例验证 `use module.AliasName` 能正确导入 alias 类型
