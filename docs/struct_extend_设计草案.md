# `extend` —— 把 struct 的方法写到别的文件（设计草案）

> 2026-11-02 起草。目标读者：以后要拆大类、或要改 parser/semantic 的人。

## 0. 实现状态（2026-11-02）

**一期已完成并通过测试**：同文件 `extend`（含定义顺序无关、多块、跨块访问 `pri` 成员）。

- 改动：`lexer.c`（关键字）、`leno_types.h`（TOK_EXTEND）、`leno_ast.h`（AST_EXTEND_DEF +
  `u.extend_def`）、`parser_func.c`（`parse_extend_stmt`）、`parser.c`（分发）、
  `semantic.c`（`extend_merge_into_block`：合并到 `struct_def.methods`）、
  `ast.c`（释放）、`optimize.c`（两个 switch 补 case）
- 用例：`assert/test_extend.leno`（正向 4 组）+ 手工核对 4 组报错场景
- 回归：`assert\run_tests.leno` **537 passed / 0 failed**

**二期已完成（2026-11-02）**：跨模块 `extend 模块.类型`，链路是

```
lib/page_download.leno   extend player.Player { func paint_dl }   ← 定义（方法体在这）
        ↓ 扫描时登记进 page_download 的符号表（extends 段，.lenosymc v42：默认值文本 + 聚合类型参数名）
主程序 import page_download
        ↓ semantic_register_extends_from_module：挂成 "Player::paint_dl" 占位符
        ↓ visit：字段名→self.字段名、前置隐式 self、方法体语义分析
        ↓ codegen：发 OP_STRUCT_EXTEND（类型名 + 方法名/函数常量表）
运行期：模块初始化时把方法**追加进已存在的 class**（struct_def_find + realloc + bump 代数）
        ↓ 调用走 struct_method_lookup(def, name) 按名字查表，不需要编译期索引 ✓
```

四块改动 + 两个踩坑：

| 坑 | 症状 | 原因 |
|---|---|---|
| 校验时机 | 报"目标模块没导入"，可明明 import 了 | `extend_merge_into_block` 在 `semantic_analyze` **最开头**跑，那时 `imported_modules` 还是空的 ⇒ 校验必须挪到 visit 阶段 |
| 重复前置 self | 带参方法第一个参数之后全是 null | 同一 extend 节点被 visit 多次 ⇒ 加幂等判据（首参已叫 `self` 就跳过） |
| 缺方法体分析 | 形参恒为 nil，但 VM 侧 argc/arity/实参寄存器全对 | 只做了 transform + 前置 self，漏了 `visit_func_as_struct_method`（参数名没进作用域 ⇒ codegen 按名字找不到槽位） |
| 缺作用域类型 | 报「未定义的类型: P（请补 use pl.P）」 | 方法体的 `self` 类型是 `P`，visit 时作用域里没有它 ⇒ 按需 `scope_define` + `semantic_attach_struct_fields` |

用例：`assert/test_extend_cross_module.leno`（三文件：定义 / 扩展 / 调用）。

**三期（2026-11-03）**：跨模块扩展**泛型 struct** —— 给隐式 `self` 带上目标类型的
`generic_args`，方法签名与方法体里的 `T` 就和写在 struct 里一样能解析（此前报
「未定义的类型: T（请 use 导入）」）。同批补了两条重名诊断（见 §5.3）。

**三期补（2026-11-04）**：跨模块扩展方法的**默认参数** —— 默认值文本随符号表
（extends 段，v41）带到调用方，codegen 省略实参时按它补齐。此前只抄参数类型，
运行期形参是 null（`p.dmg()` 在方法体里报「减法: null 不能参与运算」，`--check` 却是过的）。
另外 `export func` 修饰现在也被接受（此前被误报成"extend 块里不能加字段"）。

**三期补 2（2026-11-04）**：跨模块扩展方法的**泛型形参** —— 扩展模块里的 `T` 在符号表里
只是 `TYPE_STRUCT("T")`，调用方必须对照目标类型的 `type_params` 认出它是泛型参数（v42 起
随 extends 段带上聚合类型参数名）。此前占位符照抄 ⇒ `wrap(T x)` 的合法调用被误判成
「期望 struct, 实际 int」（实测编不过 ✗）。注意：泛型参数本身仍是"与任何类型兼容"的口径，
把 `TBox[int]` 的方法传 `string` 不会被拦（普通跨模块泛型方法同样如此，属既有口径）。

