# 类型可见性与 use 逃生阀修复追踪

> **最后更新**：2026-10-12（建档 + **US1 完成**）。
> **关联**：`docs/import使用指南.md`（§0.2 漏列 / §8.2 规则表）、
> `docs/语言摩擦与隐式契约清单.md` 的 **M9**、`docs/语言陷阱速查.md` 的 **㉝**。
> **本文定位**：不是"use 怎么用"（那是 `import使用指南`），而是**这套类型可见性设计缺的那一环**——
> 「**不用 `use` 也能写对**」的能力（逃生阀）。

***

## 0. 为什么开这张表（一次设计评估的落点）

评估结论：**机制选对了，规则的完备性差一环。**

对的部分（有证据，不是客套）：

- 「`import` 管值/模块命名空间（带别名）+ `use` 管类型命名空间（可扁平化）」与
  Java `import` / C# `using` / Rust `use` 同谱系，比 Go（不给扁平化）友好；
- `use m.(A, B)` **批量** + **支持尾逗号**（Java/C# 都没有）；
- `use` **只收类型、明确不收 func/var** ⇒ 规则边界干净（`import使用指南` §8.2 规则表）；
- 报错**自带处方**（`未定义的类型: Pt（它其实是模块 'm' 导出的 struct ⇒ 请补一句 use m.Pt）`）。

**真缺陷（本表要修的那一环）**：**限定名只在表达式位置合法**。

```leno
var p = new m.Pt()      // ✓ 表达式位置（parser_expr.c:732 专门支持）
m.Pt p = m.mk()         // ✗ 类型位置（parser_func.c:662 / parser.c:225 明确拒绝）
```

这一个不对称，把 `use` 从「便利」变成了「**义务**」，并派生三笔次生负担：

| # | 次生负担 | 证据 |
|---|---|---|
| ① | **§0.2 那类 bug**：`use` 必须列全本文件真正用到的类型，漏一个**报错点在 `use` 那一行**，与缺的类型毫无关系；且类型名只出现在 `use` 里 ⇒ **静态检查 / lint 抓不到，必须真编译** | `import使用指南` §0.2 |
| ② | **传导型类型**：中间层为了把类型传下去，得 `use` 一个自己根本不用的类型 ⇒ 同一行 `use` 承载两种意图（自用 / 转出口），**语法上无法区分**（Rust 为此专门有 `pub use`） | `import使用指南` §8.3 链式传导 |
| ③ | **没有通配兜底**：上面两条没有退路（`parse_use_stmt` 无 `*` 分支 ✓ 实测） | 本文 §1-⑨ |

**主流对照（凡是给扁平化的语言，都留了至少一个逃生阀）**：

| 语言 | 逃生阀 |
|---|---|
| Java / C# | `import java.util.*` / `using System.Collections.Generic`（**通配**） |
| Rust | `std::collections::HashMap` 在**类型位置合法** + `use x::{a,b}` |
| Go | 压根不扁平化 ⇒ 全限定（没有"必须 import 类型"这回事） |
| **Leno（现状）** | **两个都没有** ⇒ 唯一的路是"显式列全" |

⇒ 本表的目标：**把"不用 `use` 也能写对"补上**（US1），其余条目围绕它收口。

***

## 1. 现状事实（全部核对过，不靠记忆）

