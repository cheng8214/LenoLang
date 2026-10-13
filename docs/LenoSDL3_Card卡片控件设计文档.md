# LenoSDL3 `Card` 卡片控件设计文档

> **状态：🧭 设计 + 实施台账**（**v2 + v2.1 已落地**：容器化 · 状态 · 零裁剪 · **圆角图** · **多角标** · 运行时 setter 全家桶；P2/P3 待办见第十节）
> **落地文件**：`build/leno_module/LenoSDL3/lib/sdl_card.leno` + `sdl_renderer.leno`（`drawTextureRounded`，v2.1 ✓）
> **首个使用方**：`leno_gui/应用/影视海报墙/`（22 张卡 · 响应式 3↔5 列 · 圆角图 · 「新」角标 · 卡内操作行）
> **演示/回归件**：`leno_gui/控件/复合控件/卡片/test_card.leno`（自带 `CSHOT` 钩子 ✓）
> **最后写：2026-10-13**（v2/v2.1 实施记录见第十一节 ✓）

---

## 一、为什么要有这个控件（缺口与证据）

「图 + 文字」的小单元在 GUI 里出现频率极高：海报墙、商品卡、图集缩略图、列表行、详情头图、看板统计卡。
在 v1 之前，只有一条路：`Panel` + `ImageView` + `Label` 拼装 —— **拼不出来**：

| 症状 | 病因（证据） |
|---|---|
| 卡片被 GridLayout/ScrollView 摆位后，里面的图与字**原地不动**、全糊在一起 | `Panel` 的子控件走**窗口绝对坐标**；`Panel.set_pos()` 只挪自己（`sdl_panel.leno:99`），而布局容器只会对**子控件本身**调 `set_pos/set_size`（`sdl_gridlayout.leno:230-231`、`sdl_scrollview.leno:307-308`） |
| 想用 `Panel.setLayout(VBox)` 绕 | 可以动，但那是"再来一层容器"，且 `Panel` 的 `_layoutChild` 只能有一个 ⇒ 卡片里想再放两个东西就回不去了（`sdl_panel.leno:88-89,117-123`） |

⇒ 结论：卡片这种「整体移动的最小单元」**必须自己 `impl Widget`**（`sdl_widget.leno:7-21` 只要求 7 个方法）。
这就是 `sdl_card.leno` 的存在理由，也是它比 `Panel` 更适合当"列表/网格单元"的地方。

---

## 二、设计目标 / 非目标

### 目标

| # | 目标 | 判据 |
|---|---|---|
| G1 | 一个 `Widget`，**整体移动** | 塞进 `GridLayout`/`ScrollView`/`HBox` 里位置尺寸跟手；被拉伸时不变形 |
| G2 | **响应式**：宽度变 ⇒ 图高跟着变，文字永不图片挤掉 | 列宽 3↔5 列切换（`leno_gui/应用/影视海报墙`）实拍对齐、无溢出 |
| G3 | **容器化**：图片下方可挂**任意控件**（按钮 / 进度条 / 开关 / 一行 HBox） | 卡内按钮可点，且**不会**同时触发整卡点击 |
| G4 | 状态齐：`hover` / `pressed` / `selected` / `enabled` / `visible` | 颜色优先级单一来源，四个状态可视觉区分 |
| G5 | **零每帧分配、零多余裁剪** | 每帧 `pushClipRect` 次数 = 0；每帧 `malloc` = 0；每帧 `measureString` = 0（文本/字号不变时） |
| G6 | 选项可对账、可换肤 | 有 `_opts_keys()`（`sdl_opts.leno` 约定）；`impl Themeable` 支持 `applyThemeAll` |
| G7 | 后代可派生 | 横版列表行卡 / 方形卡 / banner 都能靠 opts 或复制改 `render()` 得到 |

### 非目标（明确不做，避免范围膨胀）

- **不做虚拟化**：不可见卡片仍会 `render`（`sdl_scrollview.leno:501` 把整棵内容树画一遍再靠裁剪裁掉）⇒ 上千张要自己做可见区计算。那属于 `ScrollView`/`GridLayout` 的账，不在本控件。
- **不做富文本**：标题/副标题各一行 + 省略号。要图文混排请用 `set_content` 挂 `Label`（`Label` 有 `wrap`）。
- **不做视频/动图封面**（没有贴图动画管线）。
- **不做键盘焦点链**（框架整体缺 focus 链，见第十四节）。

---

## 三、v1 已实现能力（现状）

