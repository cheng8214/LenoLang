// ============================================================================
// clib 调用操作码 (OP_CLIB_CALL)
// 格式: OP_CLIB_CALL, arg_count(2), ret_type_kind(1), user_arg_count(1), arg_types[user_arg_count](1 each)
// 栈上参数: [lib_obj, func_name_str, user_arg1, user_arg2, ...]
// ret_type_kind: TypeKind 枚举值，VM 端根据 TypeKind 做返回值自动展开
// 结果压栈
// ============================================================================

OPCODE(OP_CLIB_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int arg_count = READ_SHORT();
    int ret_type_kind = READ_BYTE();
    int user_arg_count = READ_BYTE();

    // 读取参数类型
    int arg_types[FFI_MAX_ARGS];
    memset(arg_types, 0, sizeof(arg_types));
    for (int i = 0; i < user_arg_count && i < FFI_MAX_ARGS; i++) {
        arg_types[i] = READ_BYTE();
    }

    // 栈上已有参数: [lib, func_name, user_args...]
    Value* call_args = vm.stack + vm.sp - arg_count;

    // 调用 FFI（传递 TypeKind 用于返回值自动展开和参数窄化）
    Value result = ffi_clib_call(arg_count, call_args, ret_type_kind, arg_types);

    // ★ 重新读取 frame 指针：ffi_clib_call 内部可能触发自动泵送
    // (ffi_call_impl → ffi_pump_callbacks_func → callback_dispatch_direct
    //  → vm_call_value → call_value → vm_grow_frames)，导致 vm.frames
    //  被 realloc 重新分配。此时旧的 frame 指针成为悬空指针，
    //  DISPATCH 读取 frame->ip 会触发 use-after-free。
    // 在 jumptable 模式下（Linux/GCC），DISPATCH 不重新读取 frame，
    // 所以必须在此处手动刷新。switch 模式下 DISPATCH 会自动重新读取。
    frame = &vm.frames[vm.frame_cnt - 1];

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
    // ★ 再次刷新 frame：vm_stack_push 可能触发栈扩容（realloc），
    // 虽然不影响 frames 数组，但保持一致性
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}
