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

***

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

| 参数                  | 值   | 说明                      |
| ------------------- | --- | ----------------------- |
| `JIT_HOT_THRESHOLD` | 50  | 循环执行 50 次后触发编译          |
| `JIT_CACHE_SIZE`    | 256 | 缓存哈希表大小（2 的幂）           |
| `JIT_MAX_LOCALS`    | 32  | 最多 type-guard 的 local 数 |
| `JIT_MAX_LOOP_OPS`  | 256 | 循环体最大 opcode 数          |
| `JIT_MAX_VSTACK`    | 64  | 虚拟栈最大深度                 |
| `JIT_BAILOUT_LIMIT` | 3   | bailout 3 次后放弃此循环       |

***

## 2. 寄存器约定

Windows x64 ABI，JIT 函数入口 `RCX = Value* locals`，`RDX = Value* globals`。

| 寄存器 | 用途                                       | 保留方式                         |
| --- | ---------------------------------------- | ---------------------------- |
| RCX | locals 指针（第一参数，固定）                       | 调用者保存                        |
| RDX | globals 指针（入口参数，prologue 复制到 R9）         | -                            |
| R9  | globals 指针（运行期间固定）                       | 调用者保存                        |
| R10 | `PAYLOAD_MASK = 0x0000FFFFFFFFFFFF`      | prologue 加载                  |
| R11 | `INT_TAG = 0xFFFB000000000000`           | prologue 加载                  |
| RBX | 类型位图：bit i=1 表示 local i 是 float/obj      | callee-saved，prologue xor 清零 |
| RBP | 帧指针 = scratch 区基址                        | callee-saved                 |
| RSP | 虚拟栈顶（JIT push/pop 用）                     | 运行期间管理                       |
| RAX | 通用 scratch                               | volatile                     |
| RDX | 通用 scratch（注意：入口时是 globals，prologue 后释放） | volatile                     |
| R8  | int48 检测 scratch                         | volatile                     |
| R12 | callout: 保存 RSP                          | callee-saved                 |
| R13 | callout: 保存 RCX                          | callee-saved                 |
| R14 | callout: 保存 R9                           | callee-saved                 |

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
| float         | 原始 IEEE 754 double bits | 上述检测 >1                                                  |
| null/bool/obj | 原始 NaN-boxed bits       | 上述检测 >1（top16 ≠ 0xFFFB）                                  |

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
| —      | OP\_MOD\_INT      | 1   | `cqo; idiv r8` 取余数                      |
| —      | OP\_NEG\_INT      | 1   | `neg rax` + int48 检测                    |
| —      | OP\_ADD\_INT\_IMM | 2   | `add rax, imm8` + int48 检测              |
| —      | OP\_SUB\_INT\_IMM | 2   | `sub rax, imm8` + int48 检测              |
| —      | OP\_MUL\_INT\_IMM | 2   | `imul rax, imm` + 溢出检测                  |

### 整数除法（返回 float）

| Opcode | 枚举名          | 字节数 | 说明                                                      |
| ------ | ------------ | --- | ------------------------------------------------------- |
| —      | OP\_DIV\_INT | 1   | `cqo; idiv r8; cvtsi2sd xmm0, rax` → 压入 raw double bits |

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
| 34     | OP\_NOT | 1   | 逻辑非：`test rax,rax; sete al; movzx` |

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

以下字节码出现在循环体中时，`scan_loop_body` 会将整个循环标记为 `capable=0`（不可 JIT）：

| Opcode | 枚举名               | 原因                             |
| ------ | ----------------- | ------------------------------ |
| 83     | OP\_TRY           | 异常处理涉及 VM 异常栈、try/catch 表等复杂状态 |
| 84     | OP\_CATCH         | 同上                             |
| 86     | OP\_END\_TRY      | 同上                             |
| —      | OP\_CALL          | 函数调用涉及 VM 调用栈管理                |
| —      | OP\_TAIL\_CALL    | 尾调用涉及帧复用                       |
| —      | OP\_RETURN        | 循环体中不应出现返回                     |
| —      | OP\_MODULE\_CALL  | 模块方法调用                         |
| —      | OP\_GET\_PROPERTY | 属性访问（未来可通过 callout 支持）         |
| —      | OP\_STRING\_ADD   | 字符串拼接                          |
| —      | 其他未列出 opcode      | 未实现 codegen                    |

### 特殊拒绝条件