| 能力 | opts / 方法 |
|---|---|
| 图片（懒加载、三种缩放模式、着色、悬停提亮） | `image_path` / `image_mode`(0 FIT / 1 STRETCH / 2 FILL) / `image_h` / `image_aspect` / `image_tint` / `pad` |
| 标题 + 副标题（标题加粗、超宽省略号） | `title` / `subtitle` / `title_size` / `subtitle_size` / `text_color` / `subtitle_color` |
| 评分角标（半透明胶囊 + 文字） | `badge` / `badge_bg` / `badge_color` / `badge_size` |
| 底/边框/圆角/悬停 | `bg_color` / `hover_color` / `border` / `border_color` / `radius` |
| 交互 | `on_click` / `tip`（走 `sdl_tooltip`）/ `cursor` / `enabled` / `visible` |
| 资源 | 纹理懒加载 + `dispose()` 释放；三支字体 `acquireFontAuto` + `releaseFont` |

已跑通：`leno_gui/应用/影视海报墙`（22 张 · 3↔5 列自适应 · 筛选/搜索/排序复用同一批 Card 不泄漏 · 无头截图核对）。

---

## 四、v1 的局限（按"会不会真出事"分档）

### A 级：真隐患（v2 必修）

**A1. 每帧 `pushClipRect`，而且海报墙场景 **100% 命中** —— 闪烁隐患 + 白付 flush**

```273:286:build/leno_module/LenoSDL3/lib/sdl_card.leno
            bool needClip = dx < ix - 0.5 or dy < iy - 0.5 or dx + dw > ix + iw + 0.5 or dy + dh > iy + ih + 0.5
            ...
                r.pushClipRect(c2.to_ptr())
```
而 `pushClipRect` / `popClipRect` **每次都先 `flushBatches()`**（`sdl_renderer.leno:562-563`、`595-596`）。

算术：海报 300×450（0.667）填进图片区 172×249（0.690）⇒ 按高缩放后 `dh=258 > 249` ⇒ **每张卡每帧都要裁**。
22 张 = 每帧约 **44 次额外 flush** + 22 次 `SDL_SetRenderClipRect` 状态切换。仓里已有专门记录说"裁剪过多 ⇒ D3D 闪烁"（`docs/LenoSDL3_裁剪过多导致闪烁排查记录.md`）。

**修法（零裁剪）**：改 `drawTextureRect(tex, srcrect, dstrect)`（`sdl_renderer.leno:636`）—— 把居中裁切**算进源矩形**，目标矩形就是图片区；两个 `SDL_Rect` **分配一次缓存在字段里** ⇒ 每帧 0 次分配、0 次 push/pop、还少画被丢弃的像素。

**A2. 二次 `set()` 换图漏纹理**

```128:128:build/leno_module/LenoSDL3/lib/sdl_card.leno
        tex = null; loadedPath = ""     // 二次 set = 换卡 ⇒ 旧纹理交给 dispose/重载（不在这里 free：可能还没渲染器 ✓）
```
`dispose()` 只释放**当下**那张；而 `SDL_DestroyTexture` **不需要渲染器**（原注释写错了）⇒ 反复换图 = 每次都漏一张。

**A3. 省略号缓存只有单槽**（`cKey/cMax/cOut`，`sdl_card.leno:87-89`）

标题与副标题都要省略号时两者**互相踢缓存** ⇒ 每帧各跑一次二分（log₂n 次带字符串插值 key 的 `measureString`，`sdl_font.leno:183-192` 的 256 槽缓存救不了这个 key）。

### B 级：功能天花板（按需加）

| # | 局限 | 影响 / 修法要点 |
|---|---|---|
| B1 | **不能装子控件**（内容写死） | 卡里放不了按钮/进度条/开关 ⇒ v2 容器化（本文第五节） |
| B2 | 图片只能给**文件路径**，无 `set_texture` | 同一张图出现在多卡会各自解码 + 各占显存；程序化封面 / 截图缩略图也塞不进来 |
| B3 | 图片是**直角** | 圆角只画在卡片底上，图片贴顶边会把圆角盖掉。绕法：`pad:6` 内缩；正解见第十三节 P2 |
| B4 | 只有 `hover`，无 `selected` / `pressed` | 媒体库的"当前选中 / 正在播放"、点击反馈都得外层自己画 |
| B5 | 角标写死一个、位置固定右上 6px | 不能左上/左下、不能多个（"新片"+"4K"）、不能换图标 |
| B6 | 文字只有"标题 + 副标题"、左对齐、固定 8px、标题固定加粗 | 无 `wrap`、无居中/右对齐 |
| B7 | **无虚拟化**（继承自 ScrollView/GridLayout） | 每帧成本 O(全部卡片)；几十~几百张没问题 |
| B8 | 无键盘焦点 / 无障碍 | 框架级缺口（第十四节） |

### C 级：值得表扬的地方（派生成本极低）

- 外观**全靠 opts** + `_opts_keys` 对账（写错键会警告而不是静默丢弃）
- **尺寸规则**是它最值钱的部分（第五节 5.1）
- 派生变体 = 复制文件改 `render()`：横版列表行（`image_aspect:0.56`）/ 方形（1.0）/ banner / 排行榜卡

