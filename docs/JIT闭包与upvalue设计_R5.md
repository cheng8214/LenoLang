# JIT 闭包与 upvalue 设计（roadmap R5）

> 状态：**设计稿，未动手**（2026-09-15）
> 本文只交付「设计不变量 + 分阶段计划」，不含实现。
> 前置阅读：`JIT实现与调试记录.md` §11 roadmap R5 / §12 未解决问题。
> 文中所有 `file:line` 均取自当前工作区的源码实测，不是推测；凡未核实的一律标注「待核实」。

---

## 0. 摘要

R5 的难点**不在"补 4 条 opcode 的 codegen"**，而在于：JIT（尤其循环 JIT）根本没有「可被外部长期引用的 locals 槽」——locals 活在 scratch / 机器栈 / pinned 寄存器上，迭代即复用、退出即失效，而 GC 又看不见它们。而 upvalue 机制的核心恰恰要求一个**存活期内地址不变**的 `Value*`。

因此本设计把问题切成「捕获形态」而不是「opcode 类型」，并确立一条主线不变量：

> **I1（主线）：JIT 永不制造新的 open upvalue。**

JIT 只做两件事：
1. 创建 **closed** upvalue（值捕获 `is_value_capture=1`）——`location` 指向它自己的 `closed` 字段，与 JIT 栈无关；
2. **复用**既有的 `Upvalue*`（by-upvalue 捕获 `is_local=0`）——生命周期归它的创建者（外层帧或它自己的 closed 字段）。

只要 I1 成立，「open upvalue 的关闭时机 / 悬空 location / bailout 回滚 open 状态 / `vm_grow_frames` 地址重映射」这一整类问题**被结构性消除**，而不是被小心地绕开。

> **I1 的第三条下游依赖（2026-09-15 补）**：`OP_TAIL_CALL` 的 JIT 实现（`docs/JIT实现与调试记录.md` §8.76）**依赖 I1 + C3 拒绝**。VM 的尾调用必须"关闭本帧全部 upvalue"（因为它复用当前帧，`op_call.inc:42-46`），而 JIT 侧不复用帧 —— 之所以可以不做这一步，正是因为 I1 保证 JIT 永不创建 open upvalue、C3 拒绝保证 JIT 函数不引用本帧 locals ⇒ 被跳过的"关闭"恒为空操作。
> **⇒ 若 P4（C3 提升槽）将来落地，必须重新审查 `OP_TAIL_CALL`。**（前两条下游依赖见 §3 的 C1 与 §7 第 4 条。）

收益侧同样乐观：语义分析已经保证「**循环体内的变量 ⇒ 值捕获**」（`src/semantic/semantic_upvalue.c:200`：`is_value_capture = target_sym->is_in_loop`），也就是说真实负载里「循环里建闭包捕获循环变量」这一最高频形态，**正好落在最安全的 C2 上**。

---

## 1. 范围

### 1.1 目标

| 形态 | 内容 |
|---|---|
| `OP_CLOSURE` | 在循环 JIT / 函数级 JIT / 内联体内创建闭包 |
| `OP_GET_UPVALUE` / `OP_SET_UPVALUE` | 闭包体内（被 JIT 化时）读写捕获变量 |
| `opcode_size` | 让扫描器能解析 `OP_CLOSURE` 的变长操作数（R3 故意留的坑） |
| ABI | 函数级 JIT 增加 closure 通道 |

### 1.2 非目标（明确不做，或另立项）

| 项 | 原因 |
|---|---|
| `OP_CLOSE_UPVALUE` | **编译器从不发射**（全仓 `src/codegen/**` 零引用，仅存在于枚举/分发表/VM 实现/反汇编表/长度表）⇒ 继续维持 scan 侧拒绝 |
| 引用捕获本帧 locals（`is_local=1 & is_value_capture=0`） | 唯一需要「改 locals 布局」的重活，见 P4；默认**拒绝** |
| `has_captures` 门控语义错配、`SET_UPVALUE` 无写屏障、异常展开不 `close_upvalues` | VM 侧既有疑点，**不在 R5 夹带**，见 §7 |
| 多返回值 / `OP_TAIL_CALL` | 属 R6，排在 R5 之后 |

---

## 2. 现状取证

### 2.1 VM 侧闭包语义（决定了 JIT 必须复刻什么）

