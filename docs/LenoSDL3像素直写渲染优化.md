# LenoSDL3 像素直写渲染优化

## 背景

`fireworks.leno`（烟花动画）用 Leno 实现时，**性能反而比 Python 版慢**，与"Leno 整体应比 Python 快"的预期相悖。本文记录问题定位、优化方案与最终结果。

## 问题定位

原始实现用 `Renderer.setColor(r,g,b,a)` + `Renderer.drawPoint(x,y)` 逐点绘制粒子：

- **`drawPoint` 每次调用内部都执行 `_flushBatches()` + `SDL_RenderPoint`**，每次都是一次 SDL 渲染管线状态刷新。
- 单帧最多 **6000 个粒子** = 6000 次 `SDL_RenderPoint` + 大量 `SDL_SetRenderDrawColor`，CPU 被渲染调用开销绑死。
- 而 Python 的 `pygame.Surface.set_at` 是**直接写帧缓冲内存**（无状态切换、无 SDL 调用），所以 Python 反而快。

**根因**：不是 Leno 慢，而是 `drawPoint` 的逐点渲染走的是"每次 flush + 进 SDL 管线"的最慢路径。

## 优化方案：像素直写 + 整帧一次上传

对标 Python `set_at`，改为**自己管理一块像素缓冲，逐点写内存，整帧只上传一次纹理**：

1. `ffi.malloc` 分配 `W*H*4` 字节像素缓冲 `pixBuf`（全程复用，零分配）。
2. 预生成静态背景缓冲 `bgBuf`，每帧用 `ffi.memcpy(pixBuf, bgBuf, W*H*4)` 整块清屏（C 级 memcpy，约 0.1ms）。
3. 星空/火箭/粒子全部 `ffi.write_int(pixBuf, (y*W+x)*4, color)` 直写像素（纯内存写，无 SDL 调用）。
4. 整帧用流式纹理一次上传绘制：`ren.createTexture(RGBA8888, STREAMING, W, H)` → `ren.updateTexture(tex, null, pixBuf, pitch)` → `ren.drawTextureAt(tex, 0, 0, W, H)`。

## 关键坑：像素格式字节序

SDL3 的 `SDL_PIXELFORMAT_RGBA8888` 在 **little-endian（x86）上内存字节序是 `[A,B,G,R]`**，即一个像素的 u32 = `(r<<24)|(g<<16)|(b<<8)|a`。

> ⚠️ 曾误用 `(a<<24)|(b<<16)|(g<<8)|r`（对应 [R,G,B,A]），导致整屏偏红；改成 `BGRA8888` 又变蓝。**正确编码必须是 `(r<<24)|(g<<16)|(b<<8)|a`**。

## 性能结果

| 指标 | 原 drawPoint 版 | 像素直写内联版 | 像素直写封装版 |
|------|----------------|---------------|---------------|
| 整体平均 FPS | ~25 | **155.5** | 137.4 |
| 最高 FPS | — | **414** | 465 |
| 5000+ 粒子 FPS | ~23 | ~88 | ~85 |
| 单帧耗时 | ~40ms | **6.4ms** | 7.3ms |

- **像素直写相对 drawPoint 提升约 6 倍**。
- 对比 Python 版（30~60 fps），Leno 提升约 **2.5~4 倍**。

## 封装：PixelBuffer struct

将像素直写封装为 `PixelBuffer`，收口内存管理与字节序，避免再次踩坑：

`leno_module/LenoSDL3/lib/sdl_renderer.leno`

```
export struct PixelBuffer {
    Ptr[u8] pixels; int w, h, pitch; bool ok
    init(w, h)          // 分配像素内存
    setPixel(x,y,r,g,b,a) // 单点写入（越界安全，自动字节序）
    getR(x,y)           // 读回 R 通道（常用于读亮度掩码）
    fill(r,g,b,a)       // 整块填充（全 0 走 memset 快路径）
    clear()             // 整块清零（memset，极快）
    fillCopy(src)       // 整块拷贝（memcpy）
    free()              // 释放内存
}
```

`Renderer` 新增方法 `blitBuffer(PixelBuffer buf, dx, dy, dw, dh)`：
- 内部懒创建/复用流式纹理（尺寸匹配时零分配）
- 一次 `updateTexture` + `drawTextureAt` 上传绘制
- **纹理设为 BLEND**：透明像素可透出底下内容，支持"字符/特效缓冲叠加"场景

## 案例二：matrix_rain 矩阵字符雨（18 → 51 fps）

`matrix_rain.leno` 原本每帧用 `drawText` 画 3×N 列个**随机字符**（约 333 次），每次触发 `TTF_RenderText_Blended` CPU 栅格化 + 纹理创建，仅 18fps。

优化（利用 PixelBuffer 像素直写）：

1. **字符掩码预渲染**：字符集（ASCII/KANA 共 ~97 个）初始化时用 `renderTextBlended` 栅格化**一次**，把每个字符的 alpha 提成灰度掩码存进 PixelBuffer（R 通道）。之后每帧不再触发任何 TTF 栅格化。
2. **拖尾走 GPU**：全屏逐像素 FFI 衰减（420 万次调用/帧）→ `ren.fillRect` 半透明黑（1 次 GPU 调用）。
3. **字符缓冲透明叠加**：字符画进透明 PixelBuffer（`clear()` memset 清零），`blitBuffer` 叠加在 fillRect 拖尾之上。
4. **字符写入改直接 ffi 字节操作**：从 `getR`+`setPixel` 方法调用 → 直接 `ffi.read_byte` 读亮度 + `ffi.write_byte` 写 G/A 字节（绿色只需写 G 通道），绕过方法调用开销。

