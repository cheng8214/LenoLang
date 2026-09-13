# 待办：GC 与对象分配优化（交接记录）

> 建立于 2026-09-13。背景是 §8.30（struct 字段读/方法调用内联，提交 `c4b6bbd1`）之后，
> 结论是「**调用机制已经不是瓶颈，对象分配/回收才是**」。
>
> 相关章节：`JIT实现与调试记录.md` 的 §8.31 / §8.31-a / §8.31-b / §8.32。
> 本文只记「现在到哪了、下一步做什么、别重复踩哪些坑」。

---

## 一、当前性能结论（i5-3450 + Windows，负载波动大）

`jit_probes/probe_alloc2.leno`（N=10M，直接写在循环体里，不经方法调用）：

| 场景 | 耗时 |
| --- | --- |
| 不分配（基线） | ~34 ns/次 |
| `new Pair(t, hit)`（2 字段） | ~200 ns/次 |
| `new Big(a..f)`（6 字段） | ~270 ns/次 |
| 数组字面量 `[1.0, 2.0]` | ~220 ns/次 |

拆解（§8.31）：JIT 侧 callout 包装 + 参数搬运只要 **14 ns**；成本全在 C 的分配本体。
每多一个字段 **+~15 ns**、`GC_POOL_MEM_LIMIT` 太小导致退化到裸 `malloc`（池 4MB→1GB 省 56ns）、
另有 **~79 ns 未归因**。

**→ 结论：不要再往「加 JIT 覆盖率 / 内联更多 opcode」上投入。** 分配本体才是大头。

---

## 二、已完成（本次改动）

### ✅ 写屏障 + `struct_set_field` 内联（提交信息见 §8.31-b）

`gc_write_barrier()` / `gc_write_barrier_obj()` / `struct_set_field()` 内联到
`leno_value.h`；真正入集的慢路径 `gc_remembered_set_add()`（原 `remembered_set_add`）
留在 `gc.c`。函数体逐行未改，只改链接性质 ⇒ 语义等价。

实测（基线/新版二进制**交替同轮**跑）：

| 场景 | 基线 | 内联后 | 差 |
| --- | --- | --- | --- |
| `new Big`（6 字段） | 277.2 / 280.8 ns | **253.3 / 253.4 ns** | **−24 ns（−8.6%）** |
| `new Pair`（2 字段） | 198.5 / 209.7 ns | 194.6 / 202.2 ns | −4 ns |
| 不分配（对照） | 34.4 / 37.7 ns | 34.4 / 34.1 ns | 不变 |
| 数组字面量（`arr_write` 也走屏障） | 225.6 / 223.3 ns | 217.9 / 218.0 ns | −6 ns |

验证：`assert` **273 passed / 0 failed**；对象版光追确定性输出逐位一致
（`sum=50730000.000273108`、`hits=900000`、`hits=585500`，JIT 计数 Compiled 5 /
Executed 867300 / Bailouts 0）。

---

## 三、已排除的死路（**不要再试**）

1. ❌ **`struct_instance_new_with_args()`：跳过被实参覆盖字段的默认值初始化**
   （§8.31-a）。两版实现（索引数组线性扫描 / 位图掩码）**都更慢**：
   `Pair +6~8 ns`、`Big +13~18 ns`，而对照组噪声只有 ±1.5 ns。
   原因：省掉的只是一次写向已命中缓存的 store（≈0.3 ns/字段），
   而新增的每字段跳过判定打破了原来「无条件顺序初始化」的可优化形态。
   **`+28 ns/字段` 是「6 字段与 2 字段的耗时差 ÷ 4」，包含实参写入/栈搬运/类型提升等
   全部每字段成本，不能当作「默认值初始化」的单价。**
2. ❌ **把 `GC_POOL_MEM_LIMIT` 从 4MB 放大到 16/32MB**。§8.31 已测：64MB 只省 ~4 ns。
   池必须能覆盖**整段**分配才有效，而 JIT 循环内对象不回收到 free-list，
   所以小幅度放大只是换个地方撞墙。

---

## 四、待办（按优先级）

### ✅ P0 — 加「确定性触发 GC」的测试钩子（测试基建）—— 已完成（2026-09-13，§8.35）

**交付**（全部默认关闭；只改变「已有安全点何时决定回收」，不新增 GC 调用点）：

