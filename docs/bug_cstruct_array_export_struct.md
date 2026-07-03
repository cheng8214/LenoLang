# Bug: export struct 方法内跨模块 cstruct 数组索引赋值崩溃

## 概述

在 `export struct` 的方法内对**跨模块导入的** cstruct 数组进行索引+字段赋值时，**模块加载阶段**即崩溃（0xC0000005），无需调用该方法。

## 受影响场景

`sdl_renderer.leno` 的 `Renderer`（export struct）内使用 `use core.SDL_Vertex` 并在方法中操作 `SDL_Vertex.malloc_array()[i].x = xxx`。

## 排查过程（逐步删减法）

| # | 操作 | 结果 |
|---|------|------|
| 1 | 仅 `use core.SDL_Vertex`（无方法使用） | ✅ |
| 2 | `_test()`: `malloc_array(3)` + `free()` | ✅ |
| 3 | `_test()`: `v[0].x = 1.0` | ❌ 崩溃 |
| 4 | 顶层函数内 `v[0].x = 1.0` | ✅ (同文件/跨模块均可) |
| 5 | `struct`（非 export）内 `v[0].x = 1.0` | ✅ |
| 6 | `export struct` + 同文件 cstruct | ✅ |
| 7 | `export struct` + 跨模块导入 cstruct | ❌ **崩溃** |

## 根因

编译器在处理 `export struct` 方法中跨模块 cstruct 数组的索引字段写操作时，生成的代码访问了无效内存（可能在符号表解析阶段 cstruct 数组索引操作码生成有误）。

## 不受影响

- 非 `export` 的 `struct` — 正常
- 顶层函数 — 正常
- `malloc()` (单个实例, 无索引) — 正常
- 同文件中定义的 cstruct — 正常

## 相关 Bug

- [bug_forward_ref_type_infer.md](./bug_forward_ref_type_infer.md) — 同为 export struct 方法内编译器问题

## 状态

- [ ] 待修复