| # | 事实 | 证据 |
|---|---|---|
| V1 | `OP_CLOSURE` 操作数 = `func_const(2)` + 每条捕获 `is_local(2) index(2) is_value_capture(2)`，**捕获描述表内联在字节码流里**，条数取自 `function->upvalue_count` | `src/vm/vminc/op_call.inc:556-616`；`src/debug.c:198-218` |
| V2 | 捕获本帧局部槽 = `&frame->locals[index]`；`frame->locals == NULL` 的帧（模块初始化临时帧）退化到 `&vm.stack[stack_base + index]` | `op_call.inc:575-598` |
| V3 | 值捕获 = `new_upvalue(val_null())` 后写 `closed`、`location = &closed`、并 `gc_write_barrier` —— **不加入 open 链表** | `op_call.inc:577-581`；`src/vm/vminc/vm_upvalue.inc:60-67` |
| V4 | 引用捕获 = `capture_upvalue(&locals[index])`：按 `location` 地址在 `vm.open_upvalues` 里**去重复用**，链表按地址降序；同一槽全局只有一个 open upvalue（闭包共享可变状态的基础） | `vm_upvalue.inc:70-100` |
| V5 | 关闭只发生在**函数返回**：`OP_RETURN` / `OP_RETURN_MULTI` / `OP_TAIL_CALL`，且都是**无条件**关闭（先判 `vm.open_upvalues != NULL`）`close_upvalues(locals, local_count, 1)` | `op_call.inc:680-682` / `784-786` / `44-46` |
| V6 | locals 是**每帧独立数组**（≤32 槽走帧内 `inline_locals`，否则 `malloc`），不是操作数栈 | `src/include/leno_vm.h:277-302`；`src/vm/vminc/vm_call.inc:88-98` |
| V7 | `vm.frames` 扩容时对 open 链表做**整块地址重映射**（旧块范围内的 `location` 按 delta 平移） | `vm_upvalue.inc:23-57` |
| V8 | GC：`OBJ_CLOSURE` 标 function + 前 `upvalue_count` 个 upvalue；`OBJ_UPVALUE` 标 `*location` 与 `closed`；open 链表本身是根；帧 `locals[0..local_count)` 是根 | `src/gc.c:566-637`、`gc.c:1052-1057`、`gc.c:1011-1015` |
| V9 | `gc_alloc` 三条路径（池复用 / 新块切割 / 裸 malloc）**都整套 `memset 0`** ⇒ JIT callout 只需照抄 VM 的字段初始化，`upvalues[n..255]` 自动为 NULL | `src/gc.c:396 / 406 / 415 / 424` |
| V10 | 编译器**无条件发射** `OP_CLOSURE`（`upvalue_count == 0` 也发射，没有"无捕获就跳过"的优化）；struct 方法另有「预创建闭包」旁路（仅 `upvalue_count == 0` 时预创建） | `src/codegen/codegen_func.c:352-367`、`codegen_expr.c:2159-2185`；`op_struct.inc:124-134`、`src/serialize/serialize.c:1169-1176` |

### 2.2 JIT 侧结构（决定了能做在哪一层）

| # | 事实 | 证据 |
|---|---|---|
| J1 | ABI 现在是 `int (*)(Value* locals, Value* globals)`，注释即契约 `RCX = locals`；shim 把 System V 的 RDI/RSI 搬到 RCX/RDX，然后 `R9 = globals` | `src/jit/jit.h:14-17, 41`；`src/jit/backend/x86_64.c:894-910, 928` |
| J2 | 函数级 JIT 快路径**不压 VM 帧**，实参装进 `jit_func_locals_pool[JIT_FUNC_MAX_DEPTH][JIT_MAX_LOCALS]` 后直接 C 调机器码 ⇒ 体内任何 `frame->…` 读到的都是**调用方**的帧 | `src/jit/jit_priv.h:308-309`；`src/jit/jit.c:504-508`；`jit_callout.c:1377-1414` |
| J3 | 函数级 JIT 的 locals 进 scratch 后**不写回**（`func_mode` 分支明确跳过） | `x86_64.c:1418-1426` |
| J4 | 循环 JIT 的 locals 在 scratch（`[RBP - 8*(i+1)]`，pinned 槽在 R12~R15），`frame->locals` 执行期是**过期值**；进出各一次拷贝 / 写回 | `x86_64.c:38-39, 61-63, 480-500, 936-1040, 1320-1406` |
| J5 | 栈深只在**编译期**用局部 `int vstack` 记账（scan 与 codegen 各一份），物理调整靠各 case 里的局部 `pop_bytes`；曾有 `OP_GET_PROPERTY` 独立形态漏算 8 字节导致 RSP 下漂（R12/§8.67） | `x86_64.c:1155`；`jit_scan.c:690`；`ops_callout.inc:1083-1103` |
| J6 | 两个扫描器**都拿得到 chunk**：`scan_loop_body(..., Chunk* chunk)`、`scan_callee_for_inline(Chunk* cc, ...)`；但 `opcode_size(ip)` 只有 `ip`，故 `OP_CLOSURE` 故意返回 `-1`（"宁可不编，不要猜"） | `jit_scan.c:676-677`、`408-412`、`376-380`、`311-312` |
| J7 | 闭包 4 条 opcode 的当前落点：`OP_CLOSURE` → `unknown opcode`（size<0）拒收；`GET/SET/CLOSE_UPVALUE` → 已登记长度（3/3/1 字节）但无 case ⇒ `default` 的 `unsupported` 拒收；内联侧同理 | `jit_scan.c:703-710` / `426-432`；`jit_scan.c:1437-1444` / `650-654`；`340`、`316` |

