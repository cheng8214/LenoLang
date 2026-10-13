# docs/ 文档索引

> **本文件是 `docs/` 的唯一索引**：分类、一句话主题、状态与过时标注。
> 两份 `README`（`README.md` / `README_EN.md`）只保留几条最常用链接，**找文档一律从这里进**。
>
> - **最后核对：2026-10-02**（本轮：JIT 系 5 篇归档到 `docs/archive/`、5 篇杂名改名、
>   17 篇加"历史 / 局部过时"头、新增本索引；明细见 `待办与路线图.md` 第六节）。
>   **2026-10-10 追加**：新增 1 篇『LenoSDL3_打包后窗口句柄下发静默失效排查记录.md』（仅登记新文档，其余状态未重核）。
>   **2026-10-12 追加**：新增 1 篇『线程并发能力修复追踪.md』（线程/并发待做台账 TH1…TH7，进第四节；仅登记，其余状态未重核）。
>   **2026-10-12 追加（二）**：新增 1 篇『类型可见性与use逃生阀修复追踪.md』（类型可见性待做台账 US1…US5，进第四节）；同日还新增/同步 3 篇：『语言摩擦与隐式契约清单.md』M9（类型位置限定名报错口径）、『语言陷阱速查.md』㉝（哪些类型不用 `use`）、`import使用指南` 未改（仅登记，其余状态未重核）。
>   **2026-10-13 追加**：修 **M10**（跨模块读**没 export 的变量**以前编译不报错、运行期静默 `null`；现编译期报错并给处方）—— 同步『语言摩擦与隐式契约清单.md』M10、『语言陷阱速查.md』**新增 ㉞**，并**订正 ㉝**（US1 之后"类型位置不能写限定名"已不成立 ✗→✓，此前只加了 ★ 注没改正文 ✗）。
>   **2026-10-13 追加（二）**：新增 1 篇『LenoSDL3_Card卡片控件设计文档.md』（GUI 卡片控件：尺寸模型 / 容器化 / 状态优先级 / 裁剪预算 + 实施台账，**v2 已落地**；进第三节）。同日**框架侧**落实：`sdl_card.leno` v2 + v2.1（容器化 `set_content` · `selected/pressed` · 源矩形裁切去 `pushClipRect` · **圆角图 `image_radius`** · **多角标 `badges`/`add_badge`** · 运行时 setter 全家桶）、`sdl_renderer.leno` 新增 **`drawTextureRounded`**（离屏 + 圆角蒙版「只乘 alpha」的**自定义混合**合成 —— ⚠ 不能用 MUL/MOD：它们只动 rgb、不动 alpha ⇒ 切口里原图被放大 2 倍照画 ✗，见速查附录 C）与 `currentTarget()`、`sdl_gridlayout.leno` 新增运行时 `set_cols()`、`sdl_window.leno` 句柄下发认识 `Card`、`SDL3.leno` 导出 `createCard`；演示件 `leno_gui/控件/复合控件/卡片/test_card.leno`；**三条新踩坑**进『SDL3常用API与易踩坑速查.md』**附录 C**。
>   **2026-10-13 追加（三）**：登记 **M11**（⏳ **待修**）—— struct **方法**里 `new B(键 = 隐式自字段)` 会报「未定义的变量」（写 `self.` 或先 `new B()` 再赋值都正常；**只在方法里犯、不静默错值**）：『语言摩擦与隐式契约清单.md』新增 **M11** 节（6 个探针的边界表 + 疑似定位点，**实现侧 `src/semantic` 未动**）、『语言陷阱速查.md』新增 **㉟**；同日**订正三处**旧说法（曾写成"编译器把 self 切成 B"，不准确 ✗）：`sdl_card.leno` 注释、『LenoSDL3_Card卡片控件设计文档.md』第 4 条坑、『SDL3常用API与易踩坑速查.md』**附录 C** 那条标题。
>   **2026-10-13 追加（四）**：**框架侧修 2 处 + 应用侧新增 1 页**（都真机实测过，坑都进速查 ✓）：
>   ① `win.setRootAt` 在 **run() 之后换根是静默 no-op**（主循环的绘制/派发只认 `registerOnly` 抓的快照 `_runRoots[i]`，而它只改了 `self.root` 这个布局字段 ⇒ 界面纹丝不动、日志却说换成功 ✗）⇒ 加 `_syncRunRoot()` 同步 ⇒ 运行时**整页切换**（列表 ⇄ 详情）可用 ✓；
>   ② 列表/下拉/菜单的**行高亮圆角锚错了边**（`atBottom` 拿行自己的矩形补圆角，而末行常只露一半 ⇒ 圆角落到面板底边下方 ⇒ 面板两个底角被方角铺满）+ 内容裁剪 `_int(ph)+1` 让末行文字出框 1px ⇒ 收敛成渲染器共用件 **`fillRowInPanel`**（只填「行矩形 ∩ 面板圆角矩形」，圆角**锚在面板的边**）+ 裁剪改 `ceil` 精确覆盖；`sdl_combobox` / `sdl_listbox` / `sdl_menu` 三处调用点改调它 ✓；
>   ③ `leno_gui/应用/影视海报墙/` 新增**影片详情页**（点卡片进入：大海报 + 评分环 + 年份/分类/简介 + 收藏/播放；Esc/返回切回，网格状态原样保留 ✓；自检钩子 `WALL_DETAIL` / `WALL_BACK`）；
>   速查附录 C 相应新增 3 条（圆角纹理用自定义混合 · 行高亮锚点与裁剪 · `setRootAt` 运行时换根）。
>   **2026-10-13 追加（二十三）**：**按优先级修 A1 的第一批：焦点链 / 键盘导航**（Tab · Shift+Tab · Enter · Space ✓）：
>   ① 缺口：全库原来只有 `Edit` / `Canvas` 自己管焦点 ⇒ **Tab 键根本不换焦点** ✗（`Widget` face 只有 7 个
>      方法、没有焦点槽 ✓；事件是"广播给所有控件" ✗）；本版定的默认口径（可改 ✓）：焦点链 = **注册顺序**、
>      窗口只管 Tab/Shift+Tab 换焦点 + Enter/Space 激活（方向键留给控件内部 ✓）、"谁能聚焦"用**一张类型表**
>      （本版只有 `Button` ✓）、**焦点环由控件自绘**（窗口渲染后统一叫一下 ✓ ⇒ 窗口不需要知道控件几何 ✓）、
>      `activate()` 只触发 `on_click`（与鼠标点击**同一条回调** ✓）；
>   ② 实现（`sdl_window.leno`）：焦点链**懒收集**（每次按键现扫 `widgets` ⇒ 不维护注册表 ⇒ 不会失同步 ✓）、
>      `focusIdx` + `_applyFocus` 一次性刷全部（不会"两个都以为有焦点" ✗）、键盘挂在 `_tryShortcut`
>      **之后、广播之前**（快捷键优先 ✓ 且不漏给每个控件各处理一遍 ✓）；
>   ③ 顺带补了**自测钩子** `SDL3.testKeyDown(wid, scancode, mod)`（照 `testFocusLost` 的 malloc+from_ptr ✓，
>      门面 `SDL3.leno` 同步转出 ✓ —— 上一版就是漏了门面那一步 ⇒ 探针报"模块 SDL3 中没有方法" ✗）；
>   ④ 验收（新增 `leno_gui/自测/test_focus_nav.leno`，注入**真按键** ✓）：初始全 false → Tab×1 到 A →
>      Tab×2 **跳过 `set_focusable(false)` 的 B** 到 C → Tab×3 回卷到 A → Shift+Tab 反向到 C →
>      Enter/Space 让 `hitsC` 1→2 ✓；焦点环**客观取色**：C 左沿外 2px = `(100,180,255)` = 主题 `accentFocus` ✓，
>      未聚焦的 A 同位置只有底色 ✓ ⇒ 环只在聚焦控件外沿出现 ✓；
>   ⑤ 未做（按需推进 ✓）：`CheckBox/RadioButton/Toggle/Slider/ListBox/ComboBox/Table/TreeView/TabControl`
>      进类型表（要先给它们补 `focus()/blur()/is_focused()/draw_focus_ring()` ✓）、方向键在兄弟控件间移动 ✗、
>      `tabIndex` ✗。回归：海报墙 ✓、主题验收件 ✓、opacity 验收件 ✓、清理大师 exit=0 ✓。速查附录 C 新增该条 ✓。
>
>   **2026-10-13 追加（二十二）**：**新增框架能力：控件级不透明度 `win.set_widget_opacity(w, a)`**（真机实测 ✓）：
>   ① 动机：框架原来**没有**通用控件透明度 ✗（只有 `imageview/scrim/separator` 各自的语义化 `alpha` key ✓），
>      "把某个控件整体淡出"（禁用态 / 加载中 / 逐层揭示）做不到 ✓；
>   ② 实现（**控件侧零改动**，只在两个收口处动手 ✓）：`sdl_renderer` 加**全局 alpha 缩放** `setAlphaScale`
>      —— 在 `setColor` / `setColorFloat` / `pushTexBatch` 三处统一乘（纹理批覆盖文字 ✓ 所以 `drawText`
>      **不能**再乘一次，否则平方淡化 ✗；缩放必须放在各自"缓存比较"之前 ✓，否则同色跳过优化会绕过它 ✗）；
>      `sdl_window` 在渲染循环里**只包一层**（push → render → 还原，**必须还原** ✓）；
>      值存在**稀疏**数组（下标对齐 `widgets`、越界=1.0 ✓）⇒ 不必改 40+ 处 `widgets.add` 调用点 ✓；
>   ③ 特性：**容器设透明度 ⇒ 整个子树一起淡出** ✓（子控件都在它的 render 里画 ✓）；
>      ✅ **直绘纹理已补齐（同日 ✓）**：`drawTextureAt/Rect/FromTo` 走 `_texScalePush/_texScalePop`
>      （临时设纹理 alpha 调制、画完**还原旧值** ✓ —— 不能一律还原 255 ✗）；`drawTextureRounded` 因其
>      **最后一步就是 `drawTextureAt`** ⇒ 自动覆盖 ✓（**圆角海报也能淡出** ✓）。两个前置条件都踩过 ✗：
>      ① FFI 只认 clib **声明过**的函数 ⇒ 补声明 `SDL_GetTextureAlphaMod`（sdl_core.leno ✓）；
>      ② `&` 取址只能取 **cstruct 字段**（`&局部量` 报语义错误 ✗）⇒ 出参照 `getColor` 用 `SDL_Color.malloc()` ✓；
>      ⚠ 又抓一坑并修掉：合成期间若缩放仍开着 ⇒ 蒙版顶点色 alpha 被二次相乘 ⇒ **平方淡化**
>      （实测 opacity=0.4 的圆角海报看着像 0.16，且逐通道与 0.16 理论值吻合 ✓）⇒ 口径定为
>      "**缩放只施加在最终出口**" ✓；
>   ④ 验收（新增回归件 `leno_gui/自测/test_opacity.leno`，三档 + 容器 + 对照组，量平均色）：
>      实测与该透明度下的**理论混合值逐通道吻合** ✓ —— 100% (63,142,221)；50% (49,90,134) vs 理论 (50,91,134)；
>      25% (44,64,90) vs (44,65,91)；容器 0.35 量内部按钮 (46,77,109) vs (47,76,108)（**子树确实跟着淡** ✓）、
>      对照组 (64,143,221) ✓；
>   ⑤ 回归：海报墙 ✓、主题验收件（公开色逐字一致 ⇒ 未扰动 ✓）、清理大师 exit=0 ✓。速查附录 C 新增该条 ✓。
>
>   **2026-10-13 追加（二十一）**：**按优先级修 A2：运行时主题切换（`applyThemeAll`）两个真洞**（真机实测对照 ✓）：
>   ① **发现方式**：读码时注意到仓里唯一的主题测试 `leno_gui/控件/总览/test_theme.leno` 是
>      "**关窗口重建**"式的（注释自述 ✓）⇒ `applyThemeAll` 这条**运行时**路径从来没被测过 ✗；
>   ② **洞 1（点名表漏人）**：`sdl_window.applyThemeWidget` 的合并 `case` 只列了 8 个类型，而全库有
>      **44 处 `impl Widget`** ✗ ⇒ `Canvas/ListBox/Slider/SpinBox/三种图表/Card/ScrollView/TabControl`
>      的 `applyTheme` **写好了却从没被调用** ✓；
>   ③ **洞 2（递归不下钻）**：只递归 `Splitter/Panel/GroupBox/HBox/VBox/GridLayout` ⇒
>      `ScrollView`/`Card`/`TabControl` 里的控件**永远收不到主题** ✗；
>   ④ **修法**：点名表补全 + 补 `AnchorBox` 递归；`sdl_scrollview` 补 `impl Widget, Themeable` + `applyTheme`；
>      新增**主题递归出口** `lay.setExtraApplyThemeChild`（照 `_extraPassResize` 样板 ✓，容器把内容控件
>      交回窗口继续下钻 ⇒ 类型表仍只在 `sdl_window` 一处 ✓，也绕开了 `Card._content` 是 `pri` 的问题 ✓）；
>   ⑤ **验收（新增回归件 `leno_gui/自测/test_theme_switch.leno`）**：同一次运行内截 A(dark) →
>      `applyThemeAll(light)` → 截 B，离线按矩形比区域平均色 ✓。**修前（真机 stash 对照）四个深容器 Δ 全是 0 ✗；
>      修后 202 / 176 / 125 / 186 ✓**（窗口直属 Button 对照组 18 ✓ 两侧一致）✓；
>   ⑥ **第二批（同日补完 ✓）**：`CheckBox / RadioButton / Toggle / ProgressBar / Spinner / Separator /
>      RingProgress / ImageView / LinkButton` 这 9 个补上 `applyTheme` 并进点名表（`SDLK_window` 还补了
>      `sdl_ring` 的 import —— 静态 `case` 必须显式 import ✓）；映射与各自 `set()` 的建时取色逐字段对齐 ✓，
>      `ProgressBar` 直接复用建时那三个映射函数（防口径漂移 ✓）。
>      另外 6 个**不是漏是不需要**：`PopMenu/Tooltip/Dialog` 每帧现读 `getTheme()` ✓；`Scrim` 纯 alpha 遮罩 ✓；
>      `Calendar/Steps` 设计上不依赖主题（配色全靠 opts ✓）⇒ 产品语义，本次未动 ✓。
>   ⑦ **第二批验收**（同一探针，判据强弱如实记 ✓）：`Spinner/LinkButton` 公开字段 `_color` **修前切前=切后（一字节没变）**
>      ✗ → 修后变 ✓；`Separator` `#464A54` → `#D2D2DC` ✓；`ProgressBar/RingProgress/ImageView` 暗像素占比
>      修前 42→**40** / 100→**34** / 100→**88** ✗ → 修后 42→3 / 100→0 / 100→0 ✓；`CheckBox/RadioButton` 框内
>      22×22 平均色 修前 (66,78,95)/(95,107,124) 仍深 ✗ → 修后 (214,221,237)/(211,219,235) ✓；`Toggle` 最弱
>      （轨道本就是 accent）(70,155,236)→(70,136,221) ✓。
>   ⑧ 速查附录 C 新增该条（症状 / 两个洞 / 修法 / 两批验收命令与对照表 / "pri 字段只能靠像素"的记号 ✓）。
>
>   **2026-10-13 追加（二十）**：**把这一轮攒下的 13 个探针收进仓库当正式回归件**（用户要求：留着 ✓ 改名 ✓ 归文件夹 ✓）：
>   ① **为什么必须搬出 `build/`**：`.gitignore` 明确按"`build/_*` 下划线前缀"排除临时探针 ✗（`build/` 本身是
>      **要入库**的目录 ⇒ 不能整体忽略 ✓）⇒ 搬到新目录 **`leno_gui/自测/`**（沿用 `控件/特效/系统/应用` 的短双字风格 ✓），
>      文件名统一为 **`test_*.leno`**（与 `leno_gui/控件/**/test_*.leno` 一致 ✓）；每个文件头都写明"验什么 + 怎么判" ✓；
>   ② 13 个回归件：
>
>      | 文件 | 验什么 |
>      |---|---|
>      | `test_form_controls.leno` | Toggle/CheckBox/Radio 的 `set_size` 空实现 · Radio 互斥回调 · Slider 量化 |
>      | `test_chart_labels.leno` | 图表数值标签丢小数（柱上 / Y 轴 / 饼图图例） |
>      | `test_scrim_resize.leno` | Scrim 不跟随窗口缩放（像素判据） |
>      | `test_table_cell_colors.leno` | Table 格色：删行错位 / 换数据残留（三段截图） |
>      | `test_tabcontrol_reset.leno` | TabControl 二次 `set()` 丢页 |
>      | `test_spinner_enabled_tip.leno` | Spinner `set_enabled` / `tip` / 帧率对照 |
>      | `test_spinner_shape.leno` | Spinner 圆度与厚度（逐 2° 采样 180 个角度） |
>      | `test_splitter_events.leno` | Splitter 事件转发 / 拖拽 / resize |
>      | `test_combobox_focus.leno` | 失焦收起下拉（像素 7205→0） |
>      | `test_window_handle_size.leno` | 句柄/尺寸下发（ListBox 点击 + resize 后右键菜单） |
>      | `test_table_image_cache.leno` | 图标纹理缓存上限（`PROBE_MAX` A/B） |
>      | `test_nested_context_menu.leno` | 嵌套控件右键菜单夹紧 + 缩窗后重开 |
>      | `test_lang_closure_capture.leno` | **语言**：闭包捕获语义六种形态 |
>   ③ 全仓引用一并改路径（扫出 57 处 ⇒ **只换我这 13 个名字** ✓；`_probe_combo_corner` / `_probe_round` /
>      `_probe_call` / `_probe_facebound` / `_probe_new_*` 是**别人/旧轮**的名字 ⇒ 一个没动 ✓）；
>      ⚠ 替换时踩到两个坑：映射键**自带下划线**、我又前缀 `build/_` ⇒ 实际匹配 `build/__probe_*` **全不匹配** ✗
>      （第一遍只改掉了裸名）；短语里的 `build/_*` 中 `*` 被当成**正则量词** ⇒ 那条也没匹配 ✗（第二遍才修净 ✓）；
>   ④ 验证：搬家后**照跑** ✓（闭包六形态 ✓、图标缓存 `img_cache_count()=3` ✓ ⇒ 说明探针内部的**相对路径仍然对** ✓）；
>      `git check-ignore` 退出码 1 ⇒ **没被忽略** ✓（可入库 ✓）；速查/INDEX/库注释里的验收命令已全部指向新路径 ✓；
>      海报墙回归 ✓。
>
>   **2026-10-13 追加（十九）**：**查"嵌套控件拿不到窗口尺寸"（用户点名的第 ⑤ 项）⇒ 结论与预想相反**：
>   ① **先纠正误判**：嵌套控件收到的尺寸确实是 `-1`（未知 ✓），但 `sdl_menu.clampRect` 里**早有兜底** ——
>      尺寸 ≤ 0 且有句柄时直接 `SDL_GetWindowSize(winHnd)` ✓。实测（探针 `leno_gui/自测/test_nested_context_menu.leno`：
>      Table 嵌在 根→HBox→VBox 三层、尺寸字段恒为 0）：窗口右下角右键 ⇒ 菜单**被正确翻转**、完整留在窗口内 ✓
>      ⇒ **容器层层传尺寸那条路不需要走** ✓（原本以为"最难"的一项，实测是虚警 ✓）；
>   ② **但同一支代码里有个真缺口（实测抓到）**：兜底**只查一次就缓存** ✗ ⇒ 窗口 resize 后再弹（菜单关着 ⇒
>      没人刷新它）会先用**旧值**算一遍；日志实证 `_winW=700` 先算、同帧稍后才刷成 420 重算（画的位置会被纠正 ✓，
>      但 `openContext` 已拿第一遍的结果 `setPopupCapture(...)` 登记**弹层捕获区** ✗ ⇒ 那一帧捕获区是歪的 ✓）；
>   ③ **修法**：`openContext` 弹出前按句柄刷一次真实窗口尺寸（6 行 ✓）；验收：修复前日志
>      `夹紧用尺寸 700x460 ⇒ 捕获区=365,366` ✗ → 修复后 `420x340 ⇒ 捕获区=85,256` ✓（= 翻转后的正确位置 ✓）；
>   ④ 回归：`test_menu`/`test_table`/海报墙 编译 ✓、海报墙真跑含 Card 右键链 ✓；
>      速查附录 C 新增该条（**含"别再重复怀疑这条"的结论** ✓）。
>   至此，用户列的五项（Spinner 多边形 / Splitter 双 relayout / ComboBox 失焦 / Table 缓存上限 / 嵌套尺寸）全部收敛 ✓。
>
>   **2026-10-13 追加（十八）**：**修 `Table` 图标纹理缓存"只增不减"**（用户点名的第 ④ 项）：
>   ① 病因：`get_tex` 只有"未命中 ⇒ 加载 + `add`"一条路 ✗ ⇒ 表格里"每行一个不同图标/缩略图"时显存一路涨 ✓；
>   ② 修法：照 `sdl_font.texCap` 的**环形覆盖 + 当场回收**样板（`SDL_DestroyTexture(old)` ✓，不是"丢引用" ✗）+
>      新选项 **`image_cache_max`**（默认 64，`0` = 不限制 = 老行为 ✓）+ `set_image_cache_max(n)` +
>      `img_cache_count()`（调试/自测 ✓）；覆盖 MRU 那一格时把 MRU 指向**新**纹理（否则会返回已销毁的 ✗）；
>   ③ 安全性说明（写进速查）：`IMG_LoadTexture` 每次都新建、**不共享** ⇒ 淘汰时销毁安全 ✓；
>      淘汰后已从数组移除 ⇒ `dispose` 不会再碰 ✓；⚠ 明确写下"别把这句销毁抄进 `dispose`"（那里有 double-free 教训 ✓）；
>   ④ 验收：探针 `leno_gui/自测/test_table_image_cache.leno`（6 行各一张不同海报，A/B 用 `PROBE_MAX` 切）⇒
>      `max=3` 时 `img_cache_count()=3` ✓、`max=0` 时 `=6` ✓；且截图里 **6 行缩略图全正常** ✓
>      （上限 3 < 6 行 ⇒ 渲染中途就在回收也没画坏 ✓）；`test_table_features`/`test_table_advanced`/
>      `cleaner_master`/`file_manager` 编译 ✓、海报墙列表页真跑 ✓。
>   ⑤ 顺带记两个**踩坑**（本轮实际被编译器拦下）：库内要用 `SDL_DestroyTexture` 得先 `use core.sdl3` ✓；
>      改 import 行时**别只匹配到行首那一段**（尾段 `; use core.SDL_Rect` 会被后加的 `//` 注释吃掉 ✗，
>      表现为别处一片"SDL_Rect 未定义"✗）。
>
>   **2026-10-13 追加（十七）**：**修"失焦不收弹层"**（用户点名的第 ③ 项）+ 为此补自测钩子：
>   ① ⛔ 症状：打开 ComboBox 下拉（或右键菜单）后 Alt+Tab / 切走 ⇒ 弹层**留在屏幕上、点不动也关不掉** ✗
>      （实测：注入 `WINDOW_FOCUS_LOST` 后 `isOpen()` 仍是 true ✗；下拉区域可见像素 7205，修后 0 ✓）；
>   ② 病因：这一支没人接 —— `ComboBox.process` 缺分支（`PopMenu` 早有 ✓）；右键菜单/顶部菜单下拉更收不到
>      （它们不是宿主画的，按 `isOpen()` 第二遍统一画 ✗，与 `TabControl` 切页那个同源 bug 一样 ✓）；
>   ③ 修法：`ComboBox.process` 加失焦分支 ✓ + 窗口**唯一派发点**集中收拾（`menu.closeContextMenus(hnd)` +
>      逐个 `MenuBar.close()` ✓；全工程只有一处派发 ⇒ 模态窗/普通窗全覆盖 ✓）；
>      ⚠ 踩到一个语法坑记在速查里：`for arr to x` 里 **x 是元素不是索引**（写成 `bars[b]` 编译不过 ✓）；
>   ④ 新增自测钩子 `SDL3.testFocusLost(wid)` / `testFocusGained(wid)`（照 `testClickAt` 的思路补 ✓，
>      以后这类"只有真机 Alt+Tab 才触发"的行为也能自动回归 ✓）；
>   ⑤ 验收与回归：`test_combobox_focus`（true→false + 像素 7205→0 ✓）、`test_window_handle_size`（右键菜单 true→false ✓）、
>      `test_menu` / `test_combobox` / `ide_layout` / 海报墙编译 ✓、海报墙真跑含 Card 右键链 ✓。
>
>   **2026-10-13 追加（十六）**：**用户实测报"Spinner 是多边形不是圆形" ⇒ 修掉一个渲染真 bug（含 3 个连带问题）**：
>   ① ⛔ `sdl_spinner.draw_arc` 的段数公式 `radius*span/360*0.4` 在 r=26/120° 时算出 3.46 ⇒ 夹到 **4 段**、
>      整圈轨道只有 **10 段**（十边形 ✗）；且 `thick` 形参**压根没用**（只画 1px `drawLine` ✗）
>      ⇒ 修法：**不走"加密段数"**（`drawLine` 无抗锯齿、不能设线宽 ⇒ 只是"更细腻的多边形" ✗），
>      改用 `fillCircle`（**抗锯齿** ✓，圆角蒙版缓存 ⇒ 同半径复用 ✓）沿弧线**密排** ⇒ 边缘无棱角、
>      圆角端点、粗细即 `thickness`、**不依赖背景色** ✓（没学 RingProgress 的"fillPie+抠底"，那要求知道内圆底色 ✗）；
>   ② 同批修：`set_enabled` 原来是**空实现** ✗（现在真停转 + 不再每帧请求重绘 + 变暗；实测帧率
>      启用 2s/103 帧 vs 全禁用 2s/20 帧 ✓）· `tip` 原来是**死代码** ✗（`process` 空 ⇒ 从不上报，现已接上、悬停实测弹出 ✓）·
>      `step()` 角度归一化只会减一次 360 ✗（卡顿后弧段跳变）⇒ 改 `while`，并**一并修掉 `RingProgress.step_sweep` 的同款** ✓；
>   ③ 还掉（用户点名的第二条）：`Splitter` 每帧**双 relayout**（`process` 与 `render` 各调一次、值还一样 ✗）
>      ⇒ 加"上次几何"去重（(x,y,w,h,ratio) 一致就跳过 ✓；`set_first/second` 会作废缓存 ✓）；
>   ④ 验收：探针 `leno_gui/自测/test_spinner_shape.leno`（size220/厚14/静态整圈）逐 2° 采样 180 个角度 ⇒
>      **0 未命中**、弧带中心偏差 **-1.0…+1.25px**、厚度 **13.5…16.5**（老实现内凹 ~4.7px、线宽 ~1px ✗）；
>      `test_splitter_events` 与海报墙回归照旧通过 ✓（速查附录 C 新增 Spinner 那条，含公式、修法与验收命令 ✓）。
>
>   **2026-10-13 追加（十五）**：**把（十四）提到的"句柄路径同类债"也还掉，顺带治好一处漂移**：
>   ① `sdl_layout._extraPassWindow` 钩子签名补 **窗口逻辑宽高**（`ww < 0` = 尺寸未知 ⇒ 容器路径传 `-1` ✓）；
>      `Table`/`TreeView`/`ListBox` 从布局那层 switch **挪进钩子**（它们要"句柄 **+ 尺寸**"，得由认识窗口尺寸的一方下发 ✓）；
>      `sdl_window` 里原先**两份**类型表（`_refreshWindowHandle` / `Window._passWindowHandle`）**全删**，
>      只调新导出的 `passWindowHandleSized` ✓ ⇒ 句柄相关类型判断也**只剩一处** ✓；
>   ② **顺带治好漂移** ✗→✓：那两份表原本不一致（一份有 `ListBox`/`GridLayout`，另一份有 `Card` 却**漏 `ListBox`**，
>      靠"每帧安全网"掩盖 ✓）⇒ 现在合并成一份，两边行为一致 ✓；
>   ③ 回归（均实测 ✓）：`test_window_handle_size.leno` —— `win.addListBox` 点击 `on_change` 触发 ✓（合并 case 没被剪空 ✓）、
>      窗口 resize 后**右键 Table 菜单仍能打开** ✓；`test_scrim_resize/splitter` 探针 ④ resize 断言仍过
>      （`splitter=840×420` ✓）；海报墙网格 + Card 右键链 ✓、列表页 ✓；`ide_layout`/`test_listbox`/`test_listbox_multi`/
>      `test_treeview`/`test_treeview_image`/`test_table`/`file_manager` 编译通过 ✓；
>   ④ ⚠ **仍未覆盖**（如实记）：**嵌套**在容器里的 `Table`/`Splitter` 拿不到窗口尺寸（容器只有句柄 ⇒ `ww=-1` ✓）——
>      实际影响≈0（右键菜单每帧用当前窗口尺寸刷新 ✓）；要彻底修需让容器层层携带尺寸 ✗（未做 ✓）。
>
>   **2026-10-13 追加（十四）**：**还掉（十一）记的那条设计债 —— 窗口 resize 通知的硬编码类型表收敛到一处**：
>   ① 原来 `sdl_window` 里**同一张类型表写了 3 遍**（主循环 / 模态窗 / 悬停弹层窗，只认 `Table`/`TreeView`/`Splitter`）
>      ✗ ⇒ 新控件想收 resize 得改 3 处。现照 `_passWindowHandle` 的样板做：
>      `sdl_layout` 新增 `_passWindowResize` + 导出 `passWindowResize` + `setExtraPassResize` 扩展钩子
>      （与 `setExtraPassWindow` 同款 ✓，避开循环依赖：布局模块仍不 import 扩展类型 ✓）；
>      `sdl_window` 只留**一处注册**（`Splitter → _bind_window_size`、`Scrim → on_window_resize` ✓），
>      三个循环与三处"根为 Splitter"的特殊分支全部改成 `lay.passWindowResize(...)` ✓
>      ⇒ **`sdl_window` 的 resize 逻辑现在一个具体控件类型都不认识** ✓（新控件只加一处 ✓）；
>   ② 回归（三条，均实测 ✓）：Scrim 探针 —— 缩放到 880×580 后采样点全 `878787`（无白条 ✓，说明 resize 经新路径
>      仍到达 Scrim ✓）；Splitter 探针 —— 点左栏子控件 `clicks=1` ✓、拖分隔条 `ratio 0.35→0.5` ✓、
>      **窗口拉到 840×420 后 `splitter=840.0x420.0`** ✓（Splitter 经钩子收到 resize ✓）；海报墙 +
>      `ide_layout` / `test_table` / `file_manager` 编译通过 ✓。
>
>   **2026-10-13 追加（十三）**：**继续压 Splitter / ProgressBar / TabControl**（探针 `leno_gui/自测/test_splitter_events.leno`、
>   `leno_gui/自测/test_tabcontrol_reset.leno`）：
>   ① ⛔ **修掉真 bug**：`TabControl` **二次 `set()`** 会重建 `pages/rel/_pageLayouts` ⇒ **整页内容被丢弃** ✗
>      （实测：`set(同 opts + 改一个颜色)` 后页里的按钮当场消失、`on_click` 停在 1 ✗）。修法：**页数没变就不重建**
>      页容器 ✓ + `active` 缺省值改取**当前页**（否则改配色会把页签跳回第 0 页 ✗）+ 页数真变时**故意不 dispose**
>      旧子控件（应用持有，替它释会"释放后再用"✗）。修后探针计数 1→2 ✓、按钮还在 ✓（速查附录 C 新增该条 ✓）；
>   ② **未复现（如实记，不当作问题 ✓）**：`Splitter` 的事件转发与拖拽都正确 —— 点左栏子控件 `on_click` 照常触发 ✓、
>      拖分隔条 `ratio 0.35→0.5` 精确 ✓、**拖完再点子控件照样灵** ✓（读码怀疑的"拖拽吞事件 ⇒ 子控件卡在按下态"✗不成立）；
>      `ProgressBar` 那句"无条件 `markFrameMotion` ⇒ 空转重绘"✗也不成立 —— `markFrameMotion` **只标记本帧有鼠标移动**
>      （供帧末统一应用光标 ✓、见 `sdl_core.applyFrameCursor`），**不请求重绘** ✓（读码时看着吓人而已 ✓）。
>   ③ 仍**未验**的读码怀疑：`ComboBox` 失焦不收下拉（无头难验）· `Splitter` 每帧双 relayout（幂等 ⇒ 只是白做）·
>      `Splitter.dispose` 递归 dispose 子控件（与"子控件归应用所有"的口径不一致，**双 dispose 风险**）·
>      `Table` 图标纹理缓存无上限 · `Spinner.set_enabled` 空实现 + `tip` 死代码。
>
>   **2026-10-13 追加（十二）**：**压 Table 的自定义格色（探针 `leno_gui/自测/test_table_cell_colors.leno`）⇒ 又两个"张冠李戴"**：
>   ⛔ 单元格级颜色存在 `_cellFgColors/_cellBgColors`（`Dict`，键 `"row,col"` ✓），而 `remove_row` 不重排键、
>   `set_rows`/`clear` 也不清 ⇒ ① 删一行后下面的格色**整体错位一行**（实测：R2 绿/R3 红，`remove_row(0)` 后
>   绿红停在同一屏幕行 ⇒ 实际染上 R3/R4 ✗）；② 换成全新数据后旧格色**原样残留** ✗。
>   修法（口径与"排序不许说谎"一致：**要么别张冠李戴**）：`remove_row` 把 `o>idx` 的键搬成 `o-1`（照 `swap_rows` 思路 ✓）、
>   `set_rows`/`clear` 清掉两个色表与 `rowColors` ✓（应用想换数据后染色就在 `set_rows` 之后 set —— `cleaner_master` 本就如此 ✓）。
>   验收：三段截图读同一列像素 ⇒ B 应"绿红整体上移一行" ✓、C 应"全无染色" ✓（速查附录 C 新增该条，含流程与判据 ✓）。
>
>   **2026-10-13 追加（十一）**：**压 Scrim（探针 `leno_gui/自测/test_scrim_resize.leno`）⇒ 又一个真 bug + 一条设计债**：
>   ① ⛔ `Scrim`（模态遮罩）**不跟随窗口缩放**：尺寸只在 `win.addScrim` 创建时下发一次 ✗ ⇒ 拉大窗口后
>      右下**露一条没遮住的原色**，而且那条**点得动**（`process` 按自己的矩形吞事件 ✓）—— 模态漏半边 ✓。
>      实测（白底 + alpha120）：600×400 全灰 ✓；拉到 880×580 后 (640,200)/(300,460)/(820,540) 仍是纯白 `ffffff` ✗。
>      修法：`Scrim` 补 `on_window_resize(ww,hh)` + 接进窗口那 3 处类型表 ⇒ 修后 6 个采样点全 `878787` ✓
>      （速查附录 C 新增该条，含探针命令与像素判据 ✓）；
>   ② 记**设计债**：窗口的 resize 通知是**硬编码类型表**（`Table`/`TreeView`/`Splitter`，**同一份写了 3 遍** ✗）
>      ⇒ 新控件想收 resize 得改 3 处；建议照 `_passWindowHandle` 集中到布局模块一处 + 留扩展钩子 ✓（**本次未动** ✓）。
>
>   **2026-10-13 追加（十）**：**压图表（探针 `leno_gui/自测/test_chart_labels.leno`）⇒ 又修一个"界面说谎"**：
>   ⛔ `_fmtNum(float) = _int(v) as string` ⇒ 图上数字**全被取整**：实测把评分 `7.7/8.2/9.3/8.4`
>   画成柱状图，柱上标签显示 `7/8/9/8` ✗、Y 轴把 2.5/7.5 的位置写成 `2/7` ✗ —— 而它同时供
>   **Y 轴刻度 / 柱上数值 / 悬停提示 / 折线·饼图图例**用（一处错、全图错 ✓）。
>   修法：整数显示整数、有小数如实显示（`d < 0.0001 ⇒ 取整`，否则 `_str(v)` ✓；`_int` 是截断不是四舍五入 ✓）；
>   修后柱上 `7.7/8.2/9.3/8.4` ✓、Y 轴出现 2.5/7.5 ✓、饼图图例带小数 ✓。速查附录 C 新增该条（含验收命令 ✓）。
>   ⚠ 本轮**没有**给海报墙加"统计页"：图表能力已单独验过，而这个应用的形态刚按你的意见定稿 ⇒
>   不主动再塞新视图（要加随时说 ✓）。
>
>   **2026-10-13 追加（九）**：**继续压表单控件**（探针 `leno_gui/自测/test_form_controls.leno` ✓（回归件，随仓入库 ✓））：
>   ① ⛔ **修掉真 bug**：`RadioButton` 同组互斥时**不给被取消那一项回调** —— `select()` 直接改 `rb.checked = false`
>      不走 `on_change` ⇒ 靠回调维护状态的应用"界面没选、自己以为选着"✗（实测：先点 r1 再点 r2，r1 收到 **0 次** on=false）。
>      修法：按「**用户操作**（点击/被挤掉）回调 / **程序化** `set_checked` 不回调」的口径补 `notify_change(false)` ✓
>      （终态 r1 回调 2 次 ✓）；同口径也写进了 `RadioButton` 的注释 ✓
>   ② 记下**契约**（非 bug）：`Toggle` / `CheckBox` / `RadioButton` 的 `set_size` 是**空实现**（尺寸自量 ✓）⇒
>      布局 `grow`/`basis` 改不了它画多大（实测 `set_size(200,44)` 后仍是 122×28 ✓）；要尺寸请在 `createXxx` 的 opts 里给 ✓
>      （三处空实现已补注释说明 ✓）
>   ③ 记下**自测坑**：合成点击前必须先 `SDL3.testMoveAt` —— `Radio`/`CheckBox`/`Toggle` 的点击判据含 `hover`
>      （只在 motion 里置位）⇒ 只投 `testClickAt` 会"返回 true 但毫无反应"✗（本次就这么被坑了一轮 ✓）
>   ④ **未复现**（不当作问题 ✓）：`Slider` 的量化与回调正常 —— `set_value(3.7)` + `step:1` ⇒ 4.0 且回调 1 次 ✓；
>      `set_range(0,5)` 值未变 ⇒ 不回调 ✓（合理 ✓）
>
>   **2026-10-13 追加（八）**：**照用户反馈把「列表」页调成"媒体库"的样子** + 补一个 `TabControl` 缺口：
>   ① 用户指出「在海报墙里加 table 感觉好突兀」⇒ 对截图逐条定位到 5 处：列宽等分（「收藏」列一大片死黑）、
>      纯黑底 + Excel 式**竖线网格**、表头灰条与顶栏不是同一色系、选中行是亮灰（应用别处都是橙色语言）、**没有封面** ✓；
>   ② 应用侧改成媒体库列表：第 0 列带**海报缩略图**（`image_col` + 逐行 `set_row_image` + `image_mode:2` 方图裁边、
>      行高 46 / 字号 14）、去掉空荡荡的「收藏」列（收藏改用片名前的 **★**）、
>      底色/表头/选中/分隔线**全部对齐应用色板**（`bg_color/header_color/sel_color/sep_color` + `zebra:false` 去网格感 ✓）；
>   ③ ⛔ **框架缺口（已补）**：`TabControl` 的**标签栏底 / 非激活标签底 / 两种文字色**以前**只跟主题**、opts 改不了
>      ⇒ 应用想把页签融进自己的配色时做不到，只能顶着主题那条灰底（"像外挂进来的一块" ✗）
>      ⇒ 暴露 `bar_color` / `tab_color` / `text_color` / `active_text_color` 四个 opts（`_opts_keys` 同步 ✓）；
>      ⚠ 仍**只跟主题**的：`Table` 的文字色与斑马色（`txtColor/hdrTxtColor/zebColor`）—— 暗色应用够用，本次未动 ✓。
>
>   **2026-10-13 追加（七）**：**自检时基改为定时器 + 页签/列表视图 + 抓到并修掉 Table 排序说谎**：
>   ① ⚠ **自检一律走 `Timer`**（`WALL_*` 里的数字 = 步号，100ms 一拍）：本框架是**事件驱动重绘**，
>      没有重绘请求时帧号不走 ⇒ 旧的"第 N 帧注入/截图"既不定时、还可能截到**旧画面** ✗（用户点出 ✓；
>      仓里惯例见 `cleaner_master` 的 `autoStep` 与 `dashboard_sidebar` 那条"无头下没有事件就永不重绘"✓）；
>      海报墙已改用 `win.addTimer(STEP_MS, selftest_step)`，截图仍留在 `setOverlay` ✓；
>   ② 海报墙新增**视图页签（TabControl：海报墙 / 列表）**与**列表视图（Table）**：同一份命中名单（`collect()` ✓）、
>      表头可点排序、双击进详情、右键菜单与卡片**同名同义**（`leno_gui/应用/影视海报墙/movie_wall.leno`）；
>   ③ ⛔ **抓到真 bug 并修掉**：`Table` 表头排序箭头**说谎** —— `set_rows`/`clear`/`add_row` 都不管
>      `sortCol/sortDesc` ⇒ 点「评分▼」后再换数据，行回到插入序而箭头照旧 ✗（实测 9.3 掉到第 3 行）。
>      修法：`set_rows` **重放**排序、`clear`/`add_row` **撤销**排序（口径：要么别谎报、要么别乱序 ✓），
>      应用侧改用 `set_rows` 批量换数据 ✓；速查附录 C 新增该条（含复现命令与验收判据 ✓）。
>      ⚠ 本轮只修了这一条 —— 侦察出来**尚未复现**的疑似问题另列（见下条）。
>   **2026-10-13 追加（六）**：**框架补 2 项能力 + 应用侧把海报墙做厚**（均真机实测）：
>   ① `sdl_card.leno` 新增**右键上下文菜单**（框架缺口 ⇒ 媒体库/商品墙必用的「右键 → 播放/收藏/详情」以前挂不上，只有 Table/TreeView/ListBox 有）：API 与那三者**同名同义**（`addContextItem` / `on_context_select(func(id))` / `on_context_will_open` / `clearContextItems` / `is_context_menu_open` …），菜单本体共用 `sdl_menu`、由窗口第二遍统一画；懒建（`ensure_ctx_menu`，22 张卡不会各留一个空菜单）+ `dispose` 摘注册表 ✓；顺带修掉"右键也会触发整卡 `on_click`"✗；
>   ② `sdl_scrollview.leno`：**只有横向能滚时，纵向滚轮也用来横滚**（横向列表/推荐位在普通鼠标下以前"滚轮毫无反应"✗；纵向能滚时行为不变 ✓）；
>   ③ `leno_gui/应用/影视海报墙/`：详情页加**相似推荐横排**（横向 `ScrollView` + `content_w`，推荐位复用卡片右键菜单）、网格页加**底部状态栏**（共 N 部 / 当前 / 收藏 / 排序 · 分类，`update_status` 单一出口）与**空态提示**（命中 0 部时网格区写字 —— ⚠ 必须走 `setOverlay`，放 `onRender` 会被 `ScrollView` 自己的底色整块盖掉 ✗ 同"截图钩子"那条规律）；
>   ④ 速查附录 C 新增 **1 条坑**：文本会变的 `Label` 在布局里必须给 `basis`（或 `grow`）——空文本建出来盒子宽 ≈ 0，之后 `set_text` 的文字会**溢出被窗口裁掉**（实测症状：右项只剩「默…」）。
>   **2026-10-13 追加（五）**：**澄清一条被误传的语义**（**不是 bug** —— 实现 `semantic_upvalue.c` + 回归 `assert/test_closure_value_capture.leno` 都在 ✓）：闭包的「值捕获」**只发生在循环体内**（每轮各一份 ✓）；循环外的局部量 / 模块级 / `self` 字段都是 **by-upvalue（实时）** ⇒ 捕获后改它，闭包读得到新值 ✓。『语言陷阱速查.md』新增 **㊱**（6 形态实测表 + 唯一的真坑：循环里改**外层共享**变量 ⇒ 几个闭包全读末值 ✗）；同日**全仓订正 4 个文件里的 8 处旧说法**（原文把"循环体内声明的变量才是值捕获"说成"闭包按值捕获" ✗，范围说宽了）：`leno_gui/应用/数据看板/dashboard.leno`（2 处）、`leno_gui/控件/数据看板/dashboard_sidebar.leno`（2 处）、`leno_gui/应用/影视海报墙/movie_wall.leno`（3 处，含我 2026-10-13 写详情页时顺手抄错的）、`build/leno_module/LenoSDL3/examples/基础示例/窗口管理/test_filedialog.leno`（1 处）；**剩下的"值捕获"提法都核对过**（`assert/test_closure_value_capture.leno`、`examples/性能测试/R5R6形态基准.leno`、`docs/archive/JIT闭包与upvalue设计_R5.md` 讲的都是**循环体内**那个形态 ⇒ 正确 ✓ 未动）。
> - 状态图例：**✅ 现行**（可照着写代码）· **🟡 现行但含过时段落**（文件头部已加"历史 / 局部过时"块，
>   引用前先看那一段）· **📜 历史留档**（功能已不存在或已完结，只作考古）· **🧭 台账**（待办与路线图）。
> - **维护约定**：新增 / 改名 / 状态变化（实现完成、功能移除）**都要回来改这一行**；
>   文档里**不要写死断言总数** —— 一律以 `.\build\leno.exe assert\run_tests.leno build\leno.exe assert` 实跑为准
>   （历史上同一事实被写成 245/256/263/298/305/352/366/380/410/412 十个版本）。

