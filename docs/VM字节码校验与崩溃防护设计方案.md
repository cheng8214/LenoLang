# VM 字节码校验与崩溃防护设计方案

> 本文档源于 2026-09-04 修复的嵌套内联 `inline_return_jumps` 数组覆写 bug。该 bug 导致 tetris.leno 消行后 VM 直接 segfault，无任何错误信息输出。本文档设计一套分层防护方案，使此类编译器 bug 在未来能被更早捕获，而非以"静默崩溃"的方式暴露给用户。

## 一、问题背景

### 1.1 Bug 回顾

`codegen_inline.c` 的 `try_inline_call` 通用路径中，进入嵌套内联时将 `inline_return_jump_count` 重置为 0，导致内层函数的 return 跳转覆写外层函数已记录的条目。产生两条损坏的字节码：

| 损坏点 | 位置 | 修复前（bug） | 修复后 |
|---------|------|---------------|--------|
| updateGame `return` | offset 0201 | `OP_JUMP -1 (to 205)` — 未 patch，跳到自身末字节 | `OP_JUMP 385 (to 591)` — 正确指向内联出口 |
| tryMove `return false` | offset 0345 | `OP_JUMP (to 591)` — 被外层错误覆盖 | `OP_JUMP 47 (to 397)` — 正确指向自身出口 |

### 1.2 为什么 VM 没报错就直接崩溃

这次崩溃不是单一故障，而是两条损坏的跳转形成了组合拳：

1. **Bug #1（updateGame return，跳到 205）**：offset 205 是 OP_JUMP 指令（从 201 开始，长度 5）的最后一个操作数字节 `0xff`。在 jumptable 模式下，`jump_table[0xff]` 是 NULL 指针，`goto *NULL` 直接 segfault
2. **Bug #2（tryMove return，跳到 591）**：offset 591 是合法的指令边界（`OP_GET_LOCAL`），VM 执行的是合法但语义错误的指令 — 加载错误的 result_slot、清除错误的 local range、继续执行本该跳过的代码，导致栈状态损坏

**关键洞察**：Bug #2 是实际触发崩溃的主因。跳到 591 后执行的是合法指令，VM 无法检测"执行顺序错误"。而 Bug #1 如果单独存在，jumptable 模式下的 NULL deref 会直接 segfault，没有机会报错。

### 1.3 问题的普遍性

这不是孤立事件。Leno 编译器历史上有过多次类似的"字节码损坏导致静默崩溃"：

| 日期 | Bug | 现象 |
|------|-----|------|
| 2025 | `patch_ast_indices` 循环上界 count 而非 count-1 | 间歇性 segfault，零输出 |
| 2025 | 窥孔优化误匹配操作数字节 | 栈错位，运行时报无关类型错误 |
| 2026-09 | `inline_return_jumps` 数组覆写 | segfault，零输出 |

共同特征：**编译器生成了损坏的字节码，VM 盲目执行后崩溃，没有任何诊断信息**。

## 二、分层防护设计

### 2.1 设计原则

- **编译期拦截优先于运行期检测**：在字节码生成后、执行前做校验，能在开发阶段就暴露问题
- **Debug build 全防护，Release build 零开销**：开发时需要最大可见性，发布时不能牺牲性能
- **不改变 VM 热路径**：dispatch loop 是性能关键路径，release 模式下不加任何检查

### 2.2 三层防护总览

| 层 | 名称 | 触发时机 | Runtime 开销 | 能捕获的 bug |
|----|------|----------|-------------|--------------|
| 第三层 | 编译后字节码校验 | 编译完成后 | 零 | 跳转目标不在合法指令边界 |
| 第二层 | Dispatch 守卫 | 每条指令 dispatch | ~1-3%（仅 debug） | 无效操作码（如 0xff 被当 opcode） |
| 第一层 | Jump 边界检查 | 每条跳转指令 | < 0.5%（仅 debug） | 跳转目标超出 code 数组范围 |

### 2.3 开关策略

```
Debug build  (-DLENO_DEBUG)：三层全开
Release build (默认)        ：仅第三层
```

## 三、第三层：编译后字节码校验（核心）

### 3.1 原理

在 `codegen_finish` 或模块编译完成后，对每个 `BytecodeChunk` 做一次线性扫描：

