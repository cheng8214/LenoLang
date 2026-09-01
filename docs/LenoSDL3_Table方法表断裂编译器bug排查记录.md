# LenoSDL3 Table 方法表断裂编译器 bug 排查记录

## 背景

`leno_module/LenoSDL3` 的 `sdl_table.leno` 在提交 `2c86ea7b`（fix(SDL3): 修复 Table 滚动裁剪与表头排序问题）中新增了排序禁用 / 自动数值排序功能后，编译 `examples/应用示例/文件管理器/file_manager.leno` 报错：

```
file_manager.leno(1118,15): error: [语义错误] 类型 'struct Table' 没有方法 'on_dbl_click'
file_manager.leno(1154,22): error: [语义错误] 类型 'struct Table' 没有方法 'contains'
```

但这两个方法**确实存在于** `sdl_table.leno` 中（`on_dbl_click`、`contains` 均为 `struct Table` 内方法），且 `file_manager.leno` 调用的其他 Table 方法（`on_select`、`set_editable` 等）均不报错。

## 结论（根因）

**【2026-09-01 更新：编译器已修复，真实根因如下】**

**编译器 bug：模块符号表文本扫描器（`src/module_symbol_table/inc/scan/scan_struct.inc`）使用固定栈数组 `methods[MOD_MAX_METHODS]` 收集 struct 方法，上限 128 个。`struct Table` 共有 149 个方法，超出 128 的方法被静默丢弃，导致跨模块调用 `contains`（第 132 个）、`on_dbl_click`（第 130 个）时报"没有方法"。**

最初"`_to_float` 之后插入导致方法表断裂"是误判，实际机制：

- 基线 `a1ff6efb` 有 143 个方法，`contains`@125、`on_dbl_click`@123，均在 128 以内 → 编译通过
- `2c86ea7b` 在两者之前插入 6 个新方法，索引变为 131/129，**被 128 上限截断** → 编译失败
- 把新方法挪到 struct **末尾**：`contains`/`on_dbl_click` 索引不变（仍 <128），被丢弃的变成末尾的新方法（file_manager 未直接调用它们）→ 编译通过
- 这解释了所有"与内容无关、与位置强相关"的实验现象：本质是**方法在 struct 内的序号**是否 ≥128

- 与新增方法的**内容无关**：极简方法体、改名、改返回类型、删注释、去掉双引号全部无效
- 与**插入位置**强相关：方法放在 `_to_float` 之后 → 失败；同一批方法放到 struct **末尾** → 编译通过
- `2c86ea7b` 完整版因此同时丢了 `on_dbl_click` 与 `contains`；仅复现前部新增时先丢 `contains`（定义更靠后）

## 排查过程（排除法）

1. **基线验证**：把 `sdl_table.leno` 恢复成上一个版本 `a1ff6efb`（即删掉 `2c86ea7b` 全部新增）→ 编译通过
2. **按新增块二分**（`2c86ea7b` 仅改动 `sdl_table.leno` 一个文件）：
   - 字段 `_sortDisabledCols` + 配置读取 + `_is_col_sort_disabled`（A/B/C）→ 通过
   - 加入 `_is_number_str` / `_should_numeric_sort` / `sort_by` 改动（D）→ **失败**（`contains` 缺失）
3. **细分 D 块**，逐一排除：
   - 只留 `_is_number_str` → 通过
   - `_is_number_str` + `_should_numeric_sort`（任意简单体）→ 失败
   - 删除可疑注释（含半角引号 `"9" > "89"`）→ 仍失败
   - `if s == ""` 改为 `if s.len() == 0` → 仍失败
   - 去掉 `try/catch`、去掉 `_float(s)` 调用、方法改名为 `_xyz_test`、返回类型改 void → 全部仍失败
