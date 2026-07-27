# LenoWin32 桌面截屏功能实现计划

## Summary

为 `d:\CLeno\LenoC\leno_module\LenoWin32\lib` 创建 `w32_capture.leno`，通过 Windows GDI API (user32.dll + gdi32.dll) 实现桌面截屏，返回包含像素数据的 Dict，可配合 SDL3 的 `createSurfaceFromRaw` 创建 SDL Surface 并保存为图片。

## Current State Analysis

- **Win32.leno** 已导入 `w32_capture.leno as cap`，并声明了 `captureScreen()` 转发调用
- **w32_hotkey.leno** 提供了 clib 声明、懒加载 DLL、函数调用的标准模式参考
- **SDL3.leno** 提供 `createSurfaceFromRaw(w, h, fmt, pixels, pitch)` 和 `destroySurface()`，以及 `PIXELFORMAT_BGRA8888` 常量
- FFI API 支持 cstruct (含 u16 字段)、`ffi.malloc`、`ffi.free`、`ffi.write_byte` 等操作
- cstruct 实例传给 clib 参数时自动调用 `to_ptr()` 传指针

## Proposed Changes

### 1. 创建 `d:\CLeno\LenoC\leno_module\LenoWin32\lib\w32_capture.leno`

**核心实现**：声明 user32.dll 和 gdi32.dll 的 Windows API 函数，定义 BITMAPINFOHEADER cstruct，实现截屏逻辑。

**clib 声明**：

```leno
import ffi

// ==================== USER32 ====================
clib user32 {
    Ptr GetDC(Ptr hwnd)
    i32 ReleaseDC(Ptr hwnd, Ptr hdc)
    i32 GetSystemMetrics(i32 index)
}

user32 _usr = null
func _usrLib(): user32 {
    if _usr == null { _usr = ffi.load("user32.dll") }
    return _usr
}

// ==================== GDI32 ====================
clib gdi32 {
    Ptr CreateCompatibleDC(Ptr hdc)
    Ptr CreateCompatibleBitmap(Ptr hdc, i32 w, i32 h)
    Ptr SelectObject(Ptr hdc, Ptr obj)
    bool BitBlt(Ptr dest, i32 x, i32 y, i32 w, i32 h, Ptr src, i32 x1, i32 y1, u32 rop)
    i32 GetDIBits(Ptr hdc, Ptr bmp, u32 start, u32 lines, Ptr bits, Ptr bmi, u32 usage)
    bool DeleteDC(Ptr hdc)
    bool DeleteObject(Ptr obj)
}

gdi32 _gdi = null
func _gdiLib(): gdi32 {
    if _gdi == null { _gdi = ffi.load("gdi32.dll") }
    return _gdi
}
```

**cstruct 声明**：

```leno
// BITMAPINFOHEADER (40 bytes) — GetDIBits 需要此结构
cstruct BITMAPINFOHEADER {
    u32 biSize
    i32 biWidth
    i32 biHeight
    u16 biPlanes
    u16 biBitCount
    u32 biCompression
    u32 biSizeImage
    i32 biXPelsPerMeter
    i32 biYPelsPerMeter
    u32 biClrUsed
    u32 biClrImportant
}
```

**常量**：

```leno
const SRCCOPY       = 0x00CC0020  // BitBlt 光栅操作码
const DIB_RGB_COLORS = 0          // GetDIBits 颜色格式
const SM_CXSCREEN   = 0           // 屏幕宽度索引
const SM_CYSCREEN   = 1           // 屏幕高度索引
```

**captureScreen() 实现**：

