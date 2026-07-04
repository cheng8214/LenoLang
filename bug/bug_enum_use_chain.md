# Bug: `use` 链式导入不传递（A ← B ← C）

## 环境
- 发现: `d94c4342`（enum 单层 use 已修复）

## 现象

三层模块链式 `use`，C → B → A 传递断裂：

```leno
// chain_c.leno — 定义 enum
export enum Scancode { ESCAPE = 41; SPACE = 44 }

// chain_b.leno — use 导入 c 的 enum
import "chain_c.leno" as c
use c.Scancode          // B 自己能用

// chain_a.leno — use 导入 b
import "chain_b.leno" as b
use b.Scancode          // ❌ "模块 'b' 中没有 struct、clib、face 或 alias 类型 'Scancode'"
```

所有通过 `use` 间接导入的 enum 不能被更上层再次 `use`。

## 根因

`module_symbol_table_scan_depth()` 中 `use` 传播逻辑对 enum 的处理有缺陷：

- **struct**: 找到后调用 `module_symbol_table_add_struct()` 将完整信息加入符号表 ✅
- **face**: 找到后调用 `module_symbol_table_add_face()` 将完整信息加入符号表 ✅
- **alias**: 找到后调用 `module_symbol_table_add_alias()` 将完整信息加入符号表 ✅
- **enum**: 只将名称添加到 `enum_names` 数组，**没有调用 `module_symbol_table_add_enum()`** ❌

因此上层模块 `use b.Scancode` 时，`module_symbol_table_find_enum(B的符号表, "Scancode")` 找不到。

## 修复

- 调用 `module_symbol_table_add_enum()` 将 enum 完整信息（成员名+成员值）传播到当前模块的符号表
- 添加去重检查 `!module_symbol_table_find_enum(table, type_name)`
- 错误消息补充 enum（原来只说 struct/clib/face/alias）

## 修改的文件

| 文件 | 修改内容 |
|------|---------|
| `src/module_symbol_table.c` | enum use 传播调用 `module_symbol_table_add_enum()` |
| `src/semantic/visitinc/visit_module.inc` | 错误消息补充 enum |

## 回归测试

- `assert/test_enum_use_chain_c.leno` — 最底层模块：定义 export enum
- `assert/test_enum_use_chain_b.leno` — 中间层：use c.Status
- `assert/test_enum_use_chain_a.leno` — 顶层：use b.Status（验证链式传递）

## 状态

- [x] 已修复
