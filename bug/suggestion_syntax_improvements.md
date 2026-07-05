# 建议：从 SDL3 封装中发现的 Leno 语法改进点

## 1. 整数类型自动收窄（i32/u32/u8 → int）✅ 已实现

cstruct 字段（`i32/u32/i16/u16/i8/u8`）**隐式转换为 `int`**，无需 `as int`：

```leno
// ✅ 自动（cstruct 字段）
int x   = pt.x            // i32 → int
int code = e.scancode     // u16 → int
int sum = pt.x + pt.y     // 算术运算

// ❌ 保留 as（clib 返回/Dict.get）
int t = lib().SDL_GetTicks() as int    // clib 返回仍需要
int w = opts.get("w", 800) as int      // Dict.get → var
int r = 3.14 as int                     // float → int 显式
```

SDL3 封装中 18 处 `as int` 已删除（仅剩 clib 返回和 Dict.get 的 9 处必需）。

## 2. `enum` 关键字 ✅ 已实现

```leno
export enum Scancode { ESCAPE = 41; SPACE = 44; A = 4; ... }
```

已全面应用到 SDL3 库，替代 140 行 `export var`，支持 `use` 导入和链式 `import` 访问。

## 3. 解构赋值

### 痛点

```leno
var d = getOutputSize()
int w = d.w; int h = d.h
float fw = d.w; float fh = d.h
```

SDL3 封装中 10+ 处 Dict return 模式，每处省 2 行。

### 类型问题

Dict 的值是 `var`/`any`，解构时必须显式标注类型。两种方案：

**方案 A：前缀类型**（每个字段独立）

```leno
var {int w, float h, string name} = getInfo()
```

灵活但字段多时啰嗦。

**方案 B：统一类型**（推荐）

```leno
int {w, h}    = r.getOutputSize()   // 90% 场景，同类型
float {fw, fh} = r.getTextureSize()
```

SDL3 中 `{w, h}` 永远是同类型，B 方案简洁够用。A 用于混合类型场景。编译期检查键名 + 值类型兼容性。

## 优先级

| 特性 | 收益 | 优先级 |
|------|------|--------|
| 解构赋值 | 每处省 2 行 | ⭐1 |
