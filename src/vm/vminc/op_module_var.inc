// ============================================================================
// 模块变量/函数操作类操作码 (OP_MODULE_VAR)
// ============================================================================

OPCODE(OP_GET_MODULE_VAR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t index = READ_SHORT();
    // 使用当前调用帧的模块信息
    ObjModule* module = NULL;
    if (vm.frame_cnt > 0) {
        module = (ObjModule*)vm.frames[vm.frame_cnt - 1].module;
    }
    if (!module) {
        runtime_error("模块变量访问错误: 不在模块上下文中", GET_CURRENT_LINE());
        return -1;
    }
    if (index >= module->global_count) {
        runtime_error("模块变量索引越界", GET_CURRENT_LINE());
        return -1;
    }
    Value value = module->globals[index];
    vm_stack_push(&vm, value);
    DISPATCH;
}

OPCODE(OP_SET_MODULE_VAR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t index = READ_SHORT();
    Value value = vm_stack_peek(&vm, 0);
    // 使用当前调用帧的模块信息（与 OP_GET_MODULE_VAR 一致）
    ObjModule* module = NULL;
    if (vm.frame_cnt > 0) {
        module = (ObjModule*)vm.frames[vm.frame_cnt - 1].module;
    }
    if (!module) {
        runtime_error("模块变量设置错误: 不在模块上下文中", GET_CURRENT_LINE());
        return -1;
    }
    if (index >= module->global_count) {
        runtime_error("模块变量索引越界", GET_CURRENT_LINE());
        return -1;
    }
    module->globals[index] = value;
    gc_write_barrier((Object*)module, value);
    DISPATCH;
}

OPCODE(OP_GET_MODULE_FUNC) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t index = READ_SHORT();
    // 使用当前调用帧的模块信息
    ObjModule* module = NULL;
    if (vm.frame_cnt > 0) {
        module = (ObjModule*)vm.frames[vm.frame_cnt - 1].module;
    }
    if (!module) {
        runtime_error("模块函数访问错误: 不在模块上下文中", GET_CURRENT_LINE());
        return -1;
    }
    if (index >= module->global_count) {
        runtime_error("模块函数索引越界", GET_CURRENT_LINE());
        return -1;
    }
    Value value = module->globals[index];
    vm_stack_push(&vm, value);
    DISPATCH;
}

OPCODE(OP_DEFINE_MODULE_FUNC) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t index = READ_SHORT();
    Value func = vm_stack_pop(&vm);
    // 使用当前调用帧的模块信息
    ObjModule* module = NULL;
    if (vm.frame_cnt > 0) {
        module = (ObjModule*)vm.frames[vm.frame_cnt - 1].module;
    }
    if (!module) {
        runtime_error("模块函数定义错误: 不在模块上下文中", GET_CURRENT_LINE());
        return -1;
    }
    // 确保全局变量数组有足够容量
    if (index >= module->global_capacity) {
        int new_capacity = module->global_capacity == 0 ? 8 : module->global_capacity * 2;
        while (new_capacity <= index) {
            new_capacity *= 2;
        }
        Value* new_globals = (Value*)realloc(module->globals, sizeof(Value) * new_capacity);
        if (!new_globals) {
            runtime_error("内存分配失败", GET_CURRENT_LINE());
            return -1;
        }
        // 初始化新分配的空间
        for (int i = module->global_capacity; i < new_capacity; i++) {
            new_globals[i] = val_null();
        }
        module->globals = new_globals;
        module->global_capacity = new_capacity;
    }
    // 设置函数值
    module->globals[index] = func;
    gc_write_barrier((Object*)module, func);
    if (index >= module->global_count) {
        module->global_count = index + 1;
    }
    DISPATCH;
}
