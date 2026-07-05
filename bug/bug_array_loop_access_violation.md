# Bug: while 循环中 continue 跳转未回填，导致 ACCESS_VIOLATION

## 环境
- 提交: 修复前
- 平台: Windows/Linux/macOS 全平台

## 根因

**`gen_while()` 中 `continue` 语句的跳转偏移量未被回填。**

### 代码分析

`codegen_stmt.c` 中的 `gen_while()` 函数：

- `AST_CONTINUE` 生成 `OP_JUMP` 占位指令，跳转偏移记录在 `loop->continue_jumps[]`
- 循环结束时，**只回填了 `break` 跳转，没有回填 `continue` 跳转**
- `continue` 的 `OP_JUMP` 指令跳转到未初始化的偏移量（随机内存地址），导致 ACCESS_VIOLATION

对比 `gen_for_iter()` / `gen_for()` 都有正确的 continue 回填：
```c
for (int i = 0; i < loop->continue_count; i++) {
    patch_jump_to(gen, loop->continue_jumps[i], continue_target);
}
```

而 `gen_while()` 缺少这段代码。

### 为什么之前没发现

- `continue` 在**单层 while** 循环中很少使用
- 简单的 `while ev.poll()` 循环中通常不用 `continue`
- 只有**嵌套 while + continue** 的组合才会触发：内层 `while ev.poll()` 中的 `continue` 跳转到了随机位置
- 之前误以为是 GC 或 Array[T] 的问题，实际是 `continue` 在嵌套循环中跳到了错误的位置

### 触发条件

```leno
while outer {            // 外层循环
    while inner {        // 内层循环
        if cond {
            continue    // ← 这里跳转到随机位置！
        }
    }
}
```

## 修复

在 `gen_while()` 的 break 回填之后，增加 continue 回填：

```c
// 回填 continue 跳转到循环条件检查位置（loop_start）
for (int i = 0; i < loop->continue_count; i++) {
    patch_jump_to(gen, loop->continue_jumps[i], loop_start);
}
```

同时修复 `while true` 优化路径，也增加 continue 回填。

## 文件
- `src/codegen/codegen_stmt.c` — `gen_while()` 函数

## 状态
- [x] 已修复
- [x] 全部 147 个断言测试通过
- [x] SDL3 多窗口管理器正常工作
