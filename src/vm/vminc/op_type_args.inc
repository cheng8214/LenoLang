// ============================================================================
// OP_PUSH_TYPE_ARGS — 泛型函数调用的类型参数传递
// 格式: OP_PUSH_TYPE_ARGS count(1) [const_index(2)]...
// 将类型参数暂存到 VM，供后续 OP_CALL 读取并设置到 closure
// ============================================================================

OPCODE(OP_PUSH_TYPE_ARGS) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t count = READ_BYTE();

    // 释放之前的待处理数据
    if (vm.pending_type_args) {
        for (int i = 0; i < vm.pending_type_arg_count; i++) {
            free(vm.pending_type_args[i]);
        }
        free(vm.pending_type_args);
        vm.pending_type_args = NULL;
        vm.pending_type_arg_count = 0;
    }

    if (count > 0) {
        vm.pending_type_args = (char**)malloc(sizeof(char*) * count);
        vm.pending_type_arg_count = count;
        for (int i = 0; i < count; i++) {
            uint16_t const_idx = READ_SHORT();
            Value const_val = frame->chunk->constants[const_idx];
            if (val_is_obj(const_val) && val_as_obj(const_val)->type == OBJ_STRING) {
                ObjString* type_name = (ObjString*)val_as_obj(const_val);
                vm.pending_type_args[i] = strdup(type_name->chars);
            } else {
                vm.pending_type_args[i] = strdup("any");
            }
        }
    }

    DISPATCH;
}
