# 窥孔优化 Bug 复盘与未来重写注意事项

## 1. Bug 概述

**现象**：`test_table_advanced.leno` 运行时报错 `cstruct 字段名必须是字符串，但实际类型为 'float'`，随后发现是常量池越界（`OP_CONST const_idx=152`，但 `const_cnt=144`）。

**根因**：`gen_expr_stmt` 中的窥孔优化通过检查 `code[len-1]` 的字节值来判断最后发射的指令是否为 `OP_DICT_SET`（1 字节指令），从而跳过 `OP_POP`。但 `code[len-1]` 可能是前一条**多字节指令的最后一个操作数字节**，其数值恰好等于 `OP_DICT_SET` 的枚举值，导致误判。误判后跳过了 `OP_POP`，栈上多留一个值，后续所有操作全部错位，最终引发各种看似无关的运行时错误。

## 2. 排查为何耗时如此之久

### 2.1 错误现象与根因之间隔了十万八千里

| 层级 | 表现 |
|------|------|
| 用户可见 | `cstruct 字段名必须是字符串，但实际类型为 'float'` |
| 第一层推测 | 常量池越界，`const_idx=152` 但 `const_cnt=144` |
| 第二层推测 | 编译器常量池生成 bug，struct 方法编译未执行 |
| 第三层推测 | 模块缓存（`.lenocache`）自动重建导致跳过编译 |
| **实际根因** | 窥孔优化误匹配操作数字节 → 跳过 OP_POP → 栈错位 |

从"类型错误"到"栈错位"到"窥孔优化"，中间隔了 5 层推理，每层都有看似合理的解释和排查方向，导致大量时间花在了错误的方向上。

### 2.2 模块缓存干扰调试

- `.lenocache` 会在第一次运行后自动生成，第二次运行命中缓存直接跳过编译器
- 加诊断 print 到编译器后，第二次运行看不到任何输出，误以为编译器代码没执行
- 用 `LENO_NO_CACHE=1` 禁用缓存又会因为大模块全量编译导致超时（120秒）
- **教训**：调试编译器问题时，每次运行前必须清空 `.lenocache`，或确认缓存不会干扰诊断输出

### 2.3 字节码修补型优化的隐蔽性

窥孔优化的本质是**先发射正常字节码，再回头修改已发射的字节**。这种"事后修改"有几个致命的隐蔽特性：

1. **修改发生在字节码层面，不是 AST 层面**：从 AST 结构完全看不出问题，必须看最终生成的字节码才能发现
2. **误匹配的条件极其苛刻**：需要前一条指令的操作数字节恰好等于目标 opcode 的枚举值，只在特定代码路径组合下触发
3. **错误传播距离远**：跳过一个 `OP_POP` 不会立即报错，而是让后续所有操作在错位的栈上运行，最终在某个完全不相关的位置崩溃
4. **难以复现**：依赖特定的代码序列组合，删一行代码或改一个变量名就可能让 bug 消失或出现

### 2.4 中间走了的弯路

- 怀疑序列化/反序列化 bug → 加 `#if 0` 禁用主程序序列化路径 → bug 仍在
- 怀疑模块编译路径 struct 方法预编译有问题 → 加 `[DBG-FUNC]` / `[DBG-MODULE]` 诊断 → 发现 struct 方法编译未出现在输出中 → 以为是编译器没走到那个分支
- 实际上是模块缓存命中导致整个编译被跳过，诊断输出自然也没有
- 怀疑常量池生成逻辑 → 加 `chunk_add_const` 诊断 → 但问题根本不在常量池生成，而在字节码被错误修补导致执行流错乱

## 3. 已删除的窥孔优化清单

以下是通过"先发射正常字节码，再回头修改"方式实现的优化，已全部删除：

| 优化 | 原理 | 危险点 |
|------|------|--------|
| `OP_CALL_NATIVE → OP_CALL_NATIVE_VOID` | 检查 `code[len-5]` 是否为 `OP_CALL_NATIVE`，改为 VOID 变体省掉 OP_POP | 5字节指令，opcode 在固定位置，相对安全但仍有风险 |
| `OP_DICT_SET → OP_DICT_SET_NOPUSH` | 检查 `code[len-1]` 是否为 `OP_DICT_SET`，改为 NOPUSH 变体 | **1字节指令，code[len-1] 极易与操作数字节碰撞，这就是 bug 根源** |
| `OP_SET_LOCAL → OP_SET_LOCAL_POP` | 复合赋值后检查 `code[len-3]` 是否为 `OP_SET_LOCAL` | 3字节指令，操作数字节可能碰撞 |

## 4. 保留的 AST 级前置优化（安全，无 bug）

以下优化是在 AST 层面直接判断模式并生成优化字节码，**不回看已生成的字节码**，不存在误匹配风险：

- `OP_SET_LOCAL_POP` — 赋值语句到局部变量，直接生成
- `OP_MOVE_LOCAL_POP` — 局部变量间复制，直接生成
- `OP_INDEX_SET_NOPUSH` — 索引赋值语句，直接生成
- `OP_INC_LOCAL_NOPUSH` / `OP_DEC_LOCAL_NOPUSH` — i++/i-- 语句，直接生成
- `OP_ARRAY_APPEND_NOPUSH` — arr.add(x) 语句，直接生成

**原则：在 AST 层面做决策，直接发射目标字节码，永远不要回头修改已发射的字节。**

## 5. 未来重写窥孔优化的注意事项

如果未来需要重新引入窥孔优化以恢复性能，必须遵循以下原则：

