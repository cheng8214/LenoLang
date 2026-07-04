# Bug: `use` 导入跨模块 enum 存在问题

## 环境
- 提交: 最新 main（enum 已实现）

## 测试文件
- `examples/enum/test_enum_facade2.leno` — 跨文件 facade 模式
- `examples/enum/test_test_enum_use.leno` — `use` 导入 enum
- `examples/enum/test_enum_alias.leno` — enum + alias 组合

## Bug 1: `use` 导入 enum 后显式值丢失

### 复现
```leno
// A.leno
export enum Scancode { ESCAPE = 41; SPACE = 44; A = 4; B = 5 }

// B.leno
import "A.leno" as core
use core.Scancode

main() {
    print(Scancode.ESCAPE)  // 输出 0，期望 41
    print(Scancode.SPACE)   // 输出 1，期望 44
}
```

`use` 导入跨模块 enum 时，所有成员的显式值被重置为自动递增（0, 1, 2, ...）。

### 影响的 SDL3 场景
```leno
// sdl_core.leno
export enum Scancode { ESCAPE = 41; SPACE = 44; ... }

// SDL3.leno
import "sdl_core.leno" as core
use core.Scancode     // ← 所有值变 0,1,2...，彻底断裂！

// 用户侧
if e.scancode() == SDL3.Scancode.ESCAPE  // 永远 false
```

## Bug 2: `use` 导入 enum 运行时崩溃

### 复现
```leno
// color_module.leno
export enum Color { red; green; blue }

// test.leno
import "color_module.leno" as cm
use cm.Color

main() {
    print(Color.red)  // 崩溃: 0xC0000005 Access Violation
}
```

与 Bug 1 不同——无显式值的 enum 直接崩溃，而非值错误。

## Bug 3: `alias` 不支持 enum 成员做值

### 复现
```leno
enum Signal { low = 0; mid = 5; high = 10 }
alias DefaultSignal = Signal.mid  // 语法错误: "期望表达式"
```

### 期望行为
```leno
alias DefaultSignal = Signal.mid  // 编译为 var DefaultSignal = 5
```

## 总结

| Bug | 严重度 | 现象 |
|-----|--------|------|
| `use` enum 值丢失 | 🔴 致命 | 显式值重置为 0,1,2... |
| `use` enum 崩溃 | 🔴 致命 | Access Violation crash |
| `alias = enum.member` | 🟡 次要 | 语法错误 |

目前只能通过 `import` + `module.Enum.member` 路径使用跨模块 enum，无法用 `use` 缩短路径。SDL3.leno 的 80 行 `export var` 暂时不能替换为 `enum`。
