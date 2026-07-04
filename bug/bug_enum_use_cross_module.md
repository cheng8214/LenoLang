# Bug: `use` 导入跨模块 enum 存在问题

## 环境
- 发现: 最新 main（enum 已实现）
- 修复: `d94c4342` — `fix: use导入跨模块enum三个Bug全部修复 + alias支持enum成员值`

## 状态
- [x] 已修复 — `d94c4342`

## 测试文件
- `assert/test_enum_use_alias.leno` — enum 值别名 + 成员访问回归测试
- `assert/test_enum_use_alias_mod.leno` — 跨模块 enum 测试模块

## Bug 1: `use` 导入 enum 后显式值丢失 ✅ 已修复

### 复现
```leno
// A.leno
export enum Scancode { ESCAPE = 41; SPACE = 44; A = 4; B = 5 }

// B.leno
import "A.leno" as core
use core.Scancode

main() {
    print(Scancode.ESCAPE)  // 输出 0，期望 41
    print(Scancode.SPACE)   // 输出 1，期望 44
}
```

### 根因
1. `module_symbol_table_add_enum()` 函数签名只接受 `char** member_names`，不接受 `int* member_values`，硬编码 `en->member_values[i] = i`
2. `module_symbol_table.c` 中 enum 扫描代码只提取成员名，跳过 `= value` 部分

### 修复
- 修改 `module_symbol_table_add_enum` 签名，新增 `int* member_values` 参数，`-1` 表示无显式值
- 修改 enum 扫描代码，提取 `= value` 赋值（支持十进制、负数、十六进制 `0x`）
- 添加 `;` 作为 enum 成员分隔符

## Bug 2: `use` 导入 enum 运行时崩溃 ✅ 已修复

### 复现
```leno
// color_module.leno
export enum Color { red; green; blue }

// test.leno
import "color_module.leno" as cm
use cm.Color

main() {
    print(Color.red)  // 崩溃: 0xC0000005 Access Violation
}
```

### 根因
AST union 布局冲突：`AST_NUM.bigint_str`（offset 16）与 `AST_MODULE_ACCESS.ref.kind + ref.index` 重叠。将 `AST_MODULE_ACCESS` 原地转换为 `AST_NUM` 时，`ref.kind/index` 中的非零值被 `ast_free()` 误认为 `bigint_str` 指针而调用 `free()`。

### 修复
- 转换时显式设置 `ast->u.num.bigint_str = NULL`
- 释放 enum 路径中不再使用的 `var_ast`
- 在 `scope_free()` 中释放 `enum_value_names` 和 `enum_values`

## Bug 3: `alias` 不支持 enum 成员做值 ✅ 已修复

### 复现
```leno
enum Signal { low = 0; mid = 5; high = 10 }
alias DefaultSignal = Signal.mid  // 语法错误: "期望表达式"
```

### 根因
`parse_alias_stmt` 在 `=` 后使用 `parse_type()` 解析右侧，只支持类型语法。`Signal.mid` 是枚举成员访问表达式，`parse_type` 解析 `Signal` 为 `TYPE_STRUCT` 后，`.mid` 留在 token stream 中，外层解析器遇到 `.`（无前缀解析器）报"期望表达式"。

### 修复
1. **解析器** (`parser_func.c`): `parse_alias_stmt` 在 `=` 后先检查 `IDENT DOT IDENT` 模式（值别名），匹配则构建 `AST_MODULE_ACCESS` 表达式存入 `ast->u.alias.expr`；不匹配则回退词法状态，走原有 `parse_type()` 路径（类型别名）
2. **AST** (`leno_ast.h`): `alias` 结构体新增 `Ast* expr`（值表达式）和 `SymRef ref`（符号引用）
3. **语义分析** (`visit_enum.inc`):
   - 值别名时对 `expr` 做 `visit()`，将 `AST_MODULE_ACCESS` 解析为 `AST_NUM` 常量
   - 注册为全局变量
   - 本地 enum 定义时也存储 `enum_value_names/values` 到 Symbol
   - 修改 `visit_module.inc` 中 enum 成员访问条件：从 `is_enum_type && kind == SYM_TYPE` 改为 `is_enum_type`，统一支持本地和跨模块 enum
4. **代码生成** (`codegen_stmt.c`): 值别名生成 `gen_expr(expr) + OP_DEFINE_GLOBAL/OP_SET_MODULE_VAR`
5. **AST 释放** (`ast.c`): 释放 `alias.expr` 和 `alias.ref.name`

### 附带优化
本地定义的 enum 成员访问（如 `Signal.mid`）现在也在语义分析阶段直接解析为 `AST_NUM` 常量，字节码从 `OP_GET_GLOBAL + OP_CONST "mid" + OP_INDEX` 优化为 `OP_CONST 5`。

## 修改的文件

| 文件 | 修改内容 |
|------|---------|
| `src/parser/parser_func.c` | `parse_alias_stmt` 支持 `IDENT.IDENT` 值别名模式 |
| `src/include/leno_ast.h` | `alias` 结构体新增 `expr` 和 `ref` 字段 |
| `src/ast.c` | 释放 `alias.expr` 和 `alias.ref.name` |
| `src/semantic/visitinc/visit_enum.inc` | 值别名语义分析 + enum 成员值存入 Symbol |
| `src/semantic/visitinc/visit_module.inc` | enum 成员访问条件放宽，统一支持本地/跨模块 |
| `src/codegen/codegen_stmt.c` | 值别名代码生成 + 修复 AST_USE fall-through |
| `src/module_symbol_table.c` | enum 扫描提取 `= value` + `module_symbol_table_add_enum` 接受 values |
| `src/include/module_symbol_table.h` | 函数签名变更 |
| `src/scope.c` | 释放 `enum_value_names/values` |

## 总结

| Bug | 严重度 | 状态 |
|-----|--------|------|
| `use` enum 值丢失 | 🔴 致命 | ✅ 已修复 |
| `use` enum 崩溃 | 🔴 致命 | ✅ 已修复 |
| `alias = enum.member` | 🟡 次要 | ✅ 已修复 |
