# JIT 模块调用优化与 bailout 排查记录（2026-09-11）

本文记录：`ripple_image.leno` 残留 bailout 的排查结论、JIT 模块调用（callout）的三项优化、
度量方法、踩到的 ABI 陷阱、性能数据与遗留问题。

相关文档：`docs/JIT实现与调试记录.md`（JIT 结构、寄存器约定、bailout site 编码、踩坑清单）。

---

## 1. 起因：ripple_image 的残留 bailout

现象（未修复前）：

```
=== JIT Statistics ===
  Compiled: 15   Executed: 3279   Bailouts: 3
  Bailout: fn='main' loop_bc=2158 x3 — 非溢出类 @bc_off=159
```

定位步骤（诊断能力是本轮新加的，见 §3.1）：

1. 统计行给出「函数名 + 循环体起始 bc_off + 触发指令的循环内偏移 + 可读原因」；
2. 用 `lenojit --debug-out <file> -c <src.leno>` 导出字节码（不运行），按偏移反查源码；
3. `loop_bc=2158` → 源码 `ripple_image.leno:331` 的 `while ev.poll()`；
   循环体内偏移 159（绝对 2317）→ `OP_GE_FLOAT`，即 `if bx >= 0.0 ...`（第 337 行）。

根因有两个，均已修复（上一个提交 `701692d4`）：

| 根因 | 说明 | 修法 |
| --- | --- | --- |
| 倒序 for 循环 | JIT 序言一句 `test step; jle → bailout` 把 `step < 0` 的循环整段踢回解释器（示例里 `for rcount-1 : 0 : -1 to i` 每次进入都 bailout） | 序言/`OP_FOR_LOOP` 回边/`OP_FOR_PREP` 三处按 step 符号分流；`step == 0`（site -2）与 float step（site -3）改为显式 bailout |
| 浮点比较遇 NaN | JIT 对无序（NaN）比较一律 `jp → bailout`；鼠标坐标经 `screenToBuf()` 算出 NaN（SDL 无有效鼠标位置时）即可触发 | 按 VM 的 IEEE 语义实现：`>`/`>=` 用 `JA`/`JAE`（无序天然为 0），`==`/`<`/`<=` 再 `AND 非 PF` 屏蔽无序 |

---

## 2. 度量方法（可复现，必须关 vsync）

`ripple_image.leno` 自带 `setVSync(1)` + `delay(16-elapsed)`，帧率被钉在 ~50-60 FPS，
**JIT 的真实差距会被完全掩盖**（实测关掉 vsync 后同一程序 26 FPS → 220+ FPS）。

分阶段探针（与示例同逻辑的临时副本）：

* 关 vsync、去掉限速；
* 用 `SDL3.getTicksNS()` 分段累计：事件+波纹更新 / 波纹列表更新 / `renderRipple`（再细分 memcpy 与像素循环）/ 绘制+上传+present；
* 统计内层循环迭代次数、写入像素数 → 得到 ns/迭代、ns/像素（跨帧稳定，不受波纹数量波动影响）。

**自动随机点击**（不依赖人工操作，压住「点击分支 + 满 6 波纹」这条最重路径）：
SDL3 的 Leno 绑定没有 `pushEvent`，但可以用 `ffi` 直接调：

```leno
import "../../lib/sdl_core.leno" as sdlcore   // 提供 lib() 句柄
...
ffi.memset(buf, 0, 128)
ffi.write_int(buf, 0, 0x401)     // SDL_EVENT_MOUSE_BUTTON_DOWN
ffi.write_int(buf, 16, 0)        // windowID
ffi.write_byte(buf, 24, 1)       // button = LEFT
ffi.write_byte(buf, 25, 1)       // down = true
ffi.write_byte(buf, 26, 1)       // clicks = 1
ffi.write_float(buf, 28, x)      // x（与 SDL_MouseMotionEvent 同偏移）
ffi.write_float(buf, 32, y)      // y
ffi.call_int(sdlcore.lib(), "SDL_PushEvent", buf)
```

