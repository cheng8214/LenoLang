// ============================================================================
// 函数调用实现 - 从 vm_call.c 迁移
// ============================================================================

// 辅助函数：从运行时值推断泛型类型名（返回线程本地缓冲区指针）
// 返回值如 "int"/"float"/"string"/"bool" 或 "struct FooName"
// 返回 NULL 表示无法推断
// 用于泛型 struct 实例化时推断类型参数
static inline const char* value_to_generic_type_name(Value value) {
#ifdef _MSC_VER
    static __declspec(thread) char buf[64];
#else
    static __thread char buf[64];
#endif
    if (val_is_int(value) || val_is_bigint(value)) {
        return "int";
    }
    if (val_is_float(value)) {
        return "float";
    }
    if (val_is_bool(value)) {
        return "bool";
    }
    if (val_is_obj(value)) {
        ObjType ot = val_as_obj(value)->type;
        if (ot == OBJ_STRING) {
            return "string";
        }
        if (ot == OBJ_STRUCT) {
            ObjStruct* arg_s = (ObjStruct*)val_as_obj(value);
            if (arg_s->def && arg_s->def->name) {
                snprintf(buf, sizeof(buf), "struct %s", arg_s->def->name);
                return buf;
            }
            return "struct";
        }
        if (ot == OBJ_ARRAY) {
            return "array";
        }
    }
    return NULL;
}

// 辅助函数：从 TypeInfo 推断泛型类型名
static inline const char* typeinfo_to_generic_type_name(TypeInfo* type) {
#ifdef _MSC_VER
    static __declspec(thread) char buf[64];
#else
    static __thread char buf[64];
#endif
    if (!type) return NULL;
    switch (type->kind) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_STRUCT:
            if (type->struct_name) {
                snprintf(buf, sizeof(buf), "struct %s", type->struct_name);
                return buf;
            }
            return "struct";
        default: return NULL;
    }
}

// 调用闭包函数
static inline int call(ObjClosure* closure, int arg_count, int line) {
    // 缓存 function 指针，避免重复解引用 closure->function
    ObjFunction* func = closure->function;

    // 需要扩容（罕见路径）
    if (unlikely(vm.frame_cnt >= vm.frame_capacity)) {
        // S1：硬上限——无限递归时逐倍扩容直到内存耗尽只会报"分配失败"，掩盖真实原因
        if (vm.frame_cnt >= VM_MAX_FRAMES) {
            char depth_buf[96];
            snprintf(depth_buf, sizeof(depth_buf),
                     "调用栈溢出：递归深度已达 %d（可能是无限递归）", vm.frame_cnt);
            runtime_error(depth_buf, line);
            return 0;
        }
        if (!vm_grow_frames(current_exec_vm)) {
            runtime_error("调用栈内存分配失败", line);
            return 0;
        }
    }

    // 从栈中弹出 callee 和参数
    // 栈布局: [...][arg1][arg2][callee]
    
    int local_count = func->local_count;
    Value* locals;
    int locals_is_dynamic = 0;
    int locals_capacity = 0;
    
    // 优化：小函数使用内联数组，避免 malloc/free 开销
    CallFrame* frame = &vm.frames[vm.frame_cnt];
    if (likely(local_count <= INLINE_LOCALS_MAX)) {
        locals = frame->inline_locals;
        locals_capacity = INLINE_LOCALS_MAX;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
        locals_capacity = local_count;
    }
    
    // 初始化locals数组
    TypeKind* param_types = func->param_types;
    int arity = func->arity;
    for (int i = 0; i < arg_count; i++) {
        Value arg = vm.stack[vm.sp - arg_count - 1 + i];
        
        if (param_types && i < arity) {
            TypeKind expected_type = param_types[i];
            if (expected_type == TYPE_FLOAT && val_is_int(arg)) {
                arg = val_float((double)val_as_int(arg));
            } else if (expected_type == TYPE_FLOAT && val_is_bigint(arg)) {
                arg = val_float(bigint_to_double(val_as_bigint(arg)));
            } else if (expected_type == TYPE_INT && val_is_float(arg)) {
                arg = val_int((int)val_as_num(arg));
            }
        }
        locals[i] = arg;
    }
    
    for (int i = arg_count; i < local_count; i++) {
        locals[i] = val_null();
    }
    
    // 弹出 callee 和参数，同时为返回值预留一个槽位
    vm.sp -= arg_count;
    int new_stack_base = vm.sp - 1;

    vm.frame_cnt++;
    frame->closure = closure;
    frame->chunk = func->chunk;
    frame->ip = func->chunk->code;
    frame->stack_base = new_stack_base;
    frame->slot_count = local_count;
    frame->locals = locals;
    frame->local_count = local_count;
    frame->locals_is_dynamic = locals_is_dynamic;
    frame->locals_capacity = locals_capacity;
    // 帧复用优化：无 try 的函数跳过 7 个 try 字段初始化（~50B/次），
    // 读取点必须用 FRAME_TRY_VALID 守护（脏值会导致异常误捕获/误跳 finally）
    if (func->has_try) {
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->catch_finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
    }
    frame->module = func->module;
    frame->has_captures = 0;   // 由 OP_CLOSURE 在**真建了指向本帧窗口的 upvalue** 时置 1（见 02_loads.inc）

    // 宿主调用路径（vm_call_value / 协程）：结果压回 vm.stack[stack_base]
    frame->ret_frame = -1;
    frame->ret_reg = 0;
    frame->ret_want = 1;

    return 1;
}