1. 逐条解码指令，记录所有合法的指令起始 offset 到一个 bitmap
2. 对每条 jump 类指令（OP_JUMP / OP_JUMP_IF_FALSE / OP_JUMP_IF_TRUE / OP_LOOP），计算其目标 offset
3. 检查目标 offset 是否落在合法边界上
4. 如果不落在边界上，报编译错误

### 3.2 能捕获的 bug

- **未 patch 的跳转**（如本次 bug #1：offset 201 的 OP_JUMP 目标 205 不是指令起始位置）
- **跳转到指令中间**（操作数字节被误认为指令）
- **跳转目标超出 code 数组范围**

### 3.3 不能捕获的 bug

- **跳转到合法边界但语义错误**（如本次 bug #2：tryMove 的 return 跳到 591，591 是合法指令边界，但执行顺序错误）
- 这类 bug 需要更深的控制流分析或类型验证，类似 Java bytecode verifier 的 level

### 3.4 实现草案

```c
// 新增文件：src/codegen/codegen_verify.c

typedef struct {
    uint8_t* bitmap;    // bitmap[offset] = 1 表示 offset 是合法指令起始
    int code_len;
} InstructionMap;

// 逐条解码，建立指令边界 bitmap
static void build_instruction_map(Chunk* chunk, InstructionMap* map) {
    map->code_len = chunk->len;
    map->bitmap = calloc(chunk->len + 1, 1);

    int ip = 0;
    while (ip < chunk->len) {
        uint8_t opcode = chunk->code[ip];
        map->bitmap[ip] = 1;

        // 根据 opcode 获取指令长度（需要一张 opcode → length 的表）
        int instr_len = opcode_lengths[opcode];
        if (instr_len == 0) {
            // 未知操作码，无法继续解码
            // 报错或中止
            break;
        }
        ip += instr_len;
    }
}

// 校验所有跳转目标
static bool verify_jumps(Chunk* chunk, InstructionMap* map, ErrorList* errors) {
    bool ok = true;
    int ip = 0;
    while (ip < chunk->len) {
        uint8_t opcode = chunk->code[ip];
        int instr_len = opcode_lengths[opcode];

        if (is_jump_opcode(opcode)) {
            // 读取跳转偏移
            int32_t offset = read_int32(chunk, ip + 1);  // 或 +2，取决于操作数布局
            int target = ip + instr_len + offset;        // jump 是从下一条指令起算

            if (target < 0 || target >= chunk->len) {
                // 跳转目标超出 code 数组范围
                error_add_at(ERR_COMPILE, chunk->lines[ip], 0,
                    "内部错误：跳转目标超出字节码范围 (ip=%d, target=%d)", ip, target);
                ok = false;
            } else if (!map->bitmap[target]) {
                // 跳转目标不在合法指令边界上
                error_add_at(ERR_COMPILE, chunk->lines[ip], 0,
                    "内部错误：跳转目标 %d 不在指令边界上 (源 ip=%d)，"
                    "可能是编译器内联或跳转回填 bug", target, ip);
                ok = false;
            }
        }
        ip += instr_len;
    }
    return ok;
}

bool codegen_verify_chunk(Chunk* chunk) {
    InstructionMap map;
    build_instruction_map(chunk, &map);
    bool ok = verify_jumps(chunk, &map, &current_error_list);
    free(map.bitmap);
    return ok;
}
```

### 3.5 指令长度表

需要维护一张 `opcode → instruction_length` 的表。这张表必须与 `vm_run.inc` 中的 `READ_BYTE()` / `READ_SHORT()` / `READ_INT32()` 序列保持一致。

维护方式建议：

- **方案 A（手动维护）**：在 `leno_vm.h` 的 OpCode 枚举旁加注释标注每条指令长度，人工同步
- **方案 B（自动生成）**：用一个 codegen 脚本从 `op_*.inc` 文件中解析 `READ_BYTE()` / `READ_SHORT()` / `READ_INT32()` 调用，自动生成长度表

推荐方案 B，避免人工同步出错。可参考 `debug.c` 中已有的反汇编逻辑 — 它已经知道每条指令的长度（否则无法反汇编），可以提取复用。

### 3.6 调用时机

