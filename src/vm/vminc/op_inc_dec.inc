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
        throw_runtime_error_with_operand("自增运算: 操作数必须是数字", "INC (++)", a, GET_CURRENT_LINE());
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
        throw_runtime_error_with_operand("自减运算: 操作数必须是数字", "DEC (--)", a, GET_CURRENT_LINE());
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
        throw_runtime_error_with_operand("后缀自增运算: 操作数必须是数字", "INC_LOCAL (++)", old_val, GET_CURRENT_LINE());
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
        throw_runtime_error_with_operand("后缀自减运算: 操作数必须是数字", "DEC_LOCAL (--)", old_val, GET_CURRENT_LINE());
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
        throw_runtime_error_with_operand("前缀自增运算: 操作数必须是数字", "PRE_INC_LOCAL (++)", old_val, GET_CURRENT_LINE());
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
        throw_runtime_error_with_operand("前缀自减运算: 操作数必须是数字", "PRE_DEC_LOCAL (--)", old_val, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, -1, &error);
    set_local_var(frame, slot, new_val);
    vm_stack_push(&vm, new_val);  // 前缀：返回新值
    DISPATCH;
}

OPCODE(OP_INC_LOCAL_NOPUSH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error_with_operand("自增运算: 操作数必须是数字", "INC_LOCAL_NOPUSH (++)", old_val, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, +1, &error);
    set_local_var(frame, slot, new_val);
    // 不压栈，省掉 OP_POP
    DISPATCH;
}

OPCODE(OP_DEC_LOCAL_NOPUSH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t slot = READ_SHORT();
    int error;
    Value old_val = get_local_var(frame, slot, &error);
    if (error) return -1;
    if (!val_is_num(old_val)) {
        throw_runtime_error_with_operand("自减运算: 操作数必须是数字", "DEC_LOCAL_NOPUSH (--)", old_val, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    Value new_val = do_inc_dec(old_val, -1, &error);
    set_local_var(frame, slot, new_val);
    // 不压栈，省掉 OP_POP
    DISPATCH;
}
