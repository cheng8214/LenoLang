// cfunc 回调创建操作码 (OP_CFUNC_CALLBACK)
// 格式: ret_type(1) param_count(1) param_types[param_count](1 each)
// 栈上: 函数值 -> ObjFFICallback

OPCODE(OP_CFUNC_CALLBACK) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int ret_type = READ_BYTE();
    int param_count = READ_BYTE();
    uint8_t param_types[12];
    for (int i = 0; i < param_count && i < 12; i++) {
        param_types[i] = READ_BYTE();
    }

    // 栈顶是函数值
    Value func_val = vm_stack_pop(&vm);

    Value result = ffi_callback_create_with_sig(func_val, ret_type, param_count, param_types);

    if (vm.has_exception) {
        char* msg = NULL;
        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
        } else {
            msg = value_to_string(vm.exception);
        }
        int line = GET_CURRENT_LINE();
        error_clear();
        throw_runtime_error(msg, line);
        free(msg);
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    vm_stack_push(&vm, result);
    DISPATCH;
}
