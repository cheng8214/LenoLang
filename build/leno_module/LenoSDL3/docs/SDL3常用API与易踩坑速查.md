# SDL3 常用 API 与易踩坑速查

> 目的：把「翻示例才知道」的点集中到一处 —— 尤其是**实测确认过、但读代码看不出来**的行为。
> 范畴：`leno_module/LenoSDL3/lib/` 的窗口 / 布局 / 控件 / 渲染 API。
> 约定：本文只写**实测**结论 ✓；推测的地方显式标 ⚠，不要当结论用 ✗。

---

## 一、窗口与主循环

### `run` 需要**两个**回调 ✓

```leno
win.run(
    func(Event ev): bool { if ev.isQuit() or ev.isWindowClose() { return false } return true },
    func(Renderer r) { r.clearColor(#F0F1F4FF) }
)
```

- `onEvent` `(Event) -> bool`：返回 `false` 结束主循环 ✓
- `onRender` `(Renderer)`：每帧绘制清屏 / 背景 / 网格 ✓

⚠ 只给一个回调时，报的是「只能调用函数（不是对象类型）」——**看不出缺的是哪个参数** ✗（本人踩过 ✓）。

### 三个绘制层的先后（**实测确认** ✓，不是推测）

```
① onRender（run 的第二个回调）—— 最底层，在控件之下
② 控件树（_root 根布局 + addXxx 注册的控件）
③ setOverlay 回调 —— 最上层，盖住一切
```

可复现的验证方法 ✓：放三个黄色 Panel（控件层），`onRender` 画绿、`setOverlay` 画蓝，读回像素 ——
「控件 + onRender」处**仍是黄** ✓（⇒ onRender 在下层）、「控件 + overlay」处**是蓝** ✓。

⇒ 想让自绘**盖在控件之上**必须放 `setOverlay` ✓；放 `onRender` 里会被控件盖掉 ✗。

### 不需要 `addTimer` 空转

主循环本来就一直跑 ✓，退出只由 `onEvent` 返回 `false`（或调用 `_exit`）决定 ✓。

### 从"非 onEvent 上下文"退出 / 关闭：两个新入口 ✓

托盘菜单回调、定时器、渲染回调里**没法 `return false`** ⇒ 用这两个：

```leno
SDL3.requestExit()      // 请求主循环收尾（销毁所有窗口并返回，与 SDL_QUIT 同一条清理路径 ✓）
win.requestClose()      // 投递一条 WINDOW_CLOSE_REQUESTED ⇒ 和用户点 X 走**同一条** onEvent 路径 ✓
```

- `requestExit()` 不是立即返回 ✓：动作在**主循环下一轮**生效（不打断当前事件/渲染处理）✓
- `requestClose()` 返回"投递是否成功" ✓；窗口关不关由 `onEvent` 决定（含下面的 `cancelClose` ✓）

### 「关闭到托盘」：`win.cancelClose()` ✓（实测）

```leno
win.run(func(Event ev): bool {
    if ev.isWindowClose() {                 // 关闭按钮（或 requestClose ✓）
        win.hide()                          // 先收进托盘
        win.cancelClose()                   // ★ 拦下"销毁窗口" ⇒ 主循环不退出 ✓
        return true
    }
    return true
}, ...)
```

- **实测**（`leno_gui/应用/模拟时钟` 的 `CLOCK_SELFTEST=1` ✓）：拦下后窗口**未销毁**、程序继续跑、
  再从托盘 `win.show()` 可以正常恢复 ✓；不调用 `cancelClose()` 时点 X = 老行为（销毁窗口 ⇒ 退出）✓
- 只对**这一次**关闭请求有效 ✓（标记每次进处理器前清零 ⇒ 不会跨事件残留 ✗）
- ⚠ 没托盘时别拦：窗口收起来就**没入口恢复**了 ✗（模拟时钟里 `if tray == null { return false }` ✓）

---

## 二、控件创建：`SDL3.createXxx` 与 `win.addXxx` 的区别 ✓

| 写法 | 是否注册进窗口 | 谁负责绘制 | 用在哪 |
| --- | --- | --- | --- |
| `SDL3.createXxx({...})` | ✗ 不注册 | 交给容器（布局 `add` / `panel.add` / `tab.addToPageLayout`）✓ | **放进布局的控件** ✓ |
| `win.addXxx({...})` | ✓ 注册 | 窗口自动绘制 ✓ | 用窗口绝对坐标直接摆放 ✓ |

⚠ 混用 ⇒ 同一控件**被窗口画一次、又被布局摆一次**（重复绘制 / 位置打架）✗
⇒ 放进 `HBox/VBox/AnchorBox/Panel/TabControl 页` 的控件**必须**用 `createXxx` ✓。

