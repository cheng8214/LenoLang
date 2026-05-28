// ============================================================================
// 常量加载类操作码 (OP_CONSTANTS)
// ============================================================================

OPCODE(OP_CONST) {
    Value constant = READ_CONSTANT();
    vm_stack_push(&vm, constant);
    DISPATCH;
}

OPCODE(OP_NULL) {
    vm_stack_push(&vm, val_null());
    DISPATCH;
}

OPCODE(OP_TRUE) {
    vm_stack_push(&vm, val_bool(1));
    DISPATCH;
}

OPCODE(OP_FALSE) {
    vm_stack_push(&vm, val_bool(0));
    DISPATCH;
}

OPCODE(OP_ZERO) {
    vm_stack_push(&vm, val_int(0));
    DISPATCH;
}

OPCODE(OP_ONE) {
    vm_stack_push(&vm, val_int(1));
    DISPATCH;
}
