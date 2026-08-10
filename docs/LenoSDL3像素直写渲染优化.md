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
    fill(r,g,b,a)       // 整块填充（for 循环，性能优于 while）
    fillCopy(src)       // 整块拷贝（memcpy）
    free()              // 释放内存
}
```

`Renderer` 新增方法 `blitBuffer(PixelBuffer buf, dx, dy, dw, dh)`：
- 内部懒创建/复用流式纹理（尺寸匹配时零分配）
- 一次 `updateTexture` + `drawTextureAt` 上传绘制

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
- `leno_module/LenoSDL3/examples/特效动画/fireworks.leno` — 内联 ffi 版
- `leno_module/LenoSDL3/examples/特效动画/fireworks_pixelbuf.leno` — PixelBuffer 封装版

## 记录时间

2026-08-10