### 2.3 冲突根因

一句话：**VM 的 upvalue 锚在 `Value*`（帧 locals 槽的地址）上，而 JIT 的 locals 没有这样的地址。**

具体四处对不上：

1. **地址**：JIT locals 在 scratch（机器栈）或 pinned 寄存器里，取不到"存活期内不变"的 `Value*`（J4）。
2. **可见性**：`mark_roots` 只扫 `vm.stack` 与各帧 `locals`，**看不到 JIT scratch / 机器栈**；现行契约是「JIT 帧内绝不就地回收，只置让出标志」（`src/gc.c:429-442`）。
3. **帧**：函数级 JIT 快路径不压帧（J2），而 `OP_CLOSURE` / `GET/SET_UPVALUE` 全靠 `frame->locals` / `frame->closure` ⇒ 必须由 ABI 显式带入。
4. **回退**：循环 bailout 从**循环头重跑本轮**、函数级 bailout **重跑整个调用**（`jit.c:511-519`），而"建闭包"是**堆副作用**——这是 §12 已登记取舍的新增受害面。

---

## 3. 形态分解（本设计的核心表）

按「捕获种类 × JIT 层级」穷举，只有 4 个形态可安全支持，其中 2 个覆盖真实负载的绝大多数。

| 编号 | 形态 | 需要的新机制 | 安全性 | 处置 |
|---|---|---|---|---|
| **C0** | `upvalue_count == 0` 的 `OP_CLOSURE`（捕获描述表为空） | 长度解析 + 一个分配 callout | ✅ 无 upvalue，无地址问题 | **P1 做** |
| **C1** | 全部捕获都是 `is_local=0`（**by-upvalue**，捕获外层闭包的 upvalue） | closure 通道（读 `frame->closure->upvalues[i]`，纯指针复制） | ✅ 复用既有 `Upvalue*`，生命周期归创建者 | **P2 做** |
| **C2** | `is_local=1 & is_value_capture=1`（**值捕获**） | 从 scratch 读值 → `new_upvalue()`（closed 形态） | ✅ `location` 指向自身 `closed`，与 JIT 栈无关 | **P3 做（收益主体）** |
| **C3** | `is_local=1 & is_value_capture=0`（**引用捕获本帧 locals**） | 需把被捕获槽提升为 JIT 托管的稳定地址 + 复刻 close 语义 | ❌ 破坏 I1；需改 locals 布局 | **默认拒绝；P4 视度量决定** |
| **U1** | `GET/SET_UPVALUE` 在**函数级** JIT 体内 | closure 通道 | ✅ 纯指针链（`closure→upvalues[i]→location→值`） | **P2 做** |
| **U2** | `GET/SET_UPVALUE` 在**循环级** JIT 体内 | closure 通道（`jit_try_hot_loop` 入参里已有 `frame` ⇒ 入口可取 `frame->closure`） | ✅ 同上 | **P2 做** |
| — | `OP_CLOSE_UPVALUE` | — | 编译器不发射 | **维持拒绝** |

**两条容易被忽略的连带结论**：

- C1 的前提正是 I1：如果 JIT 自己会造 open upvalue，那么「外层闭包」可能是**正在执行的 JIT 帧**，它的 open upvalue 会指向 JIT scratch ⇒ C1 也一起变得不安全。**I1 同时保护了 C1。**
- U1/U2 必须**每次重新取 `upvalue->location`**（不可缓存）：open→closed 转换会改写 `location`（V5、`vm_upvalue.inc:122`）。

---

## 4. 设计不变量（I1–I8）

每条给：陈述 / 依据 / 破坏后果 / 验证方式。**这 8 条是实现的验收标准，也是 review 时唯一需要盯的东西。**