| # | 事实 | 证据位置 | 结论 |
|---|---|---|---|
| ① | **类型位置拒绝是有意设计，不是漏做** | `src/parser/parser_func.c:662` `parser_reject_module_qualified_type()`，由 `parse_type_internal` 收尾调用（`:641`）；文案：`不支持带模块前缀的类型名（如 'a.Point'）：请先 use a.Point，然后直接写 'Point'` | 改它是**改设计决定**，不是补 bug ⇒ 必须把"为什么原来拒"写下来（见 US1 §方案） |
| ② | **声明位置另有一处拒绝**（`Module.Type varName = …`） | `src/parser/parser.c:225-231`（注释自称 `Leno 不支持此写法`，报友好错误避免级联误导） | US1 要动**两处**，只改一处会"参数位置能过、声明位置报错" |
| ③ | **表达式位置早就支持**（这是不对称的来源） | `src/parser/parser_expr.c:732`（`new module.StructName(...)`；`:41` 的 `TOK_DOT → parse_dot`） | 表达式的解析/查名链路**可复用**（M8/M9 已证实能做到"指到真因"） |
| ④ | **类型解析有单一入口** | `parse_type_internal()`（`parser_func.c`）；对外 `parse_type()` 23 个调用点，parser 内部 `parse_type_internal` 16 处 | 解析侧改动面**收敛在一个函数**（加"点号 + 模块名"分支即可） |
| ⑤ | **类型名归一化有单一入口（本表最关键的一条）** | `src/semantic/semantic_visit_func.c` 的 `resolve_alias_in_type(s, TypeInfo**, line)` —— **递归**覆盖 `generic_args` / `element_type` / `key_type` / `value_type` / `return_type` / `param_types`；调用点覆盖 **var 声明 / 函数形参与返回 / struct 字段 / destruct 槽** | ⇒ **在这里做限定名解析，下游那些"未定义类型"校验点一行都不用改** ✓（含泛型实参，天然一起解决） |
| ⑥ | **`TypeInfo` 没有模块限定字段** | `src/include/leno_types.h:285-308`（只有 `struct_name` / `type_param_name` / `constraint_name` / `generic_args` …） | 解析侧要把"模块前缀"带下来 ⇒ 需要新字段（或把 `a.Point` 塞进 `struct_name`，见 US1 的两个备选） |
| ⑦ | **`use` 只收 6 类类型** | `import使用指南` §8.2 规则表：`struct` / `cstruct` / `clib` / `face` / `enum` / `alias`；`use module.func` / `use module.var` ❌ | 边界正确，**不要动**（见 §4 明确不做） |
| ⑧ | **native 类型规格全局可见**（共 **5** 个） | `native_register_struct_spec()` 调用点：`DirEntry`（`dirs.walk`）/ `DirInfo`（`dirs.stat`）/ `RegexMatch`（`regexs.find_all`）/ `ExecResult`（`_exec`）/ `DictEntry`（`dicts.items`）；兜底在 `src/semantic/semantic_type.c:553` | 实践无害，但它是**第三套可见性规则**，用户**看不见分界线**（见 US5） |
| ⑨ | **没有通配 `use m.*`** | `parse_use_stmt()`（`parser_module.c`）无 `TOK_STAR` 分支；全仓 `TOK_STAR` 只出现在乘法/`*=` | 上面①②的负担因此没有退路 |
| ⑩ | **没有"use 了没用"的诊断** | 全仓搜 `未使用` 无相关检查 | ⇒ 传导型 `use`（②）**无法与自用 `use` 区分**，也没有 lint 兜 |
| ⑪ | **"未定义类型"的处方是共享实现** | `semantic_undefined_type_hint()`（2026-10-10 收敛：原本**八处**拷贝各自 if/else，现共用）；调用点含 `visit_var.inc`、`visit_type_def.inc`、`check_undefined_type` 等 | US1 的新错误文案**必须走它**，否则又开一处拷贝 ✓ |

***

## 2. 台账

