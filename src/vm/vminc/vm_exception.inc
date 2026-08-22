// ============================================================================
// 异常处理 - 从 vm_exception.c 迁移
// ============================================================================

// 辅助函数：获取Value的类型描述
static inline const char* value_type_desc(Value val) {
    switch (val_get_type(val)) {
        case VAL_NULL: return "null";
        case VAL_BOOL: return "bool";
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_OBJ: {
            switch (val_as_obj(val)->type) {
                case OBJ_STRING: return "string";
                case OBJ_ARRAY: return "array";
                case OBJ_DICT: return "dict";
                case OBJ_STRUCT: {
                    ObjStruct* s = (ObjStruct*)val_as_obj(val);
                    return s->def ? s->def->name : "struct";
                }
                case OBJ_FUNCTION: return "function";
                case OBJ_CLOSURE: return "closure";
                case OBJ_NATIVE: return "native";
                case OBJ_BIGINT: return "int";
                case OBJ_MODULE: return "module";
                case OBJ_SOCKET: return "socket";
                default: return "object";
            }
        }
        default: return "unknown";
    }
}

// 辅助函数：将 Value 转换为简短的调试字符串（用于报错信息）
// 截断长字符串、显示数组长度、显示结构体名称等，避免报错信息本身导致二次崩溃
static inline void value_to_short_str(Value val, char* buf, size_t buf_size) {
    switch (val_get_type(val)) {
        case VAL_NULL:
            snprintf(buf, buf_size, "null");
            break;
        case VAL_BOOL:
            snprintf(buf, buf_size, "%s", val_as_bool(val) ? "true" : "false");
            break;
        case VAL_INT:
            snprintf(buf, buf_size, "%lld", (long long)val_as_int(val));
            break;
        case VAL_FLOAT:
            snprintf(buf, buf_size, "%g", val_as_num(val));
            break;
        case VAL_OBJ: {
            Object* obj = val_as_obj(val);
            switch (obj->type) {
                case OBJ_STRING: {
                    ObjString* s = (ObjString*)obj;
                    size_t len = s->len;
                    if (len <= 32) {
                        snprintf(buf, buf_size, "\"%.*s\"", (int)len, s->chars);
                    } else {
                        snprintf(buf, buf_size, "\"%.*s...\" (len=%zu)", 28, s->chars, len);
                    }
                    break;
                }
                case OBJ_ARRAY: {
                    ObjArray* arr = (ObjArray*)obj;
                    snprintf(buf, buf_size, "array(len=%d)", arr->count);
                    break;
                }
                case OBJ_DICT: {
                    ObjDict* d = (ObjDict*)obj;
                    snprintf(buf, buf_size, "dict(count=%d)", d->count);
                    break;
                }
                case OBJ_STRUCT: {
                    ObjStruct* s = (ObjStruct*)obj;
                    snprintf(buf, buf_size, "struct(%s)", s->def ? s->def->name : "unknown");
                    break;
                }
                case OBJ_FUNCTION: {
                    ObjFunction* f = (ObjFunction*)obj;
                    snprintf(buf, buf_size, "func(%s)", f->name ? f->name : "<anonymous>");
                    break;
                }
                case OBJ_CLOSURE:
                    snprintf(buf, buf_size, "closure");
                    break;
                case OBJ_NATIVE:
                    snprintf(buf, buf_size, "native");
                    break;
                case OBJ_BIGINT: {
                    char* bs = bigint_to_string(val_as_bigint(val));
                    if (bs) {
                        snprintf(buf, buf_size, "%s", bs);
                        free(bs);
                    } else {
                        snprintf(buf, buf_size, "bigint(?)");
                    }
                    break;
                }
                case OBJ_MODULE:
                    snprintf(buf, buf_size, "module");
                    break;
                case OBJ_SOCKET:
                    snprintf(buf, buf_size, "socket");
                    break;
                case OBJ_FFI_POINTER:
                    snprintf(buf, buf_size, "ffi_ptr");
                    break;
                case OBJ_FFI_CALLBACK:
                    snprintf(buf, buf_size, "ffi_callback");
                    break;
                default:
                    snprintf(buf, buf_size, "object(type=%d)", obj->type);
                    break;
            }
            break;
        }
        default:
            snprintf(buf, buf_size, "unknown");
            break;
    }
}

// 运行时错误（不抛出异常，直接报错）
static inline void runtime_error(const char* msg, int line) {
    // 获取当前调用帧的 chunk 文件名
    const char* filename = NULL;
    if (vm.frame_cnt > 0) {
        CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
        if (frame->chunk && frame->chunk->filename) {
            filename = frame->chunk->filename;
        }
    }
    
    // 临时设置文件名用于错误报告
    const char* old_filename = error_get_filename();
    if (filename) {
        error_set_filename(filename);
    }
    
    error_add_at(ERR_RUNTIME, line, 0, msg);
    
    // 恢复原来的文件名
    error_set_filename(old_filename);
}