---

## 一、指南（面向 `leno` 使用者）

| 文档 | 一句话 | 状态 |
|---|---|---|
| [Leno入门教程.md](Leno入门教程.md) | 语言全貌教程：语法 + 类型系统 + 模块，最全的一份 | ✅ |
| [FAQ.md](FAQ.md) | 常见问题（`.leno` 直跑 vs `.lenb`、缓存、退出码…） | ✅ |
| [语言陷阱速查.md](语言陷阱速查.md) | **症状 → 病因 → 怎么办**：语言 / 编译器 / 模块 / 构建层的坑（框架层见 LenoSDL3 那份速查） | ✅ |
| [Leno_规范草稿.md](Leno_规范草稿.md) | 语言规范草案（设计口径，不是教程） | ✅（草稿） |
| [import使用指南.md](import使用指南.md) | `import` / 模块加载与缓存 | ✅ |
| [包管理与安装使用指南.md](包管理与安装使用指南.md) | `leno --init/install/publish` 与 `leno.toml` | ✅ |
| [单文件打包使用指南.md](单文件打包使用指南.md) | `-p --onefile` 打包与资源内嵌 | ✅ |
| [async_await入门指南.md](async_await入门指南.md) | 协程 / `async` / `await` | ✅ |
| [threads使用指南.md](threads使用指南.md) | 线程 / `Channel`（与 `module_threads_api.md` 有重叠） | ✅ |
| [并发选择指引.md](并发选择指引.md) | async / threads / Channel 怎么选 | ✅ |
| [类型收窄与绑定语法改进.md](类型收窄与绑定语法改进.md) | `is` 收窄与 `=>` 绑定的语义与用法 | ✅ |
| [FFI使用指南.md](FFI使用指南.md) | FFI / clib 调用全流程 | 🟡 版本历史里有一条 JIT 记录已过时 |
| [加密算法示例指南.md](加密算法示例指南.md) | `examples/` 里纯 Leno 密码学算法导览 | ✅ |
| [Leno跨模块clib开发经验.md](Leno跨模块clib开发经验.md) | 跨模块 clib 的踩坑与写法 | ✅ |