| ID | 优先级 | 事项 | 判据（做完怎么算对） | 成本 | 风险 | 状态 |
|---|---|---|---|---|---|---|
| **US1** | **P0** | ~~**类型位置支持模块限定名 `m.Pt`**（`m.Pt p` / `func f(m.Pt x)` / `: m.Pt` / 字段 / `Array[m.Pt]` / `is` / `as` / `new` 全位置）~~ ⇒ **已修**（2026-10-12，工作区未提交）：解析器记前缀 + 语义层**复用 `use` 的唯一实现**做隐式 use | 已完成 ✓（10 位置矩阵用例 + 3 条负例 + 模块模式） | — | — | ✅ |
| **US2** | P1 | **报错定位**：漏 `use`（或用 `use m.(A,B)` 漏了 C）时，报错**指到真正使用它的那一行**，而不是 `use` 那一行 | 复现 §0.2 场景：报错行号 = 使用处 ✓ 且仍带 `use m.C` 的处方 ✓ | 低–中（先要探明报错到底在哪一步产生） | 低（只改诊断，不改语义） | ⬜ |
| **US3** | 待定 | `use m.*` **通配**逃生阀 | 加则要有"与 `use m.C` 同名如何取舍"的明确规则 | 低（parser 一个分支） | 与"**显式优先 / 少报不误报**"的既有风格冲突 | ⏸ **US1 完成后重估**（US1 落地则本条很可能降级为"不做"） |
| **US4** | P2 | **传导型 `use` 正式化**：区分"自用 use" vs "转出口 use"（`export use`?），或至少把它写成一条**正式纪律** | 中间层为了传递而 `use` 一个自己不用的类型时，**意图是可表达的**（或文档里明确命名这条纪律） | 低（文档）/ 中（真做 `export use`） | 低 | ⬜ |
| **US5** | P2 | **概念收敛**：把 native 规格并进"**编译器内置类型**"（与 `int` / `DirEntry` 同性质），或至少在文档里写明这条分界 | 文档里"哪些类型不用 use"只剩**两类**（内置类型 / 源码模块类型），不是一个悬空的第三类 | 低（文档） | 低 | ⬜ |

***

## 3. US1 详述（P0）—— ✅ 已修（2026-10-12，工作区未提交）

### 落地（改了哪 8 处）

| 文件 | 改动 |
|---|---|
| `src/include/leno_types.h` | `TypeInfo` 新增 `module_qualifier`（**只解析阶段有意义**；**不参与 `type_equals`** ✓） |
| `src/type.c` | `type_free` / `type_copy` 带上该字段；`type_equals` 处写明**为什么不比较** ✓ |
| `src/parser/parser_func.c` | `parse_base_type` 认 `IDENT . IDENT`（**两 token 都确认才消费** ✓）；限定名一律先记 TYPE_STRUCT（`face_def_find` 会猜歪 ✗）；`parser_reject_module_qualified_type` 降级为**多级限定**兜底 |
| `src/parser/parser.c` | 声明处的 `qualified_decl` 从"报错"改成"走普通声明路径"；前瞻**多吞一段 `.` 链** ⇒ 多级限定能被当成类型位置、报出正确那句 ✓ |
| `src/semantic/semantic_visit_ast.c` | **把 `case AST_USE` 的 390 行分派体抽成 `semantic_import_use_symbol()`**（逐字搬 + 定点变换：13 处 case 级 `break` → `return`） |
| `src/semantic/semantic_visit_func.c` | `resolve_alias_in_type` 新增限定名分支：**隐式 use** + 成功后清空前缀（幂等 ✓）+ 失败毒化成 `any`（抑噪 ✓） |
| `src/semantic/semantic.c` + `src/include/leno_semantic.h` | 新增 `in_type_prepass` 阶段标志（预归一化阶段跳过） |
| 用例/文档 | 新增 `assert/test_qualified_type_positions.leno`（10 位置矩阵）+ `assert/qualified_type_mod.leno`；`test_qualified_type_hint.leno` **整体重写**（4 条负例翻案 + 新增多级/未导入/无此类型 3 条负例）；`test_diag_static_errors.leno` 的 M9 三条翻案；`test_diag_error_batteries.leno` 的声明处判据翻案（并删除负例 fixture）；3 篇文档同步 |

### ⚠ 实施中踩到的两个坑（都很隐蔽，值得记住）