// 调用闭包函数（跳过参数类型检查快速路径）
// 当编译期已确认所有实参类型与形参精确匹配时使用
// 与 call() 的唯一区别：跳过 param_types 类型转换循环，用 memcpy 直接拷贝参数
static inline int call_no_type_check(ObjClosure* closure, int arg_count, int line) {
    ObjFunction* func = closure->function;

    if (unlikely(vm.frame_cnt >= vm.frame_capacity)) {
        // S1：硬上限——无限递归时逐倍扩容直到内存耗尽只会报"分配失败"，掩盖真实原因
        if (vm.frame_cnt >= VM_MAX_FRAMES) {
            char depth_buf[96];
            snprintf(depth_buf, sizeof(depth_buf),
                     "调用栈溢出：递归深度已达 %d（可能是无限递归）", vm.frame_cnt);
            runtime_error(depth_buf, line);
            return 0;
        }
        if (!vm_grow_frames(current_exec_vm)) {
            runtime_error("调用栈内存分配失败", line);
            return 0;
        }
    }

    int local_count = func->local_count;
    Value* locals;
    int locals_is_dynamic = 0;
    int locals_capacity = 0;

    CallFrame* frame = &vm.frames[vm.frame_cnt];
    if (likely(local_count <= INLINE_LOCALS_MAX)) {
        locals = frame->inline_locals;
        locals_capacity = INLINE_LOCALS_MAX;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
        locals_capacity = local_count;
    }

    // 快速路径：跳过类型转换，直接拷贝参数到 locals
    // 栈布局: [...][arg1][arg2]...[argN][callee]
    // call() 中 arg = vm.stack[vm.sp - arg_count - 1 + i]，这里与之完全一致
    Value* src = vm.stack + vm.sp - arg_count - 1;
    for (int i = 0; i < arg_count; i++) {
        locals[i] = src[i];
    }
    for (int i = arg_count; i < local_count; i++) {
        locals[i] = val_null();
    }

    // 弹出 callee 和参数，为返回值预留槽位
    vm.sp -= arg_count;
    int new_stack_base = vm.sp - 1;

    vm.frame_cnt++;
    frame->closure = closure;
    frame->chunk = func->chunk;
    frame->ip = func->chunk->code;
    frame->stack_base = new_stack_base;
    frame->slot_count = local_count;
    frame->locals = locals;
    frame->local_count = local_count;
    frame->locals_is_dynamic = locals_is_dynamic;
    frame->locals_capacity = locals_capacity;
    if (func->has_try) {
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->catch_finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
    }
    frame->module = func->module;
    frame->has_captures = 0;   // 由 OP_CLOSURE 在**真建了指向本帧窗口的 upvalue** 时置 1（见 02_loads.inc）

    // 宿主调用路径：结果压回 vm.stack[stack_base]
    frame->ret_frame = -1;
    frame->ret_reg = 0;
    frame->ret_want = 1;

    return 1;
}

