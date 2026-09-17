# jit_probes —— JIT 性能探针与差分工具

用途：给 JIT 改动做**同机同轮 A/B 对照**，回答两个问题 ——「改动真的生效了吗」、
「快了多少」。这里的文件都是探针，**不参与编译器构建**；`.leno` 直接用
`build/leno.exe` 跑即可。

## 文件清单

| 文件 | 测什么 |
| --- | --- |
| `probe_method.leno` | 方法调用成本。同一套算术分别写成「循环内手工展开」/「最简方法」/「中等方法体」/「完整 hitSingle 形态」四段，用来定位调用机制（callout 装箱 + 函数级 JIT 序言）的固定开销。1000 万次。 |
| `probe_alloc.leno` | 分配成本随**累计分配量**的阶梯（微秒计时，1M/10M 混合），用来判断单次成本是否随堆增长劣化、拐点在哪。 |
| `probe_alloc2.leno` | 分配路径隔离。四个循环体算术量级一致，唯一变量是「分配什么」：`new 2 字段` / `new 6 字段` / 数组字面量 `[1.0,2.0]` / 不分配。全部直接写在循环体里，排除方法调用干扰。 |
| `diff_examples.bat` | 基线二进制 vs 新版二进制，对 `examples/{struct,func,module_export_struct,cstruct}` 的示例做 stdout + 退出码差分（约 59 个文件）。 |
| `gc_barrier_canary.leno` | 写屏障金丝雀：老年代 holder 只被「自己的字段」引用的年轻对象，屏障失效即被回收。**必须配合下面的确定性 GC 钩子**，否则不敏感（原因写在文件头的注释里）。 |
| `probe_jit_gc_safepoint.leno` | JIT 循环的 GC 安全点缺口探针（§8.36）：JIT 热循环里分配 + callout，用钩子数「实际到达的安全点」，并用值判据 + **身份判据**（`probe == n`，比读值可靠）判断 JIT 活值是否被误回收。 |
| `probe_index_slowpath.leno` | `OP_INDEX` 慢路径 bailout 回归探针（§8.33）：判据是 `Bailouts` 不增长，不是耗时。**当前基线（2026-09-15）：`Compiled: 3 / Executed: 28 / Bailouts: 0`** —— 当年的 `useDict` 残留 bailout 已由 §8.67（`OP_GET_PROPERTY` 独立形态补 `pop_bytes`，`5f82acc1`）顺带修好，见文件头注释。 |
| `probe_inline_method_call.leno` | 内联侧 `OP_GET_METHOD` 缺口探针（§8.68 / R7②）：被内联方是**struct 方法**（体内带 `for` 以绕开编译器层内联器）+ **face 动态派发**。判据：`LENO_JIT_DEBUG=1` 修复前 `inline-scan FAIL: unsupported opcode 134` / `inline=0`，修复后 `inline(method): 'compute' ...` / `inline=1`；收益用同二进制 `LENO_JIT_NOINLINE=1` 做 A/B（实测 2.27~2.40x）。 |
| `probe_inline_module_var.leno`(+`inline_modvar_helper.leno`) | 内联侧**模块变量访问**缺口探针（§8.69 / R7①）：辅助模块里模块级 `var` + 访问它的 `Acc.bump`（体内带 `for`）。判据：同模块（helper 内部 `runSame`）修复后 `inline(method): 'bump'` / `inline=1`；**跨模块（主文件 → helper 的 `Acc.bump`）必须仍 `inline-scan FAIL: 模块变量访问 …（callee 与 caller 不同模块）` / `inline=0`**，且两侧取值与其它模式逐位一致。`n` 可由命令行覆盖（默认 200；计时用 300000，A/B 用 `LENO_JIT_NOINLINE=1`，实测 ≈2~3x）。 |
| `probe_dict_set.leno` | 字典写的基线与回归锚点（§8.70 负结果 / §8.71 修复）：`d["key"]=n`（命中已有条目）、`d["obj"]="value"`（对象值）、`new` 组（每轮新键，回退路径）、`str` 组（只分配+拼接的对照）、`ikey` 组（int 键走数组部分）、bigint 值、链式累加。`n` 由命令行给出（**计时必须 ≥2000 万** —— `times.ms()` 刻度约 16ms）。**判据（§8.71）**：`new` 组 20 万轮必须 ≈ 解释器同级（修复前 2015ms，修复后 266ms；再退化说明 remembered set 又变成了 O(n) 或集合被撑爆）。 |
| `probe_eq_identity.leno` | `OP_EQ` 身份比较快路径差分探针（§8.34）：判据是 JIT 与 `LENO_NO_JIT=1` 结果逐条一致 + 哪些比较仍 bailout。 |
| `bench_yield_poll.leno` | 回边 GC 轮询的每轮成本（§8.39）：10 亿次 `arr[0]`。**必须 10 亿次** —— `times.ms()` 刻度是 ~16ms，1 亿次下成本只有 1 个刻度，分辨不出。测法：与临时关掉轮询的二进制比 best-of-N。 |
| `probe_bailout_sites.leno` | bailout 站点回归探针（§8.40）：int48 溢出（ADD 2^80）与 int64 溢出（MUL 3^60）两条桩路径。判据是「JIT 与 `LENO_NO_JIT=1` 结果逐位一致」+「`LENO_JIT_DEBUG=1` 的 site 序列与改动前相同（本例 17,17,17,14,14,14）」。 |
| `probe_cast_int_peephole.leno` | `OP_CAST_INT` 恒等省略的回归探针（§8.41）：判据是 JIT 与 `LENO_NO_JIT=1` 输出逐字一致；用 `LENO_JIT_DUMP=1` 反汇编时，白名单算术后面不应再有 CAST 归一化序列，而 bool/`and`/`or` 这些非白名单来源必须有。 |
| `probe_local_fold.leno` | 语句级折叠（`x = x ⊕ k`，§8.44）回归探针：判据是 JIT 与 `LENO_NO_JIT=1` 逐字一致。重点覆盖 **SHL 的 48 位截断语义**（第一版折叠漏了它，只用 `>>` 的基准看不出来）、ADD/SUB 溢出（检查必须保留）、以及形态 A/B 两种常量位置；`LENO_JIT_DEBUG=1` 应看到 10 行 `[JIT-CG] FOLD/FOLDB`。 |
| `probe_closure_nocap.leno` | **R5-P1** 形态 C0（零捕获闭包）：热循环体内 `func(){...}` 无捕获。判据：`LENO_JIT_CLOSURE=1` 出现 `scan:ALLOW-C0`、该循环 `Compiled` 增长且 **`Bailouts: 0`**；输出 `c0= 200000` 与 `LENO_NO_JIT=1` 一致。 |
| `probe_closure_upvalue.leno` | **R5-P2** upvalue 读写（`OP_GET/SET_UPVALUE`）：闭包体内热循环 `total = total + 1`，且**在 `make` 返回之后**才调用闭包 ⇒ 覆盖 open→closed 转换（不变量 I2：不缓存 `location`）。判据：`u1= 200000` / `u2= 400000`（同一份存储续累加）、`Bailouts: 0`。 |
| `probe_closure_value_capture.leno` | **R5-P3** 形态 C2（值捕获）：`var k = 7` 声明在**循环体内**被闭包捕获（`is_value_capture = sym->is_in_loop` ⇒ 1）。判据：`scan:ALLOW-C2`、`c2= 1400000`、`Bailouts: 0`。 |
| `probe_closure_byupvalue.leno` | **R5-P2b** 形态 C1（by-upvalue）：`base` 是外层函数的形参，循环体内那层闭包引用它 ⇒ 对它而言 `is_local=0`。判据：`scan:ALLOW-C1`、`c1= 600000`、`Bailouts: 0`。 |
| `probe_closure_ref_capture.leno` | **R5-P4 的 gate 探针**：形态 C3（引用捕获**循环外**声明的变量，`is_value_capture=0`）。**当前必须仍被拒收**（`scan:REJECT caps=1 ref_local=1 ...` + `scan REJECT: 含 OP_CLOSURE`）—— 它需要"存活期内地址不变的 `Value*`"，是唯一要改 locals 布局的重活。将来若 P4 落地，这里的判据才改为"放行"。 |
| `bench_closure.leno` | **R5 收益基准**（§8.72）：三种闭包形态各 2000 万次 —— A 循环内建零捕获闭包（C0，P1 解锁）、B 循环内建值捕获闭包（C2，P3 解锁）、C 循环内调用外部闭包且闭包体读 upvalue（P2a 解锁）。判据：JIT 与 `LENO_NO_JIT=1` 的值一致（`120000000`）且 `Bailouts: 0`。**收益基线（2026-09-15）**：A 6156 vs 8484ms（1.38x）、B 4360 vs 5125ms（1.18x）、C **281 vs 750ms（2.67x）** —— A/B 被"每轮分配一个闭包"支配，C 才是纯 JIT 收益。 |

