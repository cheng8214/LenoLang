// ============================================================================
// 迭代器操作类操作码 (OP_ITER)
// ============================================================================

OPCODE(OP_ITER_GET_VALUE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 迭代获取值：用于字典/struct遍历时获取值
    // 栈顶是索引，次栈顶是对象
    Value index_val = vm_stack_pop(&vm);
    Value obj_val = vm_stack_pop(&vm);

    if (!val_is_num(index_val)) {
        throw_runtime_error("ITER_GET_VALUE索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    int index = (int)value_to_double(index_val);

    if (!val_is_obj(obj_val)) {
        throw_runtime_error("ITER_GET_VALUE需要对象类型", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    switch (val_as_obj(obj_val)->type) {
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            if (index < 0 || index >= dict->count) {
                throw_runtime_error("字典值索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            Value value = dict_get_value_by_index(dict, index);
            vm_stack_push(&vm, value);
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)val_as_obj(obj_val);
            if (index < 0 || index >= struct_obj->def->field_count) {
                throw_runtime_error("结构体字段索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            Value field_value = struct_get_field(struct_obj, index);
            vm_stack_push(&vm, field_value);
            break;
        }
        default:
            throw_runtime_error("ITER_GET_VALUE只能用于字典或结构体类型", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
    }
    DISPATCH;
}
