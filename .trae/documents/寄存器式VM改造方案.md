# Leno 寄存器式 VM 改造方案

## Context（为什么做这件事）

Leno 目前是**栈式字节码 VM**：表达式求值完全依赖 `vm.stack` 操作数栈（`vm_stack_push/pop`），
指令本身只带常量/槽位索引，靠隐式栈顶传递操作数（[op_arith.inc](file:///d:/CLeno/LenoReg/src/vm/vminc/op_arith.inc)、[op_stack.inc](file:///d:/CLeno/LenoReg/src/vm/vminc/op_stack.inc)）。
这带来两个结构性问题：

1. **指令膨胀**：一次 `a + b` 需要 `GET_LOCAL a` / `GET_LOCAL b` / `ADD` / `SET_LOCAL` 四条指令，
   编译器只能靠不断追加融合指令（`OP_CMPJMP_LL_INT`、`OP_INVOKE_METHOD_TYPED`、`OP_ADD_INT_IMM` 等，
   见 [leno_vm.h](file:///d:/CLeno/LenoReg/src/include/leno_vm.h#L13-L219)）来补救，opcode 已达 ~200 个、逼近跳转表上限。
2. **每次数据搬运都要过栈**：压栈/弹栈既多花指令数，也多占内存带宽。

改造目标：把指令改为**显式带寄存器操作数**（`ADD A B C` 即 `R[A] = R[B] + R[C]`），
操作数直接落在帧的寄存器文件里，消除隐式栈搬运。产出物放在新建仓库
`https://gitee.com/cheng8214/lenoreg`，旧栈式版本保留为基线提交，随时可对照。

## 已确认的四项设计决策

| 项 | 决策 |
|---|---|
| 迁移策略 | 新增寄存器式 ISA + 新解释器，**与旧栈式后端并存**，按功能块逐块切换并回归，最后删除栈式代码。每阶段都必须可编译、可运行 |
| 寄存器堆 | **复用 `CallFrame.locals` 数组**作寄存器文件（参数占 R0..arity-1、声明变量紧随、临时寄存器往后）。不再用 `vm.stack` 做表达式求值 |
| 指令编码 | **定长 4 字节，Lua 5.x 风格**：iABC(op8 A8 B8 C8) / iABx(op8 A8 Bx16) / iAsBx(op8 A8 sBx16) / iAsJ(op8 + 24 位) |
| Git | 阶段 0 先 `git init` + `remote add origin` + 推送栈式基线 |

复用 `frame->locals` 是这次改造最大的杠杆：[gc.c](file:///d:/CLeno/LenoReg/src/gc.c#L1026-L1030) 已在扫描
`frame->locals[0..local_count)`，协程按帧保存 `locals`，闭包 upvalue 的 `location` 也指向 `frame->locals[i]`，
**GC / 协程 / 闭包三块零改动**。

## 总体架构

### 1. 寄存器式 ISA（新文件 `src/include/leno_reg_isa.h`）

新枚举 `OP_R_*` 与旧 `OpCode` **不共用编号**（旧表已接近 256 项跳转表上限，见
[vm_run.inc](file:///d:/CLeno/LenoReg/src/vm/vminc/vm_run.inc#L31-L32)），各自一张跳转表。

| 形态 | 布局 | 用途 |
|---|---|---|
| iABC | `op8 A8 B8 C8` | 三寄存器运算，A = 目标寄存器 |
| iABx | `op8 A8 Bx16` | 寄存器 + 常量/全局/upvalue 索引 |
| iAsBx | `op8 A8 sBx16` | 寄存器 + 有符号立即数（短跳 ±32767） |
| iAsJ | `op8 + 24 位有符号` | 长跳转 `JMP` |
| iAx | `op8 + 24 位无符号` | `EXTRAARG`（超大常量表）/ 宽寄存器前缀 |

阶段 1–2 指令集：

- **装载/移动**：`LOADK`(iABx) `LOADI`(iAsBx) `LOADNIL` `LOADTRUE/LOADFALSE` `MOV`(iABC) `NEG` `NOT` `NOP`
- **局部变量**：无专用指令 —— 局部变量**就是**寄存器，读写即 `MOV` 或直接作为 A/B/C 出现
- **全局/模块/upvalue**：`GETGLOBAL` `SETGLOBAL` `DEFGLOBAL` `GETGLOBALFUNC`(iABx)、
  `GETMODVAR` `SETMODVAR` `GETMODFUNC`(iABC)、`GETUPVAL` `SETUPVAL`(iABx) `CLOSE`(iABx)
- **算术/比较/位运算**：通用形式 + int/float 特化形式（`ADD/ADD_INT/ADD_F`、`LT/LT_INT/LT_F` …），全 iABC
- **跳转**：`JMP`(iAsJ) `TEST`(iABC) `JMP_IF_FALSE/JMP_IF_TRUE`(iAsBx)
- **调用/返回**：`CALL` `CALL_NATIVE` `RETURN` `RETURN_MULTI` `CLOSURE`(iABC / iABx)
- **集合基础**：`NEWARRAY` `NEWDICT` `APPEND` `GETIDX` `SETIDX` `LEN` `CONCAT`

兜底机制（单一机制，不引入变长格式）：
- 常量索引 > 65535：`LOADKX A` 后跟 `EXTRAARG`（24 位，上限 1600 万）
- 寄存器号 > 255：`EXTEND` 前缀指令，使紧随指令的 A/B/C 各扩展 8 位高位（16 位寄存器号，上限 65535）；再超即编译期报错
- 跳距 > 32767：编译器把短条件跳转改写为 `TEST` + `JMP`(24 位)

`chunk->lines` 仍按**字节**索引 —— 新增 `emit_reg32()` 把同一条指令的 4 个字节写同一 line，
这样 [serialize.c](file:///d:/CLeno/LenoReg/src/serialize/serialize.c#L677-L718) 的 lines 序列化、
[debug.c](file:///d:/CLeno/LenoReg/src/debug.c) 反汇编、运行时错误行号**一律无需改动**。

### 2. 调用 / 返回 ABI

```
OP_R_CALL A B C     ; R[A] = callee, R[A+1..A+B-1] = 实参, B = nargs+1, C = nresults+1
OP_R_RETURN A B     ; 返回 R[A..A+B-2]
OP_R_RETURN_MULTI A B  ; B 为「存放个数」的寄存器
```

- callee 侧继续走 [vm_call.inc](file:///d:/CLeno/LenoReg/src/vm/vminc/vm_call.inc#L68-L140) 的 `call()`：
  **保留把参数拷进 callee->locals[0..arity)** 的做法（含 param_types 数值转换、其余槽填 null），
  因为 `frame->locals` 就是 callee 的寄存器文件。`func->local_count` 改由**寄存器高水位**决定。
- 结果回写：调用方把结果写入 `R[A0..]`，不足补 null、超出截断（不再借用 `vm.sp - arg_count` 的栈槽约定）。
- 构造函数特例（`is_ctor` 返回 `self`，见 [op_call.inc](file:///d:/CLeno/LenoReg/src/vm/vminc/op_call.inc#L644-L648)）改为读 callee 的 `R0`。
- 原生函数：新增**不压栈**的调用变体，把结果直接写入目标寄存器（现有实现是 `vm_stack_push` 结果）。
- 异常：`catch_ip/finally_ip` 是 chunk 内字节偏移，语义不变，`FRAME_TRY_VALID` 守护宏照旧；
  寄存器式下 throw/catch **不需要清理操作数栈**，比栈式更简单。
- **迁移期以「整模块同 ISA」为准**（`Chunk` 增加 `isa` 字段）：不做跨 ISA 调用桥，
  避免参数/返回值两套搬运逻辑，这是控制风险的关键取舍。

### 3. codegen 寄存器分配（新文件 `src/codegen_reg/`）

新增 `CodeGenReg`（`src/include/leno_codegen_reg.h`），**不复用**现有
`max_local_slot/peak_local_slot/dtor_temp_slot` 记账，与旧 codegen 零冲突：

- 核心 API：`genr_expr_to(gen, ast, dst_reg)` 把表达式结果落到指定寄存器；
  `int genr_expr(gen, ast)` 从空闲栈借一个临时寄存器、调用前者、返回寄存器号。
- 二元运算**阶段 1 就做就地复用**：`genr_expr_to(lhs, dst)` → 借 `r2` 求 rhs → `ADD dst dst r2` → 释放 `r2`。
- 参数与声明变量按声明顺序固定占号（与现有 `Symbol.index` 槽位语义一致）⇒ `CLOSURE` 的 upvalue 索引语义不变。
- 作用域退出：空闲栈回退到 `scope_base`；被捕获/需析构的寄存器 **pin 住不复用**，块尾发 `CLOSE`。
- 函数结束时把寄存器高水位写回 `current_func->local_count`（GC 依赖它，必须是真实高水位）。
- [codegen_inline.c](file:///d:/CLeno/LenoReg/src/codegen/codegen_inline.c) 的内联在寄存器模式**完全不走**（内联假设栈布局一致）。

### 4. 并存与切换机制（不用宏）

- 新增解释器：`src/vm/vminc/reg_run.inc`（自带跳转表与 `READ_*` 宏）、`reg_op_*.inc`（各指令实现）、
  `reg_call.inc`/`reg_helpers.inc`（调用、异常展开等静态辅助函数），由 [vm.c](file:///d:/CLeno/LenoReg/src/vm/vm.c) 按
  `frame->chunk->isa` 选择进入哪套循环（`vm_run_with_vm` 是唯一入口，见 [vm.c](file:///d:/CLeno/LenoReg/src/vm/vm.c#L1003)）。
- 编译器开关：`src/main.c` 选项链加 `--isa=stack|reg`（默认 `stack`），
  两条编译路径（直接执行 / `--compile` / `--pack`）据此选择 `codegen_reg_*` 或现有 `codegen()`。
- 新增 `.c` 必须登记进 `sources_compiler.txt`（`.inc` 是 include，无需登记）；
  `build.bat` / `build.sh` 本身不改。
- 序列化：chunk 增加 `isa` 位，同时 bump `LENO_BIN_VERSION` 与 `LENO_MODCACHE_VERSION`
  （[leno_serialize.h](file:///d:/CLeno/LenoReg/src/include/leno_serialize.h)）⇒ 旧 `.lenb`/`.lenomc` 全部失效重编译，杜绝混用。

## 分阶段实施

| 阶段 | 内容 | 关键文件 | 阶段验收 |
|---|---|---|---|
| **0. Git 基线** | `git init`、`remote add origin https://gitee.com/cheng8214/lenoreg.git`、提交并推送当前栈式版本，打 tag `stack-baseline`；先跑通现有测试，记录性能基线 | 仓库根 | `git log` 有基线提交；`build\leno.exe assert\run_tests.leno build\leno.exe assert` 全绿 |
| **1. 端到端最小链路** | ISA 头文件 + `emit_reg32` 等发射封装 + `CodeGenReg` 骨架 + `Chunk.isa` + `--isa` 开关 + 寄存器式解释器骨架；支持常量/寄存器移动/算术/`GETGLOBALFUNC`/`CALL_NATIVE`/`RETURN`/`NOP` | `leno_reg_isa.h`、`codegen_reg/reg_emit.c`、`reg_codegen.c`、`vminc/reg_run.inc`、`reg_op_load.inc`、`main.c` | `build\leno.exe --isa=reg <最小脚本>` 输出与 `--isa=stack` 完全一致；`--isa=stack` 行为零变化 |
| **2. 表达式与控制流** | 算术/比较/位运算全形态与 int·float 特化、立即数、跳转三形态、赋值/自增、if/else/while/for/break/continue、短路与三元 | `codegen_reg/reg_expr.c`、`reg_stmt.c`、`vminc/reg_op_arith.inc`、`reg_op_jump.inc` | assert 中算术/控制流/循环子集通过；`examples\` 数学类用例逐一对比输出 |
| **3. 函数与闭包** | 全局函数定义/调用、多返回值与解构、尾调用、`CLOSURE`/upvalue/匿名函数、原生与模块函数调用、dtor 槽位化 | `codegen_reg/reg_func.c`、`vminc/reg_op_call.inc`、`reg_op_upval.inc` | assert 中函数/闭包/多返回值/解构子集通过 |
| **4. 复合特性** | 数组/字典/索引/切片/字符串、struct 与字段方法、enum/face、try-catch-finally-throw、协程 `await`/async、模块（`.leno` 模块 init、import、模块变量） | `vminc/reg_op_collection/-struct/-exception/-coroutine/-module.inc` | `assert\run_tests.leno` **全量通过**；`examples\` 全量输出一致 |
| **5. 切换默认 + 清理** | `--isa` 默认改为 `reg`，删除栈式 codegen 与 `op_stack.inc`/栈分支、删除旧 opcode 名表；`debug.c` 反汇编按 `isa` 分流 | `main.c`、`vm.c`、`debug.c`、旧 `codegen/`、`vminc/op_*.inc` | 全量 assert + examples 通过；`leno_gui`/`leno_lsp` 可构建；无栈式残留引用 |
| **6. 性能补齐**（可选） | 按基准结果补 `ADDK`/立即数形态、`CMPJMP`、常量折叠后的直接跳转等；必要时重启方法内联 | `codegen_reg/`、`vminc/` | 对比 `docs\性能测试总结_Leno_vs_Python.md` 的基准，确认不退步 |

**明确搁置**（阶段 5 之前不动）：`codegen_inline.c` 内联、`op_type_specialized.inc` 的特化族、
现有 5 字节融合指令、`docs/JIT*.md` 相关路线、`optimize.c` 保持 AST 级不动。

## 关键风险与对策

| 风险 | 对策 |
|---|---|
| GC 根集合 | 已扫 `frame->locals`；前提是 `local_count` = 真实寄存器高水位，且 `call()` 的 null 填充覆盖全部临时槽 |
| upvalue 捕获 | 被捕获的寄存器必须 pin 住不复用，块尾仍须发 `CLOSE`（否则 upvalue 指向被复用的槽 = 静默错误） |
| dtor | `OP_DTOR_LOCAL` 改为按寄存器号；被析构的寄存器同样 pin，防复用后二次析构 |
| 协程 | [coroutine.c](file:///d:/CLeno/LenoReg/src/coroutine.c) 挂起时的帧 `locals` 拷贝/重定向必须核对：栈式下临时值在 `vm.stack`（会被扫），寄存器式后临时值只在 `locals`，需确认挂起副本是深拷贝 |
| 异常展开 | 旧展开逻辑要清操作数栈，寄存器式不需要 —— 两套展开代码分开写，别复用 |
| 序列化兼容 | 双 bump 版本号 + `isa` 位；旧 `.lenb`/`.lenomc` 强制失效 |
| 调试/工具链 | `debug.c` 的 opcode 名字表**按顺序**排列，必须与枚举逐项对齐；新增 `opCodeNamesReg[]` 并按 `isa` 分流 |
| 编码上限 | 256 项跳转表 ⇒ 新旧各一张；深嵌套/多局部函数触发 `EXTEND` 前缀，注意 `ip` 前进量按指令实际长度算 |
| 工作量 | 旧 codegen 约 940 处 emit 调用点（`codegen_stmt.c` 565、`codegen_expr.c` 327），寄存器式 codegen 是**重写**而非改参数，按阶段推进、每阶段用测试咬住 |

## 验证方式

1. 构建：`build.bat`（Windows，gcc，产物 `build\leno.exe`）
2. 全量断言套件：`build\leno.exe assert\run_tests.leno build\leno.exe assert`
3. A/B 对照：同一 `.leno` 分别在 `--isa=stack` / `--isa=reg` 下运行，`diff` 输出（阶段 1–4 的主要判定手段）
4. 示例集：`examples\` 下按特性分组逐目录跑（闭包、泛型系统、并行赋值、插值、排序测试等）
5. 性能：沿用 `docs\性能测试总结_Leno_vs_Python.md` 的基准脚本，阶段 5/6 对比指令数（可加 `--dump-bytecode`）与耗时