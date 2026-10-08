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

**仍是硬边 ✗**：半像素位置的矩形边界；大半径超过 256 的圆/圆角矩形会回退几何路径 ✓。
（`fillPie` 扇形已于 2026-09-27 补上 AA ✓：扇体内缩 0.5px + 弧与起始半径走**线段剖面几何**
 —— 与 `spinner` 的弧同款；数据看板饼图的"外弧台阶"就是它 ✓）

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

### 开机自启动（注册表 Run 键 + `--tray` 约定）

托盘常驻类应用都要"开机自启"；本仓库的做法（`Trae签到/trae_tray.leno` ✓）：

```leno
// 1) 往 HKCU\...\Run 写值（用 LenoWin32 的 w32_reg ✓）
OpenKeyResult r = reg.openKey(reg.HKEY_CURRENT_USER,
    "Software\\Microsoft\\Windows\\CurrentVersion\\Run", KeyAccess.ALL_ACCESS)
reg.writeString(r.handle, "MyAppName", cmd)      // cmd = 要开机执行的命令行 ✓
reg.closeKey(r.handle)                            // 关 = deleteValue(r.handle, "MyAppName") ✓
// （HKEY_CURRENT_USER 是 export var ⇒ 不能 use，直接 reg.HKEY_CURRENT_USER ✓）
```

- **命令行怎么拼**：`"<exe>" "<脚本>" --tray` —— exe 路径取 **LenoWin32 的 `Win32.exePath()`**
  （模块内 `GetModuleFileNameW` + `ffi.utf16_to_utf8` ✓，UTF-8 出、中文路径不乱码 ✓，
  取不到给空串）；打包运行（`dirs.res_dir() != dirs.script_dir()` ✓）时只写 exe ✓
  ⚠ 应用侧要 `import "Win32"`（包**入口名**）—— 裸文件名 `import "w32_process.leno"` 会走全局
  搜索路径"先到先得"，别的包 lib/ 下有同名文件就被顶掉（实测踩过，见 `Win32.leno` 顶部注 ✓）
- **约定**：命令尾部带 `--tray` ⇒ 应用**启动即隐藏到托盘**（自己解析 `_args()` ✓）
  —— 否则开机时窗口会自己蹦出来 ✗
- **自检怎么写**：注册表是**真实系统状态** ⇒ 自检要用**临时值名**（如 `MyAppName_selftest` ✓），
  跑完立刻删掉 ⇒ 不污染用户真实自启 ✓（并读回值核对命令行 ✓）

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

### ⛔ 自检钩子若用 `_exit(0)` **强杀**，就永远测不出"退出卡住" ✗（2026-10-07 实测）

- `DSHOT` / `DAUTO` 这类截图/自动退出钩子若走 `_exit(0)` ⇒ 进程被**强杀** ✓ ⇒ 它**绕过了**正常收尾
  （dispose 控件 → 释放设备/线程 → uninit ✓）⇒ 「点退出卡着」这种 bug 在自检里**永远不暴露** ✗✗
- 要测退出：用 `SDL3.requestExit()` ✓ —— 它和 `SDL_QUIT` 走**同一套**收尾流程 ✓（见 sdl_window ✓）
  ```leno
      $env:DEXIT='20'      // 到第 20 拍走正常退出路径
      win.addTimer(120, func() { if 到点了 then SDL3.requestExit() })
  ```
  然后**用超时判定**（PowerShell 例 ✓）：
  ```powershell
      $p = Start-Process build\leno.exe -ArgumentList '--no-cache','app.leno' -PassThru -NoNewWindow
      if ($p.WaitForExit(9000)) { "已退出 code=" + $p.ExitCode } else { "★ 卡住"; $p.Kill() }
  ```
- ⚠ 常见"退出卡住"的原因：**外部设备/线程没释放** ✗ —— 音频设备（miniaudio）、下载 worker 线程、
  串口/网络连接等 ✓ ⇒ 退出前显式关掉它们（在 `win.run(...)` 返回后统一收摊 ✓）
- ⚠ 还有一类"看着像卡住、其实是**故意不关**"：应用里有"有任务在跑就别退"的守卫 ✗
  ⇒ 退出前先看**状态栏那行字**（通常会写明为什么不给退 ✓）

> **一条命令的 GUI 门禁**（仓库根）：`build\leno.exe assert\run_gui_checks.leno`
> ① 递归编译 `leno_gui` 下所有 `.leno`（跳过 `.lenocache` / `_tmp*`）② 无头自检
> `dashboard` / `dashboard_sidebar`（`SDL_VIDEODRIVER=dummy` + `DSHOT` 截图后退出）与
> `cleaner_master`（`--autotest`）③ 跑主套件 `assert\run_tests.leno`。
> 全通过 ⇒ 退出码 0；任何失败 ⇒ 打印失败项 + 退出码 1 ✓
> （写它时踩到的四个坑都记在文件注释里：`set VAR="值"` 会把引号存进值 ✗、
>  判截图落地要用 `files.exists` ✗、Win32 截屏在无头驱动下会挂 ✗、`_exec` 命令别加引号 ✗）


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

---

## 八、时间 / 截图 / 圆环：本轮实测新增的三条 ✓

### ⛔ `times.now()` 返回的是 **int 时间戳**，不是 `[年,月,日,…]` 数组

**症状**：`var d = times.now() as Array[int]` **直接抛异常**；若外面套了 `catch { }`（不少应用都这么写）
就成了**静默失败** —— 表面"功能没反应"，很难查 ✗。实测：`type(times.now()) == "int"`，值形如 `1790597673`。
（真实案例：电脑清理大师的「上次清理记录」与「导出报告」都因此**从未生效** ✗，直到查导出静默失败才挖出来。）

**正确用法**（strftime 风格 ✓）：
```leno
string ts = times.format(times.now(), "%Y-%m-%d %H:%M:%S")     // 2026-09-28 20:15:27
string fn = "报告_" + times.format(times.now(), "%Y%m%d_%H%M%S") + ".txt"
```

**教训**：`catch { }` 会把这类**类型错误**一起吞掉 ⇒ 调试期先让 catch 出声
（`catch e { print(_str(e)) }`），定位完再收回去 ✓。

### ⛔ 截图：钩子放 `setOverlay`，且**要等重绘**

- 钩子放 `onRender` 回调 ⇒ 拿到的是**只有底色的空白图** ✗（控件在回调**之后**才绘制）。
  实测：错放时 PNG 仅 1137 字节的纯底色；移入 `setOverlay` 后 10430 字节、内容完整 ✓。
- **还要等重绘**：在事件回调里"改完状态立刻截"⇒ 屏幕上仍是**上一帧** ✗
  （我抓到的正是"扫描完成"而不是刚切过去的"报告态"）。放到定时器里晚 1.6 秒再截即可 ✓。

### ✅ `TabControl` 同页混用两种定位方式时**会提示一次**（新增 ✓）

