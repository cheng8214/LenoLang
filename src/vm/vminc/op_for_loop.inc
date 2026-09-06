// ============================================================================
// For 循环操作类操作码 (OP_FOR_LOOP)
// ============================================================================

OPCODE(OP_FOR_PREP) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // For循环准备: 初始化循环变量并检查初始条件，如果不满足则跳转到循环后
    // 操作数: start_slot(1), end_slot(1), step_slot(1), loop_var_slot(1), inclusive(1), jump_offset(2)
    uint8_t start_slot = READ_BYTE();
    uint8_t end_slot = READ_BYTE();
    uint8_t step_slot = READ_BYTE();
    uint8_t loop_var_slot = READ_BYTE();
    uint8_t inclusive = READ_BYTE();
    uint16_t jump_offset = READ_SHORT();
    
    // 获取 start, end, step 值
    Value start_val = frame->locals[start_slot];
    Value end_val = frame->locals[end_slot];
    Value step_val = frame->locals[step_slot];
    
    // 初始化 loop_var = start
    frame->locals[loop_var_slot] = start_val;
    
    // 检查是否需要执行循环（初始条件）- 支持 int/float/bigint
    double start_num = value_to_double(start_val);
    double end_num = value_to_double(end_val);
    double step_num = value_to_double(step_val);
    
    int should_run = 0;
    if (step_num > 0) {
        // 正向循环
        should_run = inclusive ? (start_num <= end_num) : (start_num < end_num);
    } else if (step_num < 0) {
        // 反向循环
        should_run = inclusive ? (start_num >= end_num) : (start_num > end_num);
    }
    // step == 0 时不执行循环
    
    if (!should_run) {
        // 跳过循环体（包括后面的 OP_FOR_LOOP 指令）
        frame->ip += jump_offset;
    }
    DISPATCH;
}

OPCODE(OP_FOR_LOOP) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // For循环迭代: 更新 loop_var 并检查是否继续，如果继续则跳转回循环开始
    // 操作数: loop_var_slot(1), step_slot(1), end_slot(1), inclusive(1), jump_offset(2)
    uint8_t loop_var_slot = READ_BYTE();
    uint8_t step_slot = READ_BYTE();
    uint8_t end_slot = READ_BYTE();
    uint8_t inclusive = READ_BYTE();
    uint16_t jump_offset = READ_SHORT();

    /* JIT: try to run a compiled hot loop */
    if (jit_try_hot_loop(frame, &vm, (int32_t)jump_offset, 2)) {
        DISPATCH;
    }

    // 获取当前值
    Value loop_var = frame->locals[loop_var_slot];
    Value step_val = frame->locals[step_slot];
    Value end_val = frame->locals[end_slot];

    // 优化：如果 step 和 loop_var 都是普通整数（不是 BigInt），使用整数运算
    // 注意：必须用 int64_t（int48 容器），不能用 int32——否则 i=2147483647 再 +1
    // 会溢出为 -2147483648，导致 i<=end 永真，陷入约 40 亿次死循环（内存爆炸）
    if (val_is_int(loop_var) && val_is_int(step_val) && val_is_int(end_val)) {
        int64_t loop_num = val_as_int(loop_var);
        int64_t step_num = val_as_int(step_val);
        int64_t end_num = val_as_int(end_val);

        // 更新 loop_var += step
        loop_num += step_num;
        // 若增量后超出 int48 范围，转为 bigint 存储（下一次迭代自动走 bigint 路径）
        if (loop_num >= INT48_MIN && loop_num <= INT48_MAX) {
            frame->locals[loop_var_slot] = val_int(loop_num);
        } else {
            frame->locals[loop_var_slot] = val_obj((Object*)bigint_from_int64(loop_num));
        }

        // 检查是否继续循环
        int should_continue = 0;
        if (step_num > 0) {
            should_continue = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
        } else if (step_num < 0) {
            should_continue = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
        }

        if (should_continue) {
            frame->ip -= jump_offset;
        }
    } else if (val_is_bigint(loop_var) || val_is_bigint(step_val) || val_is_bigint(end_val)) {
        // BigInt 循环 - 使用 BigInt 运算
        ObjBigInt* loop_big = get_bigint_operand(loop_var);
        ObjBigInt* step_big = get_bigint_operand(step_val);
        ObjBigInt* end_big = get_bigint_operand(end_val);
        
        // 更新 loop_var += step
        Value new_loop = bigint_add(loop_big, step_big);
        frame->locals[loop_var_slot] = new_loop;
        
        // 检查是否继续循环
        ObjBigInt* new_loop_big = get_bigint_operand(new_loop);
        int cmp = bigint_compare(new_loop_big, end_big);
        int should_continue = 0;
        int step_cmp = bigint_compare(step_big, bigint_from_int64(0));
        
        if (step_cmp > 0) {
            // 正向循环
            should_continue = inclusive ? (cmp <= 0) : (cmp < 0);
        } else if (step_cmp < 0) {
            // 反向循环
            should_continue = inclusive ? (cmp >= 0) : (cmp > 0);
        }

        if (should_continue) {
            frame->ip -= jump_offset;
        }
    } else {
        // 浮点数或混合类型循环
        double loop_num = value_to_double(loop_var);
        double step_num = value_to_double(step_val);
        double end_num = value_to_double(end_val);

        // 更新 loop_var += step
        loop_num += step_num;
        frame->locals[loop_var_slot] = val_num(loop_num);

        // 检查是否继续循环
        int should_continue = 0;
        if (step_num > 0) {
            should_continue = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
        } else if (step_num < 0) {
            should_continue = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
        }

        if (should_continue) {
            frame->ip -= jump_offset;
        }
    }
    DISPATCH;
}
