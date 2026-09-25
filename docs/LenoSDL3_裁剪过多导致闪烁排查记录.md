# LenoSDL3 裁剪（clip）与闪烁排查记录

> 背景：复刻 TraeTools 界面（`leno_gui/控件/数据看板/dashboard_sidebar.leno`）时，新加的
> 用量统计 / 账号切换 / 云端签到 / 设置几个页面**整页在闪**。此前在别的界面上也遇到过同一症状，
> 当时的结论是一句话：**"裁剪过多会闪烁"**（但没有留下文档 ⇒ 本文件就是补这份记录）。
>
> 状态（2026-09-25）：按"减少裁剪层数 / 减少每帧分配"改造后**明显变好**，但**仍有残留闪烁**，
> 残留部分在无头（dummy）与真实驱动（D3D）下**连续帧逐像素比对均为 0 差异** ⇒ 未能复现，
> 见文末「未解决」。本文件前半部分是**已验证的机制与修复**，后半部分是**待确认项**。

---

## 1. 结论（先看这条）

`pushClipRect` / `popClipRect` **每次调用都会 `_flushBatches()`**，且**嵌套时还要 malloc 一个
交集 rect**。所以"每帧的裁剪层数/次数"会直接变成"每帧的 flush 次数 + 分配次数"：

| 每帧成本 | 来源 |
| --- | --- |
| 1 次 batch flush（把已入队的图元提交掉） | 每个 `pushClipRect` **和** `popClipRect` 各一次 |
| 1 次 `SDL_Rect.malloc()` + `free()` | 嵌套 push 时的交集 rect；只要控件 render 就分配 |
| 1 次 `SDL_SetRenderClipRect` | 每次 push/pop |

再叠加"clip 矩形按 `_int()` 截断（丢亚像素）"，表现就是**边缘/局部一帧有一帧无 = 闪**。

**规则：裁剪按需用 —— 能裁的才裁，子控件按构造不会溢出的一律 `clip:false`。**

---

## 2. 机制与证据（都能在代码里对上）

### 2.1 每次 push/pop 都 flush

`sdl_renderer.leno` 的裁剪栈（写作时约 516 / 549 行）：

```leno
func pushClipRect(Ptr[u8] rect): bool {
    _flushBatches()          // ★ 每次 push 先提交批处理（批处理是"当前 clip 下"的图元）
    _clipStack.add(_bxClipPtr)
    ...
}
func popClipRect(): bool {
    _flushBatches()          // ★ pop 同样
    ...
}
```

设计上这是**对的**（批处理里的图元必须带着当时的 clip 提交）；代价是 **裁剪 = 批处理断点**。
所以"层数多"不是唯一的坑，**"次数多"同样是坑**：一个页面里 10 个面板各自 `clip:true`
⇒ 每帧多 20 次 flush，即使它们互不嵌套。

### 2.2 嵌套 push 会额外 malloc 交集 rect

```leno
// pushClipRect 中：已有 clip 时计算交集
if _bxClip and _bxClipPtr != null {
    SDL_Rect inter = SDL_Rect.malloc()        // ★ 每帧每层一次分配
    ...
    _bxClipOwned = true
    return _lib.SDL_SetRenderClipRect(handle, inter.to_ptr())
}
```

面板是**每帧 render** 的 ⇒ 旧代码里"不管裁不裁都 `SDL_Rect.malloc()`"会在每帧产生纯浪费的分配
（本次已修，见 §4）。

### 2.3 clip 矩形按 `_int()` 截断 ⇒ 丢亚像素

所有控件都是这个写法（`sdl_panel.leno` / `sdl_groupbox.leno` / `sdl_listbox.leno` /
`sdl_table.leno` / `sdl_treeview.leno` / `sdl_tab.leno`）：

```leno
cr.x = _int(x); cr.y = _int(y)
cr.w = _int(w) + 1; cr.h = _int(h) + 1   // +1 防止浮点坐标截断丢失亚像素，避免子控件边缘闪烁
```

`+1` 是给 **1×** 打的补丁。开了 SSAA（`aa:true` / `aa:N`，默认关闭）时逻辑像素被放大 `N` 倍，
`+1` 物理像素 ≈ `1/N` 逻辑像素 ⇒ **不够**，滚动/亚像素移动时边缘会"一帧有一帧无"。
（SSAA 默认关；本界面未开，故不是本次的主因，但**开启 AA 的界面要留意这条**。）

### 2.4 模块自己定的判据：Tab 内容区内 2 层

`sdl_table.leno` 数据区注释（原文）：

