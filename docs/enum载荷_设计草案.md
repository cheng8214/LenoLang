# enum 载荷（`case Progress(done, total)`）设计草案

> 状态：**已实现并收官**（2026-11-10）：步 1~4 + 断言 + 跨模块 + `==` 按值 + `jsons.encode`。
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
| 2 | `src/semantic/semantic.c` + `visit_enum.inc` | 见到带参数的成员 ⇒ 在**顶层 import/use 之后**插一条**同名** `struct E{tag,p0…}`（支持前向引用；不能越过 import，否则载荷类型解析不到）；enum 那条声明在语义/codegen 阶段被跳过；混用在此报错（`export` 已支持，见 §7） | ✅ |
| 3 | `src/semantic/visitinc/enum_payload.inc` | `E.m(args)` 改写成 `AST_STRUCT_INIT(E, {tag, p<base+k>})`。⚠ 实际节点是 **`AST_MODULE_CALL`**（parser 把"变量.名字(…)"统一建成它），`AST_CALL + MODULE_ACCESS` 那条也一并接住；`E.m`（不带括号）⇒ 报错"该成员要参数" | ✅ |
| 4 | `src/semantic/visitinc/visit_control.inc` | `case E.m(a, b)` ⇒ 主题换成 `m.tag`、case 值换成 tag 常量、绑定变量注入 case 体最前面；另补一份穷尽性警告 | ✅ |
| 5 | `src/serialize/*` | 隐藏 struct 随模块一起序列化（`OBJ_STRUCT_DEF` 已有能力 ⇒ **零改动**） | ✅ 无需动 |
| 6 | `assert/` + `docs` | 新增 `assert/test_enum_payload.leno`：构造 / 三种成员匹配 / `_` / 循环里 switch / 5 条错误路径 / 穷尽性警告 / **按值 `==` + `jsons.encode` + 跨模块** | ✅ |
| 7 | 跨模块 + 运行期语义（**初稿没列，后来补的**） | `.lenosymc` v44 带载荷表；`OP_STRUCT_DEF` 尾部带签名 ⇒ `==` 按值、`jsons.encode` 用成员名 | ✅ 见 §7 |

**原定的"不做"已做掉**：跨模块 `use` 导入带载荷 enum（`.lenosymc` v44 + 消费端重建同名 struct），
所以 `export enum` 现在是正常写法，不再报错。

---

## 5. 兼容性与迁移

- 现有 int 枚举**一个字节都不变**：成员仍改写为 `AST_NUM`，`OP_*` 指令不增不减。
- 新特性只对"声明了参数的成员"生效 ⇒ 四个项目现有 `enum`（Aria 的 `Source`、LenoMusic 的
  `State` 等）不需要改。
- 想立刻用上"带数据 + 穷尽检查"的项目，可以把 struct+tag 改成带载荷 enum；
  **不强制**，两条路并存一段时间，等草案实现稳定后再在 `AGENTS.md` 里推荐。

---

## 6. 验收标准

1. `assert/test_enum_payload.leno` 全绿（构造、匹配、穷尽性、5 条错误路径、按值 `==`、
   `jsons.encode`、跨模块）；
2. 现有断言 **580/0**（579 老 + 1 新）不退（int 枚举路径零变化）；
3. `examples/` 812 个文件 `--check` 全过；`leno_gui` 110 个全过；`build/leno_module` 369 个
   里只剩 2 个**既有**失败（LenoWeb 的 generic 示例，与 enum 无关）；
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

### 三处缺口：**已补齐**（2026-11-10 第二轮）

| 缺口 | 做法 | 落点 |
|---|---|---|
| 跨模块（`export enum` + `use m.Msg`） | 载荷表随 **`.lenosymc` v44** 往返（`ModuleEnumSymbol.member_param_counts/member_param_types`）；消费端 `use` 时**自己重建**同名 struct（符号 + 字段表 + 载荷表），不依赖生产端的 AST。运行期的结构定义仍由生产模块 `import` 时 `OP_STRUCT_DEF` 注册（同名 ⇒ 对得上） | `sym_table_cache.inc`、`module_ast_exports.c`、`semantic_import_payload_enum` |
| `==` 按值比较 | `OP_STRUCT_DEF` **尾部**多带 `has_enum_sig + u16 常量`（成员/参数名签名 `progress(done,total);finished(path)`，`.lenb` v3.2.31 / `.lenomc` v46）。运行期按该标志走"逐字段比较"（`value_eq_depth` 深度 8）；`value_shallow_equal` 也同步（否则 `Array.has` / Dict 键会与 `==` 自相矛盾） | `codegen_stmt.c`、`11_struct_face_enum.inc`、`vm.c`、`leno_value.h` |
| `jsons.encode` 暴露 `tag/p0` | 有签名时输出 `{"progress":{"done":3,"total":10}}`（成员名 + 参数名）；解析不出来就退回通用输出（宁可多暴露字段，也不产出坏 JSON） | `jsons.c` 的 `json_encode_enum_payload` |