`addToPage(page, c, dx, dy)`（绝对）与 `setPageLayout` + `addToPageLayout`（布局）**同一页混用**时，
绝对定位的控件不参与布局流 ⇒ 窗口缩放时和布局内容重叠/错位 ✗
（`leno_gui/应用/数据看板/dashboard.leno` 头部注释记录的那个"中间状态"就是这一类 ✓）。
以前只有注释提醒（**注释拦不住人** ✗），现在第一次误用会打印一条 `[TabControl 用法提示]`：

- **每页只提示一次**（不会刷屏 ✓）；若确实是有意叠加的浮层，忽略即可 ✓
- 两个方向都查：布局后再绝对、绝对后再布局 ✓；重复 `setPageLayout`（重建/换布局）**不**误报 ✓
- 仓库现状：每页只用一种 ⇒ 该提示在既有代码上**零命中**
  （已无头实跑 `dashboard` / `dashboard_sidebar` 确认：退出码 0、提示 0、报错 0 ✓）

### ⛔ 空 `catch { }` 是**编译期错误**（2026-09-28 由警告升为错误 ✓）

`catch` 体里**一个语句都没有**时（**只写注释也算空** —— 注释不是 AST 节点 ✗），编译器直接报错：

```
file.leno(12,5): error: [语义错误] [空catch吞异常] 空 catch 会静默吞掉异常（含 as 收窄失败等运行期错误
⇒ 功能会「没反应」）；请至少记一行日志，确定要忽略就写 catch e { return }
```

**怎么改**（Leno **没有** `pass`／空语句 ✗，所以要放一句真语句）：

```
file.leno(12,5): warning: [空catch吞异常] 空 catch 会静默吞掉异常（含 as 收窄失败等运行期错误
⇒ 功能会「没反应」）；至少记一行日志，确定要忽略请写 catch e { return }
```

- **为什么要它（真实事故）**：`ti.now()` 返回的是 **int 时间戳**（不是数组），
  `ti.now() as Array[int]` 在**运行期**抛异常，被 `catch { }` 吞掉 ⇒ 应用里
  「上次清理记录」「导出报告」**静默失效很久** ✗（直到排查"为什么没生成文件"才挖出来）。
- **有意忽略怎么写**：体里放一个**真语句**即可 ——
  `catch e { return }`（函数里）／`catch e { continue }`（循环里，实测语义正确 ✓）／
  `catch e { _dlog(...) }`（留一行痕 ✓）。老写法"只写注释"会被提示 ⇒
  迁移方式：**注释保留，补一行 `continue` / `return`** ✓
- 现状：全仓已收敛为 **0 条**（清理大师 5 处、缓存清理工具 3 处、文件搜索 4 处 ✓）

### ⛔ `as` 到容器/struct **不会当场失败**，而是"原样带过"（实测 ✓）+ 新增编译期提示

**实测（本机 VM）**：`int as Array[int]` / `int as Dict[...]` / `float as SomeStruct` / `string as Array[int]`
**都不会抛异常** ✗ —— `as` 把原值**原样带过**；失败**推迟**到"后面把它当容器用时"才暴露
（实测 `d.len()` 报「类型 object 上不存在方法 'len'」；索引则报「对象不支持索引」）。
⇒ 所以这类写法**不是立刻炸**，而是"**悄悄埋着**" ✗ —— 一旦外层套了 `catch`，
就成了**功能静默失效**（真实事故：`ti.now()` 是 int 时间戳，`ti.now() as Array[int]`
后面 `d[0]` 抛异常被 `catch { }` 吞掉 ⇒ 应用里「上次清理记录」「导出报告」静默失效很久 ✗）。

**编译期错误**（`error: [语义错误] [不可能收窄]`，2026-09-28 由警告升为错误 ✓）
—— **只有**下列情形才报（零误报口径 ✓）：

⚠ **源表达式是"裸标识符"时一律放过** ✗ —— 实测踩过假阳性：face 类型的**参数**标识符会被
`infer_expr_type` 兜底推成数值标量 ✗，于是 `s as Circle`（face→struct 的**合法向下收窄** ✓）
被误判成"不可能" ✗（探针实测 ✓）⇒ 只信字面量/调用/算术这类源 ✓
（代价：`int n = 5; n as Dict[...]` 会漏掉 ✗，等符号类型可靠后再放开 ✓）
**改法**：字面量那类改成经 `any` 中转（如让值来自返回 `any` 的函数 ✓），运行期语义不变 ✓

| 左边（源类型） | 右边（目标） | 提示 |
| --- | --- | --- |
| **确定的 `int` / `float`** | Array / Dict / struct / cstruct / face | ✅ 提示 |
| `any`、泛型参数、`TYPE_INFER` | 任意 | ❌ 放过（可能真的是容器 ✓） |
| `string` | 容器 | ❌ 放过（字符串与字符容器间可能有转换通道 ⇒ 不敢断言 ✓） |
| 标量 → 标量（如 `int as float`） | — | ❌ 放过（那是**转换** `OP_CAST_*`，不是类型检查 ✓） |
| `null` | 容器 | ❌ 放过（`null as Array[int]` 是合法用法 ✓） |

注：写成**带类型声明**的形式（`int d = ti.now() as Array[int]`）本来就会报"声明类型不匹配" ✓；
漏网的是 `var d = ...` 这种接法 —— 本提示补的正是这个洞 ✓
（全仓 690 个 .leno 扫描：仅 2 处命中，且都是 `assert/test_as_cast.leno` 里**有意为之的反例** ✓）。

### 📏 `ScrollView` 里"子孙会不会跟着滚"——三种内容根**实测**（2026-09-28 ✓）

**先说结论：滚动本身没问题** ✓；关键是内容根里有没有**每帧重排子孙**的东西。无头实测（滚 100px，
读各子控件的 `get_y()`）：

| 内容根写法 | 滚 100px 后子孙 y | 结论 |
| --- | --- | --- |
| **VBox（布局容器）** | 10 → **-90** | ✓ 跟着滚 |
| **Panel + `setLayout(VBox/AnchorBox)`**（数据看板用的就是这种 ✓） | 22 → **-78** | ✓ 跟着滚 |
| Panel 里直接塞**窗口绝对坐标**子孙（无布局） | 100 → **100** | ✗ 原地不动 |

机制（读 `sdl_scrollview.leno:296`）：滚动只做 `_content.set_pos(x - _scrollX, y - _scrollY)`，
而 `Renderer` **只有 clip 栈、没有渲染期平移** ⇒ "子孙动不动"完全取决于**内容根会不会每帧重排子孙** ✓。
布局容器（`_relayout`）与 `Panel`（`_syncLayoutChild`）都会 ✓；裸放绝对坐标子孙则不会 ✗。

⚠ 因此**不要**写"内容根必须是布局容器"这类检查 ✗ —— `Panel + setLayout` 是合法且常用的内容根 ✓，
那样的检查会误报 ✓。

### ✅ `RingProgress` 新增：多段弧 + 副行文字（做"已清 / 跳过"双色环 ✓）

```leno
ring.set_segments([0.23, 0.47], [绿, 橙])                        // 自 12 点顺时针；自带"长出"揭示动画 ✓
ring.set_sub_text_fn(func():string { return "占可清理 23%" })      // 环中心第二行（字号更小）✓
ring.clear_segments()                                            // 回单段（重新扫描时记得清 ✓）
```

