# 缺陷：批量 `use` 在「转导出（re-export）」场景下失效

> 状态：**已修复并验证（2026-07-19 编译器修复，原复现文件已通过）**
> 影响版本：截至 2026-07-19 修复前的 `leno` 编译器（`build/leno.exe`）
> 复现文件：`examples/测试/模块/批量导入转导出bug/`
> 关联改动：`leno_module/LenoSDL3/lib/SDL3.leno` 尝试用批量 `use core.(...)` 替代多行
> `use core.x` 时触发 —— 下游 `examples/.../fireworks_run.leno` 的 `use SDL3.WindowFlag`
> 直接报「未定义」，用户随后回滚批量导入恢复正常。

---

## 1. 结论

批量导入语法 `use module.(A, B, C)` 在**「本模块内引用」**时正常，但在**「转导出」**场景下失效：

- 用批量 `use` 从子模块引入的符号，**只进入当前模块的本地作用域，不会进入「可导出类型表」**；
- 因此下游 `import wrapper.leno as W` 之后再 `use W.A` 会报
  「模块 'W' 中没有 struct、clib、face、enum 或 alias 类型 'A'」。
- 对照：语义等价的多行单行 `use module.A` 转导出完全正常。

**一句话**：批量 `use` 不能用于「把子模块符号汇总转导出给外部 `use`」的封装写法。

---

## 2. 最小复现

目录 `examples/测试/模块/批量导入转导出bug/` 下 5 个文件，互不依赖 SDL/FFI，可独立复现。

**`repro_batchimport_core.leno`**（被转导出的底层模块，模拟 `sdl_core.leno`）：

```leno
// 模块 core：定义待转导出的 enum 与 struct
export enum Color { RED = 1; GREEN = 2; BLUE = 3 }
export struct Point { int x; int y }
export func makePoint(int x, int y): Point {
    var p = new Point()
    p.x = x
    p.y = y
    return p
}
```

**`repro_batchimport_wrap_single.leno`**（对照组：单行 `use` 转导出）：

```leno
// 包装模块（对照组）：用「单行 use」把 core 的符号转导出
import "repro_batchimport_core.leno" as core
use core.Color
use core.Point
export func makePointSingle(int x, int y): Point { return core.makePoint(x, y) }
```

**`repro_batchimport_wrap_batch.leno`**（实验组：批量 `use` 转导出）：

```leno
// 包装模块（实验组）：用「批量 use」把 core 的符号转导出
import "repro_batchimport_core.leno" as core
use core.(Color, Point)
export func makePointBatch(int x, int y): Point { return core.makePoint(x, y) }
```

**`repro_batchimport_main_ok.leno`**（对照组主程序，应当通过）：

```leno
import "repro_batchimport_wrap_single.leno" as Single
use Single.Color
use Single.Point
main() {
    var c = Single.Color.RED
    var p = Single.makePointSingle(1, 2)
    print("single Color.RED = " + c)
    print("single point     = (" + p.x + ", " + p.y + ")")
    print("=== SINGLE OK ===")
}
```

**`repro_batchimport_main_bug.leno`**（复现主程序，应当报错）：

```leno
import "repro_batchimport_wrap_batch.leno" as Batch
use Batch.Color      // ❌ 批量 use 转导出，本应可用，实际报未定义
use Batch.Point
main() {
    var c = Batch.Color.RED
    var p = Batch.makePointBatch(3, 4)
    print("batch Color.RED = " + c)
    print("batch point     = (" + p.x + ", " + p.y + ")")
    print("=== BATCH OK ===")
}
```

---

### 复现命令

```powershell
cd d:/CLeno/LenoC

# 对照组：单行 use 转导出 —— 正常运行
.\build\leno.exe "examples/测试/模块/批量导入转导出bug/repro_batchimport_main_ok.leno"

# 复现组：批量 use 转导出 —— 编译报错
.\build\leno.exe "examples/测试/模块/批量导入转导出bug/repro_batchimport_main_bug.leno"
```

### 输出

对照组（`_ok`）正常：

```
single Color.RED = 1
single point     = (1, 2)
=== SINGLE OK ===
```

