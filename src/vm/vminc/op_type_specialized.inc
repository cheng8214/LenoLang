// ============================================================================
// 类型特化指令（编译器已知类型时的快速路径）
// ============================================================================
// 说明：这些指令假设操作数已经是正确的类型，跳过运行时类型检查。
// int 运算溢出时自动提升为 bigint（对外仍表现为 int 类型值）。
// 注意：由于 bigint 对外表现为 int 类型，这些指令需要处理可能是 bigint 的 int 类型值。
// 优化策略：先快速检查是否有 bigint，如果没有就直接用 int 运算（热点路径）

OPCODE(OP_ADD_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    // 快速路径：都不是 bigint
    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)val_as_int(vb);
        PUSH_SAFE_INT64_ADD_FAST(a, b);
    } else {
        // 任一操作数是 BigInt，使用 BigInt 运算
        Value result = bigint_add(get_bigint_operand(va), get_bigint_operand(vb));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_SUB_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)val_as_int(vb);
        PUSH_SAFE_INT64_SUB_FAST(a, b);
    } else {
        // 任一操作数是 BigInt，使用 BigInt 运算
        Value result = bigint_sub(get_bigint_operand(va), get_bigint_operand(vb));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_MUL_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)val_as_int(vb);
        PUSH_SAFE_INT64_MUL_FAST(a, b);
    } else {
        // 任一操作数是 BigInt，使用 BigInt 运算
        Value result = bigint_mul(get_bigint_operand(va), get_bigint_operand(vb));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_DIV_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    // 检查除零
    if (val_is_bigint(vb)) {
        ObjBigInt* bb = val_as_bigint(vb);
        if (bb->limb_count == 1 && bb->limbs[0] == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            DISPATCH;
        }
    } else if (val_is_int(vb) && val_as_int(vb) == 0) {
        throw_runtime_error("除零错误", GET_CURRENT_LINE());
        DISPATCH;
    }

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        // int / int = int（整数除法）
        int a = val_as_int(va);
        int b = val_as_int(vb);
        vm_stack_push_fast(&vm, val_int(a / b));
    } else {
        // BigInt / BigInt = int/bigint（整数除法）
        ObjBigInt* ba = get_bigint_operand(va);
        ObjBigInt* bb = get_bigint_operand(vb);
        Value result = bigint_div(ba, bb);
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_MOD_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    // 检查除零
    if (val_is_bigint(vb)) {
        ObjBigInt* bb = val_as_bigint(vb);
        if (bb->limb_count == 1 && bb->limbs[0] == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            DISPATCH;
        }
    } else if (val_is_int(vb) && val_as_int(vb) == 0) {
        throw_runtime_error("除零错误", GET_CURRENT_LINE());
        DISPATCH;
    }

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int a = val_as_int(va);
        int b = val_as_int(vb);
        vm_stack_push_fast(&vm, val_int(a % b));
    } else {
        // 任一操作数是 BigInt，使用 BigInt 运算
        ObjBigInt* ba = get_bigint_operand(va);
        ObjBigInt* bb = get_bigint_operand(vb);
        Value result = bigint_mod(ba, bb);
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_ADD_FLOAT) {
    double b = val_as_num_ex(vm_stack_pop_fast(&vm));
    double a = val_as_num_ex(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_float(a + b));
    DISPATCH;
}

OPCODE(OP_SUB_FLOAT) {
    double b = val_as_num_ex(vm_stack_pop_fast(&vm));
    double a = val_as_num_ex(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_float(a - b));
    DISPATCH;
}

OPCODE(OP_MUL_FLOAT) {
    double b = val_as_num_ex(vm_stack_pop_fast(&vm));
    double a = val_as_num_ex(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_float(a * b));
    DISPATCH;
}

OPCODE(OP_DIV_FLOAT) {
    double b = val_as_num_ex(vm_stack_pop_fast(&vm));
    double a = val_as_num_ex(vm_stack_pop_fast(&vm));
    if (b == 0.0) {
        throw_runtime_error("除零错误", GET_CURRENT_LINE());
        DISPATCH;
    }
    vm_stack_push_fast(&vm, val_float(a / b));
    DISPATCH;
}

