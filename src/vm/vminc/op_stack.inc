// ============================================================================
// 栈操作类操作码 (OP_STACK)
// ============================================================================

OPCODE(OP_POP) {
    vm_stack_pop(&vm);
    DISPATCH;
}

OPCODE(OP_DUP) {
    Value v = vm_stack_peek(&vm, 0);
    vm_stack_push(&vm, v);
    DISPATCH;
}
