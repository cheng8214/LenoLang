# 间歇性 Segfault 排查实战：从现象到根因

> 本文档记录了一次 Leno 编译器内联优化导致的间歇性 segfault (0xC0000005) 的完整排查过程，作为今后排查类似内存类崩溃问题的方法论参考。

## 一、问题现象

- **症状**：`assert/test_gc_iterative_mark.leno` 断言文件运行时约 30-50% 概率崩溃，退出码 139 (Linux) 或 -1073741819 (Windows 0xC0000005)
- **特征**：间歇性，非确定性 — 同一文件同一参数，有时崩有时不崩
- **零输出**：崩溃时 stdout/stderr 无任何输出，连程序最早期的 printf 都没打出来
- **触发条件**：void 函数被内联 + 函数体内有函数调用产生 GC 对象 + 最后一条语句是插值字符串 `print($"...")`

## 二、排查过程中走过的弯路

### 弯路 1：假设崩溃在 VM 运行期

最初的调试总结文件假设根因在 `OP_STRING_ADD` 的栈操作与 `OP_CLEAR_LOCAL_RANGE` 的槽位清理冲突。这个方向看似合理（有字节码对比佐证），但实际上：

- **错误前提**：假设崩溃发生在 VM 执行阶段
- **实际情况**：崩溃发生在 codegen 阶段（字节码生成阶段），VM 根本还没开始运行

**教训**：在假设崩溃位置之前，先确认程序执行到哪一步了。零输出是一个重要信号 — 如果连 `printf("debug模式:进入主执行流程")` 都没打出来，崩溃在 `lenolang_run()` 之前或最早期。

### 弯路 2：假设是 --debug 模式专属问题

最初只在 `--debug` 模式下测试，发现崩溃率很高。但后续去掉 `--debug` 跑 20 次发现**不用 --debug 也崩**。

**教训**：排查时要剥离变量。先确认"是否真的需要 --debug 才崩"，再决定排查方向。如果两个变量（内联 + debug）同时存在，要先固定一个。

### 弯路 3：怀疑 debug.c 反汇编器偏移计算

确实 debug.c 的 `OP_STRUCT_DEF` 反汇编有偏移计算错误（漏读 `type_param_count` 和 `nullable`，硬编码了错误的类型常量值），这会导致 `--debug` 模式下反汇编越界。但修复后发现**非 debug 模式仍然崩溃**。

**教训**：一个项目中可能同时存在多个 bug。修了一个发现还在崩，说明还有别的。不要因为找到一个 bug 就停下来，要继续验证。

## 三、正确路径：GDB 一步定位

### 关键转折：加 `-g` 编译 + 用 GDB 附加

当确认崩溃是零输出、非确定性后，唯一有效的方法是用调试器附加获取崩溃栈。

```bash
# build.bat 加 -g -O0（临时，定位后去掉）
gcc -o build\leno.exe !SOURCES! -Isrc -Wall -Wextra -std=c99 -g -O0 -lm -municode -lws2_32
```

```bash
# GDB 批量模式，反复运行直到复现
cat > /tmp/gdb_script.txt << 'EOF'
set pagination off
set print thread-events off
run test_inline_debug.leno
bt
quit
EOF

for i in $(seq 1 50); do
  output=$(gdb -batch -x /tmp/gdb_script.txt ./build/leno.exe 2>&1)
  if echo "$output" | grep -q "SIGSEGV"; then
    echo "$output" | grep -A 30 "signal SIGSEGV"
    break
  fi
done
```

### GDB 输出的关键信息

```
Thread 1 received signal SIGSEGV, Segmentation fault.
0x... in patch_ast_indices (ast=0xbaadf00dbaadf00d, offset=0) at src\codegen\codegen_inline.c:51
51      switch (ast->kind) {

#0  patch_ast_indices (ast=0xbaadf00dbaadf00d, ...) at codegen_inline.c:51
#1  patch_ast_indices (ast=0x779230, offset=0) at codegen_inline.c:108
#2  patch_ast_indices (ast=0x779130, offset=0) at codegen_inline.c:75
#3  patch_ast_indices (ast=0x779570, offset=0) at codegen_inline.c:140
#4  patch_ast_indices (ast=0x777f20, offset=0) at codegen_inline.c:136
#5  try_inline_call (gen=0x5fe080, ast=0x779a80, ...) at codegen_inline.c:436
```

**一行 GDB 输出直接锁定了根因**：

1. **`ast=0xbaadf00dbaadf00d`** — 这是 Windows debug heap 的未初始化内存标记值（"bad food"），说明读到了一个从未被赋值的指针
2. **调用栈** — `try_inline_call` → `patch_ast_indices(body)` → 递归到 `AST_INTERP_STRING` 的 `exprs[i]` 时读到了垃圾值
3. **codegent_inline.c:108** — 正是遍历 `interp_string.exprs` 的循环

### 为什么不用 GDB 之前定位不了

