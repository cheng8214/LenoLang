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
// 包括操作数类型/值、栈状态、函数名、调用栈等，便于排查栈损坏和类型错误
// 用法：throw_vm_type_error("操作数必须是数字", "ADD (+)", a, b, line);
// 注意：此为函数而非宏，避免在热路径中代码膨胀影响 icache
static void throw_vm_type_error(const char* msg, const char* op_name, Value a_val, Value b_val, int line) {
    char err_buf[1024];
    const char* fn_name = "<main>";
    CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
    if (frame->closure && frame->closure->function && frame->closure->function->name) {
        fn_name = frame->closure->function->name;
    } else if (frame->closure && frame->closure->function && !frame->closure->function->name) {
        fn_name = "<anonymous>";
    }
    const char* file = (frame->chunk && frame->chunk->filename) ? frame->chunk->filename : "unknown";
    char* a_str = value_to_string(a_val);
    char* b_str = value_to_string(b_val);
    int ip_offset = (int)(frame->ip - frame->chunk->code);
    int stack_depth = vm.sp - frame->stack_base;
    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [位置] %s:%d (func: %s, ip_offset: %d)\n"
        "  [操作数a] type=%s, value=%s\n"
        "  [操作数b] type=%s, value=%s\n"
        "  [栈状态] sp=%d, stack_base=%d, depth=%d, frame_cnt=%d\n",
        msg, op_name, file, line, fn_name, ip_offset,
        value_type_desc(a_val), a_str ? a_str : "<null>",
        value_type_desc(b_val), b_str ? b_str : "<null>",
        vm.sp, frame->stack_base, stack_depth, vm.frame_cnt);
    if (stack_depth > 0 && vm.sp > 0) {
        pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
            "  [栈内容] ");
        for (int i = (vm.sp > 8 ? vm.sp - 8 : 0); i < vm.sp; i++) {
            char* sv = value_to_string(vm.stack[i]);
            pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
                "[%d]=%s:%s ", i, value_type_desc(vm.stack[i]), sv ? sv : "<null>");
            free(sv);
        }
    }
    if (frame->locals && frame->local_count > 0) {
        pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
            "\n  [locals] ");
        for (int i = 0; i < frame->local_count && i < 8; i++) {
            char* lv = value_to_string(frame->locals[i]);
            pos += snprintf(err_buf + pos, sizeof(err_buf) - pos,
                "[%d]=%s:%s ", i, value_type_desc(frame->locals[i]), lv ? lv : "<null>");
            free(lv);
        }
    }
    free(a_str); free(b_str);
    throw_runtime_error(err_buf, line);
}

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
