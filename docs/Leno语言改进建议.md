# Leno 语言特性评估总结

> **结论：Leno 语法和特性已足够且稳定。** 以下是对各项建议的评估结果，大部分已实现或经评估后决定不实现。语言层面不再追加新特性，重心放在库生态和稳定性上。

---

## 已实现的特性

### 1. struct 可空类型 `Type?` ✅

`Type?` 可空类型已完整实现，支持 `int?`、`string?`、`Point?` 等所有类型。可空 struct 字段初始为 null，无需 bool 伴生字段。

```leno
Font? _font                    // 可空 Font，初始为 null

func _ensure_font() {
    if _font == null or not _font.ok {
        if _font != null { fnt.releaseFont(_font) }
        _font = fnt.acquireFontAuto(_fontSize)
    }
}
```

### 2. struct 构造函数 / 析构函数 / 命名参数初始化 ✅

已完整支持：

```leno
struct cs {
    int x
    int y

    func cs() {       // 构造函数，new 时自动调用
        // 初始化逻辑
    }

    func ~cs() {       // 析构函数，GC 回收时自动调用
        // 清理逻辑
    }
}

// 命名参数初始化
var c = new cs(x = 1, y = 2)

// 也支持默认值 + 部分参数
struct Point {
    int x = 0
    int y = 0
}
var p = new Point(x = 10)  // y 自动为 0
```

### 3. 字符串插值 `$"..."` ✅

```leno
var name = "Leno"
print($"Hello {name}!")   // 自动转换，无需 _str()
```

### 4. `switch case is` 类型匹配与收窄 ✅

```leno
switch w {
    case is Panel { w._bind_window(handle) }
    case is Edit  { w.set_window(handle) }
}
```

### 5. 泛型约束 `func f[T: Face](...)` ✅

### 6. `try-catch-finally` 异常安全 ✅

### 7. GC 兜底机制 ✅

FFI 资源（`ffi.malloc`/`ffi.load`/`ffi.callback`）由 GC 自动追踪，忘记 `ffi.free()` 时 GC 自动回收不会泄漏。

### 8. `format` 全局函数 ✅

```leno
format("%.2f", 3.14159)      // "3.14"
format("%05d", 42)           // "00042"
format("%s: %d", "Leno", 5)  // "Leno: 5"
```

### 9. `?.` 安全访问 / `??` 空值合并 ✅

```leno
root?.set_size(w, h)
var font = _font ?? defaultFont
```

### 10. `for-in` 遍历 / 泛型数组字典 / 数组切片 / 原始字符串 ✅

---

## 经评估后不实现的特性

### defer 延迟执行 — 已回退

defer 引入的 try-finally 包裹与内联优化的 `OP_CLEAR_LOCAL_RANGE` 有架构级冲突。去掉后零影响——FFI 手动内存管理 + GC 兜底 + `try-catch-finally` 功能等价。defer 引入太多不确定性和漏洞，得不偿失。

**替代方案**：`try-catch-finally` + GC 兜底。

### 运算符重载 — 暂不实现

现有功能已足够：
- struct 方法：`a.add(b)`
- 普通函数：`vec_add(a, b)`
- 直接字段运算：`a.x + b.x`（UI 库实际写法，最常见，性能最优）

实现代价过高（parser/codegen/vm/semantic 四层 ~500-800 行改动），且破坏 `OP_ADD_INT`/`OP_ADD_FLOAT` 类型特化快速路径。收益有限——UI 库中不存在 `Vec2 + Vec2` 整体运算场景。

### match 表达式 — 不需要

`switch case is` 已具备类型匹配和收窄能力，功能等价。不需要额外引入 `match` 语法。

### 访问控制关键字 — 暂不实现

当前用下划线前缀约定"私有"（如 `_font`、`_bind_window`），实际使用中已足够。编译器强制的 `private`/`public` 引入语义复杂度，收益不大。

### 数组解构 — 暂不实现

```leno
// 当前写法已经足够
var arr = parseColor(...)
r = arr[0]; g = arr[1]; b = arr[2]
```

收益太小，不值得增加语法复杂度。

### `case is` 逗号合并 — ✅ 已实现

`switch case is` 支持逗号合并多个类型共享同一个 body：

