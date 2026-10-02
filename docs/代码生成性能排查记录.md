# Codegen 性能排查：pe_analyzer.leno 首次编译 4.3s 瓶颈

## 问题概述

| 指标 | pe_analyzer | file_manager | 倍数 |
|------|------------|--------------|------|
| 代码行数 | 2420 | 1328 | 1.8x |
| codegen 耗时 | 4134-4380ms | ~40ms | ~110x |
| gen_expr 调用 | 7027+ | 3687 | 1.9x |
| switch 数 | 16 | 3 | 5.3x |
| func 关键字 | 66 (11顶层+42方法+13匿名) | — | — |

入口文件 .lenb 缓存已修复（第二次启动 9ms），但首次编译 codegen 仍需 4.3s，与代码量不成比例。

## 已排除的路径（有证据）

### 1. emit_byte / chunk_write — 不是瓶颈
- 364,327 次调用，总耗时仅 8.9ms
- chunk_write 使用几何扩容（malloc+memcpy），O(1) 摊销

### 2. gen_func_proto self 开销 — 不是瓶颈
- 2,214 次调用（注：源码只有 66 个 func，此数字未解释，见下文"待查"）
- self time 仅 1.6ms，时间全在递归 body 中

### 3. infer_expr_type 快速类型检查路径 — 不是瓶颈
- 用 `#if 0` 禁用整个类型推断快速路径后，codegen 从 4324ms 降至 4134ms（仅降 4%）
- 说明类型推断不是主要耗时来源

### 4. gen_block pass1/pass2 — 不是瓶颈
- pass1（全局函数预注册）= 0.5ms
- pass2（局部函数原型生成）= 0.2ms
- 三遍扫描策略本身没有开销

### 5. 字符串内化 intern_find — 不是瓶颈
- 哈希表 O(1) + 小 LRU 缓存
- 非线性扫描

### 6. clock() 精度问题 — 已排除
- Windows/MSYS 下 clock() 精度约 15ms
- 之前"gen_expr 每次 0.47ms"是精度不足的假象
- 后续改用 QueryPerformanceCounter（QPC），精度达微秒级

### 7. gc_alloc — 不是瓶颈
- 延迟 GC，不同步回收

## gen_expr profiling 数据不可靠（重要）

**gen_expr 的 per-AST-kind 计时器有设计缺陷**：大部分 switch case 内部有 `return` 语句，在到达 line 1636 的 `g_prof_ms[_kind] += perf_now_ms() - _t0` 之前就返回了。导致所有 kind 的 ms 字段都是 0.0，调用量统计准确但时间统计完全失效。

修复方向：在每个 `return` 前补计时累积，或改用 `goto end_prof;` 模式统一出口。

## 已测量但无法定位的数据

### inclusive 计时器的递归重叠问题

| 计时器 | inclusive 时间 | 说明 |
|--------|--------------|------|
| codegen total | 4324ms | QPC 测量（含 profiler 开销） |
| gen_block pass3 | 7817ms | 远超 total，因递归双重计数 |
| gen_switch | 3439ms | 与 func_proto body 重叠 |
| gen_func_proto body | 3467ms | 与 switch 重叠 |

调用链递归：`gen_func_proto → gen_block → pass3 → gen_stmt → gen_switch → gen_block → ...`

inclusive 时间在深度递归时无法定位 self 热点，需要切换到 self-time 口径。

## 待排查方向（按优先级排序）

### 优先级 1：scope_resolve 调用开销

**理由**：
- AST_VAR handler 调用 scope_resolve（24,332 次）
- AST_CALL handler 调用 scope_resolve 至少 1-2 次（7,027 次 CALL，line 1191 + line 1207 可能重复调用）
- 总计 31,000+ 次 scope_resolve 调用，从未被计时
- 如果 scope_resolve 是 O(n) 线性扫描 scope 链，且 struct 方法体内 scope 层数多、变量多，则 31000 * O(n) 可能是主要开销

**排查方法**：
- 在 `scope_resolve`（scope.c:246）入口和出口加 QPC 计时 + 调用计数
- 输出总调用次数、总耗时、平均耗时
- 如果总耗时 > 500ms，即为瓶颈

### 优先级 2：gen_switch 内部开销分解

**理由**：
- pe_analyzer 有 16 个 switch（vs file_manager 的 3 个），5.3x
- gen_switch inclusive = 3439ms（虽然与递归重叠，但仍是最大单一 inclusive 值）
- gen_switch 内部有：插入排序（O(n²) 最坏）、3 次 malloc、Value 比较（可能调 val_as_double）

