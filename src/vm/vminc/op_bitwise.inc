// ============================================================================
// 位运算类操作码 (OP_BITWISE)
// ============================================================================
// 位运算：与、或、异或、左移、右移、位非

OPCODE(OP_BITAND) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int & int（最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        vm_stack_push(&vm, val_int(val_as_int(a) & val_as_int(b)));
        DISPATCH;
    }

    // null 不能参与位运算（罕见错误）
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("位与运算: null 不能参与运算", "BITAND (&)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位运算（罕见）
    if (unlikely(needs_bigint_op(a, b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_and(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("位与运算: 操作数必须是整数", "BITAND (&)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_BITOR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int | int（最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        vm_stack_push(&vm, val_int(val_as_int(a) | val_as_int(b)));
        DISPATCH;
    }

    // null 不能参与位运算（罕见错误）
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("位或运算: null 不能参与运算", "BITOR (|)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位运算（罕见）
    if (unlikely(needs_bigint_op(a, b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_or(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("位或运算: 操作数必须是整数", "BITOR (|)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_BITXOR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int ^ int（最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        vm_stack_push(&vm, val_int(val_as_int(a) ^ val_as_int(b)));
        DISPATCH;
    }

    // null 不能参与位运算（罕见错误）
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("位异或运算: null 不能参与运算", "BITXOR (^)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位运算（罕见）
    if (unlikely(needs_bigint_op(a, b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_xor(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("位异或运算: 操作数必须是整数", "BITXOR (^)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_SHL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int << int（移位量 < 32，最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        int shift = val_as_int(b);
        if (likely(shift < 32)) {
            vm_stack_push(&vm, val_int(val_as_int(a) << shift));
            DISPATCH;
        }
        // shift >= 32 时继续执行 BigInt 路径
        ObjBigInt* ba = promote_to_bigint(a);
        Value result = bigint_shl(ba, shift);
        vm_stack_push(&vm, result);
        DISPATCH;
    }

    // null 不能参与位移运算（罕见错误）
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("左移运算: null 不能参与运算", "SHL (<<)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // 检查是否需要使用 BigInt（罕见）
    int shift;
    if (!value_to_index(b, &shift)) {
        throw_runtime_error_with_operands("左移运算: 位移量必须是整数", "SHL (<<)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    if (unlikely(val_is_bigint(a) || val_is_bigint(b) || shift >= 32)) {
        ObjBigInt* ba = promote_to_bigint(a);
        Value result = bigint_shl(ba, shift);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("左移运算: 操作数必须是整数", "SHL (<<)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_SHR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int >> int（最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        vm_stack_push(&vm, val_int(val_as_int(a) >> val_as_int(b)));
        DISPATCH;
    }

    // null 不能参与位移运算（罕见错误）
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("右移运算: null 不能参与运算", "SHR (>>)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位移（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        int shift;
        if (!value_to_index(b, &shift)) {
            throw_runtime_error_with_operands("右移运算: 位移量必须是整数", "SHR (>>)", a, b, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        Value result = bigint_shr(ba, shift);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("右移运算: 操作数必须是整数", "SHR (>>)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_SHL_IMM) {
    uint8_t imm = READ_BYTE();
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // 快速路径：int << imm（最常见，imm < 32）
    if (likely(val_is_int(a))) {
        if (likely(imm < 32)) {
            vm_stack_push(&vm, val_int(val_as_int(a) << imm));
            DISPATCH;
        }
        ObjBigInt* ba = promote_to_bigint(a);
        Value result = bigint_shl(ba, imm);
        vm_stack_push(&vm, result);
        DISPATCH;
    }

    // null 不能参与位移运算
    if (unlikely(val_is_null(a))) {
        throw_runtime_error_with_operand("左移运算: null 不能参与运算", "SHL_IMM (<<)", a, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位移
    if (unlikely(val_is_bigint(a) || imm >= 32)) {
        ObjBigInt* ba = promote_to_bigint(a);
        Value result = bigint_shl(ba, imm);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operand("左移运算: 操作数必须是整数", "SHL_IMM (<<)", a, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_SHR_IMM) {
    uint8_t imm = READ_BYTE();
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // 快速路径：int >> imm（最常见）
    if (likely(val_is_int(a))) {
        vm_stack_push(&vm, val_int(val_as_int(a) >> imm));
        DISPATCH;
    }

    // null 不能参与位移运算
    if (unlikely(val_is_null(a))) {
        throw_runtime_error_with_operand("右移运算: null 不能参与运算", "SHR_IMM (>>)", a, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 位移
    if (unlikely(val_is_bigint(a))) {
        ObjBigInt* ba = val_as_bigint(a);
        Value result = bigint_shr(ba, imm);
        vm_stack_push(&vm, result);
        DISPATCH;
    }

    throw_runtime_error_with_operand("右移运算: 操作数必须是整数", "SHR_IMM (>>)", a, GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

// >>> 逻辑右移（无符号右移，高位补 0）
OPCODE(OP_USHR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int >>> int（最常见）
    if (likely(val_is_int(a) && val_is_int(b))) {
        int64_t va = val_as_int(a);
        int64_t vb = val_as_int(b);
        // LenoC 的 int 是 48 位有符号整数
        // 逻辑右移：先转为 48 位无符号值，再右移，高位补 0
        uint64_t uva = (uint64_t)va & PAYLOAD_MASK;  // 截断为 48 位无符号
        uint64_t shifted = uva >> vb;
        // 检查结果是否在 48 位 int 范围内
        if ((int64_t)shifted >= INT48_MIN && (int64_t)shifted <= INT48_MAX) {
            vm_stack_push(&vm, val_int((int64_t)shifted));
        } else {
            // 超出 int48 范围，转为 BigInt
            vm_stack_push(&vm, val_bigint_from_int64((int64_t)shifted));
        }
        DISPATCH;
    }

    // null 不能参与位移运算
    if (unlikely(val_is_null(a) || val_is_null(b))) {
        throw_runtime_error_with_operands("逻辑右移运算: null 不能参与运算", "USHR (>>>)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 逻辑右移
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        int shift;
        if (!value_to_index(b, &shift)) {
            throw_runtime_error_with_operands("逻辑右移运算: 位移量必须是整数", "USHR (>>>)", a, b, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        Value result = bigint_ushr(ba, shift);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operands("逻辑右移运算: 操作数必须是整数", "USHR (>>>)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

// >>> 立即数逻辑右移
OPCODE(OP_USHR_IMM) {
    uint8_t imm = READ_BYTE();
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // 快速路径：int >>> imm（最常见）
    if (likely(val_is_int(a))) {
        int64_t va = val_as_int(a);
        // LenoC 的 int 是 48 位有符号整数
        // 逻辑右移：先转为 48 位无符号值，再右移，高位补 0
        uint64_t uva = (uint64_t)va & PAYLOAD_MASK;  // 截断为 48 位无符号
        uint64_t shifted = uva >> imm;
        // 检查结果是否在 48 位 int 范围内
        if ((int64_t)shifted >= INT48_MIN && (int64_t)shifted <= INT48_MAX) {
            vm_stack_push(&vm, val_int((int64_t)shifted));
        } else {
            // 超出 int48 范围，转为 BigInt
            vm_stack_push(&vm, val_bigint_from_int64((int64_t)shifted));
        }
        DISPATCH;
    }

    // null 不能参与位移运算
    if (unlikely(val_is_null(a))) {
        throw_runtime_error_with_operand("逻辑右移运算: null 不能参与运算", "USHR_IMM (>>>)", a, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // BigInt 逻辑右移
    if (unlikely(val_is_bigint(a))) {
        ObjBigInt* ba = val_as_bigint(a);
        Value result = bigint_ushr(ba, imm);
        vm_stack_push(&vm, result);
        DISPATCH;
    }

    throw_runtime_error_with_operand("逻辑右移运算: 操作数必须是整数", "USHR_IMM (>>>)", a, GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_BITNOT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // 快速路径：~int（最常见）
    if (likely(val_is_int(a))) {
        vm_stack_push(&vm, val_int(~val_as_int(a)));
        DISPATCH;
    }

    // BigInt 位非（罕见）
    if (unlikely(val_is_bigint(a))) {
        ObjBigInt* ba = val_as_bigint(a);
        Value result = bigint_not(ba);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error_with_operand("位非运算: 操作数必须是整数", "BITNOT (~)", a, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}