| # | 不变量 | 依据 | 破坏后果 | 验证 |
|---|---|---|---|---|
| **I1** | **JIT 永不创建 open upvalue**（绝不调用 `capture_upvalue`；只造 closed 值捕获、只复用既有 upvalue） | V3/V4、J4 | 悬空 `location`（指向已弹出的机器栈）→ 随机内存损坏；且 `vm_grow_frames` 的重映射覆盖不到 JIT 区 | 代码级：callout 里 grep 不到 `capture_upvalue`；用例：JIT/`LENO_NO_JIT=1` 结果与「闭包调用后仍可读」全部一致 |
| **I2** | **不缓存 `Upvalue*` 与 `location`**：闭包体内每次访问都重新取链 | V5、`vm_upvalue.inc:122` | open→closed 后仍写旧栈槽 ⇒ 语义静默错误（读到过期值/写丢） | 用例：闭包先被 open 引用、外层返回后再调用，结果与解释器一致 |
| **I3** | **值捕获逐字复刻 VM 的字段初始化与写屏障顺序**：`new_upvalue(val_null())` → `closed = value` → `gc_write_barrier` → `location = &closed` | V3、`op_call.inc:577-581` | 漏写屏障 ⇒ 老年代 upvalue 指年轻代值跨代漏标（`gc.c` 的 remembered set 不修） | 用例：老年代 upvalue 被写入新分配对象 + 强制 `gc_collect` 后仍可达 |
| **I4** | **每次执行 `OP_CLOSURE` 都新建闭包**（与 VM 一致，不做"同缓存复用"）；唯一例外是 struct 方法预创建闭包那条旁路**不动** | V10、`op_struct.inc:124-134` | 身份语义与 VM 分叉（`closure == closure` 跨执行变成 true） | 用例：循环里建 N 个闭包，两两身份比较与解释器逐位一致 |
| **I5** | **JIT 新建的闭包在离开 JIT 前必须落到 GC 可见处**（locals 写回 / 语法上已压入 vstack 干净点）；依赖「JIT 帧内不就地回收」既有契约 | V8、`src/gc.c:429-442` | 让出/退出时闭包只在寄存器里 ⇒ 解释器侧回收后使用已释放对象 | 用例：热循环内建闭包 + 触发 GC 让出（参考 §8.37 的做法），结果不变 |
| **I6** | **scan 期拒绝优先于运行期 bailout 兜底**：凡是不能静态证明安全的形态，直接 `capable=0`，不靠 bailout 补救 | §12 取舍「bailout 重跑无法回滚堆副作用」 | bailout 重跑会**重复建闭包**（若已被存进堆容器，就是可见的重复元素） | 用例：拒绝形态在 `LENO_JIT_DEBUG=1` 下必须打印 `unsupported/unknown`，且不出现 bailout |
| **I7** | **closure 通道必须嵌套安全**：按调用深度/帧保存-恢复，禁止用单一全局裸槽 | J2、`jit_func_locals_pool` 的深度索引先例 | A 的 closure 被 B 覆盖 ⇒ A 体内 `GET_UPVALUE` 静默读错环境 | 用例：闭包 A 调闭包 B、B 内再读写自己的 upvalue，交叉验证 |
| **I8** | **内联体与 upvalue 互斥**（见 P2 说明）：inline scan 维持「含 upvalue 相关 opcode 则拒绝内联」 | `jit_scan.c:650-654` 现状即拒绝 | 内联后"当前 closure"归属不清（caller 的 vs callee 的）⇒ 读错环境 | 用例：被内联方含 `GET_UPVALUE` 时必须 `inline-scan FAIL`，且不影响外层循环编译 |

> 一句话记法：**I1 管生命周期，I2 管解引用，I3/I4 管语义逐字对齐，I5 管 GC，I6 管失败模式，I7/I8 管"当前闭包是谁"。**

---

## 5. 分阶段计划

设计原则：**每个阶段都能独立验收、独立回退，且不依赖后一阶段**；先做「零风险地基」，再做「收益主体」，把唯一的重活（C3）压到最后并按度量决定是否做。

### P0 —— 度量与基线（纯诊断，无行为变化）

**目的**：先拿到形态直方图，避免做「没有实测入口的复杂度」（R3 的方法学）。

- 在 scan 侧（两套扫描器共用的一条路径即可）**只读**解析 `OP_CLOSURE` 的捕获描述表，在 `LENO_JIT_DEBUG=1` 下打印形态计数：
  `[JIT-DEBUG] CLOSURE caps=N local=x value=y byup=z`
- 真实负载至少覆盖：`file_manager` 交互负载、SDL3 UI（回调密集型）、`examples/性能测试`。
- 新增 4 支探针（见 §8），并建立「JIT vs `LENO_NO_JIT=1` 逐位一致」的对拍脚本口径。

**验收**：一张 2026-09-15 的形态表（C0/C1/C2/C3 各自出现次数与所在负载）。
**Gate**：若 **C3 占比 ≈ 0**（大概率，因为循环变量已被语义分析转成值捕获），P4 直接永久拒绝。
**风险**：无（只加打印）。**回退**：删打印。

**实测（2026-09-15，`file_manager` + 其全部依赖模块）**：

- **测量手段（可复用，且不开窗、不需交互）**：`--debug-out <file>` 会在**执行之前**把主程序 +
  所有已加载模块的字节码写盘（`src/main.c:83-98` 的 `debug_dump_all_bytecode`）；
  配合 `SDL_VIDEODRIVER=dummy`（无窗口）+ 之后强杀，就能安全拿到整份反汇编，
  且其中带 `[up(i): local=N idx=X val=Y]` 捕获描述 ⇒ 形态普查不需要让程序跑热。
- **静态普查结果**（全程序 **404** 个 `OP_CLOSURE`）：

| 形态 | 数量 | 占比 |
|---|---|---|
| C0 零捕获 | **357** | 88% |
| C3 引用捕获（本帧局部） | **57** 条捕获 | 12% |
| C2 值捕获 | **0** | — |
| C1 by-upvalue | **0** | — |

