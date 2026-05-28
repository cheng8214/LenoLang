// ============================================================================
// 一元运算和类型转换类操作码 (OP_UNARY)
// ============================================================================
// 一元运算：取负、逻辑非
// 类型转换：转 float、转 int、转 string

OPCODE(OP_NEG) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // 快速路径：int 取反（最常见）
    if (likely(val_is_int(a))) {
        vm_stack_push(&vm, val_int(-val_as_int(a)));
        DISPATCH;
    }

    // 快速路径：float 取反
    if (val_is_float(a)) {
        vm_stack_push(&vm, val_float(-val_as_num(a)));
        DISPATCH;
    }

    // BigInt 取反（罕见）
    if (unlikely(val_is_bigint(a))) {
        ObjBigInt* ba = val_as_bigint(a);
        Value result = bigint_neg(ba);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error("操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_NOT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    vm_stack_push(&vm, val_bool(is_falsey(vm_stack_pop(&vm))));
    DISPATCH;
}

OPCODE(OP_CAST_FLOAT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    // 快速路径：int/float 转 float
    if (val_is_int(a) || val_is_float(a)) {
        vm_stack_push(&vm, val_float(val_as_num(a)));
        DISPATCH;
    }
    // BigInt 转 float
    if (val_is_bigint(a)) {
        double val = bigint_to_double(val_as_bigint(a));
        vm_stack_push(&vm, val_float(val));
        DISPATCH;
    }
    if (val_is_null(a)) {
        // null 保持为 null
        vm_stack_push(&vm, a);
    } else {
        throw_runtime_error("CAST_FLOAT 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_CAST_INT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    // 先检查 BigInt，因为 val_is_num 也包含 BigInt
    if (val_is_bigint(a)) {
        ObjBigInt* bigint = val_as_bigint(a);
        if (bigint_is_int(bigint)) {
            vm_stack_push(&vm, val_int(bigint_to_int(bigint)));
        } else {
            // BigInt 超出 int 范围，保持原值
            vm_stack_push(&vm, a);
        }
    } else if (val_is_num(a)) {
        vm_stack_push(&vm, val_int((int)val_as_num(a)));
    } else if (val_is_null(a)) {
        // null 保持为 null
        vm_stack_push(&vm, a);
    } else {
        throw_runtime_error("CAST_INT 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_CAST_STRING) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    if (val_is_null(a)) {
        // null 保持为 null
        vm_stack_push(&vm, a);
    } else if (val_is_string(a)) {
        // 已经是字符串，保持原值
        vm_stack_push(&vm, a);
    } else {
        // 其他类型，转换为字符串表示
        const char* str = val_to_string(a);
        vm_stack_push(&vm, val_obj((struct Object*)str_copy(str, (int)strlen(str))));
    }
    DISPATCH;
}