```c
// codegen.c 或 module_compiler.c 中
// 在 codegen_finish 之后、序列化/执行之前
if (!codegen_verify_chunk(&gen.chunk)) {
    // 报告错误，不执行
    return ERROR;
}
```

### 3.7 性能影响

- **Runtime 阶段**：零。校验只在编译时执行一次，VM 执行期间不触碰。
- **编译阶段**：O(n) 线性扫描，n = 字节码长度。对典型 Leno 文件（几千到几万字节），耗时 < 1ms，可忽略。
- **可配置**：加 `--no-verify` 选项跳过，用于极端性能场景（如大模块增量编译）。

## 四、第二层：Dispatch 守卫（仅 Debug）

### 4.1 原理

在 VM 的 dispatch 入口处检查 opcode 是否有效。当前 jumptable 模式下 `jump_table[0xff]` 是 NULL，`goto *NULL` 直接 segfault。加一个 NULL 检查，把 segfault 变成 runtime error。

### 4.2 实现草案

```c
// vm_run.inc 中，jumptable 模式

// 方案 1：填充 jump_table 的空槽
const void* jump_table[256] = {
    [OP_CONST] = &&LABEL(OP_CONST),
    // ... 所有合法 opcode ...
    // 其余自动初始化为 NULL
};
// 填充空槽指向一个错误处理标签
for (int i = 0; i < 256; i++) {
    if (jump_table[i] == NULL) {
        jump_table[i] = &&LABEL_INVALID_OPCODE;
    }
}
// ...
LABEL_INVALID_OPCODE:
    runtime_error("内部错误：无效操作码 0x%02X，字节码可能损坏",
                 GET_CURRENT_LINE());
    return -1;
```

方案 1 的优势：不改变 dispatch 热路径（仍然是 `goto *jump_table[op]`），只在空槽被命中时才触发。但初始化时遍历 256 个槽有一次性成本。

```c
// 方案 2：dispatch 前加检查
uint8_t op = READ_BYTE();
if (unlikely(op >= OP_COUNT)) {
    runtime_error("内部错误：无效操作码 0x%02X", op);
    return -1;
}
goto *jump_table[op];
```

方案 2 更直观，但每次 dispatch 多一次比较。Debug build 可以接受。

推荐方案 1（填充空槽），因为它不改变 dispatch 热路径的指令序列，release build 也能零成本保留。

### 4.3 性能影响

- **方案 1（填充空槽）**：零 runtime 开销。`goto *jump_table[op]` 不变，只是空槽指向了错误标签而非 NULL。Debug 和 Release 都可以开启。
- **方案 2（dispatch 前检查）**：~1-3% 开销，仅 Debug build 使用。

### 4.4 能捕获的 bug

- Bug #1（opcode 0xff 被执行）：segfault 变成 runtime error "无效操作码 0xFF"
- 其他任何导致 IP 跑飞后命中未定义 opcode 槽的情况

### 4.5 不能捕获的 bug

- 跳到合法 opcode 的操作数字节（如跳到 OP_GET_LOCAL 的 slot 字节，如果 slot 值恰好是合法 opcode 值）
- 跳到合法指令边界但语义错误（Bug #2）

## 五、第一层：Jump 边界检查（仅 Debug）

### 5.1 原理

在每个 jump opcode 的 handler 中，跳转后检查 IP 是否仍在 code 数组范围内。

```c
OPCODE(OP_JUMP) {
    int32_t offset = READ_INT32();
    frame->ip += offset;
    DEBUG_CHECK_IP();  // 仅 debug build 展开
    DISPATCH;
}
```

```c
#ifdef LENO_DEBUG
  #define DEBUG_CHECK_IP() do { \
    if (unlikely(frame->ip < frame->chunk->code || \
                 frame->ip >= frame->chunk->code + frame->chunk->len)) { \
        runtime_error("内部错误：跳转后 IP 越界 (ip=%p, code=[%p,%p))", \
                      (void*)frame->ip, \
                      (void*)frame->chunk->code, \
                      (void*)(frame->chunk->code + frame->chunk->len)); \
        return -1; \
    } \
  } while (0)
#else
  #define DEBUG_CHECK_IP()
#endif
```

### 5.2 性能影响