- **热代码侧的观测**：把应用跑到**优雅退出**（发 `WM_CLOSE`，stderr 才会 flush）后，
  `LENO_JIT_CLOSURE=1` 的输出里**一行闭包形态都没有** ⇒ 这个应用的闭包**全部活在冷路径**
  （初始化、事件回调注册），被编译的热循环/函数里没有任何闭包指令。

---

### P1 —— 长度解析 + C0（零捕获闭包）

**改动点**
1. **chunk 感知的长度解析**：新增 `opcode_size_in_chunk(Chunk* chunk, const uint8_t* ip)`，**只对 `OP_CLOSURE` 特殊处理**（`2 + 6 * upvalue_count`，`upvalue_count` 从 `chunk->constants[const_idx]` 的函数对象取），其余转发现有 `opcode_size(ip)` —— 保持原函数的契约不变（R3 的"宁可不编，不要猜"由此被正经满足，而不是绕过）。
   两处调用点都已持有 chunk（J6），改动面最小。
2. **两套 scan 各加 `OP_CLOSURE` case**：解析出 `n = upvalue_count`；**仅当 `n == 0` 时放行**，否则按 C1/C2/C3 分派（P1 阶段一律 `capable=0` / `inline-scan FAIL`，但仍要正确**跳过**操作数字节，避免走错字节流）。
3. **callout**：`jit_callout_make_closure(Value func_val)`（零捕获版），照 `jit_callout_get_method` 的分配模式（`gc_alloc` → 设 `function` / `upvalue_count`），失败置 `jit_callout_failed` 交解释器报错。
4. **codegen**：`OP_CLOSURE` case（弹 0 压 1：`TOS_PRODUCE(); vstack++`）。

**验收判据**
- `jit_probes/probe_closure_nocap.leno`：热循环内建零捕获闭包并调用 ⇒ `capable=1`、`unsupported:61` 从直方图消失；
- 新用例 `assert/test_jit_op_closure.leno`：身份比较（I4）、闭包被存入数组后逐次调用、`LENO_NO_JIT=1` 对拍一致；
- 编译 0 warning。

**风险**：低。**回退**：scan 里删 case 即回到旧行为（callout 与 codegen 代码保留无害）。
**独立价值**：即使 R5 就此停下，这一步也把 `61:OP_CLOSURE` 从 `unknown` 变成 `capable`，并补上 R3 留下的长度缺口。

---

### P2 —— closure 通道 + U1/U2 + C1（by-upvalue 捕获）

**这是 ABI 阶段，也是文档 roadmap 里点名的「(a) func-JIT ABI 要加 closure 通道」。**

**改动点**
1. **ABI 第三参**：`jfn(Value* locals, Value* globals, ObjClosure* closure)`。
   - 入口 shim 已存在（`x86_64.c:894-910`），多搬一个寄存器即可；
   - **推荐形态**：prologue 把该参数**存进本帧的一个固定 scratch 合成槽**（而不是常驻某个通用寄存器）——理由：通用寄存器已被 locals/globals/scratch/pinned 占满，且存进本帧栈**天然嵌套安全**（满足 I7），不引入全局槽的保存-恢复问题；
   - **两条入口都要传**：`jit_try_hot_func_call` 手里就有 `closure`（`jit.h:143`）；`jit_try_hot_loop` 手里有 `frame` ⇒ `frame->closure`（`jit.h:116`）。
2. **U1/U2 codegen（纯指针链，无需 callout）**：
   ```
   closure → [ObjClosure.upvalues + slot*8] → Upvalue* → [Upvalue.location] → Value
   ```
   读走 `GET_UPVALUE`，写走 `SET_UPVALUE`（**注意语义：`OP_SET_UPVALUE` 是 `peek` 不弹栈**，`op_variables.inc:170-179`）。
   防御性守卫（不满足即 bailout）：`closure == NULL`、`slot >= upvalue_count`、`upvalue == NULL`。
   ⚠ **必须每次重新取 `location`**（I2）。`SET` 是否补写屏障**照抄 VM（当前 VM 没有）**，疑点记入 §7。
3. **C1 的 `OP_CLOSURE`**：`is_local=0` 时直接复制 `frame->closure->upvalues[index]`（越界则 bailout 交解释器报错）。
4. **I8 落地**：inline scan 的 `GET/SET_UPVALUE` **继续拒绝**（现状即拒），并在注释里写明理由（"当前闭包归属"在内联体里不成立）。

**验收判据**
- `jit_probes/probe_closure_upvalue.leno`：闭包体内热循环读写 upvalue ⇒ 闭包体进函数级 JIT（`FuncCompiled` 增长）、结果与 `LENO_NO_JIT=1` 逐位一致；
- I2 专项用例：闭包捕获 open 变量 → 外层返回 → 再调用，读到的值正确；
- I7 专项用例：闭包 A 调闭包 B、B 内读写自己的 upvalue；
- 既有全套 assert 无回退（ABI 改动影响面最大，必须全跑）。

