# LenoC GUI Draw 模块

本文档详细说明 `Draw` 渲染器提供的所有绘图功能。

## 目录

- [概述](#概述)
- [基础绘制](#基础绘制)
- [几何图形](#几何图形)
- [高级图形](#高级图形)
- [视口与裁剪](#视口与裁剪)
- [逻辑呈现模式](#逻辑呈现模式)
- [文字渲染](#文字渲染)
- [示例代码](#示例代码)

---

## 概述

`Draw` 是 LenoC GUI 模块的 2D 渲染器，提供丰富的软件渲染绘图 API。所有绘制操作都在内存中的像素缓冲区执行，最后通过 `present()` 显示到窗口。

### 获取 Draw 对象

```leno
import guis

main() {
    Win win = guis.create_window("Draw Test", {width: 800, height: 600})
    
    win.run(
        func(Draw ren) {
            // 使用 ren 进行绘制
            ren.set_color(_rgb(0, 0, 0, 255))
            ren.clear()
            ren.present()
        },
        func(Event e) {
            if e.is_quit() { win.set_should_close(true) }
        }
    )
}
```

---

## 基础绘制

### `ren.set_color(Rgb)`

设置当前绘制颜色，影响后续所有绘制操作。

**参数**:
- `color` (Rgb): 颜色对象，通过 `_rgb(r, g, b, a)` 创建

```leno
ren.set_color(_rgb(255, 0, 0, 255))     // 红色不透明
ren.set_color(_rgb(0, 255, 0, 128))     // 绿色半透明
```

---

### `ren.clear()`

用当前颜色清除整个渲染缓冲区。

```leno
ren.set_color(_rgb(30, 30, 46, 255))    // 深色背景
ren.clear()
```

---

### `ren.present()`

将渲染缓冲区的内容显示到窗口。每帧绘制完成后必须调用。

```leno
// 绘制代码...
ren.present()  // 显示到屏幕
```

---

### `ren.point(x, y)`

绘制单个像素点。

**参数**:
- `x`, `y` (int): 像素坐标

```leno
ren.set_color(_rgb(255, 255, 255, 255))
ren.point(100, 100)
```

---

### `ren.line(x1, y1, x2, y2)`

绘制直线（Bresenham 算法）。

**参数**:
- `x1`, `y1` (int): 起点坐标
- `x2`, `y2` (int): 终点坐标

```leno
ren.set_color(_rgb(255, 255, 0, 255))
ren.line(0, 0, 800, 600)    // 对角线
```

---

### `ren.rect(x, y, w, h)`

绘制矩形边框。

**参数**:
- `x`, `y` (int): 左上角坐标
- `w`, `h` (int): 宽度和高度

```leno
ren.set_color(_rgb(0, 255, 0, 255))
ren.rect(50, 50, 200, 100)
```

---

### `ren.fill_rect(x, y, w, h)`

填充矩形。

```leno
ren.set_color(_rgb(100, 100, 255, 200))
ren.fill_rect(50, 50, 200, 100)
```

---

## 几何图形

### `ren.circle(cx, cy, radius)`

绘制圆形边框（Bresenham 中点圆算法）。

**参数**:
- `cx`, `cy` (int): 圆心坐标
- `radius` (int): 半径

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

**参数**:
- `x`, `y`, `w`, `h` (int): 矩形位置和大小
- `radius` (int): 圆角半径

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

## 高级图形

### `ren.ellipse(cx, cy, rx, ry)`

绘制椭圆边框（中点椭圆算法）。

**参数**:
- `cx`, `cy` (int): 中心坐标
- `rx`, `ry` (int): X轴和Y轴半径

```leno
ren.set_color(_rgb(255, 100, 100, 255))
ren.ellipse(400, 300, 100, 60)     // 横向椭圆
ren.ellipse(400, 300, 60, 100)     // 纵向椭圆
```

---

### `ren.fill_ellipse(cx, cy, rx, ry)`

填充椭圆。

```leno
ren.set_color(_rgb(100, 255, 150, 200))
ren.fill_ellipse(400, 300, 80, 50)
```

---

### `ren.arc(cx, cy, r, start_angle, end_angle)`

绘制圆弧。

**参数**:
- `cx`, `cy` (int): 圆心坐标
- `r` (int): 半径
- `start_angle`, `end_angle` (float): 起止角度（度，0=右，顺时针）

```leno
ren.set_color(_rgb(255, 200, 50, 255))
ren.arc(400, 300, 80, 0.0, 90.0)       // 四分之一圆弧
ren.arc(400, 300, 80, 0.0, 270.0)      // 四分之三圆弧

// 动态旋转的圆弧
var angle = 0.0
ren.arc(400, 300, 60, angle, angle + 180.0)
```

---

### `ren.triangle(x1, y1, x2, y2, x3, y3)`

绘制三角形边框。

**参数**:
- `x1`, `y1`, `x2`, `y2`, `x3`, `y3` (int): 三个顶点坐标

```leno
ren.set_color(_rgb(255, 150, 50, 255))
ren.triangle(100, 250, 200, 250, 150, 150)
```

---

### `ren.fill_triangle(x1, y1, x2, y2, x3, y3)`

填充三角形（扫描线填充算法）。

```leno
ren.set_color(_rgb(150, 100, 255, 200))
ren.fill_triangle(300, 250, 450, 250, 375, 150)
```

---

### `ren.polygon(points_array)`

绘制多边形边框。

**参数**:
- `points_array` (array): 顶点坐标数组 `[x1, y1, x2, y2, ...]`

```leno
ren.set_color(_rgb(200, 255, 100, 255))

// 五边形
var pentagon = [150, 400, 200, 350, 250, 350, 280, 400, 215, 450]
ren.polygon(pentagon)

// 星形
var star = [650, 350, 670, 410, 730, 410, 685, 450, 700, 510, 
            650, 470, 600, 510, 615, 450, 570, 410, 630, 410]
ren.polygon(star)
```

---

### `ren.fill_polygon(points_array)`

填充多边形（扫描线交点算法）。

```leno
ren.set_color(_rgb(100, 150, 255, 200))

// 填充六边形
var hexagon = [400, 350, 450, 380, 450, 430, 400, 460, 350, 430, 350, 380]
ren.fill_polygon(hexagon)
```

---

### `ren.bezier(points_array, steps)`

绘制贝塞尔曲线（De Casteljau 算法）。

**参数**:
- `points_array` (array): 控制点坐标数组 `[x1, y1, x2, y2, ...]`
- `steps` (int): 采样步数（越大越平滑，建议 20-100）

```leno
ren.set_color(_rgb(255, 255, 255, 255))

// 三次贝塞尔曲线（4个控制点）
var bezier3 = [100, 600, 200, 500, 300, 700, 400, 600]
ren.bezier(bezier3, 50)

// 多点贝塞尔曲线
ren.set_color(_rgb(255, 100, 255, 255))
var bezier_multi = [500, 650, 550, 550, 650, 550, 700, 650, 750, 550]
ren.bezier(bezier_multi, 80)
```

---

## 视口与裁剪

### `ren.get_size()`

获取渲染器缓冲区大小。

**返回**: `[w, h]` - 宽高数组

```leno
var size = ren.get_size()
print("渲染器大小: " + size[0] + "x" + size[1])
```

---

### `ren.set_viewport(x, y, w, h)` / `ren.get_viewport()`

设置或获取渲染视口。所有后续绘制坐标相对于视口左上角。

```leno
// 设置视口为左上角 200x200 区域
ren.set_viewport(0, 0, 200, 200)

// 绘制将相对于视口
ren.fill_rect(0, 0, 100, 100)  // 实际位置 (0, 0)

// 获取当前视口
var vp = ren.get_viewport()  // [x, y, w, h]
```

---

### `ren.set_clip_rect(x, y, w, h)` / `ren.get_clip_rect()` / `ren.no_clip()`

设置、获取或禁用裁剪矩形。裁剪矩形限制绘制区域（物理坐标）。

```leno
// 设置裁剪区域
ren.set_clip_rect(100, 100, 300, 200)

// 此区域外的绘制将被裁剪
ren.fill_rect(0, 0, 500, 500)  // 只有部分可见

// 获取裁剪矩形
var clip = ren.get_clip_rect()  // [x, y, w, h]

// 禁用裁剪
ren.no_clip()
```

---

## 逻辑呈现模式

逻辑呈现模式允许以固定逻辑分辨率渲染，自动缩放到实际窗口大小（参考 SDL3）。

### `ren.set_logical_size(w, h)`

设置逻辑分辨率。

```leno
// 设置逻辑大小为 640x480
ren.set_logical_size(640, 480)

// 后续所有绘制坐标都基于 640x480
// 实际显示时会自动缩放到窗口大小
ren.fill_rect(0, 0, 320, 240)  // 逻辑上是一半大小
```

---

### `ren.get_logical_size()`

获取逻辑分辨率。

**返回**: `[w, h]` - 逻辑宽高数组

---

### `ren.set_logical_presentation(mode)`

设置逻辑呈现模式。

**参数**:
- `mode` (int): 
  - `0` - 禁用逻辑大小
  - `1` - 拉伸填充（可能变形）
  - `2` - 信箱模式（保持比例，黑边）
  - `3` - 过扫描（保持比例，裁剪）
  - `4` - 整数倍缩放

```leno
ren.set_logical_size(640, 480)
ren.set_logical_presentation(2)  // 信箱模式，保持比例
```

---

### `ren.get_logical_presentation()`

获取当前逻辑呈现模式。

**返回**: `int` - 模式值

---

### `ren.get_logical_viewport()`

获取逻辑视口（实际渲染区域）。

**返回**: `[x, y, w, h]` - 视口位置和大小

---

### `ren.reset_logical_size()`

重置逻辑大小，恢复为物理像素坐标。

```leno
ren.reset_logical_size()
// 现在 1 单位 = 1 像素
```

---

## 文字渲染

### `ren.draw_text(font, text, x, y)`

使用指定字体绘制文字。

**参数**:
- `font` (Font): 字体对象
- `text` (string): 要绘制的文本
- `x`, `y` (int): 绘制位置

```leno
var font = guis.load_font("SimSun", 16)
ren.set_color(_rgb(255, 255, 255, 255))
ren.draw_text(font, "Hello Leno!", 100, 100)
```

---

### `ren.text_size(font, text)`

计算文字尺寸。

**参数**:
- `font` (Font): 字体对象
- `text` (string): 文本

**返回**: `[w, h]` - 文字宽度和高度

```leno
var size = ren.text_size(font, "Hello")
print("文字大小: " + size[0] + "x" + size[1])
```

---

## 示例代码

### 综合示例：绘制各种图形

```leno
import guis

main() {
    Win win = guis.create_window("Draw Shapes Demo", {
        width: 900,
        height: 700,
        resizable: true
    })
    
    var angle = 0.0
    
    win.run(
        func(Draw ren) {
            // 深色背景
            ren.set_color(_rgb(20, 20, 30, 255))
            ren.clear()
            
            // 1. 椭圆
            ren.set_color(_rgb(255, 100, 100, 255))
            ren.ellipse(150, 100, 80, 50)
            ren.set_color(_rgb(100, 255, 100, 180))
            ren.fill_ellipse(350, 100, 60, 40)
            
            // 2. 圆弧（动态旋转）
            ren.set_color(_rgb(255, 200, 50, 255))
            ren.arc(750, 100, 50, angle, angle + 180.0)
            
            // 3. 三角形
            ren.set_color(_rgb(255, 150, 50, 255))
            ren.triangle(100, 250, 200, 250, 150, 150)
            ren.set_color(_rgb(150, 100, 255, 200))
            ren.fill_triangle(300, 250, 450, 250, 375, 150)
            
            // 4. 多边形
            ren.set_color(_rgb(200, 255, 100, 255))
            var pentagon = [150, 400, 200, 350, 250, 350, 280, 400, 215, 450]
            ren.polygon(pentagon)
            
            ren.set_color(_rgb(100, 150, 255, 200))
            var hexagon = [400, 350, 450, 380, 450, 430, 400, 460, 350, 430, 350, 380]
            ren.fill_polygon(hexagon)
            
            // 5. 贝塞尔曲线
            ren.set_color(_rgb(255, 255, 255, 255))
            var bezier = [100, 600, 200, 500, 300, 700, 400, 600]
            ren.bezier(bezier, 50)
            
            // 更新动画
            angle = angle + 2.0
            if angle > 360.0 { angle = 0.0 }
            
            ren.present()
        },
        func(Event e) {
            if e.is_quit() or e.is_window_close() {
                win.set_should_close(true)
            }
        }
    )
    
    win.close()
}
```

---

### 绘制图表示例

```leno
// 绘制饼图
func draw_pie_chart(Draw ren, int cx, int cy, int r, array data) {
    var colors = [
        _rgb(255, 100, 100, 255),
        _rgb(100, 255, 100, 255),
        _rgb(100, 100, 255, 255),
        _rgb(255, 255, 100, 255)
    ]
    
    var total = 0
    for var v in data { total = total + v }
    
    var start = 0.0
    for var i = 0; i < data.len(); i++ {
        var angle = 360.0 * data[i] / total
        ren.set_color(colors[i % colors.len()])
        ren.arc(cx, cy, r, start, start + angle)
        start = start + angle
    }
}

// 使用
var data = [30, 50, 20, 40]
draw_pie_chart(ren, 400, 300, 100, data)
```

---

## 算法说明

| 图形 | 算法 | 说明 |
|------|------|------|
| 直线 | Bresenham | 整数运算，无浮点 |
| 圆形 | 中点圆算法 | Bresenham 变种 |
| 椭圆 | 中点椭圆算法 | 分两个区域处理 |
| 圆弧 | 参数方程采样 | 自适应步数 |
| 三角形 | 扫描线填充 | 处理平底/平顶 |
| 多边形 | 扫描线交点 | 支持凹多边形 |
| 贝塞尔 | De Casteljau | 数值稳定 |

---

## 性能提示

1. **批量绘制**：尽量减少 `set_color()` 调用次数
2. **裁剪优化**：设置裁剪矩形避免不必要的像素计算
3. **逻辑大小**：使用逻辑呈现模式简化坐标计算
4. **步数控制**：贝塞尔曲线的 `steps` 参数根据曲率调整