> **闭包形态诊断开关 `LENO_JIT_CLOSURE=1`（R5，§8.72）**：只输出闭包形态行与闭包拒收行
> （`[JIT-CLOSURE] scan:ALLOW-C0 / ALLOW-C1 / ALLOW-C2`、
> `scan:REJECT ... caps=N ref_local=.. value_local=.. byup=..`），用于在真实负载上做形态盘点。
> **不要**为此开 `LENO_JIT_DEBUG=1` —— 它会把每次编译尝试的 body raw hex（最多 1200 字节）
> 整段打印，真实负载上输出量极大（观测体验是"这个命令跑不完"）。

## R5/R6 收益基准（`examples/性能测试/JIT_R5R6收益基准.leno`）

闭包（C1/C2）+ 多返回值 + 尾调用四项，JIT vs `LENO_NO_JIT=1`，`CHECK` 必须一致。
实测：1.11x / 1.06x / **17.5x** / **2.29x**（详见 §9 的「R5/R6 收益量化」）。
**注意**：这项基准的 `CHECK` 曾当场抓出尾调用快路径的一个静默错误（单返回只写
`jit_fn_result`、数组是残留）—— 量化收益时**必须**同时校验结果一致性。

## 内联侧「经函数值调用」（`probe_inline_call_value.leno`，R7④）

被调函数体内含 `var f = foo; f(x)`（`OP_GET_GLOBAL_FUNC` + `OP_CALL`）时，此前**不能内联**。

```powershell
build\leno.exe jit_probes\probe_inline_call_value.leno 500000                     # 有内联
$env:LENO_JIT_NOINLINE='1'; build\leno.exe jit_probes\probe_inline_call_value.leno 500000
# 实测 34.5 ms vs 93.9 ms = 2.72x（同二进制 A/B，不需要重建旧版）；对照项两侧 ±1%
```

**三个必须绕开的坑**（探针注释里有完整说明）：① 被调方体内要带 `for`（否则被**编译器层**
内联器吃掉，字节码里根本不存在该函数）；② 调用点必须用**顶层函数直接调用/方法调用**
（`OP_CALL_GLOBAL_FUNC_TYPED` / `INVOKE_METHOD_TYPED`），非 TYPED 形态 JIT 压根不尝试内联；
③ 要测"内联收益"就用 `LENO_JIT_NOINLINE=1` 做同二进制 A/B。
⚠ **开 `LENO_JIT_DEBUG=1` 时必须用小 n**（如 2000）：debug 输出是**按执行**打的，
大 n 会让 stderr 涨到 MB 级并把运行拖到分钟级（本探针踩过两次）。

## R6-b：尾调用 + 函数级 JIT（`probe_tail_call_jit.leno`）

`sumTo`（深尾递归）/ `scale`（末尾是尾调用的中间层）/ `loopTail`（循环体内尾调用）。

```powershell
build\leno.exe jit_probes\probe_tail_call_jit.leno 20000                        # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_tail_call_jit.leno 20000"
# 两侧逐字一致；FuncCompiled: 2 / Bailouts: 0；20000 层 × 200 轮 ~0.1s 不崩
```

**两个写探针的坑（否则等于没测）**：
1. **必须经函数值调用**（`var sumF = sumTo; sumF(n,0)`）——直接 `sumTo(n,0)` 会被编译器在
   调用点**内联展开**，函数级 JIT 根本不触发（`FuncCompiled: 0`）；
2. 函数级 JIT 有**热度阈值**——只调用一次不编译，要在循环里跑热。
详见 §8.76。

## R6-i：`OP_DICT`（字典字面量，`probe_dict_jit.leno`）

```powershell
build\leno.exe jit_probes\probe_dict_jit.leno 2000                              # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_dict_jit.leno 2000"   # 解释器
# 两侧逐字一致：dictPairs=2003001 / dictEmpty=2001000；Compiled: 2、Bailouts: 0
```

**两种形态都要测**：`count=2`（键值对）与 **`count=0`（空字典字面量 ⇒ net +1，
最容易把栈记账写成 -1 的那个 case）**；字节码转储里应同时出现 `OP_DICT 2` 与 `OP_DICT 0`。
语义细节（逆序取、正序插、失败通道）见 §8.85。

## R6-h：`OP_GET_FIELD_ADDR`（`&c.field` 取字段地址，`probe_field_addr_jit.leno`）

