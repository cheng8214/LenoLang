// ============================================================================
// 崩溃兜底：把"当前执行点"写进 buf（2026-10-25）
// ----------------------------------------------------------------------------
// 谁调用：error.c 的崩溃处理（Windows 的 SetUnhandledExceptionFilter / POSIX 的 signal）。
//   **野指针**在 C 层就是访问违例 —— 原来只报"未处理的系统异常，异常码 0xC0000005"，
//   用户不知道是**哪一行 Leno 代码**引出来的 ✗ ⇒ VM 初始化时注册本函数，
//   崩溃那一刻把 函数名 / 文件:行 / 当前指令 补进报错 ✓
// ⚠ 崩溃上下文里**不能** malloc / stdio / snprintf（会在崩溃里二次死掉）
//   ⇒ 这里只做定长手写拼接 ✓
// ⚠ 所有指针都判空后**逐层**取：崩溃时 vm 可能半初始化（比如崩在 vm_init 里）。
extern const char* opcode_name(int op);   // debug.c（sources_core 里，编译器/VM 都有 ✓）

static void crash_put(char** p, char* end, const char* s) {
    while (s && *s && *p < end) *(*p)++ = *s++;
}
static void crash_put_int(char** p, char* end, int v) {
    char tmp[12];
    int n = 0;
    if (v < 0) { crash_put(p, end, "-"); v = -v; }
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < 11) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n-- > 0) { if (*p < end) *(*p)++ = tmp[n]; }
}

void vm_crash_location(char* buf, int bufsz, int* out_len) {
    if (out_len) *out_len = 0;
    if (!buf || bufsz < 8) return;
    char* p = buf;
    char* end = buf + bufsz - 1;
    if (vm.frame_cnt <= 0 || vm.frames == NULL) { *buf = '\0'; return; }
    CallFrame* f = &vm.frames[vm.frame_cnt - 1];
    if (f == NULL || f->chunk == NULL) { *buf = '\0'; return; }
    const char* fname = "?";
    if (f->closure && f->closure->function && f->closure->function->name) {
        fname = f->closure->function->name;   // ObjFunction.name 是 const char*（不是 ObjString）
    }
    const char* file = f->chunk->filename ? f->chunk->filename : "?";
    int line = -1;
    int op = -1;
    if (f->chunk->code && f->chunk->lines && f->ip) {
        int off = (int)(f->ip - f->chunk->code) - 4;   // ip 已跳过当前指令（见 GET_CURRENT_LINE）
        if (off >= 0 && off < f->chunk->len) {
            line = f->chunk->lines[off];
            op = f->chunk->code[off];
        }
    }
    crash_put(&p, end, "  @ 函数 ");
    crash_put(&p, end, fname);
    crash_put(&p, end, " / ");
    crash_put(&p, end, file);
    if (line > 0) { crash_put(&p, end, ":"); crash_put_int(&p, end, line); }
    if (op >= 0 && op < OP_OPCODE_COUNT) {
        crash_put(&p, end, "（当前指令 ");
        crash_put(&p, end, opcode_name(op));
        crash_put(&p, end, "）");
    }
    crash_put(&p, end, "\n");
    *p = '\0';
    if (out_len) *out_len = (int)(p - buf);
}

// ============================================================================
// VM 辅助工具函数 - 从 vm_utils.c 迁移
// ============================================================================

// ★ 2026-10-24：把值（连同它内部的容器）递归标成**只读** —— const 深保护的唯一实现。
//   谁调用：OP_LOCK_REF（`const A = [...]` / `const s = new S()` 的声明处，跑一次）。
//   为什么必须**递归**：`const s9 = new S9()`（S9 有 Array 字段）只标 struct 本身的话，
//     `tryMut(s9)` 里 `s.arr.add(1)` 改的是**另一个对象**（那个数组）⇒ 漏保护 ✗（实测过）。
//   ⚠ 幂等 + 环守卫二合一：已标过就立刻 return —— 既省重复递归，也让 `a[0] = a` 这类
//     自引用结构不会无限下去 ✓；depth 只是额外的兜底上限。
//   ⚠ 字典用 order 数组取活键再 dict_get：直接扫 entries 会撞上墓碑值
//     （DICT_TOMBSTONE_VAL 是特殊 NaN 盒，拿去 val_is_obj 判会误判）。
static void lock_value_deep(Value v, int depth) {
    if (depth < 0 || !val_is_obj(v)) return;
    Object* obj = val_as_obj(v);
    if (obj->flags & OBJ_FLAG_READONLY) return;
    switch (obj->type) {
        case OBJ_ARRAY: {
            obj->flags |= OBJ_FLAG_READONLY;
            ObjArray* arr = (ObjArray*)obj;
            for (int i = 0; i < arr->count; i++) {
                lock_value_deep(arr->elements[i], depth - 1);
            }
            break;
        }
        case OBJ_DICT: {
            obj->flags |= OBJ_FLAG_READONLY;
            ObjDict* d = (ObjDict*)obj;
            if (d->array) {
                for (int i = 0; i < d->asize; i++) lock_value_deep(d->array[i], depth - 1);
            }
            if (d->order && d->order_count > 0) {
                for (int i = 0; i < d->order_count; i++) {
                    lock_value_deep(dict_get(d, d->order[i]), depth - 1);
                }
            }
            break;
        }
        case OBJ_STRUCT: {
            obj->flags |= OBJ_FLAG_READONLY;
            ObjStruct* so = (ObjStruct*)obj;
            if (so->def && so->field_values) {
                for (int i = 0; i < so->def->field_count; i++) {
                    lock_value_deep(so->field_values[i], depth - 1);
                }
            }
            break;
        }
        default:
            break;   // 其余对象本身不可变（字符串 / bigint / 闭包 / 模块 …）
    }
}

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
// 返回：同 throw_runtime_error（1 = 已接管，0 = 未捕获）
static int throw_vm_type_error(const char* msg, const char* op_name, Value a_val, Value b_val, int line) {
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

    return throw_runtime_error(err_buf, line);
}

// ---------------------------------------------------------------------------
// 以下 check_* 统一返回：1 = 可以继续执行（无错误，或异常已被 catch/finally 接管，
// 调用方重取 frame 后 DISPATCH）；0 = 未捕获的致命错误（调用方应停机 return -1）。
// ---------------------------------------------------------------------------

// 检查是否为 null，如果是则抛出运行时错误
static inline int check_null(Value v, const char* context, int line) {
    if (val_is_null(v)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s: null 不能参与运算", context);
        return throw_runtime_error_with_operand(msg, context, v, line);
    }
    return 1;
}

// 检查两个值是否为 null，如果是则抛出运行时错误
static inline int check_null_binary(Value a, Value b, const char* context, int line) {
    if (val_is_null(a) || val_is_null(b)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s: null 不能参与运算", context);
        return throw_runtime_error_with_operands(msg, context, a, b, line);
    }
    return 1;
}

// 检查除零，如果是则抛出运行时错误
static inline int check_div_zero(int64_t divisor, int line) {
    if (divisor == 0) {
        return throw_runtime_error("除零错误：除数为 0，不能进行除法运算", line);
    }
    return 1;
}

// 检查是否为数字类型，如果不是则抛出运行时错误
static inline int check_is_number(Value v, const char* context, int line) {
    if (!val_is_num(v)) {
        char msg[BUFFER_LARGE];
        snprintf(msg, sizeof(msg), "%s 的操作数必须是数字", context);
        return throw_runtime_error_with_operand(msg, context, v, line);
    }
    return 1;
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