## 二、模块 API 参考（18 篇）

| 文档 | 内容 |
|---|---|
| [module_io.md](module_io.md) | 文件读写与标准流 |
| [module_files.md](module_files.md) | files 模块（注意"字节保真 / 不翻译行尾"口径） |
| [module_dirs.md](module_dirs.md) | 目录操作 |
| [module_strings.md](module_strings.md) | 字符串方法 |
| [module_arrays.md](module_arrays.md) | 数组（`arr[:]` 双侧省略**不支持**） |
| [module_dicts.md](module_dicts.md) | 字典 |
| [module_types.md](module_types.md) | 类型判定与转换 |
| [module_maths.md](module_maths.md) | 数学 |
| [module_times.md](module_times.md) | 时间 |
| [module_rands.md](module_rands.md) | 随机数 |
| [module_regexs.md](module_regexs.md) | 正则 |
| [module_jsons.md](module_jsons.md) | JSON |
| [module_sockets.md](module_sockets.md) | 网络 |
| [module_sys.md](module_sys.md) | 系统（环境变量 / 进程 / 线程） |
| [module_asyncs.md](module_asyncs.md) | 异步协程 |
| [module_ffi_api.md](module_ffi_api.md) | FFI（命名带 `_api`，历史遗留） |
| [module_cstructs.md](module_cstructs.md) | cstruct 内存布局 |
| [module_threads_api.md](module_threads_api.md) | 线程 / Channel（命名带 `_api`，历史遗留） |

