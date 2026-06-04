// ============================================================================
// VM 初始化和栈操作 - 从 vm_init.c 迁移
// ============================================================================

// 虚拟机初始化
void vm_init(void) {
    vm_init_with_scope(NULL);
}

void vm_init_with_scope(Scope* global_scope) {
    extern VM vm;
    
    vm.chunk = NULL;
    vm.ip = NULL;
    vm.sp = 0;
    vm.stack_capacity = 0;
    vm.stack = NULL;
    vm.frame_cnt = 0;
    vm.frame_capacity = 0;
    vm.frames = NULL;
    vm.exception = val_null();
    vm.has_exception = 0;
    vm.catch_ip = NULL;
    vm.catch_frame = -1;
    vm.pending_exception = 0;
    vm.exception_line = 0;
    
    // 初始化字符串表（字符串内化）
    intern_table_init();
    
    // 初始化内联缓存
    for (int i = 0; i < IC_CACHE_SIZE; i++) {
        vm.ic_cache[i].valid = 0;
        vm.ic_cache[i].receiver_type = 0;
        vm.ic_cache[i].name_hash = 0;
        vm.ic_cache[i].method = NULL;
    }
    vm.ic_hits = 0;
    vm.ic_misses = 0;

    if (global_scope) {
        // 使用传入的 scope（语义分析阶段创建的）
        vm.global_scope = global_scope;
    } else {
        // 创建新的 scope
        vm.global_scope = scope_new(NULL, 0);
    }

    vm.open_upvalues = NULL;

    // 初始化全局变量动态数组
    vm.global_capacity = 8;  // 初始容量
    vm.globals = (Value*)malloc(vm.global_capacity * sizeof(Value));
    if (!vm.globals) {
        error_add(ERR_RUNTIME, 0, "全局变量数组内存分配失败");
        return;
    }
    for (int i = 0; i < vm.global_capacity; i++) {
        vm.globals[i] = val_null();
    }
    vm.global_count = 0;

    // 初始化全局函数动态数组
    vm.global_func_capacity = 8;  // 初始容量
    vm.global_funcs = (Value*)malloc(vm.global_func_capacity * sizeof(Value));
    if (!vm.global_funcs) {
        error_add(ERR_RUNTIME, 0, "全局函数数组内存分配失败");
        free(vm.globals);
        return;
    }
    vm.global_func_count = 0;

    // 初始化模块系统
    vm.current_module_frame = NULL;
    
    // 初始化协程系统
    vm.event_loop = NULL;
    vm.current_coroutine = NULL;
    vm.all_coroutines = NULL;
    
    // 初始化线程系统
    vm.active_threads = NULL;
    vm.active_thread_count = 0;
    vm.active_thread_capacity = 0;

    // 关联 GC 和 VM
    gc.vm = &vm;

    // VM 初始化完成，设置标志（必须在 native_register_globals 之前）
    vm_initialized = 1;

    // 注册内置 Native 函数
    native_register_globals();
    
    // 初始化所有实例方法（确保运行时方法表可用）
    extern void arrays_init_instance_methods(void);
    extern void strings_init_instance_methods(void);
    extern void maths_init_instance_methods(void);
    extern void dicts_init_instance_methods(void);
    extern void files_init_instance_methods(void);
    extern void guis_init_instance_methods(void);
    extern void guis_init_event_methods(void);
    extern void cstructs_init_methods(void);
    extern void threads_init_instance_methods(void);
    arrays_init_instance_methods();
    strings_init_instance_methods();
    maths_init_instance_methods();
    dicts_init_instance_methods();
    files_init_instance_methods();
    guis_init_instance_methods();
    guis_init_event_methods();
    cstructs_init_methods();
    threads_init_instance_methods();
}

// ============================================================================
// 栈操作
// ============================================================================
// 注意：vm_stack_push, vm_stack_pop, vm_stack_peek 已在 leno_vm.h 中定义为 static inline

void vm_reset_stack(void) {
    extern VM vm;
    vm.sp = 0;
    vm.frame_cnt = 0;
}

// ============================================================================
// 调用帧管理
// ============================================================================

// 在 realloc 后更新所有使用内联数组的帧的 locals 指针
static void update_inline_locals_pointers(void) {
    extern VM vm;
    for (int i = 0; i < vm.frame_cnt; i++) {
        CallFrame* frame = &vm.frames[i];
        if (!frame->locals_is_dynamic && frame->local_count > 0) {
            frame->locals = frame->inline_locals;
        }
    }
}