// ============================================================================
// 调用栈追踪 (Stack Trace)
// ============================================================================

// 构建调用栈数组，每帧格式: "func_name (file:line)"
// 从最内层帧（当前执行帧）到最外层帧（main）排列
static ObjArray* build_stack_trace(void) {
    ObjArray* stack_arr = arr_new(vm.frame_cnt);
    if (!stack_arr) return NULL;

    for (int i = vm.frame_cnt - 1; i >= 0; i--) {
        CallFrame* f = &vm.frames[i];

        // 获取函数名
        const char* func_name = NULL;
        if (f->closure && f->closure->function && f->closure->function->name) {
            func_name = f->closure->function->name;
        } else if (i == 0) {
            func_name = "<main>";
        } else {
            func_name = "<anonymous>";
        }

        // 获取文件名
        const char* filename = "unknown";
        if (f->chunk && f->chunk->filename) {
            filename = f->chunk->filename;
        }

        // 获取行号（当前 IP 对应的行）
        int line = 0;
        if (f->chunk && f->chunk->lines && f->ip && f->chunk->code) {
            int offset = (int)(f->ip - f->chunk->code);
            if (offset >= 0 && offset < f->chunk->len) {
                line = f->chunk->lines[offset];
            }
        }

        // 拼接 "func_name (file:line)"
        char frame_desc[BUFFER_XLARGE];
        snprintf(frame_desc, sizeof(frame_desc), "%s (%s:%d)", func_name, filename, line);
        ObjString* frame_str = str_copy(frame_desc, strlen(frame_desc));
        if (frame_str) {
            arr_write(stack_arr, stack_arr->count, val_obj((Object*)frame_str));
        }
    }
    return stack_arr;
}

// 打印未捕获异常的调用栈
// exception: 异常值（可能是 Dict 或字符串）
// line: 异常发生时的行号
static void print_unhandled_exception(Value exception, int line) {
    const char* msg = "未知错误";
    const char* filename = "unknown";
    ObjArray* stack_arr = NULL;
    int has_stack = 0;

    // 尝试从异常 Dict 中提取信息
    if (val_is_obj(exception) && val_as_obj(exception)->type == OBJ_DICT) {
        ObjDict* err_dict = (ObjDict*)val_as_obj(exception);

        // 获取 msg
        ObjString* msg_key = str_copy("msg", 3);
        Value msg_val = dict_get(err_dict, val_obj((Object*)msg_key));
        if (val_is_string(msg_val)) {
            msg = ((ObjString*)val_as_obj(msg_val))->chars;
        }

        // 获取 file
        ObjString* file_key = str_copy("file", 4);
        Value file_val = dict_get(err_dict, val_obj((Object*)file_key));
        if (val_is_string(file_val)) {
            filename = ((ObjString*)val_as_obj(file_val))->chars;
        }

        // 获取 line
        ObjString* line_key = str_copy("line", 4);
        Value line_val = dict_get(err_dict, val_obj((Object*)line_key));
        if (val_is_int(line_val)) {
            line = (int)val_as_int(line_val);
        }

        // 获取 stack
        ObjString* stack_key = str_copy("stack", 5);
        Value stack_val = dict_get(err_dict, val_obj((Object*)stack_key));
        if (val_is_obj(stack_val) && val_as_obj(stack_val)->type == OBJ_ARRAY) {
            stack_arr = (ObjArray*)val_as_obj(stack_val);
            has_stack = 1;
        }
    } else {
        // 字符串异常：直接使用 value_to_string
        char* str = value_to_string(exception);
        if (str) {
            // 使用静态缓冲区避免内存管理问题
            static char msg_buf[BUFFER_XXLARGE];
            snprintf(msg_buf, sizeof(msg_buf), "%s", str);
            msg = msg_buf;
            free(str);
        }

        // 获取当前文件名
        if (vm.frame_cnt > 0) {
            CallFrame* f = &vm.frames[vm.frame_cnt - 1];
            if (f->chunk && f->chunk->filename) {
                filename = f->chunk->filename;
            }
        }
    }

    // 如果异常 Dict 中没有 stack（如 throw "string"），实时构建
    if (!has_stack) {
        stack_arr = build_stack_trace();
        has_stack = (stack_arr != NULL);
    }

    // 打印错误信息和调用栈
    fprintf(stderr, "[错误] %s:%d: %s\n", filename, line, msg);
    if (has_stack && stack_arr && stack_arr->count > 0) {
        fprintf(stderr, "调用栈:\n");
        for (int i = 0; i < stack_arr->count; i++) {
            if (val_is_string(stack_arr->elements[i])) {
                fprintf(stderr, "    at %s\n", ((ObjString*)val_as_obj(stack_arr->elements[i]))->chars);
            }
        }
    }
}

