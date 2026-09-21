    LABEL(OP_JMP): {
        int sj = READ_sJ();
        frame->ip += sj;  // ip 已经 ADVANCE 了 4 字节，sj 是相对于下一条指令的偏移
        DISPATCH();
    }
    LABEL(OP_JMP_IF_FALSE): {
        int a = READ_A();
        int sbx = READ_sBx();
        if (!val_is_truthy(R(a))) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    LABEL(OP_JMP_IF_TRUE): {
        int a = READ_A();
        int sbx = READ_sBx();
        if (val_is_truthy(R(a))) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    LABEL(OP_TEST): {
        int b = READ_B(), c = READ_C();
        if (c == 0) { // not
            if (!val_is_truthy(R(b))) frame->ip += 4;  // 跳过下一条
        }
        DISPATCH();
    }

    // --- 自增自减（保持操作数类型；int 溢出/超 int48 自动提升 bigint）---
    LABEL(OP_INC): {
        int a = READ_A(), b = READ_B();
        int err = 0;
        Value r = do_inc_dec(R(b), 1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自增 (++): 操作数必须是数字", "INC (++)", R(b), GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }
    LABEL(OP_DEC): {
        int a = READ_A(), b = READ_B();
        int err = 0;
        Value r = do_inc_dec(R(b), -1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自减 (--): 操作数必须是数字", "DEC (--)", R(b), GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }

    // --- 调用 / 返回 ---
    // OP_CALL A B C: R[A] = callee, R[A+1 .. A+B-1] = 实参, B = nargs+1, C = nresults+1
