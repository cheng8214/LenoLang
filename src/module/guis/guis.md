# LenoC GUIs 模块

本文档详细说明 `guis` 模块提供的图形界面功能。

## 目录

- [概述](#概述)
- [使用方式](#使用方式)
- [窗口标志](#窗口标志)
- [模块方法](#模块方法)
- [窗口操作](#窗口操作)
- [渲染器方法（Draw 实例方法）](#渲染器方法draw-实例方法)
- [事件方法（Event 实例方法）](#事件方法event-实例方法)
- [纹理操作](#纹理操作)
- [事件系统](#事件系统)
- [回调式事件循环](#回调式事件循环)
- [输入状态查询](#输入状态查询)
- [剪贴板](#剪贴板)
- [光标与透明度](#光标与透明度)
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
| `Win` | 窗口对象 |
| `Draw` | 渲染器对象 |
| `Event` | 事件对象 |

---

## 使用方式

```leno
import guis

main() {
    Win win = guis.create_window("Hello", 800, 600, 1)
    guis.run(win,
        func(Draw ren) {
            ren.set_color(30, 30, 46, 255)
            ren.clear()
            ren.set_color(255, 0, 0, 255)
            ren.fill_rect(100, 100, 200, 150)
            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
        }
    )
    guis.cleanup(win)
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
var win = guis.create_window("App", 800, 600, 1)         // 可调整大小
var win = guis.create_window("App", 800, 600, 1 | 16)    // 可调整大小 + 置顶
var win = guis.create_window("App", 800, 600, 4)          // 无边框
```

---

## 模块方法



---

### `guis.cleanup(win)`

销毁窗口并退出 GUI 子系统。程序结束时调用。

**参数**:
- `win` (Win): 窗口对象

**返回**: `null`

```leno
guis.cleanup(win)
```

---

### `guis.create_window(title, w, h, flags)`

创建窗口。

**参数**:
- `title` (string): 窗口标题
- `w` (int): 窗口宽度
- `h` (int): 窗口高度
- `flags` (int): 窗口标志（见上文）

**返回**: `Win` - 窗口对象

```leno
Win win = guis.create_window("My App", 1024, 768, 1)
```

---

### `guis.run(win, onDraw, onEvent)`

回调式事件循环（推荐使用方式）。自动创建渲染器，循环处理事件和渲染，直到窗口关闭。

**参数**:
- `win` (Win): 窗口对象
- `onDraw` (func(Draw)): 渲染回调，每帧调用
- `onEvent` (func(Event)): 事件回调，每个事件调用

**返回**: `null`

```leno
guis.run(win,
    func(Draw ren) {
        ren.set_color(0, 0, 0, 255)
        ren.clear()
        ren.present()
    },
    func(Event e) {
        if e.is_quit() {
            guis.set_should_close(win, true)
        }
    }
)
```

---

## 窗口操作

### `guis.show(win)` / `guis.hide(win)`

显示或隐藏窗口。

```leno
guis.show(win)
guis.hide(win)
```

---

### `guis.set_title(win, title)`

设置窗口标题。

```leno
guis.set_title(win, "新标题")
```

---

### `guis.set_size(win, w, h)` / `guis.get_size(win)`

设置或获取窗口大小。`get_size` 返回 `[w, h]` 数组。

```leno
guis.set_size(win, 1024, 768)
var size = guis.get_size(win)
print("宽: " + size[0] + " 高: " + size[1])
```

---

### `guis.set_pos(win, x, y)` / `guis.get_pos(win)`

设置或获取窗口位置。`get_pos` 返回 `[x, y]` 数组。

```leno
guis.set_pos(win, 100, 100)
var pos = guis.get_pos(win)
```

---

### `guis.set_fullscreen(win, bool)`

设置窗口全屏状态。

```leno
guis.set_fullscreen(win, true)   // 全屏
guis.set_fullscreen(win, false)  // 退出全屏
```

---

### `guis.should_close(win)` / `guis.set_should_close(win, bool)`

查询或设置窗口关闭标志。

```leno
if guis.should_close(win) {
    print("窗口应该关闭")
}
guis.set_should_close(win, true)
```

---

### `guis.set_opacity(win, opacity)`

设置窗口透明度。

**参数**:
- `opacity` (float): 0.0（完全透明）~ 1.0（完全不透明）

```leno
guis.set_opacity(win, 0.85)
```

---

## 渲染器方法（Draw 实例方法）

渲染器通过 `guis.run()` 的 onDraw 回调参数获取，或通过 `guis.create_renderer(win)` 手动创建。

### `ren.set_color(r, g, b, a)`

设置绘制颜色。

**参数**:
- `r`, `g`, `b` (int): RGB 颜色分量 (0-255)
- `a` (int): Alpha 透明度 (0-255)

```leno
ren.set_color(255, 100, 50, 255)
```

---

### `ren.clear()`

用当前绘制颜色清除整个渲染缓冲区。

```leno
ren.set_color(30, 30, 46, 255)
ren.clear()
```

---

### `ren.present()`

将渲染缓冲区内容呈现到窗口。每帧绘制完成后必须调用。

```leno
ren.present()
```

---

### `ren.draw_point(x, y)`

绘制单个像素点。

```leno
ren.set_color(255, 0, 0, 255)
ren.draw_point(100, 100)
```

---

### `ren.draw_line(x1, y1, x2, y2)`

绘制直线。

```leno
ren.set_color(255, 255, 0, 255)
ren.draw_line(0, 0, 800, 600)
```

---

### `ren.draw_rect(x, y, w, h)`

绘制矩形边框。

```leno
ren.set_color(0, 255, 0, 255)
ren.draw_rect(50, 50, 200, 100)
```

---

### `ren.fill_rect(x, y, w, h)`

填充矩形。

```leno
ren.set_color(100, 100, 255, 255)
ren.fill_rect(50, 50, 200, 100)
```

---

### `ren.draw_circle(cx, cy, radius)`

绘制圆形边框（Bresenham 中点圆算法）。

```leno
ren.set_color(255, 100, 100, 255)
ren.draw_circle(400, 300, 80)
```

---

### `ren.fill_circle(cx, cy, radius)`

填充圆形（水平扫描线填充）。

```leno
ren.set_color(100, 255, 100, 255)
ren.fill_circle(400, 300, 60)
```

---

### `ren.draw_rounded_rect(x, y, w, h, radius)`

绘制圆角矩形边框。

```leno
ren.set_color(100, 150, 255, 255)
ren.draw_rounded_rect(50, 50, 200, 100, 15)
```

---

### `ren.fill_rounded_rect(x, y, w, h, radius)`

填充圆角矩形。

```leno
ren.set_color(180, 100, 255, 255)
ren.fill_rounded_rect(50, 50, 200, 100, 15)
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

### `ren.set_clip_rect(x, y, w, h)` / `ren.get_clip_rect()` / `ren.disable_clip_rect()`

设置、获取或禁用裁剪矩形。裁剪矩形限制绘制区域（物理坐标）。

```leno
ren.set_clip_rect(100, 100, 300, 200)
// 此区域外的绘制将被裁剪
ren.disable_clip_rect()
```

---

## 事件方法（Event 实例方法）

事件通过 `guis.run()` 的 onEvent 回调参数获取，或通过 `guis.poll()` 获取。

### 事件类型判断

| 方法 | 说明 |
|------|------|
| `e.is_quit()` | 是否为退出事件 |
| `e.is_window_close()` | 是否为窗口关闭事件 |
| `e.is_window_resize()` | 是否为窗口大小改变事件 |
| `e.is_key_down()` | 是否为按键按下事件 |
| `e.is_key_up()` | 是否为按键释放事件 |
| `e.is_text_input()` | 是否为文本输入事件 |
| `e.is_mouse_move()` | 是否为鼠标移动事件 |
| `e.is_mouse_down()` | 是否为鼠标按下事件 |
| `e.is_mouse_up()` | 是否为鼠标释放事件 |
| `e.is_mouse_wheel()` | 是否为鼠标滚轮事件 |

---

### 事件数据获取

| 方法 | 返回类型 | 说明 | 适用事件 |
|------|----------|------|----------|
| `e.type()` | int | 事件类型编号 | 所有事件 |
| `e.key()` | int | 按键码 | key_down / key_up |
| `e.mouse_x()` | int | 鼠标 X 坐标 | mouse_move / mouse_down / mouse_up |
| `e.mouse_y()` | int | 鼠标 Y 坐标 | mouse_move / mouse_down / mouse_up |
| `e.mouse_button()` | int | 鼠标按钮 (1=左 2=中 3=右) | mouse_down / mouse_up |
| `e.width()` | int | 窗口宽度 | window_resize |
| `e.height()` | int | 窗口高度 | window_resize |
| `e.text()` | string | 输入文本 | text_input |

### 常用按键码

| 按键 | 码值 | 按键 | 码值 |
|------|------|------|------|
| Escape | `0x1B` | Return | `0x0D` |
| Space | `0x20` | Backspace | `0x08` |
| Tab | `0x09` | Delete | `0x7F` |
| A-Z | `65-90` | 0-9 | `48-57` |

```leno
func(Event e) {
    if e.is_key_down() {
        var key = e.key()
        if key == 0x1B {
            guis.set_should_close(win, true)
        }
        print("key: " + _str(key))
    }
    if e.is_mouse_down() {
        print("mouse at: " + _str(e.mouse_x()) + ", " + _str(e.mouse_y()))
    }
    if e.is_window_resize() {
        print("resize: " + _str(e.width()) + "x" + _str(e.height()))
    }
}
```

---

## 纹理操作

### `guis.create_texture(ren, w, h)`

创建纹理（像素缓冲区）。

**返回**: `Texture` - 纹理对象

```leno
var tex = guis.create_texture(ren, 256, 256)
```

---

### `guis.destroy_texture(tex)`

销毁纹理。

```leno
guis.destroy_texture(tex)
```

---

### `ren.draw_texture(tex, x, y)`

将纹理整体绘制到渲染器目标位置。

```leno
ren.draw_texture(tex, 100, 100)
```

---

### `ren.draw_texture_src(tex, sx, sy, sw, sh, dx, dy, dw, dh)`

纹理源矩形渲染，从纹理中取子区域绘制到目标位置（可缩放）。

```leno
ren.draw_texture_src(tex, 0, 0, 64, 64, 100, 100, 128, 128)
```

---

### `ren.draw_texture_rotated(tex, x, y, angle, flip)`

纹理旋转/翻转渲染。

**参数**:
- `angle` (float): 旋转角度（度）
- `flip` (int): 翻转标志 (0=无, 1=水平, 2=垂直)

```leno
ren.draw_texture_rotated(tex, 400, 300, 45.0, 1)
```

---

### `ren.update_texture(tex, data, pitch)`

更新纹理像素数据。

```leno
ren.update_texture(tex, pixel_data, 256 * 4)
```

---

## 事件系统

### `guis.poll()`

轮询事件队列。非阻塞。

**返回**: `Event` 或 `null`（无事件时）

```leno
var ev = guis.poll()
if ev != null {
    // 处理事件
}
```

---

### `guis.wait(timeout_ms)`

等待事件（带超时）。阻塞直到有事件或超时。

**参数**:
- `timeout_ms` (int): 超时毫秒数

**返回**: `Event` 或 `null`（超时时）

```leno
var ev = guis.wait(1000)
```

---

## 回调式事件循环

`guis.run()` 是推荐的事件循环方式，内部自动处理：

1. 轮询并分发所有事件到 onEvent 回调
2. 检查窗口关闭标志
3. 自动调整渲染器大小（窗口 resize 时）
4. 调用 onDraw 回调进行渲染

```leno
guis.run(win,
    func(Draw ren) {
        // 每帧渲染
        ren.set_color(0, 0, 0, 255)
        ren.clear()
        // ... 绘制内容
        ren.present()
    },
    func(Event e) {
        // 事件处理
        if e.is_quit() or e.is_window_close() {
            guis.set_should_close(win, true)
        }
        if e.is_key_down() and e.key() == 0x1B {
            guis.set_should_close(win, true)
        }
    }
)
```

### 手动事件循环（高级用法）

如果不使用 `guis.run()`，可以手动管理渲染器和事件循环：

```leno
Win win = guis.create_window("App", 800, 600, 1)
Draw ren = guis.create_renderer(win)

while not guis.should_close(win) {
    var ev = guis.poll()
    while ev != null {
        // 处理事件
        if ev.is_quit() {
            guis.set_should_close(win, true)
        }
        ev = guis.poll()
    }

    ren.set_color(0, 0, 0, 255)
    ren.clear()
    ren.present()
}

guis.destroy_renderer(ren)
guis.cleanup(win)
```

---

## 输入状态查询

### `guis.get_key(key)`

查询指定按键是否按下。

**参数**:
- `key` (int): 按键码

**返回**: `bool`

```leno
if guis.get_key(0x20) {
    print("空格键正在按下")
}
```

---

### `guis.get_mouse()`

查询鼠标状态。

**返回**: `{x, y, buttons}` - 鼠标位置和按钮状态

```leno
var mouse = guis.get_mouse()
print("鼠标: " + mouse.x + ", " + mouse.y)
if mouse.buttons & 1 {
    print("左键按下")
}
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

### `guis.delay(ms)`

延迟指定毫秒数。

```leno
guis.delay(16)  // 延迟约 16ms（约 60 FPS）
```

---

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
    Win win = guis.create_window("Hello LenoC", 800, 600, 1)

    guis.run(win,
        func(Draw ren) {
            ren.set_color(30, 30, 46, 255)
            ren.clear()
            ren.set_color(255, 100, 100, 255)
            ren.fill_rect(100, 100, 200, 150)
            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
        }
    )

    guis.cleanup(win)
}
```

---

### 示例2：FPS 计数器

```leno
import guis

main() {
    Win win = guis.create_window("FPS Counter", 800, 600, 1)

    var frame = 0
    var start_ticks = guis.get_ticks()

    guis.run(win,
        func(Draw ren) {
            ren.set_color(0, 0, 0, 255)
            ren.clear()

            frame = frame + 1
            var elapsed = guis.get_ticks() - start_ticks
            if elapsed > 0 and frame % 60 == 0 {
                var fps = frame * 1000 / elapsed
                print("FPS: " + _str(fps))
            }

            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
        }
    )

    guis.cleanup(win)
}
```

---

### 示例3：鼠标交互

```leno
import guis

main() {
    Win win = guis.create_window("Mouse Demo", 800, 600, 1)

    var mx = 0
    var my = 0
    var clicked = false

    guis.run(win,
        func(Draw ren) {
            ren.set_color(20, 20, 30, 255)
            ren.clear()

            if clicked {
                ren.set_color(255, 100, 100, 255)
            } else {
                ren.set_color(100, 255, 100, 255)
            }
            ren.fill_circle(mx, my, 30)

            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
            if e.is_mouse_move() {
                mx = e.mouse_x()
                my = e.mouse_y()
            }
            if e.is_mouse_down() {
                clicked = true
            }
            if e.is_mouse_up() {
                clicked = false
            }
        }
    )

    guis.cleanup(win)
}
```

---

### 示例4：视口与裁剪

```leno
import guis

main() {
    Win win = guis.create_window("Viewport & Clip", 800, 600, 1)

    guis.run(win,
        func(Draw ren) {
            ren.set_color(30, 30, 46, 255)
            ren.clear()

            ren.set_viewport(50, 50, 200, 200)
            ren.set_color(60, 60, 90, 255)
            ren.fill_rect(0, 0, 200, 200)
            ren.set_color(255, 255, 100, 255)
            ren.draw_rect(10, 10, 180, 180)

            var rs = ren.get_size()
            ren.set_viewport(0, 0, rs[0], rs[1])

            ren.set_clip_rect(550, 50, 200, 200)
            ren.set_color(80, 40, 40, 255)
            ren.fill_rect(500, 0, 350, 350)
            ren.disable_clip_rect()

            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
        }
    )

    guis.cleanup(win)
}
```

---

### 示例5：综合绘图

```leno
import guis

main() {
    Win win = guis.create_window("LenoC GUI - Draw Test", 800, 600, 1)

    guis.set_opacity(win, 0.95)

    guis.run(win,
        func(Draw ren) {
            ren.set_color(30, 30, 46, 255)
            ren.clear()

            ren.set_color(255, 100, 100, 255)
            ren.draw_circle(400, 150, 80)

            ren.set_color(100, 255, 100, 255)
            ren.fill_circle(400, 350, 60)

            ren.set_color(100, 150, 255, 255)
            ren.draw_rounded_rect(50, 350, 200, 100, 20)

            ren.set_color(180, 100, 255, 255)
            ren.fill_rounded_rect(550, 350, 200, 100, 15)

            ren.set_color(255, 255, 0, 255)
            ren.draw_line(300, 450, 500, 550)

            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                guis.set_should_close(win, true)
            }
            if e.is_key_down() {
                var key = e.key()
                if key == 0x1B {
                    guis.set_should_close(win, true)
                }
            }
        }
    )

    guis.cleanup(win)
    print("test done!")
}
```

---

## 注意事项

### 1. 初始化顺序

`create_window` 会自动初始化 GUI 子系统，无需手动调用初始化。

```leno
var win = guis.create_window("App", 800, 600, 1)
```

### 2. 渲染循环

每帧必须按 `clear()` → 绘制 → `present()` 的顺序操作。缺少 `present()` 会导致画面不更新。

```leno
ren.set_color(0, 0, 0, 255)
ren.clear()           // 1. 清除
ren.fill_rect(...)    // 2. 绘制
ren.present()         // 3. 呈现
```

### 3. 窗口大小变化

使用 `guis.run()` 时，渲染器会自动调整大小。手动管理时需要调用 `guis.resize_renderer()` 处理窗口 resize 事件。

### 4. 像素格式

使用 32 位 ARGB 格式（内存中为 BGRA 小端序），颜色分量范围 0-255。

### 5. 坐标系统

- 窗口左上角为原点 (0, 0)
- X 轴向右递增，Y 轴向下递增
- 视口偏移影响所有绘制坐标

### 6. 事件处理

在 `guis.run()` 的事件回调中，必须处理 `is_quit()` 和 `is_window_close()` 事件并设置窗口关闭标志，否则窗口无法正常关闭。

---

## 最佳实践

1. **使用 `guis.run()` 管理事件循环**
   ```leno
   guis.run(win, onDraw, onEvent)
   ```

2. **始终处理退出事件**
   ```leno
   func(Event e) {
       if e.is_quit() or e.is_window_close() {
           guis.set_should_close(win, true)
       }
   }
   ```

3. **使用 `guis.cleanup()` 清理资源**
   ```leno
   guis.cleanup(win)
   ```

4. **合理控制帧率**
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

5. **使用视口简化局部绘制**
   ```leno
   ren.set_viewport(x, y, w, h)
   // 在视口内使用相对坐标绘制
   ren.fill_rect(0, 0, w, h)
   // 恢复全屏视口
   var size = ren.get_size()
   ren.set_viewport(0, 0, size[0], size[1])
   ```

6. **使用裁剪矩形限制绘制区域**
   ```leno
   ren.set_clip_rect(x, y, w, h)
   // 绘制内容不会超出裁剪区域
   ren.disable_clip_rect()
   ```

---

*文档版本: 1.0*  
*最后更新: 2026-05-30*