**实现记录（P2a，2026-09-15）**：
- ABI：`JitLoopFn` 加第三参 `ObjClosure*`；5 个入口全部传值（循环入口传 `frame->closure`、
  解释器侧函数入口传 `closure`、三个 callout 快路径分别传 `closure` / `callee` /
  `callee`-或-NULL）。后端序言把它存进**本帧固定槽** `closure_disp`
  （`-8*(total_locals + max_vstack + 7)`，帧尺寸 +8）；SysV 的 shim 必须**先**搬
  arg3（RDX→R8）再覆盖 RDX，Win64 的 arg3 本来就在 R8。
- `GET/SET_UPVALUE` 走 callout（`jit_callout_upvalue_get/set`）：纯指针链 + 三级守卫，
  每次重新取 `location`（I2）。`SET` 的顺序是"先 TOS_SPILL 保住原始表示，再 PEEK 出来
  装箱"，避免 RAW→VALUE 转换污染内存栈里的裸 int48/裸 double。
- **`GET/SET_UPVALUE` 在函数级体与循环体里都放行**；**内联体仍拒绝**（I8）。
- 实测：`probe_closure_upvalue` → 循环进 JIT、`Bailouts: 0`、`u1=200000 / u2=400000`
  与 `LENO_NO_JIT=1` 一致；`assert/test_jit_closure_upvalue_rw.leno` 覆盖四种语义
  （closed / open / 两闭包共享同一 upvalue / 热循环里的函数级 SET）；全套 assert **305/0**。
- 仍未做：**C1（by-upvalue 捕获）**——留到 P2b。

---

### P3 —— C2 值捕获（收益主体：循环内建闭包捕获循环变量）

**为什么这是主体**：`semantic_upvalue.c:200` 把「循环体内的变量」一律标为值捕获 ⇒ **真实负载里循环建闭包的形态就是 C2**，而 C2 是最安全的一类（`location` 指向 upvalue 自身的 `closed`）。

**改动点**
- scan 放行 `n > 0` 且**所有**捕获都是 (`is_local=0`) 或 (`is_local=1 & is_value_capture=1`) 的 `OP_CLOSURE`；出现任一 C3 捕获即整条 `capable=0`（I6：宁可拒收，不靠 bailout）。
- callout：`jit_callout_make_closure_caps(func_val, n, Value* captured_values)` —— 内部按 V3 逐条 `new_upvalue`（I3 的字段顺序 + 写屏障）；
  - 值来源：`is_value_capture=1 & is_local=1` → 从 **scratch 槽**读（`EMIT_LOAD_LOCAL`）；
  - `is_local=0` → 从**当前 closure 的 upvalues** 读（P2 的链）；
- vstack 记账：`OP_CLOSURE` 为弹 0 压 1，不引入新的栈深风险（对照 J5 的教训：这里没有可变 `pop_bytes`）。

**验收判据**
- 语义金标准用例 `assert/test_jit_closure_loop_capture.leno`：`for` 循环建 N 个闭包存入数组，逐个调用 ⇒ **每轮独立值**（Leno 值捕获语义，而非全部等于最后一个值）；与 `LENO_NO_JIT=1` 逐位一致；
- 直方图：`61:CLOSURE` 在实际负载上转为 `capable`；
- 让出/GC 用例（I5）：热循环内建闭包 + 触发 GC 让出后结果不变。

**风险**：中低。**回退**：scan 收紧到只放行 C0/C1。

**实现记录（P3，2026-09-15）**：
- **scan**：放行条件从"零捕获或全 by-upvalue"放宽为 **`ref_local == 0`**（即 C0 / C1 / C2 都放行，只拒 C3）。
  新增一处 **`max_vstack` 峰值记账**：C2 的值必须压到 JIT 栈上才能交给 callout，
  压入/弹出虽在同一指令内平衡，但**峰值**会临时抬高 vstack ⇒ 若不计入 `max_vstack`，
  帧尺寸不够、压栈会踩进 tmp/co/closure 区（内存安全）。
- **codegen**：把描述表里 `is_local=1` 的局部槽按捕获下标**降序**用 `EMIT_LOAD_LOCAL` 压栈
  （降序 ⇒ 压完后 `vstack_top[0]` 正好是第 0 条本帧捕获，callout 顺序消费即可），
  把 RSP 存进 `tmp3_disp` 传作第四参；callout 返回后再 `add rsp, n_local*8`。
  被捕获槽若在 scan 里没被映射（`cur_local_map[slot] < 0`）⇒ **编译期放弃**（`return 0`），
  绝不发一条读错槽的指令。
- **callout**：`jit_callout_make_closure_caps(func_val, cur, desc, vstack_top)` 统一处理 C1 + C2；
  值捕获走内部小工具 `jit_new_closed_upvalue()`（复刻 `new_upvalue` + 写屏障 + `location=&closed`，
  I3 的字段顺序）；C3 条目在 callout 里**兜底拒绝**（不建半成品闭包）。