## 三、GUI / LenoSDL3

| 文档 | 一句话 | 状态 |
|---|---|---|
| [LenoSDL3像素直写渲染优化.md](LenoSDL3像素直写渲染优化.md) | 像素直写渲染的优化记录 | ✅ |
| [LenoSDL3_Card卡片控件设计文档.md](LenoSDL3_Card卡片控件设计文档.md) | **卡片控件**（图 + 标题 + 角标 + **可挂子控件的内容区**）：尺寸模型 / 容器化 / 状态优先级 / 零裁剪预算 / **圆角图（走渲染器离屏 + 「只乘 alpha」自定义混合合成）** / **多角标** + 实施台账（v2.1 已落地；待办：列表行 `image_pos`、虚拟化） | 🧭 |
| [LenoSDL3_裁剪过多导致闪烁排查记录.md](LenoSDL3_裁剪过多导致闪烁排查记录.md) | clip / flush 闪烁排查（**仍有残留闪烁**） | 🟡 |
| [LenoSDL3_Table方法表断裂编译器bug排查记录.md](LenoSDL3_Table方法表断裂编译器bug排查记录.md) | 符号表截断 bug 的根因与修复 | 📜 已修（含旧断言数） |
| [LenoSDL3_打包后窗口句柄下发静默失效排查记录.md](LenoSDL3_打包后窗口句柄下发静默失效排查记录.md) | 打包后 Edit 拿不到 `hwnd` ⇒ 输入法候选框跑到窗口左上角（跨模块同名符号撞车 + 合并 `case is A, B` 被 DCE 剪成空桩） | ✅ 模块侧已修；编译器侧待治本 |
| SDL3 常用 API / 控件优化清单 | 在模块目录下：`build/leno_module/LenoSDL3/docs/` 的两篇 | ✅ |

