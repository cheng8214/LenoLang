# Leno 语言改进建议

基于对 LenoSDL3 完整 UI 库（~6000 行）的深入阅读和 142 个 struct null 警告的修复实践，以下是从实际痛点出发的改进建议，按优先级排列。

> 已有特性（字符串插值、for-in 遍历、泛型约束）不再列入建议，但 UI 库应积极使用。

---

## 一、高优先级：消除大量样板代码

### 1. struct 可空类型 `Type?`

**痛点**：struct 值类型声明即分配，永远不为 null，导致懒初始化模式需要额外的 bool 标志。本次修复中，仅 `_font` 一个字段就在 16 个文件中生成了 `bool _fontDirty`、`_fontDirty = true`、`_fontDirty = false` 等上百处样板代码。

**现状**：
```leno
Font _font
bool _fontDirty = true        // 仅因为 struct 不能为 null

func _ensure_font() {
    if _fontDirty or not _font.ok {   // 两个条件缺一不可
        if _font.ok { fnt.releaseFont(_font) }
        _font = fnt.acquireFontAuto(_fontSize)
        _fontDirty = false
    }
}

func set_font_size(float s) {
    _fontSize = s
    if _font.ok { fnt.releaseFont(_font) }; _fontDirty = true  // 容易漏写
}
```

**建议**：引入 `Type?` 可空类型语法糖。编译到运行时等价于"全零默认值 + ok=false"，语义上等于 null：

```leno
Font? _font                    // 可空 Font，初始为 null

func _ensure_font() {
    if _font == null or not _font.ok {   // 直觉清晰
        if _font != null { fnt.releaseFont(_font) }
        _font = fnt.acquireFontAuto(_fontSize)
    }
}

func set_font_size(float s) {
    _fontSize = s
    if _font != null { fnt.releaseFont(_font); _font = null }  // 一行搞定
}
```

**收益**：消除 `bool _fontDirty` / `_hasCtxMenu` / `_hasEdit` / `_hasBlinkTimer` 等所有伴生 bool 字段，减少约 30% 的控件样板代码，且 bool 与 struct 不同步的隐性 bug不再可能。

---

### 2. Dict 解构初始化 / struct 构造器

**痛点**：每个控件的 `set(Dict opts)` 方法中充满了 `x = opts.get("x", 0)` 式的重复代码。一个典型的 Button.set() 有 20+ 行仅做字段赋值：

```leno
func set(Dict opts) {
    text      = opts.get("text", "Button")
    x         = opts.get("x", 0)
    y         = opts.get("y", 0)
    _fontSize = opts.get("font_size", 15)
    _fontPath = opts.get("font_path", "")
    _fontStyle = opts.get("font_style", 0)
    _radius   = opts.get("radius", 6.0)
    enabled   = opts.get("enabled", true)
    visible   = opts.get("visible", true)
    _tip      = opts.get("tip", "")
    // ... 还有 10 行
}
```

**建议**：支持 struct 字段声明时指定 Dict 键名和默认值，`set()` 自动解构：

```leno
export struct Button impl Widget {
    string text = "Button"          // 字段名即键名，等号右侧即默认值
    float x = 0.0; float y = 0.0
    float _fontSize = 15.0  @key("font_size")    // 键名与字段名不同时用 @key
    string _fontPath = ""   @key("font_path")
    // ...
}

// 使用时一行搞定，自动从 Dict 提取匹配字段
Button b = new Button(); b.set(opts)
```

**收益**：每个控件减少 15-30 行样板代码，且字段声明、默认值、Dict 键名三合一，不易遗漏。

---

### 3. `case is` 逗号合并

**痛点**：Widget 类型分发是 UI 库最频繁的操作，当前只能用 `if ... is` 链：

```leno
func add(Widget w): Widget {
    if w is Panel     { w._bind_window(handle); return w }
    if w is ScrollView { w._bind_window(handle); return w }
    if w is HBox      { w._bind_window(handle); return w }
    if w is VBox      { w._bind_window(handle); return w }
    if w is AnchorBox { w._bind_window(handle); return w }
    if w is TabControl { w._bind_window(handle); return w }
    if w is Edit      { w.set_window(handle); return w }
    if w is SpinBox   { w._bind_window(handle); return w }
    if w is TreeView  { w.set_window_handle(handle); return w }
    return w
}
```