---

## 五、v2 设计

### 5.1 尺寸模型（唯一公式，实现与文档同源）

```
图片区：ix = x + pad,  iy = y + pad,  iw = w - 2·pad
        ih = image_h  (显式)  或  iw × image_aspect (默认 1.45 ≈ 2:3)
        ih ≤ h - textH - contentH - 12      ← 文字区与内容区**优先**，图片让位
文字块：titleH = (title != "" ? 行高(title_size) + 2 : 0)
        subH   = (subtitle != "" ? 行高(subtitle_size) : 0)        textH = titleH + subH
内容区：cy = iy + ih + gap + textH,  cx = x + content_pad
        cw = w - 2·content_pad,     ch = content_h > 0 ? content_h : (h - (cy - y) - content_pad)
```

三条不变式（v2 要守住）：

1. **图片永不挤掉文字/内容**：`ih` 由 `maxIH = h - textH - contentH - 12` 收紧（A 级回归点，海报墙 3↔5 列实拍过）。
2. **`auto_h:true` 时 `get_h()` 反算** = `pad + iw×image_aspect + gap + textH + content_h + content_pad`，供 `GridLayout`（add 时取 `get_h()`）与 `ScrollView`（`measure_h()`）用；默认 `false` = 用显式 `h`（布局赋的尺寸）。
3. **`hover_lift` 只影响绘制**，不改 `x/y`（hit-test 与 `GridLayout` 每帧 `set_pos` 都不能被污染）。

### 5.2 容器化（G3，本次重点）

| 项 | 设计 |
|---|---|
| 挂载 | `set_content(Widget)` / `clear_content()`；`content_h`（0=占满剩余）、`content_pad`（默认 8）、`content_bg`（0=不画） |
| 坐标 | `sync_content()` 在 **`process()` 与 `render()` 都调**（照 `Panel.syncLayoutChild` 的做法，`sdl_panel.leno:110,144`）⇒ 容器每帧 `set_pos/set_size` 到内容区 ⇒ 跟着卡片滚/动 |
| 事件 | `process` 先 `sync_content()`，再 `_content?.process(ev)`；**内容区内的点击不再触发整卡 `on_click`**（否则"点卡内收藏 = 又打开了详情"✗）。需要穿透时给 `content_click_through:true` |
| 命中 | 整卡矩形（含内容区）都算 `hover`（观感更好：鼠标在卡内任意处卡片都亮） |
| 句柄 | `Card._bind_window(hwnd)` → 记录并 `lay.passWindowHandle(_content, hwnd)`；`sdl_window` 的两处 switch 要把 `Card` 加进 `_bind_window` 那一组（否则卡内 `Edit` 拿不到 hwnd ⇒ IME 候选框跑到窗口左上角，见 `LenoSDL3_打包后窗口句柄下发静默失效排查记录.md`） |
| 裁剪 | **默认不裁内容区**（`content_clip:false`）—— 裁一次就是一次 flush（A1 同源）；要裁的由调用方开 `content_clip:true` |
| 释放 | `dispose()` → `_content?.dispose()`（照 `Panel.dispose`） |

**为什么不是"任意位置摆子控件"**：那是 `AnchorBox` 的活。Card 只提供一个**矩形内容区**（图片下方），这是"卡片"这个隐喻的自然边界，也让坐标同步只有一处、不可能漂移。

### 5.3 状态模型（颜色优先级单一来源）

```
优先级（高 → 低）：  selected → pressed → hover → 正常
disabled 单独走 mixbg(…, bg, 60) 那一档（不用 packColor(通道/2)，见 sdl_theme.leno:66-78 的理由）
```

| 状态 | 字段 | 默认 |
|---|---|---|
| `selected` | `selected_color` / `border_selected_color` | `theme.bgSel` / `theme.accent` |
| `pressed` | `pressed_color` | 0 ⇒ 自动 = `darken(hover_color, 10)` |
| `hover` | `hover_color` | 主题给 |
| 边框 | `border`(bool) / `border_color` | 关 / `theme.border` |

方法：`set_selected(bool)` / `is_selected()` / `is_hover()` / `is_pressed()`。

### 5.4 图片：三种模式 + **零裁剪** + 共享纹理

| 模式 | 源矩形 | 目标矩形 |
|---|---|---|
| `MODE_ASPECT_FIT`(0) | 整图（`null`） | 等比缩小居中（可能留边，留边色 = `image_bg`） |
| `MODE_STRETCH`(1) | 整图（`null`） | 图片区（拉伸） |
| `MODE_ASPECT_FILL`(2) | **居中裁切区（取整）** | 图片区（铺满，永不溢出） |