## 四、设计 · 台账（仍在推进）

| 文档 | 一句话 | 状态 |
|---|---|---|
| [待办与路线图.md](待办与路线图.md) | **待办总表**（T1…T28 + ⑤-* 批次记录 + 数值结论），一件事一条带判据 | 🧭 |
| [线程并发能力修复追踪.md](线程并发能力修复追踪.md) | **线程/并发待做台账**（TH1…TH7：struct 跨 Channel、入口模块化、并行原语、`Channel[T]`、threads.local），含"明确不做"五项 | 🧭 |
| [类型可见性与use逃生阀修复追踪.md](类型可见性与use逃生阀修复追踪.md) | **类型可见性待做台账**（US1…US5：类型位置限定名 `m.Pt`、漏列报错定位、`use m.*` 通配、传导型 use、native 规格归并），含"明确不做"五项 | 🧭 |
| [单一事实来源与重复实现收敛.md](单一事实来源与重复实现收敛.md) | 重复实现收敛（多条目对象是已移除的 JIT） | 🟡 |
| [GC与分配优化.md](GC与分配优化.md) | GC / 分配优化交接（多处依据 JIT 实测） | 🟡 |
| [易用性痛点（TraeSign 移植实录）.md](<易用性痛点（TraeSign 移植实录）.md>) | 易用性痛点清单（部分判据是 JIT 口径） | 🟡 |
| [错误堆栈追踪改进方案.md](错误堆栈追踪改进方案.md) | 异常对象与报错文案（现行契约在"引擎错误自带位置"） | 🟡 内联器那段已过时 |
| [类型收窄与绑定语法改进.md](类型收窄与绑定语法改进.md) | `is` 收窄 / `=>` 绑定的设计 | ✅ |
| [Leno报错提示改进计划.md](Leno报错提示改进计划.md) | 报错提示改进清单 | 🟡 落点文件已删 |
| [Leno语言改进建议.md](Leno语言改进建议.md) | 语言特性去留评估（结论：不再追加特性） | ✅ |
| [语言摩擦与隐式契约清单.md](语言摩擦与隐式契约清单.md) | 语言层隐式契约 / 类型洞 / 词法摩擦（M1…M10；含探针与修法建议） | ✅ 全部有结论（M1~M7、M9、M10 已修、M8 定口径、M5c 已决不修） |
| [cstruct_packed_align_设计文档.md](cstruct_packed_align_设计文档.md) | cstruct `packed` / `align(N)`（已实现） | ✅ |
| [struct 里const可行性.md](<struct 里const可行性.md>) | struct 关联常量可行性分析 | 📜 分析稿 |
| [VM字节码校验与崩溃防护设计方案.md](VM字节码校验与崩溃防护设计方案.md) | 字节码校验分层防护（复盘的内联器 bug 已随文件删除） | 🟡 教训仍有效 |
| [解构语法实现文档.md](解构语法实现文档.md) | 解构声明语法与语义（Phase 1/2/3） | 🟡 引用表含已删文件 |
| [错误列号准确性_工作进度.md](错误列号准确性_工作进度.md) | 编译器错误列号准确性进度 | ✅（进度文档） |
| [字节码优化分析.md](字节码优化分析.md) | 融合指令建议清单（栈式时代，未回填状态） | 🟡 |
| [字节码融合指令优化_光线追踪对象版.md](字节码融合指令优化_光线追踪对象版.md) | 融合指令落地记录（模型描述是栈式） | 🟡 |
| [优化方案_内存态序列化运行.md](优化方案_内存态序列化运行.md) | 内存态序列化 / `.lenb` 运行方案 | ✅ |
| [FFI优化方案.md](FFI优化方案.md) | FFI 模块优化清单（两条依赖 JIT） | 🟡 |

