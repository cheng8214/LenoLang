// ============================================================================
// VM 辅助工具函数 - 从 vm_utils.c 迁移
// ============================================================================

// 辅助函数：从 ObjType 获取类型名称
static inline const char* obj_type_to_name(ObjType type) {
    switch (type) {
        case OBJ_ARRAY: return "array";
        case OBJ_DICT: return "dict";
        case OBJ_STRING: return "string";
        case OBJ_BIGINT: return "bigint";
        case OBJ_FUNCTION: return "func";
        case OBJ_NATIVE: return "native";
        case OBJ_CLOSURE: return "closure";
        case OBJ_UPVALUE: return "upvalue";
        case OBJ_BOUND_METHOD: return "bound_method";
        case OBJ_MODULE: return "module";
        case OBJ_FILE: return "file";
        default: return "object";
    }
}

// 辅助函数：将 Value 转换为 BigInt（如果已经是 BigInt 则直接返回）
static inline ObjBigInt* promote_to_bigint(Value v) {
    if (val_is_bigint(v)) {
        return val_as_bigint(v);
    }
    // 将数字转换为 BigInt
    return bigint_from_int64((int64_t)val_as_num(v));
}

// 辅助函数：检查是否需要 BigInt 运算（任一操作数是 BigInt）
static inline int needs_bigint_op(Value a, Value b) {
    return val_is_bigint(a) || val_is_bigint(b);
}

// 判断假值
static inline int is_falsey(Value v) {
    return !val_is_truthy(v);
}

// ============================================================================
// 通用错误处理辅助函数
// ============================================================================

// 检查是否为 null，如果是则抛出运行时错误并返回 1
static inline int check_null(Value v, const char* context, int line) {
    if (val_is_null(v)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: null 不能参与算术运算", context);
        throw_runtime_error(msg, line);
        return 1;
    }
    return 0;
}

// 检查两个值是否为 null，如果是则抛出运行时错误并返回 1
static inline int check_null_binary(Value a, Value b, const char* context, int line) {
    if (val_is_null(a) || val_is_null(b)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s: null 不能参与算术运算", context);
        throw_runtime_error(msg, line);
        return 1;
    }
    return 0;
}

// 检查除零，如果是则抛出运行时错误并返回 1
static inline int check_div_zero(int64_t divisor, int line) {
    if (divisor == 0) {
        throw_runtime_error("除零错误", line);
        return 1;
    }
    return 0;
}

// 检查是否为数字类型，如果不是则抛出运行时错误并返回 1
static inline int check_is_number(Value v, const char* context, int line) {
    if (!val_is_num(v)) {
        char msg[256];
        if (val_is_obj(v)) {
            snprintf(msg, sizeof(msg), "%s 的操作数必须是数字，实际是 %s",
                     context, obj_type_to_name(val_as_obj(v)->type));
        } else {
            snprintf(msg, sizeof(msg), "%s 的操作数必须是数字", context);
        }
        throw_runtime_error(msg, line);
        return 1;
    }
    return 0;
}

// ============================================================================
// 局部变量访问函数
// ============================================================================

// 获取局部变量值
static inline Value get_local_var(CallFrame* frame, uint16_t slot, int* error) {
    *error = 0;
    
    if (frame->locals != NULL) {
        if (slot >= frame->local_count) {
            runtime_error("局部变量索引越界", frame->chunk->lines[(int)(frame->ip - frame->chunk->code)]);
            *error = 1;
            return val_null();
        }
        return frame->locals[slot];
    } else {
        return vm.stack[frame->stack_base + slot];
    }
}

// 设置局部变量值
static inline void set_local_var(CallFrame* frame, uint16_t slot, Value val) {
    if (frame->locals != NULL) {
        frame->locals[slot] = val;
    } else {
        vm.stack[frame->stack_base + slot] = val;
    }
}