- `drawTextureRect` + **字段级复用的 `srectF`/`drectF`**（懒分配一次，`dispose` 释放）⇒ 每帧零分配。
- `set_texture(Ptr[u8] tex, bool own)`：`own:false` 时 `dispose` 不释放（多卡共享同一纹理）；`set_image(path)` 仍是默认路径。
- ⛔ **踩坑（v2 首版实测）**：`drawTextureRect` 的源/目标矩形必须是 **`SDL_FRect`**（`SDL_RenderTexture` 的
  src/dst 都是浮点矩形），而 `pushClipRect` 要的是 **`SDL_Rect`**（`SDL_SetRenderClipRect` 是整型）。
  首版把 `SDL_Rect` 当 dst 传进去 ⇒ **不报错**、只是字节被当浮点解释 ⇒ 图片整片画不出来（22 张全空、
  文字角标却正常 ✗）。⇒ 本控件**分开存三个矩形**：`srectF`/`drectF`（浮点、画图）+ `crect`（整型、需要裁剪时才用）。
  排查口径：`getTextureSize` 正常、纹理非 null、画面却空 ⇒ 先怀疑矩形类型 ✓

### 5.5 角标（**多角标已落地** v2.1 ✓）

两条入口、一份规格表（`BadgeSpec`）：

| 入口 | 用法 | 场景 |
|---|---|---|
| 选项（主角标） | `badge` / `badge_pos` / `badge_size` / `badge_bg` / `badge_color` / `badge_icon` | 评分这种"每张卡都有"的角标 ✓ |
| 选项（追加） | `badges: Array[Dict]`（元素键 `text` / `pos` / `size` / `bg` / `color` / `icon`） | 上架前就知道要挂几个 ✓ |
| 方法（运行时） | `add_badge(Dict)` / `clear_badges()` / `set_badge(text)` / `set_badge_pos(int)` | 上架后动态加（"新"/"4K"/"限免"）✓ |

- 规格表在建卡时**转一次**（不是每帧转 Dict ✗）；每个 spec 自己有字体缓存（`Font? f` + `fSize`）⇒
  **各用各的字号**，且只在字号首次出现时创建 ✓
- **同一个角上多个自动沿边错位**：上排横向排（右角往左、左角往右），下排往上叠 ✓（错位量用 4 个局部
  float，不是每帧新建数组 ✗）
- 未给的键回落主角标值 ⇒ 只写一个 `badge` 的老代码行为不变 ✓（零破坏）

### 5.4b 图片圆角（v2.1 ✓，走渲染器合成）

`image_radius`（0 = 方角，默认 ⇒ 老代码零开销）：

```
若 radius < 1 / 已在离屏 target / 开着 SSAA ⇒ 退化为 drawTextureRect（绝不扰动离屏流水线 ✓）
否则：图 → 渲染器级**共享**离屏 target（按尺寸缓存一张）
      → 圆角蒙版以**自定义混合**乘上去（只裁 alpha、图片颜色不动 ✓；顶点色给白）
      → 恢复现场（blend/target/颜色 + **蒙版纹理的 blend 还原**）→ 贴回屏幕
```

- 蒙版复用 `sdl_renderer` 既有的覆盖率蒙版（`rrGet` ✓）⇒ **抗锯齿与圆角矩形同源**、观感一致 ✓
- 四个必须记住的点（都实测踩过，详见速查附录 C）：
  ① **不能用 MUL / MOD**：SDL 那两种只动 **rgb**、**不动 alpha** ⇒ 圆角切口里原图被放大 2 倍照画 ✗
     ⇒ 必须自组「颜色不动、alpha = srcA × dstA」的模式（`sdl_renderer.maskAlphaBlend()` ✓）；
  ② `SDL_RenderGeometry` 认**纹理**的 blend（不是渲染器级）⇒ 两边都要设；
  ③ 蒙版是**共享缓存** ⇒ 画完当场还原 BLEND，否则后续圆角矩形全糊；
  ④ 离屏 target 纹理贴回前要重设 BLEND（SDL 可能把它改成 NONE）。
- ⚠ **软件渲染器（`SDL_VIDEODRIVER=dummy`）建不了蒙版/离屏纹理** ⇒ 静默回退方角
  （有一次提示行）⇒ **圆角必须在真机验证**，dummy 截图看不出来 ✗

### 5.6 文本

- 两段（标题/副标题）各一行；`text_align`(0 左/1 中/2 右)；`title_bold`(默认 true)
- 省略号缓存**两槽**（标题/副标题各一），键 = `(文本, 上限宽)`；A3 的互相踢缓存问题消失
- 行高缓存进字段（`titleLineH/subLineH`），`set_title/subtitle/*_size` 时置脏 ⇒ **每帧 0 次测量**（G5）