## 五、性能记录

| 文档 | 一句话 | 状态 |
|---|---|---|
| [性能优化记录.md](性能优化记录.md) | fib(30) 与操作栈优化的取舍记录（**整篇是栈式 VM**） | 🟡 |
| [函数调用性能优化_调用帧与多返回值解构.md](函数调用性能优化_调用帧与多返回值解构.md) | 调用帧 / 多返回值优化（含"否决寄存器机"的旧结论） | 🟡 |
| [代码生成性能排查记录.md](代码生成性能排查记录.md) | codegen 首次编译 4.3s 的瓶颈排查 | ✅ |
| [leno对比.md](leno对比.md) | Leno vs Python 早期实测对比 | 📜 已被归档的性能总结取代 |

## 六、排查记录（已完结，留档）

| 文档 | 一句话 |
|---|---|
| [枚举求值与模块符号表问题.md](枚举求值与模块符号表问题.md) | 枚举三路径求值不一致 + 符号表扫描（全部已修；含已删文件的行号） |
| [多线程struct与模块全局变量问题记录.md](多线程struct与模块全局变量问题记录.md) | 线程 struct 定义已修，模块级全局变量串台待重构 |
| [LenoNet_崩溃问题排查记录.md](LenoNet_崩溃问题排查记录.md) | HttpClient 堆损坏崩溃（已修；根因文件已删） |
| [LenoNet开发问题记录.md](LenoNet开发问题记录.md) | libcurl 绑定踩坑 |
| [LenoHtml_开发问题记录.md](LenoHtml_开发问题记录.md) | HTML 解析器开发中暴露的编译器问题（术语是栈式） |
| [内联器崩溃排查记录.md](内联器崩溃排查记录.md) | 内联器 `patch_ast_indices` 越界导致的间歇 segfault（**方法论与教训仍有效**） |
| [窥孔优化复盘.md](窥孔优化复盘.md) | 窥孔优化 bug 复盘（旧缓存名 `.lenocache`） |

