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

环境：关 vsync、无帧限速、8 秒；探针见 §2。

> **机器标注（跨机比较必读）**：本文数据产生于两台机器 —— **i5-14400F**（Raptor Lake）
> 与 **i5-3450**（Ivy Bridge），单核性能差约 2 倍。解释器是逐指令分派，对 IPC / 主频
> 比 JIT 代码更敏感，慢机上退化更多，因此**绝对时间不可跨机比较，只能比同机比值**
> （`JIT / VM`）；JIT 的相对收益在慢机上反而更大。§7.1 末尾附 i5-3450 复测值。

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
* 2026-09-12 在 `2e5fdebb`（i5-3450）重新构建复跑：263 passed / 0 failed；
  `ripple_image.leno` `Bailouts: 0`、~50 FPS；`性能测试/for性能测试.leno` 与
  `While vs For 性能对比.leno` 均 `Bailouts: 0`；通用 opcode 差分探针逐位一致（见 §7.1）

---

## 7. 遗留问题 / 待办

### 7.1 通用 `OP_MUL`（以及 `OP_MOD` / `OP_EQ`/`OP_NEQ`）不被 JIT 支持 —— ✅ 已修复（2026-09-12）

**修复前**：`jit_scan.c` 未收录这些通用 opcode → `scan FAIL: unknown opcode N` → **整个循环**
不被编译（不是 bailout，排查时容易看错方向）。

**修复**（三处必须同步，漏一处就白改）：

1. `jit_scan.c`：`opcode_size()` 的 1-byte 段补 4 个 opcode；
   `scan_loop_body()` 与 `scan_callee_for_inline()` 的 vstack switch 都登记为「pop 2 push 1 → -1」
2. `ops_arith.inc`：新增通用 `OP_MUL`（int 快路径 → float 慢路径 → NaN-boxed bailout）
   与通用 `OP_MOD`（**仅 int 快路径**，其余 bailout）
3. `ops_icmp.inc`：新增通用 `OP_EQ` / `OP_NEQ`（int 快路径 → float 慢路径 → NaN-boxed bailout）

**语义对齐要点**（严格照解释器的分派顺序写）：

- `OP_MUL`：int×int → `imul` + `EMIT_INT64_OVF_CHECK` + `EMIT_INT48_CHECK`（两个检查缺一不可：
  前者捕 int64 溢出，后者捕「不溢出 int64 但超 48 位」）；任一是 float → `MULSD`；NaN-boxed → bailout
- `OP_MOD`：解释器**没有 float 路径**（float 落到「取模操作数必须是整数」类型错误），
  所以 JIT 只能做 int48 快路径，**绝不能照抄 OP_SUB 的 float 中间段**。另外必须自行挡除数为 0
  ——`idiv` 除零会触发 `#DE` 硬件异常，挡下后 bailout 交解释器抛「取模除零错误」。
  余数必然落在 int48 内（`|a%b| < |b|`），无需 INT48 检查
- `OP_EQ`/`OP_NEQ`：int48 对 int48 → `cmp` + `JE`/`JNE`；两边非 NaN-boxed → `UCOMISD`，
  并按 IEEE 修正无序（`==` 结果再 `AND SETNP`、`!=` 结果再 `OR SETP`）；NaN-boxed
  （字符串按内容、数组逐元素、其它对象按指针、BigInt）一律 bailout 交解释器

**实测**（临时探针，JIT 与 `LENO_NO_JIT=1` 输出**逐位一致**）：

| 场景 | 结果 |
| --- | --- |
| 通用 MUL：float×float / int×float / int×int | 走 JIT，0 bailout |
| 通用 MOD：int % int | 走 JIT，0 bailout |
| 通用 EQ/NEQ：int、float 混比、NaN、inf | 走 JIT **原生比较路径**（`UCOMISD` + `SETNP`/`SETP`），0 bailout；`nan==nan` false、`nan!=nan` true、`nan==1.0` false、`inf==inf` true |
| 通用 MUL：int64 溢出 | 按设计 bailout → 解释器升 BigInt，结果正确 |
| 字符串 `==` / `!=` | 按设计 bailout → 解释器，结果正确（**探针里 bailout 的唯一来源**：删掉这两行后 `Bailouts: 0`） |

