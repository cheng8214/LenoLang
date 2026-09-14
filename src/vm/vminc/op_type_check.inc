// ============================================================================
// 类型检查操作码 (OP_TYPE_CHECK)
//
// 字节码格式（非名字类型）:
//   [OP_TYPE_CHECK] [type_kind:1] [element_type_kind:1]           → 3 字节
// 字节码格式（struct / face / enum —— 带名字常量）:
//   [OP_TYPE_CHECK] [type_kind:1] [name_const_idx:2]              → 4 字节
//   **TYPE_STRUCT / TYPE_FACE / TYPE_ENUM 三个都读 2 字节名字常量**
//
// 执行逻辑: 弹出栈顶值 → 判定是否符合期望类型 → 压入布尔结果（pop 1 push 1）
//
// 判定逻辑抽到 type_check_value（vm.c）—— **语义唯一来源**：
//   JIT 的 callout（jit_callout_type_check）调的是同一个函数，§8.65。
// ============================================================================

OPCODE(OP_TYPE_CHECK) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t type_kind_byte = READ_BYTE();
    TypeKind expected_type = (TypeKind)type_kind_byte;

    Value value = vm_stack_pop(&vm);

    /* 操作数：struct/face/enum 是"名字常量(2)"，其余是"元素类型(1)"。
     * 消费顺序与旧实现完全一致（旧实现是在 switch 的各 case 里 READ 的）。 */
    TypeKind elem_type = TYPE_ANY;
    Value name_val = NULL_VAL;
    if (expected_type == TYPE_STRUCT || expected_type == TYPE_FACE ||
        expected_type == TYPE_ENUM) {
        uint16_t name_idx = READ_SHORT();
        name_val = frame->chunk->constants[name_idx];
    } else {
        elem_type = (TypeKind)READ_BYTE();   /* codegen 总会发出这个字节 */
    }

    int matches = type_check_value(value, expected_type, elem_type, name_val);
    vm_stack_push(&vm, val_bool(matches));
    DISPATCH;
}