```powershell
build\leno.exe jit_probes\probe_field_addr_jit.leno 2000                              # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_field_addr_jit.leno 2000"   # 解释器
# 两侧逐字一致：addrNonNull=2001 / fieldAfterWrite=7；Compiled: 1、Bailouts: 0
```

**注意这个探针的第 2 条判据**：只检查"指针非 null"是**不够**的（指针算错也非 null）——
所以它用 `ffi.write_int(&c.v, 0, 7)` 后读回 `c.v == 7` 来证明"指向的确实是那个字段"。
指针构造复用 `object_cstruct.c` 的 `cstruct_field_addr_new`（语义唯一来源），详见 §8.84。

## R6-g：`OP_AS_CAST`（`as` 安全转换，`probe_as_cast_jit.leno`）

```powershell
build\leno.exe jit_probes\probe_as_cast_jit.leno 2000                              # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_as_cast_jit.leno 2000"   # 解释器
# 两侧逐字一致（asFloatToInt=1003001 / asRoundTrip=3002001 / asMismatch=2001）
# Compiled: 3、Bailouts: 0；字节码转储里循环体应有 OP_AS_CAST
```

转换语义在 `vm.c` 的 **`vm_as_cast`**（语义唯一来源，解释器与 JIT callout 共用同一份，
与 `type_check_value` 同一模式）—— 所以这个探针同时也守着重构等价性。
⚠ 附带修掉长度表的两处混用缺陷（`AS_CAST`+`CSTRUCT` 应 4 字节、`AS_CAST`+`ENUM` 应 3 字节），
细节见 §8.83。

## 已知缺口基线：`OP_INDEX` 的错误通道（`probe_index_error_channel.leno`）—— **已修复**

修复后实测（n=60）：JIT 与 NO_JIT **都是** `caught=61 ok=61`、**exit=0** ✓
（修复前 JIT 是 `caught=50` + **exit=−1** + `发现 11 个错误`）。JIT 侧 `Bailouts: 3`
（该循环 3 次后被拉黑 ⇒ 其余走解释器 ⇒ 每次错误都被捕获）。详见 §8.86。

```powershell
build\leno.exe jit_probes\probe_index_error_channel.leno 60                              # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_index_error_channel.leno 60"   # 解释器
# 判据：两侧 caught=61 / ok=61 且**退出码为 0**、stderr 里**没有**"发现 N 个错误"
```

## 取证：`OP_INDEX` 的产出到底是什么（`probe_index_callee.leno`）

```powershell
# 极小探针（一个热循环 + 两个调用点），只用来抓 call_value 的诊断行
cmd /c "set LENO_JIT_DEBUG=1&& build\leno.exe jit_probes\probe_index_callee.leno" 2>&1 | findstr call_value
# 实测只出现：callee 是对象但 function 为空（obj_type=10 = OBJ_BOUND_METHOD）
# ⇒ JIT 的 OP_INDEX 产出与解释器一致（bound method），§8.82 的"产出不可靠"推断已被推翻
```

判据：`n=101`（JIT/NO_JIT 一致）、`Compiled: 1`、`Bailouts: 3`（3 次后循环被拉黑 ⇒
只剩 3 条诊断，便于读）。**读诊断行时务必先读到函数尾部再下结论**（§8.86 的教训）。

## 浮点步长循环的净损失（§8.97）：`nlev_e_floatstep_loop.leno` / `nlev_f_intstep_loop.leno`

E = 热外层循环里包 `for 0.0 : 1.0 : 0.05 to t`（fm 的 `render` 形态）；F = 同形状但内层 **int 步长**。
E 会命中 `OP_FOR_PREP` 的"step 静态类型是 float ⇒ bail"守卫，**外层循环整块被拉黑** ✗。

```powershell
build\leno.exe jit_probes\nlev_e_floatstep_loop.leno     # JIT ≈ NO_JIT（完全没享受到）✗
build\leno.exe jit_probes\nlev_f_intstep_loop.leno       # 2.2x ✓
```

实测（3 轮）：E 174/196/187ms（NO_JIT 158ms）、F 93/95/97ms（NO_JIT 211ms）⇒ 修好浮点步长 ≈ 2x 收益。
⚠ E 与 F 的迭代数因浮点漂移并不完全相等（1800000 vs 2000000），该比值**偏保守** ✓。

**另记（§8.97 的观测修正）**：`OP_FOR_PREP` 的两个守卫（静态 float / 运行期 `step == 0`）历史上
共用同一 `bc_off` ✗ ⇒ 只能看到 `非溢出类 @bc_off=N`；现在 `step == 0` 有独立编码
（`FOR_PREP: step == 0（运行期）`），两者**可区分** ✓。

## 值域探针（§8.99 / §8.100 的教训）：`probe_type_check_float_zero.leno`

**形状探针测不出"值域依赖"的 bug** —— `probe_cstruct_jit` 那种 JIT/NOJIT 逐字比对，
在 §8.99 的 SDL `0.0` 与 §8.100 的 `is float` 上都**通过**，因为触发条件是**具体的值**
（0.0 / -0.0 / 次正规数）而不是形状。本探针把值域维度补齐：

```powershell
build\leno.exe jit_probes\probe_type_check_float_zero.leno
$env:LENO_NO_JIT="1"; build\leno.exe jit_probes\probe_type_check_float_zero.leno
```

⇒ 五个计数必须逐字一致（`zero/negzero/sub/normal=2000`、`intzero=0`）。
修前实测 `zero=50`（热阈值前 50 次解释执行判真、之后 JIT 判假）✗；修后一致 ✓（§8.100）。

⚠ 本方言**不支持科学计数法字面量**（`1.0e-324` 会报"声明语句后期望换行"）⇒
次正规数用 `0.5` 连乘 1074 次现算 ✓。

## int/float 歧义区的三个探针（§8.100 / §8.101 → §8.103 已全部修好，**并入常规门禁**）

| 探针 | 覆盖 | 修前 → 修后 |
| --- | --- | --- |
| `probe_type_check_float_zero.leno` | `is float` 对 +0.0 / -0.0 / 次正规 | `zero=50` ✗ → `zero=2000` ✓ |
| `probe_is_int_ambiguous.leno` | **反向**：值实际是 float 0.0 时的 `is int` | `hitsInt=2950` ✗ → `0` ✓ |
| `probe_float_param_faithful.leno` | 泛型值边界：`"" + p`（p 为 float 0.0）| `fmt=[0]` ✗ → `fmt=[0.0]` ✓ |
| `probe_float_param_int0.leno` | 同一缺陷的"格式计数"形态 | `hitsF=50 hitsI=2950` ✗ → `3000/0` ✓ |

