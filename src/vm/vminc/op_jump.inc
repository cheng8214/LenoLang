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
    /* 回边 GC 安全点（§8.37）：JIT 热循环跨过年轻代阈值时会置
     * vm.gc_force_request 并请求 bailout 回到这里 —— 回收必须发生在解释器
     * 里，因为 JIT 的活值在它自己的机器栈帧里、GC 看不见（§8.36）。
     * 纯解释执行的循环顺带受益：回边从此也是安全点。
     * 只读 VM 结构体字段，不涉及 THREAD_LOCAL 访问。 */
    if (unlikely(vm.gc_force_request)) {
        vm.gc_force_request = 0;
        gc_force_collect();
    }
    int32_t offset = READ_INT32();

    /* JIT: try to run a compiled hot loop. */
    int jit_r = jit_try_hot_loop(frame, &vm, offset, 1);
    if (jit_r == 1) {
        /* Reload frame — callout may have triggered vm_grow_frames */
        frame = &vm.frames[vm.frame_cnt - 1];
        /* JIT ran the loop to completion (condition became false).
         * The while loop's exit path has OP_POP to pop the condition
         * value that JUMP_IF_FALSE left on the stack (peek semantics).
         * But JIT handled the condition internally without touching
         * the VM stack — there is no condition value to pop.
         * Skip the exit OP_POP to avoid stack underflow. */
        if (*frame->ip == OP_POP) {
            frame->ip++;  /* skip 1-byte OP_POP */
        }
        DISPATCH;
    }
    if (jit_r == 2) {
        /* JIT 循环内函数调用抛出异常，控制流已转移到新的当前帧
         * （宿主帧 catch_ip 或外层 handler 帧）：重载 frame 后直接继续 */
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

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

    int64_t a, b;
    if (frame->locals != NULL) {
        a = val_as_int(frame->locals[slot_a]);
        b = val_as_int(frame->locals[slot_b]);
    } else {
        a = val_as_int(vm.stack[frame->stack_base + slot_a]);
        b = val_as_int(vm.stack[frame->stack_base + slot_b]);
    }

    int result = 0;
    switch (cmp_op) {
        case 0: result = (a == b); break;  // EQ
        case 1: result = (a != b); break;  // NE
        case 2: result = (a <  b); break;  // LT
        case 3: result = (a >  b); break;  // GT
        case 4: result = (a <= b); break;  // LE
        case 5: result = (a >= b); break;  // GE
        default: result = 0; break;
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

    int64_t a;
    if (frame->locals != NULL) {
        a = val_as_int(frame->locals[slot]);
    } else {
        a = val_as_int(vm.stack[frame->stack_base + slot]);
    }
    int64_t b = val_as_int(vm.globals[global_idx]);

    int result = 0;
    switch (cmp_op) {
        case 0: result = (a == b); break;  // EQ
        case 1: result = (a != b); break;  // NE
        case 2: result = (a <  b); break;  // LT
        case 3: result = (a >  b); break;  // GT
        case 4: result = (a <= b); break;  // LE
        case 5: result = (a >= b); break;  // GE
        default: result = 0; break;
    }

    if (!result) {
        frame->ip += offset;
    }
    DISPATCH;
}