1. **预注册阶段早于 `import`** —— 语义分析开头会先把所有函数签名归一化一遍，那一遍**跑在 `AST_IMPORT` / `AST_USE` 之前** ⇒ 那时模块别名还没登记，动手只会得到假错误「模块 'm' 未导入」（实测一个探针里 3 个位置全报）。
   修法：加显式阶段标志 `in_type_prepass`，那一遍**原样留着**，主阶段再解析 ✓
   （**不猜**"是不是预注册"—— 猜错就是假报错或漏报 ✓）。
2. **两个"静默错"**（都靠套件才暴露，光看输出看不出来）：
   - **抽取时把 native 成功出口改成了失败**：原 case 里那一句 `break;` 位于 `if (!semantic_import_native_type(…))` **之外**（= 成功出口 ✓），我按行号映射时当成失败分支 ⇒ **所有 native 限定名（`d.DirEntry`）静默变成 `any`** ✗。
     为什么难发现：局部变量 / 形参 / 顶层变量的 `any` 是**合法**的 ⇒ 那几格"看起来正常"，只有 **struct 字段**位置报「字段不能使用 any」才把它顶出来 ✓。
     ⇒ 教训：**搬代码必须按语法层级看，不能只按行号映射** ✓。
   - **`semantic_init` 不是整体 memset**（本仓注释里专门警告过 ✓）：新加的 `in_type_prepass` 漏了显式清零 ⇒ **模块模式**下读到栈上垃圾（非零）⇒ 归一化整段被跳过 ⇒ 模块里写 `m.Pt` 报"未定义的类型" ✗（入口脚本模式恰好是 0，所以只有模块模式红 ✓）。

### 症状

```leno
import "./_mod.leno" as m
m.Pt p = m.mk()          // ✗ error: 不支持带模块前缀的类型名（如 'a.Point'）：请先 use a.Point…
func show(m.Pt x) { }    // ✗ 同上（parse_type_internal 收尾拒绝）
var q = new m.Pt()       // ✓ 表达式位置一直合法
```

⇒ 用户被迫给**每个用到类型的文件**都写 `use`，且必须**列全**（§0.2）；库的中间层还得替下游 `use`（§8.3）。

### 根因

- **类型位置**：`parse_type_internal` 只吃一个标识符，收尾调
  `parser_reject_module_qualified_type()` **主动报错**（`parser_func.c:662`）。
  注释交代了动机（`parser_func.c:653-661` 的 2026-09-16 实测）：
  **不支持时如果不管，会静默 misparse** —— `if v is a.Point {` 报"if 语句体必须用大括号"、
  `v as a.Point` **语法通过、运行期才炸**。⇒ 当年的选择是"友好拒绝"而不是"支持" ✓ **这个决定本身是对的**
  （比静默错好），本表要做的只是**把"拒绝"升级成"支持"**。
- **声明位置**：`parser.c:225-231` 同款友好拒绝。
- **表达式位置**：`parser_expr.c:732` 已支持 ⇒ 三处口径不一致才是问题的本质。

### 可用的地基（为什么现在成本可控）

1. **归一化单一入口**：`resolve_alias_in_type` 是**递归**的，且已被**所有**类型位置调用
   （var 声明 / 函数形参与返回 / struct 字段 / destruct 槽 ✓ 见 §1-⑤）
   ⇒ 在它里面做"限定名 → 本地类型名"的解析，**下游校验点（`check_undefined_type`、
   `semantic_check_undefined_subtypes`、`visit_var.inc`、`visit_type_def.inc`）全都不用动** ✓。
2. **解析单一入口**：`parse_type_internal` 一处加分支即可覆盖全部类型位置 ✓。
3. **查名链路已存在**：表达式位置早就靠模块符号表查 `m.X`（M8/M9 的报错即由它产出）
   ⇒ 复用同一条链路，不另造一套 ✓。
4. **诊断有共享实现**：`semantic_undefined_type_hint` ✓（新文案走它，不再开拷贝 ✓）。

### 方案（两步，都在"入口"上）

