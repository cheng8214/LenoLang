// ============================================================================
// 异常处理类操作码 (OP_EXCEPTION)
// ============================================================================

OPCODE(OP_TRY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // OP_TRY <catch_offset> <finally_offset>
    // 设置异常处理信息
    uint16_t catch_offset = READ_SHORT();
    uint16_t finally_offset = READ_SHORT();

    // 保存之前的异常处理状态（用于嵌套 try-catch）
    frame->prev_catch_ip = frame->catch_ip;
    frame->prev_finally_ip = frame->finally_ip;

    // catch_offset 和 finally_offset 是相对于 OP_TRY 指令起始位置的偏移量
    // 当前 ip 指向的是 finally_offset 之后的位置，需要回退 5 字节到 OP_TRY 起始位置
    uint8_t* try_start = frame->ip - 5;  // 1字节 OP_TRY + 2字节 catch_offset + 2字节 finally_offset
    frame->catch_ip = catch_offset > 0 ? try_start + catch_offset : NULL;
    frame->finally_ip = finally_offset > 0 ? try_start + finally_offset : NULL;
    DISPATCH;
}

OPCODE(OP_CATCH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 清除异常状态，表示异常已被捕获
    vm.has_exception = 0;
    // 清除 catch_ip：当前块的 catch 已被处理，防止 catch 块中的 throw 无限循环
    // 保留 prev_catch_ip（外层的 catch），这样 catch 块中的 throw 可以跳转到外层
    // （OP_THROW 查找时会使用 prev_catch_ip 作为 fallback）
    frame->catch_ip = NULL;
    // 不交换 finally_ip，保持指向当前块的 finally 块，
    // 这样 OP_RETURN 在 try-catch-finally 中可以正确跳转到当前块的 finally
    DISPATCH;
}

OPCODE(OP_FINALLY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 如果没有待传播的异常，清除当前异常状态
    // 这样 finally 中的 throw 不会误抛出已处理的异常
    if (!vm.pending_exception) {
        vm.exception = val_null();
    }
    // 标记进入 finally 块
    frame->in_finally = 1;
    DISPATCH;
}

OPCODE(OP_END_TRY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 恢复之前的异常处理状态（用于嵌套 try-catch）
    // OP_CATCH 只交换了 catch_ip/prev_catch_ip，所以：
    // - 如果经过了 OP_CATCH：catch_ip = 外层的值，prev_catch_ip = 当前块的值
    //   恢复 catch_ip = prev_catch_ip 会错误地指向当前块，所以需要清除
    // - 如果没有经过 OP_CATCH：catch_ip = 当前块的值，prev_catch_ip = 外层的值
    //   恢复 catch_ip = prev_catch_ip 是正确的（恢复到外层）
    //
    // 统一处理：恢复 prev_catch_ip/prev_finally_ip（外层的值），然后清除 prev
    frame->catch_ip = frame->prev_catch_ip;
    frame->finally_ip = frame->prev_finally_ip;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    // 清除 finally 标记
    frame->in_finally = 0;

    // 检查是否有需要在 finally 后传播的异常
    if (vm.pending_exception) {
        vm.pending_exception = 0;
        // 继续传播异常
        // 检查当前帧（已恢复 catch_ip/finally_ip）是否有异常处理器
        if (frame->catch_ip) {
            // 当前帧有 catch 处理器，直接跳转
            frame->ip = frame->catch_ip;
            vm_stack_push(&vm, vm.exception);
            // 清除 catch_ip，防止重复捕获
            frame->catch_ip = NULL;
            vm.has_exception = 0;
            // 不 return，让 DISPATCH 继续执行
        } else if (frame->finally_ip) {
            // 当前帧有 finally 处理器，跳转到 finally
            vm.pending_exception = 1;
            frame->ip = frame->finally_ip;
            vm.has_exception = 0;
        } else if (vm.frame_cnt > 1) {
            // 当前帧没有异常处理器，返回到上层调用
            CallFrame* prev_frame = &vm.frames[vm.frame_cnt - 2];
            if (prev_frame->catch_ip || prev_frame->finally_ip) {
                // 上层有异常处理
                vm.has_exception = 1;
                vm_stack_push(&vm, vm.exception);
                return -1;  // 触发调用栈回退
            }
            // 上层也没有异常处理，打印调用栈并退出
            print_unhandled_exception(vm.exception, vm.exception_line);
            // 从异常对象中提取消息字符串（避免打印整个 Dict）
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_DICT) {
                ObjDict* err_dict = (ObjDict*)val_as_obj(vm.exception);
                ObjString* msg_key = str_copy("msg", 3);
                Value msg_val = dict_get(err_dict, val_obj((Object*)msg_key));
                if (val_is_string(msg_val)) {
                    msg = strdup(((ObjString*)val_as_obj(msg_val))->chars);
                }
            }
            if (!msg) {
                msg = value_to_string(vm.exception);
            }
            runtime_error(msg, vm.exception_line);
            free(msg);
            return -1;
        } else {
            // 没有上层异常处理，打印调用栈并退出
            print_unhandled_exception(vm.exception, vm.exception_line);  // 使用保存的异常行号
            // 从异常对象中提取消息字符串（避免打印整个 Dict）
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_DICT) {
                ObjDict* err_dict = (ObjDict*)val_as_obj(vm.exception);
                ObjString* msg_key = str_copy("msg", 3);
                Value msg_val = dict_get(err_dict, val_obj((Object*)msg_key));
                if (val_is_string(msg_val)) {
                    msg = strdup(((ObjString*)val_as_obj(msg_val))->chars);
                }
            }
            if (!msg) {
                msg = value_to_string(vm.exception);
            }
            runtime_error(msg, vm.exception_line);  // 使用保存的异常行号
            free(msg);
            return -1;
        }
    }

    // 检查是否有 try 中保存的返回值需要处理
    if (frame->has_try_return) {
        frame->has_try_return = 0;
        Value return_val = frame->try_return_value;
        // 执行 return 逻辑（复制 OP_RETURN 的代码）
        int stack_base = frame->stack_base;
        Value* locals = frame->locals;
        int local_count = frame->local_count;
        int locals_is_dynamic = frame->locals_is_dynamic;

        if (locals != NULL && local_count > 0 && frame->has_captures) {
            close_upvalues(locals, local_count, frame->has_captures);
        }

        vm.frame_cnt--;

        if (vm.current_coroutine) {
            if (vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) {
                vm.current_coroutine->result = return_val;
                gc_write_barrier((Object*)vm.current_coroutine, return_val);
                if (locals != NULL && locals_is_dynamic) {
                    free(locals);
                }
                return 0;
            }
        }

        if (vm.frame_cnt == 0) {
            if (locals != NULL && locals_is_dynamic) {
                free(locals);
            }
            return 0;
        }

        vm.sp = stack_base;
        vm_stack_push(&vm, return_val);

        if (locals != NULL && locals_is_dynamic) {
            free(locals);
        }

        frame = &vm.frames[vm.frame_cnt - 1];
    }

    DISPATCH;
}