// 将字符串异常包装为异常 Dict 对象
// 用于 throw "string" 时自动包装为含 msg/line/file/stack 的异常对象
static Value wrap_string_exception(Value str_val, int line) {
    if (!val_is_string(str_val)) return str_val;

    ObjDict* err_dict = dict_new(8);
    if (!err_dict) return str_val;  // 内存不足时回退为原始字符串

    // msg 属性
    ObjString* msg_key = str_copy("msg", 3);
    if (msg_key) {
        dict_set(err_dict, val_obj((Object*)msg_key), str_val);
    }

    // line 属性
    ObjString* line_key = str_copy("line", 4);
    if (line_key) {
        dict_set(err_dict, val_obj((Object*)line_key), val_int(line));
    }

    // file 属性
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    const char* filename = (current_frame->chunk && current_frame->chunk->filename)
                           ? current_frame->chunk->filename : "unknown";
    ObjString* file_key = str_copy("file", 4);
    ObjString* file_val = str_copy(filename, (int)strlen(filename));
    if (file_key && file_val) {
        dict_set(err_dict, val_obj((Object*)file_key), val_obj((Object*)file_val));
    }

    // stack 属性
    ObjArray* stack_arr = build_stack_trace();
    if (stack_arr) {
        ObjString* stack_key = str_copy("stack", 5);
        if (stack_key) {
            dict_set(err_dict, val_obj((Object*)stack_key), val_obj((Object*)stack_arr));
        }
    }

    return val_obj((Object*)err_dict);
}

// 抛出运行时异常（用于 try-catch 捕获）
// 设置异常状态并跳转到 catch/finally 块
static inline void throw_runtime_error(const char* msg, int line) {
    // 创建异常对象（使用字典表示，包含 msg、line、file、stack 属性）
    ObjDict* err_dict = dict_new(8);
    if (!err_dict) {
        // 内存分配失败，直接报错
        runtime_error(msg, line);
        return;
    }

    // 添加 msg 属性
    ObjString* msg_key = str_copy("msg", 3);
    ObjString* msg_val = str_copy(msg, strlen(msg));
    if (!msg_key || !msg_val) {
        runtime_error(msg, line);
        return;
    }
    dict_set(err_dict, val_obj((Object*)msg_key), val_obj((Object*)msg_val));

    // 添加 line 属性
    ObjString* line_key = str_copy("line", 4);
    if (!line_key) {
        runtime_error(msg, line);
        return;
    }
    dict_set(err_dict, val_obj((Object*)line_key), val_int(line));

    // 添加 file 属性（当前文件名）
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    const char* filename = current_frame->chunk->filename ? current_frame->chunk->filename : "unknown";
    ObjString* file_key = str_copy("file", 4);
    ObjString* file_val = str_copy(filename, strlen(filename));
    if (file_key && file_val) {
        dict_set(err_dict, val_obj((Object*)file_key), val_obj((Object*)file_val));
    }

    // 添加 stack 属性（调用栈，每帧包含 "func_name (file:line)" 信息）
    ObjArray* stack_arr = build_stack_trace();
    if (stack_arr) {
        ObjString* stack_key = str_copy("stack", 5);
        if (stack_key) {
            dict_set(err_dict, val_obj((Object*)stack_key), val_obj((Object*)stack_arr));
        }
    }

    // 设置异常状态
    vm.exception = val_obj((Object*)err_dict);
    vm.has_exception = 1;
    vm.exception_line = line;  // 保存异常发生时的行号

    // 从当前调用帧开始向上查找有 catch/finally 的帧
    for (int i = vm.frame_cnt - 1; i >= 0; i--) {
        CallFrame* frame = &vm.frames[i];
        // 快速路径：如果函数确定没有 try，直接跳过
        // 注意：如果 closure 为 NULL（如顶层代码），不能跳过，需要检查 catch_ip
        if (frame->closure && frame->closure->function && !frame->closure->function->has_try) {
            continue;
        }
        // 检查是否有异常处理（包括顶层代码的 try-catch）
        if (frame->catch_ip || frame->finally_ip) {
            // 找到了有异常处理的帧，需要展开调用栈到这个帧
            // 释放当前帧到目标帧之间的所有帧的locals（仅当使用动态分配时才需要 free）
            for (int j = vm.frame_cnt - 1; j > i; j--) {
                CallFrame* unwind_frame = &vm.frames[j];
                if (unwind_frame->locals && unwind_frame->locals_is_dynamic) {
                    free(unwind_frame->locals);
                    unwind_frame->locals = NULL;
                }
            }
            // 设置帧计数为目标帧
            vm.frame_cnt = i + 1;

            // 恢复栈指针到目标帧的stack_base
            vm.sp = frame->stack_base;

            // 处理异常
            if (frame->catch_ip) {
                // 跳转到 catch 块
                frame->ip = frame->catch_ip;
                // 将异常值压入栈，供 catch 使用
                vm_stack_push(&vm, vm.exception);
                // 注意：不清除 vm.has_exception，让调用者知道异常已处理
                // OP_CATCH 会清除异常标志
            } else {
                // 没有 catch，但有 finally，先执行 finally
                // 标记需要在 finally 后传播异常
                vm.pending_exception = 1;
                frame->ip = frame->finally_ip;
            }
            return;
        }
    }

    // 没有找到任何异常处理，打印完整调用栈并设置异常标志让 VM 停止
    print_unhandled_exception(vm.exception, line);
    runtime_error(msg, line);
    vm.has_exception = 1;
}