## 1. 背景：为什么需要它

`d:/CLeno/Aria/aria.leno` 实测 **6212 行**，结构是：

| 区间 | 内容 | 行数 |
|---|---|---|
| L1-192 | 常量 / 调色板 / 主题 | ~190 |
| L193-589 | `struct Player` 的**字段** | ~400 |
| L590-6208 | `struct Player` 的 **167 个方法** | ~5600 |
| L6209+ | `main()` | 4 |

`lib/` 下已经有 22 个模块（最大的 `downloads.leno` 761 行），说明外围早就拆干净了，
**唯一没拆的就是这一个 struct**。物理原因只有一条：

> **方法只能写在 `struct X { ... }` 里面。**

于是 5600 行方法无处可去。`face`/`impl` 解决不了它 —— `impl` 是
`struct X impl Face`（声明实现了哪些接口），方法体仍然必须写在 struct 内，
一个 face 都不写、写十个 face，行数都不变。

## 2. 目标与非目标

### 目标

| # | 目标 |
|---|---|
| G1 | 方法可以写在**别的文件**，且搬运时**不用改方法体**（`self` 语义、字段名、`pri` 互调全部不变） |
| G2 | 与既有 struct 完全等价：调用、重载、可见性、泛型、codegen 都不区分"写在 struct 里"还是"写在 extend 里" |
| G3 | 跨模块可用：`lib/player_search.leno` 里的 `extend Player` 在其它模块调用时，方法照常可见 |
| G4 | **不引入继承**语义：不产生子类型、不改变布局、不改变方法解析顺序 |

### 非目标（一期不做）

- `extend` 加**字段**（只加方法；加字段会破坏布局与既有构造语义）
- 给 face / enum / cstruct 做 extend（先只支持 `struct`）
- 跨模块**双向** extend（A 模块 extend B 的类型时，B 不能反向依赖 A，见 6.3）
- 一个 extend 块里写 `static` 之类的新概念

## 3. 语法

```leno
// lib/player_search.leno
import "player.leno" as p

extend p.Player {
    pri func paint_search(r: Renderer) {
        // 直接写 tbSearch / netRows，不需要 p. 前缀（self 仍然隐式存在）
    }

    func refresh_search() { ... }
}
```

规则：

1. `extend` 是**顶层声明**，与 `struct` / `face` / `func` 同级，不能出现在函数体内
2. 目标类型写成 `Name` 或 `模块别名.Name`（后者用于跨文件/跨模块）
3. 块内**只允许 `func`**（含 `pri` / `export` 修饰）；出现字段声明直接报错
4. 同一类型可以有多个 extend 块，分散在多个文件
5. 泛型 struct **不写类型参数**：`struct Box[T]` 就写 `extend Box { func peek(): T { ... } }`
   —— 方法签名/方法体里的 `T` 自动按 struct 声明的参数解析；写成 `extend Box[T]` 会报一条
   清楚的语法错误（而不是级联出一串无关错误）

### 与 `impl` 的区别（写进实现注释，也要写进文档）

| | 位置 | 作用 |
|---|---|---|
| `struct X impl Face { }` | struct **头部** | 声明"X 实现了 Face"，供多态/类型检查 |
| `extend X { }` | **顶层**独立声明 | 把 X 的方法体搬到别处，语义等价于写在 struct 内 |

两者可以共存：extend 块里定义的方法照样能实现 face 约定的方法。

## 4. AST

```c
// 新增顶层节点
AST_EXTEND_DEF

struct {
    char* module_name;   // 可空（空 = 本模块/本文件）
    char* type_name;     // 被扩展的 struct 名（已解析掉模块前缀）
    Ast** methods;       // AST_FUNC_DEF 数组
    int   method_count;
    int   line;
} extend_def;
```

## 5. 语义

### 5.1 合并（核心）

`extend` 不新建类型，它的方法**追加进目标 struct 的 `methods` 数组**
（`AST_STRUCT_DEF.u.struct_def.methods`，见 `leno_ast.h:313` 附近的
`Ast** methods; int method_count;`）。

合并之后，下面这些**全部不用改**：