> 数据区：bodyClip 单层裁剪。裁剪深度在 Tab 内容区内仍为 2 层（Tab + body），
> 且 push/pop 次数与旧"仅全表单层裁剪"版本相同——**不引入额外 flush，避免 D3D 闪烁**。

即：**Tab 容器 + 一个自裁剪控件（Table） = 2 层**是可接受的上限；再多（或"次数"再涨）就有风险。

### 2.5 另一条独立的闪烁源：文字纹理缓存淘汰

`sdl_font.leno` 注释（原文摘）：

> 文字纹理缓存容量。早期 64 槽在「表格 + 目录树 + 菜单 + 标签」同屏存在大量不重复文字时
> 会每帧淘汰/重建大量纹理，导致重绘（如鼠标移到其它控件、弹对话框）时**偶发卡顿/闪烁**。

现容量已提到 1024。⇒ 与本界面无关（可见文字远不到 1024），但它说明"闪烁"不止裁剪一个来源。
注意这条注释也点名了触发场景是**鼠标移到其它控件（hover 重绘）时**——与 §7 的残留症状吻合。

### 2.6 不必要的整窗重绘会放大一切

本项目示例为了**无头自检**曾经无条件挂一个 50ms 的 no-op 定时器：

```leno
wt.addTimer(50, func():void { })   // 只为了让 dummy 驱动也持续重绘
```

交互运行时它就是**每秒 20 次无谓的整窗重绘**（每次都带一批 flush）。
⇒ 应只在自检模式下挂（本次已修，见 §4）。

---

## 3. 本项目改造前后的裁剪层级

| 页面 | 改前 | 改后 |
| --- | --- | --- |
| 0 仪表盘（无 ScrollView） | Tab(1) + 卡片 Panel(2) + 图卡内… | **1**（卡片全部 `clip:false`） |
| 1/3/4/5 滚动页 | Tab(1) + ScrollView(2) + 卡片 Panel(3) + 卡内深色框(4) | **1**（只剩 ScrollView；卡片/框/胶囊全关） |

> `Tab.clip` 之所以能关：滚动页的内容**整页就是一个 ScrollView**，它自己会裁到视口 ⇒
> Tab 那层是**冗余**的（同矩形求交集，等于白付一次 flush + 一次分配）。

---

## 4. 已做的修复（全部 opt-in，默认行为逐像素不变）

| 文件 | 改动 | 为什么 |
| --- | --- | --- |
| `sdl_panel.leno` | 只在 `_clip` 为真时才 `SDL_Rect.malloc()`（`SDL_Rect?` + `cr?.free()`） | 面板每帧 render；`clip:false` 的面板不该每帧分配 ✗ |
| `sdl_groupbox.leno` | 同上 | 同上 |
| `sdl_tab.leno` | 新增 `clip` 选项（默认 `true` = 老行为）；`clip:false` 时不 push/pop、也不分配 | 页面自带裁剪时 Tab 层冗余 ⇒ 每页少一层 |
| `sdl_scrollview.leno` | 新增 `bar_color`；修 `set_content` **提前测量**（子控件还没 add ⇒ 量到 0 ⇒ 把滚动位置钳成 0 且回不去）；修 `_clampScroll` 在尺寸未测量前不钳位 | 与闪烁同源的"提前做错事"类问题（顺带修的） |
| `dashboard_sidebar.leno` | 19 个 Panel 全部 `clip:false`；Tab 传 `clip:false`；定时器只在自检模式挂 | 见 §3、§2.6 |
| `sdl_button.leno` / `sdl_edit.leno` | `align` / `border_color` / `padding`（按钮）、`border_color`（输入框） | 界面复刻需要，与裁剪无关，一并记录 |

新增的开关（写新界面时按需使用）：

- `Panel` / `GroupBox`：`clip:false` —— **默认就该优先考虑关**，只在子控件可能溢出时才开；
- `TabControl`：`clip:false` —— 内容自带裁剪（整页 ScrollView）时关掉；
- `ScrollView`：`bar_color` —— 浅色界面里滚动条别用深色主题色。

---

## 5. 写新界面时的自查清单

1. **先数层数**：这个页面最多会有几层 `pushClipRect`？（Tab / ScrollView / Panel / 表格…）
   目标：**≤1 层**；带 Tab 的老式布局**≤2 层**（§2.4）。
2. **面板默认 `clip:false`**：子控件位置是按父容器算出来的 ⇒ 只要不溢出就没必要裁。
   只有"内容高度/滚动位置由运行时决定"的容器（ScrollView、Table、ListBox、TreeView）才必须裁。