// ============================================================================
// 寄存器式调用（call_reg）—— 参数直接来自调用方的寄存器文件
//   调用方（vm_frame_cnt-1）的 R[arg_base .. arg_base+arg_count-1] 即实参；
//   被调方结果按 ret_reg/ret_want 写回调用方 R[ret_reg ..]（见 OP_RETURN）。
//   has_self/self：方法调用时写入被调方的 R0（receiver）。
//
//   ⚠ 安全性：参数来源用「帧索引 + 寄存器号」而非裸指针。
//     call_reg 内部可能 realloc vm.frames，调用方帧若是 inline_locals，
//     其地址会变（vm_grow_frames 会修正 frame->locals，但调用方早就抓走的
//     裸指针不会跟着变）。所以这里每次都重新取 caller 再算 src。
// ============================================================================
// ============================================================================
// async 调用：为 async 闭包**建协程**并把 Future 写回目标寄存器（唯一实现）
// ----------------------------------------------------------------------------
// 语义：async 函数**一旦被调用就返回 Future**，与调用写法无关。
//   * 编译器静态知道 callee 是 async 时直接发 OP_ASYNC_CALL；
//   * 静态不知道时（`var f = some_async; f()`、把 async 函数当参数传、经绑定方法调用）
//     由 OP_CALL / OP_TAIL_CALL 在**运行期**按 closure->function->is_async 判定后走这里。
// 漏掉运行期这一层的后果（实测）：函数体被**同步执行** ——
//   ① 返回值不是 Future（`var r = f(3)` 拿到裸值 6）；
//   ② 体里的 `await` 把当前协程的帧搞乱 ⇒ 整段**静默无输出**、exit=0（C4）。
// 参数：regs/reg_base —— 实参在调用方帧里的位置（实参 = regs[reg_base+1 .. +arg_count]）；
//   self_val/need_self —— 绑定方法把 receiver 补到第 0 个实参（被调方 locals[0] = self）；
//   dst_reg —— Future 写回调用方的哪个寄存器。
// 返回 0 成功；-1 失败（运行时错误已注册，调用方按异常路径处理）。
// ============================================================================
static inline int vm_start_async_call(VM* vm_ptr, ObjClosure* closure, Value* regs,
                                      int reg_base, int arg_count, int dst_reg,
                                      Value self_val, int need_self, int line) {
    if (unlikely(!closure)) return -1;
    ObjCoroutine* co = coroutine_new(closure);
    if (unlikely(!co)) { runtime_error("内存不足，无法创建协程", line); return -1; }

    int total = arg_count + (need_self ? 1 : 0);
    if (total > 0) {
        co->initial_args = (Value*)malloc((size_t)total * sizeof(Value));
        if (unlikely(!co->initial_args)) { runtime_error("内存分配失败", line); return -1; }
        int w = 0;
        if (need_self) co->initial_args[w++] = self_val;
        for (int i = 0; i < arg_count; i++) co->initial_args[w++] = regs[reg_base + 1 + i];
        co->initial_arg_count = w;
    }

    ObjFuture* task_future = future_new();
    if (unlikely(!task_future)) { runtime_error("内存不足，无法创建 Future", line); return -1; }
    co->task_future = task_future;

    if (vm_ptr->event_loop) event_loop_add_ready(vm_ptr->event_loop, co);
    regs[dst_reg] = val_obj((Object*)task_future);
    return 0;
}

// no_convert：编译器已核实"每个实参的静态类型 == 形参声明类型"（见 OP_CALL_GLOBAL_FUNC_TYPED）
//   ⇒ 跳过逐参数的类型转换检查，只做复制。
static inline int call_reg_impl(ObjClosure* closure, int has_self, Value self,
                                int arg_base, int arg_count,
                                int ret_reg, int ret_want, int line, int no_convert);

// 普通调用（含逐参数类型转换）
static inline int call_reg(ObjClosure* closure, int has_self, Value self,
                           int arg_base, int arg_count,
                           int ret_reg, int ret_want, int line) {
    return call_reg_impl(closure, has_self, self, arg_base, arg_count,
                         ret_reg, ret_want, line, 0);
}

