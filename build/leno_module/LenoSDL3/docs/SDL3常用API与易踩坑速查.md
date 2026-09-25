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

## 六、回归用例怎么跑

目录：`build/leno_module/LenoSDL3/examples/`

```
$env:SDL_VIDEODRIVER='dummy'          # 无头
$env:BSHOT='out.png'                  # 截图输出路径（部分用例需要）
build\leno.exe --no-cache build\leno_module\LenoSDL3\examples\图形绘制\test_antialias.leno
```

| 用例 | 覆盖什么 |
| --- | --- |
| `图形绘制/test_antialias.leno` | 圆角矩形 + 1px 描边环 + 正圆（纯黑白 ⇒ 覆盖率 = (255−R)/255 ✓，边缘有几档灰阶一目了然 ✓） |
| `图形绘制/test_aa_clock.leno` | 大圆 + 斜线指针/刻度（`show_time` 的静态复刻） |
| `图形绘制/test_aa_radii.leno` | `r=10/20/40/80` 圆环 —— 回归「环墨迹压在蒙版纹理末列 ⇒ 整列丢失」那个坑 ✗ |
| `其他测试/test_draw_order.leno` | 实测三个绘制层先后（见第一节 ✓） |
| `其他测试/test_dump_layout.leno` | `dumpLayout` 输出示例（含"有剩余空间但无人 grow"的场景 ✓） |