## 七、已归档（`docs/archive/`）

| 文档 | 一句话 | 为什么归档 |
|---|---|---|
| [JIT实现与调试记录.md](archive/JIT实现与调试记录.md) | JIT 架构 / 寄存器约定 / bailout / 踩坑（7104 行） | JIT 已不在代码中 |
| [JIT模块调用优化与bailout排查记录.md](archive/JIT模块调用优化与bailout排查记录.md) | JIT callout 三项优化与 bailout 排查 | 同上 |
| [JIT闭包与upvalue设计_R5.md](archive/JIT闭包与upvalue设计_R5.md) | JIT 闭包 / upvalue 设计稿 | 同上（当时即未动手） |
| [JIT安全点与去优化_参考实现调研.md](archive/JIT安全点与去优化_参考实现调研.md) | JIT 安全点 / GC 的行业调研 | 同上 |
| [性能测试总结_Leno_vs_Python.md](archive/性能测试总结_Leno_vs_Python.md) | JIT vs VM vs Python 的基准与结论 | JIT 列全部失效 |

> ⚠ **JIT 不在本仓库的历史里**：`git ls-files "*jit*"` 与
> `git log --all --diff-filter=AD -- "*jit*"` 均为空（最早提交 `e8ccc46 2026-09-20 Initial commit`）
> ⇒ 归档文档里的 `src/jit/*`、`LENO_NO_JIT=1`、`JIT_HOT_THRESHOLD` 等**别去当前源码里找**，
> 其中的 `file:line` 一律不可用。

