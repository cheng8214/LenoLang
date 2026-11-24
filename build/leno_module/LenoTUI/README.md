# LenoTUI

终端输出增强：**颜色 / 清行 / 进度条 / 对齐表格**。纯 Leno，无原生依赖。

范围：只做"输出好看"这一半。不做全屏 TUI、键盘事件、鼠标（那要各平台的
termios / Win32 控制台 API，是另一个量级，也与"脚本在哪都能跑"的定位冲突）。

## 用法

```leno
import "TUI" as tui

// 颜色（NO_COLOR 环境变量存在时 init() 会自动关掉）
tui.init()
print(tui.fg("成功", "green") + " " + tui.bold("42"))

// 进度条：返回一行字符串，自己决定 print 还是拼进日志
print(tui.clear_line() + tui.progress(done, total, 30, "下载"))

// 表格
Array[string] headers = ["名称", "大小"]
Array[Array[string]] rows = []
rows.add(["a.txt", "12"])
print(tui.table(headers, rows))
```

输出示例：

```
┌──────────┬──────┐
│ 名称     │ 大小 │
├──────────┼──────┤
│ a.txt    │ 12   │
└──────────┴──────┘
```

## API

| 函数 | 说明 |
|---|---|
| `init()` | 读 `NO_COLOR` 环境变量（存在即关色） |
| `set_color(on)` | 手动开关颜色 |
| `fg(s, name)` | 前景色：`black/red/green/yellow/blue/magenta/cyan/white/gray`；**未知名字原样返回**（装饰不该让程序挂） |
| `bold(s)` / `dim(s)` / `underline(s)` | 样式 |
| `clear_line()` | `\r` + 清行（进度条重绘用） |
| `move_up(n)` / `hide_cursor()` / `show_cursor()` | 光标控制 |
| `width(s)` | **终端显示宽度**（中文 2 列、ASCII 1 列） |
| `progress(done, total, bar_width=30, label="")` | 一行进度条；`total<=0` 视为 0%、越界自动夹取 |
| `table(headers, rows, border=true)` | 按列宽对齐的表格；行的列数不足自动补空 |

## 两条需要知道的口径

1. **`width()` 按真实码点判定**（`strings.to_codepoints` 一趟 O(n) + Unicode East Asian
   Width 的常用区段）。这一点很关键：`─`（U+2500）和 `中`（U+4E2D）**都是 3 字节**，
   但前者 1 列、后者 2 列 —— 靠字节数猜一定会把表格边框算歪。
   覆盖区段：CJK 汉字/扩展 A/兼容、假名、韩文、彝文、全角 ASCII、竖排标点、emoji。
   少数"窄的 CJK 兼容字符"按 1 列算（许多终端自己也处理不一致）。
2. **颜色默认开**。Windows Terminal / VS Code / 各 Linux 终端都认 ANSI；
   老 conhost 窗口会把 `\x1b[...` 原样打出来 —— 用 `set_color(false)` 或设 `NO_COLOR`。

## 跑测试

```
build\leno.exe build\leno_module\LenoTUI\test\test_tui.leno
```