### 5.7 渲染顺序与"裁剪预算"

```
1 卡片底（圆角 fillRoundedRect）
2 图片区底色（image_bg；加载失败/留边时用）
3 图片（drawTextureRect，源矩形裁切 ⇒ 0 次 clip）
4 角标（胶囊 + 可选图标）
5 标题 / 副标题
6 content_bg（可选）→ _content.render(r)
7 边框（opt-in；selected 时可换色）
```

**目标：整卡每帧 `pushClipRect` = 0 次**（G5）。`ScrollView` 那一层裁剪已经保证了不画到视口外，卡片自己不需要再裁（v1 那 2×N 次是纯浪费）。
唯一例外：`content_clip:true` 且内容溢出时 —— 由调用方显式开启。

### 5.8 opts 全表（v2）

> 命名沿用仓内习惯：`image_*` / `badge_*` / `content_*` / 语义化 bool。**v1 的键全部保留语义**（第 5.9 节）。

| 组 | 键 |
|---|---|
| 位置 | `x` `y` `w` `h` |
| 图片 | `image_path` `image_mode` `image_h` `image_aspect` `image_tint` `image_bg` `pad` |
| 文本 | `title` `subtitle` `title_size` `subtitle_size` `title_bold` `text_color` `subtitle_color` `text_align` |
| 角标 | `badge` `badge_bg` `badge_color` `badge_size` `badge_pos` `badge_icon` |
| 内容区 | `content_h` `content_pad` `content_bg` `content_clip` `content_click_through` |
| 状态 | `selected` `selected_color` `border_selected_color` `hover_color` `pressed_color` `hover_lift` |
| 外观 | `radius` `bg_color` `border` `border_color` |
| 行为 | `cursor` `tip` `enabled` `visible` `auto_h` |

### 5.9 方法表（v2）

| 分类 | 方法 |
|---|---|
| 资源 | `set_image(path)` · `set_texture(tex, own)`（`own:false` = 借用共享纹理） |
| 角标 | `set_badge(text)` · `add_badge(Dict)` · `clear_badges()` · `set_badge_pos(int)` · `set_badge_size(float)` |
| 运行时 setter（v2.1 ✓） | `set_image_aspect` · `set_image_h` · `set_image_mode` · `set_image_radius` · `set_pad` · `set_gap` · `set_text_align` · `set_radius` · `set_hover_lift` · `set_content_h` · `set_content_pad` · `set_bg_color` · `set_hover_color` · `set_selected_color` · `set_border` · `set_tip` · `set_auto_h` · `set_title_size` · `set_subtitle_size`（字号类会连字体/行高缓存一起失效 ✓） |
| 文本 | `set_title` · `set_subtitle` |
| 容器 | `set_content(Widget)` · `clear_content()` · `content_widget()` · `content_rect()`（内容区几何 `[x,y,w,h]`，窗口绝对坐标 ⇒ 外部命中/锚弹层/自测用 ✓） |
| 状态 | `set_selected(bool)` · `is_selected()` · `is_hover()` · `is_pressed()` |
| 尺寸 | `get_w/get_h/set_pos/set_size/contains` · `measure_h()`（auto_h 用） |
| 交互 | `on_click(func():void)` · `set_enabled` / `set_visible` / `is_visible` |
| 框架 | `_bind_window(Ptr[u8])`（句柄下发到内容区）· `applyTheme(Theme)` |

### 5.10 兼容性

- **v1 → v2 全兼容**：旧键/旧方法一个不动；`render()` 的视觉结果除"图片改用源矩形裁切"外应**逐像素一致**（唯一差别在大比例失配时 ≤1px 取整误差，且**不再有裁剪闪烁**）。
- 默认行为变化只有一处：v1 在需要时会 pushClip，v2 不 push ⇒ 溢出部分不再被裁 ⇒ 改为"由源矩形裁切保证图片不溢出"（结果相同，路径不同）。

---

## 六、与框架其它部分的关系

| 关系 | 说明 |
|---|---|
| `Widget` face | 7 个方法全实现；由 `Window.run` 自动 process/render（`sdl_widget.leno:7-21`） |
| 窗口句柄下发 | `sdl_window.leno:776`（根/直接 add）与 `:100-107`（`lay.setExtraPassWindow` 回调）两处 switch 都要认 `Card`；`sdl_layout.leno:653-659` 靠 `_extraPassWindow` 兜底（sdl_layout 不能 import Card：`sdl_card → sdl_layout` 已是单向，反向 import 会成环） |
| 布局容器 | `GridLayout.add` / `ScrollView.set_content` 只调 `set_pos/set_size` ⇒ Card 必须自己把子控件摆到正确位置（本控件的 `sync_content`） |
| 主题 | `impl Themeable` ⇒ `win.applyThemeAll(t)` 生效（`sdl_window.leno:539`） |
| 工具提示 | `tip.tipReport`（`sdl_tooltip.leno`），与 `Button`/`ImageView` 同一套 |
| 选项对账 | `dochk.check("sdl_card", opts, _opts_keys())`（`sdl_opts.leno:92`；`LENO_UI_STRICT=1` 时前缀变 `[ui][STRICT]` 便于自检） |