```leno
switch w {
    case is Panel, ScrollView, HBox, VBox, AnchorBox, TabControl, SpinBox {
        w._bind_window(hwnd)
    }
    case is Edit { w.set_window(hwnd) }
}
```

### Dict 解构初始化 — 暂不实现

当前 `opts.get("key", default)` 模式虽然重复，但清晰直观，且已在整个 UI 库中稳定使用。

---

## 优先级总结

| 特性 | 状态 | 说明 |
|------|------|------|
| `Type?` 可空类型 | ✅ 已实现 | 消除 bool 伴生字段 |
| struct 构造/析构函数 | ✅ 已实现 | `func cs()` / `func ~cs()` |
| `new cs(x=1)` 命名参数 | ✅ 已实现 | 字段默认值 + 命名参数 |
| 字符串插值 | ✅ 已实现 | `$"Hello {name}"` |
| `switch case is` | ✅ 已实现 | 类型匹配与收窄 |
| `case is` 逗号合并 | ✅ 已实现 | `case is A, B, C` 多类型共享 body |
| 泛型约束 | ✅ 已实现 | `func f[T: Face](...)` |
| `try-catch-finally` | ✅ 已实现 | 异常安全 |
| GC 兜底 | ✅ 已实现 | FFI 资源自动回收 |
| `format` 全局函数 | ✅ 已实现 | printf 风格格式化 |
| `?.` / `??` | ✅ 已实现 | 安全访问 / 空值合并 |
| defer | ❌ 已回退 | 与内联优化架构冲突 |
| 运算符重载 | ❌ 暂不实现 | 现有写法已足够 |
| match 表达式 | ❌ 不需要 | `switch case is` 已覆盖 |
| 访问控制 | ❌ 暂不实现 | 下划线约定已足够 |
| 数组解构 | ❌ 暂不实现 | 收益太小 |
| Dict 解构初始化 | ❌ 暂不实现 | `opts.get` 模式已稳定 |

> **Leno 语言特性已足够且稳定。** 重心放在库生态（LenoSDL3、LenoWin32、LenoMusic）和运行时稳定性上，不再追加新语法特性。

---

## 附：外部使用者实录（LenoSDL3「数据看板」示例，2026-09-25）

> 一次**外部使用者（AI 代理）实写界面**的踩坑记录：复刻 4 张 TraeTools 页面、给控件库补 3 个控件、修平滑滚动。
> 结论与正文一致：**没有一条需要新增语法** ✓ —— 全都落在**错误信息 / 文档可见度 / lint** 三个层面 ✓。
> 每条都带可复现证据（探针 / 报错原文 ✓）。

### 一、真正的棱角（建议动，成本都极小）

#### 1. `split()` 的元素是 `any`，会污染 `var` 推断 ✗

```leno
var raw = text.replace("\r", "").split("\n")   // raw 的元素是 any ✗
string ln = raw[i]                              // 编译报错：期望 string，实际 any
```

报错原文（提示本身很好，但这一行"看着明显能编译"✗）：
```
错误: [类型不匹配] 变量 'ln' 声明类型与初始化值类型不匹配
  期望类型: string
  实际类型: any
  提示：使用 _str(value) 进行显式转换
```

**建议**：`split()` 的返回类型静态已知 ⇒ 直接给 `Array[string]` ✓
（与 `sdl_edit.leno` 中 `Array[string] _lines = text...split("\n")` 的写法对齐 ✓）。
**收益**：消除"新手卡在门口"的一类报错 ✓。

#### 2. `for A : B` 闭区间 + `arr.len()` 上界 ⇒ 建议加 lint ✗

```leno
for 0 : 3 to ci { ... }   // 闭区间 ⇒ ci = 0,1,2,3（四个角 ✓ 有意为之）
for 0 : 4 to ci { ... }   // 误以为左闭右开 ⇒ ci=4 不匹配任何 case ⇒ 空指针崩溃 ✗
```

（`sdl_renderer.leno` 里那条注释就是这个崩溃的现场记录 ✓）

**建议**：上界表达式形如 `xxx.len()` 时给一条**编译期提示**（"闭区间：这会多跑一轮" ✓）。
**收益**：能编译、能跑、跑到崩 ✗ —— 这类默认值最值得编译器兜一手 ✓。