// ============================================================================
// 带操作数上下文的异常抛出函数
// ============================================================================

// 操作码人性化名称映射
// op_name 是调用方传入的内部操作码名称（如 "ADD (+)"、"INC_LOCAL_NOPUSH (++)"）
// 返回对应的中文人性化描述
static inline const char* op_name_to_human(const char* op_name) {
    if (!op_name) return "未知操作";
    // 按操作类型匹配关键词
    if (strstr(op_name, "ADD"))       return "加法 (+)";
    if (strstr(op_name, "SUB"))      return "减法 (-)";
    if (strstr(op_name, "MUL"))      return "乘法 (*)";
    if (strstr(op_name, "DIV_INT"))  return "整数除法 (/)";
    if (strstr(op_name, "DIV_FLOAT"))return "浮点除法 (/)";
    if (strstr(op_name, "DIV"))      return "除法 (/)";
    if (strstr(op_name, "MOD_INT"))  return "取模 (%)";
    if (strstr(op_name, "MOD"))      return "取模 (%)";
    if (strstr(op_name, "BITAND"))   return "按位与 (&)";
    if (strstr(op_name, "BITOR"))    return "按位或 (|)";
    if (strstr(op_name, "BITXOR"))   return "按位异或 (^)";
    if (strstr(op_name, "BITNOT"))   return "按位非 (~)";
    if (strstr(op_name, "SHL_IMM"))  return "左移 (<<)";
    if (strstr(op_name, "SHL"))      return "左移 (<<)";
    if (strstr(op_name, "SHR_IMM"))  return "右移 (>>)";
    if (strstr(op_name, "SHR"))      return "右移 (>>)";
    if (strstr(op_name, "USHR_IMM")) return "逻辑右移 (>>>)";
    if (strstr(op_name, "USHR"))     return "逻辑右移 (>>>)";
    if (strstr(op_name, "PRE_INC"))  return "前缀自增 (++)";
    if (strstr(op_name, "PRE_DEC"))  return "前缀自减 (--)";
    if (strstr(op_name, "INC_LOCAL_NOPUSH")) return "自增 (++)";
    if (strstr(op_name, "DEC_LOCAL_NOPUSH")) return "自减 (--)";
    if (strstr(op_name, "INC_LOCAL"))return "后缀自增 (++)";
    if (strstr(op_name, "DEC_LOCAL"))return "后缀自减 (--)";
    if (strstr(op_name, "INC"))      return "自增 (++)";
    if (strstr(op_name, "DEC"))      return "自减 (--)";
    if (strstr(op_name, "NEG"))      return "取负 (-)";
    if (strstr(op_name, "CAST_FLOAT")) return "类型转换 (→float)";
    if (strstr(op_name, "CAST_INT"))   return "类型转换 (→int)";
    return op_name;  // 未知操作码，原样返回
}