- 创建时也可用 opts：`segments: [..]` / `segment_colors: [..]` / `sub_text_fn:` / `sub_font_size:` ✓
- 段数 > 颜色数 ⇒ **循环取色** ✓；设了多段弧则优先画多段（`mode:"value"` 的弧不再画 ✓）
- `anim_speed: 0` ⇒ 揭示动画**立即到 1**（无人值守截图/比对时用得上 ✓，避免帧间差异 ✗）
- 兼容性：没设多段弧、也没设副行 ⇒ 渲染路径与旧版**逐像素一致**（已用同源双构建比对验证 ✓）
| `其他测试/test_draw_order.leno` | 实测三个绘制层先后（见第一节 ✓） |
| `其他测试/test_dump_layout.leno` | `dumpLayout` 输出示例（含"有剩余空间但无人 grow"的场景 ✓） |

托盘 / 自绘菜单 / 关闭到托盘（**无头可跑 ✓**，退出码 0 = 全过）：

```
$env:SDL_VIDEODRIVER='dummy'
$env:CLOCK_SELFTEST='1'
build\leno.exe --no-cache leno_gui\应用\模拟时钟\clock.leno   # 17 项：左右键分发·子菜单下一级·菜单图标/勾选·显隐·关窗收托盘·置顶·退出 ✓
```

Trae签到 托盘 / 自绘菜单 / 开机自启（**无头可跑 ✓**，用临时注册表值名 ⇒ 不留痕 ✓）：

```
$env:SDL_VIDEODRIVER='dummy'
$env:TRAE_GUI_NO_NET='1'
$env:TRAE_TRAY_SELFTEST='1'
build\leno.exe --no-cache leno_gui\应用\Trae签到\trae_gui.leno   # 10 项 ✓ exit 0（含读回 Run 值核对命令行 ✓）
```

Trae签到 ⑥b 定时自动签到 / 设置对话框（**无头可跑 ✓**，都不联网、都不碰真实设置 ✓）：

```
$env:SDL_VIDEODRIVER='dummy'                 # 自检用**注入时钟**（[y,m,d,H,M,S]）⇒ 跨天/23 点可秒级断言 ✓
$env:TRAE_AUTO_SELFTEST='1'                  # 13 组：到点触发·1/15/30/60/120 递增封顶·触发抖动·23:00 放弃·跨天重置·成功即停·
build\leno.exe --no-cache leno_gui\应用\Trae签到\trae_gui.leno   #        9095 即停·启动补签·空结果跳过·关掉重试/开关 ✓
$env:TRAE_SETTINGS_SELFTEST='1'              # 设置读写/越界兜底/临时文件清理（用 settings_selftest.json ✓）
build\leno.exe --no-cache leno_gui\应用\Trae签到\trae_gui.leno
```

> 定时任务的**可测性**要点：把"现在几点"做成**可注入的时钟**（`autoSetClock(func():Array[int])`），
> 并把重试排期限制在**同一天内**（用"当天秒数"不用绝对时间戳）⇒ 否则"跨天 / 23:00 放弃"这类
> 分支只能靠等真实时间，自检写不动 ✗（本次就是这么做的 ✓）
> ⚠ 另一条：自检里**不能 `runModal`**（dummy 下没有事件 ⇒ 模态循环会永久挂起 ✗，见 §一）——
> 只建窗 + 断言控件初值即可 ✓
> ⚠ 后来又加了**触发抖动**（0~90s，避开"全体脚本在 00:05 同一秒发射"）⇒ 自检必须先把它钉死
> （`autoSetJitter(0)`），否则同一条断言今天过、明天不过 ✗（随机量进生产逻辑，就必须留一个"关掉随机"的口子 ✓）

⚠ 「托盘图标被点 ⇒ 拦截 ⇒ 弹菜单」这条链**要真机跑**（dummy 下没有 Win32 消息泵 ✗，见 §六）。
真机快速验证：`set LENO_SDL_FRAMES=40` 后直接跑模拟时钟 —— 右键托盘图标应在光标处弹出菜单、
左键应直接切显隐、秒针在菜单开着时照走 ✓
（想连子菜单一起看：把菜单画进主窗再截图 —— `win.setOverlay` 里调 `pm._render(r)` +
`r.readPixelsAt(...)` + `SDL3.saveImagePNG` ✓，本次就是这么做布局核对的 ✓
⚠ 但**图标**在这种探针里可能不出现：图片 DLL / 相对路径是按**脚本所在目录**找的 ✗
⇒ 看图标请直接跑应用本身 ✓）

---

## 九、标题栏：自定义动作按钮（`actions`，2026-10-07 新增 ✓）

原标题栏只有"最小化 / 最大化 / 关闭"三个固定位；现在**能把任意图标按钮塞进同一排**（系统按钮内侧）✓
应用侧只要三行：

```leno
TitleBar tb = SDL3.createTitleBar({title: "我的应用", height: 32, actions: [
    {id: "settings", icon: "gear", tip: "设置"},
    {id: "theme",    icon: "sun",  tip: "换主题"}
]})
tb.on_action(func(string id) { if id == "settings" { openSettings() } })
```

> 示例 + **无头自检**：`examples/UI组件/标题栏/test_titlebar_actions.leno`（建窗 + 断言 ⇒ 不需要显示器 ✓）；
> 真实用法：`leno_gui\应用\音乐下载器\musicdl_gui.leno`（标题栏右侧齿轮 ⇒ 打开设置窗口 ✓）

### 位置与顺序（不用自己算坐标 ✓）
- 一律排在系统按钮的**内侧**：`btn_align:"right"`（默认）⇒ 紧挨最小化左边；`"left"` ⇒ 紧挨关闭右边 ✓
- **声明顺序 = 从系统按钮往内侧排**（第一个声明的离系统按钮最近 ✓）
- 每个动作占一格 `btn_width`（与系统按钮同宽 ⇒ 一排看着齐）；标题与 `center_widget` 的留白按
  "系统按钮 + 动作按钮"**整条带子**算 ⇒ **没传 `actions` 时与旧版逐像素一致**（老应用零影响 ✓）

### 图标两种给法
1. **内置矢量名**（**不需要任何图片资源** ✓）：`gear`/`settings`(齿轮)、`menu`、`search`、`star`、
   `info`/`help`、`refresh`、`sun`、`home`、`dot`
   —— `SDL3.is_builtin_icon(name)` 可查；**不在表里的一律当图片路径** ✓
2. 图片路径：`images/x.png` ⇒ 按 id 缓存纹理（换路径自动重载；加载失败画一个小点，**不崩** ✓）

- 键名用 `icon`（与本控件 `icon_minimize` / `icon_close` 一族一致 ✓）；`image` 也认（菜单那套键名 ✓）

### 标题前的应用图标（`title_icon`，2026-10-07 新增 ✓）

只有文字时标题前面空一块、观感差 ⇒ 给一张图，与文字作为一个**整体**居中/左对齐 ✓

