# Bug: Array[alias_func_type] 跨模块参数时 [i] 返回整个数组而非元素

## 概述

当 `export func` 的参数类型为 `Array[T]`，且 `T` 是 `export alias T = func(X):Y`（函数类型别名），跨模块调用时 `arr[i]` 的实际返回类型为 `Array[T]` 而非 `T`。

## 复现（真实场景，见 SDL3 库）

**`sdl_window.leno`**（定义方）：
```leno
export alias EventHandler  = func(Event):bool

export func runMultiple(Array[Window] windows, Array[EventHandler] onEvents, ...) {
    ...
    bool cont = onEvents[i](ev)  // ❌ 期望 bool，实际 Array[func(struct Event):bool]
}
```

**编译错误**：
```
[类型不匹配] bool cont = onEvents[i](ev)
  期望类型: bool
  实际类型: Array[func(struct Event):bool]
```

## 条件

必须同时满足：
1. `export alias` 定义函数类型别名（如 `alias Fn = func():int`）
2. 该别名用于 `export func` 的 `Array[alias]` 参数
3. 别名引用的类型来自其他模块（如 `Event` 来自 `sdl_event.leno`）
4. 在函数体内通过 `[i]` 索引访问

## 触发场景

| 场景 | 是否触发 |
|------|----------|
| 同文件内 `var arr = []` + `arr[0]` | ❌ 不触发 |
| `export func(Array[alias] p)` 同文件调用 | ❌ 不触发 |
| `export func(Array[alias] p)` 跨模块调用 | ✅ 触发 |

## 临时规避

使用裸 `Array` 类型 + `is` 类型守卫：

```leno
export func runMultiple(Array[Window] windows, Array onEvents, Array onRenders) {
    var h = onEvents[i]
    if h is EventHandler { h(ev) }
}
```

## 影响

- SDL3 `runMultiple` 多窗口共享事件循环无法用强类型回调数组实现
- 所有跨模块 `Array[FuncAlias]` 参数场景受影响

## 环境

- 提交: 449cb5bf
- 场景: `sdl_window.leno` `runMultiple` 函数

## 状态

- [ ] 待修复
