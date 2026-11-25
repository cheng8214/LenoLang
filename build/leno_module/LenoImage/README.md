# LenoImage

纯 Leno 图像处理（**零原生依赖，不碰 SDL3**）：像素读写 + 缩放 + 裁剪/翻转/旋转 + PNG 编解码。

```leno
import "Image" as img
use img.Image

// 构造 + 读写（对外 RGBA 序；内部 BGRA，与 SDL3 surface 同内存序）
Image t = img.newImage(64, 64)
t.set(0, 0, 255, 0, 0, 255)      // 红
t.fill(0, 255, 0, 255)           // 全绿

// 缩放：缩小走 box、放大走 bilinear、像素画走 nearest
Image s = img.scaleToLongEdge(t, 256, false)
Image p = img.resize(t, 32, 32, img.MODE_NEAREST)

// 几何
Image c = img.crop(s, 0, 0, 128, 128)
Image r = img.rotate90(c)

// PNG 编解码（纯 Leno）
var e = img.encodePNG(t)         // Result[string]
if e.isOk() { files.write("out.png", e.val) }
var d = img.decodePNGFile("in.png")
```

## API

| 函数 | 说明 |
|---|---|
| `newImage(w, h)` / `fromPixels(w, h, px)` | 构造（`px` 长度必须 = w·h·4） |
| `clone(img)` | 深拷贝（在原图上画又不想毁原图时先克隆） |
| `Image.set/get/getA/setA` | 单点读写（RGBA 序；越界抛错） |
| `Image.fill(r,g,b,a)` / `fillRect(x0,y0,x1,y1,…)` | 全图 / 矩形填充 |
| `resize(src, dw, dh, mode)` | 按采样方式缩放（`MODE_NEAREST/BOX/BILINEAR`） |
| `resizeAuto(src, dw, dh, pxArt)` | 缩小→box、放大→bilinear；`pxArt` 一律 nearest |
| `scaleToLongEdge(src, longEdge, pxArt)` | 长边等比缩放 |
| `scaleToFit(src, maxW, maxH, pxArt)` | 等比装进 `maxW×maxH` 的框（**只缩不放**） |
| `crop(src, x, y, w, h)` / `flipH` / `flipV` | 裁剪 / 翻转 |
| `rotate90` / `rotate180` / `rotate270` | 像素完美旋转（90/270 宽高互换） |
| `blit(dst, src, x, y)` | 贴图**覆盖**（含 alpha；越界部分自动裁掉） |
| `blitAlpha(dst, src, x, y)` | 贴图**alpha 混合**（source-over，按 src.a） |
| `fillCircle(img, cx, cy, r, r,g,b,a)` | 实心圆（原地画） |
| `drawRect(img, x0,y0,x1,y1, …)` | 矩形**描边**（填充用 `Image.fillRect`） |
| `line(img, x0,y0,x1,y1, …)` | 直线（Bresenham，八方向） |
| `toGray(img)` / `invert(img)` | 灰度（BT.601，保留 alpha）/ 反色（返回新图） |
| `decodePNG(data)` / `encodePNG(img)` | PNG 编解码（返回 `Result`）；编码**逐行自适应滤波** |
| `encodePNGWithFilter(img, f)` | 指定滤波（0=None 1=Sub 2=Up 3=Average 4=Paeth，-1=自适应） |
| `encodePNGNoFilter(img)` | 旧行为（恒 None）；体积对照 / 逐字节可预测时用 |
| `decodePNGFile(path)` / `encodePNGFile(img, path)` | 文件版 |

**原地 vs 新图**：`blit/blitAlpha/fillCircle/drawRect/line` 都是**原地改 dst**（拼图/水印/标注的语义）；
`toGray/invert/crop/flip*/rotate*/resize*/scaleTo*` 返回**新图**、不改入参。

**越界策略（故意分两种）**：贴图越界**自动裁掉**（边缘贴半张是常态）；图元坐标**钳到图内**
（画到图外的部分直接不画，不抛错）。

## 口径

- **像素格式 ARGB8888，内存序 B,G,R,A**：与 SDL3 的 `PIXELFORMAT_ARGB8888` surface 一致，
  方便和"读纹理 `readPixelsAt` 的结果"零拷贝互操作；对外 `get/set` 用 RGBA 逻辑序，内部换算。
- **缩放按 alpha 预乘加权**：直接平均非预乘 RGBA 会让透明边缘发黑。
- **PNG 解码**：颜色类型 0/2/4/6、位深 8、非隔行；调色板/位深 16/Adam7 报错（不静默给错图）。
- **PNG 编码**：恒输出 8 位 RGBA（类型 6）；**每行在 None/Sub/Up/Average/Paeth 里自适应挑一个**
  （代价 = 滤波后字节按有符号取绝对值求和，取最小 —— libpng 的默认启发式）。
  实测（同一进程，本机）：

  | 图 | 自适应 | None | 差 |
  |---|---|---|---|
  | 渐变 512×512 | 6.6 KB | 314.7 KB | **−98%** |
  | 绘制图 300×220（演示窗口那张） | 7.1 KB | 87.6 KB | **−92%** |
  | 真截图 1280×800（UI 大片纯色） | 60.3 KB | 53.5 KB | **+12%（反而更大）** |

  ⚠ 最后一行是真实情况：本模块的 zlib 是**固定 Huffman**（没有动态码表），对"大片同色"的
  界面截图，None 留下的长游程反而更好压。所以提供 `encodePNGWithFilter` / `encodePNGNoFilter`：
  对这类图可以两种都编一遍取小的（或者直接指定 None）。照片/渐变/绘制图仍然是自适应明显更省。

## 变更记录

- **0.3.0**（2026-11-25）：PNG 编码改成**逐行自适应滤波**（渐变 −98%、绘制图 −92%，
  但界面截图这类大片纯色的反而 +12% ⇒ 同时给出 `encodePNGWithFilter` / `encodePNGNoFilter`
  自行选择）；编码器不再为每行 5 种滤波各物化一份候选数组（内存/GC 压力大减，快 ~15%）；
  新增绘制/合成 API（`blit` / `blitAlpha` / `fillCircle` / `drawRect` / `line` / `toGray` /
  `invert` / `clone` / `scaleToFit`）；新增可视化演示窗口 + `save` 拼图模式。
- **0.2.0**（2026-11-24）：模块初版（构造/读写/缩放/几何/PNG 编解码）。

## 来源

缩放采样从 `tools/lib/image_ops.leno`（Ptr 版，给 SDL3 零拷贝用）移植成 Array[int] 版；
PNG 编解码借 `LenoCompress` 的 `inflateZlib` / `zlib` / `crc32` 拼块 + 反滤波。

## 可视化演示（看效果，不只看断言）

`leno_gui/应用/图像处理演示/image_lab.leno`：SDL3 窗口里并排六格 ——
绘制（渐变+圆+描边+线）/ 灰度 / 反色 / 旋转 / 真图缩略 / 水印，顶部还打印 PNG 编码的 A/B 体积。

```bat
build\leno.exe leno_gui\应用\图像处理演示\image_lab.leno        :: 开窗口（ESC 退出）
build\leno.exe leno_gui\应用\图像处理演示\image_lab.leno save   :: 不开窗，拼一张 preview.png 落盘
```

`save` 模式产出的就是"这个模块画出来长什么样"的直接证据：
`leno_gui/应用/图像处理演示/preview.png`（3×2 拼图）。

## 跑测试

```bat
build\leno.exe build\leno_module\LenoImage\test\test_image.leno
```