// 类型已核实的调用（跳过逐参数转换；T10-⑤）
static inline int call_reg_typed(ObjClosure* closure, int arg_base, int arg_count,
                                 int ret_reg, int ret_want, int line) {
    return call_reg_impl(closure, 0, val_null(), arg_base, arg_count,
                         ret_reg, ret_want, line, 1);
}

static inline int call_reg_impl(ObjClosure* closure, int has_self, Value self,
                                int arg_base, int arg_count,
                                int ret_reg, int ret_want, int line, int no_convert) {
    ObjFunction* func = closure->function;

    if (unlikely(vm.frame_cnt >= vm.frame_capacity)) {
        // S1：硬上限——无限递归时逐倍扩容直到内存耗尽只会报"分配失败"，掩盖真实原因
        if (vm.frame_cnt >= VM_MAX_FRAMES) {
            char depth_buf[96];
            snprintf(depth_buf, sizeof(depth_buf),
                     "调用栈溢出：递归深度已达 %d（可能是无限递归）", vm.frame_cnt);
            runtime_error(depth_buf, line);
            return 0;
        }
        if (!vm_grow_frames(current_exec_vm)) {
            runtime_error("调用栈内存分配失败", line);
            return 0;
        }
    }

    // 扩容之后重新取调用方帧（frame 数组可能已被搬移）
    int caller_idx = vm.frame_cnt - 1;
    CallFrame* caller = &vm.frames[caller_idx];
    Value* src = caller->locals + arg_base;

    int need = arg_count + (has_self ? 1 : 0);
    int local_count = func->local_count;
    if (local_count < need) local_count = need;   // 防御：可变参数超出声明槽位
    if (local_count <= 0) local_count = 1;

    CallFrame* frame = &vm.frames[vm.frame_cnt];
    Value* locals;
    int locals_is_dynamic = 0;
    int locals_capacity = 0;   // 见 CallFrame.locals_capacity

    // ★ 零拷贝原地帧（照 Lua 参考实现的做法）：被调方的 R0.. 直接就是调用方的
    //   实参区 R[arg_base..] —— 实参一个都不搬、非参数槽位也不 nil 填充、更不占新内存。
    //   Lua 的 luaD_precall / prepCallInfo 正是这么做的：新帧的寄存器区原地复用调用方
    //   栈上那一块。改动前每次调用都要「另取一块 locals 数组 + 逐个复制实参 +
    //   把其余槽位全填 null」，对「小叶子函数 × 海量调用」正是最大的一笔固定开销。
    //   条件（缺一不可）：
    //     ① 无隐式 self —— 有 self 时被调方 R0 = self、实参整体右移一位，与调用方
    //        实参区错开，不能直接别名（绑定方法调用退回下面的复制路径）；
    //     ② 不在协程里 —— 协程 yield 会把整帧 memcpy 出去保存、resume 再拷回来
    //        （13_async_ffi.inc / vm.c），裸的 locals 指针会指到已失效的位置；
    //     ③ 调用方 locals 非空、且**物理容量**装得下被调方的整帧。判定用 locals_capacity
    //        而不是 caller->local_count：后者是**寄存器高水位/GC 扫描范围**（往往只有
    //        一二十），而自有 inline_locals 的物理容量是 INLINE_LOCALS_MAX —— 用高水位
    //        判定会白白漏掉一大片本来可以安全别名的空间（数组明明有 64 格却没被用满）。
    //        安全前提：整个 inline_locals 已由 vm_grow_frames 一次性填 null，且此后帧槽里
    //        永远只存合法 Value，所以容量内的任何格子被 GC 扫到都不会误判成对象指针。
    //   别名时 src == locals（都等于 caller->locals + arg_base）且 k == 0，
    //   所以下面「逐参数类型转换」那段循环原地读写，无需改动。
    int locals_is_alias = (!has_self && vm.current_coroutine == NULL &&
                           caller->locals != NULL &&
                           arg_base + local_count <= caller->locals_capacity);
    if (likely(locals_is_alias)) {
        locals = caller->locals + arg_base;
        locals_capacity = caller->locals_capacity - arg_base;
    } else if (likely(local_count <= INLINE_LOCALS_MAX)) {
        locals = frame->inline_locals;
        locals_capacity = INLINE_LOCALS_MAX;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
        locals_capacity = local_count;
    }

    TypeKind* param_types = func->param_types;
    int arity = func->arity;
    int k = 0;
    if (has_self) locals[k++] = self;
    if (no_convert) {
        // 类型已核实（codegen 只在"实参静态类型 == 形参声明类型"时发 typed 版）⇒ 直接复制
        // （原地帧：实参本来就在被调方的槽位里，无需复制）
        if (!locals_is_alias) {
            for (int i = 0; i < arg_count; i++) locals[k + i] = src[i];
        }
    } else {
    for (int i = 0; i < arg_count; i++) {
        Value arg = src[i];
        if (param_types && i < arity) {
            TypeKind expected_type = param_types[i];
            if (expected_type == TYPE_FLOAT && val_is_int(arg)) {
                arg = val_float((double)val_as_int(arg));
            } else if (expected_type == TYPE_FLOAT && val_is_bigint(arg)) {
                arg = val_float(bigint_to_double(val_as_bigint(arg)));
            } else if (expected_type == TYPE_INT && val_is_float(arg)) {
                arg = val_int((int)val_as_num(arg));
            }
        }
        locals[k + i] = arg;
    }
    }
    // 原地帧的非参数槽位不填 null：它们本来就是调用方的寄存器，里面一定是合法 Value
    //   （调用方建帧时整块数组已初始化过），GC 扫描安全；而被调方的局部变量在声明处
    //   必被赋值（gen_var_decl 总会发初始化/默认值）。
    if (!locals_is_alias) {
        for (int i = k + arg_count; i < local_count; i++) {
            locals[i] = val_null();
        }
    }

    vm.frame_cnt++;
    frame->closure = closure;
    frame->chunk = func->chunk;
    frame->ip = func->chunk->code;
    frame->stack_base = vm.sp;
    frame->slot_count = local_count;
    frame->locals = locals;
    frame->local_count = local_count;
    frame->locals_is_dynamic = locals_is_dynamic;
    frame->locals_capacity = locals_capacity;
    // 帧复用优化：无 try 的函数跳过 try 字段初始化（读取点用 FRAME_TRY_VALID 守护）
    if (func->has_try) {
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->catch_finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
    }
    frame->module = func->module;
    frame->has_captures = 0;   // 由 OP_CLOSURE 在**真建了指向本帧窗口的 upvalue** 时置 1（见 02_loads.inc）

    // 结果回写目标（寄存器式调用约定）
    frame->ret_frame = caller_idx;
    frame->ret_reg = ret_reg;
    frame->ret_want = ret_want;

    return 1;
}

