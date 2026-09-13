# JIT 安全点与去优化：HotSpot / LuaJIT 参考实现调研

> 建立于 2026-09-13。目的：为「JIT 循环里怎么触发 GC」找行业答案。
> 直接背景是 `JIT实现与调试记录.md` §8.36 / §8.37 —— 我们实测出的三件事：
> ① JIT 活值在机器栈里、GC 看不见；② 因此不能在机器码里就地回收；
> ③ bailout 的语义是「解释器从 JIT 进入点重跑整个循环」（不写回 locals），
> 所以它不能当周期性中断用（实测死循环）。
>
> 本文里每一条都标注了来源：「源」= 抓到的页面明确写的，**「推论」= 我的理解/推断**，
> 需要以源码为准。抓取时间：2026-09-13。

---

## 一、HotSpot：安全点怎么放、怎么停、GC 怎么看见活值

### 1.1 poll 指令的插入位置（源）

HotSpot 只在**可能长时间执行的指令序列**上放 poll：**循环回边、方法返回前、调用之后、
抛异常处**。判据是「单条指令执行极短，唯一可能长时间执行的情形是指令序列复用」。
poll 本身由 JIT 插进机器码（OpenJDK 8 上是 `test` 指令，操作数地址指向 polling page）。

> 来源：`Safepoints in HotSpot JVM`（lizb0907，2020-07-27，OpenJDK 8 源码梳理）。

**对本项目**：我们现在的安全点是 `OP_RETURN`（≈"方法返回前"）+ 本轮新加的回边，
**和 HotSpot 的放置位置一致**。缺的是"调用之后"那一处（我们的 callout 边界）。

### 1.2 怎么"停"：polling page（源）

- VM 启动时留一个**全局 polling page**；需要停线程时把它设成**不可读**
  （Linux：`mprotect(page, PROT_NONE)`），poll 就是**读这个页**；
- 线程读到不可读页 → SIGSEGV → 信号处理器把线程阻塞在
  `SafepointSynchronize::block()`；
- 这是**主动式中断**（线程自己轮询并自行挂起），不是"抢先中断"；
- 解释器侧则是把 DispatchTable 换成 `_safept_table`（解释器也有安全点）。

**对本项目（推论）**：poll 的成本就是「一次 load + 一次条件分支」。我们是单 VM 线程、
不需要停别的线程，所以不可能也不需要故障机制 —— **用 `vm.gc_force_request` 这个
普通 load+branch 是同一个数量级的成本**（本轮已在回边接上）。HotSpot 用故障页的好处是
「非安全点路径上零开销」（页可读时分支预测 100% 命中），我们靠预测也能达到同样效果。

### 1.3 GC 为什么能在 poll 处就地回收：OopMap + ScopeDesc（源 + 补充）

去优化/GC 需要两样元数据（抓到的页面只给了 `UnrollBlock` / `vframeArray` 的骨架，
OopMap / ScopeDesc 属补充知识，但与 HotSpot 的公认设计一致）：

| 元数据 | 作用 |
| --- | --- |
| **OopMap** | 在编译代码的**某个具体 PC** 上，哪些寄存器/栈槽持有对象引用 |
| **ScopeDesc / DebugInfo** | 编译代码与**源字节码（bci）**的映射：方法、bci、局部变量与操作数栈槽的对应、内联层次、monitor 信息 |

有了 OopMap，GC 在 poll 点可以直接扫寄存器+栈（**不用写回**）；有了 ScopeDesc，
去优化可以**精确重建**出"这个 bci 处的解释器帧"。

### 1.4 "bailout" 的正确形态：帧重构，而不是重跑（源 + 补充）

- 编译代码里的 **uncommon trap** 就是各类守卫的失败出口；原因分**每字节码**
  （null/range/class check）与**全局**（类卸载、方法过旧等）；
- 去优化分两阶段：**信息收集** → **帧解包**；`UnrollBlock` 装重建所需信息，
  `vframeArray` 负责把编译帧**变换为解释器帧**（连逃逸分析消除掉的对象、消除掉的锁都要恢复）；
- 触发方式之一是 `frame::deoptimize()` **改写返回地址指向去优化 stub**（即"延迟到该帧返回时再解包"）；
- 动作还有 `make_not_entrant` / `make_not_compilable` 等（对应"这个 nmethod 别再进了"）。