**另外两篇"事实上已归档但没搬家"**（被多处按名引用，路径保持原样）：

| 文档 | 状态 | 不搬的理由 |
|---|---|---|
| [寄存式与栈式的差异清单.md](寄存式与栈式的差异清单.md) | 📜 正文首部已自标归档（2026-09-23），46 处讲"与栈式对照" | 被 11 处按名引用，且是 `待办与路线图.md` 的历史锚点 |
| [窥孔优化复盘.md](窥孔优化复盘.md) | 📜 讲的是已重写的栈式 codegen | 方法论仍被引用 |

## 八、图片与其他

| 资源 | 用途 |
|---|---|
| `docs/images/dashboard-main.png` / `-usage.png` / `-checkin.png` / `-settings.png` | 两份 `README` 里的数据看板真实截图（`leno_gui/应用/数据看板/`） |

## 九、本轮（2026-10-02）变更摘要

1. **新增本索引**；两份 README 的文档段落改为"以本索引为准 + 保留常用链接"。
2. **JIT 系 5 篇 `git mv` → `docs/archive/`**（共 8218 行 / 767KB ≈ docs 字节数的 40%），
   每篇头部加统一归档块，全仓引用（含 `src/*.c` 注释、`assert/*.leno`、两份 README）同步改路径。
3. **5 篇杂名英文改名**：`error_column_progress` → `错误列号准确性_工作进度`、
   `codegen-perf-investigation` → `代码生成性能排查记录`、`peephole-optimization-postmortem` → `窥孔优化复盘`、
   `debug-inline-segfault` → `内联器崩溃排查记录`、`destructuring_design` → `解构语法实现文档`。
4. **给 17 篇加"历史 / 局部过时"头**（正文一字不动）：凡"把栈式 / JIT 当现状""引用已删文件
   （`codegen_inline.c`、`op_*.inc`、`src/jit/*`）""写着旧版本号 / 旧断言总数"的，一眼能看到。
5. `Leno入门教程.md` 另修一处**事实错误**：原文"多返回值函数不会被内联" ⇒ 现状是
   **所有脚本函数调用都是真调用**（寄存器式 codegen 缺函数内联器，见 `待办与路线图.md` 第六节）。

## 十、为什么保持扁平（没建子目录）

`docs/*.md` 的路径被 **67 个文件**按名引用（含 `src/*.c` 注释、`assert/*.leno`、`leno_gui`、
`build/leno_module`、两份 README）。热点：`单一事实来源` 28 处、`JIT实现与调试记录` 32 处、
`待办与路线图` 21 处。换成子目录/前缀要同步上百处引用，收益不抵风险 ⇒
**布局保持扁平、分类交给本索引**；`docs/archive/` 只放"整篇都是历史"的文档。
`module_*.md` 的命名约定（18 篇）也被两份 README 当规格引用，一并保持不动。
