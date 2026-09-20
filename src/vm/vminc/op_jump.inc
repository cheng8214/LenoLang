// ============================================================================
// 跳转控制类操作码 (OP_JUMP)
// ============================================================================

OPCODE(OP_JUMP) {
    int32_t offset = READ_INT32();
    frame->ip += offset;
    DISPATCH;
}

OPCODE(OP_JUMP_IF_FALSE) {
    int32_t offset = READ_INT32();
    if (is_falsey(vm_stack_peek_fast(&vm, 0))) {
        frame->ip += offset;
    }
    DISPATCH;
}

OPCODE(OP_JUMP_IF_TRUE) {
    int32_t offset = READ_INT32();
    if (!is_falsey(vm_stack_peek_fast(&vm, 0))) {
        frame->ip += offset;
    }
    DISPATCH;
}

OPCODE(OP_LOOP) {
    /* 回边 GC 安全点（§8.37）：回边是天然的回收点 —— 回收只发生在解释器里
     * （GC 的根集合只看 vm.stack 与各帧 locals）。
     * 只读 VM 结构体字段，不涉及 THREAD_LOCAL 访问。 */
    if (unlikely(vm.gc_force_request)) {
        vm.gc_force_request = 0;
        gc_force_collect();
    }
    int32_t offset = READ_INT32();

    frame->ip -= offset;
    DISPATCH;
}

// ============================================================================
// 融合指令：比较 + 条件跳转（合并 GET_LOCAL + GET_LOCAL/GLOBAL + CMP + JUMP_IF_FALSE + POP）
// cmp_op: 0=EQ, 1=NE, 2=LT, 3=GT, 4=LE, 5=GE
// 语义：比较结果为 false 时跳转（与 JUMP_IF_FALSE 一致），完全不碰栈
// ============================================================================

OPCODE(OP_CMPJMP_LL_INT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t cmp_op = READ_BYTE();
    uint16_t slot_a = READ_SHORT();
    uint16_t slot_b = READ_SHORT();
    int32_t offset = READ_INT32();

    Value va, vb;
    if (frame->locals != NULL) {
        va = frame->locals[slot_a];
        vb = frame->locals[slot_b];
    } else {
        va = vm.stack[frame->stack_base + slot_a];
        vb = vm.stack[frame->stack_base + slot_b];
    }

    int result = 0;
    /* §8.125：**bigint 的静态类型同样是 int** ✗ ⇒ 不能盲用 `val_as_int` 比整数 ✗
     * —— 那会把两个"值相等但对象不同"的 bigint 按**位型/指针**比成"不相等" ✗
     * （实测：`if got != want` 恒真、而同一次迭代里 `got == want` 恒真 ✗，
     *   即 `==` 与 `!=` 在同一表达式上给出相反结论 ✓）。
     * int48 走原快路径 ✓；其余（bigint / float / …）交 `compare_values` ✓
     * —— 与解释器 OP_EQ/OP_NE/OP_LT… 的数值语义一致 ✓。 */
    if (val_is_int(va) && val_is_int(vb)) {
        int64_t a = val_as_int(va), b = val_as_int(vb);
        switch (cmp_op) {
            case 0: result = (a == b); break;  // EQ
            case 1: result = (a != b); break;  // NE
            case 2: result = (a <  b); break;  // LT
            case 3: result = (a >  b); break;  // GT
            case 4: result = (a <= b); break;  // LE
            case 5: result = (a >= b); break;  // GE
            default: result = 0; break;
        }
    } else {
        int c = compare_values(va, vb);
        switch (cmp_op) {
            case 0: result = (c == 0); break;
            case 1: result = (c != 0); break;
            case 2: result = (c <  0); break;
            case 3: result = (c >  0); break;
            case 4: result = (c <= 0); break;
            case 5: result = (c >= 0); break;
            default: result = 0; break;
        }
    }

    if (!result) {
        frame->ip += offset;
    }
    DISPATCH;
}

OPCODE(OP_CMPJMP_LG_INT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t cmp_op = READ_BYTE();
    uint16_t slot = READ_SHORT();
    uint16_t global_idx = READ_SHORT();
    int32_t offset = READ_INT32();

    Value va;
    if (frame->locals != NULL) {
        va = frame->locals[slot];
    } else {
        va = vm.stack[frame->stack_base + slot];
    }
    Value vb = vm.globals[global_idx];

    int result = 0;
    /* §8.125：同上 —— bigint 也静态是 int ⇒ 不能盲比 ✗ */
    if (val_is_int(va) && val_is_int(vb)) {
        int64_t a = val_as_int(va), b = val_as_int(vb);
        switch (cmp_op) {
            case 0: result = (a == b); break;  // EQ
            case 1: result = (a != b); break;  // NE
            case 2: result = (a <  b); break;  // LT
            case 3: result = (a >  b); break;  // GT
            case 4: result = (a <= b); break;  // LE
            case 5: result = (a >= b); break;  // GE
            default: result = 0; break;
        }
    } else {
        int c = compare_values(va, vb);
        switch (cmp_op) {
            case 0: result = (c == 0); break;
            case 1: result = (c != 0); break;
            case 2: result = (c <  0); break;
            case 3: result = (c >  0); break;
            case 4: result = (c <= 0); break;
            case 5: result = (c >= 0); break;
            default: result = 0; break;
        }
    }

    if (!result) {
        frame->ip += offset;
    }
    DISPATCH;
}

// local int 与**立即数**比较 + 条件跳转（补上 LL/LG 缺的那一格）
// 操作数: cmp_op(1) slot(2) imm32(4) offset(4)
OPCODE(OP_CMPJMP_LI_INT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t cmp_op = READ_BYTE();
    uint16_t slot = READ_SHORT();
    int32_t imm = READ_INT32();
    int32_t offset = READ_INT32();

    Value va;
    if (frame->locals != NULL) {
        va = frame->locals[slot];
    } else {
        va = vm.stack[frame->stack_base + slot];
    }
    Value vb = val_int((int64_t)imm);

    int result = 0;
    /* §8.125：同上（立即数是 int ✓，但 local 可能是 bigint ✗） */
    if (val_is_int(va) && val_is_int(vb)) {
        int64_t a = val_as_int(va), b = val_as_int(vb);
        switch (cmp_op) {
            case 0: result = (a == b); break;  // EQ
            case 1: result = (a != b); break;  // NE
            case 2: result = (a <  b); break;  // LT
            case 3: result = (a >  b); break;  // GT
            case 4: result = (a <= b); break;  // LE
            case 5: result = (a >= b); break;  // GE
            default: result = 0; break;
        }
    } else {
        int c = compare_values(va, vb);
        switch (cmp_op) {
            case 0: result = (c == 0); break;
            case 1: result = (c != 0); break;
            case 2: result = (c <  0); break;
            case 3: result = (c >  0); break;
            case 4: result = (c <= 0); break;
            case 5: result = (c >= 0); break;
            default: result = 0; break;
        }
    }

    if (!result) {
        frame->ip += offset;
    }
    DISPATCH;
}