- **虚拟栈溢出**：`vstack > JIT_MAX_VSTACK (64)`（允许负 vstack，因嵌套循环 exit POP 在线性扫描中被重复扣减）

- **local 数过多**：超过 `JIT_MAX_LOCALS (32)`

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

| 变量                 | 作用                                            |
| ------------------ | --------------------------------------------- |
| `LENO_JIT_DEBUG=1` | 打印 scan 失败、codegen 失败、compile FAIL、BAILOUT 信息 |
| `LENO_JIT_DUMP=1`  | 将 JIT 机器码 dump 到 `jit_mc_dump.txt`            |
| `LENO_NO_JIT=1`    | 完全禁用 JIT（用于性能对比基线）                            |

### --debug 反汇编

```bash
./build/lenojit.exe --debug file.leno
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

***

## 9. 性能数据

### 测试环境

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
Results: 261 passed, 0 failed (total 261)
```

***

## 10. 架构瓶颈分析

当前 JIT 是 **loop-body template JIT**，只编译单个循环体、用 push/pop 虚拟栈、单回边。存在三个结构性天花板：

### 瓶颈 1：~~只能编译扁平循环，不能跨控制流~~ 已解决

P1 已完成嵌套循环支持。`scan_loop_body` 现在接受 `OP_FOR_PREP`（内层 for 初始化）、mid-body `OP_LOOP`（内层 while 回边）和 mid-body `OP_FOR_LOOP`（内层 for 回边）。codegen 通过 `offmap_lookup` 解析每个回边指令自带的目标偏移，天然支持任意嵌套深度，无需 loop stack。

### 瓶颈 2：不支持函数调用，递归函数永远无提速

`OP_CALL`、`OP_RETURN`、`OP_CALL_GLOBAL_FUNC` 在 JIT 中完全不存在。fib(30) 是递归调用，每次递归都走 VM 的 `call()` → 新建 CallFrame → 解释执行 → `OP_RETURN` 回到调用者。JIT 只在 `OP_LOOP`/`OP_FOR_LOOP` 处触发，递归函数里根本没有循环回边可以触发 JIT。即使通过 callout 实现 OP\_CALL，每次递归调用都要保存虚拟栈 → 切到 VM 执行 → 恢复虚拟栈，开销比纯解释执行还大。

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

4 次内存操作做一次加法。while 循环只有 \~2x 提速的根本原因：JIT 省的是 dispatch 开销，没省内存开销。LuaJIT 快是因为 register-based bytecode + 线性扫描寄存器分配，运算几乎全在寄存器里完成。

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

***

## 12. 当前未解决问题

1. **try/catch 循环（body\_start=27）**：异常处理涉及 VM 异常栈和 try/catch 表，暂不支持
2. **body\_start=1156 编译失败**：具体原因未完全定位（无 scan FAIL 和 codegen FAIL 消息）
3. **fib\_iterative(1000) 3 次 bailout**：斐波那契值约 fib(56) 溢出 int48（超 2^47），属预期行为
4. **OP\_FOR\_PREP 仅支持正步长**：step ≤ 0 时 bail out，负步长 for 循环无法 JIT 编译（已知限制，非 bug）
5. **while 比 for 慢约 2 倍**：`OP_GET_GLOBAL` 每次 NaN-box 解码开销，P0 寄存器化后有望缓解
6. **含函数调用的循环无法 JIT**（2026-09-06 起）：scan 阶段直接拒绝，待实现内联（见第 13 节）

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

## 附：关键文件索引

| 文件                             | 说明                                           |
| ------------------------------ | -------------------------------------------- |
| `src/jit/jit.c`                | JIT 编译器主文件（scan + codegen + cache + callout） |
| `src/jit/jit.h`                | 公共 API 和配置参数                                 |
| `src/jit/jit_emit.h`           | x86\_64 指令发射函数                               |
| `src/jit/jit_mem.h`            | 可执行内存分配（VirtualAlloc/mprotect）               |
| `src/vm/vminc/op_for_loop.inc` | OP\_FOR\_PREP / OP\_FOR\_LOOP 的 VM 实现        |
| `src/vm/vminc/op_loop.inc`     | OP\_LOOP 的 VM 实现                             |
| `src/include/leno_vm.h`        | OpCode 枚举定义                                  |
| `src/include/leno_value.h`     | NaN-boxing Value 定义                          |
| `examples/性能测试/全部测试.leno`      | 性能测试文件                                       |
| `assert/run_tests.leno`        | 回归测试入口                                       |