| 环境变量 | 作用 |
| --- | --- |
| `LENO_GC_YOUNG_THRESHOLD=<bytes\|KB\|MB>` | 覆盖并**钉住**年轻代阈值（本来每次回收后会被抬回 `max(young_allocated*2, 8MB)`，不钉住覆盖值立刻失效）。 |
| `LENO_GC_FORCE_EVERY=<n>` | 每 n 次分配挂一个强制回收请求，在下一个解释器安全点**无条件**回收一次（不受阈值门控）⇒ 回收次数按分配次数确定。 |
| `LENO_GC_TRACE=1` | 每次回收打一行 `[GC] minor #N young/KB old/KB rem/R freed/F promoted/P thr/KB`。 |

顺带删除死代码 `gc_check_safe_point()`（与 `gc_alloc` 开头逐字重复、零调用点）。
用法与两个必须知道的限制写在 `jit_probes/README.md`（「确定性 GC 钩子」一节）。

**⚠️ 钩子没能解决的（P1 的同一根因）**：强制请求只在解释器安全点兑现。
JIT 收编热循环后没有返回值 ⇒ 请求被推迟。实测预热循环（400 万次分配）全进 JIT 后，
第一次回收一次性 `freed=4412101`，说明这期间一次安全点都没到。
**⇒ 要在 JIT 下也确定性触发，仍然只能靠 P1（callout 边界安全点）。P1 优先级不变。**

### P1 — JIT 循环的 GC 安全点（真正的天花板）

**问题**：`gc_alloc()` 只置 `gc.deferred_gc`，不同步回收；排空点只有「解释器每 256 次返回」
和 GUI 事件循环。**JIT 编译后的热循环两处都够不到** ⇒ 循环内分配的对象无人回收，
10M 次 `new` 实测堆涨到 ~960MB，池很快耗尽后退化成裸 `malloc`（这正是 §8.31 里
「池放大到 1GB 才省 56ns」的成因）。

**两条路线，建议先做路线 2：**

| | 路线 1：回边安全点 | **路线 2：callout 边界安全点（建议）** |
| --- | --- | --- |
| 位置 | 每个 JIT 循环回边 | JIT 调 callout 之前做一次「发布」（写回 locals + 把 vstack 区间注册为 GC 根）；`gc_alloc` 超硬上限时在 callout 内同步跑一次 Minor GC |
| 覆盖面 | 最广 | **所有分配都经过 callout**（`OP_STRUCT_INIT`/`OP_ARRAY`/`OP_DICT_SET`/`OP_CALL_*`），实际够用；纯计算热循环无分配，本来也不需要 GC |
| 难度 | 需要机器码级栈映射；回边处 TOS 状态多样（`tos_live`/spilled） | callout 的 ABI 已明确、TOS 本来就要 spill |
| 风险 | 高 | 中（要保证「发布」到 callout 返回之间没有未注册的活值） |

**动手前必须先核实的两件事**（未验证）：
1. JIT 的 vstack（机器栈区）当前是否已在 GC 可见 / 根集合范围内？
2. `jit_reloaded_locals` 的确切语义，以及 `frame->locals` 在 JIT 执行期间是否是过期值？
   （§8.31 说「JIT 的 locals 在 scratch 区、`frame->locals` 内是过期值」）

**验收方式**：长跑 JIT 主循环，堆曲线不再单调上涨；`Bailouts` 不回归；
`gc_barrier_canary.leno` 从「不敏感」变成能分辨屏障好坏。

### P2 — 实参写入路径的剩余成本

内联屏障后每字段仍剩 ~15 ns，代价在 `jit_raw_to_value()` + 类型提升 + vstack 读取。
需要新的归因实验再决定要不要动（**别凭直觉改**，参考 §8.31-a 的教训）。

---

## 五、已知覆盖缺口 —— 已解除（2026-09-13，§8.35）

**写屏障的关键分支（老年代 holder ← 年轻代 value）现在可以用黑盒脚本证明了。**

历史上 `jit_probes/gc_barrier_canary.leno` 反向对照（把 `gc_write_barrier()` 改成空操作）
**仍然 `bad=0`**。加上确定性 GC 钩子后查清真因，是两件事，第一件是决定性的：

1. **写入与读取之间根本没发生过回收**：自然回收间隔 = 「256 次返回 × 每次 `churn`
   2000 次分配 = 512K 次分配」（trace 里每次 `freed=512128` 正是这个数），
   而写入→读取只隔 2 次 `churn`（4000 次分配）⇒ 目标对象永远是刚分配、还没被回收过的
   有效对象，读什么都是对的。