3. **别用定时器催重绘**：交互运行时 `win.run` 是事件驱动的；no-op 定时器 = 每秒 N 次全量重绘。
4. **别在父容器已裁的情况下再裁一次**：同矩形求交集是纯开销。
5. **开 SSAA（`aa`）时复查 clip**：`_int(w)+1` 在 N 倍缩放下不够（§2.3）。
6. **滚动/亚像素移动的场景优先怀疑裁剪**：位置带小数 ⇒ 边缘截断更容易"闪"。

---

## 6. 复现与测量方法（把"闪"变成数字）

本次给示例加了两个自检钩子（无头可跑）：

```bash
set SDL_VIDEODRIVER=dummy         # 无头；去掉这行就是真实驱动（D3D）
set DSHOT=%TEMP%\f.png            # 存 PNG 后退出
set DSHOT_AT=40                   # 从第 40 帧开始拍（等动画/首帧稳定）
set DSHOT_N=4                     # ★ 连拍 4 帧：f.png、f_1.png、f_2.png、f_3.png
set DPAGE=5 / DSCROLL=167         # 直接落到某个页面 / 某个滚动位置
leno.exe --no-cache dashboard_sidebar.leno
```

判据一（最快，够用）：**PNG 字节相同 ⇒ 帧完全相同**（同一编码器 + 相同像素 ⇒ 固定字节 ✓）

```powershell
# 三帧是否一模一样（MD5 相同即可）
Get-ChildItem f.png,f_1.png,f_2.png | Get-FileHash -Algorithm MD5 | Select-Object Hash, Path
# 或： fc /b f.png f_1.png      （cmd）
```

判据二（需要知道"差在哪"时）：抽样像素比对（每 4 像素取 1 ⇒ 1/16 像素量，秒级；纯 PowerShell，无外部依赖）

```powershell
Add-Type -AssemblyName System.Drawing
$a = [System.Drawing.Bitmap]::FromFile("f.png"); $b = [System.Drawing.Bitmap]::FromFile("f_1.png")
$n = 0; $minx = $a.Width; $miny = $a.Height; $maxx = -1; $maxy = -1
for ($y = 0; $y -lt $a.Height; $y += 4) {
  for ($x = 0; $x -lt $a.Width; $x += 4) {
    if ($a.GetPixel($x,$y).ToArgb() -ne $b.GetPixel($x,$y).ToArgb()) {
      $n++
      if ($x -lt $minx) { $minx = $x }; if ($y -lt $miny) { $miny = $y }
      if ($x -gt $maxx) { $maxx = $x }; if ($y -gt $maxy) { $maxy = $y }
    }
  }
}
"差异（抽样）= $n   位置 bbox=($minx,$miny)-($maxx,$maxy)"
```

（要"全像素 + 精确 bbox"可以用 System.Drawing 的 `LockBits` + `Marshal.Copy` 走 C#，
比 `GetPixel` 快两个数量级；日常排查用上面两个判据足够。）

**本次实测结果**（静态页面，内容不应有变化）：

| 场景 | 结果 |
| --- | --- |
| dummy 驱动，页 1，连拍 4 帧 | 每对 **0 像素差异** |
| 真实驱动（D3D），页 5，连拍 4 帧 | 每对 **0 像素差异** |
| 真实驱动（D3D），页 5 + 滚到底（`DSCROLL=999`→钳到 167） | 每对 **0 像素差异** |
| dummy 驱动，页 5 + 滚到底，连拍 3 帧 | 三帧 **MD5 相同**（字节级一致） |

⇒ **渲染内容是稳定的**：闪不出来。所以残留闪烁必然与"帧内容之外"的东西有关（见 §7）。
**局限**：`readPixelsAt` 读的是当前渲染目标（back buffer / SSAA 纹理），
**看不到**呈现（swap / vsync / 撕裂）层面的问题，也**无法合成鼠标事件**（模块未绑定
`SDL_PushEvent`）⇒ hover 路径复现不了。所以"帧相同"**不能**证明"用户看不到闪" ✗——
只能证明**画面内容本身没有逐帧变化**，把排查方向逼到呈现层/交互层 ✓。

---

## 7. 未解决（残留闪烁）与下一步

按 §6 的测量，残留闪烁**不是"帧内容一帧一变"**。按可能性排序：

1. **交互/hover 重绘路径**：`sdl_font.leno` 注释点名"鼠标移到其它控件时偶发卡顿/闪烁"。
   需要信息：**哪个页面、什么时候（移鼠标/滚动/静止）、局部还是整窗**。