4. **关键实验**：在干净 `a1ff6efb` 上：
   - 只加 D 块 → **通过**（说明 D 不独立触发）
   - 加 A + D → 通过
   - 加 A + C + D → 失败（**定位到 C 与 D 组合**）
   - 加 A + C + `_is_number_str` → 通过
   - 加 A + C + `_xyz_test` → 通过
   - 加 A + C + `_xyz_test` + `_is_number_str` → 失败
   - **把 `_xyz_test` + `_is_number_str` 移到 struct 末尾 → 通过** ✅（决定性证据）

## 绕过方案

新增的 struct 方法**统一放在 struct 定义的最末尾**（最后一个方法之后、闭合 `}` 之前），避免在 `_to_float`（以及可能的其他 try/catch 方法）之后插入。

已用该方法实测通过：`2c86ea7b` 的排序禁用 / 自动数值排序 / bodyClip 裁剪功能全部保留，且 `file_manager.leno` 编译成功。

## 相关提交

| Commit | 说明 |
| --- | --- |
| `a1ff6efb` | 上一个可用版本，编译通过（无排序禁用功能） |
| `2c86ea7b` | Table 修复版，触发本 bug（编译失败） |

## 建议

- ~~此 bug 位于编译器方法表收集逻辑，建议后续修复编译器~~ **已修复**（见下）
- ~~在修复编译器前，新写 `struct` 方法时若遇到"某方法明明存在却报没有该方法"，先尝试把方法挪到 struct 末尾验证是否为同一问题~~ 绕过方案不再需要

## 修复记录（2026-09-01）

| 修改文件 | 内容 |
| --- | --- |
| `src/module_symbol_table/inc/scan/scan_struct.inc` | struct 的 `fields`/`methods` 由固定栈数组（128 上限）改为按需翻倍增长的堆数组，去掉静默丢弃守卫与死代码报错 |
| `src/module_symbol_table/inc/scan/scan_face.inc` | face 的 `methods` 同样改为动态增长数组 |
| `src/module_symbol_table/inc/scan/scan_cstruct.inc` | cstruct 的 `fields` 动态化；删除从未使用的 `methods` 栈数组 |
| `src/module_symbol_table/module_symbol_table.c` | 删除不再使用的 `MOD_MAX_FIELDS`/`MOD_MAX_METHODS` 宏 |
| `src/module_symbol_table/inc/sym_table_cache.inc` | `LENOSYMC_VERSION` v15→v16：旧缓存中的符号表含被截断的方法列表，必须失效重建 |

验证：

- `file_manager.leno`（`2c86ea7b` 原始布局，方法在 `_to_float` 之后）编译通过，无需绕过方案
- 全量回归测试 256/256 通过

## 后续加固（2026-09-01，三层防御）

为根除"符号被静默截断"这一类 bug（而非仅修 Table 个案），实施了三层防御：

**第 1 层：数量型上限全部动态化**（消除截断源头）

| 上限 | 原值 | 修改文件 |
| --- | --- | --- |
| `MOD_MAX_TYPES`（struct/cstruct/face 名称收集，且超限时**整个扫描中断**） | 128 | `scan_pass1.inc` |
| `MOD_MAX_CLIBS` / `MOD_MAX_ENUMS` | 32 | `scan_pass1.inc` |
| `MOD_MAX_MEMBERS`（enum 成员） | 128 | `scan_enum.inc` |
| `MOD_MAX_CLIB_FUNCS`（clib 函数，主文件+子文件两处） | 512 | `scan_pass1.inc` |
| `MOD_MAX_ALIASES`（本地别名表） | 64 | `scan_pass2_init.inc`、`scan_alias.inc`、`sym_table_import_alias.inc` |
| `impl_names`（struct 实现的 face 列表） | 128/64 | `scan_struct.inc` |

统一使用 `module_symbol_table.c` 中新增的 `mod_names_push` / `mod_names_push_z` / `mod_alias_push` 辅助函数（按需翻倍增长）。
顺带修复：`clib_names`/`enum_names` 从未释放的内存泄漏；`scan_cleanup.inc` 现在释放全部数组。