> 三条都靠同一份"签名文本"，所以**三处读取方必须同序**：VM 的 `OP_STRUCT_DEF`、`debug.c` 的
> `decode_trailing`、`bytecode_verify.c` 的 `scan_struct_def`。漏一处就会把下一条指令当数据头
> ⇒ 运行期"内存反序列化失败(6)"，而 `--check` 依然是绿的（与 v3.2.29 的 `Array[T]` 同一个坑）。

### 仍未做（明确不做）

- `jsons.fill(enum_value, json)` 反向填充：载荷"属于哪个成员"是运行期概念，反填要先定 tag；
  目前请用 `switch` 分派。
- `==` 跨**不同 enum** 比较：类型不同 ⇒ 不等；`switch` 里混用两个 enum 的成员也在编译期就拦。

## 8. 压力测试记录（2026-11-10）—— 测出 3 个 bug，已修

用例固化在 `assert/test_enum_payload_stress.leno`（8 组：载荷类型 / 零参数变体 / 容器相等 /
5 层嵌套 / 名字撞车 / 322 槽宽索引 / 跨模块 / JSON）。三处**都是收官提交里没覆盖到**的：

### ① `a == b` 为真，`arr.has(b)` 为假（值比较有两条路，结论不一致）

- 现象：5 层嵌套载荷 `==` 给 true、`has` 给 false；载荷里装 `Array[int]` 时同样打架。
- 根因：`==` 走 `vm.c` 的 `value_eq_depth`（深度上限 **8**、容器逐元素），
  `Array.has / index_of / Dict 键` 走 `leno_value.h` 的 `value_shallow_equal`（上限 **4**、
  并且**不递归容器** —— 它对 Array/Dict 一律按指针比）。
- 修法：抽出**唯一实现** `value_payload_field_equal_depth()`（leno_value.h，不分配、
  覆盖数值/字符串/数组/字典/嵌套载荷），两条路都调它；深度上限统一成
  `LENO_VALUE_EQ_MAX_DEPTH = 8`。普通 struct 仍是引用语义（零变化）。

### ② `enum Cmd { add(int n)  stop() }` 被判成"混用带载荷与不带载荷"

- 现象：零参数变体（Option 的 `None`、命令的 `Stop`）**根本写不出来**，直接报混用。
- 根因：判据是"参数个数 > 0"，而 `stop()` 的参数个数也是 0，与老写法的 `stop` 不可区分。
- 修法：AST 加 `member_payload_flags`（**写没写括号**），parser / `enum_payload_synthesize` /
  `visit_enum.inc` 的判据全改成它。`stop()` 现在能构造、能 `case Cmd.stop()` 匹配、能进 JSON（`{"tick":{}}`）。
- 顺带堵一个洞：`export` 一个**全部成员零参数**的带载荷 enum 会明确报错
  （模块符号表靠"参数总数 > 0"决定存不存载荷表，全零会静默退化）—— 报错而不是错值。

### ③ 只要 enum 里有一个 struct 载荷，整条 enum 的 `==` 全部变 false

- 现象：`enum P { num(int v)  pt(Pt p) }`，`P.num(5) == P.num(5)` 是 **false**。
  4 个成员时正常、加到第 5 个（含 struct 成员）就崩 —— 非常像"随机"。
- 根因：合成 struct 的载荷槽没写默认值时，**struct / face / Ptr / cstruct 类型的槽会被自动
  造成一个新实例**（实测 `P.num(5)` 的 `p1` 是 `Pt{x=1}` 而不是 null）⇒ 两次构造的 p1 是
  两个不同对象 ⇒ 逐字段比较必不等。
- 修法：合成时给**引用类**载荷槽显式默认 `null`（标量槽不能写 null，交给原零值默认）。

### ④ 两个模块导出**同名但不同形**的带载荷 enum：运行期才报（已改成编译期）

- 现象：`import a` + `import b`，两边都 `export enum Ev`（成员不同），消费端 `use a.Ev` +
  `use b.Ev` ⇒ **编译通过**，程序先跑完 main（打印了 "ok"），然后在收尾时报
  「跨模块同名 struct 'Ev' …… 请给其中一个改名」。
- 根因：`semantic_import_payload_enum` 把"同名且已是带载荷 enum"当**幂等**直接放行，
  于是编译期默默挑了先导入的那份；形状冲突只有运行期的 `def_owner_conflict` 才查，
  而它是在两份 `OP_STRUCT_DEF` 都注册时才触发 —— 那时 main 可能已经拿其中一份跑过了。
- 修法：导入时按**成员名 + 参数个数**比形状（与 `def_owner_conflict` 同口径），一致才幂等放行，
  不一致就在**编译期**报错并点名"给其中一个改名 / 别 use、用限定名 `a.Ev` / `b.Ev`"。

### ⑤ 泛型 / 并发 / JSON 往返：压测通过，无 bug

