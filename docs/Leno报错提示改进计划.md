# Leno 报错提示改进计划

> ⚠ **局部过时（2026-10-02 注）**：文中"已在 `op_utils.inc` 的 `OP_INDEX` 增加排查提示"
> ——`op_utils.inc` 是栈式文件，已删除（`0f0cf52`）⇒ 勾选项的**落点需按现行
> `src/vm/vminc/run/*.inc` 复核**；清单本身（哪些报错该更清楚）仍可作现行待办。

> 背景：在集成 Blend2D + LenoSDL3 的开发过程中，反复遇到了若干"编译器无提示、运行时才崩溃"或"提示过于模糊"的问题。本文档系统性地梳理这些问题，按优先级给出改进方案并落地实施。

---

## 一、问题清单与归类

### A 类：我写错了（无需改编译器，记录即可）
| # | 问题 | 说明 | 状态 |
|---|------|------|------|
| A1 | Blend2D 函数名记错 `bl_context_set_stroke_width_d` | 该函数没有 `_d` 后缀 | 已修复 |
| A2 | clib 签名漏参数 | `fill_path_d(self, origin, path)` 实际 3 参数 | 已修复 |
| A3 | cstruct 值类型与堆指针错配 | 用值类型字段存 `malloc()` 的堆指针 | 已修复 |

### B 类：编译器/框架行为导致（值得改进报错）
| # | 问题 | 当前表现 | 危害 | 优先级 |
|---|------|----------|------|--------|
| B1 | **clib 调用时接收方为 null 不报错** | 直接 0xC0000005 访问违规崩溃 | 极难定位 | 高 |
| B2 | 运行时"索引操作需要对象类型，但实际类型为 'null'" | 不指出是哪个变量、哪个方法 | 排查困难 | 高 |
| B3 | `.lenb` 编译后 `dirs.script_dir()` 语义改变 | DLL 路径解析失败，`ffi.load` 返回 null 无提示 | 隐藏路径问题 | 中 |
| B4 | clib 实例不能作为 struct 字段 / 函数返回 | 引用丢失后报"库对象已释放" | 语义不清 | 中 |
| B5 | 跨模块导入的 clib 跳过参数/类型校验 | 第 418 行"允许任意调用" | 错误延迟到运行时 | 中 |
| B6 | ✅ **已修（2026-10-11）** 给"本 struct 的 struct 型字段"嵌套赋值 ⇒ 报错指错靶子 | 修前：`inner.value = 1.0` ⇒ 「字段 'inner' 类型不匹配: 期望 'struct Inner'，实际 'float'」 | 照着报错去查"右边那个值 / 字段类型"，查不出真因（**是会去改 RHS 的那种误导**）；且看着像**编译器 bug**（同类写法经局部量/参数却正常 ✓） | 高 |
| B7 | 🟡 **已修一半（2026-10-11，改进 6 的 ②）** `--check` 不检查被 import 的模块里的函数体 | 坏模块放进 `import`，`--check` 打印"检查通过"（**哪怕入口真的调了它** ✗），真编译才报 | "我已经 check 过了"是假的安心；B6 就是这么混过第一道筛子的 | 中 |

---

## 二、改进方案（按优先级）

### 改进 1（高）：clib 调用接收方为空时给出明确报错
**现状**：`_b2d.bl_xxx(...)` 里 `_b2d` 若为 `null`，语义检查不拦（TYPE_CLIB 类型），运行时直接解引用崩溃。

**目标**：在 codegen 或运行时把"clib 接收方为 null"转为可读错误。

**实施位置**：clib 调用 codegen（`src/codegen`）。

### 改进 2（高）：运行时 null 索引报错补充上下文
**现状**：`op_utils.inc` 只输出 `value_type_desc(obj_val)`。

**目标**：报错中附加源码行号（已有）+ 尽量提供操作上下文。

**实施位置**：`src/vm/vminc/op_utils.inc`（已有行号），增强文案描述。