2. **呈现层（撕裂）**：窗口默认 `_maxFPS = 30`（`sdl_window.leno` `setMaxFPS`），
   而渲染器可 `setVSync(0/1)`（见 `docs/LenoSDL3像素直写渲染优化.md`：曾主动关 vsync 提帧率）。
   30 FPS 节流 + vsync 关闭 ⇒ 帧呈现节奏不均/撕裂，在浅色大色块上**看着很像闪** ✗。
   候选动作：窗口创建后开 vsync，或把 `setMaxFPS` 提到显示器刷新率一起对齐。
3. **SSAA 下的 clip 截断**（§2.3）：本界面 `aa` 未开，故排除；但若把界面切到 `aa:2` 需复查。
4. **其它**：文字纹理缓存淘汰（§2.5）；控件内部每帧分配（可继续按 §4 的思路逐个清）。

**要回答的三个问题**（有信息才能继续定位，不然只能猜）：
① 哪个页面闪？② 什么时候闪（鼠标移过 / 滚动 / 静止）？③ 局部闪还是整窗白闪一下？

---

## 8. 附：滚动"不利索 / 有时跳一下"（帧节流 + 平滑滚动）

同一个"渲染节奏"家族的问题，一起记在这里。

### 8.1 根因：输入驱动的帧也被 30 FPS 节流卡住

`sdl_window.leno` 的主循环里：

```leno
if _hadEvent { _needRedraw = true }          // 滚轮/点击/键盘等非 motion 事件 ⇒ 请求重绘
...
if _needRedraw {
    int now = core.getTicks()
    if now - _lastRenderTicks >= _throttleMs {   // ★ 节流（默认 _maxFPS=30 ⇒ 33ms）
        ...渲染...
    }
}
```

节流的本意是压制**动画/鼠标移动**的 CPU 占用（这是对的 ✓），但套在**用户输入**上就变味了：

- 每个滚轮增量最多要等 33 ms 才落一帧；
- 连续滚动时多个增量会**攒进同一帧**一起生效 ⇒ 画面"一顿一顿、像跳一下" ✗。

**修法**：把"输入类事件"和"动画/motion"分开对待 —— 输入不等节流，立刻出一帧：

```leno
bool _inputEvent = false                                   // 本帧有滚轮/点击/键盘/resize
if not ev.isMouseMotion() { _hadEvent = true; _inputEvent = true }
...
if _inputEvent or now - _lastRenderTicks >= _throttleMs { ...渲染... }
```

（`_inputEvent` 与 `_hadEvent` 一样是"每帧局部"的 ⇒ 不会残留到下一帧 ✓）

### 8.2 附带修掉的一处脆弱约定

ScrollView 的滚轮分支原先**不请求重绘**，完全依赖主循环"非 motion 事件一律重绘"这个隐含约定 ✗。
一旦那条约定为了别的目的被改（例如收敛闪烁时把滚轮也归入"不动就不重绘"），滚轮就会**默默失效** ✗。
现在滚轮统一走 `scroll_by()`，里面 `core.markFrameRedraw()` ✓。

### 8.3 新增：平滑滚动（`smooth:true`）

瞬间跳一整格（默认 24px）本身就是"不利索"的来源之一 ⇒ 加了 opt-in 的平滑滚动：

| 新增 | 默认 | 说明 |
| --- | --- | --- |
| `ScrollView` `smooth` | `false`（老行为 ✓） | 滚轮设**目标位置**，`_sync()` 每帧按 40% 逼近 ⇒ ~150ms 到位 ✓ |
| `ScrollView` `step` | `48.0` | 每格滚轮步长（**原默认 24px 偏小** ⇒ 体感"滚了只走一点点、得使劲滚" ✗；48 ≈ Windows「3 行」惯例 ✓，可调 ✓） |
| `ScrollView.scroll_by(dx, dy)` | — | 公开方法：滚轮与程序化滚动**同一路径**（平滑逻辑只有一处 ✓，也可被自检驱动 ✓） |

**平滑的"落后上限"**：平滑再顺也不能让位置落后手指太多 —— 快速连滚时若还慢慢追，
体感就是"滚了半天只走一点点"✗。规则：**落后超过 2.5 格 ⇒ 整段立即到位** ✓

| 情形 | 行为 |
| --- | --- |
| 慢滚（1~2 格） | 平滑逼近（有"顺滑感" ✓） |
| 快速连滚（≥3 格落后） | **立即到位**（总距离一点不少 ✓，无滞后 ✓） |
| 反向 | **立即到位**（见 §8.4 极限环 ✓） |

实测（同一页面、同一自检）：

```
scroll_by(0, 48)  → y=0 → 19.2 → …（平滑）
scroll_by(0,144)  → y=144      （>2.5 格 ⇒ 一步到位 ✓）
```

