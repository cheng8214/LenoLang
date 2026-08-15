// ============================================================================
// 通用算术运算类操作码 (OP_ARITH)
// ============================================================================
// 基本算术运算：加、减、乘、除、取模
// int 为 48 位内联表示，溢出时自动提升为 BigInt

OPCODE(OP_ADD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop_fast(&vm);
    Value a = vm_stack_pop_fast(&vm);

    if (likely(val_is_int(a) && val_is_int(b))) {
        int64_t a_val = val_as_int(a);
        int64_t b_val = val_as_int(b);
        PUSH_SAFE_INT64_ADD(a_val, b_val);
        DISPATCH;
    }
    
    // 检查是否是字符串拼接
    if ((val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING) ||
        (val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING)) {
        // 快速路径：两个操作数都是 ObjString，直接使用 str_concat（正确处理 null 字节）
        if (val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING &&
            val_is_obj(b) && val_as_obj(b)->type == OBJ_STRING) {
            ObjString* result = str_concat((ObjString*)val_as_obj(a), (ObjString*)val_as_obj(b));
            vm_stack_push(&vm, val_obj((Object*)result));
        } else {
            // 一个操作数是 ObjString，另一个需要转换（转换结果不含 null 字节，strlen 安全）
            bool a_is_str = val_is_obj(a) && val_as_obj(a)->type == OBJ_STRING;
            ObjString* str_obj = a_is_str ? (ObjString*)val_as_obj(a) : (ObjString*)val_as_obj(b);
            Value other = a_is_str ? b : a;
            char* other_str = value_to_string(other);
            int other_len = (int)strlen(other_str);
            int total_len = str_obj->len + other_len;
            char* buf = (char*)malloc(total_len + 1);
            if (a_is_str) {
                memcpy(buf, str_obj->chars, str_obj->len);
                memcpy(buf + str_obj->len, other_str, other_len);
            } else {
                memcpy(buf, other_str, other_len);
                memcpy(buf + other_len, str_obj->chars, str_obj->len);
            }
            buf[total_len] = '\0';
            ObjString* result = str_new(buf, total_len);
            free(buf);
            free(other_str);
            vm_stack_push(&vm, val_obj((Object*)result));
        }
        DISPATCH;
    }

    // float 路径
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) + val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }

    // BigInt 运算（显式 Bint 类型参与）
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_add(ba, bb);
        vm_stack_push(&vm, result);
        DISPATCH;
    }
    
    // null 检查
    if (val_is_null(a) || val_is_null(b)) {
        throw_runtime_error("null 不能参与算术运算", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // 其他类型，尝试转换为字符串后拼接
    char* strA = value_to_string(a);
    char* strB = value_to_string(b);
    int lenA = (int)strlen(strA);
    int lenB = (int)strlen(strB);
    char* buf = (char*)malloc(lenA + lenB + 1);
    memcpy(buf, strA, lenA);
    memcpy(buf + lenA, strB, lenB);
    buf[lenA + lenB] = '\0';
    ObjString* result = str_new(buf, lenA + lenB);
    free(buf);
    free(strA);
    free(strB);
    vm_stack_push(&vm, val_obj((Object*)result));
    DISPATCH;
}

OPCODE(OP_SUB) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop_fast(&vm);
    Value a = vm_stack_pop_fast(&vm);

    if (likely(val_is_int(a) && val_is_int(b))) {
        int64_t a_val = val_as_int(a);
        int64_t b_val = val_as_int(b);
        PUSH_SAFE_INT64_SUB(a_val, b_val);
        DISPATCH;
    }

    // float 路径
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) - val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }

    // null 检查
    if (check_null_binary(a, b, "减法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 运算
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_sub(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_vm_type_error("操作数必须是数字", "SUB (-)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
    }
    DISPATCH;
}

OPCODE(OP_MUL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);

    // int * int：溢出时自动提升为 BigInt
    if (val_is_int(a) && val_is_int(b)) {
        int64_t a_val = val_as_int(a);
        int64_t b_val = val_as_int(b);
        PUSH_SAFE_INT64_MUL(a_val, b_val);
        DISPATCH;
    }
    
    // float 路径
    if (val_is_float(a) || val_is_float(b)) {
        double result = val_as_num_ex(a) * val_as_num_ex(b);
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }
    
    // null 检查
    if (check_null_binary(a, b, "乘法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 运算
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_mul(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_vm_type_error("操作数必须是数字", "MUL (*)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
    }
    DISPATCH;
}

OPCODE(OP_DIV) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    
    // int / int = int（整数除法，向零取整）
    if (val_is_int(a) && val_is_int(b)) {
        int64_t b_val = val_as_int(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        int64_t a_val = val_as_int(a);
        vm_stack_push(&vm, val_int(a_val / b_val));
        DISPATCH;
    }
    
    // float / 任意 = float
    if (val_is_float(a) || val_is_float(b)) {
        double b_val = val_as_num_ex(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        double result = val_as_num_ex(a) / b_val;
        vm_stack_push(&vm, val_float(result));
        DISPATCH;
    }
    
    // BigInt / BigInt
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        if (val_is_bigint(b)) {
            ObjBigInt* bb = val_as_bigint(b);
            if (bb->limb_count == 1 && bb->limbs[0] == 0) {
                throw_runtime_error("除零错误", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
        } else if (val_as_num(b) == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        Value result = bigint_div(ba, bb);
        vm_stack_push(&vm, result);
        DISPATCH;
    }
    
    if (check_null_binary(a, b, "除法", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    throw_vm_type_error("操作数必须是数字", "DIV (/)", a, b, GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_MOD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value b = vm_stack_pop(&vm);
    Value a = vm_stack_pop(&vm);
    
    // int % int
    if (val_is_int(a) && val_is_int(b)) {
        int64_t b_val = val_as_int(b);
        if (b_val == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        vm_stack_push(&vm, val_int(val_as_int(a) % b_val));
        DISPATCH;
    }
    
    // null 检查
    if (check_null_binary(a, b, "取模", GET_CURRENT_LINE())) {
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // BigInt 取模
    if (unlikely(val_is_bigint(a) || val_is_bigint(b))) {
        ObjBigInt* ba = promote_to_bigint(a);
        ObjBigInt* bb = promote_to_bigint(b);
        if (bb->limb_count == 1 && bb->limbs[0] == 0) {
            throw_runtime_error("除零错误", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        Value result = bigint_mod(ba, bb);
        vm_stack_push(&vm, result);
    } else {
        throw_vm_type_error("取模操作数必须是整数", "MOD (%)", a, b, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    DISPATCH;
}