> 来源：DeepWiki `Deoptimization System`（JetBrains/jdk8u_hotspot 2.4 节，索引于 2025-06-09）。
> 该页明确说它只是文件索引级概览；帧重构与元数据的细节要读 `deoptimization.cpp` /
> `vframeArray.cpp` / `frame.cpp`。

**这是本次调研对我们最重要的一条**：HotSpot 的"回退"是**把编译帧重建成 bci 精确对应的
解释器帧**，所以它可以频繁发生（lazy deopt）；而我们的 bailout 是
**"解释器从 JIT 进入点重跑整个循环"**（§8.37 实测）—— 两者差的就是 ScopeDesc/OopMap
那套元数据。**"bailout 会重复副作用"不是我们实现得糙，而是我们没做元数据。**

---

## 二、LuaJIT：不扫机器码，靠"栈槽是权威副本 + snapshot"

> ⚠️ 来源限制：抓到的是 **LuaJIT 3.0 新 GC 的设计草案页**（Mike Pall，最后编辑 2017-12-28，
> 明言"尚无代码、需要赞助"）。它**不能**用来推断 LuaJIT 2.1 的实际行为。
> 下面 2.1/2.2 的机制标注为「推论」，需以 `lj_gc.c` / `lj_snap.c` / `lj_asm.c` 为准。

### 2.1 草案页明确写的（源）

- **必须非移动（non-copying）**：受 Lua/C API 约束 ⇒ JIT 可以在寄存器里缓存裸指针；
- **增量但不实时、不并发**：Lua state 相互独立，不需要并发 GC ⇒ **不存在异步抢占**，
  这直接决定了"GC 只在协作式安全点运行"是可行的；
- **栈被当成"永远灰色"**：sweep 前重扫一次栈，于是**栈槽写入不需要写屏障**
  （"stores to stack slots, which are the most common kind of stores"）；
- 写屏障 2~3 条指令，"**JIT 可以消除大部分写屏障**"；
- GC64 与 GC 算法本身正交（"tagged value 的布局问题"）。

### 2.2 LuaJIT 2.x 的机制（推论）

- **GC 只扫 Lua 栈与对象图，从不扫 mcode 寄存器或录制器的 IR 状态**；
- **snapshot（`IR_SNAP`）**：每个 trace 退出点上记录每个 IR 值的位置（寄存器 / mcode 栈帧 /
  常量 / Lua 栈槽），trace 退出桩据此**重建解释器可见状态**；
- **GC 只在显式安全点跑**：分配 helper（`lj_tab_new` / `lj_str_new` / 扩容）与解释器分发循环；
- 因此凡是"活过安全点"的 GC 对象，**必须在进入该点之前同步回 Lua 栈槽**（stack sync）；
- trace 作为常量引用的 GC 对象（`IR_KGC`）也要作为 root 登记。

**对本项目（推论）**：LuaJIT 和我们的差别不是"有没有安全点"，而是
**它的栈槽就是 GC 的权威副本**（解释器与 mcode 共用同一条 Lua 栈），
而我们的 JIT 把 vstack 放在**自己的机器栈帧**里、而且放的是**裸值**。
所以 LuaJIT 的"发布"成本≈0（本来就在栈上），我们的"发布"要额外做一次 raw→Value 转换。
这正是 §8.36 得出的结论，也从另一条路径得到印证。

---

## 三、横向对照

| 维度 | HotSpot | LuaJIT 2.x（推论） | LenoJIT 现状 |
| --- | --- | --- | --- |
| 安全点位置 | 回边 / 返回前 / 调用后 / 抛异常 | 分配 helper + 解释器分发 | `OP_RETURN`（≈返回前）+ 本轮加的回边 |
| poll 形态 | 一次 load（故障页）+ 分支 | 分配点即调用点（本来就要调 C） | load `vm.gc_force_request` + 分支 |
| GC 怎么看见编译帧活值 | **OopMap**（寄存器/栈槽位图） | **栈槽是权威副本** + snapshot | ❌ 看不见（活值在机器栈 scratch / vstack） |
| 回退语义 | **帧重构**（ScopeDesc → bci 精确的解释器帧） | **snapshot 重建**退出点状态 | ❌ 重跑（从 JIT 进入点重放整个循环） |
| 能否就地回收 | 能（任意 poll 点） | 能（但要先 stack sync） | ❌ 不能 ⇒ 只能让出到解释器 |
| 移动式 GC | 有（G1/ZGC 等，靠 OopMap + 屏障） | 非移动（明言必须） | 非移动 ✓ |