```leno
SDL3.createTitleBar({title: "我的应用",
                     title_icon: dirs.join(dirs.res_dir(), "images/index.png"),   // 也认 title_image ✓
                     title_icon_size: 18,   // 缺省 ⇒ 跟 icon_size 同档（16）
                     title_icon_gap: 6})    // 图标与文字的间隙
```
- 路径要**绝对**（用 `dirs.res_dir()` 拼 ✓）——控件把字符串**原样**交给 SDL_image；PNG 直接可用 ✓
- 运行时换：`tb.set_title_icon("…")`（传 `""` ⇒ 去掉图标 ✓）
- 加载不出来 ⇒ 当"**没有图标**"处理、**不留空位** ✓；**没传** `title_icon` 的旧应用：标题位置与旧版
  **逐像素一致** ✓（居中时是"图标+文字"整块居中，不会只把文字推到一边 ✓）

### 悬停气泡
- **复用框架的 tooltip**（`sdl_tooltip`，与 Button/CheckBox 同一套延迟与配色）✓ ⇒ `tip` 直接写中文即可
- 动作区与系统按钮**互斥**：停在动作按钮上不会亮"最小化/最大化"的悬停底色 ✓

### 点击
- `tb.on_action(func(string id){ … })`，参数就是 `actions` 里的 `id` ✓
- ⚠ 动作按钮**不会**误触发"拖窗口 / 双击最大化"（`process()` 里先判动作区，命中就只回调 ✓）

### 运行时增删改（不用重建标题栏）
| 调用 | 作用 |
| --- | --- |
| `add_action({id, icon, tip, enabled})` | 追加；**id 已存在 ⇒ 就地替换** ✓ |
| `remove_action(id)` / `clear_actions()` | 移除（连带释放图片纹理 ✓）|
| `set_action_icon(id, name)` / `set_action_tip(id, t)` / `set_action_enabled(id, v)` | 换图标 / 提示 / 禁用（禁用 ⇒ 变灰且不可点 ✓）|
| `get_actions()` / `action_count()` | 只读快照（自检就靠它 ✓）|

### 三个坑（都实测过 ✓）
- ⛔ **变量名不能叫 `as`**：`Array[TitleBarAction] as = tb.get_actions()` 会报"期望变量名"
  （`as` 是类型转换关键字）⇒ 改叫 `acts` ✓
- ⛔ 无头（`SDL_VIDEODRIVER=dummy`）下**收不到事件**：点击/模态都别指望 ⇒ 自检只断言状态，
  气泡与点击必须开窗口手点 ✓（见 §一）
- ⛔ `actions` 是 `Array[Dict]`，**元素里的键写错不会报错**（`d.get(..., 默认值)` ⇒ 只是没效果 ✗）：
  顶层键（`actions` 本身）已登记进 `_opts_keys()`（写错会提示 ✓），元素键请照上面的名字写 ✓

## 附录 B：`align` 在 HBox / VBox 里的真实语义（2026-10-07 实测 ✓）

`add(child, {align: N})` 的 `align` 管的是**交叉轴**（HBox ⇒ 垂直；VBox ⇒ 水平），取值语义（见 `sdl_layout.relayout` ✓）：

| align | 含义 | 用在哪 |
| --- | --- | --- |
| `0` | 靠起始边（左 / 上 ✓） | 文本靠左 |
| `1` | **居中** | 按钮、图标、滑块这类"有自己个子"的控件 ✓ |
| `2` | 靠结束边（右 / 下 ✓） | 右对齐的时间、序号 |
| `3` | **撑满交叉轴** ✗ | 分隔线、整行条（要的就是铺满） |

- ⛔ 最常踩的：给**按钮/滑块**写 `align: 3` ⇒ 它会被抻成整行高（HBox 里）或整列宽（VBox 里）✗
  按钮背景跟着变形，看着就是"控件被拉伸了" ✗ —— 要**居中就用 `1`** ✓（本次实测栽过 ✓）
- ⚠ 反过来：`h`/`w` 是为 `align: 1` 准备的；写了 `align: 3` 时那个 `h` 不生效 ✓
- ⚠ 容器的交叉轴尺寸由 `innerW/innerH` 决定，**子控件的自然尺寸在 `add` 时记下**、之后每帧
  `relayout` 都用它覆盖 `set_size` ✗ ⇒ 运行时想改**某个子控件的高度**，只能 `remove` + 重新 `add`
  （并给 `basis` ✓）—— 光 `set_size` 是没用的 ✗

### ⛔ 滚轮方向：`wheelY()` 正值 = **远离用户（往上滚）**；框架语义 = 往上滚看**更早**的内容 ✗（2026-10-08 实测）

自绘滚动（画布 + `dy`）时最容易凭直觉写反 ✗（本仓实测：用户反馈"滚轮方向正好反过来" ✓）。

```leno
// 语义（sdl_event.leno:129 原话）：wheelY() 正值 = 向右/离开用户，负值 = 向左/朝向用户
//   ⇒ 正值 = 把滚轮往**上**推（远离自己 ✓）
// 框架各处一致：往**上**滚 ⇒ 看**更早**的内容 ✓
//   sdl_table:1439      scrollY = scrollY - ev.wheelY() * rowH
//   sdl_listbox:329 / sdl_edit:856 / sdl_treeview:692 同款 ✓

// ✗ 凭直觉写（往上滚却往后跑）
offset = offset - dy * k
// ✅ 与框架一致：`offset` 越大 = 越往前（越早）
offset = offset + dy * k
```
- 自检手法（本仓惯例 ✓）：无头**注入不了滚轮** ✗ ⇒ 直接**临时强制 `offset`** 截两张图，
  比"当前项的位置"往哪边走 ✓（本次就是这么证实方向的 ✓）
- ⚠ 顺带一条：`Canvas.on_wheel` 的坐标必须用 `ev.wheelMouseX/Y()` 取（框架已处理 ✓ 见 `sdl_canvas:201`）；
  自己写控件时别用 `mouseX()/mouseY()`（滚轮事件结构不同 ⇒ 命中判断恒 false ✗）

### ⛔ 懒加载缓存的键：**别用"逻辑名"，要用"最终资源路径"**；而且资源可能**晚到** ✗（2026-10-08 实测）

"跟着当前曲目换一份附属文件"（歌词 / 封面 / 字幕）这类懒加载，缓存键的两个坑：

```leno
// ✗ 坑 ①：拿"哪一首"当键 ⇒ 试听态 `current()` 是 ""（试听不在列表里 ✗）
//          ⇒ 附属文件路径算成 "" ⇒ **永远读不到**（界面永远显示"没有" ✗）
// ✗ 坑 ②：键变了才重读 ⇒ 附属文件**比主文件晚到**时（音频先落盘、.lrc 后落盘 ✓）
//          键根本没变 ⇒ 一直停在"空" ✗（不换歌/不重启就永远不补 ✓）

// ✅ 正解：键 = 真正要读的**文件路径**；且"上次读出来是空的 + 文件现在已经在了"⇒ 再读一次
string p = lrc_path()                       // 路径本身就把"试听 / 本地文件 / 换歌"全覆盖了 ✓
bool need = (p != lrcFor) or (not lrcTried)
if not need and lrc.texts.len() == 0 and p != "" {
    try { if files.exists(p) { need = true } } catch e { }   // 一出现就补读 ✓
}
```
- 试听/临时文件这类"**不在列表里**"的播放态，要**单独算**一次资源路径 ✓
  （本仓样例：试听的临时文件旁边没有 .lrc ✓，但**同一首在输出目录里**有 ✓ ⇒ 按 previewName 去输出目录取 ✓）