---

## 三、布局 `{basis, grow, shrink, align}`

### 语义（`sdl_layout.leno` 的 `_relayout` ✓）

```
free = 容器主轴内尺寸 − Σbasis − spacing × (n − 1)
```

- **grow**：**权重**（不是像素）✓ ⇒ 按权重分 `free`：`basis + free × g / Σg` ✓
- **shrink**：空间不足时按 `shrink × basis` 的比例收缩 ✓；写 `shrink:0` 表示**不收缩**、允许溢出 ✓
- **basis**：主轴基准尺寸 ✓；**不写**则用控件「自然尺寸」—— 注意是 **add 那一刻的 `get_w()/get_h()`** ✓，之后被 `set_size` 改写也不影响它 ✓
- **align**：交叉轴 `0=start / 1=center / 2=end / 3=stretch` ✓；子项有 `grow` 时交叉轴自动撑满 ✓

### 两个最常踩的坑 ✗

1. **写了 `basis` 却忘了 `grow`** ⇒ 窗口变宽它**不跟着长** ✗（看起来像"没铺开"）。
   按比例铺开要写 `{basis:w, grow:w}` —— 因为 grow 是权重，比值才生效 ✓。
   实测自洽：`1364 = 190 + 1210 − 12 − 24` ✓。
2. **把 `basis` 当"固定宽度"用** ✗ —— 它只是分配基数 ✓。真要钉死：`{basis:w, grow:0, shrink:0}`
   （⚠ `shrink` 默认是 1 ⇒ 空间不足时仍会被压缩 ✓）。

### 排障：用 `dumpLayout`，不要靠截图扫像素 ✓

```leno
win.dumpLayout()              // 整个窗口（根布局 + 已注册控件）
SDL3.dumpLayout(某个容器)      // 任意子树
```

- 打印：容器的 `@(x,y) / pad / spacing / inner / basis合计 / free / sumGrow` ✓，
  子项的 `w×h / nat / grow / basis / align / left|top|right|bottom` ✓
- 自动提示：`free > 0` 而某子项 `grow = 0` ⇒ 标注 `free 未被吸收` ✗ —— 这正是"没铺开"的最常见根因 ✓
- 下钻能力（实测 ✓）：`TabControl → 页布局 VBox → ScrollView → 内容 VBox → HBox → Panel` 全链路可见 ✓

---

## 四、抗锯齿（渲染器级）

**已 AA ✓**：圆角矩形（填充 / 描边）✓、圆（填充 / 描边 —— 内部就是「半径取半宽的圆角矩形」✓）、
**斜线**（线段走 1D 剖面纹理 ✓，钟表指针 / 刻度 / 趋势线都属这类 ✓）。

**刻意不 AA ✓**：轴对齐的横竖线 —— **包括落在半像素位置的那些** ✓。
原因：老路径给的是「清晰的 1px，只是位置偏半格」✓；改成 AA 会变成 2px 虚边 ✗ ⇒ UI 里**清晰**比"亚像素精确"重要 ✓
（网格线 / 分隔线全属这类 ✓）。

**仍是硬边 ✗**：`fillPie`（扇形）、半像素位置的矩形边界。大半径超过 256 的图元也回退几何路径 ✓。

**原理一句话** ✓：不再要求 SDL 光栅器吐软边（它按像素中心采样、压根不做图元 AA ✗），
而是 **CPU 按覆盖率算好 alpha 写进纹理，GPU 只做采样** ✓ ⇒ 平滑来自**像素内容**，不受光栅算法限制 ✓。

⚠ 副作用（有意 ✓）：1px 描边现在落在**路径内侧那一圈像素**上（清晰 + 左右对称 ✓，取代原先的"居中 + 半透明虚边" ✓）。

回归用例见文末 ✓。

---

## 五、编译器给出的新提示（读提示，别猜 ✓）

- **struct 字段默认值**：只能是常量 ✓。需要对象 / 控件实例时，提示会直接给出可照抄的写法
  （`Inner? _i = null` ✓ ⇒ 可空字段 + 用前判空创建 ✓）。
- **警告「字段未初始化」** ✓：标量字段（`int/float/bool/string`）不写初值时，运行期是 **null 而不是 0** ✗
  ⇒ `k + 1` 会直接报「加法运算: null 不能参与运算」并终止 ✗。
  `Array` 字段自动成空数组 ✓、`Ptr` 字段自动成 null ✓（两者安全 ✓），所以警告只针对标量 ✓。
