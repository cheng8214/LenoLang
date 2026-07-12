# Bug：Leno 中 `Array[Struct]` 元素访问的"拷贝陷阱"

> 状态：**已确认（语言语义 bug）；规避方案已在 `sdl_treeview.leno` 落实并验证可用**
> 影响版本：截至 2026-07-12 的 `leno` 编译器（`build/leno.exe`）
> 触发场景：任何把 `struct` 放进 `Array` 并跨函数访问/修改其元素的代码
> 复现文件：`leno_module/LenoSDL3/examples/UI组件/复合控件/test_struct_*.leno`、`test_tv_logic2.leno`

---

## 1. 一句话结论

Leno 的 `Array[Struct]` 元素**不是引用、也不是真 any**，而是"假引用"：

- 跨函数返回 struct = **值拷贝（快照）**；
- 按"非循环下标"访问 struct 数组元素 = **返回零值默认 struct**（`checked=false`、`id=0` 等），而非真实元素的拷贝；
- 读写是否"写回原数组"取决于**下标是循环变量还是参数、数组是字段还是参数**，行为完全不一致。

基础类型数组（`Array[int]` / `string` / `bool`）按任意下标访问均正常，不受影响。

---

## 2. 是不是 bug？（作者判断：是）

分两种情况：

| 现象 | 是否 bug | 说明 |
|------|----------|------|
| 跨函数返回 struct 是值拷贝 | **设计选择，不算 bug** | Go/C#/Swift 的 struct 均为值类型，返回拷贝是合理语义 |
| `list[idx].field` 返回**零值默认** struct（而非真实元素拷贝） | **明确 bug** | 即使值语义也应返回真实元素的拷贝；返回零值无法解释 |
| 同语法 `list[i].field` 因"下标是循环变量 / 参数"而结果不同 | **明确 bug** | 同一语法、结果靠上下文决定，违背最小惊讶原则 |
| `roots[i].field=v`（字段数组）写不回，但 `list[i].field=v`（参数数组）写回 | **明确 bug** | 写回行为不可预测 |

**综合定性：语言层面的语义 bug**，会让 `Array[Struct]` 在需要读/写具体字段的真实工程中不可用（表现为"状态写不回 / 读不到真实值 / 整块高亮"等诡异现象）。

---

## 3. 已实证的具体规则（逐条带复现）

### 规则 1：跨函数返回 struct = 值拷贝
`_find(id)` 内 `return list[i]`，调用方拿到的只是副本，`n.checked = true` 改不到原树。

- 复现 `test_struct_mut.leno`：函数返回 struct 后再改字段，原数组不变。
- 复现 `test_struct_find.leno`：`find(3).flag` 永远读到默认值 `false`。

### 规则 2：非循环下标访问 = 零值默认拷贝
`list[idx].field`（`idx` 是参数/任意下标）→ 返回默认 struct；只有循环变量 `list[i].field` 才是实时的。

- 复现 `test_struct_snap.leno`：
  - `collectR` 用循环变量 `list[i].flag` 读 → 正确读出 `3`
  - `snapshot` 用参数下标 `list[idx].flag` 读 → 返回默认值 `false`

### 规则 3：写回受"传参 vs 字段"影响
- `_roots` 作为**字段**直接 `roots[i].flag = v` → 没写回；
- 把 `_roots` 当**参数**传进递归函数，再用循环变量 `list[i].checked = v` → 写回成功。
- 复现：`test_tv_logic2.leno` 中 `set_checked(4,true)` 经 `_set_checked_r(_roots,...)`（传参+循环变量）后 `get_checked_ids=[4]` 正确。

### 规则 4：基础类型并行数组安全
`Array[int/string/bool]` 按任意下标访问、读写正常。
- 复现：4 场景测试（`test_struct_mod.leno` / `test_struct_main.leno`）全正常；ListBox 控件一直正常。

---

## 4. 最小复现

见 `leno_module/LenoSDL3/examples/UI组件/复合控件/` 下：