- 代价：空缓存期间每帧多一次 `files.exists`（只有"画布开着且没读到"时才走 ✓ 可忽略 ✓）
- 判"是不是同一首"也别用**字符串全等** ✗：展示名（`歌名 - 歌手`）和落盘名（带扩展名、可能被清过非法字符）
  天生不同 ⇒ 用 `strip_ext(basename(path))` 比 ✓（本仓 `player_bar.on_downloaded` 就是个例子 ✓）

### ⛔ 截断标签（`truncate`）放进容器：宽度也会被"冻结在 `add` 那一刻" ✗（2026-10-07 实测）

`Label` 的截断上限 = **`min(max_w, 盒子宽度)`**（`sdl_label.leno:242` ✓）；而盒子宽度来自
`_natW[i]` —— **`add()` 那一刻的自然宽度**，`align: 0/1/2` 时每帧照搬（`sdl_layout.leno:404` ✓）。

⇒ 于是踩坑连锁（本仓实测 ✓）：
1. 建窗时还没歌 ⇒ 文本短 ⇒ `_natW` ≈ 45px 被记下 ✗
2. 之后 `set_text("很长的歌名")` ⇒ 盒子仍是 45 ✗（`align: 0` 不会重算）
3. 截断上限 = `min(200, 45)` = **45** ⇒ 永远只剩「试听…」，右边空一大片 ✗✗

```leno
// ✗ 这样写：max_w 200 也没用，盒子宽才是硬边界
info.add(songLbl, {grow: 0, shrink: 0, align: 0})

// ✅ 正解：撑满交叉轴（拿下 innerW ✓）+ max_w **与块宽同源**（一个常量两处用 ✓）
const float INFO_W = 200.0
songLbl = SDL3.createLabel({text: t, font_size: 14, truncate: 1, max_w: INFO_W})
info.add(songLbl, {grow: 0, shrink: 0, align: 3})     // ← 关键
row.add(info, {grow: 0, shrink: 0, basis: INFO_W, align: 1})
```
- 想要"随块宽自动伸缩 + 超长才省略" ⇒ **`align: 3` + 两处同一个数** ✓（`player_bar.leno` 的 `INFO_W` 就是样板 ✓）
- ⚠ 排障时别量错时机：`set_text` 之后**同一拍**读 `lbl.get_w()` 拿到的是"文本自然宽"（布局还没跑 ✗）
  ⇒ 要**在渲染路径里量**（自绘回调里 ✓，那时 `relayout` 已经给过了 ✓）。本次就先后悔了一次 ✗

### ⛔ 自绘浮层会"**穿透**"到下层控件 ⇒ 必须登记弹层捕获区（2026-10-07 ✓）

容器派发事件是**挨个发给所有子控件**（`sdl_layout` 里 `for _c to c { c.process(ev) }` ✓ —— 没有命中测试、
也没有"顶层优先" ✗）⇒ 每个控件自己判"鼠标在我矩形内"就响应 ⇒ 浮层底下的 table/按钮 **照样跟着响应** ✗
（实测：歌单浮层里挪鼠标，底下表格的悬停/提示、甚至右键菜单都会冒出来 ✗）。

框架自带机制：**弹层捕获区** —— 所有标准控件在 `process` 开头都有
`if SDL3.inPopupCapture(mx,my) { return }`（table/button/slider/label/listbox… ✓），库内 Menu/ComboBox 就靠它 ✓。

```leno
// ✅ 浮层自带一块**透明画布**当"登记员"（和浮层同矩形、同挂同摘 ✓）
capSink = SDL3.createCanvas({w: W, h: H, pad: 0, border: false, border_width: 0})
capSink.set_bg_color(#00000000)                 // 画布默认有不透明底 ✗（见 ④.5）
capSink.on_draw(func(Renderer r, float ox, float oy, float cw, float ch) {
    SDL3.setPopupCapture(lb.x, lb.y, lb.w, lb.h)     // ★ 每帧渲染期登记一次
})
```
- ⚠ **必须"每帧登记"**：框架每帧渲染前会 `clearPopupCapture()`（`sdl_window` ✓）⇒ 只登记一次会在下一帧失效 ✗
  （事件处理在 render 之前 ⇒ 用的是**上一帧**登记的块，跨帧持久 ✓ 不受控件处理顺序影响 ✓）
- ⚠ 登记后**连浮层自己的控件也会被挡** ✗（ListBox 也带那句守卫 ✓）⇒ 浮层要在事件回调里
  **直投**给它：`if 在浮层矩形内 { lb.process(ev) }` ✓（滚轮/悬停靠它自己维护 ✓，库内同款范式 ✓）
- ✅ 捕获区**按窗口隔离**（2026-10-07 修 ✓）：判定只看"当前上下文窗口"那一块
  ⇒ A 窗口开浮层不会把 B 窗口同屏位置也挡住 ✓；应用回调里直投时"无上下文"⇒ 不拦 ✓
- 现成样板：`leno_gui\应用\音乐下载器\player_bar.leno`（歌单浮层 ✓）+ `assert\test_popup_capture.leno`（隔离语义 ✓）

### 想做"浮层 / 弹出面板"（下拉、歌单、气泡）⇒ 用 `AnchorBox` ✓

**渲染顺序就是层级**（框架没有 z 轴 API ✗）⇒ 浮层必须画在它要盖住的那些控件**之后** ✓。

```leno
    var box = SDL3.createAnchorBox({w: 0, h: 主行高})        // 外盒：只占主行那么高 ✓
    box.add(row,   {left: 0, right: 0, top: 0, h: 主行高})    // 常规内容
    // 浮层：只给 bottom ⇒ “底边贴着父盒底边**往上**量” ⇒ 它就是向上长 ✓（见 relayout 的 bottom 分支）
    box.add(popup, {right: 10, bottom: 主行高 + 16.0, w: 360, h: 160})
```
- `left/right` 同给 ⇒ 自动拉伸宽度 ✓；只给 `bottom` ⇒ **底边锚定**（`py = by + bh - b - ch` ✓）
- `AnchorBox` **不裁剪**越界子项 ⇒ 摆在盒子外面没问题 ✓（浮层都是这么摆的 ✓）
- ⚠ 浮层外盒要留出**缺口**（如 16px）：浮层下方的兄弟控件（忙指示细线、状态栏）是在它**之后**
  才渲染的 ✗ ⇒ 贴太近会被它们压掉一角 ✓