每 120ms 推一次随机位置点击；**每 4 次改用 NaN 坐标**，专门覆盖 §1 里那个 `OP_GE_FLOAT`
NaN 路径（修复后 16 次 NaN 点击全部被正确忽略、`Bailouts: 0`）。

微基准（每项 2000 万次迭代，JIT 下 ns/次）用于定位单次调用成本。

---

## 3. 本轮三项优化

### 3.1 JIT 统计新增 bailout 定位行

`jit.h` 的 `JitCacheEntry` 增加 `last_bailout_site / last_bailout_bc_off / last_bailout_fn`，
`jit_try_hot_loop()` 在 bailout 分支记录，`jit_print_stats()` 输出可读原因
（`jit_bailout_reason()` 翻译 site 编码）。**无需 LENO_JIT_DEBUG** 即可定位残留 bailout。

### 3.2 模块方法解析提前到编译期（消掉每次调用的字符串查表）

`jit_callout_module_call()` 原来每次调用都做
`native_find_module_method(module_name, method_name)` —— 内部是「字符串哈希 + strcmp」
（解释器侧有 `vm.ic_module_cache` inline cache 规避，**JIT 侧没有**）。

改为：codegen 期调用 `jit_resolve_module_method(chunk, module_idx, method_idx)` 解析一次，
把 `ModuleMethodMeta*` 直接嵌进机器码，运行期走新的 `jit_callout_module_call_meta()`
（无索引/名字检查、无查表）。解析失败仍退回原 callout，保留原有报错语义。
模块方法表启动注册后不再变更（`native_reset_registry()` 未被调用），指针长期有效。

### 3.3 `_int` / `_float` 降级为内联转换

两者是语言内建转换原语（`types.c` 注册，语义 = `(int)` 截断 / 提升 double，
编译器的类型推导也把它们当 cast）。`OP_CALL_NATIVE` 的 codegen 现在识别这两个名字并直接生成：

* `_int`：int48 → 原样；裸 double → `CVTTSD2SI eax` + `movsxd`（与 C 的 `(int)` 一致，含越界/NaN 的 x86 行为）；NaN-boxed（bool/null/string/bigint/ptr）→ bailout 交回解释器
* `_float`：int48 → `CVTSI2SD`；裸 double → 原样；NaN-boxed → bailout

### 3.4 通用「数值薄调用」——**不含任何模块名/方法名特判**

判据完全来自模块方法自身注册的元信息：

```
arg_count ∈ [1,3]  且  param_types[0..n-1] 全为 TYPE_FLOAT  且  return_type == TYPE_FLOAT
```

则把实参按 `raw → double` 送进 `xmm0..xmm(N-1)`，直接调用通用桥 `jit_thin_f1/2/3`；
桥内部仍调用**模块原本的 NativeFn**，因此**数学实现只有模块里那一份**，JIT 不复制公式。
maths 里所有 `double → double` 函数（abs/clamp/sqrt/rsqrt/pow/floor/ceil/round/sin_fast/cos_fast…）
一次性都走这条路，**模块零改动**。

> 为什么不逐个内联 `maths.abs` / `maths.clamp`（虽然更快）：
> 那样必须按 `module_name/method_name` 在 JIT 里特判，每加一个函数就要改一次 JIT，
> 且 JIT 里那份 SSE 实现会与 maths.c 漂移。薄调用把「谁来算」留在 C，
> 把「怎么传参」变成一套通用代码。若日后个别函数需要极致性能（去掉调用延迟），
> 可在同一机制上加一个可选的 `jit_thin` 直连入口，而不是散落特判。

---

## 4. 关键陷阱（务必记住）

### 4.1 Win64 浮点参数按「位置」分配寄存器

Win64 前四个参数按**位置**分配：位置 1→`RCX`/`XMM0`，位置 2→`RDX`/`XMM1`，
位置 3→`R8`/`XMM2`，位置 4→`R9`/`XMM3`。
即 `f(NativeFn fn, double a)` 里 `a` 在 **XMM1**（不是 XMM0）——第一版把
「fn 放第 1 位、double 放 XMM0」，桥读到的 `a` 是垃圾值（结果全错，但不崩溃）。

