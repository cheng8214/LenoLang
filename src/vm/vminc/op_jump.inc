// ============================================================================
// 跳转控制类操作码 (OP_JUMP)
// ============================================================================

OPCODE(OP_JUMP) {
    int32_t offset = READ_INT32();
    frame->ip += offset;
    DISPATCH;
}

OPCODE(OP_JUMP_IF_FALSE) {
    int32_t offset = READ_INT32();
    if (is_falsey(vm_stack_peek(&vm, 0))) {
        frame->ip += offset;
    }
    DISPATCH;
}

OPCODE(OP_JUMP_IF_TRUE) {
    int32_t offset = READ_INT32();
    if (!is_falsey(vm_stack_peek(&vm, 0))) {
        frame->ip += offset;
    }
    DISPATCH;
}

OPCODE(OP_LOOP) {
    int32_t offset = READ_INT32();
    frame->ip -= offset;
    DISPATCH;
}