2. `main` 帧的残余临时/局部槽可能让 `mark_roots` 保守保活它（仍未验证，但已不必要）。

**修法**：用 `LENO_GC_FORCE_EVERY` 把回收强制塞进「写入与读取之间」；并把「分配 + 写字段」
放进 `setHolder(h)`，返回后该帧消失 ⇒ 该年轻 Node 只经老年代 holder 可达，
**屏障是它唯一的保活路径**。

**验收**（两个只差屏障的二进制，同机）：

| 版本 | 模式 | 强制回收次数 | 结果 |
| --- | --- | --- | --- |
| 有屏障 | `LENO_NO_JIT=1` | 6000 | `bad=0`，跑完 |
| 有屏障 | JIT | 4050 | `bad=0`，跑完 |
| 无屏障 | `LENO_NO_JIT=1` | 第 2002 次崩 | 退出码 −1，stdout 无 `bad=`，末行 `rem=0` |
| 无屏障 | JIT | 第 52 次崩 | 同上 |

分水岭是 `rem`（`remembered_count`）：有屏障 `rem>=1`，无屏障恒为 `rem=0`。
⇒ 「屏障相关改动不能宣称已被测试覆盖」这条限制解除；但注意验收依赖
`LENO_GC_FORCE_EVERY`，是**探针级**（`jit_probes/`）而非自动化断言（`assert/`）。

---

## 六、测量方法（必须照做，否则结论不可信）

这台机器（i5-3450）负载波动大，**跨轮比较会得出相反结论**。§8.31-a 的第一版实验
就是因为拿「改动前一次」和「改动后一次」对比，差点把 `−24 ns` 读成 `+13 ns`。

**同轮交替 A/B 步骤**：

```bat
:: 1) 暂存改动，编译出基线二进制
git stash push -m ab-xxx
build.bat
copy /y build\lenojit.exe build\lenojit_base.exe
git stash pop
build.bat
:: 2) 交替跑，至少 2 轮，取各轮最小值比较
build\lenojit_base.exe jit_probes\probe_alloc2.leno
build\lenojit.exe      jit_probes\probe_alloc2.leno
build\lenojit_base.exe jit_probes\probe_alloc2.leno
build\lenojit.exe      jit_probes\probe_alloc2.leno
:: 3) 收工清理
del /q build\lenojit_base.exe build\lenojit_broken.exe
```

**要点**：
- 必须有**未被改动的对照路径**（`probe_alloc2` 里的「不分配」行；噪声底通常 ±1.5 ns）；
- 只做**同机同轮**对照，不要跟文档里的历史绝对值比；
- 改动 `src/` 后先删 `.lenocache` 再测（缓存 key 只看入口文件内容，改 lib 会命中旧缓存）。

---

## 七、JIT bailout 后续项（2026-09-13 追加）

本节与 GC 无关，但同属「静默不优化」类缺口，一并记在这里。详见
`JIT实现与调试记录.md` §8.33。

### ✅ 已修：`OP_INDEX` 慢路径无条件 bailout

含 `d[k]` / `obj["field"]`（元素静态类型不明）的循环**每次执行都 bailout、永远跑不进 JIT**：
`ops_index.inc` 的慢路径在 callout 后直接落进紧随其后的「数组越界」bailout 桩。
已修（+7 行，慢路径末尾补 `jmp done`）。回归探针
`jit_probes/probe_index_slowpath.leno`，判据：其中 `useStructName` 的循环
`Bailouts` 不再增长（修复前总计 9 → 修复后 3）。

### ⬜ 待办 1：`OP_ADD_FLOAT` 把 dict 读取结果判成 NaN-boxed

`jit_probes/probe_index_slowpath.leno` 里的 `useDict`（`s = s + d["k"]`，热循环）仍
bailout，位置是 `OP_ADD_FLOAT`。但 `d["k"]` 编译成 `OP_GET_PROPERTY`，其 callout 对 dict
是支持的（`dict_get`）、且调用后做了 `EMIT_VALUE_TO_RAW()`，按理浮点操作数不该被判成
NaN-boxed。**原因未查清。**

排查建议：

