# LenoSDL3 API 文档

## 概述

LenoSDL3 是 LenoC 语言对 SDL3 的原生绑定模块，通过 `clib` + `cstruct` 直接调用 SDL3.dll，无需任何 C 中间层。

## 安装

1. 从 [SDL3 Releases](https://github.com/libsdl-org/SDL/releases) 下载 `SDL3-3.x.x-win32-x64.zip`
2. 将 `SDL3.dll` 放到 `native/` 目录或项目工作目录

## 快速开始

```leno
import SDL3

SDL3.initVideo()
SDL3.Window win = SDL3.Window()
win.init("My Window", 800, 600, SDL3.WINDOW_RESIZABLE)
SDL3.Renderer ren = SDL3.Renderer()
ren.initDefault(win.handle)

SDL3.Event ev = SDL3.Event()
while true {
    while ev.poll() {
        if ev.isQuit() { break }
    }
    ren.setColor(0, 0, 0, 255)
    ren.clear()
    ren.present()
}
ren.destroy(); win.destroy(); SDL3.quit()
```

## 核心类型

### cstruct（C 布局结构体）

| 结构体 | 用途 | 字段 |
|--------|------|------|
| `SDL_Rect` | 整数矩形 | x, y, w, h (i32) |
| `SDL_FRect` | 浮点矩形 | x, y, w, h (f32) |
| `SDL_Point` | 整数点 | x, y (i32) |
| `SDL_FPoint` | 浮点点 | x, y (f32) |
| `SDL_FColor` | 浮点颜色 | r, g, b, a (f32) |
| `SDL_Event` | 事件联合体 | type (u32) + 128字节 |

### Leno struct（高级包装）

| 结构体 | 用途 | 关键方法 |
|--------|------|----------|
| `Window` | SDL窗口 | init, destroy, setTitle, setSize, size, setFullscreen |
| `Renderer` | 2D渲染器 | init, initDefault, destroy, clear, present, setColor, fillRect, drawRect, drawLine, drawPoint |
| `Event` | 事件轮询 | poll, type, isQuit, isKeyDown, isMouseMotion, scancode, mouseX |

## 初始化标志

| 常量 | 值 | 说明 |
|------|-----|------|
| `INIT_VIDEO` | 0x20 | 视频子系统（含事件） |
| `INIT_AUDIO` | 0x10 | 音频子系统 |
| `INIT_EVENTS` | 0x4000 | 事件子系统 |
| `INIT_GAMEPAD` | 0x2000 | 游戏手柄 |

## 窗口标志

| 常量 | 说明 |
|------|------|
| `WINDOW_FULLSCREEN` | 全屏模式 |
| `WINDOW_RESIZABLE` | 可调整大小 |
| `WINDOW_BORDERLESS` | 无边框 |
| `WINDOW_HIDDEN` | 隐藏 |
| `WINDOW_HIGH_PIXEL_DENSITY` | 高DPI |

## 事件类型

| 常量 | 说明 |
|------|------|
| `EVENT_QUIT` | 退出请求 |
| `EVENT_KEY_DOWN` / `EVENT_KEY_UP` | 键盘按下/释放 |
| `EVENT_MOUSE_MOTION` | 鼠标移动 |
| `EVENT_MOUSE_BUTTON_DOWN` / `EVENT_MOUSE_BUTTON_UP` | 鼠标按钮 |
| `EVENT_WINDOW_CLOSE_REQUESTED` | 窗口关闭请求 |
| `EVENT_WINDOW_RESIZED` | 窗口大小变化 |

## 扫描码

| 常量 | 说明 |
|------|------|
| `SCANCODE_ESCAPE` | ESC键 |
| `SCANCODE_RETURN` | 回车键 |
| `SCANCODE_SPACE` | 空格键 |
| `SCANCODE_W/A/S/D` | 方向键 |
| `SCANCODE_LEFT/RIGHT/UP/DOWN` | 算术键 |

## 注意事项

- SDL3 要求所有视频操作在主线程执行
- `bool` 返回值映射为 `i32`（0=false, 非0=true）
- `SDL_Event` 是 C 联合体，事件特定数据通过偏移读取辅助函数访问
- 渲染器绘制使用浮点坐标（`SDL_FRect` / `f32`）
- 调用 `quit()` 会同时卸载 DLL