- **import 写法**：`import "SDL3"` 与 `import "SDL3.leno"` **等价** ✓
  （裸文件名带 `.leno` 后缀时也会走模块搜索路径 ✓；带路径的写法仍按相对 / 绝对路径解析 ✓）。

---

## 六、系统托盘（`SDL3.createTray`）✓

### 最小用法（完整示例见 `examples/基础示例/窗口管理/test_tray.leno` ✓）

```leno
Tray? trayOpt = SDL3.createTray({icon: surface 或 null, tooltip: "我的应用"})
if trayOpt == null { return }                // Type? 判空 —— 非可空 struct 判空会触发 [struct与null比较] 警告 ✗
Tray tray = trayOpt                          // 剥离可空 ✓
TrayMenu menu = tray.getMenu()
menu.addButton("显示", func() { ... })
menu.addCheckbox("置顶", false, func() { ... })
menu.addSeparator()
menu.addButton("退出", func() { ... })
```

- **必须用 `win.run()` 驱动** ✓：托盘回调靠消息循环冒出来，事件循环不 drain 队列 ⇒ 回调不执行 ✗（见 `docs/BUG_TRAY_COMPILER.md` ✓）
- **线程/上下文有官方依据** ✓（3.4.10 的 `SDL_tray.h`）：`SDL_CreateTray` 只能**主线程**调；其余托盘 API
  "should be called on the thread that created the tray" ⇒ 回调就在主线程 ✓。即便如此仍建议
  **回调里只写标记**、真正的窗口/托盘操作放主循环 —— 因为回调里**没法 `return false`** 退出主循环 ✓
- **勾选态**：SDL 在回调**之前**就已切换 ✓，但别去反查它 —— 用应用状态当唯一真相，回调后
  `SDL3.setTrayEntryChecked(entry, 应用状态)` 强制同步 ✓（幂等 ✓）
- `setTrayEntryLabel/Checked(...)` 对 **null 条目是安全空操作** ✓（托盘创建失败时菜单句柄全为 null，不用到处判空 ✓）
- **图标**：`createTray({icon: surface})` 的 surface 在创建时就转成平台图标 ✓（SDL 3.4 windows 后端
  `WIN_CreateIconFromSurface`）⇒ 之后释放 surface 是安全的 ✓；没图标时用系统默认图标 ✓
- **无头自测**：`SDL3.clickTrayEntry(entry)` 模拟点条目（**同步**执行其回调 ✓）
  （勾选态不会被它切换 ⇒ 测试里自行 `setTrayEntryChecked` 保持一致 ✓）

### ⛔ 原生菜单在 Windows 上会**冻结画面**（实测 ✓）—— 所以要自绘

两条实测结论（都有一手依据，不是推测）：

1. **菜单开着 = 主循环被按住** ✓：SDL 的 windows 后端在托盘窗口的 WndProc 里**同步**调
   `TrackPopupMenu`（模态消息循环，源码 `src/tray/windows/SDL_tray.c`），而那个 WndProc 是在我们
   `ev.poll()`（`SDL_PollEvent`）里被调进去的 ⇒ 期间定时器不 tick、`onRender` 不跑、画面**冻住** ✗
   （实测：模拟时钟挂原生菜单时秒针停；见 `leno_gui/应用/模拟时钟` 的注释 ✓）
2. **SDL 不暴露"托盘图标被点"** ✓：3.4.10 的 `SDL_tray.h` 只有条目回调，没有 `SDL_EVENT_TRAY_*`
   ⇒ 想"点托盘图标弹自绘菜单"，必须**拦下托盘窗口的 WndProc** ✓

⇒ 本仓库的做法（**自绘菜单 + 拦截托盘点击**，模拟时钟用的就是这套 ✓）：

```leno
Tray tray = SDL3.createTray({icon: surf, tooltip: "..."})   // ★ **不要调 getMenu()**：不建原生菜单
bool hooked = SDL3.interceptTrayClick()                     // 拦下图标被点（Windows 子类化 SDL_TRAY 窗口 ✓）

PopMenu pm = SDL3.createPopMenu({})                          // 自绘菜单：无边框/置顶/**非模态** ✓
pm.addItem("隐藏窗口", imgPath("eye_off.png"), func() { ... })   // 图标 = **图片路径**（与 MenuBar 的 image: 同义 ✓）
pm.addCheck("窗口置顶", imgPath("pin.png"), false, func() { ... })  // 勾选项：右侧强调色圆点 ✓
pm.addSeparator()
pm.beginSubmenu("显示设置")                                   // **下一级**（飞出列 ✓ 可嵌套）
pm.addCheck("12 小时制", imgPath("clock.png"), false, func() { ... })
pm.endSubmenu()
pm.addItem("退出", imgPath("exit.png"), func() { ... })

// 主循环（定时器）里：
var p = SDL3.pollTrayClick()                                 // [] = 没点；[x, y, 键]（键 1=左 2=右 ✓）
if p.len() >= 3 {
    if _int(p[2]) == 2 { pm.showAt(p[0], p[1]) }              // 右键 ⇒ 弹菜单（底边贴光标 + 夹进屏幕可用区 ✓）
    else               { /* 左键 ⇒ 直接切显示/隐藏，应用自己定 ✓ */ }
}
pm.poll()                                                     // 点在菜单外自动收起（不依赖焦点 ✓）
```