- 方法调用解析（`visit_expr.inc` 的 `receiver_type->struct_name` 匹配）
- 字段可见性检查（`field_private` 已存在：默认公有，显式 `pri` 才拦）
- codegen 的方法编译（`codegen_struct_def` 遍历 `methods`）
- `.lenosymc` 里的 `ModuleStructSymbol.methods`

这就是选"追加到 methods 数组"而不是"新搞一套扩展方法表"的全部理由。

> 2026-11-04 修：扫描目标 struct 时**必须同时认 `export struct`**（它包在 `AST_EXPORT` 里）。
> 此前只认裸 `AST_STRUCT_DEF` ⇒ 模块里最常见的 `export struct Player` + `extend Player`
> 直接报「目标类型在本文件里没有定义」，方法全部不生效（实测）。
> 目标名存在但不是 struct（enum / face / cstruct）时点名报错，不再说"没有定义"。

### 5.2 顺序无关性

`extend` 可能出现在 struct 定义**之前**（同一文件）。处理方式：

1. 语义第一遍**只收集**：把 `AST_EXTEND_DEF` 挂到一张"待合并"表（`type_name → 方法列表`）
2. 遇到 `AST_STRUCT_DEF` 时，先把待合并的方法追加进去，再走原来的注册流程
3. 文件 visit 结束后，若还有未匹配的待合并项 ⇒ **报错**
   `extend 的目标类型 'Player' 在本文件里没有定义（跨模块请写 extend 模块名.Player）`

> 为什么不做"全文件两遍扫描"：那会动到 `visit_program` 的整体结构（所有
> struct/face/enum 的注册顺序都会变），风险远大于收益。**要求 struct 定义与
> 它的 extend 出现在同一文件**——这条限制对拆分没有任何影响（拆出去的方法本来就该
> 和 struct 定义在同一模块的两个文件里）。

### 5.3 重名

- 与 struct 内已有的方法同名 ⇒ 报错（不静默覆盖，否则"我改的方法怎么没生效"极难查）
- 两个 extend 块里同名 ⇒ 同样报错
- 报错文案带上两个定义的行号

**跨模块（2026-11-03 补齐）**：

- 两个导入模块给同一个 `类型::方法` 都写了扩展 ⇒ 报错（`semantic_register_extends_from_module`）
- 本文件的 `extend m.X { func f }` 与导入模块里的同名扩展 ⇒ 报错
  （`extend_check_local_dups`，在所有 import 处理完后统一扫描，与源码顺序无关）
- **本文件两个** `extend m.X` 块写同名方法 ⇒ 报错（报在**后写**那一行）★ 2026-11-04
- 扩展方法与目标 struct **自带的方法**同名 ⇒ 报错：运行期方法查找优先命中 struct 自带的，
  扩展方法**永远不会被调用**（实测写 `own(): 1`、struct 自带 `own(): 9` ⇒ 输出 9）★ 2026-11-04

> 为什么必须报错而不是"后写覆盖"：编译期占位符是**先到先得**（`func_table_find` 命中就跳过），
> 运行期 `OP_STRUCT_EXTEND` 又是按模块初始化顺序追加、方法查找取**第一个**同名项 ⇒
> 两边都以"先注册者赢"为准，后写的那份**完全不生效**且没有任何提示（实测：导入扩展模块后
> 在本文件再写同名方法，调用输出的仍是导入模块的返回值）。

### 5.4 可见性

- `pri func` 在 extend 块里：**同一模块内的其它文件可调，模块外不可调**（与写在 struct 里一致）
- extend 块**可以访问目标 struct 的 `pri` 字段**（否则拆出去没有意义）——前提是该 struct 与
  extend 在同一模块
- 跨模块 `extend m.X` **不能**访问 `pri` 成员（**字段与方法**，2026-11-04 起编译期强制）：
  `self` 的类型虽然等于目标 struct，但代码不在定义模块里。此前 `pri_inside_own_method`
  只看类型名 ⇒ 被当成"自己的方法"静默放行（实测能直接读到私有值 2、调用私有方法返回 3 ✗）。
  方法那条更隐蔽：裸调用被 `transform_method_body` 改写成 `self["方法名"](self)`（按名分发），
  点号调用的 pri 检查根本看不到它 ⇒ 现在在按名分发的解析点补查。