1. `_usrLib().GetDC(null)` 获取桌面 DC
2. `_usrLib().GetSystemMetrics(SM_CXSCREEN/CYSCREEN)` 获取屏幕尺寸
3. `_gdiLib().CreateCompatibleDC(hdcScreen)` 创建内存 DC
4. `_gdiLib().CreateCompatibleBitmap(hdcScreen, w, h)` 创建位图
5. `_gdiLib().SelectObject(hdcMem, hBitmap)` 选入位图
6. `_gdiLib().BitBlt(...)` 从屏幕拷贝到内存位图
7. 构造 `BITMAPINFOHEADER` (biHeight=-h 顶部朝下，biBitCount=32，biCompression=0)
8. `ffi.malloc(w * h * 4)` 分配像素缓冲区
9. `_gdiLib().GetDIBits(...)` 获取 BGRA 像素数据
10. 循环设置每个像素 alpha=255（Windows GDI 32位 DIB alpha 值为 0/未定义）
11. 清理 GDI 资源（SelectObject 恢复旧位图 → DeleteObject → DeleteDC → ReleaseDC → bmi.free()）
12. 返回 Dict: `{ "w": w, "h": h, "fmt": PIXELFORMAT_BGRA8888, "pixels": pixels, "pitch": w * 4 }`
13. 错误时返回 `{}`

**错误处理**：
- `GetDC` 返回 null → 返回 `{}`
- 屏幕尺寸为 0 → 返回 `{}`
- `CreateCompatibleDC` 返回 null → 释放 DC，返回 `{}`
- `CreateCompatibleBitmap` 返回 null → 释放 DC，返回 `{}`
- 使用 try-catch 包裹，确保 GDI 资源不泄漏

**关键细节**：
- `biHeight = -h`：负值表示自顶向下布局，与 SDL 期望一致
- 像素格式：Windows 32-bit DIB 返回 BGRA，对应 `PIXELFORMAT_BGRA8888`
- alpha 修正：循环 `ffi.write_byte(pixels, i * 4 + 3, 255)` 设置每个像素 alpha=255
- 内存所有权：`pixels` 由 `ffi.malloc` 分配，调用者负责 `ffi.free()`

### 2. 创建 `d:\CLeno\LenoC\leno_module\LenoWin32\examples\test_capture.leno`

测试示例：初始化 SDL3 → 截屏 → 创建 Surface → 保存 PNG → 清理。

```leno
import Win32
import SDL3
import ffi

main() {
    if not SDL3.initVideo() {
        print("SDL3 init failed: " + SDL3.getError())
        return
    }

    var shot = Win32.captureScreen()
    int w = shot.get("w", 0)
    int h = shot.get("h", 0)
    if w == 0 or h == 0 {
        print("capture failed")
        SDL3.quit()
        return
    }

    int fmt = shot.get("fmt", 0)
    Ptr[u8] pixels = shot.get("pixels", null) as Ptr[u8]
    int pitch = shot.get("pitch", 0)

    Ptr[u8] surface = SDL3.createSurfaceFromRaw(w, h, fmt, pixels, pitch)
    if surface != null {
        bool ok = SDL3.saveImagePNG(surface, "screenshot.png")
        print("Save " + (ok ? "OK" : "FAILED") + ": " + w + "x" + h)
        SDL3.destroySurface(surface)
    }

    ffi.free(pixels)
    SDL3.quit()
}
```

### 3. 验证 Win32.leno 现有代码

Win32.leno 已正确导入和调用：
```leno
import "w32_capture.leno" as cap
export func captureScreen() { ... return cap.captureScreen() }
```
无需修改。

## Assumptions & Decisions

1. **返回 Dict 而非 SDL Surface**：保持 Win32 模块独立于 SDL3，用户可自行选择如何使用像素数据
2. **32-bit BGRA 格式**：Windows GDI 标准输出格式，与 SDL3 PIXELFORMAT_BGRA8888 对应
3. **alpha=255 修正**：Windows GDI 32-bit DIB 的 alpha 通道值为 0 或未定义，需手动设为 255
4. **biHeight=-h 顶部朝下**：与 SDL 像素布局一致，避免后续翻转
5. **像素数据内存由调用者释放**：`ffi.malloc` 分配，调用者负责 `ffi.free()`
6. **不处理 DPI 缩放**：作为首版功能，DPI 感知可后续添加
7. **每个 .leno 文件独立声明 clib**：遵循 w32_hotkey.leno 的模式

## Verification Steps

1. 编译验证：确保 `w32_capture.leno` 无语法/语义错误
2. 运行测试示例 `test_capture.leno`，确认生成 `screenshot.png`
3. 打开 PNG 图片验证截屏内容正确
4. 检查截屏尺寸是否匹配当前屏幕分辨率
5. 验证 GDI 资源无泄漏（通过重复调用确认无崩溃）
