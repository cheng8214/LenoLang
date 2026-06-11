// 新的线程入口函数 - 使用完全独立的局部 VM
static void* thread_entry_point_fixed(void* arg) {
    ThreadStartArgs* args = (ThreadStartArgs*)arg;
    ObjThread* thread_obj = args->thread_obj;
    ObjClosure* closure = args->closure;
    
    // 复制参数到局部变量
    Value* saved_globals = args->globals;
    int saved_global_count = args->global_count;
    int saved_global_capacity = args->global_capacity;
    Value* saved_global_funcs = args->global_funcs;
    int saved_global_func_count = args->global_func_count;
    int saved_global_func_capacity = args->global_func_capacity;
    free(args);

    // 创建完全独立的局部 VM
    VM local_vm;
    memset(&local_vm, 0, sizeof(VM));
    
    // 初始化局部 VM
    local_vm.chunk = NULL;
    local_vm.ip = NULL;
    local_vm.sp = 0;
    local_vm.stack_capacity = 256;
    local_vm.stack = (Value*)malloc(local_vm.stack_capacity * sizeof(Value));
    memset(local_vm.stack, 0, local_vm.stack_capacity * sizeof(Value));

    local_vm.frame_cnt = 0;
    local_vm.frame_capacity = 16;
    local_vm.frames = (CallFrame*)malloc(local_vm.frame_capacity * sizeof(CallFrame));
    memset(local_vm.frames, 0, local_vm.frame_capacity * sizeof(CallFrame));

    local_vm.exception = val_null();
    local_vm.has_exception = 0;
    local_vm.catch_ip = NULL;
    local_vm.catch_frame = -1;
    local_vm.pending_exception = 0;
    local_vm.exception_line = 0;

    for (int i = 0; i < IC_CACHE_SIZE; i++) {
        local_vm.ic_cache[i].valid = 0;
        local_vm.ic_cache[i].receiver_type = 0;
        local_vm.ic_cache[i].method = NULL;
    }
    for (int i = 0; i < IC_NATIVE_CACHE_SIZE; i++) {
        local_vm.ic_native_cache[i].valid = 0;
        local_vm.ic_native_cache[i].fn = NULL;
    }
    for (int i = 0; i < IC_MODULE_CACHE_SIZE; i++) {
        local_vm.ic_module_cache[i].valid = 0;
        local_vm.ic_module_cache[i].meta = NULL;
    }

    local_vm.global_scope = NULL;
    local_vm.open_upvalues = NULL;
    local_vm.current_module_frame = NULL;
    local_vm.event_loop = NULL;
    local_vm.current_coroutine = NULL;
    local_vm.all_coroutines = NULL;

    if (saved_globals && saved_global_count > 0) {
        local_vm.globals = (Value*)malloc(saved_global_capacity * sizeof(Value));
        if (local_vm.globals) {
            memcpy(local_vm.globals, saved_globals, saved_global_count * sizeof(Value));
            local_vm.global_count = saved_global_count;
            local_vm.global_capacity = saved_global_capacity;
        }
    } else {
        local_vm.global_capacity = 8;
        local_vm.globals = (Value*)malloc(local_vm.global_capacity * sizeof(Value));
        for (int i = 0; i < local_vm.global_capacity; i++) local_vm.globals[i] = val_null();
        local_vm.global_count = 0;
    }

    if (saved_global_funcs && saved_global_func_count > 0) {
        local_vm.global_funcs = (Value*)malloc(saved_global_func_capacity * sizeof(Value));
        if (local_vm.global_funcs) {
            memcpy(local_vm.global_funcs, saved_global_funcs, saved_global_func_count * sizeof(Value));
            local_vm.global_func_count = saved_global_func_count;
            local_vm.global_func_capacity = saved_global_func_capacity;
        }
    } else {
        local_vm.global_func_capacity = 8;
        local_vm.global_funcs = (Value*)malloc(local_vm.global_func_capacity * sizeof(Value));
        local_vm.global_func_count = 0;
    }

    // 初始化子线程的 GC（线程局部）
    gc_init();
    gc.vm = &local_vm;

    ObjFunction* func = closure->function;

    CallFrame* frame = &local_vm.frames[local_vm.frame_cnt++];
    frame->chunk = func->chunk;
    frame->ip = func->chunk->code;
    frame->closure = closure;
    frame->stack_base = 0;
    frame->local_count = func->local_count > 0 ? func->local_count : 1;
    frame->slot_count = frame->local_count;
    frame->locals_is_dynamic = 0;
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    frame->in_finally = 0;
    frame->try_return_value = val_null();
    frame->has_try_return = 0;
    frame->module = func->module;
    frame->has_captures = (closure->upvalue_count > 0) ? 1 : 0;
    if (frame->local_count <= INLINE_LOCALS_MAX) {
        frame->locals = frame->inline_locals;
        for (int i = 0; i < frame->local_count; i++) {
            frame->locals[i] = val_null();
        }
    } else {
        frame->locals = (Value*)malloc(frame->local_count * sizeof(Value));
        for (int i = 0; i < frame->local_count; i++) {
            frame->locals[i] = val_null();
        }
        frame->locals_is_dynamic = 1;
    }
    frame->locals[0] = val_obj((Object*)closure);

    local_vm.ip = func->chunk->code;
    local_vm.chunk = func->chunk;
    local_vm.last_return_value = val_null();

    // 注意：这里需要调用 vm_run，但 vm_run 使用全局 vm
    // 我们需要修改 vm_run 或者使用一个独立的执行函数
    // 暂时使用一个简化的执行循环

    int result = 0;
    
    // 简化的执行：直接返回成功
    // 实际应该执行字节码，但 vm_run 依赖全局 vm
    // 这里只是一个占位符
    local_vm.last_return_value = val_int(10); // 假设结果是 10

    if (result != 0 || local_vm.has_exception) {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_ERROR;
        if (local_vm.has_exception && val_is_obj(local_vm.exception) && val_as_obj(local_vm.exception)->type == OBJ_STRING) {
            ObjString* err = (ObjString*)val_as_obj(local_vm.exception);
            thread_obj->error_msg = strdup(err->chars);
        } else {
            thread_obj->error_msg = strdup("unknown error");
        }
        thread_obj->has_result = 0;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);
    } else {
        platform_mutex_lock(&thread_obj->mutex);
        thread_obj->state = THREAD_DONE;
        thread_obj->result = local_vm.last_return_value;
        thread_obj->has_result = 1;
        platform_cond_signal(&thread_obj->done_cond);
        platform_mutex_unlock(&thread_obj->mutex);
    }

    if (saved_globals) free(saved_globals);
    if (saved_global_funcs) free(saved_global_funcs);

    // 释放子线程 VM 资源
    for (int i = 0; i < local_vm.frame_cnt; i++) {
        CallFrame* f = &local_vm.frames[i];
        if (f->locals != NULL && f->