- **扩展模块之间**：A 的 `pri func` 只有 A 自己（同模块）的扩展方法能调；B 或主程序调用
  都会编译期报错（2026-11-04）。此前 extend 段的 pri 完全公开 —— 裸调用与 `self.方法名()`
  都能进（实测 ✗）。实现：`pri_imported_method_private` 在查完目标 struct 的 methods 后，
  再扫各导入模块符号表的 **extends 段** ✓（本模块自己的扩展方法不在导入表里 ⇒ 自己调自己放行 ✓）

### 5.5 方法数上限（2026-11-04）

**单个跨模块 `extend` 块最多 255 个方法** —— `OP_STRUCT_EXTEND` 的 `add_count` 是 **u8**
（codegen 里 `cw_u8(add_count)`）。超出会被截断成 `n & 0xFF`，紧随其后的方法元数据被当成
字节码 ⇒ 模块反序列化失败 / 运行期「类型 'X' 没有方法 'lm256'」。

- 修复前实测 260 个方法：`--check` **全绿**，只有运行期才炸（`[警告] 内存反序列化失败(6)`）
  ⇒ 现在语义层（`visit_type_def.inc` 的 `AST_EXTEND_DEF` 分支）**编译期报错**并给出处方
  「请拆成多个 extend 块」；codegen 在 `gen_struct_extend` 里再兜一道，保证不写出损坏的字节码。
- 同一文件里的 `extend`（不含模块名）会被并进 struct 的 `methods`，由 struct 自身的
  "方法数 ≤ 255" 检查覆盖（报错在 struct 定义行）。
- 边界值 255 **合法且能跑**（`assert/test_diag_static_errors.leno` ⑬ 有正向对照）。

## 6. 落点

### 6.1 parser

| 位置 | 改动 |
|---|---|
| `lexer.c` 关键字表 | 加 `{"extend", TOK_EXTEND}` |
| `leno_types.h` | 加 `TOK_EXTEND`（放在 `TOK_IMPL` 附近，注释说明与 impl 的区别） |
| `parser.c` 的 `declaration()` | 识别 `TOK_EXTEND` ⇒ 解析目标类型名 + `{ func* }` |
| `leno_ast.h` | 新增 `AST_EXTEND_DEF` 与 `u.extend_def` |

### 6.2 semantic

| 位置 | 改动 |
|---|---|
| `visit_block.inc`（`AST_STRUCT_DEF` 分支附近） | 加 `AST_EXTEND_DEF` 分支：登记待合并 |
| struct 注册处 | 合并前先 append 待合并方法 |
| 文件 visit 收尾 | 校验"未匹配的 extend" |

### 6.3 跨模块（工作量最大的一块）

`module_symbol_table` 是**独立扫描器**（不跑完整 semantic），跨模块调
`player.paint_search()` 时，方法必须出现在 `ModuleStructSymbol.methods` 里。

所以扫描器也要认 `AST_EXTEND_DEF`：扫到 extend 就把它拼进对应 struct 的 methods。
**约束**：extend 只能扩展**本模块**的类型（跨模块 extend 会在扫描器里产生"谁先扫谁"的
循环依赖），跨模块调用走的是"模块 → 它自己的 struct"这条链，不需要额外处理。

### 6.4 codegen

**零改动**。方法已经并进 `methods` 数组，`codegen_struct_def` 原本就遍历它。
唯一要确认的是：extend 文件的 `import` 要能让方法体内的符号解析正常（普通的
import 规则即可，不需要额外机制）。

## 7. 诊断文案

```
error: extend 只能加方法，不能加字段（'dlJobs'）
        提示: 字段请写回 struct Player 的定义里

error: extend 的目标类型 'Player' 在本文件没有定义
        提示: 跨模块请写 `extend 模块别名.Player`

error: 方法 'paint_search' 重复定义
        提示: struct Player 里已有同名方法（第 812 行）

error: extend 不能扩展 face / enum / cstruct（'Widget'）
```

## 8. 迁移路径（Aria 试拆）

按"字段自包含程度"排序，先易后难：

