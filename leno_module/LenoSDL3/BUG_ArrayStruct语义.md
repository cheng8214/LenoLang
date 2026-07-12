# Bug：Leno 中自递归结构体字段（`Array[Self]`）访问退化成 `any`

> 状态：**已确认（语言类型系统 bug，范围比最初判断窄得多）**
> 影响版本：截至 2026-07-12 的 `leno` 编译器（`build/leno.exe`）
> 复现文件：`leno_module/LenoSDL3/examples/UI组件/复合控件/test_struct_*.leno`

---

## 1. 一句话结论（重要：否定了早期的错误判断）

**普通 `Array[Struct]` 完全正常**，不存在"拷贝陷阱"。真正的问题是：

> 当 `struct` 自身含一个"同类型数组"字段（`Array[Self]`，即自递归结构体，
> 例如 `TreeViewNode { ...; Array[TreeViewNode] children }`）时，在数组元素上访问该字段
> （`list[i].children`）会被编译器处理成 `any` 类型，读取/返回拿不到真实值。

`TreeViewNode` 的 `children` 字段正是这种自递归类型，所以 TreeView 旧代码踩中此 bug。

---

## 2. 早期错误判断（已作废，保留以作教训）

最初（含已提交的初版本文档）误判为"Array[Struct] 元素访问的拷贝陷阱"，并声称：
- 跨函数返回 struct = 值拷贝；
- 非循环下标 `list[idx]` 读 = 零值；
- 写回行为不一致（规则 3）。

经重新实测，**这些均不成立**：普通结构体数组的常量下标、循环变量下标、参数下标、
别名读写、跨函数返回——全部正确（见第 3 节证据 A）。规则 3 的"写回不一致"更是
被 `test_struct_mut.leno` 直接证伪（参数下标写、局部别名写都正确写回）。

**教训**：最初的"bug"现象部分源于 `.lenocache` 缓存过期 + 对自递归字段 bug 的误读，
在清缓存、做对照实验后才定位到真正的窄范围根因。

---

## 3. 实证（逐条带可复现文件）

### 证据 A：普通 `Array[Struct]` 一切正常（否定"拷贝陷阱"）
`test_struct_probe.leno`（`struct N { int id; bool flag }`，无 children）：
- 常量下标 `g[0].flag` / `g[1].flag` → 正确
- 循环变量 `g[i].flag` → 正确
- 参数下标 `readAt(g, idx)` → 正确（true/false 都对）
- 别名 `Node n = list[idx]; n.checked = v` → 写回成功
- 参数下标写 `list[idx].checked = v` → 写回成功
- 跨函数返回 `return list[idx]` → 正确

`test_struct_rec.leno`（`struct B { ...; Array[B] kids }`，但未做"写入后元素访问"）也全对。

### 证据 B：自递归字段 `Array[Self]` 元素访问 → 类型退化成 `any`（决定性）
`test_struct_iso3.leno`（`struct N { ...; Array[N] kids }`，与 TreeViewNode 同构）：
```leno
func findIn(Array[N] list, int id): N { ... }
func get(int id): N {
    for roots.len() to i {
        if roots[i].id == id { return roots[i] }
        N c = findIn(roots[i].children, id)   // 访问自递归字段
        ...
    }
}
```
编译直接报错：
```
[类型不匹配] findIn 第 1 个参数类型不匹配: 期望 Array[struct N], 实际 any
```
即 `roots[i].children` 被推断为 `any`，证明编译器对 `Array[Self]` 字段的元素访问类型解析失败。

### 证据 C：非自递归的"struct 数组字段"正常（收窄到"自递归"）
`test_struct_iso4.leno`（`struct N { ...; Array[M] kids }`，M 是另一个 struct）：
- `roots[0].kids.len()` = 1
- `f(roots[0].kids)` = 1（参数类型 `Array[M]` 匹配）
说明"结构体含 struct 数组字段"本身没问题，**只有自递归 `Array[Self]` 才坏**。

### 证据 D：自递归字段写入后再经元素访问读 → 拿不到真实值
`test_struct_find.leno` / `test_struct_snap.leno`（均含 `Array[N] children`）：
- `setFlag(3, true)` 写 `roots[1].flag = true`；
- 经 `_find`（`_findIn(roots[i].children, id)` 途经自递归字段）返回后，
  `find(3).flag` 读到 `false`（真实值应为 `true`）。
（与证据 B 同源：自递归字段访问退化成 `any`，读零值。）

> 注：`test_struct_iso.leno` / `test_struct_iso2.leno`（写入后直接读、循环读、参数读，
> 但**不途经 `.children` 元素访问**）均正确，进一步说明触发条件是"自递归字段元素访问"。

---

## 4. 最小复现

最干净、最具决定性的是 `test_struct_iso3.leno`：编译即报"自递归字段 → any"类型错误。
辅以 `test_struct_find.leno` / `test_struct_snap.leno` 展示运行时读零值。

对照（正常，证明非普遍问题）：`test_struct_probe.leno`、`test_struct_rec.leno`、
`test_struct_iso.leno`、`test_struct_iso2.leno`、`test_struct_iso4.leno`。

---

## 5. 在 TreeView 中的实际表现

- `TreeViewNode.children` 是自递归 `Array[TreeViewNode]`。旧 `sdl_treeview` 用
  `_find(id)` 返回节点元素、并访问 `.children` 做递归，恰好踩中"自递归字段元素访问"
  的 bug，导致选择/展开/勾选状态读不到真实值（表现为"勾选不到 / 整块高亮"）。
- 注意：早期曾误判为"Array[Struct] → any"和"拷贝陷阱"，并被 `.lenocache` 过期放大。
  真正根因见第 1、3 节。

---

## 6. 规避写法（已在 `sdl_treeview.leno` 落实并验证）

核心：**完全避免依赖 `TreeViewNode.children` 这一自递归字段的元素访问**。

1. 源数据仍用 `Array[TreeViewNode]` 树（`_roots`）存全量（作为整体传递/递归传参，
   不直接在元素上取 `.children` 再当真值读）；
2. 渲染/命中用的拍平视图用**基础类型并行数组**（`_flatIds`/`_flatText`/`_flatCheck`/
   `_flatOn`/...），按下标访问安全，彻底绕开自递归字段；
3. 按 id 的"写"走递归辅助函数原地改（`list[i].checked = v`，整体传 `list` 参数，
   不直接读取元素的 `.children` 字段值）；
4. 按 id 的"读"走递归函数返回基础类型/`Dict`（`is_checked`/`_get_expanded_r`/
   `get_data_r`），不返回 struct 元素。

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

## 7. 建议的修复方向（给语言实现）

1. **正确解析自递归结构体字段 `Array[Self]` 的类型**：`list[i].selfField` 应保留为
   `Array[Self]`，不应退化成 `any`（证据 B 的编译错误即源于此）；
2. 修复后，自递归字段的元素读写应与普通字段一致，无需用户用并行数组规避；
3. 类型推导不稳定（有时编译报错、有时静默退化成 `any` 导致读零值）应一并处理，
   保证"类型错误"与"运行时取值"行为一致。