- ⚠ **收起时要真的 `remove`**：容器的 `process` **不看 `visible`** ✗ ⇒ 留个看不见的浮层会把
  它那一带的点击全吃掉 ✗✗（"点外面即收"时尤其明显 ✓）
- ⚠ 增删**别在事件回调里做**（那是在容器遍历子控件的过程中改数组 ✗）⇒ 回调只置意图，
  真正的 `add/remove` 放到定时器的 tick 里做 ✓
- "失去焦点即收"：把窗口事件转给浮层（宿主 `win.run(onEvent, …)` 里转发 ✓），命中测试用
  `AnchorBox` 的公开字段 `x/y/w/h` 自己算矩形（它**没有** `get_x/get_y` ✗）；ESC 的处理要排在
  "ESC 关窗口"**之前** ✗ 否则按 ESC 会直接退程序 ✓

## 附录 C：三个实测坑（点击 / 断言 / 自测注入；2026-10-07 ✓）

### ① ⛔ `ListBox`（单选）**点"已选中那一行"不回调** ✗

`sdl_listbox.leno` 里是 `if not is_selected(idx) { … onChange(sel) }` ✓ ⇒ 单选模式下，
点**已经在选中的行** ⇒ 既不改变选中、也不回调 ✗。
这坑很隐蔽：列表初值若"预选中当前项"（比如歌单把正在播的曲子设为选中 ✓），
用户点那一行就会**毫无反应** ✗（本次实测就是这么踩的 ✓）。

```leno
    // 别只靠 on_change 判"点了哪一行"；要覆盖"点当前行"，就自己按纵坐标算行号 ✓
    //   ★ 2026-10-08：ListBox.index_at 已由 **pri 改成公开** ✓ ⇒ 直接用，别再抄公式 ✗
    //     （旧公式 `idx = (my - y + scroll) / itemH`、无内边距 ✓ —— 先前只能自己镜像一份 ✓）
    int r = lb.index_at(mx, my)
    if r >= 0 and r < n { play(r) }
```
（或者干脆别预选中 ✗ —— 但那样就看不到"哪首正在播"的高亮 ✓ 取舍看场景 ✓）

### ①-b ★ `ListBox` 也有右击上下文菜单了（2026-10-08 补齐 ✓）

先前**只有 Table / TreeView** 有 ✗ —— 想在播放列表上挂"删除本地文件 / 在资源管理器里定位"就没辙 ✓。
现在 `ListBox` 的 API 与 Table **同名同义** ✓（就是照 Table 那套搬的 ✓，菜单本体仍是共用的 `sdl_menu` ✓，
绘制交给窗口第二遍的 `menu.renderContextMenus` ✓）：

```leno
    lb.addContextItem({label: "删除本地文件…", id: "del"})
    lb.addContextItem({label: "在资源管理器里定位", id: "reveal"})
    lb.on_context_will_open(func(int row) { })                 // 弹出前按行重建（不同行不同项 ✓）
    lb.on_context_select(func(int row, string id) { })         // row = 右击命中行（空白处是 -1 ✓）
    lb.set_context_menu_enabled(false)                         // 想关就关 ✓
    lb.get_context_menu_row()                                  // 事后取"刚才右击哪一行" ✓
```
- ⚠ 事件顺序上**关键一条**：菜单展开时，`process` 里那段"先交菜单"必须排在 `inPopupCapture` 守卫**之前** ✗
  （菜单自己也算一个弹层 ⇒ 先判捕获会把菜单自己的事件全吃掉 ⇒ 菜单能弹、却**点不动** ✓ 已按 Table 的顺序排 ✓）
- 自测：框架新增 **`SDL3.testRightClickAt(wid, x, y)`** ✓（右键 = 按钮号 3 ✓）
  —— 先前只有左键注入 ✗ ⇒ "菜单到底弹没弹出来"只能真机手点 ✗
- ⚠ **应用侧的老坑（本仓实测 ✓）**：自己写"点一行就播"这类逻辑时**必须判按钮** ✗
  （本仓播放条先前是"只要鼠标**按下**且落在歌单矩形里 ⇒ 播那一行"✓ 没判按钮 ✗
   ⇒ 右击歌单直接把歌换掉了 ✗ 用户原话："右击没出现菜单，是播放音乐" ✓）
  ⇒ 加一句 `ev.mouseButton() != 3` 即可 ✓（右击的语义是"要菜单"，交给 `lb.process(ev)` 就行 ✓）

### ①-d ⛔ 定时器**不要补跑积压的周期**：电脑睡眠唤醒后会"假死" ✗（2026-10-08 实测）

**症状**（用户原话）："电脑睡眠后唤醒，应用就崩溃或者退出，很奇怪" ✓

**根因**：`sdl_timer.leno` 的 `tick()` 原先写的是"补跑所有错过的周期"：

```leno
// ✗ 旧写法：睡 3 小时醒来，33ms 的定时器要在这里补跑 327272 次（探针实测 ✓）
while now >= next { cb(); next += ms }
```
- 睡眠/长卡顿后 `SDL_GetTicks()` 会**一次跳几小时** ✗ ⇒ 补跑次数 = 跨过的周期数 ✓
- 实测（探针：把 `next` 往后拨 3 小时再 `tick()`）：**33ms ⇒ 327272 次**、**120ms ⇒ 90000 次** ✗
- 本仓两个应用都挂 33ms（唱片动画）/ 120ms（进度轮询）定时器 ⇒ **全都中招** ✓
- 界面表现就是"假死几十秒" ⇒ 用户会当成"崩溃/退出" ✓（真崩成 `0xc0000005` 是另一回事，见下 ✓）

```leno
// ✅ 正解：最多跑一次，把 next 对齐到"未来"（丢掉来不及的拍 ✓）
if now - next >= ms { next = now + ms } else { next = next + ms }
cb()
```
- 对"该补的事"没有影响 ✓：自动签到那种 `autoTick()` 是拿**真实时钟**判"过没过点" ✓
  ⇒ 醒来这一拍照样补签 ✓（正是"启动时过了点就补一次"的语义 ✓）
- 自检手法（框架没有断言测试目录 ⇒ 用**一次性探针** ✓，见 ①-c 那条教训 ✓）：
  `Timer t = new Timer(); t.set(33, cb); t.next = t.next - 3*3600*1000; t.tick();` 数 `cb` 调用次数 ✓

⚠ 另有一处**待核实**（同一场景的另一条线索 ✓）：应用建完托盘后立刻 `destroySurface(icon)` ✓
  （`trae_tray.leno:118` 的注释写"创建时已转成平台图标 ⇒ 可释放 ✓"）—— 若 SDL 其实**不复制**该
  surface ✗，那这就是野指针 ✓，而**睡眠唤醒/Explorer 重启后系统会重新读取托盘图标** ⇒
  崩在 `SDL3.dll` 里 ✓（与 2026-10-08 那次 `0xc0000005 in SDL3.dll` 的现象吻合 ✓）⇒ 待查 SDL 文档 ✓

### ①-c ★ 别为"绕开类型收窄"把配置结构拍扁（2026-10-08 实测更正）