| 场景 | 结果 |
|---|---|
| `Box[Ev]`（泛型 struct 的字段是跨模块导入的带载荷 enum）、`Pair[Ev,string]`、`idOf[Ev](…)` | ✅ 取出来能 switch、能按值比较、标量字段不受影响 |
| 带载荷 enum 过 `Channel`（`send`/`receive`）、`threads.start` 入参 | ✅ 跨线程是深拷贝，收回来能 switch；`==` / `Array.has` / `index_of` 仍按值成立 |
| `jsons.encode` 单值 / 零参数成员 / 数组 / struct 字段 / 再 encode | ✅ 形状稳定：`{"done":{"code":7,"who":"me"}}`、`{"quit":{}}`、`[{"text":{"s":"a"}},{"quit":{}}]`、`{"name":"box","msg":null}` |
| `jsons.decode(… ) as Ev`（反填） | 仍返回 `null`（设计上未做，见上文"仍未做"）—— 需要时用 `switch` 分派 |

回归用例：`assert/test_enum_payload_interop.leno`（泛型/跨模块/JSON）、
`assert/test_enum_payload_threads.leno`（Channel 双向 + 深拷贝后按值比较）。

### 已知边界（不是 bug，写在这里免得下次又踩）

- `switch` 的主题必须是**变量**（降级后要读 `m.tag` / `m.p0`，表达式会被重复求值）——
  语义阶段有定向报错。
- 一个 switch 里**每个 case 的绑定各占一个寄存器** ⇒ 80 个 case × 4 绑定会撞 255 上限；
  与手写 80 个 case 各声明 4 个局部量完全同价（实测对照：两者报同样的错），不是载荷特有。
- 300 成员 × 零参数的同模块 enum 可用（带载荷 enum 不发 `OP_ENUM_DEF`，255 成员上限不适用）。

---

## 9. 泛型载荷 enum（2026-11-10 起，进度 2/3）

### 决定：走**擦除**，与泛型 struct 同一条路

依据（实测，不是猜的）：

- `struct Box[T] { T v }` 现在的表示是"**类型系统记住实参、运行期只有一份同名定义**"：
  `type(new Box[int]())` 打印 `Box[int]`、`is Box[int]` 为真，但字段按 any 存。
- 泛型载荷 enum 只要**沿用同一模型**即可：载荷槽里跟类型参数同名的注解在解析期就转成
  `TYPE_GENERIC_PARAM`（`convert_to_generic_params`，struct/func/face 三处早就在用），
  合成出来的隐藏 struct 带上同样一份 `type_params` ⇒ 实例化时按实参替换。
- **不选单态化**（每个实参组合造一份 `__R_int`）：要动 struct 定义表、字段索引、
  `.lenb` 与模块缓存版本策略，而收益只是"字段不做 any"——现有泛型 struct 也没这么做，
  为 enum 单独破例会让两套泛型语义并存。

### 已完成（步 1 + 步 2）

1. **解析器**：`enum Name[T, U] { … }` 接受类型参数列表（重复参数有诊断），
   载荷参数类型里的 `T` 转成 `TYPE_GENERIC_PARAM`（与 struct 字段同款）。
2. **降级合成**：合成出的隐藏 struct 继承同一份 `type_params`；
   `TYPE_GENERIC_PARAM` 载荷槽显式默认 `null`（与压测 §8 那条同一个理由）。

现在这些都能编译且零诊断：

``leno
enum Res[T] { ok(T v), err(int code, string msg) }
func sig(Res[int] r): string { return "x" }
``

### 已完成（步 3/3）：构造从**上下文**推断

类型实参认**四处**（`Semantic.expected_type` 通道：进入时挂上、访问完立刻还原）：

1. 带类型的声明：`Res[int] a = Res.ok(42)`
2. `return`（函数签名写了返回类型）：`func f(): Res[int] { return Res.ok(1) }`
3. 给已声明变量赋值：`a = Res.ok(9)`
4. `expr as Res[int]`

四处都推不出 ⇒ **定向报错**（并提示"先赋给带类型的局部量"），不静默。
明确不做 `Res[int].ok(…)` 这种显式写法（要改 `Type[args].member` 的解析规则，收益低于成本）。

回归钉子：`assert/test_enum_payload_generic.leno`（构造三通道 / 匹配绑定 / 双实参共存 /
多类型参数 / 按值比较）。

### 已知边界

- 泛型载荷 enum 暂不支持**跨模块 export**（模块符号表里没有载荷槽的类型参数表）。
- 类型参数暂不支持约束/默认值（`enum R[T: Face]`）：解析器只认名字列表。
- **enum 不能挂方法**（`extend Res { … }` 会报"extend 只能给 struct 加方法"）⇒ 即使载荷
  enum 运行期就是个 struct，`isOk/orElse/…` 这类方法也挂不上去。**这正是 `LenoResult` 的
  `Result[T]` / `Option[T]` 必须保持 struct 的决定性理由**（不是"不能泛型"了 —— 那个已经支持）。