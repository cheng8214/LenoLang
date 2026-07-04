# 建议：从 SDL3 封装中发现的 Leno 语法改进点

## 1. 整数类型自动收窄（i32/u32/u8 → int）

### 痛点

cstruct 字段和 clib 返回值大量使用 C 布局类型，每次都需要 `as int`：

```leno
pt.x as int          // SDL_Point.x 是 i32
e.scancode as int    // SDL_KeyboardEvent.scancode 是 u16
info.w as int        // SDL_SurfaceInfo.w 是 i32
```

SDL3 封装里 `as int` 出现了 50+ 次。

### 建议

允许 `i32/u32/i16/u16/i8/u8` **隐式收窄**为 `int`（都是整数，位宽变小，无损），但保留 `float → int` 的显式转换要求：

```leno
// ✅ 自动
int x  = pt.x           // i32 → int
int code = e.key        // u32 → int

// ❌ 保留 as
int r = 3.14 as int     // float → int，必须显式
```

### 安全考量

`f32 → int` 截断、`u32 → int` 符号变化有风险，保留显式。但 `i32→int` 等同源整数无风险，可自动。

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

### 建议

```leno
var {w, h} = getOutputSize()
```

影响：SDL3 封装中 10+ 处 Dict return 模式，每处省 2 行。

## 优先级

| 特性 | 收益 | 优先级 |
|------|------|--------|
| 整数自动收窄 | 删 50 行 `as int` | ⭐1 |
| 解构赋值 | 每处省 2 行 | ⭐2 |
