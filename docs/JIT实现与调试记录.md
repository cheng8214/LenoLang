# Leno JIT 编译器实现与调试记录

> 文档记录 Leno VM JIT 编译器的架构设计、已支持字节码清单、调试方法、踩过的坑及解决方案。
>
> 代码位置（可移植层 / x86_64 后端分工）：
>
> - `src/jit/jit.c`、`jit.h`、`jit_scan.c`、`jit_callout.c`、`jit_mem.h` —— 与目标无关
> - `src/jit/backend/x86_64.c` —— x86_64 codegen 骨架（序言/宏/switch/收尾/补丁）
> - `src/jit/backend/x86_64_emit.h` —— 指令编码器（`emit_*`）与寄存器编号
> - `src/jit/backend/x86_inc/ops_*.inc` —— 按 opcode 家族拆分的 `switch(op)` case，
>   被 `#include` 进 `compile_loop()`，因此可直接使用函数内局部宏（`TOS_*` / `EMIT_*`）
> - 新增/修改某个 opcode 的 codegen：改对应的 `ops_*.inc`；新增通用宏放 `x86_64.c`

## 目录

- [1. 架构概述](#1-架构概述)

- [2. 寄存器约定与内联规则](#2-寄存器约定与内联规则)
  - [2.3 写内联操作码的规则（必读）](#23-写内联操作码的规则必读)
  - [2.4 反面案例：内联用 RSI → Windows 上段错误](#24-反面案例内联用-rsi--windows-上段错误2026-09-12)
  - [2.5 跨平台结论](#25-跨平台结论)
  - [2.6 ffi 定宽内存读写内联（表驱动）](#26-ffi-定宽内存读写内联表驱动2026-09-12)

- [3. NaN-boxing 与 JIT 虚拟栈](#3-nan-boxing-与-jit-虚拟栈)

- [4. 已支持字节码清单](#4-已支持字节码清单)

- [5. 不支持的字节码（导致 scan 拒绝）](#5-不支持的字节码导致-scan-拒绝)

- [6. Callout 机制](#6-callout-机制)

- [7. 调试方法](#7-调试方法)

- [8. 踩坑记录与解决方案](#8-踩坑记录与解决方案)

- [9. 性能数据](#9-性能数据)

- [10. 架构瓶颈分析](#10-架构瓶颈分析)

- [11. 优化路线图](#11-优化路线图)

- [12. 当前未解决问题](#12-当前未解决问题)

***

## 1. 架构概述

Leno JIT 是一个**模板 JIT（template JIT）**，工作在栈式 VM 的热循环上，不修改字节码格式和 VM 主分发循环。

### 工作流程

**两条触发路径**（2026-09-13 起）：

```
A. 热循环（原有）
VM 执行 OP_LOOP / OP_FOR_LOOP
  → jit_try_hot_loop() 被调用
  → hit_count++ → 达到阈值 (50次) → 尝试编译
  → scan_loop_body(): 扫描循环体，检查所有 opcode 是否可 JIT
  → compile_loop(): 生成 x86_64 机器码
  → 缓存到 jit_state.cache[]
  → 下次命中同循环 → 直接执行 JIT 代码
  → 执行成功返回 0 / 类型溢出 bail out 返回 1

B. 热函数（§8.25，解释器调用链）
VM 执行 OP_CALL / OP_CALL_GLOBAL_FUNC[_TYPED]
  → jit_try_hot_func_call() → hit_count++ → 达到阈值 (50次)
  → jit_compile_function(): 整函数（要求无循环回边/无 try/单返回值）编译为
    fn(locals, globals)，缓存到 jit_func_cache[]
  → 命中后直接执行机器码，按 call() 的栈约定折叠栈并写回返回值
  → 函数体内对被调函数的调用走 callout 薄桥（JIT 链），非 0 返回则交回解释器
```

### JIT 函数签名

```c
int (*JitLoopFn)(Value* locals, Value* globals);
// 返回 0 = 成功，1 = bailout（回退 VM 重跑本次迭代）
//      2 = frame-dead（callout 抛异常，控制流已转移到宿主帧 catch_ip，需写回 locals）
//      3 = frame-dead（同上，不写回）
```

### 关键参数 (`jit.h`)

| 参数                  | 值   | 说明                      |
| ------------------- | --- | ----------------------- |
| `JIT_HOT_THRESHOLD` | 50  | 循环执行 50 次后触发编译          |
| `JIT_CACHE_SIZE`    | 256 | 缓存哈希表大小（2 的幂）           |
| `JIT_MAX_LOCALS`    | 64  | 最多 type-guard 的 local 数（含内联 callee） |
| `JIT_MAX_LOOP_OPS`  | 256 | 循环体最大 opcode 数          |
| `JIT_MAX_VSTACK`    | 64  | 虚拟栈最大深度                 |
| `JIT_BAILOUT_LIMIT` | 3   | bailout 3 次后放弃此循环       |
| `JIT_FUNC_HOT_THRESHOLD` | 50 | **解释器侧**函数调用 50 次后编译整函数（§8.25；callout 侧仍第一次就编译） |
| `JIT_FUNC_CACHE_SIZE` | 256 | 函数级 JIT 缓存槽数（direct-mapped，2 的幂） |
| `JIT_FUNC_MAX_DEPTH` | 64 | JIT 函数链最大嵌套深度（C 栈保护，超限回退解释器） |

***

## 2. 寄存器约定与内联规则

JIT 后端目前只有 **x86-64**（`src/jit/backend/x86_64.c`），但它要同时跑在两种 ABI 上：

| 平台 | ABI | callout 的整型/指针参数寄存器 | 栈 |
| --- | --- | --- | --- |
| Windows x64 | Win64 | `RCX, RDX, R8, R9` | 需要 32B shadow space（`EMIT_CALLOUT_ALLOC`） |
| Linux / macOS x86-64 | SysV | `RDI, RSI, RDX, RCX`（第 5 个起入栈） | 无 shadow，仅需 16B 对齐 |

后端里按 ABI 分叉的只有**四处**：`JIT_ARG1..5` 的映射、callout 栈空间
（`EMIT_CALLOUT_ALLOC`）、**JIT 函数入口 shim**（见下）、可执行内存分配（`jit_mem.h`：
Windows `VirtualAlloc` / POSIX `mmap`）。**内联代码生成不许再引入新的 ABI 依赖**（见 §2.3）。
`build.sh` 按 `uname -m` 选后端：x86_64 → 本文件，arm64/aarch64 → `backend/arm64.c`
（已预留、**未实现**，缺文件时构建直接报错）。

JIT 函数入口（`compile_loop` 序言）：约定参数为 `RCX = Value* locals`、`RDX = Value* globals`，
但这是 **Win64 的寄存器**。JIT 函数是用**普通 C 调用**进的（`jit.c` 的 `entry->fn(locals, globals)`、
`jit_callout.c` 薄桥里的 `jfn(flocals, vm->globals)`），所以 SysV 下实参落在 `RDI/RSI`。序言
在 `mov rbp, rsp` 之后立刻补了一段 shim（`#ifndef _WIN32`）：

```c
emit_mov_rr(cb, JIT_RCX, JIT_RDI);   /* RCX = locals  (arg1) */
emit_mov_rr(cb, JIT_RDX, JIT_RSI);   /* RDX = globals (arg2) */
```

* 位置安全：此处只压过 `RBP/RBX/R12-R14`，`RDI/RSI` 尚未被动过；`RCX=locals` 之后所有
  `[rcx + slot*8]`、`RDX→R9`（globals）等既有约定**一行都不用改**。
* 与 callout 的分工：callout 参数走 `JIT_ARG1..5`（已 ABI 感知），**入口是唯一不走那些宏
  的取参路径** —— 之前漏的就是这一处，症状是 Linux 上一进 JIT 就段错误（读到垃圾 locals）。
* Win64 分支不受影响（`#ifndef _WIN32` 包住），热循环与函数级 JIT 共用同一段序言，
  所以一处即覆盖两种模式。

### 2.1 寄存器角色

| 寄存器 | 用途 | 保留方式 |
| --- | --- | --- |
| RCX | 入口 = locals 指针（第一参数，固定；SysV 下由 shim 从 `RDI` 搬入） | 调用者保存 |
| RDX | 入口 = globals 指针（prologue 复制到 R9；SysV 下由 shim 从 `RSI` 搬入） | - |
| R9  | globals 指针（运行期间固定）；`EMIT_BAILOUT_SITE_WRITE` 临时借它（自带 push/pop） | 调用者保存 |
| R10 | `PAYLOAD_MASK = 0x0000FFFFFFFFFFFF` | prologue 加载；callout 后由 `EMIT_CALLOUT_END` 重载 |
| R11 | `INT_TAG = 0xFFFB000000000000` | 同上 |
| RBX | 类型位图：bit i=1 表示 local i 是 float/obj（`BT RBX, imm8`） | callee-saved，prologue xor 清零 |
| RBP | 帧指针 = scratch/spill 区基址 | callee-saved |
| RSP | 虚拟栈顶（JIT 的 push/pop 就是参数与中间值） | 运行期间管理 |
| RAX | 通用 scratch / TOS 常驻寄存器 | volatile |
| RDX | 通用 scratch（入口时是 globals，prologue 后释放） | volatile |
| R8  | 通用 scratch（int48 检测、tag 检查……） | volatile |
| **RSI / RDI** | **❌ 内联禁用**（见 §2.3 第 2 条）；SysV 下序言入口处承载 `arg2/arg1`，只被 shim 读一次 | SysV: volatile / **Win64: callee-saved** |
| R12 | callout: 保存 RSP | callee-saved |
| R13 | callout: 保存 RCX | callee-saved |
| R14 | callout: 保存 R9（并借它搬 globals 指针） | callee-saved |
| R15 | 后端未定义（`x86_64_emit.h` 只到 `JIT_R14`） | — |

### 2.2 Epilogue（不能使用 LEAVE）

由于在 RBP 之后压入了 R12/R13/R14/RBX，无法用 `LEAVE` 指令，必须手动恢复：

```asm
mov rsp, rbp    ; 恢复栈指针
pop r14
pop r13
pop r12
pop rbx
pop rbp
ret
```

### 2.3 写内联操作码的规则（必读）

1. **可用的临时寄存器只有 `RAX / RCX / RDX / R8`** —— 这四个在两种 ABI 下都是 volatile。
   需要第 5 个及其以上的临时值时，用 `EMIT_STORE_TMP(tmp1_disp, reg)` /
   `EMIT_LOAD_TMP(reg, tmp1_disp)` 的 frame spill 槽，**不要**去找 RSI/RDI。
2. **`RSI` / `RDI` 在 Win64 下是 callee-saved，而 JIT 序言只保存了 `RBP/RBX/R12/R13/R14`**：
   内联里写它们就会破坏 `jit_try_hot_loop` / `vm_run` 等调用方的状态（§2.4 是真实事故）。
   SysV 下它们确实是 volatile（写了没事），但**同一份代码必须两端都对**，所以统一禁用；
   它们只允许出现在 callout 参数装载（`JIT_ARG1..5`）。
3. 参数与结果走 JIT 虚拟栈约定：`TOS_SPILL()` → `emit_pop_reg(...)`（**倒序**，栈顶是最后一个实参）
   → 结果 `TOS_PRODUCE()` → `vstack -= (arg_count - 1)`。
4. 任何"做不到/不确定"的情形一律写成 **bailout**，不要猜：
   `EMIT_BAILOUT_SITE_NONOVF(bc_off); int p = emit_jcc(cb, <cc>); patch_add(ctx, p, -1, 0);`
   走这条路时**不要**再 `TOS_PRODUCE()`。语义要逐条对齐解释器，包括边界判断的**无符号回绕**、
   `null`/`bool` 的具体数值、以及报错文本（bailout 后由解释器抛原样错误）。
5. 改 opcode 支持范围时**三处必须同步**，漏一处就是"整个循环静默不可 JIT"：
   `jit_scan.c` 的 `opcode_size()`、`scan_callee_for_inline()` 与 `scan_loop_body()` 的
   vstack 栈效应表、以及 backend 的实现。
6. 不要动 RBP/RSP 的约定：bailout 出口统一走 `EMIT_EPILOGUE()`（用 RBP 恢复 RSP），
   所以 bailout 之前 RSP 的临时变化（比如已 pop 掉实参）是安全的。

### 2.4 反面案例：内联用 RSI → Windows 上段错误（2026-09-12）

给 `ffi.read_int` / `ffi.write_int` 写内联时，第一版用 `RSI` 存 `size`、`RDI` 存 `offset+4`
（觉得都是 scratch）。结果：正常路径全部正确，**只有 bailout 之后**崩：

```
Thread 1 received signal SIGSEGV
0x00007ff76d728a9b in jit_try_hot_loop ()
=> addl $0x1,0x4008(%rsi)        ; RSI = 0x10 —— 正是刚读出的 size=16
```

原因：Win64 把 RSI/RDI 归为 callee-saved，而 JIT 序言没保存它们，于是内联把 `RSI=0x10`
留给了调用方，编译器用 RSI 缓存的 `&jit_state`（+0x4008）就飞了。SysV 下 RSI/RDI 是
volatile，同一份代码在 Linux 上**不会**复现 —— 典型的"Linux 能跑、Windows 崩"。
修法：只用 `RAX/RCX/RDX/R8`，边界比较的第 4 个临时值改用 frame spill 槽。

### 2.5 跨平台结论

* **入口同样是 ABI 相关**：Win64 传参在 `RCX/RDX`，SysV 在 `RDI/RSI`；序言用一段
  `#ifndef _WIN32` 的 shim 归一化（见 §2 开头），因此「内联只用 `RAX/RCX/RDX/R8`」
  这条规则在 SysV 下还多一层含义：`RDI/RSI` 在 shim 执行前**是活跃的入口参数**，
  内联更不该碰。
* 内联代码只用 `RAX/RCX/RDX/R8` + 相对 RBP 寻址，**Win64 与 SysV 通用**（打包出的
  `ffi.read_int` 内联在两种 ABI 下行为一致）。
* 换**架构**才需要重写：arm64 后端未实现；但 tag 检查、边界规则、bailout 协议、虚拟栈
  约定与架构无关，可以照搬，只有指令编码与寄存器编号要换。
* 校验基线：改动内联后至少跑 `assert/run_tests.leno`（两种模式）、
  `LENO_NO_JIT=1` 差分、以及带 `bailout` 的用例（正常路径正确不代表 bailout 路径正确 ——
  本节的段错误就是这么漏出来的）。
* **首次在 Linux 上跑必须额外确认**：
  1. `bash build.sh` 能过（`jit_mem.h` 走 POSIX `mmap`；`LENO_NO_JIT=1` 是纯解释器兜底）；
  2. 热循环与函数级 JIT 都真的被执行到（否则 shim 没被覆盖到就等于没测）；
  3. callout 相关的 5 参数用例（SysV 第 5 个参数走 `R8`，Windows 走 `[RSP+32]`）；
  4. 若 `mmap(PROT_READ|PROT_WRITE|PROT_EXEC)` 被 SELinux/PaX 之类拦掉，`jit_compile`
     会失败并**退回解释器**（不崩，只是没加速）——那是环境问题，不是 ABI 问题。

### 2.6 ffi 定宽内存读写内联（表驱动，2026-09-12）

**背景**：像素直写内层是 `ffi.read_byte` ×4 + `ffi.write_byte`/`ffi.write_int` ×1，
每像素十几次 ffi 调用。这些方法此前都走 `OP_MODULE_CALL` 的通用 callout
（实测 ~20ns/次：逐个装箱成 Value 数组 + native 调用 + 异常检查），而它们真正做的
只是一次 1/2/4 字节 memcpy。把第一批 `read_int`/`write_int` 的内联模板**表驱动化**后，
一并铺开到全部定宽读写。

**实现**：`src/jit/backend/x86_64.c` 里一张常量表 `ffi_inline_specs[]`，
`ops_return.inc` 的 `OP_MODULE_CALL` 分支命中后直接生成 load/store
（`ffi_inline_lookup()` 线性 strcmp）。加一个方法 = **加一行表**，不必把 135 行的
前置检查复制 N 份。

| 表字段 | 含义 |
| --- | --- |
| `name` | ffi 方法名 |
| `size` | 访存字节数 1/2/4，同时是 CHECK_BOUNDS 的 `access_size` |
| `sign_ext` | 读路径是否符号扩展（写路径忽略）；`read_byte`/`read_uint16`/`read_uint` 为 0 |
| `is_write` | 0 = 读（2 实参 → 结果 raw int48）；1 = 写（3 实参 → 结果 `NULL_VAL`） |

**已内联（12 个）**：

`read_byte`、`read_int8`、`read_int16`、`read_uint16`、`read_int`、`read_uint`、
`write_byte`、`write_int8`、`write_int16`、`write_uint16`、`write_int`、`write_uint`

前置检查对 12 个条目完全同构（沿用 §2.4 修好后的模板），任一不过就 bailout：
`int48 offset` → NaN-boxed 对象 → `OBJ_FFI_POINTER` → `!NULL/!freed` → owned 边界
（`(size_t)off + size > ptr->size`，**负 offset 无符号回绕必然越界**，与解释器一致）。
只有三处由表参数化：**边界检查宽度、load/store 宽度与扩展方式、写截断宽度**。
bailout 后由解释器抛原样错误（"空指针引用或指针已释放" / "内存访问越界"），
语义与报错文本逐字一致。

**有意不内联**（不要顺手往表里加）：

| 方法 | 原因 |
| --- | --- |
| `read_int64` / `write_int64` / `read_uint64` / `write_uint64` | 值域越过 Value 的 int48：uint64 超 `INT32_MAX` 返回 bigint 对象，int64 超 int48 也要转对象，机器码里无法复刻堆分配 |
| `read_float` / `read_double` / `write_float` / `write_double` | 要构造/拆 NaN-boxed float Value，不是 raw int48 |
| `read_ptr` / `read_at` / `read_string` / `offset` | 返回对象，要分配 `val_obj` |
| `read_bool` | 返回 bool Value，非 raw int48 |
| `copy4` | 双指针、两套对象检查，无逐像素调用点 |

**验证基线**：
- `assert/test_ffi_inline_widths.leno`：宽度/符号扩展边界（`0x80`/`0x8000`）、窄写保留高位、
  bigint 写回退、越界/空指针/已释放/负 offset 的 bailout 报错，JIT 与 `LENO_NO_JIT`
  两种模式均通过；JIT 统计中 `test_errors` 两个热循环各 bailout 3 次，验证回退路径。
- **机器码回归**：对 `read_int`/`write_int` 热循环 dump，屏蔽 `mov r64, imm64`
  （bailout site / 运行时地址跨进程不同）后**残差 0 字节**（base=1719 / new=1719），
  确认重构没有改动已有内联的机器码。
- 性能数字见 §9。

***

## 3. NaN-boxing 与 JIT 虚拟栈

### NaN-boxing 方案

```
QNAN        = 0x7FF8000000000000
SIGN_BIT    = 0x8000000000000000
TAG_MASK    = 0x0007000000000000
TAG_INT     = 0x0003000000000000   → NaN-boxed: 0xFFFB000000000000 (QNAN|SIGN_BIT|TAG_INT)
TAG_FALSE   = 0x0001000000000000   → NaN-boxed: 0xFFF9000000000000
TAG_TRUE    = 0x0002000000000000   → NaN-boxed: 0xFFFA000000000000
TAG_OBJ     = 0x0004000000000000   → NaN-boxed: 0xFFFC000000000000
NULL_VAL    = 0xFFF8000000000000   (QNAN|SIGN_BIT|TAG_NULL)
PAYLOAD_MASK= 0x0000FFFFFFFFFFFF
```

`Value` 类型为 `typedef uint64_t`（`leno_value.h`）。

### JIT 虚拟栈存储格式

JIT 虚拟栈不存 NaN-boxed Value，而是存 **raw int64\_t**：

| 值类型           | 虚拟栈存储格式                 | 检测方法                                                     |
| ------------- | ----------------------- | -------------------------------------------------------- |
| int           | int48 raw（符号扩展到 int64）  | `sar r8,47; inc r8; cmp r8,1; ja .not_int` — 结果 ≤1 是 int |
| float         | 原始 IEEE 754 double bits | 非 int48 且 < `0xFFF8000000000000`（无符号）                    |
| null/bool/obj | 原始 NaN-boxed bits       | 非 int48 且 ≥ `0xFFF8000000000000`（高 13 位全 1）               |

**「非 int48」不等于「float」**：负的裸 double（如 `0xBFEF...`）小于 `0xFFF8...`，所以
裸 double 与 NaN-boxed 的判别是 `cmp v, 0xFFF8000000000000; jae .tagged`。只有 NaN
位模式的 double 会与 NaN-boxing 撞车，而 `val_float()` 已把这种值归一化为 QNAN。

### int48 检测惯用法

```asm
mov r8, rax      ; 复制值
sar r8, 47       ; 算术右移 47 位
inc r8           ; 正确 int48: 0→1 或 -1→0，都 ≤1
cmp r8, 1
ja  .bailout     ; >1 表示溢出 int48 范围 [-2^47, 2^47-1]
```

### 裸数值规范化宏 `EMIT_NUM_TO_XMM`

通用算术/比较的 float 慢路径需要「把任一操作数变成 XMM 里的 double」，由
`backend/x86_64.c` 的 `EMIT_NUM_TO_XMM(dst_xmm, src_reg, scratch, tagged_var)` 完成三段分派：

```asm
mov  scratch, src          ; int48 检测
sar  scratch, 47
inc  scratch
cmp  scratch, 1
ja   .not_int48
cvtsi2sd dst, src          ; int48 → double
jmp  .done
.not_int48:
mov  scratch, 0xFFF8000000000000
cmp  src, scratch
jae  <tagged>              ; NaN-boxed（对象/null/bool）→ 调用方决定 concat 还是 bailout
movq dst, src              ; 裸 double 位模式直通
.done:
```

该方法保证「int48 与 float 混合」的表达式（如 `x + dx*invScale`）不再回退解释器。

### Prologue：locals 加载与类型守卫

```
for each local i:
  load rax = [rcx + slot*8]     ; 加载 NaN-boxed Value
  mov r8, rax; shr r8, 48       ; 取 top 16 bits
  cmp r8, 0xFFFB                ; INT_TAG >> 48
  je .is_int                    ; 是 int → 提取 int48
  ; 非 int 路径（float/obj/null/bool）：
  ;   存 raw NaN-boxed bits + BTS RBX, i 标记
  .is_int:
  and rax, R10 (PAYLOAD_MASK)
  shl rax, 16; sar rax, 16      ; 符号扩展 int48 → int64
  mov [rbp + scratch_disp(i)], rax
```

### Epilogue：locals 写回（类型感知）

```
for each local i:
  BT RBX, i           ; 测试 bit i
  jc .float_wb        ; bit=1 → float/obj 路径
  ; int 路径：重新编码为 NaN-boxed int
  load rax = [rbp + disp]
  and rax, R10 (PAYLOAD_MASK)
  or rax, R11 (INT_TAG)
  mov [rcx + slot*8], rax
  jmp .next
  .float_wb:
  ; float/obj 路径：直接存 raw bits
  load rax = [rbp + disp]
  mov [rcx + slot*8], rax
  .next:
```

***

## 4. 已支持字节码清单

### 栈操作

| Opcode | 枚举名      | 字节数 | 说明                            |
| ------ | -------- | --- | ----------------------------- |
| 4      | OP\_ZERO | 1   | 压入 0（`xor rax,rax; push rax`） |
| 5      | OP\_ONE  | 1   | 压入 1                          |
| 6      | OP\_POP  | 1   | 弹出（`add rsp,8`）               |
| 7      | OP\_DUP  | 1   | 复制栈顶                          |

### 局部变量

| Opcode | 枚举名                   | 字节数 | 说明                                                 |
| ------ | --------------------- | --- | -------------------------------------------------- |
| 8      | OP\_GET\_LOCAL        | 3   | 加载 local 到虚拟栈                                      |
| 9      | OP\_SET\_LOCAL        | 3   | peek 栈顶写入 local（不弹）                                |
| 10     | OP\_SET\_LOCAL\_POP   | 3   | 弹出写入 local                                         |
| 11     | OP\_MOVE\_LOCAL       | 5   | local 间复制（压栈）                                      |
| —      | OP\_MOVE\_LOCAL\_POP  | 5   | local 间复制（不压栈）                                     |
| —      | OP\_SET\_LOCAL\_CONST | 5   | 常量直接写入 local；非 int/float 常量存 raw bits + BTS RBX 标记 |

### 常量

| Opcode | 枚举名       | 字节数 | 说明                                                             |
| ------ | --------- | --- | -------------------------------------------------------------- |
| 0      | OP\_CONST | 3   | 压入常量；int→int48 raw，float→raw double bits，其他→raw NaN-boxed bits |

### 整数算术

| Opcode | 枚举名               | 字节数 | 说明                                      |
| ------ | ----------------- | --- | --------------------------------------- |
| —      | OP\_ADD\_INT      | 1   | `add rax, rdx` + int48 检测               |
| —      | OP\_SUB\_INT      | 1   | `sub rax, rdx` + int48 检测               |
| —      | OP\_MUL\_INT      | 1   | `imul rax, rdx` + int64 溢出检测 + int48 检测 |
| —      | OP\_MOD\_INT      | 1   | `cqo; idiv r8` 取余数（除数为 0 先 bailout，见 §8.23）      |
| —      | OP\_NEG\_INT      | 1   | `neg rax` + int48 检测                    |
| —      | OP\_ADD\_INT\_IMM | 2   | `add rax, imm8` + int48 检测              |
| —      | OP\_SUB\_INT\_IMM | 2   | `sub rax, imm8` + int48 检测              |
| —      | OP\_MUL\_INT\_IMM | 2   | `imul rax, imm` + 溢出检测                  |

### 整数除法（int / int → **int48**）

| Opcode | 枚举名          | 字节数 | 说明                                                      |
| ------ | ------------ | --- | ------------------------------------------------------- |
| —      | OP\_DIV\_INT | 1   | `cqo; idiv r8` → 商（RAX）作为 **int48** 压栈；除数为 0 / 商超 int48 先 bailout |

> 语义以解释器为准：`vm/vminc/op_type_specialized.inc:119` 压的是 `val_int(a / b)`
> —— **整除、向零截断、结果是 int**。任一侧是 float 时编译器发的是 `OP_DIV_FLOAT`。
> 本行原写作「返回 float」是错的（旧 codegen 真把它转成了 double），
> 该错误在 2026-09-12 修掉，见 §8.23。

### 位运算

| Opcode | 枚举名        | 字节数 | 说明                            |
| ------ | ---------- | --- | ----------------------------- |
| —      | OP\_BITAND | 1   | `and rax, rdx` + int48 检测     |
| —      | OP\_BITOR  | 1   | `or rax, rdx` + int48 检测      |
| —      | OP\_BITXOR | 1   | `xor rax, rdx` + int48 检测     |
| —      | OP\_BITNOT | 1   | `not rax`（`F7 /2`） + int48 检测 |

### 类型转换

| Opcode | 枚举名             | 字节数 | 说明                              |
| ------ | --------------- | --- | ------------------------------- |
| —      | OP\_CAST\_INT   | 1   | no-op（JIT 上下文中值已是 int）          |
| —      | OP\_CAST\_FLOAT | 1   | 运行时类型分派：int→`cvtsi2sd`，float→直通 |

### 浮点算术

| Opcode | 枚举名            | 字节数 | 说明                      |
| ------ | -------------- | --- | ----------------------- |
| —      | OP\_ADD\_FLOAT | 1   | SSE2 `ADDSD`（`0xF2` 前缀） |
| —      | OP\_SUB\_FLOAT | 1   | SSE2 `SUBSD`            |
| —      | OP\_MUL\_FLOAT | 1   | SSE2 `MULSD`            |
| —      | OP\_DIV\_FLOAT | 1   | SSE2 `DIVSD`            |
| —      | OP\_NEG\_FLOAT | 1   | `XORPD` 翻转符号位           |

### 浮点比较

| Opcode | 枚举名           | 字节数 | 说明                        |
| ------ | ------------- | --- | ------------------------- |
| —      | OP\_EQ\_FLOAT | 1   | `UCOMISD` + `SETE`        |
| —      | OP\_LT\_FLOAT | 1   | `UCOMISD` + `SETB`（无符号小于） |
| —      | OP\_GT\_FLOAT | 1   | `UCOMISD` + `SETA`（无符号大于） |
| —      | OP\_LE\_FLOAT | 1   | `UCOMISD` + `SETBE`       |
| —      | OP\_GE\_FLOAT | 1   | `UCOMISD` + `SETAE`       |

### 整数比较

| Opcode | 枚举名              | 字节数 | 说明                      |
| ------ | ---------------- | --- | ----------------------- |
| —      | OP\_EQ\_INT      | 1   | `cmp rax, rdx; sete al` |
| —      | OP\_LT\_INT      | 1   | `cmp; setl`（有符号）        |
| —      | OP\_GT\_INT      | 1   | `cmp; setg`             |
| —      | OP\_LE\_INT      | 1   | `cmp; setle`            |
| —      | OP\_GE\_INT      | 1   | `cmp; setge`            |
| —      | OP\_LT\_INT\_IMM | 2   | 与 imm8 比较               |
| —      | OP\_GT\_INT\_IMM | 2   | <br />                  |
| —      | OP\_LE\_INT\_IMM | 2   | <br />                  |
| —      | OP\_GE\_INT\_IMM | 2   | <br />                  |
| —      | OP\_EQ\_INT\_IMM | 2   | <br />                  |

### 通用算术与比较（int/float 混合，语义对齐解释器）

编译器在操作数类型不可静态确定时（如 `<float 变量> + <native 调用返回值>`、
`<int 循环变量> + <float>`）会发这些「通用」opcode。JIT 采用三段式：

```
① int 快路径：两边都是 int48 → 整数运算
② float 快路径：两边都不是 NaN-boxed（即都是裸数值）→ SSE2 运算
   （int48 操作数经 EMIT_NUM_TO_XMM 的 CVTSI2SD 提升；结果按 val_float 保持 float）
③ 回退：任一是 NaN-boxed（字符串/对象/null/bool）→ concat callout 或 bailout
```

| Opcode | 枚举名 | 字节数 | 说明 |
| ------ | --- | --- | --- |
| — | OP\_ADD | 1 | int 加 → `ADDSD` → `jit_callout_concat`（字符串拼接；双方都非字符串时返回 NULL_VAL → bailout） |
| — | OP\_SUB | 1 | int 减 → `SUBSD` → bailout（BigInt / 类型错误交解释器） |
| — | OP\_MUL | 1 | int 乘（`imul` + int64 溢出 + int48 双检查）→ `MULSD` → bailout（null 报错 / BigInt / 类型错误） |
| — | OP\_MOD | 1 | **仅** int48 取模（`cqo`/`idiv`，并自行挡除数为 0）；其余一律 bailout。解释器无 float 路径，故**没有** float 段 |
| — | OP\_LT | 1 | int48 有符号 `cmp` → `UCOMISD`+\\(SETB\\)（无序再 AND 非 PF）→ NaN-boxed 才 bailout |
| — | OP\_GT | 1 | 同上，`SETG` / \\(SETA\\)（JA 在无序时天然为 0） |
| — | OP\_LE | 1 | 同上，`SETLE` / \\(SETBE\\)（无序再 AND 非 PF） |
| — | OP\_GE | 1 | 同上，`SETGE` / \\(SETAE\\)（JAE 在无序时天然为 0） |
| — | OP\_EQ | 1 | int48 `cmp`+`JE` → `UCOMISD`+\(SETE\)（无序再 AND `SETNP`）→ NaN-boxed 才 bailout |
| — | OP\_NEQ | 1 | 同上，`JNE` / \(SETNE\)（无序再 OR `SETP`，使 NaN ≠ NaN 为真） |

> **NaN 语义**（2026-09-11 第二轮）：`OP_*_FLOAT` 与通用 `OP_LT..OP_GE` 的 float 慢路径
> 都按 IEEE 处理——与 NaN 比较除 `!=` 外一律 false。`UCOMISD` 无序时置 `PF=ZF=CF=1`，
> 故 `JA/JAE`（GT/GE）天然给出 false，而 `EQ/LT/LE` 的 setcc 会误判为真，
> 必须再 `SETNP DL; AND AL, DL` 把无序压成 false。旧实现遇 NaN 直接 bailout，
> 会把整个热循环永久踢回解释器（SDL 鼠标坐标可能为 NaN）。

> **实现要点（必须遵守）**：两个操作数要在**任何类型判定跳转之前**全部取到寄存器
> （`TOS_CONSUME_*`），否则从第一个判定跳走时会跳过第二个操作数的 `pop`，
> 让慢路径拿到错的操作数。详见 8.15。

### 自增自减

| Opcode | 枚举名                    | 字节数 | 说明                             |
| ------ | ---------------------- | --- | ------------------------------ |
| —      | OP\_INC\_LOCAL\_NOPUSH | 3   | `add [rbp+disp], 1` + int48 检测 |
| —      | OP\_DEC\_LOCAL\_NOPUSH | 3   | `sub [rbp+disp], 1` + int48 检测 |
| —      | OP\_INC\_LOCAL         | 3   | push 旧值后自增                     |
| —      | OP\_DEC\_LOCAL         | 3   | push 旧值后自减                     |
| —      | OP\_PRE\_INC\_LOCAL    | 3   | 自增后 push 新值                    |
| —      | OP\_PRE\_DEC\_LOCAL    | 3   | 自减后 push 新值                    |
| 40     | OP\_INC                | 1   | 栈顶 ++（pop, add, push）          |
| 41     | OP\_DEC                | 1   | 栈顶 --                          |

### 逻辑运算

| Opcode | 枚举名     | 字节数 | 说明                                 |
| ------ | ------- | --- | ---------------------------------- |
| 34     | OP\_NOT | 1   | 逻辑非：**三态分派**（int48 / NaN-boxed / 裸 double），结果压 NaN-boxed `TRUE_VAL`/`FALSE_VAL`，对齐解释器 `val_bool(is_falsey(v))`；见 §8.22 |

### 移位

| Opcode | 枚举名           | 字节数 | 说明                     |
| ------ | ------------- | --- | ---------------------- |
| —      | OP\_SHL\_IMM  | 2   | `shl rax, imm8`        |
| —      | OP\_SHR\_IMM  | 2   | `sar rax, imm8`（算术右移）  |
| —      | OP\_USHR\_IMM | 2   | `shr rax, imm8`（无符号右移） |

### 全局变量

| Opcode | 枚举名             | 字节数 | 说明                                               |
| ------ | --------------- | --- | ------------------------------------------------ |
| 12     | OP\_GET\_GLOBAL | 3   | 加载 global：int 路径提取 int48，float 路径直通，非数值 bail out |
| 13     | OP\_SET\_GLOBAL | 3   | peek 栈顶写入 global：int 重编码，float 直通                |

### 跳转

| Opcode | 枚举名                 | 字节数 | 说明                 |
| ------ | ------------------- | --- | ------------------ |
| —      | OP\_JUMP            | 5   | 无条件跳转（`JMP rel32`） |
| —      | OP\_JUMP\_IF\_FALSE | 5   | 弹出条件，为 0 跳转        |
| —      | OP\_JUMP\_IF\_TRUE  | 5   | 弹出条件，非 0 跳转        |
| —      | OP\_CMPJMP\_LL\_INT | 10  | 两个 local 比较+条件跳转   |

### 循环回边

| Opcode | 枚举名           | 字节数 | 说明                                                  |
| ------ | ------------- | --- | --------------------------------------------------- |
| 58     | OP\_LOOP      | 5   | while 循环回边（无条件跳转到循环头）                               |
| 95     | OP\_FOR\_PREP | 8   | for 循环初始化（读取 start/end/step/loop\_var，设置初始值并检查循环条件） |
| 96     | OP\_FOR\_LOOP | 7   | for 循环回边（自增 loop\_var、比较 end、条件跳转）                  |

### Callout 操作（通过 C 函数调用实现）

| Opcode | 枚举名                       | 字节数 | 说明                                        |
| ------ | ------------------------- | --- | ----------------------------------------- |
| 91     | OP\_INDEX                 | 1   | 数组/字典索引访问（callout: `jit_callout_index`）   |
| 82     | OP\_ARRAY\_APPEND\_NOPUSH | 1   | 数组追加（callout: `jit_callout_array_append`） |
| 71     | OP\_DICT\_SET             | 1   | 字典赋值（callout: `jit_callout_dict_set`）     |

***

## 5. 不支持的字节码（导致 scan 拒绝）

**判定规则**（权威来源是代码，不要凭记忆）：

1. `jit_scan.c` 的 `opcode_size()` 返回 -1（未收录该 opcode）→ `scan FAIL: unknown opcode`，
   `capable=0`；
2. 收录了但 `scan_loop_body` 的 switch 没有对应 `case` → 落到 default →
   `scan FAIL: unknown opcode`，`capable=0`；
3. 收录且 scan 通过，但 codegen 没有 `case` → `ops_misc.inc` 的 default →
   `codegen FAIL: unsupported opcode`，编译返回 0。

### 操作码覆盖面盘点与实施计划（2026-09-14 脚本盘点）

**⚠ 只补 `opcode_size()` 的长度不改变任何行为**：补完之后扫描器会在第 2 层（`scan_loop_body`
的 switch default）照样拒绝，只是日志从 `unknown opcode` 变成诚实的 `unsupported opcode`。
真正放开必须 **scan 的 `case` 与 codegen 的 `case` 成对加** —— codegen 的 default 是
bailout，只加一半会出现「能编译但一进去就 bailout」的假收益，比直接拒收更糟
（白烧 3 次 bailout 预算后整个循环被拉黑）。

**盘点方法**（权威、可复现，不要凭记忆列）：剥掉 `leno_vm.h` 枚举块的注释取全部 `OP_*`（164 项），
剥掉 `jit_scan.c` 中 `opcode_size()` 函数体的注释取 `OP_*`，做差 ⇒ **56 项未收录**。
实测锚点：`file_manager.leno` 日志里的 `unknown opcode 80/39/132/88/38`
分别正是 `OP_LENGTH` / `OP_SET_DECLARED_FACE` / `OP_SET_FIELD` / `OP_GET_MODULE_VAR` /
`OP_SET_PTR_ELEM_TYPE`，与本表编号完全一致。

| 层 | 内容 | 收益 | 状态 |
| --- | --- | --- | --- |
| L0 | 补齐 `opcode_size()` 的 56 项 + 把 `scan_loop_body` / `scan_callee_for_inline` 的 default 报错区分成 `unsupported opcode`（已收录长度但未实现） | 诊断（让「缺长度」与「缺 case」一眼可分） | **未做** |
| L1 | `OP_LENGTH`（`.len()`） | 高（`for x.len() to i` 遍地） | **已完成**（§8.52）：`opcode_size` + 两处 scan + 数字原生/对象 callout 双路径 + `assert/test_jit_op_length.leno` |
| L2 | `OP_ITER_GET` / `OP_ITER_GET_VALUE`（for-in 迭代） | 高（补完 L1 后是 file_manager 里最高频的缺口 ×4） | **已完成**（§8.53）：`opcode_size` + 两处 scan + 数组原生快路径 + callout + `assert/test_jit_op_iter.leno`；顺带修掉 `jit_callout_failed` 在循环入口未复位导致的连锁 bailout |
| L3 | `OP_SET_FIELD` / `OP_GET_FIELD`（callout，`field_idx` 已在指令里） | 高（对象状态更新 ×2；**顺带解锁 3 个 SDL 包装函数的函数级 JIT**） | **已完成**（§8.54）：`opcode_size` + 两处 scan + callout（写入复用 `struct_set_field` 保住写屏障）+ `assert/test_jit_op_field.leno` |
| L4 | `OP_GET_METHOD`（动态派发方法查找）+ `OP_SWITCH_LOOKUP`（变长：`const(2) count(2) default(4) [offset(4)]...`，×2） | 中 | **两半都已完成**：`GET_METHOD + OP_CALL` 窥孔见 §8.57（复用 `jit_callout_invoke_method`）；`OP_SWITCH_LOOKUP` 见 §8.61（roadmap R1：语义唯一来源 + 变长 `opcode_size` + 多目标前向记账 + 线性比较链，循环拒收清零） |
| L5 | `OP_SET_DECLARED_FACE`（op+const16）/ `OP_SET_PTR_ELEM_TYPE`（op+byte） | 中（×2 / ×1）。**不能当 no-op 跳过**：`declared_face` 影响后续虚拟分派与**数组元素类型推断**、`element_type` 影响 FFI 读写宽度 | **已完成**（§8.58）：`opcode_size` + 两处 scan 净 0 + 两个 callout（无失败通道）+ `assert/test_jit_op_type_tag.leno` |
| L6 | `OP_GET_MODULE_VAR` / `OP_SET_MODULE_VAR` / `OP_GET_MODULE_FUNC`（callout，用当前帧 `module`） | 高（SDL3 大量模块级变量与函数，×2 / ×2；**顺带解锁 2 个函数的函数级 JIT**） | **已完成**（§8.55）：`opcode_size` + 循环 scan（净 +1 / 净 0）+ **内联 scan 显式拒绝**（`frame->module` 来自被调函数）+ 两个 callout（写入带 `gc_write_barrier`）+ `assert/test_jit_op_module_var.leno` |
| L7 | `OP_STRING_ADD`（×1，内联扫描里也出现）/ `OP_NEG` / `OP_IS_NULL` / `OP_ARRAY_GET` / `OP_ARRAY_SET` / `OP_ARRAY_APPEND` / `OP_INDEX_SET` / `OP_DICT` / `OP_DICT_GET` / `OP_DICT_GET_KEY` / `OP_TYPE_CHECK` / `OP_AS_CAST` / `OP_SLICE` / `OP_IN` / `OP_RANGE` / `OP_U8_TO_F64` | 中 | 未做 |
| L8 | `OP_CALL` / `OP_TAIL_CALL` / `OP_CLOSURE` | 中 | **`OP_CALL` 已完成**：`OP_GET_MODULE_FUNC + OP_CALL`（§8.56）+ 裸 `OP_CALL`（§8.59：rc=1 记账 + 调用前守卫、公共核心 `jit_invoke_closure`）。**仍未做**：`OP_TAIL_CALL`、`OP_CLOSURE`（创建闭包）、`GET/SET/CLOSE_UPVALUE`（要动 func-JIT 的 ABI 与 locals 生命周期，单独排期） |
| — | **建议维持拒绝**：`OP_THROW`、`OP_AWAIT` / `OP_ASYNC_CALL`、`OP_CLIB_CALL` / `OP_CFUNC_CALLBACK`、`OP_GET_FIELD_ADDR`、`OP_DTOR_LOCAL`、`OP_TAIL_CALL_NATIVE`、`OP_PUSH_TYPE_ARGS`、模块定义期指令（`OP_DEFINE_GLOBAL*` / `OP_STRUCT_DEF` / `OP_ENUM_DEF` / `OP_FACE_DEF` / `OP_CSTRUCT_DEF` / `OP_LOAD_NATIVE_MODULE` / `OP_INIT_LENOMODULE` / `OP_DEFINE_MODULE_FUNC`） | 语义特殊（异常/协程/FFI/泛型/仅初始化期出现），实现收益低、风险高 | 维持 |

**实测拒收直方图（`file_manager.leno`，2026-09-14，补完 R1 之后）**：

```
op=138(OP_GET_CSTRUCT_DEF)   ×1   ← **仅剩它一条，且属"建议维持拒绝"**（运行时取 cstruct 定义）
inline-scan: 76(OP_STRING_ADD) / 88(模块变量访问) / 156(SWITCH_LOOKUP，R1 起显式拒绝内联)
（§8.59 后 `59:OP_CALL` 归零；§8.61（R1）后 `156:OP_SWITCH_LOOKUP` 归零 ⇒ **循环级拒收清零**）
（§8.59 的额外收益：回调式调用进 JIT 后，被调函数成批进入函数级 JIT ——
同一负载 `FuncCompiled 5 → 161`，后续实测在 100~225 之间波动，取决于自动化交互路径）
```

（演进：L1 前 `81×4 / 132×2 / 156×2 / 39×2 / 88×2 / 38×1` → 补 L1 后 `80` 消失 →
补 L2 后 `81` 消失 → 补 L3 后 `132` 消失 → 补 L6 后 `88`/`90` 消失但**新暴露 `59:OP_CALL ×4`**
→ 补 §8.56（模块函数窥孔）后 `59` 由 ×4 降到 **×2** → 补 §8.57 后 `134(OP_GET_METHOD)` 归零
→ 补 §8.58 后 `38`/`39` 归零、拒收点再前移到 `59 ×4 / 156 ×1 / 138 ×1`
→ 补 §8.61（R1）后 `156` 归零，**只剩刻意保留的 `138`**。
（一个循环只报它的第一条缺口：前面的缺口一补上，后面的缺口才露出来 ⇒ **每次都是拒收点前移**，
总行数基本不变。）

**下一步顺序（2026-09-16 更新）**：R1~R8 的规划项**已全部落地或部分落地** ——
R1（`OP_SWITCH_LOOKUP`）/ R3（诊断收口）/ R4（缓存 UAF）/ R8（基准复盘）已完成；
R5（闭包 C0/C1/C2 + upvalue 读写）已完成（§8.72）；R6（多返回值 + `OP_TAIL_CALL`）已完成
（§8.75 / §8.76）；R7 的 ①②（内联侧 `OP_GET_METHOD`、同模块模块访问）已完成、③ 维持拒绝；
R2 的多批 opcode 已补齐。⇒ **当前剩余缺口的完整清单见 §8.77**（`LENO_JIT_GAPS=1` 实测：
`inline|OP_CALL` **20**、`OP_DICT` **3**、FFI 两类合计 **84**、跨模块访问 48（刻意保留）、
函数体含循环 64（设计限制）、循环体可达 return 10（设计限制））。

**关键：单补一个 opcode ≠ 解锁循环。** 只有某个循环的**全部**缺口都被补齐，它才真正进 JIT
⇒ 这类工作要**成批推进**，并按上面这张直方图排序（本表 L4~L6 的先后就是这么定的）。
注意覆盖面还决定**函数级 JIT 与被调函数内联**能否成立：L3 补完 `OP_SET_FIELD` 后，
`set_pos` / `set_size` / `setWindowHandle` 三个 SDL 包装函数立刻进了函数级 JIT（§8.54）；
§8.59 补完裸 `OP_CALL` 后，同一负载的 `FuncCompiled` 从 5 涨到 **161**。

**未来规划（roadmap）**

| # | 项目 | 现状 / 缺口 | 前置与成本 | 风险 | 建议顺序 |
|---|---|---|---|---|---|
| ~~R4~~ | ~~`jit_func_cache` 冲突驱逐的 **use-after-free 隐患**~~ | ~~256 槽 direct-mapped，冲突时 `jit_mem_free` 掉占用者的机器码 —— 若那个函数**正在 C 栈上执行**（A 调 B、B 的 callout 又编译了撞槽的 C）就是 UAF~~ | ~~需要「执行中计数」+ 驱逐时延迟回收~~ | ~~高（内存安全）~~ | **已完成（§8.60，2026-09-14）**：确认存在（强制碰撞 4/4 崩 `0xC0000005`）并修复为**机器码延迟释放队列**（安全点判据复用 `jit_func_depth`/`jit_loop_depth`）；`LENO_JIT_FUNC_CACHE_SMALL=1` 做确定性复现，用例 `assert/test_jit_func_cache_churn.leno` |
| ~~R1~~ | ~~`OP_SWITCH_LOOKUP` 进 JIT~~ | ~~file_manager 仅剩的"可做但难"拒收点（×1）。VM 侧是 int/bigint/float/string 四路二分查找；变长编码：`const_idx(2) count(2) default_off(4) [case_off(4)]…`~~ | ~~`opcode_size` 变长解码 + scan 的多目标前向记账 + codegen 线性比较链~~ | ~~中~~ | **已完成（§8.61，2026-09-14）**：查找逻辑抽成 `switch_lookup_index`（VM/JIT 共用，语义唯一来源）+ 变长 `opcode_size` + 多目标前向记账（内联侧保守拒绝）+ 线性比较链；`assert/test_jit_op_switch_lookup.leno`；**循环拒收清零**（直方图只剩刻意保留的 `138`） |
| ~~R12~~ | ~~【内存安全】JIT 下「循环体内**按字面量属性名取值**」堆损坏~~（原描述："dict 取值 + 存局部变量"） | ~~最小形态 100% 复现 `0xC0000374`；200000 轮正常 / 262144 轮崩~~ | ~~一个 `pop_bytes` 判据~~ | ~~高（内存损坏）~~ | **已完成（§8.67，2026-09-14）**：根因不是 dict 路径，而是 **`OP_GET_PROPERTY` 独立访问形态没弹 receiver 槽** ⇒ **每次执行泄漏 8 字节** ⇒ 循环里 RSP 单调下漂 ⇒ ~2MB 栈界处 `0xC0000374`。修法：该分支 `pop_bytes` 由 `0` 改为 `8`。实测漂移 **−8/次 → 0**、该循环 **182ms → 约 20~33ms**、A 形态的 3 次 `int48` bailout 一并消失；用例 `assert/test_jit_op_get_property.leno`（40 万轮，必须 > 262144 阈值） |
| R2 | L7 余项成批补齐 | 余项：`OP_AS_CAST`(94)、`OP_GET_METHOD`(134，**裸方法取值**形态)、`OP_ARRAY_GET/SET`、`OP_DICT*`、`OP_SLICE`(79)、`OP_IN`、`OP_RANGE`、`OP_NEG`(33)、`OP_U8_TO_F64`(146) | 多为 callout 型，照 §8.52~§8.59 的模板走（`opcode_size` + 两处 scan + callout + codegen + 用例）；需要"语义唯一来源"抽取的（VM 里就地 READ 的那类）照 §8.61/§8.64/§8.65 的做法 | 低 | **批次 1（§8.63）**：`OP_IS_NULL`(48)。**批次 2（§8.64）**：`OP_STRING_ADD`(76)（由插值 `$"..."` 产生，不是 `+`；顺带解开内联侧）。**批次 3（§8.65）**：`OP_TYPE_CHECK`(93)——抽出 `type_check_value` 共用，顺带修掉 `TYPE_ENUM` 的 4 字节长度 bug；实测 `FuncCompiled 123 → 289`。**下一批候选**（实测排序）：**内联侧**补 `OP_GET_METHOD`（×36，见 R7；这是"内联能否成立"的问题，不影响循环编译）、`94:AS_CAST`（带值改写语义，直方图未出现）。注：`-d["k"]`、`x == null` 实测走已支持的路径，不必做；**循环级拒收此刻只剩刻意保留项**（`138`）/ R5 前置（`14`）/ 维持拒绝（`142`） |
| ~~R3~~ | ~~L0 诊断收口~~ | ~~`opcode_size` 余项补全；把 scan 的 default 报错区分成 `unknown opcode`（缺长度）与 `unsupported opcode`（已收录长度但缺 case）~~ | ~~纯诊断，无行为变化~~ | ~~极低~~ | **已完成（§8.63，2026-09-14）**：补齐约 40 条长度（逐条与 VM 的 `READ_*`、`debug.c` 反汇编器交叉核对）；`OP_CLOSURE` 因长度依赖常量表而**故意保持"未知"**；报错分为 `unknown（长度未知）`/`unsupported（已收录长度、未实现）`。**实测收益**：同一负载的拒收清单从"含糊的 138"变成分类可排期（76×4 / 93×3 / 14×2 / 142×2 / 138×49），R2 的下一批因此是测出来的而不是猜的 |
| R5 | 闭包创建与捕获变量（`OP_CLOSURE` / `GET/SET/CLOSE_UPVALUE`） | 循环里建闭包、闭包体内读写捕获变量都不进 JIT（被调函数带 upvalue 时自动退回 VM 重入 ⇒ 语义正确但不快） | 两件基础：**(a)** func-JIT ABI 要加 closure 通道（现为 `jfn(flocals, globals)`，无 upvalue 入口）；**(b)** 循环 JIT 的 locals 在 scratch 区（迭代即复用）⇒ 直接捕获会产生**悬空 upvalue**，只能先允许"捕获已在 upvalue 链上的变量"，或改 locals 布局 | 高（ABI + 生命周期不变量） | **已完成（§8.72，2026-09-15）**：C0（零捕获）/ C1（by-upvalue）/ C2（值捕获）+ `GET/SET_UPVALUE` 全部进 JIT（P1 `34508a90` / P2a `68002778` / P2b `6c119bd3` / P3 `2a572102`，assert 304→307/0）；设计稿与不变量 I1~I8 见 `JIT闭包与upvalue设计_R5.md`。**C3（引用捕获本帧局部）与内联体内的闭包维持拒绝**（P4 需实测授权 / I8） |
| R6 | 函数级 JIT 的多返回值 + `OP_TAIL_CALL` | `jit_compile_function` 直接拒收 `return_count > 1`；`OP_TAIL_CALL` 在循环 JIT 里等价"提前返回"（应归入 `has_reachable_return` 拒绝），只有函数级 JIT 有价值 | 多返回：扩返回值通道（`jit_fn_result` 单值 → 多槽约定），调用方回填约定已就绪（§8.59 的 helper）；尾调用：帧复用语义另算 | 中 | **R6-a（多返回值）已完成（§8.75，2026-09-15）**：发布区 `jit_fn_results[] + jit_fn_result_count` + `jit_fastpath_deliver_multi()`；三处快路径与解释器热入口全部支持多值；`return_count == -1` 靠交付前复核回落。探针 `probe_multi_ret_jit.leno` JIT/NO_JIT 逐字一致、快路径 3903 次零回退、assert 307/0。**R6-b（`OP_TAIL_CALL`）已完成（§8.76，同日）**：函数模式=「调用 + 发布结果 + epilogue」，循环/内联维持拒绝；关键是**先补上 `OP_GET_GLOBAL_FUNC`（opcode 18）的缺口**（尾调用形态是它 + `OP_TAIL_CALL`，此前整函数被拒）。"关闭本帧 upvalue"对 JIT 恒为空操作（R5 的 I1 + C3 拒绝）；空间行为靠深度守卫交解释器做真 TCO（探针 20000 层 ×200 轮不崩、0.1s）。探针 `probe_tail_call_jit.leno`、assert 307/0。**仍未支持**：`OP_TAIL_CALL_NATIVE`。**R6-d（`OP_GET_CSTRUCT_DEF`）已完成（§8.80，2026-09-16）**：三侧（loop/func/inline）全部支持，**只嵌名字常量、查找在 callout 里每次现做**（定义是运行时注册的 + 同名重声明会废弃旧 def ⇒ 不能编译期解析）；实测解锁 **8 个函数级编译 + 13 个内联点**，**循环级净解锁 0**（原来被 cstruct 挡的热循环现在被 `OP_CLIB_CALL` 挡）⇒ 下一步做 `OP_CLIB_CALL`(142)，其后是 `OP_AS_CAST`(94) 与 native 方法派发（两者都是本次露出的相邻缺口，见 §8.80）。**R6-e（`OP_CLIB_CALL`）已完成（§8.81，2026-09-16）**：操作数变长 ⇒ codegen 把整条 `ip` 交给 callout（与 `OP_CLOSURE` 传描述表同款）；参数搬上 VM 栈再调 `ffi_clib_call`。三侧支持、`142` 清零，真实负载合计 **loop -9 / func -11 / inline -5**；探针 `probe_clib_call_jit.leno` JIT/NO_JIT 逐字一致且 **Bailouts: 0**、assert 308/0。**下一批**（差分新露出、且直接影响收益兑现）：`call_value` 对 native 方法对象、`OP_AS_CAST`(94)、`OP_GET_FIELD_ADDR`(133) |
| R7 | 内联的跨模块限制 + 内联侧 opcode 缺口 | ① 模块变量/函数访问被 inline scan 一刀拒绝（§8.55/§8.56）——内联后没有 callee 的帧，模块归属不可知（**实测 ×166**，内联侧第一大）；② inline scan 没有 `OP_GET_METHOD` 的 case ⇒ 含方法调用的函数一律不能内联（**实测 ×36**）；③ `138:GET_CSTRUCT_DEF ×116`（维持拒绝） | ① 在 inline site 记录 callee 的 module，与 caller 相同才允许内联；② 照 loop scan 的记账补 `GET_METHOD`（配对形态 `-(argc+1-rc)`，rc 用同样的 `jit_resolve_method_ret_count`，解析不出就拒绝内联） | 低 | **② 已完成（§8.68，2026-09-15）**：`scan_callee_for_inline` 补 `OP_GET_METHOD`（配对/独立两形态），探针 `probe_inline_method_call.leno` 2.27~2.40x、用例 `test_jit_inline_method_dispatch.leno`。**① 已完成（§8.69，同日）**：加「callee 与 caller 同模块」守卫后放行模块变量/函数访问，探针 `probe_inline_module_var.leno` ≈2~3x、跨模块仍拒绝（安全边界）、用例 `test_jit_inline_module_var.leno`；assert 298/0。③ 维持拒绝。**④ 已完成（2026-09-16，§8.77）**：内联侧补 `OP_CALL`(59) 与 `OP_GET_GLOBAL_FUNC`(18)（"取函数值再调用"形态此前使被调方永不可内联），拒收清零、探针同二进制 A/B **2.72x**、用例 `test_jit_inline_call_value.leno`、assert 308/0 |
| ~~R8~~ | ~~性能基准复盘~~ | ~~§9 的数据需要按最新覆盖面重跑（JIT vs `LENO_NO_JIT=1`），量化"覆盖面增长到底换来多少"~~ | ~~纯测量；也顺便验证 R4 的延迟回收没有性能回退~~ | ~~极低~~ | **已完成（§9 的「R8 基准复盘」小节，2026-09-14）**：新增可复用基准 `examples/性能测试/JIT覆盖面基准.leno`（8 项，专测本轮补齐的 opcode）；JIT/VM 加速比 2.2x\~18.6x，**字符串插值 1.0x（分配主导，非测错）**；与历史基线交叉核对无回退（i++ 75 vs 78 ms/亿、arr.add 620 vs 625 ms/亿）⇒ R4 延迟释放无可测代价 |
| ~~R9~~ | ~~`OP_SWITCH_LOOKUP` 的 callout 开销~~ | ~~R9 前的基准显示：JIT 下 switch 每轮一次 callout（+15.5ms/3M 轮），比等价的 if 链慢 3.7 倍~~ | ~~编译期分流：case 值全 int 时发内联比较链~~ | ~~低（非 int 一律退回原 callout 路径）~~ | **已完成（§8.62，2026-09-14）**：int 快路径 + int48 守卫（bailout 交解释器，保住 bigint 值能命中 int case 的语义）+ 重复值排除；switch 的 JIT 时间 30.4 → **15.7 ms**，与 if 同级 |
| — | **建议维持拒绝** | `OP_THROW`、`OP_AWAIT`/`OP_ASYNC_CALL`、`OP_CLIB_CALL`/`OP_CFUNC_CALLBACK`、`OP_GET_FIELD_ADDR`、`OP_DTOR_LOCAL`、`OP_TAIL_CALL_NATIVE`、`OP_PUSH_TYPE_ARGS`、**`OP_GET_CSTRUCT_DEF`(138)**、模块定义期指令 | 语义特殊（异常/协程/FFI/泛型/仅初始化期出现），收益低、风险高 | — | 维持 |
| ~~R10~~ | ~~Linux 上 `jit_mem_free(ptr, 0)` 导致可执行内存**永不归还**~~ | ~~POSIX 侧 `munmap` 需要长度，而所有调用点都传 0 ⇒ `EINVAL`、每次驱逐漏一块；Windows 侧 `MEM_RELEASE` 忽略 size 所以一直没暴露（也解释了 R4 为何只在 Windows 复现）~~ | ~~把尺寸与代码指针一起存~~ | ~~低（Windows 行为不变）~~ | **已完成（§8.63，2026-09-14）**：`JitCacheEntry.code_size` / `JitFuncCacheEntry.code_size` + 两个编译函数的 `out_size` 出参 + 4 个释放点改用真实长度 |

**45 项未收录全量（`编号:名字`，用于 L0 逐项补长度；`80:LENGTH`、`81:ITER_GET`、
`82:ITER_GET_VALUE`、`131:GET_FIELD`、`132:SET_FIELD`、`88:GET_MODULE_VAR`、`89:SET_MODULE_VAR`、
`90:GET_MODULE_FUNC`、`134:GET_METHOD`、`38:SET_PTR_ELEM_TYPE`、`39:SET_DECLARED_FACE`、`59:CALL`、
`156:SWITCH_LOOKUP` 已于 §8.52~§8.61 补齐，从本表移除；同时补录 `137:CSTRUCT_DEF` / `138:GET_CSTRUCT_DEF`）**：

```
14:GET_UPVALUE 15:SET_UPVALUE 16:CLOSE_UPVALUE 17:DEFINE_GLOBAL 18:GET_GLOBAL_FUNC
19:DEFINE_GLOBAL_FUNC 20:GET_NATIVE 33:NEG
48:IS_NULL 53:IN 54:RANGE 60:TAIL_CALL 61:CLOSURE 65:ARRAY_GET 66:ARRAY_SET
67:ARRAY_APPEND 69:DICT 70:DICT_GET 72:DICT_GET_KEY 73:LOAD_NATIVE_MODULE
75:GET_MODULE_CONST 76:STRING_ADD 78:INDEX_SET 79:SLICE 87:THROW
91:DEFINE_MODULE_FUNC 93:TYPE_CHECK 94:AS_CAST 129:STRUCT_DEF 133:GET_FIELD_ADDR
135:ENUM_DEF 136:FACE_DEF 137:CSTRUCT_DEF 138:GET_CSTRUCT_DEF
137:CSTRUCT_DEF 138:GET_CSTRUCT_DEF 139:AWAIT 140:ASYNC_CALL 141:INIT_LENOMODULE
142:CLIB_CALL 143:CFUNC_CALLBACK 145:TAIL_CALL_NATIVE 146:U8_TO_F64 147:PUSH_TYPE_ARGS
148:DTOR_LOCAL
```

> 注意：**通用 `OP_MUL` / `OP_MOD` / `OP_EQ` / `OP_NEQ` 已于 2026-09-12 支持**（见第 4 节
> 「通用算术与比较」）；它们缺席时同样会让**整个循环**被拒绝编译（不是 bailout），
> 历史上排查时看 `scan FAIL: unknown opcode N` 而不是 `BAILOUT`。
>
> 排查通用 opcode 是否真的能进 JIT：`jit_scan.c` 三处必须同步——
> `opcode_size()`、`scan_loop_body()`、`scan_callee_for_inline()`；漏掉最后一处会导致
> 「循环本身能编，但含该 opcode 的被调函数无法内联」。
>
> 通用 `OP_MUL` 为什么会出现在字节码里（编译器 `AST_MODULE_CALL` 分支未写回
> `ast->cached_type` → codegen 读到 TYPE_ANY → 发通用 opcode），以及模块调用
> callout 的三项优化与性能数据，见 `docs/JIT模块调用优化与bailout排查记录.md`。

### 曾经"不支持"、现已支持（旧文档已过时）

| Opcode | 现状 |
| ------ | --- |
| OP\_TRY / OP\_CATCH / OP\_FINALLY / OP\_END\_TRY | JIT 中视为 **no-op**（`ops_misc.inc`）。正常路径无需 setup；若 callout 抛异常则整体 bailout 回解释器重跑完整 try/catch |
| OP\_MODULE\_CALL | callout `jit_callout_module_call`（如 `maths.sqrt`）；但 **ffi 定宽内存读写**（`read_byte`/`read_int8`/`read_int16`/`read_uint16`/`read_int`/`read_uint` 及对应 `write_*`，共 12 个）走**内联**，见 §2.6 |
| OP\_GET\_PROPERTY / OP\_INVOKE\_METHOD\_TYPED | callout（`jit_callout_get_property` / `jit_callout_invoke_method`），并带 GET\_PROPERTY+OP\_CALL 窥孔合并；`_TYPED` **支持多返回值回填**（§8.20），且因字节码带静态类型名可做编译期去虚拟化 / 方法内联（§8.30） |
| OP\_CALL\_NATIVE | callout `jit_callout_call_native` |
| OP\_INDEX / OP\_ARRAY / OP\_DICT\_SET / OP\_INDEX\_SET\_NOPUSH / OP\_ARRAY\_APPEND\_NOPUSH | callout |
| OP\_ITER\_GET / OP\_ITER\_GET\_VALUE | ITER\_GET 的「数组 + int 索引」走**原生**（直接读 `elements[idx]`）；其余（数字迭代 / dict 键值 / enum / string 单字符 / struct 字段名）走 callout `jit_callout_iter_get`，错误与越界一律 bailout 交解释器重放（§8.53，覆盖面 L2） |
| OP\_GET\_FIELD / OP\_SET\_FIELD | callout `jit_callout_get_field` / `jit_callout_set_field`（§8.54，覆盖面 L3）：struct 路径完整实现（含越界检查、int→float/bigint→float 提升）；**写入复用 `struct_set_field`，写屏障由它保证**；cstruct 读与非 struct 报错走 bailout 交解释器重放 |
| OP\_GET\_MODULE\_VAR / OP\_SET\_MODULE\_VAR / OP\_GET\_MODULE\_FUNC | callout `jit_callout_get_module_var` / `jit_callout_set_module_var`（§8.55，覆盖面 L6）：module 由 codegen **编译期嵌入**（§8.56 修正：不能查运行时帧）；`SET` 是 **peek** 语义（净 0、不改 TOS）且写入带 `gc_write_barrier`；无模块/越界走 bailout。**内联扫描显式拒绝这三条**（内联后没有被调函数的帧，会读错模块的变量） |
| OP\_GET\_MODULE\_FUNC **+** OP\_CALL（模块内部函数调用） | 窥孔合并为一次 `jit_callout_call_module_func`（§8.56，覆盖面 L8 部分）：`return_count` 编译期解析，`-1`/解析失败即拒绝整个循环；callee 从**编译期模块**现取并复核 ret_count（模块变量可被重新赋值）；多返回值沿用 `jit_callout_invoke_method` 的回填约定。**跨模块 `m.f()`、闭包值调用、`OP_TAIL_CALL` 仍未做** |
| OP\_GET\_METHOD **+** OP\_CALL（动态派发方法调用） | 窥孔合并，复用 `jit_callout_invoke_method`（§8.57）：传给它的 `vstack_top` 要 `+8`（跳过 GET_METHOD 消费的那个额外 receiver）；`rc` 按「方法名唯一且 return_count 一致」推断，推不出即拒绝整个循环；弹 `argc - rc + 2` 个槽（**比逻辑计数多 1**，见 §8.57 教训 1）。独立的 `OP_GET_METHOD`（只取方法值）拒绝 |
| OP\_SET\_PTR\_ELEM\_TYPE / OP\_SET\_DECLARED\_FACE | callout `jit_callout_set_ptr_elem_type` / `jit_callout_set_declared_face`（§8.58，覆盖面 L5）：两条都是 **peek TOS、净 0、无返回值**，类型不匹配时静默不做（与解释器逐字一致，**不设失败通道**）。`declared_face` 影响数组元素类型推断与 GC 标记，`element_type` 影响 FFI 读写宽度 —— 不能当 no-op 跳过 |
| 裸 OP\_CALL（callee 是运行时值：局部闭包 / 回调表 / 字段） | callout `jit_callout_call_value`（§8.59，覆盖面 L8）：接受 `OBJ_CLOSURE` / `OBJ_FUNCTION`（裸函数由解释器侧包闭包，JIT 不额外分配），`OBJ_NATIVE`/bound method/null 交解释器；**rc 按 1 记账 + 调用前守卫**（`return_count != 1` 即 bailout，有界退化且不会双执行）。公共核心 `jit_invoke_closure` 与 `INVOKE_METHOD_TYPED`/`GET_METHOD+CALL` 共用 |
| OP\_LENGTH | **数字原生**（32 位 `CVTTSD2SI` + 负值 clamp，复刻解释器的 `(int)double`）/ 对象与非法类型走 callout `jit_callout_length`（§8.52，覆盖面 L1） |
| OP\_STRUCT\_INIT（非泛型） | callout `jit_callout_struct_init` |
| OP\_RETURN / OP\_RETURN\_MULTI | 支持（函数级 JIT；多返回值仅内联路径）。**循环体内可达的 return 会让整个循环被拒绝**（§8.21） |
| 脚本函数调用（OP\_CALL\_GLOBAL\_FUNC\_TYPED） | **内联**展开进宿主循环（见 13.8），不再整体拒绝 |

### 特殊拒绝条件

- **虚拟栈溢出**：`vstack > JIT_MAX_VSTACK (64)`（允许负 vstack，因嵌套循环 exit POP 在线性扫描中被重复扣减）

- **local 数过多**：超过 `JIT_MAX_LOCALS (64)`（含内联 callee 的槽位）

- **循环体内存在可达的 `OP_RETURN` / `OP_RETURN_MULTI`**：循环 JIT 只能「跑完循环再回到解释器」，
  没有能力从机器码里真正返回函数（codegen 里 `OP_RETURN` 只能退回「spill 后继续往下跑」），
  这个 `return` 会被静默丢弃。`scan_loop_body` 置 `ScanResult.has_reachable_return`，
  `jit_compile()` 据此拒绝（`scan REJECT` 日志）；函数级 JIT / 内联 callee 能正确处理，不受影响。
  详见 §8.21。

- **`OP_INVOKE_METHOD_TYPED` 的返回值个数无法在编译期确定**：见 §8.20，拒绝（不猜成 1 个）。

***

## 6. Callout 机制

对于无法纯机器码实现的操作（如数组/字典操作），JIT 通过 **callout** 机制回调 C 函数。

### Callout 基础设施

```c
static VM* jit_callout_vm;  // 在 jit_try_hot_loop 中设置

// raw ↔ NaN-boxed Value 转换
static inline Value jit_raw_to_value(int64_t raw);
static inline int64_t jit_value_to_raw(Value v);

// Callout C 函数
static Value jit_callout_index(Value obj_val, Value idx_val);
static int jit_callout_array_append(Value arr_val, Value value);
static Value jit_callout_dict_set(Value dict_val, Value key_val, Value value);
```

### 机器码宏

```c
EMIT_RAW_TO_VALUE()    // RAX: raw → NaN-boxed Value（int48 重编码）
EMIT_VALUE_TO_RAW()    // RAX: NaN-boxed Value → raw（int 提取）
EMIT_CALLOUT_BEGIN()   // 保存 RSP/RCX/R9，对齐栈，分配 shadow space
EMIT_CALLOUT_END()     // 恢复 RSP/RCX/R9，重加载 R10/R11
EMIT_CALL(fn)          // mov rax, imm64; call rax
EMIT_STORE_TMP(d, reg) // 存到 RBP 临时槽
EMIT_LOAD_TMP(reg, d)  // 从 RBP 临时槽读
```

### 临时槽位

3 个临时槽位于虚拟栈下方：

```c
int tmp1_disp = -8 * (n + sr->max_vstack + 1);
int tmp2_disp = -8 * (n + sr->max_vstack + 2);
int tmp3_disp = -8 * (n + sr->max_vstack + 3);
```

### Callout 调用流程（以 OP\_DICT\_SET 为例）

```
1. pop value → EMIT_RAW_TO_VALUE → store tmp1
2. pop key   → EMIT_RAW_TO_VALUE → store tmp2
3. pop dict  → EMIT_RAW_TO_VALUE → store tmp3
4. EMIT_CALLOUT_BEGIN()
5. load RCX=tmp3(dict), RDX=tmp2(key), R8=tmp1(value)
6. EMIT_CALL(jit_callout_dict_set)
7. EMIT_CALLOUT_END()
8. EMIT_VALUE_TO_RAW() → push result
```

***

## 7. 调试方法

### 环境变量

| 变量 | 作用 |
| --- | --- |
| `LENO_JIT_DEBUG=1` | 打印 scan/codegen FAIL、`COMPILE`、`BAILOUT`（含精确 site） |
| `LENO_JIT_DUMP=1` | dump 机器码到 `jitdump<N>.bin`（二进制，配 objdump）和 `jit_mc_dump.txt`（hex 文本）。**会在当前工作目录留下文件，注意别误提交** |
| `LENO_NO_JIT=1` | 完全禁用 JIT（性能对比基线 / 隔离"解释器 bug vs JIT 机器码 bug"） |
| `LENO_JIT_TRACE=1` | 每次 JIT 执行前后打印 `frame->locals[0..39]` 的原始位模式（配 `LENO_JIT_DEBUG`） |
| `LENO_JIT_NOINLINE=1` | 关闭被调函数内联 |
| `LENO_JIT_FPROF=1` | 打印函数级 JIT 的调用开销分布 |
| `LENO_JIT_FTRACE=1` | 函数级 JIT 跟踪 |

### bailout site 编码约定（2026-09-11 起）

`jit_bailout_site` 是全局量，**每个 bailout 守卫都在跳转前写一次**，所以它永远反映真正
失败的那条指令。取值为负数区间时表示非溢出原因：

| site 取值 | 含义 |
| --- | --- |
| `>= 0` | 溢出/截断类检查，值 = 触发指令的 `bc_off`（内联帧含 `0x10000 * depth` 基址） |
| `-1` | JIT 序言的「进入自增」int48 溢出检查 |
| `-2` | JIT 序言的 `step == 0` 检查（VM 语义：不进循环） |
| `-3` | JIT 序言的 `step` 是 float 检查（类型位图为 1） |
| `<= -1000` | 其它原因（callout 失败、类型不支持、concat 失败、`step==0`/float step 的 `FOR_PREP` 侧…），`bc_off = -1000 - site` |

对应宏：`EMIT_INT48_CHECK(bc_off)` / `EMIT_INT64_OVF_CHECK(bc_off)` 写非负值；
`EMIT_BAILOUT_SITE_NONOVF(bc_off)` 写负值；两者都在 `backend/x86_64.c`。
`jit_print_stats` 退出时会用缓存条目里的 `last_bailout_site/bc_off/fn` 打印一行
「哪个函数、哪个循环、什么原因」，不必再手工换算 site。

> **历史坑**：加上这套约定之前，只有 int48 检查会写 site，其它守卫
> （`OP_ADD` concat 返回 NULL、`OP_SUB` 非 int48、callout 失败、`FOR_PREP` 步长…）
> 都不写，于是日志里的 `site=` 是**上一次写入的残留值**，会把排查方向带偏
> （实例：ripple 示例 9 次 bailout 全被报成 `OP_FOR_LOOP` 的偏移，真正原因是
> `1.0 - maths.abs(diff)` 的通用 `OP_SUB`）。

### 调试输出示例

```
[JIT-DEBUG] scan result: n_locals=28 max_vstack=4
[JIT-DEBUG]   scratch[0] = slot 29          ← bytecode 槽位 → scratch 槽位映射
[JIT-DEBUG]   for_loop_var_slot=29 for_end_slot=30 for_step_slot=31 inclusive=1
[JIT-DEBUG] COMPILE: fn='renderRipple' bc_off=462 back_edge=2, body_size=379, capable=1, n_locals=28, max_vstack=4, inline=0
[JIT-DEBUG] scan FAIL: unknown opcode 132 (size<0) at offset 20
[JIT-DEBUG] compile FAIL at body_start=654, back_edge=2
[JIT-DEBUG] BAILOUT site=372 RSP=... RAX=...
[JIT-DEBUG] BAILOUT(nonovf) bc_off=159 RSP=... RAX=...
[JIT-DEBUG] BAILOUT(prologue:step==0) RSP=... RAX=0
[JIT-DEBUG] BAILOUT(prologue:step-is-float) RSP=... RAX=...
[JIT-DEBUG] BAILOUT(prologue:entry-increment) RSP=... RAX=...
[JIT-DEBUG] BAILOUT at body_start=462, count=1
[JIT-DEBUG] FRAME-DEAD exit (2) at body_start=...
```

### 定位流程

1. 用 `LENO_JIT_DEBUG=1` 运行，先看有没有 `BAILOUT*` 行和对应的 `body_start`
2. 用 `--debug --debug-out <file>` 导出字节码，找到该函数的反汇编
3. **site 换算**：`body_start` 与 site 都是「相对于帧 chunk 起始」的偏移，
   按上表的编码换算出真正的 `bc_off`，再对照反汇编定位到源码行
   （注意：同一循环在嵌套外层循环的 body 里，`bc_off` 会不同，但三者换算到
   同一个绝对偏移——若多个 `body_start` 报出能对到同一条指令，说明它们都栽在那里）
4. 若 site 是负数区间或指向明显不该失败的指令，先怀疑「守卫没写 site / 操作数取错」
   这类 codegen 问题，用 `LENO_JIT_DUMP=1` + objdump 反汇编核对生成的机器码
5. 需要判断是解释器还是 JIT 的问题，用 `LENO_NO_JIT=1` 跑同一二进制做对照
6. **结果「偶尔对、多数错」时先算 «阈值»**：JIT 在热点阈值（`JIT_HOT_THRESHOLD=50`）后才接管，
   所以「301 次迭代里错了 251 次」= `301 - 50`，说明错值全部来自 JIT 路径（§8.22 就是这么定性的）。
7. 看 `PATCH-REDIRECT` 行：`compile_loop` 收尾时，循环体内跳转的目标偏移若在
   `off_map` 里找不到，会被**静默改成跳 `exit_mc`**（= 循环提前退出）。这条日志说明
   某条 `continue`/`break` 的目标没被编出来，通常意味着 scan 与 codegen 对循环体
   边界（`body_start` / `body_size`）的理解不一致。

### objdump 反汇编生成的机器码

```bash
LENO_JIT_DUMP=1 ./build/lenojit.exe file.leno
objdump -D -b binary -m i386:x86-64 -M intel jitdump1.bin
```

***

## 8. 踩坑记录与解决方案

### 8.1 JP 条件码 bug — 非法指令 `0F 0A`

**现象**：纯 float globals 循环崩溃，报 `Illegal instruction`。

**根因**：`emit_jcc` 函数发 2-byte Jcc 格式 `0F cc`，传入的 cc 值需要是 `0x80+条件`。但代码中 JP（Parity）传了 `0x0A`，生成 `0F 0A`——这不是合法的 Jcc 指令。JP 的正确 cc 值是 `0x8A`。

**修复**：所有 `emit_jcc` 调用统一使用 `0x80+条件码` 格式：

```
JP  = 0x8A    JNE = 0x85    JAE = 0x83
JG  = 0x8F    JGE = 0x8D    JL  = 0x8C
JLE = 0x8E    JB  = 0x82    JA  = 0x87
```

### 8.2 UCOMISD 条件码 bug — SETL 永远返回 0

**现象**：float 比较结果始终为 false。

**根因**：`UCOMISD` 设置 CF/ZF/PF 但**清零 SF/OF**。因此基于 SF/OF 的有符号条件码（SETL=SF≠OF、SETG=ZF=0且SF=OF）永远返回 0。

**修复**：改用无符号条件码（基于 CF/ZF）：

```
SETB  (CF=1)         → float <
SETA  (CF=0且ZF=0)   → float >
SETBE (CF=1或ZF=1)   → float ≤
SETAE (CF=0)         → float ≥
SETE  (ZF=1)         → float ==
```

### 8.3 SSE2 前缀 bug — packed vs scalar

**现象**：浮点算术结果错误（如 `1.5 + 2.5` 得到错误值）。

**根因**：`emit_sse2_rr` 使用 `0x66` 前缀（packed 操作），但标量双精度算术需要 `0xF2` 前缀。

```
ADDSD/SUBSD/MULSD/DIVSD → 0xF2 前缀（scalar double）
MOVQ (0F 6E / 0F 7E)    → 0x66 前缀
UCOMISD (0F 2E)         → 0x66 前缀
XORPD (0F 57)           → 0x66 前缀
```

**修复**：`emit_sse2_rr` 的前缀参数改为 `0xF2`。

### 8.4 OP\_CAST\_FLOAT 无条件 CVTSI2SD

**现象**：对已经是 float 的值执行 `OP_CAST_FLOAT` 会破坏 float 值（把 double bits 当 int 转换）。

**根因**：`OP_CAST_FLOAT` 的 codegen 无条件执行 `CVTSI2SD xmm0, rax`，但虚拟栈上 float 值已经是 raw double bits，不是整数。

**修复**：改为运行时类型分派——先用 int48 检测判断值类型，是 int 才 `CVTSI2SD`，否则直通。

### 8.5 OP\_GET\_GLOBAL 仅 int 类型守卫

**现象**：全局变量是 float 时，JIT 加载后当作 int 处理，导致后续运算错误。

**根因**：`OP_GET_GLOBAL` 只检查 `INT_TAG`，非 int 直接 bail out。

**修复**：改为 int-or-float 双路分派：检查 top16 是否为 0xFFFB（int），是则提取 int48；否则检查是否 ≥ 0xFFF8（NaN-boxed 非数值），是则 bail out；否则当作 float raw bits 直通。

### 8.6 OP\_CONST 不支持非 int/float 常量

**现象**：含字符串常量的循环（如 `dict["key"] = value`）编译失败：`codegen FAIL: OP_CONST non-int/float const`。

**根因**：`OP_CONST` codegen 遇到 string/null/bool/obj 常量直接 `return 0`。

**修复**：对非 int/float 常量，直接将 raw NaN-boxed Value bits 压入虚拟栈。`EMIT_RAW_TO_VALUE` 的 int48 检测（`sar 47; inc; cmp 1; ja`）能正确识别 NaN-boxed 值为非 int48（位移结果 >1），跳过 int 重编码，直接传给 callout。

### 8.7 OP\_SET\_LOCAL\_CONST 不支持非 int/float 常量

**现象**：`SET_LOCAL_CONST` 遇到字符串常量直接 `return 0`，导致循环编译失败。

**根因**：同 OP\_CONST，只支持 int/float。

**修复**：对非 int/float 常量，加载 raw NaN-boxed bits，并额外发 `BTS RBX, si` 指令标记该 local 为 non-int，确保 epilogue write-back 存 raw bits 而非错误地重编码为 int。

### 8.8 OP\_FOR\_PREP 注册到 opcode\_size 表 + 嵌套 for 支持

**现象**：`scan FAIL: unknown opcode 95 (size<0)`。

**根因**：`OP_FOR_PREP`（opcode 95, 8 bytes）不在 `opcode_size()` 函数的 switch 中，返回 -1。

**修复**：在 `opcode_size()` 中添加 `case OP_FOR_PREP: return 8;`。后续 P1 阶段进一步在 scan 和 codegen 中完整支持 OP\_FOR\_PREP（读取 start/end/step/loop\_var slot，设置初始值，检查循环条件，前向跳转）。

### 8.9 嵌套循环的 vstack 负值修复

**现象**：嵌套 while 200x200 输出 600 而非 40000，scan 阶段报 `vstack=-1`。

**根因**：嵌套 while 的内层循环 exit POP（条件跳转的跳转目标）在线性扫描中被当作 fall-through 路径处理，导致 vstack 被重复扣减。实际上 exit POP 是跳转目标，不应该参与线性 vstack 计算。

**修复**：将 scan 阶段的 vstack 检查从 `vstack < 0 || vstack > JIT_MAX_VSTACK` 改为仅 `vstack > JIT_MAX_VSTACK`，允许负 vstack 值。

### 8.10 JUMP\_IF\_FALSE/TRUE 跳转路径 TOS 不匹配

**现象**：嵌套 while 循环结果错误（如 200x200 输出 600 而非 40000）。

**根因**：JUMP\_IF\_FALSE/JUMP\_IF\_TRUE 的跳转目标（内层循环 exit POP）在线性 codegen 中 tos\_live=0，但跳转路径本身可能 tos\_live=1（条件值在 RAX 中）或 tos\_live=0（条件值在栈上）。

- tos\_live=1 路径：跳转时 RAX 中的条件值被"abandoned"（零成本丢弃），但跳转目标 exit POP 的 tos\_live=0 执行 `add rsp, 8`，试图弹出不存在的栈元素，导致 RSP 漂移

- tos\_live=0 路径：跳转路径上执行 `add rsp, 8` 弹出条件值，但跳转目标 exit POP 又执行一次 `add rsp, 8`，导致双重 pop

**修复**：

- tos\_live=1 路径：跳转前 `push rax` 将条件值推入栈（为跳转目标准备），fall-through 路径 tos\_live 保持 1

- tos\_live=0 路径：跳转路径不再 pop（移除 `add rsp, 8`），将条件值留在栈上供跳转目标 POP 弹出

非嵌套单层循环不受影响：跳转目标为 exit\_mc，epilogue 的 `mov rsp, rbp` 会丢弃栈上残留值。

### 8.11 Epilogue 不能用 LEAVE 指令

**现象**：JIT 函数返回时崩溃或寄存器值错乱。

**根因**：prologue 在 `push rbp` 之后压入了 `r12/r13/r14/rbx`，`LEAVE`（`mov rsp,rbp; pop rbp`）会恢复 RSP 到错误位置，弹出错误的寄存器值。

**修复**：手动恢复：`mov rsp,rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret`。

### 8.12 OP\_DICT\_SET scan vstack 计算错误

**现象**：`OP_DICT_SET` 的 callout 实现中虚拟栈深度计算错误。

**根因**：`OP_DICT_SET` 弹出 3 个值（dict, key, value），压入 1 个结果（dict），净 -2。但 scan 中 vstack 减了 -3。

**修复**：scan 和 codegen 统一为 `vstack -= 2`。

### 8.13 Bailout 后 VM 重复执行循环迭代

**现象**：JIT bail out 后，VM 从 back-edge 重新执行，导致一次循环体执行了两次。

**原因分析**：这是设计决策——bail out 时不写回 locals（保持 VM 原始状态），VM 从 back-edge 指令处重新执行循环体。这意味着 bail out 发生时的那次迭代会被 VM 重新执行一次。这是安全的（结果正确），但有微小的性能开销。

**当前状态**：设计上可接受，未修复。

### 8.14 fib\_iterative(1000) 的 3 次 Bailout

**现象**：`body_start=57` 的循环（`fib_iterative` 函数）执行 3 次 bailout 后放弃。

**根因**：`fib_iterative(1000)` 计算的斐波那契数列值在约 fib(56) 时溢出 int48 范围（2^47 ≈ 1.4×10^14），JIT 正确检测到溢出并 bail out。bail out 3 次后 JIT 停止尝试，VM 用 BigInt 继续计算。

**结论**：这是预期行为，JIT 的 int48 溢出检测正常工作。

### 8.15 通用算术/比较遇 float 直接 bailout（ripple 示例 9 次 bailout 的真凶）

**现象**：`LenoSDL3/examples/特效动画/ripple_image.leno` 报 9 次 bailout，
但 `[JIT-DEBUG] BAILOUT site=` 全部指向同一条指令（x 循环的 `OP_FOR_LOOP`），
而那条指令的 int48 检查在数学上不可能失败（循环变量 109、step 1、end 133）。

**根因**：源码第 132 行 `float tri = 1.0 - maths.abs(diff)` —— 右操作数是 native
调用返回值，类型不可静态确定，编译器发的是**通用 `OP_SUB`**，而 JIT 的通用
`OP_SUB`/`OP_ADD`/`OP_LT..GE` 只实现 int48 快路径，操作数是 float 时只能 bailout。
`site=` 指向 FOR_LOOP 是因为真正失败的守卫**不写 site**（见第 7 节历史坑）。

**修复**（2026-09-11）：见第 4 节「通用算术与比较」。用 `EMIT_NUM_TO_XMM` 把 int48
操作数 `CVTSI2SD` 提升、裸 double 直通，float 混合表达式不再回退解释器。
实测：`Executed: 424 → 5173`，`Bailouts: 9 → 3`；残留 3 次来自负步长 for，
已由后续提交 `701692d4` 的倒序 for 支持消除，最终 **`Bailouts: 0`**。
`ripple` 的内层像素循环第一次真正跑在机器码上。

**教训**：**「通用」opcode 是 JIT 覆盖率的隐形缺口**。写 `a op b` 时只要有一侧类型
不可静态确定（native 调用返回值、混合 int/float、`_int()/_float()`），编译器就发通用
opcode；JIT 若只做 int 快路径，热循环里一行普通浮点表达式就能把整个循环
永久踢出 JIT（bailout 3 次即拉黑）。

### 8.16 慢路径跳转夹在两次 TOS 消费之间（本次改动引入并修掉的 bug）

**现象**：加了 float 快路径后 bailout 归零、`Executed` 大涨，**但算出来的数是错的**：
`1.0 - |d|` 结果不对，`|d| > 100.0` 恒为 false。

**根因**：`OP_SUB` 的 codegen 原本是「取 b → 判类型 → 取 a → 判类型」，
我把慢路径跳转插在了两次 `TOS_CONSUME` 之间：

```asm
mov  r8, rdx        ; 判 b
ja   .slow          ; ← 跳走
pop  rax            ; ← 这句被跳过！慢路径却假设 RAX = 左操作数 a
```

结果从 b 的判定跳走时 `RAX` 还停在 b 上（`mov rdx, rax` 不清 RAX），
慢路径把 b 当成了 a —— 比较变成自己跟自己比（恒 false），
减法变成 `b - b = 0`。`OP_ADD` 恰好两次取值都在判定之前，所以只有它是对的——
这也是"为什么只有加法正常"的线索。

**修复**：把所有 `TOS_CONSUME` 提到任何类型判定跳转之前，并在代码里写注释固化该约束。

**教训**：codegen 里**跳转目标处需要的寄存器状态，必须在第一个跳转之前就全部就位**。
这类 bug 不会崩、只会静默算错，定位手段是 `LENO_JIT_DUMP=1` + objdump 反汇编，
直接看跳转落点处的寄存器是从哪来的（对照 13.1「生成大段机器码后必须先 dump 反汇编核对」）。

**验证方法（推荐复用）**：写一个确定性数值脚本，`lenojit x.leno` 与
`LENO_NO_JIT=1 lenojit x.leno` 的输出必须逐位一致；配合 `LENO_JIT_DEBUG=1` 确认
`Executed > 0`（否则探针根本没走 JIT，差分是假的）。

### 8.17 callout 取实参反序 —— file_manager 整个界面错位（2026-09-12）

**现象**：`leno_module/LenoSDL3/examples/应用示例/文件管理器/file_manager.leno`
界面错位：工具栏被压窄、状态栏贴在工具栏下方、splitter 高度≈0，导航树与表格行整片消失；
`LENO_NO_JIT=1` 运行完全正常。

**定位**（完整链路见 `JIT模块调用优化与bailout排查记录.md` 第 8 节）：
从界面像素反推布局分支 —— combo 宽度恰为其 `basis`（300）、按钮按 32 依次排开
⇒ `HBox._relayout` 的 `childMain()` 走的是「`free >= 0` 且 `sumGrow == 0`」分支
⇒ 累加器 `sumGrow` 不是数值。差分探针随即复现出 `sumGrow = "grow"`
—— **`.get(key, default)` 返回了键字符串本身**。

**根因**：`OP_GET_PROPERTY` + `OP_CALL` 窥孔合并成的调用 callout
（`jit_callout_get_property`）取实参方向反了。JIT 虚拟栈是「栈顶在低地址」的反向栈：

```
vstack_top[0]     = receiver      ← 编译器把 receiver 最后压栈
vstack_top[1]     = 最后一个实参
vstack_top[arg_count] = 第一个实参
```

旧实现写 `vstack_top[i + 1]`，于是 `d.get(key, def)` 实际执行 `d.get(def, key)`：
键落空 → 返回默认值。`_relayout` 里 `o.get("grow", 0.0)` 因此返回字符串 `"grow"`，
`sumGrow > 0.0` 恒假，每个子控件只拿到自己的 `basis`。

**修复**：第 i 个实参改取 `vstack_top[arg_count - i]`。三种压栈顺序的差异见第 14 节表 2。

**教训**：`vstack_top` 的下标语义必须逐个 callout 写明 ——
**「receiver 在 `vstack_top[0]`」不等于「实参从 `vstack_top[1]` 顺着排」**：
原生方法调用是实参先压、receiver 最后压，而 struct 方法（`OP_INVOKE_METHOD_TYPED`）
是 receiver 最先压。同一个 `vstack_top` 基址，取参公式完全不同。

### 8.18 类型化浮点运算不提升 int 操作数（2026-09-12）

**现象**：`acc = acc + o.get("grow", 0.0)`（`acc` 是 float 局部量、右侧类型不可静态确定）
在 JIT 下累加 600 轮，期望 200 只得 **17**；17 恰是 JIT 接管前解释执行那 49 轮的和
⇒ **JIT 段每次加的都是 0**。

**根因**：`OP_*_FLOAT` 默认操作数已是裸 double，直接 `MOVQ` + SSE 运算；但编译器只在
静态类型确定时才补 `OP_CAST_FLOAT`，动态来源（方法/模块调用返回值、数组元素、字典值）
会把 **int48 直接喂进来**：int 1 的位模式 `0x…0001` 被当成 **1e-323 次正规 double**，
加法等于加 0。比较同理（int48 位模式 ≈ 3.5e-323，与 1.0 比恒 `<`）。

**修复**：`x86_64.c` 新增 `EMIT_FLOAT_ARGS2`：int48 → `CVTSI2SD`、裸 double → 直搬、
NaN-boxed → bailout 交解释器（解释器 `val_as_num_ex`/`val_as_num` 对 BigInt 转 double、
对其余非数值按 0.0；JIT 无法区分对象种类，一律回退最稳），
覆盖 `ADD/SUB/MUL/DIV/NEG_FLOAT` 与 `EQ/LT/GT/LE/GE_FLOAT`。

**代价**：每个浮点操作数多 4-5 条判定指令。`ripple_image.leno` 每帧 13.4 → 14.8 ms
（约 5%），仍比解释器（23.2 ms）快 1.6x。

**教训**：**opcode 名字里的 FLOAT ≠ 操作数一定是 float**。判断「JIT 语义是否对齐」的
权威依据是解释器那一侧调用的转换函数：`val_as_num`（int/float，其余 0.0）
还是 `val_as_num_ex`（额外处理 BigInt）。

### 8.19 `OP_DIV_FLOAT` 除零 + 一次「自己比自己」的自伤（2026-09-12）

**固有缺口**：JIT 直接 `DIVSD`，除数为 0 得 ±inf，而解释器抛「浮点除零错误」。
（`OP_MOD` 与通用 `OP_DIV` 的除零分别由自挡 / callout 覆盖，只有类型化浮点除法漏了。）

**改动引入的自伤**：首版判零写成 `UCOMISD xmm1, xmm1` ——
**自己跟自己比恒相等 → ZF 恒 1**，于是每次浮点除法都 bailout：
`ripple` 冒出 12 次回退、4 个循环被拉黑（`Bailouts: 0 → 12`）。
靠「`git stash` 收起本次改动重建做基线对比」才确认是本次引入，而不是既有回退。

**修复**：`XORPD xmm2, xmm2` 造 0.0 后 `UCOMISD xmm1, xmm2`；
无序（NaN 除数）用 `JP` 放行（`DIVSD` 得 NaN，与解释器一致），
避免含 NaN 的热循环被整片踢回解释器。

**教训**：x86 没有「自比较判零/判 NaN」这种指令 —— `x == x` 只对 `PF`（NaN 检测）有意义，
`ZF` 永远是 1，判零必须显式准备 0.0 操作数。另外：**加 bailout 分支后必须复测
`Bailouts` 计数**（功能正确但每次都回退，同样是回归）。

### 8.20 `OP_INVOKE_METHOD_TYPED` 多返回值只按 1 个记账 —— PvZ 选卡数字每帧左右抖动（2026-09-12）

> **后记（2026-09-13，§8.32）**：本节写作时该 struct 方法调用融合指令只有 5 字节形态
> `OP_INVOKE_METHOD`（不带静态类型名）。该形态已被删除，现存唯一形态是 7 字节的
> `OP_INVOKE_METHOD_TYPED`。下述「返回值个数必须编译期解析、解析不出来就拒绝编译」的
> 结论对两者同样成立。

**症状**：`植物大战僵尸/pvz.leno` 开 JIT 时，卡片上的数字（1~5、阳光数）每帧左右乱跳；
`LENO_NO_JIT=1` 完全正常。日志里全是 `drawTextCentered`/`drawText` 的函数级 JIT 命中。

**根因**：`OP_INVOKE_METHOD_TYPED` 的 scan vstack 记账与 codegen 弹栈都写死「pop `arg_count`，
push 1 个结果」，但 struct 方法可以返回多个值。`drawTextCentered` 里

```leno
var[float, float](msw, msh) = f.measureString(text)   // Font.measureString 返回 [float, float]
float cx = x + (w - msw) / 2.0                        // msw 是脏值 → 文字水平位置乱跳
```

JIT 只留下 1 个返回值（而且落在实参槽上），于是 `msw` 读到**上一帧的栈残留**，
`cx` 每帧不同 → 文字左右抖动。注意 `OP_CALL_GLOBAL_FUNC` 早就处理了 `ret_count`，
只有 `OP_INVOKE_METHOD_TYPED` 漏了。

**修复**（4 处同步，缺一不可）：

1. `jit_scan.c` 新增 `jit_resolve_method_ret_count()`：按方法名在**编译期枚举已注册的
   struct 定义**解析 `return_count`；要求「所有同名方法返回值个数一致」，否则返回 0。
   （该函数如今是 `OP_INVOKE_METHOD_TYPED` 按静态类型名解析失败后的兜底，见 §8.32）
2. `scan_loop_body` / `scan_callee_for_inline`：`vstack -= (arg_count - ret_count)`，
   解析失败置 `capable=0`（**绝不退化成按 1 个处理**——那正是本 bug 的形态）；
3. `ops_callout.inc`：弹 `(arg_count - ret_count + 1)` 槽，保留 `ret_count - 1` 个额外返回值槽
   （与 `OP_CALL_GLOBAL_FUNC` 同构）；顺带把传给 callout 的 chunk 从 `ctx->chunk` 修正为
   `cur_chunk`（内联 callee 的方法名常量必须到 callee 常量表里取）；
4. `jit_callout.c`：`jit_callout_invoke_method` 对齐 `jit_callout_global_func`，
   把前 `ret_count-1` 个返回值写回 `vstack_top[arg_count-1-i]`；函数级 JIT 快路径限定
   `ret_count == 1`（`jit_compile_function` 本就拒收多返回值函数）。

**回归**：`assert/test_jit_multiret_method.leno`（双/三返回值 + 多返回值调用位于函数级 JIT 函数体内）。

**教训**：**同一个语义（返回值个数）在 3 个 callout 路径（global func / module call / invoke method）
各写了一遍，改一处必须扫另外两处**。另外「编译期解析不出来就拒绝编译」比「猜一个默认值」安全得多。

### 8.21 循环体内可达的 `return` 被静默丢弃 —— 五子棋 `nearStone` 恒返回 false（2026-09-12）

**症状**：五子棋 AI 对 AI 时双方都把棋子下在天元（同一格），每局都在 225 手后判平局；
关 JIT 正常。缩小到最小用例：`nearStone(g, 7, 7, 2)` 在热循环里恒返回 false。

**根因**：循环 JIT 的语义是「跑完这个循环，再把 locals 写回、回到解释器」，
**没有能力从机器码里真正返回函数**。而 codegen 里 `OP_RETURN` 的 loop 分支只写了

```c
/* Shouldn't happen in loop body, but handle gracefully */
TOS_SPILL(); vstack--; vstack = VSTACK_UNREACHABLE;
```

—— 即「spill 一下继续往下跑」。于是 `nearStone` 里的
`if g.board[rr * N + cc] != 0 { return true }` 被丢掉，函数沿循环继续走到末尾 `return false`。
`bestMove` 因此认为全盘无子，`found` 恒 false → `g.aiR=7; g.aiC=7`（空盘走天元）。

**修复**：`scan_loop_body` 在扫描到可达的 `OP_RETURN` / `OP_RETURN_MULTI` 时置
`ScanResult.has_reachable_return`，`jit_compile()` 见到就返回 NULL（日志 `scan REJECT`），
该循环交解释器。函数级 JIT（`func_mode` 的 `OP_RETURN` 是真正的返回）与内联 callee
（跳 `inline_end`）不受影响，**不能**在 scan 里一律拒绝（否则所有带 `return` 的函数都无法函数级 JIT）。

**教训**：scan 的 `dead` 标记（用 `vstack = VSTACK_UNREACHABLE` 表示「后续不可达」）只影响
**编译期记账**，不会让 codegen 停止发射代码——「不可达」不等于「不生成」。
凡是 loop JIT 语义上表达不了的指令（return 是最典型的一个），必须在 scan 阶段拒绝，不能指望 codegen 兜住。

### 8.22 `OP_NOT` 对 NaN-boxed bool 失效 —— `not <任何 bool>` 恒为真（2026-09-12）

**症状**：五子棋 AI 全部落天元（与 §8.21 同症状，但 §8.21 修完仍复现）。
逐档缩小后定位到 `OP_NOT`：

| 表达式 | JIT | 无 JIT |
| --- | --- | --- |
| `not alwaysTrue()` | **251** | 0 |
| `not callTrue()` | **251** | 0 |
| `not <局部 bool true>` | **251** | 0 |
| `not (i < 0)` | 301 | 301 |
| `not 0.0` | 301 | 301 |

（301 次迭代错 251 次 = `301 - JIT_HOT_THRESHOLD(50)`，即**错值全部来自 JIT 路径**。）

**根因**：JIT 用 **NaN-boxed** `TRUE_VAL`/`FALSE_VAL` 表示布尔（`OP_TRUE`/`OP_FALSE` 就是这么压的，
`JUMP_IF_FALSE/TRUE` 也显式比较这两个常量）。但旧的 `OP_NOT` 只区分两类：

```c
int48 → test rax,rax; sete al; movzx   // 对
其它  → movq xmm0, rax; ucomisd xmm0, 0.0; sete  // 把 NaN-boxed 值当裸 double
```

`TRUE_VAL`(0xFFFA…) / `FALSE_VAL`(0xFFF9…) / `NULL_VAL`(0xFFF8…) 的位模式作为 double 都是 **NaN**，
`UCOMISD` 遇 NaN 是 **unordered**（ZF=1），`sete` 恒得 1 —— 于是 `not <任何 bool>` 恒为真。
五子棋 `bestMove` 里 `if not nearStone(...) { continue }` 因此永远 continue，候选点恒为空。

**修复**（`ops_incdec.inc`）：`OP_NOT` 改为三态分派，结果固定产出 NaN-boxed bool，
逐条对齐解释器 `val_bool(is_falsey(v))` / `val_is_truthy()`：

| 操作数 | 判定 | 结果 |
| --- | --- | --- |
| int48 | `test rax,rax` | `0` → `TRUE_VAL`，否则 `FALSE_VAL` |
| NaN-boxed（无符号 `>= JIT_NAN_SIG`） | 与 `FALSE_VAL` / `NULL_VAL` 全值比较 | 命中 → `TRUE_VAL`；其余（对象 / `TRUE_VAL`）→ `FALSE_VAL` |
| 裸 double | `UCOMISD` vs 0.0：`JP`（NaN）→ `FALSE_VAL`，`JE` → `TRUE_VAL` | 与 `val_is_truthy` 的 `!= 0.0` 一致（NaN 为真） |

产出 NaN-boxed（而非旧的裸 0/1）同时也修掉了「`not` 结果写回 bool 槽时被解释器读成次正规 float」
这类问题（对应 §14.4-② 的同族差异）。

**影响面**：所有 JIT 循环里的 `if not <bool>` —— 包括 `if not z.alive { continue }`
（PvZ 僵尸/植物更新）、LenoSDL3 里 `if not ok { ... }` 等，此前这些分支**方向全是反的**。

**回归**：`assert/test_jit_not_bool.leno`（6 种取值形态各一个热循环 + 嵌套 `not`+`continue`）。

**教训**：JIT 里「布尔」有两个表示（裸 0/1 与 NaN-boxed `val_bool`），写任何与真假相关的
opcode 前先确认该处的表示，并对照 `val_is_truthy()`。**NaN 的无序比较（`ZF=1`）是这类 bug
的经典陷阱**，凡是「用浮点比较实现整数/布尔语义」的地方都要显式处理 `PF`。

### 8.23 `OP_DIV_INT` 把商转成 double —— 基数排序算错 + 数组越界（2026-09-12）

**症状**：`examples/排序测试/更多排序.leno` 里 8 个排序算法只有**基数排序**报
`正确: false`，并伴一条 `[运行时错误] 数组索引越界`；其余 7 个全对。`LENO_NO_JIT=1` 正常。

**定位**（差分探针，两步就到底）：

1. 复刻 `counting_sort_by_digit` 的统计循环 → JIT 得到 `395 5 397 5 393 5 394 5 396 5`，
   解释器得到正确的 `200×10`。**偶数桶约 2 倍、奇数桶恰好 5**（= 前 50 次迭代的贡献，
   即 JIT 阈值前由解释器执行的那部分）→ 说明错值全部来自 JIT 路径，且计数器被算成了别的数。
2. 逐档拆表达式：`arr[i] % 10` 对、`(arr[i] + 0) % 10` 对、`x = arr[i] / exp; x % 10` **错**、
   `arr[i] / exp` 单独看"对"（其实是被 `_str(12.0) == "12"` 骗了）。
   ⇒ 问题在 **`/` 之后**：商不是整数 → 紧接着的 `OP_MOD_INT` 拿 double 的**位模式**做整数 `idiv`。

**根因**：解释器 `OP_DIV_INT` 压 `val_int(a / b)`（int48），而 JIT 的 `OP_DIV_INT` 把商
`CVTSI2SD` 成 double 再压栈 —— 两边结果**类型**就不一样（语言语义：`100/3 == 33`、`q is int`）。

**为什么藏得住**：①`_str(12.0)` 打印成 `"12"`，单看除法结果看不出区别；
②`docs` 里 §4 那小节标题本来就写着「整数除法（返回 float）」，**文档把 bug 当成了设计**；
③只有「商又被当整数用」（取模、`is int`、位运算）时才炸。

**修复**（`ops_arith.inc`）：

* `OP_DIV_INT`：不再 `CVTSI2SD`，商直接作为 int48 压栈；补 `EMIT_INT48_CHECK`
  （`INT48_MIN / -1` 是唯一越界情形 → bailout 交解释器）；
* `OP_DIV_INT` / `OP_MOD_INT` 补**除数为 0 的守卫 → bailout**：原先直接 `idiv`，
  除零会触发 `#DE` 把进程打崩，而解释器是抛「整数/取模除零错误」
  （通用 `OP_MOD` 早有守卫，这两个类型化版本漏了；§14.3 第 7 条的要求终于补齐）。

**回归**：`assert/test_jit_int_div.leno` —— 取位串逐字符比对、向零截断、`100/3 is int`、
浮点除法不受影响、热循环内除零/模零必须**可捕获而不是崩进程**。

**教训**：**「算出来一样」不等于「一样」**——类型、位模式、后续 opcode 的假设都得对齐；
另外**文档与代码互相印证时也要怀疑**：这次正是文档把错误行为写成了规格。
差分探针要挑「能直接看见值」的输出（`_str` 会把 float 12.0 印成 12，天然掩盖问题，
换成 `%`/`is int`/位运算才暴露）。

### 8.24 比较结果必须是 val_bool，且 TRUE/FALSE 要用 **ADD** 不能 OR（2026-09-12）

本条对应 §14.4-②，同时修掉它引出的 `while` 循环早退。

**症状**（`LENO_NO_JIT=1` 全部正常）：

| 场景 | JIT | 解释器 |
| --- | --- | --- |
| `while k < 60 { … }`（任何比较做条件） | 循环第一轮就退出（计数停在热点阈值 50） | 正常跑满 |
| `for 0:59 to i { … }` | 正常 | 正常 |
| `bool b = (k >= 0)` 热循环内 | `4.9406564584124654e-324`，`is bool`→false、`_int()`→0 | `true` |
| `bool g = (k >= 0)` 全局 | `1`（int 1，丢了 bool） | `true` |
| `return (k >= 0)` | `4.9e-324` | `true` |
| `jsons.encode({"flag": k >= 0})` | `{"flag":4.9e-324}` | `{"flag":true}` |
| `int i = (k >= 0)` / `float f = (k >= 0)` | `1` / `1.0`（碰巧对） | `1` / `1.0` |

**根因（两层）**

1. **语义层**：JIT 的比较 opcode（`OP_*_INT`、`OP_*_INT_IMM`、`OP_*_FLOAT`、通用
   `OP_EQ/NEQ/LT/GT/LE/GE`）只 `setcc` 出**裸 0/1**，而解释器压的是 `val_bool`
   （`TRUE_VAL`/`FALSE_VAL`）。裸 0/1 在 JIT 内部是**歧义**的：它既是 int48 的 1，
   又会被「int48 重装箱」路径变成 int，还会被「非 int → 原样裸存」路径把 `0x1`
   直接写进 locals（解释器把它读成次正规 double 4.9e-324）。所以必须在**产生处**
   就做成 VM 的表示，而不是在消费处猜。

2. **实现陷阱（这次真正花时间的部分）**：`TRUE_VAL` 与 `FALSE_VAL` **相差
   `1<<48`，但必须用 ADD 而不是 OR**：

   ```
   FALSE_VAL = QNAN|SIGN_BIT|TAG_FALSE = 0xFFF9_000000000000   ← 第 48 位本来就是 1
   TRUE_VAL  = QNAN|SIGN_BIT|TAG_TRUE  = 0xFFFA_000000000000
   FALSE_VAL | (1<<48) == FALSE_VAL          ← OR 是空操作（错！）
   FALSE_VAL + (1<<48) == TRUE_VAL           ← 差的是**进位**，不是某一位（对）
   ```

   误用 OR 后，**比较结果恒为 `FALSE_VAL`**：于是所有 `while` 循环条件恒假、
   第一轮就退出；而 `for` 循环的条件由 `OP_FOR_LOOP` 自己比较，完全不受影响 ——
   这个「while 全挂、for 全正常」的分裂现象就是最初最迷惑人的地方。

**排查弯路（值得记住）**

反汇编出来的机器码**完全正确**（`movabs rax,FALSE_VAL; or rax,rdx` 就摆在那里，
objdump 与 dump 文件、运行时内存三者逐字节一致），但运行时 `or` 的结果就是没变。
于是先后怀疑过：rel8 补丁错位、RDX 被占用、指令缓存陈旧（还专门给
`jit_mem.h` 补了 `jit_mem_flush()`/`FlushInstructionCache`）、甚至虚拟化 CPU 模拟错误。
最后靠三步收敛：

1. **在生成代码里插探针**（`movabs r8,&g; mov [r8],rax` 写 C 全局量，不改寄存器/标志），
   逐指令打印：`op=1, rdx=1<<48, res=FALSE_VAL` —— 输入自洽、输出不变，方向已经明确
   指向「这条指令的语义不是我以为的那样」；
2. **脱离 JIT 做 14 字节最小复现**：把 `movabs rax,0xFFF9…; or rax,rdx; ret`
   直接放进 `VirtualAlloc` 的内存执行 → 结果同样是 `0xFFF9…`，换成 `add` 就是
   `0xFFFA…` —— 与 JIT 无关，是我的公式错了；
3. **手算常量位模式**：`0xFFF9` 的 bit48 = 1（`TAG_FALSE` 自身就占着这一位），
   `|1<<48` 自然不变。**教训：别凭「两个 tag 差 1<<48」就推「可以 OR 上去」，
   进位不是置位。**

**修复**

* `backend/x86_64.c`：新增 `EMIT_RAW01_TO_BOOLVAL()`（`mov rdx,rax; shl rdx,48;
  movabs rax,FALSE_VAL; **add** rax,rdx`），并在比较 opcode 的**结果产出点**调用它：
  * `ops_icmp.inc`：`OP_*_INT`、`OP_*_INT_IMM`、通用 `OP_EQ/NEQ`、通用 `OP_LT..GE`
    （后两者放在 int 快路径与 float 慢路径的**汇合点**，两边都覆盖）
  * `ops_fcmp.inc`：`OP_*_FLOAT`
* `ops_arith.inc` `OP_CAST_INT`：旧实现是「no-op（假定已经是 int）」，只在操作数确实
  是 int48 时成立。现在按解释器语义分派：int48 原样 / 裸 double `CVTTSD2SI` 向零截断
  （超出 int48 → bailout）/ `TRUE_VAL`→1 / `FALSE_VAL`→0 / `NULL_VAL` 原样 /
  其余 NaN-boxed bailout。不修它的话，`int i = (k >= 0)` 会因为比较结果变成
  NaN-boxed 而被按 int 重装箱、把位模式搅成垃圾。
* `ops_float.inc` `OP_CAST_FLOAT`：补 `TRUE_VAL`→1.0 / `FALSE_VAL`→0.0 /
  `NULL_VAL` 原样（其余 NaN-boxed 仍 bailout）—— 同时清掉 §14.4-① 的一部分。
* `backend/x86_64.c` 写回 locals：位图 bit=0（入口是 int）的分支不再盲目重装箱，
  先判「确实是 int48」，否则原样存回 —— NaN-boxed 值落进 int 槽位时不会再被搅成垃圾。
* `backend/x86_64_emit.h`：新增 `emit_cvttsd2si_r64_xmm()`。
* `jit_mem.h` / `jit.c`：补 `jit_mem_flush()`（Windows `FlushInstructionCache`），
  写完机器码后调用。这**不是**本次 bug 的原因（x86 同核自改代码由硬件保证一致），
  但它是 Windows 对生成代码的明确契约，且能挡住 DSB 陈旧 uops 这类偶发问题。

**回归**

* 新增 `assert/test_jit_bool_compare.leno`：5 种 while 条件形式（立即数/局部/不等/
  反向/浮点）、bool/int/float 局部量与全局、数组/字典/JSON 的 callout 实参路径、
  `and`/`or`/`not` 组合、返回值路径。
* `assert` **270 passed / 0 failed**（JIT 与 `LENO_NO_JIT=1` 两种模式）；
  此前因本 bug 失败的 5 个用例（`test_bigint`、`test_jit_float_ops`、
  `test_jit_float_continue`、`test_ffi_inline_widths`、`test_sorting_algorithms`）全部恢复。
* 五子棋自带 `SELFTEST`（AI 对 AI，给随机扰动加固定种子后）JIT 与解释器**逐字一致**，
  且智力梯度恢复正常（黑9 三连胜）。
* `examples/` 下 80 个示例双模式 stdout 逐字一致；`pvz.leno` 实跑 30s：
  `drawTextCentered`/`drawText` 仍被函数级 JIT 编译、无 `ret_count` 解析拒绝。

**教训**

* 「机器码反汇编正确但行为不符」时，先做**脱离 JIT 的最小字节序列复现**——
  14 个字节就能把「JIT codegen 错」和「我的公式错」分开，比盯着 1000 字节的 dump 有效得多。
* 在生成代码里插**只写全局量的探针**（不占寄存器、不改标志）能逐条指令拿到真值，
  是排查 JIT 的性价比最高的手段。
* 位运算/常量组合**手算一遍**：这次把 `OR` 当成 `ADD` 用（两者在「差 1 位」的直觉下
  看着等价），代价是几小时的排查。

### 8.25 函数级 JIT 只在「被 JIT 循环 callout」时才编译 —— 纯递归永远 1x（2026-09-13）

**现象**

`examples/性能测试/全部测试.leno` 的 `经典递归: 832040`（顶层一次性
`fib_recursive(30)`，约 2.7M 次递归调用）：

| 模式 | 耗时 |
| --- | --- |
| JIT | 93\~94 ms |
| `LENO_NO_JIT=1` | 93\~109 ms |

**加速比 1.0x** —— 开了 JIT 与纯解释器没有区别。

**根因**

`jit_func_lookup_or_compile()` 全仓只有两个调用者，都在 `jit_callout.c`
（`jit_callout_global_func` / `jit_callout_invoke_method` 的函数级 JIT 快路径）。
也就是说：**只有「被某个 JIT 热循环 callout 调用到」的函数才有机会被编译**。
`fib(30)` 的调用链起点在解释器（`main` 里一句 `fib_recursive(30)`），
而解释器的 `OP_CALL` / `OP_CALL_GLOBAL_FUNC[_TYPED]` 从不查函数级 JIT 缓存
→ 整棵递归树都在解释器里跑。

**修法**

1. `JitFuncCacheEntry` 增加 `hit_count` / `hot_disabled`（`jit_priv.h`）；
2. 新增 `jit_try_hot_func_call(closure, arg_count, typed, vm)`（`jit.c`），
   在解释器的三个调用点挂上（`op_call.inc`）。它的栈约定与 `call()` /
   `call_no_type_check` 完全一致：调用前 `[arg1..argN][callee]`，
   命中时 `sp -= arg_count`、返回值写入原 callee 槽，并同步
   `last_return_value` / `last_return_count` / `last_return_values[0]`；
   `typed` 参数用于对齐「typed 调用点不做参数提升」的既有语义；
3. 阈值 `JIT_FUNC_HOT_THRESHOLD = 50`，与循环回边阈值分开；
4. **callout 路径保持「第一次就急切编译」**，热点计数只由解释器侧自增，
   因此不回归既有的「JIT 循环调函数」性能；
5. `jit_func_lookup_or_compile()` 改为尊重 `tried`：新条目（解释器侧只建了
   计数器、还没编译）命中时必须补编译 —— 否则会把「有计数器条目」误判成
   「编译失败已缓存」，让 JIT 循环**永远不再**编译该函数（这是本改动的
   头号自伤点，`tried` 的三态语义必须记住）；
6. `jit_try_hot_func_call` 的三道保险：`jit_func_depth >= 64` 直接返回 0；
   `closure->type_param_count > 0`（泛型实例，JIT 不传类型参数）直接返回 0；
   执行返回非 0 或 `jit_callout_failed` → 置 `hot_disabled` 并复位失败标志。
7. `JIT_FUNC_CACHE_SIZE` 32 → 256：函数级 JIT 现在从解释器侧进入，同进程被编译
   的函数个数远多于从前；而 `jit_func_entry_claim` 在哈希冲突时会
   `jit_mem_free` 掉被驱逐函数的机器码，若那个函数正在 C 栈上执行
   （A 调 B，B 的 callout 又撞槽编译了 C）就是 use-after-free。

**实测**

| 场景 | 基线（`66ace270`） | 改动后 |
| --- | --- | --- |
| 顶层 `fib_recursive(30)` | 93\~94 ms | **31\~32 ms（3.0x）** |
| 热循环调 `fib_recursive(10)`×30000 | 47\~63 ms | 47\~78 ms（噪声内，无回归） |

统计新增两项可观测指标 `FuncCompiled` / `FuncExecuted`（后者只统计
**解释器侧热入口**执行次数，长期为 0 说明解释器调用点没热起来）。
上例 `FuncCompiled=1`、`FuncExecuted=115`：115 ≪ 2.7M 是**预期**的 ——
一旦进入 JIT 函数体，其内部递归全部由 callout →
`jit_func_lookup_or_compile` 命中缓存后直接调用机器码，不再经过热度计数。

**边界（三条都是硬限制，决定了后续优先级）**

| 限制 | 原因 | 表现 |
| --- | --- | --- |
| 递归深度 ≤ `JIT_FUNC_MAX_DEPTH`(64) | JIT body → callout → JIT body 是**真实 C 调用链**，超限回退解释器 | `nonTail(200000)`：47ms → 47ms，**无收益**（200000 ≫ 64） |
| 尾递归完全不覆盖 | `return f(x)` 经尾调用优化发射 `OP_TAIL_CALL`（复用当前帧），不经过新增热入口 | `isEven`/`isOdd`/`fadd`/`rep` 一个都没被编译（`LENO_JIT_DEBUG=1` 确认） |
| 不可编译的函数一次即停用 | `return_count>1` / `has_try` / 含循环回边 / 含 upvalue / 泛型实例 | 解释器调用点此后只付一次哈希+比较，不再反复试探 |

**取舍：bailout 之后不重跑是做不到的**

`jit_try_hot_func_call` 失败时把控制权交回解释器，解释器会**完整重跑**这次调用。
JIT 中途对堆的副作用（数组 append、字典写、打印）无法回滚。这与
`jit_callout_*` 快路径的既有取舍一致（见 14.4-③），也是「一次失败即
`hot_disabled`」的原因：同一份数据触发的 bailout 会稳定复现，重试没有意义。

**回归**

* 新增 `assert/test_jit_hot_func.leno`（12 个用例）：纯递归 + 结果类型、
  深度 200 > 64 的深度守卫、互递归 bool 返回值、int48 溢出回退（BigInt 语义）、
  float 形参 + int 实参提升、字符串 concat、多返回值 / 含 try 必须拒绝编译、
  「先少量解释器调用再进热循环」不得阻塞 callout 急切编译、低热度不编译、
  递归返回 Dict、函数体内含 struct 多返回值方法调用。
  该文件是**语义守卫**（改动前后都必须通过，基线版本同样 pass）；
  「JIT 是否真的执行」由 `FuncExecuted` 统计人工确认。
* `assert` **271 passed / 0 failed**（`LENO_NO_JIT=1` 同样 271）。
* `全部测试.leno` 其余各行与基线逐项一致（`arr.add` 625/641/641、
  `dict[key]` 875\~906、`arr[index]` 172\~203）。
* **示例差分（基线二进制 vs 改动版，均为 JIT 模式，逐文件比 stdout + 退出码）**：
  `examples/{func, struct, crypto, bigint, 排序测试, 闭包, 验证, try catch, switch, enum}`
  共 **205 个 `.leno`**。差异 7 处，逐一核对后**全部是计时数字**
  （`test_index_vs_field` 的 ms、`pbkdf2` 的「耗时」、
  `各种排序`/`更多排序`/`冒泡排序` 的微秒/毫秒），把计时行过滤后差异为 0；
  两处 `验证/` 用例（`doc_struct_semantics_verify`、`lang_struct_semantics_test`）
  在**两个二进制上都返回 -1**（同一运行时错误：`类型 'Node' 没有方法 'copy'`），
  非本次引入。
* **顺带发现的既有崩溃**：`examples/crypto/aes128.leno` 在 JIT 模式下栈溢出
  （退出码 `0xC00000FD`，`LENO_NO_JIT=1` 正常跑完）。
  根因不在本次改动，但已单独修掉 —— 见 §8.26。

### 8.26 内联体共用 bc_off 命名空间 —— aes128 JIT 下栈溢出（2026-09-13）

**现象**

`examples/crypto/aes128.leno` 在 JIT 模式下 `0xC00000FD`（STATUS_STACK_OVERFLOW），
stdout 停在 `--- 字符串加解密 ---` 之前；`LENO_NO_JIT=1` 完整跑完。
在 `66ace270` 上单独编译的基线二进制同样复现（同一崩溃点、同一退出码）——
**是既有 bug，不是 §8.25 引入的**，本次一并修掉。

**最小复现**

`build/probe_aes.leno`（不入库）：`gf_mul`（内部有 `for 8` + 两处 `if`）被内联到
`mix_columns` 的循环体里，循环体含 **4 个** `gf_mul` 调用点。
二分探针 `build/probe_aes2.leno` 把循环体分别缩成 **1 个**和 **2 个**内联点：

| 内联点个数 | 结果 |
| --- | --- |
| 1 | ✅ `mix1 ok` |
| 2 | ❌ 进程崩溃（`0xC00000FD`） |
| 4 | ❌ 进程崩溃（`0xC00000FD`） |

崩在第 13 次调用（`mix_columns` 的内层循环累计 50 次回边触发编译后的**首次执行**），
不是逐次累积出来的。

**根因**

`ops_callout.inc` 的内联进入点这样给内联体分配字节码偏移命名空间：

```c
bc_off = 0x10000 * inline_depth;    /* ← bug */
```

而**内联只有一层**（`FIND_INLINE_SITE` 在内联体内不会再命中，depth 恒为 1），
于是同一个循环里第 1..N 个内联点**共用同一个命名空间 `0x10000`**。

`off_map` / `ctx->patches` 都以 `bc_off` 为键，而 `offmap_lookup` 返回**第一个**
匹配项 —— 因此第 2..N 个内联体的内部跳转（`OP_JUMP_IF_FALSE` / `OP_JUMP` /
`OP_FOR_PREP` 的 skip / `OP_FOR_LOOP` 回边）全部解析到**第 1 个内联体**的机器码。
执行流于是在「第 1 个实例」与「第 N 个实例」之间来回跳，成为一个死循环；
每绕一圈 RSP 都有净变化（进入点不是该区域设计时的栈深），几十万圈后打穿
线程栈 → `0xC00000FD`。

**修法**

命名空间按**内联实例**（`inline_idx`）编号，而不是按 nesting depth：

```c
bc_off = 0x10000 * (inline_idx + 1);
```

* 调用方基址是 0，`inline_idx + 1` 从 1 开始 → 不与调用方冲突；
* `scan` 限制被内联函数体 `cc->len <= 256` ≪ `0x10000` → 各命名空间互不重叠；
* 将来若支持多层内联，需要把 depth 并入这个 id（已写在代码注释里）。

**验证**

* `build/probe_aes2.leno`：1/2/4 个内联点全部正常，`sum=5280` 与解释器一致。
* `examples/crypto/aes128.leno`：JIT 与 `LENO_NO_JIT=1` 输出**逐字一致**
  （472 字符、SHA256 相同），JIT 统计 `Compiled=14 Executed=7416 Bailouts=0`。
* 新增 `assert/test_jit_multi_inline.leno`：2 个 / 4 个内联点 + GF(2^8) 乘法，
  逐元素全文比对（期望值由解释器产出后硬编码）。
  **该断言在未修复的代码上会以 `0xC00000FD` 崩掉**（不是断言失败），
  在修复后通过。
* `assert` **272 passed / 0 failed**（`LENO_NO_JIT=1` 同样 272）。
* 14 个示例目录共 **238 个 `.leno`** 与「修复前」二进制逐文件差分：
  唯一的行为差异就是 `aes128.leno`（崩溃 → 正常），其余 5 处均为计时数字。

**顺带澄清：`PATCH-REDIRECT: target_bc=40` 是正常现象**

调试日志里每条 `while` 循环都会打印一次
`PATCH-REDIRECT: target_bc=<body_size> 未在循环体内找到 → 改为跳到 exit`。
这是**设计如此**：`while` 循环的 `OP_JUMP_IF_FALSE` 目标就在循环体之后
（`target_bc == body_size`），offmap 里自然没有这条指令，重定向到 `ctx->exit_mc`
正是正确语义。排查时不要把它当成跳转解析失败。

**教训**

* 「多个实例」类 bug 的判据是**数量**：1 个内联点正常、2 个就崩，
  比盯着反汇编快得多（`build/probe_aes2.leno` 三步定位）。
* `0x10000 * depth` 这种「按层级编号」的隔离方案，在**扁平化**的实现里
  等价于「不隔离」。给命名空间编号时要按**实例**编。
* 内联 + 内联体内有跳转/循环 = 高危组合：内联体是**复制粘贴**的机器码，
  任何以原字节码偏移为键的表都必须按实例隔离（off_map、patch 表都是）。

### 8.27 「静默降级」一律改成显式拒绝编译（2026-09-13）

§8.26 的教训是：**内联 / 大循环体路径上任何"表项写不下就丢掉"的代码，都会变成
静默生成错误机器码**（跳错地方 → 死循环 → RSP 漂移 → 栈溢出，或直接跳到
代码段开头）。本次把这类静默降级全部改成"拒绝编译"，循环退回解释器执行
（语义永远正确，只是慢）。

| 位置 | 旧行为（危险） | 新行为 |
| --- | --- | --- |
| `offmap_add`（`jit_priv.h`） | 容量判据是 `JIT_MAX_LOOP_OPS * 2`（=512），而数组实际有 `* 10`（=2560）个槽；满了**静默丢弃**条目 | 用到实际容量 2560；仍溢出则置 `off_overflow` → 收尾时拒绝编译 |
| `patch_add`（`jit_priv.h`） | 满了**静默丢弃**，那条 `rel32` 永远保持 0 → 跳转落到代码段开头 | 置 `patch_overflow` → 拒绝编译；`JIT_MAX_PATCHES` 256 → **512** |
| `OP_LOOP` / `OP_FOR_LOOP` 回边 | `offmap_lookup` 失败时**静默改成 `ctx->loop_start_mc`**（跳到外层循环起点）→ 死循环 + RSP 漂移 → 栈溢出 | 找不到目标 → 打印 `LOOP-FAIL` 并 `return 0` |
| patch 收尾（`x86_64.c`） | 找不到目标一律改成"跳到 exit" | **内联命名空间**（`target_bc >= 0x10000`）的目标必须解析，否则 `PATCH-FAIL` 拒绝；调用方命名空间里**体内**目标找不到也拒绝；只有 `target_bc >= body_size`（`break` / `while` 条件为假）才允许改道 exit |
| `scan_loop_body` 的 `fwd_targets` | `if (fwd_count < 64)`，满了**静默丢弃**前向跳转目标 → dead-code 段的 vstack 无法恢复 → 扫描与 codegen 栈深不一致 → RSP 漂移 | 上限提到 256，超出 → `scan FAIL` 拒绝整个循环 |
| `inline_ret_patches`（`ops_return.inc`） | 内联体里 `return` > 64 条时**写越界**（踩 `compile_loop` 的栈帧） | 越界即 `return 0` 拒绝编译 |

> 判据（写进 §14.3 第 18 条）：**任何以数量为界的固定数组，读不到/写不下都必须
> 让这次编译失败**，不允许"跳过这一条继续生成"。

**验证（这次没有专门造 fixture，原因见下）**

* 全量 `assert` **272 passed / 0 failed**。
* `examples/{func,struct,crypto,bigint,排序测试,闭包,array,dict,strings,maths}` +
  `性能测试/全部测试.leno` 共 **157 个文件**，带 `LENO_JIT_DEBUG=1` 逐文件跑，
  **新增的 5 条拒绝路径一次都没有触发** —— 说明加固只兜住隐患，没有误伤任何
  正常循环（这是本次改动最大的风险面）。
* `examples/crypto/aes128.leno` 仍与解释器输出逐字一致。

> 为什么不写专门的回归 fixture：触发这些上界需要「>256 个前向跳转」或
> 「>2560 条指令」的循环体（body_size 上限 4096 字节），手写 fixture 会变成
> 几百行生成代码，而它的期望行为只是"循环退回解释器、结果不变"——
> 与 `test_jit_multi_inline.leno`（真实崩溃、可小样本复现）性质不同。
> 这类加固的验证方式是：**全语料确认零误伤** + 代码审查。

### 8.28 数组越界读被静默跳过 —— 32 位字段用了 64 位加载（2026-09-13）

**现象**

```leno
var a = [1, 2, 3]
var sum = 0
var i = 0
while i < 300 {          // 先热起来
    var ix = 1
    if i > 60 { ix = 9 } // 之后访问越界下标
    sum = sum + a[ix]
    i = i + 1
}
print("sum=" + sum)
```

| 模式 | 结果 |
| --- | --- |
| `LENO_NO_JIT=1` | ✅ `数组索引越界: 索引=9, 数组长度=3` |
| JIT | ❌ `sum=122` —— **不报错，静默读数组之外的内存** |

**根因**

`OP_INDEX` 的内联快路径要用 `count` 做越界判定，代码写的是：

```c
emit_mov_reg_mem32(cb, JIT_R8, JIT_RDX, 40);   /* r8 = count ← 64 位加载！ */
emit_cmp_rr(cb, JIT_RAX, JIT_R8);
emit_jcc(cb, 0x83);                            /* JAE → slow */
```

`emit_mov_reg_mem32` 是 **REX.W + 0x8B（64 位加载）**，而 `ObjArray` 里
`count@40` 与 `capacity@44` **相邻** —— 于是 `r8 = count | (capacity << 32)`，
无符号比较 `idx < r8` 几乎永远成立，越界检查形同虚设。
（`rax` 侧是 int48 raw，负数/超界按无符号是巨值，但 `r8` 被撑到 2³² 以上，
所以连负下标也挡不住。）

**修法**

1. 新增 `emit_mov_reg32_mem8()`（32 位零扩展加载），越界判定改用它：
   `idx < count` 就是一次干净的 32 位零扩展 + 64 位无符号比较；
2. **越界改为 bailout，不再走 callout**。callout（`jit_callout_index`）只是
   `error_add_at` 记录错误然后返回 `NULL_VAL`，JIT 不检查失败标志会继续跑完循环
   —— 错误被**静默吞掉**；而解释器抛的是**可被 try/catch 捕获**的异常。实测同一段
   代码 `NO_JIT → caught=239`、`JIT（callout 版）→ caught=0`，改成 bailout 后
   JIT 也是 `caught=239` ✓。

**回归**：`assert/test_jit_array_bounds.leno` 第 1、2 段（越界 / 负下标各 239 次捕获）。
该断言在未修复代码上输出「越界捕获=0 / 负下标捕获=0」并 FAIL。

> **陷阱：`rel32` 修好的位置不能"顺路落进去"。** 第一版把越界 bailout 桩放在
> `done` 标签**处**，结果慢路径的 fall-through 也落进桩里 → 每次数组读都 bailout
> → `c[0]` 循环从 47ms 退化到 **453ms**。桩必须在 `done` 之前、并以无条件 `jmp`
> 结束（无 fall-through），`done` 紧跟其后。

### 8.29 P5：`arr.add` / `arr[i]=v` 内联快路径（2026-09-13）

**目标**（§11 P5）：数组写是真实程序里最高频的操作，而实测只有 `i++` 的 1/4~1/6
（2000 万次：`i++` ≈ 1.4ns/op、`arr[0]=1` ≈ 5.5ns/op、`add` ≈ 8.6ns/op）。

**做法**：照抄 `OP_INDEX` 读取快路径的模板（`§8.28` 已证明这个模板有效），
给两个写操作各加一条「内联快路径 + 原 callout 兜底」：

| opcode | 快路径条件 | 快路径动作 |
| --- | --- | --- |
| `OP_ARRAY_APPEND_NOPUSH`（`arr.add(v)`） | 数组对象 && `count < capacity` && 不需要写屏障 | `elements[count] = value; count++` |
| `OP_INDEX_SET_NOPUSH`（`arr[i]=v`） | 数组对象 && `i` 是 int48 && `0 <= i < capacity` && 不需要写屏障 | `elements[i] = value; if (i >= count) count = i+1` |

慢路径**原样保留**（扩容、字典/struct/cstruct 分支、全部报错文本），所以语义不变。

**写屏障**（这条决定了快路径的边界）：`gc_write_barrier(holder, value)` 的门槛是
「holder 已晋升老年代 **且** value 是年轻代对象」。快路径**只在三者同时成立时**
（value 是对象 && `arr->generation == GEN_OLD` && `value_obj->generation == GEN_YOUNG`）
才交回 callout —— 否则（绝大多数情况：元素是 int/float，或数组还年轻）直接内联完成。
判定必须在**任何变异之前**完成，这样跳到慢路径时 callout 是从原状态完整重做，
不会出现"写一半再重放"。

**实测**（`build/probe_arr.leno`，2000 万次，5 轮中位数）

| 场景 | P5 前 | P5 后 | 加速比 |
| --- | --- | --- | --- |
| `arr[0] = 1`（下标写） | 110 ms | **62 ms** | **1.8x**（最好 47ms / 2.3x） |
| `arr[0]`（下标读，本来就有快路径） | 47\~63 ms | 47\~62 ms | 1.0x |
| `arr.add(1)` 增长到 2000 万元素 | 172 ms | 156 ms | 1.1x |
| `arr.add + clear` 复用容量 | 15 ms | 15 ms | 1.0x |

`examples/性能测试/全部测试.leno` 1 亿次行：

| 行 | P5 前 | P5 后 |
| --- | --- | --- |
| `arr.add()`（真增长到 1 亿） | 641\~656 ms | **547\~563 ms**（1.17x） |
| `arr[index]` | 171\~188 ms | 171\~203 ms |
| `dict[key]=value` | 906\~922 ms | 906\~953 ms（未改动） |

**为什么 `arr.add` 只有 1.1x（而不是和下标写一样的 1.8x）**：1 亿次 append 会把数组
真涨到 1 亿元素，8 字节元素 ≈ 800MB 存储 + 约 1.6GB 的扩容 memcpy，
**内存带宽地板 ≈ 240ms**，把这部分扣掉后剩下的才是调用开销；
下标写没有扩容和内存增长，所以能看到快路径的完整收益。

**踩坑记录（两条都是"快路径静默变死代码"）**

1. **NaN-boxing 的 top16 必须用 SHR（逻辑右移）读，不能用 SAR**。
   第一版新代码写的是 `emit_sar_imm(cb, JIT_R8, 48)`：`0xFFFC0000_00000000`
   算术右移 48 位会符号扩展成 `0xFFFFFFFFFFFF_FFFC`，与 `0xFFFC` 比较**永远不等**
   → 标签检查恒失败 → 两个快路径一次都没进过（现象就是"改了但完全没效果"）。
   改用新增的 `emit_shr_imm()` 后立刻生效（下标写 110 → 62ms）。
   > 判据：**int48 判定用 SAR（需要符号位），NaN-boxing 标签判定用 SHR**。
2. **32 位字段的读/写必须用 32 位版**：`count`/`capacity` 相邻，
   读用 `emit_mov_reg32_mem8`（零扩展）、写用 `emit_mov_mem32_reg32`；
   误用 64 位版会连隔壁字段一起读/写（§8.28 就是读那条）。

**回归**：`assert/test_jit_array_bounds.leno`（越界/负下标捕获、add 后 len 与内容、
下标写覆盖与相邻元素、写入的 Value 类型、扩容不丢数据、字典慢路径仍正确）。
`assert` **273 passed / 0 failed**（`LENO_NO_JIT=1` 同样 273）；
14 个目录 **243 个 `.leno`** 与 P5 前逐文件差分：差异 10 处，
全部是计时数字或由计时派生的比值（`Speedup:` 行），**无语义差异**。

### 8.30 P5 续：struct 字段读与方法调用内联（2026-09-13，提交 `c4b6bbd1`）

**目标**：`OP_GET_FIELD_FAST`（struct 字段读）与 `OP_INVOKE_METHOD_TYPED`（struct 方法调用）
是对象密集代码的两大 callout。实测每次方法调用有 ~70ns 花在「callout 装箱 + 函数级
JIT 逐次序言」上，而这两项在循环体内每轮都要付一次。

**做法**

1. `OP_GET_FIELD_FAST` 内联快路径：`tag == 0xFFFC`（SHR 读 top16）+ `type == OBJ_STRUCT`
   + `field_idx < obj->def->field_count` 三条通过 → 机器码里直接读 `field_values[idx]`；
   任一不满足走原 callout（报错文本、越界返回 null 的语义完全不变）。
2. `OP_INVOKE_METHOD_TYPED` 内联：复用既有 `InlineSite` 机制把方法体展开进调用方循环体。
   **注意 VM 侧仍是动态分发**（字节码里的静态类型名只供 JIT 编译期去虚拟化，解释器
   仍按运行时实际类型查找），所以内联入口必须生成**接收者 def 守卫**：
   `tag → OBJ_STRUCT → obj->def == 编译期解析出的唯一 def`，任一不匹配立即 bailout
   交解释器 —— 方法查找与报错语义仍由解释器负责，**不在机器码里复刻第二份语义**。
3. 新增 `jit_resolve_method_func()`（`jit_scan.c`）：方法名在**所有已注册 def 中唯一**、
   非构造/析构、`!has_try`、单返回值、方法体 ≤256 字节、callee locals 放得下
   （`callee_lc >= arg_count`）才登记内联点；否则退回 callout（宁可不编、不要猜）。
   `InlineSite` 新增 `is_method` / `method_def` 两字段。
4. 内联体内若还有方法调用，走 callout（`FIND_INLINE_SITE` 按 bc_off 匹配，天然只有一层）。

**实测（本机 i5-3450，同机同轮 A/B，`LENO_JIT_NOINLINE=1` 用来分离两项改动）**

探针 `jit_probes/probe_method.leno`（1000 万次；探针清单与用法见 `jit_probes/README.md`）：

| 场景 | 基线 | 仅字段读内联 | 本版 | 提升 |
| --- | --- | --- | --- | --- |
| 最简方法 `return x + r` | 484\~500 ms | ~500 ms | **141\~157 ms** | **3.3x** |
| 中等方法体（无 sqrt） | 1093 ms | 1047 ms | **562\~578 ms** | **1.9x** |
| 完整 hitSingle 形态 | 1375\~1563 ms | 1281 ms | **657\~687 ms** | **2.2x** |
| 手工展开（理想上限） | 703\~719 ms | 625 ms | 625\~687 ms | — |

`examples/性能测试/光线追踪对象版.leno`：

| 阶段 | 基线 | 本版 |
| --- | --- | --- |
| A 字段访问 | 94 ms | 94 ms |
| B hit + Hit 对象 | 1266\~1343 ms | 1156\~1172 ms |
| **B2 hitSingle** | 578 ms | **297 ms（1.95x）** |
| C 递归 trace | 6016\~6094 ms | 5640\~5687 ms |

**结论**：内联后方法调用已追平「手工把方法体写进循环」（f2 657\~687 vs 展开 625\~687 ms），
说明调用机制的固定开销基本清零。B 阶段收益被对象分配吃掉（见 §8.31）。

**回归**：`assert` **273 passed / 0 failed**；59 个 struct/func/cstruct/跨模块示例
基线 vs 新版 stdout + 退出码差分 **0 处真实差异**（仅 2 处 `<ptr ...>` ASLR 地址误报，
同一二进制自身两次运行也不一致）；各阶段输出值（`sum=50730000.000273108`、
`hits=585500`）与基线逐位一致。

### 8.31 callout 包装只要 14ns —— 分配本体才是大头（2026-09-13，**未改动**，待换机续）

**动机**：上表 Phase B/C 看起来被对象分配主导（B − B2 ≈ 290ns/次），原计划给
`OP_STRUCT_INIT` 也加内联快路径。

**归因实验**（临时让 `jit_callout_struct_init` 复用首次分配结果，把「callout 包装 +
参数搬运」与「分配本体」分开；`jit_probes/probe_alloc2.leno`，N=10M，直接写在循环体里、
不经过任何方法调用）：

| 场景 | 正常 | 跳过分配本体 |
| --- | --- | --- |
| `new Pair(t, hit)`（2 字段） | 205 ns | **14 ns** |
| `new Big(a..f)`（6 字段） | 273 ns | **18 ns** |
| 数组字面量 `[1.0, 2.0]`（不池化） | 220 ns | 220 ns（不经该 callout，符合预期） |

→ **JIT 侧 callout 包装 + 参数搬运只要 14ns**。把 `OP_STRUCT_INIT` 内联进循环体
最多省这 14ns，**不值得做**；成本 100% 在 C 分配本体。

**进一步拆分**（把 `GC_POOL_MEM_LIMIT` 临时 4MB → 1GB 再测）：

| 项 | 数值 |
| --- | --- |
| `new Pair`（2 字段） | 205 → **149 ns**（池上限太小 → 退化到 `malloc`，实测省 56ns） |
| `new Big`（6 字段） | 273 → 261 ns |
| 每多一个字段 | **+28 ns/字段**（`struct_instance_new` 的默认值初始化一遍 + 实参再写一遍） |
| 其余固定开销 | **~79 ns 未归因**（`struct_def_find` 线性 strcmp + `gc_alloc` 记账 + memset） |

**根因（真正的结构性缺口）**：**JIT 循环内没有 GC 安全点**。安全点只存在于解释器的
`OP_RETURN` / `OP_RETURN_MULTI`（每 256 次），`gc_check_safe_point()` 没有被 VM 分发调用。
后果：JIT 循环内分配的对象**永远不被回收**，10M 次 `new` 实测堆涨到 ~960MB，池（4MB ≈
43K 槽）很快耗尽后每次分配退化成裸 `malloc` 且从不 free —— 这解释了「即使池放大到
1GB，单次分配仍要 149ns」。

**后续待办（按风险排序，均未做；留给性能更好的机器）**

> 完整交接（已完成的、已排除的死路、待办优先级、测量方法）见 **`docs/待办_GC与分配优化.md`**。

1. ~~**安全**：`struct_instance_new_with_args()` —— 实参给出的字段跳过一遍默认值初始化、
   每个字段只写一次（未给出的字段逐条照搬原默认值逻辑）。预计省 ~30ns/次（2 字段）到
   ~170ns（6 字段）。**解释器 `OP_STRUCT_INIT` 不动**，保留参考实现以便 JIT/解释器差分。~~
   **❌ 已实测：是负优化，已回滚。** 见下方「§8.31-a 负结果」。
2. **安全但收益小**：`GC_POOL_MEM_LIMIT` 4MB → 16/32MB（实测 64MB 折算仅省 ~4ns，
   只在容量能覆盖整段分配时才有效；本质是掩盖第 3 项）。
3. **不安全、需专门设计**：JIT 回边处加 **GC 安全点**（栈映射）。JIT 的 locals 在
   scratch 区（`frame->locals` 内是过期值）、活值还在机器栈上，触发 GC 前必须写回
   locals + 把活 vstack 溢出到 GC 可见位置并注册为根。**这是根治分配成本的前提**，
   做对需要 GC 压力测试，不能顺手改。
4. 不建议：`struct_def_find` 改哈希 —— 探针里 def 很少时收益不确定，却要动 def
   注册/覆盖/跨线程导入这套共享状态与 generation 校验。

**环境**：以上绝对值均在 **i5-3450**（Ivy Bridge，4C/4T）+ Windows 下测得，机器负载
波动大，**只做同机同轮对照，不要跨机比较绝对值**。

***

### 8.31-a 负结果：跳过「实参覆盖字段的默认值初始化」是负优化（2026-09-13）

**假设**（来自上一节的成本拆分）：`+28 ns/字段` 的一半来自「默认值初始化一遍 + 实参再写
一遍」，于是让 JIT 的 `OP_STRUCT_INIT` callout 跳过被实参覆盖字段的默认初始化
（新增 `struct_instance_new_with_args()`），预计省 ~30ns（2 字段）~170ns（6 字段）。

**实现**：把 `struct_instance_new_depth` 拆成 `struct_instance_alloc()` +
`struct_init_fields_default(def, obj, covered_mask, depth)`（位图标记要跳过的字段），
新增 `struct_instance_new_with_args()`，只改 `jit_callout_struct_init`；解释器路径不动。

**测量**（`jit_probes/probe_alloc2.leno`，N=10M；基线二进制与新版本二进制**交替跑同一轮**；
对照组是代码未改动的「数组字面量」路径）：

| 场景 | 基线 | 新版本 | 差 |
| --- | --- | --- | --- |
| `new Pair`（2 字段） | 197.8 / 200.1 ns | 204.0 / 207.5 ns | **+6 ns（慢）** |
| `new Big`（6 字段） | 265.8 / 269.6 ns | 283.7 / 291.1 ns | **+18 ns（慢）** |
| 数组字面量（对照，未改代码） | 220.2 / 219.1 ns | 219.3 / 218.8 ns | ±1.5 ns（噪声底） |

第一版实现用「索引数组 + 每字段线性扫描」判定跳过，同样更慢（Pair +8ns / Big +13ns）。
两种实现方向一致，而对照组噪声只有 ±1.5ns ⇒ **这是真实效应，不是噪声**。

**结论与纠正**：**上一节把 `+28 ns/字段` 的一半归因给「重复的默认值写」是错的。**
被省掉的只是写向已命中缓存的 store（约 0.3ns/字段，6 字段合计 ~2ns 量级），而新增的
每字段跳过判定（分支 / 掩码测试）反而打破了原来「无条件和顺序初始化」的可优化形态，
净亏。**每字段成本实际落在实参写入路径**：`jit_raw_to_value()` + 类型提升 +
`struct_set_field()`（其中 `gc_write_barrier()` 是跨 TU 调用，年轻代 holder 会早退，
但仍是一次调用）+ vstack 读取。**要压每字段成本应从这条路径下手，不要再动默认值初始化。**

**回滚**：`src/object/object_struct.c`、`src/include/leno_value.h`、`src/jit/jit_callout.c`
三处改动已全部回滚，工作区回到实验前状态。

**方法论收获**：这类「省掉一次冗余写」的微优化，**收益上限就是一次 L1 store**，
必须先算出上限再决定做不做；而 `+28 ns/字段` 是「6 字段与 2 字段的耗时差值除以 4」，
它包含实参写入、栈搬运、类型提升等**全部**每字段成本，不能当作「默认值初始化」的单价。

***

### 8.31-b 写屏障与 `struct_set_field` 内联（2026-09-13，已实施）

**动机**：§8.31-a 纠正后的成本归因指出，每字段成本落在**实参写入路径** ——
`struct_set_field()` → `gc_write_barrier()`，两者都是跨 TU 调用，在 `OP_STRUCT_INIT` /
`OP_SET_FIELD` 等热路径上每字段一次。

**改动**：把 `gc_write_barrier()` / `gc_write_barrier_obj()` / `struct_set_field()` 内联到
`leno_value.h`（函数体一字未改，只改了链接性质）；只把「确实需要入集」的慢路径
`gc_remembered_set_add()`（原 `remembered_set_add`，去重 + 扩容逻辑不变）留在 `gc.c`。
语义零变化。

**实测**（`jit_probes/probe_alloc2.leno`，N=10M，基线二进制与新版**交替同轮**跑）：

| 场景 | 基线 | 内联后 | 差 |
| --- | --- | --- | --- |
| `new Big`（6 字段） | 277.2 / 280.8 ns | **253.3 / 253.4 ns** | **−24 ns（−8.6%）** |
| `new Pair`（2 字段） | 198.5 / 209.7 ns | 194.6 / 202.2 ns | −4 ns（噪声内） |
| 不分配（对照） | 34.4 / 37.7 ns | 34.4 / 34.1 ns | 不变 |

`Big` 两轮 253.33 / 253.40 高度一致；**每字段省 ~4 ns ≈ 2 次跨 TU 调用的开销**，与
「成本在实参写入路径」的假设吻合。数组字面量路径也顺带变快（225.6/223.3 → 217.9/218.0 ns），
因为 `arr_write()` 同样走 `gc_write_barrier()`。

**正确性验证**

* `assert` **273 passed / 0 failed**；
* 对象版光追（`examples/性能测试/光线追踪对象版.leno`）确定性输出逐位一致：
  `sum=50730000.000273108`、`hits=900000`、`hits=585500`，JIT 计数也一致
  （Compiled 5 / Executed 867300 / Bailouts 0）。

**⚠️ 附带发现：写屏障的关键分支目前无法用黑盒脚本覆盖**

用 `jit_probes/gc_barrier_canary.leno`（老年代 holder 反复写入新生代 Node、写入后夹大量
分配）做**反向对照** —— 把 `gc_write_barrier()` 强制改成空操作再编译 —— 结果本用例在
`Node n` 与 `Node? n` 两种字段写法、400 万次分配下**都仍然 `bad=0`**。

**先纠正一个中途的错误判断**：一开始看到 `bad=0` 时我猜「负载根本没触发回收」，
**这是错的**。加临时 GC 探针后实测：

* GC **确实在跑**：7 次 Minor GC，每次年轻代 ≈39MB；`LENO_NO_JIT=1` 与 JIT 模式相同
  （`churn()` 每次调用都有一次 `OP_RETURN`，256 次返回就够排空一次 `deferred_gc`）。
* 屏障的**可执行分支确实被触发过**：各次 GC 入口的 `gc.remembered_count` 为
  `0, 0, 165, 1, 1, 1, 1` —— holder 晋升后稳定有 1 个老年代对象持有年轻代引用。

所以既不是「没回收」，也不是「屏障没被调用」。最可能的解释是：窗口内那个年轻 Node
**还有别的根可达**（VM / JIT 的值栈或残留操作数槽），minor GC 的 `mark_roots` 照样标记它，
与屏障无关 —— **但这个解释未经验证**。

**结论**：写屏障这条路径当时处于「**无法用黑盒脚本证明**」的状态（既有缺口，不是本次
改动引入）。`gc_barrier_canary.leno` 留在 `jit_probes/` 作为记录与验收起点，但**在
补上确定性测试钩子之前它不会报警**。
**→ ✅ 已解除（2026-09-13，§8.35）**：补上确定性 GC 钩子（`LENO_GC_YOUNG_THRESHOLD` /
`LENO_GC_FORCE_EVERY` / `LENO_GC_TRACE`）后重做金丝雀，真因是「写入与读取之间根本没发生过
回收」；改法是把回收强制塞进该窗口 + 把分配与写字段放进被调函数，使该年轻对象**只经老年代
holder 可达**。无屏障版现在会崩（末行 `rem=0`），有屏障版 6000 次回收 `bad=0`。

***

### 8.32 删除 5 字节 `OP_INVOKE_METHOD` —— 「类型名取不到」改为编译期报错（2026-09-13）

**背景**：struct 方法调用有两条融合指令形态 —— 5 字节 `OP_INVOKE_METHOD`
（`name(2) argc(2)`）与 7 字节 `OP_INVOKE_METHOD_TYPED`（多带 `struct_type_name_const(2)`）。
加 `_TYPED` 时保留了旧形态，作为「类型名常量取不到」时的回退，使 JIT 能靠
「方法名在所有 def 中唯一」的推断退化解析（§8.30 的 `jit_resolve_method_func`）。

**问题**：该回退会把一个**本该编译期确定**的类型漏到运行时，与项目一贯要求
（「能在编译时确定就别留到运行时，不确定就报错」）相悖。

**结论：回退是死路径，直接删除。** 两条硬证据（不是靠跑几个例子）：

1. **入口条件已保证类型名非空**。`codegen_expr.c` 进入融合发码块的准入条件要求接收者是
   `TYPE_STRUCT` / `TYPE_FACE` **且 `struct_name != NULL`**；原生对象（`FILE` / `SOCKET`）
   那条分支里 `method_def` 恒为 `NULL`，永远进不了发射融合指令的
   `if (has_method_def && method_def && !is_async)`。所以能到达发码点的接收者必然是
   `TYPE_STRUCT` 且 `struct_name != NULL`。
2. **`make_constant()` 永不返回负数**（`chunk_add_const()` 即使 OOM 也返回 `0`）。
   ⇒ `type_name_const < 0` 只可能来自 `struct_name == ""`；而所有构造 `TYPE_STRUCT` 的路径
   （符号表类型解析、模块 struct 导入、`AST_STRUCT_INIT` 的 struct 名）都来自非空标识符。

**实测**：273 个 `assert` 回归测试全过；另用 `-c` 编译 7 个 struct/face 方法密集的 LenoSDL3
应用（桌宠 / 文件管理器 / 文件搜索 / matrix_rain / IDE 布局 / scrollview_all / 缓存清理工具），
新的编译期报错一次都没触发。`test_jit_multiret_method` 保持 `Compiled 3 / Executed 3 /
Bailouts 0`。

**改动**

1. `codegen_expr.c`：删除 5 字节回退分支。类型名取不到时改为**编译期报错**
   （`ERR_RUNTIME`：`内部错误：struct 方法调用缺少接收者静态类型名`），不再静默发码。
2. 删除 opcode 本体及全部消费点：`leno_vm.h`（枚举）、`debug.c`（名称表 + 反汇编）、
   `vm_run.inc`（跳转表）、`op_struct.inc`（handler —— 顺带把「两形态共用公共体」合并进
   `OP_INVOKE_METHOD_TYPED` 内部，消掉 `l_invoke_method_common` 标签与 3 个外部操作数变量）、
   `jit_scan.c`（`opcode_size()` / `scan_loop_body()` / `scan_callee_for_inline()` 三处 case
   合并）、以及 `jit_priv.h` / `jit_callout.c` / `ops_callout.inc` / `x86_64.c` 的 case 与注释。
   `jit_resolve_method_func()` / `jit_resolve_method_ret_count()` **保留** —— 它们是 `_TYPED`
   解析失败时的兜底，仍在用。
3. **opcode 重新编号**：`OP_INVOKE_METHOD` 之后还有 6 个 opcode，删除后整体前移 1。按
   `leno_serialize.h` 自己的规则 bump 版本：`LENO_BIN_VERSION 2.4.0 → 2.5.0`、
   `LENO_MODCACHE_VERSION 3 → 4`，否则旧的 `entry_*.lenb` / `*.lenomc` 会按
   `magic + version` 校验通过、加载含错位 opcode 的字节码并跳转发散（`0xC0000005`）。
   `.lenosymc` 只存符号表、不含 opcode，无需 bump。

**教训**：**「回退分支」也是行为，会被写进字节码契约**。一个「理论上不会走到」的兼容
回退，如果它发的是**不同长度**的指令，就等于把不确定性固化进了字节码；删掉它、把不确定
变成编译期报错，比留着「以防万一」更安全 —— 前提是先证明它是死路径（入口条件 + 返回值
域），而不是靠跑几个样例。

***

### 8.33 `OP_INDEX` 慢路径落进越界 bailout 桩 —— 含 `d[k]`/`obj["field"]` 的循环永远跑不进 JIT（2026-09-13）

**发现路径**：matrix_rain 的 JIT 统计里 `invalidateRenderer` **每次调用都 bailout 一次**
（`loop_bc=48`、`非溢出类 @bc_off=59`）。`-c --debug-out` 定位到 bc_off 59 = `OP_INDEX`
—— 源码是 `_texCache[i].ren`，元素静态类型不明 ⇒ 编译成 `OP_INDEX`（`struct["ren"]`）而非
`OP_GET_FIELD`。

**根因**：`ops_index.inc` 的 `OP_INDEX` 慢路径（obj 不是数组：dict 取键 / struct 按字段名 /
非 int 索引）在 `jit_callout_index` + `EMIT_VALUE_TO_RAW()` 之后**没有跳转**，
直接落进紧随其后的「数组越界 → bailout 桩」：

```c
EMIT_VALUE_TO_RAW();
/* 数组越界 → bailout 桩 */
patch_rel32(cb, p_idx_oob, cb->len);     /* ← 慢路径落到这里 */
EMIT_BAILOUT_SITE_NONOVF(bc_off);
{ int bp = emit_jmp(cb); patch_add(ctx, bp, -1, 0); }
/* done: result in RAX = live TOS */
patch_rel32(cb, done_loc, cb->len);
```

**影响**：任何含 `d[k]` 或 `obj["field"]`（元素静态类型不明）的循环
**每次执行都 bailout，永远跑不进 JIT**。而 bailout 是安全的（解释器完整重跑本轮、
结果正确），所以表现为「**静默地永远不优化**」—— 不报错、不算错，只能靠 `Bailouts`
计数发现。**这一类缺口的排查直觉：先看 Bailouts 是否为 0，再谈耗时。**

**修复**（`src/jit/backend/x86_inc/ops_index.inc`，+7 行）：慢路径末尾补一条 `jmp done`，
并在 `done` 处一并 patch：

```c
EMIT_VALUE_TO_RAW();
int slow_done_loc = emit_jmp(cb);        /* 慢路径跳过越界桩直达 done */
/* 数组越界 → bailout 桩 */
patch_rel32(cb, p_idx_oob, cb->len);
EMIT_BAILOUT_SITE_NONOVF(bc_off);
{ int bp = emit_jmp(cb); patch_add(ctx, bp, -1, 0); }
/* done */
patch_rel32(cb, done_loc, cb->len);
patch_rel32(cb, slow_done_loc, cb->len);
TOS_PRODUCE();
```

**验证**

* 回归探针 `jit_probes/probe_index_slowpath.leno`：`Bailouts` **9 → 3**；
  `useStructName` 的两个循环各 3 次 → **0**；`Executed` **12 → 30**（循环真正在 JIT 里跑了）；
  结果不变（`stname=1000 dict=1000.0`）。
* `assert` **273 passed / 0 failed**。
* matrix_rain 实跑正常收尾：`总帧数 2001，整体平均 183.3fps / 5.4ms`
  （与修复前及用户侧 184.8 / 5.4 同口径，无回归）。

**修复后同一循环仍有 bailout**：位置从 `bc_off=59` 移到 `63` = `OP_EQ`，
比较两个 `Ptr[u8]`。这一处已在 **§8.34** 用「运行时身份比较快路径」解决
（matrix_rain 的 `Bailouts` 3 → 0）；过程中还**否掉了原先设想的「codegen 按静态类型发
专用比较指令」方案** —— 原因见 §8.34（`ffi.nullptr()` 包装 NULL 导致"纯身份"不安全）。

**顺带发现 → ✅ 已解释（2026-09-15 复测，与 §8.67 同源）**：`s = s + d["k"]`（dict 读取在
热循环内）当时仍会 bailout，但位置是 **`OP_ADD_FLOAT`** 而非 `OP_GET_PROPERTY`
（`d["k"]` 编译成 `OP_GET_PROPERTY`，该 callout 对 dict 是支持的 `dict_get`、且调用后做了
`EMIT_VALUE_TO_RAW()`，按理浮点操作数不该被判成 NaN-boxed）。

真因与 **§8.67 / roadmap R12** 同一个：`ops_callout.inc` 的 `case OP_GET_PROPERTY`
**独立访问形态漏弹 receiver 槽**（`pop_bytes: 0`，修复见提交 `5f82acc1`）
⇒ 编译期 vstack 模型与运行时内存栈**错位**（每执行一次多留 8 字节）
⇒ 后续操作数从**错槽**读出 ⇒ `OP_ADD_FLOAT` 的 int48 检查不过而 bailout。
`5f82acc1` 的提交说明已记「A 形态（`s + box["p"].a`）修前每次 3 次 int48 溢出/截断 bailout，
修后归零」—— 与本条是同一现象。

复测（2026-09-15）：`build\leno.exe jit_probes\probe_index_slowpath.leno` →
`stname= 1000  dict= 1000.0`、`Compiled: 3 / Executed: 28 / **Bailouts: 0**`，
原判据「`useDict` 的 `Bailouts` 归零」达成。⚠ **未做**单独的回退对照（把 `pop_bytes`
改回 0 再复测该探针），故记为"同源/同一现象"，不再作为待办跟踪。

**教训**：写完一处 bailout 桩后，必须确认**所有**能到达它的路径都显式跳走 ——
「桩没有 fall-through」这句话对*桩自身*成立，但对**桩前面紧邻的代码**不成立。
建议顺手扫一遍 `ops_callout.inc` / `ops_index.inc` 里所有
`EMIT_BAILOUT_SITE_*` 紧跟无条件 `jmp` 的位置，确认前方没有会误落的路径。

***

### 8.34 `OP_EQ` 身份比较快路径 —— 指针/对象身份比较不再 bailout（2026-09-13）

**背景**：§8.33 修掉 `OP_INDEX` 后，matrix_rain 同一个循环的 bailout 移到了 `bc_off=63`
= `OP_EQ`，比较两个 `Ptr[u8]`（源码 `_texCache[i].ren == renderer`）。
`ops_icmp.inc` 的通用 `OP_EQ/OP_NEQ` 对「任一 NaN-boxed」一律 bailout。

**先否掉一个方案：不做「按静态类型发专用身份比较 opcode」**

原设想是让 codegen 在静态类型已知时发一条专用身份比较指令。核实解释器语义后发现**不安全**：

`Ptr[T]` 是 `ObjFFIPointer`，而 `ffi.nullptr()`（`src/module/ffi/ffi.c`）与
`ffi.ptr_from_int(0)` 会产生**包装 NULL 的 `ObjFFIPointer`**（真对象，不是 `val_null`）。
解释器对 `null == Ptr` 走的是「看包装地址 `->ptr == NULL`」的**特殊规则**
（`op_compare.inc`）—— 此时为 **true**，而纯身份比较（位比较）会给 **false**。

于是三条路都不通：

* 新 opcode 做「纯身份比较」⇒ **静默改变** `ffi.nullptr() == null` 的语义；
* 把 FFI 的 null 规则也搬进新 opcode ⇒ 它就不是身份比较了，等于把解释器那套复制一份，
  还要额外承担 opcode 重编号 + `LENO_BIN_VERSION` / `LENO_MODCACHE_VERSION` bump；
* 让 codegen 用谓词排除「可能是 Ptr 的一侧」⇒ 恰好排除了发起这件事的场景，白做。

**最终做法：判定放在运行时（改 `ops_icmp.inc`，零 opcode / 版本改动）**

在 `OP_EQ/OP_NEQ` 的「任一 NaN-boxed → bailout」**之前**插一条快路径：

> 两个操作数都是 OBJ（top16 == `0xFFFC`）、`obj->type` 相同、
> 且类型 ∉ {`OBJ_STRING`(0), `OBJ_ARRAY`(1), `OBJ_BIGINT`(7)} → **值位比较**（= 对象身份）

依据解释器规则：两个 OBJ 值同类型时，只有 STRING 按内容、ARRAY 逐元素，
**其余一切对象都是身份比较**（含 struct / dict / Ptr）。不满足条件一律**落到原有数值慢路径**，
那里对 NaN-boxed 仍 bailout —— 所以不可能静默算错，只是这几类仍走解释器。

**验证**

* 差分测试 `jit_probes/probe_eq_identity.leno`：**JIT 与 `LENO_NO_JIT=1` 结果逐条一致** ——
  `sameObj=2000 / diffObj=0 / neq=2000 / nullPtr=2000 / str=2000 / arr=2000 / struct=2000`。
* bailout 归属正确：`c_str`（按内容）、`c_arr`（逐元素）、`c_nullPtr`（`ffi.nullptr() == null`）
  **仍 bailout**（语义没被吞掉）；`c_sameObj` / `c_diffObj` / `c_neq` / `c_struct` **不再 bailout**。
* `assert` **273 passed / 0 failed**。
* **matrix_rain `Bailouts` 3 → 0**（`Executed` 11204），整体平均 187.1fps / 5.3ms（无回归）。

**教训**：打算用「编译期类型谓词」把手写语义下沉成新指令之前，**必须先把解释器那段代码的
全部分支读完**。这次的关键分支（`null == Ptr` 看包装地址）就藏在 `OP_EQ` 开头那四条
`val_is_null(...) && OBJ_FFI_POINTER/CALLBACK` 特例里 —— 只读「对象按指针比较」那句注释
会直接漏掉它。**先否定法：能用运行时判定 + 落回旧路径表达的语义，就不要抬进编译期。**

***

### 8.35 确定性触发 GC 的测试钩子 —— 并顺势查出金丝雀为什么不敏感（2026-09-13）

**目标**（来自 `docs/待办_GC与分配优化.md` 的 P0）：让「回收什么时候发生」可控。
此前两件事卡在它上面：① 写屏障的关键分支无法用黑盒脚本证明；② GC 压力/差分验证没法做。

**实现**（全部默认关闭。设计约束：只改变「已有安全点何时决定回收」，**不新增 GC 调用点**，
因此不会在「GC 看不见 JIT 活值」的位置触发回收 —— 那是 §12 第 10 条 / P1 的范围）：

| 环境变量 | 作用 |
| --- | --- |
| `LENO_GC_YOUNG_THRESHOLD=<bytes\|KB\|MB>` | 覆盖并**钉住**年轻代阈值（`young_threshold_pinned`）。`gc_minor_collect` / `gc_major_collect` 结尾会把它抬到 `max(young_allocated*2, GC_YOUNG_THRESHOLD)` —— 不钉住的话覆盖值在第一次回收后立刻失效。 |
| `LENO_GC_FORCE_EVERY=<n>` | 每 n 次分配把「强制回收」请求挂到 `vm.gc_force_request`，在下一个 `OP_RETURN` / `OP_RETURN_MULTI` 由 `gc_force_collect()` 无条件回收一次（不受阈值门控）⇒ 回收次数按分配次数确定。 |
| `LENO_GC_TRACE=1` | 每次回收打一行 `[GC] minor #N young=..KB old=..KB rem=R freed=F promoted=P thr=..KB`。 |

为了 trace，把 `sweep_young` / `sweep_old` 里**已有但只算了没用**的 `freed_cnt` / `promote_cnt`
落到 `gc.last_freed` / `gc.last_promoted`（`sweep_old` 累加），并新增 `major_gc_count`。

顺带清理：**删除 `gc_check_safe_point()`** —— 它只做「`young_allocated > threshold` 就置
`deferred_gc`」，与 `gc_alloc` 开头逐字重复，且 `src/` 下零调用点（死代码）。
请求标志放在 `VM` 结构体而非 `THREAD_LOCAL gc`：安全点在 `OP_RETURN`，那里读 vm
字段可避免 TLS 访问（与 `gc_return_counter` 同一取舍）。

**顺带查清了金丝雀不敏感的真因**（§8.31-b 里那条「未验证的解释」）：

旧金丝雀在 `main` 里直接写 `h.n = new Node(...)`，然后 `churn` 两次就读，
结果**无论屏障是否生效都 `bad=0`**。加上钩子后测出真因是两件事，第一件是决定性的：

1. **写入与读取之间根本没发生过回收**。自然回收间隔 = 「256 次返回 ×
   每次 `churn` 2000 次分配 = 512K 次分配」（trace 里每次 `freed=512128` 正是这个数），
   而写入→读取只隔 2 次 `churn`（4000 次分配）。窗口太短 ⇒ 目标对象永远是
   「刚分配、还没被回收过」的有效对象，读什么都是对。
2. `main` 帧的残余临时/局部槽可能让 `mark_roots` 保守保活它（此条仍未验证，
   但已不是必要条件 —— 修掉第 1 条就够了）。

**修法**（`jit_probes/gc_barrier_canary.leno` 重做）：
- 用 `LENO_GC_FORCE_EVERY` 把回收**强制塞进**「写入与读取之间」；
- 把「分配 + 写字段」一起放进 `setHolder(h)`，返回后该帧消失 ⇒
  这个年轻 Node 只经老年代 holder 可达，**屏障是它唯一的保活路径**。

**验收（同机、两个只差屏障的二进制）**：

| 版本 | 模式 | 强制回收次数 | 结果 |
| --- | --- | --- | --- |
| 有屏障 | `LENO_NO_JIT=1` | 6000 | `bad=0`，跑完 |
| 有屏障 | JIT | 4050 | `bad=0`，跑完 |
| 无屏障（`gc_write_barrier` 改空操作） | `LENO_NO_JIT=1` | 第 2002 次回收处崩 | 退出码 −1，stdout 无 `bad=`，末行 `rem=0` |
| 无屏障 | JIT | 第 52 次回收处崩 | 同上 |

`rem`（`remembered_count`）就是分水岭：有屏障时 holder 在集合里（`rem >= 1`），
无屏障时恒为 `rem=0`。⇒ **写屏障的关键分支现在可以用黑盒脚本证明了**，
§8.31-b 那条「无法证明」的状态解除。

**钩子自身的限制（必须知道，也是 P1 的同一根因）**：强制请求只在**解释器安全点**兑现。
JIT 收编热循环后没有返回值，请求会被推迟 —— 实测预热循环（2000 次 `churn` =
400 万次分配）全部进 JIT 后，第一次回收一次性 `freed=4412101`，说明这期间**一次安全点都没到**。
所以测 GC 时若要确定性最强，加 `LENO_NO_JIT=1`；要在 JIT 下也确定，得等 P1
（callout 边界安全点）。

**回归**：`assert` **273 passed / 0 failed**（JIT 与 `LENO_NO_JIT=1`），钩子开启时同样 273/0；
13 个分配密集示例（`光线追踪对象版` + `examples/struct`）钩子开启 vs 关闭差分，
差异只有计时数字，确定性载荷逐位一致（`sum=50730000.000273108` / `hits=900000` /
`hits=585500`）；基准无回归（`i++` 1 亿次 125\~141ms、`经典递归` 31\~32ms、
`arr.add` 547\~563ms、`arr[index]` 172\~187ms、`dict[key]=` 859\~891ms）。

**性能注**：`LENO_GC_FORCE_EVERY` 在 `gc_alloc` 里只多一次可预测的 load + branch（默认 0，早退）；
`OP_RETURN` 上多一次 `vm.gc_force_request` 的 load + branch（与 `gc_return_counter`
同一条 cache line）。

***

### 8.36 P1 动手前的核实：JIT 活值不可见，改推荐「回边让出」（2026-09-13，**未改动代码**）

`docs/待办_GC与分配优化.md` 的 P1 建议走「路线 2：callout 边界安全点」，并列出两条动手前
必须核实的事。核实结果如下，**结论是路线 2 的前提不成立，改走路线 3**。

**核实 1：JIT 的活值在 GC 眼里可见吗？——不可见。**

* JIT 序言把 locals 从 `frame->locals`（RCX）**复制进自己的机器栈帧**：
  `mov rax,[rcx+slot*8]` → 类型分流 → `mov [rbp+disp],rax`（`scratch_disp(i) = -8*(i+1)`）；
  执行期的局部量读写（`OP_GET_LOCAL` / `OP_SET_LOCAL[_POP]` / `OP_MOVE_LOCAL*` /
  `OP_INC_LOCAL_NOPUSH` …）全部走 RBP 相对地址。vstack 紧随其后
  （`frame_sz = total_locals*8 + max_vstack*8 + 16 + 24`）。
* `EMIT_WRITEBACK_LOCALS` 只在 **exit / framedead** 块发射（`grep WRITEBACK` 在
  `src/jit/backend` 只有 3 处命中）⇒ 整个循环期间 `frame->locals` 是**进入循环时的快照**。
* `mark_roots` 扫的是 `vm.stack[0..sp)` 与各帧 `locals[0..local_count)`，都够不到机器栈区。

**⇒ 路线 2 的「发布」不只是写回 locals。** 还得把 JIT 的 vstack 变成 GC 可见的根，而 vstack
里放的是**裸值**（int48 / 原始 double），不是 NaN-boxed `Value`：直接按 `Value` 扫会被 tag
误判（`mark_roots` 步骤 1.5 的注释就警告过「未初始化栈内存里的垃圾值可能被 NaN-boxing
判成对象指针」）。要发布就得**逐槽做 raw→Value 转换**写进一个新的、登记为根的缓冲 ——
比 P1 表里那句「TOS 本来就要 spill」贵得多，也更容易出错。

**核实 2：`jit_reloaded_locals` 与 `frame->locals` 的语义** —— 执行期 `frame->locals` 过期
（见上）；`jit_reloaded_locals` 只是 callout 之后回读 `vm.frames[frame_cnt-1].locals`
（防 `vm_grow_frames` 重分配后 RCX 悬垂），**不参与发布**。

**实测：缺口有多大、什么条件下出现**（`jit_probes/probe_jit_gc_safepoint.leno` + §8.35 钩子）

用 `LENO_GC_FORCE_EVERY=1`（每次分配都挂请求）数「实际到达的安全点」：

| 场景 | 强制回收次数 |
| --- | --- |
| `LENO_NO_JIT=1` | 4001（= 4000 次被调函数返回 + 1） |
| JIT（默认，内联开） | **101** |
| JIT + `LENO_JIT_NOINLINE=1` | 4007 |

⇒ 默认配置下，JIT 热循环里的 4000 次调用**一次安全点都没到**：被调函数被内联进循环体、
不再经过 callout，也就没有 `OP_RETURN`。这正是「循环内分配的对象无人回收」的机制。
（纯算术循环同理：没有 callout 就没有安全点。而只要循环里有分配，就一定有一次
`OP_STRUCT_INIT` 之类的 callout —— 所以「在 callout 边界检查」这个方向本身是对的。）

**一个仍未解释的观察（别把当"已经有保护"）**

`NOINLINE` 下安全点确实到了（4007 次回收都在 JIT 循环期间），但
`probe_jit_gc_safepoint.leno` 测出**当前迭代的 `n` 没有被回收**：值判据 `bad=0`，
身份判据 `reused=0`（身份判据本身已单独验证可靠：`build/probe_identity_check.leno`
在 JIT 热循环里 `same=100000 wrongly_equal=0`）。

根来源归属（`LENO_GC_TRACE` 新增的 `roots(...)` 字段）显示：每次回收只有 2 个年轻对象
存活，**全部来自调用帧**（`roots(stk=0 mod=0 frame=2 …)`），且 `freed` 恰好等于
「窗口分配量 − 1」（紧凑窗口探针 `freed=200`；带 4000 次探测分配的探针 `freed=4199`）。
按代码，`frame->locals` 里应该是**过期**的那个对象，可实测的存活者却是当前那个。

**⇒ 在查出这 1 个存活者是谁之前，不能认为 JIT 活值已被覆盖。** 任何「在 JIT 循环里原地
触发回收」的方案（路线 1/2）都必须自己先发布活值，否则就是拿 use-after-free 换吞吐。

**因此改推荐路线 3：回边让出（yield），把回收搬回解释器**

| | 路线 3：回边让出（建议） |
| --- | --- |
| 位置 | JIT 回边处每 N 个回边让出一次，走**已有的出口路径**（写回 locals → 平衡/丢弃 vstack → epilogue），返回新码（如 4） |
| 解释器侧 | `op_jump.inc` 的 `OP_LOOP`：`jit_r == 4` → 重载 frame → `frame->ip -= offset`（跳回循环头）→ **在这里按需回收**（`gc.deferred_gc` → `gc_try_collect_deferred()`）→ DISPATCH |
| 为什么安全 | 回收发生在解释器里，而 `frame->locals` / `vm.stack` 刚被**出口路径**发布过 —— 复用一条跑了很久、已被 273 个回归覆盖的路径，**不需要发明新的发布机制** |
| 成本控制 | **只对「循环体内可能出现分配」的循环发射回边检查**（scan 阶段已能判定），纯算术热循环零开销 —— 避免给 `i++`（≈2.5 cycle/iter）加 dec+jnz 这种百分比级别的税 |
| 判据 | `probe_jit_gc_safepoint.leno`（默认内联配置）的回收次数从 ~101 升到与分配量匹配；`probe_alloc2.leno` 长跑堆曲线不再单调上涨；`assert` 273/0；`i++` / `经典递归` 无回归 |

**状态：⬜ 未实现**（本轮只做到「核实 + 定方案」）。顺带落地的诊断设施：`LENO_GC_TRACE`
新增 `roots(stk/mod/frame/glb/misc/oth)` 与 `marked` 字段（`gc.dbg_roots` / `gc.mark_new`，
仅在 trace 打开时维护 —— `gc_mark_object` 里那一次 `if (gc.trace)` 是不打开就不执行的
可预测分支，实测基准无回归）。

***

### 8.37 两条「让 JIT 循环触发回收」的尝试：一条硬化保留、一条负结果（2026-09-13）

**已保留（本地化改动）：「JIT 帧内禁止同步回收」+ 请求 bailout 退到解释器**

`gc_alloc` 的分配失败路径原本会**同步** `gc_major_collect()`。这条路径在 JIT 帧里
同样可达（正是"池耗尽退化 malloc"的下一阶段），而 JIT 活值 GC 看不见（§8.36）
⇒ 静默 use-after-free。改动：

* 新增 `jit_loop_depth` / `jit_in_frame()`（`jit_loop_depth` 在 `jit_try_hot_loop`
  执行机器码期间自增；`jit_func_depth` 原义不变）；
* `gc_alloc` 失败时若 `jit_in_frame()` → 只置 `gc.deferred_gc` + `jit_request_bailout()`，
  **不同步回收**；callout 返回 NULL 后 JIT 因 `jit_callout_failed` 走 bailout，
  解释器重跑本轮时再同步回收并重试（若回收救得回来，功能不降级）；
* 修掉两处空指针解引用：`struct_instance_new_depth()` 直接用 `gc_alloc` 返回值
  （`obj->def = def`）、`jit_callout_struct_init()` 直接用 `struct_instance_new`
  返回值 —— 分配失败时都会崩。

**负结果：不能用 bailout 做「周期性中断」让 JIT 循环回收**

思路：`gc_alloc` 跨年轻代阈值时（用 `deferred_gc` 的 0→1 锁存保证频率上界）
置 `vm.gc_force_request` 并 `jit_request_bailout()`；解释器在新增的**回边安全点**
（`OP_LOOP` / `OP_FOR_LOOP`）消费该标志并回收。这样零 codegen 改动、不需要发布
JIT 活值，看起来是最小解。

**它不成立，实测数据（`jit_probes/probe_jit_gc_safepoint.leno`，默认内联配置，
`LENO_GC_YOUNG_THRESHOLD=64KB`）**：

| 指标 | 结果 |
| --- | --- |
| 回收次数 | **6,361,951**（每次 `freed=821`、`marked=1`，即同一个迭代被反复重跑） |
| JIT 进入次数 | 6,367,902（每次进入立刻回退） |
| 程序耗时 | **428s**（原 ~3s） |
| 结果正确性 | `bad=0`（值没错，但循环计数几乎不前进） |

**根因（本轮新查清的事实，很重要）**：**bailout 不是迭代边界**。

* JIT 只在 **exit / framedead** 块写回 locals（§8.36 核实 2 已确认 `EMIT_WRITEBACK_LOCALS`
  只有这两处）；**bailout 路径不写回**。
* 而 JIT 被进入一次会在机器码内部跑**很多轮**迭代 ⇒ bailout 发生时 `frame->locals`
  还停在「JIT 进入时」的状态 ⇒ 解释器 `frame->ip -= offset` 后是**从那个旧状态
  重跑整个循环**（不是重跑当前迭代）。
* 于是「同一位置可复现的 bailout」= 无限重放 + 副作用重复。现有代码唯一的刹车是
  `JIT_BAILOUT_LIMIT=3`（3 次后永久放弃此循环）；而 GC 回退为了不把预算烧光
  **特意不计 bailout**，恰好绕过了这道闸 ⇒ 死循环。
* 这同时解释了 §14.4-③ 那条「bailout 后堆侧副作用无法回滚」的**机制来源**：
  不是"重跑一轮"，而是"从 JIT 进入点重跑"。

**⇒ 结论：路线 3（回边让出）是唯一可行的形式，理由现在是具体的**：

* 回边是**迭代边界**，且回边处 **vstack 平衡**（`OP_LOOP` 的 codegen 开头就 `TOS_SPILL()`，
  循环体净栈效果为 0）⇒ 可以走**已存在的 exit 路径**（`add rsp, vstack*8` +
  `EMIT_RELOAD_RCX` + `EMIT_WRITEBACK_LOCALS`）让出，`frame->locals` 被正确发布，
  解释器从**循环头**继续，语义与"刚执行完一次回边"完全等价；
* 让出码必须**单独计数**（不消耗 `JIT_BAILOUT_LIMIT`），但因为它发布状态、不重放，
  所以不会出现上面那种死循环；
* 实现点：JIT 自己的那条回边上加 `dec 计数; jz yield`，yield 段照抄 exit 块但返回
  新码（如 4）；`jit_try_hot_loop` 把 4 单独映射（不计 bailout）；`op_jump.inc` /
  `op_for_loop.inc` 在 `jit_r == 4` 时 `frame->ip -= offset`，并在那里消费
  `gc_force_request` 回收（**本轮已把回边安全点接好**，见下）。

**本轮保留的小改进：回边安全点**

`OP_LOOP` / `OP_FOR_LOOP` 处理器开头各加一次
`if (unlikely(vm.gc_force_request)) { vm.gc_force_request = 0; gc_force_collect(); }`：

* 只读 VM 结构体字段（非 TLS），实测基准无回归；
* 为 `LENO_GC_FORCE_EVERY`（§8.35）增加了兑现点（以前只在 `OP_RETURN` 兑现）。
  **但别高估它**：它只覆盖"解释器执行到回边"的情形。实测
  `probe_jit_gc_safepoint.leno`（默认内联配置、`FORCE_EVERY=500`）只从 70 次升到
  **91 次** —— 被内联的 JIT 循环在机器码内部跑完整个循环，解释器压根到不了回边，
  所以**机器码里的安全点仍然只能靠路线 3**；
* 它的真正价值是：路线 3 落地时，让出后的收集点就在这里。

**状态**：硬化 ✅ 已实施并随 273/0 回归；路线 3 ✅ **已实施**（见 §8.38）。

**参考实现侧的依据**：HotSpot 的 poll 就放在**回边 / 返回前 / 调用后 / 抛异常**，
且它的"回退"是**按 ScopeDesc/OopMap 把编译帧重构成 bci 精确的解释器帧**（不是重跑）；
LuaJIT 则靠"**栈槽是 GC 的权威副本** + snapshot 重建退出点状态"，从不扫机器码寄存器。
我们两条都没有 ⇒ 「活值不可见」和「bailout 只能重跑」都是**缺元数据**的必然结果。
详见 `docs/JIT安全点与去优化_参考实现调研.md`（含来源与推论标注）。

***

### 8.38 P1 路线 3「回边让出」实施完成（2026-09-13）

**做了什么（4 处，零元数据）**

| 位置 | 内容 |
| --- | --- |
| `gc.c` / `gc_alloc` | JIT 帧内（`jit_in_frame()`）跨年轻代阈值 → 置 `jit_gc_yield_flag` |
| `x86_64.c` | 新增 **yield 出口块**：照抄 exit 块（平衡 vstack + 重载 RCX + 写回 locals），差别只有返回码 **4**；`EMIT_CALLOUT_BEGIN()` 里置 `body_has_callout`（内联体里的 callout 也算） |
| `ops_loop.inc` | `OP_LOOP` / `OP_FOR_LOOP` 的**自己那条回边**上 2 条指令轮询（`mov r8, imm64(&flag); cmp [r8],0` → `jnz` yield 块），仅 `body_has_callout` 时发射 |
| `jit.c` + `op_jump.inc` / `op_for_loop.inc` | 返回码 4 单独映射（计入 `jit_state.yield_count`、**不计 bailout**）；解释器 `jit_r == 4` → 清零标志 → `gc_try_collect_deferred()` → 落回原有续跑路径 |

**为什么这样安全**（与 §8.37 的负结果对照）：回边是**迭代边界**，且回边处 vstack 平衡
（`OP_LOOP` 的 codegen 开头就 `TOS_SPILL()`），所以可以被出口路径完整发布；解释器
`frame->ip -= offset` 恰好从**循环头**继续，语义等价于「刚执行完一次回边」——
不重放、不重复副作用。bailout 这两点都不满足（§8.37）。

**实现中踩到并修掉的三条硬约束**（都写进代码注释了）

1. **只认 JIT 自己那条回边**（`bc_off + size == sr->body_size`）。嵌套内层回边与**内联
   callee 里的回边**都不能发射：让出时解释器用它自己的 `frame->ip`（= 外层 `OP_LOOP`）
   减 offset，会从**外层循环头**重跑，把已执行的半轮重复一遍。实测症状：`sum`
   6.398e+06 → **6.671e+06**，多出的量 ≈ 每个 yield 一轮迭代。
2. **锁存必须用标志自身，不能用 `deferred_gc`**。后者的清零依赖「解释器侧真的发生过
   一次回收」，而**纯分配循环（没有 `OP_RETURN`、也没有函数调用）恰恰等不到那一次**
   ⇒ `deferred_gc` 永远停在 1 ⇒ 让出永远不被请求 ⇒ 泄漏照旧。改用标志自身做锁存后，
   `probe_alloc2` 从 GC=0 变成 GC=333。
3. **`for` 循环的轮询必须在增量之前**（`EMIT_FOR_STEP_AND_CMP` 之前），且解释器的
   `jit_r == 4` 分支要**落到**它原有的「增量 + 回跳」语义（而不是像 while 那样直接
   `frame->ip -= offset`），否则会多加一次增量 ⇒ 迭代跳步。

**验收（默认配置：默认 8MB 阈值 + 内联全开）**

| 场景 | 改前 | 改后 |
| --- | --- | --- |
| `probe_jit_gc_safepoint.leno`（while，默认阈值） | GC≈0（安全点够不到） | **GC=83 / `Yields: 83`**、`freed≈104699`（≈8MB）、`sum=6.398e+06`（确定值）、`bad=0 reused=0`、1s |
| **`probe_alloc2.leno`（for 纯分配 = §8.31 的 960MB 泄漏场景）** | **GC=0** | **GC=333 / `Yields: 333`**、每次 `freed≈95083`（≈8MB）、5s |
| `assert` | 273/0 | **273/0**（JIT 与 `LENO_NO_JIT=1` 双模式） |
| 金丝雀（`FORCE_EVERY=2000` + 64KB 钉住） | GC=6010 `bad=0` | GC=18136 `bad=0`（更及时；强制钩子本身仍只在解释器安全点兑现） |
| 基准（2 轮） | i++ 125~156 / 经典递归 31 / arr.add 547~563 / arr[index] 172~187 / dict 859~891 | i++ 125~156 / 经典递归 31 / arr.add 563~610 / arr[index] 203~219 / dict 859~890 |

（`arr[index]` 这一格要配合 §8.39 读：1 亿次下「172~187 → 203~219」其实只差 **1 个计时刻度**
（`times.ms()` 刻度 ≈16ms），既不证实也不证伪。用 best-of-N 在 **10 亿次** 下复测的真实成本是
**+0.11ns/轮 ≈ +5.7%**，且仅由「循环体内有 callout」的循环支付。）

**已知成本与后续**：轮询给「循环体内有 callout」的循环每轮加 2 条指令，**真实量级见
§8.39**（10 亿次极紧循环上 +5.7%；真实分配循环上远小于 1%）。
两个原计划的后续方向，都已在 §8.39 用 A/B 实验判定：② R15 预装**已实测收益不足并撤销**；
① 更细的门因**不健全**（任何 callout 都可能分配，白名单会重新打开泄漏）而放弃。

**仍未解决（与本改动无关）**：§8.36 末尾那个「`NOINLINE` 下当前迭代对象未被回收」的
观察仍未解释。路线 3 不依赖它（回收发生在解释器，状态已发布），但**在查出那个存活者是
谁之前，不要认为 JIT 机器码内的活值有任何可见性** —— 这正是「不做就地回收（路线 1/2）」
的直接理由。

***

### 8.39 回边轮询的成本量化 + 两个「后续方向」的否定（2026-09-13）

**动机**：§8.38 唯一留下的成本项 —— 轮询给「循环体内有 callout」的循环每轮加 2 条指令。
先量化，再决定要不要优化。

**测量方法（这一步本身有收获）**：

- **`times.ms()` 的刻度是系统计时器 tick ≈ 15.625ms** —— 实测值只有 187 / 203 / 219 这种
  16ms 的倍数。最初用 `arr[index]` **1 亿次**得到「有轮询 203ms vs 无轮询 188ms」（看 +8%），
  其实**只差 1 个刻度**：在这个分辨率下无法区分「真实效应」与「刻度跳动」。
  把迭代数加到 **10 亿**（新探针 `jit_probes/bench_yield_poll.leno`）后，同样成本 ≈ 9 个刻度，
  才有分辨力。**教训：先看计时器刻度，再决定迭代数** —— 否则很容易把量化噪声写成结论。
- 三个二进制**只差轮询形态**：**无轮询**（临时把 `EMIT_CALLOUT_BEGIN` 里 `body_has_callout`
  的赋值改成 0 重新编译）、**2 条指令**（提交状态）、**R15 1 条指令**（实验版，用
  `git stash` 切出来）。A/B 有效性用 `probe_alloc2` 的 GC 次数校验：**0 / 333 / 333** ✓。

**结果（10 亿次 `arr[0]`，best-of-5，ms）**：

| 版本 | 5 轮 | 最小 |
| --- | --- | --- |
| 无轮询（地板） | 1937 / 1906 / 1906 / 1906 / 1922 | **1906** |
| 2 条指令（现状） | 2031 / 2016 / 2016 / 2015 / 2031 | **2015** |
| R15（1 条指令） | 2000 / 2015 / 2000 / 2031 / 2078 | **2000** |

⇒ 轮询成本 **+109ms / 10 亿轮 = +0.11ns/轮 ≈ +5.7%**，而这是**下界形态**：`arr[0]` 的循环体
只有一个内联数组读，是本项目能构造的最紧的「带 callout」循环。真实分配循环每轮本来要付
一次 callout（~1ns 起）⇒ 占比远小于 1%；纯算术循环根本不发射轮询（`i++` 无差异 ✓）。

**判定一：R15 预装 —— 已实测收益不足，撤销。**

- 收益 **1 个刻度**（2000 vs 2015 ≈ 0.75%），远低于「省掉 1 条指令 ⇒ 省一半」的预期
  ⇒ 那 +109ms 主要不是 uop 执行成本，而是「每轮多一组 compare + branch」本身。
- 代价：R15 从「全后端未使用」变成「保留寄存器」—— 序言 `push r15` + `mov r15, imm64`、
  **每个出口 `pop r15`**（本文件 5 个出口块共用 `EMIT_EPILOGUE`）。对**被外层反复进入的内层
  循环**，入口那 3 条指令很可能比省下的半个 uop 更贵 ⇒ 净收益不确定。按「宁可不编，不要猜」
  的原则不留（R15 定义、序言/尾声改动、`use_yield_reg` 全部回退，源码只剩这段成本注释）。
- 顺带否定了更诱人的 **RIP 相对寻址**（`cmp byte [rip+disp32], 0`，7 字节 1 条指令）：
  `jit.c` 里代码是先写进 `malloc` 缓冲、再 `memcpy` 到 `VirtualAlloc` 的内存（**会重定位**），
  且 `exec_mem` 与全局变量地址之差可能远超 disp32 范围（Windows 下 VirtualAlloc 常在
  `0x000001..` 低区、模块映像在 `0x00007FF..`，差约 138TB）。要可用就得在 memcpy 之后
  逐站点补丁 disp32 —— 为此新增一条补丁链，换 0.75%，不值。

**判定二：「只认直接分配 opcode」的更细的门 —— 放弃（不健全）。**
它能彻底去掉纯读循环（`arr[i]` / `dict[k]`）的轮询，但**任何 callout 都可能分配**
（用户函数调用、方法调用、字符串拼接、容器写入的慢路径……），白名单漏一个就重新打开
§8.31 的泄漏 —— 那正是 P1 修的语义缺陷。用性能小账换正确性大账不划算。
（真正可行的细门需要「静态接收者类型 + 该 callout 是否分配」的可靠判定，属另一个量级的工程。）

**结论**：这 +0.11ns/轮是「回边必须问一次 GC 要不要接管」的固有代价（HotSpot 的 poll
同样是每个回边一条指令），当前形态保留不变；§8.38 里那两个后续方向就此关闭。

**补记：嵌套回边的回收粒度**（§8.38 约束 1 的推论，按阈值推演、未单独实测）。
轮询只发在**本次编译体自己的**回边上 ⇒ 嵌套循环里，内层回边不轮询，最粗的回收粒度是
**一次外层迭代**。这在实践中基本不可达：要让「一次外层迭代」分配超过年轻代阈值（默认 8MB）
需要几十万次分配，而那种内层循环必然早已热到被**单独编译**（它有自己的回边、自己的轮询，
也就有自己的回收粒度）。所以这条不是实际风险，写在这里是为了说明「为什么约束 1 不构成
回收延迟问题」。

***

### 8.40 把 bailout 站点写入移出热路径（每检查省 4 条指令）（2026-09-13）

**动机**：C / C# / LenoJit 三方对比（`examples/性能测试/bench_bitwise_loop.{leno,c,cs}`，
2 亿次位运算循环）：C 224ms / C# 272ms / LenoJit 750ms —— JIT 比解释器快 **16.4x**，
但离 C 还差 **3.35x**。反汇编逐条数（`LENO_JIT_DUMP=1` + `objdump -D -b binary -m i386:x86-64`；
主机 ≈5.1GHz，用 `1e9 次 x=x+1` = 195ms 标定）：C 约 **7 条指令/轮**（5 条依赖运算 = 5.7 周期，
**延迟下界**），LenoJit 约 **112 条指令/轮**（19.2 周期 ⇒ ≈5.8 IPC，**前端吞吐打满**）。
多出来的三块里最大的一块是：

**每次 int48 / 溢出检查之前都发 4 条「记录失败位置」的指令**：

```asm
push r9 ; movabs r9,&jit_bailout_site ; mov dword [r9],site ; pop r9
```

（`movabs` 占 10 字节。）检查本体只有 4 条 ⇒ **检查的约一半开销花在「万一要 bailout 时
能报出位置」上**，而那个全局量只在**真的 bailout 之后**被读（`jit.c` 记进
`last_bailout_site`，供 `jit_print_stats` 翻译成可读原因）。本例每轮约 7 处 ⇒ 约 35 条指令（31%）。

**做法：延迟记录 + 每个 bailout 分支一个桩**

1. `EMIT_BAILOUT_SITE_WRITE(site)` 不再发射任何代码，只置 `ctx->pending_site` / `_valid`；
2. `patch_add(..., target_bc = -1, ...)`（bailout 分支的约定）消费它，转成一条**桩记录**
   `{jcc 的 rel32 位置, site}`；
3. bailout 块之后统一发射桩：`movabs r9,&jit_bailout_site; mov [r9],imm32; jmp <bailout>`
   —— 只有真的 bailout 才执行；R9 可以随便用（马上要 bailout，调用方会重新装载）；
4. 桩表（`JIT_MAX_BAILOUT_STUBS` = 512）满 ⇒ 按项目原则「宁可不编，不要猜」**拒绝编译**
   （与 off_map / patch 溢出同一策略），绝不静默丢站点（那会让 stats 报出错误的失败位置）。

好处是 **45 个调用点一行都没改**：所有站点本来就都是「写 site → 分支 → `patch_add(-1)`」
的形状，所以拦截点只需要 `EMIT_BAILOUT_SITE_WRITE` 与 `patch_add` 两处。
（顺带：bailout 分支不再占用 patch 槽，patch 表压力反而下降。）

**实测**（同机，两个二进制只差本改动）：

| 基准 | 改前 | 改后 |
| --- | --- | --- |
| `bench_bitwise_loop`（2 亿次） | 828 / 750 / 782（最小 750） | 640 / 625 / 641（**最小 625，1.20x**） |
| `arr.add` 1 亿次 | 547~563 | 516~532 |
| `dict[key]=` 1 亿次 | 859~891 | 812~828 |
| `arr[index]` 1 亿次 | 172~203 | 172~187 |
| `fib(30)`（函数级 JIT） | 110ms | **94ms** |

**验证**：

- **诊断可证明未变**：与改动前的二进制逐项对照 `[JIT-DEBUG] BAILOUT site=` —— 本例
  `[17,17,17,14,14,14]`、`probe_alloc2` `[0,0,0]` **完全相同**；stats 的
  `Bailout: fn='benchNone' loop_bc=37 x3 — int48 溢出/截断 @bc_off=37（= loop_bc 37 + 0）`
  也逐字相同；
- `assert` **273 passed / 0 failed**（JIT 与 `LENO_NO_JIT=1`）；
- 溢出路径结果与解释器逐位一致（2^80 / 3^60 / BigInt+int），固化为
  `jit_probes/probe_bailout_sites.leno`；
- **130 个示例**（`examples/{struct,func,module_export_struct,cstruct}`）与改动前的二进制
  差分：**确定性输出 0 差异**，只有指针地址（ASLR）与计时数字不同。

**顺带查清的两件事**（都影响后续方向）：

1. 反汇编里每个语句后「重复的 int48 检查 + 装箱路径」**不是死代码，也不属于
   `OP_SET_LOCAL`**（`OP_SET_LOCAL` 的 codegen 只有一条原始存储：`ops_stack.inc:53-65`
   `TOS_PEEK_TO(RAX); mov [rbp+disp], RAX`）—— 它是**编译器在赋值前插的 `OP_CAST_INT`**。
   JIT 侧 `OP_CAST_INT`（`ops_arith.inc:335-388`）与反汇编逐条对应：int48 → 原样；
   裸 double → `cvttsd2si`；`TRUE_VAL`→1 / `FALSE_VAL`→0 / `NULL_VAL` 原样；其余 bailout。
   之所以被插进来：`assign_cast_needed()`（`codegen_stmt.c:92-103`）对**非字面量一律返回 1**
   （要 cast），且注释里写明了理由 —— 曾放宽到「静态类型一致即消除」，结果 GUI 布局拿到
   未规范化的值（属性对话框尺寸算错）。**这是有意的保守**，而且它是**语义载荷**：
   解释器的 `OP_BITAND` 有 BigInt 分支（`op_bitwise.inc`），`+`/`*` 溢出也会提升 BigInt，
   所以「静态类型是 int」并不等于「运行时是 int48」。
2. `OP_BITAND/BITOR/BITXOR` 只检查**结果**、不检查**操作数**类型 —— 之所以安全，是因为
   **扫描器会拒绝「操作数静态类型是 float/null」的循环**（实测 `x & 3`（x = 1.5 或 null）
   在 JIT 与解释器下报同样的错，且 `Compiled: 0`）。这条不变量值得记住。

**剩余瓶颈（量化，留给下一步）**：优化后约 95 条指令/轮（≈16 周期），仍是吞吐受限。

| 项 | 每轮可省 | 说明 |
| --- | --- | --- |
| 立即数折叠 | ~12 条 | 现在是 `movabs rax,7; mov rdx,rax; pop rax; and rax,rdx`，应为 `and rax,7`。JIT 侧**安全**折叠只能省 1 条（`movabs` 已经发出去了，回退 `cb->len` 会破坏已记录的补丁偏移）；彻底做法是编译期发融合 opcode（`OP_BITAND_IMM` 等，要动字节码格式 + 版本号） |
| 局部量进寄存器 | ~10 条 | 每个语句都 `mov rax,[rbp-8]` + `mov [rbp-8],rax`（2 次访存，依赖链穿内存） |
| `OP_CAST_INT` 归一化省略 | ~25 条 | 编译器为「非字面量赋值给 int 局部量」无条件插入（`assign_cast_needed`）。JIT 侧要有把握才敢跳：**必须证明「前一条指令的 int48 检查覆盖了所有到达该 CAST 的路径」**（`OP_ADD` 这种多路径 opcode 就不满足），且该 bc_off 不是跳转目标（三目/短路会产生落在 CAST 上的合并点） |

***

### 8.41 跳过恒等 OP_CAST_INT：一个「证明型」窥孔（2026-09-14）

**起因**：一个提问 —— 「`OP_SET_LOCAL` 不能在编译时确认类型?」。查下去发现 §8.40 里我
把 disasm 中「重复的 int48 检查 + 装箱路径」记成 `OP_SET_LOCAL` 的类型派发**是错的**：

- `OP_SET_LOCAL` 的 codegen 只有一条原始存储（`ops_stack.inc:53-65`），槽里是 raw 值
  （int48 原样 / float 存裸 double 位），**不需要任何类型信息**；
- 那段是**编译器插入的 `OP_CAST_INT`**，JIT 侧 `ops_arith.inc:335-388` 与 disasm 逐条
  对应：int48 → 原样；裸 double → `cvttsd2si`；`TRUE_VAL`→1、`FALSE_VAL`→0、`NULL_VAL`
  原样；其余 bailout。

**编译器为什么插它**：`assign_cast_needed()`（`codegen_stmt.c:92-103`）对**非字面量一律
返回 1**（要 cast），注释写明理由 —— 曾放宽到「静态类型一致即消除」，结果 GUI 布局拿到
未规范化的值（属性对话框尺寸算错）。而且 `&`/`|`/`^` 连类型规则都没有
（`codegen_expr.c:237-239` 直接 `emit OP_BITAND`，不像 `+`/`-`/`*` 会按左右类型选 typed
opcode），所以 `inp & 7` 的表达式类型是「未知」⇒ 必然补 CAST。**这是有意的保守。**

**为什么不能直接删 CAST**：它是**语义载荷**。解释器的 `OP_BITAND` 有 BigInt 分支
（`op_bitwise.inc`），`+`/`*` 溢出也会提升 BigInt ⇒「静态类型是 int」**不等于**「运行时是
int48」。另外静态已知的不匹配（`int a = 2.7`）会被**语义层**直接报错，根本到不了 CAST。

**做法：只有「可证明」时才省（纯 JIT 侧，不动字节码）**。三个前提缺一不可：

1. **支配性**：紧邻的前一条指令必须已经用 int48 检查证明了结果是 raw int48。为此加了
   白名单（15 个 opcode，`EMIT_INT48_CHECK_TOS` / `MARK_TOS_RAW_INT48` 的调用点）：
   `OP_ADD_INT/SUB_INT/MUL_INT/NEG_INT/DIV_INT/MOD_INT`、`OP_ADD/SUB/MUL_INT_IMM`、
   `OP_BITAND/BITOR/BITXOR/BITNOT`、`OP_SHL_IMM/SHR_IMM`。
   **明确排除**：`OP_ADD/SUB/MUL` 通用版（int/float/concat 三条路径，非 int 路径不做检查）、
   `OP_USHR_IMM`（`shr` 结果可能越出 int48）、`OP_CAST_INT` 自身（null 路径原样返回）。
   标记是**一次性**的：每条指令开头消费并清零，跨一条就失效。
2. **值仍在 RAX**（`tos_live`）—— 被 spill 到内存栈时前提不成立。
3. **本 bc_off 不是跳转目标、且不在内联体内** —— 短路/分支的合并点会恰好落在 CAST 上，
   从别的路径跳进来时前一条指令并不是那个检查（复用 `x86_64.c` 里已有的
   `patch target_bc == bc_off` 扫描思路，抽出 `bc_is_jump_target()`）。

成立时 `OP_CAST_INT` 是恒等变换，整段（4 条检查 + 冷路径）直接不发。

**实测**：

| 项 | 只有 §8.40 | 加上本窥孔 |
| --- | --- | --- |
| `bench_bitwise_loop`（2 亿次） | 640/625/641（最小 625ms） | **453/469/453，复测 437~453（最小 437ms ⇒ 1.38~1.43x）** |
| 该基准生成的机器码 | 591 行 | **386 行（−35%）**，CAST 的冷路径（含 3 个 `movabs`）一并消失 |
| `fib(38)`（函数级 JIT） | 1328 / 1375 / 1343 ms | 1329 / 1344 / 1328 ms（无变化） |
| `arr.add` / `arr[index]` / `dict[key]=` / `i++` 1 亿次 | — | 均在噪声内（A/B 二进制对照） |

累计：`bench_bitwise_loop` **750ms → 453ms（1.66x）**，与 C 的差距从 3.35x 缩到 **2.0x**。

**验证**：

- `assert` **273 passed / 0 failed**（JIT 与 `LENO_NO_JIT=1`）；
- **130 个示例 JIT vs 解释器输出逐字一致**（比「改动前后一致」更强的不变量：它同时是
  语义等价性检查），只有指针地址（ASLR）与计时数字不同；
- 新增 `jit_probes/probe_cast_int_peephole.leno`（判据写在文件头：JIT vs 解释器逐字一致
  + 反汇编里白名单算术后不应再有 CAST 归一化、bool/`and`/`or` 后必须有）；
- ⚠️ **方法论**：`fib(32)` 一度显示 15ms → 31ms，看着像 2x 回归 —— 其实是 §8.39 那条
  16ms 计时刻度（恰好跨过一个 tick）。换 `fib(38)` 立刻显示无变化。**别用 1~2 个刻度的
  量级下结论。**
- 顺带确认真实性：`i++` 一度显示 125~156ms → 93~110ms，A/B 二进制对照后确认是**机器状态
  差异**（两个二进制都跑 78~94ms），与本改动无关。

***

### 8.43 局部量驻留寄存器 v1：机制落地，但**单独零收益**（2026-09-14）

**判定清单（动手前的审计）**：`cur_local_map[` 在 `src/jit/backend` 下共 **20 处**访问点 ——
`OP_GET_LOCAL` / `OP_SET_LOCAL` / `OP_SET_LOCAL_POP` / `OP_MOVE_LOCAL[_POP]` /
`OP_SET_LOCAL_CONST` / `OP_CLEAR_LOCAL_RANGE` / `OP_INC|DEC_LOCAL[_NOPUSH]` /
`OP_PRE_INC|PRE_DEC_LOCAL` / `OP_FOR_PREP` / `OP_FOR_LOOP` / `OP_CMPJMP_LL_INT` /
`OP_CMPJMP_LG_INT` / `OP_GET_FIELD_FAST`，加上 `x86_64.c` 里 FOR_LOOP 的 step 检查与回边。
**任何一处漏改都会让寄存器与 scratch 槽出现两份值（静默算错）**，这个清单是本次改动的安全边界。

**v1 做法（保守）**：

- 只 pin「访问点全部已转换」的 scratch 槽 —— 即只被 `OP_GET_LOCAL` / `OP_SET_LOCAL` /
  `OP_SET_LOCAL_POP` 访问的槽；其余 17 处 opcode 命中的槽一律排除
  （`pick_pin_local` 的排除表就是上面那份清单，代码里注明「新增访问点必须同步」）。
- 最多 pin **1 个**槽（访问次数最多、且 ≥ 2 次的）。**可用寄存器只有 R15**：RBX 是类型位图、
  RBP 是帧、R12/R13/R14 被 callout 的保存/恢复（RSP/RCX/R9）占用。
- 只在循环 JIT 启用（函数级 JIT 每次调用都进入，push/pop + 装载的固定成本更高）。
- pinned 槽在**序言**里装载、在**写回前**刷回 scratch 槽（`EMIT_WRITEBACK_LOCALS` 首行），
  写成/退出/让出路径共用 ✓；**bailout 不写回**（语义是「整个 JIT 运行被丢弃、解释器重放」，
  §8.37）✓。

**实测（关键）**：

| 基准 | §8.41 之后 | 加上 pin 之后 |
| --- | --- | --- |
| `bench_bitwise_loop` 2 亿次 | 437~453ms | **437 / 438 / 438ms —— 完全相同** |
| `i++` / `arr.add` / `arr[index]` / `dict[key]=` / `fib(32)` | — | 全部在噪声内（逐项对照） |

debug 打印确认 pin 确实命中：`[JIT-CG] pin: scratch[0] -> r15 (access=10)`（正是最热的 `inp`）。

**为什么零收益（本轮最有价值的结论）**：pin 把 `mov rax,[rbp-8]` / `mov [rbp-8],rax`
换成了 `mov rax,r15` / `mov r15,rax` —— **指令条数一条没减**，而这条循环是
**前端吞吐受限**的（§8.40 实测 ≈5.9 IPC，即解码/发射宽度打满）。所以：

> **省「内存访问」不划算，只有省「指令条数」才划算。** 后续优化的判据应该是
> 「这条改动让每轮少发几条指令」，而不是「让访存更快」。

验证：`assert` 273/0（双模式）；**130 个示例 JIT vs 解释器逐字一致**（3 个差异全是计时行，
同前两次核对）；两个探针输出不变。

**保留理由**：它是下一步「惰性常量 + 立即数折叠」的前提 —— 有了 pinned 槽，`x = x & 7`
这种语句才能被折成 `mov rax,r15; and rax,7`（**2 条**，现在是 12 条）。按本项目的
「不留没有证据的复杂度」原则，这次是**有证据的中性**（机制正确 + 单独零收益 + 明确解锁下一步），
而不是猜。剩下的 17 处访问点会在各自转换时逐步缩小排除表。

***

### 8.44 语句级折叠：`x = x ⊕ k` 直接落在寄存器上 + 可证明冗余的检查（2026-09-14）

**动机**：§8.43 的判据是「只有省指令数才划算」，而 pin 机制（§8.42）已经就位。

**两个形态**（编译器生成的固定形状，识别后整段吃掉）：

| 形态 | 字节码 |
| --- | --- |
| A | `GET_LOCAL x` `[ADD/SUB_INT_IMM i \| SHL/SHR_IMM i \| CONST k + BITAND/BITOR/BITXOR]` `[CAST_INT]` `SET_LOCAL_POP x` |
| B | `CONST k` `GET_LOCAL x` `BITAND/BITOR/BITXOR` `[CAST_INT]` `SET_LOCAL_POP x`（`x = k ⊕ x`，常量在左） |

折叠后（x 是 pinned 槽）：

```asm
<op> r15, imm        ; 1 条（A 形态的 ADD/SUB/SHL/SHR 直接内联立即数）
[int48 检查]          ; 只有 ADD/SUB 需要（见下）
```

原来是 ~12 条（取局部量、把常量物化进 RAX、搬进 RDX、弹左操作数、运算、检查、写回）。
只折**语义等价**的组合：AND/OR/XOR 可交换；ADD/SUB/SHL/SHR 的立即数本来就在右侧。
被吃掉的每个字节码偏移都检查过「不是跳转目标」（否则 off_map 缺条目，收尾会拒绝编译）。

**归因实验（本轮关键）**：临时把折叠里的 int48 检查去掉 —— **422ms → 313ms（1.35x）**。
⇒ 瓶颈不是指令条数，而是**检查**（4 条 + 一个分支 × 每语句）。于是：

> **只对会溢出 int48 的运算保留检查**：`ADD/SUB` 会溢出（溢出 bailout 交解释器走 BigInt）⇒ 保留；
> `AND/OR/XOR`（位运算保持符号扩展模式）、`SHR`（算术右移只会变小）在 int48 操作数下结果
> 必然仍在 int48 内 ⇒ **检查恒真，去掉语义不变**。

**为什么可以断定「操作数必然是 int48」（两重保证，都实测过）**：

1. **扫描器**：位运算的操作数静态类型必须是 int —— 浮点/null 入口的循环会被拒绝编译
   （实测 `x & 3`（x=1.5 或 null）`Compiled: 0`，JIT 与解释器报同样的错，§8.41）；
2. **语义层**：循环内也注入不进去 —— 向推断为 int 的局部量赋 float/null 会直接编译报错
   （实测 `var x = 1; x = 1.5/null` → 「类型不匹配：无法将值赋值给变量」）。

所以「int 局部量 & int 字面量」的两个操作数都是 int48 ⇒ 结果必在 int48 内。

**踩到的坑（差点静默算错）**：`OP_SHL_IMM` 的 VM 语义是「int64 左移后**截断**为 48 位」，
**不 bailout**（原 codegen 用 `EMIT_INT48_TRUNCATE`：`shl 16; sar 16`）。我的折叠第一版只发了
`shl r15, imm`，没做截断 —— 基准里恰好是 `>>`（SHR + 无需检查）所以数值仍然对，
**换个 `<<` 的用例就会错**。现在折叠显式补上 `shl r15,16; sar r15,16`。

**实测（2 亿次 `bench_bitwise_loop`）**：

| 阶段 | 最小耗时 | 相对上一版 |
| --- | --- | --- |
| 会话开始（§8.38 之前） | 750ms | — |
| ①bailout 桩（§8.40） | 625ms | 1.20x |
| ②CAST 窥孔（§8.41） | 437ms | 1.43x |
| ③pin（§8.42，中性） | 437ms | 1.00x |
| ④折叠（本步） | **297ms** | **1.47x** |

累计 **750ms → 297ms（2.5x）**；与 C（224ms）的差距从 **3.35x 缩到 1.33x**（C# 是 272ms）。
其它基准不变（`i++` 78~109 / `arr.add` 531 / `arr[index]` 172~187 / `dict` 828~859 / `fib` 在刻度边界）。

**验证**：`assert` 273 passed / 0 failed（双模式）；**130 个示例 JIT vs 解释器逐字一致**
（4 个差异全是计时行）；3 个探针（`probe_cast_int_peephole` / `probe_bailout_sites` /
`probe_jit_gc_safepoint`）输出不变。

**防御性设计**：折叠的「形态识别」与「发射表」是两处清单，容易不一致 —— 所以加了护栏：
只有 `alu` 在发射表里确实有实现时才折叠（否则整段放弃，绝不静默少发一条运算）。

***

### 8.45 pin v2（4 槽）+ 循环计数器进寄存器；以及「为什么本基准不涨」的归因（2026-09-14）

**做了什么（两块，零语义变化）**

1. **腾寄存器**：callout 跨调用的状态保存（RSP/RCX/R9）从 R12/R13/R14 挪到 3 个新帧槽
   （`frame_sz` +24），那三个 callee-saved 因此空闲；序言/尾声不再压弹它们。
   代价只有每次 callout 3 存 + 3 取（L1、被被调函数延迟掩盖）。
2. **pin v2**：从「最多 1 槽（R15）」扩到**最多 4 槽**（R15/R14/R13/R12），并给
   **计数循环三元组（loop_var/step/end）最高优先级**；`OP_FOR_PREP` / `OP_FOR_LOOP`
   的 5 处访问点全部改走 `EMIT_LOAD_LOCAL` / `EMIT_STORE_LOCAL`（不再排除在 pin 之外），
   三元组全驻留寄存器时 FOR_LOOP 回边折成 `add lv,step` / `cmp lv,end`。

**结果：指令数腰斩，耗时不变**

| | 循环体 | 2e9 轮耗时 |
| --- | --- | --- |
| 改前（§8.44 状态） | ~39 条 | 2938 / 2953 / 2953 ms |
| 改后 | **21 条**（−46%） | 2938 / 2969 / 3031 ms |

正确性：`assert` 273/0（JIT 与 `LENO_NO_JIT=1`）、`probe_alloc2` GC=333、
`probe_jit_gc_safepoint` GC=83 且 `sum=6.398e+06` 逐位正确、结果 1198。

**归因：把 JIT 产物逐条写进内联汇编来测（本轮最有价值的方法）**

`build/asmloop.c` / `asmloop2.c` 把 21 条原样放进 `.p2align` 可控的内联汇编，2e9 轮：

| 变体 | ns/轮 |
| --- | --- |
| A 完整 21 条（= JIT 产物） | 1.497 |
| B 去掉两处 int48 检查 | 1.393 |
| C 最简 8 条（只留 5 运算 + 计数器） | 1.357 |
| D 完整形状 + 32 字节对齐 | 1.480 |
| **真实 LenoJit** | **1.469** |
| **真实 C 版（gcc -O2）** | **1.121** |

⇒ ① JIT 产物性能 = 手写同序汇编（甚至略好）⇒ **不是 codegen 质量问题**；
② 对齐无关（A≈B）、step 方向分流无关（C≈D）、两处检查合计 ≈0.10~0.14ns；
③ **真实 C 版的反汇编只有 6 条**：`sar rdx,8; and edx,7; xor rdx,0x465; add rdx,0x4d;
sub rax,1; jne` —— **`inp & 1135` 整条被 gcc 删掉了**。

为什么能删：`v = inp & 7` ⇒ v ⊆ [0,7]；1125 的置位是 1135 的子集，且 7 ⊆ 1135
⇒ `(v ^ 1125) & 1135 == v ^ 1125`。于是 gcc 的**依赖链是 4 环**，我们的是 5 环。

**结论**：这个循环是**延迟受限**（链长决定），不是指令数受限 —— 这正是「39 → 21 条
零变化」的原因，也说明 §8.44/§8.45 这类"减指令"工作对它的上限就是 0。

**下一步（已定位，未实施）**：在 §8.44 的折叠窥孔里加一个**「可能置位掩码 M」的一步回看**：

- 同槽、紧邻的上一条折叠语句是 `and imm` ⇒ `M = imm`；是 `xor imm` ⇒ `M |= imm`；
  **其它任何 opcode（含 add/sub/shr/shl）一律让 M 失效** ⇒ 只依赖直线相邻的一条语句，
  不做跨迭代/跨分支推断（与「宁可不编，不要猜」一致）。
- `M & ~imm2 == 0` ⇒ 该 `and imm2` 是恒等 ⇒ 不发（正是 gcc 在上面做的）。
- `max(M) + 立即数` 仍在 int48 内 ⇒ 该 ADD/SUB 的 int48 检查恒真 ⇒ 不发（上表 B 的 0.10ns）。

两者合起来就能把本基准的 5 环链 + 两处检查压到 gcc 的形态（4 环链 + 计数器），
预期 1.47 → ~1.2 ns/轮。

***

### 8.46 折叠「一步回看」：值域掩码 ⇒ 删恒等 AND、省恒真的 int48 检查（2026-09-14）

**动机（§8.45 归因的直接结论）**：C 版循环是 6 条 / **4 环依赖链**，LenoJit 是 21 条 / **5 环**
——差别不在指令数，而在 gcc 把 `inp & 1135` 整条删了（`(v ^ 1125) & 1135 == v ^ 1125`，
因为上一条 `inp & 7` 已经把值夹进 [0,7]）。这是**可证明**的推理，LenoJit 也能做。

**做了什么**

1. **扫描器导出两个静态事实**（`ScanResult`）：
   - `jt_fwd[off]` —— 该偏移是某个**前向**跳转的目标（`OP_JUMP` / `OP_JUMP_IF_*` /
     `OP_CMPJMP_*` 的目标全部标记；`body_size > 512` 或越界 ⇒ `jt_ok = 0`）；
   - `has_back_jump` —— 体内存在**非本条回边**的反向跳转（内层 `OP_LOOP` /
     `OP_FOR_LOOP`；判据 `ip + size < end`）。
2. **折叠路径加「一步回看」状态**（`prev_fold_si` / `prev_fold_mask`）：只有**紧邻的
   上一条语句是同槽折叠**、且**当前语句起始偏移不是前向跳转目标**、且**体内无内层回边**
   时，掩码才有效。主循环每轮开头快照并清空（任何非折叠 opcode ⇒ 失效）。
   掩码语义：`M = 值的可能置位位`（且位全在低 47 位 ⇒ 值 ∈ [0, M]、非负、在 int48 内）。
3. 两条应用：
   - **恒等 AND**：`M & ~imm == 0` ⇒ `x & imm` 不改变 x ⇒ 整条不发（正是 gcc 的推理）；
   - **恒真的溢出检查**：`M` 与立即数算出的最坏区间仍在 int48 内 ⇒ ADD/SUB 的
     `EMIT_INT48_CHECK_REG` 不发。
   掩码更新规则：`AND imm` ⇒ 收紧到 `M`（或 `imm`）；`XOR/OR imm` ⇒ `M | imm`；
   其它（ADD/SUB/SHL/SHR 及任何非折叠 opcode）⇒ 失效。

**为什么健全（两条都来自扫描器的静态事实，不做跨迭代/跨分支猜测）**
回看的前提是「当前语句只可能由紧邻的上一条直落到达」：
- 若有跳转**落到**当前语句的起始偏移 ⇒ 上一条可能被跳过 ⇒ 由 `jt_fwd` 排除；
- 若是 `do{}while` 形态（循环头就是第二条语句），第二轮会从第二条进入而跳过第一条
  ⇒ 由 `has_back_jump` 排除（这类目标一定是体内回边）。
两条任一不满足就退化成「不回看」（语义与 §8.44 完全一致）。

**实测（`bench_bitwise_loop`，2e9 轮）**

| | 循环体 | 耗时 |
| --- | --- | --- |
| §8.45（改前） | 21 条 | 2938 / 2969 / 3031 ms |
| §8.46（改后） | **15 条**（21 − 恒等 AND 1 条 − ADD 检查 5 条） | **2329 / 2266 / 2250 ms** |
| 真实 C 版（gcc -O2） | 6 条 | 2239 / 2334 ms |

⇒ **1.28x**，与 C 版**基本持平**（2330 vs 2266 ms，差 3%）。调试输出可直接看到命中：
`[JIT-CG] FOLD slot=0 alu=26 imm=1135 len=11 [恒等 AND 不发]`、
`[JIT-CG] FOLD slot=0 alu=118 imm=77 len=9 [检查恒真不发]`。
正确性：`assert` 273/0（JIT 与 `LENO_NO_JIT=1`）、`probe_alloc2` GC=333、
`probe_jit_gc_safepoint` GC=83 且 `sum=6.398e+06`、结果 1198。

**本轮方法学**：先量化（§8.45 的汇编对照）再动刀 —— 第一轮的三个"显而易见"优化里有两个
（循环对齐、方向分流提升）被实验否掉，真正有效的那个（掩码）是**先知道链长差在哪**才找到的。

***

### 8.47 `OP_CMPJMP_LI_INT`（local vs 立即数）+ callee 解析缓存 —— 以及「我连着两次测错」（2026-09-14）

**起点**：`fib(42)` 每次调用 27.2ns，而 C 1.05ns、C# 1.74ns。反汇编 fib 的函数体后发现，
调用路径的结构性死重是「JIT body → C callout → JIT body」（每次 Leno 调用 2 次 native C 调用
+ 实参经内存 vstack 编组 + callee 的 locals 走内存池），但里面还夹着两块**纯浪费**，本轮先收掉。

**一、`if (local <cmp> 立即数)` 从来没走过融合**

后端早有 `OP_CMPJMP_LL_INT`（local vs local）与 `OP_CMPJMP_LG_INT`（local vs global），
结构就是 `cmp` + 反条件 `jcc`。缺的是**立即数那一格**，根因在 codegen 的 peephole：

```c
// codegen_stmt.c: try_emit_cmpjmp
if (!l || l->kind != AST_VAR) return 0;
if (!r || r->kind != AST_VAR) return 0;   // ← `n < 2` 右边是字面量，直接放弃
```

于是 `if n < 2 { return n }` 会先构造一个 bool Value（`setcc` + `shl 48` + `movabs` 标签 + `add`），
再走 `OP_JUMP_IF_FALSE` 的真值测试（`test` + 与 `FALSE_VAL`/`NULL_VAL` 各比一次，又是 2 个
`movabs`）—— **共 21 条指令、其中 3 个是 10 字节 `movabs`**，只为测一个刚由 `cmp` 产生的条件。

新增 `OP_CMPJMP_LI_INT`（`cmp_op(1) slot(2) imm32(4) offset(4)` = 12 字节），**追加在枚举末尾**
（既有 opcode 编号全部不变）。同步点按 `OP_CMPJMP_LG_INT` 的**全部出现位置**对照补齐，共 13 处：

| 文件 | 改了什么 |
| --- | --- |
| `include/leno_vm.h` | 枚举（追加末尾） |
| `debug.c` | `opCodeNames[]`（**位置表**）+ 反汇编 |
| `vm/vminc/vm_run.inc` | 计算跳转表项 |
| `vm/vminc/op_jump.inc` | 解释器 handler |
| `codegen/codegen_stmt.c` | peephole 模式 4（含「字面量在左侧」的反转 `lit OP local ≡ local rev(OP) lit`） |
| `codegen/codegen_emit.c` / `codegen.h` | `emit_cmpjmp_li_int` |
| `jit/jit_scan.c` | `opcode_size`(12) / 内联扫描 / 条件跳转目标登记 |
| `jit/backend/x86_inc/ops_jump.inc` | JIT codegen |
| `jit/backend/x86_64.c` | `pin_excl`（该局部量不能驻留寄存器，内存副本才是权威，同 LL/LG） |
| `jit/backend/x86_64_emit.h` | `emit_cmp_reg_imm32` |

立即数限 int32；bigint / float 字面量一律退回非融合路径（**不猜**）。

效果 —— fib 的 `if n < 2` 由 21 条变成 `cmp $0x2,%rax` + `jge`，函数体机器码 954 → 828 字节。

**二、callee 解析缓存**

`jit_callout_global_func` 每次调用都要走 `jit_func_lookup_or_compile`（跨 TU 调用 + 哈希探测），
而递归 / 固定调用点每次都是**同一个 callee**。加了 1 项缓存。

**安全要点（比优化本身重要）**：缓存里**不存机器码裸指针**。`jit_func_entry_claim` 在槽冲突时
会 `jit_mem_free` 掉被驱逐函数的机器码（`jit_priv.h` 明确记载了这个 use-after-free 场景，256 槽
就是为压概率）。所以只缓存**槽地址**（`jit_func_cache` 是静态数组，地址稳定）+ `lcount`，
每次命中重读 `e->func` / `e->fn` 校验；槽被回收 ⇒ 校验失败 ⇒ 自动回退原路径。
顺带去掉了实参槽的 `NULL_VAL` 预填（只需填 `arg_count` 之外的槽）。

**三、两个基准开关**（与 `LENO_NO_JIT` / `LENO_JIT_NOINLINE` 同类）
`LENO_NO_CMPJMP` 关掉比较+跳转融合、`LENO_NO_CALLCACHE` 关掉 callee 缓存。价值见下一段。

**版本**：opcode 集合变化 ⇒ `LENO_BIN_VERSION 2.5.0 → 2.6.0`、`LENO_MODCACHE_VERSION 4 → 5`
（枚举追加末尾所以既有编号不变，但新字节码含旧构建不认识的 opcode，仍要 bump）。

**验证**：build 无警告；`assert` 273/273（JIT 与 `LENO_NO_JIT=1` **各一遍** —— 后者才覆盖新指令的
解释器实现，fib 走 JIT 验证不到）；语义专测覆盖 `n<2` / `2<n`（字面量在左）/ `==` / `!=` / `>=` /
`0<=n` / 大立即数，JIT 开与关输出逐字一致。

**性能**（位置平衡的四配置交错，A=全开 B=融合关 C=缓存关 D=全关）

| 规模 | A | B | C | D | 合并 |
| --- | --- | --- | --- | --- | --- |
| fib(32) | 166.7ms | 171.7 | 182.3 | 195.3 | **−14.7%** |
| fib(36) | 1152.8ms | 1215.3 | 1242.0 | 1375.0 | **−16.2%** |

两档都严格单调 A < B < C < D；归因：callee 缓存 ≈ −9%、融合 ≈ −7%。用户独立实测
`fib(32)` 186 → 157ms（−15.6%）与此吻合。

**教训：这件事上我连着测错两次，值得单独记**

1. **第一次错在「跨时段对比」**。我先给出 +13.1%（缓存）、+20.2%（合并），方法是用 `git stash`
   切基线、两边各 3 轮。看上去是 A/B，实际不在同一时间窗 —— 同一个二进制、同一个负载，
   我在不同时段跑出过 **3000ms 和 3516ms（差 25%）**。那组数字幅度碰巧对，方法不成立。
2. **第二次错得更多，错在「顺序」和「判据」**。我随后用同一二进制 + 环境开关重测，把结论
   改成「≈0」，还据此写下「fib 对指令条数不敏感」—— **这是错的**。两个错误叠加：
   - **顺序没平衡**：那轮固定按 `ABCD` 顺序跑，而机器有明确的「越跑越慢」趋势（有一个轮次是
     156/172/188/235 严格递增），于是总落在靠后位置的 C/D 被系统性加价。改成 `ABCD`/`DCBA`
     交替（A/D 各占位置 1、4 各三次）后，读数立刻分开了。
   - **判据选错**：我用「min 相同」判定无差异，但这里 min 恰恰不敏感（C 组最小也是 156），
     而中位数/均值能稳定分开（171 vs 188）。`jit_probes/README.md` 原先写的「各 3 轮取最小值」
     **在效应体现在分布而不是最优值时是失效的**，已按此修正。
3. **结论**：本机上 **位置平衡 + 足够多轮 + 看中位数/均值** 才够用；单靠「交替 + 取 min」会同时
   产生假阳性与假阴性 —— 我两种都撞上了。而用户自己测的 186→157ms **从头到尾都是对的**。

**未解释**：同一口径下 `fib(38)` 只量到约 1%（3.4s/轮），与小 N 两档的 ~15% 不自洽。那一轮没做
位置平衡，暂归因于长跑下的热/带宽状态差异，**标记待查**。

***

### 8.48 形参类型化快路径：序言 11 条 → 4 条（2026-09-14）

**依据（语言语义，不是推测）**：写了具体类型的形参，**运行期一定是该类型** ——
`any` 形参必须先收窄才能使用，编译器会拦住。所以 `param_types[slot]` 对函数级 JIT
不是"提示"，是可以直接依赖的**前提**。这与既有 `OP_CMPJMP_LL/LG`（直接按 int64
比较局部量）所依赖的前提是同一档次。

**改了什么**：函数级 JIT 的序言此前对**每个** scratch 槽做「tag 检查 + 双路径」
（int → 符号扩展；非 int → 原样存 NaN-boxed 位 + 置 RBX 位图位），并在两路之间跳转。
现在只要 `ctx->func_mode && slot < arity && param_types[slot] == TYPE_INT`，就直接取
int48 载荷（`<<16 >>16`）写进 scratch 槽，**不置位图位**（0 = int，与原 int 路径
逐位等价）。配套在 `CodegenCtx` 加了 `func` 指针（`jit_compile_function` 填入），
否则 codegen 拿不到 `arity` / `param_types`。

**边界（不照搬）**：只对 `TYPE_INT` 生效；float / struct / any / 元信息缺失
（`param_types == NULL`）一律走原路径 —— 它们的 raw 表示不同。

机器码（fib 序言）：

```asm
; 之前：~11 条 + 双路径分支
mov 0x0(%rcx),%rax ; mov %rax,%r8 ; shr $0x30,%r8 ; cmp $0xfffb,%r8 ; je .is_int
bts $0x0,%rbx ; mov %rax,-0x8(%rbp) ; jmp .next
.is_int: and %r10,%rax ; shl $0x10,%rax ; sar $0x10,%rax ; mov %rax,-0x8(%rbp)

; 现在：4 条，无分支
mov 0x0(%rcx),%rax ; shl $0x10,%rax ; sar $0x10,%rax ; mov %rax,-0x8(%rbp)
```

**验证**：build 无警告；`assert` 273/273（JIT 与 `LENO_NO_JIT=1`）；另写专测覆盖
**int / float / string / int+float 混合 / any** 五种形参，在**默认 /
`LENO_JIT_NOINLINE=1`（强制全走 callout）/ `LENO_NO_JIT=1`** 三种配置下输出逐字一致。

**性能：估 5%，实测 1.5%**（fib(36)，6 轮位置平衡）

| 统计量 | ON | OFF | 差 |
| --- | --- | --- | --- |
| 均值 | 1119.8ms | 1132.7ms | −1.1% |
| 中位数 | 1101.5ms | 1125.0ms | −2.1% |
| 最小值 | 1093ms | 1109ms | −1.4% |

三个统计量方向一致（所以不是噪声），但**远低于估算的 5%**。偏差原因值得单独记：
被删掉的是「一个**高度可预测**的分支 + 几条**相互独立**的指令」，在前端 / 分支预测
不是瓶颈时，省指令并不换时间。**「按指令条数线性推时间」只能当预测，必须实测** ——
这条在 §8.47 已经打脸一次，这里是第二次。

**度量刻度（本轮的关键方法学收获）**：先试 `fib(32)`，得到 ON 组四次全等 172ms、
OFF 组 172/172/203/250。172→203→250 的间隔暴露了 `times.ms()` 的刻度是 **~15.6ms**，
而 fib(32) 总耗时才 172ms ⇒ **5% ≈ 8.6ms 不足一个刻度，物理上不可分辨**。
换到 fib(36)（~1.1s）后才得出结论。
**规则：要测百分之几的改动，基准必须跑到「效应 > 1 个刻度」。**

**开关**：新增 `LENO_NO_TYPEDPARAM`。它与 `LENO_NO_CMPJMP` 都在**编译期**路径上，
运行时零成本；**但同批的 `LENO_NO_CALLCACHE` 一开始放错了位置** —— 写成「static
首调用判负」塞在 `jit_callout_global_func`（每次 Leno 调用都进的函数，fib 8.67 亿次）
里，等于给一个默认关闭的开关每次多付 2~3 条指令。已改为在 `jit_init()` 一次性解析成
`jit_state.no_callcache`，热路径只剩一次 test。教训已写进 `jit_probes/README.md`
的开关表，并归纳成一句：**新增开关时先问「这个判定在热路径上吗？」**

### 8.49 四张类型定义表：五个注册点、四处不一致、跨模块同名静默覆盖（2026-09-14）

**四张表**（运行时全局定义表，各自 `register` + `find`，都是**线性扫描、只按名字**）：

| 表 | 文件 | 注册 | 查找 |
| --- | --- | --- | --- |
| `struct_def_table` | `src/object/object_struct.c` | `struct_def_register` | `struct_def_find` |
| `enum_def_table` | `src/object/object_struct.c` | `enum_def_register` | `enum_def_find` |
| `face_def_table` | `src/object/object_face.c` | `face_def_register` | `face_def_find` |
| `cstruct_def_table` | `src/object/object_cstruct.c` | `cstruct_def_register` | `cstruct_def_find` |

**关键结构性事实**：四张表都是**全局扁平、只认名字**（`strcmp(table[i]->name, def->name)`），
**没有模块维度**。同一进程里两个模块各定义一个同名类型，后注册者**覆盖**前者 —— 无错、无警告，
程序照跑。这不是 bug 的"表现"，是这张表的数据结构决定的（见下面「跨模块同名」一节）。

**语义侧的五个注册点**（四个种类分居四个文件，长期各写一套策略）：

1. `semantic.c` 预注册阶段（支持前向引用）—— 覆盖 `struct` / `face` / `cstruct`，**没有 `enum`**；
2. `visit_type_def.inc` `AST_STRUCT_DEF`；
3. `visit_type_def.inc` `AST_FACE_DEF`；
4. `visit_ffi.inc` `AST_CSTRUCT_DEF`；
5. `visit_enum.inc` `AST_ENUM_DEF`。

**排查中发现并修掉的四处不一致**（`a6a0973` + `c69bb8cf`）：

| # | 不一致 | 后果 | 修法 |
| --- | --- | --- | --- |
| 1 | face 分支跨种类时**没有 `else`** | `struct Foo` + `face Foo` **静默通过**，两张全局表各存一份 | 补同一套跨种类检查 |
| 2 | struct/cstruct/enum 三处文案**漏列 `face`** | 撞上 face 时退化成"重复定义"，看不出原因 | 四种两两互换都能点名先声明的种类 |
| 3 | struct/face **报错后仍** `*_def_new + register` | 非法定义留在全局表，后续 `*_def_find` 取到它并派生次级错误 | 报错后不再写入全局表（四者统一） |
| 4 | 覆盖时的资源清理不一致：struct/cstruct/enum 都把旧定义资源指针置 NULL 防 `gc_free_all` 时 double-free，**face 没有** | face 同名覆盖后旧对象资源未置空 | face 补齐（并加 name 非空判断） |

**更深的一处：`enum` 没进预注册（本轮修）。**

`enum E1`（37 行）+ `face E1`（40 行）修复前的报错是：

```
probe.leno(37,1): error: [重复定义] 类型 'E1' 已经定义为 face，不能重复定义为 enum
```

报在**先声明**的那条上，文案却指向**源码里更靠后**的 face —— 定位与归因同时错位。根因：
预注册覆盖 struct/face/cstruct，face 先把 `E1` 占成 `TYPE_FACE`，主阶段处理 enum 时撞上它。
修法两条：

- 把 enum 纳入预注册，kind **沿用主阶段将采用的取值**（`SYM_MODULE`/`SYM_GLOBAL`）——
  只提前占位、不改语义（改成 `SYM_ENUM` 会动到 `codegen_expr.c` 的类型定义分支，另说）；
- 主阶段加「复用已预注册符号」路径（与另三种对齐）。**注意别直接 `scope_define`** ——
  它会因重名返回 NULL，反被下面的 `else` 当成"重复定义"**误报**。

修后四个种类两两互换的 7 种组合全部落在**后声明**那条上、点名**先声明**的种类。

**跨模块同名：危害是真的，但现有语料没触发。**

正对照（两个模块各定义 `Shared`，1 字段 / 2 字段，main 同时 import）：

```
[DUPNAME] struct name=Shared old{fc=1,mc=0,f0=x} new{fc=2,mc=0,f0=x}
```

无错无警告，B 的定义静默覆盖 A。诊断方法：在四个 `*_def_register` 的**同名覆盖分支**用
环境变量 `LENO_DEBUG_DUPNAME` 打印新旧定义的指纹（字段数 / 方法数 / 首个字段名）——
指纹相同 = 同一份定义被重复注册（无害），不同 = 真撞名。

| 语料 | DUPNAME 行 | 去重形态 | 同名异形 |
| --- | --- | --- | --- |
| `assert` 全部 270 个用例 | 318 | 25 | **0** |
| LenoWeb `crawl_all_quotes.leno` | 14 | 14 | **0** |

**方法学坑（单独记）**：第一次扫全语料只得到 **1 行** —— 因为没关缓存，绝大多数用例命中
`.lenosymc` / 字节码缓存、**根本没进语义阶段**。加 `LENO_NO_CACHE=1` 后才是 318 行。
**扫语料前先关缓存**，与 §8.48 的"先确认度量刻度"是同一类纪律：先确认被测的东西真的被执行了。

**另一条纪律：改之前先证伪自己的前提。** 本轮的起点判断是"struct 分支缺 face 检查会漏报"，
读了 `scope.c:166` 才发现 `scope_define` 在同作用域重名时返回 NULL、调用方的 `else` 仍会报错
—— 那三处只是**文案退化**，真正静默通过的只有 face 一处。若不先证伪，就会去改三处"不存在的
漏洞"。

**若要根治跨模块同名**（本轮只记账，未动）：二选一 —— 给全局定义表加模块维度（`find` 需要
模块上下文，牵动 VM / JIT 的 def 查找），或让 `mod.Type` 的限定解析不走全局表。属结构改动，
与 §8.47 的"调用边界税"同一档：不是删几条指令能解决的。

***

### 8.50 JIT 长跑栈溢出（`0xC00000FD`）：合并点 TOS 形态不一致 + `RDI`/`RSI` 未保存（2026-09-14）

**症状**：matrix_rain 这类「外层 while + 内层 for + 退出报告分桶循环」的程序，JIT 开到一定
执行量后必崩：3s（1567 帧）不崩、7s（3539 帧）不崩、**10s（≈5100 帧）3/3 崩**（退出码
`0xC00000FD`，RSP 撞栈底）；`LENO_NO_JIT=1` 干净退出；`LENO_JIT_DEBUG=1`（I/O 巨慢、
JIT 执行次数只有 1/20）也不崩 ⇒ 与**JIT 执行次数**而非墙钟相关。崩溃点固定在**退出报告
阶段**：stdout 停在分桶表最后一行，缺「整体平均」。

> 注意：这不是「加限时自动关闭」引入的。自动关闭只是让「长跑」变成常规操作，把原本
> 潜伏的 codegen bug 暴露出来；Leno 层那几行（`_args()` + 一句比较）与 JIT 无关，
> 关掉 JIT 就不再崩。

**最小复现**（无 GUI，秒级）：

```leno
var logs = []; var ms = []
for 0 : 20000 - 1 to i { logs.add(250 + (i % 50)); ms.add(1) }
var s = 0; var t = 0; var b0 = 0; var b1 = 49
while b0 <= 5000 {                          // 101 个桶
    for 0 : logs.len() - 1 to k {           // 每桶 20000 轮
        var dc = logs[k]
        if dc >= b0 and dc <= b1 { s += dc; t += ms[k] }
    }
    b0 += 50; b1 += 50
}
```

`if` 里去掉 `and` 短路同样崩；把 `s += dc` 换成 `s += 1` 也崩；**没有 `if` 就不崩**
（纯算术嵌套循环 2000 万轮也正常）。临界规模实测：101 桶 × 5000 轮/桶 = 505,000 轮不崩、
101 × 6000 = 606,000 轮就崩（≈4.4MB 漂移）⇒ **每轮迭代泄漏一个 8 字节 slot**。

**定位手段**（临时探针，已删除，思路留档）：在 codegen 的每条算子前插一段自检
`drift = (rbp - frame_sz - 8*内存栈项数) - rsp`，按 `bc_off` 分别记最大值 —— 漂移在某条
算子之后整段抬高，那条算子就是漏点。实测阶梯：

```
bc_off 0..77 = 15984      ← 1998 × 8
bc_off 90    = 15992      ← 1999 × 8，即 +8/轮
```

（探针必须放在 vstack 恢复**之后**，否则跳转目标处的 `vstack` 还是 `VSTACK_UNREACHABLE`
会被跳过；期望值的符号也踩过一次坑：内存栈项是 push 在 `rbp - frame_sz` **之下**，
期望应为 `rbp - frame_sz - 8*项数`。）

**根因①：合并点两条到达路径的 TOS 形态不一致**（`x86_64.c`）

短路 `and` 编译成两条 `JUMP_IF_FALSE`：第一条的 false 分支 push 一个 slot 后**跳到第二条
`JUMP_IF_FALSE` 本身**（bc_off 90），而第二条又能从上一条 `LE_INT` **直落**到达 —— 此时
TOS 还在 RAX 里（`tos_live=1`）。codegen 只按直落形态发码（内存栈项数 =
`vstack - tos_live` 记成 0），于是跳转路径 push 进去的那一项**永远没人消费**，每经过一次
这个合并点就漏 8 字节。（`patch_add` 只记了 vstack、没记 TOS 形态；而 vstack 恢复块只在
`VSTACK_UNREACHABLE` 时才把 `tos_live` 清 0，所以「直落可达的合并点」留下了这个分歧。）

**修法**：合并点统一到「TOS 在内存栈」形态 —— 若本偏移是某条前向跳转的目标
（新增 `patch_targets()`）且当前 `tos_live`，就在**直落路径**上补一条 `push rax`
（`vstack` 不变，只把 `tos_live` 清 0），并把 `offmap_add` 挪到这条 spill **之后**：
跳转路径落在 spill 之后（本来就是内存形态、不该再 push），直落路径执行 spill 后同样变成
内存形态，两条路径一致。值在两条路径上都在 RAX（条件跳转的内存形态分支会先
`mov rax,[rsp]` 读回），所以 push 的是正确的值。只在 `vstack` 可达（直落会执行）时才需要。

**根因②：`RDI`/`RSI` 没保存（Win64 nonvolatile）**

JIT 用 `RDI`/`RSI` 当 callout 实参寄存器（`JIT_ARG1/JIT_ARG2`），但序言只 push 了
`rbp/pin(4)/rbx` —— 每个含 callout 的 JIT 循环执行完都会破坏 C 调用方
（`jit_try_hot_loop` → 解释器主循环）的 `RDI`/`RSI`，解释器一旦把活值放在这两个寄存器里
就会用被污染的值访存/设栈。本轮是用「JIT 返回后 C 侧紧接着以 `%rsi` 为基址访存」的崩溃现场
抓到的。（SysV 上 `RDI`/`RSI` 是 volatile，多两条 push/pop 无害，故两平台统一保存。）
这一条与根因①**独立**：只修②不修①，最小复现照样崩。

**验证**

* 最小复现 `exit=0`，`s=5490000 t=20000` 与解释器逐位一致；断点探针零漂移。
* matrix_rain 10s × 3：全部 `exit=0`（修复前 3/3 崩），帧率无回归（≈5170 帧/10s vs 修复前 ≈5100）。
* 新增回归测试 `assert/test_jit_rsp_drift.leno`（§8.50 专用：`and`/单条件/`or` 三种短路形态
  + 值断言）：**修复版 3/3 pass；关掉修复的对照二进制 3/3 以 `0xC00000FD` 崩溃**。
* `assert` **274 passed / 0 failed**（273 + 新增 1）。
* 开/关 JIT 差分（stdout + 退出码，只比较 stdout 以免与 stderr 的统计块交错）：
  `examples/{struct,func,module_export_struct,cstruct}` **59 个文件零差异**；
  `assert` 里 JIT/控制流/循环/闭包相关 **62 个文件零差异**。
* `probe_local_fold` / `probe_eq_identity` / `probe_bailout_sites` / `probe_index_slowpath` /
  `probe_cast_int_peephole` 与 `LENO_NO_JIT=1` 输出一致；
  `probe_jit_gc_safepoint`（`bad=0 reused=0 sum=6.398e+06`）、`probe_method` / `probe_alloc2`
  的 `sum` 类结果一致（仅耗时数字不同）。

**教训**

1. **合并点必须约定唯一的栈形态**：跳转路径一律把 TOS 落栈，而直落路径可能把它留在寄存器。
   两者汇到同一个 bc_off 时，只记 `vstack` 是不够的，`tos_live` 必须一起归一 —— 否则
   「形态差一个 slot」会变成每轮一次的静默泄漏。修的时候要把 `offmap_add` 放到归一化
   代码之后，才能让跳转绕开它。
2. **跨 ABI 边界先数清「被调方必须保存」的寄存器**：Win64 的 `RDI`/`RSI` 很容易被当成
   「参数寄存器 = 随便用」而漏保存。这类 bug 只在 C 侧恰好把活值放在那里时发作，随执行次数显现。
3. **「与执行次数相关、与墙钟无关」的崩溃 ⇒ 优先怀疑每次迭代/调用漏一点栈**：按 `bc_off`
   记录漂移阶梯的探针能在几分钟内把漏点缩到一条算子；相比之下「盯着耗时/看汇编」要慢得多。
4. 想造差分探针时注意**别把 stdout 与 stderr 混到同一个文件**（JIT 统计在 stderr，会与
   stdout 交错，按区间过滤会把程序的真实输出行一起吃掉，表现为假的「缺行」）。

***

### 8.51 `OP_EQ`/`OP_NEQ` 遇 NaN-boxed 一律 bailout —— file_manager 五个热循环整循环退回解释器（2026-09-14）

**症状**：`leno_module/LenoSDL3/examples/应用示例/文件管理器/file_manager.leno` 交互一段后退出统计：

```
Compiled: 62  Executed: 73760  Bailouts: 15
Bailout: fn='_ensure_fitted_range' loop_bc=303 x3 — 非溢出类 @bc_off=398（= loop_bc 303 + 95）
Bailout: fn='_ensure_fitted_range' loop_bc=260 x3 — 非溢出类 @bc_off=131425（= loop_bc 260 + 131165）
Bailout: fn='render' loop_bc=380 x3 — 非溢出类 @bc_off=540
Bailout: fn='_topRects' loop_bc=246 x3 — 非溢出类 @bc_off=320
Bailout: fn='_colW' loop_bc=33 x3 — 非溢出类 @bc_off=117
```

5 个站点各回退满 3 次即被拉黑 ⇒ `Table.render` / `_ensure_fitted_range`（含 1 份内联副本）/
`MenuBar._topRects` / `MenuBar._colW` **永远跑解释器**。注意 `loop_bc` 是循环体起始偏移，
`rel=131165` 是内联帧基址 `0x10000 × depth` 没被解码（真实 `+93`，深度 2）。

**定位手段**：`LENO_JIT_DEBUG=1` + **自动扫鼠标制造热度**（空跑 8s 一次 bailout 都没有 ——
循环要跨 50 次命中阈值才会编译，交互式程序不制造热度就复现不出来）。解码 `_topRects`
（`bc_off=246`）循环体字节码：

```
[69] a1 00 00 12   OP_GET_FIELD_FAST   → _font（对象，RAX=0xfffc...）
[73] 01            OP_NULL
[74] 2f            OP_NEQ             ← 每次都在这条回退
```

对应 `sdl_menu.leno` 的 `if _font != null`。

**根因**：`ops_icmp.inc` 的 `OP_EQ`/`OP_NEQ` 只有「int48 快路径 + 对象身份快路径 + float 慢路径」，
**任一操作数是 NaN-boxed（字符串/数组/对象/null/BigInt/FFI 指针）就 bailout**。而
`x != null`、`s != ""` 这类比较在渲染/布局/菜单代码里遍布 ⇒ 这些循环一进去就回退、3 次拉黑。

**修复**

1. 新增 callout `jit_callout_value_eq(a, b, invert)`（`jit_callout.c` + `jit_priv.h` 声明）：
   语义**逐条复刻**解释器 `vm/vminc/op_compare.inc` 的 `OP_EQ` —— int/int、任一是 float 时按
   double（BigInt 与 float 混合同序）、BigInt 按值、与 null 比较的 `ObjFFIPointer`/`ObjFFICallback`
   看包装地址（**仅顶层**，数组元素不适用）、类型不同为假、同类型下 null/bool/string 按内容/
   array 逐元素/其余按身份。`OP_NEQ` 复用同一函数 `invert=1`。
2. `ops_icmp.inc`：EQ/NEQ 的 bailout 桩改成 callout 桩（`EMIT_RAW_TO_VALUE` → 传参 →
   `EMIT_VALUE_TO_RAW` → 用新跳转**跳过**裸 0/1 的 `EMIT_RAW01_TO_BOOLVAL`）。

**同一天内被连带修掉的两个「被 bailout 掩盖」的旧 bug**（同类问题，都是移除 bailout 后立刻暴露）：

* **(a) 身份判定失败跳数值慢路径时 `RAX` 已被改写成 `type_a`，没有恢复。**
  该路径上 `and rax, R10` + `mov_reg32_mem8` 把 `a` 换成了「类型编号」，而紧随其后的数值慢路径
  直接拿 `RAX` 当操作数 —— 以前那段紧跟着就是 bailout（解释器重跑），所以从未被发现。
  后果：`s1 == s2`（内容相同的两个字符串）、`arr1 == arr2`（逐元素）在 JIT 里**恒为「不相等」**，
  `!=` 恒为真（差分实测 `c3=50 c5=2950` —— 只有 JIT 接管前那 50 轮是对的）。
  修法：慢路径入口分两处 —— 类型检查直落的寄存器完好，用 `slow_body_jmp` 越过恢复块；
  身份判定跳来的先 `EMIT_LOAD_TMP` 从 `tmp1/tmp2` 恢复 `a`/`b`。
* **(b) `OP_NULL` 压的是字面量 0**（`ops_misc.inc` 注释写着「NULL = 0」）。但
  `val_is_null(v)` 是 `v == NULL_VAL` 的**精确比较**（`NULL_VAL = QNAN|SIGN_BIT|TAG_NULL`，非 0），
  且 `OP_JUMP_IF_FALSE` 明确分开比较 `FALSE_VAL` 与 `NULL_VAL` ⇒ JIT 里「null」与「整数 0」
  不可区分：`x == null` 在 x 为 0 时误判为真、**`null == null` 误判为假**。
  后果：`assert/test_gc_iterative_mark.leno`（`while cur != null` 遍历 5000 层链表）**死循环**
  （`JIT TIMEOUT(25s)`；`LENO_NO_JIT=1` exit=0）。修法：`emit_mov_reg_imm64(JIT_RAX, NULL_VAL)`。
3. 顺带修统计显示：非溢出类 site 的内联帧偏移剥掉 `0x10000 × depth` 基址并标出内联深度
   （此前报 `131425` 这种假偏移，误导排查方向）。

**验证**

* `file_manager` 同一自动交互负载（7s 扫鼠标）：`Bailouts: 15 → 0`，`Compiled 30 / Executed 878`，
  `[JIT-DEBUG] BAILOUT` 行 0 条。
* `assert` 全套 **276 passed / 0 failed**。
* 自写差分探针（对象 vs null、字符串按内容、数组逐元素、`null==null`、类型不同、`!=` 全反向）：
  JIT 与 `LENO_NO_JIT=1` **逐条一致**（修复前 `c3=50 / c5=2950`）。
* 修复中途一度让 `test_gc_iterative_mark.leno` 卡死 —— 那正是 (b) 的暴露，不是新引入的独立问题；
  用「`jit.gc_eq` 复现脚本按进度打印 A/B/C」把卡点缩到 `count()` 的 `while cur != null`。

**教训**

1. **bailout 会掩盖同一条路径上的旧错误假设。** 这条路径以前「一进来就回退」，所以
   「RAX 被改写」「null 表示错误」这类问题永远不会被执行到。**把 bailout 换成 callout / 原生
   实现时，必须把那条路径从入口到出口重新核一遍**（本次 3 个 bug 里 2 个是这么暴露的）。
2. 判「热点循环进没进 JIT」不能只看 `Bailouts`：`Bailouts=0` 也可能是**根本没编译成功**
   （scan 拒收）。必须同时看 `Compiled` / `Cached` 与 `scan FAIL` 行数（第 5 节第 1/2 类）。
3. 复现此类问题要**制造热度**：交互程序空跑不热（阈值 50 次命中），自动扫鼠标/长跑才有复现率；
   现象与**执行次数**相关、与墙钟无关。
4. `val_is_null` / `val_is_bool` 这类**精确比较**的谓词是「表示不一致」类 bug 的照妖镜：
   写任何「压 null / 比 null」的 codegen 前先 grep 它们的定义。

***

### 8.52 `OP_LENGTH` 进 JIT：数字原生 + 对象 callout（§5 覆盖面 L1）（2026-09-14）

**背景**：§5 盘点的 56 项未收录 opcode 里，`OP_LENGTH`（编号 **80**）是 `scan FAIL` 日志中出现
最频繁的一个（`.len()` 在 SDL3 库里遍地都是）。它此前不在 `opcode_size()` 里 ⇒
**任何含 `.len()` 的循环都被整循环拒收**（连编译都不做）。

**实现（4 处）**

1. `jit_scan.c`：`opcode_size()` 补 `OP_LENGTH → 1 字节`；`scan_loop_body()` 与
   `scan_callee_for_inline()` 各补 `case OP_LENGTH`（栈净效应 0：pop 1 push 1）。
   **三处必须同步**，漏最后一处会出现「循环能编，但含 `.len()` 的被调函数无法内联」。
2. `jit_callout.c` + `jit_priv.h`：新增 `jit_callout_length(Value)`，语义逐条对齐
   `op_utils.inc` 的 `OP_LENGTH`：数字 → `(int)value_to_double` 截断、负数 clamp 到 0；
   `string→char_len`（**字符数**，不是字节数）、`array→count`、`dict→order_count`、
   `enum_def→member_count`、`cstruct_array→count`、`struct→def->field_count`。
   **非法类型不在这里造错误** ⇒ 置 `jit_callout_failed` → bailout → 解释器重放本条指令，
   报错文本/行号与 `LENO_NO_JIT=1` 完全一致。
3. `ops_misc.inc`：新增 `case OP_LENGTH`，双路径 ——
   * **数字（原生）**：`EMIT_NUM_TO_XMM`（int48 与裸 double 都吃）→ **32 位** `CVTTSD2SI eax, xmm0`
     \+ `movsxd rax, eax` → `test` + `jns`，负数 `xor` 成 0 ⇒ 与解释器的
     `(int)double` + `len < 0 → 0` 逐位一致。
     用 32 位而非 64 位是关键：`(int)1e10` 在 x64 硬件上得到 `INT32_MIN` → clamp 成 0；
     64 位版本会保留 `10000000000`，那才是 JIT/解释器分歧。
   * **对象 / 非法类型（callout）**：`jit_callout_length` + `jit_callout_failed` 检查
     （`JNZ → bailout` 桩）。
   * 两条路径结果都是 int（callout 侧经 `EMIT_VALUE_TO_RAW`）⇒ `MARK_TOS_RAW_INT48()`（§8.41）。
4. `assert/test_jit_op_length.leno`：覆盖 string（中文串，验「字符数 ≠ 字节数」）/ Array / Dict /
   字符串常量 / 数字形态（数字形态只有 `for n to i`（n 是变量）会产生 `OP_LENGTH`，且必须落在
   外层循环体内才被 JIT 覆盖 ⇒ 用嵌套 `for`）。循环跑 3000 轮（> 热阈值 50）让 JIT 接管，
   JIT 与 `LENO_NO_JIT=1` 两种模式都必须通过。

**验证**

* 新增测试：JIT `exit=0`、`LENO_NO_JIT=1` `exit=0`，长度值 `str=5 arr=4 dict=2 const=4 num=3` 两边一致。
* 该循环在 `LENO_JIT_DEBUG=1` 下 `capable=1`、`Compiled 2 / Executed 35 / Bailouts 0`，
  且 `scan FAIL` 与 `BAILOUT` 均 0 行 ⇒ 确实**在 JIT 里**跑，不是「没编成」。
* `file_manager` 自动交互负载：日志里 `unknown opcode 80 (OP_LENGTH)` 归零，`Bailouts` 仍 0；
  `assert/test_gc_iterative_mark.leno` 正常 `exit=0`。

**必须记住的结论：单补一个 opcode ≠ 解锁循环。**
`file_manager` 的 `scan FAIL` **总行数仍是 16**，只是 `OP_LENGTH` 那几条消失、**拒收点前移**到下一个缺口：

```
op=81 (OP_ITER_GET)          ×4      ← 现在最高频
op=132(OP_SET_FIELD)         ×2
op=156(OP_SWITCH_LOOKUP)     ×2
op=39 (OP_SET_DECLARED_FACE) ×2
op=88 (OP_GET_MODULE_VAR)    ×2
op=38 (OP_SET_PTR_ELEM_TYPE) ×1
inline-scan: 132 / 76(OP_STRING_ADD) / 88   ← 只影响内联，不阻断循环编译
```

只有当某个循环的**全部**缺口都被补齐，它才真正进 JIT ⇒ 这类工作要**成批推进**，
并按实测直方图排序（§5 的表已按此顺序更新）。

**教训**

1. 判「收益」不能看 `scan FAIL` 行数，要看 **opcode 直方图 + 每个循环的缺口集合**。
2. 解释器侧存在 UB 的地方（`(int)double` 越界），JIT 必须**复刻硬件实际行为**
   （32 位 `CVTTSD2SI` → `INT32_MIN`），而不是「数学上更合理」的 64 位结果。
3. 用 `LENO_JIT_DEBUG=1` 时必须同时看 `capable` / `Compiled` / `Executed`：
   **结果对 ≠ 走了 JIT** —— 扫描拒绝后解释器照样算出正确结果，探针会静默失效。
4. 别把两次运行（JIT / `LENO_NO_JIT`）的输出串着看：stdout 与 stderr 交错会让人把
   「解释器的结果」当成「JIT 的结果」（本次就被 `str=5 …` 那行串过一次）。分开跑、分开读。

***

### 8.53 `OP_ITER_GET` / `OP_ITER_GET_VALUE` 进 JIT：数组原生 + 其余 callout（§5 覆盖面 L2）（2026-09-14）

**背景**：补完 L1 后，`file_manager` 拒收直方图里最高频的是 `op=81`（×4）。
语法 `for 集合 to 元素[, 索引]`（`codegen_stmt.c` 的 for 生成器）会发这组指令：
`ITER_GET`（array→元素 / dict→**键** / string→单字符 / struct→字段名 / 数字→索引本身）
\+ `ITER_GET_VALUE`（dict→值 / struct→字段值）；两者都是「弹 obj + index，压 1 个结果」，net -1。

**实现（4 处）**

1. `jit_scan.c`：`opcode_size()` 补 1 字节；`scan_loop_body()` 与 `scan_callee_for_inline()`
   各补 case（net -1，与 `OP_INDEX` 同组）。
2. `jit_callout.c` + `jit_priv.h`：`jit_callout_iter_get(obj, index, want_value)`，
   语义逐条对齐 `vm/vminc/op_utils.inc`（ITER\_GET）与 `vm/vminc/op_iter.inc`（ITER\_GET\_VALUE）。
3. `ops_index.inc`：原生快路径（**仅 ITER\_GET**：obj 是数组 + index 是 int48 +
   无符号比较判界内 → 直接 `mov rax, [elements + idx*8]`）+ callout 慢路径。
   所有错误与越界由 callout 置 `jit_callout_failed` → bailout → 解释器重放本条指令，
   保证异常语义与 `try/catch` 可捕获性（§8.28 的教训）。
4. `assert/test_jit_op_iter.leno`：四种形态各 3000 轮，JIT 与 `LENO_NO_JIT=1` 都必须过。

**踩坑①（本次自引入，与 §8.51(a) 同类）：跨 `EMIT_RAW_TO_VALUE()` 用 R8 保存值**
第一次实现每次迭代都 bailout。诊断打印显示 callout 收到的 `index` 是**未装箱的裸位模式**
（`0xffff…fff9`）—— 因为 `EMIT_RAW_TO_VALUE()` **以 R8 为 scratch**，而我把 index 放进 R8
*之后*又调它去转换 obj，R8 被踩。修法：两次转换都在 RAX 上做完、就地写回 tmp 槽；
实参寄存器一律留到 `EMIT_CALLOUT_BEGIN()` 之后再载入（与 `OP_INDEX` 的既有写法同构）。

**踩坑②：`jit_callout_failed` 粘滞 ⇒ 「1 次真因 + N 次连锁」**
上面那个真因只发生 **1 次**，但统计报 `Bailouts: 15`（5 个循环 × 3 次）。原因：codegen 只在
「检查通过的成功路径」上清标志，bailout 那条路没机会清，于是标志一直保持 1，
此后每次进入**任何**带检查的循环都会在第一条检查处立刻 bailout，白烧 `JIT_BAILOUT_LIMIT`
（3 次后循环被拉黑）。修法：`jit_try_hot_loop` 执行机器码前复位一次
（与函数级入口 `jit_try_hot_func_call` 的做法一致）。
**排查提示**：看 `Bailouts` 时要定位**第一次**失败点（诊断行取 `-First 1`），否则会被连锁数字误导。

**验证**

* 新增测试：JIT / `LENO_NO_JIT=1` 都 `exit=0`，`sum=48000 cnt=18000 keylen=9000 chsum=9000` 两边一致。
* 同一用例 `LENO_JIT_DEBUG=1`：`capable=1`、`Executed 145`、**`Bailouts 0`**（修复前 15）。
* `assert` 全套 **278 passed / 0 failed**（277 + 新增 1）。
* `file_manager` 自动交互负载：`unknown opcode 81` 归零、运行时 bailout 仍 **0**、
  `Compiled 27 / Executed 491`。
* 直方图继续前移（见 §5）：现为 `132(SET_FIELD) / 134(GET_METHOD) / 156(SWITCH_LOOKUP) /
  39 / 88 / 90(GET_MODULE_FUNC) / 38`。

**教训**

1. **宏的副作用就是契约**：`EMIT_RAW_TO_VALUE` / `EMIT_VALUE_TO_RAW` 都以 R8 为 scratch ——
   写新 case 前先读宏定义。这与 §8.51(a) 的「RAX 被改写成 type_a」是**同一类**错误，
   一天内出现两次，说明「新增 codegen 分支」的检查清单里必须包含「我用的每个宏动了哪些寄存器」。
2. 诊断打印要一次给足（类型 + 位模式 + 参数归属）：`obj=0xfffc…(objtype=2) / idx=0xffff…fff9 /
   want=0` 一行就能判定「实参被踩成裸位模式」，比对着汇编猜快得多。
3. 全局状态标志（`jit_callout_failed`）必须有**明确的复位点且覆盖所有入口**（函数级 + 循环级），
   否则错误会跨执行粘滞，把统计数字和 bailout 预算一起带偏。

***

### 8.54 `OP_GET_FIELD` / `OP_SET_FIELD` 进 JIT —— 顺带解锁 3 个函数的函数级 JIT（§5 覆盖面 L3）（2026-09-14）

**背景**：L1/L2 之后 `file_manager` 拒收直方图里 `op=132`（`OP_SET_FIELD`）出现 ×2。看 `op_struct.inc`：

* `OP_GET_FIELD`（2 字节：op + `field_idx`）：弹 obj、压 field（净 0）。struct 走越界检查 +
  `struct_get_field`；cstruct 有 str16 转换 / 数组视图 / 嵌套 cstruct 三条**分配**路径。
* `OP_SET_FIELD`（2 字节）：弹 value + obj、**压回 value**（赋值表达式的值，净 -1）。
  struct 有 int→float / bigint→float 自动提升 + GC 写屏障。

**实现（4 处）**

1. `jit_scan.c`：`opcode_size()` 2 字节组补两条；`scan_loop_body()`（GET 净 0 / SET 净 -1）
   与 `scan_callee_for_inline()` 同步。
2. `jit_callout.c` + `jit_priv.h`：
   * `jit_callout_get_field(obj, field_idx)`：实现 **struct 路径**（越界检查 + `struct_get_field`）；
     cstruct 与非 struct/cstruct 的错误路径置 `failed` → bailout → 解释器重放
     （那三条分配路径语义复杂，交解释器最稳，报错文本也天然一致）。
   * `jit_callout_set_field(obj, field_idx, value)`：struct 路径完整复刻（越界 +
     int→float / bigint→float 提升 + `struct_set_field`），cstruct 路径也实现
     （数值自动转换 + `cstruct_set_field_value`）。
   * **写入一律走 `struct_set_field`**：它是 `leno_value.h` 里 static inline 的唯一写入入口、
     内含写屏障；JIT 不自己发内联写，避免漏屏障破坏 GC 契约（§8.36）。
3. `ops_callout.inc`：两条都走 callout 的 codegen（净效应 GET 0 / SET -1）。
   `SET` 的 3 个实参顺序按 §8.53 踩坑① 的规矩来：先把两个操作数就地转成 Value 写回 tmp 槽，
   再在 `EMIT_CALLOUT_BEGIN()` 之后载入实参寄存器（`EMIT_RAW_TO_VALUE` 以 R8 为 scratch）。
4. `assert/test_jit_op_field.leno`：int 字段、float 字段（值静态类型不可知 ⇒ 必须走提升）、
   嵌套 struct 字段、string 字段（对象值 ⇒ 写屏障路径）四种形态各 3000 轮。

**验证**

* 新增测试：JIT / `LENO_NO_JIT=1` 都 `exit=0`，`sum_i=4498500 sum_f=21000.0 cnt=18000` 两边一致。
* 该用例 `LENO_JIT_DEBUG=1`：`capable=1`、`scan FAIL 0`、`FIELD-FAIL 0`、**`Bailouts 0`**。
* `file_manager` 自动交互负载：`unknown opcode 132` 归零；运行时 bailout 仍 **0**；
  直方图剩 `134(GET_METHOD) / 156(SWITCH_LOOKUP) / 39 / 88 / 90(GET_MODULE_FUNC) / 38`。
* **额外收益：函数级 JIT 首次编到 3 个 SDL 包装函数** —— `set_pos` / `set_size` /
  `setWindowHandle`（`FuncCompiled 3 / FuncExecuted 186`）。它们的函数体里就写着
  `_hwnd = hwnd` 这类 `OP_SET_FIELD`，以前整个函数被 scan 拒收、只能解释执行。
* `assert` 全套 **279 passed / 0 failed**（278 + 新增 1）。

**教训**

1. 一个 opcode 的覆盖面同时决定**三件事**：循环能否 JIT、被调函数能否内联、函数级 JIT 能否编译。
   本次就是「顺手解锁函数级 JIT」的例子 —— 评估收益时三条都要算。
2. 涉及**堆写入**的 opcode（这里 `SET_FIELD`）必须复用 VM 的写入入口
   （`struct_set_field` / `cstruct_set_field_value`），不要在 codegen 里重写、更不要漏写屏障：
   漏屏障不会立刻崩，而是在下一次 GC 时静默丢对象（§8.36 的同类问题）。
3. 静态类型已知时编译器发的是 `OP_GET_FIELD_FAST`（JIT 早已支持）；现在 `OP_GET_FIELD` 也支持了，
   两条路径不要混淆：FAST 版**没有**运行时越界检查（靠编译期保证），通用版有。

***

### 8.55 模块变量/函数进 JIT（§5 覆盖面 L6）—— 顺带解锁 2 个函数的函数级 JIT；L4 优先级修正（2026-09-14）

**背景与优先级修正**：原计划 L4 = `OP_GET_METHOD` + `OP_SWITCH_LOOKUP`。动手前先查清 `GET_METHOD` 的产出场景
（`codegen_expr.c`），结论是**它永远后跟 `OP_CALL` / `OP_ASYNC_CALL`** —— 原生方法、face 动态派发、
async/未知方法定义、安全访问（`obj?.m()`）四条路径都是这个形态。而 `OP_CALL` 属 L8（未做）
⇒ **单补 `GET_METHOD` 解锁不了任何循环**，是 §5 那条「单补一个 opcode ≠ 解锁循环」的又一次应验。

于是先做 L6：`OP_GET_MODULE_VAR`(88) / `OP_SET_MODULE_VAR`(89) / `OP_GET_MODULE_FUNC`(90) ——
三条都是 callout 型、无控制流、累计频次 ×6，且直方图里 `88` / `90` 同时在榜。

**解释器语义**（`vm/vminc/op_module_var.inc`）

* `OP_GET_MODULE_VAR` / `OP_GET_MODULE_FUNC`（3 字节：op + index16）：压入 `module->globals[index]`（净 +1）。
  解释器里两者实现完全相同，区别只在编译期（取变量还是取函数值）。
* `OP_SET_MODULE_VAR`（3 字节）：**`peek` TOS**（不弹栈）→ `globals[index] = value;`
  `gc_write_barrier((Object*)module, value);`（净 0）。
* `module` 一律取**当前帧**的 `frame->module`，而它来自被调函数（`vm_call.inc: frame->module = func->module`）。

**实现（4 处）**

1. `jit_scan.c`：`opcode_size()` 3 字节组补三条；`scan_loop_body()` 加净 +1（GET_VAR/GET_FUNC）与净 0（SET_VAR）；
   **内联扫描直接拒绝这三条** —— 内联后 JIT 手上只有调用方的帧，被调函数来自别的模块就会读错模块的变量，
   语义依赖运行时帧（与泛型 `OP_STRUCT_INIT` 同理），拒绝内联即可，循环本身照常 JIT。
2. `jit_callout.c` + `jit_priv.h`：`jit_callout_get_module_var(module, index)` /
   `jit_callout_set_module_var(module, index, value)`。module 由 codegen **编译期嵌入**
   （`jit_scan_get_module()`）—— 详见 §8.56 的修正：初版是运行时查 `frames[frame_cnt-1].module`，
   在函数级 JIT 快路径（不压帧）下会读到**调用方**的模块；写入照抄解释器的 `gc_write_barrier`
   （模块是 GC 根）。无模块 / 索引越界 → failed → bailout 交解释器报错。
3. `ops_callout.inc`：GET 走标准「callout → `EMIT_VALUE_TO_RAW` → 失败标志检查 → `TOS_PRODUCE` / `vstack++`」；
   SET 用 `TOS_SPILL()` + 从 `[rsp]` 读 TOS，callout 之后**不做 `TOS_PRODUCE`、不动 vstack**
   （解释器 peek 语义：TOS 原样保留）。
4. `assert/test_jit_op_module_var.leno` + 辅助模块 `assert/jit_modvar_mod.leno`：模块内函数的 3000 轮热循环，
   覆盖 int 模块变量读写、对象值写入（写屏障）、模块函数取值。

**验证**

* 新用例：JIT 与 `LENO_NO_JIT=1` 都通过（`r=3004531504`）；`LENO_JIT_DEBUG=1` 下
  `capable=1 / scan FAIL 0 / CALLOUT-FAIL 0 / Bailouts 0`，且 `inline-scan FAIL: 模块变量访问 op=88`
  如期出现（证明「拒绝内联」那条守卫生效）。
* `file_manager` 交互负载：直方图里 `88` / `90` 消失（拒收点前移到 **`59:OP_CALL ×4`**），运行时 bailout 仍 0。
* **函数级 JIT 再 +2**：`ttfLib` / `lib`（各 162 字节，函数体里就是模块变量访问）进入函数级 JIT，
  `FuncCompiled 4 / FuncExecuted 171`。

**教训**

1. 动手前先查「这个 opcode 是不是**总是**和另一个未支持的 opcode 绑在一起」——
   `GET_METHOD` 就是要配套 `OP_CALL` 的典型，一次 `codegen` 检索省掉整轮返工。
2. 需要**模块上下文**的 opcode 要区分三种 JIT 形态：**内联**不行（内联把被调函数的帧抹掉了）
   ⇒ 内联扫描必须显式拒绝，否则会静默读错模块的变量。
   本节初版还犯了第二个错：以为「循环 JIT / 函数级 JIT 都能从 `frames[frame_cnt-1]` 拿到当前帧」——
   函数级 JIT 的快路径**根本不压帧**（见 §8.56 的修正）。正确做法是把模块当作**编译期已知量**
   嵌进机器码，运行时一次帧都不用查。
3. `peek` 型 opcode（净 0、不改 TOS）的 codegen 与 push/pop 型不同：**不能** `TOS_PRODUCE()`，
   否则栈顶会被改写成 callout 的返回值。

***

### 8.56 模块函数调用进 JIT（`GET_MODULE_FUNC + OP_CALL` 窥孔）—— 并修正 §8.55 的模块归属（2026-09-14）

**背景**：补完 L6 后直方图新暴露 `59:OP_CALL ×4`。查产出路径（`codegen_expr.c` 的 `SYM_MODULE` 分支）：
模块**内部**调用同模块函数（`foo(x)`）编译为 `OP_GET_MODULE_FUNC idx(3B)` + `OP_CALL argc(3B)`，
而跨模块的 `m.f()` 走的是「模块对象 + `GET_PROPERTY`/`GET_METHOD`」形态 ⇒ 这个窥孔只覆盖模块内部调用
—— 正好就是 `file_manager` 里那 4 个循环的形态。

**返回值个数（本轮的关键前提）**：`ObjFunction.return_count` 是**编译期**算好的
（`codegen_func.c` 注释就写着「供 JIT 等消费方使用」）：

* 无显式 return ⇒ **1** 个（隐式 null）；
* 各 return 个数不一致 / fall-through 且 != 1 ⇒ **-1**（静态不可知）→ **scan 直接拒绝该循环**，绝不猜。

多返回值错记账正是 §8.48 那个「第二个返回值落在实参槽上 → 数值每帧乱跳」的老 bug 形态，
所以这里宁可拒绝 JIT 也不退化成「按 1 个处理」。

**实现（4 处）**

1. `jit_scan.c`：
   * 新增 `jit_resolve_module_func(index)`：从**编译期模块**的 `globals[index]` 取闭包 → 返回
     `return_count`（0 = 解析失败 / 不可知）。
   * `scan_loop_body` 的 `OP_GET_MODULE_FUNC` case：后随 `OP_CALL` 时合并（`size = 6`），
     栈效应 `vstack -= (argc - rc)`；解析不出 `rc` 就 `capable = 0` 拒绝整个循环。
   * 内联扫描继续拒绝模块变量/函数（内联体的模块与调用方不同，§8.55）；**裸 `OP_CALL`**
     （callee 是局部/上值的闭包值）也继续拒绝 —— `opcode_size(OP_CALL)` 保持未收录，
     「非窥孔形态」自动拒收，不需要额外代码。
2. `jit_callout.c`：`jit_callout_call_module_func(module, vstack_top, argc, index, rc)` ——
   callee 从**编译期模块**现取并复核 `return_count`（模块变量可能被重新赋值换了 callee，不一致就
   bailout）；先走「callee 自身函数级 JIT」快路径，失败再 VM 重入；多返回值沿用
   `jit_callout_invoke_method` 的回填约定（前 `rc-1` 个写回实参槽）。
3. `ops_callout.inc`：合并路径 codegen，弹 `(argc - rc + 1)` 个槽（第 5 个参数在 Win64 走
   `[RSP+32]`，与 `OP_STRUCT_INIT`/`MODULE_CALL` 同款写法）。
4. `assert/test_jit_op_module_call.leno` + 辅助模块 `jit_modcall_mod.leno` / `jit_modcall_other.leno`：
   单返回值热循环、多返回值热循环、跨模块模块变量归属。

**⚠ 同时修正 §8.55 的一个错误**：L6 的 callout 原本在运行时读 `vm.frames[frame_cnt-1].module` 取模块，
这个取法是**错的** —— 函数级 JIT 的快路径（`jit_try_hot_func_call` / `jit_callout_invoke_method` 的快路径）
**不压帧**（flocals 直接从 VM 栈装填、机器码直调，随后折叠栈），此时栈顶帧是**调用方**的帧
⇒ 跨模块调用时会读调用方的模块，若对方的 `globals[index]` 恰好是另一个变量就**静默读错值**
（只有对方 `global_count` 不够大时才退化成 bailout，所以很容易蒙混过关）。

修法：模块归属本来就是**编译期已知**的（函数 → module），改为由 codegen 把模块指针**嵌进机器码**
（`jit_scan_get_module()`：循环 JIT 用 `frame->closure->function->module`，函数级 JIT 用 `func->module`），
callout 全程不再查运行时帧。`test_jit_op_module_call.leno` 里 `other.hammer()` 那段就是这个语义的守卫。

**验证**

* 新用例：JIT 与 `LENO_NO_JIT=1` 均通过；`LENO_JIT_DEBUG=1` 下 `run`（单返回值）与 `runPair`（多返回值）
  都是 `capable=1`，`scan FAIL` 只出现在 `hammer`（走模块对象方法调用，预期），
  `CALLOUT-FAIL 0 / Bailouts 0`，callee `single` 被编成函数级 JIT（`func compiled: 'single'`）。
* `file_manager` 交互负载：`59(OP_CALL)` 从 ×4 降到 **×2**（另 2 个循环的调用不是 `GET_MODULE_FUNC` 形态），
  无 `ret_count 不可知` 拒收，运行时 bailout 仍 **0**；`FuncCompiled 5 / FuncExecuted 217`。
* L2/L3/L6 既有用例 + `assert` 全套 **282 passed / 0 failed**。

**教训**

1. 决定**栈记账**的信息（返回值个数）宁可"解析不出来就拒绝"，也不能退化成「按 1 个」——
   后者是静默的数值错乱（§8.48 的老 bug 形态）。
2. 窥孔只在**确定形态**下成立：`GET_MODULE_FUNC + OP_CALL` 覆盖模块内部调用；跨模块 `m.f()` 是另一种
   形态（模块对象 + `GET_PROPERTY`），别指望一个窥孔吃两种。
3. **三种执行形态（循环 / 函数级 / 内联）的帧形态不同**，是实现与评审时最容易漏的一环：
   凡是想"从当前帧拿上下文"的实现，都要先确认这三种形态下都成立 —— §8.55 的错误就出在这里。

***

### 8.57 `OP_GET_METHOD + OP_CALL` 进 JIT（动态派发方法调用）—— 并踩到"弹栈数比计数多 1"（2026-09-14）

**背景**：补完 §8.56 后直方图剩 `134(OP_GET_METHOD) ×2`。`GET_METHOD` 的产出场景（`codegen_expr.c`）：
face 动态派发、接收者静态类型解析不出来、原生方法、async/未知方法定义 —— **四条都后跟 `OP_CALL`**，
所以和 `GET_MODULE_FUNC + OP_CALL` 一样必须做成窥孔（单补 `GET_METHOD` 解锁不了循环）。

**栈布局（用最小探针反汇编实测，别再靠猜）**：

```
0085  OP_GET_LOCAL   0        // self（作为 args[0]）
0088  OP_GET_LOCAL   0        // 供 OP_GET_METHOD 消费的额外 receiver
0091  OP_GET_METHOD  8 (area)
0094  OP_CALL        1        // argc 含 self
```

⇒ 调用方在 `[self][args...]` 之上**多压了一个 receiver**。合并后的逻辑净效应 = `-(argc + 1) + rc`；
原生方法形态（先压实参、再压 receiver，`OP_CALL` 的 argc 不含 self）恰好也是「argc + 1 个值待消费」，
所以记账统一。

**实现（3 处）**

1. `jit_scan.c`：`opcode_size` 把 `OP_GET_METHOD` 归入 3 字节组；`scan_loop_body` 新增 case ——
   后随 `OP_CALL` 时合并（`size = 6`，`vstack -= argc + 1 - rc`）；`rc` 只能按
   `jit_resolve_method_ret_count`（方法名在所有 struct def 中唯一、return_count 一致）推断，
   推不出来就拒绝整个循环；**独立的 `OP_GET_METHOD`**（只取方法值）要建 bound method/闭包 ⇒ 也拒绝。
2. `ops_callout.inc`：`OP_GET_METHOD` 合并路径，复用 `jit_callout_invoke_method` ——
   传给它的 `vstack_top` 要 `+8`（跳过那个额外 receiver 槽），这样它看到的正好是
   「receiver 在 `[argc-1]`」的实参块。
3. `jit_callout.c`：给 `jit_callout_invoke_method` 的两条错误路径补上 `jit_callout_failed = 1`
   （见教训 3）。

**⚠ 本轮踩的坑：内存弹栈数 ≠ vstack 计数（差 1）**

第一版 codegen 弹 `(argc + 1 - rc)` 个槽（= 照抄逻辑净效应），结果 **A 形状（纯算术）正常、
B 形状（`acc = acc + a.area()`）稳定 bailout 3 次**（`site=28` → 报 "int48 溢出/截断"，
随后被 `JIT_BAILOUT_LIMIT` 拉黑）。原因是：

* 内存里待消费 = 实参块(`argc`) + 额外 receiver(1)；
* 保留在内存的 = 多返回值的前 `rc-1` 个（callout 已写回实参槽），最后一个在 RAX；
* ⇒ 应弹 `(argc + 1) - (rc - 1) = argc - rc + 2` 个槽 —— **比逻辑计数多 1**（TOS 在寄存器里，
  内存弹栈数天然比计数差 1；`INVOKE_METHOD` 的 `argc - rc + 1` 同款）。

少弹 1 格 ⇒ RSP 漂移 ⇒ 紧随其后的 `OP_ADD_INT` 读到错槽（值不是合法 int48）⇒ bailout。
**这次是 bailout 救了正确性**（栈漂移若没被检查拦住就是静默算错）。

定位手段：**形状二分**比手解 hex 快得多 —— 同一个文件里放三条循环（纯算术 / 调用结果直接参与
运算 / 调用结果先落局部），一次运行就看得出是哪一种形态触发，再去反汇编那条循环。

**验证**

* 新用例 `assert/test_jit_op_get_method.leno`（face 派发、两种实现交替调用 3000 轮）：
  JIT 与 `LENO_NO_JIT=1` 都通过，且 JIT 侧 **`Bailouts: 0`**（修漂移前是 3）。
* 形状探针 `acc=4552500`（= 4498500 + 27000 + 27000）在 JIT / NO_JIT 下**一致**。
* `file_manager` 交互负载：`134(OP_GET_METHOD)` 归零，直方图剩
  `156(SWITCH_LOOKUP) ×2 / 39 ×2 / 38 ×1 / 59(OP_CALL) ×2`；运行时 bailout 仍 **0**。
* `assert` 全套 **282 passed / 0 failed**。

**教训**

1. **callout 型 opcode 的「内存弹栈数」与「vstack 计数」是两个数**：TOS 在寄存器里时，
   弹栈数 = |vstack 净变化| + 1（rc ≥ 1）。写新窥孔时两者都要显式算一遍，别用一个推另一个。
2. 定位栈漂移用**形状二分**（把一条语句拆成几种写法，一次跑完看哪条 bailout），
   比手解字节码 hex 快一个数量级 —— 本轮就是靠它 30 秒锁定。
3. 复用既有 callout 时，要逐个检查它「理论上不该发生」的错误路径**是否置 `failed`**：
   `jit_callout_invoke_method` 原来对非 struct 接收者只 `error_add_at` 就 `return NULL_VAL`
   —— 对 `INVOKE_METHOD_TYPED`（静态保证是 struct）无害，但被动态派发复用后，
   一旦布局判断失手就会**带着 NULL 继续**（静默算错）。已补 `jit_callout_failed = 1`，
   配合循环侧的 `JIT_BAILOUT_LIMIT`（最坏回退几次即拉黑）兜底。

***

### 8.58 存值前的「类型标记」进 JIT（`OP_SET_PTR_ELEM_TYPE` / `OP_SET_DECLARED_FACE`，覆盖面 L5）（2026-09-14）

**背景**：直方图剩 `39:SET_DECLARED_FACE ×2` + `38:SET_PTR_ELEM_TYPE ×1`。两条都是
「**存值前给栈顶打标记**」（`vm/vminc/op_unary.inc`）：

* `OP_SET_PTR_ELEM_TYPE`（2 字节：op + elem_type）：TOS 是 `ObjFFIPointer`（且未 freed）
  → `ptr->element_type = elem_type`；**peek、不弹不推、类型不匹配静默不做**。
* `OP_SET_DECLARED_FACE`（3 字节：op + name_const16）：TOS 是 `ObjStruct`
  → `obj->declared_face = chunk 常量里的名字`；同样 peek、静默。

发射点固定在 `codegen_stmt.c` 的 var-decl / 赋值路径（`Ptr[T]` 与 `face` 声明各 4 处），
位置固定在「值已压栈、存槽之前」。

**为什么不能当 no-op 跳过**（§5 表里早就标过）：`declared_face` 是
`module/types/types.c` 做**数组元素类型推断**的依据（有它 ⇒ 元素类型推断为 `TYPE_FACE` 而不是
struct），也是 GC 的标记对象（`gc.c`）；`element_type` 决定 FFI 读写的宽度。

**实现（4 处）**：`opcode_size`（2 字节 / 3 字节）＋ 两处 scan 的净 0 组 ＋
两个 callout（`jit_callout_set_ptr_elem_type` / `jit_callout_set_declared_face`，
逐字照抄解释器，包括「类型不匹配静默不做」）＋ codegen 的 peek 型写法
（`TOS_SPILL()` → 读 `[rsp]` → 调 callout，**不 `TOS_PRODUCE()`、不动 vstack**，
与 §8.55 的 `SET_MODULE_VAR` 同款）。
**这两条不需要失败通道**：解释器本身不报错、语义就是"能设就设" ⇒ callout 不置 `failed`，
少一个 bailout 来源。

**测试（`assert/test_jit_op_type_tag.leno`）**：断言特意做成**自洽**的 —— 把「循环内用 face 元素
构造的数组」与「循环外同样方式构造的数组」的 `type()` 对比：若 JIT 跳过了 `declared_face` 写入，
循环内那个数组会被推断成 struct 元素类型 ⇒ 两者不等（比硬编码类型串更耐改）。
`OP_SET_PTR_ELEM_TYPE` 用 `Ptr[u8] q = null` 覆盖 JIT 侧代码路径（两种模式都静默 no-op）。
真实 FFI 指针形态（`&c.r`）由 file_manager 冒烟负载覆盖 —— 注意 `&` 取地址是
`OP_GET_FIELD_ADDR`，它在"建议维持拒绝"清单里，所以**含它的循环不会进 JIT**，
这也是这条 opcode 没法写 JIT 单测的原因。

**验证**

* 新用例：JIT 与 `LENO_NO_JIT=1` 均通过；JIT 侧 `capable=1`、`Bailouts: 0`。
* `file_manager` 冒烟负载：`38` / `39` 归零，运行时 bailout 仍 **0**
  （`FuncCompiled` / `FuncExecuted` 随鼠标轨迹波动，属正常）。
* `assert` 全套 **283 passed / 0 failed**。
* 拒收点再次前移：新暴露 `138:GET_CSTRUCT_DEF ×1`（按枚举倒推确认；属"建议维持拒绝"：
  运行时取 cstruct 定义，只出现在定义期/稀有路径）、`156:SWITCH_LOOKUP ×1`、`59:OP_CALL ×4`。

**教训**

1. "peek 型" opcode 有固定写法：`TOS_SPILL()` → 读 `[rsp]` → callout → **不 `TOS_PRODUCE()`、
   不动 vstack**。现在 `SET_MODULE_VAR` 等三处同款，可以照抄。
2. **能"静默不做"的 opcode 不要硬塞 bailout 通道**：多一条失败路径就多一个漂移/回归面
   （解释器不报错的语义，JIT 也不该造出失败分支）。
3. 测试要挑**可观测且自洽**的断言：`declared_face` 的效果藏在类型推断里，
   直接硬编码 `type()` 串会被格式变更打碎；"循环内 vs 循环外同构对比"更耐改。

***

### 8.59 裸 `OP_CALL` 进 JIT（callee 是运行时值：局部闭包 / 回调表）—— 覆盖面 L8 的第二块（2026-09-14）

**背景**：§8.58 之后直方图剩最大一块 `59:OP_CALL ×4`。VM 侧（`op_call.inc`）：

* `arg_count = READ_SHORT()`（3 字节指令），`callee = vm_stack_peek_fast(&vm, 0)` —— **callee 就是栈顶**，
  栈约定 `[args...][callee]`（`call()` 读 `vm.stack[sp - arg_count - 1 + i]` 作实参）。
* 分派（`call_value`）：`OBJ_CLOSURE` → `call()`；`OBJ_FUNCTION`（裸函数）→ **先 `gc_alloc` 包成闭包**再 `call()`；
  `OBJ_NATIVE` / bound method → 其它分支；`null` → 报"函数未定义"。
* 返回（`OP_RETURN`）：`vm.sp = stack_base; push(result)` ⇒ `arg_count + 1` 个槽**塌缩成 1 个值**；
  `OP_RETURN_MULTI count(1)` 塌缩成 count 个。

**探针实测的两种形态**（`.leno` 级）：
`var f = addOne; f(i)` → `OP_GET_LOCAL callee` + `OP_CALL`；
`callbacks[j % 2](j)` → `OP_INDEX` + `OP_CALL`。file_manager 的 `_runEvts[i](ev)` 正是后者。

**实现**

1. **抽公共核心 `jit_invoke_closure(ObjFunction* fn, Value callee_val, argc, vstack_top, ret_count, who)`**
   （把 §8.57 复用过的"函数级 JIT 快路径 + VM 重入 + 多返回值回填"整段搬出来，三个调用方共用）。
   入参刻意用 **`ObjFunction*` + callee 原始 `Value`** 而不是 `ObjClosure*`：函数值可能是**裸 `ObjFunction`**
   （`OP_GET_GLOBAL_FUNC` / `OP_GET_MODULE_FUNC` 取出来的就是这种）；快路径本来只用 `function`，
   慢路径把原始值交给 `vm_call_value` 由解释器按自己的分支处理 ⇒ **JIT 不必为裸函数多分配一次闭包**。
2. `jit_callout_call_value(vstack_top, argc)`：取 `callee = vstack_top[-1]`（见下），
   接受 `OBJ_CLOSURE` / `OBJ_FUNCTION`，其余 failed 交解释器；
   **返回值个数守卫放在调用之前**（`fn->return_count != 1` 直接 bailout）。
3. scan：`opcode_size` 收录 `OP_CALL` = 3；`case OP_CALL` 按 **rc = 1** 记账（`vstack -= ac`），
   由 callout 运行时复核；内联扫描经 default 自动拒绝。
4. codegen：`rsp + 8` 得到实参块 → callout → 失败检查 → 弹 `(ac + 1)` 个槽
   （= §8.57 的 `argc - rc + 2` 在 rc=1 时的取值）→ `VALUE_TO_RAW` + `TOS_PRODUCE` → `vstack -= ac`。

**为什么敢假设 rc = 1**：调用点消费几个值由编译期静态函数类型决定，多返回值函数必须显式声明多返回类型 ⇒
绝大多数是 1。`rc != 1` 的调用点（多返回解构）由守卫挡下 → bailout → 循环被 `JIT_BAILOUT_LIMIT` 拉黑，
代价有界。**守卫必须前置**：若"先调用再检查"，bailout 后解释器重跑整轮会让 callee **执行两次**、副作用翻倍。

**⚠ 本轮踩的两个坑（都是"差一格"）**

1. **callee 读错格**：callout 的 `vstack_top` 约定是「**+8 之后的实参块**指针」
   （`vstack_top[0]` = 最后一个实参），callee 在它**之上**（JIT 栈向下增长 ⇒ 更低地址）
   ⇒ 必须读 `vstack_top[-1]`。第一版读 `[0]` ⇒ 把实参当 callee ⇒ 报"callee 不是闭包/函数"。
2. **双重 +8**：codegen 已经传了 `rsp + 8`，我在 callout 里又写了 `vstack_top + 1` ⇒ 实参指到更深的槽，
   **`addOne` 收到的是累加器而不是循环变量**（`addOne(50)` 返回 1276 = 累加器+1）。
   症状却是**后继 `OP_ADD_INT` 报 "int48 溢出/截断"**（bailout 又救了一次正确性）。

**定位手法（两次都靠这个，没碰大反汇编）**：
先看 stats 的 bailout **站点**——"非溢出类 @bc_off=X" 是我 callout 自己的检查，
"int48 溢出/截断 @bc_off=X" 则是**值不对**（报错点在下一条指令）；再用现成的
`LENO_JIT_FTRACE=1` 把快路径的**入参/出参**打出来（`arg0=0x32 → ret=0x4FC` 一眼看出传错了变量）。
最后加一条 gated `[CV-DBG]` 打印直接照出 callee/实参，确认约定，再删掉。

**验证**

* 新用例 `assert/test_jit_op_call.leno`：形状 1（局部闭包值）、形状 2（回调表交替两种函数）、
  反例（rc = 2 经闭包值调用 ⇒ 守卫 bailout、结果仍正确）。8 个 JIT 用例 JIT + `LENO_NO_JIT=1` 双模式全绿。
* `file_manager` 冒烟负载：**`59:OP_CALL` 从直方图消失**（4 个循环全进 JIT），运行时 bailout **0**，
  **`FuncCompiled 5 → 161`** —— 回调式调用进 JIT 后，被调函数（`process` / `drawText` /
  `_draw_cached_text` / `pushTexBatch` / `snapX` …）成批进入函数级 JIT，覆盖面收益远超"解锁 4 个循环"本身。
* `assert` 全套 **284 passed / 0 failed**。直方图剩 `138:GET_CSTRUCT_DEF ×1`、`156:SWITCH_LOOKUP ×1`。

**教训**

1. **"shift 只能做一次"**：`+8`/`+1` 这类"跳过 callee/接收者槽"的约定必须写在函数头（本文档与 `jit_priv.h` 都写了
   "vstack_top[0] = 最后一个实参"）；两个调用方对同一参数的理解差一格，症状会出现在**几十条指令之后**
   （下一条算术指令的溢出检查），而不是出错的指令上。
2. 重构成公共核心时，**先只做搬移、不加新逻辑，并立刻跑既有用例**（本轮 Step A 后 7/7 全绿才继续），
   否则"重构的锅"和"新逻辑的锅"会混在一起无法区分。
3. 诊断"值不对"类问题：**先读 bailout 站点**（哪条指令、哪种站点编码）→ 再开 FT trace 看入参出参 →
   最后才考虑加临时打印。三步下来不用读机器码。

***

### 8.60 修复 R4：驱逐机器码时的 use-after-free（机器码延迟释放）（2026-09-14）

**问题定性**（roadmap 的 R4，先确认再修）：

1. `jit_func_entry_claim` 是 direct-mapped，冲突时**无条件**释放占用者的机器码：
   ```c
   if (e->fn) jit_mem_free((void*)e->fn, 0);
   ```
2. `jit_mem_free` = `VirtualFree(ptr, 0, MEM_RELEASE)` / `munmap` —— **真归还 OS**，不是池内回收。
3. 触发形态：**函数级 JIT 的机器码正在执行时，callout 里又编译了别的函数**，而那个新函数
   撞到了正在执行者的槽位 ⇒ 正在执行的机器码被 unmap ⇒ 一返回就跳进未映射页。
   构造最小复现（`build/probe_r4_collide.leno`：main 热循环经**函数值**调 `callIt`，`callIt`
   体内再经函数值调 `h0..h5`），把缓存掩码缩到 4 位后 **4/4 必崩**：
   ```
   exit=-1073741819    (0xC0000005 STATUS_ACCESS_VIOLATION)
   ```

**暴露面**：只有「机器码正在执行 + 同时触发编译」这条链才有风险。§8.59 的裸 `OP_CALL`
callout 会**急切编译**被调者，正好落在这条链上 ⇒ 风险被 §8.59 放大。循环缓存的驱逐是
「窗口 + 价值启发式」（正在执行的循环 hit_count 最高、worth 最大，基本不会被选中），
安全得多，但同族风险仍在 ⇒ 一并处理。

**修法：延迟释放队列**

* `jit_code_retire(ptr, size)`：驱逐统一走它 —— `jit_func_depth == 0 && jit_loop_depth == 0`
  （**C 栈上没有任何 JIT 机器码**）时立即释放，否则挂队列。判据直接复用 §8.36/§8.37 已有的
  两个深度计数（`jit_in_frame()` 的同源状态），不新造状态。
* `jit_retire_drain()`：挂在**全部 5 个退出点**（`jit_callout.c` 的 3 个函数级 JIT 返回点 +
  `jit.c` 的 `jit_try_hot_func_call` / `jit_try_hot_loop`），`depth--` 之后调用。
* 队列满（`JIT_RETIRE_MAX 4096`）时**宁可泄漏也不 free**：每次驱逐的代码量很小且有界，
  `jit_close` 会兜底释放全部遗留；UAF 不可恢复。长跑热循环期间（`jit_loop_depth > 0`）
  队列会累积到"该轮被驱逐的函数数量"，属已知且有界的取舍。
* `jit_close` 的释放保持直接 `jit_mem_free`（此时已无机器码在执行），并顺带清空队列。

**可测性：把编译期常量做成运行时可配**
`LENO_JIT_FUNC_CACHE_SMALL=1` 把函数缓存掩码缩到 4 位（默认仍是 256 槽）⇒ 概率性崩溃变成
**确定性复现**；否则 256 槽下要 ~1/256 的碰撞概率，测试只能是碰运气。

**验证**

* 同一复现（强制碰撞）：修前 **4/4 崩**（`-1073741819`）→ 修后 **4/4 正常**，且
  `acc=10801080000` 与 `LENO_NO_JIT=1` 完全一致（`callIt(i)=6i+21`，Σ 手算核对）。
* 固化用例 `assert/test_jit_func_cache_churn.leno`（注释里写了确定性复现命令）：
  JIT / 强制碰撞 / `LENO_NO_JIT=1` 三种条件都 `exit=0`。
* 8 个 JIT 用例 JIT + `LENO_NO_JIT=1` 双模式全绿；`assert` 全套见提交说明。
* `file_manager` 交互负载：`closed cleanly`、运行时 bailout **0**、拒收直方图无新项。

**附带发现（2026-09-14 实测更正）**：崩溃会在 Windows 上留下一堆 `lenojit` 条目。实测
（`Get-Process` 与 `tasklist` 交叉验证）：

* 这些条目的**线程数全为 0、工作集 0 MB**，`HasExited=True` ⇒ **不是"在跑的进程"，也不是
  「WER 挂住」**（同时确认**没有** `WerFault`/`wermgr` 在运行）。合计约 **2 MB** 内核记账、
  0 CPU，**不影响程序运行**。
* 任务管理器默认视图**看不到**它们是正常的（它列的是"活着"的进程）；只有按进程对象列举的
  接口（`Get-Process`/`tasklist`）会显示 ⇒ 用户看不到 ≠ 计数撒谎，而是两类"进程"定义不同。
* `build\lenojit.exe` **确实会不可写**（以写方式打开失败）⇒ 后续构建报
  `ld: cannot open output file ... Permission denied`。成因**不是 WER**：进程对象未释放时映象
  section 仍钉住 exe；而进程对象靠**启动它的进程持有的句柄**维持 ⇒ **`Stop-Process`/`Kill()`
  对它们无效**（早已终止，没有可杀对象），只有持有句柄的 shell 退出才会消失。
* **实用做法**：遇到这种锁不要纠缠清理僵尸，**换个输出名构建**即可绕开
  （`gcc -o build\lenojit_xxx.exe <同 build.bat 的源列表>`）。
* **另一种更常见的瞬时锁**：构建命令紧跟在同一批地跑过一次 `lenojit` 之后（进程尚在退出）
  ⇒ `collect2.exe: error: ld returned 1 exit status`。此时 `Get-Process lenojit` 可能已经是 0、
  exe 也可写 —— **直接重试构建即可**，不必怀疑代码或环境。
* 仍然成立的经验：**看到一堆残留 `lenojit` 条目，就该怀疑之前崩过**。

**教训**

1. **释放可执行内存必须与"是否正在执行"绑定**：要么引用计数，要么像这里一样延迟到安全点。
   直接 `free` 的机器码缓存 = 定时炸弹，而且只在特定嵌套形态下才响。
2. 判据优先复用既有状态（`jit_func_depth` / `jit_loop_depth` 这套"在不在 JIT 里"的计数早就在
   §8.36/§8.37 维护着），不要为修复再造一套并行状态。
3. **可测性也是修复的一部分**：把编译期常量（缓存槽数）做成运行时可配，才能把"概率性崩溃"
   转成"确定性用例"—— 否则修完了也只能靠运气证明修好了。

***

### 8.61 `OP_SWITCH_LOOKUP` 进 JIT —— 循环拒收清零（roadmap R1）（2026-09-14）

**背景**：`file_manager` 里仅剩的"可做但难"拒收点（×2）。VM 侧（`op_switch_lookup.inc`）是
int / bigint / float / string 四路二分查找。

* 编码：`const_idx(2) case_count(2) default_off(4) [body_off(4)]…` → **变长** `9 + 4*case_count`。
* **偏移基准 = 指令起点 + 9 + 4*case_count**（即偏移表之后，见 `frame->ip` 的两段推进）。
* 栈效应：弹 switch 值、不压回 ⇒ **net -1**。
* 编译器只在「**≥4 个同类型常量 case**」时才发它（`codegen_stmt.c` 的 `gen_switch`），
  float / string 同样走这一条。

**实现（4 处代码）**

1. **先把查找抽成"语义唯一来源"**：`switch_lookup_index(Value switch_val, Value arr_val,
   int case_count)` 落在 `vm.c`（紧邻 `compare_values`），VM 的 `OP_SWITCH_LOOKUP` 与 JIT 的
   callout **调同一个函数**。声明放 `leno_vm.h`，只依赖 `Value`（数组也按 `Value` 传）⇒
   头文件零耦合，且与原实现的 `OBJ_ARRAY` 检查天然合一。
   理由同 §8.54 的 `struct_set_field`：**两边各写一套二分查找迟早分叉**，而分叉的表现是
   "某些极端值悄悄跳到错误分支"，平时测不出来。
2. `jit_scan.c`：`opcode_size()` 加变长解码；`scan_loop_body()` 解出全部 case 体 + default 体，
   **每个都登记为前向跳转目标**（vstack 用"弹出后"的深度，与 if/else 各分支同理）、
   `dead = 1`（switch 之后没有 fall-through：体一律由跳转表进入）、目标表满则拒绝整个循环；
   **内联扫描显式拒绝**（这条是变长 + 多目标，而 inline 侧只有简化版 vstack 走查，
   没有目标表/dead-code 机制 —— 与 §8.55 拒绝模块变量访问同一取舍；循环本身照常 JIT）。
3. `jit_callout.c`：`jit_callout_switch_lookup` 只是**一行转发**。它是纯计算：不分配、不报错、
   不改任何 VM/JIT 状态 ⇒ **永不置 `jit_callout_failed`** ⇒ 调用方没有 bailout 分支
   （解释器在这条指令上从不报错，JIT 也就没有"交回解释器需要还原的语义"）。
4. `ops_jump.inc`：callout 求出下标后发**线性比较链** `cmp rax,k / je case_k … / jmp default`
   （case 少时足够，不需要间接跳转表、不在代码段里放地址表）；所有目标走 `patch_add`
   （记录 vstack，收尾统一 patch）；结尾 `vstack = VSTACK_UNREACHABLE`。
   弹值用 **`TOS_CONSUME_RAX()`** 而不是 `TOS_SPILL()` —— 这条指令**消费** TOS
   （TOS 规则要求跳转目标处 `tos_live == 0`），选错宏就会多弹/少弹一格（§8.55 记过一次）。

**验证**

* 新用例 `assert/test_jit_op_switch_lookup.leno`：热循环体里的 4-case switch（含每轮一次
  default）与被调函数里的 4-case switch 各 3000 轮；JIT / `LENO_NO_JIT=1` 都 `exit=0`，
  同一份断言（`65400` / `119400`）。`LENO_JIT_DEBUG=1`：两个循环 `capable=1`、
  **`Bailouts 0`**、`inline-scan FAIL: OP_SWITCH_LOOKUP` 如期出现（守卫生效）。
* **顺带解锁函数级 JIT**：含 switch 的 `classify` 进入函数级 JIT（`FuncCompiled 1`）——
  R1 之前它整个函数被拒收。
* **VM 侧重构的等价性回归**：既有 `test_switch_optimization.leno`（int 正序/无序/负数、
  float、字符串、中文、大数、少量 case、只有 default）JIT 与 `LENO_NO_JIT=1` 都通过
  ⇒ 抽成 `switch_lookup_index` 是行为等价的重构。
* **`file_manager` 交互负载**：拒收直方图里 `156(OP_SWITCH_LOOKUP) ×2` **消失**，
  只剩 **`138(OP_GET_CSTRUCT_DEF) ×1`（刻意保留项）** ⇒ **循环级拒收清零**；
  运行时 bailout 仍 **0**、`closed cleanly`。
* 相关用例 16 个（switch 6 + JIT 10）双模式全绿；`assert` 全套见提交说明。

**教训**

1. **控制流类 opcode 的成本不在"比较"而在"目标记账"**：查找本身三行 callout 就够，
   真正要设计的是 `opcode_size` 的变长解码 + scan 的**多目标**前向记账 + patch 容量。
   评估这类工作时按"目标数"而不是"逻辑复杂度"估工。
2. **"语义唯一来源"要主动维护**：opcode 的逻辑本来就写在 `.inc` 里，JIT 想复用得整段抄。
   正确做法是**先抽成函数再让两边都调它** —— 否则将来修 VM 侧边界情况（例如 bigint 比较）
   就会漏掉 JIT，而这类分叉只在极端输入上暴露。
3. **能"永不失败"的 callout 是最好的 callout**：不分配、不报错 ⇒ 没有 bailout 分支、
   没有失败标志检查、没有"部分副作用无法回滚"的取舍（对比 §8.59 的调用 callout）。
   设计 callout 时优先问"能不能把失败面压到零"。
4. 跳转指令弹值前先想清楚是**消费**还是 **peek**：`TOS_CONSUME_RAX` 与 `TOS_SPILL`
   在不同 opcode 上语义相反，而机器码层面只差一条 pop，错了就是静默的栈深错位。

> **后续（§8.62，同日）**：上面的 callout 路径虽然语义最稳，但每轮一次真实 C 调用在热循环里
> 太贵（基准显示比等价的 `if` 链慢 3.7 倍）⇒ 同日加了 **R9 编译期分流**：case 值全 int 时改发
> 内联比较链（+ int48 守卫），switch 的 JIT 时间 30.4 → 15.7 ms；callout 路径保留给
> float/string/bigint case。语义完全一致，见 §8.62。

***

### 8.62 R9：`switch` 的 JIT 分派提速（int 内联比较链）—— 一次"基准驱动"的优化（2026-09-14）

**起因：先量，再动手。** 用 4 个等价负载（switch 分派 / if 链分派 / 纯循环 / 空程序）
在 VM 与 JIT 下各跑 3 次取最小（N = 3,000,000，扣除同模式进程启动开销）得到：

| 负载（净耗时） | VM | JIT（R9 前） | JIT（R9 后） |
|---|---|---|---|
| 纯循环 | 143.9 ms | 6.1 ms | 7.9 ms |
| + `switch`（4 case + default） | 157.2 ms | **21.6 ms** | **8.0 ms** |
| + `if/else if` 链 | 162.9 ms | 5.9 ms | 6.6 ms |

* **VM 里两者基本持平**（switch 靠二分查找略省几次比较，但循环开销占 90% 以上）；
* **JIT 里 `if` 的分派几乎免费**（`cmp`+`jcc`，且 `i%5` 的分支被预测器吃满），
  而 `switch` 每轮要走一次 **callout**（真实 C 调用 + 序言保存 RSP/RCX/R9 + 对齐 + 回来），
  约 **5.2 ns/轮 ≈ 15~20 周期**，全花在调用本身而不是查找 ⇒ switch 比 if 慢 3.7 倍。

**修法：编译期分流**（`ops_jump.inc` 的 `case OP_SWITCH_LOOKUP`）

* **A. int 快路径**：case 值**全是 int 且无重复**时，直接发内联比较链
  `[int48 守卫] + cmp rax,case_k / je case_k … / jmp default` —— 与 `if` 链同级。
* **B. callout 路径**：case 值含 float/string/bigint 或常量表解析不出时，仍走
  `jit_callout_switch_lookup`（§8.61 的语义唯一来源）。**语义完全不变**，只是给常见情形提速。

**正确性的三个要点**

1. **int48 守卫不能省，而且它就是语义关键**：非 int48 的值（裸 double / NaN-boxed 的
   null/bool/对象/bigint）一律 **bailout → 解释器重放**。因为 **bigint 值配 int case 时
   解释器是能命中的**（bigint 分支接受 int 元素），拿 bigint 的位模式去比 int 常量会算错。
   守卫用与 `EMIT_INT48_CHECK` 完全相同的指令序列（`sar 47 / inc / cmp 1 / ja`）——
   这套判据在本工程里已经用了上百处，自创检查只会引入偏差。
2. **重复 case 值必须排除**：解释器的二分查找在重复值上命中的是**中间**那个，而线性链命中的是
   **第一个** ⇒ 两者会跳到**不同的 case 体**（不同语句！）。数组是排序过的，重复必然相邻，
   编译期扫一遍即可。
3. **裸 int48 的符号扩展**：JIT 虚拟栈里 int 是**未装箱**的 int48，负数以全 1 符号扩展存储，
   所以 case 常量也要按 `(int64_t)val_as_int(v)` 比较 —— 而不是装箱后的 `val_int(v)`。

**踩坑（值得记）**：第一版我把比较常量写成了装箱形态 `val_int(v)`，于是位比较**恒不成立**、
所有 int 值都会掉到 default。**断言确实会失败**（结果从 65400 变成 297000），
但真正先暴露问题的是基准：分派耗时**没有任何变化**（30.4 ms 原样）——
"优化写了但没生效"这类问题，**只有基准看得出来**。

**验证**

* 基准（同机同法，取 3 次最小）：switch 的 JIT 时间 **30.4 → 15.7 ms**；
  分派净开销 **+15.5 ms → +0.1 ms**，与 `if` 链（+0 ms 级）同级；结果 `acc=65400000` 不变。
* 扩展用例 `assert/test_jit_op_switch_lookup.leno`：新增
  ① 负数 case（裸 int48 符号扩展）、② **字符串 case**（走 callout 路径，命中 + default）、
  ③ **int case 配非 int 值**（float / 字符串 ⇒ 守卫 bailout）。
  JIT 与 `LENO_NO_JIT=1` 都 `exit=0`，断言完全一致；`LENO_JIT_DEBUG` 下 `Bailouts: 6`
  （两个非 int 循环各 3 次 = 守卫按设计触发后交解释器）—— **这正是守卫正确性的证明**。
* `file_manager` 交互负载：**运行时 bailout 仍 0**（真实 switch 的值都是 int ⇒ 全走快路径）、
  拒收直方图不变（只剩刻意的 `138`）、`closed cleanly`。
* `assert` 全套见提交说明；编译 0 warning。

**教训**

1. **基准是"优化是否生效"的唯一证据**：断言只能证明"没算错"，证明不了"变快了"或"真的走了新路径"。
   今后这类优化一律先写等价负载、记录 before/after。
2. **复用工程里已有的判据与指令序列**（`EMIT_INT48_CHECK`、`val_is_int` 的语义）比自己拼一套更安全 ——
   自创检查的偏差只会在极端输入上暴露。
3. 编译器生成的**常量数组**要先想清楚它的边界（这里：重复值 ⇒ 二分查找与线性链命中位置不同），
   否则"等价改写"会悄悄改变语义。

***

### 8.63 Linux `munmap` 尺寸修复 + R3 诊断收口 + R2 批次 1（`OP_IS_NULL`）（2026-09-14）

**A. 修 Linux 上的可执行内存泄漏（前几轮发现、本轮补记并修复）**

* 现象：`jit_mem_free(ptr, size)` 在 POSIX 侧是 `munmap(ptr, size)`（**需要长度**），
  而所有调用点都传 `0` ⇒ `munmap(ptr,0)` 返回 `EINVAL`、**机器码永不归还**，
  每次缓存驱逐漏一块可执行内存。Windows 侧 `VirtualFree(ptr,0,MEM_RELEASE)` 忽略 size，
  所以一直没暴露（开发机是 Windows）。
* 修法：**让尺寸跟着代码指针一起存** —— `JitCacheEntry.code_size` /
  `JitFuncCacheEntry.code_size`，由 `jit_compile` / `jit_compile_function` 通过新增的
  `size_t* out_size` 出参带出来（两个编译函数都是本文件 static、各只有一个调用点），
  4 个释放点（`jit_close` ×2、`jit_func_entry_claim`、循环缓存驱逐）改用真实长度。
* Windows 行为不变（size 仍被忽略）✓；Linux 上恢复"驱逐即归还"。
* 说明：这也解释了为什么 **R4（use-after-free）只会在 Windows 上发生** ——
  Linux 侧 `munmap` 一直失败，代码根本没被 unmap，只泄漏不 UAF。

**B. R3：`opcode_size` 余项补全 + 把两类报错分开**

* 补齐约 40 个 opcode 的长度（`OP_GET_UPVALUE`/`SET_UPVALUE`、`OP_DEFINE_GLOBAL*`、
  `OP_GET_NATIVE`、`OP_TAIL_CALL`、`OP_ARRAY_GET/SET/APPEND`、`OP_DICT*`、`OP_STRING_ADD`、
  `OP_INDEX_SET`、`OP_SLICE`、`OP_THROW`、`OP_AWAIT`、`OP_INIT_LENOMODULE`、`OP_U8_TO_F64`、
  `OP_RANGE`、`OP_GET_FIELD_ADDR`、`OP_GET_CSTRUCT_DEF`、`OP_ASYNC_CALL`、`OP_DTOR_LOCAL`、
  `OP_GET_MODULE_CONST`、`OP_TYPE_CHECK`/`OP_AS_CAST`（按 kind 3 或 4 字节）、
  `OP_PUSH_TYPE_ARGS`、`OP_CFUNC_CALLBACK`、`OP_CLIB_CALL` …）。
  **每条都与 VM 实现里的 `READ_*` 次数逐条核对，并与 `debug.c` 的反汇编器交叉验证。**
  `OP_LOAD_NATIVE_MODULE` / `OP_DEFINE_MODULE_FUNC` 等也一并归类。
* **`OP_CLOSURE` 故意保持"长度未知"**：它的 upvalue 数量记在常量表的函数对象里，
  而 `opcode_size(ip)` 只有 `ip`、拿不到 chunk ⇒ 硬猜长度会让扫描**静默走错字节流**。
  这正是"宁可不编，不要猜"。
* 报错分成两类：`unknown opcode N（长度未知）` 与
  `unsupported opcode N（已收录长度、未实现）` —— 排查时一眼可分。
* **实测收益（`file_manager` 交互负载）**：拒收直方图从含糊的一行变成可直接排期的清单：

```
unsupported:138 x49  ← OP_GET_CSTRUCT_DEF（刻意保留）
unsupported:14  x2   ← OP_GET_UPVALUE（R5 闭包）
unsupported:142 x2   ← OP_CLIB_CALL（维持拒绝）
unsupported:76  x4   ← OP_STRING_ADD   ← 实测出来的 R2 下一批目标
unsupported:93  x3   ← OP_TYPE_CHECK   ← 同上
（unknown: 0 —— 本负载用到的 opcode 长度已全部登记）
```

**C. R2 批次 1：`OP_IS_NULL`（`?.` / `??` 编译出）**

* 做法照 §8.52~§8.59 的模板：`opcode_size`（已在 B 里）+ 两处 scan（净 0：pop1 push1）
  + callout `jit_callout_is_null` + `ops_misc.inc` 的 case。
* 语义：解释器注释里写明这条 opcode 存在就是为了**避开 `OP_EQ` 的 `0.0 == null` 陷阱**，
  所以判据固定是 `val_is_null`（位比较 `NULL_VAL`），**不能退化成"等于零"**。
  纯判断、不分配、不报错 ⇒ 无失败通道、无 bailout 分支。
* **选择顺序的方法论**：先用 R3 的新诊断**照出**哪些构造真的会发这条 opcode
  （探针实测 `d["k"] ?? 0` → `unsupported opcode 48`），再实现、再验证同一负载转为
  `capable=1` —— 而不是"照清单挑一个做"。顺带确认了另外两条**不需要做**：
  `-d["k"]`（字面量字典）走 `OP_NEG_INT`（已支持），`x == null` 走比较（已支持）。
* 验证：`assert/test_jit_op_is_null.leno`（3 个热循环：`??` 取左/取右、判空结果参与比较）
  JIT 与 `LENO_NO_JIT=1` 都 `exit=0`；`LENO_JIT_DEBUG` 下 `Compiled 3 / Bailouts 0`。
* `assert` 全套 **287 passed / 0 failed**（286 + 新增 1）；`file_manager` 仍
  `closed cleanly`、运行时 bailout **0**。

**教训**

1. **诊断质量直接决定后续排期质量**：这次同一负载、同一时刻，只是把长度补齐、把报错分类，
   拒收清单就从"一条含糊的 138"变成"76 ×4 / 93 ×3 / 14 ×2 / 142 ×2 / 138 ×49"——
   R2 该做哪两个 opcode 是**测出来的**，不是从清单里猜的。
2. **长度表是"静默走错字节流"级别的改动**：每条都必须与 VM 的 `READ_*` 次数、
   以及 `debug.c` 的反汇编器交叉核对；长度依赖常量表的（`OP_CLOSURE`）宁可留"未知"。
3. **同一份代码在 Windows 与 POSIX 上的"释放语义"可以完全不同**：
   `VirtualFree(MEM_RELEASE)` 忽略 size、`munmap` 必填 size —— 一个传 0 的调用点
   在两个平台上分别是"正确"和"永不生效"。跨平台代码里，**能忽略的参数迟早会被传错**。

***

### 8.64 R2 批次 2：`OP_STRING_ADD` 进 JIT（字符串插值）+ 顺带解开内联侧（2026-09-14）

**选题依据（R3 实测）**：上一轮的分类直方图里 `76:STRING_ADD ×4` 是 file_manager 里
最高频的可做缺口（`93:TYPE_CHECK ×3` 次之，下一批做）。

**关键事实：这条 opcode 由字符串插值 `$"..."` 编译出**，不是 `+`：

```1288:1303:src/codegen/codegen_expr.c
                emit_byte(gen, OP_STRING_ADD, ast->line);
```

`+`（两侧都已知是 string 时）走的是**已支持的通用 `OP_ADD`**，所以它出现在"拼显示文本"的
热循环里 —— 这也解释了它为什么在 file_manager 里高频。

**实现**

1. **语义唯一来源**：把 `op_string.inc` 的拼接逻辑整段抽成 `string_add(Value, Value)`
   （落在 `vm.c`，声明进 `leno_vm.h`）；解释器的 `OP_STRING_ADD` 与 JIT 的 callout 调同一个。
   它覆盖两条路径：两侧都是 `ObjString` → `str_concat`（正确处理内嵌 NUL）；
   否则两侧各自 `value_to_string` 再拼。
2. `jit_callout.c` / `jit_priv.h`：`jit_callout_string_add` = **一行转发**；
   纯转发 ⇒ 不报错（任何值都能转字符串）⇒ 无失败通道、无 bailout 分支。
3. `ops_callout.inc`：取值用 `TOS_CONSUME_TO(JIT_RDX)`（b）+ `TOS_CONSUME_RAX()`（a）——
   与 `OP_EQ_INT` 同款；两次 `EMIT_RAW_TO_VALUE`（只动 RAX/R8，RDX 安全）后传参；
   结尾 `EMIT_VALUE_TO_RAW` + `TOS_PRODUCE` + `vstack--`（净 **-1**）。
4. 两处 scan 都按净 -1 记账 ⇒ **内联扫描不再拒绝 76**（roadmap 里"STRING_ADD 还能恢复
   一部分内联"这一条同时兑现）。

**关于"callout 里分配对象"的 GC 契约**（本轮必须想清楚的一点）
新字符串是**分配**出来的，而这个 callout 是从 JIT 机器码里调用的。安全，理由是本工程既有
的设计：`gc_alloc` 在 JIT 执行期间**只置让出标志**、不就地回收（§8.36 的 `jit_in_frame()`
避让 + §8.37 的让出标志），所以不会在 callout 中途把 JIT 机器栈里的活值当垃圾收掉。

**踩坑（探针必须自证）**：第一版探针我写成 `"v${i}"` —— 这门语言的插值语法是 `$"...{expr}..."`，
`${i}` 被当成**普通字面量**，于是循环照常编译、结果"看着也正常"，结论会完全错
（会误判成"插值走的是已支持路径"）。是"输出原样打印了 `v${i}`"才暴露的。
⇒ 判定"某 opcode 是否真被这条源码触发"，**必须看编译侧证据**（R3 的 `unsupported opcode N`
消失、`body_size` 变化、探针输出本身），不能只看退出码/断言。

**验证**

* 探针：两个插值热循环 `capable=1`（实现前会被 `unsupported opcode 76` 拒收），
  结果 `last=v2999` / `last2=x2999` 与 `LENO_NO_JIT=1` 完全一致。
* 新用例 `assert/test_jit_op_string_add.leno`：三条路径（int 插值 ⇒ 转换路径、
  string+int 混合 ⇒ 快路径+转换、反复拼接 200 轮核对长度），JIT 与 `LENO_NO_JIT=1` 都 `exit=0`。
* `file_manager` 交互负载：**`unsupported:76` 归零**、内联侧 76 也消失；
  拒收点前移到 `93:TYPE_CHECK ×16`（`138 ×104 / 14 ×9 / 142 ×2` 为保留项与 R5）；
  运行时 bailout **0**、`closed cleanly`。
* `assert` 全套见提交说明；编译 0 warning。

**教训**

1. **opcode 的名字会骗人**：`OP_STRING_ADD` 不是"字符串 `+` 的通用路径"，
   而是**插值专用**。选题/写用例前先看编译器在哪发它（`codegen_expr.c` 一行 grep），
   比从名字猜快得多。
2. **探针要先自证**：错误语法会被静默当成另一种合法语义（这里是字面量字符串），
   于是"跑通了"反而掩盖了"根本没测到目标"。

***

### 8.65 R2 批次 3：`OP_TYPE_CHECK` 进 JIT（`is` / `switch case is`）+ 修掉一个长度表 bug（2026-09-14）

**选题依据（R3 实测）**：`93:TYPE_CHECK` 是当前最高频的可做缺口（**×16**），来源是
`is` 与 `switch ... case is` —— 后者正是 file_manager 事件分发的形态。

**实现**

1. **语义唯一来源**：把 `op_type_check.inc` 里那坨约 170 行的 TypeKind 判定整段抽成
   `type_check_value(value, expected_type, elem_type, name_val)`（落在 `vm.c`，
   声明进 `leno_vm.h`）。与原实现只有两点不同：操作数不再用 `READ_BYTE/READ_SHORT`
   就地消费，而是由调用方传入 —— 因为**名字常量要查 `chunk->constants`，而 JIT 在编译期
   就能查好**（与 §8.61 的 switch case 数组、§8.64 的名字常量同一做法）。
2. `op_type_check.inc` 变薄：读 `type_kind` → struct/face/enum 读 `name_const(2)`、
   其余读 `elem_type(1)` → 调判定 → 压 `val_bool`。
3. `jit_callout.c` / `jit_priv.h`：`jit_callout_type_check` 一行转发；纯判定、不分配、
   不报错 ⇒ 无失败通道、无 bailout。
4. `ops_callout.inc`：编译期解出 `type_kind`/`elem_type`/`name_const` → callout →
   `EMIT_RAW01_TO_BOOLVAL()`（与解释器一致：压的是 `val_bool`，不是 int48 的 0/1）。
5. 两处 scan 按净 0 记账（pop1 push1）。

**本轮的额外发现（两条，都比实现本身更值钱）**

* **`TYPE_ENUM` 也是 4 字节指令** —— `op_type_check.inc` 里 `case TYPE_ENUM` 与
  `case TYPE_STRUCT/FACE` 同形，都 `READ_SHORT()` 读名字常量。而我上一轮写 `opcode_size`
  时是**照抄 `debug.c` 的反汇编器**，那里同样只写了 FACE/STRUCT ⇒ **把参考实现的 bug
  一起抄了进来**（长度少 1 字节 ⇒ 含 enum 类型检查的字节流会走错位）。
  本轮对 VM 的 `READ_*` 逐条核对时发现，**两处都修了**（`jit_scan.c` + `debug.c`）。
* 直方图**新暴露** `134:OP_GET_METHOD ×4`：§8.57 只做了「`GET_METHOD` + `OP_CALL` 窥孔」，
  **不带调用的裸方法取值**（`obj.m` 作为值传递）还没支持 —— 这是被 `93` 遮住的下一层缺口。

**验证**

* 探针（三种形态：face 变量的 `is`、`switch case is`、dict 无类型值的 `is int`）：
  实现前三条循环都被 `unsupported opcode 93` 拒收；实现后全部 `capable=1`，
  结果 `h1=h2=h3=200` 与 `LENO_NO_JIT=1` 完全一致。
* 新用例 `assert/test_jit_op_type_check.leno`：5 组（struct/face 名字判定**含反例**、
  `switch case is`、dict 基本类型判定**含反例**、enum 判定），JIT 与 `LENO_NO_JIT=1`
  都 `exit=0`；`LENO_JIT_DEBUG` 下 `Compiled 4 / Bailouts 0`。
* `file_manager` 交互负载：**`unsupported:93` 归零**（原 ×16），
  **`FuncCompiled 123 → 289`**（`case is` 分发的主力函数如 `process`/`_relayout`
  首次进入函数级 JIT），运行时 bailout **0**、`closed cleanly`。
* `assert` 全套见提交说明；编译 0 warning。

**教训**

1. **"照抄参考实现"会把参考实现的 bug 一起抄过来**：长度表这类"静默走错字节流"的改动，
   必须回到 VM 的 `READ_*` 逐条核对 —— 反汇编器是**方便**的对照物，不是**权威**。
2. **一个 opcode 的收益要看三条**（§8.54 的老教训再次成立）：这里 `TYPE_CHECK` 不只是
   解锁循环，更让 `case is` 事件分发的整批函数进了**函数级 JIT**，`FuncCompiled` 几乎翻倍
   —— 评估"补一条 opcode 值不值"时，循环 / 内联 / 函数级 JIT 三条都要算。
3. 诊断（R3）是**递归见效**的：分类直方图先把 `76`/`93` 照出来，补完之后又照出了 `134`
   —— 补齐一层就露出下一层，这比一次猜一个 opcode 高效得多。

***

### 8.66 R2 批次 4：`OP_GET_METHOD` 独立取值形态 + 一次**测量方法学纠错**（2026-09-14）

**做了什么**

* **规则唯一来源**：把 `op_struct.inc` 里 `OP_GET_METHOD`（struct 分支）的方法表查找规则抽成
  `struct_method_lookup(def, name, &closure, &func)`（`vm.c`，声明进 `leno_vm.h`）——
  按名字线性比对、**跳过 ctor/dtor**。只抽**规则**：inline cache 更新、GC 安全的 push/pop、
  报错文本都留在调用方（JIT 侧没有 IC，报错一律走 bailout 交解释器）。
* `jit_callout_get_method`：只做**成功路径**（struct 预创建闭包 / 新建 closure /
  原生方法 → bound method；File/Socket 同理），其余 failed → bailout。
* `ops_callout.inc`：`case OP_GET_METHOD` 顶部加**独立形态**分支，判定条件与
  `jit_scan.c` 的同名 case **完全一致**（`ip + 6 <= end && ip[3] == OP_CALL` 才是可合并的
  调用形态）—— 两边条件必须一字不差，否则 size 记账错位。
* `jit_scan.c`：原来对独立形态是**显式拒绝**，现在按净 0 记账（pop 1 obj push 1 方法值）。

**测量方法学纠错（本轮最有价值的收获）**

排查中我按老习惯统计"拒收直方图"，得到 `unsupported:134 ×36`，一度以为"裸 GET_METHOD ×4
是最高频缺口"。实际拆开才发现：

* 我的 grep 模式 `scan FAIL: ... opcode (\d+)` **同时命中了 `inline-scan FAIL: ...`**
  （子串重叠！）⇒ **内联侧**的拒收被当成**循环侧**的拒收，两个完全不同的结论被混在一起。
* 用确切消息 `[JIT-DEBUG] scan FAIL` 拆开后，真实分布是：

```
循环级（决定"循环能否编译"）：138 ×2（刻意保留） / 14 ×2（R5 闭包） / 142 ×2（维持拒绝）
内联级（只影响"能否内联"）：134 ×36（本轮的 GET_METHOD）+ 138 ×116 + 模块变量访问 ×166
```

⇒ **循环级拒收此刻只剩刻意保留项**；而我本轮补的 `GET_METHOD` 在**内联侧**才是真正的大头
（内联扫描压根没有这条 opcode 的 case）。教训：**统计必须按扫描器分开**，
"循环扫描"与"内联扫描"是两套完全不同的判据与收益（前者决定正确性/覆盖面，后者只是性能）。

**一个必须说清楚的限制**：本轮的独立形态**当前无源码入口** ——
grep 编译器可见，`OP_GET_METHOD` 的所有发射点后面都紧跟调用（`emit_call` / `OP_ASYNC_CALL`），
**没有**"取了方法值就不调用"的写法；那些后面跟 `OP_ASYNC_CALL` 的（`obj?.m()`、async 方法）
属于**维持拒绝**，所以扫描到它们仍会在 `OP_ASYNC_CALL` 处拒收 ⇒ 新分支目前不可达。

* 那么为什么不回退？因为**一致性是强制的**：扫描已经按净 0 接受了独立形态，
  codegen 就**必须**能处理它（否则会按"配对形态"去读 `ip[4]` 当 argc ⇒ 记账错位）。
  两条路径要么都支持、要么都拒绝，不能只改一边。
* 本轮真正在跑的收益是**抽出 `struct_method_lookup`**（配对形态用例 `test_jit_op_get_method`
  在跑，规则被两边共用）；独立形态本身是 `ASYNC_CALL` / 安全访问（`obj?.m()`）将来的**前置**。

**验证**

* 配对形态回归：`assert/test_jit_op_get_method.leno` JIT 与 `LENO_NO_JIT=1` 都 `exit=0`
  （确认新分支没吞掉原有配对路径 —— 这是本轮最关键的回归）。
* `file_manager` 交互负载：循环级拒收只剩 `138 ×2 / 14 ×2 / 142 ×2`（全部为刻意保留项
  或 R5 前置），**运行时 bailout 0**、`closed cleanly`；内联侧分布见上表。
* `assert` 全套见提交说明；编译 0 warning。

**教训**

1. **grep 子串会骗人**：`inline-scan FAIL: unsupported opcode` 里含有 `scan FAIL:` ——
   统计拒收原因必须用**行首锚定**或分别按两种扫描器过滤，否则"内联"的问题会被当成
   "循环"的问题，据此排出的优先级全错（本轮差点如此）。
2. **扫描与 codegen 必须同进同退**：opcode 的"接受/拒绝"是**成对**的契约。
   只放开扫描会造成记账错位（静默算错），只放开 codegen 则是死代码。
3. **"补一条 opcode"之前先确认它有没有源码入口**：这一条的名字叫"取值形态"，
   但编译器根本不发不带调用的裸形态 —— 花在建言前的这一次 grep 比事后的返工便宜得多
   （§8.64 的教训在这里第二次成立）。

***

### 8.67 修复 R12：`OP_GET_PROPERTY` 独立访问漏弹 receiver 槽（每轮泄漏 8 字节 ⇒ RSP 漂移 ⇒ 长跑堆损坏）（2026-09-14）

**症状**：循环里做 `var q = box["p"]`（`box` 是字典），30 万轮后 `exit=-1073741571`
（`0xC0000374` STATUS_HEAP_CORRUPTION）。阈值实测：**200000 轮正常 / 262144 轮崩**。

**第 0 步曾走错**：最初把它归类为"循环内 dict 取值"（怀疑 `OP_INDEX` 的 dict callout /
`vm.stack` 越界），并据此写了一版诊断。**字节码 dump 一句话纠正了它**（`--debug-out`）：

```
崩溃形态（box["p"]）： OP_GET_LOCAL 0 → OP_GET_PROPERTY 11 (p) → OP_SET_LOCAL_POP 4
正常形态（box[k]  ）： OP_GET_LOCAL 0 → OP_GET_LOCAL 1 → OP_INDEX → OP_SET_LOCAL_POP 5
```

⇒ **`obj["常量"]` 与 `obj[变量]` 是两个不同 opcode**。这解释了全部"奇怪"的判别结果：
换任何字面量都崩（都走 `OP_GET_PROPERTY`）、键放局部变量就不崩（走 `OP_INDEX`）、
int 键不崩（`OP_INDEX`）。而 `jit_callout_index` 的 dict 分支**一次都没被调用**
（诊断打印 0 行）—— 这就是"**该被调用的 callout 没被调用**"给出的第一个硬信号。

**测量：这种 bug 别读代码，先量漂移**。在 callout 里打印一个**局部变量的地址**即可 ——
它随 JIT 调用点的 RSP 一起移动：

```
[PROP] n=100000  vm_sp=2 (delta=0)  rsp_delta=-800000     ← 每轮 -8 字节
[PROP] n=200000  vm_sp=2 (delta=0)  rsp_delta=-1600000
```

`vm_sp` 不动 ⇒ 不是 VM 值栈；**机器栈每执行一次少 8 字节**。8 × 262144 ≈ **2.1MB**
⇒ 与崩溃阈值精确吻合 ⇒ 根因锁定为"某处漏弹一个槽"。

**根因**（`ops_callout.inc` 的 `case OP_GET_PROPERTY`）：

```c
/* Merged call: pop receiver + args, push result → net -arg_count.
 * Standalone: receiver replaced by value → no pop.        ← 错在这里 */
int pop_bytes = merged ? (merged_argc + 1) * 8 : 0;
```

"receiver 被结果取代"只在**结果写回那个槽**时成立；这里结果进的是 **TOS 缓存（RAX）**，
内存里的 receiver 槽必须显式弹掉，否则不变量"内存栈 = vstack − tos_live"被破坏 ——
每次执行永久多留一个槽。合并调用分支按"弹掉本指令消费掉的操作数"（`(argc+1)*8`）**是对的**，
错的只有独立访问这条（照同一判据补 `8`）。

**修法**：`int pop_bytes = merged ? (merged_argc + 1) * 8 : 8;`

**验证**

* 漂移复测：`n=100000/200000/300000` 的 `rsp_delta` **恒为 0**（修前 −8/次）。
* 原最小复现（100 万轮）`exit=0`；本轮全部崩溃探针（262144 轮、换字面量、`probe_crash_a`）通过。
* **一个修复解释了两个症状**：A 形态（`s + box["p"].a`）修前每次 3 次 `int48 溢出/截断` bailout、
  比解释器还慢（156ms）—— RSP 错位使 JIT 读到错位的 vstack 槽（拿到 struct 对象而不是字段值）
  ⇒ 修后 **bailout 归零、28.1ms**。同一循环 `probe_crash_b` 从 182ms 降到 19.6~32.5ms。
* 新用例 `assert/test_jit_op_get_property.leno`：40 万轮（**必须大于 262144 的崩溃阈值**，
  否则回归用例形同虚设）+ 一条 `OP_INDEX` 对照路径；JIT / `LENO_NO_JIT=1` 都通过。
* `file_manager`：`closed cleanly`、运行时 bailout **0**、`FuncCompiled 280`。
* `assert` 全套 **290 passed / 0 failed**；编译 0 warning。

**已排除"本轮引入"**：`git worktree` 在 `4cf9e9ed`（R9 之前）重建二进制，同一探针同样崩，
而这段 codegen 更早就存在 ⇒ **长期潜伏的老 bug**。此前"崩溃后留下一堆僵尸进程条目"
（§8.60 记过一次）很可能正由它造成 —— 只是当时没找到触发形态。

**教训**

1. **"该被调用的 callout 一次没被调用"是最强信号**：它一秒否掉整套假设，比读十遍代码有用。
   诊断要打印**计数**，而不是只断言"应该是它"。
2. **换一个写法就不崩 ⇒ 先 dump 字节码**：`a[b]` 会按操作数形态（常量/变量）分派到**不同 opcode**。
   凭语义直觉猜 opcode 是这次走弯路的唯一原因。
3. **"长跑才崩/内存损坏"先量漂移再读代码**：在 callout 里打印局部量地址就能直接读出
   "每轮少 8 字节"，几分钟定位；而这类 bug 的代码读起来完全"正常"（注释还写着自认为对的理由）。
4. **TOS 缓存与内存栈必须一起记账**："结果放到 RAX"不等于"内存栈少一项"。
   凡"结果取代栈顶"的形态都要显式弹掉被消费的槽（可对照同文件其它 `pop_bytes` 的写法）。

***

### 8.68 R7②：内联侧补 `OP_GET_METHOD` —— 含动态派发调用的函数终于能内联（2026-09-15）

**做了什么**

`jit_scan.c` 的 `scan_callee_for_inline()` 新增 `case OP_GET_METHOD`，记账与
`scan_loop_body` 的同名 case **一字不差**：

* 配对形态（`ip + 6 <= end && ip[3] == OP_CALL`）：`size = 6` 连带消费 `OP_CALL`，
  `vstack -= (argc + 1 - rc)`；`rc` 用 `jit_resolve_method_ret_count(cc, name_idx)` 推断
  —— 注意 `cc` 是**被调函数自己的 chunk**（名字常量索引属于它）；
  **推不出来就 `return 0`（拒绝内联）**，绝不用「按 1 个返回值」蒙过去。
* 独立取值形态（只取方法值不调用）：net 0，codegen 走 callout（§8.66）。

判定条件必须与 codegen（`ops_callout.inc` 的 `case OP_GET_METHOD`）一致，且 `end` 与
codegen 的内联体相同（都是 `callee_chunk->code + callee_body_size`），否则 size 记账错位。

**为什么此前一直没被发现**

* 缺口只在**内联侧**：循环级从 L4（§8.57）起就支持了 `GET_METHOD + CALL` 窥孔；
* 复现它必须**同时绕开两件事**，否则探针看起来"一切正常"（我第一版探针就踩了这两脚）：

| 坑 | 现象 | 绕过办法 |
| --- | --- | --- |
| **编译器层内联器**（`codegen_inline.c`，`MAX_INLINE_STMTS = 8`、体内含 `for` 即拒绝） | 被调函数在字节码里**根本不存在**，JIT 级内联问题无从暴露 | 让被内联方**体内带 `for`** |
| **调用点形态**：`scan_loop_body` 只对 `OP_CALL_GLOBAL_FUNC_TYPED` 与 `OP_INVOKE_METHOD_TYPED` 尝试内联 | 非 TYPED 的 `OP_CALL_GLOBAL_FUNC` **压根不尝试内联**（它可能需要运行时类型提升，只走 callout）⇒ 也被"看不见" | 用 **struct 方法**（`INVOKE_METHOD_TYPED`）当被内联方 |

⇒ 探针 `jit_probes/probe_inline_method_call.leno`（struct 方法 + 体内带 for + face 动态派发）
才复现出 `inline-scan FAIL: unsupported opcode 134 at off 36`。

**收益**（同二进制 A/B，`LENO_JIT_NOINLINE=1` 关掉 JIT 级内联 ≈ 修复前该调用点的形态）：

| 场景（2,000,000 轮，内层 4 次 face 调用） | 内联开 | 内联关 | 加速 |
| --- | --- | --- | --- |
| `Holder.compute` + `Circle.area` | **172 ms** | 390 ms | **2.27x** |
| 同上换 `Square` 实现 | **156 ms** | 375 ms | **2.40x** |

**验证**

* 探针：`LENO_JIT_DEBUG=1` 修复前 `inline-scan FAIL: unsupported opcode 134 at off 36` + `inline=0`；
  修复后 `inline(method): 'compute' bc_off=12 arg_count=3 callee_lc=9 base=3 mv=3` + `inline=1`。
* 值：JIT 与 `LENO_NO_JIT=1` **逐位一致**（`circle= 201061760.00458556`、`square= 144000000.0`、
  `pair= 16000000.0`）；`Bailouts: 0`。
* 新用例 `assert/test_jit_inline_method_dispatch.leno`：1 参 / 2 参动态派发（不同 argc ⇒
  不同记账值）、**同一函数体内两处派发**（`size = 6` 连读两次）、同一 face 两个实现
  （动态分发跟着运行时类型走）、外加 `INVOKE_METHOD_TYPED` 对照路径；JIT / `LENO_NO_JIT=1` 双模式通过。
* `assert` 全套 **297 passed / 0 failed**；编译 0 warning。

**教训 / 边界**

1. **内联侧的缺口不表现为"慢"，而是"没内联"** ⇒ 判据只能是 `LENO_JIT_DEBUG` 的 `inline...` 行
   （脚本里读不到 JIT 统计），所以这类改动的用例只能是**记账守卫**（数值精确），
   "是否真的内联"必须靠探针 —— 用例文件头要把这一点写清楚，别让后人误以为它能测出"没内联"。
2. **写探针前先 dump 字节码确认 opcode**：`a.pair(b)`（struct 方法内裸名调兄弟方法）走
   `INVOKE_METHOD_TYPED`（本来就能内联），`measure(s, 2.0)`（非 TYPED 全局调用）**从不尝试内联**
   —— 两者都"看不到缺口"，与 §8.67 教训 2 同源：**形态不对，探针白写**。
3. `rc` 推不出来时必须拒绝内联。face 语法**不允许声明多返回值方法**（`face` 内只允许方法签名，
   `[int,int]` 会被 parser 拒），所以 `GET_METHOD` 的 `rc > 1` 目前**无源码入口**；
   本 case 的 `rc > 1` 分支属于"为一致性而实现"（扫描与 codegen 必须同进同退，见 §8.66 教训 2）。

***

### 8.69 R7①：内联侧放行**同模块**的模块变量访问（含跨模块安全边界）（2026-09-15）

**做了什么**

`scan_callee_for_inline()` 增加「模块归属守卫」：

* 新增参数 `ObjModule* callee_module`（两个调用点分别传 `mf->module` / `func2->module`）；
* 函数内与 `jit_scan_get_module()` 比较 —— 后者是**调用方**函数的 module
  （`jit_compile` / `jit_compile_function` 在 scan 前设置，见 §8.55/§8.56 的教训）；
* **同模块且都非 NULL** → 放行，记账与 `scan_loop_body` 的同名 case **完全一致**：
  * `GET_MODULE_FUNC + OP_CALL` 配对形态 → `size = 6`、`vstack -= (argc - rc)`，
    `rc` 由 `jit_resolve_module_func` 解析（失败即拒绝内联）；
  * 单独取函数值 / 读模块变量 → `vstack++`；
  * 写模块变量 → **peek** TOS，net 0；
* 其余（含任一为 NULL）→ 照旧拒绝内联，日志注明「callee 与 caller 不同模块」。

**为什么"同模块"是正确的判据**

内联后 codegen 用的是**编译期嵌入**的 module（`jit_priv.h`：函数级 JIT 的快路径不压帧，
编译期就得知道是哪个模块）。同模块时该指针与「被调函数运行时的 `frame->module`」等价，
模块变量的 index 也是同一个 `globals` 数组下标 ⇒ 语义不变；跨模块时它指向**调用方**的模块
⇒ 用错下标、**静默读错变量**，所以必须拒绝。

**收益**（同二进制 A/B，`LENO_JIT_NOINLINE=1`；`n = 300,000`，每次调用内层 4 轮，
被内联体内每轮 2 次模块变量访问）：

| 场景 | 内联开 | 内联关 | 说明 |
| --- | --- | --- | --- |
| **同模块** `runSame` → `Acc.bump` | **15~16 ms** | 31~47 ms | ≈2~3x；本机负载波动大，只做同轮对照 |
| **跨模块**（主文件 → helper 的 `Acc.bump`） | 47 ms | 47 ms | **设计上不内联**，两侧相同正是预期 |

**验证**

* 探针 `jit_probes/probe_inline_module_var.leno` + 辅助模块 `jit_probes/inline_modvar_helper.leno`：
  `LENO_JIT_DEBUG=1` 下同模块出现 `inline(method): 'bump' bc_off=9 arg_count=2 ...` + `inline=1`；
  跨模块出现 `inline-scan FAIL: 模块变量访问 op=88 at off 27（callee 与 caller 不同模块）` + `inline=0`。
  取值在 JIT / `LENO_NO_JIT=1` / `LENO_JIT_NOINLINE=1` 三种模式下**完全一致**
  （`same= 720000600000 sum= 1200000 seen= 300000`、`cross= 2160000600000`）；`Bailouts: 0`。
* 新用例 `assert/test_jit_inline_module_var.leno` + 辅助模块 `assert/jit_inline_modvar_mod.leno`：
  同模块精确断言（`total = 8n(n-1) + 10n`、`_seen = n`）+ 跨模块边界断言
  （期望值由**实测模块状态**推出，不硬编码）；双模式通过。
* `assert` 全套 **298 passed / 0 failed**；编译 0 warning。

**教训 / 边界**

1. **只有模块文件里的顶层 `var` 才是 `OP_GET_MODULE_VAR`**（入口脚本的顶层 var 走
   `OP_GET_GLOBAL`）⇒ 测"同模块"的用例必须**整个放在被 import 的模块里**，
   "入口脚本 + 本地 struct"那套写法根本走不到这条路径（这是我第一版探针第二次"看不到缺口"）。
2. **放行条件必须写成"同模块"，不是"能解析出模块"**：跨模块不内联是**正确性边界**而不是性能取舍
   ⇒ 用例里必须有跨模块对照，并断言"两侧耗时相同/不内联"才是预期，
   否则后人很容易把它当成"顺手也放开"的优化。
3. 守卫失败必须继续 `return 0`（拒绝内联），**绝不能**退化成"按调用方模块读" —— 那就是静默算错。
   这与 §8.68 的"rc 推不出来就拒绝"是同一条原则：**宁可不编，不要猜**。

### 8.70 P5 尝试：`dict[key]=v` 的内联快路径 —— **负结果** + 两条副产品（2026-09-15）

**目标**：按 §11 P5 的指引给字典写加「内联快路径 + callout 兜底」（目标是最高频的 `d["k"]=v`）。

**动手前的核实（硬事实，值得记住）**

1. `gc_write_barrier` **不能省**：GC 是分代的（young/old + remembered set），老年代 holder 写入
   年轻代引用必须入集。但它的常路径只有两个早退分支（值非对象 / holder 非老年代），
   完全可以在机器码里判定，只有**真要入集**时才回退 callout。
2. `key_to_array_index()` **只认 int 键**（源码注释："字符串键始终走哈希表"）
   ⇒ 字符串键不存在"数组部分赢"的分支。
3. 字符串键的哈希就是 `str->hash`，而 `dict_find_slot` 的探测起点 = `hash & (capacity-1)`，
   且 `capacity` 恒为 2 的幂（`dict_new` 从 8 起翻倍）⇒ **槽位可以在机器码里直接算**。
4. `dict_set` 的「更新现有键」分支只有两句：`entries[index].value = value; gc_write_barrier(...)`。

⇒ 于是实现了：dict 类型 + 字符串键 + capacity + entries + 屏障 + **槽内 key 与本次键是同一个 Value**
⇒ 直接写值；8 个失败点全部回退原 callout（编译 0 warning，值也全对）。

**结果：快路径一次都没被执行**（哨兵实验：在写值前插一条 `add rax, 1`，取值不变 ⇒ 从未走到）。

**根因：字典的键相等是「内容相等」，不是「指针相等」。** 同一段字面量在常量表里可以是
**两个不同的 `ObjString` 对象**（探针 dump：插入用 `OP_CONST 0 (key)`、循环里用
`OP_CONST 4 (key)`）。解释器用 `value_shallow_equal`（字符串比内容）判定相等 ⇒ 是同一个键；
而机器码里的"同一个对象"判据永远不成立 ⇒ 每次都回退。
要让快路径生效就必须在机器码里做字符串**内容**比较（长度 + memcmp，或再调一次 `str_equal`
callout）—— 那已经把省下的 callout 开销吃掉，**与 §11 P5 早就写下的判断一致**：
"字典写要做哈希、探测、tombstone、扩容乃至「数组部分/哈希部分」双结构，无法像定宽访存那样内联"。

⇒ **整处改动已回退**（不留未验证的内存写入代码）。本节的探针保留，作为基线与回归锚点。

**副产品 1（重要）：`d["k"]=v` 与 `d.set("k",v)` 是两条不同的 opcode**

| 写法 | opcode | 备注 |
| --- | --- | --- |
| `d["k"] = v`（赋值） | **`OP_INDEX_SET_NOPUSH`** | 与 `arr[i]=v` 同一个 opcode，字典走它的 callout 分支 |
| `d.set("k", v)`（方法形式） | `OP_DICT_SET` | `examples/性能测试/全部测试.leno` 的「dict[key]=value」基准用的就是它 |

⇒ 真实代码里更常见的是 `d[k]=v`，但**做字典相关优化前必须先 dump 字节码确认形态**
（§8.68 教训 2 在这里第三次成立 —— 我第一次把快路径加在 `OP_DICT_SET` 上，A/B 完全没变化，
因为探针里的 `d["key"]=n` 走的是 `OP_INDEX_SET_NOPUSH`）。

**副产品 2（未解释，留给后续）：JIT 下「每轮新键」的字典写比解释器慢 5~10 倍**

`jit_probes/probe_dict_set.leno` 的 `new` 组（`d["k"+_str(i)] = i`）：

| 轮数 | JIT | `LENO_NO_JIT=1` | 说明 |
| --- | --- | --- | --- |
| 2,000,000 | **12484 ms** | **2359 ms** | 两个循环都编译成功、**`Bailouts` 为 0**（不是 bailout 风暴） |

同一探针的 `hit` 组（命中已有条目）JIT 266ms vs 解释器 407ms —— JIT 在命中路径上是正常受益的。

**已做的 4 个定因实验（2026-09-15，20 万轮同机同轮）**

| # | 实验 | JIT | `LENO_NO_JIT=1` | 结论 |
| --- | --- | --- | --- | --- |
| 1 | 看 `Yields` 计数 | `Yields: 4` | — | **不是**回边让出风暴（每轮让出会到百万级）；`EXEC returned 4` 只出现 4 次 |
| 2 | `str` 组：只做 `"k"+_str(i)` 分配+拼接，不碰字典 | 171 ms | 157 ms | **分配/拼接的 callout 没问题**（两边基本相等） |
| 3 | `new` 组：同样的分配+拼接**再写进字典（每轮新键）** | **671 ms** | 156 ms | 差异**全在"写进字典"这一步**（+515ms / 20 万轮 ≈ 2.5µs/轮） |
| 4 | `hit` 组：同样的字典写但**键已存在**（哈希+探测+写值，无分配） | **16 ms** | 47 ms | 哈希查找+写值本身在 JIT 下**很快**（8ns/轮）⇒ 慢的是**新键专属**的那串（`dict_add_to_order` / `dict_resize` / 写屏障） |

⇒ **根因已查明并修复：见 §8.71**（`remembered set` 入集去重是 O(|set|) 线性扫描，
而 JIT 的让出回收把整批年轻对象晋升并把集合撑到几万条 ⇒ 每次插入几微秒）。
修复后 20 万轮 **2015ms → 266ms**。

复现本轮实验：

```
build\leno.exe jit_probes\probe_dict_set.leno 2000000      # JIT：看 str / new / ikey 三行
set LENO_NO_JIT=1 && build\leno.exe jit_probes\probe_dict_set.leno 2000000
```

**回退后的基线（i5-14400F，20,000,000 轮）**

| 形态 | JIT | 解释器 |
| --- | --- | --- |
| `d["key"] = n`（命中已有条目） | 266 ms | 407 ms |
| `d["obj"] = "value"`（对象值） | 234 ms | 375 ms |
| 每轮新键（2,000,000 轮） | **12484 ms** | 2359 ms |

**教训**

1. **先 dump 字节码再动手**（第三次）：`d[k]=v` 与 `d.set(k,v)` 是不同 opcode，
   优化加错地方的表现是"A/B 毫无变化"，而不是报错 —— 这种"静默无效"最费时间。
2. **内联字典写的前提是能在机器码里复刻键相等**：我们的键相等是内容相等（字符串），
   `str->hash` + 槽位可以算，但"命中"的最后一步必须比内容 ⇒ 这一步吃掉全部收益。
   想做就该换思路（例如 dict 自带 **版本号/位移** 让 JIT 缓存"上次解析出的槽位"，
   缓存失效即回退），而不是在 codegen 里复刻哈希表。
3. **A/B 必须用足够长的循环**：`times.ms()` 刻度约 16ms，200 万轮的差异先天被量化噪声吞掉；
   本次真正的判据是"同二进制 + 哨兵"，比任何耗时对比都直接。

***

### 8.71 修复：remembered set 入集去重 O(n) → O(1)（"JIT 反而慢 10 倍"的根因）（2026-09-15）

**症状**（§8.70 副产品 2）：`d["k"+_str(i)] = i`（每轮新键）JIT **2015ms** / 20 万轮，
解释器只要 **203ms**；且 JIT 时间**超线性**（10 万轮 188ms → 20 万轮 2015ms）。

**定因链**（每一步都是实测，可复现）

| # | 实验 | 结果 | 排除/指向 |
| --- | --- | --- | --- |
| 1 | `Yields` 计数 | 4 | 不是回边让出风暴 |
| 2 | `str` 组：只分配+拼接不写字典 | JIT 171ms ≈ 解释器 157ms | 分配/拼接没问题 |
| 3 | `hit` 组：同样的写但键已存在 | JIT 16ms vs 解释器 47ms | 哈希查找+写值本身很快 |
| 4 | `_gc(false)` | JIT **2015ms → 188ms** | 问题在「JIT 与 GC 的交互」上（**解释器同样有 GC 却不慢** ⇒ 不是"GC 慢"） |
| 5 | `LENO_GC_TRACE=1` | JIT 3 次 minor GC，每次 `promoted≈51000` / `rem≈51000`；**解释器 0 次 GC** | 见下 |

**机制（三段链条）**：JIT 循环没有安全点，只能靠回边让出 ⇒ 一次回收把**当时全部**年轻对象
（含所有键字符串）**整批晋升**，而 `remembered_set_add_promoted` 把**每个**晋升对象
无去重地塞进 remembered set（≈5 万条）⇒ 此后每次 `dict[key]=v` 都是"老年代 dict ← 年轻 key"
⇒ `gc_write_barrier_obj` ⇒ `gc_remembered_set_add(dict)`，而它的去重原本是
**对整个集合的线性扫描**（O(|set|)）⇒ 每次插入几微秒 × 剩余十万次 ≈ 1.8 秒，与实测吻合。
解释器侧**一次回收都没发生**（阈值 10.7MB 没跨过，回收点只在 `OP_RETURN`）⇒ 集合为空
⇒ 屏障在第一个早退分支就返回 —— **这就是"VM 也有 GC 却快"的原因**。

**修法**：用 `Object.flags` 的空闲位 `OBJ_FLAG_IN_REMSET (0x40)` 记录"已在集合中"，
入集/移除都变 O(1)（`gc_remembered_set_add` / `remembered_set_add_promoted` / `remembered_set_remove`）。

> ⚠️ **唯一的正确性风险点**：**剪枝时必须同步清位** —— `scan_remembered_set` 会剪掉
> "不再持有年轻引用的条目"和"FULL 模式下不可达的条目"；若只从数组里删掉而不清位，
> 这些对象之后再被写屏障写入时会被 O(1) 去重挡住 ⇒ **漏入集** ⇒ 年轻代子引用被误回收。
> 两条剪枝路径都已清位（无效指针那条不清：指针可能已失效，靠 `gc_alloc` 的
> `obj->flags = pooled_flags`（赋值而非或）在新对象上兜底清零）。

**验证**

* `probe_dict_set.leno`：`new` 组 20 万轮 **2015ms → 266ms**（解释器 203ms 同级），取值 `ok=1` 不变。
* **写屏障金丝雀**（确定性钩子 `LENO_GC_YOUNG_THRESHOLD=64KB` + `LENO_GC_FORCE_EVERY=2000`）：
  JIT `bad=0`、`exit=0`（8100 次回收、`Yields: 8100`）；`LENO_NO_JIT=1` 也 `bad=0`、`exit=0`。
* `assert` 全套 **303 passed / 0 failed**；编译 0 warning。

**教训**：性能异常先问「**两种模式差在哪**」，而不是「哪段代码慢」。
本次是「JIT 改变了**回收时机**（回边让出 vs 安全点）⇒ 回收时机改变了**remembered set 规模**
⇒ 集合规模决定热路径的复杂度（O(|set|)）」—— 一个**隐形的 O(n)** 只在集合被撑大时才暴露，
而撑大它的正是 JIT 的大块分配。

***

### 8.72 R5：闭包进 JIT（C0/C1/C2 + upvalue 读写）—— 设计先行，三轮提交，一步一验（2026-09-15）

**背景**：`OP_CLOSURE` 的操作数长度取决于常量表里函数对象的 `upvalue_count`，
而 `opcode_size(ip)` 只有 `ip` ⇒ R3 起它被**故意**登记为"长度未知"（"宁可不编，不要猜"），
落到 scan 的 `unknown opcode 61` 拒收；`OP_GET/SET_UPVALUE` 则因无 case 落 default
被报 `unsupported opcode 14/15`。后果：**循环里只要建闭包、或循环在闭包体内访问捕获变量，
整个循环都不进 JIT**。

**设计先行**（`docs/JIT闭包与upvalue设计_R5.md`）：先交形态矩阵 + 不变量 + 分阶段计划，再动手。
按「捕获形态 × JIT 层级」切成 C0/C1/C2/C3 与 U1/U2，确立主线不变量：

> **I1：JIT 永不创建 open upvalue**。JIT 只造 **closed** upvalue（值捕获）或**复用**既有
> `Upvalue*`（by-upvalue）⇒「关闭时机 / 悬空 location / bailout 回滚 open 状态 /
> `vm_grow_frames` 地址重映射」这一整类问题被**结构性消除**，而不是被小心地绕开。
> 次要但同样关键：**I2** 不缓存 `upvalue->location`（open→closed 会改写它）、
> **I7** closure 通道按帧保存（嵌套安全）、**I8** 内联体与 upvalue 互斥。

**落地四步**（每步独立编译 + 用例 + 全套 assert）：

| 步 | 提交 | 内容 | 验证 |
|---|---|---|---|
| P1 | `34508a90` | `opcode_size_chunk()` 解析 `OP_CLOSURE` 变长操作数（`3 + 6×upvalue_count`）+ 放行 **C0 零捕获** | 探针 `scan:ALLOW-C0`、循环 `Compiled: 1`、**`Bailouts: 0`** |
| P2a | `68002778` | **ABI 第三参 closure**（存入本帧固定槽）+ `GET/SET_UPVALUE` 进 JIT（callout + 三级守卫 + 每次重取 location） | 闭环 `u1=200000 / u2=400000`（同一份存储续累加） |
| P2b | `6c119bd3` | **C1 by-upvalue**（指针复制；把字节码里的捕获描述表指针直接传进 callout） | `scan:ALLOW-C1`、`c1=600000` |
| P3 | `2a572102` | **C2 值捕获**（局部值经 JIT 栈传给 callout，建 closed upvalue） | `scan:ALLOW-C2`、`c2=1400000`；判别性判据 `collect(1000)=20` |

**本轮踩到并修掉的三个坑**（都属"看起来对、其实静默错"那一类，故逐条记下）：

1. **`OP_CLOSURE` 长度少算 1 字节**：写成 `2 + 6n`，实际是 `opcode(1) + const(2) + 6n`。
   后果：字节流从闭包之后**整体错位**——错位后仍可能解出"合法"的 opcode 序列，
   所以 scan 不报错，只是后面的局部槽存取落到错槽上（表现为 `f()` 的 callee 读成 `NULL_VAL`，
   触发 3 次 bailout 后靠解释器兜底，结果"看着还是对的"）。
   定位链：先把 `jit_callout_call_value` 的失败诊断补成**打印实际读到的值/对象类型**
   （原来只有"不是闭包/函数"一句话）⇒ 看到 `raw=0xfff8000000000000`（= NULL_VAL）
   ⇒ 用 `--debug` 反汇编交叉核对（`0010` 的 `OP_CLOSURE` → 下一条在 `0019`，即 9 字节）坐实。
   **教训：长度必须与反汇编器逐字节核对**（R3 的注释早已写明，本次是它救的场）。
2. **`pick_pin_locals` 的走查在 `OP_CLOSURE` 处被截断**：它用 `opcode_size`，拿不到长度就
   `break` ⇒ pin 的**排除项漏记** ⇒ 可能把本该按内存访问的槽 pin 进寄存器（两份值、读到过期值、
   静默算错）。这正是 §8.42 那句"新增任何局部量访问点都必须同步这里"的场景 ⇒ 一并改成带 chunk 的解析。
3. **C2 的瞬时压栈峰值必须计入 `max_vstack`**：callout 看不到 scratch/寄存器，所以局部值要先压到
   JIT 栈上；压入/弹出虽在同一指令内平衡，但**峰值**会临时抬高 vstack ⇒ 不计入 `max_vstack`
   则帧尺寸不够、压栈会踩进 `tmp/co/closure` 区（内存安全）。

**未做/维持拒绝**：**C3**（引用捕获本帧局部——需要"长期稳定的槽地址"，是唯一要改 locals 布局的重活，
按设计文档留 P4 且需实测授权）；**内联体内的闭包与 upvalue**（I8，有意）；`OP_CLOSE_UPVALUE`
（编译器从不发射）。

**验证总账**：`assert` 全套 **304 → 307 passed / 0 failed**；5 支探针（C0/C1/C2/C3 + upvalue 读写）
全部 `Bailouts: 0` 且与 `LENO_NO_JIT=1` 逐位一致；P3 用例里 `collect(1000)` 必须得 **20**
（若实现错误地让所有闭包共享一份 upvalue 会得 40，用例当场失败）。

**顺带加的诊断开关**：`LENO_JIT_CLOSURE=1` —— 只打印闭包形态行
（`[JIT-CLOSURE] scan:ALLOW-C2 caps=1 ref_local=0 value_local=1 ...`）。
加它的原因很实际：`LENO_JIT_DEBUG=1` 会把每次编译尝试的 body raw hex（最多 1200 字节）整段打印，
真实负载上输出量极大，观测体验是"这个命令跑不完"。

**收益实测**（`jit_probes/bench_closure.leno`，2000 万轮，JIT vs `LENO_NO_JIT=1`，
`Compiled: 3 / FuncCompiled: 3 / Bailouts: 0`）：

| 形态 | JIT | 解释器 | 加速 |
|---|---|---|---|
| A 循环内建**零捕获**闭包 + 调用（C0，P1 解锁） | 6156 ms | 8484 ms | 1.38x |
| B 循环内建**值捕获**闭包 + 调用（C2，P3 解锁） | 4360 ms | 5125 ms | 1.18x |
| C 循环内**调用**外部闭包（闭包体读 upvalue，P2a 解锁） | **281 ms** | 750 ms | **2.67x** |

**解读（这条决定了下一步该做什么）**：A/B 每轮都分配一个闭包 ⇒ 完全被"对象分配 ~200ns/次"
支配（§12 第 10 条），JIT 只能拿回 1.2~1.4x；**C 不分配，闭包体整体进函数级 JIT ⇒ 2.67x**，
这才是 R5 的纯 JIT 收益量级。⇒ **当前瓶颈已不在闭包覆盖面，而在分配路径**
（与 `待办_GC与分配优化.md` 的 P1/P2、§8.31 的分级待办一致）。

***

### 8.73 修复：函数级 JIT 缓存冲突颠簸（每帧成批重编译；`FuncCompiled` 5 万 → 工作集大小）（2026-09-15）

**现象**：`file_manager` 交互后报 `FuncCompiled: 50483 / Evicted: 352`，同一应用 idle 只有 `327`。

**诊断链**（每步都用一个可复现的度量，不靠猜）：

1. 用 `LENO_JIT_DEBUG=1` 的 `func compiled:` 行统计：交互 18s ⇒ **468 次编译 / 只有 53 个不同函数名**
   （`inPopupCapture` 单函数 82 次）⇒ 存在重编。
2. 日志加 `func=%p` 区分「同名不同实例」与「同一函数重编」：**74 个不同函数 / 233 次编译**
   ⇒ 同名多实例是**正常**的（各控件模块各有一份 `set_pos` / `set_size`），
   但 `setWindowHandle` **编译 18 次却只有 1 个指针** ⇒ **真重编**。
3. 日志再加 `slot=%d` + 新增 `FuncEvict` 计数：74 个函数只用到 **65 个槽**（真撞车仅 9 个槽），
   但 **`FuncEvict: 452`** ⇒ 是那 9 对**热函数在互相驱逐**（每次调用踢走对方，对方下次调用又重编）
   —— direct-mapped 的**无界颠簸**。

**根因**（都在函数级缓存，两件事叠加）：

- `jit_func_entry_claim` 取槽用 `(ptr >> 4) & mask`，**只用到地址低 8 位**；GC 池里
  同 size class 对象的槽步长是 16 的倍数 ⇒ 低 8 位高度重复。
  **循环缓存早在 §8.6x 就用混合哈希修过（`cache_hash` 的注释），函数缓存漏了。**
- 更根本的是**命中失败即驱逐**：direct-mapped 下哪怕只有 9 个槽撞车，也足以让这几对热函数无界互踢。
  循环缓存有 `JIT_CACHE_PROBES` 探测窗口，函数缓存也没有。

**修法**：

1. 新增 `jit_func_cache_slot()`：与 `cache_hash` 同款的混合哈希（`v >>= 4; v ^= v >> 8; v ^= v >> 16`），
   并把**所有**直接索引 `jit_func_cache[]` 的地方统一到它（`jit_callout.c` 的调用点 memo 缓存
   原先自己算，还漏了 R4 的 `jit_func_cache_mask`）。
2. `jit_func_entry_claim` 改为**探测窗口**：① 窗口内精确命中 → ② 窗口内有空槽就占用（不驱逐任何人）
   → ③ 窗口满才驱逐首个（机器码仍走 §8.60 的延迟释放队列）。
3. 顺手补诊断：`func compiled` 行带 `func=%p slot=%d`；stats 新增 `FuncEvict`
   （冲突驱逐次数）。**判据公式：编译次数 ≈ 不同函数数 + FuncEvict。**

**验证**（同一负载、同一口径：`file_manager`，16~18s，含同样 4 次模拟点击，`WM_CLOSE` 优雅退出）：

| 版本 | 编译次数 | 不同函数 | FuncEvict |
|---|---|---|---|
| 修复前 | **468** | 53 | — |
| 仅换混合哈希 | 323 | 74 | 452 |
| **加探测窗口后** | **74** | **74** | **0** |

⇒ **每个函数只编译一次**。`assert` 全套 **307/0**；R4 的 `LENO_JIT_FUNC_CACHE_SMALL=1`
（缩到 4 槽）用例 `test_jit_func_cache_churn` 仍 OK。

**附带收益**：机器码延迟释放队列（§8.60）的压力一并消失 —— 修复前每次驱逐都要
`jit_code_retire()`（挂队列，之后 `jit_mem_free`：Linux 上是 `munmap`），
按 `FuncEvict: 452` 的量级就是几百次可执行内存的分配/释放 churn。

**可复现的度量方法**：
- **一次性/调试类观测**（如上面的 `func compiled` 统计）：以真实窗口跑应用若干秒 → 发 `WM_CLOSE`
  让它**优雅退出**（stderr 才会 flush；直接 kill 会丢缓冲）→ 再 `Select-String` 统计。
- **性能类 A/B 一律用确定性驱动器**（2026-09-15 起，见 `jit_probes/README.md` 的
  「确定性 GUI 负载驱动器」）：`SDL_VIDEODRIVER=dummy`（**不开任何窗口**）+
  `LENO_SDL_FRAMES=<n>` ⇒ 固定帧数、自终止，打印 `sum`（负载一致性判据）与 `us_per_frame`。
  实测 `sum` 四次逐字相同、`us_per_frame` **±1.5%** —— 比之前的壁钟口径（±400%、方向还会翻转）
  可用得多。**注意**：动过 `leno_module/**/lib/**` 后以前必须手清 `.lenocache`，否则会静默跑旧模块
  （驱动器开发时正是这么被坑了 90s）。**已于 2026-09-16 修复**（`fccd1534`：入口缓存加
  `entry_<hash>.lenb.deps` 依赖清单）并**实测验证**：给 `sdl_window.leno` 追加一行注释后，
  入口缓存 `.lenb` 时间戳立刻更新（07:15:10 → 07:34:02）⇒ 依赖变更会正确失效。
  遇到"改了却没生效"时可先清 `.lenocache` 作排查兜底，但不再是必需步骤。
- **完全不开窗的静态手段**：`--debug-out <file>` 在执行前落盘主程序 + 全部模块的字节码
  （见 `JIT闭包与upvalue设计_R5.md` 的 P0 实测）。

**后续（2026-09-16，§8.78）**：本节修的是**哈希碰撞**（混合哈希 + 探测窗口）；但真实应用的
**工作集 > 表长**时仍会颠簸 —— 实测 file_manager 同配置三跑 `FuncEvict` = 4 / 504 / 3767、
`FuncCompiled` = 97 / 597 / 2748、`us_per_frame` ±3.2%。**函数级缓存 256 → 1024 槽**后
`FuncEvict` **归零**、`FuncCompiled` 逐字可复现（恒 97）、噪声降到 **±0.6%**。
（缺口直方图 v1 的虚高也是同一根源，见 §8.77 的 v1→v2 修正。）

***

### 8.74 分配路径：把池上限提到实测饱和点 32MB（`new Pair` −38%、`new Big` −47%）（2026-09-15）

**背景**：`待办_GC与分配优化.md` 的 §一 结论是"分配本体才是大头"，但**它列的负结果里有一条
已经过期**：§三 第 2 条说"放大 `GC_POOL_MEM_LIMIT` 只省 ~4ns，因为池必须覆盖**整段**分配，
而 JIT 循环内对象不回收到 free-list"。这条推理的前提是"**没有**回边让出"；
§8.37/§8.38 落地让出之后，回收会周期性发生 ⇒ 池只需覆盖**一个让出间隔**内的分配量。
⇒ 值得重测（项目自己的方法论：负结果也要检查其前提是否还成立）。

**做法（先做可调的测试基建，再 A/B）**：把池上限从编译期常量变成**运行时可覆盖**——
`LENO_GC_POOL_LIMIT=<bytes|KB|MB>`（`gc.c`，只在 `gc_init` 读一次），
这样能在**同一个二进制**上切配置做 A/B，不受重建差异干扰。

**实测**（`jit_probes/probe_alloc2.leno`，N=1000 万，两轮，噪声 ±2 ns）：

| 池上限 | `new Pair`(2 字段) | `new Big`(6 字段) | 数组字面量 |
| --- | --- | --- | --- |
| 4MB（旧默认） | **110.0 / 107.2 ns** | **170.2 / 168.5 ns** | 148 ns |
| 16MB | 69.9 ns | 95.5 ns | 148 ns |
| **32MB（新默认）** | **68.1 / 69.3 ns** | **88.8 / 90.1 ns** | 149 ns |
| 64MB | 69.9 ns | 90.4 ns | 150 ns |
| 1024MB | 67.8 ns | 87.6 ns | 148 ns |

⇒ **32MB 即饱和**（16MB 略欠、64MB 无更多收益）；默认值已改为 32MB。
上限只是**天花板**（池按需增长），所以对"分配很少"的程序零代价。

**一个被顺带纠正的归因**：原记录把成本拆成"每字段 ~28ns 的双遍写 + ~79ns 未归因"，
实测**边际每字段成本只有 ~4.7 ns**（`Big − Pair` 的差 ÷ 4 = (88.8−68.1)/4）。
差额去哪儿了？——**冷内存**：从不复用的内存每次都要 `memset(0)`、触发 page fault、
且首次触碰是 cache miss。池能复用后这部分基本消失。

**验证**：`assert` 全套 **307/0**；`数组字面量` 不受影响（`OBJ_ARRAY` 有意排除在池外：
它有自己的 header 回收链，`arr_new` 复用时会清 `flags`，与池链双重持有会破坏链）。

**端到端能看到多少？（如实记录，避免以后被当成"有效/无效"的证据）**

- **math-bound 负载看不到**：`examples/性能测试/光线追踪对象版.leno`，4MB vs 32MB =
  **2.20s vs 2.19s**（墙钟）。原因：该负载分配率低 —— `Yields: 122` 说明池只需覆盖
  一个让出间隔内的少量分配，4MB 早就够。
- **GUI 负载用这套 harness 测不出**（不是"没效果"，是**分辨率不够**）：`file_manager`
  同窗口时长 + 同 4 次模拟点击，比 CPU 时间，**交替同轮** A/B/A/B：
  `4MB/32MB = 1.48s/1.11s`（第一轮）→ `0.28s/0.36s`（第二轮，**方向翻转**）。
  两次运行的 `FuncExecuted` 相差 60 倍（32506 vs 550）⇒ **负载本身在两次运行间就变了**
  （GUI 的工作量取决于状态与交互节奏）。这与 `待办_GC与分配优化.md` 开头那句
  "负载波动大"是同一回事。
  ⇒ **GUI 应用的端到端 A/B 必须用确定性驱动器（固定脚本 / 固定帧数），否则不要下结论。**
  （**已实现，2026-09-15**：`LENO_SDL_FRAMES` + `SDL_VIDEODRIVER=dummy`，
  见 §8.73 的「可复现的度量方法」与 `jit_probes/README.md`；实测 `sum` 四次逐字相同、
  `us_per_frame` ±1.5%。）
- 因此本项的**唯一判据是微基准**（同二进制切 `LENO_GC_POOL_LIMIT`、交替、噪声 ±2 ns）：
  `new Pair −38%`、`new Big −47%`。

***

### 8.75 R6-a：函数级 JIT 的多返回值（单值 → 发布区 + 逐点交付）（2026-09-15）

**原状态**：`jit_compile_function` 直接 `return NULL` 拒收 `func->return_count > 1`
（原注释写"多返回值语义复杂"）；`ops_return.inc` 的 `OP_RETURN_MULTI` 在 `func_mode` 下
对 `rc > 1` 只发射一条 bailout —— 那是**占位**，不是实现。后果：**返回多个值的函数永远
进不了函数级 JIT**，包括 `Font.measureString` 这种每帧被调成百上千次的真实热点。

**动手前最有价值的一条取证**：调用侧的**多返回值交付合约早就存在且完备** ——
VM 重入路径（`jit_invoke_closure` / `jit_callout_global_func`）已在做同一件事：

```
RAX = results[rc-1]（新 TOS）；vstack_top[arg_count-1-i] = results[i]（i = 0..rc-2）
```

更关键的是 **scan/codegen 早已按 callee 的 `return_count` 记好了多返回值的栈布局**
（§8.56 的 `jit_resolve_module_func`、§8.48 的方法 rc 推断、`OP_CALL_GLOBAL_FUNC_TYPED`
的 `ret_count`）。缺的其实只有两处：`rc == 1` 这道守卫，以及 **callee 侧根本没有发布多返回值
的地方**。⇒ 工作量比"扩返回值通道"的原始估计小得多（这也是本条的第一个教训：
**先找现有的对偶机制，再动手设计新通道**）。

**契约（发布区，见 `jit_priv.h`）**

- `jit_fn_results[VM_MAX_RETURNS]`：机器码在 `OP_RETURN_MULTI(rc)` 里把全部结果写进来，
  顺序与解释器**逐字一致**（`results[0]` 最深、`results[rc-1]` = TOS）；
- `jit_fn_result_count` = rc（编译期常量，写死进机器码）；`jit_fn_result` 仍同步为 `results[0]`，
  **单返回值路径一行未改**（继续只写 `jit_fn_result`，热路径零额外开销）；
- 调用方按静态 rc 取用；`rc == 1` 时行为与改动前逐字相同。

**交付**：新增 `jit_fastpath_deliver_multi()`（`jit_callout.c`）。三处快路径
（`call_module_func` / `jit_invoke_closure`（invoke_method 与 call_value 共用）/
`call_global_func`）由 `rc == 1` 放宽到 `1 ≤ rc ≤ 16` 后统一调它；解释器热入口
（`jit_try_hot_func_call`）的折叠也按 `jit_fn_result_count` 支持多值（结果从原 callee 槽
按序排列，用 `vm_stack_push` 而非直接写：rc 可以大于 `arg_count + 1`，那时需要扩容检查）。

**防御（特意加的，建议保留）**：交付前复核 `jit_fn_result_count == rc`，不符就**回落 VM 重入**
—— 覆盖 `return_count == -1`（静态不可知）的函数：机器码按运行时实际个数发布，调用方按
编译期假设取用，两者不一致时宁可慢也不能错记账（§8.48 老 bug 的形态正是"第一个结果落在
实参槽上，读到残留值"）。

**实施时踩到的坑（记下来避免重犯）**

1. **机器码只能用 8 字节 store**（没有 32 位 store 的发射器）⇒ `jit_fn_result_count` 必须
   声明成 `int64_t`。写成 `int` 会**连带覆写相邻 4 字节的全局**（`jit_func_depth` 之类）。
2. `EMIT_RAW_TO_VALUE()` 用 R8 当 scratch ⇒ store 地址必须在转换**之后**才装填，否则被踩。
3. `jit_fastpath_deliver_multi` 的 `vstack_top` 是 **raw 的 `int64_t*`**，不是 `Value*`
   （写回要过 `jit_value_to_raw`）—— 编译器的 `-Wpointer-sign` 直接把这处点出来了。
4. **不需要注册 GC 根**：JIT 帧内 GC 只置让出标志、不就地回收（§8.36/§8.37），只要调用方在
   下一次安全点之前把值搬上 JIT 操作数栈 / VM 栈就安全 —— 与既有 `jit_fn_result` 的前提相同。
   这条不写清，后人很容易以为漏了根注册。

**验证**

- 新探针 `jit_probes/probe_multi_ret_jit.leno`：2 值（int）/ 2 值（float）/ 3 值 /
  **多返回值嵌套**（被调函数内部再调多返回值函数，两层共用同一发布区）。
  JIT 与 `LENO_NO_JIT=1` 输出**逐字一致**，`FuncCompiled: 4 / Bailouts: 0`；
  `LENO_JIT_FTRACE=1` 下 **3903 次快路径完成（`jr=0 failed=0`）、零回退**
  ⇒ 新交付代码确实在执行（不是"两边都退回解释器所以看着一致"）。
- **既有用例的有效性被这次改动提升了**：`assert/test_jit_multiret_method.leno`
  （热点循环 → callout → 函数级 JIT 内的多返回值方法调用）与 `test_jit_hot_func.leno` 用例 12
  （解释器热入口）**以前"能过"是因为 callee 被拒收后退回 VM 重入**，实际测的是解释器路径；
  现在 callee 真的被编译，它们才真正覆盖函数级 JIT 的多返回值。
- assert **307 passed / 0 failed**（与改动前一致）。

**剩余（R6-b `OP_TAIL_CALL`）**：循环模式下它与 `OP_RETURN` 同类（loop JIT 无法从机器码返回
函数）⇒ 应归入 `has_reachable_return` 判据**维持拒收**；函数模式要当"调用 + 返回"实现，
但动手前必须先取证 VM 的**帧复用**与 **open upvalue 关闭**语义（否则会踩 R5 那类生命周期
不变量）：① 尾递归不增长 VM 栈（JIT 走 C 栈 ⇒ 深递归靠 `JIT_FUNC_MAX_DEPTH` 回退解释器）；
② 返回值个数匹配；③ 当前帧的 open upvalue 是否必须关闭。

***

### 8.76 R6-b：`OP_TAIL_CALL`（函数级 JIT 实现；循环 / 内联维持拒绝）（2026-09-15）

**结论先行**：尾调用在 JIT 里实现为「**调用 + 把结果当作本函数的返回值**」；
循环模式与内联体一律拒绝。落地过程中**先挖出一个前置缺口**（见下），
它才是"尾调用函数根本进不了 JIT"的真正原因。

**前置缺口：`OP_GET_GLOBAL_FUNC` 没有 case（opcode 18）**

`return f(x)` 的字节码形态是 `OP_GET_GLOBAL_FUNC f` + `OP_TAIL_CALL argc`
（**不是**合体的 `OP_CALL_GLOBAL_FUNC`）：

```
0000 OP_GET_LOCAL      0
0003 OP_GET_GLOBAL_FUNC 1     ← opcode 18
0006 OP_TAIL_CALL      1
```

而这条指令此前**只在长度表里**（3 字节）、没有 case ⇒ 落 default 被报
`unsupported opcode 18` ⇒ 含尾调用的函数**整体被拒**。也就是说：R6-b 的第一步不是写尾调用，
而是补 `OP_GET_GLOBAL_FUNC`（scan `vstack++` / callout 读 `vm.global_funcs[slot]` /
codegen 推送值）。**顺带收益**：解锁「取函数值再调用」的一般形态
（`OP_GET_GLOBAL_FUNC + OP_CALL`，如把函数存进数组/字典再调用）。

**实现（四处）**

| 位置 | 内容 |
|---|---|
| scan（循环侧） | `case OP_TAIL_CALL`：`vstack -= (ac+1)` + `has_reachable_return = 1` + `dead = 1`。循环模式据此被 `jit_compile` 拒绝；**函数模式忽略该标志** ⇒ 正常编译（与 `OP_RETURN` 同一套机制） |
| scan（内联侧） | **拒绝内联**：尾调用要求"callee 的结果成为**外层函数**的返回值"，而内联机制只能把结果变成"这次调用的结果"（jump 到 inline_end）⇒ 语义不等价 |
| codegen | 函数模式：callout → 失败检查 → `EMIT_EPILOGUE()`（结果已在发布区，RAX 不需要回读）；循环模式 / 内联体 → `return 0`（防御性兜底） |
| callout | `jit_callout_tail_call()`：深度守卫 → `vm_call_value` 完整路径 → 按 VM **实际发布**的个数把结果搬进 `jit_fn_results[] + jit_fn_result_count` |

**三处关键语义（必须留痕）**

1. **"关闭本帧 upvalue"对 JIT 函数恒为空操作**。VM 复用当前帧，所以必须先
   `close_upvalues`（`op_call.inc:42-46`）；JIT **不复用帧**（机器码没有 interpreter frame），
   而按 R5 的不变量 **I1（JIT 永不制造新的 open upvalue）+ C3（引用捕获本帧局部）维持拒绝**，
   JIT 化的函数**不可能**有指向自己 locals 的 open upvalue ⇒ 那一步无事可做。
   ⚠ **跨轮耦合**：若将来 P4（C3 提升槽）落地，`OP_TAIL_CALL` 必须重新审查
   （已同步写进 `JIT闭包与upvalue设计_R5.md` 的 I1 处）。
2. **空间行为靠深度守卫 + 解释器 TCO 兜底**。帧复用的意义是"尾递归不增长 VM 栈"；
   JIT 是 C 调用链 ⇒ `jit_func_depth >= JIT_FUNC_MAX_DEPTH` 时**直接 bailout**，
   把这条指令交给解释器。实测走向比预期更好：JIT 第 0 层做尾调用 → `vm_call_value` →
   **解释器用自己的 `OP_TAIL_CALL` 真 TCO 走完剩余全部层数** ⇒ JIT 深度恒为 1、
   C 栈不增长、VM 帧数不增长（探针 20000 层 × 200 轮，`Bailouts: 0`）。
3. **rc 不做静态假设**。尾调用的 callee 是运行期值（局部闭包 / 参数 / 全局函数…），
   所以 callout 读 VM 实际发布的 `last_return_count / last_return_values` 原样搬运 ——
   单返回 / 多返回 / 原生函数都正确（多返回值**直接**写成 `return f()` 目前被类型检查拒绝
   ⇒ 现实里 rc 恒为 1，这条是安全网）。

**验证**

- 新探针 `jit_probes/probe_tail_call_jit.leno`：`sumTo`（深尾递归）/ `scale`（末尾是尾调用的
  中间层）/ `loopTail`（循环体内的尾调用，loop JIT 必须拒收）。JIT 与 `LENO_NO_JIT=1` 输出
  **逐字一致**；`FuncCompiled: 2 / Bailouts: 0`；20000 层 × 200 轮墙钟 **0.1s**、不崩。
- **探针写法上的两个坑（值得记住）**：
  ① **必须经函数值调用**：直接 `sumTo(n,0)` 时编译器会在调用点**内联展开**这个小函数
  （`main` 里出现的是展开后的循环体）⇒ 函数级 JIT 根本不触发。第一版探针就是这么
  "测了个寂寞"（`FuncCompiled: 0`），改成 `var sumF = sumTo; sumF(n,0)` 后立刻看到
  `func compiled: 'scale'`。
  ② 函数级 JIT 有**热度阈值**：只调用一次不编译，要在循环里跑热。
- assert **307 passed / 0 failed**（与改动前一致）。

**仍未支持**：`OP_TAIL_CALL_NATIVE`（`return 原生函数(x)`）同样只登记了长度、没有 case ⇒
含它的函数维持拒收（诊断已从 "unknown" 变成 "unsupported"，可排期）。

**补充（同日，收益量化时发现的两件事）**

1. **必须加"callee 也是 JIT 函数"的快路径，否则 R6-b 是负优化**。
   第一版 `jit_callout_tail_call` 直接走 `vm_call_value`（VM 重入：压参、压 callee、建帧、
   跑解释器、拆帧），实测 **0.88x（比纯解释器慢 12%）** —— 因为解释器的 `OP_TAIL_CALL`
   只是"复用当前帧 + 跳转"，近乎零成本（与 §9 那条 2026-09-13 的评估结论完全一致：
   "尾调用本身一点都不慢"）。
   修法：callee 可函数级 JIT 时直接调机器码，且 **callee 的发布区
   （`jit_fn_results[] + jit_fn_result_count`）正好就是本函数要发布的东西** ⇒ 原地返回，
   连拷贝都不需要。实测 **0.88x → 2.29x**。参数提升逐条对齐 `call()`
   （尾调用的 callee 是运行期值、调用点没有静态类型 ⇒ 不能指望调用方插了转换指令）。
2. **收益基准的 `CHECK` 当场抓出一个静默错误（值得记）**。
   快路径最初"直接读 `jit_fn_results[]`"来发布，但**单返回路径只写 `jit_fn_result`、
   不写数组**（数组里留的是上一次多返回调用的残留）⇒ 返回值静默错误；
   同时 `jit_fn_result_count` 也没在调用前复位为 1（会读到上一次多返回的个数）。
   两个基准的 `CHECK:` 不一致立刻暴露了它：JIT `acc=666713333726` vs 解释器
   `2666680333334`，差额正好是尾调用那一项。
   ⇒ **凡是"量化收益"的测量，都必须配一个跨模式一致的 `CHECK`**。只看耗时的话，
   这次会看到"2.29x 的巨大提升"，却把结果算错 —— 比没做优化更糟。
   修法：`rc == 1` 走 `jit_fn_result`、`rc > 1` 走数组，且调用前复位 `jit_fn_result_count = 1`。

***

### 8.77 `LENO_JIT_GAPS`：拒收原因的**聚合**直方图 + 当前缺口清单（2026-09-16）

**为什么又加一个开关**：做"剩余缺口盘点"必须能**低开销**地跑真实应用，而
`LENO_JIT_DEBUG=1` 做不到 —— 实测 file_manager 60 帧：**stderr 8.4MB、>120s 都跑不完**
（每次编译尝试都把 body raw hex = 最多 1200 字节整段打出来）。
`LENO_JIT_GAPS=1` 只累加计数、退出时打印一次：**300 帧 2.5s、stderr 1.3KB**。
键形如 `模式|原因`，模式 ∈ {`loop`, `func`, `inline`}；原因里带 **opcode 名**
（复用 `debug.c` 新暴露的 `opcode_name()` —— 避免"人工对着枚举数编号"这个最易错的步骤）。

```powershell
$env:SDL_VIDEODRIVER='dummy'; $env:LENO_SDL_FRAMES='300'; $env:LENO_JIT_GAPS='1'
build\leno.exe file_manager.leno        # 退出时打印 === JIT 拒收原因 ===
```

**v2（同日）：按「对象身份」去重 —— 计数 = "有多少个不同的函数/循环/被调方因该原因被拒"**

v1 的逐次计数**不可用**：编译尝试会因 `jit_func_cache` 冲突驱逐（§8.73）而重复发生 ⇒ 同一应用
重跑能差 20 倍（`inline|OP_CALL` = 259/259/9、`func|函数体含循环` = 13/305/13、
`FuncCompiled` = 97~597）；更要命的是**记账本身的 I/O 会扰动时序、反过来加剧驱逐**（观察者效应）。
按 (对象身份, 原因) 去重后：**同一应用两次运行逐字相同**（file_manager 两跑的整个直方图与
`FuncCompiled: 97` 完全一致）⇒ 这个计数才是可比的。

**稳定结果（4 个样本的并集：file_manager ×2 / cache_cleaner / 五子棋；headless + 300 帧）**

| 计数 | 模式 | 缺口 | 性质 |
| --- | --- | --- | --- |
| 39 / 9 | inline | 模块变量（`OP_GET_MODULE_VAR` 88）/ 模块函数（`OP_GET_MODULE_FUNC` 90）**跨模块** | **正确性边界，刻意保留**（§8.69） |
| 36 + 28 | func | 函数体含循环（`OP_FOR_PREP` 95 / `OP_LOOP` 58） | **设计限制**（`func_mode` 只保证无循环函数正确） |
| 28 + 20 + 4 | inline / func / loop | `OP_GET_CSTRUCT_DEF`(138) | **FFI / cstruct**，需单独立项 |
| 12 + 12 + 8 | func / loop / inline | `OP_CLIB_CALL`(142) | **FFI / C 库调用**，同上 |
| ~~20~~ → **0** | inline | ~~`OP_CALL`(59) 内联扫描没有 case~~ | **✅ 已解决（2026-09-16，R7④，见下）** |
| 10 | loop | 循环体含可达 return | 设计限制（loop JIT 无法从机器码返回函数） |
| 3 | func | `OP_DICT`(69)（字典字面量 `{}`） | 可做，但**量很小** |

> **修正（重要）**：v1 里"`OP_DICT` ×251"是**驱逐重编译造成的虚高** —— 去重后只有 **3 个对象**。
> 按 v1 的数据会把"字典字面量"误判成第一大缺口。这正是"**先让测量稳定、再下结论**"的价值；
> 也说明：**任何随编译次数变化的计数，都必须先与缓存驱逐解耦**。

**R7④（2026-09-16 完成）：内联侧补 `OP_CALL`(59) 与 `OP_GET_GLOBAL_FUNC`(18)**

`scan_callee_for_inline()` 此前**没有这两条 case** ⇒ **被调函数体内只要有一次"取函数值再调用"
（`var f = foo; f(x)` / `arr[i](x)` / `self.cb(x)`），该函数就永远不能内联**（落 default 报
`unsupported opcode 59` / `18`）。修法照 loop scan 的同名 case 补记账：
`[args(ac)][callee]` → 净效应 `rc - (ac + 1)`，rc 静态不可知 ⇒ **按 `rc = 1` 记账**，
由 callout 在**调用前**复核 `return_count`（不等于 1 就 bailout，多返回值解构交解释器）。

- **实测（拒收清零）**：file_manager / cache_cleaner 的 `opcode 59` 与 `opcode 18` 计数
  **双双归零**，且**没有前移到新的 opcode**。
- **实测（收益）**：探针 `jit_probes/probe_inline_call_value.leno` 用**同一二进制**跑
  `LENO_JIT_NOINLINE=1` 做 A/B —— **34.5 ms vs 93.9 ms = 2.72x**；
  对照组（`sumViaClosure`：体内含闭包、按 I8 不被内联）两侧 **±1%** ⇒ 差异确实来自内联本身。
- **用例**：`assert/test_jit_inline_call_value.leno`（argc = 1/2/3 + 局部零捕获闭包当 callee +
  同体三处调用，**数值精确**判据）。assert **308 passed / 0 failed**。

**分布特征**：FFI 两类合计 **84 个对象**（最大的一档）、"函数体含循环" **64**（设计限制）、
"跨模块访问" **48**（刻意保留）⇒ 内联侧 `OP_CALL` 补齐后，**"补 opcode 就能拿下"的只剩
`OP_DICT`(3)**。覆盖面工作基本收尾：剩下的要么是刻意保留 / 设计限制，要么需按 FFI 单独立项。

**负面结论（确认前几轮的收口）**：清单里**没有** `OP_GET/SET_UPVALUE`(14/15)、
`OP_CLOSURE`(61)、`OP_TAIL_CALL`、`OP_GET_GLOBAL_FUNC`(18) ⇒ **R5 / R6 的缺口确实清零了**
（对比 2026-09-14 的 `76×4 / 93×3 / 14×2 / 142×2 / 138×49`）。

***

### 8.78 函数级 JIT 缓存 256 → 1024 槽：颠簸清零、测量噪声降 5 倍（2026-09-16）

**动机**（这条是被"整理剩余工作"逼出来的）：§8.73 修掉的是**哈希碰撞**（混合哈希 + 探测窗口），
但做缺口盘点时发现真实应用的 `FuncEvict` 仍会飙到几千 —— 而且**同配置重跑差异巨大**，
直接威胁"编译相关计数"的可信度。

**症状（file_manager，headless + `LENO_SDL_FRAMES=300`，同配置 3 次）**

| 指标 | run1 | run2 | run3 |
| --- | --- | --- | --- |
| `sum`（负载一致性判据） | 21600 | 21600 | 21600 |
| `us_per_frame` | 6854 | 7301 | 6971 |
| `FuncEvict` | 504 | **3767** | 4 |
| `FuncCompiled` | 597 | **2748** | 97 |

⇒ 渲染工作量三次**完全相同**（`sum` 逐字一致），但编译次数差 **28 倍**、驱逐次数差 **900 倍**，
`us_per_frame` 波动 **±3.2%**。根因不是碰撞（§8.73 已修），而是**工作集 > 表长**：
file_manager 里同名多实例（各控件模块各有一份 `set_pos` / `set_size`…）使"被编译的函数实例数"
远超 256 ⇒ direct-mapped 表无论哈希多好都会持续互踢。

**修法**：`JIT_FUNC_CACHE_SIZE` 256 → **1024**（`jit_priv.h`，一行）。1024 槽 ≈ 32KB 元数据。
`LENO_JIT_FUNC_CACHE_SMALL=1`（4 槽）的 R4 复现开关不受影响（它直接改掩码）。

**结果（同配置 3 次）**

| 指标 | 256 槽 | 1024 槽 |
| --- | --- | --- |
| `us_per_frame` | 6854 / 7301 / 6971（**±3.2%**） | **6817 / 6739 / 6743（±0.6%）** |
| `FuncEvict` | 504 / 3767 / 4 | **0 / 0 / 0** |
| `FuncCompiled` | 597 / 2748 / 97 | **97 / 97 / 97** |

⇒ 颠簸清零、编译次数**逐字可复现**、测量噪声降 **5 倍**（最好的单次耗时基本持平，
说明原来的开销主要体现在**最坏情况**与**不可复现**上）。

**方法论收获**：这属于"**先把测量做干净，再谈优化**"的典型 —— 前几轮所有
"编译数量/拒收计数"类观测都被这个颠簸污染过（缺口直方图的虚高也是同一根源，
见 §8.77 的 v1→v2 修正）。

***

### 8.79 两项"先量收益再立项"的实测裁决：C3 确认不做、FFI 立为立项候选（2026-09-16）

**方法**：§8.77 的 `LENO_JIT_GAPS=1`（按编译对象去重）+ 闭包专用开关 `LENO_JIT_CLOSURE=1`，
三个真实应用（file_manager / cache_cleaner / 五子棋，headless + 300 帧）。

#### ① C3 闭包（引用捕获本帧 locals）：**gate 再确认 = 0 ⇒ 不做**

R5 设计稿 2026-09-15 的 **静态普查**口径已给出"57 条 C3 捕获，但全部落在冷路径 ⇒ 不做 P4"。
本次从**热代码口径**独立复核，结论一致且更直接：

| 证据（2026-09-16） | 结果 |
| --- | --- |
| 三个应用的 `loop\|OP_CLOSURE C3（引用捕获本帧局部）` 计数 | **0 / 0 / 0** |
| file_manager 60 帧 + `LENO_JIT_CLOSURE=1` 的闭包形态行总数 | **0 行**（连放行形态 C0/C1/C2 的 `scan:ALLOW-*` 行都没有）|

⇒ **真实应用"能走到编译尝试"的热循环里根本没有 `OP_CLOSURE`**。
**裁决**：**维持不做 P4**（它要改 locals 布局、破坏 I1，并牵连 `OP_TAIL_CALL`，见 §8.76 的跨轮耦合）。
**触发条件（照抄 R5 设计稿的口径）**：一旦出现「热循环里创建/注册回调」的写法
（如按列表项批量 `arr.add(func(){...})`），按本节方法重测后优先做。

#### ② FFI（`OP_GET_CSTRUCT_DEF` 138 / `OP_CLIB_CALL` 142）：**当前唯一有量化收益面的剩余缺口**

先读 VM 实现把它俩是什么搞清楚（不按名字猜）：

- **`OP_GET_CSTRUCT_DEF`**（`op_cstruct.inc:103`）：按**名字字符串**查 cstruct 定义注册表
  （`cstruct_def_find`）后把定义对象压栈 —— 是**一次注册表查找**，不是字段访问。
  ⇒ JIT 侧可照 `jit_resolve_module_method`（§8.56）的做法**编译期解析**（注册表启动后不变），
  codegen 用 callout 把解析好的 def 压栈。**成本低、模板现成。**
- **`OP_CLIB_CALL`**（`op_clib_call.inc`）：真正的 FFI 调用，带 `ret_type_kind(1)` +
  逐参 `arg_types[]`，返回值按 TypeKind 自动展开 ⇒ 需复用既有 native 调用 callout 的机械 +
  逐条对齐窄化/展开规则。**成本中等。**

**收益面（同一次实测，4 样本并集、按编译对象去重）**

| 波及面 | cstruct(138) | CLIB(142) | 合计 |
| --- | --- | --- | --- |
| **被整循环拒收的热循环** | 4 | 12 | **16** |
| 不能内联的被调方 | 28 | 8 | 36 |
| 不能进函数级 JIT 的函数 | 20 | 12 | 32 |

- "热循环"指**回边 ≥ `JIT_HOT_THRESHOLD`(50) 才会走到编译尝试** ⇒ 这 16 个循环确实在反复执行，
  现在退化为纯解释执行；同类循环 JIT 化后的实测加速比量级是 **5~9x**（§9 覆盖面基准）。
- ⇒ 这是**当前唯一有量化收益面的剩余缺口**（对比：跨模块访问 48 个是刻意保留的正确性边界、
  "函数体含循环" 64 个是设计限制、其余可做的只剩 `OP_DICT` 3 个）。

**建议顺序**：**cstruct 优先**（编译期解析 + 压栈，与 §8.56/§8.68 同构，风险低），
CLIB 次之（ABI/类型展开/报错文本要逐条对齐）。两者都必须带"解析不出来就拒绝编译"的守卫
（宁可不编，不要猜），并配 `assert/test_jit_*.leno` 用例 + `LENO_JIT_GAPS` 复核拒收清零。

**更新（2026-09-16，R6-d 落地后）**：cstruct 已做完（§8.80），但上面对 cstruct 的
"编译期解析"判断**是错的** —— 实测发现定义是**运行时注册**的、且同名重声明会覆盖条目并废弃旧
def（`object_cstruct.c:229-241`）⇒ 只能嵌名字常量、在 callout 里每次现查（详见 §8.80）。
结果：cstruct 在 census 里**归零**，但**循环级净解锁 = 0** —— 原来被 138 挡的 4 个热循环
全部转为被 `142(OP_CLIB_CALL)` 挡（fm / cc / gomoku 各剩 4 个热循环，全部是 CLIB）
⇒ **`OP_CLIB_CALL` 现在是循环级唯一剩下的 FFI 遮断原因**，优先级应排在 cstruct 的后续项
（`OP_AS_CAST`、native 方法派发）之前。

***

### 8.80 R6-d：`OP_GET_CSTRUCT_DEF` 进 JIT（+ 度量：解锁 8 个函数 / 13 个内联点，循环级净 0，并露出两个相邻缺口）（2026-09-16）

**原状态**：`OP_GET_CSTRUCT_DEF`(138) 在 scan 的长度表里有（3 字节，`name_const(2)`），但
`scan_loop_body` / `scan_callee_for_inline` 都**没有 case** ⇒ 落 `default` 被报成
"unsupported opcode 138" ⇒ 含它的**整个循环 / 函数 / 被调方**全被拒（§8.77 实测：
loop 4 / func 20 / inline 28 个对象）。这是 R6-c 之后**唯一有量化收益面的剩余 FFI 缺口的一半**
（另一半是 `OP_CLIB_CALL`）。

**语义取证**（`op_cstruct.inc:103-129`）：读名字常量（必须是字符串）→ `cstruct_def_find(name)`
→ 找不到报"找不到 cstruct 定义 'X'"，找到则 push `val_obj(def)`。codegen 侧触发点是
「**cstruct 类型名当表达式**」（`codegen_expr.c` 的 `SYM_CSTRUCT` / `cached_type` 两处），
所以真实写法就是 `T.malloc()` / `T.size()` 这种"类型名当接收者"。

**关键设计判断：这里**不能**做编译期解析（与 §8.56 模块方法、`OP_CALL_NATIVE` 的 native 表相反）**

| 事实 | 来源 | 后果 |
| --- | --- | --- |
| cstruct 定义是**运行时注册**的（`OP_DEFINE_CSTRUCT` 执行时才注册） | `op_cstruct.inc:76` | 编译期可能查不到（不是"不会变"，是"可能还没有"）|
| 同名重声明会**覆盖**注册表条目，并把**旧 def 的 name / 字段表清空** | `object_cstruct.c:229-241` | 编译期嵌入的 `def*` 会被废弃 ⇒ 与解释器分叉 |

⇒ 采用：**只嵌入名字字符串指针**（GC 非移动 = "槽地址稳定"，`gc.c:295`，与
`ops_callout.inc:450` 嵌模块指针同一前提），**查找每次执行在 callout 里做一遍**。
代价是每次执行一次线性表 + `strcmp`（一个程序里 cstruct 定义通常个位数），换来的是
**与解释器逐字一致**（含覆盖语义与报错文本）。

**实现**（4 处，全部对照既有模板抄）

| 位置 | 内容 |
| --- | --- |
| `jit_scan.c`（`scan_loop_body`） | `case OP_GET_CSTRUCT_DEF: vstack++`（net +1）|
| `jit_scan.c`（`scan_callee_for_inline`） | 同上（内联侧允许 callout 类 opcode，与 `OP_GET_GLOBAL_FUNC` 同一先例）|
| `ops_callout.inc` | codegen 照 `OP_GET_GLOBAL_FUNC`（§8.68）逐行同构：常量解码检查 → `TOS_SPILL` → callout → `failed` 检查 → `VALUE_TO_RAW` → `TOS_PRODUCE`；**常量越界/非字符串一律 `return 0` 拒编**（交解释器报"cstruct 名称必须是字符串"，报错文本零改动）|
| `jit_callout.c` | `jit_callout_get_cstruct_def(ObjString*)`：查到返回 `val_obj(def)`；查不到 `failed=1` + `NULL_VAL` ⇒ bailout 交解释器报原文 |

**验证**

1. **探针 `jit_probes/probe_cstruct_jit.leno`**（n=20000）：
   - JIT 与 `LENO_NO_JIT=1` 输出**逐字一致**（`defLoop=20001` / `loopDef=20001`）；
   - `Compiled: 2`（两个热循环都编了）；
   - **字节码转储核对**（`--debug-out`）：循环体里确有 `OP_GET_CSTRUCT_DEF 2 (Cell)`（bc_off=29）
     与 `... 11 (Vec2)`（bc_off=105）——**只跑对拍不看这条是不够的**，否则可能整个探针
     根本不含目标指令（本次差点如此：第一版探针用的是 `Vec2.size()`，被下面的缺口掩盖）；
   - `LENO_JIT_DEBUG=1` 下 `get_cstruct_def` **零失败**（0 条 `[JIT-CALLOUT-FAIL]`）。
2. **真实应用逐对象差分**（改前/改后两份 `LENO_JIT_GAPS` census 做集合差，三应用）：

   | 侧 | 138 消除 | 转移到其它原因 | **净解锁** |
   | --- | --- | --- | --- |
   | loop | fm 1 / cc 1 / gomoku 1 | 全部转 `142(OP_CLIB_CALL)`（各 3→4） | **0** |
   | func | fm 8 / cc 4 | fm：2 转 142、**1 转 `133(OP_GET_FIELD_ADDR)`** | **8 个函数** |
   | inline | fm 9 / cc 7 / gomoku 3 | 各 +2 转 142 | **13 个被调方** |

3. **两个真实应用端到端 JIT / `LENO_NO_JIT=1`**（headless 300 帧）：`[SDL-BENCH]` 的
   `sum` 逐字相同（file_manager 21600、cache_cleaner 4800），`exit=0`；JIT 侧
   `FuncCompiled: 109 / 72`、`Bailouts: 3 / 0`。
4. assert **308 passed / 0 failed**。

**⚠ 本次最重要的方法论教训：census 每个对象只记「第一个」遮断原因**

`scan` 一遇到不认识的 opcode 就 `return`（长度未知无法继续）。所以修掉一个原因后，
**必须重测并做逐对象集合差**，否则会把"换了个遮断原因"当成"解锁" —— 本次正是靠差分才发现
**循环级净解锁 = 0**（原来被 cstruct 挡的循环现在被 `OP_CLIB_CALL` 挡）。这条已写进
`jit_probes/README.md` 的使用约定。

**顺带露出的两个相邻缺口（都值得单独立项，本次不夹带）**

| 缺口 | 实测证据 | 影响 |
| --- | --- | --- |
| `OP_AS_CAST(94)`（`x as T` 转换） | 探针 census：`1 loop\|unsupported opcode 94(OP_AS_CAST)` | i32 字段取值**必须**用它（`c.v as int`）⇒ 含 cstruct 字段读的循环整循环被拒 |
| `call_value` 对 **native 方法对象** | 探针 6 条 bailout 全是 `[JIT-CALLOUT-FAIL] call_value: callee 是对象但 function 为空（obj_type=10）` | `T.size()/T.offset_of()/T.malloc()/T.free()` 这些"类型名上的方法调用"在 JIT 里每次执行都 bailout |

⇒ 含义：**cstruct 的"取定义"已通，但它的"用"还差两步**（`as` 转换 + native 方法派发）。
下一步按 §8.79 的顺序做 **`OP_CLIB_CALL`(142)**（它现在是循环级唯一剩下的 FFI 遮断原因：
fm / cc / gomoku 各 4 个热循环）；`OP_AS_CAST`(94) 与 native 方法派发可作为紧随其后的两项。

***

### 8.81 R6-e：`OP_CLIB_CALL` 进 JIT（FFI 动态库调用；循环级净解锁 9 个热循环）（2026-09-16）

**原状态**：`OP_CLIB_CALL`(142) 的长度**早已收录**（`opcode_size` 返回 `5 + ip[4]`，变长），
但 loop / inline 扫描都没有 case ⇒ 落 `default` 报 "unsupported opcode 142"。它是 R6-d 之后
**循环级唯一剩下的 FFI 遮断原因**（§8.80 实测：fm / cc / gomoku 各有 4 个热循环）。

**语义取证**（`op_clib_call.inc`）：字节码
`arg_count(2) ret_type_kind(1) user_arg_count(1) arg_types[user_arg_count](1 each)`；
VM 栈上是 `[lib_obj, func_name_str, user_arg1..N]`（**arg_count = user_arg_count + 2**）；
调用 `ffi_clib_call(arg_count, args, ret_type_kind, arg_types)` → 结果压栈 ⇒ 净 `-(arg_count-1)`。

**实现（4 处）**

| 位置 | 内容 |
| --- | --- |
| `jit_scan.c`（loop） | `vstack -= (arg_count - 1)`；`arg_count < 2` 视为畸形 ⇒ 拒收（交解释器）|
| `jit_scan.c`（inline） | 同上（callout 属可内联形态，与 `OP_CALL_NATIVE` 同一先例）|
| `ops_callout.inc` | `case OP_CLIB_CALL`：**操作数变长 ⇒ 把整条指令的 `ip` 交给 callout 现读**（只占 2 个寄存器参数，与 `OP_CLOSURE` 传捕获描述表指针同一先例）；其余逐行照 `OP_CALL_NATIVE` 的通用路径（`TOS_SPILL` → callout → failed 检查 → 收缩物理栈 `arg_count*8` → `VALUE_TO_RAW` → `TOS_PRODUCE`）|
| `jit_callout.c` | `jit_callout_clib_call(vstack_top, ip)`：读操作数 → **参数按序搬上 VM 栈**（call 期间是 GC 根，且满足 `ffi_call_impl` 对 str16/类型窄化转换的假设）→ 调 `ffi_clib_call` → 恢复 `sp` → `vm.has_exception` ⇒ failed ⇒ bailout |

**关键取舍（与 `jit_callout_call_native` 完全一致，两处注释都写明）**

- `ffi_clib_call` 内部**可能回调进 VM**（自动泵送 → `vm_call_value` → `vm_grow_frames`）⇒
  callout 内**不持有跨调用的指针**（`vm->sp` / `vm->stack` 全部在调用后重取）；
- 异常时不自己造报错文本：bailout 让解释器按原指令重新执行 ⇒ 行号（`GET_CURRENT_LINE()`）
  与提示文本逐字一致；
- ⚠ 已知取舍：**已发生的 FFI 副作用不回滚**（§14）⇒ bailout 后解释器会重新执行这条指令。
  触发条件只有 `vm.has_exception`（FFI 自己报错），那时解释器语义本来就要抛错终止。

**验证**

1. 探针 `jit_probes/probe_clib_call_jit.leno`（① `kernel32.Sleep(0)`：每轮一次 **void** FFI 调用；
   ② `msvcrt.abs(i-1000)`：每轮一次**有返回值**调用 + `_int()` 原生桥累加）：
   - JIT / `LENO_NO_JIT=1` **逐字一致**（`ffSleep=5001`、`ffAbs=8502500`）；
   - `Compiled: 2 / Executed: 2 / **Bailouts: 0**`（比 R6-d 那次更干净）；
   - 字节码转储核对：两个循环体里确有 `OP_CLIB_CALL args=3 ret_kind=9 user_args=1 types=[22]`
     与 `args=3 ret_kind=21 user_args=1 types=[21]`；
   - `LENO_JIT_DEBUG=1` 下 **0 条** `[JIT-CALLOUT-FAIL] clib_call`。
2. 真实应用逐对象差分（改前 `c4_*` / 改后 `c5_*` census）：

   | 应用 | loop 净解锁 | func 净解锁 | inline 净解锁 |
   | --- | --- | --- | --- |
   | file_manager | **-3**（4→1）| **-7**（32→25）| **-2**（22→20）|
   | cache_cleaner | **-3**（4→1）| **-2**（17→15）| **-2**（19→17）|
   | 五子棋 | **-3**（4→1）| **-2**（5→3）| **-1**（12→11）|

   ⇒ 合计 **loop -9 / func -11 / inline -5**，`142` 三侧清零。**但**（复用 R6-d 的教训）差分同时
   显示**有对象换到了别的原因**：每个应用各有 **1 个循环转到 `133(OP_GET_FIELD_ADDR)`**、
   内联侧 2~3 个转到 `88(OP_GET_MODULE_VAR)` ⇒ 这两个是**下一批**目标。
3. 端到端：两个应用 JIT/NO_JIT 的 `[SDL-BENCH] sum` 逐字相同（21600 / 4800）；
   `FuncCompiled` 109→114、72→73。
4. assert **308 passed / 0 failed**。

**一个新的性能现象（值得记住）：`call_value` 缺口会把"刚解锁的循环"变成"能编但立刻 bail"**

R6-e 之后 fm / cc 的 `Bailouts` 从 3/0 变成 6/3；`LENO_JIT_DEBUG=1` 分类显示**全部**
CALLOUT-FAIL（797 条）都是 `call_value: callee 是对象但 function 为空（obj_type=10）`、
**`clib_call` 0 条** ⇒ 新增 bailout 来自"以前被 142 整循环拒收、现在能编、但在
`T.native方法()` 调用处 bail"的循环（与 §8.80 记录同一根因）。
**实测这类 bailout 不构成可测损耗**：3 轮交替测 file_manager 的 `us_per_frame`
（JIT vs `LENO_NO_JIT=1`）为 6754/6756、6760/6763、6827/6883 ⇒ **持平**（首轮单测曾出现
+4.7%，被 3 轮复测判定为**噪声**，再次印证 §8.73 的"单次测量不下结论"）。
⇒ 结论：**`call_value`（native 方法对象）与 `OP_AS_CAST` 应紧跟 R6-e** —— 否则 FFI / cstruct 的
解锁在真实负载里只能部分兑现（当前是"不亏但也没赚"）。

> **踩坑提醒（自己又踩了一次）**：`LENO_JIT_DEBUG=1` 跑真实应用会输出爆量
> （§8.77 已记"8.4MB 且 60 帧跑不完"）—— 本次为查 bailout 分类而开，300 帧被 150s 超时杀掉。
> 只需分类时**用部分输出就够**，或改用轻量开关。
>
> ✅ **已解决（2026-09-17）**：根因是 JIT 把浮点 **0.0** 当成 int48 交给 FFI ⇒ Win64 浮点分发
> 报「超过上限」⇒ CLIB callout 失败 ⇒ bailout ⇒ 卡死；修在 `ffi.c` 的浮点通道判定
> （声明为浮点的形参收到整数时走 XMM），**R6-e 已恢复**，详见 **§8.99** ✓。

> ⚠ **后续（2026-09-17，必读）**：R6-e 解锁的 `OP_CLIB_CALL` 在**真实游戏**里让飞机大战
> **开火后窗口卡死** ⇒ **曾临时回滚**（`jit_scan.c` 两处 case 改回拒收），**真因已查清（§8.99）**。
> **动手改这条路径前先读 §8.98。** 本节的"797 条 CALLOUT-FAIL **不构成可测损耗**"结论
> **只在 GUI 长跑之外成立** —— 在游戏里它对应的是"函数被 JIT 跑一半 + 解释器从头重跑"，
> 那是**正确性**问题，不是性能问题。

***

### 8.82 R6-f（**已回退**）：把非闭包 callee 交 `vm_call_value` 的尝试与两个反证（2026-09-16）

**动机**：§8.80/§8.81 实测到"被解锁的循环立刻 bail"——真实应用 fm/cc 的 `Bailouts` 3/0 → 6/3，
`LENO_JIT_DEBUG=1` 下 CALLOUT-FAIL **全部**是 `call_value: callee 是对象但 function 为空
（obj_type=10 = OBJ_BOUND_METHOD）`。看起来只要把这类 callee 交给 VM 的 `vm_call_value`
（它确实支持 bound method：`vm_call.inc:272` 的 `OBJ_BOUND_METHOD` 分支同时覆盖用户 struct 方法
与 **native 绑定方法**）就能兑现收益。**动手前先验证**，结论是**不能**。

**两个反证（都在真机上，且都复现）**

1. `probe_cstruct_jit.leno` 直接**跑错**（本来 NO_JIT/JIT 逐字一致的探针）：
   `只能调用函数（不是对象类型）`（`vm_call.inc:374` 的**外层**错误 ⇒ 传进去的**不是对象**）。
   而**同一处**旧代码（失败前的诊断打印）报的却是 `obj_type=10`（一个合法对象）。
   ⇒ 这些调用点上 callee 槽的内容**不可靠**。形态是 `OP_GET_LOCAL + OP_CONST(name) + OP_INDEX + OP_CALL 0`
   （`c.free()` / `T.malloc()`），怀疑 **JIT 的 `OP_INDEX` 在「cstruct 定义 / 类型名 + 字符串键」上
   与解释器产出不同**。**在查清 `OP_INDEX` 之前，把这里的 callee 交给 VM 执行 = 拿脏值调用**
   （比"安全地 bailout"危险得多）。
2. **更严重的一条**：`assert/test_jit_closure_byupvalue.leno` 开始**算出错值**
   （第 46 行 `assert_eq(nested(2000), 2001000)` 实际得 **2006847**，JIT 确定性复现、NO_JIT 通过；
   全套回归 310/1）。注意这**不是**崩溃而是**静默算错** —— 说明这次改动破坏了别处的状态。

**结论与动作**：**整块回退**（`jit_callout_call_value` 恢复 2 参数 + `vstack_top[-1]` 读法；
codegen 去掉多传的第 3 个参数）。回退后：单跑该用例 `OK`、全套 **311 passed / 0 failed** ✓、
三个探针（cstruct / clib / multi-ret）全部 `IDENTICAL` ✓。

**留给下一步的两条硬信息**

- 想修这条路径，**先查 JIT 的 `OP_INDEX`**（cstruct 定义 / 类型名 + 字符串键）产出什么、
  与解释器差在哪；查清之前不要动这里的 callee。
- **告警（值得写进编码约定）**：`OP_CALL` 的 callout 参数装载区**不要再用 R8/JIT_ARG3**。
  Windows 下 `JIT_ARG3 = R8`（`x86_64.c:800-803`），而 R8 同时是大量发射器宏的 scratch，
  并且可能承载 **pinned TOS** —— 本次"只多读一个槽"的改动就造成了静默算错。
  在 callout 里加参数时，先把「该寄存器此刻是否被 TOS_SPILL/发射器约定占用」查清。

#### 取证更正（同日，§8.86）：上面两条结论**有一条是错的**

按"先查 `OP_INDEX`"的建议做了一次最小探针取证（`jit_probes/probe_index_callee.leno`：
一个热循环里放 `Cell.malloc()`（类型名接收者）与 `c.free()`（实例接收者）两个调用点），
用 `LENO_JIT_DEBUG=1` 只看诊断行，结论**推翻**了本节的第 1 条：

- 探针实测（`Compiled: 1 / Bailouts: 3`，3 次后循环被拉黑，结果仍正确 `n=101`）：
  **全部 3 条诊断都是** `[JIT-CALLOUT-FAIL] call_value: callee 是对象但 function 为空（obj_type=10）`
  —— 即 **callee 是一个合法的 `OBJ_BOUND_METHOD`**。**没有**任何"callee 不是对象"的记录。
- 读完 `jit_callout_index`（`jit_callout.c:255-330`）后确认：它对 **MODULE / STRUCT /
  CSTRUCT / ENUM_DEF** 等接收者都有分支，并在 cstruct 原生方法处返回
  `bound_method_new(obj_val, native_method)`（:305-309）⇒ **JIT 的 `OP_INDEX` 产出与解释器一致**。
  ⇒ 本节的"callee 槽的内容不可靠 / 怀疑 `OP_INDEX` 产出不同"是**错误推断**（当时只读了
  `jit_callout_index` 的前半段就下了结论 —— 教训：**读到函数尾部再下结论**）。

**更正后的正确结论**：真正的缺口只是「**裸 `OP_CALL` 的 callee 是 `OBJ_BOUND_METHOD` 时
`jit_callout_call_value` 不支持**」（它只认 CLOSURE / FUNCTION）。而这恰恰是
`vm_call_value` **支持**的类型（`vm_call.inc:272` 的 `OBJ_BOUND_METHOD` 分支）⇒
R6-f 的**思路是对的**，失败出在**实现细节**（最可疑的仍是 R8/JIT_ARG3 那个点，
或者 `arg_count == 0` 时 slow path 的实参/接收者排布），应当**带仪器重试**，而不是放弃。

**顺带发现一个潜在隐患（单独立项）**：`OP_INDEX` 的慢路径**不检查任何失败标志**
（`ops_index.inc:99-127` 直接 `TOS_PRODUCE`），而 `jit_callout_index` 用 **`NULL_VAL`**
同时表示"合法的 null 结果"与"未覆盖的接收者类型"。⇒ 对**没有被分支覆盖**的接收者类型
（如 `OBJ_FUNCTION` / `FILE` / `THREAD` 之类），JIT 会**静默压 null**，而解释器会报错
（例如"索引操作需要对象类型"之类）⇒ 语义分歧。已知被覆盖的类型足够常用，所以现在没暴露；
修法是给该 callout 加一个**独立的失败出参**（或返回一个哨兵），让 codegen 能区分
"null 结果"与"不支持"。**在动 ⑥ 之前应先确认这一点**。

***

### 8.83 R6-g：`OP_AS_CAST`进 JIT（`as` 安全转换；顺带修掉长度表两处混用缺陷）（2026-09-16）

**原状态**：`OP_AS_CAST`(94) 在两个扫描器里都没有 case ⇒ 落 `default` 报 "unsupported opcode 94"
⇒ **含 `as` 的热循环整循环被拒**（实测探针 census：`1 loop|unsupported opcode 94(OP_AS_CAST)`）。
它是 §8.80/§8.81 之后剩下的"小而有明确收益面"的缺口之一，且是 **cstruct / FFI 字段取值的搭档**
（`c.v as int` 是 i32 字段取值的唯一途径）。

**语义取证**（`op_as_cast.inc`）：`[type_kind:1]` + 操作数 = 名字常量(2) 或 元素类型(1)；
弹栈顶 → 按期望类型检查/转换 → **匹配（可能是转换后的值）或 null 压回**（pop 1 push 1，net 0）。
**永不报错**，但可能分配（字符串转换 / 整数转 FFI 指针）。

**实现：照 `OP_TYPE_CHECK` 的"语义唯一来源"模式（§8.65）**

| 位置 | 内容 |
| --- | --- |
| `vm.c` | **新增 `vm_as_cast(value, expected_type, elem_type, name_val)`**：把 `op_as_cast.inc` 里那一坨 TypeKind switch（`+442` 行）**整段搬过来**，只把 `READ_BYTE/READ_SHORT` 换成入参（名字常量要查 `chunk->constants`，JIT 在编译期就能查好）|
| `vminc/op_as_cast.inc` | 收缩为「读 `type_kind` → 读操作数 → `vm_as_cast` → 压结果」；**消费顺序与旧实现逐字一致** |
| `leno_vm.h` | 声明 `vm_as_cast`（与 `type_check_value` 并列，注释写明"两处共用同一份"）|
| `jit_callout.c` | `jit_callout_as_cast(...)` —— 一行转发（与 `jit_callout_type_check` 同款）|
| `ops_callout.inc` | `case OP_AS_CAST`：编译期解出 `type_kind/elem/name_val` → `TOS_CONSUME_RAX` + `RAW_TO_VALUE` → callout → `VALUE_TO_RAW` + `TOS_PRODUCE`（net 0；**不设失败通道**，本操作码不报错）|
| `jit_scan.c` | loop / inline 两个扫描器各加 `case OP_AS_CAST`（net 0）|

**顺带修掉的两处长度表缺陷（此前被"94 一律拒收"掩盖）**

长度表原来把 `OP_TYPE_CHECK` 与 `OP_AS_CAST` **合并成一条**（`STRUCT/FACE/ENUM ⇒ 4`），
但两者的名字常量集合**不同**：

| 操作码 | 带 2 字节名字常量的类型 | 来源 |
| --- | --- | --- |
| `OP_TYPE_CHECK` | `STRUCT` / `FACE` / **`ENUM`** | `op_type_check.inc:27-30` |
| `OP_AS_CAST` | `STRUCT` / `FACE` / **`CSTRUCT`**（`ENUM` **落到 default ⇒ 只读 1 字节**）| `op_as_cast.inc` 的 `case TYPE_CSTRUCT`（`READ_SHORT`）|

⇒ 混用的后果：**`AS_CAST`+`CSTRUCT` 少读 1 字节**（把它后面那个字节当成下一条指令的开始）、
**`AS_CAST`+`ENUM` 多读 1 字节**。两者此前不会暴露，只因为扫描遇到 `OP_AS_CAST` 就 `return` 了
—— 一旦支持该操作码，长度错就会让**扫描走错字节流**（`§8.83` 已拆成两条，各自的注释写明依据）。

**验证**

1. 新探针 `jit_probes/probe_as_cast_jit.leno`（三个热循环：① float→int 截断 ② int→float→int
   往返 + `(i%2==0) as int` ③ **不匹配 `"x" as int` 必须得 null**）：
   - JIT / `LENO_NO_JIT=1` **逐字一致**（`asFloatToInt=1003001` / `asRoundTrip=3002001` /
     **`asMismatch=2001`** —— 最后一条专门盯"不匹配 ⇒ null"的语义）；
   - `Compiled: 3 / Executed: 3 / **Bailouts: 0**`；
   - 字节码转储核对：循环体里确有 `OP_AS_CAST`（如 `OP_AS_CAST 2 11`）。
2. `LENO_JIT_GAPS=1` 三应用（fm/cc/gomoku）：**`94` / `AS_CAST` 条目全部消失**（改前探针口径为
   `1 loop|unsupported opcode 94`）。⚠ 按 §8.80 的教训说明：这一条只能证明"94 不再是任何对象的
   **首个**遮断原因"，不能证明它们都变成了可编译（可能换成了别的原因）。
3. **等价性最强的一条**：`vm_as_cast` 是解释器与 JIT 的**同一份**实现（抽取式重构），
   加上 assert **311 passed / 0 failed**（含全部 `as` 相关用例），语义分歧只可能来自
   "操作数解码"这一处，而它已按上表逐条对照 `READ_*` 核对过。

***

### 8.84 R6-h：`OP_GET_FIELD_ADDR`进 JIT（`&c.field` 取字段地址）（2026-09-16）

**原状态**：`OP_GET_FIELD_ADDR`(133) 长度早已登记（2 字节 = opcode + `field_idx(1)`），但没有 case
⇒ 落 `default` 报 "unsupported opcode 133" ⇒ 含 `&c.field` 的循环 / 函数 / 被调方全被拒。
它是 §8.81 的 census 差分**新露出**的缺口，且**三个应用完全一致**（R6-e/§8.83 两轮都显示
fm / cc / gomoku 各有 1 个热循环卡在它上面）。

**语义取证**（`op_struct.inc:866-925`）：弹 obj → **必须是 cstruct**（否则报"& 取地址仅支持
cstruct 字段（实际类型: %s）"）→ 索引越界检查（报"cstruct 'X' 字段索引越界…"）→
构造**非拥有**（`owned=0`）的 `ObjFFIPointer`，指向 `obj->data + field->offset`、
`size = field->size`、`element_type = field->type` → 压回（pop 1 push 1，**net 0**）。

**实现：指针构造抽成"语义唯一来源"（与 §8.83 的 `vm_as_cast` 同一手法）**

| 位置 | 内容 |
| --- | --- |
| `object_cstruct.c` | **新增 `cstruct_field_addr_new(obj, field_idx)`**：把那段构造（10 行）从 `op_struct.inc` 搬过来；分配失败返回 NULL，由调用方各自报错 |
| `vminc/op_struct.inc` | 保留类型/越界校验与两条报错文本（**逐字不变**），构造改为调新 helper |
| `leno_value.h` | 声明 `cstruct_field_addr_new`（与 `cstruct_get_field_value` 并列）|
| `jit_callout.c` | `jit_callout_get_field_addr(obj_val, field_idx)`：**同一套校验**（非 cstruct / 无 def 或 data / 越界 / 分配失败 → `failed`）→ 调同一 helper |
| `ops_callout.inc` | `case OP_GET_FIELD_ADDR`：`TOS_CONSUME_RAX` + `RAW_TO_VALUE` → callout → `failed` 检查 → `VALUE_TO_RAW` + `TOS_PRODUCE`（net 0）|
| `jit_scan.c` | loop / inline 各加 `case OP_GET_FIELD_ADDR`（net 0）|

错误路径一律 `failed` → bailout → 解释器重放本条指令报原文 ⇒ 报错文本与行号零改动
（与 `jit_callout_get_field` 对 cstruct 的处理同一取舍）。

**验证**

1. 新探针 `jit_probes/probe_field_addr_jit.leno`：
   - 热循环里每轮 `&c.v`（`OP_GET_FIELD_ADDR`），JIT / `LENO_NO_JIT=1` **逐字一致**
     （`addrNonNull=2001`）；
   - **语义侧**（只比"非 null"不够 —— 指针算错也非 null）：`ffi.write_int(&c.v, 0, 7)` 之后
     `c.v == 7`（`fieldAfterWrite=7`），证明确实指向那个字段；
   - `Compiled: 1 / Executed: 1 / **Bailouts: 0**`；字节码转储核对循环体里确有
     `OP_GET_FIELD_ADDR`（offset 108/166）。
2. `LENO_JIT_GAPS=1` 三应用：**`133` 条目全部消失**，且条目总数**净减**
   （fm 8→6、cc 7→6、gomoku 5→4 ⇒ **1 个 func + 3 个 loop** 解除遮断，**没有**"换个原因"的转移）
   —— 与 §8.83 相比，这次 census 差分给出了干净的净解锁读数。
3. assert **311 passed / 0 failed**。

***

### 8.85 R6-i：`OP_DICT`进 JIT（字典字面量；解锁 2 个函数）（2026-09-16）

**原状态**：`OP_DICT`(69) 长度早已登记（3 字节 = opcode + `count(2)`），但没有 case
⇒ 落 `default` 报 "unsupported opcode 69"。§8.84 之后的 census 显示 **fm / cc 各有 1 个函数**
卡在它上面（`func|unsupported opcode 69`）、**loop 级 0 个** —— 合理：字典字面量每轮都要分配，
不会出现在热循环里；但含字典字面量的函数进不了函数级 JIT。

**语义取证**（`op_dict.inc:5-33`）：`dict_new(max(count,8))` → 从栈顶**逆序**取 `count` 组
「键、值」（每组**先弹 value 再弹 key**）→ 按**正序** `dict_set` 写入 → 压回字典。
栈效应：弹 `2*count` 压 1 ⇒ **net `1 - 2*count`**（`count=0` 是"空字典字面量"⇒ **net +1**）。
codegen 侧（`codegen_expr.c` 的 `AST_DICT`）按 `key_i, value_i` 顺序逐个压栈
⇒ `vstack_top[0]` 是最后一组的 value，**从 `vstack_top[0]` 递增读 = 从栈顶往下**，
与解释器的 `vm_stack_pop` 顺序逐字一致。

**实现（4 处，全部照 `OP_ARRAY` 抄 —— 二者完全同构，只差两倍槽位）**

| 位置 | 内容 |
| --- | --- |
| `jit_callout.c` | `jit_callout_dict_new(vstack_top, count)`：`dict_new` → 逆序收集到两个临时数组（**必须先收集再正序插入**：重复键的覆盖次序才与解释器一致）→ 共享的 `dict_set` 逐对插入（**写屏障在它里面**，语义唯一来源）→ 返回 dict |
| `ops_callout.inc` | `case OP_DICT`：照 `OP_ARRAY` 的结构（`TOS_SPILL` → callout → failed 检查 → 弹 `2*count*8` 字节 → `VALUE_TO_RAW` + `TOS_PRODUCE` → `vstack -= (2*count-1)`）|
| `jit_scan.c` | loop / inline 各加 case（`vstack -= (2*cnt - 1)`）—— inline 侧此前连 case 都没有 ⇒ 顺带解锁"造字典的被调方"|
| `jit_priv.h` | 声明 + 契约注释（栈映射 / 覆盖次序 / 失败通道）|

**失败通道**（与 `as`、`cstruct def` 那两条不同，这条**有**失败）：`dict_new` / 临时数组
分配失败 → `failed`（交解释器报"内存分配失败"）；`dict_set` 若置了 `vm.has_exception`
（如不可哈希的键）也一并 bailout —— JIT 不吞异常，与 `jit_callout_call_native` 同款
（此时字典刚建好、无外部可见副作用）。

**验证**

1. 新探针 `jit_probes/probe_dict_jit.leno`，覆盖两种形态：
   - ① `count=2`（键常量 + 值是循环变量）：`{"a": 1, "b": i}`；
   - ② **`count=0`**（未初始化字典声明 ⇒ codegen 发 `OP_DICT 0`，net **+1**）+ 后续 `DICT_SET`；
   - JIT / `LENO_NO_JIT=1` **逐字一致**（`dictPairs=2003001`、`dictEmpty=2001000`，
     与手算 `(n+1) + n(n+1)/2` / `n(n+1)/2` 一致）；
   - `Compiled: 2 / Executed: 2 / **Bailouts: 0**`；
   - 字节码转储核对：`OP_DICT 2` 与 `OP_DICT 0` 都在循环体里（两种形态都真被测到）。
2. `LENO_JIT_GAPS=1` 三应用：**`69` 条目全部消失**，fm / cc 条目数 **6→5**（各解锁 1 个函数）、
   gomoku 不变（本来就没有）—— 干净的净解锁，无"换原因"。
3. assert **311 passed / 0 failed**。

***

### 8.86 `OP_INDEX` 慢路径的错误通道：现状测量、一次尝试（已回退）与**正确修法**（2026-09-17）

**触发探针** `jit_probes/probe_index_error_channel.leno`：热循环里做**字符串索引越界**
（走 `jit_callout_index` 的 STRING 分支 `error_add_at("字符串索引越界")`，**不是**数组越界
——那条本来就有专门的 bailout 桩），外面套 `try/catch` 计数；另有一个正常索引的对照循环。

**现状基线（回退后实测，`n=60`）**

| | stdout | exit | stderr |
| --- | --- | --- | --- |
| JIT | `caught=50 ok=61` | **−1** ✗ | `发现 11 个错误（字符串索引越界，已合并重复 11 次）` |
| `LENO_NO_JIT=1` | `caught=61 ok=61` | 0 ✓ | 干净 |

⇒ **两个独立缺陷**（都在 JIT 侧）：
1. 有 **11 次**错误**没有作为可捕获异常抛出**（`caught` 少 11）⇒ `try/catch` 语义分歧；
2. 这些错误进了**全局错误收集器** ⇒ **即使 Leno 层 catch 成功，进程仍以 `发现 N 个错误`
   + **exit = -1** 结束** —— 这一条比 ① 更严重：**正常程序会"看起来失败"**（CI/脚本会误判）。

**根因**：`jit_callout_index` 的错误路径用 `error_add_at`（**编译期/解释器的报错通道**）+ 返回
`NULL_VAL`，而 `OP_INDEX` 的慢路径（`ops_index.inc`）**不检查任何失败标志** ⇒ 两者叠加：
错误既没被抛出（被 `NULL_VAL` 吞掉）又被永久记进收集器。

**尝试过的修法（已回退，记为反例）**：把 `jit_callout_index` 包一层，比较调用前后的
`errors.count`，**本次新增了错误**就置 `jit_callout_failed`，codegen 检查后 bailout。
实测：`caught` 只从 50 → **51**（几乎没变）、`exit = -1` **依旧** ⇒ **无效**。
原因很直白：**错误已经被 `error_add_at` 记进收集器了**，再 bailout 只是"记完再退出"，
收集器里的记录不会消失 ⇒ 治不了根。

**正确修法（已定位，待做）**：`jit_callout_index` 的 ~20 条错误路径**不要调 `error_add_at`**，
**只置 `jit_callout_failed`**（`return` 一个哨兵值），codegen 侧加失败检查
（形状已在本次尝试中写好过：`EMIT_STORE_TMP` → `mov r8,&jit_callout_failed` →
`test` → `EMIT_BAILOUT_SITE_NONOVF` + `jnz` + `patch_add(-1,0)` → 清标志 → 恢复结果）。
这样才与项目既有约定一致（**报错文本与抛异常都交回解释器**，同 `jit_callout_call_native` /
`jit_callout_get_field`）：不污染收集器 ✓，bailout 后解释器重放本条指令 ⇒ 每次都是
可捕获的**原文**错误 ⇒ 预期 `caught=61`、`exit=0`。

**风险与注意**：① 错误迭代会触发 bailout ⇒ 该循环可能被拉黑（性能权衡，正确性优先）；
② 该 callout 里哪些 `error_add_at` 是"给解释器用"的、哪些是 JIT 路径可达的，要逐条过一遍；
③ **应当顺手扫一遍其它 callout 是否也有同样的"`error_add_at` + `NULL_VAL` + 调用方不检查"
组合**（这是同一类隐患）。

#### 修复完成（2026-09-17，同日）

按上面的"正确修法"落地：

| 位置 | 改动 |
| --- | --- |
| `jit_callout_index` | **20 处错误路径全部去掉 `error_add_at`，改为只置 `jit_callout_failed`**（含末尾兜底：接收者类型未被任何分支覆盖，也置 failed ⇒ 解释器会抛 `不支持的索引类型: '%s' 不支持索引访问`，对齐 `op_utils.inc` 的 `default`）|
| `ops_index.inc`（OP_INDEX 慢路径） | callout 之后加失败检查：结果先存 `tmp3` → `mov r8,&jit_callout_failed` → `test` → `EMIT_BAILOUT_SITE_NONOVF` + `jnz` + `patch_add(-1,0)` → 清标志 → 恢复结果。注释写明**不能用"结果是 NULL_VAL"判断失败**（`d["missing"]` 的 null 是合法结果）|

**前后实测对比**（`probe_index_error_channel.leno`，`n=60`）

| | 修复前 | 修复后 |
| --- | --- | --- |
| JIT stdout | `caught=50 ok=61` | **`caught=61 ok=61`** ✓ |
| JIT exit | **−1** ✗ | **0** ✓ |
| JIT stderr | `发现 11 个错误（字符串索引越界）` ✗ | 只有 JIT stats ✓ |
| JIT `Bailouts` | 0（静默吞） | **3**（该循环 3 次后被拉黑 ⇒ 其余走解释器 ⇒ 每次错误都被捕获，正是设计意图）|
| NO_JIT | `caught=61 / exit=0` | 不变 ✓ |

真实应用回归：fm `sum=21600`、cc `sum=4800`（JIT/NO_JIT 一致）、`Bailouts` 6/3（与修复前相同 ⇒ 无回归）、**无错误汇总、exit=0** ✓；assert **311/0** ✓。

**同类隐患的系统性扫描（待做，已量化）**：`grep -n 'error_add_at(ERR_RUNTIME' src/jit/jit_callout.c`
**还剩 36 处**（分布在 `jit_callout_index_set`、`jit_callout_div`、`jit_callout_get_method`、
`jit_callout_acc_fields`、`jit_callout_module_call`、`jit_callout_get_property` 等）。审计判据两条：
1. 该 callout 的错误路径是否**也置了** `jit_callout_failed`（只 `error_add_at` 不置 failed
   ⇒ 就是本次修掉的那种"静默吞 + 污染收集器"✗）；
2. 对应 codegen 站点是否**检查并清除**该标志（不检查 ⇒ 标志会残留、污染后续 callout 的判断 ✗✗）。
⚠ 不能一刀切：`test_jit_int_div.leno`（热循环内除零必须可捕获）**已经通过** ⇒ 说明部分站点
（如除零）是**正确配对**的，要逐条核对。

***

### 8.87 R6-j：bound-method callee 的第二次尝试（也回退）—— 把"两个症状"干净分开（2026-09-17）

**动机**：§8.86 修完 `OP_INDEX` 的错误通道后，`OP_CALL` 的 bound-method 缺口成了"**已解锁但
每次执行都 bail**"的唯一来源（fm/cc 的 `Bailouts` 3/6、797 条 CALLOUT-FAIL 全是它）。
而 §8.82 的结论是"思路对、失败在实现细节"，所以做一次**隔离实验**再试。

**实验设计（关键：只改一处）**：只改 `jit_callout_call_value` 的 `!fn` 分支 ——
交 `jit_invoke_closure(NULL, callee, arg_count, vstack_top, 1, "call_value(non-fn)")`
（传 NULL ⇒ 跳过函数级快路径、直接走 VM 重入；`vm_call_value` 本来就支持 `OBJ_BOUND_METHOD`）。
**完全不碰 codegen、不碰 `JIT_ARG3`/R8** —— 与 R6-f 唯一的差异就是这个。

**对照结果（决定性）**

| 症状 | R6-f（含 codegen ARG3=R8 改动） | **R6-j（只改 callout）** | 结论 |
| --- | --- | --- | --- |
| `assert/test_jit_closure_byupvalue.leno` 静默算错（2001000→2006847） | ✗ 出现 | **✓ 通过** | ⇒ 那个症状**就是 R8/ARG3**（§8.82 的归因成立）|
| `probe_cstruct_jit.leno` 报 `只能调用函数（不是对象类型）` | ✗ 出现 | ✗ **仍然出现** | ⇒ 与 R8 **无关**，是**独立原因** |
| `probe_index_callee.leno` | — | ✗ `n=50` + `发现 1 个错误` | 同上 |
| assert 全套 | 310/1 ✗ | **311/0** ✓ | 症状隔离成功 |

⇒ **同一个"bound-method callee 不能交 VM"的表象背后有两个独立原因**：

1. **闭包用例算错 = codegen 里用 R8 传 callee**（`JIT_ARG3 = R8`，而 R8 是发射器 scratch /
   可能承载 pinned TOS）⇒ §8.82 的告警**成立**，以后加 callout 参数**不要用 ARG3/R8**。
2. **"不是对象" = 这些站点的 `vstack_top[-1]` 确实不是 callee 槽** ✗ —— 与 R8 无关。
   ⚠ §8.82 关于这一条的归因（"callee 槽内容不可靠 / 怀疑 OP_INDEX 产出不同"）**当时猜对了**，
   但 §8.86 的取证只看了 `probe_index_callee` 那**一类**站点（那里 callee 是合法 bound method）
   就把它否掉了 —— **教训：同一现象在不同站点可能不同因，单点观测不足以否定它。**
   现在的怀疑点：`OP_INDEX` 慢路径 pop 掉 2 个原始槽后 `TOS_PRODUCE()` 可能只把结果 pin 在
   寄存器、没有补回物理栈 ⇒ 之后的 `rsp` 相对寻址（`vstack_top = rsp + 8`）整体偏移
   （这也解释了为什么 `c.free()` 那类站点是对的、`T.malloc()` 这类不对 —— 走的路径不同）。

**动作**：**再次整块回退**。回退后三探针 `IDENTICAL`（`n=101` / `defLoop=20001` / `ffAbs=8502500`）、
闭包用例 `OK`、assert **311/0**，与 R6-j 之前逐字一致 ⇒ 现状仍是"**能编但每次执行 bail
（3 次后被拉黑）、正确但慢**"（fm/cc `Bailouts` 6/3）。

**下一步取证计划（必须先做，带仪器、按站点分类）**

不要再靠单点观测下结论（前两次都栽在这里）。要一起打印三样东西做对照：
`vstack_top`、`*(int64_t*)(vstack_top - 8)`（实际读到的 callee raw）、以及**期望的 callee 值**
（在 `OP_INDEX` 的 callout 返回时把它记到一个旁路变量里）。分类维度：
`T.method()`（OP_GET_CSTRUCT_DEF/类型名接收者）vs `c.method()`（实例接收者）vs 局部闭包。
若确认是 `OP_INDEX` 慢路径的物理栈记账问题，修法应在**那条指令**（把结果真正补回栈、
或让 `TOS_PRODUCE` 与快路径一致），而不是在 `OP_CALL` 侧绕开 —— 这也解释了为什么
"把 callee 显式传参"这种绕法会踩到别的坑（R8）。

***

### 8.88 R6-k：native 绑定方法（`T.malloc()` / `c.free()`）进 JIT —— 三次归因的取证 + 同条件 A/B **1.79x**（2026-09-17）

**目标（⑥ 的正路）**：§8.80 露出的两个相邻缺口之一 —— `call_value` 对 **native 方法对象**
（`T.malloc()` / `T.size()` / `T.offset_of()` / `c.free()`，callee 是
`OBJ_BOUND_METHOD` 且 `closure == NULL`）不支持 ⇒ 含这类调用的热循环**能编但每次执行都
bail**（3 次后被拉黑 ⇒ 退化为解释执行）：§8.80/§8.85 实测 fm/cc `Bailouts` 6/3、
`LENO_JIT_DEBUG=1` 下 **797 条 CALLOUT-FAIL 全是这一条**。

#### 三次尝试：前两次的**归因都是错的**（本节最重要的部分）

| 尝试 | 改动 | 症状 | 当时的归因 | 实际根因 |
| --- | --- | --- | --- | --- |
| **R6-f**（§8.82）| codegen 把 callee 当第 3 个参数（`JIT_ARG3 = R8`）+ callout 交 VM | ① `test_jit_closure_byupvalue` 静默算错 ② `只能调用函数（不是对象类型）` | "思路对，失败在实现细节" | ① 是 **R8 被当 scratch 踩** ✓ ② 与 R8 **无关** ✗ |
| **R6-j**（§8.87）| **只**改 callout 一支（隔离实验）| ① 闭包用例 ✓ 通过 ② 探针返回**陈值**（`NULL_VAL` / int48 50）| "`vstack_top[-1]` 不是 callee 槽" ✗ | 差得远：**native 类 callee 经 `vm_call_value` 调用本身就不成立** ✗ |
| **R6-k**（本节）| native 类 callee **绕过 `vm_call_value`、原地直调** | 三探针 `IDENTICAL`、`Bailouts` 全 0 ✓ | — | ✓ |

#### 取证过程（可复现的四步仪器法）

1. **站点诊断**（回退状态，`LENO_JIT_DEBUG=1`）：这些站点 `不是对象 0 条 / obj_type=10 6 条`
   ⇒ **推翻** R6-j 的"callee 槽不是 callee"假设（对该类站点不成立）。
2. **委派前后打印**：`callee = bound name=malloc closure=NULL method=…` ✓ 是合法对象；
   但**委派返回** `raw=NULL_VAL`（探针 A）/ `int48 50`（探针 B）✗。
3. **试修 `last_return_value`**（在 `vm.c` 的 `vm_call_value` 里发布 native 结果）⇒ **无效** ✗
   ⇒ 说明"数据没到那儿"，而不是"取值位置不对"。
4. **双向仪器**（决定性）：
   `[R6K-VM] publish=0xfffc019fda2c9350 last_return_value=0xfffc019fda2c9350`（发布成功 ✓）
   → `[R6K-JIT] raw=0xfff8000000000000 last_return_value=0xfff8000000000000`（**被冲掉** ✗）
   ⇒ 锁定**被调用的那个函数本身** ✗。

#### 根因：`vm_call_value` 是「脚本调用」入口，对 native callee 不成立

`src/vm/vm.c:1021`：
```c
if (!call_value(callee, arg_count, line)) return 0;
vm.stop_frame_cnt = saved_frame_cnt;
int r = vm_run_with_vm(vm_ptr);   /* ← 进解释器循环，跑到"新帧"返回 */
```
`call_value` 的 native 分支（`vm_call.inc:268-270` / `328-330`）是
`vm.sp -= arg_count + 1; vm_stack_push(&vm, result); return 1;` —— **同步执行完、不压帧**
⇒ 随后的解释器循环**继续执行调用方的字节码** ✗（此时栈上还多着一个结果槽）。三条实测后果：

| 后果 | 实测 |
| --- | --- |
| 刚算出的结果被冲成 `NULL_VAL` | `Cell.malloc()` 返回 null ⇒ `c.v = …` 报"在非对象类型上设置字段（实际类型: null）" |
| 热循环局部量被写坏 | `probe_index_callee` 的 acc 只加到 **50**（应 101）|
| 执行到栈布局不符的 `OP_CALL` | `只能调用函数（不是对象类型）`（`vm_call.inc:374`）|

#### 修法（最小且语义对齐解释器）

在 `jit_invoke_closure` 的慢路径**之前**加 native 分支：`OBJ_NATIVE` /
`OBJ_BOUND_METHOD` 且 `closure == NULL` ⇒ **复用 `jit_callout_call_native` 的机械原地直调**
（实参搬上 VM 栈当 GC 根 → `native->function(total, args)` → 恢复 sp）；
native 绑定方法把**接收者插到 `args[0]`**（与 `call_value` 的 bound-native 分支同序）；
`vm.has_exception` → `failed` → bailout（与 `jit_callout_call_native` 既有取舍一致）。

**仍走 `vm_call_value` 的三类**（它们确实压帧、结果由帧返回写 `last_return_value` ✓）：
闭包、裸 `ObjFunction`、**用户 struct 方法的绑定闭包**（`bound->closure != NULL`）。

#### 验证

1. 三个探针 JIT / `LENO_NO_JIT=1` **逐字一致** ✓ 且 `Bailouts` 全 **0**：
   `probe_index_callee` `n=101`（3→**0**）；`probe_cstruct_jit` `defLoop=20001 / loopDef=20001`
   （6→**0**）；`probe_clib_call_jit`（0，未回归）。
2. `assert/test_jit_closure_byupvalue.leno` **OK** ✓（R6-f 会算错的那个）。
3. assert **311 passed / 0 failed**。
4. 真实应用 headless 300 帧：JIT/NOJIT 的 `sum` **逐字一致**
   （fm 21600 / cc 4800 / 五子棋 1500）；`FuncCompiled` 114→**120**（fm）、73→**77**（cc）；
   `Bailouts` 6→**3**（fm）、3→**0**（cc）。
5. **同条件 A/B**（fm，300 帧，3 轮交替：R6-k 与改前源码各自编译的二进制）：

   | 轮 | R6-k | 改前 | 比值 |
   | --- | --- | --- | --- |
   | 1 | 3823 | 6794 | 1.78x |
   | 2 | 3804 | 6789 | 1.78x |
   | 3 | 3788 | 6830 | 1.80x |

   波动 **±0.5%** ⇒ **1.79x**。注意改前的 **6804 ≈ NOJIT 的 6679** ⇒ 那些热循环原本
   被 bailout **拉黑、退化成解释执行**（这也是"JIT 打开却几乎不提速"的成因）。

#### 顺带留下的事项

- ~~**`vm_call_value` 对 native callee 的陷阱仍在**（本节只在 JIT 侧绕开）：`arrays.c:341`
  （`arr.map` 回调）与 `ffi.c:2607`（FFI 回调）同样按"成功 ⇒ `last_return_value` 有效"
  取结果 ⇒ 若传 native 值会拿到陈值**并**让解释器多跑一段字节码。~~ → **已修（§8.90）**：
  在 `vm_call_value` 里对 native 类 callee **同步直调 + 发布单返回 + 跳过 `vm_run_with_vm`**，
  一处修掉 `arrays.c` / `ffi.c` / JIT 三个调用方；新探针 `probe_vm_call_value_native.leno`
  用 A/B 证明它在**解释器里也是活的 bug**（改前：6 个运行时错误）。
- ~~fm 仍有 **3** 条 bailout（来自别的站点）：用 `LENO_JIT_DEBUG=1` 定位~~ → **已定位（§8.89）**：
  是内层 **float 步长** for 循环的序言检查（`OP_FOR_PREP`），属**设计限制**、bail 正确；
  并因此补了"bailout 触发指令名"的**常驻诊断**（零成本，替代跑不完的 `LENO_JIT_DEBUG`）。

#### 方法论教训（已同步进 `jit_probes/README.md`）

1. **只改一处做隔离实验**：R6-j 把"R8 改动"从"callout 改动"里分离出来，才确认闭包算错
   只由 R8 引起。
2. **单点观测不足以否定一个假设**：§8.86 只看 `probe_index_callee` 一类站点就否掉了
   "callee 槽不可靠" —— 该结论对**别的**站点是错的（真正的错还更底层）。
3. **修共享机械而无效时，先证明"数据有没有到达"**（双向仪器），再改取值位置。
4. **`native 不压帧`** 是本次所有怪象的总开关：凡把 native callee 送进"假定会压帧"的
   路径（`vm_call_value`）都会静默错，不是崩溃 —— 所以必须用**对拍 + 探针**兜住。

***

### 8.89 常驻诊断：bailout 的**触发指令名**（+ fm 剩余 3 条 bailout 定位为设计限制）（2026-09-17）

**动机（一次自找的教训）**：定位 fm 剩余 3 条 bailout 时，我又用 `LENO_JIT_DEBUG=1` 跑真实应用
—— 产出 8MB+ stderr、**10 分钟没跑完被中止**。这条在 §8.71 就记过，但当时**只记了"不能这么做"，
没有给替代工具** ⇒ 于是又栽一次。本次补上替代：**零成本**的 `stats` 诊断
（"是哪条指令 bail 的"这件事，不该需要 debug 洪水才能回答）。

**实现**（`jit.h` + `jit.c`，约 25 行）

| 位置 | 内容 |
| --- | --- |
| `JitCacheEntry`（jit.h） | 新增 `const char* last_bailout_op;` |
| bailout 记录处（jit.c） | 用与 `jit_bailout_reason` **同一套编码**还原偏移（非溢出类 `rel = -1000 - site`、溢出/截断类 `rel = site`；两者都相对循环体起点，内联帧先剥 `0x10000 * depth` 基址），再从 `chunk->code[loop_bc_off + rel]` 取 `opcode_name()`（静态字符串）；边界检查 `abs_off ∈ [0, chunk->len)`，取不到打 `?` |
| `jit_print_stats`（jit.c） | `Bailout: fn=… — <原因> \| 触发指令=OP_xxx` |

**它立刻给出了答案**（fm，300 帧，无需 debug）

```
Bailout: fn='render' loop_bc=414 x3 — 非溢出类 @bc_off=787（= loop_bc 414 + 373） | 触发指令=OP_FOR_PREP
```

⇒ **不是 callout 失败**，而是**内层 for 循环的序言检查**：`ops_loop.inc:31-40` 的
`BT RBX, step_si` —— **step 的静态类型是 float** ⇒ JIT 的 `FOR_LOOP` 只走 int48 快路径，
处理不了 double 位模式的方向/比较 ⇒ **按设计交回解释器**（bail 是**正确行为**）。
唯一后果：外层 `render` 热循环因此被 `JIT_BAILOUT_LIMIT`(3) 拉黑、退化为解释执行
（fm 的 `us_per_frame` 3796，仍是 NOJIT 6679 的 **1.76x**）。

**裁决：不修**（float 步长循环进 JIT 是独立特性：方向/比较要改走 SSE）。但它现在**可见**了：
stats 里 `Bailouts` 连同**触发指令名**一起输出；`LENO_JIT_GAPS`（scan 期口径）看不到它
—— 两者互补：**scan 期拒收**看 `LENO_JIT_GAPS`，**运行期 bailout** 看 stats 的 `Bailout:` 行。

***

### 8.90 修掉 `vm_call_value` 的 native callee 陷阱（一处修所有调用方；附 A/B + 新探针）（2026-09-17）

**问题（§8.88 遗留项①）**：`vm_call_value`（`vm.c`）是「**脚本**调用」入口 —— `call_value`
之后进 `vm_run_with_vm` 跑到"新帧"返回。但 **native 类 callee 不压帧**
（`call_value` 同步执行完、把结果 push 回栈、**从不写 `vm.last_return_value`**）⇒ 那个解释器
循环**继续执行调用方帧的字节码**（栈上还多着一个结果槽）⇒ 静默错。JIT 侧（§8.88 / R6-k）
只是**绕开**了它；另两个调用方仍按"成功 ⇒ `last_return_value` 有效"取结果：
`arrays.c:341`（`arr.map` / `filter` 的脚本回调）、`ffi.c:2607`（FFI 回调派发）。

**它是活 bug，不是理论隐患（A/B 实测）**：新探针 `jit_probes/probe_vm_call_value_native.leno`
用**原生函数值**当回调（`nums.map(print)` / `nums.filter(print)`）：

| | 改前（无修复） | 修复后 |
| --- | --- | --- |
| `LENO_NO_JIT=1` | `probe:24 可空值（null）不能访问属性或方法`、`函数未定义`、`加法运算: null 不能参与运算`、**发现 6 个错误** ✗ | 干净 ✓ `after_len=3` / `total=6` |
| JIT | 同样 6 个错误 ✗ | 同上 ✓ |

⇒ **传原生函数值当回调在解释器里也是坏的**（不只 JIT ✗）。

**修法（`vm.c` 的 `vm_call_value`，一处修所有调用方）**

```c
if (call_callee_is_native_like(callee)) {      /* OBJ_NATIVE / bound method(closure==NULL) */
    int base = saved_sp - arg_count - 1;       /* call_value 把 arg_count+1 槽换成 1 个结果 */
    Value res = (base >= 0 && vm_ptr->sp > base) ? vm_ptr->stack[base] : val_null();
    vm_ptr->last_return_value     = res;       /* ① 发布成单返回（native 恒 1 个返回值） */
    vm_ptr->last_return_values[0] = res;
    vm_ptr->last_return_count     = 1;
    vm_ptr->stop_frame_cnt        = saved_stop_frame_cnt;
    return 1;                                  /* ② **跳过** vm_run_with_vm */
}
```

① 让「成功 ⇒ `last_return_value` 有效」这条契约对**所有** callee 类别成立；
② **跳过**解释器循环 —— native 调用不产生帧，没有属于它的东西要跑（native 内部若回调脚本，
那些嵌套调用由各自的 `vm_call_value` 收尾）。
⚠ **上一次尝试只做了 ① 没做 ②** ⇒ 结果被 `vm_run_with_vm` 的收尾冲成 `NULL_VAL`
（§8.88 的双向仪器）：

```
[R6K-VM]  publish=0xfffc019fda2c9350 last_return_value=0xfffc019fda2c9350   ← 发布了 ✓
[R6K-JIT] raw=0xfff8000000000000     last_return_value=0xfff8000000000000   ← 又被冲掉 ✗
```

⇒ **教训：修共享机械时，必须同时检查"之后谁会覆盖它"**（发布成功 ≠ 最终有效）。

**JIT 侧的 native 直调保留**（§8.88 的分支）：它比走 `vm_call_value` 少一次栈搬运、已在探针上
验证；两侧语义一致（接收者插 `args[0]` + 同步直调 + `has_exception` → failed → bailout）。

**验证**：四个探针 JIT/`LENO_NO_JIT=1` **逐字一致**（新探针 `before=3 / after_len=3 / total=6 /
keep_len=0`）、assert **311 passed / 0 failed**。

***

### 8.91 错误通道统一（36 处 `error_add_at` → 只置 failed）+ 补上 `OP_DIV` 漏掉的守卫（2026-09-17）

**背景**：§8.86 只修了 `OP_INDEX` 一处"在 JIT 里写全局错误收集器"✗。审计 `src/jit/`：
`ops_index.inc` 的 3 处命中**全是注释** ✓；真正的调用点在 **`jit_callout.c` 36 处** ✗
（除零 / append / 索引赋值 / struct / cstruct / 模块调用 / 属性 …）。

#### 第 1 步：统一通道（一次性文本替换，不会再漏一处）

新增 `jit_callout_error(msg)` —— **只做一件事：置 `jit_callout_failed = 1`**
（原文只在 `LENO_JIT_DEBUG` 下打到 stderr，便于定位是哪条 callout 失败）；然后把
`error_add_at(ERR_RUNTIME, 0, 0, X)` 用一次 `replace_all` 全部换成 `jit_callout_error(X)`
（36 处 ✓，剩余命中全是注释 ✓）。之后按既有约定：codegen 的 `failed` 检查 → bailout →
**解释器重放本条指令** ⇒ 抛与 `LENO_NO_JIT=1` 一致、**可被 try/catch 捕获**的原文错误 ✓。

#### 第 2 步：新探针抓出"光置 failed 还不够"✗（本节最重要的教训）

`probe_error_channel_div.leno`：让操作数**静态类型未知**（`any`）⇒ 走**通用 `OP_DIV` callout**，
而不是 JIT 内联的 `OP_DIV_INT`（那条自带零检查 bailout 桩，本来就是对的 ✓）。

| | 改前（`error_add_at`）| 只做第 1 步（统一通道）| 第 1+2 步（补守卫）|
| --- | --- | --- | --- |
| JIT | `ok=250 caught=50`，**exit=-1** + `发现 250 个错误` ✗ | `ok=250 caught=50`，exit=0 ✗（**彻底静默**）| **`ok=0 caught=300`**，exit=0 ✓ |
| NO_JIT | `ok=0 caught=300`，exit=0 | 同左 | 同左 ✓ |

- "改前 exit=-1" = §8.86 的既有 bug 形态（250 次除零被**永久**记进全局表 ✓）；
- "只做第 1 步"却**更糟**：`jit_callout_div` 置了 `failed`，但 `case OP_DIV`
  （`ops_float.inc:77-96`）**没有检查它** ⇒ JIT 把 NULL 当结果继续跑（`Bailouts: 0` ✗），
  错误**彻底静默** ✗；
- ⇒ **教训：`failed` 只是"信号"，必须有"检查者"**。改造 callout 的错误通道时，
  必须同时核对该 callout 在 **codegen 侧的调用点**是否含 `failed` 守卫。

#### 第 2 步的修法

给 `case OP_DIV` 补上与 `OP_INDEX` / `OP_CALL` 同款的守卫块：
`EMIT_STORE_TMP(tmp2)` → 读 `&jit_callout_failed` → `test` → `EMIT_BAILOUT_SITE_NONOVF(bc_off)`
跳 bailout → 清标志 → 恢复 RAX → `EMIT_VALUE_TO_RAW / TOS_PRODUCE`。

#### 第 3 步：把 17 处"无守卫"逐条核完 —— 又抓出 **3 处真漏**（并确认 13 处无害）

用"调用点 vs 守卫点行号配对"先定位 17 处可疑 ✗，再**逐条读代码**核实
（⚠ 配对法会**高报**：守卫常常放在 `if/else` 的**合并点之后**、两个调用点共用；
也可能用**返回值**当守卫 ⇒ 不能靠计数下结论）：

| 站点 | callout | 判定 |
| --- | --- | --- |
| `ops_index.inc` `OP_ARRAY_APPEND_NOPUSH` 慢路径 | `array_append` | ✗ **补守卫**（失败路径在变异之前 ⇒ 重放无副作用）|
| `ops_index.inc` `OP_DICT_SET` | `dict_set` | ✗ **补守卫**（失败时会把 NULL_VAL 当字典压栈 ⇒ 后续在 null 上取索引）|
| `ops_index.inc` `OP_INDEX_SET_NOPUSH` 慢路径 | `index_set` | ✗ **补守卫**（新探针实测：JIT `caught=50` ✗ → 补后 `caught=300` ✓）|
| `ops_callout.inc` `OP_GET_FIELD_FAST` 慢路径 | `get_field_fast` | ✗ **补守卫**（快路径的三项检查都跳慢路径 ⇒ 非 struct 接收者必进 callout；代码证据）|
| `ops_return.inc` `MODULE_CALL` | `module_call_meta` | ✓ 守卫在 `if/else` 之后、两个调用点共用 |
| `ops_callout.inc` `OP_SET_FIELD` | `set_field` | ✓ 同上（`OP_GET_FIELD` 共用后置守卫）|
| `ops_misc` `is_null` / `ops_icmp` `value_eq` / `ops_jump` `switch_lookup` | — | ✓ 无失败模式（不是"忘了检查"）|
| `ops_arith` `concat` | `jit_callout_concat` | ✓ 用**返回值**判 `NULL_VAL` 当守卫（等价手段）|
| `ops_callout` `set_ptr_elem_type` / `set_declared_face` | — | ✓ 静默 no-op（`jit_callout.c:1153-1165`），永不置 `failed` |
| `ops_callout` `string_add` / `type_check` / `as_cast` | — | ✓ 一行转发，"永不置 `failed`"（注释与实现一致）|

⇒ 审计结论：**4 处真漏全部补上**，其余 13 处逐条核实为无害 ✓。

⚠ **本次最重要的连带教训**：第 1 步"把 `error_add_at` 换成只置 `failed`"会**放大**第 2/3 步
这类漏洞 —— 原来错误至少会被记进全局表（可见但结果错），换掉之后如果调用点没守卫，
就变成**彻底静默**。所以"统一错误通道"和"核对每个调用点的守卫"**必须同时做完**，
不能分两次发布。

**最终验证**：10 个探针（4 个错误通道 + `get_field_addr` / cstruct / CLIB / native 回调 /
dict / bound method）JIT/`LENO_NO_JIT=1` **逐字一致** ✓、assert **311 passed / 0 failed** ✓。

***

### 8.92 覆盖面合成表（工具 + 当前实测）—— 结论：**三个应用里已没有"实现缺口"**（2026-09-17）

**动机**：JIT 覆盖面有两个**互不相通**的观测口径，过去只能手工分别看：
**scan 期拒收**（`LENO_JIT_GAPS=1`，编译前就拒绝的对象）与**运行期 bailout**
（stats 的 `Bailout:` 行，编了但执行中失败、最终被拉黑的对象 —— 行内含**触发指令名**，§8.89）。
只看前者会漏掉"编了却每次都 bail"（R6-k 之前的 bound-method 就是 ✗），只看后者会漏掉
"根本没尝试编" ✗。工具：`jit_probes/jit_census.ps1`（一次跑完两边，见 `jit_probes/README.md`）。

#### 当前实测（3 个真实应用 × 300 帧，`SDL_VIDEODRIVER=dummy`）

| 应用 | Compiled | Executed | **Bailouts** | FuncCompiled | `us_per_frame` | NO_JIT `us_per_frame` |
| --- | --- | --- | --- | --- | --- | --- |
| file_manager | 52 | 58746 | **3** | 115 | 3698 | 6679（**1.81x**）|
| cache_cleaner | 33 | 31366 | **0** ✓ | 66 | 5490 | 5423 |
| gomoku | 11 | 4319 | **0** ✓ | 13 | 2925 | 2915 |

#### scan 期拒收（按编译对象去重，跨应用合计）

| 类别 | fm | cc | gomoku | 合计 | 性质 |
| --- | --- | --- | --- | --- | --- |
| `OP_GET_MODULE_VAR`（跨模块取模块变量，inline 侧）| 17 | 15 | 10 | **42** | §8.55 **刻意保留的正确性边界** |
| `OP_GET_MODULE_FUNC`（跨模块取函数，inline 侧）| 3 | 2 | 1 | **6** | 同上 |
| 函数体含循环 `OP_FOR_PREP`（func_mode 不支持）| 13 | 7 | 3 | **23** | **设计限制**（函数级 JIT 只编无循环函数）|
| 函数体含循环 `OP_LOOP`（同上）| 10 | 6 | 0 | **16** | 同上 |
| 循环体含可达 return（RETURN / RETURN_MULTI / TAIL_CALL）| 3 | 2 | 2 | **7** | 设计限制（单出口假设）|
| **合计** | 46 | 32 | 16 | **94** | — |

运行期 bailout 只有 **1 个循环**（fm 的 `render`，`触发指令=OP_FOR_PREP` = **float 步长内层循环**，
§8.89 已定位为设计限制 ✓）。

#### 结论（这就是"下一步该做哪个"的答案）

**94 个 scan 期拒收 + 1 个运行期 bailout，逐类看全是"刻意的正确性边界"或"设计限制"** ✗：

- **跨模块访问（48）**：§8.55 定的边界 —— 模块变量会被重新赋值，嵌入"编译期解析结果"就不安全；
  要做等于**改变正确性前提**（每次查表 + 版本校验），不是"补个 case"；
- **函数体含循环（39）**：这是**函数级 JIT 的定义**（只编无循环函数）；那些函数里的循环
  **本来由循环级 JIT 覆盖**（`Executed: 58746 / 31366 / 4319` 就是证据）；
- **可达 return（7）**：多出口 ⇒ 返回值折叠需要静态单出口假设；
- **float 步长（1）**：`FOR_LOOP` 只走 int48 快路径（§8.89）。

⇒ 因此：**C 的三个候选里，`OP_AS_CAST(94)` 已经做完（R6-g）** ✓，另两个
**不是"缺口"而是"边界"** ✗ —— 而且"拒收数"**不等于"净损失"**：
被拒对象的循环可能已被循环级 JIT 编掉（"函数体含循环"这一类尤其如此）✓。

#### 由此得到的下一步建议：先量**净损失**，再谈大项

便宜但决定性的实验：把某个"因含循环而被拒"的函数改写成**循环留在 main**
（语义等价、只移动代码位置），比较 `us_per_frame`：

- 若**没有差别** ⇒ "函数体含循环"类拒收的**净损失 = 0** ✓ ⇒ 这一类永远不用做；
- 若有差别 ⇒ 才值得评估"函数级 JIT 支持循环"。

`OP_AS_CAST` 的教训同样适用：**先量收益再立项**（§8.79）—— 本轮合成表就是这把尺子。

**⚠ 工具本身的经验**：脚本必须**纯 ASCII**（Windows PowerShell 5.1 会把**无 BOM 的 UTF-8**
按 ANSI 读，中文字符串字面量会被拆坏、直接解析失败 —— 实测踩过一次 ✗）；
中文目录名改用 **ASCII 文件名 glob 定位** ✓。

***

### 8.93 「净损失」实验：「函数体含循环」这条拒收**不是 0，而是 2.36x**（推翻了 §8.92 的乐观判断）（2026-09-17）

**背景**：§8.92 的合成表显示 3 个真实应用剩下的 94 个 scan 期拒收**逐类都是"边界/设计限制"**，
其中最大的一类是「函数体含循环」（39 个）✗。我当时**假设**它的净损失 ≈ 0，
理由是"那些函数里的循环本来由**循环级 JIT** 覆盖" ✓ ⇒ 结论是"不该投大项" ✗。
按 §8.79 的规矩（先量收益再立项），本节把这个假设**做成实验**验证 —— 结果**被推翻** ✓✗。

**实验设计**（两个探针，语义完全等价、只挪代码位置）

| 探针 | 形态 | 落进 JIT 的部分 |
| --- | --- | --- |
| `netloss_a_func_with_loop.leno` | 热循环里调用「**函数体含循环**」的辅助函数 `dot(a,b,n)` | 调用方的 `while`（循环级 JIT ✓）+ `dot` 内部的 `for`（循环级 JIT ✓）；**但每次调用都走一次解释器调用** ✗ |
| `netloss_b_loop_in_main.leno` | 同一份代码，内层 `for` **直接写在 `main` 的热循环里** | 整个嵌套循环都在**一个**编译区域里 ✓ |

工作量两边相同（`total=240000000` 双向一致 ✓），1,000,000 次调用 × 8 次内层迭代；
计时取 3 轮 A/B 交替的**最小值**（进程启动开销因工作量大而可忽略 ✓）。

**结果**

| | A（含循环的函数被热调用）| B（循环写在 `main`）|
| --- | --- | --- |
| JIT | **236ms**（251/239/236）| **100ms**（100/105/107）|
| `LENO_NO_JIT=1` | 427ms | 413ms |
| JIT 收益 | 1.81x | **4.13x** |
| checksum | `240000000` ✓ | `240000000` ✓ |
| 统计 | `Compiled: 2 / Executed: 999995 / Bailouts: 0` | 同 |

⇒ **A 比 B 慢 2.36x**，即「函数体含循环」这条拒收的净损失 = **每次调用约 136ns**
（`(236-100)ms / 1e6`）✗✗ —— **不是 0** ✓。

**A 的全部拒收只有一行**（`LENO_JIT_GAPS=1`）：

```
1  func|函数体含循环（func_mode 不支持）op=95(OP_FOR_PREP)
```

注意：**没有 `inline|` 行** ⇒ 调用方的循环**根本没有把 `dot` 当作内联候选** ✗。
也就是说这条损失由两个"堵点"共同造成：① 函数级 JIT 不收含循环的函数；
② 循环级 JIT 也不把它内联进调用方 ⇒ 每次调用都退化成解释器调用。

**§8.92 的判断修正** ✗→✓：**"拒收数 ≠ 净损失"这句话本身对** ✓，但**方向错了** ——
我当时猜"净损失 ≈ 0"，实测是 **2.36x**（本例）。**"含循环的被调方"是真实且可观的收益面**，
C 不该跳过它 ✓。

**下一步（两条路线，需先读代码定成本）**

| 路线 | 内容 | 备注 |
| --- | --- | --- |
| ① 函数级 JIT 支持循环 | 放开 `func_mode` 对 `OP_FOR_PREP` / `OP_LOOP` 的拒绝 | 需确认这条拒绝是**策略**还是**技术限制**（若只是策略，改动可能很小）|
| ② 内联支持含循环的被调方 | 让循环级 JIT 把含循环的 callee 内联进调用方 | B 探针就是这条路的**存在性证明**（手工内联后 4.13x ✓）✓；但要处理 callee 的 locals / `OP_RETURN` → inline-end 跳转 |

**⚠ 方法论提醒**：本节的假设方向错了 —— 说明"**设计限制**"这四个字**不能当作"无收益"** ✗。
凡是 census 里出现的拒收类别，都要像本节这样用等价改写做 A/B，才能判定净损失 ✓。

***

### 8.94 补掉内联侧的观测盲区 → 立刻暴露三处静默门（+ 一次**负结果**的阈值实验）（2026-09-17）

**动机（§8.93 的遗留）**：probe A 的净损失是 **2.36x**，但 `LENO_JIT_GAPS` 里只有一行**无关**的
`func|函数体含循环` ✗ ⇒ 内联侧的拒绝**根本不可观测** ✓ —— 先修观测，再谈优化。

#### 第 1 步：给所有静默门补记录（`jit_scan.c`）

| 路径 | 补上的记录 |
| --- | --- |
| 全局函数内联 | `func2`/`chunk` 解析不出来、`inline_count ≥ 4`、`callee locals 放不下`、`callee 字节码长度超上限` |
| 方法内联 | 方法体定位不出来、方法体含 try、方法体字节码长度超上限 |
| **最关键** | **未类型化的 `OP_CALL_GLOBAL_FUNC` 从来没有内联路径** ✗ —— 内联块历史上只对 `OP_CALL_GLOBAL_FUNC_TYPED` 开放（`jit_scan.c:1446` 的 `if (op == …_TYPED)` ✓），而 §8.93 的 probe A 用的正是**未类型化**形态 ⇒ 现在记为显式缺口 |

#### 第 2 步：一补记录就露出一个更大的门（第一次可见）

| 类别 | file_manager | cache_cleaner | 五子棋 |
| --- | --- | --- | --- |
| `内联未尝试：…字节码长度超上限`（旧的硬编码 **256** ✗）| **33** | **20** | 8 |
| `未实现内联：未类型化 OP_CALL_GLOBAL_FUNC 调用点` | 0 | 0 | 2 |

#### 第 3 步：阈值实验 —— **负结果**（已还原）

把硬编码 256 提成具名常量并试调 1024（`JIT_INLINE_MAX_BODY`）：
"长度超上限" 33 → **3** ✓，**但**那些 callee 立刻被**别的门**挡住 ✗ ——
`跨模块访问` 17 → **21**、新出现 `OP_SWITCH_LOOKUP` 1 ⇒ **瓶颈只是换了位置**
（正是 §8.80 记过的"census 每个对象只记第一个原因"）。

**同条件 A/B（fm，300 帧，3 轮交替，改前二进制已留存）**：

| 轮 | 256（改前）| 1024（实验）|
| --- | --- | --- |
| 1 | 3754 | 3701 |
| 2 | 3738 | 3782 |
| 3 | 3773 | 3823 |

⇒ **完全重叠、无可测收益** ✗✓（机制上也讲得通：那些 callee **并没有真的被内联** ✓）
⇒ **还原成 256** ✓（保留具名常量与全部诊断记录 —— 零行为变化 ✓）。
**方法论**：负结果也是结果；实验做完要**还原**，不留没有依据的改动 ✓。

#### 结论：内联侧的门按规模重新排序

| 门 | fm | cc | 五子棋 | 合计 | 性质 |
| --- | --- | --- | --- | --- | --- |
| **跨模块变量/函数访问**（`OP_GET_MODULE_VAR` 42 + `OP_GET_MODULE_FUNC` 6）| 20 | 17 | 11 | **52** | §8.55 的边界（内联体的 module 上下文 vs `cur_chunk` 不一致）|
| 字节码长度超上限（256）| 33 | 20 | 8 | **61** | 实测**调大无收益**（瓶颈在别处）✗ |
| 未实现内联：未类型化 `OP_CALL_GLOBAL_FUNC` | 0 | 0 | 2 | 2 | probe A 的形态（**2.36x** 净损失 ✓）|

⇒ 下一步按此排序：① **让内联体支持跨模块访问**（52 处，最大 ✓；需要把 callee 的 module
传进内联 codegen，或对内联体内的跨模块访问退化为 callout ✓）；② 未类型化
`OP_CALL_GLOBAL_FUNC` 的内联（需"运行时 callee 仍是编译期那个"的守卫 ✓）。
**先量再立项**（§8.79）：每一项动手前都用 §8.93 的等价改写 A/B 量一次净损失 ✓。

**验证**：还原后 fm census 回到 **33 / 17** ✓（行为与改前逐字一致 ✓）、10 个探针
JIT/`LENO_NO_JIT=1` **IDENTICAL** ✓（含净损失 A/B 的 checksum ✓）、assert **311/0** ✓。

***

### 8.95 解锁「跨模块变量访问」的被调方内联（+ 一个诚实的负结果：fm 无可测收益）（2026-09-17）

**背景**：§8.94 把内联侧的头号门定位为**跨模块访问**（3 应用合计 52 处 ✗）。旧实现在
`scan_callee_for_inline` 里直接拒绝跨模块 ✓，理由写得对：内联后 codegen 嵌入的 module 是
**调用方**的（`jit_scan_get_module()`）⇒ 跨模块会用错 globals 下标、**静默读错变量** ✗。

**修法：把 callee 的模块随内联点带下去**

| 位置 | 改动 |
| --- | --- |
| `InlineSite`（`jit_priv.h`）| 新增 `ObjModule* callee_module` |
| `jit_scan.c`（两个创建路径）| 分别填 `func2->module` / `mf->module`；**解除跨模块拒绝**（只保留"拿不到 callee 模块"→ 记为 `（无 callee 模块）`）|
| `jit_scan.c` | 新增 `jit_resolve_module_func_in(module, index)` —— ⚠ **关键坑**：内联体里 `GET_MODULE_FUNC + CALL` 的 `ret_count` 解析**也必须用 callee 的模块** ✗，否则读的是调用方 globals 的同名下标（静默算错 ✓）|
| `x86_64.c` | 新增 `cur_module`（初值 = 调用方模块）+ `InlineFrame.module` 保存/恢复 |
| `ops_callout.inc` | 两个内联入口切 `cur_module = <site>->callee_module`；三处模块访问发射（`get/set_module_var`）改用 `cur_module` |
| `jit_scan.c` | **本轮只解锁 VAR 两形态**（`OP_GET_MODULE_VAR` / `OP_SET_MODULE_VAR`，fm 20 / cc 17 / 五子棋 11 的绝大多数）；`OP_GET_MODULE_FUNC`（值形态与配对形态走**模块方法解析**）单独记为 `OP_GET_MODULE_FUNC（§8.95 暂未放行）` |

**验证**

1. **语义（关键）**：两个真实多模块应用逐字对拍 —— fm `sum=21600/21600` ✓、cc `4800/4800` ✓
   （跨模块内联**没有算错**）；assert **311 passed / 0 failed** ✓；10 个探针
   JIT/`LENO_NO_JIT=1` **IDENTICAL** ✓。
2. **census**：fm 的 `模块变量/函数访问（跨模块）` **17 + 3 → 0** ✓✓；新出现
   `OP_GET_MODULE_FUNC（暂未放行）` 3 ✓；`方法体字节码长度超上限` 33 → **28**（5 个对象越过了这一层 ✓）。
3. **性能：负结果** ✗ —— fm 的 `us_per_frame` **3764**（改前 3754/3738/3773）⇒ **无可测变化** ✓。
   原因：fm 里被解锁的那些被调方**不够热**（内联省下的是调用开销，但它们不在最热路径上 ✓）。

**结论**：改动**结构上正确、语义已验证、性能中性** ⇒ **保留**（它是一条真实限制的解除，
也是后续"热点内联"的前提 ✓）；但**不得宣称收益** ✗。下一步按 §8.79：找一个**真正热的**
跨模块调用点做等价改写 A/B（像 §8.93 那样量 ✓），或继续放行 `OP_GET_MODULE_FUNC` ✓。

***

### 8.96 裁决：**不做**「未类型化 `OP_CALL_GLOBAL_FUNC` 的内联」（§8.93 那个 2.36x 的另一半）（2026-09-17）

**背景**：§8.93 用等价改写量到「含循环的被调方无法内联」的净损失 **2.36x**（probe A 236ms vs
probe B 100ms）；§8.94 把它的成因拆成两个门，其中一个是
**`未实现内联：未类型化 OP_CALL_GLOBAL_FUNC 调用点`** ✗（内联块历史上只对 `..._TYPED` 开放，
`ops_callout.inc:945`）⇒ 看起来是个"有实测收益背书"的好目标。

**读代码后否掉**，理由两条（都在实现前就能判定 ✓）：

1. **需要 callee 身份守卫** ✓：`vm.global_funcs[slot]` **会被重新赋值**
   （`OP_DEFINE_GLOBAL_FUNC`，`op_variables.inc:225` ⇒ 重定义/提升都可能换 callee）；
   内联等于把"编译期那个 callee"烘进机器码 ⇒ 必须逐次校验运行时槽内容仍是它，
   不一致就 bailout（否则**静默编错** ✓）。
2. **会跳过参数提升** ✗（更硬的一条）：解释器的同名 opcode 调
   `jit_try_hot_func_call(closure, arg_count, **typed=0**, &vm)`，注释写明
   `typed=0：**需按 param_types 做参数提升**`（`op_call.inc:863`）；而内联直接按
   callee 的 local 槽铺参数 ✗ ⇒ **int→float 之类的提升就丢了** ⇒ 静默算错 ✓。
   要让内联安全，得先把"按 `param_types` 提升实参"这套机械搬进 codegen ✗ —— 这不是"补个 case"，
   而是一个**独立特性**。

**收益面也小** ✗：`LENO_JIT_GAPS` 实测这种调用形态在三个真实应用里
**gomoku 2 处、file_manager 0、cache_cleaner 0** ⇒ 与 §8.93 的合成探针不同，
**真实应用里几乎没有这个形状** ✓（§8.93 的 2.36x 是"含循环被调方不能内联"这一整类的代价，
其中绝大部分属于**别的**门 ✓）。

**裁决：不立项**（记录在此，避免后人重复评估）✓。若将来要做，顺序必须是：
**① 先把参数提升搬进内联（或对内联体只接受"无需提升"的调用点）→ ② 再加 callee 身份守卫 →
③ 最后接扫描/内联入口** ✓。**先量再立项**（§8.79）在这里再次省下一次大改 ✓。

***

### 8.97 §8.89 的结论改判为**已证实**（fm 的 bailout = 静态 float 步长）；并量到其净损失 ≈ **2x**（2026-09-17）

#### 先补掉一处观测缺陷：`OP_FOR_PREP` 的两个守卫共用同一 `bc_off`

`OP_FOR_PREP` 里有**两个** bailout 守卫 —— 静态"step 是 float"（类型位图检查）与运行期"step == 0" ——
历史上都写 `EMIT_BAILOUT_SITE_NONOVF(bc_off)` ⇒ **同一个 `bc_off`** ✗ ⇒ 只看
`非溢出类 @bc_off=N` **分不出是哪一个** ✗（§8.89 的"float 步长"当时其实是**猜测** ✓）。
本次给 `step == 0` 单独一个基址：`EMIT_BAILOUT_SITE_FORSTEP0(off)` = `-2000 - off` ✓，
并在 `jit_bailout_reason` 与"触发指令"还原各加一支 ✓。

**改判**：重跑 fm ⇒ 打印仍是 `非溢出类 @bc_off=787（= loop_bc 414 + 373） | 触发指令=OP_FOR_PREP` ✓
—— **新标签没有出现** ✓ ⇒ 按排除法，命中的就是**静态 float 步长**那条 ⇒
§8.89 的结论**从猜测升级为已证实** ✓。

#### 净损失量化（探针 E / F，等价改写 A/B）

- `nlev_e_floatstep_loop.leno`（E）：热外层 `while` 里包 `for 0.0 : 1.0 : 0.05 to t`（fm 的 `render` 形态）
- `nlev_f_intstep_loop.leno`（F）：同形状、内层改 **int 步长**

| | JIT（3 轮）| `LENO_NO_JIT=1` | Bailouts |
| --- | --- | --- | --- |
| E（float 步长）| **174 / 196 / 187 ms** | 158 ms | **6**（外层 + 内层各 3）|
| F（int 步长）| **93 / 95 / 97 ms** | 211 ms | **0** |

⇒ ① **E 的 JIT 几乎等于（甚至略慢于）NO_JIT** ✗ —— 那条守卫让这块代码**完全没享受到 JIT**；
② F 拿到 **2.2x**（93 vs 211）✓ ⇒ **修好浮点步长能拿回的正是这个量级（≈2x）** ✓；
③ 附带发现：E 里还额外出现 `序言: step 为 float（非 int 循环）`（= 专用编码 `-3`，来自**内层循环自己**的
JIT 尝试）⇒ **外层与内层两条循环都被拒** ✓（6 条 bailout 的来历 ✓）。

**⚠ 口径诚实说明**：E 与 F 的**迭代数并不完全相等** ✗ —— 浮点累加漂移使 E 的内层实际走 18 次
（checksum 1800000）而 F 走 21 次（2000000）⇒ 严格的等价比 F **更吃亏** ⇒ 上面的 ≈2x **只会偏保守** ✓。

#### 立项评估（实现"浮点步长 for 进 JIT"）

- **要做的**：为 double 循环变量在 `FOR_PREP` / `FOR_LOOP` 里走 SSE（`ucomisd` / `addsd`），
  复用既有的"三态取操作数"机械（int48 提升 / 裸 double / NaN-boxed 回退），
  对齐解释器的方向（正/负步长）、含端点与 NaN 语义，并放开类型位图那条守卫 ✓。
- **量级**：以 `ops_loop.inc` 为主，约 150~250 行 + 探针/回归 ✓（属**特性**而非修补 ✓）。
- **收益**：本形状 ≈ **2x**；并**解除 fm `render` 主循环的拉黑**（§8.89 的 3 条 bailout 一并归零 ✓）。

***

### 8.98 【真因见 §8.99，回滚已撤销】R6-e 解锁的 `OP_CLIB_CALL` 让飞机大战**开火后窗口卡死**（2026-09-17）

> ✅ **更新（同日）**：根因**已查清并修复** —— 见 **§8.99**（JIT 的「裸 double 0.0」与 int48 撞码
> ⇒ FFI 收到整数 ⇒ Win64 浮点分发报错）。**临时回滚已撤销，R6-e 已恢复**。
> 本节保留二分定位与排除过程作为记录；下面「真因未查清 / 修好前不要把 case 删回去」的说法**已过期** ✓。

> **一句话**：§8.81（R6-e）让 `OP_CLIB_CALL` 进 JIT 后，LenoSDL3 的**真实游戏**在开火后卡死；
> 二分定位到 `26875acf`（该提交**唯一**的代码改动就是 R6-e）；**已临时回滚**（两处 scan case 改回拒收），
> assert 311/0 ✓。真因**未查清**，⚠ **在修复前不要把这两处 case 删回去**。

#### 现象（真实游戏，不是探针）

`leno_module/LenoSDL3/examples/应用示例/游戏/飞机大战/plane_war.leno`：
回车开始游戏 → **打两发子弹 → 窗口卡死**（无异常文本、无 0xC0000005；JIT stats 能正常打印，
无 SIGSEGV 迹象）。`LENO_NO_JIT=1` 或 R6-e 之前（`294c6e2`）**不崩** ✓。

`26875acf` 上的 stats（可复现，稳定）：

```
Compiled: 13   Executed: 2026   Bailouts: 4
Bailout: fn='_flushTexBatch' loop_bc=25  x3 — 非溢出类 @bc_off=138（= loop_bc 25 + 113）
Bailout: fn='renderPlay'     loop_bc=1004 x1 — 非溢出类 @bc_off=1056（= loop_bc 1004 + 52）
FuncCompiled: 3   FuncExecuted: 899
```

⚠ 这 4 条**不能**当成证据：`非溢出类` 是一个大杂烩（callout 失败 / NaN-boxed 浮点操作数 /
其它守卫），而 `26875ac` 时的 stats **还不会打印触发指令名**（那是 §8.89 的 `last_bailout_op`，更晚才有）
⇒ 无法从 site 值分辨是哪条指令。**不要把"有 "bailout" 直接等同于"就是 CLIB 那条"**。

#### 二分定位（决定性）

| 步骤 | 提交 | 结果 |
| --- | --- | --- |
| 起点 good | `294c6e2` `perf(JIT): 尾调用加 callee-亦-JIT 快路径…` | 不崩 ✓ |
| 起点 bad | `26875acf` `feat(JIT): R6-e 支持 OP_CLIB_CALL` | **必崩** ✗ |
| 中间点 1 | `bebff66b`（纯 docs） | 不崩 ✓ |
| 中间点 2 | `b0f0f6b6`（函数级缓存 256→1024） | 不崩 ✓ |
| 中间点 3 | `207dbe25`（R6-d `OP_GET_CSTRUCT_DEF`） | 不崩 ✓ |
| **首坏** | **`26875acf`（R6-e）** | **崩** ✗ |

区间 `294c6e2..26875ac` 共 10 个提交，其中只有 5 个动了代码（其余是 docs/diag/test），
R6-d 已被中间点 3 单独证清白 ⇒ **指向 R6-e** ✓。

**复现每个测试点的固定流程**（缺一不可，否则结论不可信）：

```powershell
git switch --detach <commit>
cmd /c build.bat                       # 或 .\build.bat
Remove-Item -Recurse -Force "D:\CLeno\Leno\.lenocache","…\飞机大战\.lenocache"
leno --no-cache "plane_war.leno"       # --no-cache 同时禁用模块缓存与入口缓存
```

> ⚠ 缓存 key 只基于**入口文件内容**哈希 ⇒ 只换编译器不换入口脚本时**必然命中旧缓存**，
> 表现为"切了提交但行为不变"。`--no-cache` 是这件事的标准解法（见 main.c 对
> `module_loader_is_cache_enabled()` 的两处 gate）。

#### 为什么探针没抓住（本次最重要的教训）

`jit_probes/probe_clib_call_jit.leno` 在 R6-e 交付时是**全绿**的：JIT/`LENO_NO_JIT=1` 逐字一致、
**Bailouts 0**、`LENO_JIT_DEBUG=1` 下 `clib_call` 方向 0 条 CALLOUT-FAIL ✓。

差别在**形态与时长**：

| | 探针 | 飞机大战 |
| --- | --- | --- |
| 用户实参个数 | 1 | 多个（含 float / 指针 / 字符串）|
| `ret_type_kind` | void / i32 | 各式（含 float、指针、void）|
| 循环轮数 | 20000 轮纯计算 | 长跑 + 与 SDL 渲染/状态耦合 |
| 上下文 | 无外部副作用 | 每次调用都在改 SDL 资源（纹理批、渲染队列）|

⇒ **"探针绿"只证明"这条 opcode 的记账在简单形态下自洽"，不能证明它在真实负载里可用** ✓。
（§8.81 其实已经留了线索：R6-e 后 fm/cc 出现 **797 条 CALLOUT-FAIL**，当时按"性能噪声"放过了 —— 
**"能编但每次执行都 bail"在 GUI 长跑里不是性能问题，是正确性问题**。）

#### 已逐条排除（把 CLIB 路径与解释器/既有 callout 逐行对照）

| 怀疑点 | 结论 |
| --- | --- |
| emit 的物理栈账：`TOS_SPILL → callout → failed 检查 → add rsp,arg_count*8 → VALUE_TO_RAW → TOS_PRODUCE` + `vstack -= (arg_count-1)` | 与 `OP_CALL_NATIVE` **逐行同构**，且 `tos_live` 不变量（mem = vstack-1）两边都对得上 ✓ |
| 实参顺序：callout 的 `vstack_top[arg_count-1-i]`（i 递增 ⇒ 先压 lib、再 func_name、再实参） | 与 VM 的 `vm.stack + vm.sp - arg_count`（= 最先压入者）语义一致 ✓；`ffi_call_impl` 的 `arg_start = 2` 对得上 ✓ |
| 变长操作数交给 callout 现读（`ip+1/ip[3]/ip[4]/ip[5..]`） | emit 里的 `ip` 是**opcode 字节**（与 `OP_CALL` 的 `rd_short(ip+1)`、`OP_CALL_NATIVE` 的 `rd_short(ip+3)` 同一约定）✓ |
| `arg_types[]` 未初始化（JIT 只填 `[0,user_arg_count)`，解释器额外 `memset` 整个数组） | **无害**：`ffi_call_impl` 只读 `i < sig.nargs(= user_arg_count)` ✓（但建议顺手 memset 对齐解释器）|
| callout 失败守卫是否漏（§8.91 的 45 处审计）| CLIB 站点**不在** 4 处真漏里 ✓ |
| §8.42 pin 走查（局部量驻留寄存器）需要的新增排除项 | CLIB emit **不访问局部量**（无 `cur_local_map[`）⇒ 无需同步 ✓ |
| `opcode_size` 的变长长度 `5 + ip[4]` | 与解释器的 `READ_*` 次数一致 ✓，R6-e 之前就已收录 ✓ |

#### 尚未排除的三个方向（下一步取证）

1. **函数级 bailout 是"从头 VM 重入"**：`jit_callout_global_func` / `invoke_method` / `call_value`
   的失败路径都落到 `vm_call_value(callee, arg_count, 0)`（**把整个函数在解释器里从头重跑**）。
   R6-e 让一批**原本被整体拒收**的函数新解锁（`func 净解锁 11`）⇒ 只要它们在某个缺口处 bail，
   **bail 点之前的所有副作用会执行两遍**（子弹、数组 push、状态机……）。
   对纯函数无所谓，对游戏循环可能就是"状态错位 → 某个等待条件的循环永不结束 → 卡死"。
   ※ 与 §8.81 记录的"797 条 CALLOUT-FAIL / 每次执行都 bail"是同一现象的**正确性**侧面。
2. **`jit_callout_clib_call` 是唯一不更新 `jit_reloaded_locals` 的重入型 callout**：
   `ffi_clib_call` 内部**会**回调进 VM（自动泵送 → `vm_call_value` → `vm_grow_frames`），
   而 `global_func` / `invoke_method` / `call_value` / `tail_call` 四条都写了
   `jit_reloaded_locals = vm->frames[vm->frame_cnt-1].locals`，**只有 clib_call 没写** ✗。
   （它只在机器码**写回 locals** 时被读 ⇒ 影响面可能是"写回到旧数组"，需实测确认。）
3. **长跑才暴露的物理栈漂移**：§8.67 已证过同类（`OP_GET_PROPERTY` 独立取值漏弹 receiver
   ⇒ 每次泄漏 8 字节 ⇒ ~2MB 栈界崩）。CLIB 的 `arg_count` 是**变长**的，
   这类"每轮差一个槽"的 bug 在小探针里完全看不出来。
   ⇒ 推荐做法：写一个 **多实参 + 长跑 + 带副作用**的 CLIB 探针（例如在循环里
   `ffi` 调一个带状态的 DLL 函数并核对状态），把"轮数"拉过 262144。

#### 诊断命令（main 上的诊断能力比 26875ac 强得多，先用它）

```powershell
# ① stats 现在会打印 bailout 的触发指令名（§8.89）
leno --no-cache "plane_war.leno"
# ② 卡死前到底是谁失败：重定向到文件，卡死后关窗口再看尾部
$env:LENO_JIT_DEBUG="1"; leno --no-cache "plane_war.leno" 2> jit.log
Select-String -Path jit.log -Pattern "CALLOUT-FAIL|BAILOUT" | Select-Object -Last 40
Remove-Item Env:LENO_JIT_DEBUG
```

#### 处置：临时回滚（本次，2026-09-17）

`src/jit/jit_scan.c` **两处** `case OP_CLIB_CALL` 改回拒收（emit / callout 的代码**保留不删**）：

| 位置 | 改法 | 效果 |
| --- | --- | --- |
| `scan_callee_for_inline`（inline 侧）| `jit_gaps_record_inline("OP_CLIB_CALL（R6-e 已回滚：卡死）"); return 0;` | 含 FFI 调用的被调方不再内联 |
| `scan_loop_body`（loop 侧）| `r->capable = 0; return;` | 含 FFI 调用的循环整循环拒收 → 交解释器 |

= **等价于 R6-e 之前的行为**（`294c6e2` 已实测不崩 ✓），代价是丢掉 R6-e 的
`loop -9 / func -11 / inline -5` 收益。

**验证**：assert **311 passed / 0 failed** ✓（与基线一致）；飞机大战**人工复测：不崩** ✓
（2026-09-17）⇒ **确认卡死就出在 R6-e 的 `OP_CLIB_CALL` 进 JIT 这条路径上** ✓
（回滚前必崩 / 回滚后不崩，单变量对照，排除"另有原因"）。

**恢复条件**：上面三个方向查清并修好后，**删掉这两处 case** 即恢复 R6-e（emit/callout 一行没动，
所以恢复是纯删除 ✓）。

***

### 8.99 【已修复】§8.98 的根因：JIT 的「裸 double 0.0」与 int48 **撞码** ⇒ FFI 收到整数 ⇒ Win64 浮点分发失败（2026-09-17）

**一句话**：R6-e 之后，SSE 算出的**浮点 0.0** 被 `jit_raw_to_value` 判成 **int 0** 交给 FFI；
`SDL_SetRenderDrawColorFloat(renderer, 0.9, 1.0, **0.0**, 1.0)` 的 `b=0.0` 就是触发点 ⇒
`ffi_call_impl` 的 Win64 精确分发检查报「超过 Win64 精确分发上限」⇒ 抛异常 ⇒
CLIB callout failed ⇒ bailout ⇒ 解释器按原指令重放（异常仍在）⇒ **飞机大战卡死**。

#### 根因链（每一环都有实测）

1. JIT 把浮点运算结果以**裸 double** 留在寄存器里（`EMIT_MOVQ_RAX_XMM0` ⇒ TOS = raw double）。
2. `jit_raw_to_value` 判类型用位型启发式：`test = raw >> 47; test++; if ((uint64_t)test <= 1) ⇒ int48`。
   **double 0.0 的位型正好是 `0x0000000000000000`** ⇒ `test == 1` ⇒ 被当成 **int 0** ✓
   （`-0.0`（`0x8000…`）与次正规数同理）。
3. 该值经 `vm_stack_push(jit_raw_to_value(...))` 交给 `ffi_clib_call` ⇒ 是一个 **VAL_INT** ✓
   （实测 trace：`arg[3] raw=0000000000000000 kind=int`，而 `arg_types[] = 40 25 25 25 25`，
   第 4 个形参声明为浮点）。
4. `ffi_call_impl` 的整数分支按 `param_tk` 分流，**没有 `TYPE_F32/TYPE_F64` 的 case** ⇒
   `default` 落成 `FFI_TYPE_INT` ⇒ 0.0 被安排进**整数寄存器（GPR）**而不是 XMM ✓。
5. 调用前的 Win64 检查（`ffi.c`「前 4 个形参浮点计数」）：形状本该是 `(ptr, f32, f32, f32, f32)`
   ⇒ `float_in_reg == 3 && f32_total == 4` ⇒ 命中「路径 3.5」精确分发 ✓；
   但第 4 个变成 INT ⇒ `f32_total == 3` ⇒ 例外不成立 ⇒ **抛「超过 Win64 精确分发上限」** ✓。
6. callout 置 `jit_callout_failed` ⇒ 机器码 bailout ⇒ 解释器按原指令重放
   （**FFI 副作用已发生**，且 `vm.has_exception` 未清）⇒ 卡死 ✓。

**为什么探针抓不到**（呼应 §8.98）：探针的浮点实参是 `i - 1000` 这类**非零**值，
位型不落 int48 区间；只有 **0.0 / -0.0 / 次正规数**才撞码 ⇒ 这是**值域依赖**的 bug，
形状探针（哪怕 JIT/NO_JIT 逐字比对）也测不出来 ✓。

#### 定位方法（可复用：GUI 负载自动化 —— 不再需要人工按键）

1. 复制游戏脚本为 `_pw_auto.leno`，只加两处：`startGame(g)` + 渲染回调里每帧 `g.player.shooting = true`；
2. `$env:LENO_SDL_FRAMES="600"; $env:SDL_VIDEODRIVER="dummy"` ⇒ **无窗口、定帧、自动退出、可判超时**；
3. 外层 `Start-Process … -PassThru` + `WaitForExit(ms)` ⇒ 「卡死」= 超时不退出，可自动化判定 ✓；
4. 专用轻量 trace（`LENO_CLIB_TRACE=1`，保留在 `debug/r6e-clib` 分支）打印每次 CLIB 的
   实参**原值 + 类型判定**，失败那次一眼就是根因 ✓；
5. 对照：`LENO_NO_JIT=1` 同脚本同帧数 ⇒ 跑完、零错误 ⇒ 确认是 JIT 侧问题 ✓。

#### 修复

`src/module/ffi/ffi.c` 的 `ffi_call_impl` 整数分支：**声明为浮点的形参收到整数时走浮点通道**
（判据复用 `typekind_to_ffitype`，与返回类型同一处语义来源）：

```c
default: {
    FFIType ft = typekind_to_ffitype(param_tk);
    if (ft == FFI_TYPE_FLOAT || ft == FFI_TYPE_DOUBLE) {
        sig.arg_types[i] = ft; ffi_args[i].type = ft;
        if (ft == FFI_TYPE_FLOAT) ffi_args[i].value.f = (float)ival;
        else                      ffi_args[i].value.d = (double)ival;
        break;
    }
    /* …旧路径：默认 FFI_TYPE_INT… */
}
```

同时**撤销 §8.98 的临时回滚**（`jit_scan.c` 两处 `case OP_CLIB_CALL` 恢复 R6-e）。

**为什么修在 FFI 边界而不是 CLIB callout 里**：① 它顺带修掉一个**解释器侧也存在**的隐患 ——
`clib { void f(f32 x) }` 里写 `f(1)`，整数会被安排进 GPR 而 XMM 是陈旧值（静默错值）；
② 符合本项目「语义唯一来源」的做法；③ 不必在 JIT 侧猜类型 ✓。

#### 验证

| 项 | 结果 |
| --- | --- |
| 自动驱动 600 帧（JIT）| 修复前**卡死**（90s 未退出，`[CLIB-FAIL] #4659`）/ 修复后**跑完并退出**，`CLIB-FAIL 0`、`Bailouts 0`、129000 次 CLIB 全成功 ✓ |
| 自动驱动 1800 帧（JIT）| 跑完，`sum=19822`、`CLIB-FAIL 0`、`Bailouts 0` ✓ |
| 自动驱动 600 帧（`LENO_NO_JIT=1`）| 跑完、零错误（对照）✓ |
| assert | **311 passed / 0 failed** ✓ |

#### 同类隐患（同一根因，本次**未**修，记录备查）

「裸 double vs int48」的歧义是 JIT 的**固有**表示问题，凡是"把 JIT 值交给需要精确类型的一方"
都可能同病：

| 位置 | 现状 |
| --- | --- |
| FFI 实参（clib / native 方法）| **本次已修**（FFI 边界统一提升）✓ |
| struct 字段赋值（`jit_callout_struct_init`）| 已有同类保护（`expected_type == TYPE_FLOAT && val_is_int` ⇒ 提升）✓ |
| 函数级 JIT 的实参（`jit_callout_global_func` 填 `flocals`）| **未修**：浮点形参可能拿到 int 0；因 JIT 浮点运算走「三态取操作数」（int48 → CVTSI2SD），数值上一致 ⇒ 目前只在「对形参做 `is float` 判断」这类形态下才可见 |
| `OP_TYPE_CHECK`（`x is float`）| **未修**：裸 double 0.0 会被判成 int ⇒ 与解释器分叉（同样值域依赖、探针测不出）|
| `OP_AS_CAST` / 类型化形参守卫 | 同上，需逐条评估 |

**教训**：`raw >> 47 ∈ {-1,0}` 这条启发式的边界是 **0.0 / -0.0 / 次正规数 / 部分 NaN**，
而「值为 0 的浮点」是最常见的浮点值之一 ⇒ 凡是跨**类型敏感边界**都要按**声明类型**兜底，
不能只信位型启发式 ✓。

#### 8.99.1 遗留观察：bailout → 重放路径的**退出健壮性**（类型：健壮性 / **待验证**）

**现象（实测，2026-09-17，真因未修前）**：CLIB callout 失败后走「bailout → 解释器按原指令重放」，
此时 ——

- **JIT stats 已经打印出来了**（说明程序走到了清理/退出路径），但进程 **90s / 150s 都不退出**，
  被外层 `WaitForExit(超时)` 判为「卡死」后强杀；窗口表现为**无响应**（这就是用户最初看到的
  "窗口卡死"）✗；
- ⇒ **不是"循环转不完"**（真死循环不会打印 stats），而是**退出/清理阶段挂住** ✓；
- ⚠ 该现象**未做进一步验证**，下面只是按可能性的排序，**不要当成结论**。

**待验证的怀疑（按可能性排序）**

1. **音频清理**：`main()` 尾部的 `g.bgm.stop() / sfxXxx.uninit() / audio.uninit()` 在
   "异常已抛出"这条路径上挂住（miniaudio 线程 join / 设备未初始化分支）；
2. **SDL 清理**：`win.destroy()` / `SDL3.quit()` 在 `SDL_VIDEODRIVER=dummy` + 异常路径下挂住；
3. **异常展开与 JIT 状态交互**：VM 帧展开与 JIT 的 `jit_loop_depth` / 机器码延迟释放队列
   （§8.60）之间的停等。

**为什么值得单独记**：它**放大任何一次 callout 失败的用户可见代价** ——
本该「打印错误并退出」的情况变成「窗口卡死、只能强杀」，而这正是本次排查里最耗时的表象
（用户第一次报的就是"窗口卡死"，与"报错退出"完全不同的排障路径）。
它与 §8.81 的「已发生的 FFI 副作用不回滚」是同一条通道的两个侧面 ✓。

**复现/验证方法（未执行）**

1. 用 §8.99 的自动驱动（`_pw_auto.leno` + `LENO_SDL_FRAMES=600` + `SDL_VIDEODRIVER=dummy`）；
2. 在 `jit_callout_clib_call` 里**人为置一次** `jit_callout_failed = 1`（或在 `debug/r6e-clib` 分支
   的 trace 里加一个"第 N 次调用强制失败"的开关）；
3. 看 `Start-Process … -PassThru` + `WaitForExit(ms)` 是否超时；并在 `main()` 的清理段
   **加 `print` 打点**（`audio.uninit` 前/后、`win.destroy` 前/后、`SDL3.quit` 前/后）
   定位挂在哪一步 ✓。

**结论**：不阻塞 R6-e 恢复（真因已修，正常路径不再产生 callout 失败）；
但**任何 callout 失败都必须能被用户看见并正常退出**，这条通道应单独排一轮 ✓。

***

### 8.100 修复：`is float` 在**歧义区**（+0.0 / 正次正规）JIT 与解释器分叉 —— 不猜，交回解释器（2026-09-17）

**症状**（新值域探针 `jit_probes/probe_type_check_float_zero.leno`）

```
JIT   : zero=50  negzero=2000 sub=2000 normal=2000 intzero=0
NOJIT : zero=2000 negzero=2000 sub=2000 normal=2000 intzero=0
```

⇒ `zero` **50 vs 2000** —— 50 正是**热阈值**：前 50 次解释执行（判真 ✓）、JIT 接管后判假 ✗。

**根因**（与 §8.99 同源）：JIT 虚拟栈用**裸位型**表示数值 —— int48 = 裸 int64、float = 裸
double；而 `OP_TYPE_CHECK` 的 codegen 用 `EMIT_RAW_TO_VALUE()`（`x86_64.c:641`，启发式
`raw>>47 ∈ {-1,0}` ⇒ 当 int48 装箱）把操作数转成 Value ⇒ 浮点 **0.0**（位型全 0）落进该区间
⇒ 被贴成 **int 0** ⇒ `0.0 is float` 判假（解释器判真）✗。

**值域分析（为什么只有 +0.0 分叉）**

| 值 | 位型 | `raw>>47` | 结果 |
| --- | --- | --- | --- |
| float **+0.0** | `0x0` | `0` ⇒ **装箱成 int** | ✗ 假（本 bug）|
| float -0.0 | `0x8000…` | 大负（≠ -1/0）⇒ 不装箱 | ✓ 真 |
| float 3.5 | `0x400C…` | 大正 ⇒ 不装箱 | ✓ 真 |
| 正次正规 `0x1` | `0x1` | `0` ⇒ 装箱 | 理论上同病；探针里 `sub` 却一致 ⇒ **待观察**（那条路径未经过此装箱）|

**修法（不猜）**：期望类型属浮点族（`TYPE_FLOAT/F32/F64`）**且** `raw>>47 == 0`
（⇔ `0 ≤ raw < 2^47`，即 int48 与 float 位型**重合的唯一区间**）⇒ **bailout**，
交解释器按真实类型判定。

**为什么不能"猜"**：这条信息在裸表示里**已经丢失** —— int 0 与 float 0.0 位型完全相同，
无法从位型恢复；`RBX` 位图也不行（排查确认它标的是"**非 int**（float 或 object/null/bool）"，
不是"是 float"）。

**代价与实测**

- 代价面：只有"对 **{0, 正次正规}** 做 `is float`"才回退（新探针里 `hitsIntZero` 也会 bail —— 正确但不必）。
- 三真实应用：**fm `Bailouts` 3（未变，仍是 FOR_PREP）**、cc **0**、五子棋 **0** ⇒
  新站点**一次都没触发**；`sum` 逐字一致、`upf` 全在噪声内（3830 / 5527 / 2977 vs 3791 / 5490 / 2995）。
- 13 探针 JIT/NOJIT **IDENTICAL**、assert **311 passed / 0 failed**。

**待决（同一表示缺口的另一面，未修）**：`int 0 is int` 方向 —— 歧义区里若值实际是 float，
JIT 会把它判成 int（如 `0.0 is int` JIT 判真）。要修得在同族检查上也 bail，但那会让
**每次对非负小整数做 `is int` / `switch case int`** 都回退（`x is int` 是常见形态）⇒
**先量其热路径占比再定**（用 `Bailout:` 行的 `触发指令=OP_TYPE_CHECK` 即可观测）。

**根因层面**：JIT 的"裸位型"表示抹掉了 int/float 之分 —— 彻底修须给 TOS 保留类型位
（或改用带标签的 float 表示），成本远超本缺口 ⇒ 记录在此，不再展开。

***

### 8.101 【部分修复 / 定位未完成】float 形参在 JIT 调用边界被贴成 int（2026-09-17）

**症状（忠实值域探针 `jit_probes/probe_float_param_faithful.leno`）**

```leno
func fmtFloat(float p): string { return "" + p }   // 格式化放在 callee 侧（解释器）
```
⇒ JIT `fmt=[0]` ✗ vs `LENO_NO_JIT=1` `fmt=[0.0]` ✓ —— 声明为 `float` 的形参**在 callee 里已经是 int**。

**机制**：JIT 虚拟栈用裸位型表示数值 ⇒ 实参经 `jit_raw_to_value`（启发式 `raw>>47 ∈ {-1,0}` ⇒ int48）
转成 Value 时，浮点 **0.0**（位型全 0）被贴成 **int 0** ⇒ 之后 VM 侧的形参提升
（`vm_call.inc:101-109`，逻辑正确）已无从恢复（它只把 int 升成 float，值已是 int 0）。

**本轮已落地（正确但未覆盖到本探针的路径）**：抽 `jit_promote_arg_by_decl(a, fn, i)` —— 按 **callee 的
声明形参类型**补齐提升（float←int / float←bigint / int←float，与 `call()` / `jit_try_hot_func_call` 逐条对齐），
并应用到 4 处 callout 实参边界（`jit_callout_global_func` 的函数级快路径、`jit_invoke_closure` 的函数级快路径
与其 **VM 重入**、另一处闭包快路径），另补「全局函数槽可以是**裸 `OBJ_FUNCTION`**」的提取分支。
原则与 §8.99 / struct 字段赋值一致：**跨类型敏感边界按声明类型兜底，不只信位型启发式**。

**为什么本探针仍红（已排除的路径 + 已知证据）**

| 证据 | 含义 |
| --- | --- |
| 4 处补丁应用后探针**无变化** | 污染点不在这些 callout 的实参填充处 |
| 在 `jit_callout_global_func` 的**函数级快路径**内加临时仪器 ⇒ **零输出** | 该路径**没被走到**（`jfn == NULL`，callee 未被函数级 JIT 编）|
| `vm_call.inc:101-109` 有正确的形参提升 | 到 VM 时值已是 int 0 ⇒ 污染在其**上游**（JIT→VM 交接）|

⇒ **下一步（精确）**：在 `jit_invoke_closure` 的 **VM 重入前**（`vm_stack_push` 循环）与
codegen 发出的 `EMIT_CALL(jit_callout_global_func)` 之后各自加一行仪器，打印
`callee 类型 / 声明 param_types / 每个槽的 raw→Value`，逐点缩小；`jit_raw_to_value` 全库约 24 处调用点
需按"是否跨类型敏感边界"分类治理。

**两个跟踪探针（当前**故意**保留 DIFF，不是门禁项）**

- `probe_float_param_faithful.leno`：上面这条（未修完）。
- `probe_is_int_ambiguous.leno`：§8.100 的**反向**分叉 —— 值实际是 float 0.0 时 JIT 判 `is int`
  为真（`hitsInt=2950` ✗ vs 0 ✓）；修它需在 int 族检查上也 bail，代价是"对非负小整数做 `is int`"
  全部回退 ⇒ **先量热路径占比**。

#### 8.102 方法论：`is` 观测会被同一歧义污染（两条 confound）

本次最初两个探针都**误报**了，值得记下来：

1. 用 `is float` 观测 ⇒ 被 §8.100 的**歧义区 bailout** 兜住（回退解释器）⇒ 缺陷被**掩盖**；
2. 改用 `is int` 观测 ⇒ 恰好命中**同一歧义的反向**（float 0.0 的位型被贴成 int）⇒ **假阳性**；
3. 连 `"" + v` 都不行 —— 当它在**调用方 JIT** 里执行时，会经过 concat callout 的同一个启发式 ✗。

⇒ **类型污染的观测口径必须"忠实"**：把转换/格式化放到**不受该启发式影响的一侧**
（本项目里 = 放进 callee 由解释器执行，见 `probe_float_param_faithful.leno`），
或直接比对**值本身**。**凡是跨 JIT 边界的类型判定，都别只在调用方一侧观测。**

***

### 8.103 【已修】int/float 歧义区不再"猜"：类型判定族 + concat 全部保守化（正确性优先）（2026-09-17）

**方针**（用户定的优先级：**正确性 > 效率**）：JIT 裸位型表示里，浮点 **+0.0 / -0.0 / 正次正规数**
与 **int48** 位型重合 ⇒ 任何"从裸位型猜类型"的地方都会**静默**把 float 当 int。
彻底修 = 让 JIT 在这些地方**不猜**：**能用声明类型就用，不能就交回解释器**。

**本轮修的两类边界**

1. **类型判定族（`OP_TYPE_CHECK`）**：期望类型属**数值族**
   （`TYPE_INT` / `TYPE_FLOAT` / `TYPE_I8…TYPE_C_SSIZE`）**且** 操作数 `raw>>47 == 0`
   （⇔ `0 ≤ raw < 2^47`，即 int48 与 float 位型重合的唯一区间）⇒ **bailout** ✓。
   覆盖 `is int` / `is float` / `switch case int|float` 的**两个方向** ✓。
2. **值物化边界（`OP_ADD` 的 concat 段）**：任一操作数落歧义区 ⇒ **bailout** ✓。
   **这是本轮定位到的真正污染点**：`fmtFloat(float p) { return "" + p }` —— `"" + p` 编译成
   **generic `OP_ADD`**（不是 `OP_STRING_ADD`），而 callee **自己也被函数级 JIT 编**
   ⇒ concat 段的 `EMIT_RAW_TO_VALUE` 把浮点 0.0 贴成 int 0 ⇒ 拼出 `"0"`（解释器 `"0.0"`）✗。

**定位过程（两个"零输出"仪器 + 字节码转储）**：在 `jit_callout_global_func` 入口与
`jit_invoke_closure` 的 **VM 重入**各加一行仪器 ⇒ **都没触发** ✗；配合 `LENO_JIT_DEBUG=1` 的
`COMPILE: fn='main' … inline=0`（未内联 ✗）、和 `--debug-out` 转储（`fmtFloat` =
`OP_CONST ""` / `OP_GET_LOCAL 0` / **`OP_ADD`** / `OP_RETURN`）⇒ 锁定污染在 **callee 内部那条 `OP_ADD`** ✓。
⇒ 教训：**"忠实探针"必须确认 callee 自己没被 JIT 编**，否则观测点仍在启发式之后 ✗（§8.102 的延伸）。

**验证：15 探针全绿 + 真实应用零代价**

| 探针 | 修前 | 修后 |
| --- | --- | --- |
| `probe_float_param_faithful` | `fmt=[0]` ✗ | **`fmt=[0.0]`** ✓ |
| `probe_float_param_int0` | `hitsF=50 hitsI=2950` ✗ | **`hitsF=3000 hitsI=0`** ✓ |
| `probe_is_int_ambiguous` | `hitsInt=2950` ✗ | **`hitsInt=0`** ✓ |
| 其余 12 个 | IDENTICAL | IDENTICAL ✓ |

三应用（headless 300 帧）：`sum` 逐字一致 ✓、**`Bailouts` 无新增** ✓（fm 3 = 仍是 FOR_PREP、
cc 0、五子棋 0）、`upf` 全在噪声内（3803 / 5559 / 2941 vs 3791~3830 / 5490~5527 / 2977~2995）
⇒ **"不猜"的守卫在真实应用里一次都没触发** ✓；assert **311 passed / 0 failed** ✓。

**延伸原则（与 §8.99 / §8.101 同一条线）**：凡"从裸位型推断类型"的边界 ——
- **声明类型可得** ⇒ 用它 ✓（形参 `jit_promote_arg_by_decl` 已覆盖 4 处 callout 边界、
  struct 字段赋值 ✓、FFI 实参 ✓）
- **不可得** ⇒ **bail** ✓（类型判定 ✓、concat ✓）
- **剩余待办**：其余泛型值边界（容器写入 / `_string()` / 其它 callout）按同一原则过一遍；
  审计口径 = `jit_raw_to_value` 全库约 **24** 处调用点逐个判定"是否跨类型敏感边界" ✓。

**代价与后续**：本轮 bail 会让"小非负整数参与 concat / `is int` 判定"的循环回退解释器（实测三应用**未触发** ✓）；
若将来在别处成为瓶颈，正解是给 **TOS / 栈槽补静态类型跟踪**（或不再擦除 int 标签 ✓），届时可回收 ✓。

***

### 8.104 用运行时类型位图（RBX）治"值物化"边界：来源槽已知时**跳过错误装箱**（2026-09-17）

**背景**：§8.103 的"不猜"守卫覆盖了**类型判定**与 **concat** 两类边界，但**其余泛型值边界**
（容器写入、返回值、upvalue 捕获、switch 值、通用比较…）仍在用 `EMIT_RAW_TO_VALUE` 的位型启发式 ✗。
关键观察：若操作数来自**局部量**，它的真实类型**已经记录在 RBX 类型位图里** ✓
（prologue 按入参"是否 int48"建立、写回路径也按它判定 ⇒ **bit=1 = 该槽不是裸 int48**
= 裸 double 的 float / 对象 / bool / null，**这些全都已经是合法的 Value** ✓）。

**修法（一处宏 + 两处跟踪，不动任何 callout）**

- codegen 主循环每轮重置 `tos_from_si = -1`；只有 `OP_GET_LOCAL` 把它置成 `si`
  ⇒ 只对"**紧跟 GET_LOCAL 的那次转换**"生效，绝不外溢到别的操作数 ✓
- `EMIT_RAW_TO_VALUE`：当 `tos_from_si ∈ [0, 64)` 时先 `BT RBX, si` + `JC` **跳过装箱** ✓；
  其余情形保持原启发式 ✓

**为什么这是精确的（不是又一次"猜"）**：bit=1 ⇒ 值**不是**裸 int48 ⇒ 装箱本来就不该发生 ✓；
跳过 ⇒ 值原样作为 Value 通过（float 保持裸 double ✓、对象/bool/null 保持 NaN-boxed ✓）
⇒ **只有"原本被错误贴成 int 的浮点"改变行为** ✓，其余路径与改前逐字节同效 ✓。

**验证**：16 探针全 IDENTICAL（含三个原分叉项 ✓）、assert **311 passed / 0 failed** ✓；
三应用 `sum` 逐字一致 ✓、`Bailouts` 无新增（fm 3 = FOR_PREP、cc 0、五子棋 0 ✓）、
`upf` 全在噪声内（fm **3786**；cc **5516 / 5472 / 5491** —— 先前一次 5796 经复测确认是噪声 ✓；
五子棋 **2950** ✓）。

**残余与正解**（延续 §8.103）：来源**不是局部量**时（中间表达式结果、tmp 槽、callout 返回值）
仍无类型信息 ⇒ 该情形由 §8.103 的守卫兜底（bail ✓，正确但慢 ✓）。
**彻底解**仍需给 **TOS / 栈槽补静态类型跟踪**（或让 int 永不带裸表示 ✓）——
届时可同时去掉守卫、把性能收回来 ✓。

***

### 8.105 【已修】JIT 区域内构建的**容器字面量**会污染元素类型（污染值能活过循环）（2026-09-17）

**探针 `jit_probes/probe_array_literal_float.leno`**：`var a = [z]` / `{"v": z}` 在 **JIT 热循环**
里构建 → 存入**全局** → **循环外**用解释器读回：

```
JIT   : g=[0]        ✗
NOJIT : g=[0.0]      ✓        （Compiled: 1, Bailouts: 0 ⇒ 循环确实在 JIT 里跑，不是被守卫掩盖）
```

**根因**：字面量元素由 **callout 内部**转换（`jit_callout_array_new` / `jit_callout_dict_new`
里的 `jit_raw_to_value` 位型启发式）⇒ 那里**拿不到"来源槽"** ⇒ §8.104 的 RBX 位图判据用不上 ✗
⇒ 浮点 0.0（位型全 0）被贴成 int 0 并**存进容器**。这次循环内没有类型敏感操作 ⇒ 不 bail
⇒ **污染值活过循环**（真实数据污染，不是观测假象）。

**修法**：`OP_ARRAY` / `OP_DICT` 的 codegen 在交给 callout **之前**逐个检查元素 raw
（此刻元素就在 `RSP` 上）⇒ 任一 `raw>>47 == 0`（歧义区）即 **bailout** ⇒ 交解释器按真实类型建容器 ✓。

**探针可达性三坑（避免以后再空转）**

1. 字面量放**单独小函数**里 ⇒ 函数级 JIT 不编它（实测只编 `main`）⇒ 字面量在解释器里建 ⇒ 空转 ✗；
2. 字面量在 JIT 循环里、但**同循环内**还有 `"" + a[0]` ⇒ concat 被 §8.103 守卫 ⇒ 整循环拉黑、
   退回解释执行 ⇒ 结果被"掩盖"成正确 ✗；
3. **正确形态 = JIT 区域内建 → 存进全局 → 循环外用解释器读回** ✓（本探针）。

**验证**：17 探针全 IDENTICAL ✓、assert **311 passed / 0 failed** ✓；三应用 `sum` 逐字一致 ✓、
`Bailouts` 无新增（fm 3 = FOR_PREP、cc 0、五子棋 0 ⇒ **守卫在真实应用零触发** ✓）、
`upf` 全在噪声内（fm **3723** / cc **5507** / 五子棋 **2982** ✓）。

**剩余（同类"callout 内部转换"，同源同解）**：闭包捕获块、native 方法 / 模块调用实参、
属性接收者等 —— 都在 callout 里转、都没有来源槽信息 ✗；可按本手法继续逐个推广，
或由 §8.103 的守卫整体兜底 ✓。**彻底解**仍是给 **TOS / 栈槽补静态类型跟踪** ✓。

***

### 8.106 【**误判已纠正** / 真因已定位 / 待修】float 裸位型落进歧义区时，JIT 的**物化/写回**会把它当 int 装箱（2026-09-17）

**探针 `jit_probes/probe_native_arg_float.leno`** —— 口径必须用**次正规数**：
`0.0` 不行 ✗（`_str(0.0)` 本来就输出 `"0"`，与 `_str(0)` **无法区分**，实测两边都是 `"0"`）；
改用 **2^-1074**（位型 `0x1`，撞码对象正好是 **int 1**）后一眼可辨：

```
JIT   : cast:   tiny=[1]                     native: tiny=[1.0]                     ✗
NOJIT : cast:   tiny=[4.94066e-324]          native: tiny=[4.9406564584124654e-324] ✓
```

⇒ 不只是"类型错"—— **量级都变了**（5e-324 → 1）✗。机制与 §8.105 同源：callout 内部
`jit_raw_to_value` 的位型启发式把 float 的裸位型贴成 int 字面量。

**本轮已落地**：新增判据 `jit_raw_block_ambiguous(vstack_top, n)`（实参块里是否存在
`raw>>47 == 0` 的歧义区值），并在 **4 处 native 实参边界**加守卫：`jit_callout_module_call`
的两个变体、`jit_callout_call_native`、`jit_invoke_closure` 的 native 直调。

**仍未修（探针依旧红）**：字节码转储确认 `_str` / `print` 编译成 **`OP_CALL_NATIVE`** 专用
opcode ✓（`ops_callout.inc:1223`，先解析 native 指针、`_int`/`_float` 有专用内联快路径），
它的**通用 callout 路径尚未定位** ✗ —— 下一步就是在这个 case 里找出 `EMIT_CALL(jit_callout_…)`
后把同一判据接上去。**注意**：这也解释了为何先前的 4 处守卫"零触发"（`Bailouts: 0`）。

**门禁**：17 探针 IDENTICAL ✓、assert **311 passed / 0 failed** ✓（本探针作为**跟踪项**保留 DIFF）。

#### 8.106.1 纠正：初版归因（native 实参 / `_str` / `print`）是**错的**，真因在**物化/写回**

**证据链**：① 仪器确认 `jit_callout_call_native` **零调用** ✗ ⇒ 那几行打印是**解释器**执行的，
native 路径无辜 ✗（也就解释了 4 处新守卫为何 `Bailouts: 0` —— 它们不会被触发）；
② 用 `2^-n` 夹逼（`probe_tiny_make_where.leno`）把出错档位钉死：

| n（2^-n）| 位型 | 是否在歧义区 | JIT | NO_JIT |
| --- | --- | --- | --- | --- |
| 1021 / 1022 / 1023 | ≥ `0x0008_0000_0000_0000` | 否 | 正确 ✓ | 正确 ✓ |
| **1060** | `0x4000` | **是** | **16384.0** ✗ | 8.0948e-320 ✓ |
| **1074** | `0x1` | **是** | **1.0** ✗ | 4.94e-324 ✓ |

⇒ JIT 给出的数值**恰好等于该 float 的位型当作整数**（`0x4000`=16384、`0x1`=1）✓✓
—— 这是"**裸 double 被按 int48 装箱**"的铁证 ✓，与"打印/native"无关 ✓。

**真因（已定位，待修）**：JIT 的**局部量写回**路径（`x86_64.c` 的写回宏）用**位型启发式**决定是否
重新装箱（`sar 47; inc; cmp 1; ja .raw_wb`），浮点裸位型落进 `[0, 2^47)` 时会被判成 int48
⇒ 装箱为 `int(raw bits)` ✗。`RBX` 类型位图本可作判据 ✓，但它**只在 prologue 按入参建立** ✓、
后续 store 不维护 ✗ ⇒ 不可靠（这也正是 §8.104 那只在"来源槽"场景生效的原因）。

**正解（建议，未实施）**：让 `RBX` 位图成为"该槽**当前**是否非 int48"的**忠实标志** ——
在所有 store / 写回路径按被存值的实际类型**置位/清位** ✓ ⇒ 写回、§8.104、`FOR_LOOP` 的
float 判定**三处同时变正确** ✓。代价：每次 store 多几条指令（按既定方针：正确性优先 ✓）。

#### 8.106.2 两个实验（都否定性，但把范围钉得更小）

1. **分离"取操作数"与"返回/写回"**（`probe_tiny_fetch_or_store.leno`）：`half(x) = x * 0.5` 与
   `keep(x) = x` 分别作用于同一个 2^-1060：
   ```
   JIT   : tiny=16384.0   half=8192.0      keep=16384.0      ✗
   NOJIT : tiny=8.09e-320 half=4.047e-320  keep=8.09e-320    ✓
   ```
   ⇒ `half` 的**乘法本身正确**（16384×0.5=8192 ✓），而 `tiny` **已经是** 16384.0 ✗
   ⇒ 污染发生在**产生/落下这个值的那一步**，不在后续运算 ✓。
2. **把写回的"重新装箱"整个停掉**（临时实验）：症状**完全不变** ✗ ⇒ **写回不是污染点** ✓
   （该实验已回退）。

**收敛后的怀疑点**：`EMIT_NUM_TO_XMM`（`x86_64.c:760+`）的 **int48 分支** —— 它对落进歧义区的
**裸 double** 会按 int48 走 `CVTSI2SD` 提升 ✗（注释里"数值上一致"只对**真整数**成立；
对裸位型在 `[0, 2^47)` 的浮点就把 `0x4000` 当 16384 算 ✗）。修法需"**位图 + 操作数来源槽**"
（同 §8.104 手法 ✓：来源槽已知 ⇒ 按位图判为 float ⇒ 走 `MOVQ` 直搬 ✓；未知且落歧义区 ⇒ bail ✓）。

#### 8.106.3 一次**已回退**的尝试：位图忠实化（§8.107 草案）

按 8.106.1 的建议实现了"写槽即维护 RBX 位图"（在 `EMIT_STORE_LOCAL` 里加 `BT/置位/清位`），
但**打破**了 `probe_cstruct_jit` / `probe_float_param_faithful`（IDENTICAL → DIFF ✗）且三应用无输出 ✗
⇒ **已整块回退** ✓。可能原因：临时寄存器 R8 在那些站点上是**活值** ✗（或 rel8 补丁算错 ✗）。
下次重做时应该：① 先确认每个 store 站点的**活寄存器集合**；② 或用不占临时寄存器的判据。

**另记（待清理）**：§8.106 的 4 处 native 实参守卫（归因已被证伪 ✗）会拦"小整数实参"，
三应用 `Bailouts` 因此从 3/0/0 升到 **24/12/3** ✗（性能未见影响：`upf` 与 `sum` 都在基线内 ✓）
⇒ 建议随后**移除**（保留判据函数即可），把基线还给后续实验 ✓。

***

### 8.109 反汇编取证：歧义位型的浮点走 `CVTSI2SD`；且浮点局部量**被 pin 在寄存器**（2026-09-17）

**手段**：`LENO_JIT_DUMP=1` ⇒ `jit_mc_dump.txt`（十六进制文本）+ `jitdump<N>.bin`（原始机器码）；
用 PowerShell 脚本按字节模式定位（`F2 0F 59` = MULSD、`F2 48 0F 2A` = CVTSI2SD r64、
`66 48 0F 6E` = MOVQ xmm,r64 —— 注意 CVTSI2SD 带 **REX.W**，只搜 `F2 0F 2A` 会漏 ✗）。

**发现（最小用例 `jit_probes/probe_mul_float_min.leno` 的循环机器码）**

```asm
    49 89 d0                mov  r8, rdx
    49 c1 f8 2f             sar  r8, 47
    49 ff c0                inc  r8
    49 83 f8 01             cmp  r8, 1
    0f 87 0a 00 00 00       ja   .not_int48
    f2 48 0f 2a c2          cvtsi2sd xmm0, rdx      ← 歧义位型（2^-1060 的 0x4000）走**这条** ✗
    e9 18 00 00 00          jmp  .done
.not_int48:
    66 48 0f 6e c2          movq xmm0, rdx          ← 只有"非 int48"才搬位 ✓
    f2 0f 59 c1             mulsd xmm0, xmm1
    66 48 0f 7e c0          movq rax, xmm0
```

⇒ ① 代码与设计**完全一致** ✓ —— 缺陷就是"浮点裸位型落进 int48 区间时走了整数提升" ✗
（注释里"数值上一致"只对**真整数**成立）；
② 操作数来自 **`r14`**（`mov rax, r14`）⇒ **浮点局部量 `t` 被 pin 在寄存器** ✓
⇒ **这解释了为什么前两次"维护写回 / 内存侧位图"都没碰到它** ✗（值根本不在内存 scratch 里）；
③ 也说明 `pick_pin_locals` 选中的槽，其类型信息（位图）必须**随寄存器 store 一起维护** ✓。

**据此确定的修法（下一步，逐步小改、每步跑门禁）**

1. **只在"静态已知类型"的 store 上维护位图**：给 codegen 加 `tos_kind ∈ {UNKNOWN, INT, FLOAT}`
   （`OP_*_FLOAT` 结果置 FLOAT、整数运算置 INT、其余 UNKNOWN），在 `EMIT_STORE_LOCAL` 里按它发
   **单条** `BTS/BTR RBX, si` ✓ ⇒ **不需要任何临时寄存器** ✓（前两次失败的原因之一正是借
   R8 / RSI 踩了活值 ✗ ⇒ 这次从根上避开）。
2. `tos_kind` 未知时才用运行期判据 —— 这一档**先不动** ✓（一次只改一处 ✓）。
3. 把 §8.108 的"来源槽提示"改成**跨指令粘性** ✓：按"每次入栈"更新历史 ✓，
   而不是每条指令清零 ✗ —— 上次清零导致提示恒为 -1 ✗（`t * 0.5` 的 `GET_LOCAL t` 在**前一条**指令里）。

***

### 8.110 【已修】浮点结果的**正次正规**会毁掉整条计算链 —— 在结果处 bail（2026-09-17）

**症状**（四个探针：`probe_mul_float_min` / `probe_tiny_make_where` / `probe_tiny_fetch_or_store` / `probe_native_arg_float`）

```
JIT   : 1060=16384.0    1074=1.0        ✗   ← 数值恰等于"该 float 的位型当整数"
NOJIT : 1060=8.09e-320  1074=4.94e-324  ✓
```

**根因**（§8.106.2 → §8.106.3 → §8.109 三步取证的最后一块）：JIT 虚拟栈用**裸位型**表示数值
⇒ 位型落进 `[0, 2^47)` 的浮点（**+0.0** 与**正次正规数**）与 int48 **撞码** ⇒
下游任何"从位型推断类型"的环节（取操作数的 `CVTSI2SD`、物化装箱、FFI 分发）都会把
次正规数的**位型**当成整数 ⇒ 数值彻底失真 ✓（`0x4000`→16384、`0x1`→1 ——
"输出恰等于位型当整数"是判据级铁证 ✓）。

**修法（一处收口）**：`EMIT_MOVQ_RAX_XMM0()`（只用于**浮点结果**：add/sub/mul/div/neg/cast_float）
在写回 RAX 后加守卫：

```
r8 = raw >> 47
  非 0                              ⇒ 不在歧义区 ⇒ 放行（负次正规 / 负数 / 已归一化 NaN ✓）
  == 0 且 raw != 0                  ⇒ 正次正规 ⇒ **bailout**（交解释器算这一轮）
  == 0 且 raw == 0（+0.0）          ⇒ 放行
```

- **为什么"放行 +0.0"安全**：`+0.0` 与 `int 0` 在**数值**上不可区分 ⇒ 算术结果一致 ✓；
  需要区分它们的只有**类型敏感**操作（`is`/`as`/concat/FFI）⇒ 那些边界已分别由
  §8.100 / §8.103 / §8.106 的守卫兜住 ✓。
- **为什么"bail 次正规"代价可忽略**：正常代码几乎不产生次正规数 ✓ —— 实测三应用
  `Bailouts` **无新增**（fm 21† / cc 12† / 五子棋 3†）、`upf` 全在噪声内（3849 / 5495 / 2979
  vs 3791~3830 / 5490~5527 / 2977~2995 ✓）。
  † 这些既有计数来自 §8.106 的 native 实参守卫 —— 当初的**归因**是错的 ✗，
  但该守卫对"**从内存**来的次正规"仍是**载荷路径** ⇒ 保留 ✓。

**验证**：**21 探针全 IDENTICAL** ✓（含四个原红项 ✓）、assert **311 passed / 0 failed** ✓；
`probe_mul_float_min` 的诊断行确认走的正是新守卫（`Bailout: fn='mulTo' … 触发指令=OP_MUL_FLOAT` ✓）
⇒ 机制与设计一致 ✓。

**残余（诚实记录）**：若次正规数**从内存**（全局量 / 容器 / FFI 返回值）进入 JIT，取操作数仍会误读 ✗
⇒ 由上面那些类型敏感守卫兜住 ✓；表示层的彻底修复（TOS 带类型位 / float 带标签）仍记录在 §8.100 末尾 ✓。

***

### 8.111 【环境陷阱，非 JIT 缺陷】`examples/性能测试/光线追踪对比.leno` 打印出 10^9 量级"毫秒" —— 入口缓存的**静态签名陈旧**（2026-09-17）

**症状**（用户实测，跑 `leno 光线追踪对比.leno`）

```
Phase A 数组索引: 3000000次循环(4次索引/轮): 1224928028ms (sum=50730000.000273108)
Phase B sphereHit多返回值: 3000000次调用:   3807095921ms (hits=900000)
Phase C 递归trace: 300000条射线(depth=5):  25997104854ms (hits=585500)
```

**先排除的三个嫌疑（都有探针，全部否掉）**

| 嫌疑 | 判据 | 结论 |
| --- | --- | --- |
| `times.ms()` 单位错了（返回 ns/µs）| `jit_probes/probe_times_units.leno`：`ms` delta 0.368 / `us` 367.9 / `ns` 367899 ⇒ 三者精确 ×1000 | ✗ 单位正确 |
| 浮点转字符串**丢小数点** | `jit_probes/probe_float_str_point.leno`：`1224.9280279999998` ✓（JIT 与解释器逐字一致）| ✗ 渲染正确 |
| 是 JIT 缺陷 / 性能回归 | 见下（清缓存后 35 / 110 / 109 / 832 ms）| ✗ 都不是 |

**真因：入口缓存 `entry_<hash>.lenb` 里存着**陈旧的静态签名** ✗**

- `893a0045` 把 `times.ms` 的**注册**由 `TYPE_INT` 改成 `TYPE_FLOAT`（`src/module/times/times.c`）✓；
- 但入口缓存的**键**只由【入口文件自身】的内容哈希决定（`main.c` 的 `entry_%llx.lenb`）✓，
  它的 `.deps` 依赖清单只登记**有源码的模块**（`loaded_modules` 的 `source_path`）✓ ——
  **native 模块（times/sdl3/…）没有源码路径 ⇒ 一条都登记不上** ✗；
- ⇒ 改 `times.c` 后，缓存**仍被判为有效** ✗ ⇒ 编译期继续按 `ms(): int` 生成代码 ✗
  ⇒ `t2 - t1` 对**两个 float 值**按整数路径相减 ⇒ 输出 10^9 量级的无意义整数 ✓。
- 为什么只有计时"看起来坏了"：`sum` 是普通 float（`+=` 出来的）✓ ⇒ 显示正常 ✓；
  只有**来自 `times.ms()` 的静态 int 值**参与运算时才出问题 ✓ —— 这正是"只坏在时间列"的原因 ✓。

**证据**（同一二进制、同一脚本，只把 `.lenocache` 改名）

| Phase | 清缓存前 | 清缓存后 |
| --- | --- | --- |
| A 数组索引 | `1137317406ms` ✗ | **`35.19ms`** ✓ |
| B 多返回值 | `3855145868ms` ✗ | **`109.69ms`** ✓ |
| B2 单返回值 | `4034628524ms` ✗ | **`108.68ms`** ✓ |
| C 递归 trace | `26576563050ms` ✗ | **`832.18ms`** ✓ |

**用户侧对策**：删掉该目录的 `.lenocache` 即可 ✓（本次只把它改名成 `.lenocache.bak` 保留现场 ✓，
确认无用后可删 ✓）。

**⚠ 真 bug（待修，与 §8.16「单一事实来源」同主题）**：入口缓存的有效性判定必须纳入
**native 模块签名**（例如对所有已注册 native 模块方法算一枚指纹：模块名 + 方法名 + 参数/返回类型
+ 元素类型 ✓），否则**任何 native 模块签名变化都不会让缓存失效** ✗ —— 本次的
`TYPE_INT → TYPE_FLOAT` 只是最容易被看见的一例 ✓（表现是"编译期类型与运行期值不一致"，
属**静默错误代码** ✗，比崩溃更坏 ✓）。

***

### 8.112 【已修】入口缓存的失效判定漏掉「原生模块 ABI」—— 新增运行中 exe 的指纹作为失效输入（2026-09-17）

**背景与事故**（§8.111 的根因那一面）：`893a0045` 把 `times.ms` 的注册由 `TYPE_INT` 改成
`TYPE_FLOAT`，但**旧 `.lenocache` 仍被判为有效** ✗ ⇒ 编译期继续按 int 生成 `t2 - t1`
⇒ 对两个 float 值按整数相减 ⇒ 基准脚本打印 10^9 量级的"毫秒"（**静默错误代码** ✓，
`examples/性能测试/光线追踪对比.leno` ✓）。

**为什么会漏**：`.deps` 的依赖清单只登记**有源文件的模块**（`loaded_modules` 的 `source_path`）；
而 `native` 模块（`times` / `sdl3` / `ffi` …）**没有源文件** ⇒ 一条都登记不上 ✗。
于是一整类"编译期输入"都不在失效判定里：**native 方法签名 / 模块常量 / 实例方法表 / 编译器求值语义** ✗。

**修法（取"最粗但最全"的那把尺）**：把**运行中可执行文件自身的指纹**
（`size + mtime + 内容 FNV-1a(64)`，`cache_runtime_binary_fingerprint()`，放在
`serialize.c` —— 与另外三个 source snapshot 同处、同一个"唯一实现"约定 ✓）写入 `.deps`
的第二行（格式标识随之 `LENODEPS1` → `LENODEPS2` ✓），加载缓存前比对，不符即失效并删除 ✗✓。

- **为什么用 exe 指纹而不是逐表枚举 ABI**：上述四类输入里"实例方法表"等没有便宜的枚举接口 ✗，
  逐表枚举必然漏表 ✗；而它们**任何一处变化都必须重新构建 exe** ⇒ 指纹必变 ⇒ 一次覆盖整类 ✓
  （代价：重建后所有入口缓存失效一次 ⇒ 每个程序重编译一次，可接受 ✓）。
- **失败方向 fail-closed** ✓：指纹取不到（返回 0）时**不写**缓存、读到 0 的清单判失效 ✓
  （POSIX 上 `readlink("/proc/self/exe")` 取不到的平台即属此列 ⇒ 宁可重编译 ✓）。
- **不升 `LENO_BIN_VERSION`**：`.lenb` 的**字节布局与序列化语义一字未改**，改的只是"什么时候认
  这份缓存"的外部判定条件 ✓ ⇒ 升它会让 `.lenomc` / `.lenosymc` 跟着无谓失效 ✗。
  已登记进 `docs/待办_单一事实来源与重复实现收敛.md` 第七节（表 + 实例四）✓。

**验证（e2e，复刻事故的形态）** ✓

1. 临时把 `times.ms` 改回**自洽的旧形态**（`TYPE_INT` 签名 + 返回 int ✓）⇒ 重建 ⇒ 跑基准：
   `Phase A 36ms / Phase C 795ms` ✓（自洽 ⇒ 正确 ✓），缓存里写下 `BIN  52c1c286a68fe65` ✓；
2. `git checkout` 恢复新形态（float 签名 + 返回 float ✓）⇒ 重建 ⇒ 再跑：
   **`Phase A 45.6ms / B 118.1ms / B2 112.9ms / C 821.2ms`** ✓✓ —— **不是** 10^9 量级 ✗，
   且指纹变为 `BIN  d10708efd1054dfd` ✓ ⇒ 旧缓存被**自动**判失效并重编译 ✓✓。

#### 8.112.1 补上另外两类缓存（`.lenomc` / `.lenosymc`）—— 同一把尺，全部收口（同日）

§8.112 只覆盖了**入口**缓存 ✓；`.lenomc`（模块编译产物）与 `.lenosymc`（模块符号表）同样
**只登记"有源文件的依赖"** ✗ ⇒ 同一类漏洞 ✗。现在两者都把 exe 指纹写进自己的 header 并校验：

| 产物 | 改动 | 版本 |
| --- | --- | --- |
| `.lenomc` | header 末尾追加 `bin_fp(8)`（大端 ✓）；`module_cache_read_source_snapshot` 与 `module_cache_deserialize` 两处读取点都校验 ✓；取不到指纹 ⇒ **不写** ✓ | `LENO_MODCACHE_VERSION` v9 → **v10** |
| `.lenosymc` | header 末尾追加 `bin_fp(8)`（宿主端序 ✓）；`sym_cache_deserialize` 校验 ✓；指纹在**打开文件之前**算 ⇒ 失败不留半截文件 ✓ | `LENOSYMC_VERSION` v25 → **v26** |

**这次必须升版本号**（与 §8.112 入口那处不同 ✗）：`.lenomc` / `.lenosymc` 的**字节布局真的变了**
⇒ 不升会让旧文件被按新布局错读 ✓（正是登记表第二节 S5 那次"数值相同、格式不同"的坑 ✓）。
两行都已更新进 `docs/待办_单一事实来源与重复实现收敛.md` 第七节 ✓。

**模块级 e2e 验证** ✓（fixture：`build/cachetest/{mod,main}.leno`，`mod` 自己调 `times.ms()`
并把差值返回 ⇒ 模块**自己的字节码**里烙着 int 还是 float 的减法 ✓）

1. 临时切到自洽旧形态 X（int 签名 + 返回 int ✓）⇒ 重建 ⇒ 运行：`mod elapsed = 5ms` ✓，
   并产出 `.lenomc 1421B` + `.lenosymc 104B` + 入口缓存 ✓；
2. `git checkout` 恢复 Y（float 签名 + 返回 float ✓）⇒ 重建 ⇒ 再运行：
   **`mod elapsed = 8.196 ms`** ✓✓ —— 不是 10^9 量级 ✗ ⇒ 两类模块缓存都被**自动**判失效并重扫 ✓。

**残余（诚实记录）**：指纹取不到的平台（无 `/proc/self/exe` 且非 Windows ✓）会退化成
"每次都重编译/重扫" ✓ —— 安全方向 ✓，不是错误方向 ✓。

***

## 9. 性能数据

### 测试环境

- **机器：Intel i5-3450**（Ivy Bridge，4 核 4 线程）+ Windows。
  该机负载波动大（同一基准各轮可差 10\~20%），所有耗时**只做同机同轮对照**，
  不要与本机之外的绝对值比较；换机器后需要重新建立基线。

- 测试文件：`examples/性能测试/全部测试.leno`（539 行，含算术/函数调用/数组字典/try/while vs for/fib 对比）

- 编译命令：`cmd //c build.bat`

- 运行命令：`./build/lenojit.exe "examples/性能测试/全部测试.leno"`

### JIT 统计

| 指标       | 值     |
| -------- | ----- |
| Compiled | 35    |
| Executed | 20032 |
| Bailouts | 3     |
| Cached   | 31    |
| Tried    | 34    |

### 关键性能对比

| 测试项                   | JIT   | 无 JIT  | 加速比    |
| --------------------- | ----- | ------ | ------ |
| 1 亿次 i++              | 78ms  | —      | —      |
| 1 亿次 a = b            | 47ms  | —      | —      |
| 1 亿次 dict\[key]=value | 906ms | 1781ms | \~2.0x |
| 1 亿次 arr.add()        | 625ms | 1359ms | \~2.2x |
| for 1000 万次 i++       | 343ms | —      | —      |
| while 1000 万次 i++     | 422ms | —      | —      |
| 嵌套 for 10000×10000    | 78ms  | —      | —      |
| 嵌套 while 3000×3000    | 15ms  | —      | —      |
| 三层嵌套 while 20×20×20   | <1ms  | —      | —      |

### 回归测试

```
Results: 270 passed, 0 failed (total 270)   // JIT 与 LENO_NO_JIT=1 两种模式均通过
```

### 通用算术 float 快路径（2026-09-11）

| 测试项 | JIT | 无 JIT | 加速比 |
| --- | --- | --- | --- |
| 计算热循环 200 万次（通用 `1.0 - maths.abs(d)` + 通用 `>` + `rsqrt` 累积 + 限幅） | 164ms | 422ms | **2.56x** |

两侧输出逐位一致（`acc` / `s` 完全相同）。

`LenoSDL3/examples/特效动画/ripple_image.leno` JIT 统计对比：

| 指标 | 修复前 | 通用 float 快路径 | 倒序 for 支持后 |
| --- | --- | --- | --- |
| Executed | 424 | 5173 | 3557 |
| Bailouts | 9 | 3（全部为负步长 for） | **0** |

> `Executed` 随机波动较大（波纹位置随机 → 各循环达到热度阈值的时间不同），
> 关注 `Bailouts` 归零即可。

> 该示例自带的 `平均 FPS` 无法体现收益：它每帧 `SDL3.delay(16 - elapsed)` 且开了
> VSync，帧时间被钉在 16ms。要看收益请用 `Executed`/`Bailouts` 或纯计算基准。

### ffi 定宽内存读写内联（2026-09-12，见 §2.6）

复刻像素直写内层：每迭代 `write_byte`×1 + `read_byte`×4，共 3,000,000 次迭代
= **15,000,000 次 ffi 调用**（`build/ffiscratch/t_bench.leno`，对照基线
`build/lenojit_base.exe`）：

| 版本 | 耗时（3 次均值） |
| --- | --- |
| 改动前（通用 callout） | 334 ms |
| 改动后（内联） | 221 ms |

**约 -34%**，折算每次 ffi 调用省下 ~7.5ns（callout 本身 ~20ns，其余开销在循环与访存）。
两侧 JIT 统计均为 `Compiled=1 Cached=1 Bailouts=0`，即整个热循环都命中 JIT、无回退。

### 类型化浮点操作数提升的开销（2026-09-12，见 §8.18）

`EMIT_FLOAT_ARGS2` 给每个浮点操作数加了 4-5 条 int48 判定，代价可测但可控：

| 测试项 | 提升前 | 提升后 | 说明 |
| --- | --- | --- | --- |
| `ripple_image.leno` 平均每帧 | 13.4 ms | 14.8 ms | 约 +5% |
| 同上（`LENO_NO_JIT=1`） | 23.2 ms | 23.2 ms | 解释器基准，JIT 仍快 **1.6x** |
| 同示例 `Bailouts` | 0 | **0** | 提升本身不引入回退 |

换来的是一致性：`acc + o.get("k", 0.0)` 这类「float 局部量 + 动态类型调用结果」在 JIT 下
不再把 int48 位模式当次正规 double 加（§8.18，修复前 600 轮累加只得 17）。

### 递归 / 函数调用场景的基线（2026-09-13）

`build/bench_rec.leno`（`build/` 不入库），6 次取中位数，基线与改动后同机对照：

| 场景 | 说明 | 基线 `66ace270` | 改动后 | 加速比 |
| --- | --- | --- | --- | --- |
| A 顶层 `fib_recursive(30)` | 纯递归，调用链起点在解释器 | 93\~94 ms | **31\~32 ms** | **3.0x** |
| B 热循环调 `fib_recursive(10)`×30000 | 走既有「循环 JIT → callout → 函数级 JIT」 | 47\~63 ms | 47\~78 ms | 1.0x（无回归） |
| C `fib_tail(30)` | 30 次尾调用，量级太小 | 0 ms | 0 ms | — |
| D `mix(2000)`（循环 + 一次递归） | — | 0 ms | 0 ms | — |

`nonTail(200000)`（深线性递归）：基线 47 ms / 改动后 47 ms —— **无收益**，
因为 `jit_func_depth` 上限 64，200000 ≫ 64（见 §8.25 边界表）。

> 注意本机各次运行的绝对耗时波动较大（`i++` 1 亿次实测 78\~172 ms，随机器负载变化），
> 所以上表只做**同机同轮对照**，绝对值不要跨文档比较。

### 与上一轮评估的核对（2026-09-13）

上一轮评估给了一条很有价值的结论（函数级 JIT 触发点与循环回边耦合 → 递归 1x），
但也有**三处已经过期**，会直接影响优先级排序，逐条核对如下：

| 评估中的说法 | 核对结果 |
| --- | --- |
| ①「还没做虚拟栈寄存器化，每次运算带 12 次栈访存，P0 估计还有 3\~5x」 | **过期**。单元素 TOS 寄存器缓存（`TOS_SPILL` / `TOS_CONSUME_*` / `TOS_PRODUCE`）2026-09-06 已在 `x86_64.c` 落地。依据：`i++` 1 亿次 78\~172 ms（≈2.5 cycle/iter）、`a=b` 47\~94 ms，已经贴着「int48 装箱 + 溢出检查」的标量循环下限；1 槽扩 2 槽的剩余空间是「少几次 L1 命中」，不是数量级。**不建议现在做。** |
| ②「收尾两套表示：比较 opcode 压裸 0/1 vs `val_bool`，改了就行」 | **已完成**。`66ace270`（2026-09-13 00:11）比较 opcode 已在产生处产出 NaN-boxed `TRUE_VAL`/`FALSE_VAL`（§8.24）。§14.4 现在只剩 ①（`OP_CAST_FLOAT` 对 bool/BigInt/null 原样透传）与 ③（下方新增）。 |
| ③「函数/递归完全没有 hot path，应在 `call()` 路径挂独立热点计数」 | **成立且已实现**（§8.25）：顶层 `fib_recursive(30)` 由 1.0x → **3.0x**；已有的 B 场景无回归。 |

**真正剩下的两项**（按性价比）：

1. **`OP_TAIL_CALL` 覆盖**（§11 P4）：`return f(x)` 经尾调用优化后是 `OP_TAIL_CALL`，
   复用当前帧、不走 `OP_CALL`，因此尾递归/深递归仍是 1x。本语言**自动**做尾调用优化，
   所以「写递归」的代码大量落在这一形态上，是当前最大的覆盖缺口。
   注意：把 `OP_TAIL_CALL` 接到热入口**不够** —— 真正要的是「自尾调用编译成回边跳转」，
   否则被 JIT 的函数体里含 `OP_TAIL_CALL` 时 scan 仍会拒收。
2. **callout 内联**（`arr.add` / `arr[index]` / `dict_set`）：这两项实测 ~2x，
   瓶颈是「一次 C 调用 + 逐参数装箱」。ffi 定宽读写内联拿到 -34%（§2.6）
   是可复制的模板；但内联里必须守住「类型不对 → bailout，不在机器码里复刻第二份语义」。

### 尾递归（`OP_TAIL_CALL`）的 JIT 收益评估（2026-09-13，决定 P4 值不值得做）

`build/probe_tail3.leno`（不入库），同机三轮：

| 场景 | JIT | `LENO_NO_JIT=1` | 加速比 |
| --- | --- | --- | --- |
| `nonTail(200000)`（非尾递归） | 31\~47 ms | 47 ms | ~1.5x |
| `tailRec(200000)`（轻 body：`acc + n`） | 0\~16 ms | 0\~16 ms | 1.0x |
| `heavyTail(200000)`（重活在体内的 `while`） | 15\~31 ms | 78 ms | **~3.5x** |

结论：**尾调用本身一点都不慢** —— 解释器的 `OP_TAIL_CALL` 复用当前帧、无分配，
20 万次不到 16ms（约 0\~5ns/次）。尾递归函数里真正贵的是**体内的循环**，
而那部分 loop JIT 已经独立覆盖（`heavyTail` 的 3.5x 正是这么来的）。
所以「自尾调用编译成回边」（§11 P4）能多拿的，只剩"直筒子 body 的几十条指令"。

→ **P4 暂缓，P5（callout 内联）提前为下一项**。

### R8 基准复盘：覆盖面工作的收益量化（2026-09-14）

**方法**：新写一份**专测"这一轮补齐的 opcode"**的基准
`examples/性能测试/JIT覆盖面基准.leno`（每一项都对应一条曾经被拒收的指令），
8 项 × {JIT, `LENO_NO_JIT=1`} × 3 次取最小，同机同轮：

| # | 测试项 | 规模 | JIT | 无 JIT | 加速比 | JIT ns/次 |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 纯循环 `i++` | 500 万 | 4.75 ms | 40.6 ms | **8.55x** | 0.95 |
| 2 | 空函数调用（**可内联**的 callee） | 500 万 | 3.87 ms | 71.6 ms | **18.5x** | 0.77 |
| 2b | 空函数调用（**不可内联**的 callee，见下） | 500 万 | 6.32 ms | 71.7 ms | **11.34x** | 1.26 |
| 3 | 字符串插值（`OP_STRING_ADD`） | 50 万 | 117.1 ms | 118.3 ms | **1.01x** | 234 |
| 4 | `switch` 4 case（`OP_SWITCH_LOOKUP` + int 快路径） | 500 万 | 53.3 ms | 363.7 ms | **6.82x** | 10.7 |
| 5 | `is` 类型判定（`OP_TYPE_CHECK`） | 500 万 | 61.5 ms | 342.6 ms | **5.57x** | 12.3 |
| 6 | struct 字段读写（`OP_GET_FIELD/SET_FIELD`） | 500 万 | 27.7 ms | 158.7 ms | **5.73x** | 5.5 |
| 7 | `arr.add`（通用 callout 路径） | 300 万 | 16.0 ms | 39.1 ms | **2.45x** | 3.2 |
| 8 | `dict.set`（通用 callout 路径） | 300 万 | 24.0 ms | 50.6 ms | **2.10x** | 4.8 |

（同机多轮波动约 ±10%，上表为 3 次取最小。）

### R5/R6 收益量化 + 覆盖面基准复核（2026-09-15）

**方法**：新写 `examples/性能测试/JIT_R5R6收益基准.leno`（4 项，专测 R5/R6 解锁的形态），
与 `JIT覆盖面基准.leno` 一起跑：JIT / `LENO_NO_JIT=1` **交替同轮、各 3 次取最小**、同机。
两侧 `CHECK:` 必须**逐字一致**（正确性先于耗时）。

**R5/R6 收益**

| # | 项目 | 规模 | JIT | 无 JIT | 加速比 |
| --- | --- | --- | --- | --- | --- |
| 1 | C2 闭包（循环体内建闭包·值捕获）+ 调用 | 50 万 | 375.2 ms | 415.6 ms | **1.11x** |
| 2 | C1 闭包（by-upvalue 捕获）+ 调用 | 50 万 | 371.5 ms | 394.1 ms | **1.06x** |
| 3 | R6-a 多返回值（函数级 JIT 发布多值） | 200 万 | **8.6 ms** | 150.9 ms | **17.5x** |
| 4 | R6-b 尾调用（callee 亦为 JIT 函数） | 200 万 | **50.4 ms** | 115.7 ms | **2.29x** |

- 闭包两项只有 1.06~1.11x：**每轮都要分配一个闭包对象**（约 768 ns/次）⇒ 分配主导，
  与 §8.72 的结论一致（"当前瓶颈已不在闭包覆盖面，而在分配路径"）。它们真正的价值是
  「**从不能进 JIT 变成能进**」；纯 JIT 收益要看闭包体内的计算量（§8.72 的 C 形态实测 2.67x）。
- 多返回值 17.5x 是本轮最大单项：callee 从"拒收后退回 VM 重入"变成函数级 JIT，
  且交付多值不再需要一次 VM 往返。
- 尾调用 2.29x —— **但这项是先踩了负优化再修回来的**：第一版直接走 `vm_call_value`，
  实测 **0.88x（比解释器慢 12%）**；加"callee 也是 JIT 函数就直接进机器码"的快路径后才是
  2.29x。过程与教训见 §8.76 的补充。

**覆盖面基准复核**（与 2026-09-14 基线同表比对，均为 3 次取最小）

| # | 项目 | JIT（今回） | JIT（基线） | 无 JIT（今回） | 加速比（今回 / 基线） |
| --- | --- | --- | --- | --- | --- |
| 1 | 纯循环 `i++` | 4.34 ms | 4.75 ms | 40.51 ms | 9.34x / 8.55x |
| 2 | 空函数调用（可内联） | 3.76 ms | 3.87 ms | 70.98 ms | 18.89x / 18.5x |
| 2b | 空函数调用（不可内联） | 6.01 ms | 6.32 ms | 68.25 ms | 11.35x / 11.34x |
| 3 | 字符串插值 | 101.8 ms | 117.1 ms | 109.2 ms | 1.07x / 1.01x |
| 4 | `switch` 4 case | 56.8 ms | 53.3 ms | 349.6 ms | 6.15x / 6.82x |
| 5 | `is` 类型判定 | 64.4 ms | 61.5 ms | 336.7 ms | 5.23x / 5.57x |
| 6 | struct 字段读写 | 27.2 ms | 27.7 ms | 154.9 ms | 5.71x / 5.73x |
| 7 | `arr.add` | 15.0 ms | 16.0 ms | 38.1 ms | 2.54x / 2.45x |
| 8 | `dict.set` | 24.2 ms | 24.0 ms | 49.7 ms | 2.06x / 2.10x |

结论：**9 项全部落在基线 ±10% 内、`CHECK` 两侧逐字一致 ⇒ 无回退**。
其中 [4]/[5] 的 JIT 绝对值比基线高 5~7%，但这两项的机器码**一行都没被 R5/R6 触碰**
（switch/is 的 codegen 与本轮完全无关），且它们在 5 次重跑里波动 **53~71 ms**、
而 NO_JIT 侧稳定在 350/339 ms —— 典型的"JIT 侧太快（几百微秒/轮）、被 Turbo 与后台负载
放大"的测量噪声，不是回退。（这也再次说明：**快的那一侧噪声大**，比加速比要看同轮最小值的
稳定性，而不是单次结果。）

**内联值多少？—— 用一个"不可内联"的对照项直接量出来（[2] vs [2b]）**

`[2b]` 的 callee 函数体读一个**模块变量**，按 §8.55 内联扫描必然拒绝它（内联后没有 callee
的帧、模块归属不可知）⇒ 这次调用只能走非内联路径。两次调用的唯一差别就是"能否内联"：

| 对照 | JIT（500 万次） | ns/次 | 差值 |
| --- | --- | --- | --- |
| [2] 可内联 | 3.87 ms | 0.77 | — |
| [2b] 不可内联 | 6.32 ms | 1.26 | **+0.49 ns/次（+63%）** |

⇒ **内联能省掉约 38% 的调用开销**，而且这恰好就是 R7 要解锁的那一类调用
（module 归属可判定的跨模块内联）——**R7 第一次有了可量化的收益依据**（此前只有拒收计数）。

`LENO_JIT_DEBUG` 复核：**8 个循环全部 `capable=1`、`Bailouts 0`** ——
上表 JIT 列确实是机器码的成绩，不是"没编上、退回解释器"。

**三个结论**

1. **第 3 项（字符串插值）JIT 与解释器持平（1.0x）是真结论，不是测错**：该循环每次迭代
   都**分配一个新字符串**（`Yields: 6` 说明确实在触发 GC 让出），成本由分配 + GC 主导，
   JIT 消掉的"解释器指令调度"占比很小。⇒ **覆盖一条 opcode ≠ 一定提速**：
   对分配密集型负载，JIT 收益天然被摊薄。
2. **收益与"这条指令原本有多贵"成正比**：纯循环 / 函数调用 10\~19x（解释器每条指令
   一次 opcode dispatch，JIT 全消掉）；而走 callout 的 `arr.add`/`dict.set` 只有 2.2\~2.3x
   —— callout 那一段（进出 C、序言保存恢复）两种模式都要付，**这正是 §8.62 给 switch
   做 int 内联快路径的同一类理由**。
3. **与 §9 的历史基线交叉核对：没有性能回退**：
   * `i++`：历史 1 亿次 78 ms ⇒ 本次 2000 万 15 ms（折算 1 亿 ≈ 75 ms）✓
   * `arr.add`：历史 1 亿次 625 ms ⇒ 本次 1000 万 62 ms（折算 1 亿 ≈ 620 ms）✓

   中间隔了几十次提交（含 R4 的机器码延迟释放、R9 的 switch 快路径），核心循环与
   callout 路径的绝对耗时仍在同一水平 ⇒ **R4 的延迟释放没有可测代价**（它只在 JIT 出口
   多做"队列非空？"两条指令，而队列通常是空的）。

**覆盖面的定性进展（同一批负载的 JIT 统计）**

| 指标 | R1 之前 | 现在 |
| --- | --- | --- |
| `file_manager` 循环级拒收 | `81/132/156/39/88/90/38` 一堆 | **只剩刻意保留的 `138`** + R5 前置（`14`）+ 维持拒绝（`142`） |
| `file_manager` `FuncCompiled` | 5（§8.59 之前） | **≈ 100\~290**（随交互路径波动） |
| `unknown opcode`（长度未知） | 常见 | **0**（R3 补齐长度表后本负载已无） |
| 运行时 bailout | 0 | **0**（一直保持） |

**方法学提醒（本轮踩到三次，全部值得记）**

1. **`times.ms()` 在 Windows 上不适合微基准**：它的实现是 `GetTickCount64`，
   只有 **~15.6ms 的跳变粒度** ⇒ 会把快项目测成 `0 ms`（无法比较），
   还会把两个不同的值测成**完全相等**（第一版就出现过"字符串插值 250ms vs 250ms"）。
   改用 `times.us()`（走 `QueryPerformanceCounter`，真高精度）后，同一个用例是
   116.0ms vs 117.9ms —— 结论没变，但比值从"可疑的完全相同"变成**可信的 1.02x**。
   另外注意：`us()`/`ns()` 返回 **float**，`ms()` 返回 int。
   *（正是这一条让第一版表格里第 1 项虚高到 10.4x —— µs 重测后是 7.94x。）*
2. **必须用 `LENO_JIT_DEBUG` 复核"测得的是不是 JIT"**：第 3 项 1.0x 若不复核，最自然的
   误判是"JIT 对插值无效"；复核后（8 个循环全部 `capable=1`、`Bailouts 0`）才知道它确实
   在 JIT 里跑，结论应是"分配主导"。（与 §8.64"探针必须自证"同一条教训。）
3. **规模要按 JIT 侧标定**：JIT 把多数循环压到毫秒级以下 ⇒ 用 ns/µs 计时后，
   每项只需 300 万\~500 万次就能拿到稳定读数（比第一版的 2000 万次更快更准）。

***

## 10. 架构瓶颈分析

当前 JIT 是 **loop-body template JIT**，只编译单个循环体、用 push/pop 虚拟栈、单回边。存在三个结构性天花板：

### 瓶颈 1：~~只能编译扁平循环，不能跨控制流~~ 已解决

P1 已完成嵌套循环支持。`scan_loop_body` 现在接受 `OP_FOR_PREP`（内层 for 初始化）、mid-body `OP_LOOP`（内层 while 回边）和 mid-body `OP_FOR_LOOP`（内层 for 回边）。codegen 通过 `offmap_lookup` 解析每个回边指令自带的目标偏移，天然支持任意嵌套深度，无需 loop stack。

### 瓶颈 2：函数调用/递归 —— 已大幅缓解，仍有深度与形态两个天花板

历史上这里是「JIT 完全不支持函数调用」。现状（2026-09-13）：

* ✅ **JIT 循环内的调用**：`OP_CALL_GLOBAL_FUNC[_TYPED]` / `OP_INVOKE_METHOD_TYPED` /
  `OP_CALL_NATIVE` / `OP_MODULE_CALL` 都实现了 callout（§13.8），且 callee 可
  被内联或函数级 JIT 编译。
* ✅ **被调函数整体进 JIT**：`jit_compile_function` + 薄桥（877688c3）。
* ✅ **解释器调用链也能进函数级 JIT**（§8.25，2026-09-13）：顶层一次性
  `fib_recursive(30)` 由 1.0x → **3.0x**。

剩余的两个天花板：

1. **递归深度 ≤ `JIT_FUNC_MAX_DEPTH`(64)**：JIT body → callout → JIT body 是
   真实 C 调用链，超过 64 层回退解释器。实测 `nonTail(200000)` 无收益。
   这是「用 C 栈当 JIT 调用栈」的结构性代价，要根治需要 JIT 自管调用栈。
2. **`OP_TAIL_CALL` 完全不覆盖**：`return f(x)` 经编译器尾调用优化后发射
   `OP_TAIL_CALL`（复用当前帧），既不经过新增热入口，也不在 JIT 支持列表里。
   本语言**自动**做尾调用优化，所以「写递归」的代码大量落在这一形态
   → 当前最大的覆盖缺口（§11 P4）。

另外 callout 本身的单次成本（一次 C 调用 + 逐参数装箱）在对象/调用密集场景仍是
主瓶颈：`arr.add` / `dict[key]=` 只有 ~2x（§9 基线表）。

### 瓶颈 3：push/pop 虚拟栈 —— 单元素 TOS 缓存已落地，剩余空间有限

历史分析是「`a + b` 要 4 次内存操作」。2026-09-06 已落地**单元素 TOS 寄存器缓存**
（`x86_64.c` 的 `TOS_SPILL` / `TOS_CONSUME_RAX` / `TOS_CONSUME_TO` / `TOS_PRODUCE`）：
RAX 常驻栈顶，`a + b` 退化成「pop 一次 + add + 留在 RAX」，只在需要第 2 个临时值时
才 spill。收益证据是实测的标量循环成本已经贴着下限：

| 场景 | 1 亿次耗时 | 折算 |
| --- | --- | --- |
| `i++` | 78\~172 ms | ≈2.5 cycle/iter @3GHz |
| `a = b` | 47\~94 ms | ≈1.4 cycle/iter |

再往下走的空间是「2 槽 TOS + 循环不变量驻寄存器」，相对 1 槽是**减少若干次 L1
命中**，不是数量级；而当前真正的瓶颈在 callout（瓶颈 2）与函数覆盖缺口（§11 P4），
所以 P0 的剩余部分优先级被下调。

***

## 11. 优化路线图

按投入产出比排序：

### P0：虚拟栈寄存器化（预计提速 3-5x）

- **目标**：维护 register cache，栈顶 1-2 个元素常驻 RAX/RDX，不下推到内存

- **改动范围**：只改 codegen 中的 push/pop 语义，不碰 scan 逻辑和 bailout 机制

- **实现要点**：

  - `push` → 如果 RAX 空，放 RAX；否则 spill 栈顶到 `[rsp]` 再放 RAX

  - `pop` → 如果 RAX 有值，取 RAX；否则从 `[rsp]` 加载

  - 二元运算（ADD/SUB/MUL）→ 左操作数在 RAX，右操作数在 RDX，直接 `add rax,rdx`，0 次内存操作

- **状态**：✅ 已完成（2026-09-06）。TOS 缓存 RAX，push/pop 从 4 次内存操作降到 1-2 次。修复 OP\_JUMP\_IF\_FALSE/TRUE 的 `tos_live=0` 编译期赋值污染 fall-through 路径的 bug（导致 while 循环 RSP 漂移、提前退出）。261 回归测试全过，while 50-5000 次迭代验证正确。
- **剩余部分（2026-09-13 复核后下调优先级）**：1 槽 → 2 槽（RDX）+ 循环不变量驻寄存器。
  依据见 §9「与上一轮评估的核对」：标量循环实测已 ≈1.4\~2.5 cycle/iter，剩余空间是
  「少几次 L1 命中」而不是数量级；当前真正的瓶颈在 callout 与函数覆盖缺口，
  所以不排在 P4 之前。

### P1：嵌套循环支持（解锁大量真实代码）

- **目标**：for 套 for、while 套 while 能被 JIT 编译

- **改动范围**：scan + codegen 的 loop patch 表

- **实现要点**：

  - `OP_FOR_PREP` 不再拒绝，记录为内层循环入口，跳过初始化字节

  - body 中途出现 `OP_LOOP`/`OP_FOR_LOOP` 时，记录为内层回边

  - ~~codegen 需要一个 loop stack（记录每层循环的~~ ~~`loop_start_mc`~~ ~~和~~ ~~`exit_mc`）~~ 不需要：每个回边指令自带跳转偏移，通过 `offmap_lookup` 独立计算目标地址，天然支持任意嵌套深度

  - 正确处理内层循环退出后回到外层循环体的跳转

  - 修复 JUMP\_IF\_FALSE/TRUE 跳转路径 TOS 不匹配（见 8.10）

  - 修复 scan vstack 负值检查（见 8.9）

- **状态**：✅ 已完成（2026-09-06）。支持 while-while / for-for / while-for / for-while / 三层嵌套，261 回归测试全过，嵌套 10000x10000 = 1亿次 for+for 仅 78ms。

### P2：OP\_CALL callout（让更多代码进入 JIT 路径）

- **目标**：循环体中遇到 OP\_CALL 不再整个循环拒绝编译

- **改动范围**：利用现有 callout 基础设施

- **注意**：能让 fib"跑通"JIT 路径但不会提速——callout 开销远大于解释执行省下的 dispatch 开销

- **状态**：❌ 实验失败（2026-09-06），已回退。完整踩坑记录见第 13 节。实测 callout 单次调用
  开销约为解释器原生调用路径的 20\~30 倍，调用密集循环（基准 Phase B 300 万次调用）
  从 859ms 恶化到 18000ms+。结论：callout 方案是死路，必须走内联（见 13.6）。

### P3：架构方向决定——函数级 JIT（method JIT）

| 方向            | 代表                 | 工作量 | 收益       | 适合场景   |
| ------------- | ------------------ | --- | -------- | ------ |
| 继续改进 loop JIT | 当前                 | 低   | 中（2-5x）  | 算术密集循环 |
| 函数级 JIT       | V8 TurboFan / PyPy | 中-高 | 高（10x+）  | 通用     |
| Trace JIT     | LuaJIT             | 很高  | 很高（20x+） | 通用+递归  |

**不推荐 Trace JIT**：Mike Pall 花了几年全职才做出来，需要 trace recording、SSA IR、trace stitching、snapshot-based deoptimization、trace exit 修复，一个人无法完成。

**推荐函数级 JIT**：

- 天然解决嵌套循环（编译整个函数体，循环只是函数内跳转）

- 可支持函数间调用（JIT 函数调用 JIT 函数，开销远低于 callout）

- 可做叶子函数内联

- 不需要 trace recording，可用现有 codegen 基础设施扩展

- 渐进式迁移（先编译最热的几个函数）

- 核心改变：触发点改为函数被调用 N 次后编译整个函数体；编译范围从 `OP_CALL` 到 `OP_RETURN`；OP\_CALL 走快路径（已 JIT 函数直接 `call jit_fn`）或慢路径（callout 到 VM）；用 simple linear-sscan allocator 把局部变量分配到 callee-saved 寄存器

- **状态（2026-09-13）**：🟡 部分落地。「触发点改为函数被调用 N 次后编译整个函数体」
  已由 §8.25 的解释器侧热入口实现（`JIT_FUNC_HOT_THRESHOLD = 50`）；
  「JIT 函数调用 JIT 函数」已由 callout 薄桥实现，但仍占用 C 栈（深度上限 64）；
  「局部变量分配到寄存器」未做。

### P4：自尾调用编译成回边 —— 解锁深递归 / 尾递归（2026-09-13 提出）

- **目标**：让 `OP_TAIL_CALL` 也能进 JIT，尤其是「自己调自己」的尾递归
  （`return f(n-1, acc+x)` 形态，本语言编译器自动做尾调用优化，
  所以这是最常见的递归写法）。

- **为什么不能只在 `OP_TAIL_CALL` 上挂热入口**：
  - `OP_TAIL_CALL` 的语义是**复用当前帧**（关 upvalue → 释放 locals →
    memmove args/callee → `vm.sp = old_stack_base + arg_count + 1` → 就地换
    `closure/chunk/ip/locals`），并不「返回」到调用者。要把它接到
    函数级热入口，得在算完结果后额外模拟一次 `OP_RETURN` 的拆帧（含
    catch/finally、`stop_frame_cnt`、module 帧、动态 locals 释放），风险远高于收益。
  - 更关键的是：即使接了入口，**被编译的函数体自身仍含 `OP_TAIL_CALL`**，
    `scan_loop_body` 会在 `default` 分支把它判成不可编译 → 白忙。

- **正解**：在 `func_mode` 下把「对自身的尾调用」编译成**跳回函数入口的回边**
  （等价于把递归写成循环），参数直接写进 scratch 槽。这样尾递归函数变成
  一个有回边的函数体，配合 P1 已有的回边基础设施（`offmap_lookup` + patch）
  就有机会整体编译。同时它天然不消耗 C 栈，**顺带解掉深度上限 64 的问题**
  （深度不再重要，因为不再递归）。

- **改动范围**：`jit_scan.c` 增加 `OP_TAIL_CALL` 的识别与「是否自调用」判定；
  `ops_callout.inc` 增加自尾调用 → 回边 codegen；非自尾调用仍走原有 callout /
  VC 重入路径（或继续 bailout）。三处同步规则见 §2.3 第 5 条。

- **验证**：`tailRec(200000)` 差分 + `assert/test_jit_hot_func.leno` 扩用例 +
  `Bailouts` 计数不得上升（§14.3 第 12 条）。

- **状态（2026-09-16 更新）**：🟡 **已由 R6-b 部分实现，剩余价值需重估**。
  * **已做**（R6-b，§8.76）：`OP_TAIL_CALL` 在**函数级 JIT** 里实现为「**调用 callee
    （可走 JIT 机器码快路径）+ 按 VM 实际发布的个数产出结果 + epilogue**」；循环/内联维持拒绝。
    实测收益 **2.29x**（`examples/性能测试/JIT_R5R6收益基准.leno` 第 4 项）。
    ⚠ 关键教训：第一版直接走 `vm_call_value`（VM 重入）实测 **0.88x 负优化**，
    "callee 也是 JIT 函数就直接进机器码"的快路径是**必需**的，不是优化。
  * **仍未做**：本节原本设想的"**把对自身的尾调用编译成跳回函数入口的回边**"。它剩下的价值只有
    **不消耗 C 栈**（顺带解掉 §12 第 6 条的深度上限 64）与"直筒子 body"的少量指令；
    而且 R6-b 之后实测深尾递归的 JIT 深度**恒为 1**（第 0 层进 JIT，其余由解释器
    `OP_TAIL_CALL` 真 TCO 走完、不崩）⇒ **优先级低于其它项**。
  * 原始依据（仍成立）：见 §9「尾递归的 JIT 收益评估」—— 解释器的 `OP_TAIL_CALL`
    只要 0\~5ns/次，尾递归函数里贵的是体内循环，而那部分 loop JIT 已独立覆盖
    （`heavyTail` 实测 3.5x）。

### P5：callout 内联第二批（`arr.add` / `arr[index]` / `dict_set`）

- **现状**：这三项在 1 亿次基准里只有 ~2x（`arr.add` 625\~641ms、
  `dict[key]=` 875\~906ms，而标量 `i++` 只有 140ms），瓶颈是
  「一次 C 调用 + 逐参数装箱」。**真实程序里数组/字典写是最高频操作**，
  所以这是当前性价比最高的一项（2026-09-13 起取代 P4 排在首位）。
- **依据**：ffi 定宽读写内联（§2.6）拿到 **-34%**，是同一条路可复制的模板。
- **设计要点（与 ffi 不同，不能照抄）**：`arr.add` 的 C 侧是
  `array_append(arr, value)`，里面有**容量增长（malloc/realloc）**和
  **GC 写屏障**，不可能像 ffi 的定宽 memcpy 那样整体内联。可行方案是
  **内联快路径 + 完整 callout 兜底**：
  1. 检查 `obj->type == OBJ_ARRAY` 且 `arr->count < arr->capacity`
     （最热的追加场景通常命中，包括 1 亿次基准）；
  2. 命中 → 直接 `mov [arr->items + count*8], value; inc count`；
  3. 未命中（容量满 / 非数组 / 需要写屏障）→ **bailout**，
     让解释器跑原 `jit_callout_array_append`，报错文本与语义完全一致。
- **必须先确认的一件事**：`gc_write_barrier` 在「老年代 → 新生代引用」场景是否
  真的必要 —— 若快路径必须调用它，就退化成"少一次装箱"的小优化；
  若可以按代条件化（或者本实现的 GC 是非分代/保守的），快路径才成立。
- **约束**：内联必须把「类型不对 → 写 site 后 bailout」这条守住 ——
  宁可不编、不要猜，也不要在机器码里复刻第二份语义（否则就是 §14 那批静默算错的温床）。
- **状态（2026-09-13）：🟡 数组部分已完成，见 §8.29**
  * **字典写（`d.set(k,v)` / `d[k]=v`）**：**已尝试并放弃**（**§8.70 负结果**，2026-09-15）——
    机器码里能算到槽位（`str->hash & (capacity-1)`），但**"命中"的最后一步必须比字符串内容**
    （键相等是内容相等，不是指针相等）⇒ 收益被吃掉。要做请换思路（dict 自带版本号让 JIT
    缓存"上次解析出的槽位"），不要在 codegen 里复刻哈希表。另注意 `d[k]=v` 发的是
    `OP_INDEX_SET_NOPUSH`、`d.set(k,v)` 才是 `OP_DICT_SET`。
  * `OP_ARRAY_APPEND_NOPUSH`（`arr.add`）与 `OP_INDEX_SET_NOPUSH`（`arr[i]=v`）
    已加「内联快路径 + 原 callout 兜底」：下标写实测 **1.8\~2.3x**（110ms → 62ms / 2000 万次），
    `arr.add` 因扩容与内存带宽地板只拿到 1.1\~1.17x（1 亿次 641ms → 547ms）。
  * 顺带修掉一个既有 bug：`OP_INDEX` 快路径用 64 位加载读 `count` 导致
    **数组越界读被静默跳过**（§8.28），并把它改成 bailout 以对齐解释器的
    可捕获异常语义。
  * **2026-09-13 追加（§8.30，提交 `c4b6bbd1`）**：`OP_GET_FIELD_FAST`（struct 字段读）
    与 `OP_INVOKE_METHOD_TYPED`（struct 方法调用）也已内联 —— 方法调用实测 **1.9\~3.3x**
    （探针），对象版光追 Phase B2 578 → 297 ms。**新结论**：`OP_STRUCT_INIT`（对象分配）
    的 JIT 侧包装只要 **14ns**，成本全在 C 分配本体，内联它没有意义；后续见 §8.31。
  * **未做**：`dict[key]=v`（`OP_DICT_SET`）。字典写要做哈希、探测、tombstone、
    扩容乃至「数组部分 / 哈希部分」双结构，无法像定宽访存那样内联；
    真要提速得换实现思路（例如先内联「命中的数组部分连续整数键」快速路径），
    优先级低于其它项。

***

## 12. 当前未解决问题

### 12.1 【已修复·§8.62】循环体内「按字面量属性名取值」obj["key"] 在 JIT 下堆损坏（2026-09-14 发现 → 同日修复）

> **状态：已修复**（2026-09-14，详见 §8.67）。根因：`OP_GET_PROPERTY` 的**独立访问**形态在 JIT 里
> **没有弹掉 receiver 槽** —— 结果只进 RAX（TOS 缓存），内存栈却永久多留一个槽 ⇒
> **每次执行泄漏 8 字节** ⇒ 循环里 RSP 单调下漂 ⇒ 到 ~2MB 栈界时 `0xC0000374`。
> 下面保留完整的发现/夹逼/排除过程，**以及被推翻的两条线索**，供同类问题参考。

**症状**：`exit=-1073741571` = **`0xC0000374` STATUS_HEAP_CORRUPTION**。

**最小形态**（探针，已隔离）：

```leno
struct P { int a = 7; int b = 9 }
main() {
    var box = {}
    box["p"] = new P(a = 7, b = 9)
    var s = 0
    var i = 0
    while i < 300000 {
        var q = box["p"]   // ← 循环体内的 dict 取值 + 存入局部变量
        s = s + 1
        i = i + 1
    }
}
```

**四变体夹逼（每个只差一处）**

| 变体 | 循环体内 | JIT 结果 |
| --- | --- | --- |
| B | `var q = box["p"]` + `s = s + q.a` | **崩** `0xC0000374` |
| B2 | dict 取值**移到循环外**，循环内只 `s = s + q.a` | 正常（0.73ms/30万轮 = 2.4ns，真 JIT） |
| B3 | 静态类型 struct 局部 + `s = s + q.a` | 正常（0.74ms） |
| B4 | **只** `var q = box["p"]`，不读字段 | **崩** |

⇒ 充分条件是**循环体内的「dict 取值 → 存局部变量」**；**与 `OP_GET_FIELD` 无关**（B4 证明）。
`LENO_NO_JIT=1` 下四个变体全部正常。

**已排除：不是本轮（R9/R2 各批/R8/基准）引入**
用 `git worktree` 在 `4cf9e9ed`（R9 之前）重建二进制，同一探针**同样崩溃**；
且涉及的两个 codegen 文件（`ops_index.inc` 的 dict 慢路径、`ops_stack.inc` 的 `OP_SET_LOCAL`）
自 `bbc99809` 起未再改动。（`b817680b^`（R4 之前）在本机已无法构建，故只能证到"R9 之前"。）

**诊断线索（已按证据修正到第二步）**

1. 第一版线索（"`OP_SET_LOCAL` 不检查 `si < 0`、`mark_local` 静默返回 ⇒ 偏移越界"）**已证伪**：
   `mark_local` 在槽位耗尽时会 `capable = 0` 拒绝循环（不是静默返回），
   而 `LENO_JIT_DEBUG` 打出的实际映射干净（`scratch[0..2]`、`disp=-8/-16/-24`）。
2. **当前最有力的线索**：把四个变体按"循环内是否有 dict 取值"与"是否真的留在 JIT 里"排开 ——

| 变体 | 循环内 dict 取值 | 是否留在 JIT | 结果 |
| --- | --- | --- | --- |
| B / B4 | 有（30 万次） | **是**（`capable=1`、无 bailout） | **崩** |
| A（`s + box["p"].a`） | 有 | **否**（3 次 `int48` bailout 后放弃 JIT） | 不崩 |
| B2 / B3 | 无 | 是 | 不崩 |

⇒ 充分条件是「**循环留在 JIT 机器码里反复执行该 opcode**」。
（这条推断当时写的是"`OP_INDEX` 的 dict callout" —— **是错的**：真正被反复执行 30 万次的是
`OP_GET_PROPERTY`，而 `jit_callout_index`（dict 分支）**一次都没被调用** —— 诊断代码证明了这点。
纠正它的是**字节码 dump**（`--debug-out`）：`obj["常量"]` 与 `obj[变量]` 发的是**不同 opcode**。
详见 §8.67 的教训。）
`LENO_JIT_DEBUG` 显示崩溃有 `[JIT-TRACE] PRE` 行、**没有** POST 行。
3. 同一形态另有性能异常：A 形态在 JIT 下 3 次 bailout 后放弃 ⇒ **比纯解释器还慢**
   （156ms vs 146ms）。记账修好后这两件事很可能一起消失。

**为什么优先级最高**：内存损坏 > 一切性能项（同 §8.60 / R4 的判断）。
在修复前，**不要**把"含 dict 取值 + 局部变量声明"的循环当作已支持场景。

***

1. **`step == 0` / `step` 是 float 的 for 循环无法 JIT**（`site -2` / `site -3`，已知限制非 bug）：
   `step == 0` 的 VM 语义是「不进循环」，float 步长无法走 int48 快路径判断方向，
   两者都显式 bailout 交回解释器。**正/负步长（±1/±2/±3）现已全部支持**，
   日志直接报 `BAILOUT(prologue:step==0)` / `(prologue:step-is-float)`
2. **while 比 for 慢约 2 倍**：`OP_GET_GLOBAL` 每次 NaN-box 解码开销，P0 寄存器化后有望缓解
3. **`body_start=1156` 编译失败**（历史记录，待复核）：具体原因未完全定位
   （无 scan FAIL 和 codegen FAIL 消息）
4. **fib_iterative(1000) 3 次 bailout**：斐波那契值约 fib(56) 溢出 int48（超 2^47），
   属预期行为，JIT 的溢出检测正常工作
5. ~~**`OP_TAIL_CALL` 不在 JIT 支持范围内**（2026-09-13 新增，§8.25 / §11 P4）~~
   —— **✅ 已解决（2026-09-16，R6-b，§8.76）**。
   （历史记录）`return f(x)` 经编译器尾调用优化发射 `OP_TAIL_CALL`，它复用当前帧、
   既不经过函数级热入口，也不在 scan 的 opcode 支持列表里。后果：① 尾递归函数无论
   调用多少次都不进函数级 JIT；② 任何含 `return f(x)` 的函数体都会被 `scan_loop_body`
   的 `default` 分支拒收（`capable=0`）。本语言自动做尾调用优化，所以当时这是最大的
   函数覆盖缺口。
   **现状**：函数级 JIT 已支持（"调用 + 发布结果 + epilogue"；循环与内联维持拒绝）。
   关键前置是补上 **`OP_GET_GLOBAL_FUNC`(18)** 的缺口 —— 尾调用的字节码形态是
   `OP_GET_GLOBAL_FUNC + OP_TAIL_CALL`（不是合体的 `OP_CALL_GLOBAL_FUNC`）。
   实测：含尾调用的函数能进 `FuncCompiled`、JIT 与 `LENO_NO_JIT=1` 输出逐字一致、
   深尾递归 20000 层 ×200 轮不崩（空间行为靠深度守卫交解释器做真 TCO），收益 **2.29x**。
   **仍未支持**：`OP_TAIL_CALL_NATIVE`（`return 原生函数(x)`）。
6. **JIT 函数调用的递归深度上限 64（2026-09-13 新增）**：JIT body → callout →
   JIT body 是真实 C 调用链，`jit_func_depth >= JIT_FUNC_MAX_DEPTH` 即回退解释器。
   实测 `nonTail(200000)` 无加速（§8.25）。根治需要 JIT 自管调用栈。
7. **函数级 JIT 中途 bailout 会重跑整个调用（2026-09-13 新增，§8.25 / 14.4-③）**：
   与 `jit_callout_*` 快路径同一取舍 —— JIT 中途对堆的副作用（数组 append、
   字典写、打印）在回退时无法回滚。缓解措施是 `hot_disabled`：一次失败即永久
   停用解释器侧入口，避免「每次调用都进 JIT 再回退」的反复试探。
8. **`examples/crypto/aes128.leno` 在 JIT 模式下栈溢出** —— **✅ 已修复（2026-09-13，§8.26）**。
   （历史记录）现象：JIT 模式退出码 `0xC00000FD`（STATUS_STACK_OVERFLOW），stdout 停在
   `--- 字符串加解密 ---` 之前；`LENO_NO_JIT=1` 完整跑完。
   根因：内联体的 `bc_off` 基址按 nesting depth 编号（`0x10000 * inline_depth`），
   而内联只有一层 → 同一循环内多个内联点共用命名空间，
   `offmap_lookup` 把第 2..N 个实例的跳转解析到第 1 个实例的机器码
   → 死循环 + RSP 漂移 → 栈溢出。修法：按**内联实例**编号
   （`bc_off = 0x10000 * (inline_idx + 1)`）。回归见 `assert/test_jit_multi_inline.leno`。

9. **比较结果的位模式：JIT 写回裸 0/1，解释器压 `val_bool`** —— **✅ 已修复（2026-09-12，§8.24）**。
   （历史记录）`var x = (a > b)` 或 `bool flag = (a > b)` 在 JIT 循环里
   写回后，解释器读到的是**次正规 float**（≈4.9e-324 / 0.0）而不是 bool。
   真值判断恰好仍然正确（非零/零），但 `x is bool` 变 false、`_int(flag)` 由 1 变 0
   —— 后者是**静默算错**。复现探针：`build/probe8.leno`（`boolLocal` / `intOfFlag`）。
   注：同类问题里的 **`OP_NOT` 已在 2026-09-12 修掉**（现在产出 NaN-boxed bool，§8.22），
   剩下的只是各比较 opcode 自身仍压裸 0/1。

10. ~~**对象分配 ~200ns/次，且 JIT 循环内没有 GC 安全点**（2026-09-13 新增，§8.31）~~
    **✅ 已大幅改善（2026-09-15，§8.74）**：
    - **安全点**：§8.37/§8.38 的**回边让出**已落地（10M 次 `new` 的 ~960MB 泄漏场景解决，
      `Yields` 会按分配量出现）。
    - **分配成本**：池上限 4MB → **32MB**（实测饱和点）后，
      `new Pair` **110 → 68 ns**、`new Big` **170 → 89 ns**（同机同构建 A/B，**−38%/−47%**）。
      重新归因：原来记的"每字段 ~15ns 的双遍写 / ~79ns 未归因"**主要是"冷内存"代价**
      （从不复用的内存要付 memset + page fault + cache miss）；池能复用后边际每字段成本
      降到 **~4.7 ns**。旧结论"放大池只省 4ns"随让出机制落地而**失效**
      （更正记在 `待办_GC与分配优化.md` §三）。
    - **剩余空间**（需先做实验）：cold path 的 `memset(0)` 是否可省、`OBJ_ARRAY` 的复用
      （目前有意排除，见 `gc.c` 池设计注释）、`OP_STRUCT_INIT` callout 包装的 ~14 ns。

11. **内联体内不支持闭包与 upvalue（R5，2026-09-15，有意拒绝，不是待修项）**：
    含 `OP_CLOSURE` 或 `OP_GET/SET_UPVALUE` 的被调方一律不能内联（inline scan 直接拒）。
    原因：函数级 JIT 不压帧，"当前闭包"由 ABI 第三参带入；一旦内联，同一段机器码里
    会同时存在 caller 与 callee 两个闭包环境，`GET_UPVALUE` 该读谁归属不清 ⇒ 可能静默读错环境。
    这是设计文档的不变量 **I8**；代价只是"含闭包的函数不被内联"，循环本身仍可 JIT。

12. **bailout 重放会连"建闭包"这个堆副作用一起重放（R5 新增受害面，2026-09-15）**：
    循环 bailout 从**循环头重跑本轮**、函数级 bailout 重跑**整个调用**（见本节第 7 条），
    而 R5 之后 `OP_CLOSURE` 也是 JIT 能执行的**分配**操作 ⇒ 若那一轮已经"建闭包 + 存进容器"，
    重放会再建一次（容器里多一个）。属既有取舍的延伸，缓解同样是
    `hot_disabled` / `JIT_BAILOUT_LIMIT`；主要防线是**扫描期把不安全形态拒掉**
    （设计文档 I6：宁可拒收，不靠 bailout 兜底）。

> 已关闭的旧条目：
>
> - ~~负步长 for 循环无法 JIT~~ → 已支持倒序 for：序言 / `FOR_PREP` / `FOR_LOOP`
>   三处按 step 符号分流比较方向（`701692d4`）
> - ~~浮点 NaN 比较 bailout~~ → 已按 IEEE 语义：`EQ/LT/LE` 用 `SETNP` 屏蔽无序，
>   `GT/GE` 的 `JA/JAE` 天然正确（`701692d4`）
> - ~~try/catch 循环不可 JIT~~ → `OP_TRY/CATCH/FINALLY/END_TRY` 现为 no-op（第 5 节）
> - ~~含函数调用的循环无法 JIT~~ → 已实现被调函数内联 + 函数级 JIT（13.8）
> - ~~通用算术/比较遇 float 即 bailout~~ → 已补 float 快路径（8.15）
> - ~~通用 `OP_MUL` / `OP_MOD` / `OP_EQ` / `OP_NEQ` 不被 JIT 支持~~ → 已补齐（2026-09-12，
>   三处同步：`jit_scan.c` 的 size 表与两处 vstack switch + `ops_arith.inc` / `ops_icmp.inc`）
> - ~~原生方法调用 callout 取实参反序~~ → 已修（2026-09-12，§8.17）：
>   `d.get(key, def)` 曾变成 `d.get(def, key)`
> - ~~`OP_*_FLOAT` 不提升 int 操作数~~ → 已修（2026-09-12，§8.18）：`EMIT_FLOAT_ARGS2`
> - ~~`OP_DIV_FLOAT` 除零静默算 inf~~ → 已修（2026-09-12，§8.19）：
>   与 0.0 比较后 bailout，NaN 除数放行
> - ~~JIT 下数组越界读被静默跳过~~ → 已修（2026-09-13，§8.28）：`OP_INDEX` 快路径
>   用 64 位加载读 `count` 导致越界判定失效；改用 32 位零扩展加载，并把越界改成
>   bailout 以对齐解释器的「可被 try/catch 捕获」语义
> - ~~内联体共用 bc_off 命名空间（aes128 栈溢出）~~ → 已修（2026-09-13，§8.26）
> - ~~表项溢出等 6 处静默降级~~ → 已改为显式拒绝编译（2026-09-13，§8.27）

***

## 13. 函数调用 callout 实验的完整踩坑记录（2026-09-06，已回退）

本章记录尝试让 JIT 支持循环内 `OP_CALL_GLOBAL_FUNC` / `OP_CALL_GLOBAL_FUNC_TYPED`（光线追踪基准
Phase B/B2/C 未命中 JIT 的根因）的完整过程。实验最终失败回退，但暴露了一批有价值的坑，
供后续实现内联时参考。**当前代码状态**：scan 阶段遇到函数调用直接判 loop 不可编译（jit.c
中注释 "inlining not implemented"），相关 callout 代码已全部删除；编译期 `return_count`
统计基础设施保留（见 13.5）。

实验方案：JIT 机器码把参数转成 NaN-boxed 存入栈帧内 args 缓冲区 → callout C 函数
`jit_callout_call_global` 模拟解释器 OP\_CALL 的栈布局（压参数、压 callee、`vm_call_value`
重入解释循环执行被调函数）→ 返回值从 VM 栈拷回 ret 缓冲区 → JIT 压回虚拟栈。

### 13.1 坑一：ModRM 编码 mod 位错误（寄存器复制 ≠ 内存加载）

- **症状**：JIT 编译通过，一执行就崩溃（Exit code 0xC0000005，访问违例）

- **根因**：返回值循环里 `mov rax, [rdx]` 的 ModRM 字节用了 `mod=11`（寄存器-寄存器），
  实际语义变成 `mov rax, rdx`（寄存器复制），rdx 里是缓冲区地址而非值，后续解引用野指针

- **修复**：ModRM 用 `mod=00, rm=RDX`（`48 8B 02`）才是从 `[rdx]` 内存加载

- **教训**：手写机器码时 `mov reg, [reg]` 和 `mov reg, reg` 只差一个 mod 位，
  objdump 反汇编一眼可辨（`mov (%rdx),%rax` vs `mov %rdx,%rax`），生成大段机器码后
  必须先 dump 反汇编核对再跑

### 13.2 坑二：r9 被挪用作循环计数器，摧毁 globals 基址

- **症状**：修完坑一后仍崩溃。反汇编看到 `mov 0x0(%r9),%rax` —— 对空指针取值

- **根因**：返回值压栈循环拿 r9 当计数器（`sub r9, 1`），但 r9 在整个 JIT 代码里
  固定持有 globals 基址指针（入口处 `mov %rdx,%r9`，所有 OP\_GET\_GLOBAL/SET\_GLOBAL
  都靠它寻址）。循环结束后 r9=0，下一条全局变量访问即空指针崩溃

- **修复**：改用 r11 当计数器（用完恢复 `r11 = INT_TAG` 常量），r9 全程不动

- **教训**：JIT 的寄存器分配是全局约定（rcx=locals / r9=globals / r10=PAYLOAD\_MASK /
  r11=INT\_TAG / rbx=类型位图），任何临时代码借用约定寄存器都必须用完恢复。

### 13.3 坑三：args 缓冲区读写方向相反（本实验最隐蔽的 bug，导致结果错误）

- **症状**：不崩了，但 Phase B 结果错误：hits=15（正确值 900000），且恰好等于前 50 次
  解释执行（JIT 热度阈值）期间的命中数——即 **JIT 接管后所有调用全部 miss**

- **定位过程**：

  1. `LENO_NO_JIT=1` 对照跑：结果全对 → 排除解释器和 op\_call.inc 修改，锁定 JIT 机器码路径
  2. callout 里打印返回值位模式：`[r0]=0xbff0000000000000 [r1]=0xfffb000000000000`
     （float -1.0, int 0）——返回值完全正确
  3. callout 里打印收到的参数位模式：`[a1]=0xbff0000000000000`——**a1 是上一次调用的
     返回值 t！** 且 a3..a6 = 0（从未写入的槽）

- **根因**：JIT 生成侧把 args\[i] 存到 `args_base - 8*i`（向低地址排列），callout 侧按
  `args[i] = args_ptr[i]`（向高地址）读取。方向相反 → 整体错位：callout 的 args\[1] 读到
  了 ret 缓冲区残留的 ret\[0]（上次返回值 -1.0），args\[2] 读到 ret\[1]，后面的读到脏内存。
  sphereHit 拿到错乱的 ox/dx 参数，几何上永远不命中

- **修复**：store 方向改为 `args_base + 8*i`，与 callout 读取方向一致

- **教训**：跨边界缓冲区（机器码写 / C 函数读）的方向约定必须写进注释并在两侧对齐；
  这类 bug 的特征是"程序不崩但结果全错"，最有效的定位手段是**在边界两侧都打印原始位模式**，
  一对比就现形

### 13.4 坑四：ret 缓冲区向高地址增长，越界覆盖 tmp/vstack 槽

- **症状**：设计期发现（代码走查时定位，未实际触发数据错乱）。原始布局下 ret 缓冲区
  从 tmp3 正下方一格开始向高地址增长：ret\[1] 即覆盖 tmp3、ret\[2] 覆盖 tmp2、ret\[3]
  覆盖 tmp1、ret\[4] 起覆盖 vstack 最深槽（向上逐槽推进）。Phase B（ret\_count=2）恰好
  没踩到正在使用的槽才"侥幸正确"；一旦循环体内同时有 OP\_INDEX/OP\_DICT\_SET 等用到
  tmp 槽的 callout、或 vstack 较深，数据即被静默破坏。若 n+v+4 < 16（小帧），
  ret\[i] 最终顶到 rbp+0（覆盖保存的 rbp）乃至返回地址

- **根因**：callout 写 ret\_out\[i] 时 i 递增、**向高地址增长**，但 ret\_base 放在了
  缓冲区最高端（tmp3 之下），增长方向直冲帧内数据区

- **修复**：ret\_base 挪到缓冲区最低端，让 ret\[15] 恰好紧贴 tmp3 之下（`ret_base_disp =
  -8 * (n + max_vstack + 3 + JIT_RET_MAX)`），增长方向只在自己 16 槽范围内

- **教训**：帧内缓冲区的"增长方向"和"容量边界"必须一起设计；向高地址增长的缓冲区，
  base 必须放最低端，否则静默踩踏 tmp/vstack/saved rbp

### 13.5 坑五：序列化缓存缺 return\_count 字段（反序列化出垃圾值）

- **症状**：JIT scan 读 `ObjFunction.return_count` 时得到 0（垃圾值），多返回值函数
  被当成 0 返回值处理

- **根因**：给 ObjFunction 新增 return\_count 字段后，serialize.c 的
  ObjFunction 序列化/反序列化没同步；且旧 `.lenomc` 模块缓存命中后跳过反序列化修复

- **修复**：序列化写入/读取 return\_count（u32），版本号 LENO\_BIN\_VERSION → 0x00020300、
  LENO\_MODCACHE\_VERSION → 0x2，使旧缓存整体失效

- **教训**：**任何给 ObjFunction/Chunk 等持久化结构加字段的改动，必须同步三处**：
  序列化写、反序列化读、版本号提升。否则旧缓存的字段错位比崩溃更难查

- **该基础设施已保留**（这是本次实验唯一留下的成果）：

  - `codegen_func.c`：`ast_return_count()` 编译期统计所有 return 语句的返回值个数
    （各 return 一致 → 记录个数；不一致 → -1；无 return → 1），存入
    `ObjFunction.return_count`

  - serialize.c / leno\_serialize.h：return\_count 的持久化

  - **未来内联实现可直接消费该字段做静态 vstack 跟踪**

### 13.6 根本问题：callout 性能模型不成立（实验失败的核心原因）

修完全部正确性 bug 后，结果对了，但性能惨败：

| Phase               | 纯解释    | callout JIT（正确性修复后） | 说明                 |
| ------------------- | ------ | ------------------- | ------------------ |
| A 数组索引（无调用）         | 219ms  | 47ms                | 不含调用，JIT 正常收益 4.6x |
| B sphereHit 300万次调用 | 859ms  | **\~18600ms**       | 负优化 21 倍（结果已正确）    |
| B2 单返回值 300万次调用     | 844ms  | **\~18700ms**       | 同上                 |
| C 递归 trace          | 2906ms | 崩溃（VM 栈下溢 sp=-2）    | 嵌套 JIT 问题，未深入定位    |

（中间态记录：args 方向 bug 未修时 B/B2 为 \~3600ms 但 hits=15 全错、C 为 297ms 但
hits=50 全错——"快"是因为几何判断全走 miss 短路路径，毫无意义）

- **根因**：callout 每次调用的固定开销 ≈ 参数逐个 raw→NaN-boxed 转换 + 保存/恢复
  5 个寄存器 + Windows x64 shadow space + `vm_call_value` 重入整个解释循环（含
  stop\_frame\_cnt 协议）+ 返回值逐个拷回。实测单次 \~5µs；而解释器原生 OP\_CALL 路径
  只是压帧改 ip 续跑 dispatch 循环，单次 \~0.2µs。**callout 比被它"加速"的路径贵 25 倍**，
  调用密集循环 JIT 后必然大幅负优化——这不是实现 bug，是方案结构性缺陷

- **附带发现**：嵌套 JIT（外层 JIT callout 进 trace，trace 的内层循环又被 JIT 编译）
  出现 VM 栈下溢崩溃（sp=-2），未深入定位——方案已废弃，问题一并消失。
  若未来内联实现引入"JIT 代码内再进解释器再进 JIT"的场景，需重新审视该

### 13.7 最终决策与现状

1. **scan 阶段直接拒绝含调用的循环**（jit.c，注释 "inlining not implemented"），
   B/B2/C 回退解释器，性能与纯解释版持平，A 保留 4.6x 收益
2. op\_call.inc 曾为 callout 加过"stop 分支压回返回值"的改动，**已还原**——该改动对
   arrays.map / FFI 回调等所有 vm\_call\_value 调用方都有栈漂移风险，属回归隐患
3. op\_jump.inc / op\_for\_loop.inc 的 `jit_try_hot_loop` 返回值 2（异常转移）分支保留，
   与调用实验无关

### 13.8 后续正确方向：被调函数内联

要真正让 Phase B/B2/C 命中 JIT，唯一可行路径是把 callee 的字节码**直接编进宿主循环的
机器码**（inline），而不是经 C callout 绕行解释器。要点：

1. **触发条件**：callee 是脚本闭包、`return_count >= 0`（编译期可确定，字段已在）、
   循环体内调用点固定（func\_slot 静态可读）、callee 无 try/upvalue 捕获
2. **返回值个数**：直接用 `ObjFunction.return_count`（13.5 的基础设施），vstack 静态可跟踪
3. **参数传递**：宿主虚拟栈弹出 N 个值 → 直接映射为 callee 局部变量的 scratch 槽，
   无需 NaN-boxed 缓冲区（避开坑三/坑四的整个缓冲区设计）
4. **多返回值解构**：callee 的 OP\_RETURN\_MULTI → 机器码跳到内联体的出口序列，
   把返回值压回宿主虚拟栈
5. **递归（Phase C 的 trace）**：内联只展开固定深度（如 1-2 层），深层递归仍走解释器
   OP\_CALL——只有 method JIT（P3）才能根治
6. **内联体里的 bailout**：类型守卫失败时整体退出 JIT 回解释器，从调用点重新执行

### 13.9 本次验证有效的调试手段（复用价值）

| 手段                          | 用法                                   | 破案场景                              |
| --------------------------- | ------------------------------------ | --------------------------------- |
| `LENO_NO_JIT=1`             | 同一二进制禁用 JIT 跑对照                      | 隔离"解释器 bug"vs"JIT 机器码 bug"        |
| `LENO_JIT_DUMP=1` + objdump | dump 生成的机器码反汇编核对                     | 坑一（ModRM）、坑二（r9）肉眼可辨              |
| 边界两侧打印位模式                   | callout 入口打 args、出口打 ret 的 0x%016llx | 坑三：a1=上次返回值，一眼锁定错位                |
| `--debug-out` 字节码导出         | 确认源码层生成的操作码序列                        | 排除解构（OP\_SET\_LOCAL\_POP 15/14）嫌疑 |
| 结果数字反推                      | hits=15 ≈ 50 次解释执行的命中数               | 确认"JIT 接管后全错"而非"偶发"               |

***

## 14. JIT 与解释器语义差异清单（核对用）

本节的用途：**新增/修改一个 opcode 或 callout 时，逐条对照，避免「JIT 快是快，但算出来的
东西和解释器不一样」**。这类差异不会崩、只会静默算错，而且往往只在大循环里才显形
（§8.15 / §8.17 / §8.18 / §8.19 全是这个套路）。

结论先写在这里：**JIT 的每一个语义分支都必须能在解释器里指到对应的一行**；
指不到的那一支，要么 bailout，要么在表里显式登记为「已知差异」。

### 14.1 算术 / 比较 / 转换语义对照

| 领域 | 解释器语义（`src/vm/vminc/`） | JIT 做法 | 状态 |
| --- | --- | --- | --- |
| 通用 `OP_ADD` | int+int（溢出升 BigInt）→ 任一 float 则 float 加 → 任一 ObjString 则拼接 → 否则类型错误 | int 快路径 + `EMIT_NUM_TO_XMM` float 慢路径 + concat callout（双方非字符串 → bailout） | ✅ |
| 通用 `OP_SUB` / `OP_MUL` | int 运算（溢出升 BigInt）→ float → 类型错误 | int 快路径（`EMIT_INT64_OVF_CHECK` + `EMIT_INT48_CHECK`，两个检查缺一不可）→ float 慢路径 → NaN-boxed bailout | ✅ |
| 通用 `OP_MOD` | **仅 int%int**（除零抛错）；float 落到「取模操作数必须是整数」 | 仅 int48 快路径，并自行挡除数为 0 → 其余 bailout | ✅ |
| 通用 `OP_EQ` / `OP_NEQ` | int/float 数值比较 → 字符串按内容 → 数组逐元素 → 其它对象按指针 → BigInt → FFI 指针与 null | int 快路径（`JE`/`JNE`）→ float（IEEE：无序时 `==` 需 `AND SETNP`、`!=` 需 `OR SETP`）→ NaN-boxed bailout | ✅ |
| 通用 `OP_LT..GE` | int/float 数值（与 NaN 比较一律 false）→ BigInt → 字符串 | int 快路径 → float（`JB/JBE` 再 `AND SETNP` 压无序；`JA/JAE` 天然正确）→ 其余 bailout | ✅ |
| `OP_*_FLOAT` 算术 | `val_as_num_ex`：int/float 提升、BigInt 转 double、**其余一律 0.0** | `EMIT_FLOAT_ARGS2`：int48 → `CVTSI2SD`、裸 double → 直搬、NaN-boxed → bailout | ✅（2026-09-12，§8.18） |
| `OP_*_FLOAT` 比较 | `val_as_num`：int/float 提升、其余 0.0（无 BigInt 转换） | 同上 | ✅（2026-09-12，§8.18） |
| `OP_DIV_FLOAT` 除零 | 抛「浮点除零错误：除数为 0.0」 | 与 0.0 比较 → bailout；NaN 除数放行（DIVSD 得 NaN） | ✅（2026-09-12，§8.19） |
| 类型化整数运算（`OP_ADD_INT` …） | 溢出升 BigInt | `EMIT_INT48_CHECK` / `EMIT_INT64_OVF_CHECK` → bailout | ✅ |
| `OP_DIV_INT`（int/int） | `val_int(a / b)`：**整除、向零截断、结果 int** | `cqo; idiv` → 商作为 **int48** 压栈 + `EMIT_INT48_CHECK`；除数为 0 → bailout | ✅（2026-09-12，§8.23） |
| `OP_MOD_INT`（int%int） | `val_int(a % b)`；除数为 0 抛「取模除零错误」 | `cqo; idiv` 取 RDX；除数为 0 → bailout | ✅（2026-09-12，§8.23） |
| `OP_CAST_FLOAT` | int→`(double)`、bool→1.0/0.0、BigInt→double、null 保持 null | int48 → `CVTSI2SD`；其余**原样透传** | ⚠️ 见 14.4-① |
| BigInt 参与算术/比较 | BigInt 路径 | NaN-boxed → bailout 交解释器 | ✅（有意为之） |
| 逻辑非 `OP_NOT` | `val_bool(is_falsey(v))`；假值 = `int 0` / `float ±0.0` / `false` / `null`，**NaN 为真** | 三态分派（int48 / NaN-boxed 全值比较 `FALSE_VAL`·`NULL_VAL` / 裸 double `UCOMISD`+`JP`）→ 压 NaN-boxed `TRUE_VAL`/`FALSE_VAL` | ✅（2026-09-12，§8.22） |
| 比较结果的**类型** | 压 `val_bool`（`TRUE_VAL` / `FALSE_VAL`） | 同解释器：比较 opcode 在产出点调 `EMIT_RAW01_TO_BOOLVAL()` 压 `val_bool`（**用 ADD 拼装，不能用 OR**，见 §8.24） | ✅（2026-09-12，§8.24） |
| struct 方法调用的**返回值个数** | 由 callee 的 `return_count` 决定（压 N 个值） | 编译期 `jit_resolve_method_ret_count()` 解析；解析不出 → 拒绝 JIT | ✅（2026-09-12，§8.20） |
| 循环体内可达的 `return` | 真返回（弹帧、写回栈） | 循环 JIT 表达不了 → **scan 拒绝该循环** | ✅（2026-09-12，§8.21） |
| `len()` / 模块方法 / FFI 等 callout | 原生实现 | 同一原生函数（callout 调用），除参数/返回值装箱外无第二份实现 | ✅ |

> **一条由此得出的通用原则**：JIT 中途算错的值**通常不会泄漏** ——
> bailout 会整迭代重放、且不写回 locals，所以「跑到一半发现不对」的那批值会被丢弃（14.4-① 就是这样）。
> 真正危险的是「**JIT 把整个循环跑完、再写回 locals**」的那一批值：错值会安静地传给解释器
> （14.4-② 即此类）。**优先怀疑"循环跑完才暴露"的错值。**

### 14.2 callout 取实参方向（**最容易踩的一类**）

JIT 虚拟栈是「栈顶在低地址」的反向栈，`vstack_top` 永远指向**最后压入**的那个值。
但不同 opcode 的压栈顺序不同，取参公式必须逐个对照：

| opcode / callout | 压栈顺序 | 取参公式 |
| --- | --- | --- |
| `OP_MODULE_CALL`（`jit_callout_module_call`） | 实参按源码顺序，第一个实参先压（最高地址） | `arg[i] = vstack_top[arg_count-1-i]` |
| `OP_CALL_GLOBAL_FUNC[_TYPED]`（`jit_callout_global_func`） | 同上 | 同上 |
| `OP_STRUCT_INIT` / `OP_ARRAY` / `OP_CALL_NATIVE` | 同上 | 同上 |
| `OP_INVOKE_METHOD_TYPED`（struct 方法，`arg_count` 含 self） | **receiver 最先压**，再压实参 | `receiver = vstack_top[arg_count-1]`；`flocals[i] = vstack_top[arg_count-1-i]` |
| `OP_GET_PROPERTY` + `OP_CALL` 合并（原生方法） | **实参先压，receiver 最后压（栈顶）** | `receiver = vstack_top[0]`；`arg[i] = vstack_top[arg_count - i]`（§8.17 的坑） |

> 判据：编译器生成 `obj.m(a, b)` 时，`OP_GET_PROPERTY` 需要 receiver 在 TOS，所以实参先压；
> 而 `OP_INVOKE_METHOD_TYPED` 的约定是 receiver 占参数区首位，所以最先压。**写新 callout 前先
> `--debug-out` 看一眼字节码序列，别凭直觉。**
>
> **多返回值补充**：`OP_CALL_GLOBAL_FUNC[_TYPED]` 与 `OP_INVOKE_METHOD_TYPED` 都可能一次返回 N 个值
> （`ObjFunction.return_count > 1`）。约定是：callout 返回**最后一个**返回值（作为新 TOS），
> 其余 N-1 个直接写回虚拟栈的**实参槽** `vstack_top[arg_count-1-i]`（i = 0..N-2），
> 与 codegen 的「弹 `arg_count - ret_count + 1` 槽」配对。少了这段回填，
> 解构出来的第一个值就是上一帧的栈残留（§8.20 的 PvZ 数字抖动）。

### 14.3 新增 opcode / callout 的一致性检查清单

1. **三处同步**：`jit_scan.c` 的 `opcode_size()`、`scan_loop_body()` 的 vstack switch、
   `scan_callee_for_inline()` 的 vstack switch。漏任一处 → 整个循环 `scan FAIL`（不是 bailout）
2. `opcode_size()` 与 VM 侧的 `READ_*` 数量必须完全一致（变长 opcode 用 `ip[n]` 取值）
3. vstack 净效应（pop N push M）与 codegen 里 `vstack` 的增减必须一致
4. **`tos_live` 约束**：所有 `TOS_CONSUME` 必须在任何类型判定跳转**之前**（§8.16）
5. **数值提升**：以解释器那侧调用的函数为准 —— `val_is_int` / `val_as_num`
   （int/float，其余 0.0）/ `val_as_num_ex`（额外 BigInt）
6. **溢出**：区分「int48 截断」与「int64 溢出」两条分支，确认哪条对应解释器的 BigInt 提升
7. **除零**：整数除/模、浮点除三种除零都必须与解释器的报错行为一致（JIT 挡下 → bailout 让解释器抛）。
   **2026-09-12 已全部补齐**：`OP_DIV`（callout 内判零）、`OP_MOD`（自挡）、`OP_DIV_FLOAT`（§8.19）、
   `OP_DIV_INT` / `OP_MOD_INT`（§8.23）。此前后两者会直接 `idiv 0` 触发 `#DE` 崩进程。**新增任何
   除法/取模类 opcode 时，先确认这一条**。
8. **结果类型**：解释器压的是 `val_bool` 还是 `val_int`（见 14.4-②）
9. **失败路径**：`jit_callout_failed` 置位 + `EMIT_BAILOUT_SITE_NONOVF(bc_off)` 写 site。
   不写 site 会让日志里的 `site=` 停留在**上一次**写过的值，排查方向被带偏
10. **callout 取参方向**按 14.2 表，并在代码注释里写明该 opcode 的压栈顺序
11. **回归**：差分探针（`LENO_NO_JIT=1` 对照，并用 `LENO_JIT_DEBUG=1` 确认 `Executed > 0`）
    + 一个 `assert/test_jit_*.leno`；断言要**先在未修复代码上跑一遍确认会失败**
12. **加 bailout 分支后必须复测 `Bailouts` 计数**（§8.19 的教训：功能对了但每次都回退）
13. **结果类型的位模式**：bool 结果必须是 NaN-boxed `val_bool`；拼装时注意
    `FALSE_VAL` 与 `TRUE_VAL` 相差 `1<<48` 且**必须用 ADD**（`TAG_FALSE` 自带第 48 位，
    OR 是空操作）—— 写成 OR 会让比较结果恒为 false、**所有 `while` 循环第一轮退出**
    （§8.24，且反汇编看起来完全正确，极易误判成 codegen/CPU 问题）
14. **写回侧的槽位类型**：类型位图只记录「**进循环时**该槽位是不是 int」，槽位在循环里
    仍可能被写入 NaN-boxed 值 → int 分支要先确认「确实是 int48」再重装箱，否则会把
    `TRUE_VAL` 的位模式搅成垃圾 int（§8.24）
15. **函数级 JIT 的准入条件**（`jit_compile_function`，任一不满足就拒绝，且**静默**拒绝）：
    无 `OP_LOOP`/`OP_FOR_LOOP`/`OP_FOR_PREP`（`func_body_is_simple`）、无 `has_try`、
    `return_count <= 1`、body 内所有 opcode 都在 scan 支持列表内（含 upvalue /
    `OP_TAIL_CALL` / 泛型相关 opcode → 一律拒收）。新增 opcode 时如果它可能出现在
    函数体里，必须想清楚「是否要支持函数级 JIT」。
16. **解释器侧热入口（§8.25）的三个自伤点**：
    ① `jit_func_lookup_or_compile` 的 `tried` 是三态（未尝试 / 已编译 / 尝试失败）——
       解释器侧会先建「只有计数器」的条目，命中该条目时**必须补编译**，
       否则 JIT 循环会永远不再编译该函数；
    ② 栈折叠约定必须与 `call()` 完全一致（`sp -= arg_count`，结果落原 callee 槽，
       并同步 `last_return_value/last_return_count/last_return_values`）；
    ③ `typed` 调用点不做参数提升，与 `call_no_type_check` 对齐。
17. **内联体的 bc_off 命名空间必须按「实例」隔离**（§8.26）：`off_map` 与
    `ctx->patches` 都以 bc_off 为键，`offmap_lookup` 返回**第一个**匹配项。
    同一循环内有多个内联点时，每个实例必须有不同的基址
    （当前 `0x10000 * (inline_idx + 1)`）；按 depth 编号在「只有一层内联」的
    现实下等于不隔离，第 2..N 个实例的内部跳转会被解析到第 1 个实例的机器码
    → 死循环 + RSP 漂移 → `0xC00000FD`。新增任何「以字节码偏移为键」的表时，
    都要先确认它在内联场景下是按实例隔离的。
18. **以数量为界的固定数组：读不到/写不下都必须让本次编译失败**（§8.27）。
    绝不允许"跳过这一条继续生成" —— 那会产出错误机器码（跳错地方 / `rel32=0`），
    而且症状是崩溃或静默算错，离现场很远。现有做法：
    `offmap_add` / `patch_add` 置溢出标志 → 收尾拒绝；
    回边 `offmap_lookup` 失败、内联命名空间目标解析失败、体内目标解析失败、
    `scan` 的前向跳转表满、内联 `return` 补丁表满 → 一律拒绝编译。
19. **位宽与移位方向必须与字段/位模式匹配**（§8.28 / §8.29 两次踩坑）：
    * 读 4 字节整型字段（如 `ObjArray.count@40`）用 `emit_mov_reg32_mem8`
      （零扩展）、写用 `emit_mov_mem32_reg32`；用 64 位版会把相邻字段一起读/写
      （`count|capacity<<32` 让越界判定恒不成立 → 越界读被静默跳过）。
    * **int48 判定用 SAR**（需要符号位扩散），**NaN-boxing 的 top16 判定用 SHR**
      （`0xFFFC` 的最高位是 1，SAR 符号扩展成 `0xFFFF...FFFC` 后比较恒不等，
      快路径直接变死代码 —— 现象是"改了但完全没效果"）。
20. **新增内联快路径后必须确认它真的在执行**（§8.29）：先看 `Compiled/Executed/
    Bailouts`，再用 `LENO_JIT_DUMP=1` + objdump 确认快路径的特征指令确实生成，
    最后才看耗时。只对比耗时容易把"快路径是死代码"误判成"这类操作没有优化空间"。

### 14.4 已知差异（未修，**改动相关代码时要留意**）

**① `OP_CAST_FLOAT` 对非 int 值原样透传**
解释器（`op_unary.inc`）对 bool → 1.0/0.0、BigInt → double、null 保持 null；
JIT 只处理 int48，其余原样透传（`ops_float.inc`）。
因为值本身没被破坏，下游的 `OP_*_FLOAT` 现在遇到 NaN-boxed 操作数会 bailout（§8.18），
所以**不会静默算错**，但 `float f = <bool>` 这类循环会退化成「每次迭代都回退」。

实测（`build/probe9.leno`）：`float f = o.get("flag", false); acc = acc + f + 1.0`
两模式都是 `600.0` ✓ —— 因为**回退会整迭代重放、且不写回 locals，JIT 中途算错的值不会泄漏**。
彻底修法：按解释器分支补 bool/null/BigInt 三条快路径（可同时消掉这里的回退）。

**② 比较结果的位模式：裸 0/1 vs `val_bool`**
JIT 的比较 opcode 产出裸 0/1；写回 locals 时按 slot 的类型位图决定装箱还是裸存，
而「原值是 bool」的 slot 在位图里标记为"非 int" → **裸存** →
解释器把 `0x…0001` 读成次正规 float（≈4.9e-324 / 0.0）。

```leno
// 复现（本地探针 build/probe8.leno，未入库；断言化的部分见 14.5）
bool flag = false
while k < iters { flag = (k >= 0); k = k + 1 }   // 热循环 → 进 JIT
print(flag is bool)     // 解释器 1(bool) / JIT 2(float)
print(_int(flag))       // 解释器 1      / JIT 0   ← 静默算错
print(not flag)         // 两边都是 0（真值判断恰好正确）
```

影响面：真值判断（`if` / `while` / `not`）恰好正确，**但 `is bool`、`_int(bool)`、
与 `true`/`false` 比较、以及把结果序列化（JSON 会写出 `4.9e-324`）都不一致**。

**✅ 已修复（2026-09-12，§8.24）**：比较 opcode 现在在**产生处**就产出 NaN-boxed
`TRUE_VAL`/`FALSE_VAL`（`EMIT_RAW01_TO_BOOLVAL()`：`mov rdx,rax; shl rdx,48;
movabs rax,FALSE_VAL; **add** rax,rdx`），并同步修掉 `OP_CAST_INT` / `OP_CAST_FLOAT`
对 NaN-boxed bool 的处理与写回侧的「盲装箱」。

> **实现陷阱（务必记住）**：两个 tag 相差 `1<<48`，但 `TAG_FALSE` **自己就占着第 48 位**，
> 所以 `FALSE_VAL | (1<<48) == FALSE_VAL`（OR 是空操作，得到的比较结果恒为 false，
> 表现为**所有 `while` 循环第一轮退出**），必须用 `FALSE_VAL + (1<<48)`。

**③ 函数级 JIT 中途 bailout 会重跑整个调用，堆侧副作用无法回滚**

解释器侧热入口（`jit_try_hot_func_call`，§8.25）与 `jit_callout_*` 快路径同构：
JIT 机器码返回非 0（bailout / `jit_callout_failed`）时，把控制权交回解释器，
由解释器**完整重跑**这次调用。JIT 中途已经发生的堆侧副作用
（数组 `append`、字典写、`print`、FFI 写内存）不会被撤销。

- 影响面：只有「JIT 跑到一半失败 + 调用方 catch 住异常后继续跑」才会看到重复副作用。
  纯函数递归（fib 类）无影响。
- 为什么接受：能回滚就得给 callout 加事务/日志，成本远超收益；
  `jit_callout_*` 从引入起就是这个取舍（13.6 的结论）。
- 缓解：`hot_disabled` —— 一次失败即**永久**停用解释器侧入口，
  避免「每次调用都进 JIT 再回退」的反复试探与反复重跑。
- **新增 callout / 新 opcode 时**：若它的 JIT 分支可能在产生副作用之后才 bailout，
  必须在 §14 表里登记，并确认「重跑一次」不会破坏语义（比如不能有计数器式副作用）。

### 14.5 回归断言索引

| 断言文件 | 覆盖 |
| --- | --- |
| `assert/test_jit_method_args.leno` | 原生方法 callout 实参顺序（§8.17）：`_relayout` 累加循环现场 + `Dict.get(key, def)` |
| `assert/test_jit_float_ops.leno` | `OP_*_FLOAT` int 操作数提升（§8.18）、热循环内浮点除零报错（§8.19） |
| `assert/test_ffi_inline_widths.leno` | ffi 定宽内存读写内联 + 越界/空指针/已释放的 bailout 报错 |
| `assert/test_jit_multiret_method.leno` | struct 方法多返回值（§8.20）：双/三返回值 + 多返回值调用位于函数级 JIT 函数体内 |
| `assert/test_jit_not_bool.leno` | `OP_NOT` 对 NaN-boxed bool / int48 / 裸 double 的三态分派（§8.22）：6 种取值形态各一个热循环 + 嵌套 `not`+`continue` |
| `assert/test_jit_int_div.leno` | `OP_DIV_INT` 结果类型/向零截断（§8.23）：取位串逐字符比对、`100/3 is int`、浮点除法不受影响、热循环内除零/模零必须可捕获而不崩进程 |
| `assert/test_jit_bool_compare.leno` | 比较结果必须是 `val_bool`（§8.24）：5 种 while 条件形式（立即数/局部/不等/反向/浮点）+ bool/int/float 局部量与全局 + 数组/字典/JSON 的 callout 实参路径 + `and`/`or`/`not` + 返回值路径 |
| `assert/test_jit_hot_func.leno` | 解释器侧函数级 JIT 热入口（§8.25，**语义守卫**，基线版本同样 pass）：纯递归 + `is int` 结果类型、深度 200 > `JIT_FUNC_MAX_DEPTH`(64) 的深度守卫、互递归 bool 返回值 + `is bool`、int48 溢出（`pow2(60)` 必须与解释器的 BigInt 语义一致）、float 形参 + int 实参提升、字符串 concat 递归、`return_count>1` 与 `has_try` 必须拒绝编译、低热度不编译、递归返回 `Dict`、函数体内含 struct 多返回值方法调用 |
| `assert/test_jit_multi_inline.leno` | 循环体内**多个内联点**的 bc_off 命名空间隔离（§8.26）：2 个 / 4 个 `gm()`（GF(2^8) 乘法，体内有 for + 两处 if）内联点，逐元素全文比对 + 20 轮反复调用。**未修复的代码上会以 `0xC00000FD` 崩溃**（不是断言失败），修复后通过 |
| `assert/test_jit_array_bounds.leno` | 数组索引读/写与 append 的内联快路径（§8.28 / §8.29）：越界与负下标必须**像解释器一样被 try/catch 捕获 239 次**、合法下标读值正确、`add` 后 len 与内容正确、下标写覆盖且不破坏相邻元素、写入的是 Value（`is int`）、扩容不丢数据、字典慢路径仍正确。**未修复代码（P5 前）输出「越界捕获=0」并 FAIL**；同时覆盖 `arr[i]=v` 快路径的 Value 装箱 |
| `assert/test_jit_rsp_drift.leno` | 合并点两条到达路径的 TOS 形态必须一致（§8.50）：`and` 短路 / 单条件 / `or` 短路三种形态 + 值断言。101 桶 × 20000 轮（2.02M 轮内层迭代）把「每轮漏 8 字节」放大到栈溢出量级。**未修复的代码上会以 `0xC00000FD` 崩溃**（不是断言失败），修复后通过 |
| `assert/test_jit_inline_method_dispatch.leno` | 内联侧 `OP_GET_METHOD` 的栈记账（§8.68 / R7②）：face 动态派发被内联时的 1 参 / 2 参（不同 argc ⇒ 不同记账值）与**同一函数体内两处派发**必须取值精确，两个 face 实现走运行时分发。**它是「修后记账守卫」而非「修前必失败」用例**：修复前只是不内联、结果依然正确 ⇒ 「是否真的内联」由 `jit_probes/probe_inline_method_call.leno` 的 `inline(...)` 行判定 |
| `assert/test_jit_inline_module_var.leno`(+`assert/jit_inline_modvar_mod.leno`) | 内联侧模块变量访问的**安全边界**（§8.69 / R7①）：同模块（辅助模块内部 `runSame` → `Acc.bump`）允许内联且取值精确（`8n(n-1)+10n`）；**跨模块必须仍拒绝内联**（期望值由实测模块状态推出，不硬编码）。同模块那侧必须整体放在被 import 的模块里 —— 入口脚本的顶层 `var` 是 `OP_GET_GLOBAL`，走不到这条路径 |

***

## 附：关键文件索引

| 文件                             | 说明                                           |
| ------------------------------ | -------------------------------------------- |
| `src/jit/jit.c`                | compile 驱动 + 缓存 + `jit_try_hot_loop` + 统计打印 |
| `src/jit/jit.h`                | 公共 API 和配置参数                                 |
| `src/jit/jit_scan.c`           | 热循环扫描 / 可 JIT 判定 / 内联分析                      |
| `src/jit/jit_callout.c`        | 运行期 C 辅助函数（callout、bailout 调试、`jit_debug_on`） |
| `src/jit/jit_mem.h`            | 可执行内存分配（VirtualAlloc/mprotect）+ `jit_mem_flush()`（写完代码刷指令缓存） |
| `src/jit/backend/x86_64.c`     | x86_64 codegen 骨架 + 通用宏（`EMIT_NUM_TO_XMM` 等）   |
| `src/jit/backend/x86_64_emit.h` | x86\_64 指令发射函数与寄存器编号                          |
| `src/jit/backend/x86_inc/`     | 按 opcode 家族拆分的 codegen case（`ops_arith/icmp/fcmp/loop/callout/local/...`） |
| `src/vm/vminc/op_for_loop.inc` | OP\_FOR\_PREP / OP\_FOR\_LOOP 的 VM 实现        |
| `src/vm/vminc/op_loop.inc`     | OP\_LOOP 的 VM 实现                             |
| `src/include/leno_vm.h`        | OpCode 枚举定义                                  |
| `src/include/leno_value.h`     | NaN-boxing Value 定义                          |
| `examples/性能测试/全部测试.leno`      | 性能测试文件                                       |
| `assert/run_tests.leno`        | 回归测试入口                                       |