### 5.1 绝对禁止：检查 `code[len-N]` 的字节值

```c
// 禁止：code[len-1] 可能是多字节指令的操作数字节
if (gen->chunk->code[gen->chunk->len - 1] == OP_DICT_SET) { ... }

// 禁止：即使检查 code[len-5]，如果中间有变长指令也无法保证位置正确
if (gen->chunk->code[gen->chunk->len - 5] == OP_CALL_NATIVE) { ... }
```

操作数字节是任意值，任何字节值都可能出现在 `code` 数组中，无法通过字节值区分 opcode 和 operand。

### 5.2 推荐方案 A：AST 级前置优化（当前已采用）

在 `gen_expr_stmt` 入口处根据 AST 节点类型直接决定生成哪个字节码，不生成后再修改：

```c
// 正确：在 AST 层面判断，直接生成目标字节码
if (is_dict_set_pattern(expr)) {
    emit_byte(gen, OP_DICT_SET_NOPUSH, line);  // 直接生成 NOPUSH 版本
    return;
}
gen_expr(gen, expr);
emit_byte(gen, OP_POP, line);
```

### 5.3 推荐方案 B：结构化追踪（如果前置优化不可行）

在 `CodeGen` 中维护结构化的"最后发射的指令"信息，而非检查原始字节：

```c
typedef struct {
    OpCode opcode;      // 最后发射的 opcode（枚举值，非字节）
    int offset;         // 该 opcode 在 code[] 中的偏移
    int total_len;      // 该指令的总长度（含操作数）
} LastEmission;

// 在 CodeGen 结构体中
LastEmission last_emission;

// 在每个 emit_xxx 函数末尾设置
void emit_call_native(CodeGen* gen, ...) {
    emit_byte(gen, OP_CALL_NATIVE, line);
    emit_byte(gen, ...);
    gen->last_emission = (LastEmission){ OP_CALL_NATIVE, gen->chunk->len - 5, 5 };
}
```

然后在窥孔优化中检查结构化信息而非原始字节：

```c
// 安全：检查的是枚举值，不是字节值
if (gen->last_emission.opcode == OP_CALL_NATIVE) {
    gen->chunk->code[gen->last_emission.offset] = OP_CALL_NATIVE_VOID;
    return;
}
```

**关键**：`last_emission` 必须在每次 `emit_byte` 调用后被正确更新，且在 `gen_expr` 等复杂路径中也要传播。如果中间有任何路径发射了字节码但没有更新 `last_emission`，追踪就会失效。

### 5.4 推荐方案 C：两遍编译

第一遍正常编译生成所有字节码（含 `OP_POP`），第二遍扫描字节码流做优化替换。这种方式可以完整看到每条指令的边界，不会误匹配操作数字节。代价是多一遍扫描，但只对最终 chunk 做一次，不影响编译过程中的逻辑。

### 5.5 测试要求

重写窥孔优化后，必须通过以下测试：

1. **全量测试套件**：`build/leno.exe assert/run_tests.leno` — 246 个测试全部通过
2. **清缓存后重跑**：删除所有 `.lenocache` 后重新运行，排除缓存干扰
3. **Table 控件测试**：`test_table_advanced.leno` — 这是触发本次 bug 的场景，必须通过
4. **大量 dict.set / native call 语句的场景**：构造密集使用 `dict[k]=v` 和 `print()` 等表达式语句的测试用例，验证栈深度正确

### 5.6 代码审查检查清单

引入任何字节码修补型优化时，逐条确认：

- [ ] 是否检查了 `code[len-N]` 的字节值？如果是，**停止**，改用其他方案
- [ ] 优化的触发条件是否依赖特定指令长度？如果指令集未来新增变长指令，条件是否仍然安全？
- [ ] 操作数字节的取值范围是否与目标 opcode 的枚举值有重叠？（必然有，因为操作数字节是 0-255 全覆盖）
- [ ] 优化跳过 `OP_POP` 后，是否在所有可能的代码路径组合下都保证栈平衡？
- [ ] 是否有对应的 `_NOPUSH` / `_VOID` opcode 在 VM 端实现？
- [ ] 优化是否被 `.lenocache` 缓存？缓存失效后重新编译是否仍然正确？

## 6. 时间线

| 阶段 | 耗时 | 做了什么 |
|------|------|----------|
| 初期排查 | 较长 | 定位到常量池越界，怀疑编译器常量池生成 bug |
| 诊断迷途 | 较长 | 加 OP_INDEX 诊断、DBG-FUNC、DBG-MODULE 诊断，被模块缓存误导 |
| 序列化排查 | 中等 | 禁用序列化路径，确认 bug 在编译器本身 |
| 缓存干扰 | 较长 | 发现模块缓存自动重建导致诊断失效，尝试 LENO_NO_CACHE 超时 |
| 用户定位 | 极短 | 用户直接指出窥孔优化是根因 |
| 修复验证 | 较短 | 删除窥孔优化，编译测试通过 |
| VM 清理 | 较短 | 删除 OP_CALL_NATIVE_VOID / OP_DICT_SET_NOPUSH 的 VM 实现 |
| 颜色测试修复 | 较短 | test_color_literal.leno 从 #AARRGGBB 改为 #RRGGBBAA |

**最大教训**：当错误现象与根因之间隔了多层间接性时，不要在中间层反复加诊断，而应该回到最基础的问题——"字节码是否正确"。直接 dump 出问题函数的完整字节码（含常量池索引和操作数），人工逐步模拟栈操作，是最快定位栈错位类 bug 的方法。