| 顺序 | 目标 | 方法 | 估计行数 | 为什么先做它 |
|---|---|---|---|---|
| 1 | `lib/page_download.leno` | `dl*` / `paint_dl*` / `apply_dl*` | ~900 | 只碰 `dlJobs` / `dlCur` / `dlCh` 少数几个字段 |
| 2 | `lib/page_search.leno` | `paint_search*` / `apply_search*` / `sync_search*` | ~1200 | 字段集中，但与表格耦合 |
| 3 | `lib/player_playback.leno` | `play_*` / `open_*` | ~900 | 依赖 LenoMusic，边界清楚 |
| 4 | `lib/player_tick.leno` | `on_*` / `sync_*` | ~600 | 通道排空，逻辑独立 |

搬运步骤（每次都一样）：

1. 原文件里剪切方法 → 新文件 `extend Player { ... }`
2. 新文件补 `import`（`player.leno` + 方法体用到的模块）
3. `--check` 通过 ⇒ 方法体一个字都没改

## 9. 验收清单

- [ ] 单文件：`struct P { }` + `extend P { func get() }` ⇒ 能调 `p.get()`
- [ ] 顺序：`extend P` 写在 `struct P` **之前** ⇒ 同样能编
- [ ] `extend` 加字段 ⇒ 报错
- [ ] 重名方法 ⇒ 报错，带两个行号
- [ ] `extend` 目标不存在 ⇒ 报错
- [ ] extend 块内访问 `pri` 字段 ⇒ 允许（同模块）
- [ ] 跨文件（同模块两个 .leno）⇒ 能编能跑
- [ ] 跨模块调用 extend 出来的方法 ⇒ 能编能跑（走 `.lenosymc`）
- [ ] `--check` 全量套件零误报
- [ ] Aria 拆出下载页后：`--check` 通过 + 下载功能正常

## 10. 风险与取舍

| 风险 | 影响 | 对策 |
|---|---|---|
| 方法表在 semantic 中途变了 | 已解析过的方法缓存失效 | extend 的合并在 **visit 该文件开头**完成（不边 visit 边加） |
| 扫描器与语义两套逻辑漂移 | 跨模块调用 suddenly 找不到方法 | 抽一个公共的 `struct_append_methods()`，两处都调 |
| 用户误以为能加字段 | 编译期报错即可，文案要清楚 | 见第 7 节 |
| 被理解成继承 | 设计困扰 | 语法上不允许出现在 struct 头部；文档与报错文案强调"不产生子类型" |
| extend 泛滥导致"方法在哪"更难找 | 可维护性 | 约定：**一个文件只 extend 一个类型**；类型名与方法名前缀一致（`paint_dl_*` 都在 `page_download.leno`） |

## 12. 跨模块 extend：三条路，成本差很多

拆 Aria 的真实形状是：`struct Player` 在 `lib/player.leno`，页面方法在
`lib/page_download.leno` 里 `extend player.Player` —— **必然跨模块**。三条路：

| 方案 | 做法 | 改动 | 代价 |
|---|---|---|---|
| A. 运行时注册 | 模块初始化时把 extend 的方法**追加进目标 class** | 需要"class 按名字查"的全局注册表 + 一条 `OP_EXTEND_METHOD`；方法在调用方模块编译，但 `self` 的字段索引必须与目标 struct 一致（字段布局来自符号表，可算） | 中高，动 codegen + VM |
| B. 外部函数 | `func paint_dl(p: Player)`，不改类 | **零语言改动**（跨模块读写字段已验证可行） | 方法体里 `field` → `p.field`，约 100 个方法机械改动；`pri` 字段不可见（要么放开可见性，要么加 `pub`） |
| C. 单文件内拆分 | 只做一期（同文件 extend） | 已完成 | 减不了行数（还在同一文件） |

**判断**：先用 **B** 验证"拆了以后是否更好维护"，再决定要不要为 A 动 codegen/VM。
理由是 A 的收益（搬运零改动）只在一次性迁移时兑现，而 B 的代价是一次性的机械替换，
之后两者维护成本相同。为一次性便利改动 VM 不划算。

## 11. 后续（不在本期）

- **`self` 字段切片**：`func paint_dl(self: Player{dlJobs, dlCur})` —— 让"这个函数碰哪些字段"
  成为签名的一部分，编译器就能算方法之间的字段连通性，**自动建议拆分边界**。这是让
  拆分从"手工"变成"工具可推导"的关键，但需要改类型系统与字段访问检查。
- **具名实参**：拆出去后函数签名会变长，具名调用能降低调用点维护成本。
