# LenoSDL3 示例分类说明

## 目录结构

本目录下的示例文件已按功能分类到以下子文件夹中：

### 1. 基础示例/
- `hello_window.leno` - 基本绘图示例（矩形、圆、线、混合模式）
- `simple_window.leno` - 最简 SDL3 窗口（回调模式）
- `show_time.leno` - 在窗口中实时显示当前时间

### 2. 图形绘制/
- `test_draw.leno` - 测试绘制：矩形/圆形/三角形/线条/阴影/渐变
- `test_heart.leno` - 心形绘制测试
- `test_rounded_rect.leno` - 圆角矩形测试

### 3. 字体文本/
- `test_font_api.leno` - 测试 SDL3_ttf 新增 API
- `test_font_v2.leno` - 字体测试 v2
- `test_text.leno` - 文本渲染测试
- `test_text_input.leno` - 文本输入测试

### 4. 图像处理/
- `test_image.leno` - 测试 SDL3_image 加载 PNG/JPG
- `test_image_v2.leno` - 图像测试 v2
- `test_crop.leno` - 图像裁剪测试
- `test_transform.leno` - 图像变换测试
- `test_zoom.leno` - 鼠标滚轮放大/缩小图片

### 5. 事件交互/
- `test_event_v2.leno` - 测试新增事件 API (文本输入/拖放/窗口状态/键盘修饰等)
- `test_mouse.leno` - 鼠标事件测试
- `keyboard.leno` - 键盘事件测试

### 6. 特效动画/
- `fireworks.leno` - 烟花压力测试（FPS 统计）
- `fireworks_run.leno` - 烟花运行版本
- `test_timer.leno` - 测试 Timer：stop/start/reset/setOnce

### 7. UI组件/
- `test_button.leno` - 交互式按钮测试
- `test_cursor_msg.leno` - 光标消息测试

### 8. 其他测试/
- `test_auto_alias.leno` - 自动别名测试
- `test_extras.leno` - 额外功能测试
- `test_icon.leno` - 图标测试
- `test_new_api.leno` - 新 API 测试
- `test_renderer.leno` - 渲染器测试

## 文件统计
- 总计: 28 个 .leno 文件
- 1 个配置文件: `project.json`
- 8 个分类目录

## 使用说明
每个示例文件都可以独立运行，展示了 LenoSDL3 库的不同功能特性。建议按照分类顺序学习，从基础示例开始逐步深入。