**第 2 层：设计型上限改为响亮失败**（保留上限，但必须报错）

保留的语言设计型限制（防笔误、防病态代码，超限即报错）：
`MOD_MAX_NAMES`（64）、`MOD_MAX_TYPE_STR`（256）、`MOD_MAX_PARAMS`（64）、`MOD_MAX_TYPE_PARAMS`（16）、`MOD_MAX_GENERIC_RET`（16）。

其中 `MOD_MAX_PARAMS`（clib/cfunc 参数数）原本也是静默截断：超限时调用 `error_add(ERR_SEMANTIC, 行号, ...)` 报真正的编译错误（带位置，行号由新增的 `mod_source_line` 辅助函数计算），替换所有静默截断与 `fprintf(stderr)` 死代码（`scan_pass1.inc` 共 6 处，含 4 处 cfunc 参数循环条件中的上限）。

**第 3 层：扫描后自校验断言**（防未来复发，最关键）

`sym_table_scan.inc` 在扫描完成后，对每个 struct/face 符号重新在源码中定位其定义体，
统计体内第一层的 `func ` 关键字出现次数（跳过注释/字符串、排除方法体内的局部函数），与符号表 `method_count` 交叉验证。
不一致立即报编译错误并定位到定义处——即使将来再次出现截断类 bug，错误也会在**定义处**当次暴露，而不是在远处的调用点报"没有方法 X"。

**自校验的实战首秀**：上线当次编译即报告 `HBox`（定义于 `sdl_layout.leno:53`）"源码 16 个 func，符号表 15 个"。
排查确认是**误报**：`sdl_layout.leno:169` 存在方法体内的**局部函数** `func childMain`（Leno 支持 func 局部定义），
被计数器误计为方法（符号表不收集局部函数是正确行为）。
修复：计数时增加 `depth == 1` 约束，只统计 struct 体第一层的方法声明。
这恰好证明断言真实在工作——任何方法表不一致都会被当场抓住。

## 验证结果（2026-09-01，三层加固后）

1. 编译器 `build.bat` 重新编译通过
2. `file_manager.leno`（`2c86ea7b` 原始布局，含 149 个方法的 `struct Table`）编译通过，**无需任何绕过方案**
3. 全量回归测试 `assert/run_tests.leno`：**256/256 通过**
4. `LENOSYMC_VERSION` 同步升级 v16→v17（旧缓存可能含被截断的符号，需失效重建）

修改文件汇总（三层加固）：

| 文件 | 内容 |
| --- | --- |
| `src/module_symbol_table/module_symbol_table.c` | 新增 `mod_names_push`/`mod_names_push_z`/`mod_alias_push`/`mod_source_line` 动态数组辅助函数；删除全部数量型上限宏 |
| `src/module_symbol_table/inc/scan/scan_pass1.inc` | 类型/clib/enum 名称收集、clib 函数数组（主+子文件）动态化；cfunc 参数超限改响亮报错 |
| `src/module_symbol_table/inc/scan/scan_pass2_init.inc` | 本地别名表动态化；use 导入类型传导改动态追加 |
| `src/module_symbol_table/inc/scan/scan_alias.inc` | 别名写入改动态追加 |
| `src/module_symbol_table/inc/sym_table_import_alias.inc` | 签名改为传数组指针+容量，5 处依赖传导改动态追加 |
| `src/module_symbol_table/inc/scan/scan_enum.inc` | enum 成员数组动态化；表达式求值快照改动态分配 |
| `src/module_symbol_table/inc/scan/scan_struct.inc` | `impl_names` 动态化 |
| `src/module_symbol_table/inc/scan/scan_cleanup.inc` | 释放全部动态数组（顺带修复 clib/enum 名称数组泄漏） |
| `src/module_symbol_table/inc/sym_table_scan.inc` | 新增扫描后 struct/face 方法数自校验断言 |
| `src/module_symbol_table/inc/sym_table_cache.inc` | `LENOSYMC_VERSION` v16→v17 |