- 崩溃零输出，无法从输出推断崩溃位置
- 间歇性意味着无法通过加 printf 来定位（printf 本身会改变时序，可能掩盖 bug）
- `0xbaadf00d` 只在 debug heap 模式下出现，普通运行时这个值可能是任何东西

## 四、根因分析

### interp_string 的数据布局

Leno 的插值字符串 `$"count={c}"` 在 AST 中表示为：

```
interp_string {
    parts: ["count=", ""]     // count 个片段
    exprs: [c_expr]           // count-1 个表达式
    count: 2                  // 片段数量
}
```

**关键**：`parts` 数组有 `count` 个元素，`exprs` 数组只有 `count-1` 个元素。最后一个位置只有 parts 没有 exprs。

### bug 所在

`patch_ast_indices`（codegen_inline.c:107）遍历 `exprs` 时：

```c
// BUG: 循环上界用了 count，但 exprs 只有 count-1 个元素
for (int i = 0; i < ast->u.interp_string.count; i++)
    patch_ast_indices(ast->u.interp_string.exprs[i], offset);
```

当 `i == count-1` 时，`exprs[count-1]` 是一个**从未被赋值的未初始化指针**，解引用后 segfault。

### 为什么是间歇性

`exprs` 用 `malloc` 分配，未初始化槽位的值取决于堆状态：
- 有时是 `0xbaadf00d`（Windows debug heap 标记）→ 必崩
- 有时是 `NULL` → `patch_ast_indices` 开头有 `if (!ast) return` 保护 → 不崩
- 有时恰好是某个看起来有效的地址 → 可能不崩，也可能崩在别的地方

这完美解释了 30-50% 的崩溃率。

### 为什么只有内联时触发

`patch_ast_indices` 只在函数内联时被调用 — 它负责给内联函数体中所有局部变量索引加上偏移量。非内联路径根本不会执行这段代码。

## 五、修复内容

### 修复 1：循环边界（根本修复）

```c
// 修复前
for (int i = 0; i < ast->u.interp_string.count; i++)

// 修复后：与 gen_expr、ast_free 等所有其他遍历点一致
for (int i = 0; i < ast->u.interp_string.count - 1; i++)
```

### 修复 2：parser 初始化 exprs 数组（防御性修复）

```c
// parser_expr.c — malloc 后初始化为 NULL
ast->u.interp_string.exprs = (Ast**)malloc(sizeof(Ast*) * capacity);
for (int i = 0; i < capacity; i++) {
    ast->u.interp_string.exprs[i] = NULL;
}

// realloc 扩容后同样初始化新增槽位
int old_cap = capacity;
capacity *= 2;
ast->u.interp_string.exprs = (Ast**)realloc(..., sizeof(Ast*) * capacity);
for (int i = old_cap; i < capacity; i++) {
    ast->u.interp_string.exprs[i] = NULL;
}
```

### 修复 3：debug.c OP_STRUCT_DEF 反汇编偏移（独立 bug）

在排查过程中发现 `--debug` 模式下 `OP_STRUCT_DEF` 的反汇编偏移计算也有三个错误：

| 问题 | 修复前 | 修复后 |
|------|--------|--------|
| 漏读 `type_param_count` | 不跳过 | 读取 count + 跳过每个参数 2 字节 |
| 漏读字段 `nullable` 标记 | 不跳过 | 每个字段 +1 字节 |
| 硬编码类型常量 | `21`/`26`（实际是 TYPE_I32/TYPE_F64） | `TYPE_STRUCT`/`TYPE_PTR_GENERIC` 枚举常量 |

## 六、方法论总结

### 排查间歇性 segfault 的步骤

1. **确认崩溃范围**：用最小复现代码 + 控制变量法，逐一剥离条件，确认哪些是必要条件
2. **检查输出**：零输出 = 崩溃在极早期，不要假设崩溃在运行期
3. **用 GDB 定位**：加 `-g -O0` 编译，用 `gdb -batch` 批量运行直到复现，`bt` 获取调用栈
4. **分析崩溃值**：`0xbaadf00d` = 未初始化内存，`0xcccccccc` = 未初始化栈变量，`0xfeeefeee` = 已释放内存
5. **修复后全量验证**：连续运行 50+ 次确认 0 崩溃

### 防御性编程原则

- `malloc`/`realloc` 后立即初始化为 NULL 或 0
- 数组遍历上界要与实际数据布局一致，不要假设所有数组长度相同
- 当一个结构有"parts 有 N 个，exprs 有 N-1 个"这种不对称布局时，在头文件注释中标注清楚

### Leno 编译器特定经验

- `interp_string` 的 `parts` 有 `count` 个元素，`exprs` 只有 `count-1` 个 — 遍历 exprs 必须用 `count-1`
- `patch_ast_indices` 只在内联展开时被调用，所以这个 bug 只在内联时触发
- `TypeKind` 枚举值会随新增类型而变化，debug.c 中不能硬编码数字，必须用枚举常量