**排查方法**：
- 给 gen_switch 的排序阶段、malloc 阶段、emit 阶段分别加 QPC 计时
- 统计每个 switch 的 case_count
- 检查插入排序是否触发 O(n²) 路径

### 优先级 3：ast_has_try 递归遍历

**理由**：
- gen_func_proto 内部调用 `ast_has_try(ast->u.func.body)`（codegen_func.c:56-78）
- ast_has_try 递归遍历整个函数体的 AST 子树
- 如果函数嵌套深，同一个子树被多次遍历（父函数遍历包含子函数，子函数又单独遍历）
- 66 个函数 * 各自 AST 子树 = 可能 O(n²) 级别的重复遍历

**排查方法**：
- 在 ast_has_try 入口加计数器 + QPC 计时
- 输出总调用次数和总耗时
- 如果总耗时显著，考虑缓存 has_try 结果到 AST 节点

### 优先级 4：2214 次 gen_func_proto 调用之谜

**理由**：
- 源码只有 66 个 `func` 关键字
- 但 profiler 报告 gen_func_proto 被调用 2214 次
- 33x 放大，从未解释
- 可能是 profiler 计数器 bug，也可能有未知代码路径重复编译函数

**排查方法**：
- 在 gen_func_proto 入口打印函数名 + 调用栈深度
- 运行后检查哪些函数被重复编译
- 检查 gen_stmt 的 AST_STRUCT_DEF case 是否与 gen_struct_def 函数重复调用

### 优先级 5：使用真正的采样 profiler

**理由**：
- 手工插桩计时器在深度递归代码中不可靠
- QPC 调用本身有开销（~1μs/次），70k 次 gen_expr * 2 = 140ms 额外开销
- 采样 profiler 无观测开销

**排查方法**：
- 方案 A：用 GCC `-pg` 编译，用 gprof 分析（需 MSYS 下可用）
- 方案 B：用 Windows Performance Recorder (WPR) / xperf 采样，导入符号
- 方案 C：用 Very Sleepy（Windows 原生采样 profiler，支持 MSYS 符号）

### 优先级 6：二分法定位热点代码段

**理由**：
- 不需要改编译器代码，只需修改 pe_analyzer.leno 源码
- 通过注释掉大段代码来缩小范围

**排查方法**：
- 将 pe_analyzer.leno 的 struct 方法逐段注释
- 每次注释一半，观察 codegen 时间变化
- 当时间从 4s 降至 ~2s 时，说明热点在被注释的一半中
- 递归二分直到定位到具体函数

## 关键文件路径

| 文件 | 作用 |
|------|------|
| `src/codegen/codegen_expr.c` | gen_expr（L1235），AST_CALL handler（L1204-1270） |
| `src/codegen/codegen_stmt.c` | gen_block（L2428），gen_stmt（L2520），gen_switch（L169） |
| `src/codegen/codegen_func.c` | gen_func_proto（L82），ast_has_try（L56），gen_func（L234） |
| `src/codegen/codegen_emit.c` | emit_byte / chunk_write |
| `src/main.c` | codegen 入口调用（L210） |
| `src/scope.c` | scope_resolve（L246） |

## 关键数据基线（供对比）

不带任何 profiling 代码的原始运行：
```
[TIME] parse:     3.0 ms
[TIME] semantic: 423.0 ms
[TIME] optimize:   1.0 ms
[TIME] codegen: 4134.0 ms
```

gen_expr 调用分布（调用量准确，时间不准）：
```
VAR     24332    BINOP 10963    CALL 7027
NUM      4925    STRING 3387    BOOL 1007
NULL      733    ARRAY   407    DICT  111
UNARY    856
```

总计 ~53,248 次 gen_expr 调用。按 4134ms 算，平均每个 AST 节点 78μs，异常偏高。

## 总结

codegen 4.3s 的根因尚未定位。已排除 emit_byte（8.9ms）、gen_func_proto self（1.6ms）、infer_expr_type（禁用后仅降 4%）、字符串内化（O(1)）。最可能的嫌疑对象是 scope_resolve 的调用开销、gen_switch 内部的排序/malloc、ast_has_try 的重复遍历，以及未解释的 2214 vs 66 函数调用差异。建议优先用采样 profiler（Very Sleepy 或 WPR）获取 self-time 分布，或用源码二分法快速缩小范围。
