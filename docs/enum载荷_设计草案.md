# enum 载荷（`case Progress(done, total)`）设计草案

> 状态：**草案，未实现**（2026-11-10）。P4 只剩这一项。
> 目标：把"enum 只能当 int 常量用"补成"enum 成员可以带数据"，且**不破坏**现有 int 枚举。

---

## 1. 现状与痛点

现在：

```leno
enum State { idle, running, done }
```

- 成员是 **int 常量**（`State.running` 在语义阶段被改写成 `AST_NUM`，见 `visit_module.inc`）；
- 想带数据只能退回 struct + tag：

```leno
struct Msg { int kind; int done; int total }   // kind 用 "0".."5" 或裸 int
```

痛点：`kind` 与 `done/total` 的**有效性耦合**没有任何检查（`kind=0` 时 `done` 该是 0 还是没意义？），
这正是评估报告 §3.5「enum 半能用」的落点。

---

## 2. 语言形态（提案）

```leno
enum Msg {
    progress(int done, int total)      // 带载荷成员
    finished(string path)
    failed(string why, int code)
}

Msg m = Msg.progress(3, 10)            // 构造

switch m {
    case Msg.progress(d, t) { print(_str(d) + "/" + _str(t)) }
    case Msg.finished(p)    { print(p) }
    case Msg.failed(w, c)   { throw w }
}
```

规则（刻意窄）：

1. **成员要么全带载荷、要么全不带**。同一个 enum 里混用 ⇒ 编译错误。
   （理由：混用会让"值是 int"和"值是对象"两种表示并存，`switch` 与 `==` 的语义立刻分叉。）
2. 同一 enum 内**成员名唯一**，参数个数/类型按成员各自声明，不做重载。
3. **没有载荷的 enum 完全保持现状**（int 常量），既有代码与字节码零变化。
4. `E.m(...)` 只能用于**带载荷**的 enum；带载荷 enum 的成员**不能**当 int 用
   （`int x = Msg.progress(1,2)` ⇒ 编译错误，避免"看起来是数字"）。

---

## 3. 降级（lowering）：一个隐藏 struct + tag

不为"带载荷 enum"发明新运行时对象，直接降级成编译器合成的 struct：

```leno
// 编译器为 enum Msg 合成（用户看不到名字，只在这里用）
struct __Msg {
    int tag = 0                    // 成员序号（声明顺序，0-based）
    int p0 = 0                     // 载荷槽（按类型的最大槽数开）
    string p1 = ""                 // ⚠ 槽按**位置**而不是按成员复用 —— 见下
}
```

- **槽位布局**：取所有成员里"第 k 个参数"的类型集合，`p_k` 用其中"最宽"的类型。
  若同一位置的类型不可统一（如 `int` 与 `string`）⇒ **该 enum 不支持**（报错并给出
  改成 struct 的建议）。这条限制是有意的：统一类型才保证 `p_k` 是强类型字段，
  不用退回 `any`（`any` 会把今天所有的类型检查问题原样搬回来）。
- **构造**：`Msg.progress(3, 10)` ⇒ `AST_STRUCT_INIT(__Msg, {tag: 0, p0: 3, p1: ...})`。
  codegen 复用现有 struct 构造路径（零新指令）。
- **匹配**：`case Msg.progress(d, t)` ⇒ 两条判断 + 两次字段读：
  1. `__Msg.tag == 0`（比较 int，复用 `OP_CMPJMP_EQ`）
  2. `d = p0`、`t = p1`（复用 `OP_GET_FIELD`）
  整段由语义阶段改写成"若干个 case is 形态"，codegen 不需要知道 enum 载荷的存在。
- **穷尽性**：按 `tag` 收敛到本次已实现的 `WARN_SWITCH_NOT_EXHAUSTIVE`
  （成员序号集合 = tag 取值集合，天然按值比对）。

### 为什么用 tag + 位置槽，而不是"每个成员一个 struct 类型"

- 每个成员一个 struct ⇒ 变量类型得写成 `any` 或 union，`switch` 只能靠 `case is` 逐个试，
  且**穷尽性检查**退化成"运行期判定"，静态阶段无法给出"漏了谁"。
- tag + 位置槽 ⇒ 变量有**单一具体类型** `__Msg`，字段访问、赋值、传参全部沿用现有规则，
  `switch` 仍是对 int 的比较链（与今天 int enum 的字节码形态一致）。

---

## 4. 需要动的地方（按顺序，每步都能独立编译 + 全绿）

| 步 | 文件 | 内容 | 风险 |
|---|---|---|---|
| 1 | `src/parser/*`、`src/ast.c` | `enum` 成员允许 `name(type a, type b)`；`Ast` 里加 `member_params`（名字 + 类型数组）。**纯新增**，不带参数的成员走老路径 | 低 |
| 2 | `src/semantic/visitinc/visit_enum.inc` | 见到带参数的成员 ⇒ 建隐藏 `__E` struct 定义（字段：`tag` + `p0..pn`），登记到符号表；同一 enum 内"混用/类型不可统一"在此报错 | 中 |
| 3 | `src/semantic/visitinc/visit_expr.inc` | `E.m(args)` 从 `AST_FIELD_ACCESS` 改写成 `AST_STRUCT_INIT(__E, …)`；`E.m`（不带括号）⇒ 报错"该成员要参数" | 中 |
| 4 | `src/semantic/visitinc/visit_control.inc` | `case E.m(a, b)` ⇒ 改写为 `tag == k` + 绑定 `a/b` 到 `p0/p1`（复用 `case is … => var` 的绑定槽位机制） | **高**（这块最容易漏槽位回收） |
| 5 | `src/serialize/*` | 隐藏 struct 随模块一起序列化（`OBJ_STRUCT_DEF` 已有能力 ⇒ 多数情况零改动，只需确认 tag 字段名不冲突） | 中 |
| 6 | `assert/` + `docs` | 新增 `test_enum_payload.leno`：构造 / 三种成员匹配 / 穷尽性警告 / 错误路径（混用、类型不可统一、漏参数） | 低 |

**不做**：跨模块 `use` 导入带载荷 enum 的成员构造（按步 3 的改写发生在语义阶段，跨模块需要
模块符号表带上"成员是否有载荷"，属额外一步；先只在同模块可用，报错信息写清限制）。

---

## 5. 兼容性与迁移

- 现有 int 枚举**一个字节都不变**：成员仍改写为 `AST_NUM`，`OP_*` 指令不增不减。
- 新特性只对"声明了参数的成员"生效 ⇒ 四个项目现有 `enum`（Aria 的 `Source`、LenoMusic 的
  `State` 等）不需要改。
- 想立刻用上"带数据 + 穷尽检查"的项目，可以把 struct+tag 改成带载荷 enum；
  **不强制**，两条路并存一段时间，等草案实现稳定后再在 `AGENTS.md` 里推荐。

---

## 6. 验收标准

1. `assert/test_enum_payload.leno` 全绿（构造、匹配、穷尽性、三类错误路径）；
2. 现有断言 **579/0** 不退（int 枚举路径零变化）；
3. `examples/` 812 个文件 `--check` 全过；
4. 四个项目 `--check` 通过（它们不写带载荷 enum，等价于"没影响"）；
5. `docs/Leno入门教程.md` 的 enum 章节补一节（含与 struct+tag 的取舍）。
