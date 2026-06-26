# LenoC GUI Image 模块

本文档详细说明 `GImage` 图像对象提供的所有操作。

## 目录

- [概述](#概述)
- [加载图像](#加载图像)
- [图像绘制](#图像绘制)
- [图像信息](#图像信息)
- [资源管理](#资源管理)
- [翻转常量](#翻转常量)
- [示例代码](#示例代码)

---

## 概述

`GImage` 是 LenoC GUI 模块的图像对象，支持从文件或内存加载图片，并提供丰富的绘制功能。

### 支持的格式

- PNG
- JPEG/JPG
- BMP
- TGA
- 其他常见格式

### 类型定义

```leno
type GImage  // 图像对象类型
```

---

## 加载图像

### `guis.load_image(path)`

从文件加载图像。

**参数**:
- `path` (string): 图像文件路径

**返回**: `GImage` - 图像对象，失败返回 `null`

```leno
import guis

var img = guis.load_image("assets/player.png")
if img == null {
    print("加载图像失败!")
}
```

---

### `guis.load_image_ex(path, options)`

带选项加载图像。

**参数**:
- `path` (string): 图像文件路径
- `options` (Dict): 选项字典
  - `flip_vertical` (bool): 是否垂直翻转

**返回**: `GImage` - 图像对象

```leno
// 垂直翻转加载（OpenGL纹理坐标兼容）
var img = guis.load_image_ex("texture.png", {flip_vertical: true})
```

---

### `guis.load_image_from_memory(data)`

从内存数据加载图像。

**参数**:
- `data` (string): 图像二进制数据（字节字符串）

**返回**: `GImage` - 图像对象

```leno
import files

var f = files.open("image.png", "rb")
var bytes = f.read()
f.close()

var img = guis.load_image_from_memory(bytes)
```

---

### `guis.image_info(path)`

获取图像信息（不加载像素数据）。

**参数**:
- `path` (string): 图像文件路径

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

### `guis.image_info_from_memory(data)`

从内存数据获取图像信息。

```leno
var info = guis.image_info_from_memory(image_bytes)
```

---

### `guis.is_16_bit(path)` / `guis.is_16_bit_from_memory(data)`

检查图像是否为16位深度。

**返回**: `bool`

```leno
if guis.is_16_bit("image.png") {
    print("16位图像")
}
```

---

### `guis.is_hdr(path)` / `guis.is_hdr_from_memory(data)`

检查图像是否为HDR格式。

**返回**: `bool`

```leno
if guis.is_hdr("scene.hdr") {
    print("HDR图像")
}
```

---

## 图像绘制

### `image.draw(ren, x, y)`

在指定位置绘制图像（原始大小）。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 绘制位置

```leno
img.draw(ren, 100, 100)
```

---

### `image.draw_scaled(ren, x, y, w, h)`

缩放绘制图像到目标矩形。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 目标位置
- `w`, `h` (int): 目标宽度和高度

```leno
// 将图像缩放到 200x150 绘制
img.draw_scaled(ren, 100, 100, 200, 150)
```

---

### `image.draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh)`

从图像中取子区域绘制到目标位置（可缩放）。

**参数**:
- `ren` (GDraw): 渲染器对象
- `sx`, `sy`, `sw`, `sh` (int): 源图像区域（x, y, 宽, 高）
- `dx`, `dy`, `dw`, `dh` (int): 目标绘制区域（x, y, 宽, 高）

```leno
// 从图像中截取 64x64 区域，放大到 128x128 绘制
img.draw_src(ren, 0, 0, 64, 64, 100, 100, 128, 128)
```

---

### `image.draw_rotated(ren, x, y, angle, flip)`

旋转/翻转绘制图像（原始尺寸）。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 中心点位置
- `angle` (float): 旋转角度（度，顺时针）
- `flip` (int): 翻转标志

```leno
// 旋转45度
img.draw_rotated(ren, 400, 300, 45.0, guis.FLIP_NONE)

// 水平翻转
img.draw_rotated(ren, 400, 300, 0.0, guis.FLIP_HORIZONTAL)

// 旋转+垂直翻转
img.draw_rotated(ren, 400, 300, 90.0, guis.FLIP_VERTICAL)
```

---

### `image.draw_rotated_scaled(ren, x, y, w, h, angle)`

旋转+缩放绘制图像。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 中心点位置
- `w`, `h` (int): 目标宽度和高度
- `angle` (float): 旋转角度

```leno
// 缩放到 200x200 并旋转30度
img.draw_rotated_scaled(ren, 400, 300, 200, 200, 30.0)
```

---

### `image.draw_flipped(ren, x, y, flip)`

翻转绘制图像（原始尺寸）。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 绘制位置
- `flip` (int): 翻转标志

```leno
// 水平翻转
img.draw_flipped(ren, 100, 100, guis.FLIP_HORIZONTAL)

// 垂直翻转
img.draw_flipped(ren, 100, 100, guis.FLIP_VERTICAL)

// 水平和垂直都翻转
img.draw_flipped(ren, 100, 100, guis.FLIP_BOTH)
```

---

### `image.draw_flipped_scaled(ren, x, y, w, h, flip)`

翻转+缩放绘制图像。

**参数**:
- `ren` (GDraw): 渲染器对象
- `x`, `y` (int): 绘制位置
- `w`, `h` (int): 目标宽度和高度
- `flip` (int): 翻转标志

```leno
// 水平翻转并缩放到 200x150
img.draw_flipped_scaled(ren, 100, 100, 200, 150, guis.FLIP_HORIZONTAL)
```

---

## 图像信息

### `image.width()` -> int

获取图像宽度。

**返回**: `int` - 宽度（像素）

```leno
var w = img.width()
```

---

### `image.height()` -> int

获取图像高度。

**返回**: `int` - 高度（像素）

```leno
var h = img.height()
```

---

### `image.size()` -> [w, h]

获取图像尺寸。

**返回**: `[w, h]` - 宽高数组

```leno
var size = img.size()
print("图像大小: " + size[0] + "x" + size[1])
```

---

### `image.access()` -> int

获取图像访问模式。

**返回**: `int` - 访问模式
- `0` - 静态（STATIC）
- `1` - 流式（STREAMING）
- `2` - 目标（TARGET）

```leno
var mode = img.access()
```

---

## 资源管理

### `image.close()`

关闭并释放图像资源。

**注意**: 图像不再使用时应该调用，释放内存。

```leno
var img = guis.load_image("temp.png")
// 使用图像...
img.close()  // 释放资源
```

---

## 翻转常量

| 常量 | 值 | 说明 |
|------|-----|------|
| `guis.FLIP_NONE` | 0 | 不翻转 |
| `guis.FLIP_HORIZONTAL` | 1 | 水平翻转 |
| `guis.FLIP_VERTICAL` | 2 | 垂直翻转 |
| `guis.FLIP_BOTH` | 3 | 水平和垂直都翻转 |

---

## 示例代码

### 基础图像显示

```leno
import guis

main() {
    GWin win = guis.create_window("Image Demo", {width: 800, height: 600})
    
    var img = guis.load_image("assets/logo.png")
    if img == null {
        print("无法加载图像!")
        return
    }
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()
            
            // 居中绘制
            var size = img.size()
            var x = (800 - size[0]) / 2
            var y = (600 - size[1]) / 2
            img.draw(ren, x, y)
        },
        func(GEvent e) {
            if e.quit() or e.window_close() {
                win.set_should_close(true)
            }
        }
    )
    
    img.close()
    win.close()
}
```

---

### 精灵动画

```leno
import guis

main() {
    GWin win = guis.create_window("Sprite Animation", {width: 800, height: 600})
    
    // 加载精灵表
    var sprite_sheet = guis.load_image("assets/character.png")
    var frame_w = 64
    var frame_h = 64
    var frame_count = 8
    var current_frame = 0
    var anim_timer = 0
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(100, 150, 200, 255))
            ren.clear()
            
            // 更新动画
            anim_timer = anim_timer + 1
            if anim_timer > 5 {
                anim_timer = 0
                current_frame = (current_frame + 1) % frame_count
            }
            
            // 绘制当前帧
            var sx = current_frame * frame_w
            sprite_sheet.draw_src(ren, sx, 0, frame_w, frame_h, 
                                  368, 268, frame_w * 2, frame_h * 2)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
        }
    )
    
    sprite_sheet.close()
    win.close()
}
```

---

### 图像缩放和旋转

```leno
import guis

main() {
    GWin win = guis.create_window("Transform Demo", {width: 800, height: 600})
    
    var img = guis.load_image("assets/arrow.png")
    var angle = 0.0
    var scale = 1.0
    var scale_dir = 0.01
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.clear()
            
            // 更新变换
            angle = angle + 1.0
            if angle > 360.0 { angle = 0.0 }
            
            scale = scale + scale_dir
            if scale > 2.0 or scale < 0.5 {
                scale_dir = -scale_dir
            }
            
            // 计算绘制尺寸
            var size = img.size()
            var w = size[0] * scale
            var h = size[1] * scale
            
            // 旋转+缩放绘制
            img.draw_rotated_scaled(ren, 400, 300, w, h, angle)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
        }
    )
    
    img.close()
    win.close()
}
```

---

### 图像镜像

```leno
import guis

main() {
    GWin win = guis.create_window("Flip Demo", {width: 800, height: 600})
    
    var img = guis.load_image("assets/player.png")
    var facing_right = true
    var player_x = 400
    var player_y = 300
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(50, 50, 70, 255))
            ren.clear()
            
            // 根据朝向翻转绘制
            var flip = if facing_right then guis.FLIP_NONE else guis.FLIP_HORIZONTAL
            img.draw_flipped(ren, player_x, player_y, flip)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
            
            // 左右移动改变朝向
            if e.key_down() {
                if e.key() == 0x25 {  // 左箭头
                    facing_right = false
                    player_x = player_x - 10
                }
                if e.key() == 0x27 {  // 右箭头
                    facing_right = true
                    player_x = player_x + 10
                }
            }
        }
    )
    
    img.close()
    win.close()
}
```

---

### 九宫格缩放

```leno
import guis

// 绘制可缩放的UI元素（如按钮背景）
func draw_nine_slice(GImage img, GDraw ren, int x, int y, int w, int h, int border) {
    var iw = img.width()
    var ih = img.height()
    
    // 四个角
    img.draw_src(ren, 0, 0, border, border, x, y, border, border)
    img.draw_src(ren, iw-border, 0, border, border, x+w-border, y, border, border)
    img.draw_src(ren, 0, ih-border, border, border, x, y+h-border, border, border)
    img.draw_src(ren, iw-border, ih-border, border, border, x+w-border, y+h-border, border, border)
    
    // 四条边
    img.draw_src(ren, border, 0, iw-border*2, border, x+border, y, w-border*2, border)
    img.draw_src(ren, border, ih-border, iw-border*2, border, x+border, y+h-border, w-border*2, border)
    img.draw_src(ren, 0, border, border, ih-border*2, x, y+border, border, h-border*2)
    img.draw_src(ren, iw-border, border, border, ih-border*2, x+w-border, y+border, border, h-border*2)
    
    // 中间
    img.draw_src(ren, border, border, iw-border*2, ih-border*2, x+border, y+border, w-border*2, h-border*2)
}

main() {
    GWin win = guis.create_window("Nine Slice", {width: 800, height: 600})
    
    var button_bg = guis.load_image("assets/button_bg.png")
    
    win.run(
        func(GDraw ren) {
            ren.set_color(_rgb(30, 30, 46, 255))
            ren.clear()
            
            // 绘制不同大小的按钮
            draw_nine_slice(button_bg, ren, 100, 100, 200, 60, 10)
            draw_nine_slice(button_bg, ren, 100, 200, 300, 80, 10)
            draw_nine_slice(button_bg, ren, 100, 320, 150, 50, 10)
        },
        func(GEvent e) {
            if e.quit() {
                win.set_should_close(true)
            }
        }
    )
    
    button_bg.close()
    win.close()
}
```

---

## 方法速查表

### 模块方法（guis.*）

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `load_image(path)` | string | GImage/null | 加载图像 |
| `load_image_ex(path, opts)` | string, Dict | GImage | 带选项加载 |
| `load_image_from_memory(data)` | string | GImage | 从内存加载 |
| `image_info(path)` | string | Dict/null | 获取图像信息 |
| `image_info_from_memory(data)` | string | Dict/null | 从内存获取信息 |
| `is_16_bit(path)` | string | bool | 是否16位 |
| `is_16_bit_from_memory(data)` | string | bool | 从内存检查 |
| `is_hdr(path)` | string | bool | 是否HDR |
| `is_hdr_from_memory(data)` | string | bool | 从内存检查 |

### 实例方法（image.*）

| 方法 | 参数 | 返回值 | 说明 |
|------|------|--------|------|
| `draw(ren, x, y)` | GDraw, int, int | null | 绘制图像 |
| `draw_scaled(ren, x, y, w, h)` | GDraw, int x4 | null | 缩放绘制 |
| `draw_src(ren, sx, sy, sw, sh, dx, dy, dw, dh)` | GDraw, int x8 | null | 子区域绘制 |
| `draw_rotated(ren, x, y, angle, flip)` | GDraw, int, int, float, int | null | 旋转绘制 |
| `draw_rotated_scaled(ren, x, y, w, h, angle)` | GDraw, int x4, float | null | 旋转+缩放 |
| `draw_flipped(ren, x, y, flip)` | GDraw, int, int, int | null | 翻转绘制 |
| `draw_flipped_scaled(ren, x, y, w, h, flip)` | GDraw, int x4, int | null | 翻转+缩放 |
| `width()` | - | int | 获取宽度 |
| `height()` | - | int | 获取高度 |
| `size()` | - | [w,h] | 获取尺寸 |
| `access()` | - | int | 获取访问模式 |
| `close()` | - | null | 释放资源 |
