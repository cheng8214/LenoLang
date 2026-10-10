# LenoSDL3 打包后「窗口句柄下发」整条静默失效排查记录

> **一句话**：`-p --onefile` 打包后，`Edit` 拿不到窗口句柄（`hwnd == null`）⇒
> `SDL_SetTextInputArea` 从不调用 ⇒ **中文输入法的候选框跑到窗口最左上角**；
> `.leno` 直跑完全正常。根因是**两个各自独立、又叠在一起**的问题：
> ① 跨模块**同名符号**在"合并成一个编译单元"时串了；
> ② 句柄下发路径上的**合并 `case is A, B, C`** 退化成按名动态派发，被入口 `.lenb` 的 **DCE 剪成空桩**。

- 日期：2026-10-10
- 触发方：`LenoMusic`（音乐下载器）打包版
- 涉及模块：`build/leno_module/LenoSDL3`（本次已改：`sdl_layout.leno` / `sdl_window.leno`）
- 状态：**模块侧已修并验证**；**编译器侧也已治本**（2026-10-10 补做，见 §七 与 §九）

---

## 一、现象（用户原话）

> "编辑框在 tab 里，我输入歌曲名称时，输入法跑到了窗口最左上角去了"
> "不打包就正常，打包后输入法就到窗口左上角"

复现条件：

| 运行方式 | DCE | 表现 |
|---|---|---|
| `build\leno.exe app.leno`（开发态直跑） | 不开 | ✅ 正常 |
| 模块走 `.lenomc` 缓存 | 不开 | ✅ 正常 |
| `build\leno.exe -p --onefile app.leno`（打包版） | **开** | ❌ 候选框在窗口左上角 |

应用结构（触发点）：搜索框 `Edit` 在 **TabControl 的「布局页」**里 ——
`Window → VBox root → HBox body → TabControl → VBox page0 → HBox row1 → Edit`。

---

## 二、快速复现法（打包专属 bug，不必真打包）

```bat
:: 1) 写出「入口 .lenb」—— 它是**唯一**会开 DCE 裁剪的产物
build\leno.exe -c app.leno

:: 2) 用 VM 直接跑这个 .lenb ⇒ 打包态的故障当场复现
build\leno_vm.exe app.lenb
```

> `.leno` 直跑 / 模块 `.lenomc` 缓存写出路径都**不开 DCE**（见 `src/serialize/serialize.c`
> 的 `dce_set_active`），所以只有"单文件 `.lenb` / 打包"这一条路有病 ✓

---

## 三、链条与定位（插桩结论）

`Edit` 的 IME 位置只由一处决定（`sdl_edit.leno`）：

```leno
pri func update_ime() {
    if not focused or hwnd == null { imeX = -1; …; return }   // ← hwnd 为 null 就直接放弃
    …
    core.setTextInputArea(hwnd, rect.to_ptr(), icur)          // ← 从不执行
}
```

插桩实测（打包态）：

```
[probe] Window.setRootAt 进入 type(root)=VBox
[probe] Window.setRootAt 返回          ← 中间整条句柄下发**一个调用都没发生**
[probe] update_ime 跳过：focused=true hwndNull=true   ← 于是 Edit.hwnd 永远是 null
```

句柄下发链（正常时）：

```
Window.setRootAt → Window._passWindowHandle(root)
  → VBox._bind_window → _layout_pass_window → _lay_dispatch_handle（原 _passWindowHandle）
      → HBox._bind_window → … → TabControl._bind_window → _tab_pass_window
      → lay.passWindowHandle → … → Edit.set_window(hwnd) ✓
```

---

## 四、根因 ①：跨模块**同名符号**在合并单元里串了

`sdl_layout.leno` 与 `sdl_window.leno` 里各有一个 `_passWindowHandle`：

| 位置 | 形态 | 角色 |
|---|---|---|
| `sdl_layout.leno` | `func _passWindowHandle(Widget, Ptr[u8])` | **句柄递归下发的干活函数** |
| `sdl_window.leno` | `Window._passWindowHandle(Widget)` | 窗口方法（`setRoot` / `setRootAt` 用） |