OPCODE(OP_THROW) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 从栈中弹出异常值
    Value exception = vm_stack_pop(&vm);
    // 如果 throw 没有参数（null），重新抛出当前捕获的异常
    if (val_is_null(exception)) {
        exception = vm.exception;
    }
    // 清除 pending_exception，因为我们要重新抛出异常
    // 这会触发新的异常处理流程
    if (vm.pending_exception) {
        vm.pending_exception = 0;
    }
    vm.exception = exception;
    vm.has_exception = 1;
    vm.exception_line = GET_CURRENT_LINE();  // 保存异常发生时的行号

    // 查找 catch 块（从当前帧开始向上层查找）
    // 注意：如果当前在 finally 块中执行 throw，需要清除当前帧的 finally_ip
    // 防止再次跳转到同一个 finally
    CallFrame* current_frame = &vm.frames[vm.frame_cnt - 1];
    if (current_frame->in_finally && current_frame->finally_ip) {
        current_frame->finally_ip = NULL;
    }

    for (int i = vm.frame_cnt - 1; i >= 0; i--) {
        CallFrame* target_frame = &vm.frames[i];
        // 快速路径：如果函数没有 try，直接跳过
        if (target_frame->closure && target_frame->closure->function && !target_frame->closure->function->has_try) {
            continue;
        }
        // 检查当前帧的 catch_ip 或 prev_catch_ip（用于嵌套 try-catch）
        uint8_t* target_catch_ip = target_frame->catch_ip ? target_frame->catch_ip : target_frame->prev_catch_ip;
        uint8_t* target_finally_ip = target_frame->finally_ip ? target_frame->finally_ip : target_frame->prev_finally_ip;
        if (target_catch_ip || target_finally_ip) {
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
            vm.sp = target_frame->stack_base;

            // 如果当前帧有 finally 块且 catch 已处理（catch_ip 被清除），
            // 优先执行 finally，这确保 catch 块中 throw 时 finally 仍会执行
            if (target_frame->finally_ip && !target_frame->in_finally && !target_frame->catch_ip) {
                // 先执行 finally，标记需要在 finally 后传播异常
                vm.pending_exception = 1;
                target_frame->ip = target_frame->finally_ip;
                vm.has_exception = 0;
                frame = target_frame;
            } else if (target_catch_ip) {
                // 跳转到 catch 块
                target_frame->ip = target_catch_ip;
                // 更新 frame 变量为目标帧，这样 DISPATCH 会使用正确的 ip
                frame = target_frame;
                // 将异常值压入栈，供 catch 使用
                vm_stack_push(&vm, exception);
                vm.has_exception = 0;
                // 清除已使用的 catch_ip，防止重复捕获
                if (target_frame->catch_ip) {
                    target_frame->catch_ip = NULL;
                } else {
                    target_frame->prev_catch_ip = NULL;
                }
            } else {
                // 没有 catch，但有 finally（通过 prev_finally_ip），先执行 finally
                // 标记需要在 finally 后传播异常
                vm.pending_exception = 1;
                target_frame->ip = target_finally_ip;
                // 清除 has_exception，因为异常将被处理（传播到 finally）
                vm.has_exception = 0;
            }
            break;
        }
    }

    // 如果没有找到任何异常处理
    if (vm.has_exception) {
        // 如果在协程中，不立即报告错误，让异常通过 Future 传播
        if (vm.current_coroutine) {
            return -1;
        }
        // 不在协程中，打印完整调用栈并报错
        print_unhandled_exception(exception, GET_CURRENT_LINE());
        // 从异常对象中提取消息字符串（避免打印整个 Dict）
        char* msg = NULL;
        if (val_is_obj(exception) && val_as_obj(exception)->type == OBJ_DICT) {
            ObjDict* err_dict = (ObjDict*)val_as_obj(exception);
            ObjString* msg_key = str_copy("msg", 3);
            Value msg_val = dict_get(err_dict, val_obj((Object*)msg_key));
            if (val_is_string(msg_val)) {
                msg = strdup(((ObjString*)val_as_obj(msg_val))->chars);
            }
        }
        if (!msg) {
            msg = value_to_string(exception);
        }
        runtime_error(msg, GET_CURRENT_LINE());
        free(msg);
        return -1;
    }
    DISPATCH;
}
