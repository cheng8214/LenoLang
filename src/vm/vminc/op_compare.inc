// ============================================================================
// 通用比较运算类操作码 (OP_COMPARE)
// ============================================================================
// 完整的类型检查和比较逻辑

OPCODE(OP_EQ) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    int result = 0;

    // 快速路径：int == int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) == val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float == float / int == float
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) == val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp == 0));
        DISPATCH;
    }

    // Ptr 与 null 比较：检查指针地址是否为 NULL
    if (val_is_null(a) && val_is_obj(b) && val_as_obj(b)->type == OBJ_FFI_POINTER) {
        vm_stack_push(&vm, val_bool(((ObjFFIPointer*)val_as_obj(b))->ptr == NULL));
        DISPATCH;
    }
    if (val_is_null(b) && val_is_obj(a) && val_as_obj(a)->type == OBJ_FFI_POINTER) {
        vm_stack_push(&vm, val_bool(((ObjFFIPointer*)val_as_obj(a))->ptr == NULL));
        DISPATCH;
    }
    if (val_is_null(a) && val_is_obj(b) && val_as_obj(b)->type == OBJ_FFI_CALLBACK) {
        vm_stack_push(&vm, val_bool(((ObjFFICallback*)val_as_obj(b))->trampoline == NULL));
        DISPATCH;
    }
    if (val_is_null(b) && val_is_obj(a) && val_as_obj(a)->type == OBJ_FFI_CALLBACK) {
        vm_stack_push(&vm, val_bool(((ObjFFICallback*)val_as_obj(a))->trampoline == NULL));
        DISPATCH;
    }

    if (val_get_type(a) == val_get_type(b)) {
        switch (val_get_type(a)) {
            case VAL_NULL:
                result = 1;
                break;
            case VAL_BOOL:
                result = (val_as_bool(a) == val_as_bool(b));
                break;
            case VAL_INT:
            case VAL_FLOAT:
                result = 0;
                break;
            case VAL_OBJ:
                if (val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING) {
                    ObjString* sa = (ObjString*)val_as_obj(a);
                    ObjString* sb = (ObjString*)val_as_obj(b);
                    result = (sa->len == sb->len &&
                              memcmp(sa->chars, sb->chars, sa->len) == 0);
                } else if (val_as_obj(a)->type == OBJ_ARRAY && val_as_obj(b)->type == OBJ_ARRAY) {
                    ObjArray* aa = (ObjArray*)val_as_obj(a);
                    ObjArray* ab = (ObjArray*)val_as_obj(b);
                    if (aa->count != ab->count) {
                        result = 0;
                    } else {
                        result = 1;
                        for (int i = 0; i < aa->count; i++) {
                            Value ea = aa->elements[i];
                            Value eb = ab->elements[i];
                            // 数组元素比较：数值类型值比较，其他类型严格比较
                            // int vs int
                            if (val_is_int(ea) && val_is_int(eb)) {
                                if (val_as_int(ea) != val_as_int(eb)) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_is_float(ea) || val_is_float(eb)) {
                                // float 参与比较
                                if (val_as_num(ea) != val_as_num(eb)) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_is_bigint(ea) || val_is_bigint(eb)) {
                                // BigInt 比较
                                ObjBigInt* bea = promote_to_bigint(ea);
                                ObjBigInt* beb = promote_to_bigint(eb);
                                if (bigint_compare(bea, beb) != 0) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_get_type(ea) != val_get_type(eb)) {
                                result = 0;
                                break;
                            } else {
                                int elem_equal = 0;
                                switch (val_get_type(ea)) {
                                    case VAL_NULL:
                                        elem_equal = 1;
                                        break;
                                    case VAL_BOOL:
                                        elem_equal = (val_as_bool(ea) == val_as_bool(eb));
                                        break;
                                    case VAL_INT:
                                    case VAL_FLOAT:
                                        elem_equal = 0;
                                        break;
                                    case VAL_OBJ:
                                        if (val_as_obj(ea)->type == OBJ_STRING && val_as_obj(eb)->type == OBJ_STRING) {
                                            ObjString* sea = (ObjString*)val_as_obj(ea);
                                            ObjString* seb = (ObjString*)val_as_obj(eb);
                                            elem_equal = (sea->len == seb->len &&
                                                      memcmp(sea->chars, seb->chars, sea->len) == 0);
                                        } else {
                                            elem_equal = (val_as_obj(ea) == val_as_obj(eb));
                                        }
                                        break;
                                }
                                if (!elem_equal) {
                                    result = 0;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    result = (val_as_obj(a) == val_as_obj(b));
                }
                break;
        }
    }
    vm_stack_push(&vm, val_bool(result));
    DISPATCH;
}

OPCODE(OP_NEQ) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    int result = 0;

    // 快速路径：int == int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) != val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float == float / int == float
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) != val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp != 0));
        DISPATCH;
    }

    // Ptr 与 null 比较：检查指针地址是否为 NULL
    if (val_is_null(a) && val_is_obj(b) && val_as_obj(b)->type == OBJ_FFI_POINTER) {
        vm_stack_push(&vm, val_bool(((ObjFFIPointer*)val_as_obj(b))->ptr != NULL));
        DISPATCH;
    }
    if (val_is_null(b) && val_is_obj(a) && val_as_obj(a)->type == OBJ_FFI_POINTER) {
        vm_stack_push(&vm, val_bool(((ObjFFIPointer*)val_as_obj(a))->ptr != NULL));
        DISPATCH;
    }
    if (val_is_null(a) && val_is_obj(b) && val_as_obj(b)->type == OBJ_FFI_CALLBACK) {
        vm_stack_push(&vm, val_bool(((ObjFFICallback*)val_as_obj(b))->trampoline != NULL));
        DISPATCH;
    }
    if (val_is_null(b) && val_is_obj(a) && val_as_obj(a)->type == OBJ_FFI_CALLBACK) {
        vm_stack_push(&vm, val_bool(((ObjFFICallback*)val_as_obj(a))->trampoline != NULL));
        DISPATCH;
    }

    if (val_get_type(a) == val_get_type(b)) {
        switch (val_get_type(a)) {
            case VAL_NULL:
                result = 1;
                break;
            case VAL_INT:
            case VAL_FLOAT:
                result = 0;
                break;
            case VAL_BOOL:
                result = (val_as_bool(a) == val_as_bool(b));
                break;
            case VAL_OBJ:
                if (val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING) {
                    ObjString* sa = (ObjString*)val_as_obj(a);
                    ObjString* sb = (ObjString*)val_as_obj(b);
                    result = (sa->len == sb->len &&
                              memcmp(sa->chars, sb->chars, sa->len) == 0);
                } else if (val_as_obj(a)->type == OBJ_ARRAY && val_as_obj(b)->type == OBJ_ARRAY) {
                    ObjArray* aa = (ObjArray*)val_as_obj(a);
                    ObjArray* ab = (ObjArray*)val_as_obj(b);
                    if (aa->count != ab->count) {
                        result = 0;
                    } else {
                        result = 1;
                        for (int i = 0; i < aa->count; i++) {
                            Value ea = aa->elements[i];
                            Value eb = ab->elements[i];
                            // 数组元素比较：数值类型值比较，其他类型严格比较
                            // int vs int
                            if (val_is_int(ea) && val_is_int(eb)) {
                                if (val_as_int(ea) != val_as_int(eb)) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_is_float(ea) || val_is_float(eb)) {
                                // float 参与比较
                                if (val_as_num(ea) != val_as_num(eb)) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_is_bigint(ea) || val_is_bigint(eb)) {
                                // BigInt 比较
                                ObjBigInt* bea = promote_to_bigint(ea);
                                ObjBigInt* beb = promote_to_bigint(eb);
                                if (bigint_compare(bea, beb) != 0) {
                                    result = 0;
                                    break;
                                }
                            } else if (val_get_type(ea) != val_get_type(eb)) {
                                result = 0;
                                break;
                            } else {
                                int elem_equal = 0;
                                switch (val_get_type(ea)) {
                                    case VAL_NULL:
                                        elem_equal = 1;
                                        break;
                                    case VAL_BOOL:
                                        elem_equal = (val_as_bool(ea) == val_as_bool(eb));
                                        break;
                                    case VAL_INT:
                                    case VAL_FLOAT:
                                        elem_equal = 0;
                                        break;
                                    case VAL_OBJ:
                                        if (val_as_obj(ea)->type == OBJ_STRING && val_as_obj(eb)->type == OBJ_STRING) {
                                            ObjString* sea = (ObjString*)val_as_obj(ea);
                                            ObjString* seb = (ObjString*)val_as_obj(eb);
                                            elem_equal = (sea->len == seb->len &&
                                                      memcmp(sea->chars, seb->chars, sea->len) == 0);
                                        } else {
                                            elem_equal = (val_as_obj(ea) == val_as_obj(eb));
                                        }
                                        break;
                                }
                                if (!elem_equal) {
                                    result = 0;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    result = (val_as_obj(a) == val_as_obj(b));
                }
                break;
        }
    }
    vm_stack_push(&vm, val_bool(!result));
    DISPATCH;
}

// OP_IS_NULL：弹出栈顶值，压入 val_is_null 结果
// 用于 ?. 和 ?? 的 null 检查，避免 OP_EQ 的 float 0.0 == null bug
OPCODE(OP_IS_NULL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value val = vm_stack_pop(&vm);
    vm_stack_push(&vm, val_bool(val_is_null(val)));
    DISPATCH;
}

OPCODE(OP_LT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int < int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) < val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float < 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) < val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp < 0));
        DISPATCH;
    }

    // 字符串比较（罕见）
    if (unlikely(val_is_obj(a) && val_is_obj(b) &&
        val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING)) {
        ObjString* sa = (ObjString*)val_as_obj(a);
        ObjString* sb = (ObjString*)val_as_obj(b);
        int result = strcmp(sa->chars, sb->chars);
        vm_stack_push(&vm, val_bool(result < 0));
        DISPATCH;
    }

    throw_runtime_error("操作数必须是数字或字符串", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_GT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int > int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) > val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float > 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) > val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp > 0));
        DISPATCH;
    }

    // 字符串比较（罕见）
    if (unlikely(val_is_obj(a) && val_is_obj(b) &&
        val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING)) {
        ObjString* sa = (ObjString*)val_as_obj(a);
        ObjString* sb = (ObjString*)val_as_obj(b);
        int result = strcmp(sa->chars, sb->chars);
        vm_stack_push(&vm, val_bool(result > 0));
        DISPATCH;
    }

    throw_runtime_error("操作数必须是数字或字符串", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_LE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int <= int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) <= val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float <= 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) <= val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp <= 0));
        DISPATCH;
    }

    // 字符串比较（罕见）
    if (unlikely(val_is_obj(a) && val_is_obj(b) &&
        val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING)) {
        ObjString* sa = (ObjString*)val_as_obj(a);
        ObjString* sb = (ObjString*)val_as_obj(b);
        int result = strcmp(sa->chars, sb->chars);
        vm_stack_push(&vm, val_bool(result <= 0));
        DISPATCH;
    }

    throw_runtime_error("操作数必须是数字或字符串", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_GE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // 快速路径：int >= int
    if (val_is_int(a) && val_is_int(b)) {
        vm_stack_push(&vm, val_bool(val_as_int(a) >= val_as_int(b)));
        DISPATCH;
    }

    // 快速路径：float >= 任意数值
    if (val_is_float(a) || val_is_float(b)) {
        vm_stack_push(&vm, val_bool(val_as_num(a) >= val_as_num(b)));
        DISPATCH;
    }

    // BigInt 比较（罕见）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        int cmp = bigint_compare(ba, bb);
        vm_stack_push(&vm, val_bool(cmp >= 0));
        DISPATCH;
    }

    // 字符串比较（罕见）
    if (unlikely(val_is_obj(a) && val_is_obj(b) &&
        val_as_obj(a)->type == OBJ_STRING && val_as_obj(b)->type == OBJ_STRING)) {
        ObjString* sa = (ObjString*)val_as_obj(a);
        ObjString* sb = (ObjString*)val_as_obj(b);
        int result = strcmp(sa->chars, sb->chars);
        vm_stack_push(&vm, val_bool(result >= 0));
        DISPATCH;
    }

    throw_runtime_error("操作数必须是数字或字符串", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}