- **实测**：`probe_closure_value_capture` → `scan:ALLOW-C2`、`c2=1400000`、**0 bailout**；
  `assert/test_jit_closure_value_capture.leno` 两条金标准判据 ——
  ① **每轮独立取值**：`collect(1000)` 必须 = 20（若错误地共享一份 upvalue 会得 40，用例立刻失败）；
  ② **C1 + C2 混合捕获**：一个闭包同时捕获外层 upvalue 与循环内局部，`mixed(1000) = 599500`。
  全套 assert **307/0**。
- 仍未做：**C3（引用捕获本帧局部）**（P4，默认不做）；**内联体内的闭包/upvalue 一律仍拒绝**（I8）。

---

### P4 —— C3（引用捕获本帧 locals）：默认不做，需 P0 度量授权

**若 P0 显示 C3 有可观占比**，才进入本阶段；否则**永久拒绝并写进文档**（保持"不留没有证据的复杂度"）。

两条候选路线：

| 路线 | 做法 | 必须同时解决 |
|---|---|---|
| (a) **提升槽** | scan 发现本帧存在 C3 捕获时，把这些 slot 从 scratch 提升到一块 **JIT 托管的稳定 `Value` 数组**（随本帧机器码生命周期管理，登记给 GC / 或确保退出前关闭） | ① 关闭时机必须复刻 V5 的无条件关闭（含 `gc_write_barrier` + 摘链）；② 让出/bailout 边界：bailout 不写回 locals，而 open upvalue 已建 ⇒ 需与 I6 的"重跑"语义对齐；③ `vm_grow_frames` 的地址重映射覆盖不到 JIT 托管区 ⇒ 地址排序不变量（V4 的降序假设）要重新论证 |
| (b) **拒绝** | 维持现状 | 无 |

**Gate（进入条件）**：P0 形态表里 C3 出现次数 ≥ 阈值（建议按"是否出现在真实负载的热循环/热函数体内"判断，而非绝对条数）。

**Gate 实测数据（2026-09-15）**：`file_manager` + 其全部模块的静态普查给出 **C3 = 57 条捕获**
（占全部捕获条目的 **100%**，因为 C1/C2 均为 0）⇒ **C3 在真实代码里绝不是"少数派"**，
它是这个应用里**唯一**的捕获形态（回调闭包在循环外创建、捕获外层局部）。
**但**同一应用的**热代码**里闭包指令为 0 ⇒ 这 57 处全部落在冷路径（初始化 / 回调注册），
做成 P4 也**体现不出收益** ⇒ **维持"不做 P4"**。
同时把这条数据记为**触发条件**：一旦出现「热循环里创建/注册回调」的写法
（例如按列表项批量 `arr.add(func(){...})`），P4 的收益就有落点，应当优先做。

---

### P5 —— 收口

- `LENO_JIT_DUMP` 反汇编补 `OP_CLOSURE` / `GET/SET_UPVALUE` 的可读输出（排查必备）。
- 更新 `JIT实现与调试记录.md`：新增本轮小节、roadmap R5 行改为「完成（§x.xx）」、§12 补「bailout 重跑会把建闭包这一堆副作用一并重放」。
- `jit_probes/README.md` 登记新探针与基线判据。

---

## 6. 需要拍板的决策

| # | 决策点 | 建议 |
|---|---|---|
| D1 | ABI 第三参 + 存帧内合成槽（推荐）vs 全局 closure 槽 | **推荐前者**：嵌套安全（I7）不需额外保存-恢复；后者省一次搬迁但引入 I7 的实现负担 |
| D2 | P3 是否在线 | **做**：它是真实负载的绝大多数形态，且是安全形态 |
| D3 | C3 是否永久拒绝 | **先拒绝**，P0 度量后再定（P4 的 gate） |
| D4 | 是否顺带修 §7 的 3 个 VM 侧疑点 | **不夹带**，单独立项（避免把"JIT 支持"和"VM 修正"混在一个提交里，出问题无法二分） |
| D5 | U 访问的防御性守卫（NULL 检查）是否保留 | **保留**：代价是一条比较指令，换来"宁可 bailout 交解释器报错，不做 NULL 解引用" |

---

## 7. 顺带发现的 VM 侧疑点（本轮不修，记录备查）

1. **`has_captures` 门控语义错配**：`has_captures` 的语义是「本帧是否捕获了**父层**」（`vm_call.inc:148`：`closure->upvalue_count > 0`），但 `OP_CLOSE_UPVALUE`（`op_variables.inc:183`）与 try-return 路径（`op_exception.inc:144`）把它当作「本帧的 locals 是否**被**别人捕获」来门控关闭 ⇒ 这两条路径可能漏关闭。`OP_RETURN`/`OP_TAIL_CALL` 是正确写法（无条件关闭）。
   ⚠ **JIT 侧不要沿用这个门控**（JIT 不关闭任何东西，见 I1，天然免疫）。
