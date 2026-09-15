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
⚠ 计数是**指示性**的（同配置重跑 `FuncCompiled` 在 97~346 波动）—— 看种类与量级，
不要当精确指标。当前缺口清单见 §8.77。

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

**⚠️ 改 `leno_module/**/lib/**` 之后必须先清缓存，否则会静默用旧模块**（本驱动就是这么被坑的：
`文件管理器\.lenocache` 的时间戳早于 lib 改动 ⇒ 跑了 90s 都不退出的"老代码"）：
```powershell
Remove-Item -Recurse -Force "<引用目录>\.lenocache", "leno_module\LenoSDL3\lib\.lenocache"
```
根因：`.lenocache` 是**按引用目录**存的，改被引用模块（lib）不会让引用方失效。
（这是个**缓存失效缺陷**，值得单独修；在那之前，动过 lib 就清缓存。）

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