### 改进 3（中）：`ffi.load` 全失败时给出汇总提示
**现状**：`lib()` 里 7 个候选路径都失败时 `g_b2d` 保持 null，无提示。

**目标**：当 `ffi.load` 从未被成功调用（或全部候选失败）时，提示"所有候选路径均未找到库"。

**实施位置**：`sdl_blend2d.leno` 的 `lib()`（脚本层包装）+ `ffi.c` 加载失败文案已有，可补充候选路径汇总。

### 改进 4（中）：跨模块 clib 增加"空接收方"提示
**现状**：第 417-420 行"允许任意调用"导致 `null.bl_xxx` 无编译期提示。

**目标**：当接收方类型为 clib 但值为 null 时，在语义阶段给出警告或运行时明确报错。

**实施位置**：`src/semantic/visitinc/visit_expr.inc` + codegen。

### 改进 5（高，2026-10-11 登记）：嵌套字段赋值的报错 —— 要么支持它、要么说清"这条路没通"

**现象**（最小复现 12 行，实测 2026-10-11 ✓）：

```leno
struct Inner {
    float value = 0.0
}

struct Outer {
    Inner inner
    func f() { inner.value = 1.0 }     // ← 报的就是这一行
}
```

```
error: [类型不匹配] 字段 'inner' 类型不匹配: 期望 'struct Inner'，实际 'float'
```

**为什么误导**：报的字段名是**外层**字段 `inner`、期望类型是**外层字段的类型**、实际类型是
**右边那个值**的类型 —— 读起来像"你把 float 赋给了 struct 字段" ✗，可源码里**压根没有
"给 inner 赋值"这回事**（写的是 `inner.value` ✓）。真正的问题是"**嵌套成员赋值**
（`字段.字段 = v`）这条路没走通" ✓，报错却把靶子立在别处 ⇒ 照着它去查 RHS 表达式 /
字段声明，查不出所以然（**本次就是这么被带偏的**：还一度误判成"`value` 是关键字"✗，
实测 `value` 当字段名/读/写都完全正常 ✓ —— 换任何名字一样报 ✓）。

**边界**（同一份探针里逐格实测 2026-10-11 ✓）：

| 接收者是 | 写法 | 结果 |
|---|---|---|
| **本 struct 的字段** | `inner.value = x` | ✗ 报上面那条 |
| 局部量（`new` 出来的） | `o.inner.value = x` | ✅ 正常编译 |
| 参数 | `func g(Outer o) { o.inner.value = x }` | ✅ 正常编译 |
| 局部量（绕一手 copy） | `Inner c = inner; c.value = x; inner = c` | ✅ 正常编译 |
| 本 struct 的**标量**字段 | `other = x` | ✅ 正常编译 |

⇒ 只差"**基座是自己的字段**"这一格 ✗ ⇒ 更像**编译器 bug**（不是有意的语言限制：
struct 是**引用语义**，`o.inner.value = x` 与 `inner.value = x` 语义上是一回事 ✓）。

**两种修法（择一，都能让这条误导消失）**：

- ①（推荐）**把它支持起来**：`字段.字段 = v` 按"先取外层字段（引用）⇒ 再写内层字段"处理
  ⇒ 与 `o.inner.value = v` 走同一条路，报错自然没了 ✓（也顺手消掉一类"合法代码被拒" ✓）；
- ② 若确实**不打算支持**：报错要说到点上，例如
  「嵌套字段赋值暂不支持：'inner.value' —— 可以先取出来改再放回：
   `Inner c = inner; c.value = …; inner = c`」✓
  ⚠ 且**不能再用**"字段 'inner' 类型不匹配"这种**把外层字段当靶子**的文案 ✗。
  文案口径照本文档「改进 1 / 改进 2」那两条的先例：**说清"差什么" + 顺手给一句"怎么改"** ✓。

**实施位置**（本次只**定位到**这两处，**未逐行走查** ✗ —— 开工前先把这一段补完）：

