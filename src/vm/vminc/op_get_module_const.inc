// ============================================================================
// 原生模块常量获取操作码 (OP_GET_MODULE_CONST)
// ============================================================================
// 用法：模块常量获取
// 参数：module_name_constant (16-bit), const_name_constant (16-bit)
// 行为：从模块常量表中查找值，压入 int
// ============================================================================

OPCODE(OP_GET_MODULE_CONST) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int module_idx = READ_SHORT();
    int const_idx = READ_SHORT();

    // 获取模块名和常量名
    Value module_val = frame->chunk->constants[module_idx];
    Value const_val = frame->chunk->constants[const_idx];

    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_STRING ||
        !val_is_obj(const_val) || val_as_obj(const_val)->type != OBJ_STRING) {
        runtime_error("模块名和常量名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

    const char* module_name = ((ObjString*)val_as_obj(module_val))->chars;
    const char* const_name = ((ObjString*)val_as_obj(const_val))->chars;

    bool found = false;
    int value = native_find_module_const(module_name, const_name, &found);

    if (!found) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg), "未找到模块常量: %s.%s", module_name, const_name);
        runtime_error(msg, GET_CURRENT_LINE());
        return -1;
    }

    vm_stack_push(&vm, val_int(value));
    DISPATCH;
}
