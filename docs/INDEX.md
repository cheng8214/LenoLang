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