#### 3. struct 字段默认值不能是 `new X()` ⇒ 报错文案可以再补一句

```leno
export struct Badge impl Widget {
    Label _lbl = new Label()   // ✗ 报错
}
```

报错原文：
```
error: [语义错误] struct 字段 '_lbl' 的默认值不是常量表达式，请使用构造器初始化
```

**实际正解**（语法文档里有，但报错没给 ✓）：
```leno
    Label _lbl
    func Badge() { _lbl = new Label() }   // 构造函数：new 时自动调用 ✓
```

**建议**：在报错里直接附这 3 行 ✓（现在只说"请使用构造器初始化"，使用者得翻文档 ✗ —— 我就翻了 ✓）。
**收益**：这是**复合控件**（持子控件）的必经之路 ✓，而 UI 库里复合控件遍地都是 ✓。

### 二、我没发现的**既有特性**（说明"可发现性"比"有没有"更关键）

以下全在文档里 ✓，但我**一个都没用上** ✗，全程用最笨的写法硬拼：

| 特性 | 我的笨写法 ✗ | 本该这么写 ✓ |
| --- | --- | --- |
| 字符串插值 | `"y=" + _str(y) + " / " + _str(max)` | `$"y={y} / {max}"` |
| 空安全 / 空合并 | `if _lbl != null { _lbl.set_text(t) }` | `_lbl?.set_text(t)` / `??` |
| 构造函数 + 命名参数 | `X x = new X(); x.set(opts)` | `new X(w = 10, h = 20)` |
| 遍历 | `for arr.len() to i { var a = arr[i] ... }` | `for a in arr { ... }` |
| 数字格式化 | `_str(round(v*100)/100)` | `format("%.2f", v)` |
| 类型分派 | 一长串 `is` 判断 | `switch w { case is A, B, C { ... } }` |

**建议（收益最大的一条）**：文档加一份**一页「语法速查表」**，把上面并排 ✓。
**依据**：我是在**读完你这份评估文档**之后才发现它们的 ✓ ⇒ 它们的可见度低于语言的实际能力 ✗。
这不是语法问题，是**入口问题** ✓。

### 三、运行期提示（库/工具层，供参考）

- **孤儿控件静默不画** ✗：`createPanel(...)` 后忘 `wt.add(...)` ⇒ 不报错、不提示、就是看不见 ✗（踩过一次：整块内容区全空 ✓）。
  **建议**：dev 模式在帧末统计"已创建但从未挂载到父级"的控件并打印一次 ✓。
- **动画帧必须每帧请求重绘**（框架约定，但没有任何地方提示 ✗）：动画控件若不每帧 `requestRedraw()`，
  现象就是"画面不动、鼠标一动才更新" ✗（`sdl_scrollview.leno` 的平滑滚动就踩过 ✓，已修 ✓）。
  **建议**：把这条约定写进 `Widget` 接口注释 ✓（一句话省一次排查 ✓）。
- **帧循环的等待被系统计时器 tick 量化** ✗（非语言问题，但影响所有 GUI）：
  窗口内实测（60 次平均）：`waitTimeout` 1ms→23.7 / 16ms→37.3 / 33ms→54.2 ✗；
  纯 `SDL_Delay` **一模一样**（22.8 / 37.3 / 54.2 ✓）⇒ 与事件泵无关，是 OS 睡眠粒度 ✓。
  模型：`实际 ≈ 请求 + ~21ms`（≈1 个 tick 15.6ms + 余量 ✓）⇒ 帧周期落在 tick 网格上：
  请求 16ms 实得 31.2ms（2 格）= **32fps** ✓、请求 33ms 实得 46.8ms（3 格）= **21fps** ✓
  ⇒ 与实测 31.8 / 20.5 fps 逐个吻合 ✓；而空窗口**自计渲染仅 1.7ms/帧** ✓ ⇒ 瓶颈全在等 ✗。
  **建议**：启动时提高计时器精度（Windows：`timeBeginPeriod(1)`，winmm ✓）——
  不改任何算法，只让 sleep 变准 ✓，就能让 16ms → 真正的 60fps ✓。