| 阶段 | 做法 | FPS |
|------|------|-----|
| 原始 | 每帧 333 次 `drawText` 随机字符 | 18 |
### 完整的逐步优化历程（每一步的教训）

| 版本 | 做法 | FPS | 关键教训 |
|------|------|-----|---------|
| v0 原始 | 每帧 333 次 `drawText` 随机字符（TTF 栅格化+纹理创建） | 18 | 实时栅格化是灾难 |
| v1 | 像素直写 + **全屏逐像素 FFI 衰减**拖尾 | 更卡（误伤） | 700000 像素逐像素 `read_byte/write_byte` 是 420 万次 FFI/帧，比 drawText 还慢 |
| v2 | GPU `fillRect` 半透明黑拖尾 + 透明字符缓冲叠加 | 38 | 拖尾必须交给 GPU，不能 CPU 逐像素衰减 |
| v3 | `blitGlyph` 改**直接 ffi 字节操作**（只写 G/A 字节） | 51 | 热点路径避免 `setPixel/getR` 方法调用开销 |
| v4 | **`setVSync(0)` 关闭垂直同步** | 51→**172** | **vsync 锁帧把帧率锁死到 ~50**，是最大隐藏瓶颈 |
| v5 | **整行 `ffi.memcpy`** 拷贝字符掩码（替代逐像素字节操作）+ 右边界越界裁剪 | **175** | memcpy 一次 FFI 拷整行，FFI 调用从 ~10 万降到 ~3000 次 |

**最终结果**：
```
总帧数: 2051  峰值绘制字符数: 311
整体平均 FPS: 175.5  平均 5.6ms/帧
高负载（250-299字符）: 173 fps
```

**对比 Python**：Python（实时 `font.render` + GPU `blit`）约 131fps，Leno 优化后 **175fps，反超 ~1.3 倍**。

**经验**：
- **`setVSync(1)` 是最大隐藏瓶颈**——垂直同步把帧率锁死到显示器刷新率（约 50-60fps），无论绘制多快都上不去。**性能测试/特效场景务必 `setVSync(0)`**。
- **矩阵雨/拖尾类特效不适合"纯像素直写全屏衰减"**（700000 像素逐像素衰减太慢），应把拖尾交给 GPU `fillRect` 半透明黑。
- 字符类特效应**预渲染字符集为掩码**，运行时只做内存拷贝/字节写入，绝不实时栅格化。
- 热点路径应**直接 ffi 字节操作**，避免 `setPixel` 等方法的参数绑定+越界检查开销。
- **批量操作优先于逐点**：整行 `ffi.memcpy` 拷贝字符掩码，比逐像素 `read_byte/write_byte` 快一个量级（FFI 调用数下降 30 倍以上）。
- **`ffi.memcpy` 的指针偏移用 `ffi.offset(ptr, byteOffset)`**，不能用 `ptr + int`（Leno 的 Ptr 不支持直接算术）。
- **memcpy 必须做边界裁剪**（右边缘字符可能超出窗口，memcpy 越界写会破坏堆，触发"堆缓冲区溢出"运行时错误）。

## 对比版本

| 文件 | 版本 | 特点 |
|------|------|------|
| `fireworks.leno` | 内联 ffi 版 | 手写 `ffi.write_int`，性能最高（155 fps） |
| `fireworks_pixelbuf.leno` | PixelBuffer 封装版 | 代码清晰、可复用（137 fps），方法调用有少量开销 |

**取舍**：封装版引入方法调用开销（~13%），但换来清晰 API 与零字节序风险。需要极致性能用内联版，需要可维护性用封装版。

## 经验总结

1. **`Renderer.drawPoint` 是逐点立即提交**（`_flushBatches()` + `SDL_RenderPoint`），海量逐点绘制时是性能瓶颈，绝不能用它画粒子/分形/画板。
2. **逐点异色绘制的正确路径是"像素缓冲直写 + 整帧一次上传"**，对标 Python 的 `Surface.set_at`。
3. **SDL3 像素格式字节序反直觉**：`RGBA8888` 在 little-endian 内存里是 `[A,B,G,R]`（u32 = `(r<<24)|(g<<16)|(b<<8)|a`），易踩坑，建议封装成 `PixelBuffer.setPixel` 收口。
4. **静态背景用 memcpy 清屏**，比逐点循环填充快一个量级。
5. **流式纹理全程复用**（`createTexture` 一次 + 每帧 `updateTexture`），避免每帧创建/销毁纹理的分配开销。

## 涉及文件

- `leno_module/LenoSDL3/lib/sdl_renderer.leno` — 新增 `PixelBuffer` struct、`Renderer.blitBuffer` 方法
- `leno_module/LenoSDL3/examples/特效动画/fireworks.leno` — 烟花内联 ffi 版
- `leno_module/LenoSDL3/examples/特效动画/fireworks_pixelbuf.leno` — 烟花 PixelBuffer 封装版
- `leno_module/LenoSDL3/examples/特效动画/matrix_rain.leno` — 矩阵字符雨（像素直写优化）
- `leno_module/LenoSDL3/examples/特效动画/fireworks_run.leno` — 烟花 win.run 回调版

## 记录时间

2026-08-10