- `.leno` 直跑：模块各自编译、各自解析 ⇒ 互不干扰 ✓
- 单文件 `.lenb` / 打包：所有模块并成**一个编译单元** ⇒ 同名符号被绑到 `Window` 那个方法上
  （**参数个数都不同**）⇒ `sdl_layout` 里三处 `_passWindowHandle(c, hwnd)` 整条变成**空操作**：
  不报错、不执行、链路到此为止 ✗

**证据**：插桩后打包运行，`_passWindowHandle`（sdl_layout 那份）的 entry print 一次都不出现，
而调用它的三个包装函数（`_layout_pass_window` / `passWindowHandle`）自己的 entry/return print 都正常。

**处置**：把它改名成**模块树内唯一**的 `_lay_dispatch_handle`（定义 + 2 处调用点），
并在函数上方写了"函数名必须全模块树唯一"的注释。

> 加一次探针即确认：把同款包装**另起一个名字**（`passWindowHandle2`）后，
> 两个包装的 print 都正常，而它们内部对 `_passWindowHandle` 的调用依然无声 ⇒ 名字本身是变量 ✓

> ⚠ **2026-10-10 复核（治本时做的）**：本条描述的"顶层自由函数被绑到同名方法上"**未能复现** ✗。
> 两个最小复现（`sdl_layout` 那种形状：自由函数 `f(a,b)` + 另一个模块里的方法 `T.f(a)`，
> 调用点在**同模块的另一个自由函数**体内）在 `.leno` 直跑与单文件 `.lenb` 两条路上**行为都正确** ✓；
> 而且真要按这条路径走，编译器会当场报「调用方法 'x' 时参数过多」**而不是静默** ✗ —— 与本节的
> "不报错、不执行"矛盾 ✓。⇒ 当时治好打包态的，**应当就是根因 ②（拆 case）** ✓ —— 这也与
> 本节自己的 A/B（`LENO_NO_DCE=1` 一开就好 ✓）方向一致 ✓。§九 记了复核时**另外**抓到并修掉的那个
> 真·同名 bug（已修 ✓）。

---

## 五、根因 ②：合并 `case is A, B, C` 的动态派发被 **DCE 剪成空桩**

句柄下发路径上到处是**合并 case**（这也是它当初的写法）：

```leno
switch c {
    case is HBox, VBox, AnchorBox { c._bind_window(hwnd); return }
    case is Table, TreeView, ListBox { c.set_window_handle(hwnd); return }
    …
}
```

合并 case 下变量收窄不出具体类型（等价 `any`）⇒ `c._bind_window(...)` 退化成
**按名动态派发** ⇒ `src/dce.c` 只在"名字被通配引用 **且** 所属类型被引用"时才保活：

```c
// src/dce.c → resolve_wildcard_methods()
if (s_wild_safe || strset_has(&s_used_types, s_funcs[idx].type_name)) mark_live(idx);
```

于是入口 `.lenb`（**唯一开 DCE 的产物**）把这些方法判成死代码，写成
`<dce-cut>` **空桩**（`serialize.c` 的 `CONST_TAG_DEAD_FUNCTION`：空常量池 + 一条 `OP_RETURN`，
语义 = 调用方拿到 null、**静默 no-op**）✗

**证据**：`LENO_DCE_VERBOSE=2` 打包应用，裁剪清单里正好是被依赖的那些：

```
[DCE] cut: _bind_window [ComboBox] / [VBox] / [AnchorBox] / [TabControl] / [SpinBox]
[DCE] cut: _bind_window [GridLayout] / [ScrollView] / [GroupBox] / [Splitter] / [Canvas]
```

**A/B 判定**：

| 打包时 | 结果 |
|---|---|
| 默认（开 DCE） | ❌ 链路断，`Edit.set_window` 不被调用 |
| `LENO_NO_DCE=1` | ✅ 链路立刻恢复 |
| `LENO_DCE_WILD_SAFE=1` | ✅（同理：名字通配不再按类型过滤） |

**处置**：把句柄下发路径上的合并 case **拆成单类型 case**（每支一个类型）：

```leno
case is HBox { c._bind_window(hwnd); return }
case is VBox { c._bind_window(hwnd); return }
case is AnchorBox { c._bind_window(hwnd); return }
```

