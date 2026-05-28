// ============================================================================
// 自增自减操作码 (OP_INC_DEC)
// ============================================================================
// 使用公共函数简化

OPCODE(OP_INC) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    int error;
    Value result = do_inc_dec(a, +1, &error);
    if (error) {
        throw_runtime_error("++ 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    vm_stack_push(&vm, result);
    DISPATCH;
}

OPCODE(OP_DEC) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    int error;
    Value result = do_inc_dec(a, -1, &error);
    if (error) {
        throw_runtime_error("-- 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    vm_stack_push(&vm, result);
    DISPATCH;
}

OPCODE(OP_INC_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error("++ 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, +1, &error);
    set_local_var(frame, slot, new_val);
    vm_stack_push(&vm, old_val);  // 后缀：返回旧值
    DISPATCH;
}

OPCODE(OP_DEC_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error("-- 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, -1, &error);
    set_local_var(frame, slot, new_val);
    vm_stack_push(&vm, old_val);  // 后缀：返回旧值
    DISPATCH;
}

OPCODE(OP_PRE_INC_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error("++ 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, +1, &error);
    set_local_var(frame, slot, new_val);
    vm_stack_push(&vm, new_val);  // 前缀：返回新值
    DISPATCH;
}

OPCODE(OP_PRE_DEC_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error("-- 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, -1, &error);
    set_local_var(frame, slot, new_val);
    vm_stack_push(&vm, new_val);  // 前缀：返回新值
    DISPATCH;
}
