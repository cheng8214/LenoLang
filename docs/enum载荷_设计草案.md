# enum 载荷（`case Progress(done, total)`）设计草案

> 状态：**已实现**（2026-11-10，步 1~4 + 断言；步 5 无需改动、步 6 完成）。
> 目标：把"enum 只能当 int 常量用"补成"enum 成员可以带数据"，且**不破坏**现有 int 枚举。
> ⚠ 实现与本文初稿**有三处偏离**（都是为了不退回 `any`、不让类型名对不上），
>   见文末 §7「实现记录」——读 §2/§3 时以 §7 为准。

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

不为"带载荷 enum"发明新运行时对象，直接降级成编译器合成的 **同名** struct：

```leno
// 编译器为 enum Msg 合成（与 enum **同名** —— 见 §7-① 为什么不用 __Msg）
struct Msg {
    int tag          // 成员序号（声明顺序，0-based）
    int p0           // progress 第 1 个参数
    int p1           // progress 第 2 个参数
    string p2        // finished 的第 1 个参数
    string p3        // failed 的第 1 个参数
    int p4           // failed 的第 2 个参数
}
```

- **槽位布局（§7-② 改过）**：**按成员分段** —— 成员 m 的参数占用从
  `base(m) = Σ_{j<m} 参数个数(j)` 开始的连续几个槽。于是每个槽的静态类型就是
  该参数声明的类型，**一个 `any` 都不用**；`progress(int,int)` 与 `finished(string)`
  这类最常见的组合也不再冲突。
  代价：字段数 = 1 + 所有参数个数之和（实例里多几个恒为 null 的槽）。
- **构造**：`Msg.progress(3, 10)` ⇒ `AST_STRUCT_INIT(Msg, {tag: 0, p0: 3, p1: 10})`。
  codegen 复用现有 struct 构造路径（零新指令）。
- **匹配**：`switch m { case Msg.progress(d, t) { … } }` ⇒
  `switch m.tag { case 0 { var d = m.p0; var t = m.p1; … } }`。
  主题被换成 `m.tag`（int 比较链，复用 `OP_CMPJMP_EQ`），绑定变量是注入 case 体
  最前面的 `AST_VAR_DECL`（初值就是 `m.p0/p1`）。codegen 完全不知道 enum 载荷存在。
  ⚠ 因此**主题必须是变量**（`m`），表达式会被求值两次。
- **穷尽性**：tag 取值集合 = 成员序号，沿用 `WARN_SWITCH_NOT_EXHAUSTIVE`
  （老 enum 那条只认 `TYPE_ENUM`，降级后主题是 int ⇒ 由 `enum_payload.inc` 单独补一份）。

### 为什么用 tag + 位置槽，而不是"每个成员一个 struct 类型"

- 每个成员一个 struct ⇒ 变量类型得写成 `any` 或 union，`switch` 只能靠 `case is` 逐个试，
  且**穷尽性检查**退化成"运行期判定"，静态阶段无法给出"漏了谁"。
- tag + 槽位 ⇒ 变量有**单一具体类型** `Msg`，字段访问、赋值、传参全部沿用现有规则，
  `switch` 仍是对 int 的比较链（与今天 int enum 的字节码形态一致）。

---

## 4. 需要动的地方（按顺序，每步都能独立编译 + 全绿）

| 步 | 文件 | 内容 | 状态 |
|---|---|---|---|
| 1 | `src/parser/*`、`src/ast.c` | `enum` 成员允许 `name(type a, type b)`；`Ast` 里加 `member_params`（名字 + 类型数组）。**纯新增**，不带参数的成员走老路径 | ✅ 7c32a521 |
| 2 | `src/semantic/semantic.c` + `visit_enum.inc` | 见到带参数的成员 ⇒ 在**顶层列表最前面**插一条**同名** `struct E{tag,p0…}`（支持前向引用）；enum 那条声明在语义/codegen 阶段被跳过；混用 / export 在此报错 | ✅ |
| 3 | `src/semantic/visitinc/enum_payload.inc` | `E.m(args)` 改写成 `AST_STRUCT_INIT(E, {tag, p<base+k>})`。⚠ 实际节点是 **`AST_MODULE_CALL`**（parser 把"变量.名字(…)"统一建成它），`AST_CALL + MODULE_ACCESS` 那条也一并接住；`E.m`（不带括号）⇒ 报错"该成员要参数" | ✅ |
| 4 | `src/semantic/visitinc/visit_control.inc` | `case E.m(a, b)` ⇒ 主题换成 `m.tag`、case 值换成 tag 常量、绑定变量注入 case 体最前面；另补一份穷尽性警告 | ✅ |
| 5 | `src/serialize/*` | 隐藏 struct 随模块一起序列化（`OBJ_STRUCT_DEF` 已有能力 ⇒ **零改动**） | ✅ 无需动 |
| 6 | `assert/` + `docs` | 新增 `assert/test_enum_payload.leno`：构造 / 三种成员匹配 / `_` / 循环里 switch / 5 条错误路径 / 穷尽性警告 | ✅ |