单类型 case 收窄成具体类型 ⇒ **静态直接调用** ⇒ 引用被 DCE 看见 ⇒ 不会被剪 ✓

> ⚠ 单类型 case 是**静态**调用 ⇒ **被调类型必须在调用方文件里可见**。
> 拆的时候发现 `sdl_layout.leno` 里原先那句合并 case 里的 `ListBox` 根本没被 import：
> 它在合并 case 里"看起来能编译"，实际是动态派发**从没真正命中过**
> （`ListBox.set_window_handle` 一直没被执行 ⇒ 它的右击菜单拿不到句柄）。
> 故本次补了 `import "sdl_listbox.leno" as lstlib; use lstlib.ListBox` ✓

---

## 六、本次改动清单（模块侧，已随本记录一起提交）

`build/leno_module/LenoSDL3/lib/`

| 文件 | 改动 |
|---|---|
| `sdl_layout.leno` | ① `_passWindowHandle` → 改名 `_lay_dispatch_handle`（+ 唯一性注释）；② 补 `import "sdl_listbox.leno" as lstlib; use lstlib.ListBox`；③ 句柄分发 switch 的合并 case 拆成单类型 case |
| `sdl_window.leno` | `Window._passWindowHandle`、`_refreshWindowHandle`、`setExtraPassWindow` 回调、`Window.add`、`Window.addZ` —— 五处合并 case 全部拆成单类型 case（+ 注释说明为什么不能写合并 case） |

> ⚠ **2026-10-10 更新**：上表里的"**拆成单类型 case**"已在编译器治本（§九）之后**全部改回合并写法** ✓ ——
> 框架是门面代码，就按最自然的写法写 ✓（`case is HBox, VBox, AnchorBox { ... }` ✓），
> 让读代码的人/AI 知道这种写法现在是被正确支持的 ✓。
> 同理，`sdl_layout` 里那个为绕行而改的名字也**已改回 `_passWindowHandle`** ✓（与
> `Window._passWindowHandle(Widget)` 同名 ✓ —— 两者参数个数不同，编译器现在会正确区分 ✓，
> 改回后再跑同一条端到端探针：`.lenb` 里仍 `null=false` ✓ 链上零裁剪 ✓）。

验证（**开 DCE 的默认打包流程**，真实应用）：

```
[probe] Window.setRootAt 进入 type(root)=VBox
[probe] Edit.set_window null=false      ×N     ← 页签里的搜索框拿到句柄 ✓
[probe] Window.setRootAt 返回
```

最小复现（Edit 在 TabControl 布局页里）打包后：

```
[probe] SetTextInputArea rect=(12,17,876,19) ret=true
```

（探针已全部撤除，工作区只剩上面的正式改动 ✓）

---

## 七、建议的治本改动（编译器侧 —— **2026-10-10 已做**，见 §九）

1. **`src/dce.c` → `resolve_wildcard_methods()`**：名字通配的方法**不要再按"所属类型是否被引用"过滤**
   —— 即把 `LENO_DCE_WILD_SAFE=1` 的语义变成默认。
   理由：**动态派发在静态上不可判定**，类型过滤只是启发式；误剪的代价是"静默 no-op"（比崩溃更难查），
   而多留几个函数体只是体积。
2. **`codegen`（多类型收窄处，`codegen_expr.c` 的 `dce_note_method_ref(...)` 调用点）**：
   合并 `case is A, B` 收窄后按名调用方法时，应把 A/B **也记进类型引用**（`dce_note_type_ref`），
   或者干脆对"多类型收窄"退化成**逐类型的静态分支**（等价于本节模块侧的做法，由编译器自动完成）。
3. （更彻底）合并单元里的**同名符号解析应按模块作用域**，而不是全局同名 —— 见下节清单。

---

## 八、自查清单（以后再遇到"只有打包才犯"的怪事，照这个顺序走）

