# 缺陷：Leno 处理自递归结构体类型（`Array[Self]`）时不可靠

> 状态：**已确认（编译器缺陷；是否称"bug"见第 8 节，但"静默产生错误代码"无可辩驳）**
> 影响版本：截至 2026-07-12 的 `leno` 编译器（`build/leno.exe`）
> 复现文件：`leno_module/LenoSDL3/examples/UI组件/复合控件/test_struct_*.leno`

---

## 1. 结论（否定了两次早期误判）

- ❌ 第一次误判："`Array[Struct]` 拷贝陷阱"（普通结构体数组一切正常，已证伪）。
- ❌ 第二次误判："只要访问自递归字段 `list[i].children` 就退化成 `any`"（**常量下标
  `roots[0].children` 完全正常**）。
- ✅ 实测结论：**自递归结构体类型 `N { ...; Array[N] children }` 在 Leno 中不可靠**——
  类型推断不稳定，且**能编译时静默产生错误代码**（读不到真实值）。这是真正的问题。

`TreeViewNode.children` 正是 `Array[TreeViewNode]` 自递归字段，所以旧 TreeView 代码踩中。

---

## 2. 早期错误判断（已作废，保留以作教训）

1. "Array[Struct] 拷贝陷阱"：`test_struct_probe.leno` 证明普通结构体数组的常量/循环/参数
   下标、别名、跨函数返回全部正确；`test_struct_mut.leno` 证明写回也正确。
2. "访问自递归字段即退化成 any"：`test_struct_children.leno` 证明 `roots[0].children`
   （常量下标）直接访问、赋值、传参、取子元素**全部正常**。

**教训**：两次都先入为主下了"通用结论"，靠清缓存 + 对照实验（常量 vs 非常量下标、
递归 vs 非递归函数）才逐步收窄到真实行为。文档必须如实描述"哪种情况坏、哪种正常"。

---

## 3. 已实测的行为矩阵（关键：哪些正常、哪些坏）

以自递归 `struct N { int id; bool flag; Array[N] children }`、全局 `Array[N] roots` 为例：

| 用法 | 结果 | 复现 |
|------|------|------|
| 常量下标读 `roots[0].children` | ✅ 正常（len=1、可取子元素） | `test_struct_children.leno` |
| 常量下标 `.children` 赋值/传参 | ✅ 正常 | `test_struct_children.leno` |
| 常量下标读 `roots[0].flag` | ✅ 正常 | probe/children |
| 循环变量下标 `roots[i].flag`（非递归字段） | ✅ 正常 | probe |
| 非递归函数 + `roots[i].children` 传参 | ❌ **编译报错**：期望 `Array[N]` 实际 `any` | `test_struct_iso3.leno` |
| 递归函数 + `roots[i].children` 传参 | ⚠️ **编译通过但静默返回错误值** | `test_struct_find.leno` / `test_struct_iso5.leno` |
| 非递归 struct 含 `Array[别的struct]` 字段 | ✅ 正常 | `test_struct_iso4.leno` |

**核心规律**：
- 仅"自递归"（`Array[N]` 含自身）坏；"含别的 struct 数组"（`Array[M]`）正常 → 问题在自递归。
- **常量下标**的自递归字段访问正常；**非常量下标（`roots[i]`）** 才触发。
- 触发后表现二选一且不稳定：**要么编译报错 `any`，要么编译通过却静默产生错误代码**。

---

## 4. 决定性证据

### 4.1 静默错误代码（最严重，能编译能跑但结果错）
`test_struct_find.leno` / `test_struct_iso5.leno`（被调函数 `_findIn`/`findIn` 为**递归**）：
```
setFlag(3, true) 后 _find(3).flag = false     // 真实值应为 true
```
代码合法、编译通过、运行无报错，但 `get(3).flag` 返回 `false`。**编译器静默生成了错误代码**，
这是无可辩驳的正确性缺陷（正确实现应要么拒绝、要么给对的值）。

### 4.2 类型推断不稳定（同模式一会儿报错一会儿通过）
- `test_struct_iso3.leno`：被调 `findIn` 为**非递归**，调用 `findIn(roots[i].children,id)`
  → 编译报错 `期望 Array[struct N], 实际 any`。
- `test_struct_iso5.leno`：`findIn` 改成**递归**（内部 `findIn(list[i].kids,id)`）后，
  同样的 `findIn(roots[i].kids,id)` 调用 → **编译通过**（但仍静默返回错值，见 4.1）。
二者仅差"被调函数是否递归引用自身"，说明 `Array[N]` 类型解析依赖递归上下文，推断不稳定。

### 4.3 常量下标正常（收窄触发条件）
`test_struct_children.leno`：`roots[0].children.len()=1`、`ch=roots[0].children; ch[0].id=2`、
`passParam(roots[0].children)=1` 全部正确 → 坏的是"非常量下标"，不是"访问该字段"本身。

---

## 5. 在 TreeView 中的实际表现

- `TreeViewNode.children` 是自递归 `Array[TreeViewNode]`。旧 `sdl_treeview` 用 `_find(id)`
  返回节点、并 `roots[i].children` 做递归（`i` 为循环变量 = 非常量下标）→ 踩中上述缺陷，
  选择/展开/勾选状态读不到真实值（"勾选不到 / 整块高亮"）。
- 早期误判为"Array[Struct] → any"与"拷贝陷阱"，并被 `.lenocache` 过期放大；真正根因见第 3、4 节。

---

## 6. 规避写法（已在 `sdl_treeview.leno` 落实并验证）

核心：**不在 Leno 里依赖自递归结构体类型的元素访问**。

1. 源数据用 `Array[TreeViewNode]` 树（`_roots`）存全量，但避免 `roots[i].children` 这种
   非常量下标的自递归字段访问；
2. 渲染/命中用**基础类型并行数组**（`_flatIds`/`_flatText`/`_flatCheck`/`_flatOn`/...）绕开；
3. 按 id 的写走递归辅助函数原地改（`list[i].checked = v`，整体传 `list` 参数）；
4. 按 id 的读走递归函数返回基础类型/`Dict`（`is_checked`/`_get_expanded_r`/`get_data_r`）。

### 6.1 验证结果（`test_tv_logic2.leno`，清 `.lenocache` 后）

| 检查项 | 期望 | 实测 |
|--------|------|------|
| 选中 id | `4` | `selId=4` ✓ |
| 勾选 id=4 | `[4]` | `get_checked_ids=[4]` ✓ |
| `is_checked(4)` | `true` | `true` ✓ |
| 取消勾选后 | `[]` | `checked2=[]` ✓ |
| 展开后可见行数 | `4` | `4` ✓ |

模块 `SDL3.leno` 与示例 `test_treeview.leno` 均编译通过。

---

## 7. 给语言实现的修复方向

1. **自递归结构体类型 `Array[Self]` 需被类型系统正确支持**：常量下标已正常，应把同一正确性
   推广到非常量下标（`roots[i].children`）；
2. **消除类型推断不稳定**：递归/非递归上下文下对 `Array[N]` 的解析结果应一致；
3. **严禁静默错误代码**：对无法可靠支持的类型/访问，应直接拒绝（报错），绝不应编译通过却
   产生错误运行结果（4.1 是最严重项）。

---

## 8. 这算不算"bug"？

- "能编译却静默返回错误值"（第 4.1 节）：**是 bug**，任何编译器都不应静默生成错误代码。
- "自递归类型有时被拒、有时可用"：可视为**未完善支持的特性/限制**，而非严格意义的 bug。
- 对 TreeView 用户而言，结论不变：**绕开自递归结构体类型的元素访问**，用并行数组。
