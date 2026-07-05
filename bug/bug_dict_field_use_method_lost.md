# Bug: `export struct` 含 `Dict` 字段时，`use` 导入后方法全部丢失

## 环境
- 发现: 最新
- 复现: `examples/测试/repro_dict_field_use_*.leno`

## 现象

```leno
// A.leno
export struct A_Int  { int  val = 0; func ok(): int { return val } }
export struct B_Dict { Dict val = {}; func ok(): int { return 1 } }

// B.leno
import "A.leno" as m; use m.A_Int; use m.B_Dict
A_Int a = new A_Int(val=1); a.ok()  // ✅ 正常
B_Dict b = new B_Dict(val={}); b.ok()  // ❌ "没有方法 'ok'"
```

`Dict` 作为 `export struct` 字段时，通过 `use` 导入后该 struct 的**所有方法不可见**。`int`/`bool`/`string` 等基本类型正常。

## 影响

- `Dialog` 结构体（含 `Dict style` 字段）无法跨模块使用
- 所有含 `Dict` 字段的导出结构体受影响

## 根因

`module_symbol_table.c` 中模块符号表扫描器的字段跳过逻辑（struct 第1910行，cstruct 第2134行）在扫描字段时，遇到 `}` 就停止扫描。当字段默认值包含 `Dict val = {}` 时，`{}` 中的 `}` 被误认为是 struct 体的结束花括号，导致后续所有方法丢失。

原始代码：
```c
while (*after_struct && *after_struct != '\n' && *after_struct != '}' && *after_struct != ';') after_struct++;
```

## 修复

修改字段扫描逻辑，正确处理包含嵌套 `{}`/`[]` 的默认值：

1. 先扫描到 `=`（有默认值）或行尾/`;`/`}`（无默认值）
2. 遇到 `=` 时，进入默认值解析，用深度计数正确匹配嵌套的 `{}` 或 `[]`
3. 分号跳过放在 `if (=)` 块外面，确保无默认值的字段（如 `f32 x;`）也能正确跳过分号

修复位置：`src/module_symbol_table.c` 第1910行（struct）和第2134行（cstruct）

## 测试

- `test_dict_field_struct.leno` / `test_dict_field_struct_mod.leno` — 验证各种字段类型（int、Dict、Array、Dict+Array、多字段）的 struct use 导入后方法可调用
- `test_cstruct_export_array.leno` — 验证 cstruct 不受回归影响
- 150个测试全部通过

## 状态

- [x] 已修复
