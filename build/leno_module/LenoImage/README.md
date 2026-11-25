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
| `decodePNG(data)` / `encodePNG(img)` | PNG 编解码（返回 `Result`） |
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
- **PNG 编码**：恒输出 8 位 RGBA（类型 6），每行 filter=None。

## 来源

缩放采样从 `tools/lib/image_ops.leno`（Ptr 版，给 SDL3 零拷贝用）移植成 Array[int] 版；
PNG 编解码借 `LenoCompress` 的 `inflateZlib` / `zlib` / `crc32` 拼块 + 反滤波。

## 跑测试

```bat
build\leno.exe build\leno_module\LenoImage\test\test_image.leno
```