**观测口径（§8.102，踩坑换来的）**：这类值域/类型污染缺陷，**不要只用 `is`** ——
`is float` 会被歧义区 bailout 掩盖、`is int` 会因同一歧义**假阳性**；连调用方 JIT 里的
`"" + v` 也会经过 concat 的同一启发式 ✗。而且**"把格式化放进 callee"也不够**：
若 callee 自己也被函数级 JIT 编（本例就是），观测点仍在启发式之后 ✗
⇒ 先确认 callee 未被 JIT 编（`LENO_JIT_DEBUG=1` 看 `COMPILE:` 行）或直接比对值本身。

## 容器字面量的类型污染（§8.105）：`probe_array_literal_float.leno`

**要害是"可达性"** —— 本探针前两版都**空转**（跑了但没测到东西 ✗），记下来避免重犯：

| 形态 | 结果 |
| --- | --- |
| 字面量放在**单独小函数**里 | 函数级 JIT 不编它（实测只编 `main`）⇒ 在解释器里建 ⇒ **空转** ✗ |
| 字面量在 JIT 循环里，但同循环内还有 `"" + a[0]` | concat 被 §8.103 守卫 ⇒ 整循环拉黑退回解释 ⇒ 结果被**掩盖成正确** ✗ |
| **JIT 区域内建 → 存进全局 → 循环外读回** | ✓ 能看见真值（修复前 `g=[0]` vs NO_JIT `g=[0.0]`）|

⇒ 一句话：**测"值被污染"必须让污染值活过 JIT 区域**，否则守卫会把症状掩盖掉 ✓。

## 值域：次正规数（**§8.110 已修，现为常规门禁**）—— 四个探针

`probe_mul_float_min.leno` / `probe_tiny_make_where.leno` / `probe_tiny_fetch_or_store.leno` / `probe_native_arg_float.leno`

**触发条件**：浮点结果的位型落进 `[0, 2^47)` ⇒ 与 int48 **撞码** ⇒ 被判成整数、数值失真
（`0x4000`→`16384`、`0x1`→`1` —— 输出恰等于位型当整数）。`+0.0` 除外（数值上无害 ⇒ 放行）。

**判读**：四个探针的数值必须与 `LENO_NO_JIT=1` **逐字一致**；JIT 侧会出现
`Bailout: fn='…' … 触发指令=OP_MUL_FLOAT`（§8.110 的结果守卫 ✓，这是**预期**行为）。

⚠ 口径要点：`0.0` 区分不了 int/float（`_str(0.0)` 与 `_str(0)` 都输出 `"0"`）；
必须用 **2^-1074**（位型 `0x1`，撞码对象是 int 1）这类值才能一眼可辨 ✓。

## （历史过程记录；**最终结论见上方 §8.110**，本段描述的是中途的排除轨迹）

> 这段记录了三步取证（native 实参 ✗ → 写回/物化 ✗ → **浮点结果守卫 ✓**）中的**排除过程** ✓；
> 其中"真因是物化/写回"的中间结论**已被推翻** ✗ —— 反汇编（§8.109）证明取操作数的 `CVTSI2SD`
> 分支同样命中，而**最终修复**是在**浮点结果处**加守卫（§8.110 ✓）。
> 保留本段是因为"哪些路径被实验排除过"对后来者有价值 ✓。

**§8.106 真因**（初版归因 native 实参是**错的** ✓）：JIT 的**物化/写回**把裸 double 按 int48 装箱。
`probe_tiny_make_where.leno` 用 `2^-n` 夹逼给出铁证 —— JIT 输出**恰好等于该 float 的位型当整数**：

| n | 位型 | 歧义区 | JIT | 正确 |
| --- | --- | --- | --- | --- |
| 1021~1023 | ≥ `0x0008…` | 否 | 正确 ✓ | ✓ |
| 1060 | `0x4000` | 是 | **16384.0** ✗ | 8.0948e-320 |
| 1074 | `0x1` | 是 | **1.0** ✗ | 4.94e-324 |

⇒ **正解**：让 `RBX` 类型位图成为"该槽当前是否非 int48"的忠实标志（store/写回按实际类型置位/清位）。

## （同源）`probe_native_arg_float.leno`

§8.106：native 实参路径（`_str` / `print`，编译成 **`OP_CALL_NATIVE`**）把**次正规浮点**贴成 int
⇒ JIT `"1"` vs NO_JIT `"4.94066e-324"` ✗（**量级**都变了，不只是类型）。

**口径要点**：这类缺陷必须用**能区分 int 与 float 的值** —— `0.0` 不行 ✗
（`_str(0.0)` 与 `_str(0)` 都输出 `"0"`）；**2^-1074**（位型 `0x1`，撞码对象是 int 1）才可以 ✓。

已在 4 处 native 实参边界加守卫（`jit_raw_block_ambiguous`），但本路径走的是 `OP_CALL_NATIVE`
的通用 callout（尚未定位）⇒ 探针仍红，作为跟踪项保留。

## 浮点转字符串与时钟单位（§8.111 的诊断双探针）

- `probe_float_str_point.leno` —— **门禁探针**：浮点在拼接里是否保留小数点 ✓
  （§8.111 曾怀疑"拼接丢小数点"，此探针否掉了它：`1224.9280279999998` ✓ JIT/解释器逐字一致）。
- `probe_times_units.leno` —— **诊断探针（非门禁）**：核对 `times.ms()/us()/ns()` 的真实单位
  （要求三者 delta 精确成 ×1000 关系：如 `0.368 / 367.9 / 367899` ✓）；顺带照抄"热循环两端取
  `times.ms()`"的形态。⚠ 它打印**绝对**时刻，故不进 IDENTICAL 门禁 ✓。

**⚠ `.lenocache` 陷阱（§8.111 症状 / §8.112 已修）**：`entry_<hash>.lenb` 是**整程序**快照，
键只含入口文件哈希；`.deps` 里原先只登记**有源码的模块**（native 模块没有 source_path ✗）
⇒ **改了 `src/module/**`（C 侧 native）不会让缓存失效** ✗。症状是"计时/类型诡异地不对，
但数值计算看着正常" ✓（实例：`times.ms()` 的注册由 `TYPE_INT` 变 `TYPE_FLOAT` ⇒ 缓存里仍是
int 签名 ✗ ⇒ `t2 - t1` 按整数相减 ⇒ 打印 10^9 量级"毫秒" ✗）。