- 赋值目标的解析：`src/parser/parser_expr.c:1237-1307`（左值是 `AST_MODULE_ACCESS` 时建
  `AST_ASSIGN{names[0] = module_name, targets[0] = <module_access>}` —— 注意 `names[0]` 取的是
  **最左边那个名字** ✓ 与报错里的 `inner` 吻合 ✓）；
- 报错来源：`src/semantic/visitinc/visit_expr.inc:400`
  （`AST_INDEX_ASSIGN` 的 struct 分支 `"字段 '%s3' 类型不匹配: 期望 '%s1'，实际 '%s2'"`，
  `field_name` 取自 `ast->u.index_assign.index` 的字符串 ✓ —— 即那条被脱糖出来的"单层"字段名 ✓）；
- ⚠ 同一个分支还吃 `.field = v` / `pa["x"] = v` / `pa.x += v` 的**脱糖形态**（见该段注释 ✓）
  ⇒ 动它要连带回归 `assert/test_diag_static_errors.leno` 与 examples 全量扫描 ✓。

#### ✅ 已修（2026-10-11）：按上面的**修法 ①**（把它支持起来）+ 顺带补了"成员不存在"的编译期诊断

**真因不在上面那两处定位里**（照那两处查会查空 —— 记一笔，免得下次又绕）：报错**确实**出自
`visit_expr.inc` 的 `AST_INDEX_ASSIGN` struct 分支，但让它"指错靶子"的是**前一步**：
`src/semantic/semantic_visit_method.c` 的 `transform_method_body_ex`（`case AST_ASSIGN`）里那条
"裸字段名 ⇒ `self[字段] = 值`"的整句重写 —— 它**只看 `names[i]`（最左边那个名字）**，
于是把 `inner.value = 1.0` 改写成 `self["inner"] = 1.0`：**末级成员被丢掉** ⇒ 期望类型变成外层
字段的类型、实际类型变成 RHS 的类型 ⇒ 报出那条假错 ✗。（而 `o.inner.value = v` 走的是
parser 的 `AST_INDEX_ASSIGN` 分支，不经这里 ⇒ 才出现"两种写法行为不一致"✓）

**改动两处**

| 文件 | 改动 |
|---|---|
| `src/semantic/semantic_visit_method.c` | `case AST_ASSIGN`：targets 已被 transform 成**嵌套访问**（`AST_INDEX`）时，按"先取外层字段（引用）⇒ 再写内层字段"收尾 —— 直接复用 `targets[i]` 的 obj/index 建 `AST_INDEX_ASSIGN`，**不再**按裸字段名整句重写 ✓（与经局部量/参数走同一条路 ✓） |
| `src/semantic/visitinc/visit_expr.inc` | 同分支补：**字段表已知**（`struct_def_sym` 拿得到、字段数 > 0）却查不到这个名字，且 `infer_field_type` 也落空 ⇒ 报 `struct 'X' 没有字段 'y'`（与**读**路径同一句文案，读写对称 ✓）。加了 `field_table_known` 闸门 ⇒ "struct 定义不在当前作用域"（跨模块等，两级都查不到）**不报**，避免误伤 ✓ |

**验证**（全部实跑 ✓）

- 值级：`inner.value = 1.5` + `inner.value = inner.value + 2.0` ⇒ `3.5` ✓（钉"真的写进同一个对象"，
  不只是"能编译"—— struct 是引用语义 ✓）；三层 `mid.deep.value = 4.5` ✓；参数/局部量/拷回照旧 ✓
- 诊断：`inner.nope = 1.0` ⇒ 编译期 `struct 'Inner' 没有字段 'nope'`（此前只报**运行时**错 ✗，
  而经局部量的同一句是编译期错 ⇒ 现在两条路一致 ✓）；`inner.value = "oops"` ⇒ 靶子是 `value` ✓
