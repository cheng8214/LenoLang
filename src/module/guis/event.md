# LenoC GUI Event 模块

本文档详细说明 `Event` 事件对象提供的所有操作。

## 目录

- [概述](#概述)
- [事件类型判断](#事件类型判断)
- [键盘事件](#键盘事件)
- [鼠标事件](#鼠标事件)
- [窗口事件](#窗口事件)
- [通用事件数据](#通用事件数据)
- [常用按键码](#常用按键码)
- [修饰键标志](#修饰键标志)
- [示例代码](#示例代码)

---

## 概述

`Event` 是 LenoC GUI 模块的事件对象，封装了所有用户输入和窗口状态变化事件。

### 获取 Event 对象

事件通过 `win.run()` 的回调函数获取：

```leno
win.run(
    func(Draw ren) {
        // 渲染回调
    },
    func(Event e) {
        // 事件回调 - e 就是 Event 对象
        if e.is_key_down() {
            print("按键: " + e.key())
        }
    }
)
```

---

## 事件类型判断

### 窗口相关

| 方法 | 说明 |
|------|------|
| `e.is_quit()` | 程序退出请求 |
| `e.is_window_close()` | 窗口关闭按钮被点击 |
| `e.is_window_resize()` | 窗口大小改变 |
| `e.is_window_move()` | 窗口位置移动 |
| `e.is_window_focus()` | 窗口获得焦点 |
| `e.is_window_unfocus()` | 窗口失去焦点 |
| `e.is_window_show()` | 窗口显示 |
| `e.is_window_hide()` | 窗口隐藏 |
| `e.is_window_exposed()` | 窗口暴露（需要重绘） |
| `e.is_window_minimized()` | 窗口最小化 |
| `e.is_window_maximized()` | 窗口最大化 |
| `e.is_window_restored()` | 窗口从最小化/最大化恢复 |

### 键盘相关

| 方法 | 说明 |
|------|------|
| `e.is_key_down()` | 按键按下 |
| `e.is_key_up()` | 按键释放 |
| `e.is_text_input()` | 文本输入（字符） |

### 鼠标相关

| 方法 | 说明 |
|------|------|
| `e.is_mouse_move()` | 鼠标移动 |
| `e.is_mouse_down()` | 鼠标按钮按下 |
| `e.is_mouse_up()` | 鼠标按钮释放 |
| `e.is_mouse_wheel()` | 鼠标滚轮滚动 |

---

## 键盘事件

### `e.key()` -> int

获取按键码（虚拟键码）。

**返回**: `int` - 按键码

```leno
if e.is_key_down() {
    var key = e.key()
    if key == 0x1B {           // ESC
        win.set_should_close(true)
    }
    if key == 0x20 {           // 空格
        jump()
    }
}
```

---

### `e.scancode()` -> int

获取按键扫描码（物理位置，与键盘布局无关）。

**返回**: `int` - 扫描码

```leno
// 使用扫描码处理 WASD（不受键盘布局影响）
if e.is_key_down() {
    var scancode = e.scancode()
    if scancode == 17 { move_forward() }   // W位置
    if scancode == 31 { move_backward() }  // S位置
}
```

---

### `e.mod()` -> int

获取修饰键状态标志。

**返回**: `int` - 修饰键位掩码

```leno
if e.is_key_down() {
    var mod = e.mod()
    if e.key() == 0x5A {  // Z键
        if mod & 0x01 {   // Ctrl
            undo()
        }
        if mod & 0x02 {   // Shift
            redo()
        }
    }
}
```

---

### `e.repeat()` -> bool

判断是否为重复按键（长按自动重复）。

**返回**: `bool` - true=重复按键

```leno
if e.is_key_down() {
    if !e.repeat() {   // 只处理首次按下
        fire()
    }
}
```

---

### `e.text()` -> string

获取输入的文本字符（text_input 事件）。

**返回**: `string` - 输入字符

```leno
if e.is_text_input() {
    var ch = e.text()
    input_buffer = input_buffer + ch
}
```

---

## 鼠标事件

### `e.mouse_x()` / `e.mouse_y()` -> int

获取鼠标坐标（相对于窗口客户区）。

**返回**: `int` - X或Y坐标

```leno
if e.is_mouse_move() {
    var x = e.mouse_x()
    var y = e.mouse_y()
    print("鼠标位置: (" + x + ", " + y + ")")
}
```

---

### `e.mouse_button()` -> int

获取鼠标按钮编号。

**返回**: `int` - 1=左键, 2=中键, 3=右键

```leno
if e.is_mouse_down() {
    var btn = e.mouse_button()
    if btn == 1 { select() }      // 左键
    if btn == 2 { pan() }         // 中键
    if btn == 3 { context_menu() } // 右键
}
```

---

### `e.clicks()` -> int

获取点击次数（用于双击检测）。

**返回**: `int` - 1=单击, 2=双击

```leno
if e.is_mouse_down() {
    if e.clicks() == 2 {   // 双击
        open_file()
    } else {               // 单击
        select()
    }
}
```

---

### `e.xrel()` / `e.yrel()` -> int

获取鼠标相对移动量（仅 mouse_move 事件）。

**返回**: `int` - 相对移动像素数

```leno
if e.is_mouse_move() {
    var dx = e.xrel()
    var dy = e.yrel()
    camera.rotate(dx * 0.1, dy * 0.1)
}
```

---

### `e.wheel_x()` / `e.wheel_y()` -> int

获取滚轮滚动量（仅 mouse_wheel 事件）。

**返回**: `int` - 滚动量（正=向上/右，负=向下/左）

```leno
if e.is_mouse_wheel() {
    var dy = e.wheel_y()
    if dy > 0 { zoom_in() }
    if dy < 0 { zoom_out() }
}
```

---

## 窗口事件

### `e.width()` / `e.height()` -> int

获取窗口新尺寸（仅 window_resize 事件）。

**返回**: `int` - 新宽度或高度

```leno
if e.is_window_resize() {
    var w = e.width()
    var h = e.height()
    resize_viewport(w, h)
}
```

---

## 通用事件数据

### `e.type()` -> int

获取事件类型编号。

**返回**: `int` - 事件类型码

```leno
var type = e.type()
print("事件类型: " + type)
```

---

### `e.window_id()` -> int

获取窗口 ID（多窗口应用使用）。

**返回**: `int` - 窗口标识符

```leno
var wid = e.window_id()
if wid == main_window_id {
    handle_main_window_event(e)
}
```

---

## 按键常量（推荐）

guis 模块提供了一系列按键常量，使用常量比直接使用数字更清晰：

```leno
if e.is_key_down() and e.key() == guis.KEY_SPACE {
    jump()
}

if e.is_key_down() and e.key() == guis.KEY_ESCAPE {
    win.set_should_close(true)
}
```

### 功能键常量

| 常量 | 说明 |
|------|------|
| `guis.KEY_UNKNOWN` | 未知按键 |
| `guis.KEY_RETURN` | 回车键 |
| `guis.KEY_ESCAPE` | ESC键 |
| `guis.KEY_BACKSPACE` | 退格键 |
| `guis.KEY_TAB` | Tab键 |
| `guis.KEY_SPACE` | 空格键 |
| `guis.KEY_DELETE` | Delete键 |
| `guis.KEY_INSERT` | Insert键 |
| `guis.KEY_HOME` | Home键 |
| `guis.KEY_END` | End键 |
| `guis.KEY_PAGEUP` | Page Up |
| `guis.KEY_PAGEDOWN` | Page Down |

### 方向键常量

| 常量 | 说明 |
|------|------|
| `guis.KEY_LEFT` | 左方向键 |
| `guis.KEY_RIGHT` | 右方向键 |
| `guis.KEY_UP` | 上方向键 |
| `guis.KEY_DOWN` | 下方向键 |

### 功能键常量 F1-F12

| 常量 | 说明 |
|------|------|
| `guis.KEY_F1` ~ `guis.KEY_F12` | F1 到 F12 |

### 修饰键常量

| 常量 | 说明 |
|------|------|
| `guis.KEY_LSHIFT` | 左Shift |
| `guis.KEY_RSHIFT` | 右Shift |
| `guis.KEY_LCTRL` | 左Ctrl |
| `guis.KEY_RCTRL` | 右Ctrl |
| `guis.KEY_LALT` | 左Alt |
| `guis.KEY_RALT` | 右Alt |
| `guis.KEY_CAPSLOCK` | Caps Lock |
| `guis.KEY_NUMLOCK` | Num Lock |

### 鼠标按钮常量

| 常量 | 说明 |
|------|------|
| `guis.MOUSE_LEFT` | 左键 (1) |
| `guis.MOUSE_MIDDLE` | 中键 (2) |
| `guis.MOUSE_RIGHT` | 右键 (3) |

---

## 事件类型常量

用于 `e.type()` 比较：

| 常量 | 说明 |
|------|------|
| `guis.EVT_QUIT` | 程序退出 |
| `guis.EVT_WINDOW_CLOSE` | 窗口关闭 |
| `guis.EVT_WINDOW_RESIZE` | 窗口大小改变 |
| `guis.EVT_WINDOW_MOVE` | 窗口移动 |
| `guis.EVT_KEY_DOWN` | 按键按下 |
| `guis.EVT_KEY_UP` | 按键释放 |
| `guis.EVT_MOUSE_DOWN` | 鼠标按下 |
| `guis.EVT_MOUSE_UP` | 鼠标释放 |
| `guis.EVT_MOUSE_MOVE` | 鼠标移动 |
| `guis.EVT_MOUSE_WHEEL` | 鼠标滚轮 |

---

## 修饰键标志

`e.mod()` 返回的位掩码：

| 常量 | 值 | 说明 |
|------|-----|------|
| `guis.MOD_CTRL` | 1 | Ctrl 按下 |
| `guis.MOD_SHIFT` | 2 | Shift 按下 |
| `guis.MOD_ALT` | 4 | Alt 按下 |
| `guis.MOD_SUPER` | 8 | Win/Cmd 按下 |

```leno
if e.is_key_down() {
    var mod = e.mod()
    if e.key() == guis.KEY_Z {
        if mod & guis.MOD_CTRL {
            undo()  // Ctrl+Z
        }
        if mod & guis.MOD_SHIFT {
            redo()  // Shift+Z (或 Ctrl+Shift+Z)
        }
    }
}
```

---

## 示例代码

### 完整事件处理示例

```leno
import guis

main() {
    Win win = guis.create_window("Event Demo", {width: 800, height: 600})
    
    var mouse_x = 0
    var mouse_y = 0
    var key_log = ""
    
    win.run(
        func(Draw ren) {
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.clear()
            
            // 显示鼠标位置
            ren.set_color(_rgb(255, 255, 255, 255))
            // ren.draw_text(font, "Mouse: (" + mouse_x + ", " + mouse_y + ")", 10, 10)
            
            // 显示按键记录
            // ren.draw_text(font, "Keys: " + key_log, 10, 40)
            
            ren.present()
        },
        func(Event e) {
            // 退出处理
            if e.is_quit() or e.is_window_close() {
                win.set_should_close(true)
            }
            
            // 键盘事件
            if e.is_key_down() {
                var key = e.key()
                
                // ESC 退出
                if key == 0x1B {
                    win.set_should_close(true)
                }
                
                // F11 切换全屏
                if key == 0x7A {
                    // toggle_fullscreen()
                }
                
                // 记录按键
                key_log = key_log + " " + key
                if key_log.len() > 30 {
                    key_log = ""
                }
            }
            
            // 鼠标移动
            if e.is_mouse_move() {
                mouse_x = e.mouse_x()
                mouse_y = e.mouse_y()
            }
            
            // 鼠标点击
            if e.is_mouse_down() {
                var btn = e.mouse_button()
                var clicks = e.clicks()
                
                if btn == 1 and clicks == 2 {
                    print("左键双击!")
                }
            }
            
            // 滚轮
            if e.is_mouse_wheel() {
                var dy = e.wheel_y()
                print("滚轮: " + dy)
            }
            
            // 窗口大小改变
            if e.is_window_resize() {
                print("新大小: " + e.width() + "x" + e.height())
            }
            
            // 窗口焦点变化
            if e.is_window_focus() {
                print("窗口获得焦点")
            }
            if e.is_window_unfocus() {
                print("窗口失去焦点")
            }
        }
    )
    
    win.close()
}
```

---

### 拖拽移动示例

```leno
import guis

main() {
    Win win = guis.create_window("Drag Demo", {width: 800, height: 600})
    
    var dragging = false
    var drag_start_x = 0
    var drag_start_y = 0
    var win_start_x = 0
    var win_start_y = 0
    
    win.run(
        func(Draw ren) {
            ren.set_color(_rgb(40, 40, 60, 255))
            ren.clear()
            
            // 绘制可拖拽区域
            ren.set_color(_rgb(100, 150, 255, 255))
            ren.fill_rect(0, 0, 800, 40)
            
            ren.present()
        },
        func(Event e) {
            if e.is_quit() {
                win.set_should_close(true)
            }
            
            // 在标题栏区域按下左键开始拖拽
            if e.is_mouse_down() and e.mouse_button() == 1 {
                if e.mouse_y() < 40 {
                    dragging = true
                    drag_start_x = e.mouse_x()
                    drag_start_y = e.mouse_y()
                    var pos = win.get_pos()
                    win_start_x = pos[0]
                    win_start_y = pos[1]
                }
            }
            
            // 释放鼠标结束拖拽
            if e.is_mouse_up() {
                dragging = false
            }
            
            // 拖拽中移动窗口
            if dragging and e.is_mouse_move() {
                var dx = e.mouse_x() - drag_start_x
                var dy = e.mouse_y() - drag_start_y
                win.set_pos(win_start_x + dx, win_start_y + dy)
            }
        }
    )
    
    win.close()
}
```

---

### 输入框示例

```leno
import guis

main() {
    Win win = guis.create_window("Input Demo", {width: 400, height: 200})
    
    var text = ""
    var cursor_visible = true
    var cursor_timer = 0
    
    win.run(
        func(Draw ren) {
            ren.set_color(_rgb(30, 30, 30, 255))
            ren.clear()
            
            // 绘制输入框背景
            ren.set_color(_rgb(60, 60, 60, 255))
            ren.fill_rect(20, 80, 360, 40)
            
            // 绘制文本
            ren.set_color(_rgb(255, 255, 255, 255))
            // ren.draw_text(font, text, 30, 90)
            
            // 绘制光标
            cursor_timer = cursor_timer + 1
            if cursor_timer > 30 {
                cursor_timer = 0
                cursor_visible = !cursor_visible
            }
            if cursor_visible {
                // var text_w = ren.text_size(font, text)[0]
                // ren.fill_rect(30 + text_w, 90, 2, 20)
            }
            
            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                win.set_should_close(true)
            }
            
            // 处理文本输入
            if e.is_text_input() {
                text = text + e.text()
            }
            
            // 退格删除
            if e.is_key_down() and e.key() == 0x08 {
                if text.len() > 0 {
                    text = text[0:text.len()-1]
                }
            }
            
            // Ctrl+A 全选（示例）
            if e.is_key_down() and e.key() == 0x41 {
                if e.mod() & 0x01 {  // Ctrl
                    print("全选: " + text)
                }
            }
        }
    )
    
    win.close()
}
```

---

## 方法速查表

| 方法 | 返回值 | 适用事件 |
|------|--------|----------|
| `is_quit()` | bool | 所有 |
| `is_window_close()` | bool | 所有 |
| `is_window_resize()` | bool | 所有 |
| `is_window_move()` | bool | 所有 |
| `is_window_focus()` | bool | 所有 |
| `is_window_unfocus()` | bool | 所有 |
| `is_key_down()` | bool | 所有 |
| `is_key_up()` | bool | 所有 |
| `is_text_input()` | bool | 所有 |
| `is_mouse_move()` | bool | 所有 |
| `is_mouse_down()` | bool | 所有 |
| `is_mouse_up()` | bool | 所有 |
| `is_mouse_wheel()` | bool | 所有 |
| `type()` | int | 所有 |
| `window_id()` | int | 所有 |
| `key()` | int | key_down/key_up |
| `scancode()` | int | key_down/key_up |
| `mod()` | int | key_down/key_up |
| `repeat()` | bool | key_down |
| `text()` | string | text_input |
| `mouse_x()` | int | 鼠标事件 |
| `mouse_y()` | int | 鼠标事件 |
| `mouse_button()` | int | mouse_down/mouse_up |
| `clicks()` | int | mouse_down/mouse_up |
| `xrel()` | int | mouse_move |
| `yrel()` | int | mouse_move |
| `wheel_x()` | int | mouse_wheel |
| `wheel_y()` | int | mouse_wheel |
| `width()` | int | window_resize |
| `height()` | int | window_resize |
