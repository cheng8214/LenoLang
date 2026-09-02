// ============================================================================
// 函数调用类操作码 (OP_CALL)
// ============================================================================

#include <string.h>  // for memmove

OPCODE(OP_CALL) {
    int arg_count = READ_SHORT();
    Value callee = vm_stack_peek_fast(&vm, 0);

    if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
        ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
        // 闭包调用是热路径，line 仅用于错误报告，传 0 避免 GET_CURRENT_LINE() 的数组访问开销
        if (!call(closure, arg_count, 0)) {
            if (!vm.has_exception) {
                return -1;
            }
        }
    } else if (!call_value(callee, arg_count, GET_CURRENT_LINE())) {
        if (!vm.has_exception) {
            return -1;
        }
    }
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_TAIL_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int arg_count = READ_SHORT();

    // 获取 callee（在栈顶）
    Value callee = vm_stack_peek(&vm, 0);

    // 关闭当前帧的 upvalues（包括嵌套函数捕获的 upvalue）
    // 需要关闭所有指向当前 frame->locals 的 upvalue，而不仅仅是当前 frame 的 upvalue
    if (frame->locals != NULL && frame->local_count > 0) {
        close_upvalues(frame->locals, frame->local_count, 1);
    }
    
    // 保存当前栈基址，稍后需要用它来移动参数
    int old_stack_base = frame->stack_base;
    
    // 释放当前帧的 locals 数组（仅当使用动态分配时才需要 free）
    if (frame->locals != NULL && frame->locals_is_dynamic) {
        free(frame->locals);
        frame->locals = NULL;
    }
    
    // 计算参数在新栈中的位置
    // 当前栈布局: [old_locals...][arg1][arg2]...[argN][callee]
    // 新的布局应该是: [arg1][arg2]...[argN]（在当前帧位置）
    
    // 将参数和 callee 移动到栈基址处
    // callee 位置: vm.sp - 1
    // 第一个参数位置: vm.sp - arg_count - 1
    // 目标位置: old_stack_base
    
    Value* args_start = vm.stack + vm.sp - arg_count - 1;
    
    // 移动参数到新的位置（复用当前栈帧）
    // 使用 memmove 替代循环，处理内存重叠情况
    memmove(&vm.stack[old_stack_base], args_start, (arg_count + 1) * sizeof(Value));
    
    // 调整栈指针
    vm.sp = old_stack_base + arg_count + 1;
    
    // 现在调用函数，它会复用当前帧
    // 但我们需要特殊处理：不让 call_value 创建新帧
    
    if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE) {
        ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
        ObjFunction* function = closure->function;

        // 复用当前帧，只更新必要信息
        frame->closure = closure;
        frame->chunk = function->chunk;
        frame->ip = function->chunk->code;
        // stack_base 保持不变（已调整）
        
        // 分配新的 locals 数组（根据大小选择内联或动态分配）
        int local_count = function->local_count;
        if (local_count <= INLINE_LOCALS_MAX) {
            frame->locals = frame->inline_locals;
            frame->locals_is_dynamic = 0;
        } else {
            frame->locals = (Value*)malloc(local_count * sizeof(Value));
            if (!frame->locals) {
                runtime_error("局部变量内存分配失败", GET_CURRENT_LINE());
                return -1;
            }
            frame->locals_is_dynamic = 1;
        }
        
        // 初始化参数（带类型转换）
        for (int i = 0; i < arg_count; i++) {
            Value arg = vm.stack[frame->stack_base + i];
            
            // 根据参数类型进行类型转换
            if (function->param_types && i < function->arity) {
                TypeKind expected_type = function->param_types[i];

                // int -> float 自动提升
                if (expected_type == TYPE_FLOAT && val_is_int(arg)) {
                    arg = val_float((double)val_as_int(arg));
                }
                // bigint -> float 自动提升
                else if (expected_type == TYPE_FLOAT && val_is_bigint(arg)) {
                    arg = val_float(bigint_to_double(val_as_bigint(arg)));
                }
                // float -> int 自动截断
                else if (expected_type == TYPE_INT && val_is_float(arg)) {
                    arg = val_int((int)val_as_num(arg));
                }
            }

            frame->locals[i] = arg;
        }
        
        // 初始化其他局部变量为 null
        for (int i = arg_count; i < local_count; i++) {
            frame->locals[i] = val_null();
        }
        
        frame->local_count = local_count;
        frame->slot_count = local_count;
        // 帧复用优化：无 try 的函数跳过 try 字段初始化（读取点用 FRAME_TRY_VALID 守护）
        if (function->has_try) {
            frame->catch_ip = NULL;
            frame->finally_ip = NULL;
            frame->prev_catch_ip = NULL;
            frame->prev_finally_ip = NULL;
            frame->in_finally = 0;
            frame->try_return_value = val_null();
            frame->has_try_return = 0;
        }
        frame->module = function->module;

        // 应用待处理的泛型类型参数到 closure
        if (function->type_param_count > 0 && vm.pending_type_args) {
            closure->type_param_count = vm.pending_type_arg_count;
            closure->type_param_args = vm.pending_type_args;
            vm.pending_type_args = NULL;
            vm.pending_type_arg_count = 0;
        }

        // 弹出 callee
        vm.sp--;

    } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_FUNCTION) {
        ObjFunction* func = (ObjFunction*)val_as_obj(callee);
        ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
        if (!closure) {
            runtime_error("内存分配失败", GET_CURRENT_LINE());
            return -1;
        }
        closure->function = func;
        closure->upvalue_count = 0;
        for (int i = 0; i < MAX_UPVALUES; i++) {
            closure->upvalues[i] = NULL;
        }
        
        // 复用当前帧
        frame->closure = closure;
        frame->chunk = func->chunk;
        frame->ip = func->chunk->code;
        
        // 分配新的 locals 数组（根据大小选择内联或动态分配）
        int local_count = func->local_count;
        if (local_count <= INLINE_LOCALS_MAX) {
            frame->locals = frame->inline_locals;
            frame->locals_is_dynamic = 0;
        } else {
            frame->locals = (Value*)malloc(local_count * sizeof(Value));
            if (!frame->locals) {
                runtime_error("局部变量内存分配失败", GET_CURRENT_LINE());
                return -1;
            }
            frame->locals_is_dynamic = 1;
        }
        
        for (int i = 0; i < arg_count; i++) {
            frame->locals[i] = vm.stack[frame->stack_base + i];
        }
        for (int i = arg_count; i < local_count; i++) {
            frame->locals[i] = val_null();
        }
        
        frame->local_count = local_count;
        frame->slot_count = local_count;
        frame->catch_ip = NULL;
        frame->finally_ip = NULL;
        frame->prev_catch_ip = NULL;
        frame->prev_finally_ip = NULL;
        frame->in_finally = 0;
        frame->try_return_value = val_null();
        frame->has_try_return = 0;
        frame->module = func->module;

        // 应用待处理的泛型类型参数到 closure
        if (func->type_param_count > 0 && vm.pending_type_args) {
            closure->type_param_count = vm.pending_type_arg_count;
            closure->type_param_args = vm.pending_type_args;
            vm.pending_type_args = NULL;
            vm.pending_type_arg_count = 0;
        }

        // 弹出 callee
        vm.sp--;

    } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_NATIVE) {
        // 原生函数：直接调用，然后返回
        ObjNative* native = (ObjNative*)val_as_obj(callee);

        Value result = native->function(arg_count, vm.stack + vm.sp - arg_count - 1);

        // 检查是否有异常（原生函数可能通过 native_throw_error 抛出异常）
        if (vm.has_exception) {
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
            } else {
                msg = value_to_string(vm.exception);
            }
            int line = GET_CURRENT_LINE();
            error_clear();
            throw_runtime_error(msg, line);
            free(msg);
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }

        vm.sp = frame->stack_base;
        vm_stack_push(&vm, result);

    } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_BOUND_METHOD) {
        ObjBoundMethod* bound = (ObjBoundMethod*)val_as_obj(callee);

        // 处理绑定闭包方法（用户自定义 struct 方法通过 OP_GET_PROPERTY 获取）
        if (bound->closure) {
            // 将 receiver 插入到参数列表开头
            Value* args_start = vm.stack + vm.sp - arg_count - 1;
            for (int i = arg_count - 1; i >= 0; i--) {
                args_start[i + 1] = args_start[i];
            }
            args_start[0] = bound->receiver;
            vm.sp = old_stack_base + arg_count + 1;

            // 关闭当前帧的 upvalues
            if (frame->locals != NULL && frame->local_count > 0) {
                close_upvalues(frame->locals, frame->local_count, 1);
            }

            // 释放当前帧的 locals 数组
            if (frame->locals != NULL && frame->locals_is_dynamic) {
                free(frame->locals);
                frame->locals = NULL;
            }

            // 调用闭包（复用当前帧）
            ObjClosure* closure = bound->closure;
            ObjFunction* func = closure->function;

            frame->closure = closure;
            frame->chunk = func->chunk;
            frame->ip = func->chunk->code;

            int local_count = func->local_count;
            if (local_count <= INLINE_LOCALS_MAX) {
                frame->locals = frame->inline_locals;
                frame->locals_is_dynamic = 0;
            } else {
                frame->locals = (Value*)malloc(local_count * sizeof(Value));
                if (!frame->locals) {
                    runtime_error("局部变量内存分配失败", GET_CURRENT_LINE());
                    return -1;
                }
                frame->locals_is_dynamic = 1;
            }

            for (int i = 0; i < arg_count + 1 && i < local_count; i++) {
                Value arg = vm.stack[old_stack_base + i];
                if (func->param_types && i < func->arity) {
                    TypeKind expected_type = func->param_types[i];
                    if (expected_type == TYPE_FLOAT && val_is_int(arg)) {
                        arg = val_float((double)val_as_int(arg));
                    } else if (expected_type == TYPE_FLOAT && val_is_bigint(arg)) {
                        arg = val_float(bigint_to_double(val_as_bigint(arg)));
                    } else if (expected_type == TYPE_INT && val_is_float(arg)) {
                        arg = val_int((int)val_as_num(arg));
                    }
                }
                frame->locals[i] = arg;
            }
            for (int i = arg_count + 1; i < local_count; i++) {
                frame->locals[i] = val_null();
            }

            frame->local_count = local_count;
            frame->slot_count = local_count;
            // 帧复用优化：无 try 的函数跳过 try 字段初始化（读取点用 FRAME_TRY_VALID 守护）
            if (func->has_try) {
                frame->catch_ip = NULL;
                frame->finally_ip = NULL;
                frame->prev_catch_ip = NULL;
                frame->prev_finally_ip = NULL;
                frame->in_finally = 0;
                frame->try_return_value = val_null();
                frame->has_try_return = 0;
            }
            frame->module = func->module;
            frame->has_captures = (closure->upvalue_count > 0) ? 1 : 0;

            // 应用待处理的泛型类型参数到 closure
            if (func->type_param_count > 0 && vm.pending_type_args) {
                closure->type_param_count = vm.pending_type_arg_count;
                closure->type_param_args = vm.pending_type_args;
                vm.pending_type_args = NULL;
                vm.pending_type_arg_count = 0;
            }

            vm.sp--;
            DISPATCH;
        }

        ObjNative* native = bound->method;

        // 将 receiver 插入到参数列表开头
        // 当前栈布局: [old_locals...][arg1][arg2]...[argN][bound_method]
        // 目标布局: [receiver][arg1][arg2]...[argN]

        // 计算参数在新栈中的位置
        Value* args_start = vm.stack + vm.sp - arg_count - 1;

        // 移动参数：将参数向后移动一位，为 receiver 腾出位置
        for (int i = arg_count - 1; i >= 0; i--) {
            args_start[i + 1] = args_start[i];
        }

        // 插入 receiver
        args_start[0] = bound->receiver;

        // 调整栈指针（去掉 bound_method，加上 receiver，净变化为 0）
        // 但我们需要调整 vm.sp 以匹配新的布局
        vm.sp = old_stack_base + arg_count + 1;

        // 调用原生函数
        Value result = native->function(arg_count + 1, vm.stack + old_stack_base);

        // 检查是否有异常（原生函数可能通过 native_throw_error 抛出异常）
        if (vm.has_exception) {
            char* msg = NULL;
            if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
            } else {
                msg = value_to_string(vm.exception);
            }
            int line = GET_CURRENT_LINE();
            error_clear();
            throw_runtime_error(msg, line);
            free(msg);
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }

        vm.sp = frame->stack_base;
        vm_stack_push(&vm, result);

    } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_STRUCT_DEF) {
        // 尾调用 struct 定义创建实例
        ObjStructDef* def = (ObjStructDef*)val_as_obj(callee);

        // 注意：参数数量检查已在编译时完成

        // 创建实例
        ObjStruct* obj = struct_instance_new(def);
        if (!obj) {
            runtime_error("内存不足，无法创建结构体实例", GET_CURRENT_LINE());
            return -1;
        }

        // 设置参数字段（位置参数按顺序赋值）
        for (int i = 0; i < arg_count; i++) {
            Value field_value = vm.stack[old_stack_base + i];

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

        // 清理栈并压入结果
        vm.sp = old_stack_base;
        vm_stack_push(&vm, val_obj((Object*)obj));

        // 继续执行（相当于已经返回了）

    } else {
        throw_runtime_error("只能调用函数", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
    }

    DISPATCH;
}

// ============================================================================
// 原生函数调用合并指令（省掉 OP_GET_NATIVE + OP_CALL/TAIL_CALL 配对）
// ============================================================================

OPCODE(OP_CALL_NATIVE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_idx = READ_SHORT();
    int arg_count = READ_SHORT();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("native 函数名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

#ifdef VM_USE_THREAD_LOCAL
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    ObjNative* native = native_find_function(name);
    if (!native) {
        runtime_error("未找到 native 函数", GET_CURRENT_LINE());
        return -1;
    }
#else
    uint8_t* ip_before = frame->ip - 5;  // OP_CALL_NATIVE = 5 字节
    int cache_idx = ((uintptr_t)ip_before >> 2) & (IC_NATIVE_CACHE_SIZE - 1);
    InlineNativeCacheEntry* cache = &vm.ic_native_cache[cache_idx];
    uint32_t name_hash = ((ObjString*)val_as_obj(name_val))->hash;

    ObjNative* native;
    if (cache->valid && cache->name_hash == name_hash) {
        vm.ic_hits++;
        native = cache->fn;
    } else {
        vm.ic_misses++;
        const char* name = ((ObjString*)val_as_obj(name_val))->chars;
        native = native_find_function(name);
        if (!native) {
            runtime_error("未找到 native 函数", GET_CURRENT_LINE());
            return -1;
        }
        cache->valid = 1;
        cache->name_hash = name_hash;
        cache->fn = native;
    }
#endif

    Value result = native->function(arg_count, vm.stack + vm.sp - arg_count);

    if (vm.has_exception) {
        char* msg = NULL;
        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
        } else {
            msg = value_to_string(vm.exception);
        }
        int line = GET_CURRENT_LINE();
        error_clear();
        throw_runtime_error(msg, line);
        free(msg);
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    vm.sp -= arg_count;
    vm_stack_push(&vm, result);
    DISPATCH;
}

OPCODE(OP_TAIL_CALL_NATIVE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_idx = READ_SHORT();
    int arg_count = READ_SHORT();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("native 函数名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

#ifdef VM_USE_THREAD_LOCAL
    const char* name = ((ObjString*)val_as_obj(name_val))->chars;
    ObjNative* native = native_find_function(name);
    if (!native) {
        runtime_error("未找到 native 函数", GET_CURRENT_LINE());
        return -1;
    }
#else
    uint8_t* ip_before = frame->ip - 5;  // OP_TAIL_CALL_NATIVE = 5 字节
    int cache_idx = ((uintptr_t)ip_before >> 2) & (IC_NATIVE_CACHE_SIZE - 1);
    InlineNativeCacheEntry* cache = &vm.ic_native_cache[cache_idx];
    uint32_t name_hash = ((ObjString*)val_as_obj(name_val))->hash;

    ObjNative* native;
    if (cache->valid && cache->name_hash == name_hash) {
        vm.ic_hits++;
        native = cache->fn;
    } else {
        vm.ic_misses++;
        const char* name = ((ObjString*)val_as_obj(name_val))->chars;
        native = native_find_function(name);
        if (!native) {
            runtime_error("未找到 native 函数", GET_CURRENT_LINE());
            return -1;
        }
        cache->valid = 1;
        cache->name_hash = name_hash;
        cache->fn = native;
    }
#endif

    // 栈上只有 args（无 callee），args 在 vm.stack + vm.sp - arg_count
    Value result = native->function(arg_count, vm.stack + vm.sp - arg_count);

    if (vm.has_exception) {
        char* msg = NULL;
        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
        } else {
            msg = value_to_string(vm.exception);
        }
        int line = GET_CURRENT_LINE();
        error_clear();
        throw_runtime_error(msg, line);
        free(msg);
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    // 弹掉 args，push 结果（后续 OP_RETURN 会处理帧清理）
    vm.sp -= arg_count;
    vm_stack_push(&vm, result);
    DISPATCH;
}

OPCODE(OP_CLOSURE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    Value func_val = READ_CONSTANT();
    if (!val_is_obj(func_val) || val_as_obj(func_val)->type != OBJ_FUNCTION) {
        runtime_error("OP_CLOSURE: 常量不是函数对象", GET_CURRENT_LINE());
        return -1;
    }
    ObjFunction* function = (ObjFunction*)val_as_obj(func_val);
    ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
    if (!closure) return -1;

    closure->function = function;
    closure->upvalue_count = function->upvalue_count;

    for (int i = 0; i < function->upvalue_count; i++) {
        uint16_t is_local = READ_SHORT();
        uint16_t index = READ_SHORT();
        uint16_t is_value_capture = READ_SHORT();
        if (is_local) {
            if (frame->locals != NULL) {
                if (index < frame->local_count) {
                    if (is_value_capture) {
                        closure->upvalues[i] = new_upvalue(val_null());
                        closure->upvalues[i]->closed = frame->locals[index];
                        gc_write_barrier((Object*)closure->upvalues[i], frame->locals[index]);
                        closure->upvalues[i]->location = &closure->upvalues[i]->closed;
                    } else {
                        closure->upvalues[i] = capture_upvalue(&frame->locals[index]);
                    }
                } else {
                    runtime_error("闭包捕获变量索引越界", GET_CURRENT_LINE());
                    return -1;
                }
            } else {
                // 对于内联 locals 的情况
                if (is_value_capture) {
                    closure->upvalues[i] = new_upvalue(val_null());
                    closure->upvalues[i]->closed = vm.stack[frame->stack_base + index];
                    gc_write_barrier((Object*)closure->upvalues[i], vm.stack[frame->stack_base + index]);
                    closure->upvalues[i]->location = &closure->upvalues[i]->closed;
                } else {
                    closure->upvalues[i] = capture_upvalue(&vm.stack[frame->stack_base + index]);
                }
            }
        } else {
            // 非局部变量，从当前闭包中获取
            if (frame->closure == NULL) {
                runtime_error("没有闭包环境", GET_CURRENT_LINE());
                return -1;
            }
            if (index >= frame->closure->upvalue_count) {
                runtime_error("闭包捕获变量索引越界", GET_CURRENT_LINE());
                return -1;
            }
            closure->upvalues[i] = frame->closure->upvalues[index];
        }
    }

    vm_stack_push(&vm, val_obj((Object*)closure));
    DISPATCH;
}

OPCODE(OP_RETURN) {
    frame = &vm.frames[vm.frame_cnt - 1];

    // 函数返回是天然的 GC 安全点，每 256 次 return 检查一次延迟 GC
    // 使用计数器摊销开销，避免高频递归（如 fib）的性能回退
    // 注意：GC 检查必须在 vm_stack_pop 之前，确保返回值仍在栈上被 GC 扫描到
    // 计数器放在 VM 结构体中，避免 THREAD_LOCAL 的 TLS 访问开销
    if (unlikely(++vm.gc_return_counter >= 256)) {
        vm.gc_return_counter = 0;
        if (gc.deferred_gc && gc.enabled && !gc.running) {
            gc_try_collect_deferred();
        }
    }
    // 栈下溢检测：采样检查 sp 是否低于 stack_base（每 256 次返回检查一次）
    // 如遇栈下溢问题，将下面的 256 改为 1 即可每次返回都检查以定位出错函数
    #define STACK_UNDERFLOW_CHECK_INTERVAL 256
    if ((vm.frame_cnt & (STACK_UNDERFLOW_CHECK_INTERVAL - 1)) == 0 && vm.sp < frame->stack_base) {
        const char* _fn = (frame->closure && frame->closure->function && frame->closure->function->name) ? frame->closure->function->name : "<main>";
        const char* _file = (frame->chunk && frame->chunk->filename) ? frame->chunk->filename : "unknown";
        int _ip_off = (int)(frame->ip - frame->chunk->code);
        fprintf(stderr, "\n[VM 栈下溢] sp=%d, stack_base=%d, depth=%d\n  func=%s, file=%s, ip_offset=%d, frame_cnt=%d\n",
            vm.sp, frame->stack_base, vm.sp - frame->stack_base, _fn, _file, _ip_off, vm.frame_cnt);
        runtime_error("VM 栈下溢：栈指针低于当前帧基址，可能是编译器生成了多余的 OP_POP", 0);
        return -1;
    }

    Value result = vm_stack_pop_fast(&vm);

    // 构造函数：返回 self 而不是返回值表达式
    if (frame->closure && frame->closure->function &&
        frame->closure->function->is_ctor) {
        result = frame->locals[0];  // self
    }

    // 检查是否有 finally 块需要执行（且当前不在 finally 中）
    // FRAME_TRY_VALID：无 try 函数跳过了 try 字段初始化，finally_ip 为脏值
    if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
        // 保存返回值
        frame->try_return_value = result;
        frame->has_try_return = 1;
        // 跳转到 finally 块
        frame->ip = frame->finally_ip;
        // 将 null 压入栈保持栈平衡
        vm_stack_push(&vm, val_null());
        DISPATCH;
    }

    int stack_base = frame->stack_base;
    Value* locals = frame->locals;
    int local_count = frame->local_count;
    int locals_is_dynamic = frame->locals_is_dynamic;

    // 关闭所有指向当前函数局部变量的 upvalues
    // 这包括当前函数捕获的 upvalues，以及当前函数创建的闭包的 upvalues
    // 优化：直接检查 open_upvalues 是否为空，跳过无 upvalue 的函数（如 fib）
    // 注意：不能用 frame->has_captures，因为它只表示当前函数是否捕获父函数的 upvalue，
    // 不表示内部闭包是否捕获了当前函数的局部变量
    if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
        close_upvalues(locals, local_count, 1);
    }

    vm.frame_cnt--;

    if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
        vm.last_return_value = result;
        vm.stop_frame_cnt = 0;
        if (locals != NULL && locals_is_dynamic) {
            free(locals);
        }
        return 0;
    }

    if (vm.current_coroutine) {
        if (vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) {
            vm.current_coroutine->result = result;
            gc_write_barrier((Object*)vm.current_coroutine, result);
            if (locals != NULL && locals_is_dynamic) {
                free(locals);
            }
            return 0;
        }
    }

    if (vm.frame_cnt == 0) {
        vm.last_return_value = result;
        if (locals != NULL && locals_is_dynamic) {
            free(locals);
        }
        return 0;
    }

    vm.sp = stack_base;
    vm_stack_push(&vm, result);

    // 仅当使用动态分配时才需要 free
    if (locals != NULL && locals_is_dynamic) {
        free(locals);
    }

    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

// ============================================================================
// OP_RETURN_MULTI: 多值返回
// 字节格式: OP_RETURN_MULTI count(1)
// 栈布局: [ret0][ret1]...[retN-1]（N 个返回值在栈顶）
// 返回后调用方栈上留下 N 个值
// ============================================================================
OPCODE(OP_RETURN_MULTI) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int ret_count = READ_BYTE();

    // GC 安全点检查（同 OP_RETURN）
    if (unlikely(++vm.gc_return_counter >= 256)) {
        vm.gc_return_counter = 0;
        if (gc.deferred_gc && gc.enabled && !gc.running) {
            gc_try_collect_deferred();
        }
    }

    // 从栈上弹出 count 个返回值（保存到临时数组）
    Value results[16];  // 最多支持 16 个返回值
    if (ret_count > 16) {
        runtime_error("多返回值数量超过限制（最大 16）", 0);
        return -1;
    }
    for (int i = ret_count - 1; i >= 0; i--) {
        results[i] = vm_stack_pop_fast(&vm);
    }

    // 检查是否有 finally 块需要执行（且当前不在 finally 中）
    // 多返回值 + finally：保存第一个返回值，弹出其余的
    // FRAME_TRY_VALID：无 try 函数跳过了 try 字段初始化，finally_ip 为脏值
    if (FRAME_TRY_VALID(frame) && frame->finally_ip != NULL && !frame->in_finally) {
        // 保存返回值（只保存第一个，finally 后只返回第一个值）
        frame->try_return_value = results[0];
        frame->has_try_return = 1;
        // 跳转到 finally 块
        frame->ip = frame->finally_ip;
        // 将 null 压入栈保持栈平衡
        vm_stack_push(&vm, val_null());
        DISPATCH;
    }

    int stack_base = frame->stack_base;
    Value* locals = frame->locals;
    int local_count = frame->local_count;
    int locals_is_dynamic = frame->locals_is_dynamic;

    // 关闭 upvalues
    if (locals != NULL && local_count > 0 && vm.open_upvalues != NULL) {
        close_upvalues(locals, local_count, 1);
    }

    vm.frame_cnt--;

    if (vm.stop_frame_cnt > 0 && vm.frame_cnt <= vm.stop_frame_cnt) {
        vm.last_return_value = results[0];
        vm.stop_frame_cnt = 0;
        if (locals != NULL && locals_is_dynamic) {
            free(locals);
        }
        return 0;
    }

    if (vm.current_coroutine) {
        if (vm.frame_cnt < vm.current_coroutine->saved_frame_cnt) {
            vm.current_coroutine->result = results[0];
            gc_write_barrier((Object*)vm.current_coroutine, results[0]);
            if (locals != NULL && locals_is_dynamic) {
                free(locals);
            }
            return 0;
        }
    }

    if (vm.frame_cnt == 0) {
        vm.last_return_value = results[0];
        if (locals != NULL && locals_is_dynamic) {
            free(locals);
        }
        return 0;
    }

    // 恢复栈到调用前状态，压入多个返回值
    vm.sp = stack_base;
    for (int i = 0; i < ret_count; i++) {
        vm_stack_push(&vm, results[i]);
    }

    if (locals != NULL && locals_is_dynamic) {
        free(locals);
    }

    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

// ============================================================================
// 全局函数调用合并指令（省掉 OP_GET_GLOBAL_FUNC + OP_CALL 配对）
// 字节格式: OP_CALL_GLOBAL_FUNC func_slot(2) arg_count(2)
// 栈布局: [...][arg1][arg2]...[argN]  （无 callee 在栈顶）
// ============================================================================

OPCODE(OP_CALL_GLOBAL_FUNC) {
    uint16_t func_slot = READ_SHORT();
    int arg_count = READ_SHORT();

    if (likely(func_slot < vm.global_func_capacity)) {
        Value callee = vm.global_funcs[func_slot];
        if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
            // 闭包快速路径：在栈顶临时放入 callee 供 call() 读取
            vm_stack_push(&vm, callee);
            if (!call((ObjClosure*)val_as_obj(callee), arg_count, 0)) {
                if (!vm.has_exception) {
                    return -1;
                }
            }
        } else if (!call_value(callee, arg_count, GET_CURRENT_LINE())) {
            if (!vm.has_exception) {
                return -1;
            }
        }
    } else {
        runtime_error("全局函数索引越界", GET_CURRENT_LINE());
        return -1;
    }
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

// 全局函数调用（类型已确认版）：编译期确认参数类型匹配，跳过类型转换循环
// 字节格式与 OP_CALL_GLOBAL_FUNC 完全相同: func_slot(2) arg_count(2)
OPCODE(OP_CALL_GLOBAL_FUNC_TYPED) {
    uint16_t func_slot = READ_SHORT();
    int arg_count = READ_SHORT();

    if (likely(func_slot < vm.global_func_capacity)) {
        Value callee = vm.global_funcs[func_slot];
        if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
            // 闭包快速路径：在栈顶临时放入 callee 供 call_no_type_check() 读取
            vm_stack_push(&vm, callee);
            if (!call_no_type_check((ObjClosure*)val_as_obj(callee), arg_count, 0)) {
                if (!vm.has_exception) {
                    return -1;
                }
            }
        } else {
            // 非闭包回退到 call_value（罕见路径）
            vm_stack_push(&vm, callee);
            if (!call_value(callee, arg_count, GET_CURRENT_LINE())) {
                if (!vm.has_exception) {
                    return -1;
                }
            }
        }
    } else {
        runtime_error("全局函数索引越界", GET_CURRENT_LINE());
        return -1;
    }
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}
