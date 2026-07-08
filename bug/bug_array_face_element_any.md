# Bug：`Array[face]` 元素退化为 `any`，且 `as` 裸表达式不收窄

## 环境
- 发现: 2026-07-08
- 复现: `assert/_tmp_face_a.leno` + `assert/_tmp_face_b.leno`
- 状态: ⚠️ 待修复（已找到可用 workaround）

## 背景

在 `LenoSDL3` 做 GUI 控件统一注册时，需要把 `Button`/`Label`/`Edit`（`impl Widget`）放进
`Array[Widget]` 由 `Window.run()` 统一调度。过程中发现 face 类型作为数组元素时存在类型推断问题。

`Widget` 定义在 `sdl_widget.leno`：
```leno
export face Widget {
    func process(Event ev)
    func render(Renderer r)
    func dispose()
}
```

## 已逐条验证的行为（repro 实测，非推测）

| 代码 | 结果 |
|------|------|
| `Array[F] arr; arr.add(s)`（`s` 为 `impl F` 的 struct，不加 `as`） | ❌ 语义错误：期望 face，实际 struct |
| `arr.add(s as F)` | ✅ 通过 |
| `arr[0].f()`（索引后直接调用 face 方法） | ❌ 不能在 any 类型上调用方法 'f' |
| `for arr to w { w.f() }`（遍历变量直接调用） | ❌ 不能在 any 类型上调用方法 'f' |
| `for arr to w { if w is F { w.f() } }` | ✅ 通过 |
| `var e = arr[0]; if e is F { e.f() }` | ✅ 通过 |
| `var fa = a0 as F; fa.f()`（`as` 绑定到变量后调用） | ✅ 通过 |
| `(a0 as F).f()`（`as` 作为**裸表达式**直接调用） | ❌ 不能在 any 类型上调用方法 'f' |

## 根因分析

语义分析阶段，当数组的泛型实参为 **face 类型** 时：
1. 数组元素的静态类型被推导为 `any`，而非该 face 类型。
   因此 `arr[i]` 与 `for-to` 迭代变量都退化为 `any`，无法在其上直接调用 face 方法。
2. `as` 表达式的结果类型推断存在不一致：
   - 绑定到变量声明时（`var fa = x as F`）——结果被正确收窄为 `F`，可调用方法。
   - 作为裸表达式时（`(x as F).method()`）——结果仍被当作 `any`，调用报错。
   同一 `as` 操作，仅因是否绑定到变量而结果不同，属于类型推断不一致。

> 说明：第 1 点是否属于“bug”存在设计取舍空间（`is` 收窄本身是语言支持的合法用法，
> 可能意图就是“face 容器元素需在使用处收窄”）。但第 2 点的 `as` 裸表达式不收窄是
> 明确的**行为不一致**，属于真 bug。两者建议一并修复，使 face 数组与 `as` 的行为自洽。

## 可用 workaround（已在 `sdl_window.leno` 落地）

`run()` 中统一用 `for-to` + `is` 收窄遍历控件：

```leno
var ws = _runWidgets[i]            // Array[Widget]
for ws to w {
    if w is Widget {                // is 收窄后 w 为 Widget，方法可调用
        w.process(ev)
        w.render(ren)
    }
}
```

存入数组时显式 `as`：
```leno
_widgets.add(b as Widget)          // b 为 struct Button，需 as 转 face
```

## 建议修复方向

1. 泛型实参为 face 时，保留数组元素/迭代变量的 face 类型（而非退化为 `any`），
   使 `arr[i].method()` 与 `for-to` 变量直接调用可用。
2. 统一 `as` 表达式与变量绑定两种路径的结果类型推断，使 `(x as F).method()` 与
   `var f = x as F; f.method()` 行为一致。

## 状态
- [ ] 待修复
- [x] workaround 已验证可用（三控件例子 `test_button`/`test_edit`/`test_label` 全部 EXIT=0）

## 附注（非 bug，仅记录）
- 泛型类型实参中不能直接写模块限定名 `Array[a.F]`，需先 `use a.F` 再写 `Array[F]`。
  此为语法限制，有明确 workaround，不列为 bug。
- 索引访问需收窄、`as` 裸表达式不收窄均为本次实测确认，非对 Leno 特性的误解。
