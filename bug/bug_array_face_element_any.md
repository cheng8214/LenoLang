# Bug：`Array[face]` 索引访问退化为 `any`，且 `as` 裸表达式不收窄

> 重要更正（2026-07-09）：本文初版误称“`for arr to w { w.f() }` 遍历变量报 any”，
> 经用复现文件**实际编译**确认该写法在当前版本**已正常工作**。特此更正，
> 仅保留经实测可复现的两个问题。

## 环境
- 发现: 2026-07-08
- 复现文件（保留，不删除）:
  - 同模块: `examples/测试/repro_array_face_same.leno`
  - 跨模块: `examples/测试/repro_array_face_face.leno`（face 定义）+
    `examples/测试/repro_array_face_impl.leno`（struct impl）+
    `examples/测试/repro_array_face_cross.leno`（驱动）
- 验证二进制: `build/leno.exe`，发现问题时构建时间 2026-07-08 21:41（源码 `src/` 干净）；
  修复后重建二进制时间 2026-07-09 0:18:19
- 状态: ✅ 已修复（2026-07-09，用户修复后重建二进制；复现文件实测 EXIT=0）

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

## 已逐条验证的行为（复现文件实测，非推测）

| 代码 | 结果 |
|------|------|
| `Array[F] arr; arr.add(s)`（`s` 为 `impl F` 的 struct，不加 `as`） | ❌ 语义错误：期望 face，实际 struct |
| `arr.add(s as F)` | ✅ 通过 |
| `arr[0].f()`（**索引访问**后直接调用 face 方法） | ❌ 不能在 any 类型上调用方法 'f' |
| `for arr to w { w.f() }`（**遍历变量**直接调用） | ✅ 通过（初版误报为 ❌，已更正） |
| `for arr to w { if w is F { w.f() } }` | ✅ 通过 |
| `var e = arr[0]; if e is F { e.f() }` | ✅ 通过 |
| `var fa = a0 as F; fa.f()`（`as` 绑定到变量后调用） | ✅ 通过 |
| `(a0 as F).f()`（`as` 作为**裸表达式**直接调用） | ❌ 不能在 any 类型上调用方法 'f' |

> 实测关键对比：**`arr[0].f()`（索引）报错，而 `for arr to w { w.f() }`（遍历）正常**。
> 说明数组迭代变量已正确推导为 face 类型，唯独**下标索引访问** `arr[i]` 退化为 `any`。

## 根因分析

语义分析阶段，当数组的泛型实参为 **face 类型** 时：
1. **仅下标访问** `arr[i]` 的结果类型被推导为 `any`，而非该 face 类型；
   而 `for-to` 迭代变量、变量绑定均能正确收窄为 `face`，可调用方法。
   （初版“迭代变量也退化为 any”的描述有误，已更正。）
2. `as` 表达式的结果类型推断存在不一致：
   - 绑定到变量声明时（`var fa = x as F`）——结果被正确收窄为 `F`，可调用方法。
   - 作为裸表达式时（`(x as F).method()`）——结果仍被当作 `any`，调用报错。
   同一 `as` 操作，仅因是否绑定到变量而结果不同，属于类型推断不一致（确凿真 bug）。

> 第 1 点属于“特定路径（下标访问）类型丢失”，第 2 点是“`as` 表达式与变量绑定路径行为不一致”。
> 两者建议一并修复，使 face 数组的下标访问与 `as` 表达式的行为自洽。

## 可用 workaround（历史记录，bug 修复后已不再需要）

> 以下为修复前的临时绕法。修复后 `for-to` 遍历变量已正确收窄为 `Widget`，
> `sdl_window.leno` 中的 `if w is Widget { }` 收窄已移除，直接 `w.process(ev)` / `w.render(ren)` 即可。

修复前 `run()` 中统一用 `for-to` + `is` 收窄遍历控件：

```leno
var ws = _runWidgets[i]            // Array[Widget]
for ws to w {
    if w is Widget {                // 修复前：is 收窄后 w 为 Widget，方法可调用
        w.process(ev)
        w.render(ren)
    }
}
```

修复前若需按下标访问元素，需先收窄：
```leno
var e = _widgets[i]
if e is Widget { e.process(ev) }    // 修复前：索引取出为 any，必须 is 收窄后才能调用
```

存入数组时显式 `as`：
```leno
_widgets.add(b as Widget)           // b 为 struct Button，需 as 转 face
```

## 建议修复方向

1. 泛型实参为 face 时，下标访问 `arr[i]` 也应保留元素的 face 类型（而非退化为 `any`），
   使 `arr[i].method()` 可用，与 `for-to` 迭代变量行为一致。
2. 统一 `as` 表达式与变量绑定两种路径的结果类型推断，使 `(x as F).method()` 与
   `var f = x as F; f.method()` 行为一致。

## 状态
- [x] 已修复（commit 084ce287 后续，2026-07-09）
- [x] workaround 已验证可用（三控件例子 `test_button`/`test_edit`/`test_label` 全部 EXIT=0）

## 修复说明

**根因**：`visit_expr.inc` 中 `AST_CALL(AST_INDEX)` 和 `AST_FIELD_ACCESS` 路径对 `TYPE_FACE` 类型的方法调用没有处理，`native_get_type_name(TYPE_FACE)` 返回 NULL，直接报 "不能在 any 类型上调用方法"。

**修复**：在 `visit_expr.inc` 的两个路径中增加 `TYPE_FACE` 分支，查找 `face_def_find()` 和模块符号表验证 face 方法存在性，与 `visit_module.inc` 中 `AST_MODULE_CALL` 的处理逻辑一致。

## 修复验证（2026-07-09 重建 `build/leno.exe` 0:18:19 后实测）

复现文件 `examples/测试/repro_array_face_same.leno` 与 `repro_array_face_cross.leno` 均 EXIT=0：

```
$ leno examples/测试/repro_array_face_same.leno
same arr[0].f() = 7
same for-to w.f() = 7
SAME_MODULE_OK
EXIT=0

$ leno examples/测试/repro_array_face_cross.leno
cross arr[0].f() = 7
cross for-to w.f() = 7
bare as = 7
CROSS_MODULE_OK
EXIT=0
```

原复现的两个问题均已消除：
1. `arr[0].f()` 索引访问不再退化为 `any`，正确输出 `7`。
2. `(a0 as F).f()` `as` 裸表达式已正确收窄，输出 `7`。
（`for-to` 遍历变量本就正常，初版误报已更正。）



## 附注（非 bug，仅记录）
- 泛型类型实参中不能直接写模块限定名 `Array[a.F]`，需先 `use a.F` 再写 `Array[F]`。
  此为语法限制，有明确 workaround，不列为 bug。
- 跨模块 `new` 需 `use b.S` 后写 `S s = new S()`，不能写 `b.S s = new b.S()`（语法限制）。
- 下标访问退化为 `any`、`as` 裸表达式不收窄均为复现文件编译实测确认，非对 Leno 特性的误解。
