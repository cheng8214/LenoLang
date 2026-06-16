// ============================================================================
// 类型特化指令（编译器已知类型时的快速路径）
// ============================================================================
// int 为 48 位内联表示。运算结果超出 int48 范围时自动提升为 BigInt。

OPCODE(OP_ADD_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    // 快速路径：两个 int 相加
    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = val_as_int(va);
        int64_t b = val_as_int(vb);
        // 检测 int64 溢出
        if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
            ObjBigInt* ba = bigint_from_int64(a);
            ObjBigInt* bb = bigint_from_int64(b);
            Value result = bigint_add(ba, bb);
            vm_stack_push_fast(&vm, result);
        } else {
            int64_t sum = a + b;
            // 检测 int48 溢出，超出则提升为 BigInt
            if (sum < INT48_MIN || sum > INT48_MAX) {
                vm_stack_push_fast(&vm, val_bigint_from_int64(sum));
            } else {
                vm_stack_push_fast(&vm, val_int(sum));
            }
        }
    } else {
        // BigInt 运算（显式 Bint 类型）
        Value result = bigint_add(get_bigint_operand(va), get_bigint_operand(vb));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_SUB_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = val_as_int(va);
        int64_t b = val_as_int(vb);
        // 检测 int64 溢出
        if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) {
            ObjBigInt* ba = bigint_from_int64(a);
            ObjBigInt* bb = bigint_from_int64(b);
            Value result = bigint_sub(ba, bb);
            vm_stack_push_fast(&vm, result);
        } else {
            int64_t diff = a - b;
            // 检测 int48 溢出，超出则提升为 BigInt
            if (diff < INT48_MIN || diff > INT48_MAX) {
                vm_stack_push_fast(&vm, val_bigint_from_int64(diff));
            } else {
                vm_stack_push_fast(&vm, val_int(diff));
            }
        }
    } else {
        Value result = bigint_sub(get_bigint_operand(va), get_bigint_operand(vb));
        vm_stack_push_fast(&vm, result);
    }
    DISPATCH;
}

OPCODE(OP_MUL_INT) {
    Value vb = vm_stack_pop_fast(&vm);
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = val_as_int(va);
        int64_t b = val_as_int(vb);
        // 检测 int64 溢出（通过除法回验）
        if (a == 0 || b == 0) {
            vm_stack_push_fast(&vm, val_int(0));
        } else {
            int64_t result = a * b;
            if (result / a != b) {
                // int64 溢出，使用 BigInt
                ObjBigInt* ba = bigint_from_int64(a);
                ObjBigInt* bb = bigint_from_int64(b);
                Value r = bigint_mul(ba, bb);
                vm_stack_push_fast(&vm, r);
            } else if (result < INT48_MIN || result > INT48_MAX) {
                // int48 溢出，提升为 BigInt
                vm_stack_push_fast(&vm, val_bigint_from_int64(result));
            } else {
                vm_stack_push_fast(&vm, val_int(result));
            }
        }
    } else {
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
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
    } else if (val_is_int(vb) && val_as_int(vb) == 0) {
        throw_runtime_error("除零错误", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = val_as_int(va);
        int64_t b = val_as_int(vb);
        vm_stack_push_fast(&vm, val_int(a / b));
    } else {
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

    if (val_is_bigint(vb)) {
        ObjBigInt* bb = val_as_bigint(vb);
        if (bb->limb_count == 1 && bb->limbs[0] == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
    } else if (val_is_int(vb) && val_as_int(vb) == 0) {
        throw_runtime_error("除零错误", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    if (likely(!val_is_bigint(va) && !val_is_bigint(vb))) {
        int64_t a = val_as_int(va);
        int64_t b = val_as_int(vb);
        vm_stack_push_fast(&vm, val_int(a % b));
    } else {
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
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    vm_stack_push_fast(&vm, val_float(a / b));
    DISPATCH;
}

OPCODE(OP_NEG_INT) {
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int64_t a = val_as_int(va);
        // 检测 INT48_MIN 取负会超出 int48 范围
        if (a == INT48_MIN) {
            vm_stack_push_fast(&vm, val_bigint_from_int64(-(int64_t)INT48_MIN));
        } else {
            vm_stack_push_fast(&vm, val_int(-a));
        }
    } else {
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
// 立即数操作码（imm 范围为 -128 到 127）
// ============================================================================

OPCODE(OP_ADD_INT_IMM) {
    int8_t imm = READ_BYTE();
    Value va = vm_stack_pop_fast(&vm);

    if (likely(!val_is_bigint(va))) {
        int64_t a = val_as_int(va);
        // imm 在 [-128, 127] 范围内，不会导致 int64 溢出
        int64_t sum = a + imm;
        if (sum < INT48_MIN || sum > INT48_MAX) {
            vm_stack_push_fast(&vm, val_bigint_from_int64(sum));
        } else {
            vm_stack_push_fast(&vm, val_int(sum));
        }
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
        int64_t a = val_as_int(va);
        // imm 在 [-128, 127] 范围内，不会导致 int64 溢出
        int64_t diff = a - imm;
        if (diff < INT48_MIN || diff > INT48_MAX) {
            vm_stack_push_fast(&vm, val_bigint_from_int64(diff));
        } else {
            vm_stack_push_fast(&vm, val_int(diff));
        }
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
        int64_t a = val_as_int(va);
        if (a == 0 || imm == 0) {
            vm_stack_push_fast(&vm, val_int(0));
        } else {
            int64_t result = a * imm;
            // 通过除法回验检测 int64 溢出
            if (result / a != imm) {
                ObjBigInt* ba = bigint_from_int64(a);
                ObjBigInt* bimm = bigint_from_int64(imm);
                Value r = bigint_mul(ba, bimm);
                vm_stack_push_fast(&vm, r);
            } else if (result < INT48_MIN || result > INT48_MAX) {
                vm_stack_push_fast(&vm, val_bigint_from_int64(result));
            } else {
                vm_stack_push_fast(&vm, val_int(result));
            }
        }
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
