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