**现状**：`switch case is` 已支持类型匹配和收窄（`switch w { case is Panel { ... } }`），
但同处理逻辑的多个类型必须重复写多个 case，无法合并。

**建议**：支持 `case is` 逗号合并语法，多个类型共享同一个 body：

```leno
func add(Widget w): Widget {
    switch w {
        case is Panel, ScrollView, HBox, VBox, AnchorBox, TabControl, SpinBox {
            w._bind_window(handle)
        }
        case is Edit {
            w.set_window(handle)
        }
        case is TreeView {
            w.set_window_handle(handle)
        }
    }
    return w
}
```

**收益**：类型分发代码量减少 50%+，同处理逻辑的类型可合并，可读性大幅提升。
（`match is` 不再需要——`switch case is` 已具备类型收窄能力，只需加逗号合并即可）

---

## 二、中优先级：提升开发效率

### 4. `defer` 延迟执行

**痛点**：资源清理模式需要每个退出路径都手动调用 dispose：

```leno
func render(Renderer r) {
    SDL_Rect cr = SDL_Rect.malloc()
    r.pushClipRect(cr.to_ptr())
    // ... 渲染逻辑 ...
    r.popClipRect()        // 必须记得手动 pop
    cr.free()              // 必须记得手动 free
}
```

**建议**：

```leno
func render(Renderer r) {
    SDL_Rect cr = SDL_Rect.malloc()
    defer { cr.free() }
    r.pushClipRect(cr.to_ptr())
    defer { r.popClipRect() }
    // ... 渲染逻辑，无论如何退出都会自动 pop + free
}
```

**收益**：彻底消除"忘记清理"类 bug，尤其在多 return 路径的函数中。

---

### 5. 访问控制关键字

**痛点**：当前用下划线前缀约定"私有"（如 `_font`、`_bind_window`），但无编译器强制，外部仍可访问。库作者无法防止用户误用内部 API。

**建议**：

```leno
export struct Button impl Widget {
    private Font _font           // 编译器禁止外部访问
    private func _ensure_font()  // 编译器禁止外部调用
    public func set_text(string t)  // 显式公开（默认 public）
}
```

---

## 三、低优先级：锦上添花

### 6. 数组解构

```leno
var [r, g, b] = parseColor("#ff8800")   // 函数返回 Array，直接解构
```

当前需要 `var arr = parseColor(...); r = arr[0]; g = arr[1]; b = arr[2]`。

### 7. 运算符重载

```leno
struct Vec2 { float x; float y }
func +(Vec2 a, Vec2 b): Vec2 { return Vec2{x: a.x+b.x, y: a.y+b.y} }
```

UI 库中坐标/尺寸运算频繁，运算符重载能让代码更自然。

---

## 附：已有特性但 UI 库未充分使用

以下特性 Leno 已支持，但 UI 库代码中仍在用旧写法，建议逐步迁移：

| 已有特性 | 语法 | UI 库中的旧写法 | 迁移建议 |
|---------|------|----------------|---------|
| 字符串插值 | `$"Hello {name}"` | `"Hello " + name` | 全局替换拼接为插值 |
| for-in 遍历 | `for arr to item` | `for arr.len() to i { arr[i] }` | 改用 for-in，带索引用 `for arr to item, idx` |
| 泛型约束 | `func f[T: Face](...)` | 手动类型检查 | 新代码使用约束 |

---

## 优先级总结

| 优先级 | 特性 | 直接痛点 | 预估减少代码量 |
|--------|------|---------|--------------|
| 高 | `Type?` 可空类型 | 142 个 null 警告 + bool 伴生字段 | ~30% 控件样板 |
| 高 | Dict 解构初始化 | 每个 set() 20+ 行重复赋值 | ~20% 控件代码 |
| 高 | `case is` 逗号合并 | 9 层重复 case-is | ~50% 分发代码 |
| 中 | defer | 忘记清理资源 | 防止 bug |
| 中 | 访问控制 | 内部 API 无保护 | 设计规范 |
| 低 | 数组解构 | 多返回值不便 | 小幅 |
| 低 | 运算符重载 | 坐标运算不自然 | 小幅 |

> 前三项（可空类型、Dict 解构、case is 合并）如果能实现，Leno 的 UI 库代码量预计可减少 25-35%，且显著降低 bool 标志与 struct 不同步的隐性 bug 风险。
