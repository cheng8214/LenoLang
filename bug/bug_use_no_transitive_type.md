# Bug: `use` 不携带类型依赖

## 现象

```leno
// sdl_window.leno:  export alias EventHandler = func(Event):bool
// sdl_dialog.leno:
use wnd.EventHandler    // 拿到了 func(Event):bool，但 Event 不可见!
// 必须额外 import sdl_event.leno 才能用 EventHandler
```

`use` 导入类型别名时，别名引用的底层类型（如 `EventHandler → func(Event):bool` 中的 `Event`）不会自动带入当前作用域。

## 影响

- `sdl_dialog.leno` 需要显式 import `sdl_event.leno` + `sdl_renderer.leno`
- 所有依赖别名类型的模块都需要手动补链

## 状态

- [ ] 待修复
