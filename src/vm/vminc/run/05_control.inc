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
    // ★ int 快路径（T10 后续）：`i++` 是最高频语句之一，而 do_inc_dec() 是**跨 TU 调用 +
    //   逐类型派发** ⇒ 实测 `i++` 20.8ns **反而比 `i = i + 1`（15.6ns）更慢**（栈式两者
    //   都是 20ns），这是纯亏。这里对"操作数是普通 int48"（最高频形态）直接内联并复用
    //   OP_ADD_INT 的溢出/提升口径（VM_INT_RESULT：超 int48 ⇒ 提升 bigint）；
    //   bigint / float / 非法类型仍原样走 do_inc_dec，语义不变。
    //   ⚠ val_is_int 对 bigint 对象为假（tag 不同）⇒ 不会把 bigint 当 int48 处理。
    LABEL(OP_INC): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (likely(val_is_int(v))) {
            int64_t x = val_as_int(v), s = 0;
            if (likely(!__builtin_add_overflow(x, 1, &s))) {
                R(a) = VM_INT_RESULT(s);
                DISPATCH();
            }
        }
        int err = 0;
        Value r = do_inc_dec(v, 1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自增 (++): 操作数必须是数字", "INC (++)", v, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }
    LABEL(OP_DEC): {
        int a = READ_A(), b = READ_B();
        Value v = R(b);
        if (likely(val_is_int(v))) {
            int64_t x = val_as_int(v), s = 0;
            if (likely(!__builtin_sub_overflow(x, 1, &s))) {
                R(a) = VM_INT_RESULT(s);
                DISPATCH();
            }
        }
        int err = 0;
        Value r = do_inc_dec(v, -1, &err);
        if (unlikely(err)) {
            if (!throw_runtime_error_with_operand("自减 (--): 操作数必须是数字", "DEC (--)", v, GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = r;
        DISPATCH();
    }

    // --- 调用 / 返回 ---
    // OP_CALL A B C: R[A] = callee, R[A+1 .. A+B-1] = 实参, B = nargs+1, C = nresults+1
