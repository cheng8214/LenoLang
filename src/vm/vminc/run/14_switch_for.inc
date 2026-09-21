    LABEL(OP_SWITCH_LOOKUP): DISPATCH();

    // --- for 数值循环（8 字节定长网格；与栈式 op_for_loop.inc 语义一致）---
    // OP_FOR_PREP: op + A(start_slot) + B(end_slot) + C(step_slot)
    //              + [var_slot:u8][inclusive:u8][sBx:u16]           （共 8 字节）
    //   局部变量槽位号即寄存器号（每个 slot 独立，不要求连续）
    LABEL(OP_FOR_PREP): {
        int start_slot = READ_A();
        int end_slot = READ_B();
        int step_slot = READ_C();
        int var_slot = frame->ip[0];
        int inclusive = frame->ip[1];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        if (unlikely(start_slot >= frame->local_count || end_slot >= frame->local_count ||
                     step_slot >= frame->local_count || var_slot >= frame->local_count ||
                     start_slot < 0 || end_slot < 0 || step_slot < 0 || var_slot < 0)) {
            if (!throw_runtime_error("for 循环槽位越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        Value start_val = frame->locals[start_slot];
        Value end_val = frame->locals[end_slot];
        Value step_val = frame->locals[step_slot];
        frame->locals[var_slot] = start_val;

        double start_num = value_to_double(start_val);
        double end_num = value_to_double(end_val);
        double step_num = value_to_double(step_val);

        int should_run = 0;
        if (step_num > 0) {
            should_run = inclusive ? (start_num <= end_num) : (start_num < end_num);
        } else if (step_num < 0) {
            should_run = inclusive ? (start_num >= end_num) : (start_num > end_num);
        }
        // step == 0 时不执行

        if (!should_run) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    // OP_FOR_LOOP: op + A(var_slot) + B(step_slot) + C(end_slot)
    //              + [inclusive:u8][pad:u8][sBx:u16]                （共 8 字节）
    LABEL(OP_FOR_LOOP): {
        int var_slot = READ_A();
        int step_slot = READ_B();
        int end_slot = READ_C();
        int inclusive = frame->ip[0];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        if (unlikely(var_slot >= frame->local_count || step_slot >= frame->local_count ||
                     end_slot >= frame->local_count ||
                     var_slot < 0 || step_slot < 0 || end_slot < 0)) {
            if (!throw_runtime_error("for 循环槽位越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        /* 回边是天然 GC 安全点 */
        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        }

        Value loop_var = frame->locals[var_slot];
        Value step_val = frame->locals[step_slot];
        Value end_val = frame->locals[end_slot];

        if (likely(val_is_int(loop_var) && val_is_int(step_val) && val_is_int(end_val))) {
            int64_t loop_num = val_as_int(loop_var) + val_as_int(step_val);
            int64_t step_num = val_as_int(step_val);
            int64_t end_num = val_as_int(end_val);
            frame->locals[var_slot] = (loop_num >= INT48_MIN && loop_num <= INT48_MAX)
                                          ? val_int(loop_num)
                                          : val_obj((Object*)bigint_from_int64(loop_num));
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        } else if (unlikely(val_is_bigint(loop_var) || val_is_bigint(step_val) || val_is_bigint(end_val))) {
            ObjBigInt* lb = get_bigint_operand(loop_var);
            ObjBigInt* sb = get_bigint_operand(step_val);
            ObjBigInt* eb = get_bigint_operand(end_val);
            Value nv = bigint_add(lb, sb);
            frame->locals[var_slot] = nv;
            int cmp = bigint_compare(get_bigint_operand(nv), eb);
            int step_cmp = bigint_compare(sb, bigint_from_int64(0));
            int cont = 0;
            if (step_cmp > 0) cont = inclusive ? (cmp <= 0) : (cmp < 0);
            else if (step_cmp < 0) cont = inclusive ? (cmp >= 0) : (cmp > 0);
            if (cont) frame->ip += sbx;
        } else {
            double loop_num = value_to_double(loop_var) + value_to_double(step_val);
            double step_num = value_to_double(step_val);
            double end_num = value_to_double(end_val);
            frame->locals[var_slot] = val_num(loop_num);
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        }
        DISPATCH();
    }

    // --- 扩展指令 ---
    LABEL(OP_EXTRAARG): DISPATCH();
    LABEL(OP_EXTEND): DISPATCH();

    #if !VM_USE_JUMPTABLE
                default:
                    // 未知 opcode
                    goto vm_exit;
            } // switch
        } // for
    #endif

vm_exit:
    #undef R
    #undef READ_OPCODE
    #undef READ_A
    #undef READ_B
    #undef READ_C
    #undef READ_Bx
    #undef READ_sBx
    #undef READ_sJ
    #undef READ_Ax
    #undef GET_CURRENT_LINE
    #undef ADVANCE
    #undef DISPATCH
    #undef LABEL
    #undef REG_FINISH_RETURN
    #undef READ_CONST_IDX
    return 0;
