# SDL3 控件优化清单

> 审查范围：`leno_module/LenoSDL3/lib/sdl_*.leno`（42 个控件文件）
> 生成时间：2026-08-23

---

## 🔴 高优先级（性能影响大）

### 1. ListBox `_is_selected()` O(n²) → O(n)

**文件**: `sdl_listbox.leno`

**问题**: `render()` 循环每行调用 `_is_selected(i)`，内部线性扫描 `_sel` 数组。多选模式 + 大量选中项时整体为 O(n×m)，即 **O(n²)**。

**方案**: 维护 `Array[bool] _selMap`（按索引直接查 true/false），在选中/取消时同步更新，将 `_is_selected()` 降为 O(1)，整体 render 降为 O(n)。

---

### 2. ComboBox `_get_tex()` 线性扫描 → Dict 缓存

**文件**: `sdl_combobox.leno`

**问题**: 纹理缓存用两个平行数组 `_imgCachePaths` / `_imgCacheTex` 做线性查找 O(n)。每次渲染选中项图标 + 弹层每项图标都调用，项数多时性能差。

**方案**: 用 `Dict[string, Ptr[u8]]` 做缓存，O(1) 查找。

---

### 3. Edit `_compute_sb()` 结果缓存

**文件**: `sdl_edit.leno`

**问题**: `_sb_width()` 和 `_sb_height()` 各自调用 `_compute_sb()`，而 `process` 和 `render` 中多处分别调用 `_sb_width()` / `_sb_height()`，导致同一帧内重复计算（涉及 `_content_height()` → `_ensure_layout()` 等重操作）。

**方案**: 仿照 `sdl_table.leno` 的做法，缓存 `_compute_sb()` 的结果到 `[float, float] _sbCache`，加 `_sbDirty` 脏标志，在文本/尺寸/字体变化时置脏，下次 `_compute_sb()` 时才重算。

---

### 4. Chart 标签测量缓存

**文件**: `sdl_chart.leno`

**问题**: `BarChart.render` 每帧对 `cats` 逐个 `f.measureString()` 求最大宽度；`LineChart` 同理对 `xlabels`。数据量大时每帧重复测量。

**方案**: 在 `set_data` / `render` 首次时预计算标签最大宽度并缓存到字段，加脏标志。

---

## 🟡 中优先级（代码质量/可维护性）

### 5. Button.render 中 `measureString(text)` 被调用两次

**文件**: `sdl_button.leno` 第 431 行和第 509 行

**问题**: 第 431 行测量了 `text` 供图标尺寸自适应，第 509 行又测量了同一个 `text`，重复。

**方案**: 复用第一次测量的结果。

---

### 6. ListBox/ComboBox 循环内 `measureString("Ag")` 提取

**文件**: `sdl_listbox.leno` 第 366 行、`sdl_combobox.leno` 第 472 行

**问题**: 在 `for items.len()` 循环内每行都调用 `_font.measureString("Ag")` 获取行高，但这个值对同一字体是常量。

**方案**: 在循环外测量一次并缓存到局部变量。

---

### 7. ProgressBar hover 逻辑修正

**文件**: `sdl_progress.leno` 第 124-130 行

**问题**: 第 124 行 `if ev.isMouseMotion()` 块中先设 `_wasHover = hit`（而不是保存旧值 `_wasHover = hover`），导致 hover 状态变化检测失效——永远 `_wasHover == hover`。

**方案**: 修正为 `_wasHover = hover; hover = hit`。

---

### 8. ScrollView `_sync()` 帧内去重

**文件**: `sdl_scrollview.leno` 第 268 行和第 336 行

**问题**: `_sync()` 在 `process` 和 `render` 中各调用一次，做了内容尺寸测量、滚动夹紧、拇指几何计算、子控件定位等，重复计算。

**方案**: 加帧标志 `_syncedFrame`，一帧内只计算一次。

---

### 9. `measureString("Ag")` 缓存到字段

**文件**: `sdl_checkbox.leno`、`sdl_radio.leno`、`sdl_toggle.leno`、`sdl_slider.leno` 等

**问题**: 每次渲染都调用 `_font.measureString("Ag")` 获取字体行高，但字体不变时这个值是常量。

**方案**: 在 `_ensure_font()` 中缓存行高到 `_fontH` 字段（参考 `sdl_edit.leno` 已有的 `_lineHeight` 缓存做法）。

---

## 🟢 低优先级（改进建议）

### 10. Slider `process` 中变量遮蔽

**文件**: `sdl_slider.leno` 第 141 行

**问题**: 第 135 行已有 `float mx = ev.mouseX(), my = ev.mouseY()`，第 141 行 `if ev.isMouseMotion()` 块内又声明了同名局部变量 `mx` / `my`，遮蔽外部变量。

**方案**: 移除内部的重复声明，直接复用外部变量。

---

### 11. 重复的 `_ensure_font()` 模式

**文件**: 几乎所有控件

**问题**: 每个控件都写了近乎相同的 `_ensure_font()` 函数（约 6-8 行代码重复）。

