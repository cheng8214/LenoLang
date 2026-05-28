// ============================================================================
// 原生模块加载操作码 (OP_LOAD_NATIVE_MODULE)
// ============================================================================
// import times 时生成此操作码，运行时调用对应的 native_init_module()
// 参数：常量池索引（模块名字符串）
// 行为：加载模块方法到全局方法表，压入 null（占位，兼容栈平衡）
// ============================================================================

OPCODE(OP_LOAD_NATIVE_MODULE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int module_idx = READ_SHORT();

    // 获取模块名
    Value module_val = frame->chunk->constants[module_idx];

    if (!val_is_obj(module_val) || val_as_obj(module_val)->type != OBJ_STRING) {
        runtime_error("模块名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

    const char* module_name = ((ObjString*)val_as_obj(module_val))->chars;

    // 运行时加载原生模块（调用对应的 *_init_module()）
    native_init_module(module_name);

    // 压入 null 占位，保持栈平衡（import 语句作为表达式语句需要有一个值）
    vm_stack_push(&vm, val_null());

    DISPATCH;
}