曾流传"`jsons.decode` 取回来的嵌套结构难收窄 ⇒ 干脆把配置拍成 `key → 裸串`" ✗。
**实测这几条都正常，不必绕** ✓（详见 `docs/module_jsons.md` 注意事项 8 与
`docs/类型收窄与绑定语法改进.md`）：

```leno
    jsons.write_file(p, {version: 1, arr: ["x", "y"]})   // ✓ 字面量字段里直接放数组
    Dict[string, any] d = {}
    d["arr"] = ["x", "y"]                                // ✓ 内存与落盘都正确，不会被丢
    var got = jsons.get_obj(d, "arr")
    if got is Array[string] => lst { … }                 // ✓ JSON 解出来的数组也过得了这个收窄
```
- 需要"几个字符串"的配置项 ⇒ **直接存 `Array[string]`** ✓（本仓例：音乐下载器 `dl_settings.sources` ✓）
- ⚠ 真正要在意的是另外两条：① `write_file` 会把参数**再编码一次** ⇒ 写"已编码好的文本"用 `write_text` ✓
  ② 空数组字面量 `var a = []` 的元素类型是 `any` ⇒ 要**显式标注** `Array[string] a = []` ✓
- ⚠ 教训（本仓实例）：我一度把"自己那条 `if 标志` 守卫为假"误判成"数组被语言丢了" ✗ 还写进注释 ✗
  ⇒ **下结论前先写最小探针**（`%TEMP%` 下临时脚本，跑完删掉 ✓）别拿推测当实测 ✓

### ② ⛔ 断言别读 `lbl.text`：它**读不到 `set_text` 之后的新值** ✗

本次实测：`stateLbl.set_text(...)` 之后，隔几帧再读 `stateLbl.text` 仍是**旧值** ✗
⇒ 拿它做断言会得到**假阴性** ✓（我因此白查了一轮 ✓）。
要断言就读自己的状态量（如 `bar.cur_index()` ✓），或让控件暴露一个 getter ✓。

### ③ ⛔ HBox 里 `grow > 0` **连交叉轴一起撑满** ✗（进度条会变成圆饼 ✓ 实测）

`sdl_layout` 的 HBox 排版里：`if align == 3 { ch = innerH } else if g > 0.0 { ch = innerH }` ✗
⇒ 想让某个子项**横向吃满剩余宽度**而写了 `grow: 1` ✓ ⇒ 它的**高度也被拉成整行高** ✗。

- 症状：一条 `ProgressBar/h: 8` 在 52px 高的行里被拉成 8..52 ✗，若宽度又被其他子项挤窄 ⇒
  `drawBar` 用 `h/2` 当圆角 ⇒ 看着就是**一个大圆饼** ✗（本次实测 ✓）
- ✅ 解法一：那个子项改成**画布自绘** ⇒ 画布照旧 `grow` 吃宽度 ✓，里头的条高自己定（居中画 8px ✓）
- ✅ 解法二：用 `minH/maxH` 约束（`relayout` 认这两个键 ✓）；⚠ 但 `g>0` 分支会把它**顶到行首** ✗
  （`cy = y + pt` ✓）⇒ 想垂直居中就别用 grow ✓

### ④.5 ⛔ 自绘控件（`Canvas`）默认有**不透明底** ⇒ 看着像"自绘的东西带白底" ✗

`Canvas` 默认 `bgColor` 是深色，但 `applyTheme` 会把它设成 **主题的"输入框底色"**
（`sdl_canvas.leno:107 bgColor = th.bgInput` ✓）⇒ **浅色主题下就是白白一块** ✗。
你只想画一条线/一个圆，却得到一个方底 ✓（本仓实测：进度条上就是这么冒出来的 ✓）。

```leno
    Canvas c = SDL3.createCanvas({w: 400, h: 52, pad: 0, border: false, border_width: 0})
    c.set_bg_color(#00000000)        // ★ 必须**用 setter**：写进 opts 会被 applyTheme 覆盖 ✗
```
- 同一个坑在别的控件上也一样（`Button` 悬停色那次 ✓）⇒ **凡"主题也会管的颜色"，创建后用 setter 压 ✓**
- ⚠ `border: false` 只是不画边框；布局仍按 `border_width`(默认 1) **内缩** ⇒ 要精确尺寸就一并给 `border_width: 0` ✓
- ⚠ 转盘/图标这类"我自己画形状"的画布**都要设透明** ✓：不然圆外的角是一块实心底 ✗
  （颜色恰好和页面撞上时看不出来 ✗ —— 换主题/换底色就露馅 ✓）
- 验证方式（本仓惯例 ✓）：截图后**按列取色** ✓ —— 期望"只有页面色 + 我画的那点颜色"，多出别的色就是控件底 ✓

### ④ ⛔ `Button` 的"自然宽度"**把图标也算进去** ⇒ 容器凭空变宽 ✗

实测：三个 `w: 34/46/34` 的纯图标按钮放在一个 HBox 里 ⇒ 那个 HBox **自报 ≈248px**（不是 122 ✗）
⇒ 它后面就多出 ~130px 空档 ✗（我怎么算都对不上 ✓ 最后靠打印每个子项的 x 才揪出来 ✓）。

- ✅ 给容器（或子项）**显式 `basis`** ✓ —— 布局对 `basis` 是认的 ✓
  （同一个界面里，转盘 `basis: 44`、信息列 `basis: 200` 都精确对上 ✓ 只有没给 basis 的两个容器飘 ✓）
- 排障套路 ✓：**别靠算** ✗ —— 把每个子项的 `x/w` 打出来（本仓的播放条就是这么定位的 ✓）

### ⑤ ⚠ 自测注入点击：**先注入一次移动**，否则按钮偶发不生效 ✗

`SDL3.testClickAt(wid, x, y)` 只发 down/up ✓ ⇒ 若按钮的点击判定依赖 `hover`，
"同一坐标有时开门、有时不开" ✗（实测 ✓）。**先 `testMoveAt(wid, x, y)` 再点击** ✓
（真人点之前总会有移动 ✓）。坐标是**客户区**坐标 ✓（见 sdl_window:271 那段教训 ✓）。

### ⑥ ⛔ 图标按钮"没有 hover"：`background: false` 会连 hover 一起跳过 ✗

- `background: false`（"只要图标"）会把 **背景 + hover + 按压** 整块跳过 ✗
  （`sdl_button.leno` 里 `if showBackground { … }` ✓）⇒ 表现就是"鼠标移上去毫无反应" ✗
- 那改用框架给纯图标按钮的自动高亮行不行 ✗ —— 它**写死是白色** `#FFFFFF23`
  （`if iconOnly and (hover or pressed)` ✓）⇒ 落在**浅色**界面上白压白 = 看不见 ✗✗（本次实测 ✓）
  （`文件管理器` 用它有效果，是因为那边页面底色不同 ✓）
- ✅ 正解（照 `leno_gui\应用\电脑清理大师\cleaner_master.leno:221-240` ✓）：
  `background: true` + **与页面同色**的 `bg_color`（平时＝看不出有底 ✓）
  + 显式 `hover_color` / `pressed_color`（比页面略深 ✓）
  + `border: false, shadow: false`（浅色页面上别冒白边/阴影 ✗）
