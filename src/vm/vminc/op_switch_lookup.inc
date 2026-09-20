// ============================================================================
// Switch 查找操作码 (OP_SWITCH_LOOKUP)
// 通用二分查找优化：支持 int、float、string case 值
// 编码: const_idx(2) case_count(2) default_offset(4) [body_offset(4)]...
// 常量表存储排序后的 Value 数组（按类型分组排序）
// 栈: [switch_value] -> [] (弹出 switch 值，跳转到匹配的 body 或 default)
// ============================================================================

OPCODE(OP_SWITCH_LOOKUP) {
    uint16_t const_idx = READ_SHORT();
    uint16_t case_count = READ_SHORT();
    int32_t default_offset = READ_INT32();
    // frame->ip 现在指向 offset 表的第一个 entry

    Value switch_val = vm_stack_pop(&vm);

    /* 查找逻辑抽到 switch_lookup_index（vm.c）—— **语义唯一来源**：
     * 别在本地再写一份二分查找（历史上两份实现漂移过，§8.61）。 */
    int matched_index = switch_lookup_index(switch_val, frame->chunk->constants[const_idx],
                                            (int)case_count);

    // 读取匹配的 offset，或使用 default
    if (matched_index >= 0) {
        uint8_t* offset_ptr = frame->ip + matched_index * 4;
        int32_t offset = (int32_t)((offset_ptr[0] << 24) | (offset_ptr[1] << 16) | (offset_ptr[2] << 8) | offset_ptr[3]);
        frame->ip += case_count * 4;  // 跳过 offset 表
        frame->ip += offset;
    } else {
        frame->ip += case_count * 4;  // 跳过 offset 表
        frame->ip += default_offset;
    }

    DISPATCH;
}