**§8.112 起自动失效** ✓：`.deps` 第二行记入**运行中 exe 的指纹**（size+mtime+内容哈希 ✓），
任何"必须重建 exe 才能发生"的变化（native 签名/常量/实例方法/编译器语义 ✓）都会让缓存作废 ✓。
手工核对与兜底（怀疑旧 `LENODEPS1` 清单时）：

```powershell
# 看清单头两行：应为 LENODEPS2 + 一行 BIN <hex>
Get-Content .\examples\性能测试\.lenocache\entry_*.lenb.deps -TotalCount 2
# 兜底：直接删掉缓存（会重编译一次）
Remove-Item -Recurse -Force .\examples\性能测试\.lenocache
```

**回归验证步骤（本项无法写成普通 assert 用例 ✓）**：该机制的触发条件是"exe 变了" ✗，
而 `assert/run_tests.leno` 只能跑当前 exe ✗ ⇒ 手工 e2e（§8.112 / §8.112.1 记录里有完整步骤）：
① 临时把 `times.ms` 改成自洽旧形态（int 签名+返回 int）→ 重建 → 跑一次（写缓存 ✓）；
② `git checkout` 恢复 → 重建 → 再跑 ⇒ 输出必须**正常**（若不是，说明缓存没被失效 ✗）。
**三类产物都要看** ✓：入口 `entry_*.lenb`（§8.112）+ **模块** `.lenomc` / `.lenosymc`（§8.112.1）；
模块级用 `build/cachetest/{mod,main}.leno` 那种"模块自己调 `times.ms()` 再返回差值"的形态 ✓ ——
模块自身的字节码里烙着 int/float 的减法 ✓，缓存没失效就会打出 10^9 量级 ✓（一眼可辨 ✓）。

## 覆盖面合成表（§8.92）：`jit_census.ps1`

JIT 覆盖面有两个**互不相通**的口径，过去只能手工分别看 ——
**scan 期拒收**（`LENO_JIT_GAPS=1`，编译前就拒绝的对象）与
**运行期 bailout**（stats 的 `Bailout:` 行，编了但执行中失败、最终被拉黑的对象，行内含
**触发指令名**）。只看前者会漏掉"编了却每次都 bail"（R6-k 之前的 bound-method 就是 ✗），
只看后者会漏掉"根本没尝试编" ✗。

```powershell
powershell -File jit_probes\jit_census.ps1              # 默认 3 个真实应用 × 300 帧
powershell -File jit_probes\jit_census.ps1 -Frames 60   # 热循环需 ≥ JIT_HOT_THRESHOLD(50) 回边
```

输出：每个应用的 `Compiled/Executed/Bailouts/FuncCompiled` 与 `[SDL-BENCH]` 基准、
运行期 bailout 明细、scan 期拒收清单（按编译对象去重计数）。

**内联侧的记录已补齐（§8.94）**：以前"为什么没内联"完全不可见 ✗，现在每道门都有记录 ——
包括 `内联未尝试：…字节码长度超上限`、`内联未尝试：callee locals 放不下`、
`未实现内联：未类型化 OP_CALL_GLOBAL_FUNC 调用点` 等 ✓。
⚠ 看 census 时记住：**每个对象只记第一个原因**（§8.80）—— 修掉第一名后必须重测，
否则会把"换了个原因"当成"解锁" ✗（§8.95 的 256→1024 实验就是这么暴露瓶颈换位的 ✓）。

**⚠ 不要用 `LENO_JIT_DEBUG=1` 来回答"还有哪些没进 JIT"**：真实应用上它产出 8MB+ stderr
并把运行拖到跑不完（§8.71 / §8.89 各栽过一次）。本表用的两个开关都是"退出时打印一次"量级。

## B 审计（§8.91）：错误通道 —— 4 个探针

| 探针 | 压的是什么 | 判据 |
| --- | --- | --- |
| `probe_error_channel_more.leno` | 热循环里的 `10 / (i - 150)`（**静态 int** ⇒ JIT 内联 `OP_DIV_INT` + 自有零检查）| `caught=1 / acc=299 / i=300`，与 NO_JIT 一致、exit=0 |
| `probe_error_channel_div.leno` | `any` 形参做除数 ⇒ **通用 `OP_DIV` callout**（`jit_callout_div`）| `ok=0 / caught=300`，与 NO_JIT 一致、exit=0、**无「发现 N 个错误」** |
| `probe_index_set_error.leno` | 热循环里 `arr[1000] = i`（3 元素数组）⇒ `OP_INDEX_SET_NOPUSH` 慢路径的越界 | `caught=300 / arr_len=3`，与 NO_JIT 一致、exit=0 |
| `probe_index_error_channel.leno` | 字符串索引越界（§8.86 的原始用例）| `caught=61 / ok=61`，与 NO_JIT 一致、exit=0 |

```powershell
foreach ($f in @('probe_error_channel_more','probe_error_channel_div','probe_index_set_error','probe_index_error_channel')) {
  build\leno.exe jit_probes\$f.leno
  cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\$f.leno"   # 两侧输出必须逐字一致、exit=0
}
```

**这些"坏法"都真实出现过**（详见 §8.86 / §8.91）：① `error_add_at` 写全局表 ⇒ 即使 try/catch
成功，进程仍 `发现 N 个错误` + **非零退出码** ✗；② callout 置了 `failed` 但 **调用点没有守卫**
⇒ 错误被**彻底静默**（JIT 把 NULL/半成品当成功继续跑）✗ —— `OP_DIV` / `arr[idx]=v` /
`dict[k]=v` / `arr.append` / 局部 struct 字段读都各漏过一次 ✓ 已全部补上。

## R6-k 后续（§8.90）：`vm_call_value` 的 native callee 路径（`probe_vm_call_value_native.leno`）

用**原生函数值**当回调（`nums.map(print)` / `nums.filter(print)`）—— 走 `vm_call_value` 的
native 分支。判据：`after_len=3`、`total=6`（**回调之后**的热循环计数没被写坏）、
`keep_len=0`，且与 `LENO_NO_JIT=1` 逐字一致。

```powershell
build\leno.exe jit_probes\probe_vm_call_value_native.leno
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_vm_call_value_native.leno"
```

**改前的症状（这就是它抓到的东西）**：`...:24 可空值（null）不能访问属性或方法`、
`函数未定义`、`加法运算: null 不能参与运算`、`=== 发现 6 个错误 ===`
（native callee 不压帧 ⇒ 解释器循环多跑了调用方字节码 ⇒ 静默错，见 §8.90）。

## （已解决）R6-f / R6-j / R6-k：把非闭包 callee 交出去

**同一个症状、三个层次的原因**（前两次都回退，**R6-k 已修好**；详见 §8.82 / §8.87 / §8.88）：