```leno
    Button b = SDL3.createButton({w: 32, h: 28, text: "", background: true,
        bg_color: #F0F0F5FF, hover_color: #E2E5EBFF, pressed_color: #D3D7DEFF,   // 页面色 → 悬停略深 ✓
        border: false, shadow: false, image_path: "…/play.png"})
```
- ⚠ 想靠"把图标变灰/变亮"来区分状态时**别用 `image_tint`** ✗：它是**相乘**着色 ⇒
  只会让深色图标更黑 ✓（浅色图标才适合用它 ✓）⇒ 改用**底色**区分 ✓
- ⚠ 顺带：`background` 是**开关**（true/false），**不是颜色** ✗（给颜色要写 `bg_color` ✓）

## 附录 A：生成**带透明**的 PNG（做图标用；2026-10-07 实测 ✓）

要"画一次、控件复用"的小图标（播放/暂停/上一曲…），离线生成 PNG 比每帧自绘省得多 ✓
（`Button` 本来就有 `image_path` / `image_hover` / `image_tint` ✓ 生成物交给它就行 ✓）。

- ⛔ **最大的坑：别把图标画在窗口后缓冲上再 `readPixelsAt`** —— 后缓冲**没有 alpha 通道** ✗
  ⇒ 你 `clearColor(#00000000)` 的"透明底"存出来是**实心黑** ✗（图标带黑底，深色主题上看着像个黑方块 ✗）
- ✅ 正解：渲染到一张 **ARGB8888 的 target 纹理**上（目标纹理带 alpha ✓），再读它：
```leno
    var win = SDL3.createWindow({title: "gen_icons", w: sz, h: sz})   // 无头跑记得 SDL_VIDEODRIVER=dummy
    Renderer r = SDL3.createRenderer(win.handle)
    Ptr[u8] tex = r.createTexture(SDL3.PIXELFORMAT_ARGB8888, SDL3.TextureAccess.TARGET, sz, sz)
    r.setRenderTarget(tex)              // ★ 关键：切到带 alpha 的目标
    r.clearColor(#00000000)             // 透明底（这次留得住 ✓）
    ...画图标（纯白 ✓ 颜色交给 image_tint）...
    // ⚠ 这里**不要** present()：present 是给窗口后缓冲的，目标纹理上没意义 ✗
    var s = r.readPixelsAt(0, 0, sz, sz)        // 读的是**当前渲染目标** ✓
    SDL3.saveImagePNG(s, "images\\play.png")    // 色彩类型 6 = RGBA ✓
    r.setRenderTarget(null); SDL3.destroyTexture(tex)
```
- 怎么**验证真的透明**：看 PNG 第 25 字节（色彩类型）—— `6` = RGBA ✓ / `2` = RGB（没 alpha ✗）
- 尺寸按**显示尺寸 1:1** 生成（如按钮上显示 24px 就出 24px ✓）：缩放会插值 ⇒ 白图标边上发灰 ✗
- 现成例子：`tools\gen_player_icons.leno`（7 个播放器图标 ✓）；用法 `build\leno.exe tools\gen_player_icons.leno [输出目录] [边长]`

## 十、Table 单元格：进度条列 与 自绘回调（2026-10-07 新增 ✓）

单元格**不再只有文本** —— 两种能力都长在原有的"可见行循环"里 ⇒ **不牺牲虚拟化**（只处理看得见的行 ✓）。

### 进度条列（`cell_types`）
```leno
var tbl = win.addTable({
    headers: ["歌曲", "进度", "状态"],
    rows: [["夜曲", "8", "下载中"], ["晴天", "100", "已完成"]],
    cell_types: ["", "progress", ""]      // 下标=列；"" / "text" = 文本（默认）
})
tbl.set_cell(0, 1, "42")                  // ★ 写**数值**：值就存在该列的单元格文本里 ✓
```
- 运行时改列类型：`tbl.set_col_cell_type(1, "progress")` / 读回来 `tbl.get_col_cell_type(1)` ✓
  （切回 `"text"` 就会看到**裸数值**——因为显示与取值用的是同一格文本 ✓）
- **值怎么写**（规则在 `sdl_progress.parseProgress`）：

  | 写法 | 结果 |
  | --- | --- |
  | `"0.37"` / `"37"` / `"37%"` / `"  42 "` | 37% / 37% / 37% / 42% ✓ |
  | `"1"` 与 `"100"` | 都是 100%（**≤1 当比例、>1 当百分数** ✓）|
  | `"150"` / `"-1"` | 夹到 100% / 0% ✓ |
  | `"等待中"` / `"abc"` / `""` | 0%（画成空条，**不抛错、不打断整帧** ✓）|

- ⛔ 非数字写进进度列**不会崩**，但画成 0% 空条（分不清"没开始"和"写错了" ⇒ 状态请另起一列 ✓）
- 观感与 `ProgressBar` **同源**（轨道/填充/文字色都出 `sdl_progress.drawBar` / `barFillColor` ✓）
  ⇒ 表格里那条与弹窗底下那条**不会一大一小两种样式**（复选框当年就是栽在"两套观感" ✗）
- ⛔ 进度列**别与 `image_col` 同列**：图标和条会挤在一格里（本轮不做"图标+条"组合 ✗）
- 与 `check_col` / `image_col` 一样是**按列**的 ⇒ 列重排（拖表头）时类型跟着列走 ✓
  而**值**在行数据里 ⇒ 排序 / 行重排**自动跟着走**（不必维护并行数组 ✓）

### 自绘回调（`on_cell_draw`）
```leno
tbl.on_cell_draw(func(Renderer r, int row, int col, float x, float y, float w, float h) {
    if col != 2 { return }
    r.setColor(#3CC850FF)
    r.fillRoundedRect(x + w - 16.0, y + h / 2.0 - 4.0, 8.0, 8.0, 4.0)   // 状态列点个小圆点 ✓
})
```
- 时机：该格**常规内容画完之后**；矩形已含滚动偏移与冻结列 ⇒ 应用不用自己算坐标 ✓
- 覆盖范围：复选框列**也会**回调；**正在行内编辑的那格不回调**（那一刻归 Edit 管 ✓）
- 表格**不接管格内点击**：命中仍是行/单元格级（`on_cell(row, col)` ✓）
  ⇒ "格内小按钮 / 可拖进度条"那种真控件要处理可见行实例化与生命周期，本轮**没做** ✓
- ⛔ 别在回调里 `measureString` 造字符串：表格刻意避开"每格每帧测宽"（进度条自己的百分比
  文字宽度按 0..100 **一生只测一次** ✓）

### 自检（都无头可跑 ✓）
- 纯逻辑：`assert\test_table_progress_cells.leno`（进度值解析 + 列类型边界，**不建窗口、不加载字体** ✓）
- 真渲染：`leno_gui\控件\表格\test_table_cells.leno`（自带 `DSHOT` 钩子 ⇒ 截图后退出，见 §七 ✓）
  它在 `assert\run_gui_checks.leno` 的无头清单里 ✓
