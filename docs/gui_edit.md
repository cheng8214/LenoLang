# GEdit 文本编辑控件

`GEdit`（文本框）用于输入和编辑单行或多行文本，支持中文输入、Undo/Redo、选区、滚动条、密码模式、占位符等。

## 创建文本框

```leno
import gui

var win = gui.window("文本框示例", 800, 600)
var edit = win.add_edit({
    x: 50,
    y: 50,
    w: 300,
    h: 32,
    text: "",
    placeholder: "请输入内容...",
    font_size: 14
})

win.show()
gui.run()
```

## 常用方法

### 文本操作

| 方法 | 说明 | 示例 |
|------|------|------|
| `set_text(text)` | 设置文本（不进入撤销栈） | `edit.set_text("hello")` |
| `get_text()` | 获取当前文本 | `var t = edit.get_text()` |
| `set_placeholder(text)` | 设置占位符文字 | `edit.set_placeholder("提示文字")` |
| `set_placeholder_color(color)` | 设置占位符颜色 | `edit.set_placeholder_color(0x999999)` |
| `set_placeholder_font_size(size)` | 设置占位符字体大小 | `edit.set_placeholder_font_size(12)` |
| `set_password(bool)` | 是否密码模式 | `edit.set_password(true)` |
| `set_max_length(n)` | 最大输入长度 | `edit.set_max_length(100)` |

### 事件

| 方法 | 说明 | 示例 |
|------|------|------|
| `on_change(cb)` | 文本变化时回调 | `edit.on_change(fn() { print("changed") })` |
| `on_submit(cb)` | 按回车时回调 | `edit.on_submit(fn() { print("submit") })` |

### 样式

| 方法 | 说明 | 示例 |
|------|------|------|
| `set_text_color(color)` | 文字颜色 | `edit.set_text_color(0x333333)` |
| `set_bg_color(color)` | 背景颜色 | `edit.set_bg_color(0xFFFFFF)` |
| `set_border_color(color)` | 边框颜色 | `edit.set_border_color(0xCCCCCC)` |
| `set_focus_color(color)` | 聚焦时边框颜色 | `edit.set_focus_color(0x409EFF)` |
| `set_cursor_color(color)` | 光标颜色 | `edit.set_cursor_color(0x000000)` |
| `set_selection_color(color)` | 选区背景颜色 | `edit.set_selection_color(0x409EFF)` |
| `set_border_width(n)` | 边框宽度 | `edit.set_border_width(1)` |
| `set_radius(n)` | 圆角半径 | `edit.set_radius(4)` |
| `set_font_size(n)` | 字体大小 | `edit.set_font_size(14)` |
| `set_letter_spacing(n)` | 字间距 | `edit.set_letter_spacing(0)` |
| `set_padding(x, y)` | 内边距 | `edit.set_padding(8, 6)` |
| `set_pos(x, y)` | 设置位置 | `edit.set_pos(50, 100)` |
| `set_size(w, h)` | 设置尺寸 | `edit.set_size(300, 120)` |
| `set_enabled(bool)` | 是否可编辑 | `edit.set_enabled(false)` |
| `set_anchor(...)` | 设置锚点 | `edit.set_anchor(0, 0, 1, 0)` |

### 滚动条样式

| 方法 | 说明 | 示例 |
|------|------|------|
| `set_sb_track(color)` | 滚动条轨道颜色 | `edit.set_sb_track(0xE0E0E0)` |
| `set_sb_thumb(color)` | 滑块颜色 | `edit.set_sb_thumb(0x909399)` |
| `set_sb_thumb_hover(color)` | 滑块悬停颜色 | `edit.set_sb_thumb_hover(0xA6A9AD)` |
| `set_sb_thumb_press(color)` | 滑块按下颜色 | `edit.set_sb_thumb_press(0x606266)` |

### 选区与查找

| 方法 | 说明 | 示例 |
|------|------|------|
| `select(start, end)` | 选中文本范围 | `edit.select(0, 5)` |
| `find(text, start)` | 查找文本位置 | `var pos = edit.find("abc", 0)` |
| `set_range_color(start, len, color)` | 给一段文本设置颜色 | `edit.set_range_color(0, 5, 0xFF0000)` |
| `clear_colors()` | 清除手动设置的颜色 | `edit.clear_colors()` |

## 快捷键

| 快捷键 | 说明 |
|--------|------|
| `Ctrl+C` | 复制选区 |
| `Ctrl+X` | 剪切选区 |
| `Ctrl+V` | 粘贴 |
| `Ctrl+A` | 全选 |
| `Ctrl+Z` | 撤销 |
| `Ctrl+Y` / `Ctrl+Shift+Z` | 重做 |
| `Shift+方向键` | 选区扩展 |
| `鼠标拖动` | 选中文本 |
| `鼠标滚轮` | 垂直滚动 |

## 颜色格式

颜色使用 `0xRRGGBB` 或 `0xAARRGGBB` 格式，例如：

- `0xFF0000` 红色
- `0x00FF00` 绿色
- `0x409EFF` 蓝色
- `0xFFFFFFFF` 不透明白色
- `0x80000000` 半透明黑色

## 完整示例

```leno
import gui

var win = gui.window("登录表单", 400, 300)

var user_label = win.add_label("用户名", { x: 50, y: 40, w: 80, h: 30 })
var user_edit = win.add_edit({
    x: 130, y: 40, w: 200, h: 32,
    placeholder: "请输入用户名",
    font_size: 14
})

var pwd_label = win.add_label("密码", { x: 50, y: 90, w: 80, h: 30 })
var pwd_edit = win.add_edit({
    x: 130, y: 90, w: 200, h: 32,
    placeholder: "请输入密码",
    password: true,
    font_size: 14
})

var btn = win.add_button("登录", {
    x: 130, y: 150, w: 200, h: 36,
    bg_color: 0x409EFF,
    text_color: 0xFFFFFF,
    radius: 4
})

btn.on_click(fn() {
    print("用户名：" + user_edit.get_text())
    print("密码：" + pwd_edit.get_text())
})

win.show()
gui.run()
```
