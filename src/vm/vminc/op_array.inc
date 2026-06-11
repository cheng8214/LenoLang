// ============================================================================
// 数组操作类操作码 (OP_ARRAY)
// ============================================================================

OPCODE(OP_ARRAY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t count = READ_SHORT();
    ObjArray* arr = arr_new(count);
    if (!arr) return -1;

    // 从栈中弹出元素（从后往前，因为栈是后进先出）
    for (int i = count - 1; i >= 0; i--) {
        arr->elements[i] = vm_stack_pop(&vm);
    }
    arr->count = count;

    // 设置运行时类型信息
    if (count > 0) {
        // 检查所有元素类型是否一致
        Value first = arr->elements[0];
        int all_same = 1;
        for (int i = 1; i < count; i++) {
            if (val_get_type(arr->elements[i]) != val_get_type(first)) {
                all_same = 0;
                break;
            }
        }
        
        if (all_same) {
            const char* elem_type_name = NULL;
            switch (val_get_type(first)) {
                case VAL_NULL: elem_type_name = "null"; break;
                case VAL_BOOL: elem_type_name = "bool"; break;
                case VAL_INT: elem_type_name = "int"; break;
                case VAL_FLOAT: elem_type_name = "float"; break;
                case VAL_OBJ: {
                    if (val_as_obj(first)->type == OBJ_STRING) {
                        elem_type_name = "string";
                    } else if (val_as_obj(first)->type == OBJ_ARRAY) {
                        elem_type_name = "Array";
                    } else if (val_as_obj(first)->type == OBJ_DICT) {
                        elem_type_name = "Dict";
                    }
                    break;
                }
            }
            if (elem_type_name) {
                // 根据元素类型创建 TypeInfo
                TypeKind elem_kind = TYPE_ANY;
                if (strcmp(elem_type_name, "int") == 0) elem_kind = TYPE_INT;
                else if (strcmp(elem_type_name, "float") == 0) elem_kind = TYPE_FLOAT;
                else if (strcmp(elem_type_name, "string") == 0) elem_kind = TYPE_STRING;
                else if (strcmp(elem_type_name, "bool") == 0) elem_kind = TYPE_BOOL;
                else if (strcmp(elem_type_name, "Array") == 0) elem_kind = TYPE_ARRAY;
                else if (strcmp(elem_type_name, "Dict") == 0) elem_kind = TYPE_DICT;
                
                TypeInfo* elem_type = type_new(elem_kind);
                arr->type_info = type_array(elem_type);
            }
        }
    }

    vm_stack_push(&vm, val_obj((Object*)arr));
    DISPATCH;
}

OPCODE(OP_ARRAY_GET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value index_val = vm_stack_pop(&vm);
    Value arr_val = vm_stack_pop(&vm);

#ifdef LENO_RELEASE
    // 发布版：最小检查，信任代码
    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
    int index = likely(val_is_int(index_val)) ? val_as_int(index_val) : (int)value_to_double(index_val);
    vm_stack_push(&vm, arr->elements[index]);
#else
    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) {
        throw_runtime_error("索引操作需要数组", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    if (!val_is_num(index_val)) {
        throw_runtime_error("数组索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    int index = (int)value_to_double(index_val);
    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);

    if (index < 0 || index >= arr->count) {
        throw_runtime_error("数组索引越界", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    vm_stack_push(&vm, arr->elements[index]);
#endif
    DISPATCH;
}

OPCODE(OP_ARRAY_SET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value value = vm_stack_pop(&vm);
    Value index_val = vm_stack_pop(&vm);
    Value arr_val = vm_stack_pop(&vm);

#ifdef LENO_RELEASE
    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
    int index = likely(val_is_int(index_val)) ? val_as_int(index_val) : (int)value_to_double(index_val);
    arr->elements[index] = value;
    gc_write_barrier((Object*)arr, value);
    if (index >= arr->count) {
        arr->count = index + 1;
    }
    vm_stack_push(&vm, value);
#else
    // 调试版：完整检查
    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) {
        throw_runtime_error("赋值操作需要数组", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    if (!val_is_num(index_val)) {
        throw_runtime_error("数组索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    int index = (int)value_to_double(index_val);
    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);

    if (index < 0 || index >= arr->capacity) {
        throw_runtime_error("数组索引越界", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    arr->elements[index] = value;
    gc_write_barrier((Object*)arr, value);
    if (index >= arr->count) {
        arr->count = index + 1;
    }
    vm_stack_push(&vm, value);
#endif
    DISPATCH;
}

OPCODE(OP_ARRAY_APPEND) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value value = vm_stack_pop(&vm);
    Value arr_val = vm_stack_pop(&vm);

    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) {
        throw_runtime_error("append 操作需要数组", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);

    // 扩容检查
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) {
            throw_runtime_error("数组扩容失败", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
    }

    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
    vm_stack_push(&vm, val_int(arr->count));
    DISPATCH;
}

OPCODE(OP_ARRAY_APPEND_NOPUSH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value value = vm_stack_pop(&vm);
    Value arr_val = vm_stack_pop(&vm);

    if (!val_is_obj(arr_val) || val_as_obj(arr_val)->type != OBJ_ARRAY) {
        throw_runtime_error("append 操作需要数组", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    ObjArray* arr = (ObjArray*)val_as_obj(arr_val);

    // 扩容检查
    if (arr->count >= arr->capacity) {
        if (!arr_grow(arr)) {
            throw_runtime_error("数组扩容失败", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
    }

    arr->elements[arr->count++] = value;
    gc_write_barrier((Object*)arr, value);
    // 不压栈结果 - 用于表达式语句场景，省掉一次 OP_POP
    DISPATCH;
}