1. `build\leno.exe -c x.leno` + `build\leno_vm.exe x.lenb` —— **先复现，不必真打包** ✓
2. `LENO_DCE_VERBOSE=2` 打包，grep `cut:`，看有没有你**依赖的函数/方法**被剪
3. `LENO_NO_DCE=1` / `LENO_DCE_WILD_SAFE=1` 做 A/B：能好 = **DCE 误剪**
4. 全模块扫一遍「**顶层函数名 = 某类型的方法名**」的同名撞车（本次扫描结果见下表）
5. 涉及 IME / 右击菜单 / 弹层这类"要窗口句柄"的能力：在 `Edit.set_window` 与
   `SDL_SetTextInputArea` 调用点临时插桩，先确认 `hwnd` 是不是 null（本 bug 的现象全由它派生）

### 附：同类同名撞车清单（本次全模块扫描所得，均**只在打包 / 单文件时**暴露）

| 顶层函数（所在文件） | 同名方法 |
|---|---|
| `init`（`sdl_core.leno`） | `Event.init`（`sdl_event.leno`） |
| `setRoot` / `setRootAt`（`SDL3.leno` 包装） | `Window.setRoot` / `Window.setRootAt` |
| `createRenderer`（`sdl_renderer.leno`） | `Window.createRenderer` |
| `dumpLayout`（`sdl_window.leno` 顶层） | `Window.dumpLayout` |
| `load`（`sdl_image.leno`） | `Font.load`（`sdl_font.leno`） |
| `dispose`（`sdl_label.leno`） | `Alert.dispose`（`sdl_alert.leno`） |
| `applyTheme`（`sdl_treeview.leno`） | `Canvas.applyTheme`（`sdl_canvas.leno`） |
| `isWindowMaximized`（`sdl_core.leno`） | `Event.isWindowMaximized`（`sdl_event.leno`） |
| `_ensureFont`（`sdl_tooltip.leno`） | `Drag._ensureFont`（`sdl_drag.leno`） |
| `_passWindowHandle`（`sdl_layout.leno`） | `Window._passWindowHandle` → **已改回同名 ✓**（2026-10-10 ✓ 两者参数个数不同 ⇒ 编译器正确区分 ✓，本条不再是撞车 ✗）|

> 结论：**LenoSDL3 里"顶层函数名"与"方法名"必须全模块树唯一**。
> 建议以后加个轻量自检（扫 `^(export )?func NAME(` 与缩进 `func NAME(` 求交集）钉在模块回归里。
>
> ⚠ **2026-10-10 放宽**：编译器治本之后，这条只在"**形参个数也相同**"时还需要 ✓ ——
> 个数不同的撞车（如 `f(a,b)` vs `T.f(a)`）现在能正确区分到**自由函数** ✓（见 §九）。
> 个数相同时仍按"方法优先"（保留兼容 ✓）并由编译器给「同名歧义 / 疑似无限递归」告警 ✓，
> 那种情形**建议仍显式写 `self.x(...)` 或改名** ✓。

---

## 九、编译器侧治本记录（2026-10-10 补做）

### 9.1 复核：① 未能复现，② 复现并修好

- **① 未能复现** ✗：照本文§四的形状做了两个最小复现，`.leno` 直跑与单文件 `.lenb` 两条路**都正确** ✓；
  且那条路径若真命中，编译器会**当场报"参数过多"**（响的 ✗）而不是静默 ✓ ⇒ 与§四的观测不符 ✓。
  结论：当时治好打包态的是 **②**，① 是对 ② 的误诊 ✓（§四 已就地补注 ✓）。
- **② 复现成功** ✓：最小复现（两个 struct 各有方法 `ping` + 合并 `case is A1, A2 { c.ping() }`，
  编译成入口 `.lenb` 后用 VM 跑）⇒ 修前 `A2.ping` 被剪成空桩、**静默 no-op** ✗（连 case 里的
  `return` 都照走 ✓），`LENO_DCE_VERBOSE=2` 里正是 `[DCE] cut: ping [A2]` ✓。

### 9.2 改了什么（两件，互补）

