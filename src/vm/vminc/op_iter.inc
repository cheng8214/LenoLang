// ============================================================================
// 迭代器操作类操作码 (OP_ITER)
// ============================================================================

OPCODE(OP_ITER_GET_VALUE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 迭代获取字典值：专门用于字典遍历时获取值
    // 栈顶是索引，次栈顶是字典对象
    Value index_val = vm_stack_pop(&vm);
    Value obj_val = vm_stack_pop(&vm);

    if (!val_is_num(index_val)) {
        throw_runtime_error("ITER_GET_VALUE索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    int index = (int)value_to_double(index_val);

    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_DICT) {
        throw_runtime_error("ITER_GET_VALUE只能用于字典类型", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
    if (index < 0 || index >= dict->count) {
        throw_runtime_error("字典值索引越界", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    Value value = dict_get_value_by_index(dict, index);
    vm_stack_push(&vm, value);
    DISPATCH;
}