| 症状 | 元凶 | 结论 |
| --- | --- | --- |
| `test_jit_closure_byupvalue` 静默算错（2001000→2006847） | **只在改 codegen 那次**出现 ⇒ `JIT_ARG3 = R8`（R8 是发射器 scratch / 可能承载 pinned TOS）| **加 callout 参数不要用 ARG3/R8** |
| `probe_cstruct_jit` 报 `只能调用函数（不是对象类型）` / 探针返回陈值 | **`vm_call_value` 是「脚本调用」入口**：它假定 `call_value` 压了新帧，然后进解释器循环跑到该帧返回；而 **native callee 不压帧** ⇒ 循环**继续执行调用方的字节码** | **native 类 callee（`OBJ_NATIVE` / `OBJ_BOUND_METHOD` 且 `closure==NULL`）必须原地直调**，不能走 `vm_call_value`（R6-k 的做法，复用 `jit_callout_call_native` 的机械）|

**取证法（踩坑换来的）**：①一次只改一处做隔离实验；②单点观测不足以否定假设（§8.86 只看
`probe_index_callee` 就否掉了"callee 槽不可靠"，对别的站点是错的）；③改共享机械前先证明
"数据有没有到达"（**双向仪器**：`vm.c` 发布处 + JIT 取值处各打一行）。

**回归基线**：三个探针 JIT/NOJIT `IDENTICAL` 且 `Bailouts: 0`、
`probe_index_callee` 的 `n=101`、闭包用例 OK。

## R6-e：`OP_CLIB_CALL`（FFI 动态库调用，`probe_clib_call_jit.leno`）

```powershell
build\leno.exe jit_probes\probe_clib_call_jit.leno 5000                              # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_clib_call_jit.leno 5000"   # 解释器
# 两侧逐字一致（ffSleep=5001 / ffAbs=8502500）、Compiled: 2、**Bailouts: 0**
build\leno.exe --debug-out build\dump_clib.txt jit_probes\probe_clib_call_jit.leno 10
#   → 循环体里应有 OP_CLIB_CALL args=3 ret_kind=.. user_args=1 types=[..]
```

**为什么这个探针是"干净"的**：形态刻意选成 ① `Sleep(0)`（void，无需类型转换）
② `abs(i-1000)` + `_int()` 累加（原生桥已支持）—— 避开 `OP_AS_CAST` 与 `call_value`
两个相邻缺口，所以 `Bailouts: 0`。**真实应用里做不到这么干净**：R6-e 之后 fm/cc 的
`Bailouts` 3/0 → 6/3，全部来自 `call_value`（`T.native方法()`），见 §8.81。

## R6-d：`OP_GET_CSTRUCT_DEF`（cstruct 类型名取定义，`probe_cstruct_jit.leno`）

```powershell
build\leno.exe jit_probes\probe_cstruct_jit.leno 20000                             # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_cstruct_jit.leno 20000"  # 解释器
# 两侧逐字一致（defLoop=20001 / loopDef=20001）、统计 Compiled: 2
# 二次确认"这条指令真在体里"（**必做**，否则可能整个探针不含目标指令）：
build\leno.exe --debug-out build\dump_cs.txt jit_probes\probe_cstruct_jit.leno 10
#   → 应出现 OP_GET_CSTRUCT_DEF ... (Cell) / (Vec2) 落在循环体的 bc_off 上
# 三次确认"失败来自谁"：
cmd /c "set LENO_JIT_DEBUG=1&& build\leno.exe jit_probes\probe_cstruct_jit.leno 200" 2>&1 | findstr CALLOUT-FAIL
#   → 本探针实测全是 call_value（def 的消费方，独立缺口），get_cstruct_def 零失败
```

**⚠ 两条"看着像失败、其实不是"的读数**（详见 §8.80）：
- `Bailouts: 6` 全部来自 `call_value: callee 是对象但 function 为空（obj_type=10）`
  —— `T.malloc()/T.size()/T.offset_of()` 这类**类型名上的方法调用**（native 方法对象）
  在 JIT 里每次执行都 bailout。**与 R6-d 无关**，但也说明 cstruct 的"取定义"通了、"用"还差一步。
- `c.v as int` 会引入 `OP_AS_CAST(94)` ⇒ 整循环被 scan 拒（`loop|unsupported opcode 94`）。

## 使用约定：census 每个对象只记「第一个」遮断原因

`scan` 遇到不认识的 opcode 就 `return`（长度未知无法继续走）⇒ **修掉一个原因后必须
重测 + 做逐对象集合差**，否则会把"换了个遮断原因"当成"解锁"。R6-d 正是靠差分才发现
**循环级净解锁 = 0**（原来被 cstruct 挡的循环转为被 `OP_CLIB_CALL` 挡）。差分方法：

```powershell
# 改前/改后各跑一次 LENO_JIT_GAPS=1，把 '^\s+\d+\s+(loop|func|inline)\|' 行收成集合再相减
# （计数是集合元素的一部分 ⇒ 计数变化会同时出现在"仅前有/仅后有"，要看的是**原因**的出现与消失）
```

## R6-a：多返回值 + 函数级 JIT（`probe_multi_ret_jit.leno`）

覆盖 2 值（int）/ 2 值（float）/ 3 值 / **多返回值嵌套**（被调函数内部再调多返回值函数）。
判据：

```powershell
build\leno.exe jit_probes\probe_multi_ret_jit.leno 20000                       # JIT
cmd /c "set LENO_NO_JIT=1&& build\leno.exe jit_probes\probe_multi_ret_jit.leno 20000"
# 两侧逐字一致；统计里 FuncCompiled: 4 / Bailouts: 0
# 另可 LENO_JIT_FTRACE=1 看快路径是否真在跑（应出现 3903 次 <<FT> ... done jr=0 failed=0>>、零回退）
```

**注意**：`FuncCompiled > 0` 不足以证明快路径被执行 —— 必须用 `LENO_JIT_FTRACE=1`
看到 `jr=0 failed=0`，否则可能只是"两边都退回了解释器"。详见 §8.75。

## 拒收原因直方图（`LENO_JIT_GAPS=1`，2026-09-16）

盘点"剩余缺口"用这个开关，**不要**用 `LENO_JIT_DEBUG=1`（后者在真实应用上 stderr 8MB+、
跑不完）：

```powershell
$env:SDL_VIDEODRIVER='dummy'; $env:LENO_SDL_FRAMES='300'; $env:LENO_JIT_GAPS='1'
build\leno.exe <app.leno>     # 退出时打印「模式|原因（含 opcode 名）」的计数表
```