**备注**: Leno 无 mixin/继承字段机制，此重复暂时可接受。如需改进，可在 `sdl_font.leno` 中提供工具函数。

---

### 12. 三个 Chart 的 `_step_anim()` 代码重复

**文件**: `sdl_chart.leno`

**问题**: `BarChart._step_anim()`、`LineChart._step_anim()`、`PieChart._step_anim()` 逻辑几乎一致。

**备注**: 由于 Leno 泛型限制，不易统一。当前重复可接受。

---

## 修复进度

| # | 优先级 | 问题 | 状态 |
|---|--------|------|------|
| 1 | 🔴 | ListBox `_is_selected` O(1) | ✅ 已修 |
| 2 | 🔴 | ComboBox `_get_tex` Dict 缓存 | ✅ 已修 |
| 3 | 🔴 | Edit `_compute_sb` 缓存 | ✅ 已修 |
| 4 | 🔴 | Chart 标签测量缓存 | ✅ 已修 |
| 5 | 🟡 | Button measureString 去重 | ✅ 已修 |
| 6 | 🟡 | ListBox/ComboBox 循环内 measureString 提取 | ✅ 已修 |
| 7 | 🟡 | ProgressBar hover 逻辑修正 | ✅ 已修 |
| 8 | 🟡 | ScrollView _sync() 帧内去重 | ✅ 已修 |
| 9 | 🟡 | measureString("Ag") 缓存到字段 | ✅ 已修 |
| 10 | 🟢 | Slider 变量遮蔽 | ✅ 已修 |

---

## 第二轮审查（2026-08-27）：仍未优化的控件

> 对 `lib/sdl_*.leno` 全部 42 个控件逐一复查渲染热路径后，发现以下遗漏点。

### 🔴 高优先级

#### A. Menu `_get_tex()` 线性扫描（无 MRU 快路径）
**文件**: `sdl_menu.leno` 第 674-680 行
**问题**: 与第 2 项同类反模式。`_drawPanel` 循环内对每个菜单项调用 `_get_tex`，内部 `for _imgCachePaths.len()` 线性扫描（O(n)）。Table（1629）/TreeView（231）已加 `_imgLastPath`/`_imgLastTex` 单槽 MRU 快路径降为 O(1)，Menu 未跟进。菜单项图标多时（文件管理器右键菜单等）每帧重复 string 比较。
**方案**: 对齐 Table/TreeView，加 `_imgLastPath`/`_imgLastTex` 单槽快路径；或直接改 `Dict[string, Ptr[u8]]`（与 ComboBox 一致）。

#### B. Menu 循环内 `measureString("Ag")` 未提取
**文件**: `sdl_menu.leno` 第 630 行、第 755 行
**问题**: 与第 6 项同类反模式。`render()` 在标题循环内、`_drawPanel()` 在菜单项循环内，每项都调 `measureString("Ag")` 取行高——同字体下是常量。ListBox/ComboBox 已修，Menu 遗漏。
**方案**: 在 `_ensure_font()` 中缓存 `_fontH`（对齐 CheckBox/Radio/Toggle），循环内直接读字段。

#### C. Chart 每帧 Y 轴刻度标签测量
**文件**: `sdl_chart.leno` 第 164-169 行（BarChart）、第 436 行（LineChart）
**问题**: 第 4 项只缓存了类目轴标签最大宽度，但 Y 轴刻度标签 `maxYLabW` 每帧重新对 `ticks+1` 个标签调 `measureString`。`ymax` 仅在数据变化时才变。
**方案**: 加 `_ymaxDirty` 脏标志，`set_data` 时置脏；`_niceMax(vmax)` 结果 + `maxYLabW` 一起缓存，ymax 不变时直接复用。

#### D. Chart 类目标签单宽未缓存（每帧逐个测量）
**文件**: `sdl_chart.leno` 第 238/250/259/273 行（BarChart）、第 468/481 行（LineChart）
**问题**: 第 4 项只缓存了 `_maxCatW`（最大宽度，用于抽稀步长），但绘制每个标签时仍每帧 `measureString(cat)` 取该标签宽度做居中对齐。类目多时每帧 O(n) 次测量。
**方案**: 在 `_catDirty` 失效时一并缓存 `Array[float] _catW`（每个类目标签宽度），绘制时直接读数组。

### 🟡 中优先级

#### E. Edit `_render_sl`/`_render_ml` 仍每帧 `measureString("Ag")`
**文件**: `sdl_edit.leno` 第 1539、1673、1696 行
**问题**: 第 9 项已为多数控件缓存 `_fontH`，Edit 也有现成的 `_lineHeight` 字段（第 40 行，`_ensure_font` 第 124 行已赋值），但渲染路径却没用，仍每帧调 `measureString("Ag")` 取行高画光标。三处遗漏。
**方案**: 用 `_lineHeight`（或新增 `_fontH = _lineHeight - 2.0`）替换这三处的 `measureString("Ag")`。