void vm_load(Chunk* c) {
    extern VM vm;
    vm.chunk = c;
    vm.ip = c->code;

    // 确保调用帧容量
    if (vm.frame_cnt >= vm.frame_capacity) {
        int new_capacity = grow_capacity(vm.frame_capacity);
        CallFrame* new_frames = (CallFrame*)realloc(vm.frames, new_capacity * sizeof(CallFrame));
        if (!new_frames) {
            error_add(ERR_RUNTIME, 0, "调用栈内存分配失败");
            return;
        }
        vm.frames = new_frames;
        vm.frame_capacity = new_capacity;
        // realloc 后更新内联数组指针
        update_inline_locals_pointers();
    }

    // 为主代码块分配locals数组（支持闭包捕获全局变量）
    int local_count = c->local_count > 0 ? c->local_count : 64;  // 默认至少64个槽位
    
    // 初始化调用帧
    CallFrame* frame = &vm.frames[vm.frame_cnt++];
    
    // 根据大小选择内联或动态分配
    Value* locals;
    int locals_is_dynamic;
    if (local_count <= INLINE_LOCALS_MAX) {
        locals = frame->inline_locals;
        locals_is_dynamic = 0;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            error_add(ERR_RUNTIME, 0, "主帧locals内存分配失败");
            return;
        }
        locals_is_dynamic = 1;
    }
    
    for (int i = 0; i < local_count; i++) {
        locals[i] = val_null();
    }

    frame->chunk = c;
    frame->ip = c->code;
    frame->stack_base = 0;
    frame->slot_count = 0;
    frame->closure = NULL;
    frame->locals = locals;  // 主帧也有独立的locals，支持闭包捕获
    frame->local_count = local_count;
    frame->locals_is_dynamic = locals_is_dynamic;
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    frame->in_finally = 0;
    frame->try_return_value = val_null();
    frame->has_try_return = 0;
    frame->module = NULL;  // 主帧没有模块
    // 主帧默认没有捕获 upvalue
    frame->has_captures = 0;
}

// ============================================================================
// 独立 Chunk 执行（用于模块初始化）
// ============================================================================

// 前向声明（vm_run 在 vm.c 中定义）
extern int vm_run(void);

// 执行独立的 chunk（用于模块初始化）
int vm_run_chunk(Chunk* chunk) {
    extern VM vm;
    
    if (!chunk || chunk->len == 0) return 0;
    
    // 保存当前状态
    Chunk* prev_chunk = vm.chunk;
    uint8_t* prev_ip = vm.ip;
    int prev_frame_cnt = vm.frame_cnt;
    
    // 设置新的 chunk
    vm.chunk = chunk;
    vm.ip = chunk->code;
    
    // 创建临时调用帧
    if (vm.frame_cnt >= vm.frame_capacity) {
        int new_capacity = vm.frame_capacity ? vm.frame_capacity * 2 : 8;
        CallFrame* new_frames = (CallFrame*)realloc(vm.frames, new_capacity * sizeof(CallFrame));
        if (!new_frames) return -1;
        vm.frames = new_frames;
        vm.frame_capacity = new_capacity;
        // realloc 后更新内联数组指针
        update_inline_locals_pointers();
    }
    
    CallFrame* frame = &vm.frames[vm.frame_cnt++];
    frame->closure = NULL;
    frame->chunk = chunk;
    frame->ip = chunk->code;
    frame->stack_base = vm.sp;
    frame->slot_count = 0;
    frame->locals = NULL;
    frame->local_count = 0;
    frame->locals_is_dynamic = 0;  // 明确初始化
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    frame->in_finally = 0;
    frame->try_return_value = val_null();
    frame->has_try_return = 0;
    // 使用当前模块帧的模块信息
    frame->module = vm.current_module_frame ? vm.current_module_frame->module : NULL;
    // 临时帧默认没有捕获 upvalue
    frame->has_captures = 0;

    // 执行 chunk
    int result = vm_run();
    
    // 恢复状态
    vm.frame_cnt = prev_frame_cnt;
    vm.chunk = prev_chunk;
    vm.ip = prev_ip;
    
    return result;
}
