# LenoSDL3 打包后「窗口句柄下发」整条静默失效排查记录

> **一句话**：`-p --onefile` 打包后，`Edit` 拿不到窗口句柄（`hwnd == null`）⇒
> `SDL_SetTextInputArea` 从不调用 ⇒ **中文输入法的候选框跑到窗口最左上角**；
> `.leno` 直跑完全正常。根因是**两个各自独立、又叠在一起**的问题：
> ① 跨模块**同名符号**在"合并成一个编译单元"时串了；
> ② 句柄下发路径上的**合并 `case is A, B, C`** 退化成按名动态派发，被入口 `.lenb` 的 **DCE 剪成空桩**。

- 日期：2026-10-10
- 触发方：`LenoMusic`（音乐下载器）打包版
- 涉及模块：`build/leno_module/LenoSDL3`（本次已改：`sdl_layout.leno` / `sdl_window.leno`）
- 状态：**模块侧已修并验证**；**编译器侧治本未做**（见末节「建议的治本改动」）

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

## 七、建议的治本改动（编译器侧，未做）

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
| ~~`_passWindowHandle`（`sdl_layout.leno`）~~ | ~~`Window._passWindowHandle`~~ → **本次已改名 ✓** |

> 结论：**LenoSDL3 里"顶层函数名"与"方法名"必须全模块树唯一**。
> 建议以后加个轻量自检（扫 `^(export )?func NAME(` 与缩进 `func NAME(` 求交集）钉在模块回归里。