> 探针手法：用「声明返回 `any` 的函数」或模块调用结果当操作数，让**编译期类型未知**
> （→ 发通用 opcode）而**运行时类型确定**（→ 分别命中整数快路径 / float 慢路径 / bailout）。
> `any` 参与运算后结果也是 `any`，赋值处需 `_int()/_float()` 收敛，否则类型检查会报错。
> 运行时 NaN / inf 的构造：`_float("nan")` / `_float("inf")`（`0.0/0.0`、`maths.sqrt(-1.0)`
> 等在语言层会直接抛错，不能用来造 NaN）。

**复测（2026-09-12，i5-3450，`2e5fdebb`；差分探针 `dyn(any v): any { return v }`）**：

* 输出 JIT 与 `LENO_NO_JIT=1` **逐位一致**：`fsum=900.0 isum=1200 eqn=22020022200`
* `Bailouts: 3`，全部指向同一条：`main loop_bc=63 非溢出类 @bc_off=510` —— 即字符串
  `==`/`!=`。删掉字符串比较后（**保留 NaN / inf 比较**）`Bailouts: 0` → 证实 NaN / inf
  比较走 JIT 原生路径、并未回退，字符串比较的 bailout 是设计内行为而非回归
* 顺带覆盖：`-17 % 5` 负数取模、`2.0 == 2` 的 int/float 混比、`±int` 乘法，均与解释器一致

**副产品：7.2 的 A 写法循环现在能进 JIT 了**（本轮最大收益）：

```
修复前: [JIT-DEBUG] scan FAIL: unknown opcode 23 (size<0) at offset 16
        [JIT-DEBUG] COMPILE: fn='main' bc_off=34 ... capable=0
修复后: [JIT-DEBUG] COMPILE: fn='main' bc_off=34 ... capable=1
        Executed: 1   Bailouts: 0
```

`s = s + maths.abs(gacc) * 1.0000001` 跑 100 万次：解释 38ms → JIT **17ms（2.26x）**
（i5-14400F）。i5-3450 复测：解释 **64.7–67.7ms** → JIT **20.4–21.6ms（约 3.2x）**
—— 与 §5 的机器标注一致：慢机上解释器退化更多（38→65ms），JIT 本身只慢约 20%
（17→21ms），所以**JIT 的相对收益在慢机上反而更高**。

### 7.2 编译器侧：`AST_MODULE_CALL` 没有写回 `cached_type` —— ✅ A′ 已实现（2026-09-12）

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

**修复现状**：7.1 已落地 —— 通用 opcode 现在 JIT 能编、能跑，所以这里的后果从
「**整个循环进不了 JIT**」降级为「解释器里多一层类型分派 + 多发一条字节码」。
上例 A 写法的循环已实测 `capable=1`（见 7.1）。

**A′ 实现（2026-09-12，`src/semantic/visitinc/visit_module.inc`）**：在
`case AST_MODULE_CALL:` 的末尾（`break` 前）统一写回：

```c
if (ast->kind == AST_MODULE_CALL && ast->cached_type == NULL) {
    TypeInfo* mc_type = infer_expr_type(s, ast);
    if (mc_type) {
        if (mc_type->kind != TYPE_ANY) ast->cached_type = type_copy(mc_type);
        type_free(mc_type);
    }
}
```

为什么放语义访问、而不是 `semantic_type.c` 里那若干条 `return` 上：模块调用节点在语义阶段
必经此处，且语义阶段早于 `optimize_constant_fold` / `optimize_dead_code_elimination` / `codegen`；
`optimize.c` 对 `AST_MODULE_CALL` 只递归参数、**不会清它的 `cached_type`**（清缓存的只有
`if/while` 被分支原地替换那几处），所以 codegen 一定读得到。三条守卫各有用途：

* `ast->kind == AST_MODULE_CALL` —— 实例方法在这条路径上已被改写成 `AST_CALL`，跳过；
* `cached_type == NULL` —— clib 路径已写入 `TYPE_CLIB` 哨兵（供 codegen 识别），不能覆盖；
* `kind != TYPE_ANY` —— 返回类型运行期才确定的方法保持走通用路径，语义不变。

**效果**（同一源码 `--debug` 逐指令比，**全程序只差一条**）：

```leno
// 修复前
0040 OP_MODULE_CALL 4 5 1
0050 OP_MUL                 ← 通用乘法
// 修复后
0050 OP_MUL_FLOAT          ← 特化乘法
```