1. **解析侧**（`parser_func.c` + `parser.c`）
   - `parse_type_internal`：识别 `IDENT . IDENT` ⇒ 把模块前缀与类型名一起存进 `TypeInfo`
     （备选 A：新增 `char* module_name` 字段；备选 B：`struct_name` 存 `"m.Pt"`）。
     推荐 **A**：语义清楚、`type_to_string` 好写、不会与真正的嵌套名混淆。
   - 撤掉/改写 `parser_reject_module_qualified_type`：**保留**它作为"其余非法点号"的兜底
     （如 `m.Pt.Sub`），但不再拦 `IDENT.IDENT`。
   - `parser.c:225-231` 的声明路径：从"报错"改为"交给类型解析"。
2. **语义侧**（`resolve_alias_in_type`）
   - 遇到带模块前缀的类型 ⇒ 在该模块**已导入**的前提下，去它的符号表查导出类型：
     - 命中 ⇒ 就地替换成**不带前缀**的 `TypeInfo`（后续流程完全不知情 ✓）；
     - 未 import / 未导出 / 名字错 ⇒ 走 `semantic_undefined_type_hint` 同口径处方 ✓。
   - ⚠ 替换后**不要**留着 `module_name`（否则 `type_equal` 会把 `m.Pt` 与 `Pt` 判成不同类型 ✗）。

### 实施前必须先回答（别猜）

1. `TypeInfo` 加字段的**连带面**：`type_copy` / `type_free` / `type_equal` / `type_to_string`
   全要跟着改（漏一个就是泄漏或误判）—— 先数清这四处。
2. **`is` / `as` / `case is` 的解析**是不是也走 `parse_type`？（§1-① 的实测里它们曾经静默 misparse
   ⇒ 必须确认它们现在都经过 `parse_type_internal`，否则 US1 覆盖不全、又留一个位置不一致 ✗）
3. **前瞻冲突**（唯一的真语法风险）：语句开头的 `IDENT . IDENT` 现在是**表达式语句**
   （`m.func()`）⇒ 改成"也可能是类型声明"后，要看 `parser.c:225` 那段的前瞻够不够，
   会不会把 `m.func()` 误判成类型声明。
4. **模块解析的时机**：`resolve_alias_in_type` 跑的时候，模块符号表**一定已经加载**了吗？
   （`import` 是否总是先于类型解析？若不是，"限定名"就得两遍解析或推迟到查名阶段。）

### 验收判据（**按位置矩阵钉全**，这是 US1 的核心风险）

一份用例覆盖**每一个**类型位置，每处一条正例 + 一条"未 import 的处方"：

| # | 位置 | 正例 |
|---|---|---|
| 1 | var 声明 | `m.Pt p = m.mk()` |
| 2 | 函数形参 | `func f(m.Pt x)` |
| 3 | 返回类型 | `func g(): m.Pt` |
| 4 | struct 字段 | `struct S { m.Pt p }` |
| 5 | 泛型实参 | `Array[m.Pt]`、`Dict[string, m.Pt]`、`m.Box[m.Pt]` |
| 6 | `is` / `case is` | `if v is m.Pt => q { }` |
| 7 | `as` | `var w = v as m.Pt` |
| 8 | `new` | `new m.Pt()`（**已合法 ✓ 回归不许破**） |
| 9 | catch / destruct 槽 | 各自一条（若支持） |
| 10 | 负例 | `m.Nope p`（未导出）⇒ 处方口径与 `use` 路线**一致** |

- 套件：`.\\build\\leno.exe assert\\run_tests.leno build\\leno.exe assert` 全绿 ✓
- 老判据翻案：`assert/test_diag_static_errors.leno` 里钉"不支持带模块前缀的类型名"的断言要**改**（M9 刚加的反例同理）✓

### 验收（实测，2026-10-12）

