// ============================================================================
// 通用算术运算类操作码 (OP_ARITH)
// ============================================================================
// 基本算术运算：加、减、乘、除、取模

OPCODE(OP_ADD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 快速路径：int + int（fib 热点，常见小整数情况）
    // 使用快速栈操作，因为编译器保证这里栈上有两个值
    Value b = vm_stack_pop_fast(&vm);
    Value a = vm_stack_pop_fast(&vm);

    if (likely(val_is_int(a) && val_is_int(b))) {
        int a_val = val_as_int(a);
        int b_val = val_as_int(b);

        // 优化：如果两个操作数都在安全范围内，直接计算
        // 安全范围：结果在 [INT32_MIN/2, INT32_MAX/2] 内肯定不会溢出
        if (likely(a_val > -1073741824 && a_val < 1073741824 &&
                   b_val > -1073741824 && b_val < 1073741824)) {
            vm_stack_push_fast(&vm, val_int(a_val + b_val));
            DISPATCH;
        }

        // 慢路径：需要检查溢出
        // 使用 int64_t 进行计算，避免溢出
        int64_t a_int64 = (int64_t)a_val;
        int64_t b_int64 = (int64_t)b_val;

        // 检查 int64 溢出
        if ((b_int64 > 0 && a_int64 > INT64_MAX - b_int64) ||
            (b_int64 < 0 && a_int64 < INT64_MIN - b_int64)) {
            // int64 溢出，使用 BigInt
            ObjBigInt* ba = bigint_from_int64(a_int64);
            ObjBigInt* bb = bigint_from_int64(b_int64);
            Value result = bigint_add(ba, bb);
            vm_stack_push(&vm, result);
        } else {
            int64_t result_int64 = a_int64 + b_int64;
            // 检查是否超出 int32 范围
            if (result_int64 < INT32_MIN || result_int64 > INT32_MAX) {
                // 结果超出 int32 范围，转换为 BigInt
                vm_stack_push(&vm, val_bigint_from_int64(result_int64));
            } else {
                vm_stack_push(&vm, val_int((int)result_int64));
            }
        }
        DISPATCH;
    }
    
    // 检查是否是字符串拼接（必须在 float 检查之前，因为 string + float 应该拼接而不是运算）
    if ((val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING) ||
        (val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING)) {
        // 字符串拼接：将非字符串转换为字符串
        char* strA = value_to_string(a);
        char* strB = value_to_string(b);
        int lenA = (int)strlen(strA);
        int lenB = (int)strlen(strB);
        char* buf = (char*)malloc(lenA + lenB + 1);
        memcpy(buf, strA, lenA);
        memcpy(buf + lenA, strB, lenB);
        buf[lenA + lenB] = '\0';
        ObjString* result = str_new(buf, lenA + lenB);
        free(buf);
        free(strA);
        free(strB);
        vm_stack_push(&vm, val_obj((Object*)result));
        DISPATCH;
    }

    // 快速路径：float + 任意数值（在字符串检查之后，确保 string + float 能正确拼接）
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) + val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }

    // BigInt 运算（罕见情况）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_add(ba, bb);
        vm_stack_push(&vm, result);
        DISPATCH;
    }
    
    // null 不能参与算术运算
    if (val_is_null(a) || val_is_null(b)) {
        throw_runtime_error("null 不能参与算术运算", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // 其他类型，尝试转换为字符串后拼接
    char* strA = value_to_string(a);
    char* strB = value_to_string(b);
    int lenA = (int)strlen(strA);
    int lenB = (int)strlen(strB);
    char* buf = (char*)malloc(lenA + lenB + 1);
    memcpy(buf, strA, lenA);
    memcpy(buf + lenA, strB, lenB);
    buf[lenA + lenB] = '\0';
    ObjString* result = str_new(buf, lenA + lenB);
    free(buf);
    free(strA);
    free(strB);
    vm_stack_push(&vm, val_obj((Object*)result));
    DISPATCH;
}

OPCODE(OP_SUB) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 快速路径：int - int（常见情况：斐波那契 n-1, n-2 等）
    // 使用快速栈操作，因为编译器保证这里栈上有两个值
    Value b = vm_stack_pop_fast(&vm);
    Value a = vm_stack_pop_fast(&vm);

    if (likely(val_is_int(a) && val_is_int(b))) {
        int a_val = val_as_int(a);
        int b_val = val_as_int(b);

        // 优化：如果两个操作数都在安全范围内，直接计算
        // 安全范围：结果在 [INT32_MIN/2, INT32_MAX/2] 内肯定不会溢出
        if (likely(a_val > -1073741824 && a_val < 1073741824 &&
                   b_val > -1073741824 && b_val < 1073741824)) {
            vm_stack_push_fast(&vm, val_int(a_val - b_val));
            DISPATCH;
        }

        // 慢路径：需要检查溢出
        int64_t a_int = (int64_t)a_val;
        int64_t b_int = (int64_t)b_val;
        int64_t result_int = a_int - b_int;

        // 检查溢出
        if ((a_int > 0 && b_int < 0 && result_int < 0) ||
            (a_int < 0 && b_int > 0 && result_int > 0)) {
            vm_stack_push(&vm, val_bigint_from_int64(result_int));
        } else if (result_int < INT32_MIN || result_int > INT32_MAX) {
            vm_stack_push(&vm, val_bigint_from_int64(result_int));
        } else {
            vm_stack_push(&vm, val_int((int)result_int));
        }
        DISPATCH;
    }

    // 快速路径：float - 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) - val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }

    // null 检查
    if (check_null_binary(a, b, "减法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 运算或其他数字类型（罕见情况）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_sub(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error("操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
    }
    DISPATCH;
}

OPCODE(OP_MUL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int * int
    if (val_is_int(a) && val_is_int(b)) {
        int64_t a_int = (int64_t)val_as_int(a);
        int64_t b_int = (int64_t)val_as_int(b);
        
        if (a_int != 0 && b_int != 0) {
            int64_t result_int = a_int * b_int;
            if (result_int / a_int != b_int) {
                vm_stack_push(&vm, val_bigint_from_int64(result_int));
            } else if (result_int < INT32_MIN || result_int > INT32_MAX) {
                vm_stack_push(&vm, val_bigint_from_int64(result_int));
            } else {
                vm_stack_push(&vm, val_int((int)result_int));
            }
        } else {
            vm_stack_push(&vm, val_int(0));
        }
        DISPATCH;
    }
    
    // 快速路径：float * 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) * val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }
    
    // null 检查
    if (check_null_binary(a, b, "乘法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 运算或其他数字类型（罕见情况）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_mul(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error("操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
    }
    DISPATCH;
}

OPCODE(OP_DIV) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    
    // 快速路径：int / int（返回 int，整数除法）
    if (val_is_int(a) && val_is_int(b)) {
        int b_val = val_as_int(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        int result = val_as_int(a) / b_val;
        vm_stack_push(&vm, val_int(result));
        DISPATCH;
    }
    
    // 快速路径：float / 任意数值（返回 float）
    if (val_is_float(a) || val_is_float(b)) {
        double b_val = val_as_num_ex(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        double result = val_as_num_ex(a) / b_val;
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }
    
    // BigInt / BigInt（返回 int/bigint，整数除法）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        // 检查除零
        if (val_is_bigint(b)) {
            ObjBigInt* bb = val_as_bigint(b);
            if (bb->limb_count == 1 && bb->limbs[0] == 0) {
                throw_runtime_error("除零错误", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
        } else if (val_as_num(b) == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        
        // 都转换为 BigInt 进行整数除法
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_div(ba, bb);  // 整数除法
        vm_stack_push(&vm, result);
        DISPATCH;
    }
    
    // null 检查
    if (check_null_binary(a, b, "除法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    throw_runtime_error("操作数必须是数字", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_MOD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    
    // 快速路径：int % int
    if (val_is_int(a) && val_is_int(b)) {
        int b_val = val_as_int(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        vm_stack_push(&vm, val_int(val_as_int(a) % b_val));
        DISPATCH;
    }
    
    // null 检查
    if (check_null_binary(a, b, "取模", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 取模（罕见情况）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        // 检查除零
        if (bb->limb_count == 1 && bb->limbs[0] == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        Value result = bigint_mod(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error("取模操作数必须是整数", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}
