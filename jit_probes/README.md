# jit_probes —— JIT 性能探针与差分工具

用途：给 JIT 改动做**同机同轮 A/B 对照**，回答两个问题 ——「改动真的生效了吗」、
「快了多少」。这里的文件都是探针，**不参与编译器构建**；`.leno` 直接用
`build/lenojit.exe` 跑即可。

## 文件清单

| 文件 | 测什么 |
| --- | --- |
| `probe_method.leno` | 方法调用成本。同一套算术分别写成「循环内手工展开」/「最简方法」/「中等方法体」/「完整 hitSingle 形态」四段，用来定位调用机制（callout 装箱 + 函数级 JIT 序言）的固定开销。1000 万次。 |
| `probe_alloc.leno` | 分配成本随**累计分配量**的阶梯（微秒计时，1M/10M 混合），用来判断单次成本是否随堆增长劣化、拐点在哪。 |
| `probe_alloc2.leno` | 分配路径隔离。四个循环体算术量级一致，唯一变量是「分配什么」：`new 2 字段` / `new 6 字段` / 数组字面量 `[1.0,2.0]` / 不分配。全部直接写在循环体里，排除方法调用干扰。 |
| `diff_examples.bat` | 基线二进制 vs 新版二进制，对 `examples/{struct,func,module_export_struct,cstruct}` 的示例做 stdout + 退出码差分（约 59 个文件）。 |
| `gc_barrier_canary.leno` | 写屏障金丝雀：老年代 holder 只被「自己的字段」引用的年轻对象，屏障失效即被回收。**必须配合下面的确定性 GC 钩子**，否则不敏感（原因写在文件头的注释里）。 |
| `probe_jit_gc_safepoint.leno` | JIT 循环的 GC 安全点缺口探针（§8.36）：JIT 热循环里分配 + callout，用钩子数「实际到达的安全点」，并用值判据 + **身份判据**（`probe == n`，比读值可靠）判断 JIT 活值是否被误回收。 |
| `probe_index_slowpath.leno` | `OP_INDEX` 慢路径 bailout 回归探针（§8.33）：判据是 `Bailouts` 不增长，不是耗时。 |
| `probe_eq_identity.leno` | `OP_EQ` 身份比较快路径差分探针（§8.34）：判据是 JIT 与 `LENO_NO_JIT=1` 结果逐条一致 + 哪些比较仍 bailout。 |
| `bench_yield_poll.leno` | 回边 GC 轮询的每轮成本（§8.39）：10 亿次 `arr[0]`。**必须 10 亿次** —— `times.ms()` 刻度是 ~16ms，1 亿次下成本只有 1 个刻度，分辨不出。测法：与临时关掉轮询的二进制比 best-of-N。 |
| `probe_bailout_sites.leno` | bailout 站点回归探针（§8.40）：int48 溢出（ADD 2^80）与 int64 溢出（MUL 3^60）两条桩路径。判据是「JIT 与 `LENO_NO_JIT=1` 结果逐位一致」+「`LENO_JIT_DEBUG=1` 的 site 序列与改动前相同（本例 17,17,17,14,14,14）」。 |
| `probe_cast_int_peephole.leno` | `OP_CAST_INT` 恒等省略的回归探针（§8.41）：判据是 JIT 与 `LENO_NO_JIT=1` 输出逐字一致；用 `LENO_JIT_DUMP=1` 反汇编时，白名单算术后面不应再有 CAST 归一化序列，而 bool/`and`/`or` 这些非白名单来源必须有。 |

## 确定性 GC 钩子（§8.35）

测 GC / 写屏障 / 分配相关的东西时，「回收什么时候发生」必须可控，否则用例不敏感
（`gc_barrier_canary.leno` 的第一版就是这么失败的）。三个环境变量：

| 变量 | 作用 |
| --- | --- |
| `LENO_GC_YOUNG_THRESHOLD=<bytes\|KB\|MB>` | 覆盖年轻代阈值并**钉住**（禁止 `gc_minor_collect` 结尾把它抬回 `max(young_allocated*2, 8MB)`）。不钉住的话第一次回收后覆盖值就没了。 |
| `LENO_GC_FORCE_EVERY=<n>` | 每 n 次分配挂一个「强制回收」请求，在**下一个解释器安全点**无条件回收一次（不受阈值门控）⇒ 回收次数按分配次数确定。 |
| `LENO_GC_TRACE=1` | 每次回收向 stderr 打一行：`[GC] minor #N young=..KB old=..KB rem=R freed=F promoted=P thr=..KB roots(stk=.. mod=.. frame=.. glb=.. misc=.. oth=..) marked=..`。`roots(...)` 是**根来源归属**（各来源新标记的对象数），`marked` 是本轮新标记总数 —— 用来定位「某个活对象靠哪个根活下来的」（排查 JIT 活值可见性时就是靠它确认 `stk=0 frame=2`）。 |

```bat
set LENO_GC_YOUNG_THRESHOLD=64KB
set LENO_GC_FORCE_EVERY=2000
set LENO_GC_TRACE=1
build\lenojit.exe jit_probes\gc_barrier_canary.leno
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
build\lenojit.exe jit_probes\probe_method.leno
build\lenojit.exe jit_probes\probe_alloc.leno
build\lenojit.exe jit_probes\probe_alloc2.leno
jit_probes\diff_examples.bat          REM 在仓库根目录执行
```

## A/B 对照的标准流程

1. **造基线二进制**（改动前的版本，供 A/B 用）：

   ```bat
   git stash push -m probe-baseline -- <你改动的文件>
   build.bat
   copy /y build\lenojit.exe build\lenojit_base.exe
   git stash pop
   build.bat
   ```

2. **交替**跑基线与新版（不要先跑完 3 轮 A 再跑 3 轮 B），各 3 轮取**最小值**。
   本机（i5-3450）负载波动可达 10~20%，取 min 比取平均更能反映真实成本。

3. **分离单项改动**：用环境开关只关掉其中一项（例：`LENO_JIT_NOINLINE=1` 只关闭
   内联，其它改动保留），于是「基线 / 只 A / A+B」三方对照能把每项贡献拆干净。

4. **差分验证语义**：跑 `diff_examples.bat`，并在 `.leno` 基准里比对**输出值本身**
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