复现组（`_bug`）报错：

```
=== 发现 2 个错误 ===
[语义错误] ...\repro_batchimport_main_bug.leno 第 6 行第 1 列: use 语句错误：模块 'Batch' 中没有 struct、clib、face、enum 或 alias 类型 'Color'
[语义错误] ...\repro_batchimport_main_bug.leno 第 7 行第 1 列: use 语句错误：模块 'Batch' 中没有 struct、clib、face、enum 或 alias 类型 'Point'
```

**关键证据**：`repro_batchimport_wrap_batch.leno` 自身能编译通过（其 `makePointBatch(...): Point`
返回类型能被解析），说明批量 `use` 已把 `Color`/`Point` 放入本地作用域；但下游 `use Batch.Color`
读不到 —— 证明问题出在「转导出登记表」，而非本地作用域。

---

## 3. 行为矩阵

| 包装模块写法 | 包装模块自身编译 | 下游 `use W.X` | 说明 |
|------|------|------|------|
| `use core.A` / `use core.B`（多行单行） | ✅ | ✅ 正常 | 单行 `use` 会登记到导出表 |
| `use core.(A, B)`（批量） | ✅ | ✅ 正常（**修复后**） | 修复前 ❌ 报「未定义」，漏登记导出表 |

两组唯一差异就是 `use` 的写法，排除其他干扰，锁定为批量 `use` 的转导出缺陷。

---

## 4. 根因假设（待编译器侧确认）

- 编译器对单行 `use module.X` 的处理会「既把 `X` 加入本地作用域、又把它登记进模块的
  导出符号表（供下游 `use Wrapper.X` 读取）」两步都做。
- 对批量 `use module.(A, B, C)` 只执行了「加入本地作用域」这步，**漏掉「登记导出表」**，
  导致下游 `use` 时在该模块的导出表中找不到这些符号。
- 修复点：批量 `use` 的展开逻辑需对每个成员调用与单行 `use` 相同的「导出表登记」流程。

---

## 5. 影响 / 牵连

- `LenoSDL3` 入口 `SDL3.leno` 依赖 `use core.X`（多行单行）做类型转导出，下游
  `use SDL3.WindowFlag` / `use SDL3.Event` 等才能用。一旦改成批量 `use`，全部依赖该入口的
  example（如 `fireworks_run.leno`）编译失败。
- 任何「A 模块汇总 re-export B/C/D 模块符号、供外部 `use`」的封装写法都受影响。

---

## 6. 临时绕过（workaround）

不要对**需要转导出**的符号使用批量 `use`，继续用多行单行写法：

```leno
// 需要被下游 use 的，用单行（即使很长也不要合并）
use core.Color
use core.Point

// 仅在本模块内部用、不需要转导出的，才适合批量 use
use core.(HelperA, HelperB)
```

---

## 7. 待办

- [x] 编译器侧修复批量 `use` 的转导出登记（展开时对每个成员登记导出表）。
- [x] 修复后用本目录 `repro_batchimport_main_bug.leno` 回归：已从「报错」变为「正常运行」。
- [x] （可选）已在 `SDL3.leno` 及 `sdl_window`/`sdl_renderer`/`sdl_event`/`sdl_font` 等汇总模块
      推广批量 `use` 以精简代码，并用 `leno -c` 编译 `fireworks_run` / `test_layout` /
      `test_charts` / `hello_window` 等 example 全部通过（EXIT=0）验证转导出正常。

---

## 8. 修复验证（2026-07-19）

复现文件 `repro_batchimport_main_bug.leno` 重新运行，**已通过**：

```powershell
cd d:/CLeno/LenoC
.\build\leno.exe "examples/测试/模块/批量导入转导出bug/repro_batchimport_main_bug.leno"
```

输出：

```
batch Color.RED = 1
batch point     = (3, 4)
=== BATCH OK ===
```

原报错「模块 'Batch' 中没有 ... 类型 'Color' / 'Point'」消失，批量 `use` 转导出恢复正常。
对照组 `repro_batchimport_main_ok.leno` 仍正常，行为一致。
