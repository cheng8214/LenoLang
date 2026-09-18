# Bug 5: JIT 将 Array[int] 元素读成 float，偶发污染 AES/解密结果

> 来源：**另一台机器**在 `leno_module/LenoSDL3/examples/应用示例/Trae签到/` 上的发现
> （原始复现件 `mini_loop.leno` 未随仓库传过来）。本目录下的
> `bug5_aes_int_array_float.leno` 是**按报告在本地重建**的复现件（基于 `examples/crypto/aes128.leno`，
> 单文件主脚本模式），下面同时保留原始证据与本地实测。

## 现象（原始报告）

- `examples/crypto` 与 `Trae签到` 的派生解密在 **主脚本(直接运行)模式 + JIT 开启** 下偶发失败：
  - `get_token` 返回长度随机漂移（1316/1319/1287/1304...，正确 1747/1760）
  - 严重时 `add_round_key` 直接报运行时错误：
    `位异或运算: 操作数必须是整数 [操作数a] type=float, value=0 [操作数b] type=int`（aes128.leno:157）
  - 频率：60 次循环调用出 5 次错误（≈8%），错误位置随机（i=7/20/43/45/58）
- `LENO_NO_JIT=1` 下 60/60 全过，100% 正确。

## 触发条件特殊性

- **主脚本直接运行**（`leno checkin_core.leno status`）必现；
- **以 import 模块方式运行**（`leno repro_main.leno status`，main 在别的文件、核心逻辑在模块里）不现。
- 复现文件 `bug5_aes_int_array_float.leno` 主脚本运行 60 次必现（JIT 下 bad_count>=3），
  运行 `LENO_NO_JIT=1` 对照则 bad_count=0。

## 证据链（同一输入、无逻辑改动）

| i= | t.len | skey.len | dec.len | json.len |
|----|-------|----------|---------|----------|
| 0  | 1862  | 64       | 1824    | 1760     |  <- 基线
| 7  | 1862  | 64       | 1779    | ...      |  <- dec 长度已经污染
| ...| ...   | ...      | ...     | ...      |

- 探针显示：t/skey 长度恒正确，**污染发生在 `aes128_cbc_decrypt_bytes` 内部（dec 长度漂移）**。
- 栈：`add_round_key`(aes128.leno:157) -> `aes_decrypt_block`(:234) -> `cbc_decrypt_blocks`(:308)。
- 直接证据：`type=float, value=0` 参与 `^` —— JIT 编译的数组元素读取指令把 int 槽位当 double。

## 根因推断（原始报告）

JIT 对 `Array[int]` 的 **GET_INDEX/SET_INDEX（或数组基址+槽位宽度）** 处理：主脚本模式编译
`add_round_key` 的 `state[i] ^ round_key[i]` 循环时，索引读入被当 64 位 double 槽位，低 32 位
读到 0（float 0），高 32 位才是原 int —— 偶发取决于寄存器分配。

---

# 本地复现（2026-09-18，`ceef05ba` + 工作区干净）

复现件：`_jit_bugs/bug5_aes_int_array_float.leno`（= `examples/crypto/aes128.leno` 原样 +
`main()` 开头调用 `bug5_torture()`：60 轮 × 4 项校验，`try/catch` 计异常）。

| 组 | bad_count | 结果 |
| --- | --- | --- |
| JIT | **1**（连跑 5 次全部 `i=56`）| ✅ 复现 |
| `LENO_NO_JIT=1` | 0（连跑 5 次）| 对照干净 |

**关键：不是偶发，是确定性的** —— 每一次 JIT 运行都在**同一轮 i=56** 失败，
失败项固定为「字符串多块往返」（`block`/块往返/`add_round_key` 三项都 true）。
⇒ 更像"**某个 JIT 热阈值在第 57 轮附近生效**，而生效后的第一段机器码算错了一个值"。

## 开关切分（全部无效 ⇒ 不在这些面上）

`LENO_JIT_NOINLINE=1` / `LENO_NO_CALLCACHE=1` / `LENO_NO_CMPJMP=1` / `LENO_NO_TYPEDPARAM=1`
—— 四者都是 `bad_count=1 @ i=56`，与 baseline 完全一致。

## JIT stats（决定性线索）

