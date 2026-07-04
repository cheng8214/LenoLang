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
int x = pt.x       // i32 → int，语义：取整数值
int code = e.key    // u32 → int

// ❌ 保留 as
int r = 3.14 as int  // float → int，必须显式
```

## 2. `export *` 批量导出常量模块

### 痛点

`SDL3.leno` 中 80+ 行重复代码：

```leno
export var SCANCODE_A = core.SCANCODE_A
export var SCANCODE_B = core.SCANCODE_B
// ... 80 more lines
export var INIT_VIDEO = core.INIT_VIDEO
export var EVENT_QUIT = core.EVENT_QUIT
```

### 建议一：`export *` 语法

```leno
// 把 core 模块所有 export var 全部透传
export * from core
```

### 建议二：`enum` 关键字（更优雅）

把这些常量组声明为枚举，自动获得命名空间：

```leno
// sdl_core.leno
export enum Scancode {
    ESCAPE = 41, RETURN = 40, SPACE = 44, ...
    A = 4, B = 5, C = 6, ...
}

// 用户侧
if e.scancode() == SDL3.Scancode.ESCAPE { ... }
```

优势：
- 一行 `use SDL3.Scancode` 就能用所有值
- 编译期枚举值检查
- 不需要 80 行 `export var`

## 3. 解构赋值

### 痛点

Dict 返回模式非常啰嗦：

```leno
var d = getOutputSize()
int w = d.w; int h = d.h
var d = getTextureSize()
float fw = d.w; float fh = d.h
```

### 建议

```leno
var {w, h} = getOutputSize()    // 类型从 getOutputSize(): Size 推断
float w, h = getTextureSize()   // 显式类型
```

影响：SDL3 封装中 10+ 处 Dict return 模式。

## 优先级建议

| 特性 | 收益 | 实现成本 | 优先级 |
|------|------|---------|--------|
| `export *` / enum | 删 80 行重复 | 中 | ⭐1 |
| 整数自动收窄 | 删 50 行 as int | 低 | ⭐2 |
| 解构赋值 | 每处省 2 行 | 中 | ⭐3 |
