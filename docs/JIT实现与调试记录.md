# Leno JIT 编译器实现与调试记录

> 文档记录 Leno VM JIT 编译器的架构设计、已支持字节码清单、调试方法、踩过的坑及解决方案。
> 代码位置：`src/jit/jit.c`、`src/jit/jit.h`、`src/jit/jit_emit.h`、`src/jit/jit_mem.h`

## 目录

- [1. 架构概述](#1-架构概述)
- [2. 寄存器约定](#2-寄存器约定)
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

---

## 1. 架构概述

Leno JIT 是一个**模板 JIT（template JIT）**，工作在栈式 VM 的热循环上，不修改字节码格式和 VM 主分发循环。

### 工作流程

```
VM 执行 OP_LOOP / OP_FOR_LOOP
  → jit_try_hot_loop() 被调用
  → hit_count++ → 达到阈值 (50次) → 尝试编译
  → scan_loop_body(): 扫描循环体，检查所有 opcode 是否可 JIT
  → compile_loop(): 生成 x86_64 机器码
  → 缓存到 jit_state.cache[]
  → 下次命中同循环 → 直接执行 JIT 代码
  → 执行成功返回 0 / 类型溢出 bail out 返回 1
```

### JIT 函数签名

```c
int (*JitLoopFn)(Value* locals, Value* globals);
// 返回 0 = 成功，1 = bailout（类型溢出，回退 VM）
```

### 关键参数 (`jit.h`)

| 参数 | 值 | 说明 |
|------|----|------|
| `JIT_HOT_THRESHOLD` | 50 | 循环执行 50 次后触发编译 |
| `JIT_CACHE_SIZE` | 256 | 缓存哈希表大小（2 的幂） |
| `JIT_MAX_LOCALS` | 32 | 最多 type-guard 的 local 数 |
| `JIT_MAX_LOOP_OPS` | 256 | 循环体最大 opcode 数 |
| `JIT_MAX_VSTACK` | 64 | 虚拟栈最大深度 |
| `JIT_BAILOUT_LIMIT` | 3 | bailout 3 次后放弃此循环 |

---

## 2. 寄存器约定

Windows x64 ABI，JIT 函数入口 `RCX = Value* locals`，`RDX = Value* globals`。

| 寄存器 | 用途 | 保留方式 |
|--------|------|----------|
| RCX | locals 指针（第一参数，固定） | 调用者保存 |
| RDX | globals 指针（入口参数，prologue 复制到 R9） | - |
| R9 | globals 指针（运行期间固定） | 调用者保存 |
| R10 | `PAYLOAD_MASK = 0x0000FFFFFFFFFFFF` | prologue 加载 |
| R11 | `INT_TAG = 0xFFFB000000000000` | prologue 加载 |
| RBX | 类型位图：bit i=1 表示 local i 是 float/obj | callee-saved，prologue xor 清零 |
| RBP | 帧指针 = scratch 区基址 | callee-saved |
| RSP | 虚拟栈顶（JIT push/pop 用） | 运行期间管理 |
| RAX | 通用 scratch | volatile |
| RDX | 通用 scratch（注意：入口时是 globals，prologue 后释放） | volatile |
| R8 | int48 检测 scratch | volatile |
| R12 | callout: 保存 RSP | callee-saved |
| R13 | callout: 保存 RCX | callee-saved |
| R14 | callout: 保存 R9 | callee-saved |

### Epilogue（不能使用 LEAVE）

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

---

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

JIT 虚拟栈不存 NaN-boxed Value，而是存 **raw int64_t**：

| 值类型 | 虚拟栈存储格式 | 检测方法 |
|--------|---------------|----------|
| int | int48 raw（符号扩展到 int64） | `sar r8,47; inc r8; cmp r8,1; ja .not_int` — 结果 ≤1 是 int |
| float | 原始 IEEE 754 double bits | 上述检测 >1 |
| null/bool/obj | 原始 NaN-boxed bits | 上述检测 >1（top16 ≠ 0xFFFB） |

### int48 检测惯用法

```asm
mov r8, rax      ; 复制值
sar r8, 47       ; 算术右移 47 位
inc r8           ; 正确 int48: 0→1 或 -1→0，都 ≤1
cmp r8, 1
ja  .bailout     ; >1 表示溢出 int48 范围 [-2^47, 2^47-1]
```

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

---

## 4. 已支持字节码清单

### 栈操作

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 4 | OP_ZERO | 1 | 压入 0（`xor rax,rax; push rax`） |
| 5 | OP_ONE | 1 | 压入 1 |
| 6 | OP_POP | 1 | 弹出（`add rsp,8`） |
| 7 | OP_DUP | 1 | 复制栈顶 |

### 局部变量

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 8 | OP_GET_LOCAL | 3 | 加载 local 到虚拟栈 |
| 9 | OP_SET_LOCAL | 3 | peek 栈顶写入 local（不弹） |
| 10 | OP_SET_LOCAL_POP | 3 | 弹出写入 local |
| 11 | OP_MOVE_LOCAL | 5 | local 间复制（压栈） |
| — | OP_MOVE_LOCAL_POP | 5 | local 间复制（不压栈） |
| — | OP_SET_LOCAL_CONST | 5 | 常量直接写入 local；非 int/float 常量存 raw bits + BTS RBX 标记 |

### 常量

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 0 | OP_CONST | 3 | 压入常量；int→int48 raw，float→raw double bits，其他→raw NaN-boxed bits |

### 整数算术

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_ADD_INT | 1 | `add rax, rdx` + int48 检测 |
| — | OP_SUB_INT | 1 | `sub rax, rdx` + int48 检测 |
| — | OP_MUL_INT | 1 | `imul rax, rdx` + int64 溢出检测 + int48 检测 |
| — | OP_MOD_INT | 1 | `cqo; idiv r8` 取余数 |
| — | OP_NEG_INT | 1 | `neg rax` + int48 检测 |
| — | OP_ADD_INT_IMM | 2 | `add rax, imm8` + int48 检测 |
| — | OP_SUB_INT_IMM | 2 | `sub rax, imm8` + int48 检测 |
| — | OP_MUL_INT_IMM | 2 | `imul rax, imm` + 溢出检测 |

### 整数除法（返回 float）

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_DIV_INT | 1 | `cqo; idiv r8; cvtsi2sd xmm0, rax` → 压入 raw double bits |

### 位运算

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_BITAND | 1 | `and rax, rdx` + int48 检测 |
| — | OP_BITOR | 1 | `or rax, rdx` + int48 检测 |
| — | OP_BITXOR | 1 | `xor rax, rdx` + int48 检测 |
| — | OP_BITNOT | 1 | `not rax`（`F7 /2`） + int48 检测 |

### 类型转换

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_CAST_INT | 1 | no-op（JIT 上下文中值已是 int） |
| — | OP_CAST_FLOAT | 1 | 运行时类型分派：int→`cvtsi2sd`，float→直通 |

### 浮点算术

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_ADD_FLOAT | 1 | SSE2 `ADDSD`（`0xF2` 前缀） |
| — | OP_SUB_FLOAT | 1 | SSE2 `SUBSD` |
| — | OP_MUL_FLOAT | 1 | SSE2 `MULSD` |
| — | OP_DIV_FLOAT | 1 | SSE2 `DIVSD` |
| — | OP_NEG_FLOAT | 1 | `XORPD` 翻转符号位 |

### 浮点比较

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_EQ_FLOAT | 1 | `UCOMISD` + `SETE` |
| — | OP_LT_FLOAT | 1 | `UCOMISD` + `SETB`（无符号小于） |
| — | OP_GT_FLOAT | 1 | `UCOMISD` + `SETA`（无符号大于） |
| — | OP_LE_FLOAT | 1 | `UCOMISD` + `SETBE` |
| — | OP_GE_FLOAT | 1 | `UCOMISD` + `SETAE` |

### 整数比较

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_EQ_INT | 1 | `cmp rax, rdx; sete al` |
| — | OP_LT_INT | 1 | `cmp; setl`（有符号） |
| — | OP_GT_INT | 1 | `cmp; setg` |
| — | OP_LE_INT | 1 | `cmp; setle` |
| — | OP_GE_INT | 1 | `cmp; setge` |
| — | OP_LT_INT_IMM | 2 | 与 imm8 比较 |
| — | OP_GT_INT_IMM | 2 | |
| — | OP_LE_INT_IMM | 2 | |
| — | OP_GE_INT_IMM | 2 | |
| — | OP_EQ_INT_IMM | 2 | |

### 自增自减

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_INC_LOCAL_NOPUSH | 3 | `add [rbp+disp], 1` + int48 检测 |
| — | OP_DEC_LOCAL_NOPUSH | 3 | `sub [rbp+disp], 1` + int48 检测 |
| — | OP_INC_LOCAL | 3 | push 旧值后自增 |
| — | OP_DEC_LOCAL | 3 | push 旧值后自减 |
| — | OP_PRE_INC_LOCAL | 3 | 自增后 push 新值 |
| — | OP_PRE_DEC_LOCAL | 3 | 自减后 push 新值 |
| 40 | OP_INC | 1 | 栈顶 ++（pop, add, push） |
| 41 | OP_DEC | 1 | 栈顶 -- |

### 逻辑运算

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 34 | OP_NOT | 1 | 逻辑非：`test rax,rax; sete al; movzx` |

### 移位

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_SHL_IMM | 2 | `shl rax, imm8` |
| — | OP_SHR_IMM | 2 | `sar rax, imm8`（算术右移） |
| — | OP_USHR_IMM | 2 | `shr rax, imm8`（无符号右移） |

### 全局变量

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 12 | OP_GET_GLOBAL | 3 | 加载 global：int 路径提取 int48，float 路径直通，非数值 bail out |
| 13 | OP_SET_GLOBAL | 3 | peek 栈顶写入 global：int 重编码，float 直通 |

### 跳转

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| — | OP_JUMP | 5 | 无条件跳转（`JMP rel32`） |
| — | OP_JUMP_IF_FALSE | 5 | 弹出条件，为 0 跳转 |
| — | OP_JUMP_IF_TRUE | 5 | 弹出条件，非 0 跳转 |
| — | OP_CMPJMP_LL_INT | 10 | 两个 local 比较+条件跳转 |

### 循环回边

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 58 | OP_LOOP | 5 | while 循环回边（无条件跳转到循环头） |
| 96 | OP_FOR_LOOP | 7 | for 循环回边（自增 loop_var、比较 end、条件跳转） |

### Callout 操作（通过 C 函数调用实现）

| Opcode | 枚举名 | 字节数 | 说明 |
|--------|--------|--------|------|
| 91 | OP_INDEX | 1 | 数组/字典索引访问（callout: `jit_callout_index`） |
| 82 | OP_ARRAY_APPEND_NOPUSH | 1 | 数组追加（callout: `jit_callout_array_append`） |
| 71 | OP_DICT_SET | 1 | 字典赋值（callout: `jit_callout_dict_set`） |

---

## 5. 不支持的字节码（导致 scan 拒绝）

以下字节码出现在循环体中时，`scan_loop_body` 会将整个循环标记为 `capable=0`（不可 JIT）：

| Opcode | 枚举名 | 原因 |
|--------|--------|------|
| 83 | OP_TRY | 异常处理涉及 VM 异常栈、try/catch 表等复杂状态 |
| 84 | OP_CATCH | 同上 |
| 86 | OP_END_TRY | 同上 |
| 95 | OP_FOR_PREP | 嵌套 for 循环的初始化指令，涉及内层循环变量初始化和跳转 |
| — | OP_CALL | 函数调用涉及 VM 调用栈管理 |
| — | OP_TAIL_CALL | 尾调用涉及帧复用 |
| — | OP_RETURN | 循环体中不应出现返回 |
| — | OP_MODULE_CALL | 模块方法调用 |
| — | OP_GET_PROPERTY | 属性访问（未来可通过 callout 支持） |
| — | OP_STRING_ADD | 字符串拼接 |
| — | 其他未列出 opcode | 未实现 codegen |

### 特殊拒绝条件

- **OP_LOOP 出现在循环体中间**：表示嵌套 while 循环，当前不支持
- **虚拟栈溢出**：`vstack < 0 || vstack > JIT_MAX_VSTACK (64)`
- **local 数过多**：超过 `JIT_MAX_LOCALS (32)`

---

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

### Callout 调用流程（以 OP_DICT_SET 为例）

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

---

## 7. 调试方法

### 环境变量

| 变量 | 作用 |
|------|------|
| `LENO_JIT_DEBUG=1` | 打印 scan 失败、codegen 失败、compile FAIL、BAILOUT 信息 |
| `LENO_JIT_DUMP=1` | 将 JIT 机器码 dump 到 `jit_mc_dump.txt` |
| `LENO_NO_JIT=1` | 完全禁用 JIT（用于性能对比基线） |

### --debug 反汇编

```bash
./build/leno.exe --debug file.leno
```

输出每个函数的字节码反汇编，包括 offset、行号、指令名和操作数。用于定位 `body_start` 对应的字节码序列。

### 调试输出示例

```
[JIT-DEBUG] codegen FAIL: OP_CONST non-int/float const at bc_off=3, op=0, ci=69
[JIT-DEBUG] compile_loop returned 0, cb.len=303, capable=1, n_locals=4, body_size=18
[JIT-DEBUG] raw bytes: 8 0 40 0 0 69 0 0 70 71 6 96 45 43 42 0 0 18
[JIT-DEBUG] body opcodes: 8 0 0 71 6 96
[JIT-DEBUG] compile FAIL at body_start=654, back_edge=2
[JIT-DEBUG] scan FAIL: try/catch opcode 83 at offset 0
[JIT-DEBUG] BAILOUT at body_start=57, count=1
```

### 定位流程

1. 用 `--debug` 找到 `body_start` 对应的函数和字节码序列
2. 用 `LENO_JIT_DEBUG=1` 运行，查看具体的 FAIL/BAILOUT 原因
3. 对照 `raw bytes` 和 `body opcodes` 确认字节码序列
4. 在 `jit.c` 对应的 codegen case 中定位问题

---

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

### 8.4 OP_CAST_FLOAT 无条件 CVTSI2SD

**现象**：对已经是 float 的值执行 `OP_CAST_FLOAT` 会破坏 float 值（把 double bits 当 int 转换）。

**根因**：`OP_CAST_FLOAT` 的 codegen 无条件执行 `CVTSI2SD xmm0, rax`，但虚拟栈上 float 值已经是 raw double bits，不是整数。

**修复**：改为运行时类型分派——先用 int48 检测判断值类型，是 int 才 `CVTSI2SD`，否则直通。

### 8.5 OP_GET_GLOBAL 仅 int 类型守卫

**现象**：全局变量是 float 时，JIT 加载后当作 int 处理，导致后续运算错误。

**根因**：`OP_GET_GLOBAL` 只检查 `INT_TAG`，非 int 直接 bail out。

**修复**：改为 int-or-float 双路分派：检查 top16 是否为 0xFFFB（int），是则提取 int48；否则检查是否 ≥ 0xFFF8（NaN-boxed 非数值），是则 bail out；否则当作 float raw bits 直通。

### 8.6 OP_CONST 不支持非 int/float 常量

**现象**：含字符串常量的循环（如 `dict["key"] = value`）编译失败：`codegen FAIL: OP_CONST non-int/float const`。

**根因**：`OP_CONST` codegen 遇到 string/null/bool/obj 常量直接 `return 0`。

**修复**：对非 int/float 常量，直接将 raw NaN-boxed Value bits 压入虚拟栈。`EMIT_RAW_TO_VALUE` 的 int48 检测（`sar 47; inc; cmp 1; ja`）能正确识别 NaN-boxed 值为非 int48（位移结果 >1），跳过 int 重编码，直接传给 callout。

### 8.7 OP_SET_LOCAL_CONST 不支持非 int/float 常量

**现象**：`SET_LOCAL_CONST` 遇到字符串常量直接 `return 0`，导致循环编译失败。

**根因**：同 OP_CONST，只支持 int/float。

**修复**：对非 int/float 常量，加载 raw NaN-boxed bits，并额外发 `BTS RBX, si` 指令标记该 local 为 non-int，确保 epilogue write-back 存 raw bits 而非错误地重编码为 int。

### 8.8 OP_FOR_PREP 未注册到 opcode_size 表

**现象**：`scan FAIL: unknown opcode 95 (size<0)`。

**根因**：`OP_FOR_PREP`（opcode 95, 8 bytes）不在 `opcode_size()` 函数的 switch 中，返回 -1。

**修复**：在 `opcode_size()` 中添加 `case OP_FOR_PREP: return 8;`，并在 `scan_loop_body` 中将其标记为不支持（嵌套 for 循环太复杂）。

### 8.9 Epilogue 不能用 LEAVE 指令

**现象**：JIT 函数返回时崩溃或寄存器值错乱。

**根因**：prologue 在 `push rbp` 之后压入了 `r12/r13/r14/rbx`，`LEAVE`（`mov rsp,rbp; pop rbp`）会恢复 RSP 到错误位置，弹出错误的寄存器值。

**修复**：手动恢复：`mov rsp,rbp; pop r14; pop r13; pop r12; pop rbx; pop rbp; ret`。

### 8.10 OP_DICT_SET scan vstack 计算错误

**现象**：`OP_DICT_SET` 的 callout 实现中虚拟栈深度计算错误。

**根因**：`OP_DICT_SET` 弹出 3 个值（dict, key, value），压入 1 个结果（dict），净 -2。但 scan 中 vstack 减了 -3。

**修复**：scan 和 codegen 统一为 `vstack -= 2`。

### 8.11 Bailout 后 VM 重复执行循环迭代

**现象**：JIT bail out 后，VM 从 back-edge 重新执行，导致一次循环体执行了两次。

**原因分析**：这是设计决策——bail out 时不写回 locals（保持 VM 原始状态），VM 从 back-edge 指令处重新执行循环体。这意味着 bail out 发生时的那次迭代会被 VM 重新执行一次。这是安全的（结果正确），但有微小的性能开销。

**当前状态**：设计上可接受，未修复。

### 8.12 fib_iterative(1000) 的 3 次 Bailout

**现象**：`body_start=57` 的循环（`fib_iterative` 函数）执行 3 次 bailout 后放弃。

**根因**：`fib_iterative(1000)` 计算的斐波那契数列值在约 fib(56) 时溢出 int48 范围（2^47 ≈ 1.4×10^14），JIT 正确检测到溢出并 bail out。bail out 3 次后 JIT 停止尝试，VM 用 BigInt 继续计算。

**结论**：这是预期行为，JIT 的 int48 溢出检测正常工作。

---

## 9. 性能数据

### 测试环境

- 测试文件：`examples/性能测试/全部测试.leno`（539 行，含算术/函数调用/数组字典/try/while vs for/fib 对比）
- 编译命令：`cmd //c build.bat`
- 运行命令：`./build/leno.exe "examples/性能测试/全部测试.leno"`

### JIT 统计

| 指标 | 值 |
|------|----|
| Compiled | 35 |
| Executed | 20032 |
| Bailouts | 3 |
| Cached | 31 |
| Tried | 34 |

### 关键性能对比

| 测试项 | JIT | 无 JIT | 加速比 |
|--------|-----|--------|--------|
| 1 亿次 i++ | 78ms | — | — |
| 1 亿次 a = b | 47ms | — | — |
| 1 亿次 dict[key]=value | 906ms | 1781ms | ~2.0x |
| 1 亿次 arr.add() | 625ms | 1359ms | ~2.2x |
| for 1000 万次 i++ | 343ms | — | — |
| while 1000 万次 i++ | 422ms | — | — |
| 嵌套 for 1000×1000 | 78ms | 110ms | ~1.4x |

### 回归测试

```
Results: 261 passed, 0 failed (total 261)
```

---

## 10. 架构瓶颈分析

当前 JIT 是 **loop-body template JIT**，只编译单个循环体、用 push/pop 虚拟栈、单回边。存在三个结构性天花板：

### 瓶颈 1：只能编译扁平循环，不能跨控制流

`scan_loop_body` 在遇到 `OP_FOR_PREP`（嵌套 for）或 body 中途出现 `OP_LOOP`/`OP_FOR_LOOP`（嵌套 while）时直接 `capable=0` 拒绝编译。任何"for 里套 for"或"while 里套 while"的代码都无法 JIT。

### 瓶颈 2：不支持函数调用，递归函数永远无提速

`OP_CALL`、`OP_RETURN`、`OP_CALL_GLOBAL_FUNC` 在 JIT 中完全不存在。fib(30) 是递归调用，每次递归都走 VM 的 `call()` → 新建 CallFrame → 解释执行 → `OP_RETURN` 回到调用者。JIT 只在 `OP_LOOP`/`OP_FOR_LOOP` 处触发，递归函数里根本没有循环回边可以触发 JIT。即使通过 callout 实现 OP_CALL，每次递归调用都要保存虚拟栈 → 切到 VM 执行 → 恢复虚拟栈，开销比纯解释执行还大。

### 瓶颈 3：push/pop 虚拟栈是性能杀手

当前 codegen 中 `a + b` 实际生成的指令序列：

```
push a      ; 内存写 [rsp-8]
push b      ; 内存写 [rsp-8]
pop rax     ; 内存读 [rsp]
pop rdx     ; 内存读 [rsp]
add rax,rdx
push rax    ; 内存写
```

4 次内存操作做一次加法。while 循环只有 ~2x 提速的根本原因：JIT 省的是 dispatch 开销，没省内存开销。LuaJIT 快是因为 register-based bytecode + 线性扫描寄存器分配，运算几乎全在寄存器里完成。

---

## 11. 优化路线图

按投入产出比排序：

### P0：虚拟栈寄存器化（预计提速 3-5x）

- **目标**：维护 register cache，栈顶 1-2 个元素常驻 RAX/RDX，不下推到内存
- **改动范围**：只改 codegen 中的 push/pop 语义，不碰 scan 逻辑和 bailout 机制
- **实现要点**：
  - `push` → 如果 RAX 空，放 RAX；否则 spill 栈顶到 `[rsp]` 再放 RAX
  - `pop` → 如果 RAX 有值，取 RAX；否则从 `[rsp]` 加载
  - 二元运算（ADD/SUB/MUL）→ 左操作数在 RAX，右操作数在 RDX，直接 `add rax,rdx`，0 次内存操作
- **状态**：实施中

### P1：嵌套循环支持（解锁大量真实代码）

- **目标**：for 套 for、while 套 while 能被 JIT 编译
- **改动范围**：scan + codegen 的 loop patch 表
- **实现要点**：
  - `OP_FOR_PREP` 不再拒绝，记录为内层循环入口，跳过初始化字节
  - body 中途出现 `OP_LOOP`/`OP_FOR_LOOP` 时，记录为内层回边
  - codegen 需要一个 loop stack（记录每层循环的 `loop_start_mc` 和 `exit_mc`）
  - 正确处理内层循环退出后回到外层循环体的跳转

### P2：OP_CALL callout（让更多代码进入 JIT 路径）

- **目标**：循环体中遇到 OP_CALL 不再整个循环拒绝编译
- **改动范围**：利用现有 callout 基础设施
- **注意**：能让 fib"跑通"JIT 路径但不会提速——callout 开销远大于解释执行省下的 dispatch 开销

### P3：架构方向决定——函数级 JIT（method JIT）

| 方向 | 代表 | 工作量 | 收益 | 适合场景 |
|------|------|--------|------|---------|
| 继续改进 loop JIT | 当前 | 低 | 中（2-5x） | 算术密集循环 |
| 函数级 JIT | V8 TurboFan / PyPy | 中-高 | 高（10x+） | 通用 |
| Trace JIT | LuaJIT | 很高 | 很高（20x+） | 通用+递归 |

**不推荐 Trace JIT**：Mike Pall 花了几年全职才做出来，需要 trace recording、SSA IR、trace stitching、snapshot-based deoptimization、trace exit 修复，一个人无法完成。

**推荐函数级 JIT**：
- 天然解决嵌套循环（编译整个函数体，循环只是函数内跳转）
- 可支持函数间调用（JIT 函数调用 JIT 函数，开销远低于 callout）
- 可做叶子函数内联
- 不需要 trace recording，可用现有 codegen 基础设施扩展
- 渐进式迁移（先编译最热的几个函数）
- 核心改变：触发点改为函数被调用 N 次后编译整个函数体；编译范围从 `OP_CALL` 到 `OP_RETURN`；OP_CALL 走快路径（已 JIT 函数直接 `call jit_fn`）或慢路径（callout 到 VM）；用 simple linear-sscan allocator 把局部变量分配到 callee-saved 寄存器

---

## 12. 当前未解决问题

1. **try/catch 循环（body_start=27）**：异常处理涉及 VM 异常栈和 try/catch 表，暂不支持
2. **body_start=1156 编译失败**：具体原因未完全定位（无 scan FAIL 和 codegen FAIL 消息）
3. **fib_iterative(1000) 3 次 bailout**：斐波那契值约 fib(56) 溢出 int48（超 2^47），属预期行为
4. **while 比 for 慢约 2 倍**：`OP_GET_GLOBAL` 每次 NaN-box 解码开销，P0 寄存器化后有望缓解

---

## 附：关键文件索引

| 文件 | 说明 |
|------|------|
| `src/jit/jit.c` | JIT 编译器主文件（scan + codegen + cache + callout） |
| `src/jit/jit.h` | 公共 API 和配置参数 |
| `src/jit/jit_emit.h` | x86_64 指令发射函数 |
| `src/jit/jit_mem.h` | 可执行内存分配（VirtualAlloc/mprotect） |
| `src/vm/vminc/op_for_loop.inc` | OP_FOR_PREP / OP_FOR_LOOP 的 VM 实现 |
| `src/vm/vminc/op_loop.inc` | OP_LOOP 的 VM 实现 |
| `src/include/leno_vm.h` | OpCode 枚举定义 |
| `src/include/leno_value.h` | NaN-boxing Value 定义 |
| `examples/性能测试/全部测试.leno` | 性能测试文件 |
| `assert/run_tests.leno` | 回归测试入口 |
