// ============================================================================
// 集合操作类操作码 (OP_COLLECTION)
// ============================================================================
// in 操作符和范围创建

OPCODE(OP_IN) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value collection = vm_stack_pop(&vm);
    Value item = vm_stack_pop(&vm);
    int found = 0;

    if (val_is_obj(collection)) {
        switch (val_as_obj(collection)->type) {
            case OBJ_ARRAY: {
                ObjArray* arr = (ObjArray*)val_as_obj(collection);
                for (int i = 0; i < arr->count; i++) {
                    Value elem = arr->elements[i];
                    if (val_get_type(item) == val_get_type(elem)) {
                        if (val_is_num(item) && val_is_num(elem)) {
                            if (value_to_double(item) == value_to_double(elem)) {
                                found = 1;
                                break;
                            }
                        } else if (val_is_bool(item) && val_as_bool(item) == val_as_bool(elem)) {
                            found = 1;
                            break;
                        } else if (val_is_obj(item) && val_as_obj(item) == val_as_obj(elem)) {
                            found = 1;
                            break;
                        } else if (val_is_null(item)) {
                            found = 1;
                            break;
                        }
                    }
                }
                break;
            }
            case OBJ_DICT: {
                ObjDict* dict = (ObjDict*)val_as_obj(collection);
                found = dict_has(dict, item);
                break;
            }
            case OBJ_STRING: {
                ObjString* str = (ObjString*)val_as_obj(collection);
                if (val_is_obj(item) && val_as_obj(item)->type == OBJ_STRING) {
                    ObjString* substr = (ObjString*)val_as_obj(item);
                    found = (strstr(str->chars, substr->chars) != NULL);
                }
                break;
            }
            case OBJ_RANGE: {
                ObjRange* range = (ObjRange*)val_as_obj(collection);
                if (val_is_num(item)) {
                    found = range_contains(range, (int)value_to_double(item));
                }
                break;
            }
            default:
                break;
        }
    }

    vm_stack_push(&vm, val_bool(found));
    DISPATCH;
}

OPCODE(OP_RANGE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int inclusive = READ_BYTE();
    Value end = vm_stack_pop(&vm);
    Value start = vm_stack_pop(&vm);

    int start_val = 0, end_val = 0;
    if (val_is_num(start)) {
        start_val = (int)value_to_double(start);
    }
    if (val_is_num(end)) {
        end_val = (int)value_to_double(end);
    }

    ObjRange* range = range_new(start_val, end_val, inclusive);
    vm_stack_push(&vm, val_obj((Object*)range));
    DISPATCH;
}
