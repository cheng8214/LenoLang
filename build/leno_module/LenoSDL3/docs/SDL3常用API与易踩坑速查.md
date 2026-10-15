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
TitleBar tb = SDL3.createTitleBar({title: "我的应用", h: 32, actions: [
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

---

## 附录 C：`Card` 卡片控件（2026-10-13 新增 ✓）—— 本轮实测的坑

`Card` = 「图片 + 标题 + 副标题 + 角标 + **可挂子控件的内容区**」的网格单元（`sdl_card.leno`）。
为什么不能用 `Panel` 拼：Panel 的子控件走**窗口绝对坐标**、`Panel.set_pos()` 只挪自己（`sdl_panel.leno:99`），
而 GridLayout / ScrollView 只会对**子控件本身**调 `set_pos` ⇒ 卡里的图和字**原地不动** ✗。
设计文档：`docs/LenoSDL3_Card卡片控件设计文档.md`；首个使用方：`leno_gui\应用\影视海报墙\`。

### ⛔ 纹理绘制的 dst 必须是 `SDL_FRect` —— 传 `SDL_Rect` 画面直接"空掉"（实测 ✓）

SDL3 的 `SDL_RenderTexture(ren, tex, const SDL_FRect* src, const SDL_FRect* dst)` **两个都是浮点矩形**，
而框架的 `Renderer.drawTextureRect(tex, srcrect, dstrect)` 收的是裸 `Ptr[u8]`（**不做类型检查** ✗）——
把一个 `SDL_Rect`（i32）传进去**不会报任何错**，只是字节被当成浮点解释 ⇒ `w/h` 变成约 1e-45 ⇒
海报全变深色空板 ✗（本控件 v2 首版就这么栽的：22 张卡全空、文字与角标却正常 ✓）。

```leno
// ✗ 空板：SDL_Rect 是 i32
SDL_Rect dst = SDL_Rect.malloc(); r.drawTextureRect(tex, null, dst.to_ptr())
// ✅ 正确：源/目标都用 SDL_FRect
SDL_FRect dst = SDL_FRect.malloc(); r.drawTextureRect(tex, null, dst.to_ptr())
// 高层 helper 也是浮点口径，照它们抄就对了 ✓
//   drawTextureAt(sdl_renderer:627) / drawTextureFromTo(:643) —— 内部用 SDL_FRect ✓
```

- 对照：`pushClipRect` 要的**就是** `SDL_Rect`（`SDL_SetRenderClipRect` 是整型 ✓）⇒ 两者**别混用同一个变量** ✓
- 排查手法：`r.getTextureSize(tex)` 正常、纹理非 null，但画面是空的 ⇒ **先怀疑 dst 矩形类型** ✓

### ✅ 图片"覆盖填满"要**裁源**，不要裁屏（目标：整卡每帧 0 次 `pushClipRect`）

`pushClipRect` / `popClipRect` 每次都先 `flushBatches()`（`sdl_renderer:562,595`）⇒ 每张卡每帧 2 次 flush；
22 张卡 = 44 次/帧，正是 `LenoSDL3_裁剪过多导致闪烁排查记录.md` 里那条隐患 ✓。
正解：把居中裁切算进**源矩形**（`image_mode:2` / `MODE_ASPECT_FILL`），dst 就等于图片区
⇒ 零裁剪、零溢出、还少画被丢弃的像素 ✓（v1 走裁屏、本仓海报墙 **100% 命中** ✗；v2 改裁源后为 0 ✓）

### ✅ 容器化（`set_content`）的两条硬约束

1. 内容控件要**每帧**跟着卡片走：`process` 与 `render` 里都 `set_pos/set_size`（照 `Panel.syncLayoutChild` ✓）
   —— 只在一个地方同步的话，滚动/换筛选后的第一帧命中会错位 ✓
2. 卡内控件的点击**不能再触发整卡 `on_click`**（"点收藏 ⇒ 又打开了详情" ✗）：
   命中点落在内容区时直接跳过 `onClick`；确实要穿透就给 `content_click_through:true` ✓
   自测手法：`Card.content_rect()` 取内容区几何 → `SDL3.testClickAt(wid, x, y)` 注入点击 →
   断言日志里**没有**整卡回调那一条（`leno_gui\应用\影视海报墙` 的 `WALL_CLICKSEL` 就是这么验的 ✓）

### ⚠ 卡片高度是 `GridLayout` 在 `add` 时**捕获**的

行高取该行子项的**自然高**（`sdl_gridlayout.leno:181-193` 用的是 `_natH` ✗）⇒ 运行时改卡片高度
（例如挂了内容行之后变高）必须 `grid.clear()` 后重新 `add` 一遍；光 `set_size` **不会**让行高变 ✓
（对照：`grid.set_cols()` 是每帧重算 ⇒ 下一帧即生效 ✓ —— 两者行为不同，别记混 ✓）

### ⛔ 圆角**纹理**合成：「只乘 alpha」的混合模式要**自组**，且 `SDL_RenderGeometry` 看**纹理自身**的 blend（2026-10-13 实测 ✓）

`Renderer.drawTextureRounded` 是「图 → 离屏 → 圆角蒙版乘上去（**只裁 alpha**、图片颜色不动）→ 贴回」。
这里连着踩了两个坑，第二个的症状很像"圆角根本没生效"：

**① SDL 自带的 BLEND / ADD / MOD / MUL 一个都不能用**

第一版按"MUL = `dst.rgb *= src.rgb; dst.a *= src.a`"写（**这个理解是错的** ✗），结果圆角切口里
照原图**又画了一遍**：SDL3 的 MUL 实际是 `dst.rgb = src.rgb*dst.rgb + dst.rgb*(1-src.a)`、
**alpha 原样不动**（MOD 同样只动 rgb）⇒ 蒙版 a=0 处 rgb 反被放大成 **2 倍**、alpha 还是 1
⇒ 症状就是"圆角切了，又补回一块更亮的直角原色"（像素实证：图内 `0f1f3c`、切口 `1e3e78` ≈ ×2 ✗）。

正解是**自组**一个混合模式（`sdl_renderer.maskAlphaBlend()`：懒创建一次、全进程复用 ✓，
底下的 C API 是 `SDL_ComposeCustomBlendMode` ✓）：
色域因子 `ZERO/ONE + ADD` ⇒ `dst.rgb` **原样**；alpha 因子 `DST_ALPHA/ZERO + ADD` ⇒ `dst.a = src.a * dst.a` ✓

**② 纹理级 blend 才是生效的那一份**

第一版只设了**渲染器级** MUL（`setBlendMode(MUL)`）⇒ 蒙版被当普通 BLEND 盖上去 ⇒ **整块变白** ✗
（`SDL_RenderGeometry` 用的是**纹理的** blend，不认 `SDL_SetRenderDrawBlendMode`）⇒ **两边都设**。

```leno
int mm = maskAlphaBlend()                                 // ★ 只乘 alpha 的自定义模式
_lib.SDL_SetTextureBlendMode(mask, mm)                    // ★ 关键：纹理级
setBlendMode(mm)                                          // 渲染器级也设一份（双保险）
emitMask9(mask, Mq, 0.0, 0.0, tw, th, corner, corner)
_lib.SDL_SetTextureBlendMode(mask, core.BlendMode.BLEND)  // ★ 蒙版是**共享缓存**，必须当场还原 ✗
```
- 蒙版来自 `rrGet()` 的**缓存**（`fillRoundedRect` 也在用）⇒ 不还原的话，之后所有圆角矩形全糊 ✗
- 贴回前还要把**离屏 target 纹理**的 blend 再设成 BLEND（被当作 target 用过后 SDL 可能改成 NONE ✗）
- 顶点色给白即可（这也顺带解释了为什么不能用 `fillRoundedRect` 那种"顶点色染蒙版"的路子 ✗）

### ⛔ 软件渲染器建不了离屏/蒙版纹理 ⇒ 圆角**静默回退方角**（验证必须真机 ✓）

`SDL_VIDEODRIVER=dummy`（软件渲染器）下 `SDL_CreateTexture(RGBA8888, TARGET)` 与蒙版创建都失败，
`drawTextureRounded` 会**静默走 `drawTextureRect`**（方角）⇒ **对着 dummy 截图永远看不出圆角有没有生效** ✗
（本轮一开始就是这么误判的：以为圆角没做出来，实际是 dummy 下走了回退 ✓）
- 回退时有**一次**提示行（`[LenoSDL3] 圆角纹理合成不可用…`）；只提示一次，否则 22 卡 × 60fps 会刷屏 ✗
- **验证圆角必须真机**：`Remove-Item Env:SDL_VIDEODRIVER`（或新开 shell）后直接跑，
  自带 `CSHOT`/`CAT` 钩子截图（本仓惯例 ✓，窗口闪一下即退 ✓）
- ⛔ 顺带一个探针坑：**自己 `createRenderer` 的句柄在 `win.run` 跑起来后会失效**
  （SDL 报 `Parameter 'renderer' is invalid`，于是建纹理/离屏全失败 ✗）⇒
  探针里所有纹理都要用**回调参数里给的** `Renderer r` 创建（`r.handle` ✓）

### ⛔ `win.setRootAt` 在 **run() 之后**换根以前是**静默 no-op**（整页切换必踩 ✓ 2026-10-13 修）

症状：想在运行时"整页切换"（列表页 ⇄ 详情页），于是 `win.setRootAt(detailPage, 0, tbH)` ——
**日志说换成功、界面纹丝不动** ✗（还是旧页）。自己 `createXxx` + `add` 的新页一个像素都不画。

病因：`run()` → `registerOnly()` 会把本窗口的根**拷一份**进主循环的快照 `_runRoots[i]`，
而主循环的**绘制与事件派发**只认那份快照（`rootWid?.render` / `rootWid?.process`）⇒
`setRootAt` 只改了 `self.root` 这个布局字段 ⇒ 快照里还是旧根 ⇒ 换了等于没换 ✗。

**怎么办**（框架侧已修 ✓）：`setRoot` / `setRootAt` 末尾都会调 `_syncRunRoot()`，
把当前根同步回 `_runRoots[i]` ⇒ 运行中换根**当场生效** ✓（run() 之前调用时找不到 wid，什么都不做 ✓）。
写法照旧：

```leno
// 整页切换 = 换根（比"隐藏/显示"干净：隐藏的页在 VBox 里照样占位 ✗）
win.setRootAt(dpage, 0, tbH)      // 进详情页
win.setRootAt(root, 0, tbH)       // 返回列表页（root 要**存字段**，别只留局部量 ✓）
```

回归件：`leno_gui/应用/影视海报墙/movie_wall.leno`（`WALL_DETAIL=n` 进详情 / `WALL_BACK=n` 返回 ✓）

### ⚠ 嵌套控件"拿不到窗口尺寸"：**菜单其实自己会兜底**，真正的缺口是"弹出瞬间用了缓存旧值"（2026-10-13 实测 + 已修 ✓）

**先纠正一个很容易误判的点**：`sdl_layout` 的尺寸下发里，**嵌套在容器里**的控件收到的是 `ww = -1`（尺寸未知 ✓）
⇒ `Table`/`TreeView`/`ListBox` 的窗口尺寸字段一直是 0 ✓。**但这不等于弹层会跑偏** ——
`sdl_menu.clampRect` 里早就有一条兜底：尺寸 ≤ 0 且拿得到句柄时，直接 `SDL_GetWindowSize(winHnd)` ✓。

实测（探针 `leno_gui/自测/test_nested_context_menu.leno`：Table 嵌在 `根 → HBox → VBox` 三层里、尺寸字段恒为 0）：
在窗口**右下角**右键 ⇒ 菜单被**正确翻转**、完整留在窗口内 ✓ ⇒ **不需要**让容器层层携带尺寸 ✓✓
（所以"给句柄路径层层加尺寸参数"那条路可以不走 ✓）

**真正的小缺口**（同一支代码里，实测抓到）：那条兜底**只查一次就缓存住** ✗ ⇒ 窗口 resize 之后再弹
（此刻菜单是关着的 ⇒ 没有任何人刷新这个字段 ✓）会**先用旧值算一遍**。日志实证：
```
clampRect：_winW=700（旧）锚点=390,310   ← 第一遍用旧值
clampRect：_winW=420（新）锚点=390,310   ← 同帧稍后 renderContextMenus 刷成新值并重算（画的位置对了 ✓）
```
画的位置会被纠正 ✓，但 `openContext` 正是拿**第一遍**的结果去 `setPopupCapture(...)` 登记**弹层捕获区** ✗
⇒ 那一帧的捕获区是歪的（点击可能穿透 / 被误吞 ✓）。

**修法（已落库 ✓）**：`openContext` 弹出前**按句柄刷一次真实窗口尺寸**（6 行 ✓），两处都用新值 ✓。

**验收**：探针里"开一次（700×460）→ 关掉 → 窗口缩到 420×340 → 再右键右下角"：
- 修复前日志：`openContext 夹紧用尺寸 700x460 ⇒ 捕获区=365,366` ✗（与菜单实际画的位置不符）
- 修复后日志：`420x340 ⇒ 捕获区=85,256` ✓（85 = 390 − 305 = 翻转后的正确位置 ✓）

### ⛔ `Table` 的图标纹理缓存**只增不减** ⇒ 显存一路涨（2026-10-13 实测 + 已修 ✓）

**症状**：表格有"**每行一个不同图标**"的场景（最典型就是"每行一张缩略图/海报"）⇒ 用久了显存持续增长 ✗
（缓存按 path 存 ⇒ 路径一多就无限累积 ✓）。

**病因**：`get_tex` 只有"没命中 ⇒ 加载 + add"这一条路 ✗，**没有任何上限 / 淘汰** ✓。

**修法（已落库 ✓）**：照 `sdl_font` 的 `texCap` 样板做**环形覆盖 + 当场回收** ✓：

```leno
if imgCacheMax > 0 and imgCachePaths.len() >= imgCacheMax {
    Ptr[u8] old = imgCacheTex[imgCacheHead]
    if old != null and old != t { core.lib().SDL_DestroyTexture(old) }   // ★ 是"回收"，不是"丢引用" ✗
    imgCachePaths[imgCacheHead] = path; imgCacheTex[imgCacheHead] = t
    imgCacheHead = (imgCacheHead + 1) % imgCacheMax
    imgLastPath = path; imgLastTex = t     // ⚠ 被覆盖那格可能正是 MRU 快路径指着的 ⇒ 必须指向新纹理 ✓
    return t
}
```
- 新选项 **`image_cache_max`**（默认 **64**；`0` = 不限制 = 老行为 ✓）+ `set_image_cache_max(n)` + `img_cache_count()`（调试/自测 ✓）
- ⚠ 上限要**比"同时可见的行数"大一些**（否则每帧反复重载 ✗）
- ⚠ 这里销毁是**安全**的：纹理由 `IMG_LoadTexture` 新建、**不共享**（每次调用都是新纹理 ✓），
  且淘汰时已从数组移除 ⇒ `dispose` 不会再碰到它 ✓（`dispose` 里剩下那 ≤ max 张仍交给渲染器统一释放 ✓）
- ⚠ **别把这句"销毁"照抄到 `dispose` 里** —— 那里有 double-free 的历史教训（见 `sdl_font.leno` 注释 ✓）

**验收**（探针 `leno_gui/自测/test_table_image_cache.leno`：6 行各贴一张不同海报 p01..p06）：
```bash
set PROBE_MAX=3 & build\leno.exe leno_gui\自测\test_table_image_cache.leno     # ⇒ img_cache_count()=3 ✓（上限生效）
set PROBE_MAX=0 & build\leno.exe leno_gui\自测\test_table_image_cache.leno     # ⇒ =6 ✓（老行为）
```
且截图里 **6 行缩略图全部正常** ✓（上限 3 < 6 行 ⇒ 渲染中途就在回收，也没画坏 ✓）

### ⛔ **失焦不收弹层**：Alt+Tab 切走后下拉/菜单**留在屏幕上、点不动也关不掉**（2026-10-13 实测 + 已修 ✓）

**症状**：打开 ComboBox 下拉（或任何右键菜单）后 `Alt+Tab` / 切到别的应用 ⇒ 弹层**照旧挂在屏幕上** ✗；
又因为事件已经不在它那儿了 ⇒ **点不动也关不掉** ✗（只能切回去再点一下 ✓）。

**病因**：`WINDOW_FOCUS_LOST`（`0x20F`）这一支没人接 ——
- `ComboBox.process` 里**没有**这个分支（而 `PopMenu` 早就有：`if ev.isWindowFocusLost() { hide(); return }` ✓）
- 右键上下文菜单（`MenuBar` 那套）与顶部菜单下拉**根本收不到**：它们不是宿主控件画的，而是按
  `isOpen()` **第二遍统一画** ✗（与 `TabControl` 切页时那个同源 bug 一模一样 ✓）

**修法（已落库 ✓，两处）**：
1. `ComboBox.process` 开头：失焦 ⇒ `close()` + 清悬停/拖拽态 ✓（照 `PopMenu` 的既有写法 ✓）
2. 窗口事件循环的**派发处集中收拾**（全工程只有这一处派发 ✓，模态窗/普通窗都走它 ✓）：
```leno
if ev.isWindowFocusLost() {
    menu.closeContextMenus(_runHnds[j])       // 右键菜单（按窗口隔离 + 未登记句柄的兜底 ✓）
    var bars = _runMenus[j]
    for bars to b { b.close() }               // 顶部菜单下拉
}
```
⚠ **`for arr to x` 里的 `x` 就是元素、不是索引**（第一版写成 `bars[b]` 直接编译不过 ✗；
   仓库惯例见 `for widgets to w { w.dispose() }` ✓）

**新增自测钩子**（本次为了能让它自动回归而补 ✓）：
```leno
SDL3.testFocusLost(win.getID())      // 注入一次窗口失焦（与 testClickAt 同一思路 ✓）
SDL3.testFocusGained(win.getID())    // 配对
```

**验收**（两个探针，判据都是"修前 true / 修后 false"）：
- `leno_gui/自测/test_combobox_focus.leno`：点开下拉 ⇒ `isOpen()=true` ✓ ⇒ 注入失焦 ⇒ `isOpen()=false` ✓；
  且截图里下拉区域可见像素 **7205 → 0** ✓（修前一直是 true、下拉挂着 ✗）
- `leno_gui/自测/test_window_handle_size.leno`：右键 `Table` 打开菜单 ✓ ⇒ 注入失焦 ⇒ `get_context_menu().isOpen()=false` ✓

### ⛔ `Spinner` 画出来是**多边形**（段数公式 + `thick` 形参没用上）（2026-10-13 实测 + 已修 ✓）

**症状**：转圈指示器看着是**十边形**（棱角明显）而不是圆 ✗；粗细也跟 `thickness` 无关（像固定 1px ✗）。

**病因**（`sdl_spinner.draw_arc` 老实现）：
```leno
int segs = _int(radius * span / 360.0 * 0.4)   // r=26、span=120° ⇒ 3.46 ⇒ 被夹到 4 段 ✗
if segs < 4 { segs = 4 }
...
r.drawLine(x1, y1, x2, y2)                     // ⚠ `thick` 形参**压根没用** ✗
```
⇒ 120° 的弧只有 **4 段**、整圈轨道只有 **10 段** ⇒ 十边形 ✓（弦中点内凹 `r(1-cos 18°)`，r=96 时约 **4.7px** ✓）

**修法（已落库 ✓）**：**别走"把段数调大"这条路** —— `drawLine` **没有抗锯齿、也不能设线宽** ⇒ 加密只会得到
"更细腻的多边形" ✗。改用 `fillCircle`（**抗锯齿** ✓；走圆角蒙版缓存 ⇒ 同半径复用同一张蒙版、开销很低 ✓）
沿弧线**密排**：

```leno
float rad = thick / 2.0
float stepLen = rad * 0.5; if stepLen < 1.0 { stepLen = 1.0 }   // 相邻圆必重叠 ✓
int steps = _int(radius * span * d2r / stepLen); /* 上限 720 保护 ✓ */
for 0 : steps to i {
    float aa = a0 + stepA * (_float(i) + 0.5)
    r.fillCircle(cx + radius * maths.cos(aa * d2r), cy + radius * maths.sin(aa * d2r), rad)
}
```
⇒ 边缘无棱角 ✓、天然圆角端点 ✓、粗细就是 `thickness` ✓、**不依赖背景色** ✓
（⚠ 没照 `RingProgress` 的 "fillPie + fillCircle 抠底"：那要求知道**内圆底色**（它会露在环内侧 ✓），
而本控件是盖在任意父容器上的 ⇒ 抠底会露出错色 ✗，除非新增 bg 选项、DX 变差 ✗）

**同批修的另外三条**（同一文件）：
- `set_enabled` 原来是**空实现** ✗ ⇒ 现在真的停转 + **不再每帧请求重绘** + 弧线变暗 ✓
  （实测帧率：启用 **2s / 103 帧** ⇒ 两个都禁用 **2s / 20 帧** ✓，那 20 帧是自检定时器自己的 ✓）
- `tip` 原来是**死代码** ✗（`process` 是空实现，从不上报）⇒ 现在照 `RadioButton` 写法在 motion 里上报 ✓（悬停实测弹出 ✓）
- `step()` 的角度归一化只减一次 360 ✗ ⇒ 一次卡顿（dt 大）后弧段会**跳变** ⇒ 改用 `while` ✓（`RingProgress.step_sweep` 同款一并修 ✓）

**验收**（探针 `leno_gui/自测/test_spinner_shape.leno`：size 220 / thickness 14 / `running:false` + `full_track:true` ⇒ 静态整圈）：
```bash
set PROBE_SHOT=%TEMP%\s.png & build\leno.exe leno_gui\自测\test_spinner_shape.leno
```
用 PowerShell 逐 2° 采样（共 180 个角度）沿射线找弧带 ⇒ 修后：**0 个未命中**、
弧带中心相对理想半径偏差 **-1.0 … +1.25px**（只剩量化 + AA 噪声 ✓）、弧带厚度 **13.5 … 16.5**（≈14 ✓）。
老实现：按公式 10 段 ⇒ 内凹 ~4.7px、线宽 ~1px ✗

### ⛔ `TabControl` 二次 `set()` 会把**整页内容丢掉**（"运行时改配色/字号"就会踩）（2026-10-13 实测 + 已修 ✓）

**症状**：页里放好的按钮/表格，**运行时改一下外观就当场消失**、点击也不再响应 ✗ ——
实测：`tabs.set(同 opts + 改一个颜色)` 之后，页里的按钮从界面消失、`on_click` 计数停在 1 ✗（截图只剩标签条 ✓）。

**病因**：`set()` 每次都**无条件重建** `pages / rel / _pageLayouts`（页子控件数组）✗ ⇒ 已加进页里的控件
连同 `setPageLayout` 设过的布局一起被丢弃 ✓。而"二次 set"在别处**是常见用法**（`Card.set` 就是运行时改样式 ✓）。

**修法（已落库 ✓）**：
- **页数没变就不重建**页容器（同数量的重配只该改外观、不该动内容 ✓）
- `active` 缺省值改取**当前页**（`opts.get("active", _active)` ✓）—— 否则只改个配色也会把页签**跳回第 0 页** ✗
- 页数**真的变了**才重建；此时旧页子控件**故意不 dispose**（它们是应用创建并持有的，框架替它释会变成"释放后再用" ✗）

**验收**（探针 `leno_gui/自测/test_tabcontrol_reset.leno`）：页 A 放一个按钮 → 点一次（计数 1 ✓）→ `tabs.set(同 opts)` → 再点：
⇒ 计数应为 **2** ✓ 且截图里按钮仍在 ✓（修前：计数停在 1、按钮消失 ✗）

### ⛔ `Table` 的自定义格色**不跟着数据走**：`remove_row` 后错位一行、`set_rows` 后残留（2026-10-13 实测 + 已修 ✓）

**症状**（两条都是"张冠李戴"，实测过 ✓）：
- **删行**：给 R2 上绿、R3 上红 ⇒ `remove_row(0)`（数据整体上移一行）之后，绿/红**停在同一屏幕行** ✗
  ⇒ 等于染错了行（实际染上 R3/R4 ✓）
- **换数据**：`set_rows(全新 N0..N4)` 之后，上一批的绿/红**原样还在** ✗ ⇒ 新数据莫名其妙带着颜色 ✓

**病因**：单元格级颜色存在 `_cellFgColors/_cellBgColors`（`Dict`，键是 **`"row,col"`** ✓）里，
而 `remove_row` **不动**这些键（下面的行上移了、键没跟上 ✗）、`set_rows` / `clear` 也**不清** ✗。
（`swap_rows` 里**早有**同款同步 ⇒ 缺的就是这两处 ✓）

**修法（已落库 ✓）**：口径与"排序不许说谎"**完全一致** —— **要么别张冠李戴**：
- `remove_row(idx)` ⇒ 老行 `o > idx` 的键整体搬成 `o-1`（照 `swap_rows` 的思路；用新行数 `n` 表达 ⇒ 搬 `(r+1)→(r)` ✓）
- `set_rows` / `clear`（**换整批数据**）⇒ 清掉 `_cellFgColors/_cellBgColors` 与 `rowColors` ✓
  （应用想换数据后染色，就在 `set_rows` **之后**再 set —— `cleaner_master` 本来就这么写 ✓）

**验收**（探针 `leno_gui/自测/test_table_cell_colors.leno`：行 R0..R4，R2 绿 / R3 红）：
```bash
set PROBE_SHOT_A=%TEMP%\a.png & set PROBE_SHOT_B=%TEMP%\b.png & set PROBE_SHOT_C=%TEMP%\c.png
build\leno.exe leno_gui\自测\test_table_cell_colors.leno
```
流程：截图 A → `remove_row(0)` → 截图 B → `set_rows(N0..N4)` → 截图 C；然后读**同一列**各行像素：
- A：绿在行 2、红在行 3 ✓
- B：绿/红应**整体上移一行** ✓（修前停在原位 ✗）
- C：**全无染色** ✓（修前绿/红原样残留 ✗）

### ⛔ `Scrim`（模态遮罩）**不跟随窗口缩放** ⇒ 拉大窗口后右下露一条"没遮住、还点得动"（2026-10-13 实测 + 已修 ✓）

**症状**：模态遮罩本来铺满得好好的；**一拉大窗口**，右侧/下侧就露一条**没被压暗**的原色 ✗ ——
更糟的是那条**点得动**（`Scrim.process` 正是按自己的矩形吞事件的 ✓）⇒ 模态等于漏了半边 ✓。

**病因**：尺寸只在 `win.addScrim` **创建时**下发一次（`s.set_size(_w,_h)` ✗），之后没人管；而窗口的
resize 通知是一张**硬编码类型表**（只认 `Table` / `TreeView` / `Splitter`）⇒ `Scrim` 从来收不到 ✓。

**修法（已落库 ✓）**：给 `Scrim` 加 `on_window_resize(ww,hh) { set_size(...) }`，并把 `Scrim`
接进窗口那 3 处类型表（主循环 / 模态窗 / 悬停弹层窗 ✓）。

**验收**（探针 `leno_gui/自测/test_scrim_resize.leno`：底色纯白 + 遮罩 alpha 120 ⇒ 被盖住处 ≈ `878787`、露出处仍是纯白 ✗）：
```bash
set PROBE_SHOT=%TEMP%\s.png & build\leno.exe leno_gui\自测\test_scrim_resize.leno
```
600×400 → `show()` → `setSize(880,580)`，然后逐点读像素：
修前 `(640,200)` / `(300,460)` / `(820,540)` 全是 `ffffff` ✗ ⇒ 修后 6 个采样点全 `878787` ✓

⚠ ~~顺带记一条框架设计债~~ ⇒ **债已还（2026-10-13 ✓）**：窗口那条 resize 通知原来是**硬编码类型表、
同一份写了 3 遍**（主循环 / 模态窗 / 悬停弹层窗）✗，现已照 `_passWindowHandle` 的样板收敛到
`sdl_layout._passWindowResize` **一处** + `setExtraPassResize` 扩展钩子（由 `sdl_window` 注册 ✓）⇒
**以后新控件想收 resize，只加一处即可** ✓（`sdl_window` 那三个循环现在一个具体控件类型都不认识了 ✓）

⚠ 同一批还掉了**句柄路径**的同类债（2026-10-13 ✓）：`sdl_layout` 的 `_extraPassWindow` 钩子签名补上
**窗口逻辑宽高**（`ww < 0` = 尺寸未知 ✓，容器路径就传 -1 ✓）；`Table/TreeView/ListBox` 从布局那层挪进钩子
（它们要"句柄 **+ 尺寸**" ⇒ 得由认识窗口尺寸的一方下发 ✓）；`sdl_window` 里原先**两份**类型表
（`_refreshWindowHandle` 与 `Window._passWindowHandle`）**全部删掉**、只调 `passWindowHandleSized` ✓
—— 顺带治好它俩的**漂移** ✗（一份有 `ListBox`/`GridLayout`、另一份有 `Card` 却漏 `ListBox`，靠"每帧安全网"掩盖 ✓）。
⚠ **仍未覆盖**：嵌套在容器里的 `Table`/`Splitter` 拿不到窗口尺寸（容器只知道句柄 ⇒ `ww = -1` ✓）——
实际影响≈0（右键菜单每帧由 `menu.renderContextMenus` 用**当前窗口尺寸**刷新 ✓），要彻底修得让容器层层带尺寸 ✗

### ⛔ 图表数值标签会**丢小数**（`_fmtNum` 拿 `_int(v)` 截断）—— 2026-10-13 实测 + 已修 ✓

**症状**：把**小数指标**画进图里，图上的数字是**取整后**的 —— 实测评分 `7.7 / 8.2 / 9.3 / 8.4` 画成柱状图，
柱上标签显示 `7 / 8 / 9 / 8` ✗；Y 轴刻度同理（真值 2.5 / 7.5 的位置写成 `2` / `7` ✗）⇒ **图表在说谎** ✓
（最坑的是它看着"很正常"：数字是整的、位置也对，只有**读数**是错的 ✓）。

**病因**：`_fmtNum(float v) = (_int(v) as string)`（原注释还写着"图表数据通常为整数指标" ✗）。
它同时供 **Y 轴刻度 / 柱上数值 / 悬停提示 / 折线图与饼图的图例** 用 ⇒ **一处错、全图错** ✗。

**修法（已落库 ✓）**：整数显示整数、有小数如实显示：
```leno
int i = _int(v)                        // ⚠ `_int` 是**截断**、不是四舍五入 ✓
float d = v - _float(i); if d < 0.0 { d = 0.0 - d }     // 负数（-7.9 ⇒ d=-0.9）取绝对值再比 ✓
if d < 0.0001 { return (i as string) } // 整数值 ⇒ "8"（别写成 "8.0" ✗ 刻度上一堆 .0 很啰嗦）
return _str(v)                         // 有小数 ⇒ 如实（实测 `_str(7.7)="7.7"`、尾随 0 会去掉 ✓）
```

**验收**（探针 `leno_gui/自测/test_chart_labels.leno`，数据 7.7 / 8.2 / 9.3 / 8.4）：
```bash
set PROBE_SHOT=%TEMP%\c.png & build\leno.exe leno_gui\自测\test_chart_labels.leno
```
⇒ 柱上标签应为 `7.7 / 8.2 / 9.3 / 8.4` ✓、Y 轴出现 `2.5 / 7.5` ✓、饼图图例带小数 ✓（修前全是整数 ✗）

### ⛔ `RadioButton` 同组互斥：**被挤掉那一项也要有回调**（2026-10-13 实测 + 已修 ✓）

**症状**：两组单选各挂 `on_change`，点第二个 ⇒ 界面是对的（第一个的圆点消失 ✓），
但**第一个的 `on_change(false)` 从来不响** ✗ ⇒ 靠回调维护状态的应用会"界面上没选、自己还以为选着" ✗
（最坑的是**静默**：没有报错、画面也对，只有背后的状态错了 ✓）。

**病因**：`RadioButton.select()` 取消同组其它项时**直接改字段**（`rb.checked = false`），没走回调 ✗。

**修法（已落库 ✓）**：取消时照样通知（`rb.notify_change(false)` ✓）。记牢这条契约（与 `CheckBox`/`Toggle` 一致 ✓）：
- **用户操作**（点自己 / 被同组挤掉）⇒ 回调 ✓
- **程序化** `set_checked(true/false)` / `set_on(...)` ⇒ **不回调** ✓（否则"设值 → 回调 → 又设值"互相打架 ✗）

**验收**（探针 `leno_gui/自测/test_form_controls.leno`）：先点 r1、再点 r2 ⇒ 依次应看到
`r1 收到回调 on=true` → `r1 收到回调 on=false` → `r2 收到回调 on=true`，终态 **r1 回调 2 次 / r2 回调 1 次** ✓
（修前 r1 只有 1 次 ✗）

### ⛔ `Toggle` / `CheckBox` / `RadioButton`：尺寸**自量**，布局给的尺寸被忽略（2026-10-13 实测 ✓）

**症状**：`hbox.add(tg, {grow: 1, basis: 300})`、或到处 `set_size(...)` 想拉长它 ⇒ **毫无变化** ✗
（它照自己的老尺寸画，只是在布局里多占了空位 ✓）。

**病因**：这三个控件的 `set_size(float, float) {}` 是**空实现**（设计使然：开关宽 = `w`、方框 = `box`、
总宽还要加文字宽 ⇒ 见各自的 `get_w()` ✓）。实测：`set_size(200,44)` 之后 `get_w` 仍是 122、`get_h` 仍是 28 ✓。

**怎么办**：
- 尺寸**建的时候**给：`createToggle({w: 64, h: 32})` / `createCheckBox({box: 26})` / `createRadioButton({box: 26})` ✓
- `grow` / `basis` 只决定它**占的位置与间距**，改不了它画多大 ✓（要横向铺满得换分组方式 ✓）
- ⚠ 同类"自量尺寸"控件还有 `Badge` / `LinkButton`（`get_w()` 也是算出来的 ✓）

### ⛔ 自测：合成点击前**必须先投一条鼠标移动**（`testMoveAt`）—— 否则被 hover 门吃掉（2026-10-13 实测 ✓）

**症状**：`SDL3.testClickAt(win.getID(), x, y)` 返回 **true**（两条事件确实都投出去了 ✓），
但控件**毫无反应** ✗（回调不响、状态不变 ⇒ 极易被误判成"控件坏了" ✗）。

**病因**：一批控件的点击判据是 `ev.isMouseButtonDown() and hover`
（`RadioButton` / `CheckBox` / `Toggle` 都是 ✓），而 `hover` **只在 `MouseMotion` 里置位** ⇒
只投 click 的话 `hover=false`，点击被**静默忽略** ✗。

**怎么办**：注入点击前先补一条**同坐标**的移动 ✓：
```leno
SDL3.testMoveAt(win.getID(), x, y)      // ★ 先把 hover 点亮
SDL3.testClickAt(win.getID(), x, y)     // 再点（一步一行、走定时器 ✓）
```
⚠ 只有"看 hover 才认点击"的控件需要这样（Card 这类**按坐标判命中**的不受影响 ✓）——
自测脚本里**统一先移动再点击**最省心 ✓

### ⭐ 自测跑法：**应用一律带"无人值守"参数**（2026-10-13 定规 ✓）

**为什么定这条**：自动化回归里**裸跑应用**，窗口会一直挂着**等手动关闭** ✗ ——
实测：清理大师被跑了好几次，每次都要作者手动点掉 ✗（直接干扰人 ✓）。而**库和示例早就备好了钩子** ✓，
用起来只是多加一个参数 ✓：

| 场景 | 跑法 | 效果 |
|---|---|---|
| 任意应用（冒烟 / 量帧） | `set LENO_SDL_FRAMES=30` + 应用路径 | 跑满 N 帧**自动退出**并打印确定性摘要 ✓（`sdl_window.leno:1448`；配 `SDL_VIDEODRIVER=dummy` 可完全无窗口 ✓） |
| 海报墙 | `set WALL_SHOT=%TEMP%\w.png` + `set WALL_AT=8` | 第 8 步截图后自行退出 ✓ |
| 清理大师 | `build\leno.exe leno_gui\应用\电脑清理大师\cleaner_master.leno **--autotest**` | 扫到 done ⇒ 存图 ⇒ `_exit(0)` ✓（`--autoclean` 走真实清理管线 ✓） |
| `leno_gui\自测\test_*.leno` | 直接跑 | 这些回归件**自带 `_exit(0)`** ✓，本就无人值守 ✓ |

⚠ 两个注意：① `LENO_SDL_FRAMES` 会**去掉帧率节流**（每帧都真渲染 ✓）⇒ 它数的是**帧**、不是定时器步数 ✓
⇒ 依赖 `addTimer` 逐步推进的探针**不能**用它 ✗（会被"几帧就跑完"截断 —— 那种件自带退出就够了 ✓）；
② 想彻底无窗口就配 `SDL_VIDEODRIVER=dummy`，但**要截图的场合别开** ✗（会拿到空白帧 ✓）。

⛔ **批量扫探针/示例时的三条硬规矩**（2026-10-14 亲测踩过 ✓）：
1. **CWD 必须切到临时目录**再跑 ✗ —— 很多探针不配环境变量时会往**当前目录**写默认产物
   （`op.png` 就是 `test_opacity.leno` 的默认输出名 ✓），在仓库根批量跑一次就**改脏入库文件** ✓
   （实测：一次扫 95 个入口 ⇒ 覆盖了 3 份入库 png + 28 张海报图 ✓ 全靠 `git checkout --` 才还原 ✓）；
2. **跳过生成器**（`gen_icons.leno` / `gen_assets.leno` 之类 ✓）—— 它们会**真的重写资源图** ✗
   （按文件名扫"所有带 `main()` 的 .leno"就一定会扫到它们 ✓）；
3. **两条兜底**：`LENO_SDL_FRAMES=<n>` 管不住**自己写循环**的样例 ✓（实测：`特效/fireworks`·`ripple` 等
   根本**没调 `Window.run`** ✓ ⇒ 它们只能靠"起进程 + `WaitForExit(超时)` 后 kill" ✓）；
   扫完**务必 `git status --short` 复核** ✓ —— 干净才算真扫过 ✓。

### ⭐ 控件选项对账（`dochk`）：本轮把**覆盖铺满**（2026-10-13 ✓）

**它是什么**：`sdl_opts.leno` 的 `dochk.check(who, opts, 白名单)` —— 在**入口**把 opts 的键与白名单比一遍，
不认识的打一行警告 + "你是不是想要 'xxx'？"的**近名提示** ✓（`LENO_UI_STRICT=1` 时前缀变 `[ui][STRICT]` ✓）。
**为什么值得**：框架早期对不认识的键**静默丢弃** ✗ ⇒ `icon` 拼成 `image`、给自量尺寸控件传 `w/h`
这类"传了没生效"的坑只能靠读源码查 ✗（实测代价是"图标不出现、文字不显示"✓）。

**本轮补的三件事**
| 事 | 原来 | 现在 |
|---|---|---|
| **覆盖** | `window.init`（`createWindow`）/ `layout.set()`（建 HBox/VBox/AnchorBox）/ `filedialog` 三入口 / `popmenu` / `tray` —— **一个都没接** ✗ | 全接上 ✓（`layout` 尤其阴：`add(child,opts)` 那条**早就有** ✓、`set()` 这条路**完全没有** ✗） |
| **误报** | 窗口侧通用键 `z`（由 `addXxx` 读走做层级 ✓）**33 份白名单里一个都没有** ✗ ⇒ 迟早"一传就报" | 在 `sdl_opts` **一处豁免** ✓（不去改 33 份白名单 ✓）；`zz` 这类错拼**照报** ✓ |
| **可断言** | "STRICT 前缀"只写在注释里 ✗（`take_log()` 存的是**原文**、前缀只打 stdout ⇒ 断言不到 ✗） | 抽成纯函数 `format_line(msg)` ✓ ⇒ 自测**直接断言**它 ✓ |

**白名单口径**（照此写就不会误报 ✓）：按本文件**全部 `.get("…")`** 抽取 ⇒ **宁可是超集**（可少报、不误报 ✓）。
⚠ 还要含**门面收的键**：`createWindow` 的 `icon` 是 `SDL3.createWindow` 收的（它在 Linux/macOS 上
补一次 `SDL_SetWindowIcon` ✓）然后**原样透传**给 `init` ✗ ⇒ 漏了它就把海报墙的合法用法报出来 ✓（实测踩到 ✓）。

**验收**（两件缺一不可 ✓）
```
build\leno.exe assert\test_ui_opts_guard.leno      # ① 纯逻辑：拦得住 / 不误报 / 去重 / STRICT / z 豁免 / 内边距分裂
build\leno.exe leno_gui\自测\test_opts_live.leno   # ② 真控件入口：VBox / createWindow / addButton 真的接上了
```
**规模验证**：`leno_gui/应用/` 下**十个应用**各跑一遍筛 `[ui]` ⇒ **零误报** ✓
（按上一节"无人值守跑法"配 `LENO_SDL_FRAMES=30` ✓ 不需要手动关窗 ✓）。

**内边距键为什么**没**统一**（仍是**四套** ✗，但**用错会被指出来** ✓）：`pad`（Canvas/Calendar/Card）、
`pad_x·pad_y`（Badge/Alert/DragHint/TreeView）、`padX·padY`（layout/GridLayout）、`padding`（Button ⇒ 那是
**文字内边距** ✓ 语义不同）。其中若干是**刻意的**：Card 的 `pad` 与 `content_pad` 是两种内边距 ✓、
layout 的 `padTop/Left/…` 是**四边非对称** ✓（收敛成标量会丢表达力 ✗）
⇒ **不强行改名**（牵动 10 个文件、还伤表达力 ✗），改为自测钉住"**跨套用错时必有近名提示**" ✓：
`Badge` 传 `pad` ⇒ 提示 `pad_x` ✓；`Canvas` 传 `pad_x` ⇒ 指回 `pad` ✓。

### ⭐ 焦点链与键盘导航（2026-10-13 第一批 ✓ Tab / Shift+Tab / Enter / Space）

**为什么加**：全库原来只有 `Edit` / `Canvas` 自己管焦点 ⇒ **Tab 键根本不换焦点** ✗ ——
`Widget` face（`sdl_widget.leno`）只有 7 个方法、**没有焦点槽** ✗，事件又是"广播给所有控件" ✗。

**本版口径**（都是**可改的默认** ✓，别当死规矩 ✓）
| 项 | 取值 | 理由 |
|---|---|---|
| 焦点链顺序 | **注册顺序**（`addXxx` 的先后 ✓） | 稳定可预期 ✓；`tabIndex` 留待后续 ✓ |
| 窗口管什么 | 只接管 **Tab / Shift+Tab 换焦点** + **Enter / Space 激活** ✓ | 方向键留给控件自己（ListBox/Table/TreeView/ComboBox 内部导航 ✓） |
| 谁能聚焦 | 一张**类型表**（本版只有 `Button` ✓） | 与"主题点名表"同款口径：类型判断只在窗口**一处** ✓ |
| 焦点环 | 控件**自绘**（`draw_focus_ring` ✓），窗口渲染后统一叫一下 ✓ | 控件知道自己的几何 ⇒ 窗口不必知道 ✓ |
| 激活语义 | `activate()` = **只触发 `on_click`** ✓ | 与鼠标点击**同一条回调**，不写两份 ✓ |

**API**：窗口 `focus_next()` / `focus_prev()` / `focus_widget(w)` / `clear_focus()`；
控件（本版 `Button`）`set_focusable(b)` / `is_focusable()` / `focus()` / `blur()` / `is_focused()` / `activate()`。

**实现落点**（`sdl_window.leno`）：焦点链**懒收集** ✓（每次按键现扫 `widgets` 挑可聚焦的 ⇒
不维护注册表 ⇒ **不会与控件列表失同步** ✓）；`focusIdx` 存序号、`_applyFocus` **一次性**刷全部
（只让一个亮 ⇒ 不会"两个都以为有焦点" ✗）；键盘挂在 `_tryShortcut` **之后、广播之前** ✓
（快捷键优先 ✓，且换焦点这类键不会漏给每个控件各处理一遍 ✓）。

**验收**：`leno_gui/自测/test_focus_nav.leno` —— **注入真按键**（本轮为此新增钩子
`SDL3.testKeyDown(wid, scancode, mod)`，照 `testFocusLost` 的 malloc+from_ptr 套路 ✓，
门面 `SDL3.leno` 也要转出 ✓）
```
set SHOT=%TEMP%\focus.png & build\leno.exe leno_gui\自测\test_focus_nav.leno
```
实测（A 可聚焦 / B `set_focusable(false)` / C 可聚焦；注册顺序 A→B→C ✓）：
```
初始        A=false B=false C=false
Tab×1       A=true                ← 焦点到手 ✓
Tab×2                  C=true     ← **跳过了不可聚焦的 B** ✓
Tab×3       A=true                ← 末尾回卷 ✓
Shift+Tab              C=true     ← 反向 ✓
Enter/Space            hitsC=1 → 2（激活与鼠标点击同一条回调 ✓）
```
焦点环**客观取色**（不靠肉眼 ✓）：C 聚焦时，其左沿外 2px = `(100,180,255)` = 主题 `accentFocus` ✓；
未聚焦的 A 同位置只有底色 `(38,40,48)` ✓ ⇒ **环只在聚焦控件外沿出现** ✓（1px ✓）。

**第二批（2026-10-13 同日 ✓）：开关类进链条** —— `CheckBox` / `Toggle` / `RadioButton` 各补了同一套
方法（`set_focusable/is_focusable/focus/blur/is_focused/draw_focus_ring/activate` ✓），
**激活语义 = "切一下"**，而且刻意**与鼠标点击那条路逐字段对齐** ✓：
`CheckBox` 切值+回调 ✓；`Toggle` **连动画状态机一起照抄** ✓（否则键盘切换会是"瞬跳"、鼠标是平滑 ✗）；
`RadioButton` 直接调 `select()` ✓（与鼠标**同一个入口** ⇒ 互斥/回调都不会漂 ✗）。
  ⚠ 焦点环要用 `get_w()/get_h()` ✗ 不能读 `w/h` —— 开关类是**自量尺寸**控件、**根本没有 `w/h` 字段** ✓
  （实测：探针里写 `ck.w` 直接编译不过 ✓）。

**第三批（2026-10-13 同日 ✓）：`Slider` 进链条 + 键盘微调** —— `←/↓` 减、`→/↑` 加、`Home/End` 到两端 ✓；
★ 微调**只走 `set_value`** ✓（量化 / 夹取 / 回调全在它里面 ⇒ 与鼠标那条路同口径 ✓ 不写第二份 ✗）；
`activate()` 对滑块**没有标准语义** ⇒ 空实现 ✓（但方法必须在 ⇒ 才能进类型表、焦点链才停得上 ✓）。
  ⚠ 控件自己收键要注意：**键事件是广播的** ✗ ⇒ 必须 `if focused and ev.isKeyDown()` **自己按焦点过滤** ✓
  （不聚焦时不消费、不影响别的控件 ✓ —— 与 `Edit` 的做法一致 ✓；窗口那边只拦 Tab / Enter / Space ✓）。

**第四批（2026-10-13 同日 ✓）：`ListBox` 进链条** —— `↑/↓` 移动选中（会自动**滚到可见** ✓，因为走的是
`set_selected` ✓）、`Home/End` 到首末 ✓、**Enter = 激活选中项** ✓。
  ★ 列表原来**只有 `on_change`**（"选中变了"）✗、没有"确认/打开"这个语义 ⇒ 本轮补了 **`on_activate(func(int))`**
  ✓（与菜单选中、`Table` 双击属同一种"确认"动作 ✓；将来双击也可以接到这里 ✓）。
  ⚠ `activate()` 里"没选中就不激活" ✓（`get_selected_index() < 0` ⇒ 直接 return ✓）。

**第五批（2026-10-13 同日 ✓）：`ComboBox` 进链条** —— `↓/↑` 移动选中（走 `set_selected` ⇒ 回调 + 滚入视野 +
**内嵌 Edit 文本同步**一起 ✓）、`Home/End` 首末、**Enter/Space 开关下拉**（开着时按 = 选定并收起 ✓）、`Esc` 收起 ✓。
  ★ 顺手把"打开下拉"抽成**单一入口** `open_list()` ✓ —— 原来那段"滚到选中项"是**内联在点击路径里**的 ✗，
  键盘要用就得抄第二遍 ⇒ 迟早漂 ✓（现在键盘与鼠标点击共用它 ✓）。
  ⚠ 键盘分支的位置：放在**所有"弹层捕获"守卫之前** ✓ —— 下拉开着时鼠标事件会被捕获区拦掉，
  但**键盘必须照常工作** ✓（否则"开着下拉就没法用键盘选"✗）。

**第六批（2026-10-13 同日 ✓）：`TabControl` 与 `Table` 进链条**
- `TabControl`：←/→ 切页签（走 `set_active` ⇒ 与鼠标点页签**同一入口** ✓）；本版让它**可停靠** ✓
  （不然 ←/→ 没有触发时机 ✗；`show_header:false` 的场合可以 `set_focusable(false)` 退出 ✓）；
- `Table`：↑/↓/Home/End/翻页/Ctrl+A 用它**既有**的导航 ✓，焦点环 / Enter 走既有路径 ✓。

⛔⛔⛔ **写新能力前，先搜"这个控件是不是已经有这套能力"**（本轮最值钱的教训 ✓）：
我给 `Table` 又写了一份键盘导航 ⇒ 同一颗键被处理**两次** ⇒ 选中/回调双触发 ✗。而真相是
`Table` **早就有**那一套（`process` 里"键盘导航：表格聚焦时处理方向键/Enter/PageUp/…" ✓），
只是**缺一个焦点守卫** ✗。**口径**：先 `grep`（`isKeyDown` / `Scancode.` / 现成的 `select_key` 等 ✓），
再决定是"**给它补守卫**"还是"新写" ✓ —— 补守卫往往才是正确解 ✓。

⛔ **顺带修掉一个既有 bug（实测抓到 ✓）**：`Table` 那段键盘导航的注释写着"**表格聚焦时**"，
代码却是 `if ev.isKeyDown() {` —— **根本没查焦点** ✗ ⇒ 表格**没聚焦也在吃键** ✓。
症状很有代表性：别的控件按 ↓/Home/End，**表格偷偷把选中行换了** ✓
（实测：给滑块和列表发的那几个键，全被表格顺带吃了一遍 ✓）。修法：`if focused and ev.isKeyDown() {` ✓
—— **注释说"聚焦时"、代码就得真的判聚焦** ⭐。

⛔⛔ **键盘的 Enter / Space 只能由"一处"处理**（实测踩到 ✓，很值钱）：键事件是**广播**的 ⇒
窗口的焦点链调了 `activate()` ✓，控件的 `process` **又收到同一个键**、再 toggle 一次 ⇒ 两边互相抵消 ✗。
症状：`ComboBox` 的 Enter 变成"**开了又关**"（`isOpen()` 仍是 false ✓）；`ListBox` 则是**回调发两次** ✓。
**口径**：**Enter / Space 归窗口** ✓（它统一调 `activate()`）、**方向键 / Esc / Home / End 归控件自己** ✓
（控件那边 `if focused and ev.isKeyDown()` 只处理这几个键 ✓）。
⭐ **写自测件时一定要"数次数"**，光记"最后值"是**看不出来**双触发的 ✗ —— 实测就是这么抓到的 ✓
（`actLb` 一直是 0 看着没问题，加上 `actN` 才发现是 2 ✓）。

⛔ **写控件事件分支时的坑（实测踩到 ✓）**：`if ev.isMouseButtonDown() and ev.mouseButton() == 3 {`
这类句子在同一个文件里**可能出现两次**（一次在"菜单已展开"分支里、一次在主流程 ✓）⇒
用文本替换插代码时**命中了第一处** ✗ ⇒ 结果"只有菜单开着时键盘才生效"（实测：`↓` 后选中不动、
`on_change` 也不触发 ✗）。**口径**：插事件分支一律**带上前后的独特上下文**（如
`if core.inPopupCapture(mx, my) { return }` 那一段 ✓），插完再**读一遍确认落在主流程** ✓。

**第七批（2026-10-13 同日 ✓）：`TreeView` 进链条（A1 到此收尾 ✓）** —— `↑/↓` 在**可见行**里移动
（`flatten()` 出来的 `flatIds` ✓）、`Home/End` 首末、`→` 展开当前节点、`←` 收起当前节点、
**Enter = 激活**（新增 `on_activate(func(id))` ✓ 与 `ListBox` 同款 ✓）。
  ★ 一律复用公开入口：移动用 `select(flatIds[i])` ✓、展开收起用 `expand(id, bool)` ✓
  ⇒ 与鼠标那条路同口径（选中回调 / 滚入视野 / 重排可见行都在里面 ✓）。
  ⚠ 本控件**本来确实没有**键盘导航 ✓（按"先 grep"的口径查过 ✓ —— 与 `Table` 那次不同，这次是真要新写 ✓）。
  ⚠ v1 边界：`←` 对**叶子**不做"回父级" ✗（标准行为会回父级 ✓ 未做 ✓）。

⭐ **建树用的键名是 `items`**（不是 `nodes` ✗）—— 实测写成 `nodes` 只会得到一棵**空树**
（`get_visible_count()==0` ✓ 没有任何报错 ✗，是那种"静默不生效"的老毛病 ✓；
好在 `dochk.check` 现在会提示"不认识的选项 'nodes'" ✓）。

**第八批（2026-10-13 同日 ✓）：递归进容器 —— 容器里的控件也能 Tab 到** ⭐
**病根**：`Window._focusables()` 原来**只扫 `widgets`** ✗ ⇒ 装在 `HBox/VBox/AnchorBox/GridLayout/`
`Panel/GroupBox/Splitter/ScrollView/Card/页签页` 里的控件 **Tab 永远到不了** ✗
（连**根布局 `root`** 都不扫 ⇒ 走 `setRoot` 布局的应用里**所有控件都不在链条里** ✓）。
**实测复现**（很典型）：面板里放一个按钮 ⇒ 链尾本该是它，结果 Tab **直接回卷到 A** ✓
（探针输出 `D.focus=false` / `A=true` ✓ —— **零报错**，只是"少停一站" ✗，又一种静默失效 ✓）。
**修法**：`_focusables()` 改成**递归收集** ✓ —— 自己可聚焦就收，然后继续下钻容器的**直接子控件** ✓
（表的口径与 `applyThemeWidget` **同一张** ✓；⚠ 以后新增容器类型，**这两处都要加** ✓）。
容器取子控件**全走公开 API** ✓：`Panel.children` / `GroupBox.children` / `Splitter._first·_second` /
`ScrollView._content` / `TabControl._pageLayouts`（含 `HBox/VBox/AnchorBox/GridLayout._c` ✓）；
`Card._content` 是 `pri` ✗ ⇒ 走它公开的 `content_widget()` ✓ —— **不用**再加"回调出口" ✓（比主题那次省事 ✓）。
  ⚠ **去重**：同一个控件可能既在 `widgets`、又在某容器子树里 ⇒ 必须"已收过就跳过" ✓
  （不然会**两次 Tab 停在同一控件** ✗）。
  ⚠ `focusIdx` 是**序号** ⇒ 树的增删会改变链条长度 ✓（与第一批同口径：每次按键现算 ✓）。
**修后实测**：`D.focus=true` ✓ → Space ⇒ `hitsD=1`（与鼠标同一条回调 ✓）→ 再 Tab 才回卷到 A ✓；
第一批~第七批的断言**一条没回归** ✓。

**A1 现状（2026-10-13 收尾 ✓）**：进链条的控件 = `Button / CheckBox / RadioButton / Toggle / Slider /
ListBox / ComboBox / TabControl / Table / TreeView` ✓。
**仍未做**：① 链只扫**顶层控件** ⇒ **已修（第八批 ✓ 见上）**；② 方向键在**兄弟控件间**移动 ✗（WPF 风格）；
③ `tabIndex` 显式排序 ✗。

⭐ **写这类自测件时注意**：`CheckBox.checked` / `Toggle.on` 等**值字段是 `pri` 私有** ✗
（探针直读会被编译器拦下 ✓）⇒ 断言一律走**公开回调**（`on_change` ✓ 它本来就是"值变了"的官方出口 ✓）。

### ⭐ 控件级不透明度：`win.set_widget_opacity(w, a)`（2026-10-13 新增能力 ✓）

**为什么加**：框架原来**没有**通用控件透明度 ✗（只有 `imageview/scrim/separator` 各自一个语义化 `alpha` key ✓）
⇒ "把某个控件整体淡出"（禁用态 / 加载中 / 逐层揭示 / 遮罩提示）做不到 ✓。

**用法**（一行 ✓）
```leno
Button b = win.addButton({x: 20, y: 40, w: 180, h: 64, text: "50%", type: "primary"})
win.set_widget_opacity(b, 0.5)      // 0.0~1.0，越界自动夹紧 ✓
```

**实现（要点：**控件侧零改动** ✓，只在收口处动手 ✓）**
- `sdl_renderer`：新增**全局 alpha 缩放** `setAlphaScale(s)`，在 3 个收口处统一乘 ——
  `setColor`（所有着色矩形/线/圆角矩形 ✓）、`setColorFloat` ✓、`pushTexBatch`（**纹理批**；文字也走
  这里 ✓ ⇒ **别在 `drawText` 里再乘一次**，否则"平方淡化" 0.5×0.5=0.25 ✗）；
  ⚠ 缩放要放在各自的**缓存比较之前** ✓（否则"同色跳过设色"的优化会绕过它 ✗）。
- `sdl_window`：渲染循环里**只包一层** —— `setAlphaScale(op)` → `render()` → `setAlphaScale(1.0)`；
  **必须还原** ✓（否则后面所有绘制都跟着淡 ✗）。值存在一个**稀疏**数组（下标对齐 `widgets`、
  越界一律 1.0 ✓）⇒ 不必去改 40+ 处 `widgets.add` 调用点 ✓，后续新增控件天然是 1.0 ✓。
- **容器设了透明度 ⇒ 整个子树一起淡出** ✓（子控件都在它的 `render()` 里画 ✓）。

**直绘纹理也覆盖（2026-10-13 同日补齐 ✓）**：`drawTextureAt / drawTextureRect / drawTextureFromTo`
三条都走 `_texScalePush` / `_texScalePop` —— 临时给纹理设 alpha 调制、画完**还原旧值** ✓
（**不能一律还原 255** ✗：纹理可能被别人设过永久调制 ✓）；`drawTextureRounded` **自动被覆盖** ✓
（它最后一步就是 `drawTextureAt` 贴回 ✓）⇒ **圆角海报也能淡出** ✓。
  ⚠ 两个前置条件（都踩过 ✗）：① FFI 只认 clib 里**声明过**的函数 ⇒ 补声明了 `SDL_GetTextureAlphaMod`
  （`sdl_core.leno` ✓）；② `&` 取址**只能取 cstruct 字段**（`&局部量` 直接报语义错误 ✗）⇒ 出参照
  `getColor` 的现成写法用 `SDL_Color.malloc()` + `&c.r` ✓。

**⚠ 踩坑（已修 ✓）：合成型绘制会"平方淡化"** —— `drawTextureRounded` 内部要**用顶点色乘蒙版**
（`setColorFloat(1,1,1,1)` ✓），若合成期间缩放还开着，蒙版 alpha 也被压到 0.4，而贴回屏幕时纹理调制
**又**乘一次 ⇒ 0.4² = 0.16 ✗（实测：opacity=0.4 的圆角海报看着像 0.16，且**逐通道与 0.16 的理论值吻合** ✓
—— 这正是"像素验收必须逐点读"的价值 ✓）。**口径**：合成函数体内**临时关掉**缩放（`savedAS` 存起来），
**只在最后一步贴回时还原** ⇒ 恰好乘一次 ✓。
  ⭐ 一般规律：**凡是"中间结果也走同一套颜色管线"的绘制，缩放只能施加在最终出口** ✓。

**验收**：`leno_gui/自测/test_opacity.leno`（同色按钮 100% / 50% / 25% 三档 + 容器 0.35 与对照组并排）
```
set SHOT=%TEMP%\op.png & build\leno.exe leno_gui\自测\test_opacity.leno
```
截图后逐块量平均色 —— 实测（2026-10-13，底色 ≈ `(38,40,48)`、按钮纯色 ≈ `(63,142,221)`）：

| 目标 | 实测平均色 | 理论混合值 `a·按钮 + (1−a)·底色` |
|---|---|---|
| 100% | (63,142,221) | (63,142,221) ✓ |
| 50% | (49,90,134) | (50,91,134) ✓ |
| 25% | (44,64,90) | (44,65,91) ✓ |
| 容器 0.35（量里面的按钮 ⇒ 验子树淡出 ✓） | (46,77,109) | (47,76,108) ✓ |
| 对照组 1.0 | (64,143,221) | — ✓ |

**逐通道吻合 ⇒ 不是"看着淡了"，是比例正确 ✓**。

### ⛔ 运行时切主题（`applyThemeAll`）：控件"没响应"的**两个真洞**（2026-10-13 实测 + 已修 ✓）

**症状**：`win.applyThemeAll(SDL3.lightTheme())` 之后，一批控件**一个像素都不变**（仍停在旧主题）✗ ——
高发位置：`ScrollView` 本体、以及放进 `ScrollView` / `Card` / `TabControl` 里的**任何控件** ✓
（普通放在窗口/布局里的 Button、Label 等反而是好的 ⇒ 容易误判成"就这几个控件不支持" ✗）。

⚠ 为什么活了很久：仓里原来唯一的主题测试 `leno_gui/控件/总览/test_theme.leno` 是
"**关闭当前窗口、以另一主题重新创建**"式的（它自己的注释第 3 行就这么写 ✓）⇒ 走的根本不是
`applyThemeAll` 这条**运行时**路径 ✗ ⇒ 两个洞都不在它的覆盖里 ✓。

**两个洞（都在 `sdl_window.leno` 的 `applyThemeWidget`）**
1. **点名表漏人**：合并 `case` 只列了 8 个类型（`Button/Label/Edit/Table/TreeView/ComboBox/MenuBar/TitleBar`），
   而全库有 **44 处 `impl Widget`** ✗ ⇒ `Canvas / ListBox / Slider / SpinBox / BarChart / LineChart / PieChart /
   Card / ScrollView / TabControl` 的 `applyTheme` **早就写好了、却从没被调用过** ✓；
2. **递归不下钻**：只递归 `Splitter / Panel / GroupBox / HBox / VBox / GridLayout` ⇒
   `ScrollView`（内容控件）/ `Card`（内容控件）/ `TabControl`（各页布局）里的控件**永远收不到主题** ✗。

**修法**（口径：类型判断仍然**只在 `sdl_window.leno` 一处** ✓，别把类型表抄进各容器 ✗）
- 点名表补成"**所有实现了 `Themeable` 的类型**"，并补上 `AnchorBox` 的递归 ✓；
- `sdl_scrollview.leno`：补 `impl Widget, Themeable` + `applyTheme`（它以前连 `Themeable` 都没 impl ✗）；
- 新增**主题递归出口** `lay.setExtraApplyThemeChild(...)`（照 `_extraPassResize` 的样板 ✓）：
  容器在自己的 `applyTheme` 末尾调 `lay.applyThemeChild(内容控件, t)` ⇒ 交回窗口继续下钻 ✓
  （`Card._content` 是 `pri` ✗ 外部拿不到，只能这么交 ✓；`ScrollView._content` / `TabControl._pageLayouts` 同理统一走它 ✓）。

**验收**：`leno_gui/自测/test_theme_switch.leno`（**同一次运行**内先截 A(dark) → `applyThemeAll(light)` → 再截 B）
```
set SHOT_A=%TEMP%\a.png & set SHOT_B=%TEMP%\b.png
build\leno.exe leno_gui\自测\test_theme_switch.leno
```
探针会把每个目标的矩形打出来（`[theme] 名字 rect=x,y,w,h`）⇒ 离线按矩形比 A/B **区域平均色**，
`Δ ≥ 12` 视为"变色" ✓。实测（2026-10-13，同一探针、同一窗口）：

| 目标 | 修前 Δ | 修后 Δ |
|---|---|---|
| `ScrollView` 自己的空白区 | **0 ✗ 没变** | 202 ✓ |
| `ScrollView` 内容里的 Card | **0 ✗ 没变** | 176 ✓ |
| Card 内容里的 Card | **0 ✗ 没变** | 125 ✓ |
| `TabControl` 页里的 Card | **0 ✗ 没变** | 186 ✓ |
| 窗口直属 Button（对照组） | 18 ✓ | 18 ✓ |

（"修前"那组是**真机测出来的**：`git stash` 掉本次 5 个库文件、跑同一探针、再 `stash pop` ✓ ——
不是靠读代码推断 ✓。）

⚠ **第二批（同日补完 ✓）**：上面这批里 **9 个"表单/展示"控件补上了 `applyTheme` 并进表** ✓ ——
`CheckBox / RadioButton / Toggle / ProgressBar / Spinner / Separator / RingProgress / ImageView / LinkButton`
（映射与各自 `set()` 里"建时取主题"的取法**逐字段对齐** ✓；`ProgressBar` 直接复用建时那三个映射函数
`barTrackColor/barFillColor/barTextColor` ✓ ⇒ 建时口径与切时口径不会漂移；重绘由 `applyThemeAll` 结尾的
`core.requestRedraw()` 统一负责 ⇒ 各控件内**不必**再请求 ✓）。
另外 6 个**不是漏，是不需要** ✓：`PopMenu` / `Tooltip` / `Dialog` 每帧现读 `getTheme()` ⇒ 本来就跟随主题 ✓；
`Scrim` 是纯黑 alpha 遮罩、无颜色可切 ✓；`Calendar` / `Steps` **设计上就不依赖主题**（配色全靠 opts，
文件头自述 ✓）⇒ "要不要跟主题"是**产品语义**，本次**没动**它们 ✓（要接主题再说 ✓）。

**第二批验收证据**（同一探针；判据强弱不同，如实记 ✓）：

| 控件 | 判据 | 修前 | 修后 |
|---|---|---|---|
| `Spinner` / `LinkButton` | 公开字段 `_color`（精确 ✓） | 切前 = 切后（**一个字节没变** ✗） | 切后变化 ✓ |
| `Separator` | 公开字段 `_color`（精确 ✓） | `#464A54`（不变 ✗） | `#D2D2DC` ✓ |
| `ProgressBar` | 矩形内暗像素占比 | 42% → **40%** ✗ | 42% → 3% ✓ |
| `RingProgress` | 同上 | 100% → **34%** ✗ | 100% → 0% ✓ |
| `ImageView` | 同上 | 100% → **88%** ✗ | 100% → 0% ✓ |
| `CheckBox` | 框内 22×22 平均色 | (66,78,95) 仍深 ✗ | (214,221,237) ✓ |
| `RadioButton` | 同上 | (95,107,124) 仍深 ✗ | (211,219,235) ✓ |
| `Toggle` | 轨道平均色 | (70,155,236) 未变 ✗ | (70,136,221) ✓ |

⚠ `Toggle` 那条最弱（轨道本来就是主题 `accent`，两套主题只差一点点 ✓）⇒ 它的"修前没变"另有一条硬依据：
`set()` 里那次取色是**一次性的**、而当时**根本没有 `applyTheme`** ✓。
⭐ 另记：`CheckBox/RadioButton/Toggle/ProgressBar/RingProgress/ImageView/Calendar/Steps` 的颜色字段**全是 `pri`** ✗
（外部读不到）⇒ 这类控件的自测**只能靠像素**，别指望打印字段 ✓。

### ⛔ `Table`：表头的排序箭头会**说谎** —— 换数据分 `set_rows` / `clear`+`add_row` 两条路（2026-10-13 实测 + 已修 ✓）

**症状**：点表头把「评分」排成降序（表头出现 `▼`），随后**换一批数据**（搜索 / 换筛选 / 刷新）
⇒ 行变回**插入序**（实测：9.3 掉到第 3 行、首行 7.7 ✗），可表头 `▼` **照旧显示** ⇒ 界面上写着"已按评分降序"，
数据却不服 ⇒ 这是最坑的一类：**界面说谎**，用户会以为数据错了 ✗。

**病因**：排序状态 `sortCol/sortDesc` 只在 `set()` 里被重置，`set_rows()` / `clear()` / `add_row()`
**都不碰它们** ⇒ 箭头（看 `sortCol`）与数据（新的插入序）分道扬镳。

**修法（已落库 ✓）**：口径是"**要么别谎报、要么别乱序**"——
- `set_rows()`（**批量**换数据）⇒ **重放**当前排序（保住用户意图 ✓）
- `clear()` / `add_row()`（改"行集合"）⇒ **撤销**排序状态（箭头老老实实消失 ✓）
⇒ 应用侧换数据**优先用 `set_rows`**（一次批量、还保住排序 ✓）；逐行 `add_row` 会**丢排序** ✓（预期行为 ✓）

**验收**：`WALL_VIEW=1 WALL_TSORT=3 WALL_TFILTER=1`（先按评分降序，再换一次数据）
⇒ 日志两行都应是 `首行 光影百年 评分=9.3` ✓（修前第二行是 7.7 ✗）；
截图里 `评分 ▼` 那一列应严格递减 ✓

### ⛔ 文本会变的 `Label`：布局里必须给 `basis`（或 `grow`），否则 `set_text` 的文字"溢出去被裁"（2026-10-13 实测 ✓）

**症状**：状态栏 / 角标那种"先建**空** Label、之后 `set_text` 填内容"的写法 —— 文字**不会把盒子撑大**，
而是从盒子里**溢出去**：靠窗口右边的那一项只剩开头两个字（实测：`默认排序 · 分类：全部` 只显示 `默…` ✗）。

**病因**：布局容器按"**加入时**的自然宽"给盒子定尺寸（`HBox.add` 会把 `_natW/_natH` 缓存下来）；
空文本的自然宽 = 0 ⇒ 盒子宽 ≈ 0；之后 `set_text` 只改了控件自己的 `_w`，**布局不会把那个盒子重排** ✗
⇒ 文字的起点仍在那个 0 宽盒子处，往右画就出了窗口（看着像"文字被窗口切了"，其实是盒子没宽 ✓）。

**怎么办**（都在"建 / 加"的时候把盒子定死，三选一）：
- 右侧固定项：`bar.add(lbl, {grow: 0, shrink: 1, basis: 300, align: 1})` —— **给显式 `basis`** ✓（推荐）
- 左侧占满项：`bar.add(lbl, {grow: 1, shrink: 1, align: 1})` —— 让它**撑满**、文字左对齐 ✓
- 建的时候就带**初始文本**（`createLabel({text: "共 0 部"})`）⇒ 自然宽不为 0 ✓
- ⚠ 同一行里要让左右两项都保持自然宽：中间插一个 `grow: 1` 的**空标签当撑位**（`gapLbl`）✓

### ⛔ 列表/下拉/菜单的"行高亮"：圆角要**锚在面板边上**（否则底角冒方块 + 文字出框 ✓ 2026-10-13 实测）

`sdl_combobox` 弹层 / `sdl_listbox` / `sdl_menu` 下拉 三个控件原来**各抄了一份**"行底色"画法：
`atTop` → 圆角 + 补行底两角；`atBottom` → 圆角 + 补行顶两角。看着对，但**圆角锚错了边** ✗：

- 面板底边**通常不落在**行边界上：弹层高 200、行高 26 ⇒ 最后一行只露 18px，
  它的"底边"在面板底边**下方 8px** ⇒ 照它自己补的圆角也落在下面 ⇒
  面板真正的两个底角仍是**方角**、被这行整行铺满：症状 =「面板左下/右下各冒出一小块底色方块」
  （实测像素：面板底行整行铺到边框线上，左下/右下各约 6×4px 方角色块 ✗）
- 同族第二处：内容裁剪矩形写成 `_int(ph) + 1`（本意是"覆盖浮点截断的亚像素"），
  它比面板真身**多 1px** ⇒ 末行（常只露一半）的**文字下沿**漏到面板/边框**外面** 1px ✗
  （症状：「字体下沿有一丢丢出框」✓）

**怎么办**（已收敛成**单一实现** ✓，别再各写一份 ✗）：

- 行底色一律走 `Renderer.fillRowInPanel(px, py, pw, ph, radius, rx, ry, rw, rh)` ——
  只填 **(行矩形 ∩ 面板圆角矩形)**，圆角**锚在面板的上/下边**上 ✓（首行/末行/只露半行都对 ✓）
- 裁剪矩形要**正好等于面板**：`clip.w = _int(maths.ceil(px + pw)) - _int(px)`（`h` 同理 ✓）
- 验证件：`build/_probe_combo_corner.leno`（行高 26 / 弹层高 200 / 12 项 ⇒ 必出"只露一半"的末行 ✓）
  `set CSHOT=%TEMP%\combo.png & build\leno.exe build\_probe_combo_corner.leno`；
  再加 `set CSEL=7` 把"只露一半的末行"设成**选中** ⇒ 压高亮行的形状 ✓

### ✅（原 ⛔，**2026-10-13 已修**）在 A 的**方法**里写 `new B(键=值)`：值里的裸字段名曾报"未定义变量"

**现在直接写就行** ✓：`var b = new BadgeSpec(text = badge, …)`（`badge` 是 **Card 的字段**）✓ ——
本条**不再需要任何绕法**（下面那些是历史记录，保留是为了让老代码 / 老笔记对得上 ✓）。

**历史上的症状**：`Card` 里构造 `BadgeSpec` 时实测 `new BadgeSpec(text = badge, …)`
⇒ 编译器报 `未定义的变量: badge` / `字段 'text' 类型不匹配: 期望 'string'，实际 'unknown'`；
当时的改法有两条（值里写显式 `self.badge` ✓，或先 `new BadgeSpec()` 再逐字段赋值 ✓）。

**真因（2026-10-13 查实，与早先的猜测都不同 ✗）**：**不是**"拿不到外层方法的 self" ——
是**方法体改写器的 switch 白名单漏了 `AST_STRUCT_INIT`**（`src/semantic/semantic_visit_method.c`）：
`new B(键=值)` 的**值**整段被 `default:` **静默跳过** ⇒ 值里的裸字段名没被改写成 `self.字段`
⇒ 后面只按**变量**解析 ⇒ 报"未定义变量" ✗。

⚠ 旧说法"它把当前结构体上下文当成 `BadgeSpec` 了 / `self` 被切成 B"**不准确** ✗：
`B` 若有**同名字段**，同样**响亮报错**、**不会**静默取到 B 的值 ✓。
完整边界表（6 条，含"取外层而不是 B 的同名值"这条防静默错值的钉子）与真因见
`docs/语言摩擦与隐式契约清单.md` 的 **M11**（✅ 已修）/ `docs/语言陷阱速查.md` ㉟ ✓

### ✅ 卡片里别留"每帧测量"

行高与省略号结果都带缓存（两槽：标题/副标题各一 ✓）⇒ 文本与字号不变时，**默认左对齐**路径
每帧 `measureString` = 0 ✓。反过来：把省略号缓存写成**单槽**，标题与副标题会互相踢缓存、
每帧各跑一次二分 ✗（v1 就是单槽 ✓ 已修）。

### ⛔ 光标"**按下去变回箭头**"：`markFrameRedraw()` 把"要重绘"当成了"鼠标动过"（2026-10-13 实测 + 已修 ✓）

**症状**：鼠标移到按钮上是**手型** ✓；**一按下去**（还没松开）光标**变回普通箭头** ✗；
再动一下又变回手型 ⇒ 看着像"按下态把悬停丢了"。

**病根**（集中在 `sdl_core.leno` 的"帧级光标决策"）：`_frameCursorSeen` 本该表示
"**本帧要重新决定光标**"，但 `markFrameRedraw()` **也**把它置位 ✗ —— 而那是"**要重绘**"，
与"鼠标位置变了"是**两件事** ✗。于是**按下 / 松开 / 键盘激活 / 任何 `set_xxx`** 触发的重绘帧
都被当成"要重算光标"；可**控件只在 mouse motion 里请求光标** ⇒ 那一刻**没有任何请求** ⇒
`applyFrameCursor` 走到「**无请求 ⇒ `DEFAULT`**」兜底 ⇒ **光标被打回箭头** ✗。

**范围**（不是 Button 一家）：凡"**只在 motion 里请求光标**"的控件全中 ——
Button / Card / CheckBox / Toggle / Radio / Edit / ImageView / LinkButton / Progress / Spinbox /
Tab / TreeView …；**键盘 Enter 激活**、**`set_xxx` 触发的重绘**同样会打回箭头 ✓。

**修法（核心两处，控件一行未改 ✓）**：

1. `markFrameRedraw()` **只**请求重绘，**不再**置 "seen" ✓；
2. 由**窗口**在派发 motion 事件时统一登记（`core.markFrameMotion()` ✓）——
   判据唯一：**只有真的收到 motion 事件**才算"鼠标位置变了" ✓；
3. `requestCursor()` **自己**置 seen ⇒ "只在拖拽里请求"的控件（`Table` 列宽分割线 / `Splitter`）
   不依赖 motion 也能被应用 ✓。

**判据（无人值守）**：`leno_gui/自测/test_cursor_press.leno` —— 注入真事件
（`SDL3.testMoveAt` / `testMouseAt`）+ 读 `SDL3.testLastAppliedCursor()`：
① 移上去 ⇒ `POINTER(11)`；② **按下（无 motion）⇒ 仍是 11**；③ 松开 ⇒ 11；
④ 移开 ⇒ `DEFAULT(0)`（**反向对照** —— 防止"一律沿用"的假修 ✓）。
实测：**修好前 ②③ 当场红（`got=0`）**、修好后 4/4 ✓ ⇒ 用例确实能抓住这个 bug ✓

⚠ **自查口径**：**"本帧要重绘" ≠ "本帧鼠标动过"**。任何"集中式状态决策 + 各处 `mark*()` 打标记"
的机制，都要检查它有没有被**别的语义**借用 —— 这一例就是被"重绘请求"借用了 ✓
（同族的还有：`markFrameMotion()` 曾被当成"随便打个标记"，实际它决定要不要重算光标 ✓）。

**同族审计：其他控件有没有同样的问题？（2026-10-13 逐个查完 ✓）**

修在核心 ⇒ 该类问题**一次全消**；在此之上又按"写法特征"把全库控件扫了一遍：

| 查什么 | 结论 |
|---|---|
| 谁**直接** `setSystemCursor()`（绕过集中式决策 ✗） | 只有 `core` 自己与 `SDL3` 门面的转发 ✓（`sdl_image` 那处是**注释** ✓） |
| 有 `cursor` 字段却**从不请求**光标 | 无 ✓（每个可点击控件都有 `requestCursor` ✓） |
| `markFrameMotion()` 是否被**非 motion** 场景调用（会把 bug 带回来 ✗） | 全部 19 处**都在 motion 守卫内** ✓（`label` / `ring` / `listbox` / `menu` / `canvas` / `combobox` / `progress` / `splitter` / `tab` / `table` / `button` / `card` / `edit` / `imageview` / `linkbutton` / `spinbox` / `treeview` / `calendar` ✓） |
| `requestCursor()` 是否都**被 hover/命中守卫**（无条件请求会**劫持**别人 ✗） | 全部有守卫 ✓ |

⇒ **没有第二个环节有同类漏洞** ✓。但审计**顺带查出 3 处真不一致**（已修）：

- **`CheckBox` / `Toggle` / `RadioButton` 的默认光标是 `DEFAULT`（箭头）** ✗ ——
  它们和 `Button` 一样是**可点击**控件，悬停却是箭头 ⇒ 现统一改成 `Pointer` ✓
  （不想手型就传 `cursor: SystemCursor.DEFAULT` ✓）。
  ⚠ 同一轮核对：其余控件默认都是 `Pointer`（`canvas` = `Crosshair`、`imageview` = `-1`「不设」，
  均**有意** ✓）；`progress` 默认 `Pointer`，但它默认 `clickable: false`（只读）⇒
  只读时**不会**进 hover 分支、也就不会请求 ✓（**不是** bug：只读控件本来不该有手型 ✓）。

**实测判据**：`leno_gui/自测/test_cursor_press_multi.leno` —— **10 类控件**（Button / CheckBox /
Toggle / RadioButton / Slider / ProgressBar / ListBox / Edit / LinkButton / ComboBox）逐个
"悬停 → 按下 → 松开 → 移开"，**41 条断言全过** ✓（`Edit` 额外钉"悬停 = 文本光标 `TEXT(1)`" ✓）。

**写这类用例踩的两个坑**（会造出**假失败**，记下来 ✓）：

1. **自量控件不能传 `w`/`h`** —— `CheckBox` / `Toggle` / `RadioButton` / `LinkButton` **忽略**它们
   （还会报「不认识的选项」✗）⇒ 按声明坐标去点会**打偏**、断言假红 ✗；
   正解：落点取**真实几何**（`get_x/get_y/get_w/get_h` ✓）。
2. **会弹层的控件（`ComboBox`）要放最后测** —— 测它时那一下"按下"会**把它点开** ✗，
   弹出的下拉面板盖住下方控件 ⇒ **它合法地继续请求手型**（`applyFrameCursor` 取**最大 kind** ✓），
   后面控件的断言就被"别人的弹层"污染了 ✓（实测：`Edit` 因此拿到 `11` 而不是 `1` ✗ —— 查了半天
   才发现是**用例的锅**，不是框架的 ✓）。

---

## 附录 D：选项命名统一（2026-10-13 —— 审计 + 改名表机制 + 四个仓库一起迁完 ✓）

### 现状审计（全库 36 个控件 / 724 个键位 / **297 个不重名**）

**底子是好的** —— 核心词汇高度一致，前 14 名全是共用名 ✓：
`x`(30) · `y`(30) · `visible`(29) · `w`(25) · `h`(24) · `enabled`(23) · `font_size`(22) ·
`bg_color`(19) · `border_color`(18) · `cursor`(17) · `tip`(17) · `font_path`(17) ·
`text_color`(16) · `font_style`(15)
⇒ **"背景色"这一族本来就是一致的**（`bg_color` 被 19 个控件采用 ✓；语义色后缀统一为 `*_color` ✓）。

**三类不一致，只动两类**：

| 类别 | 例子 | 处置 |
|---|---|---|
| ① 风格笔误（camelCase 混入） | `fontSize`(popmenu) / `padX`·`padY`(gridlayout、layout) / `imageMode`·`imageExpanded`(treeview) | ✅ **本轮改掉**（唯一没有争议的一类 ✓） |
| ② 同概念不同名 | `h` vs `height`(titlebar) / `sel_color` vs `selected_color`(card) / `sep_color` vs `separator_color` vs `divider_color` / `head_*` vs `header_*` / `item_h` vs `row_h` | ✅ **同日第二轮处理完**（逐组判"是不是刻意的" ⇒ 见下面的第二轮小节 ✓） |
| ③ 语义色名各指各的东西 | `fill_color` / `track_color` / `bar_color` / `thumb_color` / `on_color` / `off_color`… | ❌ **不动**：统一成一个通用名会把"哪个色是哪个"的信息丢掉 ✗；要统一的是**后缀**，而后缀已经一致 ✓ |

### 本轮的机制：**改名表（过渡用 —— 当天已撤）**，过程比结论更值得记 ✓

改名的**最大风险**是"用户代码**悄悄**失效" ✗ ⇒ 先加了一张 **改名表**（在 `sdl_opts`）：
**旧名仍可用 + 每次用到都警告** ⇒ **警告清零 = 迁移完成**，**可验证** ✓
—— 靠它把四个仓库**一次迁完**（52 + 7 + 1 + 2 个文件 ✓），不必"改完再靠人肉翻代码找残留" ✓

```leno
// 当时的形状（现已撤 ✓）
Array[string] _renamed_old = ["fontSize", "padX", "padY", "imageMode", "imageExpanded"]
Array[string] _renamed_new = ["font_size", "pad_x", "pad_y", "image_mode", "image_expanded"]
```

**★ 随后决定「旧名全部放弃」⇒ 这张表当天就撤了** ✓（连同各控件里旧名的**别名读取**与**白名单项** ✓）：

- 四个仓库**已经一个旧名使用者都没有**了 ⇒ "旧名仍可用"这句反而成了**假话** ✗
  （表还在、却没有任何控件再读旧名 ✓）—— 文档与代码**不一致**比"少一层兼容"更糟 ✓
- **发现能力并没有丢**：传旧名现在走"**不认识的选项 + 你是不是想要 `pad_x`？**" ✓
- ⚠ 代价说清：这等于**放弃对外兼容** —— 本仓那三个应用都是自家仓库、一起迁 ✓；
  若以后有第三方使用者，**改名前必须先把这张表加回来**（它当初就是为此设计的 ✓）

**两条纪律（只要再动命名就仍然适用 ✓）**：

1. **改名表必须在 `known` 判断之前查** —— 旧名还在白名单里（控件还在读它）⇒ 放后面就**永远不报** ✗
2. **表里有的名，所有读它的控件都必须同时认新名** ✗ ——
   否则会提示一个**根本不生效**的新名（比不提示更糟 ✗）。
   本轮就是这么发现 `sdl_layout.leno` 的 VBox / HBox / AnchorBox **也读** `padX`/`padY` 的 ✓

### 迁移结果（四个仓库一起迁 ✓）

| 仓库 | 改动 | 验收 |
|---|---|---|
| `Leno`（含 `build/leno_module/LenoSDL3`） | 52 个文件 | 含新名的 **99 个文件逐个真编译：0 失败** ✓；断言套件 **452 passed / 0 failed** ✓ |
| `LenoMusic` / `LenoGame` / `LenoTrae` | 各若干 | **32 个入口逐个真编译：0 失败、0 条旧名警告** ✓ |

**迁移铁律**：只动**字典键位置**（`padY: 8`）与**字符串形式**（`"padY"`）；
**字段名 / 局部变量 / 内部存储一律不动** ✓（`float padY = 8.0`、`2.0 * padY` 照旧 ✓）——
它们不是"暴露给用户的字典名"，改了只徒增风险 ✗。
**刻意保留**旧名的地方只有三处：改名表本身 / 读该键的 5 个控件的别名读取与白名单 / 钉住该机制的用例 ✓

### 命名规约（新控件照这个写 ✓）

- **颜色**：一律 `*_color` ✓（`bg_color` / `text_color` / `border_color` / `hover_color` /
  `pressed_color`；语义色也 `*_color`：`track_color` / `fill_color` / `thumb_color` ✓）
- **尺寸**：短名 `x` / `y` / `w` / `h` ✓（**不要**新造 `width` / `height` ✗）；
  内边距 `pad_x` / `pad_y` / `gap` / `item_gap` ✓（**不要**驼峰 `padX` ✗）
- **开关**：`show_*` ✓（`show_header` / `show_text` / `show_grid` / `show_lines` / `show_check` ✓）
- **字号/字体**：`font_size` / `font_path` / `font_style` ✓（**不要** `fontSize` ✗）
- **回调**：走 `on_xxx()` **方法**，**不进 opts** ✓（`btn.on_click(func(){…})` ✓）
- **旧名一律不留**（2026-10-13 定 ✓）：四个仓库一次迁完 ⇒ 别名读取与白名单项**一并删掉** ✓
  （"旧名仍可用"若还留着就是**假话** ✗）；改名表是**过渡件**、迁完即撤 ✓
- **以后再要改名时**：**先把改名表加回来**（保证"零破坏 + 迁移可验证" ✓），
  让**所有**读该键的控件同时认新名，迁完、警告清零后再撤 ✓
- **状态色一族**（多状态控件）：`{状态}_color`（底）+ `{状态}_text_color`（文字）+
  `{状态}_border_color`（边）✓（`pending_color` / `alert_text_color` / `today_border_color` ✓）

---

## 附录 E：选项命名统一 · 第二轮（第②类"同概念不同名"，2026-10-13 同日 ✓）

第一轮只清掉了 5 个 camelCase 笔误（附录 D ✓）；第二轮逐组判"**是不是刻意的**"，**只统一真同概念的**：

### 统一的 9 组

| 概念 | 统一为 | 改掉 |
|---|---|---|
| 高 | `h` | titlebar 的 `height` |
| 分隔色 / 宽 | `sep_color` / `sep_width` | titlebar `separator_color`；splitter `divider_color` / `divider_width` |
| 线宽后缀 | `*_w` | canvas `border_width` → `border_w` |
| 表头 | `header_*` | calendar `head_h` / `head_text_color` / `show_head` |
| 状态色 | `{状态}_color` + `{状态}_text_color` | calendar `plain_bg/fg`、`filled_bg/fg`、`alert_bg/fg`、`today_bg/fg`、`today_border`→`today_border_color`；steps `pending_bg`→`pending_color`、`pending_border`→`pending_border_color`、`pending_text`→`pending_text_color` |
| 选中色 | `sel_color` | card `selected_color` |
| 行高 | `item_h` | table / treeview 的 `row_h` |
| 别名 | 删掉 | table 的 `multi`（`multi_select` 的旧别名 ✓） |

### 刻意保留（不同对象 / 不同概念 ⇒ **不统一** ✓）

| 名 | 在哪 | 为什么保留 |
|---|---|---|
| `active_color` / `active_text_color` | tab | 页签的"**激活页**"是独立概念（配套 `active` / `set_active()` ✓），与"选中行"不同 |
| `separator` | menu（**菜单项**级） | 它是"**这一项就是分隔线**"的**项种类**标记，不是开关 ⇒ 改成 `show_*` 反而错 ✗ |
| `size` | ring · spinner | 圆环/转子的**形状尺寸**（不是 `w`/`h` 两个方向 ✓） |
| `thickness` | ring · separator · spinner | 环粗 / 线粗 —— 对象各不相同；且分隔线横竖都算"厚" ⇒ 比 `line_w` 更准 ✓ |
| `line_w` | steps | 连接线宽度 ✓（`_w` 后缀本就合规 ✓） |

### ⚠ 本轮踩到并纠正的**自己的错**（最值得记的一条 ✓）

calendar 的颜色是 `{状态}_{bg,fg}` **矩阵**（plain / filled / alert / today × 背景·文字 + `today_border`）。
第一遍我**只改了一半**（`plain_*` / `alert_*` ✗，`filled_*` / `today_*` 没动 ✗）——
**那比不改更糟** ✗：同一族里出现两种写法，看图的人再也猜不出规律 ✓。
⇒ 规矩：**一族要么整族不动、要么整族改完** ✓。本轮选了**改完**
（`{状态}_color` / `{状态}_text_color` / `{状态}_border_color` ✓，与 `header_color`/`header_text_color`、
以及 steps 的 `pending_*_color` 完全一致 ✓）。

### 验收（本轮）

| 检查 | 结果 |
|---|---|
| 全仓旧名（**键位置**）残留 | **归零** ✓（四个仓库的 `*.leno` 逐个扫 ✓） |
| **GUI 全部 107 个文件**逐个真编译 | **0 失败、0 选项警告** ✓（用户要求"重点跑 GUI" ✓） |
| 框架 `examples` 56 文件 + 三个应用 **32 个入口** | **0 失败、0 警告** ✓ |
| 断言套件 | **452 passed / 0 failed** ✓ |
| `leno_gui/自测` **19 个用例逐个真跑** | 全 0 ✓ |

⚠ 其中 `test_chart_labels.leno` 在批量跑时看着"超时" —— 查实是**它读的环境变量是 `PROBE_SHOT` 而不是
`SHOT`** ✗ ⇒ 不给它那个变量就**不会自动退出**（设计如此：等人看窗口 ✓）⇒ **不是回归** ✓；
补上 `PROBE_SHOT` 后 `exit=0` ✓。批量跑自测时**每条的 SHOT 变量名要单独确认** ✓。

⚠ 迁移铁律同第一轮：只动**字典键位置**，字段/局部变量一律不动 ✓；
`height` 这种**常用词**必须**限定作用域**（只改 `createTitleBar(...)` 调用内部 ✓）——
否则会误伤 HTML 字符串里的 `height:` 和普通 dict ✓（本轮实测有这个假阳性 ✓）。
