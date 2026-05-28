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
                case OBJ_BIGINT: return "bigint";
                case OBJ_MODULE: return "module";
                default: return "object";
            }
        }
        default: return "unknown";
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
    
    error_add(ERR_RUNTIME, line, msg);
    
    // 恢复原来的文件名
    error_set_filename(old_filename);
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
    dict_set(err_dict, msg_key, val_obj((Object*)msg_val));

    // 添加 line 属性
    ObjString* line_key = str_copy("line", 4);
    if (!line_key) {
        runtime_error(msg, line);
        return;
    }
    dict_set(err_dict, line_key, val_int(line));

    // 添加 file 属性（当前文件名）
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    const char* filename = current_frame->chunk->filename ? current_frame->chunk->filename : "unknown";
    ObjString* file_key = str_copy("file", 4);
    ObjString* file_val = str_copy(filename, strlen(filename));
    if (file_key && file_val) {
        dict_set(err_dict, file_key, val_obj((Object*)file_val));
    }

    // 添加 stack 属性（调用栈）
    ObjArray* stack_arr = arr_new(vm.frame_cnt);
    if (stack_arr) {
        for (int i = 0; i < vm.frame_cnt; i++) {
            CallFrame* f = &vm.frames[i];
            const char* func_name = NULL;
            if (f->closure && f->closure->function && f->closure->function->name) {
                func_name = f->closure->function->name;
            } else if (i == 0) {
                func_name = "<main>";
            } else {
                func_name = "<anonymous>";
            }
            ObjString* name_str = str_copy(func_name, strlen(func_name));
            if (name_str) {
                arr_write(stack_arr, stack_arr->count, val_obj((Object*)name_str));
            }
        }
        ObjString* stack_key = str_copy("stack", 5);
        if (stack_key) {
            dict_set(err_dict, stack_key, val_obj((Object*)stack_arr));
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

    // 没有找到任何异常处理，报错并设置异常标志让 VM 停止
    runtime_error(msg, line);
    vm.has_exception = 1;
}
