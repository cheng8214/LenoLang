// ============================================================================
// 字典操作类操作码 (OP_DICT)
// ============================================================================

OPCODE(OP_DICT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t count = READ_SHORT();
    ObjDict* dict = dict_new(count > 0 ? count : 8);
    if (!dict) return -1;

    // 先收集所有键值对（从栈中弹出，顺序是反的）
    Value* keys = (Value*)malloc(count * sizeof(Value));
    Value* values = (Value*)malloc(count * sizeof(Value));
    if (!keys || !values) {
        runtime_error("内存分配失败", GET_CURRENT_LINE());
        return -1;
    }

    for (int i = count - 1; i >= 0; i--) {
        values[i] = vm_stack_pop(&vm);
        keys[i] = vm_stack_pop(&vm);
    }

    // 按正序插入字典（键支持 int、string、float、bool 等类型）
    for (int i = 0; i < count; i++) {
        dict_set(dict, keys[i], values[i]);
    }

    free(keys);
    free(values);
    vm_stack_push(&vm, val_obj((Object*)dict));
    DISPATCH;
}

OPCODE(OP_DICT_GET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value key_val = vm_stack_pop(&vm);
    Value dict_val = vm_stack_pop(&vm);

    if (!val_is_obj(dict_val) || val_as_obj(dict_val)->type != OBJ_DICT) {
        throw_runtime_error("索引操作需要字典", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    ObjDict* dict = (ObjDict*)val_as_obj(dict_val);
    Value result = dict_get(dict, key_val);
    vm_stack_push(&vm, result);
    DISPATCH;
}

OPCODE(OP_DICT_SET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value value = vm_stack_pop(&vm);
    Value key_val = vm_stack_pop(&vm);
    Value dict_val = vm_stack_pop(&vm);

    if (!val_is_obj(dict_val) || val_as_obj(dict_val)->type != OBJ_DICT) {
        throw_runtime_error("赋值操作需要字典", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    ObjDict* dict = (ObjDict*)val_as_obj(dict_val);

    dict_set(dict, key_val, value);
    vm_stack_push(&vm, dict_val);  // 返回字典，支持链式操作
    DISPATCH;
}

OPCODE(OP_DICT_GET_KEY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 按索引获取字典键：栈顶是索引，次栈顶是字典
    Value index_val = vm_stack_pop(&vm);
    Value dict_val = vm_stack_pop(&vm);
    
    if (!val_is_obj(dict_val) || val_as_obj(dict_val)->type != OBJ_DICT) {
        runtime_error("DICT_GET_KEY需要字典", GET_CURRENT_LINE());
        return -1;
    }
    
    if (!val_is_num(index_val)) {
        throw_runtime_error("DICT_GET_KEY索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    int index = (int)value_to_double(index_val);
    ObjDict* dict = (ObjDict*)val_as_obj(dict_val);
    
    // count = 数组部分 + 哈希部分条目数
    (void)(dict->acount + dict->count);  // 保留语义，消除 unused 警告
    if (index < 0 || index >= dict->order_count) {
        throw_runtime_error("字典键索引越界", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // 按索引获取键（返回 Value，可能为 int/string/float 等）
    Value key = dict_get_key_by_index(dict, index);
    vm_stack_push(&vm, key);
    DISPATCH;
}
