# Bug: `use` 链式导入不传递（A ← B ← C）

## 环境
- 发现: `d94c4342`（enum 单层 use 已修复）
- 复现文件: `examples/enum/chain_*.leno`

## 现象

三层模块链式 `use`，C → B → A 传递断裂：

```leno
// chain_c.leno — 定义 enum
export enum Scancode { ESCAPE = 41; SPACE = 44 }

// chain_b.leno — use 导入 c 的 enum
import "chain_c.leno" as c
use c.Scancode          // B 自己能用

// chain_a.leno — use 导入 b
import "chain_b.leno" as b
use b.Scancode          // ❌ "模块 'b' 中没有 struct、clib、face 或 alias 类型 'Scancode'"
```

所有通过 `use` 间接导入的类型（enum/alias）都不能被更上层再次 `use`。

## 影响

SDL3.leno facade 模式无法完全用 `use` 链式实现：

```
sdl_core.leno      export enum Scancode { ... }
     ↓ use
sdl_xxx.leno       中间模块用 use 引入
     ↓ use
SDL3.leno          顶层 facade 无法 use 传递
```

## 错误信息

```
use 语句错误：模块 'b' 中没有 struct、clib、face 或 alias 类型 'xxx'
```

错误信息本身也需要更新——enum 已经实现，但提示仍只说 struct/clib/face/alias。

## 复现

```
leno examples/enum/chain_a.leno
```

## 状态

- [ ] 待修复