两个关键实现细节（都踩过/有理由）：

1. **内容定位取整**：`_content.set_pos(x - _int(_scrollX), ...)` —— 分数偏移会让文字落在半像素上
   ⇒ 边缘发虚/闪 ✗（与裁剪截断同源 ✓）；取整后动画仍然是"逐像素推进"，观感不受影响 ✓。
2. **所有"立即到位"的路径都要取消动画**：`set_scroll` / `ensure_visible` / 拖动滚动条 /
   PageUp·PageDown·Home·End —— 否则残留的平滑目标会把视图**拽回去** ✗，
   这正是"来回跳一下"的另一种成因 ✓。

### 8.4 快速上下滚 ⇒ "抽搐"（极限环）——平滑滚动的坑，已修

**现象**：快速来回滚轮时，内容不是跟手地上下，而是像**抽搐**一样抖 ✗。

**实测（修复前）**：用自检每 2 帧反向滚 ±200，逐帧打印位置 ⇒ 位置**永不收敛**，
在 `53 ↔ 147` 之间**永久振荡**（极限环 ✗）：

```
y=128 → 76.8 → 46.08 → 107.6 → 144.6 → 86.8 → 52.1 → 111.2 → 146.7 → 88.0 → 52.9 → …（一直抖）
```

**两个成因（都不是"正常现象"✗）**：

1. **指数逼近只看位置**：用户一反向，`_target` 在抖、`_scrollY` 在追，两者相差一个固定相位
   ⇒ 形成闭环振荡，永远停不下来 ✗。
2. **`_sync()` 的帧内去重只看帧号**：同一帧里可能有**多个滚轮事件**（快速滚 ✗），
   去重导致渲染用的还是"第一个事件"留下的旧位置，其余要等下一帧 ⇒ 视觉滞后、一顿一顿 ✗。

**修法**：

| 位置 | 改动 |
| --- | --- |
| `ScrollView.scroll_by` | **方向反转 ⇒ 先立即到位**（`_scrollX/_scrollY = 目标`）再继续平滑 ⇒ 反向 1:1 跟手 ✓ |
| `ScrollView._sync` | 去重条件改成"帧号 **且** 位置未变"（位置变了就重做 ✓）；`firstThisFrame` 单独保证**动画每帧只推进一步** ✓ |

**实测（修复后）**：同一组 ±200 交替 ⇒ 每次反向都精确落到累计目标，不再振荡 ✓：

```
scroll_by(+200) → y=0    （之后平滑逼近）
scroll_by(-200) → y=200  （反向 ⇒ 立即到位 ✓）
scroll_by(+200) → y=0    （再反向 ⇒ 立即到位 ✓）
```

**结论**：平滑滚动做成了 opt-in（`smooth:true`），但仍然遵守两条硬规则 ——
**方向反转要立即到位**、**同一帧内的多次滚动都要生效**。
两条都不满足时，用户看到的就是"抽搐"，不是"平滑" ✗。

### 8.5 怎么验证平滑真的在动

新增自检开关（示例里）：

```bash
set DSCROLL_SMOOTH=200     # 以"滚轮语义"朝目标滚 200px（走平滑路径 ✓）
set DSCROLL_TRACE=1        # 逐帧打印滚动位置
set DSHOT=... DSHOT_AT=3 DSHOT_N=3
```

实测缓动曲线（每帧 40% 收敛 ✓）：

```
y=0 → y=80 → y=128 → y=157 → ... → 200（钳到 maxScrollY）
```

同一轮抓的三帧 **MD5 各不相同** ✓（说明动画期间帧确实在变 ⇒ 也可以反向验证 §6 的"0 差异"确实是"没变" ✓）。

---

## 9. 相关文件

- 复刻示例（本次的"案发现场"）：`leno_gui/控件/数据看板/dashboard_sidebar.leno`
- 渲染器裁剪栈：`build/leno_module/LenoSDL3/lib/sdl_renderer.leno`
- 各控件裁剪：`sdl_panel.leno` / `sdl_groupbox.leno` / `sdl_tab.leno` / `sdl_scrollview.leno` /
  `sdl_listbox.leno` / `sdl_table.leno` / `sdl_treeview.leno`
- 文字纹理缓存：`sdl_font.leno`
- 同类历史记录：`docs/LenoSDL3_Table方法表断裂编译器bug排查记录.md`（其中也提到"避免 D3D 闪烁"）
- 渲染/性能背景：`docs/LenoSDL3像素直写渲染优化.md`（其中 v4 = `setVSync(0)`）
