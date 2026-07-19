# BUG: FFI `Ptr` 类型擦除导致指针目标类型信息丢失

## 严重程度

中等 — 不崩溃，但静默产生错误数据，难以排查。

## 问题描述

Leno FFI 层在取 cstruct 字段地址时，将所有 `Ptr<i32>`、`Ptr<f32>`、`Ptr<u8>` 统一擦除为不透明 `Ptr` 类型。编译器无法区分指针指向的数据的实际类型。

当开发者**误将 `SDL_Point`（`i32` 字段）的地址传给期望 `float*` 的 SDL3 API 时**，编译器**不报错也不警告**。float 的 IEEE 754 位模式被原样写入 int 内存，读取时得到完全错误的值。

## 复现

运行 `test_ptr_type_erase.leno`：

```
[错误] SDL_Point(i32) 读回: 1136230400    ← float 位模式错解为 int
[正确] SDL_FPoint(f32) 读回: 371           ← 正确坐标
```

## 根因分析

```leno
// SDL_Point: i32 x, i32 y
cstruct SDL_Point { i32 x; i32 y }

// SDL3 声明: SDL_GetGlobalMouseState(float *x, float *y) → 期望 float*
u32 SDL_GetGlobalMouseState(Ptr x, Ptr y)

// 误用：传入 SDL_Point 字段地址（实际是 int*）
var pt = SDL_Point.malloc()
lib().SDL_GetGlobalMouseState(&pt.x, &pt.y)  // ← 无编译错误！
int x = pt.x  // ← 读到 float 的 IEEE 754 位模式，例如 1136230400
```

关键链路：
1. `&pt.x` 返回 `Ptr`（不透明），编译器不知道它指向 `i32`
2. 函数签名 `SDL_GetGlobalMouseState(Ptr, Ptr)` 也接受 `Ptr`
3. 类型检查通过 ✅ — 但实际上语义错误 ❌
4. SDL 按 `float*` 写入 4 字节，Leno 按 `int*` 读取 4 字节 → 值全乱

## 正确写法

必须用字段类型匹配的 cstruct：

```leno
// SDL_FPoint: f32 x, f32 y  ← 匹配 SDL_GetGlobalMouseState 的 float* 参数
var pt = SDL_FPoint.malloc()
lib().SDL_GetGlobalMouseState(&pt.x, &pt.y)
float x = pt.x  // ← 正确
```

## 期望行为

编译器在以下情况应该**报错或至少警告**：

- `&struct_field` 传给函数时，如果 `struct_field` 的类型（`i32`）与函数参数名义类型（从 `f32*` 语义推导）不匹配
- 或者，`&` 运算符应产生带类型标记的指针（如 `Ptr<i32>`），与函数参数的类型标记匹配

## 影响范围

所有通过 `Ptr` 传 cstruct 字段地址的 SDL3 API 调用，如果字段类型与 C 函数期望的类型不一致，都会静默出错。
