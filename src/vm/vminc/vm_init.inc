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
    vm.last_return_count = 0;
    
    // 初始化字符串表（字符串内化）
    intern_table_init();
    
    // 初始化内联缓存
    for (int i = 0; i < IC_CACHE_SIZE; i++) {
        vm.ic_cache[i].valid = 0;
        vm.ic_cache[i].receiver_type = 0;
        vm.ic_cache[i].name_hash = 0;
        vm.ic_cache[i].method = NULL;
    }
    for (int i = 0; i < IC_NATIVE_CACHE_SIZE; i++) {
        vm.ic_native_cache[i].valid = 0;
        vm.ic_native_cache[i].name_hash = 0;
        vm.ic_native_cache[i].fn = NULL;
    }
    for (int i = 0; i < IC_MODULE_CACHE_SIZE; i++) {
        vm.ic_module_cache[i].valid = 0;
        vm.ic_module_cache[i].combined_hash = 0;
        vm.ic_module_cache[i].meta = NULL;
    }
    for (int i = 0; i < IC_METHOD_CACHE_SIZE; i++) {
        vm.ic_method_cache[i].valid = 0;
        vm.ic_method_cache[i].def = NULL;
        vm.ic_method_cache[i].name_hash = 0;
        vm.ic_method_cache[i].gen = 0;
        vm.ic_method_cache[i].closure = NULL;
    }
    for (int i = 0; i < IC_STRUCTDEF_CACHE_SIZE; i++) {
        vm.ic_structdef_cache[i].valid = 0;
        vm.ic_structdef_cache[i].name_hash = 0;
        vm.ic_structdef_cache[i].gen = 0;
        vm.ic_structdef_cache[i].def = NULL;
    }
    for (int i = 0; i < IC_DICT_CACHE_SIZE; i++) {
        vm.ic_dict_cache[i].dict = NULL;
        vm.ic_dict_cache[i].cap = 0;
        vm.ic_dict_cache[i].slot = -1;
        vm.ic_dict_cache[i].miss_streak = 0;
        vm.ic_dict_cache[i].dead = 0;
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
        error_add_at(ERR_RUNTIME, 0, 0, "全局变量数组内存分配失败");
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
        error_add_at(ERR_RUNTIME, 0, 0, "全局函数数组内存分配失败");
        free(vm.globals);
        return;
    }
    vm.global_func_count = 0;

    // 初始化 GC 返回计数器
    vm.gc_return_counter = 0;
    vm.gc_force_request = 0;

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
    extern void cstructs_init_methods(void);
    extern void threads_init_instance_methods(void);
    arrays_init_instance_methods();
    strings_init_instance_methods();
    maths_init_instance_methods();
    dicts_init_instance_methods();
    files_init_instance_methods();
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