| # | 位置 | 改动 |
|---|---|---|
| ②-a | `src/codegen/codegen_stmt.c`（`gen_switch`） | `case is` 每个类型都 `dce_note_type_ref`（**知道类型**✓）；合并 case（`match_type_count > 1`）的 case 体内打开"多类型"区间 ✓ |
| ②-b | `src/dce.c`（`dce_note_method_ref`） | 多类型区间内的方法引用**额外按名字通配**登记一次 ✓ ⇒ 结合 ②-a 的类型记录 ⇒ 该 case 涉及类型的同名方法都被保活 ✓（精度不塌 ✓） |
| ②-c | `src/dce.c`（`dce_init`） | "名字通配"默认改成**安全档** ✓（原启发式可用 `LENO_DCE_WILD_STRICT=1` 复原做 A/B ✓）|
| ①-a | `src/semantic/semantic_visit_method.c` | 裸名调用被"同名方法优先"改写前加**形参个数闸门** ✓：实参**多于**方法形参 ⇒ 铁定不是方法 ⇒ 不劫持 ✓（零风险：默认参数只会让实参更少 ✓）；实参更少且同名函数个数正好吻合 ⇒ 让位给函数 ✓ |

> ① 的真实症状（复核时抓到 ✓）：**方法体内的裸名调用**若与 struct 的某个方法同名，
> 会被无条件改写成"调那个方法" ✗ ⇒ 个数不符时**把正确写法挡在编译期** ✗、个数吻合时
> **静默调错目标** ✗。修前实测报错原文：`调用方法 'poke' 时参数过多: 最多 1, 实际 2` ✓。
> 顺带修掉一个隐蔽坑：方法名/个数表必须在"逐个方法补隐式 `self`"**之前**建 ✓ ——
> 否则读到的 `pcnt` 是 +1 后的值（实测 `poke(Widget c)` 被读成 2 ✗）⇒ 闸门失效 ✓。

### 9.3 验证（每条都实测）

| 项 | 结果 |
|---|---|
| ② 最小复现（默认档） | 修前静默 ✗ → 修后 `[A1] ping` + `[A2] ping` **都跑** ✓ |
| ② 严格档（`LENO_DCE_WILD_STRICT=1`） | 同样都跑 ✓ ⇒ 说明 ②-\\a/②-b 的"精确半"已足够 ✓ |
| **体积代价** | 飞机大战 `.lenb`：严格档 588737 B / 默认档 588737 B ⇒ **0 B（0%）** ✓ |
| ① 撞车复现 | 修前报"参数过多" ✗ → 修后走**自由函数** ✓（`[free] poke h=5` ✓）|
| ① 防回归：默认参数的方法调用 | 裸写 `scale(3)`（方法声明 2 参 + 1 默认）仍走**方法** ✓（`a=3 b=10` ✓）|
| ① 防回归：诊断告警 | "裸名纯转发 ⇒ 疑似无限递归"告警照旧触发 ✓（文字未改 ✓）|
| 仓库断言套件 | **432 passed / 0 failed** ✓ |
| 编译电池（框架 5 + 游戏 2 + 应用 3） | 10/10 OK ✓ 0 警告 ✓ |
| **端到端（决定性）** | 把§六的绕过**改回绕前写法**（合并 `case is HBox, VBox, AnchorBox` ✓）+ 在 `Edit.set_window` 插探针 ✓<br>⇒ 新编译器的入口 `.lenb` 里 `[probe] Edit.set_window null=false` ✓、裁剪清单无 `_bind_window` ✓<br>⇒ **那个绕过不再需要** ✓ |
| **正式回归（改回合并写法之后）** | 框架 5 处合并 case 已**正式改回** ✓（sdl_layout 1 处 + sdl_window 5 组 ✓），再跑一次同一探针 ⇒ `.lenb` 里仍是 `null=false` ✓、链上零裁剪 ✓；<br>断言套件 **432 passed / 0 failed** ✓；编译电池（框架 5 + 用例 1 + 游戏 2 + 应用 2）**10/10 OK、0 警告** ✓ |

### 9.4 未做 / 保留

- §七建议 3（合并单元按模块作用域解析）暂**不需要** ✓：本次两个病根都在"按名/按类型"的
  启发式与改写上 ✓，与模块作用域无关 ✓。
- "同名且**形参个数相同**"的裸调用仍按**方法优先**（兼容 ✓）+ 告警 ✓ —— 口径未改 ✓；
  框架里那条"函数名全模块树唯一"的规矩**可以在个数不同时放宽** ✓（§八 已注 ✓）——
  本仓已据此把当初的绕行**改名改回同名** ✓（`sdl_layout._passWindowHandle` ✓，改动与复验见 §六 更新 ✓）。
