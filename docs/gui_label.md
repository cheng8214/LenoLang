# GLabel 标签控件

`GLabel` 用于在窗口中显示静态文本，支持颜色、字体、对齐、阴影、圆角、边框等样式。

## 创建标签

```leno
import gui

var win = gui.window("标签示例", 800, 600)
var lbl = win.add_label("Hello Leno", {
    x: 50,
    y: 50,
    w: 200,
    h: 40,
    text_color: 0x333333,
    font_size: 16
})

win.show()
gui.run()
```

## 常用方法

| 方法 | 说明 | 示例 |
|------|------|------|
| `set_text(text)` | 设置文字内容 | `lbl.set_text("新文字")` |
| `set_pos(x, y)` | 设置位置 | `lbl.set_pos(100, 100)` |
| `set_size(w, h)` | 设置尺寸 | `lbl.set_size(200, 40)` |
| `set_text_color(color)` | 设置文字颜色 | `lbl.set_text_color(0xFF0000)` |
| `set_bg_color(color)` | 设置背景颜色 | `lbl.set_bg_color(0xEEEEEE)` |
| `set_font_size(size)` | 设置字体大小 | `lbl.set_font_size(20)` |
| `set_font_name(name)` | 设置字体名称 | `lbl.set_font_name("Microsoft YaHei")` |
| `set_font_bold(b)` | 设置是否粗体 | `lbl.set_font_bold(true)` |
| `set_padding(x, y)` | 设置内边距 | `lbl.set_padding(10, 5)` |
| `set_align(align)` | 设置对齐方式 | `lbl.set_align(1)` |
| `set_visible(v)` | 设置是否可见 | `lbl.set_visible(false)` |
| `set_enabled(v)` | 设置是否启用 | `lbl.set_enabled(false)` |
| `set_opacity(val)` | 设置透明度 0~255 | `lbl.set_opacity(128)` |
| `set_radius(r)` | 设置圆角半径 | `lbl.set_radius(8)` |
| `set_border(width, color)` | 设置边框 | `lbl.set_border(2, 0x000000)` |
| `set_letter_spacing(n)` | 设置字间距 | `lbl.set_letter_spacing(2)` |
| `set_shadow(dx, dy, blur, color)` | 设置阴影 | `lbl.set_shadow(2, 2, 4, 0x80000000)` |
| `set_anchor(x, y, w, h)` | 设置锚点 | `lbl.set_anchor(0.5, 0.5, 100, 30)` |
| `get_text()` | 获取文字内容 | `var t = lbl.get_text()` |
| `close()` | 移除标签 | `lbl.close()` |

## 对齐方式

`set_align(align)` 参数说明：

| 值 | 含义 |
|----|------|
| 0 | 左对齐 |
| 1 | 居中对齐 |
| 2 | 右对齐 |

## 颜色格式

颜色使用 `0xRRGGBB` 或 `0xAARRGGBB` 格式，例如：

- `0xFF0000` 红色
- `0x00FF00` 绿色
- `0x0000FF` 蓝色
- `0xFFFF00` 黄色
- `0xFFFFFFFF` 不透明白色
- `0x80000000` 半透明黑色

## 完整示例

```leno
import gui

var win = gui.window("标签样式示例", 800, 600)

var title = win.add_label("Leno GUI", {
    x: 250, y: 50, w: 300, h: 60,
    text_color: 0xFFFFFF,
    font_size: 32,
    font_bold: true,
    align: 1
})

var subtitle = win.add_label("欢迎使用 Leno 编程语言", {
    x: 200, y: 120, w: 400, h: 40,
    text_color: 0x666666,
    font_size: 14,
    align: 1
})

var card = win.add_label("带圆角和背景的标签", {
    x: 250, y: 200, w: 300, h: 80,
    text_color: 0x333333,
    bg_color: 0xF0F0F0,
    font_size: 16,
    radius: 12,
    align: 1
})

win.show()
gui.run()
```