`s = s + maths.abs(gacc) * 1.0000001` 跑 100 万次（i5-3450）：
**20.0–23.8ms → 18.0–18.6ms**（约 +10~15%），对解释器（61.7–67.7ms）约 **3.5x**。

**验证**：`assert/run_tests.leno` 263/263（JIT 与 `LENO_NO_JIT=1` 两种模式）；
`ripple_image.leno` `Bailouts: 0`；差分探针覆盖 `maths.abs/sqrt/floor/ceil/round/pow`、
`strings.len/to_upper/has/trim`、`strings.split`→`Array[string]`、`times.datetime`→`Array[int]`，
JIT 与解释器输出**逐位一致**；`examples/` 下 73 个非 GUI 示例两模式 stdout 全一致
（`测试 times 方法.leno` 因打印时间戳天然不同；`深拷贝功能.leno` 是 `Start-Process`
重定向未排空的测量假象，严格 `WaitForExit()` 后一致）。

> A′ 只解决「已知类型没传给 codegen」。**跨语句的 `OP_CAST_FLOAT` 仍然存在**
> （见下条收益修正），所以 A 写法与 B 写法的 opcode 差异缩小到「同一表达式内的一层分派」。

两条修法：

* **B′（JIT 侧）**：见 7.1，纯兜底、不改语义 —— **已完成**。
* **A′（编译器侧）**：✅ **已实现（2026-09-12，见上）**。原方案是「让 `AST_MODULE_CALL`
  各 return 路径写回 `cached_type`」
  （或让 codegen 按需查 `native_get_module_method_return_type`）→ 直接发特化指令。

  收益需修正：**A′ 并不会省掉 `OP_CAST_FLOAT`**。`assign_cast_needed()`
  （`src/codegen/codegen_stmt.c:93-104`）只对字面量（`AST_NUM/STRING/BOOL/NULL`）消除 CAST，
  其余一律保留 1，所以跨语句的规范化照旧。A′ 的收益仅限于**同一表达式内的 opcode 特化**
  （少一层通用分派），比原估的小。

  风险描述也需修正三处：

  1. **失败形态是静默偏差，不是崩溃**。特化浮点指令内部走 `val_as_num_ex()`
     （`src/vm/vminc/op_type_specialized.inc:159-178`）：int/float/bigint 正常转换，
     **其它类型一律按 0.0**。而解释器通用 `OP_MUL` 对 null 是
     `check_null_binary` → 抛「乘法运算: null 不能参与运算」。即 A′ 会把「报错」变成
     「静默算 0.0」，比崩溃更隐蔽。
  2. **信任级别弱于「用户函数返回值」**。用户函数的返回类型是语义分析**校验过**的
     （声明 `: float` 却 `return null` 会编译报错）；native 元信息是 C 里手写注册、
     **无人校验**。真实形式反例：`maths.sqrt/asin/acos/log/log2/rsqrt/fmod` 都注册
     `TYPE_FLOAT`，非法输入时 `native_throw_error(...) + return val_null()`
     （`src/module/maths/maths.c:33-41` 等）。**缓解因素**：它们都伴随 native 抛错，
     实际影响被掩盖；且 maths 模块所有成功路径都是 `val_float(...)`，目前不存在
     「不报错却返回非 float」的实例 —— 风险属于「未来新增 native 可能违反契约」。
  3. **这份「信任元信息」JIT 侧其实已经在用了**。本轮的通用数值薄调用就是按
     `ModuleMethodMeta.param_types/return_type` 决定走 xmm0..N 的 double ABI，返回也按
     `val_as_num(result)` 取 double，仅用 `vm->has_exception` 兜底
     （`src/jit/jit_callout.c:1094-1104`）。所以 A′ 不是新建信任边界，而是把同一份信任
     从「JIT 薄调用」扩展到「解释器 + 特化 opcode」。

  若将来要动 A′，建议加一道门槛：仅当 `meta->return_type` 明确（非 `TYPE_ANY`）**且**该
  调用点的参数类型已通过 `param_types` 校验时才写回 `cached_type`。

### 7.3 `ffi.read_int` / `ffi.write_int` 仍是 2 次 callout

每像素约 40ns。可内联为一条未对齐 `mov`（需一并内联 `ObjFFIPointer` 的类型/空指针/边界检查），
或再做一套「整数签名」的薄调用。