- 回归：`assert/test_nested_field_assign.leno`（新增，6 组）；套件 **437/437** ✓；
  LenoSDL3 全 52 个库文件 `--check` 全过 ✓；`showcase / file_manager / cleaner_master / dashboard /
  gomoku` 真编译（`-c`）通过 ✓

### 改进 6（中，2026-10-11 登记）：`--check` 的检查范围要说清（或补齐）

**现象**（实测 2026-10-11 ✓，见 `语言陷阱速查.md` §四 的表）：把带类型错误的模块放进
`import`，`--check 入口` 打印「检查通过」✗ —— **哪怕入口真的调用了那个坏函数** ✓；
而同一个坏模块**自己当入口**时 `--check` 会报 ✓；真编译/真跑当然也报 ✓。

**目标（择一）**：
- ① 让 `--check` 也把**被引用模块的函数体**过一遍（代价：慢一些；好处："改完 check 一下"才名副其实 ✓）；
- ② 至少在**输出里说清范围** —— 例如结尾打印
  「检查通过（**仅入口文件**；被 import 的模块函数体未检查 ⇒ 发版前请真编译一次 ✓）」
  ⇒ 这个更便宜，且能掐掉"我已经 check 过了"这种假安心 ✓（B6 正是这么混过去的 ✓）。

**实施位置**：`src/main.c` 的 `--check` 分支（① = 走完整模块语义；② = 只改收尾文案 ✓）。

#### ✅ 已按 ② 落地（2026-10-11）

`src/main.c` 的 `--check` 收尾改两行：第一行把范围写进"检查通过"里，第二行点名边界并给出正解：

```
检查通过: <入口文件>（仅入口文件；未产出 .lenb）
          ⚠ 被 import 的模块「函数体」未检查 —— 发版前请真编译一次：leno -c <入口>
```

⚠ ① **没做**，理由（代价 + 风险，不只是"慢一点"）：要让 `--check` 真去过模块体，就得把模块编译
管线（parse + 语义）对每个被引用模块跑一遍，同时还得管住 **`.lenomc` 缓存写入**（`--check` 现在
的卖点之一正是"**无副作用**" ✓）与**错误归属**（模块编译自己有一套 `errors.count` 前后差分，
见 `module_compiler.c` ✓）⇒ 收益不抵风险 ✓ —— 想查模块体时：`-c` 一次，或对那个模块**自己**
跑 `--check`（它自己当入口时，函数体是查的 ✓，本轮 52 个库文件的回归就是这么做的 ✓）。

**回归**：`assert/test_check_flag.leno` —— ① 钉"成功输出含范围"、④ 钉"该边界仍在"
（④ 是**故意**钉边界：改进 6 ① 一旦落地，④ 会变红 ⇒ 那时改成"应当非零退出"即可 ✓）。

---

## 三、已确认的既有良好机制（勿破坏）
- `ffi_load_func` 加载失败已 `native_throw_error`（`ffi.c:419`），含错误码/搜索路径。
- clib 本文件定义时已有参数数量/类型校验（`visit_expr.inc:360-378`）。
- `format_type_error` 的 `%s1~%s4` 位置占位符是**有意设计**，非 bug。

---

## 四、实施记录
（实施过程中逐项勾选）
- [x] 改进 1：clib 空接收方报错 —— 已在 `ffi.c` 的 `ffi_call_impl()` 开头增加 `val_is_null/val_is_obj` 检查，报错含函数名、可能原因、排查建议。验证：`test_null_clib.leno` 由崩溃变为可读运行时错误。
- [x] 改进 2：运行时 null 索引报错文案增强 —— 已在 `op_utils.inc` 的 `OP_INDEX` 增加排查提示。验证：`test_null_index.leno` 报错含提示。
- [x] 改进 3：ffi.load 全失败提示 —— 已在 `sdl_blend2d.leno` 的 `lib()` 增加"所有候选路径均未找到"的汇总打印。
- [x] 改进 4：跨模块 clib 空接收方提示 —— 与改进 1 共用运行时检查（`ffi_call_impl` 对 null 库对象的检查对本地/导入 clib 均生效）。
- [x] 改进 5：**嵌套字段赋值的报错**（B6）—— 2026-10-11 登记并**当天修好** ✓（按修法 ① 支持它，
  顺带补了"内层成员不存在"的**编译期**诊断；真因在 `semantic_visit_method.c` 而非原先定位的报错点、
  改动与验证见该节末的 ✅ 块；回归 `assert/test_nested_field_assign.leno` ✓）