---

## 四、对本项目的结论（按性价比）

### 4.1 现在该做的：路线 3「回边让出」= HotSpot 的 poll 位置，但不做 OopMap

- HotSpot 在回边放 poll 是为了**让 GC 能等**；我们在回边让出是为了**让 GC 能跑**。
  位置相同，代价不同（我们多一次"退出到解释器"的往返），但**不需要任何元数据** ——
  因为退出路径（`EMIT_WRITEBACK_LOCALS` + vstack 平衡）本来就把状态发布好了。
- 与 §8.37 的负结果对照：**必须用 exit 路径让出，不能用 bailout**（后者不发布状态）。
  这一条现在有了行业侧的解释：bailout 是"守卫失败"语义，本来就假设"退回去重来"，
  而重来的前提是**能重建状态**；我们没有重建能力，所以不能用它做周期中断。

### 4.2 长期要做才能"就地回收"的：栈位图（我们的 OopMap）

要像 HotSpot 那样在机器码里直接回收，最小可行集是：

1. **每个 safepoint 的栈位图**：哪些 scratch/vstack 槽此刻是"活的对象引用"（编译期可算，
   因为 scan 阶段已经知道每个 bc_off 的 vstack 深度与类型位图 `RBX`）；
2. **GC 按位图扫机器栈区**（`rbp` 相对区间），而不是按 NaN-box 标签猜 ——
   这一步也顺带解决"裸 double 与 tag 撞车"的问题；
3. 回退时（bailout）**按位图 + bci 重建**，而不是重跑。

第 2 点有个更便宜的替代（LuaJIT 路线）：**让 JIT 的 vstack 直接就是 `vm.stack`**
（或镜像发布），那它就天然被 `mark_roots` 扫到。代价是每次 TOS 变更都要落内存，
与当前"TOS 常驻 RAX"的优化冲突 —— 与本项目 P0 的收益判断一致：**不值得现在做**。

### 4.3 值得顺手记下的设计判断

- **HotSpot 的 poll 位置清单里有"调用之后"**：我们的 callout 边界也是天然安全点候选。
  但 §8.36 说明在 callout 边界回收需要**完整发布活值**（含 `a + f(b)` 里只在栈上的 `a`），
  成本高于回边让出 ⇒ 仍然优先回边。
- **"不能太少也不能太频繁"**：HotSpot 的原话判据。对应到我们的设计就是
  "只对循环体内可能分配的循环发射回边检查"（否则给 `i++` 加百分比级税）。
- **非移动 + 不并发 ⇒ 协作式安全点可行**：LuaJIT 与我们都满足这两条，
  所以"GC 只在显式安全点跑"是合法设计，不需要信号/抢占那一套。

---

## 五、来源清单

| 来源 | 用途 | 备注 |
| --- | --- | --- |
| [Safepoints in HotSpot JVM](https://lizb0907.github.io/2020/07/27/Safe-Point/)（2020-07-27，OpenJDK 8） | poll 插入位置、polling page（`mprotect PROT_NONE`）、主动式中断、解释器 `_safept_table` | 概览级，作者自述细节未抠完 |
| [Deoptimization System](https://deepwiki.com/JetBrains/jdk8u_hotspot/2.4-deoptimization-system)（DeepWiki，索引 2025-06-09） | uncommon trap 的原因/动作表、两阶段帧重构、`UnrollBlock`/`vframeArray`、`frame::deoptimize()` 改返回地址 | 索引页，OopMap/ScopeDesc 需读源码 |
| [New Garbage Collector](https://yanl.cc/luajit-wiki/New-Garbage-Collector.html)（LuaJIT Wiki，Mike Pall，最后编辑 2017-12-28） | 非移动/增量不并发/栈永远灰色无需栈槽写屏障/GC64 正交 | **3.0 草案，无代码**；2.x 机制需查源码 |

> 抓不到可用权威源的部分（本文件已明确标注为「推论」）：LuaJIT 2.x 的 snapshot /
> stack sync 具体实现、HotSpot C1/C2 插入 poll 的编译阶段、counted-loop 的 poll 消除。
> 如果要引用这些，应直接读 `lj_snap.c` / `lj_record.c` 与 `graphKit.cpp` / `loopnode.cpp`。