因此薄桥签名定为 **double 参数在前、NativeFn 最后**：

```c
double jit_thin_f1(double a, NativeFn fn);
double jit_thin_f2(double a, double b, NativeFn fn);
double jit_thin_f3(double a, double b, double c, NativeFn fn);
```

这样 double 参数在两个 ABI 下都落在 `xmm0..N-1`；`fn` 的寄存器按 ABI 不同：
Win64 按位置 → 第 N+1 个位置寄存器（`RDX`/`R8`/`R9`，codegen 里 `JIT_ARG2..4`），
SysV 按「第几个整型参数」→ 第 1 个整型寄存器（`RDI`，即 `JIT_ARG1`）。

### 4.2 失败信号沿用 callout 约定

桥内部若 native 抛错（`vm->has_exception`）→ 桥置 `jit_callout_failed`；
codegen 在调用后检查该标志 → bailout → 解释器重跑该循环迭代并给出正确报错。
薄桥参数为 NaN-boxed 非数值时，codegen 侧直接 bailout（保持 `get_number` 的宽松语义）。

### 4.3 调试手段

`LENO_JIT_DUMP=1` 导出 `jitdump<N>.bin`，配合
`objdump -D -b binary -m i386:x86-64 -M intel jitdump0.bin` 反汇编，
可直接核对接线（本次就是靠它确认 `movabs rcx, ...; call rcx` 与 xmm 装载序列）。

---

## 5. 性能数据

环境：同机、关 vsync、无帧限速、8 秒；探针见 §2。

### 5.1 自动点击 + 满 6 波纹（最重路径）

| 指标 | VM（`LENO_NO_JIT=1`） | JIT | 倍数 |
| --- | --- | --- | --- |
| 每次内层迭代 | 708.9 ns | **122.2 ns** | 5.8x |
| 每写 1 像素 | 1181.9 ns | **282.1 ns** | 4.2x |
| 像素循环 / 帧 | 38.4 ms | 2.4–6.6 ms | 6–16x |
| 总 FPS | 25.3 | 132–310 | 5–12x |

### 5.2 纵向对比（无点击、2 波纹的探针）

| 阶段 | 优化前 JIT | 本轮后 JIT | VM |
| --- | --- | --- | --- |
| 像素循环 / 帧 | 3.47 ms | **0.096 ms** | 36.6 ms |
| `renderRipple` / 帧 | 3.54 ms | **0.154 ms** | 36.7 ms |
| 总 FPS | 220.8 | **982** | 26.3 |

### 5.3 微基准（ns/次，2000 万次）

| 循环体 | 优化前 | 本轮后 | 说明 |
| --- | --- | --- | --- |
| 纯 float `a*c+b` | 8.5 | 8.5 | JIT 原生算术（基线） |
| `+maths.abs(x)` | ~73（注：该循环当时未被 JIT） | **20.3** | 薄调用 |
| `+_int(x)` | 31.1 | **13.6** | 内联转换 |
| `+ffi.read_int+write_int` | 109.6（同上未 JIT） | 37.0 | 仍是 2 次 callout |
| `+rsqrt+clamp+sin_fast`（串行链） | — | 73.1 | 3 次薄调用，受调用延迟限制 |

结论：内层循环已从「**callout 固定开销主导**」变为「**调用延迟串行链主导**」；
帧时间构成也变了 —— 6 波纹场景里 `绘制+上传+present` 约 0.70ms（约 25%），
继续压 JIT 收益递减。

---

## 6. 回归验证清单

* `assert/run_tests.leno`：263 passed / 0 failed
* `maths.*` 一致性：abs（float/int 实参）、clamp（float/int 参数）、sqrt、rsqrt、pow、
  floor、ceil、round、sin_fast、cos_fast —— JIT 与 `LENO_NO_JIT=1` **逐位一致**
* `ripple_image.leno`：vsync 下 ~50 FPS、`Bailouts: 0`
* 自动点击探针（含 16 次 NaN 坐标点击）：`Bailouts: 0`
* 正/倒序（±1/±2/±3）、嵌套 for、float 步长、NaN 比较：JIT 与解释器一致

---