- [x] 改进 6：**`--check` 的检查范围说清或补齐**（B7）—— 2026-10-11 按更便宜的 ② **落地** ✓
  （成功输出自报范围；①「让 `--check` 也过一遍模块体」**未做**，理由见该节的 ✅ 块 ✓）
  （两者都是"在 LenoMusic 移植过程中实测撞到并定位"的 ✓ 出处：`语言陷阱速查.md` §三 ㉙ / §四 ✓）

### 改动文件清单
| 文件 | 改动 |
|------|------|
| `src/module/ffi/ffi.c` | `ffi_call_impl` 开头增加库对象 null 检查，报错含函数名/原因/建议 |
| `src/vm/vminc/op_utils.inc` | `OP_INDEX` null 对象报错增加排查提示 |
| `leno_module/LenoSDL3/lib/sdl_blend2d.leno` | `lib()` 候选路径全失败时打印汇总 |
| `docs/Leno报错提示改进计划.md` | 本文档 |

### 验证结果
- 编译器重新构建成功（gcc 无警告）
- `test_null_clib.leno`：null clib 调用 → 从 0xC0000005 崩溃 → 变为可读运行时错误
- `test_null_index.leno`：null 索引 → 报错含排查提示

### 测试脚本
- `examples/改进测试/test_null_clib.leno`
- `examples/改进测试/test_null_index.leno`

---

## 五、2026-10-20：写 LenoHTTP 时撞到的两个报错问题（都已修）

### 1. `if (a) == b then …` 报「if 表达式需要使用 'then' 关键字」—— ✅ 已修（解析 bug）

**现象**：`if (id + 1) == 4 then 10 else 20` 编译失败，报"if 表达式需要使用 then"；
而 `if (id + 1 > 4) then ...` 正常。写
`out.send(if (r as string) == expect then 1 else -3)` 时撞到，第一反应是"我忘写 then 了" ✗

**根因**（`src/parser/parser_expr.c` 的 `parse_if_expr`）：看到 `if` 后面是 `(` 就**手工消费**
它、再 `consume(')')`，等于假设"括号正好包住整个条件"。于是解析完 `(id + 1)` 就去找 `then`，
撞上 `==` ⇒ 报「需要使用 then」。而 `(a) == b` 在普通表达式里完全合法 ⇒ 报错误导 ✗

**修法**：条件整体交给 `parse_expression`（括号由分组规则处理），删掉手工消费。
覆盖：`(a > b)` / `(a) == b` / `and` 链 / 嵌套 if 表达式 / `eif` 链 / 条件里带函数调用。
用例 `assert\test_if_expr_paren.leno`（7 条断言）。

### 2. `Dict opts = {}` 报「默认值必须是字面量常量」—— ✅ 文案已改

**现象**：`{}` 明明是字面量，报错却说"必须是字面量常量或编译期常量表达式" ⇒ **自相矛盾**，
让人以为是"我写错语法了"，实际是"数组/字典字面量不支持作默认值"。

**修法**（`src/semantic/semantic_visit_func.c`）：单独识别 `AST_DICT` / `AST_ARRAY`，文案改为
> 参数 'opts' 的默认值不支持 数组/字典 字面量 —— 请改用 `null`（函数内判空）或模块级常量

> 附带影响：`http.get(app, path, handler, Dict opts = {})` 因此写成 `any opts = null`（函数内判空）。
> 这是受该限制影响的真实写法，见 `LenoHTTP/lib/http_app.leno`。