| # | 位置 | 用例 | 结果 |
|---|---|---|---|
| 1 | var 声明 | `m.Pt p = m.mk(1,2)` | ✓ |
| 2 | 形参 | `func area_of(m.Pt p): int` | ✓ |
| 3 | 返回 | `func origin(): m.Pt` | ✓ |
| 4 | struct 字段 | `struct Holder { m.Pt p  m.Shape s }` | ✓（含 face 字段） |
| 5 | 泛型实参 | `Array[m.Pt]` / `Dict[string,m.Pt]` / `new m.Box[m.Pt]()` | ✓ |
| 6 | `is` / `case is` / 守卫 | 三种都跑通 | ✓ |
| 7 | `as` | `v as m.Pt` | ✓ |
| 8 | `new m.Pt()` | 回归（本来就合法） | ✓ |
| 9 | cstruct / enum / alias | `m.Raw.malloc()` / `m.Color.RED` / `m.Size` | ✓（三条不同注册通道） |
| 10 | 模块函数**语句级**调用 | `m.hello()`（前瞻不许误判成声明） | ✓ |
| 11 | **顶层全局变量**的限定类型 | `m.Helper h = new m.Helper()` | ✓（`test_diag_error_batteries` 钉） |
| 12 | **模块模式**（`semantic_analyze_module`） | 模块内部用 `m.Pt` 做字段/形参/返回 | ✓ |
| — | native 类型（`dirs` 的 `DirEntry`） | 与源码模块**不是同一条通道**，单独钉 ✓ 且**真类型**（字段可访问、错字段会报） | ✓ |
| — | 负例 | 多级限定 / 模块未导入 / 模块里无此类型 | ✓ 三条文案都点名真因 |
| — | 套件 | `run_tests.leno` | **448 passed / 0 failed** |

### 顺带收益

- §0.2 那类 bug（漏列 ⇒ 报错点错位）**从"难查"变成"不会发生"** ✓
- 传导型 `use`（§1-②）**不再需要**（下游直接写 `m.Pt`）✓
- US3（通配）的必要性大幅下降 ⇒ 可以把"通配"与"显式"的风格之争长期搁置 ✓

***

## 4. 明确不做（想清楚了，不再反复）

| 不做 | 为什么 |
|---|---|
| **给 `use` 加 func / var** | 运行期实体走模块名（`m.func()` / `m.var`）是**正确的边界**（否则名字冲突、初始化顺序都成问题）✓ 见 `import使用指南` §8.2 |
| **反向收紧**（要求所有类型都 `use`，连 native 规格也强制） | 与 §1-⑧ 的既有兜底冲突，且**增加**摩擦、零收益 ✗ |
| **限制 `new m.Pt()` 这类表达式位置写法** | 它是**正确的**那一半 ✓ 要补的是类型位置，不是砍表达式位置 |
| **立刻做 `use m.*` 通配** | 与"显式优先 / 少报不误报"风格冲突；US1 落地后**可能完全不需要** ⇒ 留到 US3 重估 |
| **在解析器里做类型名解析**（拿模块符号表） | 解析器不该知道模块加载状态 ⇒ 归一化放语义侧 `resolve_alias_in_type`（§1-⑤ 的单一入口） |

***

## 5. 变更记录

| 日期 | 条目 | 动作 |
|---|---|---|
| 2026-10-12 | — | 建档；快照：设计评估结论（机制对 / 缺逃生阀）、现状事实 ⑪ 条、US1–US5 立项、"明确不做"五项 |
| 2026-10-12 | US1 | **已完成（工作区未提交）**：解析器记模块前缀 + 语义层**复用 `use` 的唯一实现**（抽出 `semantic_import_use_symbol`）+ 新用例 `test_qualified_type_positions.leno`（10 位置矩阵）/ `test_qualified_type_hint.leno`（重写）/ `test_diag_*`（M9 三条翻案）+ 3 篇文档同步；套件 **448/0**。⚠ 顺带查实两个隐蔽坑：**预注册阶段早于 import**（要显式阶段标志）、**`semantic_init` 不是 memset**（新字段漏清 ⇒ 模块模式读到栈垃圾） |