### 7.4 其它

* `cache_hash()` 只用地址的 8 位（`>>4 & 255`），热循环多时会出现槽位冲突 → 反复重编译
  （调试运行中曾观察到 305 次编译）；建议改为线性探测/开放寻址。
* 函数级 JIT 已覆盖部分热点，但 `maths` 这类「叶子函数」的极致优化仍受调用延迟限制。

### 7.5 A′ 验证中发现的两个既有 JIT 缺口（与 A′ 无关，已用回退基线对比确认）

1. **`OP_CAST_STRING` 未收录进 `jit_scan.c`** → 循环体里只要出现「赋值/声明为 `string`」
   （如 `ssum = strings.to_upper("ab")`：赋的若不是字面量，`assign_cast_needed()` 会补 CAST），
   就报 `scan FAIL: unknown opcode 37 (OP_CAST_STRING) at offset 144`，**整个循环 `capable=0`**。
   回退 A′ 的基线与 A′ 后报错位置完全一致（`offset 144` 是相对循环体起点，绝对偏移 183），
   确认是既有缺口而非 A′ 引入。修法：`opcode_size()` 记 1 字节、vstack 不变；backend 侧
   「tag 是 string → 原样；否则 bailout 交解释器」即可覆盖绝大多数情形。
2. **热循环里 `_int(<返回 float 的模块调用>)` 每次进入 JIT 都 bailout**：
   `OP_CALL_NATIVE` 的 `_int/_float` 内联快路径（`ops_callout.inc`）拿到的操作数是
   `TRUE_VAL`（`0xFFFA000000000000` = `QNAN|SIGN_BIT|TAG_TRUE`），而不是
   `maths.round(2.6)` 的裸 double → `cmp rax, 0xFFF8…; jae bailout` 命中：
   `[JIT-DEBUG] BAILOUT(nonovf) bc_off=139 RAX=0xfffa000000000000`。
   基线（回退 A′）是同样的现象、同一个 `bc_off`、同一个 `RAX`，故与 A′ 无关。
   **嫌疑**：float 返回的模块调用走薄桥时结果在 `xmm0`，若没被搬回 RAX 就写入 vstack，
   就会留下上一个调用（`strings.has(...)` → `true`）的残值 —— 需要单独查证。

---

## 8. 涉及文件

本轮（A′ 编译期类型落地，2026-09-12）：

* `src/semantic/visitinc/visit_module.inc`：`case AST_MODULE_CALL` 末尾把推断出的返回类型
  写回 `cached_type`（仅 `AST_MODULE_CALL` 且未缓存且非 `TYPE_ANY` 时）

本轮（通用 opcode 兜底，2026-09-12）：

* `src/jit/jit_scan.c`：`opcode_size()` 补 `OP_MUL/OP_MOD/OP_EQ/OP_NEQ`；
  `scan_loop_body()` 与 `scan_callee_for_inline()` 的 vstack 登记
* `src/jit/backend/x86_inc/ops_arith.inc`：通用 `OP_MUL` / `OP_MOD` 的 codegen
* `src/jit/backend/x86_inc/ops_icmp.inc`：通用 `OP_EQ` / `OP_NEQ` 的 codegen

上一轮（模块调用优化）：

* `src/jit/jit_callout.c`：`jit_resolve_module_method`、`jit_callout_module_call_meta`、
  `jit_thin_f1/2/3` + `jit_thin_bridge_for`（通用数值薄桥）
* `src/jit/jit_priv.h`：上述声明
* `src/jit/backend/x86_inc/ops_return.inc`：`OP_MODULE_CALL` 的薄调用快速路径
* `src/jit/backend/x86_inc/ops_callout.inc`：`OP_CALL_NATIVE` 的 `_int`/`_float` 内联

更早（bailout 修复）：

* `src/jit/backend/x86_64.c`（序言 step 分流 + bailout 诊断埋点）
* `src/jit/backend/x86_inc/ops_loop.inc`（`OP_FOR_LOOP` / `OP_FOR_PREP` 负步长）
* `src/jit/backend/x86_inc/ops_fcmp.inc`、`ops_icmp.inc`（浮点比较 NaN 的 IEEE 语义）
* `src/jit/jit.c`、`src/jit/jit.h`（bailout 定位信息 + 统计输出）