// 获取当前帧的源码位置信息
// 将 ip_offset 映射回 chunk->lines 的行号，并获取文件名和函数名
static inline void get_current_context(
    const char** func_name, int* ip_offset,
    int* src_line, const char** filename) {

    *func_name = "<main>";
    *ip_offset = 0;
    *src_line = 0;
    *filename = "unknown";

    if (vm.frame_cnt > 0) {
        CallFrame* frame = &vm.frames[vm.frame_cnt - 1];
        // 函数名
        if (frame->closure && frame->closure->function) {
            if (frame->closure->function->name) {
                *func_name = frame->closure->function->name;
            } else {
                *func_name = "<anonymous>";
            }
        }
        // 文件名
        if (frame->chunk && frame->chunk->filename) {
            *filename = frame->chunk->filename;
        }
        // IP 偏移和源码行号
        if (frame->chunk && frame->chunk->code && frame->ip) {
            *ip_offset = (int)(frame->ip - frame->chunk->code);
            if (*ip_offset >= 0 && *ip_offset < frame->chunk->len &&
                frame->chunk->lines) {
                *src_line = frame->chunk->lines[*ip_offset];
            }
        }
    }
}

// 构建调用栈回溯字符串（追加到 err_buf 中 pos 位置之后）
// 格式：
//   调用栈:
//     #0 main @ test.leno:12
//     #1 foo  @ test.leno:8
static inline int append_stack_trace(char* buf, int pos, size_t buf_size) {
    if (vm.frame_cnt <= 0) return pos;

    pos += snprintf(buf + pos, buf_size - pos, "  [调用栈]\n");

    for (int i = vm.frame_cnt - 1; i >= 0; i--) {
        CallFrame* f = &vm.frames[i];

        // 函数名
        const char* fn = "<anonymous>";
        if (f->closure && f->closure->function && f->closure->function->name) {
            fn = f->closure->function->name;
        } else if (i == 0) {
            fn = "<main>";
        }

        // 文件名
        const char* file = "unknown";
        if (f->chunk && f->chunk->filename) {
            file = f->chunk->filename;
        }

        // 行号
        int line = 0;
        if (f->chunk && f->chunk->lines && f->chunk->code && f->ip) {
            int offset = (int)(f->ip - f->chunk->code);
            if (offset >= 0 && offset < f->chunk->len) {
                line = f->chunk->lines[offset];
            }
        }

        pos += snprintf(buf + pos, buf_size - pos,
            "    #%d %s @ %s:%d\n",
            vm.frame_cnt - 1 - i, fn, file, line);

        // 限制最多 16 层，避免极深递归时输出过长
        if (vm.frame_cnt - 1 - i >= 15) {
            pos += snprintf(buf + pos, buf_size - pos,
                "    ... (省略 %d 帧)\n", vm.frame_cnt - 1 - i - 15 + 1);
            break;
        }
    }
    return pos;
}

// 带两个操作数上下文的异常抛出
static inline void throw_runtime_error_with_operands(
    const char* msg, const char* op_name,
    Value a_val, Value b_val, int line) {

    char err_buf[BUFFER_XXLARGE];
    char a_str[128], b_str[128];
    const char* func_name = "<main>";
    const char* filename = "unknown";
    int ip_offset = 0, src_line = 0;
    get_current_context(&func_name, &ip_offset, &src_line, &filename);

    value_to_short_str(a_val, a_str, sizeof(a_str));
    value_to_short_str(b_val, b_str, sizeof(b_str));

    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [内部] %s\n"
        "  [位置] %s:%d (func: %s)\n"
        "  [操作数a] type=%s, value=%s\n"
        "  [操作数b] type=%s, value=%s\n",
        msg,
        op_name_to_human(op_name), op_name,
        filename, src_line > 0 ? src_line : line, func_name,
        value_type_desc(a_val), a_str,
        value_type_desc(b_val), b_str);

    // 添加调用栈回溯
    pos = append_stack_trace(err_buf, pos, sizeof(err_buf));

    throw_runtime_error(err_buf, line);
}

// 带单个操作数上下文的异常抛出
static inline void throw_runtime_error_with_operand(
    const char* msg, const char* op_name,
    Value a_val, int line) {

    char err_buf[BUFFER_XXLARGE];
    char a_str[128];
    const char* func_name = "<main>";
    const char* filename = "unknown";
    int ip_offset = 0, src_line = 0;
    get_current_context(&func_name, &ip_offset, &src_line, &filename);

    value_to_short_str(a_val, a_str, sizeof(a_str));

    int pos = snprintf(err_buf, sizeof(err_buf),
        "%s\n"
        "  [操作] %s\n"
        "  [内部] %s\n"
        "  [位置] %s:%d (func: %s)\n"
        "  [操作数] type=%s, value=%s\n",
        msg,
        op_name_to_human(op_name), op_name,
        filename, src_line > 0 ? src_line : line, func_name,
        value_type_desc(a_val), a_str);

    // 添加调用栈回溯
    pos = append_stack_trace(err_buf, pos, sizeof(err_buf));

    throw_runtime_error(err_buf, line);
}
