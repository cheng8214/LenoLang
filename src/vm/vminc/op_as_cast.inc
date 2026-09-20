// ============================================================================
// 安全类型转换操作码 (OP_AS_CAST)
//
// 字节码格式（带名字常量的类型）:
//   [OP_AS_CAST] [type_kind:1] [name_const_idx:2]                   → 4 字节
//   **TYPE_STRUCT / TYPE_FACE / TYPE_CSTRUCT 三个读 2 字节名字常量**
// 字节码格式（其余类型）:
//   [OP_AS_CAST] [type_kind:1] [element_type_kind:1]                → 3 字节
//   （element_type_kind 无元素类型时为 TYPE_ANY）
//
// ⚠ 与 OP_TYPE_CHECK 的**唯一差别**（长度表必须分开登记，§8.83 修掉过混用缺陷）：
//   这里带名字常量的是 STRUCT / FACE / **CSTRUCT**；而 **ENUM 不在本操作码的 switch 里**
//   （抽取前它落到 default ⇒ matches=0 且只消费 1 字节）。OP_TYPE_CHECK 那边带名字常量的
//   是 STRUCT / FACE / **ENUM**、且没有 CSTRUCT 分支 —— 两者不能共用同一条长度规则。
//
// 执行逻辑: 弹出栈顶值 → 按期望类型检查/转换 → 匹配（可能是转换后的值）或 null 压回
//          （pop 1 push 1，**net 0**）
//
// 转换逻辑抽到 vm_as_cast（vm.c）—— **语义唯一来源**：
//   任何路径都必须调它（历史上两份实现漂移过，§8.83）。
// ============================================================================

OPCODE(OP_AS_CAST) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t type_kind_byte = READ_BYTE();
    TypeKind expected_type = (TypeKind)type_kind_byte;

    Value value = vm_stack_pop(&vm);

    /* 操作数：STRUCT / FACE / CSTRUCT 是"名字常量(2)"，其余（含 ENUM）是"元素类型(1)"。
     * 消费顺序与旧实现完全一致（旧实现是在 switch 的各 case 里 READ 的）。 */
    TypeKind elem_type = TYPE_ANY;
    Value name_val = NULL_VAL;
    if (expected_type == TYPE_STRUCT || expected_type == TYPE_FACE ||
        expected_type == TYPE_CSTRUCT) {
        uint16_t name_idx = READ_SHORT();
        name_val = frame->chunk->constants[name_idx];
    } else {
        elem_type = (TypeKind)READ_BYTE();   /* codegen 总会发出这个字节 */
    }

    Value result = vm_as_cast(value, expected_type, elem_type, name_val);
    vm_stack_push(&vm, result);
    DISPATCH;
}
