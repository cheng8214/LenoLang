// ============================================================================
// VM 辅助工具函数 - 从 vm_utils.c 迁移
// ============================================================================

// 辅助函数：从 ObjType 获取类型名称
static inline const char* obj_type_to_name(ObjType type) {
    switch (type) {
        case OBJ_ARRAY: return "array";
        case OBJ_DICT: return "dict";
        case OBJ_STRING: return "string";
        case OBJ_BIGINT: return "int";
        case OBJ_FUNCTION: return "func";
        case OBJ_NATIVE: return "native";
        case OBJ_CLOSURE: return "closure";
        case OBJ_UPVALUE: return "upvalue";
        case OBJ_BOUND_METHOD: return "bound_method";
        case OBJ_MODULE: return "module";
        case OBJ_FILE: return "file";
        case OBJ_SOCKET: return "socket";
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

// 通用 VM 类型错误函数：输出详细的调试信息
// 包括操作数类型/值、调用栈等，便于排查栈损坏和类型错误
// 用法：throw_vm_type_error("操作数必须是数字", "ADD (+)", a, b, line);
static void throw_vm_type_error(const char* msg, const char* op_name, Value a_val, Value b_val, int line) {
    char err_buf[BUFFER_XXLARGE];
    char a_short[128], b_short[128];
    const char* fn_name = "<main>";
    const char* file = "unknown";
    int src_line = 0;
    int ip_offset = 0;

    CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
    if (frame->closure && frame->closure->function && frame->closure->function->name) {
        fn_name = frame->closure->function->name;
    } else if (frame->closure && frame->closure->function) {
        fn_name = "<anonymous>";
    }
    if (frame->chunk && frame->chunk->filename) {
        file = frame->chunk->filename;
    }
    if (frame->chunk && frame->chunk->code && frame->ip) {
        ip_offset = (int)(frame->ip - frame->chunk->code);
        if (ip_offset >= 0 && ip_offset < frame->chunk->len && frame->chunk->lines) {
            src_line = frame->chunk->lines[ip_offset];
        }
    }

    value_to_short_str(a_val, a_short, sizeof(a_short));
    value_to_short_str(b_val, b_short, sizeof(b_short));

    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [内部] %s\n"
        "  [位置] %s:%d (func: %s)\n"
        "  [操作数a] type=%s, value=%s\n"
        "  [操作数b] type=%s, value=%s\n",
        msg,
        op_name_to_human(op_name), op_name,
        file, src_line > 0 ? src_line : line, fn_name,
        value_type_desc(a_val), a_short,
        value_type_desc(b_val), b_short);

    // 添加调用栈回溯
    pos = append_stack_trace(err_buf, pos, sizeof(err_buf));

    throw_runtime_error(err_buf, line);
}

// 检查是否为 null，如果是则抛出运行时错误并返回 1
static inline int check_null(Value v, const char* context, int line) {
    if (val_is_null(v)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s: null 不能参与运算", context);
        throw_runtime_error_with_operand(msg, context, v, line);
        return 1;
    }
    return 0;
}

// 检查两个值是否为 null，如果是则抛出运行时错误并返回 1
static inline int check_null_binary(Value a, Value b, const char* context, int line) {
    if (val_is_null(a) || val_is_null(b)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s: null 不能参与运算", context);
        throw_runtime_error_with_operands(msg, context, a, b, line);
        return 1;
    }
    return 0;
}

// 检查除零，如果是则抛出运行时错误并返回 1
static inline int check_div_zero(int64_t divisor, int line) {
    if (divisor == 0) {
        throw_runtime_error("除零错误：除数为 0，不能进行除法运算", line);
        return 1;
    }
    return 0;
}

// 检查是否为数字类型，如果不是则抛出运行时错误并返回 1
static inline int check_is_number(Value v, const char* context, int line) {
    if (!val_is_num(v)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s 的操作数必须是数字", context);
        throw_runtime_error_with_operand(msg, context, v, line);
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