// 调用值（处理不同类型的可调用对象）
static inline int call_value(Value callee, int arg_count, int line) {
    // 检查是否是null（函数未定义）
    if (val_is_null(callee)) {
        runtime_error("函数未定义", line);
        return 0;
    }
    if (val_is_obj(callee)) {
        switch (val_as_obj(callee)->type) {
            case OBJ_CLOSURE:
                return call((ObjClosure*)val_as_obj(callee), arg_count, line);
            case OBJ_FUNCTION: {
                ObjFunction* func = (ObjFunction*)val_as_obj(callee);
                ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (!closure) {
                    runtime_error("内存分配失败", line);
                    return 0;
                }
                closure->function = func;
                closure->upvalue_count = 0;
                // upvalues 数组初始化为 NULL
                for (int i = 0; i < MAX_UPVALUES; i++) {
                    closure->upvalues[i] = NULL;
                }
                return call(closure, arg_count, line);
            }
            case OBJ_NATIVE: {
                ObjNative* native = (ObjNative*)val_as_obj(callee);

                // 参数在 callee 之前
                // 栈布局: [...][arg1][arg2][callee]
                // 参数从 vm.sp - arg_count - 1 开始
                Value result = native->function(arg_count, vm.stack + vm.sp - arg_count - 1);
                // 检查 native 函数是否抛出异常（通过 native_throw_error）
                if (vm.has_exception) {
                    // 获取异常信息并调用 throw_runtime_error 来处理异常
                    char* msg = NULL;
                    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                        msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                    } else {
                        msg = value_to_string(vm.exception);
                    }
                    error_clear();
                    throw_runtime_error(msg, line);
                    free(msg);
                    return 0;
                }
                vm.sp -= arg_count + 1;
                vm_stack_push(&vm, result);
                return 1;
            }
            case OBJ_BOUND_METHOD: {
                ObjBoundMethod* bound = (ObjBoundMethod*)val_as_obj(callee);

                // 处理绑定闭包方法（用户自定义 struct 方法通过 OP_GET_PROPERTY 获取）
                if (bound->closure) {
                    // 将 receiver 插入到参数列表开头
                    // 栈布局: [...][arg1][arg2][bound_method]
                    // 需要变为: [...][receiver][arg1][arg2][closure]
                    vm.sp--;
                    for (int i = 0; i < arg_count; i++) {
                        vm.stack[vm.sp - i] = vm.stack[vm.sp - i - 1];
                    }
                    vm.stack[vm.sp - arg_count] = bound->receiver;
                    vm.sp++;

                    // 压入闭包并调用
                    vm_stack_push(&vm, val_obj((Object*)bound->closure));
                    return call(bound->closure, arg_count + 1, line);
                }

                ObjNative* native = bound->method;

                // 将 receiver 插入到参数列表开头
                // 栈布局: [...][arg1][arg2][bound_method]
                // 需要变为: [...][receiver][arg1][arg2]
                
                // 先弹出绑定方法
                vm.sp--;
                
                // 将参数向后移动一位
                for (int i = 0; i < arg_count; i++) {
                    vm.stack[vm.sp - i] = vm.stack[vm.sp - i - 1];
                }
                
                // 插入 receiver
                vm.stack[vm.sp - arg_count] = bound->receiver;
                vm.sp++;
                
                // 调用原生函数
                Value result = native->function(arg_count + 1, vm.stack + vm.sp - arg_count - 1);

                // 检查 native 函数是否抛出异常（通过 native_throw_error）
                if (vm.has_exception) {
                    // 获取异常信息并调用 throw_runtime_error 来处理异常
                    char* msg = NULL;
                    if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                        msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                    } else {
                        msg = value_to_string(vm.exception);
                    }
                    error_clear();
                    throw_runtime_error(msg, line);
                    free(msg);
                    return 0;
                }

                vm.sp -= arg_count + 1;
                vm_stack_push(&vm, result);
                return 1;
            }
            case OBJ_STRUCT_DEF: {
                ObjStructDef* def = (ObjStructDef*)val_as_obj(callee);
                
                // 注意：参数数量检查已在编译时完成
                
                // 创建实例
                ObjStruct* obj = struct_instance_new(def);
                if (!obj) {
                    runtime_error("内存不足，无法创建结构体实例", line);
                    return 0;
                }
                
                // 设置参数字段（位置参数按顺序赋值）
                for (int i = 0; i < arg_count; i++) {
                    Value field_value = vm.stack[vm.sp - arg_count - 1 + i];
                    
                    // 检查类型并进行自动类型提升
                    TypeKind expected_type = def->fields[i].type;
                    
                    // int -> float 自动提升
                    if (expected_type == TYPE_FLOAT && val_is_int(field_value)) {
                        field_value = val_float((double)val_as_int(field_value));
                    }
                    // bigint -> float 自动提升
                    else if (expected_type == TYPE_FLOAT && val_is_bigint(field_value)) {
                        field_value = val_float(bigint_to_double(val_as_bigint(field_value)));
                    }
                    
                    struct_set_field(obj, i, field_value);
                }
                
                // 清理栈并压入实例
                vm.sp -= arg_count + 1;
                vm_stack_push(&vm, val_obj((Object*)obj));
                return 1;
            }
            default:
                throw_runtime_error("只能调用函数（对象类型不正确）", line);
                return 0;
        }
    }

    throw_runtime_error("只能调用函数（不是对象类型）", line);
    return 0;
}
