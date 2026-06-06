// ============================================================================
// clib 调用操作码 (OP_CLIB_CALL)
// 格式: OP_CLIB_CALL, arg_count(2), ret_type(1), user_arg_count(1), arg_types[user_arg_count](1 each)
// 栈上参数: [lib_obj, func_name_str, user_arg1, user_arg2, ...]
// 结果压栈
// ============================================================================

OPCODE(OP_CLIB_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int arg_count = READ_SHORT();
    int ret_type = READ_BYTE();
    int user_arg_count = READ_BYTE();

    // 读取参数类型
    int arg_types[FFI_MAX_ARGS];
    memset(arg_types, 0, sizeof(arg_types));
    for (int i = 0; i < user_arg_count && i < FFI_MAX_ARGS; i++) {
        arg_types[i] = READ_BYTE();
    }

    // 栈上已有参数: [lib, func_name, user_args...]
    Value* call_args = vm.stack + vm.sp - arg_count;

    // 调用 FFI（传递参数类型用于 str16 自动转换）
    Value result = ffi_clib_call(arg_count, call_args, ret_type, arg_types);

    // 检查是否有异常
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

    // 清理栈上的参数
    vm.sp -= arg_count;

    // 压入返回值
    vm_stack_push(&vm, result);
    DISPATCH;
}
