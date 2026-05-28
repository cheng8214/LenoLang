// ============================================================================
// 协程相关操作码 (OP_AWAIT, OP_ASYNC_CALL)
// ============================================================================

OPCODE(OP_ASYNC_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int arg_count = READ_SHORT();
    
    // 参数从栈底到栈顶：arg0, arg1, ..., argN-1, callee
    // callee 在 vm_stack_peek(0)，参数在 callee 下面
    Value callee = vm_stack_peek(&vm, 0);
    
    // 检查是否是闭包
    if (!val_is_obj(callee) || val_as_obj(callee)->type != OBJ_CLOSURE) {
        throw_runtime_error("async 调用只能调用 async 函数", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
    
    // 创建协程对象
    ObjCoroutine* co = coroutine_new(closure);
    
    // 保存参数到协程（从栈中复制）
    if (arg_count > 0) {
        co->initial_args = (Value*)malloc(arg_count * sizeof(Value));
        if (co->initial_args) {
            // 参数在栈上的顺序：第一个参数在最下面
            // vm_stack_peek(arg_count) 是第一个参数
            for (int i = 0; i < arg_count; i++) {
                co->initial_args[i] = vm_stack_peek(&vm, arg_count - i);
            }
            co->initial_arg_count = arg_count;
        }
    }
    
    // 弹出参数和 callee 从栈
    for (int i = 0; i <= arg_count; i++) {
        vm_stack_pop(&vm);
    }
    
    // 创建 Future 对象 - 这是 task 返回给调用者的 Future
    ObjFuture* task_future = future_new();
    
    // 保存 task_future 到协程，以便协程完成时可以触发它
    co->task_future = task_future;
    
    // 将协程加入事件循环的就绪队列
    if (vm.event_loop) {
        event_loop_add_ready(vm.event_loop, co);
    }
    
    // 将 Future 压栈给调用者
    vm_stack_push(&vm, val_obj((Object*)task_future));
    
    DISPATCH;
}

OPCODE(OP_AWAIT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value future_val = vm_stack_pop(&vm);
    
    // 检查是否是 Future 对象
    if (!val_is_obj(future_val) || val_as_obj(future_val)->type != OBJ_FUTURE) {
        throw_runtime_error("await 只能等待 Future 对象", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    ObjFuture* future = (ObjFuture*)val_as_obj(future_val);
    
    // 如果 Future 已完成
    if (future->completed) {
        // 检查是否有错误
        if (!val_is_null(future->error)) {
            // Future 失败了，抛出异常
            vm.exception = future->error;
            vm.has_exception = 1;
            // 将异常压入栈（throw_runtime_error 会用到）
            vm_stack_push(&vm, vm.exception);
            // 查找 catch/finally 并跳转
            // 从当前调用帧开始向上查找有 catch/finally 的帧
            for (int i = vm.frame_cnt - 1; i >= 0; i--) {
                CallFrame* f = &vm.frames[i];
                // 快速路径：如果函数确定没有 try，直接跳过
                if (f->closure && f->closure->function && !f->closure->function->has_try) {
                    continue;
                }
                // 检查是否有异常处理
                if (f->catch_ip || f->finally_ip) {
                    // 找到了有异常处理的帧，需要展开调用栈到这个帧
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
                    vm.sp = f->stack_base;
                    // 处理异常
                    if (f->catch_ip) {
                        // 跳转到 catch 块
                        f->ip = f->catch_ip;
                        // 异常值已经在栈上
                    } else {
                        // 没有 catch，但有 finally
                        vm.pending_exception = 1;
                        f->ip = f->finally_ip;
                    }
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH;
                }
            }
            // 没有找到任何异常处理，报错
            runtime_error("未捕获的异常", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        vm_stack_push(&vm, future->result);
        DISPATCH;
    }
    
    // 挂起当前协程
    ObjCoroutine* current = vm.current_coroutine;
    if (!current) {
        throw_runtime_error("await 只能在 async 函数中使用", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
    
    // 保存协程状态
    current->state = COROUTINE_SUSPENDED;
    // 保存当前指令指针（恢复时从这里继续）
    current->saved_ip = frame->ip;
    // 保存栈基址和当前栈指针位置
    current->saved_stack_base = frame->stack_base;
    // 保存当前的 frame_cnt，以便恢复时知道有多少个 frame 属于此协程
    current->saved_frame_cnt = vm.frame_cnt;
    // 保存 frame 副本（因为其他协程可能会覆盖 vm.frames）
    if (!current->saved_frame_copy) {
        current->saved_frame_copy = (CallFrame*)malloc(sizeof(CallFrame));
    }
    if (current->saved_frame_copy) {
        memcpy(current->saved_frame_copy, frame, sizeof(CallFrame));
        current->has_saved_frame = 1;
    }
    current->waiting_for = future;
    
    // 设置 Future 的等待者
    future->waiter = current;
    
    // 注意：不减少 frame_cnt，因为我们需要保持调用帧以便恢复
    // 帧的locals数据保留在原地，不需要拷贝
    // vm_run_coroutine 会正确处理挂起的协程
    
    // 返回，让控制权回到事件循环
    return 0;
}
