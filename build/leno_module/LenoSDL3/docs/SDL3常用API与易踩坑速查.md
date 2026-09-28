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

### ✅ 空 `catch { }` 会提示（新增编译器警告 ✓）

`catch` 体里**一个语句都没有**时（**只写注释也算空** —— 注释不是 AST 节点 ✗），编译器给一条
`warning: [空catch吞异常]`：

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

**新增编译期提示**（`warning: [不可能收窄]`）——**只有**下列情形才提示（零误报口径 ✓）：

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
