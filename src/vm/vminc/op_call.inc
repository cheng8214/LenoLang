// ============================================================================
// 函数调用类操作码 (OP_CALL)
// ============================================================================

#include <string.h>  // for memmove

OPCODE(OP_CALL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    int arg_count = READ_SHORT();
    Value callee = vm_stack_peek(&vm, 0);

    if (likely(val_is_obj(callee) && val_as_obj(callee)->type == OBJ_CLOSURE)) {
        ObjClosure* closure = (ObjClosure*)val_as_obj(callee);
        if (!call(closure, arg_count, GET_CURRENT_LINE())) {
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
        
        // 检查参数数量
#ifndef LENO_RELEASE
        if (runtime_type_check && arg_count != function->arity) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "调用函数 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                    function->name, function->arity, arg_count);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
#endif
        
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

            // 泛型参数类型检查：如果参数类型是泛型类型参数（如 T, K, V），
            // 从参数栈中获取 self 实例，查找实际类型进行匹配
#ifndef LENO_RELEASE
            if (runtime_type_check && function->param_generic_names &&
                i < function->arity && function->param_generic_names[i]) {
                const char* generic_name = function->param_generic_names[i];
                // self 是第一个参数
                Value self_val = vm.stack[frame->stack_base + 0];
                if (val_is_obj(self_val) && val_as_obj(self_val)->type == OBJ_STRUCT) {
                    ObjStruct* self_obj = (ObjStruct*)val_as_obj(self_val);
                    ObjStructDef* self_def = self_obj->def;
                    if (self_def->type_param_count > 0 && self_def->type_param_names &&
                        self_obj->generic_type_arg_count > 0 && self_obj->generic_type_args) {
                        for (int pi = 0; pi < self_def->type_param_count && pi < self_obj->generic_type_arg_count; pi++) {
                            if (self_def->type_param_names[pi] && strcmp(self_def->type_param_names[pi], generic_name) == 0) {
                                const char* actual_type = self_obj->generic_type_args[pi];
                                int type_match = 0;
                                if (strcmp(actual_type, "int") == 0) {
                                    type_match = val_is_int(arg) || val_is_bigint(arg);
                                } else if (strcmp(actual_type, "float") == 0) {
                                    type_match = val_is_float(arg) || val_is_int(arg) || val_is_bigint(arg);
                                } else if (strcmp(actual_type, "string") == 0) {
                                    type_match = val_is_obj(arg) && val_as_obj(arg)->type == OBJ_STRING;
                                } else if (strcmp(actual_type, "bool") == 0) {
                                    type_match = val_is_bool(arg);
                                } else if (strncmp(actual_type, "struct ", 7) == 0) {
                                    type_match = val_is_obj(arg) && val_as_obj(arg)->type == OBJ_STRUCT;
                                } else {
                                    if (val_is_obj(arg) && val_as_obj(arg)->type == OBJ_STRUCT) {
                                        ObjStruct* arg_struct = (ObjStruct*)val_as_obj(arg);
                                        if (arg_struct->def->name && strcmp(arg_struct->def->name, actual_type) == 0) {
                                            type_match = 1;
                                        }
                                    }
                                }
                                if (!type_match) {
                                    char msg[BUFFER_MEDIUM];
                                    char* arg_str = value_to_string(arg);
                                    snprintf(msg, sizeof(msg),
                                        "泛型参数类型不匹配: 方法 '%s' 参数 %d 期望类型 %s (泛型参数 %s), 实际传入 '%s'",
                                        function->name ? function->name : "unknown", i, actual_type, generic_name, arg_str);
                                    free(arg_str);
                                    throw_runtime_error(msg, GET_CURRENT_LINE());
                                    frame = &vm.frames[vm.frame_cnt - 1];
                                    DISPATCH;
                                }
                                break;
                            }
                        }
                    }
                }
            }
#endif
            
            frame->locals[i] = arg;
        }
        
        // 初始化其他局部变量为 null
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
        frame->module = function->module;

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

        // 弹出 callee
        vm.sp--;

    } else if (val_is_obj(callee) && val_as_obj(callee)->type == OBJ_NATIVE) {
        // 原生函数：直接调用，然后返回
        ObjNative* native = (ObjNative*)val_as_obj(callee);
#ifndef LENO_RELEASE
        if (runtime_type_check && native->arity >= 0 && arg_count != native->arity) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "调用函数 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                    native->name, native->arity, arg_count);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
#endif

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
        ObjNative* native = bound->method;

#ifndef LENO_RELEASE
        if (runtime_type_check && native->arity >= 0 && arg_count + 1 != native->arity) {
            char msg[BUFFER_MEDIUM];
            snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                    native->name, native->arity - 1, arg_count);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
#endif

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
    Value result = vm_stack_pop(&vm);

    // 函数返回是天然的 GC 安全点，每 256 次 return 检查一次延迟 GC
    // 使用计数器摊销开销，避免高频递归（如 fib）的性能回退
    static int gc_return_counter = 0;
    if (unlikely(++gc_return_counter >= 256)) {
        gc_return_counter = 0;
        if (gc.deferred_gc && gc.enabled && !gc.running) {
            gc_try_collect_deferred();
        }
    }

    // 检查是否有 finally 块需要执行（且当前不在 finally 中）
    if (frame->finally_ip != NULL && !frame->in_finally) {
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
    if (locals != NULL && local_count > 0) {
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