void vm_load(Chunk* c) {
    extern VM vm;
    vm.chunk = c;
    vm.ip = c->code;

    // 确保调用帧容量
    if (vm.frame_cnt >= vm.frame_capacity) {
        if (!vm_grow_frames(&vm)) {
            error_add_at(ERR_RUNTIME, 0, 0, "调用栈内存分配失败");
            return;
        }
    }

    // 为主代码块分配locals数组（支持闭包捕获全局变量）
    int local_count = c->local_count > 0 ? c->local_count : 64;  // 默认至少64个槽位
    
    // 初始化调用帧
    CallFrame* frame = &vm.frames[vm.frame_cnt++];
    
    // 根据大小选择内联或动态分配
    Value* locals;
    int locals_is_dynamic;
    int locals_capacity;
    if (local_count <= INLINE_LOCALS_MAX) {
        locals = frame->inline_locals;
        locals_is_dynamic = 0;
        locals_capacity = INLINE_LOCALS_MAX;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            error_add_at(ERR_RUNTIME, 0, 0, "主帧locals内存分配失败");
            return;
        }
        locals_is_dynamic = 1;
        locals_capacity = local_count;
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
    frame->locals_capacity = locals_capacity;
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->catch_finally_ip = NULL;
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
        if (!vm_grow_frames(&vm)) return -1;
    }
    
    CallFrame* frame = &vm.frames[vm.frame_cnt++];
    frame->closure = NULL;
    frame->chunk = chunk;
    frame->ip = chunk->code;
    frame->stack_base = vm.sp;
    frame->slot_count = 0;
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->catch_finally_ip = NULL;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    frame->in_finally = 0;
    frame->try_return_value = val_null();
    frame->has_try_return = 0;
    // 使用当前模块帧的模块信息
    frame->module = vm.current_module_frame ? vm.current_module_frame->module : NULL;
    // 临时帧默认没有捕获 upvalue
    frame->has_captures = 0;

    // ★ 寄存器式：chunk 里的指令按 R[i] 访问帧局部，必须给帧分配 locals。
    //   （栈式这里 locals 为 NULL 也能跑，因为栈式不通过 locals 取操作数。）
    int lc = chunk->local_count > 0 ? chunk->local_count : 1;
    Value* locals;
    int dyn = 0;
    int cap;
    if (lc <= INLINE_LOCALS_MAX) {
        locals = frame->inline_locals;
        cap = INLINE_LOCALS_MAX;
    } else {
        locals = (Value*)malloc(sizeof(Value) * (size_t)lc);
        if (!locals) {
            vm.frame_cnt = prev_frame_cnt;
            vm.chunk = prev_chunk;
            vm.ip = prev_ip;
            return -1;
        }
        dyn = 1;
        cap = lc;
    }
    for (int i = 0; i < lc; i++) locals[i] = val_null();
    frame->locals = locals;
    frame->local_count = lc;
    frame->locals_is_dynamic = dyn;
    frame->locals_capacity = cap;

    // ★ 宿主返回语义 + 帧数下界：模块 chunk 的 RETURN 让帧数回落到 prev_frame_cnt
    //   时，OP_RETURN 直接结束本次 vm_run，而不会继续跑调用方的字节码。
    frame->ret_frame = -1;
    frame->ret_reg = 0;
    frame->ret_want = 1;
    int prev_stop = vm.stop_frame_cnt;
    vm.stop_frame_cnt = prev_frame_cnt;

    // 执行 chunk
    int result = vm_run();

    // 恢复状态
    vm.stop_frame_cnt = prev_stop;
    // ★ 释放本帧的动态 locals —— **只在这个帧还挂在栈上时**（错误/异常退出）：
    //   正常 RETURN 时 `REG_FINISH_RETURN` 的三个分支**各自已经 free 过** `_dyn` 的 locals
    //   （01_prologue.inc 的 236/246/265 行），这里再 free 就是**双重释放** ⇒ 堆损坏
    //   （退出码 0xC0000374 STATUS_HEAP_CORRUPTION，崩点正落在这一行）。
    //   实测触发条件 = 「模块 chunk 的槽位 > INLINE_LOCALS_MAX 且模块正常 return」：
    //     模块 chunk 槽位 ≤ 64 ⇒ 内联帧 `dyn=0` ⇒ 两条路径都不 free（不崩）；
    //     槽位 ≥ 65 ⇒ malloc 帧 ⇒ RETURN 已 free，这里又 free（必崩）；
    //     普通函数帧不崩 —— 它走 265 行那一支，只 free 一次。
    //   判据用「帧索引仍在栈上 + locals 指针仍是本帧的」：`vm_run()` 期间 `vm.frames`
    //   可能被 realloc，所以按索引**重新取**帧，不能沿用上面那个可能已失效的 `frame` 指针。
    if (dyn && vm.frame_cnt > prev_frame_cnt) {
        CallFrame* own = &vm.frames[prev_frame_cnt];
        if (own->locals == locals) {
            free(locals);
            own->locals = NULL;
            own->locals_is_dynamic = 0;
        }
    }
    vm.frame_cnt = prev_frame_cnt;
    vm.chunk = prev_chunk;
    vm.ip = prev_ip;

    return result;
}

// ============================================================================
// 进程退出码
// ============================================================================

// 读取 main 函数的返回值作为进程退出码
// 由顶层帧的 OP_RETURN 存入 vm.last_return_value：
//   - int   → 直接作为退出码
//   - null  → 0（main 无返回值/隐式返回）
//   - 其他类型（any 兜底路径）→ 0，不作为错误
int vm_get_exit_code(void) {
    extern VM vm;
    Value v = vm.last_return_value;
    if (val_is_int(v)) {
        return (int)val_as_int(v);
    }
    return 0;
}
