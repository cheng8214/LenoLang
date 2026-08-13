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
