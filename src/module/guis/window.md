# LenoC GUI Window 模块

本文档详细说明 `GWin` 窗口对象提供的所有操作。

## 目录

- [概述](#概述)
- [创建窗口](#创建窗口)
- [窗口控制](#窗口控制)
- [窗口属性](#窗口属性)
- [事件循环](#事件循环)
- [拖拽区域](#拖拽区域)
- [示例代码](#示例代码)

---

## 概述

`GWin` 是 LenoC GUI 模块的窗口对象，封装了跨平台的窗口管理功能。

### 类型定义

```leno
type GWin  // 窗口对象类型
```

---

## 创建窗口

### `guis.create_window(title, style)`

创建一个新窗口。

**参数**:
- `title` (string): 窗口标题
- `style` (Dict | Style[window]): 窗口样式

**样式选项**:
| 字段 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `width` | int | 800 | 窗口宽度 |
| `height` | int | 600 | 窗口高度 |
| `x` | int | 居中 | 窗口位置X |
| `y` | int | 居中 | 窗口位置Y |
| `resizable` | bool | true | 是否可拖拽边缘调整大小 |
| `maximizable` | bool | true | 是否显示最大化按钮（独立于 resizable） |
| `fullscreen` | bool | false | 是否全屏 |
| `borderless` | bool | false | 是否无边框 |
| `hidden` | bool | false | 初始是否隐藏 |
| `always_on_top` | bool | false | 是否置顶 |
| `opacity` | float | 1.0 | 透明度 0.0~1.0 |
| `visible` | bool | true | 初始是否可见 |

**返回**: `GWin` - 窗口对象

```leno
import guis

main() {
    // 方式一：使用 Style[window]（推荐，支持IDE补全）
    Style[window] w_style = {
        width: 1024,
        height: 768,
        resizable: true,
        opacity: 0.95
    }
    GWin win = guis.create_window("My Application", w_style)
    
    // 方式二：使用字典（简洁）
    GWin win2 = guis.create_window("Quick Window", {
        width: 800,
        height: 600,
        fullscreen: false
    })
}
```

---

## 窗口控制

### `win.show()`

显示窗口。

```leno
win.show()
```

---

### `win.hide()`

隐藏窗口。窗口仍在内存中，可再次显示。

```leno
win.hide()
```

---

### `win.close()`

关闭并销毁窗口，释放所有资源。

**注意**: 调用后会退出 GUI 子系统。

```leno
win.close()
```

---

### `win.set_should_close(bool)`

设置窗口关闭标志。事件循环检测到后会退出。

```leno
win.set_should_close(true)   // 请求关闭窗口
```

---

### `win.should_close()` -> bool

查询窗口是否应该关闭。

**返回**: `bool` - 是否设置了关闭标志

```leno
if win.should_close() {
    print("窗口即将关闭")
}
```

---

## 窗口属性

### `win.set_title(title)`

设置窗口标题。

**参数**:
- `title` (string): 新标题

```leno
win.set_title("新标题 - " + filename)
```

---

### `win.set_size(w, h)`

设置窗口大小。

**参数**:
- `w`, `h` (int): 宽度和高度

```leno
win.set_size(1920, 1080)
```

---

### `win.get_size()` -> [w, h]

获取窗口当前大小。

**返回**: `[w, h]` - 宽度和高度数组

```leno
var size = win.get_size()
print("窗口大小: " + size[0] + "x" + size[1])
```

---

### `win.set_pos(x, y)`

设置窗口位置。

**参数**:
- `x`, `y` (int): 屏幕坐标

```leno
win.set_pos(100, 100)   // 移动到屏幕左上角 (100,100)
```

---

### `win.get_pos()` -> [x, y]

获取窗口当前位置。

**返回**: `[x, y]` - 屏幕坐标数组

```leno
var pos = win.get_pos()
print("窗口位置: (" + pos[0] + ", " + pos[1] + ")")
```

---

### `win.set_fullscreen(bool)`

设置全屏模式。

**参数**:
- `fullscreen` (bool): true=全屏, false=窗口模式

```leno
win.set_fullscreen(true)   // 进入全屏
win.set_fullscreen(false)  // 退出全屏
```

---

### `win.set_opacity(opacity)`

设置窗口透明度。

**参数**:
- `opacity` (float): 0.0（完全透明）~ 1.0（完全不透明）

```leno
win.set_opacity(0.8)   // 80% 不透明
```

---

## 事件循环

### `win.run(on_draw, on_GEvent)`

运行窗口事件循环。这是 GUI 应用的核心机制。

**参数**:
- `on_draw` (func(GDraw)): 渲染回调，每帧调用
- `on_event` (func(GEvent)): 事件回调，每个事件调用

**工作流程**:
1. 创建渲染器
2. 循环处理事件（调用 on_event）
3. 调用 on_draw 进行渲染
4. 检查关闭标志，决定是否退出

```leno
win.run(
    func(GDraw ren) {
        // 每帧渲染
        ren.set_color(_rgb(30, 30, 46, 255))
        ren.clear()
        
        // 绘制内容...
        ren.set_color(_rgb(255, 255, 255, 255))
        ren.fill_rect(100, 100, 200, 150)
    },
    func(GEvent e) {
        // 事件处理
        if e.quit() or e.window_close() {
            win.set_should_close(true)
        }
        if e.key_down() and e.key() == 0x1B {  // ESC键
            win.set_should_close(true)
        }
    }
)
```

---

## 拖拽区域

### `win.set_drag_area(x, y, w, h)`

设置窗口的客户区拖拽区域。在此区域内的鼠标拖拽会移动窗口（用于无边框窗口）。

**参数**:
- `x`, `y`, `w`, `h` (int): 拖拽区域矩形

```leno
// 设置顶部 40 像素为拖拽区域（模拟标题栏）
win.set_drag_area(0, 0, 800, 40)
```

---

### `win.clear_drag_area()`

清除拖拽区域设置。

```leno
win.clear_drag_area()
```

---

## 示例代码

### 基础窗口示例

```leno
import guis

main() {
    Style[window] style = {
        width: 800,
        height: 600,
        resizable: true
    }
    GWin win = guis.create_window("基础窗口", style)
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.clear()
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

### 无边框可拖拽窗口

```leno
import guis

main() {
    GWin win = guis.create_window("无边框窗口", {
        width: 600,
        height: 400,
        borderless: true,
        resizable: false
    })
    
    // 设置顶部为拖拽区域
    win.set_drag_area(0, 0, 600, 40)
    
    win.run(
        func(GDraw ren) {
            // 绘制自定义标题栏
            ren.set_color(_rgb(40, 40, 60, 255))
            ren.fill_rect(0, 0, 600, 40)
            
            // 绘制内容区域
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.fill_rect(0, 40, 600, 360)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
            // 双击标题栏关闭
            if e.mouse_down() and e.mouse_y() < 40 and e.mouse_clicks() == 2 {
                win.set_should_close(true)
            }
        }
    )
    
    win.close()
}
```

---

### 响应窗口大小变化

```leno
import guis

main() {
    GWin win = guis.create_window("自适应窗口", {
        width: 800,
        height: 600,
        resizable: true
    })
    
    var win_w = 800
    var win_h = 600
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()
            
            // 居中绘制矩形
            var rect_w = 200
            var rect_h = 150
            var x = (win_w - rect_w) / 2
            var y = (win_h - rect_h) / 2
            
            ren.set_color(_rgb(100, 150, 255, 255))
            ren.fill_rect(x, y, rect_w, rect_h)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
            // 窗口大小改变时更新尺寸
            if e.window_resize() {
                win_w = e.width()
                win_h = e.height()
            }
        }
    )
    
    win.close()
}
```

---

### 全屏切换

```leno
import guis

main() {
    GWin win = guis.create_window("全屏演示", {
        width: 800,
        height: 600,
        resizable: true
    })
    
    var is_fullscreen = false
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(0, 0, 0, 255))
            ren.clear()
            
            ren.set_color(_rgb(255, 255, 255, 255))
            var size = win.get_size()
            ren.fill_rect(size[0]/2 - 50, size[1]/2 - 50, 100, 100)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
            // F11 切换全屏
            if e.key_down() and e.key() == 0x7A {  // F11
                is_fullscreen = !is_fullscreen
                win.set_fullscreen(is_fullscreen)
            }
            // ESC 退出全屏
            if e.key_down() and e.key() == 0x1B and is_fullscreen {
                is_fullscreen = false
                win.set_fullscreen(false)
            }
        }
    )
    
    win.close()
}
```

---

## 方法速查表

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `show()` | - | null | 显示窗口 |
| `hide()` | - | null | 隐藏窗口 |
| `close()` | - | null | 关闭窗口 |
| `set_title(s)` | string | null | 设置标题 |
| `set_size(w,h)` | int, int | null | 设置大小 |
| `get_size()` | - | [w,h] | 获取大小 |
| `set_pos(x,y)` | int, int | null | 设置位置 |
| `get_pos()` | - | [x,y] | 获取位置 |
| `set_fullscreen(b)` | bool | null | 设置全屏 |
| `set_opacity(f)` | float | null | 设置透明度 |
| `set_should_close(b)` | bool | null | 设置关闭标志 |
| `should_close()` | - | bool | 查询关闭标志 |
| `set_drag_area(x,y,w,h)` | int x4 | null | 设置拖拽区域 |
| `clear_drag_area()` | - | null | 清除拖拽区域 |
| `run(GDraw, GEvent)` | func, func | null | 运行事件循环 |
