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

### 建议：`enum` 关键字 ✅ 已实现

`enum` 已在最新版本中实现，语法：

```leno
// sdl_core.leno
export enum Scancode {
    ESCAPE = 41, RETURN = 40, SPACE = 44, ...
    A = 4, B = 5, C = 6, ...
}

// 用户侧 — import 模式（当前可用）
import "SDL3.leno" as SDL3
if e.scancode() == SDL3.Scancode.ESCAPE { ... }
```

优势：编译期枚举值检查，不需要 80 行 `export var`。

⚠️ **当前限制**（见 `bug/bug_enum_use_cross_module.md`）：
- `use` 导入跨模块 enum 有 bug（显式值丢失/崩溃）
- 暂时只能用 `import` + `module.Scancode.ESCAPE` 全路径访问
- 修复后即可 `use SDL3.Scancode` 一行替代 80 行

> `export *` 不需要 —— enum 命名空间隔离已足够，`export *` 反而制造冲突。

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
