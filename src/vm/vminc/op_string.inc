// ============================================================================
// 字符串操作类操作码 (OP_STRING)
// ============================================================================

OPCODE(OP_STRING_ADD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 字符串拼接：将栈顶两个值弹出，转换为字符串后拼接
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    /* 拼接逻辑抽到 string_add（vm.c）—— **语义唯一来源**：
     * 别在本地再写一份拼接（历史上两份实现漂移过，§8.64）。 */
    vm_stack_push(&vm, string_add(a, b));
    DISPATCH;
}