2. **`OP_SET_UPVALUE` 无写屏障**（`op_variables.inc:170-179`）：close 时（`vm_upvalue.inc:121`）有屏障、`OP_CLOSURE` 值捕获（`op_call.inc:580/594`）有屏障，唯独 set 没有。老年代 upvalue 写入年轻代对象时存在跨代漏标的理论隐患。**本轮 JIT 照抄现状**（bug-for-bug 对齐），修正另立项。
3. **异常展开路径不 `close_upvalues`**：`op_exception.inc:223-229`、`vm_exception.inc:379-385`、`op_coroutine.inc:94-98`、`src/vm/vm.c:805-809` 这些"释放中间帧"的循环只 `free` 动态 locals，未关闭其上的 open upvalue ⇒ 可能悬空/脏值。与 JIT 无直接关系，但任何"整帧消失但不走 `OP_RETURN`"的新路径（包括未来若给 JIT 加 open upvalue 支持）都会继承这个坑 —— 这是 I1 的另一条保险。
4. **方法闭包的 upvalue 填充**（P2 前置，**已核实 2026-09-15**）：
   - **语言允许** struct 方法捕获外层变量。实测（`build/` 下临时程序，已删）：
     ```leno
     func make(int base): int {
         struct S { int x = 1; func add(): int { return x + base } }
         var s = new S()
         return s.add()
     }
     ```
     反汇编显示 `S.add` 的方法体是 `OP_GET_FIELD_FAST slot=0 field=0` + **`OP_GET_UPVALUE 0`**
     ⇒ 该方法 `func->upvalue_count == 1`。
   - 而创建路径（`op_struct.inc:124-134` 明确"需要 upvalue 的方法，标记为 NULL，在运行时
     动态创建"；`op_property.inc:111-121`、`op_struct.inc:949-955` / `1114-1120`、
     `jit_callout_get_method`）新建时把 `upvalues[0..n)` **一律置 NULL**。
   - **该形态在解释器侧当前另有问题**：上述程序运行到 `s.add()` 时报
     「尝试在非 struct 类型上调用方法 'add'」（属 VM 侧另一处缺陷，本轮未追）。
   - **结论（已落地）**：JIT 侧**不依赖**"方法闭包一定无捕获"这一假设，`GET/SET_UPVALUE`
     一律做三级守卫（`closure == NULL` / `slot >= upvalue_count` / `upvalues[slot] == NULL`）
     —— 任一不满足即 bailout 交解释器。JIT 在这里比 VM 更保守是正确的：宁可回退，
     也不要 NULL 解引用（P2a 已实现，见 §P2 的实现记录）。

---

## 8. 探针与用例清单（待建）

| 文件 | 形态 | 判据 |
|---|---|---|
| `jit_probes/probe_closure_nocap.leno` | C0：热循环内建零捕获闭包并调用 | `capable=1`；`unsupported:61` 消失；与 `LENO_NO_JIT=1` 一致 |
| `jit_probes/probe_closure_upvalue.leno` | U1/U2：闭包体内热循环读+写 upvalue | 闭包体进函数级 JIT；值逐位一致 |
| `jit_probes/probe_closure_value_capture.leno` | C2：循环内建闭包捕获循环变量 | 每轮独立值；`capable=1` |
| `jit_probes/probe_closure_byupvalue.leno` | C1：嵌套闭包捕获外层 upvalue | 指针复用正确；与解释器一致 |
| `jit_probes/probe_closure_c3_reject.leno` | C3：非循环局部被引用捕获 | **必须** `unsupported/unknown` 拒收，且无 bailout（I6） |
| `assert/test_jit_op_closure.leno` | P1 验收 | 身份语义（I4）+ 存入容器后逐次调用 |
| `assert/test_jit_closure_loop_capture.leno` | P3 验收（语义金标准） | 每轮独立值，逐位一致 |
| `assert/test_jit_closure_upvalue_rw.leno` | P2 验收 | 读写 upvalue 交叉验证；含 I2/I7 两个专项 |

---

## 附：为什么"先设计不变量"是对的（本次取证的净收获）

- 原本 roadmap 的表述是两条前置（ABI closure 通道、循环 JIT locals 布局）。取证后发现真正的**第一性问题只有一个**：JIT 没有稳定 `Value*`；而它可以用 I1 从语义上绕开，而不必解决"布局"问题。
- 剩下三件事（`opcode_size` 的 chunk 化、ABI 第三参、GC 可见性）都有现成先例可抄：`switch_lookup` 的变长解析（§8.61）、`jit_func_locals_pool` 的深度索引（J2）、`jit_callout_get_method` 的"callout 里 gc_alloc 闭包"（`jit_callout.c:1089-1099`）。
- 形态表（§3）把工作量从"4 条 opcode"重估为"1 个长度解析 + 1 个 ABI 参数 + 3 个 callout + 1 组指针链 codegen"，**且其中 P1 有独立价值、P4 可以永久不做**。
