# LenoC GUIs 模块

本文档详细说明 `guis` 模块提供的图形界面功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [窗口标志](#窗口标志)
- [模块方法](#模块方法)
- [窗口操作](#窗口操作)
- [渲染器方法（GDraw 实例方法）](#渲染器方法draw-实例方法)
- [事件方法（GEvent 实例方法）](#事件方法event-实例方法)
- [图片加载](#图片加载)
- [zlib 解压](#zlib-解压)
- [GImage 实例方法](#image-实例方法)
- [事件循环](#事件循环)
- [剪贴板](#剪贴板)
- [光标与透明度](#光标与透明度)
- [文件对话框](#文件对话框)
- [字体操作](#字体操作)
- [逻辑呈现模式](#逻辑呈现模式)
- [计时器](#计时器)
- [示例代码](#示例代码)
- [注意事项](#注意事项)
- [最佳实践](#最佳实践)

---

## 概述

GUIs 模块是 Leno 语言的跨平台图形界面模块，灵感来源于 SDL3，无外部依赖。

### 特性

- **跨平台支持**：Windows (Win32+GDI)、Linux (X11)、macOS (Cocoa)
- **软件渲染**：像素缓冲区 + 平台 blit，无需 GPU
- **双缓冲**：后备缓冲区渲染完成后 blit 到窗口，避免闪烁
- **回调式事件循环**：参考 SDL3 主回调机制，简洁高效
- **丰富绘图 API**：矩形、圆形、圆角矩形、直线、纹理等
- **视口与裁剪**：参考 SDL3 的 RenderViewport / ClipRect 机制

### 架构

```
leno_guis.h          - 平台抽象层（类型定义 + 函数声明）
leno_guis_win32.c    - Windows 实现 (Win32 API + GDI 双缓冲)
leno_guis_linux.c    - Linux 实现 (X11)
leno_guis_macos.c    - macOS 实现 (Cocoa)
guis.c               - LenoC 模块注册
```

### 类型关键字

| 类型 | 说明 |
|------|------|
| `GWin` | 窗口对象 |
| `GDraw` | 渲染器对象 |
| `GEvent` | 事件对象 |
| `GImage` | 图像对象 |
| `GFont` | 字体对象 |
| `GRgb` | 颜色对象（通过 `_rgb(r, g, b, a?)` 创建） |

---

## 使用方式

```leno
import guis

main() {
    // 方式一：显式 Style 类型（推荐，支持 IDE 字段补全）
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("Hello", w_style)

    // 方式二：直接传入 Dict（简洁，适合快速原型）
    // GWin win = guis.create_window("Hello", {width: 800, height: 600})

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()
            ren.set_color(_rgb(255, 0, 0, 255))
            ren.fill_rect(100, 100, 200, 150)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
        }
    )
    win.close()
}
```

---

## 窗口标志

创建窗口时通过 `flags` 参数组合使用（位或运算）：

| 标志值 | 说明 |
|--------|------|
| `1` | 可调整大小 (`RESIZABLE`) |
| `2` | 全屏 (`FULLSCREEN`) |
| `4` | 无边框 (`BORDERLESS`) |
| `8` | 隐藏窗口 (`HIDDEN`) |
| `16` | 置顶 (`ALWAYS_ON_TOP`) |
| `32` | 最小化 (`MINIMIZED`) |
| `64` | 最大化 (`MAXIMIZED`) |

```leno
// 使用 Style[window] 定义窗口样式
Style[window] w_style = {
    width: 800,
    height: 600,
    resizable: true
}
var win = guis.create_window("App", w_style)

// 或使用内联样式
var win = guis.create_window("App", {width: 1024, height: 768, fullscreen: true})
```

---

## 模块方法



---

### `win.close()`

销毁窗口并退出 GUI 子系统。程序结束时调用。

**返回**: `null`

```leno
win.close()
```

---

### `guis.create_window(title, style_dict)`

创建窗口。

**参数**:
- `title` (string): 窗口标题
- `style_dict` (Dict): 窗口样式字典，支持以下字段：
  - `width` (int): 窗口宽度，默认 800
  - `height` (int): 窗口高度，默认 600
  - `x` (int): 窗口位置 X
  - `y` (int): 窗口位置 Y
  - `fullscreen` (bool): 是否全屏
  - `borderless` (bool): 是否无边框
  - `resizable` (bool): 是否可拖拽边缘调整大小，默认 true（独立于 maximizable）
  - `maximizable` (bool): 是否显示最大化按钮，默认 true（独立于 resizable）
  - `opacity` (float): 透明度 0.0~1.0
  - `visible` (bool): 是否可见
  - `always_on_top` (bool): 是否置顶

**返回**: `GWin` - 窗口对象

```leno
// 方式一：显式 Style 类型（推荐，支持 IDE 字段补全）
Style[window] w_style = {
    width: 1024,
    height: 768,
    resizable: true
}
GWin win = guis.create_window("My App", w_style)

// 方式二：直接传入 Dict（简洁，适合快速原型）
GWin win = guis.create_window("My App", {width: 800, height: 600})
```

---

### `win.run(onDraw, onEvent)`

GWin 实例方法：回调式事件循环（推荐使用方式）。自动创建渲染器，循环处理事件和渲染，直到窗口关闭。

**参数**:
- `onDraw` (func(GDraw)): 渲染回调，每帧调用
- `onEvent` (func(GEvent)): 事件回调，每个事件调用

**返回**: `null`

```leno
win.run(
    func(GDraw ren) {
        ren.set_color(_rgb(0, 0, 0, 255))
        ren.clear()
    },
    func(GEvent e) {
        if e.quit() {
            win.set_should_close(true)
        }
    }
)
```

---

## 窗口操作（GWin 实例方法）

GWin 类型支持实例方法调用，使用 `win.method()` 风格：

### `win.show()` / `win.hide()`

显示或隐藏窗口。

```leno
win.show()
win.hide()
```

---

### `win.set_title(title)`

设置窗口标题。

```leno
win.set_title("新标题")
```

---

### `win.set_size(w, h)` / `win.get_size()`

设置或获取窗口大小。`get_size` 返回 `[w, h]` 数组。

```leno
win.set_size(1024, 768)
var size = win.get_size()
print("宽: " + size[0] + " 高: " + size[1])
```

---

### `win.set_pos(x, y)` / `win.get_pos()`

设置或获取窗口位置。`get_pos` 返回 `[x, y]` 数组。

```leno
win.set_pos(100, 100)
var pos = win.get_pos()
```

---

### `win.set_fullscreen(bool)`

设置窗口全屏状态。

```leno
win.set_fullscreen(true)   // 全屏
win.set_fullscreen(false)  // 退出全屏
```

---

### `win.should_close()` / `win.set_should_close(bool)`

查询或设置窗口关闭标志。

```leno
if win.should_close() {
    print("窗口应该关闭")
}
win.set_should_close(true)
```

---

### `win.set_opacity(opacity)`

设置窗口透明度。

**参数**:
- `opacity` (float): 0.0（完全透明）~ 1.0（完全不透明）

```leno
win.set_opacity(0.85)
```

---

## 渲染器方法（GDraw 实例方法）

渲染器通过 `win.run()` 的 onDraw 回调参数获取，或通过 `guis.create_renderer(GWin)` 手动创建。

### `ren.set_color(GRgb)`

设置绘制颜色。

**参数**:
- `color` (GRgb): RGB 颜色对象，通过 `_rgb(r, g, b, a)` 创建

```leno
var c = _rgb(255, 100, 50, 255)
ren.set_color(c)
```

---

### `ren.clear()`

用当前绘制颜色清除整个渲染缓冲区。

```leno
ren.set_color(_rgb(30, 30, 46, 255))
ren.clear()
```

---

### `ren.point(x, y)`

绘制单个像素点。

```leno
ren.set_color(_rgb(255, 0, 0, 255))
ren.point(100, 100)
```

---

### `ren.line(x1, y1, x2, y2)`

绘制直线。

```leno
ren.set_color(_rgb(255, 255, 0, 255))
ren.line(0, 0, 800, 600)
```

---

### `ren.rect(x, y, w, h)`

绘制矩形边框。

```leno
ren.set_color(_rgb(0, 255, 0, 255))
ren.rect(50, 50, 200, 100)
```

---

### `ren.fill_rect(x, y, w, h)`

填充矩形。

```leno
ren.set_color(_rgb(100, 100, 255, 255))
ren.fill_rect(50, 50, 200, 100)
```

---

### `ren.circle(cx, cy, radius)`

绘制圆形边框（Bresenham 中点圆算法）。

```leno
ren.set_color(_rgb(255, 100, 100, 255))
ren.circle(400, 300, 80)
```

---

### `ren.fill_circle(cx, cy, radius)`

填充圆形（水平扫描线填充）。

```leno
ren.set_color(_rgb(100, 255, 100, 255))
ren.fill_circle(400, 300, 60)
```

---

### `ren.round_rect(x, y, w, h, radius)`

绘制圆角矩形边框。

```leno
ren.set_color(_rgb(100, 150, 255, 255))
ren.round_rect(50, 50, 200, 100, 15)
```

---

### `ren.fill_round(x, y, w, h, radius)`

填充圆角矩形。

```leno
ren.set_color(_rgb(180, 100, 255, 255))
ren.fill_round(50, 50, 200, 100, 15)
```

---

### `ren.get_size()`

获取渲染器缓冲区大小。

**返回**: `[w, h]` - 宽高数组

```leno
var size = ren.get_size()
ren.set_viewport(0, 0, size[0], size[1])
```

---

### `ren.set_viewport(x, y, w, h)` / `ren.get_viewport()`

设置或获取渲染视口。所有后续绘制坐标相对于视口左上角。

```leno
ren.set_viewport(50, 50, 200, 200)
var vp = ren.get_viewport()  // [x, y, w, h]
```

---

### `ren.set_clip_rect(x, y, w, h)` / `ren.get_clip_rect()` / `ren.no_clip()`

设置、获取或禁用裁剪矩形。裁剪矩形限制绘制区域（物理坐标）。

```leno
ren.set_clip_rect(100, 100, 300, 200)
// 此区域外的绘制将被裁剪
ren.no_clip()
```

---

## 事件方法（GEvent 实例方法）

事件通过 `win.run()` 的 onEvent 回调参数获取，或通过 `guis.poll()` 获取。

### 事件类型判断

| 方法 | 说明 |
|------|------|
| `e.quit()` | 是否为退出事件 |
| `e.window_close()` | 是否为窗口关闭事件 |
| `e.window_resize()` | 是否为窗口大小改变事件 |
| `e.window_move()` | 是否为窗口移动事件 |
| `e.window_focus()` | 是否为窗口获得焦点事件 |
| `e.window_unfocus()` | 是否为窗口失去焦点事件 |
| `e.window_show()` | 是否为窗口显示事件 |
| `e.window_hide()` | 是否为窗口隐藏事件 |
| `e.window_exposed()` | 是否为窗口暴露事件（需要重绘） |
| `e.key_down()` | 是否为按键按下事件 |
| `e.key_up()` | 是否为按键释放事件 |
| `e.text_input()` | 是否为文本输入事件 |
| `e.mouse_move()` | 是否为鼠标移动事件 |
| `e.mouse_down()` | 是否为鼠标按下事件 |
| `e.mouse_up()` | 是否为鼠标释放事件 |
| `e.mouse_wheel()` | 是否为鼠标滚轮事件 |
| `e.is_drop_file()` | 是否为文件拖放事件 |
| `e.is_drop_text()` | 是否为文本拖放事件 |

---

### 事件数据获取

| 方法 | 返回类型 | 说明 | 适用事件 |
|------|----------|------|----------|
| `e.type()` | int | 事件类型编号 | 所有事件 |
| `e.window_id()` | int | 窗口 ID | 所有事件 |
| `e.key()` | int | 按键码 | key_down / key_up |
| `e.scancode()` | int | 扫描码 | key_down / key_up |
| `e.mod()` | int | 修饰键标志 | key_down / key_up |
| `e.repeat()` | int | 是否为重复按键 | key_down |
| `e.mouse_x()` | int | 鼠标 X 坐标 | mouse_move / mouse_down / mouse_up / mouse_wheel |
| `e.mouse_y()` | int | 鼠标 Y 坐标 | mouse_move / mouse_down / mouse_up / mouse_wheel |
| `e.mouse_xrel()` | int | 鼠标 X 相对移动量 | mouse_move |
| `e.mouse_yrel()` | int | 鼠标 Y 相对移动量 | mouse_move |
| `e.mouse_button()` | int | 鼠标按钮 (1=左 2=中 3=右) | mouse_down / mouse_up |
| `e.mouse_clicks()` | int | 点击次数（双击检测） | mouse_down / mouse_up |
| `e.wheel_x()` | int | 滚轮水平滚动量 | mouse_wheel |
| `e.wheel_y()` | int | 滚轮垂直滚动量 | mouse_wheel |
| `e.width()` | int | 窗口宽度 | window_resize |
| `e.height()` | int | 窗口高度 | window_resize |
| `e.x()` | int | 窗口新位置 X | window_move |
| `e.y()` | int | 窗口新位置 Y | window_move |
| `e.text()` | string | 输入文本 | text_input / drop_file / drop_text |

### 常用按键码

| 按键 | 码值 | 按键 | 码值 |
|------|------|------|------|
| Escape | `0x1B` | Return | `0x0D` |
| Space | `0x20` | Backspace | `0x08` |
| Tab | `0x09` | Delete | `0x7F` |
| A-Z | `65-90` | 0-9 | `48-57` |

```leno
func(GEvent e) {
    if e.key_down() {
        var key = e.key()
        if key == 0x1B {
            win.set_should_close(true)
        }
        print("key: " + _str(key))
    }
    if e.mouse_down() {
        print("mouse at: " + _str(e.mouse_x()) + ", " + _str(e.mouse_y()))
    }
    if e.window_resize() {
        print("resize: " + _str(e.width()) + "x" + _str(e.height()))
    }
}
```

---

## 图片加载

### `guis.load_image(path)`

从文件加载图片。

**参数**:
- `path` (string): 图片文件路径

**返回**: `GImage` - 图片对象，失败返回 `null`

```leno
var img = guis.load_image("photo.png")
if img != null {
    print("图片尺寸: " + img.width() + "x" + img.height())
    img.close()
}
```

---

### `guis.load_image_ex(path, options)`

带选项加载图片。

**参数**:
- `path` (string): 图片文件路径
- `options` (Dict): 选项字典
  - `flip_vertical` (bool): 是否垂直翻转（OpenGL兼容）

**返回**: `GImage` - 图片对象

```leno
// 垂直翻转加载（OpenGL纹理坐标兼容）
var img = guis.load_image_ex("texture.png", {flip_vertical: true})
```

---

### `guis.load_image_from_memory(data)`

从内存数据加载图片。

**参数**:
- `data` (string): 图片二进制数据（字节字符串）

**返回**: `GImage` - 图片对象

```leno
import files

var f = files.open("image.png", "rb")
var bytes = f.read()
f.close()

var img = guis.load_image_from_memory(bytes)
```

---

### `guis.image_info(path)` / `guis.image_info_from_memory(data)`

获取图片信息（不加载像素数据）。

**返回**: `{width, height, channels}` 或 `null`

```leno
var info = guis.image_info("photo.png")
if info != null {
    print("宽度: " + info.width)
    print("高度: " + info.height)
    print("通道: " + info.channels)  // 1=灰度, 3=RGB, 4=RGBA
}
```

---

### `guis.is_16_bit(path)` / `guis.is_16_bit_from_memory(data)`

检查图片是否为16位深度。

**返回**: `bool`

```leno
if guis.is_16_bit("image.png") {
    print("这是16位图片")
}
```

---

### `guis.is_hdr(path)` / `guis.is_hdr_from_memory(data)`

检查图片是否为HDR格式。

**返回**: `bool`

```leno
if guis.is_hdr("scene.hdr") {
    print("这是HDR图片")
}
```

---

## zlib解压

### `guis.zlib_decode(data)`

解压zlib压缩的数据。

**参数**:
- `data` (string): 压缩的二进制数据

**返回**: `string` - 解压后的数据，失败返回 `null`

```leno
var compressed = files.read("data.zlib")
var raw = guis.zlib_decode(compressed)
if raw != null {
    print("解压成功，大小: " + raw.len())
}
```

---

### `guis.zlib_decode_noheader(data)`

解压无头部的zlib压缩数据。

**参数**:
- `data` (string): 压缩的二进制数据（无zlib头部）

**返回**: `string` - 解压后的数据

```leno
// 某些格式使用无头部的zlib压缩
var raw = guis.zlib_decode_noheader(compressed_data)
```

---

## GImage 实例方法

### `image.close()`

关闭并释放图像资源。

```leno
var img = guis.load_image("logo.png")
// 使用图像...
img.close()
```

---

### `image.width()` / `image.height()`

获取图像尺寸。

**返回**: `int` - 宽度或高度（像素）

```leno
var w = img.width()
var h = img.height()
```

---

### `image.draw(ren, x, y)`

绘制图像到渲染器。

```leno
img.draw(ren, 100, 100)
```

---

### `image.draw_scaled(ren, x, y, w, h)`

缩放绘制图像。

```leno
img.draw_scaled(ren, 100, 100, 200, 150)
```

---

### `image.draw_rotated(ren, x, y, angle, flip)`

旋转/翻转绘制图像。

**参数**:
- `angle` (float): 旋转角度（度）
- `flip` (int): 翻转标志 (0=无, 1=水平, 2=垂直)

```leno
img.draw_rotated(ren, 400, 300, 45.0, guis.FLIP_NONE)
```

---

### `image.draw_rotated_scaled(ren, x, y, w, h, angle)`

旋转+缩放绘制图像。

```leno
img.draw_rotated_scaled(ren, 400, 300, 200, 150, 45.0)
```

---

### `image.draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh)`

从图像中取子区域绘制到目标位置（可缩放）。

**参数**:
- `ren` (GDraw): 渲染器对象
- `sx`, `sy`, `sw`, `sh`: 源图像区域（x, y, 宽, 高）
- `dx`, `dy`, `dw`, `dh`: 目标绘制区域（x, y, 宽, 高）

```leno
img.draw_src(ren, 0, 0, 64, 64, 100, 100, 128, 128)
```

---

### `image.draw_flipped(ren, x, y, flip)`

翻转绘制图像。

**参数**:
- `flip` (int): 翻转标志 (0=无, 1=水平, 2=垂直, 3=两者)

```leno
img.draw_flipped(ren, 100, 100, guis.FLIP_HORIZONTAL)
```

---

### `image.draw_flipped_scaled(ren, x, y, w, h, flip)`

翻转+缩放绘制图像。

```leno
img.draw_flipped_scaled(ren, 100, 100, 200, 150, guis.FLIP_HORIZONTAL)
```

---

### `image.size()`

获取图像尺寸。

**返回**: `[w, h]` - 宽高数组

```leno
var size = img.size()
print("尺寸: " + size[0] + "x" + size[1])
```

---

### `image.access()`

获取图像访问模式。

**返回**: `int` - 访问模式（0=静态, 1=流式, 2=目标）

```leno
var mode = img.access()
```

---

## 事件循环

`win.run()` 是事件循环方式，内部自动处理：

1. 轮询并分发所有事件到 onEvent 回调
2. 检查窗口关闭标志
3. 自动调整渲染器大小（窗口 resize 时）
4. 调用 onDraw 回调进行渲染

```leno
win.run(
    func(GDraw ren) {
        // 每帧渲染
        ren.set_color(_rgb(0, 0, 0, 255))
        ren.clear()
        // ... 绘制内容
    },
    func(GEvent e) {
        // 事件处理
        if e.quit() or e.window_close() {
            win.set_should_close(true)
        }
        if e.key_down() and e.key() == 0x1B {
            win.set_should_close(true)
        }
    }
)
```

---

## 剪贴板

### `guis.get_clipboard()` / `guis.set_clipboard(text)`

获取或设置剪贴板文本。

```leno
guis.set_clipboard("Hello from Leno!")
var text = guis.get_clipboard()
print(text)
```

---

## 光标与透明度

### `guis.show_cursor(bool)`

显示或隐藏光标。

```leno
guis.show_cursor(false)  // 隐藏光标
guis.show_cursor(true)   // 显示光标
```

---

### `guis.msg_box(title, message, type)`

显示系统消息框。

**参数**:
- `type` (int): 0=信息, 1=警告, 2=错误

**返回**: `int` - 用户选择结果

```leno
guis.msg_box("提示", "操作完成", 0)
guis.msg_box("警告", "磁盘空间不足", 1)
guis.msg_box("错误", "无法连接服务器", 2)
```

---

### `guis.set_cursor(cursor_type)`

设置系统光标样式。

**参数**:
- `cursor_type` (int): 光标类型，可用值：

| 常量 | 值 | 说明 |
|------|-----|------|
| 0 | 默认 | 默认箭头 |
| 1 | 文本 | 文本选择 I 型 |
| 2 | 等待 | 等待（沙漏） |
| 3 | 十字 | 十字准星 |
| 4 | 进度 | 后台忙（带箭头沙漏） |
| 5 | 调整大小 NW-SE | 西北-东南双箭头 |
| 6 | 调整大小 NE-SW | 东北-西南双箭头 |
| 7 | 调整大小 E-W | 东西双箭头 |
| 8 | 调整大小 N-S | 南北双箭头 |
| 9 | 移动 | 四向箭头（移动） |
| 10 | 禁止 | 禁止操作 |
| 11 | 手型 | 链接手型 |

```leno
guis.set_cursor(1)  // 文本输入光标
guis.set_cursor(11) // 手型光标
guis.set_cursor(0)  // 恢复默认
```

---

## 文件对话框

### `guis.file_dialog(type, callback, opts?)`

显示系统原生文件对话框。对话框在后台线程中打开，不会阻塞主线程。用户选择完成后，回调在主线程中执行。

**参数**:
- `type` (int): 对话框类型
  - `0` — 打开文件 (`OPENFILE`)
  - `1` — 保存文件 (`SAVEFILE`)
  - `2` — 打开文件夹 (`OPENFOLDER`)
- `callback` (func): 回调函数，签名为 `func(file_list, filter_index)`
  - `file_list` (array): 用户选择的文件路径数组（取消时为 `null`）
  - `filter_index` (int): 用户选择的过滤器索引
- `opts` (Dict, 可选): 选项字典，支持以下字段：
  - `title` (string): 对话框标题
  - `path` (string): 默认路径
  - `multiple` (bool): 是否允许多选
  - `filters` (array): 文件过滤器数组，每个元素为 `{name: "显示名", pattern: "匹配模式"}`
  - `window` (GWin): 父窗口（Windows 下为模态对话框）

```leno
// 打开文件对话框
guis.file_dialog(0, func(file_list, filter_index) {
    if file_list != null {
        for var i = 0; i < file_list.len(); i = i + 1 {
            print("选中文件: " + file_list[i])
        }
    } else {
        print("用户取消了选择")
    }
}, {
    title: "选择文本文件",
    filters: [
        {name: "Text Files", pattern: "*.txt"},
        {name: "All Files", pattern: "*.*"}
    ],
    multiple: true
})

// 保存文件对话框
guis.file_dialog(1, func(file_list, filter_index) {
    if file_list != null {
        print("保存到: " + file_list[0])
    }
}, {
    title: "保存文件",
    path: "untitled.txt"
})

// 打开文件夹对话框
guis.file_dialog(2, func(file_list, filter_index) {
    if file_list != null {
        print("选中目录: " + file_list[0])
    }
}, {
    title: "选择目录"
})
```

> **注意**：文件对话框的回调在主线程的事件循环中执行，回调中可以安全地使用闭包捕获外部变量。

---

## 字体操作

### `guis.load_font(name, size)`

加载系统字体。

**参数**:
- `name` (string): 字体名称，如 `"Arial"`、`"Consolas"`、`"SimSun"` 等
- `size` (int): 字体大小（像素）

**返回**: `GFont` 或 `null`（加载失败时）

```leno
var font = guis.load_font("Consolas", 16)
```

---

### `font.close()`

关闭并释放字体资源。

```leno
font.close()
```

---

### `ren.draw_text(font, text, x, y)`

使用指定字体绘制文字。

**参数**:
- `font` (GFont): 字体对象
- `text` (string): 要绘制的文本
- `x`, `y` (int): 绘制位置

```leno
var font = guis.load_font("SimSun", 16)
ren.set_color(_rgb(255, 255, 255, 255))
ren.draw_text(font, "Hello Leno!", 100, 100)
font.close()
```

---

### `ren.text_size(font, text) -> [w, h]`

计算指定字体的文字尺寸。

**参数**:
- `font` (GFont): 字体对象
- `text` (string): 要计算的文本

**返回**: `[宽度, 高度]` 数组

```leno
var font = guis.load_font("SimSun", 16)
var size = ren.text_size(font, "Hello")
print("宽: " + size[0] + " 高: " + size[1])
font.close()
```

---

## 逻辑呈现模式

逻辑呈现模式（借鉴 SDL3 `SDL_RendererLogicalPresentation`）允许将渲染坐标与窗口物理尺寸解耦，实现自适应缩放。

### `ren.set_logical_size(w, h)`

设置逻辑渲染大小。后续所有绘制坐标基于此逻辑尺寸，自动缩放到窗口实际大小。

```leno
ren.set_logical_size(1920, 1080)
```

---

### `ren.get_logical_size() -> [w, h]`

获取当前逻辑渲染大小。

---

### `ren.set_logical_presentation(mode)`

设置逻辑呈现模式。

**参数**:
- `mode` (int):

| 值 | 说明 |
|----|------|
| `0` | 禁用逻辑大小 |
| `1` | 拉伸填充 |
| `2` | 信箱模式（保持比例，加黑边） |
| `3` | 过扫描 |
| `4` | 整数倍缩放 |

```leno
ren.set_logical_size(1920, 1080)
ren.set_logical_presentation(2)  // 信箱模式
```

---

### `ren.get_logical_presentation() -> int`

获取当前逻辑呈现模式。

---

### `ren.get_logical_viewport() -> [x, y, w, h]`

获取逻辑尺寸映射到窗口的实际视口区域。

---

### `ren.reset_logical_size()`

重置逻辑大小，恢复为窗口物理尺寸。

---

## 计时器

### `guis.get_ticks()`

获取自系统启动以来的毫秒数。

**返回**: `int`

```leno
var start = guis.get_ticks()
// ... 执行操作
var elapsed = guis.get_ticks() - start
print("耗时: " + elapsed + "ms")
```

---

### `guis.get_perf_counter()` / `guis.get_perf_freq()`

获取高精度性能计数器值和频率。

```leno
var counter = guis.get_perf_counter()
var freq = guis.get_perf_freq()
var seconds = counter / freq
```

---

### `guis.add_timer(interval_ms, callback)`

添加定时器回调。参考 SDL3 的 `SDL_AddTimer` 设计，在事件循环中检查并触发。

**参数**:
- `interval_ms` (int): 定时间隔（毫秒）
- `callback` (func): 回调函数，签名为 `func(timer_id, interval_ms) -> int`
  - 返回值 > 0：下次定时间隔（毫秒），可以动态调整
  - 返回值 = 0：取消定时器

**返回**: `int` - 定时器 ID（0 表示失败）

```leno
var tid = guis.add_timer(1000, func(id, interval) {
    print("定时器触发! id=" + _str(id))
    return interval  // 返回原间隔继续定时，返回 0 取消
})
```

---

### `guis.remove_timer(timer_id)`

取消定时器。

**参数**:
- `timer_id` (int): `add_timer` 返回的定时器 ID

**返回**: `bool` - 是否成功取消

```leno
guis.remove_timer(tid)
```

---

### 定时器回调规则

参考 SDL3 的 `SDL_AddTimer` 设计：

| 回调返回值 | 行为 |
|-----------|------|
| `0` | 取消定时器，不再触发 |
| `interval`（原值） | 保持原间隔继续定时 |
| `新值 > 0` | 以新间隔继续定时 |

```leno
var count = 0
var tid = guis.add_timer(500, func(id, interval) {
    count = count + 1
    print("第 " + _str(count) + " 次触发")

    if count >= 10 {
        return 0  // 触发 10 次后取消
    }

    if count >= 5 {
        return 200  // 第 5 次后加速到 200ms
    }

    return interval  // 保持 500ms
})
```

### `guis.get_display()` / `guis.get_dpi()`

获取显示器尺寸和 DPI。

```leno
var size = guis.get_display()
print("屏幕: " + size[0] + "x" + size[1])

var dpi = guis.get_dpi()
print("DPI: " + dpi)
```

---

## 示例代码

### 示例1：基础窗口

```leno
import guis

main() {
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("Hello LenoC", w_style)

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()
            ren.set_color(_rgb(255, 100, 100, 255))
            ren.fill_rect(100, 100, 200, 150)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
        }
    )

    win.close()
}
```

---

### 示例2：FPS 计数器

```leno
import guis

main() {
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("FPS Counter", w_style)

    var frame = 0
    var start_ticks = guis.get_ticks()

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(0, 0, 0, 255))
            ren.clear()

            frame = frame + 1
            var elapsed = guis.get_ticks() - start_ticks
            if elapsed > 0 and frame % 60 == 0 {
                var fps = frame * 1000 / elapsed
                print("FPS: " + _str(fps))
            }
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
        }
    )

    win.close()
}
```

---

### 示例3：鼠标交互

```leno
import guis

main() {
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("Mouse Demo", w_style)

    var mx = 0
    var my = 0
    var clicked = false

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.clear()

            if clicked {
                ren.set_color(_rgb(255, 100, 100, 255))
            } else {
                ren.set_color(_rgb(100, 255, 100, 255))
            }
            ren.fill_circle(mx, my, 30)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
            if e.mouse_move() {
                mx = e.mouse_x()
                my = e.mouse_y()
            }
            if e.mouse_down() {
                clicked = true
            }
            if e.mouse_up() {
                clicked = false
            }
        }
    )

    win.close()
}
```

---

### 示例4：视口与裁剪

```leno
import guis

main() {
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("Viewport & Clip", w_style)

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()

            ren.set_viewport(50, 50, 200, 200)
            ren.set_color(_rgb(60, 60, 90, 255))
            ren.fill_rect(0, 0, 200, 200)
            ren.set_color(_rgb(255, 255, 100, 255))
            ren.rect(10, 10, 180, 180)

            var rs = ren.get_size()
            ren.set_viewport(0, 0, rs[0], rs[1])

            ren.set_clip_rect(550, 50, 200, 200)
            ren.set_color(_rgb(80, 40, 40, 255))
            ren.fill_rect(500, 0, 350, 350)
            ren.no_clip()
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
        }
    )

    win.close()
}

---

### 示例5：综合绘图

```leno
import guis

main() {
    Style[window] w_style = {
        width: 800,
        height: 600,
        resizable: true,
        opacity: 0.95
    }
    GWin win = guis.create_window("LenoC GUI - Draw Test", w_style)

    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()

            ren.set_color(_rgb(255, 100, 100, 255))
            ren.circle(400, 150, 80)

            ren.set_color(_rgb(100, 255, 100, 255))
            ren.fill_circle(400, 350, 60)

            ren.set_color(_rgb(100, 150, 255, 255))
            ren.round_rect(50, 350, 200, 100, 20)

            ren.set_color(_rgb(180, 100, 255, 255))
            ren.fill_round(550, 350, 200, 100, 15)

            ren.set_color(_rgb(255, 255, 0, 255))
            ren.line(300, 450, 500, 550)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
            if e.key_down() {
                var key = e.key()
                if key == 0x1B {
                    win.set_should_close(true)
                }
            }
        }
    )

    win.close()
    print("test done!")
}
```

---

## 注意事项

### 1. 初始化顺序

`create_window` 会自动初始化 GUI 子系统，无需手动调用初始化。

```leno
var win = guis.create_window("App", {width: 800, height: 600})
```

### 2. 渲染循环

每帧按 `clear()` → 绘制的顺序操作即可，引擎会在每帧结束后自动将内容显示到窗口。

```leno
ren.set_color(_rgb(0, 0, 0, 255))
ren.clear()           // 1. 清除
ren.fill_rect(...)    // 2. 绘制
                       // 3. 引擎自动 present
```

### 3. 窗口大小变化

使用 `win.run()` 时，渲染器会自动调整大小。

### 4. 像素格式

使用 32 位 ARGB 格式（内存中为 BGRA 小端序），颜色分量范围 0-255。

### 5. 坐标系统

- 窗口左上角为原点 (0, 0)
- X 轴向右递增，Y 轴向下递增
- 视口偏移影响所有绘制坐标

### 6. 事件处理

在 `win.run()` 的事件回调中，必须处理 `quit()` 和 `window_close()` 事件并设置窗口关闭标志，否则窗口无法正常关闭。

---

## 最佳实践

1. **始终处理退出事件**
   ```leno
   func(GEvent e) {
       if e.quit() or e.window_close() {
           win.set_should_close(true)
       }
   }
   ```

2. **使用 `win.close()` 清理资源**
   ```leno
   win.close()
   ```

3. **合理控制帧率**
   ```leno
   var frame = 0
   var start = guis.get_ticks()
   // 在渲染回调中
   frame = frame + 1
   var elapsed = guis.get_ticks() - start
   if elapsed > 0 and frame % 60 == 0 {
       var fps = frame * 1000 / elapsed
       print("FPS: " + _str(fps))
   }
   ```

4. **使用视口简化局部绘制**
   ```leno
   ren.set_viewport(x, y, w, h)
   // 在视口内使用相对坐标绘制
   ren.fill_rect(0, 0, w, h)
   // 恢复全屏视口
   var size = ren.get_size()
   ren.set_viewport(0, 0, size[0], size[1])
   ```

5. **使用裁剪矩形限制绘制区域**
   ```leno
   ren.set_clip_rect(x, y, w, h)
   // 绘制内容不会超出裁剪区域
   ren.no_clip()
   ```

---

*文档版本: 1.2*  
*最后更新: 2026-06-02*