实测开销：file_manager 300 帧 **2.5s / stderr 1.3KB**。
计数**已按「编译对象身份」去重**（一次运行内同一函数 + 同一原因只记一次）⇒ 与
`jit_func_cache` 的驱逐重编译解耦，**同一应用两次运行逐字相同**（去重前能差 20 倍，
且记账的 I/O 本身会扰动时序、加剧驱逐 —— 观察者效应）。当前缺口清单见 §8.77。

## 确定性 GUI 负载驱动器（2026-09-15，`LENO_SDL_FRAMES`）

GUI 负载以前**无法可靠测量**：按"跑满 N 秒"测时，同一壁钟时长内实际跑了多少帧随系统噪声变化
（实测同一应用交替同轮测 CPU 时间能差 4 倍、两轮方向还会翻转，`FuncExecuted` 甚至差 60 倍）。

现在 `sdl_window.leno` 的 `run()` 支持**固定帧数**模式（环境变量门控，**默认关**，不设时行为逐字不变）：

```powershell
$env:SDL_VIDEODRIVER='dummy'     # 或 offscreen —— **不开任何窗口**
$env:LENO_SDL_FRAMES='60'        # 跑满 60 帧后自动清理退出
build\leno.exe leno_module\LenoSDL3\examples\应用示例\文件管理器\file_manager.leno
# → [SDL-BENCH] frames=60 total_us=416050 us_per_frame=6934 fill=900 line=3300 out=120 sum=4320
```

**读数约定**（重要）：
- `sum` = 批处理计数之和（`fill+line+out`）是**负载一致性判据**：两次运行的 `sum` 必须相同，
  此时比 `us_per_frame` 才有意义；不同就说明渲染路径分叉了（例如命中了不同的分支）。
- 实测分辨率：同配置 4 次运行 `sum` 逐字相同、`us_per_frame` **±1.5%**（file_manager 60 帧 ~7ms/帧）。
- 每帧有一个 **≥1ms 的 `ev.waitTimeout` 地板**（事件循环本身要 poll），所以**很轻的负载**下
  `us_per_frame` 会被这 1ms 抬高；重负载（如 file_manager 每帧 7ms）可忽略。A/B 是两次测量相减，
  这个常数地板会自然抵消。

**✅ 改 `leno_module/**/lib/**` 不再需要手动清缓存（2026-09-16 已修）**

历史坑（本驱动开发时踩过：`文件管理器\.lenocache` 时间戳早于 lib 改动 ⇒ 跑了 90s 都不退出的"老代码"）：
`entry_<hash>.lenb` 是**整程序**的序列化快照（模块字节码全内联），文件名只由入口文件内容哈希决定
⇒ 只改被引用模块时键不变、内容却是旧的。修法：写入口缓存的同时落一份依赖清单
`entry_<hash>.lenb.deps`（参与编译的每个模块的 size+hash，取自各模块 `.lenomc` header），
加载入口缓存前逐条校验，不符即删除缓存回退源码编译（`src/main.c` 的 `entry_deps_*`）。
旧版本残留的、没有 `.deps` 的 `entry_*.lenb` 会在下次运行时被判失效并自动删除。

**仍然需要手动清缓存的两种情况**：
- 改了**编译器本身**（`src/**`）：缓存不记录编译器二进制身份，只有 `LENO_BIN_VERSION` /
  `LENO_MODCACHE_VERSION` 手工 bump 才整体失效 ⇒ 改 src 后先清 `.lenocache` 再测。
- 改了**原生库**（`SDL3.dll` 等）：DLL 是运行时加载的，不在 `.leno` 源码快照覆盖范围内。

**前提验证探针**：`probe_sdl_headless.leno`（headless 能否建窗+建渲染器+绘制）、
`probe_headless_maximized.leno`（带 `MAXIMIZED` 标志的窗口在 headless 下是否卡住 —— 实测不卡）。

## 确定性 GC 钩子（§8.35）

测 GC / 写屏障 / 分配相关的东西时，「回收什么时候发生」必须可控，否则用例不敏感
（`gc_barrier_canary.leno` 的第一版就是这么失败的）。三个环境变量：

| 变量 | 作用 |
| --- | --- |
| `LENO_GC_YOUNG_THRESHOLD=<bytes\|KB\|MB>` | 覆盖年轻代阈值并**钉住**（禁止 `gc_minor_collect` 结尾把它抬回 `max(young_allocated*2, 8MB)`）。不钉住的话第一次回收后覆盖值就没了。 |
| `LENO_GC_FORCE_EVERY=<n>` | 每 n 次分配挂一个「强制回收」请求，在**下一个解释器安全点**无条件回收一次（不受阈值门控）⇒ 回收次数按分配次数确定。 |
| `LENO_GC_POOL_LIMIT=<bytes\|KB\|MB>` | 覆盖小对象池的持有上限（默认 **32MB**，§8.74 的实测饱和点）。**分配成本 A/B 就用它**：同一个二进制上切 4MB/16MB/32MB/64MB，即可复现 `new Pair` **110→68 ns**、`new Big` **170→89 ns** 的差异（配合 `probe_alloc2.leno`）。上限只是**天花板**（池按需增长），小程序的常驻内存不受影响；只有分配量大的程序才用得到，代价是池内存不归还 OS。 |
| `LENO_GC_TRACE=1` | 每次回收向 stderr 打一行：`[GC] minor #N young=..KB old=..KB rem=R freed=F promoted=P thr=..KB roots(stk=.. mod=.. frame=.. glb=.. misc=.. oth=..) marked=..`。`roots(...)` 是**根来源归属**（各来源新标记的对象数），`marked` 是本轮新标记总数 —— 用来定位「某个活对象靠哪个根活下来的」（排查 JIT 活值可见性时就是靠它确认 `stk=0 frame=2`）。 |

```bat
set LENO_GC_YOUNG_THRESHOLD=64KB
set LENO_GC_FORCE_EVERY=2000
set LENO_GC_TRACE=1
build\leno.exe jit_probes\gc_barrier_canary.leno
```

⚠️ **三个必须知道的限制**：

1. **`LENO_GC_FORCE_EVERY` 只在解释器安全点生效**（`OP_RETURN` / `OP_RETURN_MULTI`）。
   JIT 编译后的热循环里没有返回值 ⇒ 强制请求会被推迟到「解释器下一次返回」才兑现。
   实测：预热循环（2000 次 `churn`，共 400 万次分配）全部被 JIT 收编后，第一次回收
   一次性 `freed=4412101` —— 说明这期间**一次安全点都没到**。测 GC 时若想确定性最强，
   加 `LENO_NO_JIT=1`。
   （**注意与 3 的区别**：这里说的是上面那个「强制」钩子；**按阈值触发的（自然）回收
   现在已经能进 JIT 循环了**，见 3。）