## 7. 遗留问题 / 待办

### 7.1 通用 `OP_MUL`（以及 `OP_MOD` / `OP_EQ`/`OP_NE`）不被 JIT 支持

`jit_scan.c` 未收录这些通用 opcode → `scan FAIL: unknown opcode N` → **整个循环**不被编译
（不是 bailout，排查时容易看错方向）。建议给 JIT 补上（与现有 `OP_ADD/OP_SUB` 同构：
int 快路径 + float 慢路径 + 其余 bailout），受益面覆盖「类型确实未知」的动态代码。

### 7.2 编译器侧：`AST_MODULE_CALL` 没有写回 `cached_type`

`src/semantic/semantic_type.c` 的 `case AST_MODULE_CALL:` 多条 `return type;` 路径
**都没有** `ast->cached_type = ...`（而 `AST_NUM`/`AST_VAR`/`AST_BINOP`/`AST_CALL`/实例方法都有）。
后果：codegen 的 `get_expr_type_kind()` 读到 `TYPE_ANY` → 发**通用 opcode**。

实测对比（同一源码，只差一个中间变量）：

```leno
// A：直接参与运算
s = s + maths.abs(gacc) * 1.0000001
//   OP_MODULE_CALL 3 4 1 / OP_MUL          ← 通用乘法（opcode 23）

// B：先存到 float 局部
float t = maths.abs(gacc)
s = s + t * 1.0000001
//   OP_MODULE_CALL 3 4 1 / OP_CAST_FLOAT / OP_MUL_FLOAT   ← 特化乘法
```

即：**语义分析算出了 float，但只用于类型检查，没传给代码生成**，运行时看到的是动态类型。
`ripple_image.leno` 没中招是因为它把 `maths.rsqrt(...)` 先赋给了 `float` 局部变量。

两条修法（未做，需评估）：

* **A′（编译器侧）**：让 `AST_MODULE_CALL` 各 return 路径写回 `cached_type`（或让 codegen
  按需查 `native_get_module_method_return_type`）→ 直接发特化指令，解释器也变快；
  风险：等于信任模块声明的返回类型（与用户函数返回值同等信任级别），若某 native 声明
  `TYPE_FLOAT` 却返回 null，特化指令不再做运行时派发。
* **B′（JIT 侧）**：见 7.1，纯兜底、不改语义。

### 7.3 `ffi.read_int` / `ffi.write_int` 仍是 2 次 callout

每像素约 40ns。可内联为一条未对齐 `mov`（需一并内联 `ObjFFIPointer` 的类型/空指针/边界检查），
或再做一套「整数签名」的薄调用。

### 7.4 其它

* `cache_hash()` 只用地址的 8 位（`>>4 & 255`），热循环多时会出现槽位冲突 → 反复重编译
  （调试运行中曾观察到 305 次编译）；建议改为线性探测/开放寻址。
* 函数级 JIT 已覆盖部分热点，但 `maths` 这类「叶子函数」的极致优化仍受调用延迟限制。

---

## 8. 涉及文件

本轮（模块调用优化）：

* `src/jit/jit_callout.c`：`jit_resolve_module_method`、`jit_callout_module_call_meta`、
  `jit_thin_f1/2/3` + `jit_thin_bridge_for`（通用数值薄桥）
* `src/jit/jit_priv.h`：上述声明
* `src/jit/backend/x86_inc/ops_return.inc`：`OP_MODULE_CALL` 的薄调用快速路径
* `src/jit/backend/x86_inc/ops_callout.inc`：`OP_CALL_NATIVE` 的 `_int`/`_float` 内联

上一提交（bailout 修复）：

* `src/jit/backend/x86_64.c`（序言 step 分流 + bailout 诊断埋点）
* `src/jit/backend/x86_inc/ops_loop.inc`（`OP_FOR_LOOP` / `OP_FOR_PREP` 负步长）
* `src/jit/backend/x86_inc/ops_fcmp.inc`、`ops_icmp.inc`（浮点比较 NaN 的 IEEE 语义）
* `src/jit/jit.c`、`src/jit/jit.h`（bailout 定位信息 + 统计输出）
