# 缺陷：struct 方法的默认参数不被识别为可选参数

> 状态：**已确认（编译器缺陷，最小复现见下）**
> 影响版本：截至 2026-07-14 的 `leno` 编译器（`build/leno.exe`）
> 复现文件：`examples/repro_default_func_param.leno`
> 关联改动：`leno_module/LenoSDL3/lib/sdl_window.leno` 的 `Window.run` 想加可选第 3 参数时触发

---

## 1. 结论

**struct（类）方法上声明的默认参数完全失效**——无论参数类型是普通类型（如 `int`）还是函数别名
（如 `func():void`），调用方少传该参数都会报「参数数量不匹配 / 期望 N 实际 N-1」。

**对照：顶层函数（module 级 `func`）的同款默认参数完全正常。**

---

## 2. 最小复现

`examples/repro_default_func_param.leno`：

```leno
alias EventHandler  = func(int):bool
alias RenderCallback = func(int)
alias UpdateCallback = func():void

struct WinA {
    func run(EventHandler onEvent, RenderCallback onRender, UpdateCallback onUpdate = null) {}
}
struct WinB {
    func run(int x, int y = 0) {}          // 普通类型默认，也失效
}
alias F = func():void
func topFoo(int x, F cb = null) {}         // 顶层函数对照

main() {
    var a = new WinA()
    a.run(func(int e):bool { return true }, func(int r){})   // ❌ 期望 3 实际 2
    var b = new WinB()
    b.run(1)                                                  // ❌ 期望 2 实际 1
    topFoo(1)                                                 // ✅ 正常
}
```

复现命令：

```powershell
cd d:/CLeno/LenoC
./build/leno.exe -c "examples/repro_default_func_param.leno"
```

输出：

```
=== 发现 2 个错误 ===
[语义错误] ...repro_default_func_param.leno 第 25 行: 方法 'run' 参数数量不匹配: 期望 3, 实际 2
[语义错误] ...repro_default_func_param.leno 第 27 行: 方法 'run' 参数数量不匹配: 期望 2, 实际 1
```

把 `repro_default_func_param.leno` 里两处方法调用补成满参即可编译通过，进一步证明：
**不是默认值类型不合法，而是「方法维度」的默认参数被编译器直接忽略。**

---

## 3. 行为矩阵

| 场景 | 声明 | 调用 | 结果 |
|------|------|------|------|
| 顶层函数 + 函数别名 `= null` | `func topFoo(int x, F cb = null)` | `topFoo(1)` | ✅ 正常（可选生效） |
| 方法 + 函数别名 `= null` | `func run(EventHandler, RenderCallback, UpdateCallback = null)` | `run(e, r)` | ❌ 期望 3 实际 2 |
| 方法 + 普通 `int = 0` | `func run(int x, int y = 0)` | `run(1)` | ❌ 期望 2 实际 1 |

结论：**与方法/参数的类型无关，问题出在「方法（struct 成员函数）的默认参数」这一维度整体失效。**

---

## 4. 根因假设（待编译器侧确认）

- 编译器在校验方法调用实参数目时，直接按「形参总数」做硬性匹配，未读取/应用方法参数的
  `default` 标注；而顶层函数路径正确读取了默认值并允许缺省。
- 可能的修复点：方法调用参数校验阶段需复用顶层函数那套「跳过带默认值形参」的逻辑。

---

## 5. 影响 / 牵连

- 任何想给 struct 方法加「可选参数（靠默认值实现重载/缺省）」的写法都会编译失败。
- 本次是在 `sdl_window.leno` 的 `Window.run(EventHandler, RenderCallback, UpdateCallback = null)`
  试图加可选第 3 个 `onUpdate` 回调时触发，导致全项目 60+ 处两参 `win.run(onEvent, onRender)`
  调用全部报「至少需要 3 个参数」，整个模块无法编译。

---

## 6. 临时绕过（workaround，不依赖方法默认值）

不要用方法默认参数。改用「单独注册方法」或「重载方法」实现可选能力，例如：

```leno
// 不写 run 的第 3 默认参数，改为独立 setter：
func setOnUpdate(UpdateCallback cb) { _onUpdate = cb }

// 主循环每帧调用：
if _onUpdate != null { _onUpdate() }
```

这样所有既有两参 `win.run(...)` 调用无需改动，新能力通过 `win.setOnUpdate(...)` 显式挂载。

---

## 7. 待办

- [ ] 编译器侧修复方法默认参数识别（校验调用参数数目时纳入默认值）。
- [ ] 修复后用本文件第 2 节最小复现回归验证。
- [ ] （可选）`sdl_window.leno` 的 `run` 若改用默认参数方案需在修复后重新评估。