OPCODE(OP_NEG_INT) {
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int a = val_as_int(va);
        if (unlikely(a == INT32_MIN)) {
            vm_stack_push_fast(&vm, val_bigint_from_int64(-(int64_t)a));
        } else {
            vm_stack_push_fast(&vm, val_int(-a));
        }
    } else {
        // 值是 BigInt，使用 BigInt 运算
        Value result = bigint_neg(val_as_bigint(va));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_NEG_FLOAT) {
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_float(-a));
    DISPATCH;
}

OPCODE(OP_EQ_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) == val_as_int(vb)));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_values(va, vb) == 0));
    }
    DISPATCH;
}

OPCODE(OP_LT_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) < val_as_int(vb)));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_values(va, vb) < 0));
    }
    DISPATCH;
}

OPCODE(OP_GT_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) > val_as_int(vb)));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_values(va, vb) > 0));
    }
    DISPATCH;
}

OPCODE(OP_LE_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) <= val_as_int(vb)));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_values(va, vb) <= 0));
    }
    DISPATCH;
}

OPCODE(OP_GE_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) >= val_as_int(vb)));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_values(va, vb) >= 0));
    }
    DISPATCH;
}

OPCODE(OP_EQ_FLOAT) {
    double b = val_as_num(vm_stack_pop_fast(&vm));
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_bool(a == b));
    DISPATCH;
}

OPCODE(OP_LT_FLOAT) {
    double b = val_as_num(vm_stack_pop_fast(&vm));
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_bool(a < b));
    DISPATCH;
}

OPCODE(OP_GT_FLOAT) {
    double b = val_as_num(vm_stack_pop_fast(&vm));
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_bool(a > b));
    DISPATCH;
}

OPCODE(OP_LE_FLOAT) {
    double b = val_as_num(vm_stack_pop_fast(&vm));
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_bool(a <= b));
    DISPATCH;
}

OPCODE(OP_GE_FLOAT) {
    double b = val_as_num(vm_stack_pop_fast(&vm));
    double a = val_as_num(vm_stack_pop_fast(&vm));
    vm_stack_push_fast(&vm, val_bool(a >= b));
    DISPATCH;
}

// ============================================================================
// 立即数操作码（避免加载小常量开销，imm 范围为 -128 到 127）
// ============================================================================

OPCODE(OP_ADD_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)imm;
        PUSH_SAFE_INT64_ADD_FAST(a, b);
    } else {
        ObjBigInt* bimm = bigint_from_int64(imm);
        Value result = bigint_add(val_as_bigint(va), bimm);
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_SUB_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)imm;
        PUSH_SAFE_INT64_SUB_FAST(a, b);
    } else {
        ObjBigInt* bimm = bigint_from_int64(imm);
        Value result = bigint_sub(val_as_bigint(va), bimm);
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_MUL_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int64_t a = (int64_t)val_as_int(va);
        int64_t b = (int64_t)imm;
        PUSH_SAFE_INT64_MUL_FAST(a, b);
    } else {
        ObjBigInt* bimm = bigint_from_int64(imm);
        Value result = bigint_mul(val_as_bigint(va), bimm);
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_LT_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) < imm));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_value_imm(va, imm) < 0));
    }
    DISPATCH;
}

OPCODE(OP_GT_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) > imm));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_value_imm(va, imm) > 0));
    }
    DISPATCH;
}

OPCODE(OP_LE_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) <= imm));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_value_imm(va, imm) <= 0));
    }
    DISPATCH;
}

OPCODE(OP_GE_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) >= imm));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_value_imm(va, imm) >= 0));
    }
    DISPATCH;
}

OPCODE(OP_EQ_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        vm_stack_push_fast(&vm, val_bool(val_as_int(va) == imm));
    } else {
        vm_stack_push_fast(&vm, val_bool(compare_value_imm(va, imm) == 0));
    }
    DISPATCH;
}