1. 先确认 `dict_get` 返回的 `1.0` 在 Value 表示下是否真是 raw double（`val_float`）；
2. 再看 `OP_GET_PROPERTY` 那条代码路径有没有分支绕过 `EMIT_VALUE_TO_RAW()`；
3. 用 `LENO_JIT_DUMP=1` 反汇编该循环，确认 `EMIT_NUM_TO_XMM` 的 `jae → tagged` 是否真被走到。

**判据**：`useDict` 的 `Bailouts` 归零。

### ✅ 已修：`OP_EQ` 对指针 / 对象身份比较会 bailout

matrix_rain 的 `invalidateRenderer` 比较两个 `Ptr[u8]`，通用 `OP_EQ` 原来对「任一
NaN-boxed」一律 bailout，整个循环跑不进 JIT。

**做法**：在 `ops_icmp.inc` 的 `OP_EQ/OP_NEQ` 里加**运行时身份比较快路径** ——
两个操作数都是 OBJ、`obj->type` 相同、且类型 ∉ {`OBJ_STRING`, `OBJ_ARRAY`, `OBJ_BIGINT`}
→ 值位比较（= 对象身份）。不满足则落回原有慢路径（对 NaN-boxed 仍 bailout），
**语义不可能被改变**，只是 string / array / bigint / `null == Ptr` 仍走解释器。

**⚠️ 原计划的「codegen 发专用身份比较 opcode」已被否掉**：`Ptr[T]` 是 `ObjFFIPointer`，
而 `ffi.nullptr()` / `ffi.ptr_from_int(0)` 会产生**包装 NULL 的 ObjFFIPointer**，
解释器对 `null == Ptr` 走「看包装地址」的特殊规则（为真），纯身份比较会给假 ——
抬进编译期就会静默改语义。详见 `JIT实现与调试记录.md` §8.34。

**验证**：`jit_probes/probe_eq_identity.leno`（JIT 与 `LENO_NO_JIT=1` 结果逐条一致；
string / array / `ffi.nullptr()==null` 仍走解释器）；`assert` 273/0；
**matrix_rain `Bailouts` 3 → 0**。

---

## 八、当前工作区状态（2026-09-13 收工）

- 代码改动 1：写屏障 / `struct_set_field` 内联（`src/gc.c`、`src/include/leno_value.h`、
  `src/object/object_struct.c`）—— 已验证，已提交（`0ead45e9`）。
- 代码改动 2：`OP_INDEX` 慢路径越界 bailout 桩修复（`src/jit/backend/x86_inc/ops_index.inc`，
  +7 行）—— 已验证，见第七节与 `JIT实现与调试记录.md` §8.33。
- 代码改动 3：`OP_EQ/OP_NEQ` 身份比较快路径（`src/jit/backend/x86_inc/ops_icmp.inc`）
  —— 已验证，见第七节与 `JIT实现与调试记录.md` §8.34。
- 代码改动 4（2026-09-13 晚，P0）：确定性 GC 钩子（`src/gc.c`、`src/include/leno_value.h`、
  `src/include/leno_vm.h`、`src/vm/vminc/vm_init.inc`、`src/vm/vminc/op_call.inc`）
  —— `LENO_GC_YOUNG_THRESHOLD` / `LENO_GC_FORCE_EVERY` / `LENO_GC_TRACE`，
  并删除死代码 `gc_check_safe_point()`；见 `JIT实现与调试记录.md` §8.35。
- 新增：`jit_probes/gc_barrier_canary.leno`（**已重做，现在敏感**：配合钩子时
  无屏障版会崩、有屏障版 6000 次回收 `bad=0`，见第五节）、
  `jit_probes/probe_index_slowpath.leno`（`OP_INDEX` 慢路径回归探针，判据见第七节）、
  `jit_probes/probe_eq_identity.leno`（`OP_EQ` 身份比较差分探针，判据见第七节）。
- 文档：`JIT实现与调试记录.md` §8.31-a（负结果）、§8.31-b（内联屏障 + 覆盖缺口）、
  §8.33（`OP_INDEX` 慢路径 bailout）、§8.34（`OP_EQ` 身份比较快路径）、
  §8.35（确定性 GC 钩子 + 金丝雀真因）；`jit_probes/README.md` 增加「确定性 GC 钩子」一节。
- 环境已复位：临时 GC 探针已移除、对照二进制与诊断临时文件已删除。
  （2026-09-13 晚另建了无屏障对照二进制用于 P0 验收，已删除。）