- `interceptTrayClick()` 返回 false（非 Windows / 找不到窗口）⇒ 静默降级：托盘只剩图标与提示 ✓
- 拦截实现 = `FindWindowA("SDL_TRAY")` + `SetWindowLongPtrW(GWLP_WNDPROC)` 换掉 WndProc，
  只截 `WM_TRAYICON` 且通知码为 `WM_CONTEXTMENU/WM_RBUTTONUP`（= 右键）或 `WM_LBUTTONUP`（= 左键），
  **返回 0、不链回 SDL**；其余消息 `CallWindowProcW` 原样交还 SDL ✓（已实测：`msg=1025` 命中、主循环照转 ✓）
- ★ **顺序**：`Tray.destroy()` 内部**先恢复原 WndProc 再**销毁托盘 ✓（反了会打到已释放的托盘数据 ✗）
- 自绘菜单的关闭时机：点条目 / 点菜单外 / 失焦 / ESC ✓；**点父项 = 展开/收起下一级** ✓
- 图标 = **图片路径**（与 MenuBar / TreeView 的 `image:` **同一套约定** ✓）：按 path 载纹理 + **缓存**
  （MRU 快路径，同 `sdl_menu.leno` 的 `_get_tex` ✓），`aspectFit` 居中到 16px 槽位 ✓
  → 图标资源放**各应用自己的 `images/`**：`_imgDir = dirs.join(dirs.res_dir(), "images")` ✓（读资源用
  `res_dir`，别用 `script_dir` ✗ —— 打包后只有 `res_dir` 指向释放出来的资源 ✓ 见 `dirs` 模块注释 ✓）
  → 换图标 = `setIcon(idx, 路径)` ✓（模拟时钟就是用它把"隐藏窗口/显示窗口"两个图标来回切 ✓）
- 勾选态/文字/图标由调用方维护（`setChecked/setText/setIcon`，菜单**不反查** ✓）；
  展开态可程序化（`setExpanded/isExpanded`）✓；`clickItem(idx)` 等价于用户点了它（无头自测 ✓）
- 子菜单指示是 `drawTriangle` 实心三角（与 MenuBar 一致 ✓）：收起朝右、展开朝下 ✓
- ★ **子菜单 = 另一块独立面板**（不是同一块里分列 ✗）：菜单窗口是 **TRANSPARENT** 的，
  每级各画一块圆角面板、彼此**紧贴**（`_GAP = 0` ✓ —— 别留缝 ✗：透明窗口里缝会直接透出桌面，
  看着像断开），交界处再补一条 2px 的**底色"桥"**盖掉两边描边与抗锯齿缝 ⇒ 连成一体 ✓
  （`_render` 开头必须 `clearColor(#00000000)` 清透明，否则上一帧残留 ✗ —— 透明窗口时主循环**不填背景** ✓）
- ★ **位置规则（实测断言过 ✓）**：主列位置**只由自己决定**（底边贴光标 + 按自身尺寸夹紧）；
  子列右边放得下就贴父列右侧，放不下就**翻到父列左边** —— 两种情况下主菜单都**纹丝不动** ✓
  （之前的写法是把整个窗口夹回屏幕内 ⇒ 子菜单一展开就把主菜单挤走 ✗，已改 ✓）
  验证方式：`_pm._x + _pm._colX[l]` 即第 l 级的**屏幕 x** ⇒ 断言"展开前后主列屏幕 x 不变" ✓
  以及"贴屏幕右边缘时子列屏幕 x < 主列屏幕 x"（翻边 ✓）✓
- ⚠ 子菜单展开时**窗口矩形变大**（覆盖主面板 + 子面板的并集 ✓；窗口本身是透明的 ⇒ 间隙透桌面 ✓），
  但**主面板的屏幕位置不变** ✓（排版按弹出时的光标锚点重算 ✓）
  （鼠标悬停父项也会展开 —— 这条只能真机验 ✓）