**不做**：跨模块 `use` 导入带载荷 enum 的成员构造（按步 3 的改写发生在语义阶段，跨模块需要
模块符号表带上"成员是否有载荷"，属额外一步；先只在同模块可用，`export` 直接报错写清限制）。

---

## 5. 兼容性与迁移

- 现有 int 枚举**一个字节都不变**：成员仍改写为 `AST_NUM`，`OP_*` 指令不增不减。
- 新特性只对"声明了参数的成员"生效 ⇒ 四个项目现有 `enum`（Aria 的 `Source`、LenoMusic 的
  `State` 等）不需要改。
- 想立刻用上"带数据 + 穷尽检查"的项目，可以把 struct+tag 改成带载荷 enum；
  **不强制**，两条路并存一段时间，等草案实现稳定后再在 `AGENTS.md` 里推荐。

---

## 6. 验收标准

1. `assert/test_enum_payload.leno` 全绿（构造、匹配、穷尽性、5 条错误路径）；
2. 现有断言 **580/0**（579 老 + 1 新）不退（int 枚举路径零变化）；
3. `examples/` 812 个文件 `--check` 全过；`leno_gui` 110 个全过；
4. 四个项目 `--check` 通过（它们不写带载荷 enum，等价于"没影响"）；
5. `docs/Leno入门教程.md` 的 enum 章节补一节（含与 struct+tag 的取舍）✅。

---

## 7. 实现记录（2026-11-10）—— 与初稿的三处偏离

初稿有三处在实现时被否掉了，原因都是"照草案写会退回 `any` 或让类型名对不上"：

### ① 隐藏 struct 用 **enum 同名**，不是 `__Msg`

初稿：`__Msg` + 在类型解析处做名字映射。
实现：直接叫 `Msg`。理由：类型名一旦对不上，`Msg m = Msg.progress(...)`、形参/返回/字段
声明、`m.tag` 的字段索引、`OP_STRUCT_INIT` 的运行时名字查找 —— 每处都要多一条映射规则，
漏一处就是**静默错配**（本仓最怕的失效形态）。同名则整条链路零改动。
代价：`Msg` 不能再被别的同名类型占用（合成阶段直接报"与第 N 行声明的同名类型冲突"）；
显式 `new Msg(...)` 要在 `visit_struct_init.inc` 里拦掉（那是实现细节，不是用户 API）。

### ② 载荷槽 **按成员分段**，不是"按位置取共同类型"

初稿：第 k 个槽取所有成员第 k 个参数的"最宽共同类型"，不可统一就不支持。
问题是初稿 §2 自己的例子（`progress(int,int)` + `finished(string)` + `failed(string,int)`）
第 1 个位置就是 `int` vs `string` —— 照初稿要么不支持、要么退回 `any`。
实现：`base(m) = Σ_{j<m} 参数个数(j)`，成员 m 的第 k 个参数落在 `p<base(m)+k>`，
类型就是这个参数自己声明的类型。**一个 `any` 都没有**，初稿那个例子原样能用。
代价：字段数 = 1 + 所有参数个数之和（多几个恒为 null 的槽）。

### ③ `switch` 是"换主题 + 注入绑定"，不是"展开成多个 case is"

初稿说"整段改写成若干个 case is 形态"。实现更简单且**保住 break/default 语义**：
把主题换成 `m.tag`（`AST_FIELD_ACCESS`，复用原 AST_VAR 节点），case 值换成 tag 常量，
绑定变成注入 case 体最前面的 `AST_VAR_DECL`（`var d = m.p0`）。
因此**主题必须是变量** —— 表达式会被求值两次（副作用 + 两次取值可能不同）。

### 顺手补的两处

- **穷尽性**：老的那段只认 `sw_expr_type->kind == TYPE_ENUM`，降级后主题是 int ⇒
  在 `enum_payload.inc` 里按成员序号集合另补一份 `WARN_SWITCH_NOT_EXHAUSTIVE`（文案同口径）。
- **合成产物不打扰用户**：`struct_def.is_synthetic` / `var_decl.is_synthetic` 两个标记
  用来跳过"字段未初始化"、"与 native/导入模块同名"、"绑定变量未使用"这类面向用户源码的诊断
  —— 用户没写这些声明，报给他也无处可改。

### 已知缺口（下一步才做）

- 跨模块：`export enum` 直接报错；消费端 `use m.Msg` 拿到的仍是 enum 符号 ⇒ 构造会落到
  "类型 'int' 没有方法"这条远因错。要彻底解决得让模块符号表带上"成员参数个数/类型"
  （`.lenosymc` 再 bump 一次）。
- `==` 比较：两个带载荷的成员值目前是 struct 比较（引用语义），不按 tag+载荷逐字段比。
- `jsons.encode` 会暴露 `tag/p0…` 这些实现字段（可接受，但别当成稳定格式）。