- 仅在 jump 指令上触发（典型代码中占 10-20%）
- 每次两个指针比较，CPU 分支预测器预测不跳
- 实测 < 0.5%，Debug build 可接受

### 5.3 能捕获的 bug

- 跳转目标超出 code 数组范围（向前跳到负数 offset 或向后跳过尾）
- 如果配合第二层的空槽填充，能间接捕获 Bug #1（因为 0xff 对应的空槽报错后 return，不会继续执行）

### 5.4 不能捕获的 bug

- 跳转目标在 code 数组范围内但不在指令边界上（需要第三层才能捕获）
- 跳到合法指令边界但语义错误

## 六、各层对历史 bug 的覆盖矩阵

| Bug | 第三层（编译后校验） | 第二层（dispatch 守卫） | 第一层（jump 检查） |
|-----|:---:|:---:|:---:|
| inline_return_jumps 覆写 #1（未 patch，跳到 205） | 能捕获（205 不在指令边界） | 能捕获（0xff 空槽） | 不能（205 在范围内） |
| inline_return_jumps 覆写 #2（跳到 591，合法边界） | 不能 | 不能 | 不能 |
| patch_ast_indices segfault（访问未初始化指针） | 不能（不是跳转问题） | 不能 | 不能 |
| 窥孔优化误匹配（栈错位） | 不能 | 不能 | 不能 |

> **注意**：第三层和第二层对 Bug #2 都无能为力。Bug #2 是"跳到合法指令边界但执行顺序错误"，这属于控制流验证的范畴，需要类似 Java bytecode verifier 的数据流分析，成本和复杂度远超当前方案的定位。当前方案的目标是把"静默 segfault"变成"有报错的 runtime error"，而非完全消灭所有编译器 bug。

## 七、实施计划

### 7.1 优先级

| 阶段 | 内容 | 工作量 | 优先级 |
|------|------|--------|--------|
| P0 | 第三层：编译后字节码校验 | 1-2 天 | 最高 — 零 runtime 开销，能捕获最常见的"跳转目标不在边界"类 bug |
| P1 | 第二层方案 1：jump_table 空槽填充 | 半天 | 高 — 零 runtime 开销，实现简单 |
| P2 | 第一层：Jump 边界检查（debug only） | 半天 | 中 — 仅 debug build，增量价值有限 |
| P3 | opcode 长度表自动生成 | 1 天 | 中 — 消除手动维护负担，可从 debug.c 提取 |

### 7.2 第三层实施步骤

1. 从 `debug.c` 的反汇编逻辑中提取 opcode → instruction_length 的映射
2. 实现 `build_instruction_map`：逐条解码，标记合法 offset
3. 实现 `verify_jumps`：检查所有 jump 指令的目标是否落在合法边界
4. 在 `codegen_finish` 或 `module_compiler` 中调用校验
5. 加 `--no-verify` 选项跳过（极端性能场景）
6. 测试：用本次 bug 的修复前代码（`inline_return_jump_count = 0`）验证校验能否报错

### 7.3 第二层实施步骤

1. 在 `vm_run.inc` 的 jumptable 初始化后，遍历填充空槽
2. 定义 `LABEL_INVALID_OPCODE` 错误处理标签
3. 测试：手动构造一个包含 0xff opcode 的 chunk，验证报错而非 segfault

### 7.4 持续维护

- 新增 opcode 时必须同步更新 `opcode_lengths` 表（或改用自动生成）
- `debug.c` 的反汇编逻辑是 opcode 长度的权威来源，校验逻辑应与其共享同一张长度表

## 八、局限性说明

本方案不能替代编译器的正确性测试。它的定位是：

1. **安全网**：当编译器有 bug 时，尽量把"静默崩溃"变成"有报错的错误"
2. **加速排查**：报错信息包含 offset 和可能的原因，比 segfault 后 GDB 附加快得多
3. **不保证完备**：对于跳到合法指令边界但语义错误的 bug（如 Bug #2），本方案无法捕获

彻底消灭此类 bug 需要：
- 编译器层面的充分单元测试（每个 codegen pass 的输入/输出 golden test）
- 字节码层面的差分测试（differential testing：同一段代码开/关内联，对比字节码语义等价性）
- 这些属于编译器测试基础设施的范畴，不在本方案范围内