---

## 七、验证计划

| # | 验什么 | 怎么验 | 判据 |
|---|---|---|---|
| V1 | 响应式不变形、文字不被挤掉 | 海报墙 `WALL_SIZE=900x600` vs 默认 1180 | 3 列 / 5 列，标题副标题完整显示 |
| V2 | **零裁剪** | `r.getBatchFillSubmits()` 之外的裁剪计数：临时给 Card 加 `_dbgClipCount` 计数（或对比"开/关源矩形裁切"两版的 `flushBatches` 次数） | 每帧 pushClip 次数 = 0 |
| V3 | 容器化 | 海报墙点「卡内操作」开关：每卡挂 `HBox(播放 + 收藏)` | 按钮可点；**点按钮不触发整卡 `on_click`** |
| V4 | 状态 | 点卡片 → `selected` 高亮；按下 → pressed；悬停 → hover + `hover_lift` | 四态可区分，截图核对 |
| V5 | 共享纹理 | 两张卡 `set_texture(同一 tex, own:false)` | 只解码一次；都不释放；第三张 `own:true` 释放时前两张不受影响 |
| V6 | 无泄漏 | 反复 `set_image` / `set_texture` + `dispose` | 纹理数不涨（`N` 轮后进程 RSS 稳定） |
| V7 | 每帧 0 测量 | 临时计数 `measureString` 调用 | 文本/字号不变时 = 0 |
| V8 | 选项对账 | `LENO_UI_STRICT=1` 跑海报墙 | 无 `[ui][STRICT]` 行 |
| V9 | **圆角图**（v2.1） | **真机**跑 `test_card.leno`（`CRBN=1`）+ `build/_probe_round.leno`（半径 48 放大看角） | 四角平滑裁掉、图片颜色不变 ✓（dummy 下会回退方角，见速查附录 C ✓） |
| V10 | **多角标**（v2.1） | `test_card.leno`（`CBADGE=1`）；海报墙给 2026 片挂「新」 | 三个角标各自位置/字号/底色正确、同角自动错位 ✓ |
| V11 | **运行时改布局**（v2.1） | `test_card.leno`（`CVIEW=1` 横幅 + `CSEL=1` 选中） | 文字/角标/图片比例全跟着变，行高重排正确 ✓ |
| V12 | `badges` 选项路径 | `test_card.leno` 初始角标走 opts（切换才走 `add_badge`） | 两条入口行为一致 ✓ |

无头跑法（仓内惯例）：

```powershell
$env:SDL_VIDEODRIVER='dummy'
$env:WALL_SHOT="$env:TEMP\wall.png"; $env:WALL_AT='30'
build\leno.exe leno_gui\应用\影视海报墙\movie_wall.leno
```

---

## 八、实施顺序与验收

| 批次 | 内容 | 验收 | 状态 |
|---|---|---|---|
| **P0** | A1 源矩形裁切去 clip · A2 换图释放 · A3 两槽省略号缓存 | V2/V6/V7 | ✅ 2026-10-13 |
| **P0** | 容器化：`set_content` / `sync_content` / 事件转发与"不穿透" / 句柄下发 | V3 | ✅ 2026-10-13 |
| **P0** | 状态：`selected` / `pressed` / `hover_lift` | V4 | ✅ 2026-10-13 |
| **P1** | `set_texture(tex, own)` · `badge_pos` / `badge_icon` · `text_align` · `auto_h` · `image_bg` · `content_rect()` | V5 + 截图 | ✅ 2026-10-13 |
| **P1** | 圆角图片（`image_radius`，复用 `_rrMask` 合成）· 多角标（`badges` + `add_badge`）· 运行时 setter 全家桶 · 演示/回归件 `test_card.leno` | V9/V10/V11/V12 | ✅ 2026-10-13（v2.1） |
| **P1 余项** | `content_clip` 的 D3D 实测 · **列表行变体**（图在左/文在右 ⇒ 需新增 `image_pos`）· 横幅变体已可做 ✓ | — | ⏳ 下轮 |
| **P2** | 多角标再加"图标+文字"组合细节 · 圆角图的 9 宫格优化（少一次离屏） | 需改 `sdl_renderer` | ⏳ 待定 |
| **P3** | 虚拟化网格（可见区计算）· 键盘焦点链 | 属于 ScrollView/框架层 | ⏳ 待定 |