- ⚠ **无头测不了拦截链**：`SDL_VIDEODRIVER=dummy` 下 SDL **不跑 Win32 消息泵** ⇒ 托盘窗口收不到
  任何消息 ✗（实测）⇒ 自检要么直接调自己的分发函数，要么驱动菜单窗口本身（`showAt/clickItem` ✓），
  拦截链得真机跑（`SDL3.clickTrayIcon(1|2)` 可合成左右键 ✓）

### ⚠ 多返回值只能**解构**，不能下标（本模块真崩过一次 ✗）

`getGlobalMouse()` / `measureString()` 这类返回 `[int, int]` 的函数是**多返回值**：

```leno
var[int, int](gx, gy) = core.getGlobalMouse()   // ✓ 正确（仓库里其它地方都这么写，见 sdl_titlebar.leno）
var gm = core.getGlobalMouse(); gm[0]           // ✗ 运行时抛「下标访问: 对象不支持索引」
```

- 这种写法**编译期不报错**（下标记在 any 上能过 ⚠）⇒ 只有真跑到那一行才炸 ✗
- 更隐蔽：那行前面有"没鼠标按下就 return"的守卫 ⇒ **无头自检/探针都碰不到** ✗，
  是用户手点才暴露的 ⇒ 这类分支要**单独造条件**验（本次用 PowerShell 注入一次真实按下 ✓）
- 顺带：判"点在菜单里"那段已抽成纯函数 `_insidePanels(lx, ly)` ⇒ 自检能直接断言 ✓
  （面板内 / 子面板内 / 外面 三种都进了自检 ✓）

### 自检断言别用绝对时刻 ⚠⇒✗（实测踩过）

首帧预热（渲染器/字体/首帧 present）在本机可到 **几百 ms** ⇒ 80ms 的定时器可能 260ms 才到 ✗。
断言要写成"**等它真发生**"（状态机逐拍推进），不要写"第 X 毫秒时它应该已经变了" ✗
（模拟时钟的自检就是这么写的 ✓）。

---

## 七、回归用例怎么跑

目录：`build/leno_module/LenoSDL3/examples/`

```
$env:SDL_VIDEODRIVER='dummy'          # 无头
$env:BSHOT='out.png'                  # 截图输出路径（部分用例需要）
build\leno.exe --no-cache build\leno_module\LenoSDL3\examples\图形绘制\test_antialias.leno
```

| 用例 | 覆盖什么 |
| --- | --- |
| `图形绘制/test_antialias.leno` | 圆角矩形 + 1px 描边环 + 正圆（纯黑白 ⇒ 覆盖率 = (255−R)/255 ✓，边缘有几档灰阶一目了然 ✓） |
| `图形绘制/test_aa_clock.leno` | 大圆 + 斜线指针/刻度（`leno_gui/应用/模拟时钟/clock.leno` 的静态复刻） |
| `图形绘制/test_aa_radii.leno` | `r=10/20/40/80` 圆环 —— 回归「环墨迹压在蒙版纹理末列 ⇒ 整列丢失」那个坑 ✗ |
| `其他测试/test_draw_order.leno` | 实测三个绘制层先后（见第一节 ✓） |
| `其他测试/test_dump_layout.leno` | `dumpLayout` 输出示例（含"有剩余空间但无人 grow"的场景 ✓） |

托盘 / 自绘菜单 / 关闭到托盘（**无头可跑 ✓**，退出码 0 = 全过）：

```
$env:SDL_VIDEODRIVER='dummy'
$env:CLOCK_SELFTEST='1'
build\leno.exe --no-cache leno_gui\应用\模拟时钟\clock.leno   # 17 项：左右键分发·子菜单下一级·菜单图标/勾选·显隐·关窗收托盘·置顶·退出 ✓
```

⚠ 「托盘图标被点 ⇒ 拦截 ⇒ 弹菜单」这条链**要真机跑**（dummy 下没有 Win32 消息泵 ✗，见 §六）。
真机快速验证：`set LENO_SDL_FRAMES=40` 后直接跑模拟时钟 —— 右键托盘图标应在光标处弹出菜单、
左键应直接切显隐、秒针在菜单开着时照走 ✓
（想连子菜单一起看：把菜单画进主窗再截图 —— `win.setOverlay` 里调 `pm._render(r)` +
`r.readPixelsAt(...)` + `SDL3.saveImagePNG` ✓，本次就是这么做布局核对的 ✓
⚠ 但**图标**在这种探针里可能不出现：图片 DLL / 相对路径是按**脚本所在目录**找的 ✗
⇒ 看图标请直接跑应用本身 ✓）
