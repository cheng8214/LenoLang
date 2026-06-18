// ============================================================================
// 一元运算和类型转换类操作码 (OP_UNARY)
// ============================================================================
// 一元运算：取负、逻辑非
// 类型转换：转 float、转 int、转 string

OPCODE(OP_NEG) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);

    // int 取反：int48 回绕
    if (likely(val_is_int(a))) {
        vm_stack_push(&vm, val_int(-val_as_int(a)));
        DISPATCH;
    }

    // float 取反
    if (val_is_float(a)) {
        vm_stack_push(&vm, val_float(-val_as_num(a)));
        DISPATCH;
    }

    // BigInt 取反
    if (unlikely(val_is_bigint(a))) {
        ObjBigInt* ba = val_as_bigint(a);
        Value result = bigint_neg(ba);
        vm_stack_push(&vm, result);
    } else {
        throw_runtime_error("操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_NOT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    vm_stack_push(&vm, val_bool(is_falsey(vm_stack_pop(&vm))));
    DISPATCH;
}

OPCODE(OP_CAST_FLOAT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    // 快速路径：int/float 转 float
    if (val_is_int(a) || val_is_float(a)) {
        vm_stack_push(&vm, val_float(val_as_num(a)));
        DISPATCH;
    }
    // BigInt 转 float
    if (val_is_bigint(a)) {
        double val = bigint_to_double(val_as_bigint(a));
        vm_stack_push(&vm, val_float(val));
        DISPATCH;
    }
    if (val_is_null(a)) {
        // null 保持为 null
        vm_stack_push(&vm, a);
    } else {
        throw_runtime_error("CAST_FLOAT 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_CAST_INT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    if (val_is_bigint(a)) {
        // BigInt 转 int48
        ObjBigInt* bigint = val_as_bigint(a);
        int64_t val = bigint_to_int64(bigint);
        if (val >= INT48_MIN && val <= INT48_MAX) {
            vm_stack_push(&vm, val_int(val));
        } else {
            // BigInt 超出 int48 范围，保持原值
            vm_stack_push(&vm, a);
        }
    } else if (val_is_num(a)) {
        vm_stack_push(&vm, val_int((int64_t)val_as_num(a)));
    } else if (val_is_null(a)) {
        vm_stack_push(&vm, a);
    } else {
        throw_runtime_error("CAST_INT 操作数必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}

OPCODE(OP_CAST_STRING) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    if (val_is_null(a)) {
        // null 保持为 null
        vm_stack_push(&vm, a);
    } else if (val_is_string(a)) {
        // 已经是字符串，保持原值
        vm_stack_push(&vm, a);
    } else {
        // 其他类型，转换为字符串表示
        const char* str = val_to_string(a);
        vm_stack_push(&vm, val_obj((struct Object*)str_copy(str, (int)strlen(str))));
    }
    DISPATCH;
}

OPCODE(OP_U8_TO_F64) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value a = vm_stack_pop(&vm);
    // u8 范围 0-255，转换为 f64
    uint8_t u8val = (uint8_t)val_as_int(a);
    vm_stack_push(&vm, val_float((double)u8val));
    DISPATCH;
}

OPCODE(OP_SET_PTR_ELEM_TYPE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    TypeKind elem_type = (TypeKind)READ_BYTE();
    // 栈顶是 FFI 指针，设置其 element_type（不弹出，后续赋值操作会弹出）
    Value top = vm_stack_peek(&vm, 0);
    if (val_is_obj(top) && val_as_obj(top)->type == OBJ_FFI_POINTER) {
        ObjFFIPointer* ptr = (ObjFFIPointer*)val_as_obj(top);
        if (!ptr->freed) {
            ptr->element_type = elem_type;
        }
    }
    DISPATCH;
}

OPCODE(OP_SET_DECLARED_FACE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_const = READ_SHORT();
    Value name_val = frame->chunk->constants[name_const];
    // 栈顶是 struct 实例，设置其 declared_face
    Value top = vm_stack_peek(&vm, 0);
    if (val_is_obj(name_val) && val_as_obj(name_val)->type == OBJ_STRING
        && val_is_obj(top) && val_as_obj(top)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(top);
        obj->declared_face = (ObjString*)val_as_obj(name_val);
    }
    DISPATCH;
}
