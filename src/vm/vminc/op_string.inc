// ============================================================================
// 字符串操作类操作码 (OP_STRING)
// ============================================================================

OPCODE(OP_STRING_ADD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 字符串拼接：将栈顶两个值弹出，转换为字符串后拼接
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：两个操作数都是 ObjString，直接使用 str_concat（正确处理 null 字节）
    if (val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING &&
        val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING) {
        ObjString* result = str_concat((ObjString*)val_as_obj(a), (ObjString*)val_as_obj(b));
        vm_stack_push(&vm, val_obj((Object*)result));
        DISPATCH;
    }

    // 将两个值都转换为字符串
    char* str_a = value_to_string(a);
    char* str_b = value_to_string(b);

    // 计算新字符串长度（转换后的字符串不含 null 字节，strlen 安全）
    int len_a = (int)strlen(str_a);
    int len_b = (int)strlen(str_b);
    int total_len = len_a + len_b;

    // 创建新字符串
    ObjString* result = str_alloc(total_len);
    memcpy(result->chars, str_a, len_a);
    memcpy(result->chars + len_a, str_b, len_b);
    result->chars[total_len] = '\0';
    result->hash = hash_string(result->chars, total_len);

    // 释放临时字符串
    free(str_a);
    free(str_b);

    vm_stack_push(&vm, val_obj((Object*)result));
    DISPATCH;
}