#### F. ComboBox `render`/`renderPopup` 每帧 `measureString("Ag")`
**文件**: `sdl_combobox.leno` 第 404、446 行
**问题**: 第 6 项把循环内的提到循环外（446 行注释“循环外测量一次”），但每帧仍调一次；`render()` 第 404 行同样每帧调。未缓存到字段。
**方案**: 在 `_ensure_font()` 中缓存 `_fontH`，两处直接读字段。

#### G. Tab 每帧逐个标签 `measureString`
**文件**: `sdl_tab.leno` 第 341 行
**问题**: `render()` 循环内对每个标签调 `measureString(_tabs[i])` 取宽度做居中。标签文本几乎不变，每帧重复测量。
**方案**: 加 `_tabWDirty` 脏标志 + `Array[float] _tabW` 缓存，`add_tab`/`remove_tab`/`set_font_size` 时置脏；或对齐 Table `_fittedW` 模式。

#### H. Label 普通绘制每帧 `measureString(text)`
**文件**: `sdl_label.leno` 第 198、215、258 行
**问题**: `render()` 三种模式（截断/滚动/普通）每帧都对完整 `text` 调 `measureString`。Label 的 `set_text` 已把宽度缓存到 `_w`，但渲染对齐时又重测。文本不变时浪费。
**方案**: 加 `_textW` 字段，`set_text`/`set_font_size` 时一并缓存，渲染对齐直接读 `_textW`。

#### I. Label 截断模式逐字符测量无缓存
**文件**: `sdl_label.leno` 第 127-159 行 `_truncatedText()`
**问题**: 截断模式下每帧调用 `_truncatedText()`，内部对每个字符 `slice` + `measureString`（O(n) 次测量 + n 次 slice 分配）。文本/max_w/字体不变时结果相同，每帧重算开销大。
**方案**: 缓存截断结果到 `_truncText`，加 `_truncDirty` 脏标志（text/max_w/font 变化时置脏）。

#### J. Button 每帧 `measureString(text)`
**文件**: `sdl_button.leno` 第 420 行
**问题**: 第 5 项只去掉了 render 内部的二次测量重复，但首次测量本身仍每帧执行。按钮文本通常静态。
**方案**: 加 `_textW`/`_textH` 缓存 + `_textDirty`，`set_text`/`set_font_size` 时置脏。影响小，低优先级即可。

### 🟢 低优先级（可选）

#### K. GroupBox 每帧 `measureString(_title)`
**文件**: `sdl_groupbox.leno` 第 156 行
**问题**: 标题文本几乎不变，每帧重测。
**方案**: 加 `_titleW` 缓存，`set`/`set_title`/`set_font_size` 时更新。

#### L. 标题栏 `getWindowSize` 每帧查询
**文件**: `sdl_titlebar.leno` 第 515-518 行
**问题**: `render()` 每帧调 `core.getWindowSize(_winHandle)` 同步宽度。窗口大小仅在 resize 事件时变。
**方案**: 由 Window 在 resize 时回调 `titlebar.onResize(w,h)` 更新，render 不再查询。可选。

### 复查确认（已优化、无遗漏）
- ✅ ListBox `_is_selected` O(1)（`_selMap`）
- ✅ Table / TreeView `_get_tex` MRU 快路径 + 可见行区间遍历 + 拟合文本缓存
- ✅ ScrollView `_sync()` 帧内去重（`_frameSynced`）
- ✅ CheckBox/Radio/Toggle/Slider/Progress `measureString("Ag")` 已缓存 `_fontH`
- ✅ ImageView/Titlebar/Button 纹理按需加载并缓存
- ✅ Spinner/Spinbox/Splitter/Panel/Scrim/Separator/Separator/LinkButton 渲染路径无重复测量
- ✅ ProgressBar hover 逻辑已修正；百分比文本每帧变化无法缓存（可接受）
- ✅ Slider 数值标签每帧变化无法缓存（可接受）

## 第二轮修复进度

| # | 优先级 | 问题 | 状态 |
|---|--------|------|------|
| A | 🔴 | Menu `_get_tex` MRU/Dict 缓存 | ⬜ 待修 |
| B | 🔴 | Menu 循环内 `measureString("Ag")` 提取 | ⬜ 待修 |
| C | 🔴 | Chart Y 轴刻度标签测量缓存 | ⬜ 待修 |
| D | 🔴 | Chart 类目标签单宽缓存 | ⬜ 待修 |
| E | 🟡 | Edit render 复用 `_lineHeight` | ⬜ 待修 |
| F | 🟡 | ComboBox `measureString("Ag")` 缓存到字段 | ⬜ 待修 |
| G | 🟡 | Tab 标签宽度缓存 | ⬜ 待修 |
| H | 🟡 | Label `measureString(text)` 缓存 | ⬜ 待修 |
| I | 🟡 | Label `_truncatedText()` 结果缓存 | ⬜ 待修 |
| J | 🟢 | Button `measureString(text)` 缓存 | ⬜ 待修 |
| K | 🟢 | GroupBox 标题宽度缓存 | ⬜ 待修 |
| L | 🟢 | Titlebar `getWindowSize` 改事件回调 | ⬜ 待修 |
