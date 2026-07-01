# Bug: alias 类型不支持跨模块重新导出

## 现象

`export alias` 定义的别名无法通过 `use` 或 `export alias` 在其他模块中重新导出。

## 复现

```leno
// types.leno
export alias MySize = Dict[string, int]
```

```leno
// wrapper.leno — 想让调用方只 import wrapper
import "types.leno" as t

// ❌ 方式1: 语法错误 "期望表达式"
export alias MySize = t.MySize

// ❌ 方式2: 语义错误 "模块中没有 struct/clib/face 类型"
use t.MySize

// ✅ 唯一办法: 重复定义
alias MySize = Dict[string, int]  // 重复代码，破坏 DRY 原则
```

## 影响

SDL3.leno 作为入口模块，无法重新导出 sdl_core.leno 中的类型别名（Color/Size 等），用户必须直接 import sdl_core 或者重复定义别名。

## 期望行为

`use` 语句支持 alias 类型导入，或者 `export alias X = module.X` 支持跨模块别名引用。

## 相关文件

- `examples/import/alias_bug/types.leno`
- `examples/import/alias_bug/wrapper.leno`
