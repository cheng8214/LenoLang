# Bug: `use` 导入的类型不能通过链式 `import` 访问

## 环境
- 发现: `b87e055c`
- 复现: `examples/测试/repro_use_null_*.leno`

## 现象

模块 B 用 `use` 导入的类型（enum/alias 等），外部 C 通过 `import B as b` 无法访达，`b.TypeName` 为 `null`：

```leno
// A.leno: export enum Flag { ON = 1; OFF = 0 }

// B.leno: import "A.leno" as a; use a.Flag  ← B 自己能用 Flag.ON

// C.leno: import "B.leno" as b
main() {
    print(b.Flag)       // → null
    print(b.Flag.ON)    // → 崩溃: "索引操作需要对象类型，但实际类型为 'null'"
}
```

## 影响

SDL3.leno 中 `use core.Scancode` 等 10 个枚举/别名，用户通过 `import SDL3.leno` 后用 `SDL3.Scancode.ESCAPE` 得到 null，运行时崩溃。

**当前 workaround**：用户必须自己加 `use SDL3.Scancode`，然后直接用 `Scancode.ESCAPE`（不带 `SDL3.` 前缀）。

## 对比：struct 链式 import 部分正常

```leno
// A.leno: export struct Info { string name }
// C.leno: import A as a
main() {
    new a.Info(name="x")  // ✅ 构造调用 OK
    a.Info y              // ❌ 类型声明 语法错误
    func f(a.Info x)      // ❌ 参数类型 语法错误
}
```

`import` 别名只能用于 `new` 构造，不能作为类型名声明变量/参数/返回类型。

## 状态

- [ ] 待修复