```
Compiled: 19   Executed: 262970   Bailouts: 9   FuncCompiled: 2
Bailout: fn='aes_decrypt_block' loop_bc=238 x3 — 非溢出类 @bc_off=430（= 238 + 192） | 触发指令=OP_ARRAY
Bailout: fn='aes128_decrypt'    loop_bc=35  x3 — 非溢出类 @bc_off=198（= 35 + 163）  | 触发指令=OP_ARRAY
Bailout: fn='aes_encrypt_block' loop_bc=236 x3 — 非溢出类 @bc_off=439（= 236 + 203） | 触发指令=OP_ARRAY
```

- **9 次 bailout 全部是 `OP_ARRAY`**（数组字面量），且都是 `x3`（= `JIT_BAILOUT_LIMIT`，
  三次后被拉黑 ⇒ 之后这些循环全走解释器 ⇒ 这解释了"只错一次，之后全对"）。
- 三个函数各自的循环里都含数组字面量（如 `aes128_decrypt` 的 16 元素块构造，
  绝对 `bc_off=198` ↔ 源码 `aes128.leno:364`）。

## 可疑点 1【已证实】：`OP_ARRAY` 的"歧义区守卫"判据过宽（`src/jit/backend/x86_inc/ops_callout.inc`）

**已证实（2026-09-18，`_jit_bugs/diag_array_literal_guard.leno`，10 行最小探针，JIT 开）**：

| 探针 | 循环内字面量 | `raw>>47` | JIT stats |
| --- | --- | --- | --- |
| `pos()` | `[1, 2, 3]` | 0（非负）| **`fn='pos' 触发指令=OP_ARRAY x3`**（三次后拉黑）|
| `zer()` | `[0, 0, 0]` | 0 | **`fn='zer' 触发指令=OP_ARRAY x3`**（三次后拉黑）|
| `neg()` | `[-1, -2, -3]` | -1 | **无 bailout**（循环保持 JIT）|

⇒ 判据确实只命中"非负"这一半：**合法非负 int48 被误杀（过宽）**，
而 `raw>>47 == -1`（负 int48，同样是歧义区）**反而被放行（漏报）**。
两个方向都错 ⇒ **值域判据在这件事上不可用**：要么改成"来源槽类型"判据（§8.104 的 RBX 位图），
要么让 OP_ARRAY 直接走 §8.104 的物化通道 ✓。

§8.105 在该 case 里加的守卫（交给 callout **之前**逐元素检查）：

```c
for (int _e = 0; _e < (int)count; _e++) {
    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RSP, _e * 8);  /* 64 位读（后缀指位移宽度）*/
    emit_mov_rr(cb, JIT_R8, JIT_RAX);
    emit_sar_imm(cb, JIT_R8, 47);                      /* r8 = raw >> 47（算术）*/
    emit_test_rr(cb, JIT_R8, JIT_R8);
    EMIT_BAILOUT_SITE_NONOVF(bc_off);
    int _amb_e = emit_jcc(cb, 0x84);                   /* JZ（== 0）→ bailout */
    patch_add(ctx, _amb_e, -1, 0);
}
```

判据 `raw >> 47 == 0` 等价于 **`0 <= raw < 2^47`** —— 也就是**所有非负 int48**。
而 AES 的字节数组元素恰好全是 `0x00..0xFF`（非负）⇒ 这些循环**每次都在 OP_ARRAY 处 bail**
⇒ 三次后被拉黑（与 stats 完全吻合）。§8.115 记录过"该族守卫判据写错 ⇒ 热循环被大面积拉黑"，
这里看起来是**同一族问题的残留落点**（`JZ` 只覆盖 `== 0`，不覆盖 `== -1`；
且"歧义区"本身用值域判据无法与合法非负 int 区分 —— 需要 §8.104 的**来源槽类型位图**）。

## 可疑点 2：污染发生在 **bailout → 解释器重放** 的边界

失败恰好出现在"某个循环第一次进 JIT"的那一轮（i=56），此后（循环被拉黑、纯解释）
全部正确 ⇒ 更像 **JIT 已执行的前缀（如 `ct_bytes[i]` 的数组元素读）算出了一个错值，
再由 bailout 把它交给解释器**继续用；而不是"JIT 全程算错"。
原始报告里 `type=float, value=0`（一个本该是 int 0 的槽位被当成 float 0.0）
与 §8.99 的「裸 double vs int48 在 0 上撞码」是同族表现。

