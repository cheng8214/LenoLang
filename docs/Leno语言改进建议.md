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

> **状态：已回退**。经实践评估，defer 引入的 try-finally 包裹与内联优化的 `OP_CLEAR_LOCAL_RANGE` 有架构级冲突，修一个漏一个。去掉后零影响——FFI 手动内存管理（`ffi.malloc()` + `ffi.free()` 配对）已经很简洁，GC 兜底机制完善（忘记 `free` 时 GC 自动回收 `ObjFFIPointer` 不会泄漏），需要异常安全时用 `try-catch-finally` 功能等价。defer 引入太多不确定性和漏洞，得不偿失。

<details>
<summary>原始建议（已废弃，仅供参考）</summary>

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

</details>

**替代方案（已采用）**：`try-catch-finally` + GC 兜底。详见《FFI使用指南》8.2 节。

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

> **状态：评估后建议暂不实现**。经分析，现有功能已经足够应对 UI 库需求，运算符重载的收益不足以覆盖其引入的复杂度。

**原始设想**：

```leno
struct Vec2 { float x; float y }
func +(Vec2 a, Vec2 b): Vec2 { return Vec2{x: a.x+b.x, y: a.y+b.y} }
```

**评估结论——现有功能已足够**：

1. **UI 库实际用法分析**：SDL3 库中坐标运算全部是 `float x, float y` 独立变量或 struct 字段（如 `pt.x + offset`），不存在 `Vec2 + Vec2` 整体运算的场景。运算符重载解决的是一个**不存在的痛点**。

2. **现有等价写法已经足够简洁**：

```leno
// 方式一：struct 方法（已有，推荐）
struct Vec2 {
    float x, y
    func add(Vec2 other): Vec2 {
        return Vec2{x: x + other.x, y: y + other.y}
    }
}
var c = a.add(b)  // 比 a + b 多几个字符，但语义清晰

// 方式二：普通函数（已有）
func vec_add(Vec2 a, Vec2 b): Vec2 {
    return Vec2{x: a.x + b.x, y: a.y + b.y}
}
var c = vec_add(a, b)

// 方式三：直接字段运算（UI 库实际写法，最常见）
float nx = a.x + b.x
float ny = a.y + b.y
```

3. **实现代价过高**：
   - **Parser**：需要在 struct 方法解析中支持运算符函数名（`+`、`-`、`*`、`==` 等）作为方法名
   - **Codegen**：`gen_binary()` 中每个 `TOK_PLUS`/`TOK_MINUS`/... 分支都需检查操作数是否为 struct 类型，如是则改走 `OP_GET_METHOD` + `OP_CALL` 路径——破坏当前的类型特化优化（`OP_ADD_INT`/`OP_ADD_FLOAT` 快速路径）
   - **VM**：`OP_ADD` 等操作码需增加 struct 方法分派逻辑，增加热路径分支
   - **语义分析**：需处理运算符方法的签名检查、左右操作数类型匹配、交换律等
   - 估算改动：~500-800 行 C 代码，涉及 parser/codegen/vm/semantic 四层

4. **收益有限**：UI 库中坐标运算用直接字段加法 `a.x + b.x` 已经足够自然，且性能最优（直接 float 运算，无方法分派开销）。运算符重载只对"值语义数学库"有价值，而 Leno 的定位不是数学语言。

**结论**：现有 struct 方法 + 普通函数 + 直接字段运算已覆盖所有实际场景。运算符重载引入的复杂度（parser/codegen/vm/semantic 四层改动）远超收益（语法糖层面的微小便利），**建议暂不实现**。

---

## 附：已有特性但 UI 库未充分使用

以下特性 Leno 已支持，但 UI 库代码中仍在用旧写法，建议逐步迁移：

| 已有特性 | 语法 | UI 库中的旧写法 | 迁移建议 |
|---------|------|----------------|---------|
| 字符串插值 | `$"Hello {name}"` | `"Hello " + name` | 全局替换拼接为插值 |
| 泛型约束 | `func f[T: Face](...)` | 手动类型检查 | 新代码使用约束 |

---

## 优先级总结

| 优先级 | 特性 | 直接痛点 | 预估减少代码量 |
|--------|------|---------|--------------|
| 高 | `Type?` 可空类型 | 142 个 null 警告 + bool 伴生字段 | ~30% 控件样板 |
| 高 | Dict 解构初始化 | 每个 set() 20+ 行重复赋值 | ~20% 控件代码 |
| 高 | `case is` 逗号合并 | 9 层重复 case-is | ~50% 分发代码 |
| 中 | ~~defer~~（已回退） | 忘记清理资源 | 用 try-finally + GC 兜底替代 |
| 中 | 访问控制 | 内部 API 无保护 | 设计规范 |
| 低 | 数组解构 | 多返回值不便 | 小幅 |
| 低 | ~~运算符重载~~（暂不实现） | 坐标运算不自然 | 现有 struct 方法+直接字段运算已足够 |

> 前三项（可空类型、Dict 解构、case is 合并）如果能实现，Leno 的 UI 库代码量预计可减少 25-35%，且显著降低 bool 标志与 struct 不同步的隐性 bug 风险。