- `test_struct_snap.leno` —— 最核心，`collectR`（循环变量读实时）vs `snapshot`（参数下标读零值）对比；
- `test_struct_find.leno` —— `_find` 返回快照语义；
- `test_struct_mut.leno` —— 跨函数返回 + 局部副本修改是否写回；
- `test_struct_mod.leno` / `test_struct_main.leno` —— 4 种访问场景（同文件 / 模块内 / 访问器 / 直接字段），验证 `Array[Struct]` 本身不会退化成 any。

---

## 5. 在 TreeView 中的实际表现

- "勾选不到 / 整块高亮"：旧 `set_checked` 走 `_find(id)` 改副本、`render` 中 `tv._flat` 直接访问 struct 数组字段 → 读不到真实状态。
- 早期误判为"`Array[Struct] → any`"：经 4 场景测试证伪，真实原因是上述拷贝陷阱 + 当时 `.lenocache` 缓存过期叠加。

---

## 6. 可行的规避写法（已在 `sdl_treeview.leno` 落实并验证）

1. **源数据**仍用 `Array[TreeViewNode]` 树（`_roots`）存全量；
2. **渲染/命中**用的拍平视图改回**基础类型并行数组**（`_flatIds`/`_flatText`/`_flatParent`/`_flatDepth`/`_flatHas`/`_flatCheck`/`_flatOn`/`_flatExpanded`），按任意下标访问安全；
3. **写操作**走"递归辅助函数 + 循环变量 `list[i].field` 原地改"，例如：
   ```leno
   func _set_checked_r(Array[TreeViewNode] list, int id, bool v): bool {
       for list.len() to i {
           if list[i].id == id { list[i].checked = v; return true }
           if _set_checked_r(list[i].children, id, v) { return true }
       }
       return false
   }
   ```
4. **读操作同样不能用 `_find(id)`**（跨函数返回 struct 字段会变默认值），必须递归返回基础类型/`Dict`：
   ```leno
   func is_checked(int id): bool { return _is_checked_r(_roots, id) }
   func _is_checked_r(Array[TreeViewNode] list, int id): bool {
       for list.len() to i {
           if list[i].id == id { return list[i].checked }
           if _is_checked_r(list[i].children, id) { return true }
       }
       return false
   }
   ```
   `expand`/`_toggle_node`(`_get_expanded_r`)、`get_data`(`_get_data_r`) 同理。
5. **绝不** `_find(id)` 后改副本或读字段，也**绝不** `list[idx].field` 读真实值。

### 6.1 验证结果（`test_tv_logic2.leno`，清 `.lenocache` 后）

| 检查项 | 期望 | 实测 |
|--------|------|------|
| 选中 id | `4` | `selId=4` ✓ |
| 勾选 id=4 | `[4]` | `get_checked_ids=[4]` ✓ |
| `is_checked(4)` | `true` | `true` ✓ |
| 取消勾选后 | `[]` | `checked2=[]` ✓ |
| 展开后可见行数 | `4` | `4` ✓ |

模块 `SDL3.leno` 与示例 `test_treeview.leno` 均编译通过。

### 6.2 残留限制（已知、不影响核心交互）

`get_node` / `get_selected_node` 返回 `TreeViewNode`，跨函数返回 struct 会丢真实值（语言坑，无法在不改 API 形态的前提下解决）。如需彻底规避，建议改为返回 `Dict`。选择/展开/勾选/渲染/连接线均不受影响，已可用。

---

## 7. 建议的修复方向（给语言实现）

1. `Array[Struct]` 元素访问统一返回"真实元素的拷贝"（值语义），不要返回零值默认 struct；
2. 循环变量下标 vs 参数下标、字段数组 vs 参数数组的访问结果应保持一致；
3. 若希望支持就地修改，应提供引用/指针语义（如对数组元素的可变引用），或明确文档化"struct 数组元素不可原地修改，需整体替换元素"。