⇒ 下一步应同时看两处：**① `OP_ARRAY` 守卫的判据**；**② bailout 时 JIT 操作数栈写回 VM 栈的转换**
（int 0 / raw 0 是否被写成 float 0.0）。

## 进展（2026-09-18，第二轮）

### 已修：`OP_ARRAY` 守卫（独立子缺陷，会造成**数据污染**）

`ops_callout.inc` 的 `case OP_ARRAY` 里 §8.105 守卫判据 = `raw>>47 == 0` = **所有非负 int48** ⇒
循环内建非负元素字面量必 bail（三次后拉黑）。而机器码 bailout 块**不写回 locals**
（源码注释：`locals not written back (VM re-executes from back-edge)`）⇒ 解释器**重做整轮**、
JIT 半轮的副作用不回滚 ⇒ **副作用执行两遍**：

| 探针 | 修前 | 修后 |
| --- | --- | --- |
| `_jit_bugs/diag_array_literal_guard.leno` | `pos`/`zer` 各 `OP_ARRAY x3` | **`Bailouts: 0`** ✓ |
| `_jit_bugs/diag_bailout_double_side_effect.leno` | `sum=201 **len=204**`（多追加 3 次）✗ | **`len=201`** ✓ |

处置：**撤销该守卫**（同 §8.115 对 native 实参守卫的处置）。残余：JIT 区域内多元素字面量
若含"浮点 ±0.0/次正规"元素会被贴成 int（类型标签分叉，数值相同）——
门禁探针 `probe_array_literal_float` 因此从 `g=[0.0]` 变为 `g=[0]`（**已知代价**）。

### 未修完：Bug5 的错值是**第一次 GC 回收**造成的，与守卫/让出无关

排除过程（每一步都有实测）：

| 步骤 | 结果 | 结论 |
| --- | --- | --- |
| 撤销守卫后重跑 | `Bailouts: 0`，仍 `bad_count=1 @ i=56` | 与 bailout/重放**无关** |
| `JIT_FUNC_HOT_THRESHOLD` 拉到 100000 | 仍 `i=56` | **不是函数级 JIT** |
| 新增轻量编译日志 `LENO_JIT_CLOG=1`（`jit.c`） | i=49 后编译了外层循环，i=50–56 之间**无任何编译** | 不是"刚编译就错" |
| 同日志的 YIELD 打点 | **`[CLOG] YIELD at body_bc=30 (#1)` 与 `[BUG5] i=56 结果错` 同轮** | 错值出现在**第一次 GC 让出**那一轮 |
| `LENO_NO_JIT_YIELD=1`（禁止让出，新开关） | 仍 `bad_count=1` | **不是 YIELD 机制** ⇒ 指向 **GC 回收本身** |

⇒ 剩余根因（待查）：**第一次 GC 回收发生在 JIT 场景下会破坏状态**
（`LENO_NO_JIT=1` 全程干净）—— 头号嫌疑是 §8.36 那一类"**JIT 活值 GC 看不见**"：
某个在 JIT 机器栈/寄存器里存活的值在回收点上没有被当根 ⇒ 被当垃圾回收 ⇒ 后续读到污染值。
建议下一步：在 GC 回收点打印 `jit_in_frame()` / `jit_loop_depth` / `jit_func_depth`，
并用一个"强制提前回收"的小用例把窗口定到指令级。

## 复现方法

```
cd D:\CLeno\Leno
build\leno.exe --no-cache _jit_bugs\bug5_aes_int_array_float.leno          # JIT：bad_count=1 @ i=56
set LENO_NO_JIT=1
build\leno.exe --no-cache _jit_bugs\bug5_aes_int_array_float.leno          # 对照：bad_count=0
```

## 修复后判据

JIT 下 `bad_count` 必须为 0（且 `Bailouts` 不再出现 `触发指令=OP_ARRAY` 的 x3 拉黑）；
`LENO_NO_JIT=1` 仍为 0；`assert` 全套通过。