每批落地后：更新本文"状态"、`docs/INDEX.md` 第三节、以及 `leno_gui/应用/影视海报墙` 的实拍截图判据。

---

## 九、已知取舍

1. **内容区是"矩形"，不是任意锚点**：想绝对摆位请挂 `AnchorBox`（`Panel.setLayout` 同款思路）。
2. **卡片不做溢出裁剪**：内容控件溢出的责任在调用方（要裁就 `content_clip:true`，代价是一次 flush）。
3. **不给卡片加滚动**：要滚就用 `ScrollView` 套卡片，别反向（卡片内滚动 = 事件与滚动条打架）。
4. **图片比例靠 `image_aspect` 声明**，而非读图后自适应：布局在**第一帧就要有确定高度**（读图是懒加载、可能失败）⇒ 显式声明是唯一稳的；`image_h` 可覆盖。
5. **`hover_lift` 只改绘制**：卡片视觉上浮，但命中区仍是布局给的位置（差 2px，无感，换来 hit-test 永不错位）。

---

## 十、遗留与后续（P1/P2/P3 待办）

- [x] 圆角图片（`image_radius`）：**已落地** —— 复用 `sdl_renderer` 的圆角覆盖率蒙版 + 离屏合成（`drawTextureRounded`，混合模式为自定义「只乘 alpha」`maskAlphaBlend()` ✓）
- [x] **右键上下文菜单**（2026-10-13 补 ✓ **框架缺口**：媒体库/商品墙必用的「右键 → 播放/收藏/详情」以前挂不上，只有 `Table`/`TreeView`/`ListBox` 有 ✓）：
      API 与那三者**同名同义**（`addContextItem` / `on_context_select(func(id):void)` / `on_context_will_open` /
      `clearContextItems` / `is_context_menu_open` / `set_context_menu_enabled` …）；
      菜单本体共用 `sdl_menu`（弹出/命中/关闭/边界夹紧都由它管 ✓、窗口第二遍 `renderContextMenus` 统一画 ✓）；
      **懒建**（`ensure_ctx_menu`：不用的卡不各留一个空菜单 ✓，句柄先到就先存着 ✓）、`dispose` 摘全局注册表 ✓；
      顺带修掉一个旧毛病：**右键也会触发整卡 `on_click`**（等于"右键打开详情"✗）—— 现在右键先拦下并弹菜单 ✓
      （回归件：`leno_gui/应用/影视海报墙/movie_wall.leno` 的 `WALL_RCLICK=n` 钩子 —— 注入右键 + 3 帧后点第 3 项，
      日志应**各只出现一次** ✓ 实测 ✓）
- [x] 多角标（`badge` 单值 → `badges` / `add_badge`，旧键与旧行为全保留 ⇒ 零破坏 ✓）
- [ ] **列表行变体（图在左、文/内容在右）**：当前 `image_pos` 固定"图在上" ⇒ 真列表行需要新增
  `image_pos: left/right` + `image_w`（图占宽/占高两种口径）——**下一批的首选项**
- [ ] `content_clip` 的 D3D 实测（与 `docs/LenoSDL3_裁剪过多导致闪烁排查记录.md` 的记录交叉验证）
- [ ] 圆角图的 9 宫格优化：现在每张卡每帧一次离屏合成（同尺寸共享 target ⇒ 已不算多）；
  想再省可以"同尺寸同图 ⇒ 缓存合成结果"，但会占显存 ⇒ 待评估
- [ ] 虚拟化网格（`ScrollView` 侧）：按可见区只 `render` 命中行 —— 这才是"上千张海报"的瓶颈
- [ ] 键盘焦点链（框架级）：`Tab` 能在卡片/按钮间移动

---

## 十一、v2 实施记录（2026-10-13）

| 验项 | 怎么跑 | 实测结果 |
|---|---|---|
| V1 响应式 | `$env:WALL_SIZE='900x600'` | `列=3`；默认宽 `列=5`；标题/副标题完整 ✓ |
| V2 零裁剪 | 代码路径（`grep pushClipRect sdl_card.leno`） | 只剩 `content_clip:true` 一处（默认关）；22 张卡每帧 **0 次** ✓ |
| V3 容器化 | `WALL_ACTIONS=1` + `WALL_CLICKSEL=20` | 「播放/收藏」行渲染正常；注入点击 → `card0.selected=true`，**没有**整卡回调的打印 ⇒ 不穿透 ✓ |
| V4 状态 | 同上（注入点击「收藏」后截图） | 卡片橙色选中描边可见；`hover_lift` 悬停上浮 ✓ |
| V7 每帧零测量 | 代码路径 | 行高走 `metricsDirty` 缓存；省略号两槽缓存；左对齐路径不量宽 ✓ |
| 回归 | 默认启动截图 | 与 v1 逐像素近似一致（仅取消裁剪）✓ |