2. **`LENO_GC_TRACE` 的输出在 stderr**，且程序若崩溃/退出，最后一行可能来不及 flush。
   判定「无屏障」这类失败时，用**退出码 + stdout 是否为空**更可靠。
3. **JIT 热循环里的（自然）回收靠「回边让出」**（§8.37/§8.38）。机制：`gc_alloc` 在
   **JIT 帧内**跨过年轻代阈值时置 `jit_gc_yield_flag`；JIT 在**自己那条回边**上轮询它
   （2 条指令），命中就走出口路径让出到解释器，由解释器在状态已发布的前提下回收。
   由此带来两条使用上的注意：
   - **只在「循环体内发射过 callout」的循环上轮询**（`body_has_callout`）⇒ 纯算术循环
     零开销；但**读数组/字典的紧凑循环也会发射**（`arr[i]` 是 callout）。
     成本见 `bench_yield_poll.leno`（§8.39）：10 亿次极紧循环 +0.11ns/轮 ≈ +5.7%，
     真实分配循环上远小于 1%（每轮本就要付一次 callout）。**注意别用 1 亿次去测** ——
     `times.ms()` 刻度 ~16ms，那个量级下只有 1 个刻度，分不清真假。
   - **让出频率 = 「每跨一次年轻代阈值一次」**，所以默认阈值（8MB）下开销可忽略；
     但如果用 `LENO_GC_YOUNG_THRESHOLD` 钉一个很小的值（如 1KB），回收会变得很频繁
     （那是压力测试场景，不是默认行为）。统计里看 `Yields:` 计数。

## 怎么跑

```bat
build\leno.exe jit_probes\probe_method.leno
build\leno.exe jit_probes\probe_alloc.leno
build\leno.exe jit_probes\probe_alloc2.leno
jit_probes\diff_examples.bat          REM 在仓库根目录执行
```

## A/B 对照的标准流程

1. **造基线二进制**（改动前的版本，供 A/B 用）：

   ```bat
   git stash push -m probe-baseline -- <你改动的文件>
   build.bat
   copy /y build\leno.exe build\leno_base.exe
   git stash pop
   build.bat
   ```

2. **交替**跑基线与新版（不要先跑完 3 轮 A 再跑 3 轮 B），**并且让顺序位置平衡**。
   本机（i5-3450）负载波动可达 10~20%，而且有明确的「越跑越慢」趋势 ——
   固定按 `ABCD` 顺序跑，靠后的配置会被系统性加价（§8.47 里有一个轮次是
   156/172/188/235 严格递增）。**做法：一半轮次用 `ABCD`、一半用 `DCBA`**，
   让每个配置在各顺序位置上的出现次数相同。

3. **判据不要只用 min**。`min` 在「效应体现在最优值」时最好用（如 §8.39 的回边轮询成本），
   但在「效应体现在分布」时会失效：§8.47 里 C 组的 min 与 A 组相同（都是 156），
   中位数/均值却能稳定分开（171 vs 188）。**默认报中位数 + 各轮原始值 + 两组 min/mean**，
   让判据自己说话，别只挑一个统计量下结论。

4. **分离单项改动**：用环境开关只关掉其中一项，于是「全开 / 只关 A / 只关 B / 全关」
   四方对照能把每项贡献拆干净。现有开关：

   | 开关 | 关掉什么 |
   | --- | --- |
   | `LENO_JIT_NOINLINE=1` | callout 处的内联 |
   | `LENO_NO_CMPJMP=1` | 「比较+条件跳转」融合（§8.47） |
   | `LENO_NO_CALLCACHE=1` | callee 解析缓存（§8.47） |

   **注意这些开关是「存在即生效」（不看值）**，所以「打开」必须 `set VAR=` 清掉变量，
   不能用 `=0` —— 写 `set VAR=0` 反而会把它关掉。

   **开关的判定位置要挑**（§8.47 踩过）：`LENO_NO_CMPJMP`（`try_emit_cmpjmp`）与
   `LENO_NO_TYPEDPARAM`（序言发射）都在**编译期**路径上，运行时零成本；
   而 `LENO_NO_CALLCACHE` 原先写成「static 首调用判负」放在
   `jit_callout_global_func` 里 —— 那是**每次 Leno 调用**都进的函数（fib 8.67 亿次），
   等于给一个默认关闭的开关每次多付 2~3 条指令。现已改为在 `jit_init()` 里一次性
   解析成 `jit_state.no_callcache`，热路径只剩一次 test。
   新增开关时先问一句：**这个判定在热路径上吗？**

5. **差分验证语义**：跑 `diff_examples.bat`，并在 `.leno` 基准里比对**输出值本身**
   （如 `sum=` / `hits=`）。输出值比耗时更可信 —— 措辞、顺序、数值都能暴露静默算错。

## 已知坑（都是踩过的）

- **`times.ms()` 只有毫秒粒度**：小规模（≤2 万次）测出来直接是 `0ms`，要用 `times.us()`。
- **假差异**：输出里含 `<ptr 0x...>`（ASLR 地址）或计时数字的行必然不同。判定回归前，
  先用**同一个二进制跑两遍**自查该文件本身是否确定。
- **「改了但没效果」先怀疑快路径没进去**：用 `LENO_JIT_DEBUG=1` 看
  `Compiled / Executed / Bailouts` 计数与 `inline(...)` 日志；只看耗时容易把
  「快路径是死代码」误判成「这类操作没有优化空间」。
- **绝对值不要跨机比较**：本机为 **Intel i5-3450**（Ivy Bridge 4C/4T）+ Windows，
  见 `docs/JIT实现与调试记录.md` §9 测试环境；换机器后需要重建基线。
- **别把被测程序直接接进管道**再取退出码或比对输出：`prog 2>&1 | findstr ... > f`
  会让 `ERRORLEVEL` 变成 `findstr` 的退出码，长行还可能被 `findstr` 截断/吞掉，
  表现为**随机假差异**（`diff_examples.bat` 踩过：先落盘存原始输出、再过滤，
  退出码也在加过滤之前取）。

## 相关记录

- **§8.30**：struct 字段读 / 方法调用内联（提交 `c4b6bbd1`）——`probe_method.leno` 的实测表。
- **§8.31**：分配成本归因（**代码未改动**，待办按风险分三级）——`probe_alloc*.leno` 的实测表。
- **§12 未解决问题 第 10 条**：对象分配 ~200ns/次 + JIT 循环内无 GC 安全点。