**过程中新增的两条踩坑**（已写进 `build/leno_module/LenoSDL3/docs/SDL3常用API与易踩坑速查.md` 附录 C ✓）：

1. ⛔ `drawTextureRect` 的 dst 必须是 `SDL_FRect`（传 `SDL_Rect` 不报错、图片直接空掉 —— 首版 22 张全空）
2. ⚠ 卡片高度被 `GridLayout` 在 `add` 时捕获 ⇒ 运行时改高（挂内容行）必须 `clear()` + 重新 `add`

**顺带修正**：海报墙应用现在用上 `content_h` / `content_pad` / `gap` / `hover_lift` / `badge_pos` /
`border_selected_color` / `set_selected` / `set_content`（`leno_gui/应用/影视海报墙/movie_wall.leno`）。

### 第二批（v2.1，同日）

| 验项 | 怎么跑 | 实测结果 |
|---|---|---|
| V9 圆角图 | 真机 `test_card.leno`（`CRBN=1`）+ `build/_probe_round.leno`（半径 48） | ⚠ **首测结论是错的**：靠"看截图"以为四角裁好了，其实切口里是**2 倍亮度的原图色**（MUL 不动 alpha ⇒ 见下）⇒ **像素验收必须逐点读**，别靠肉眼 |
| V9b 回退 | `SDL_VIDEODRIVER=dummy` + `CRBN=1` | 静默回退方角 + **一次**提示行 ✓（不刷屏 ✓） |
| V9c 切口颜色（补测） | 真机跑海报墙 + 截图后用 `System.Drawing` 逐像素读四角 | 修后：切口 = 页面底 `0e1116` / 边框 `232a36`，图内 `c8102e` 与修前**逐点一致**（图没被变亮）✓ —— 这**才是**圆角生效的判据 |
| V10 多角标 | 真机 `CBADGE=1`；海报墙 2026 片挂「新」 | 右上评分 / 左上「新」/ 左下「4K」三处并存、字号各自生效 ✓ |
| V11 运行时改布局 | 真机 `CVIEW=1 CSEL=1` | 横幅比例 + 行高重排 + 选中橙边，三者同时正确 ✓ |
| V12 `badges` 选项路径 | `test_card.leno` 初始走 opts | 与 `add_badge` 路径行为一致 ✓ |
| 回归 | 海报墙真机截图 + 两个入口 `-c` 编译 | 22 张卡圆角图 + 「新」角标正常；编译无告警 ✓ |

**第二批新增的四条坑**（已进速查附录 C ✓）：

1. ⛔ **`MUL` / `MOD` 都不能用来"乘 alpha"** —— 它们只动 rgb、**不动 alpha**：MUL 的实际公式是
   `dst.rgb = src.rgb*dst.rgb + dst.rgb*(1-src.a)` ⇒ 蒙版 a=0 的圆角切口里 rgb 被放大成 **2 倍**、
   alpha 还是 1 ⇒ 切口照原图（更亮）画出来（症状极像"圆角没生效"）。要用
   `SDL_ComposeCustomBlendMode(ZERO, ONE, ADD, DST_ALPHA, ZERO, ADD)` 自组（`maskAlphaBlend()` ✓）
2. ⛔ `SDL_RenderGeometry` 认**纹理自身**的 blend ⇒ 只设渲染器级会让蒙版把图盖成**白块**；
   且蒙版是**共享缓存** ⇒ 画完必须当场还原 BLEND
3. ⛔ 软件渲染器（dummy）建不了蒙版/离屏纹理 ⇒ 圆角静默回退方角 ⇒ **验证圆角必须真机**；
   附带一条探针坑：`win.run` 之前自建渲染器的句柄在跑起来后失效（`renderer is invalid`）
4. ~~⛔ 在 A 的**方法**里 `new B(键=值)`，参数值那一段**看不见隐式 `self`**（A 的字段被报成
   "**未定义的变量**"）⇒ 值里写 `self.x`、或先 `new B()` 再逐字段赋值 ✓~~
   **★ 2026-10-13 已修（M11）⇒ 这条不再需要绕** ✓：真因是**方法体改写器的 switch 白名单
   漏了 `AST_STRUCT_INIT`**（`src/semantic/semantic_visit_method.c`），`new B(键=值)` 的**值**
   整段被 `default:` **静默跳过** ⇒ 值里的裸字段名没被改写成 `self.字段` ✗。
   现在**直接写裸名就行** ✓（`var b = new BadgeSpec(text = badge)` ✓）。
   （⚠ 旧说法"编译器把 `self` 切成 B"**不准确**：B 有同名字段时也照样响亮报错、不会静默取值 ✓；
   6 条边界与真因见 `docs/语言摩擦与隐式契约清单.md` 的 **M11** ✓）
