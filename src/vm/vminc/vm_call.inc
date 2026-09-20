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
    
    // 优化：小函数使用内联数组，避免 malloc/free 开销
    CallFrame* frame = &vm.frames[vm.frame_cnt];
    if (likely(local_count <= INLINE_LOCALS_MAX)) {
        locals = frame->inline_locals;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
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
    // 帧复用优化：无 try 的函数跳过 7 个 try 字段初始化（~50B/次），
    // 读取点必须用 FRAME_TRY_VALID 守护（脏值会导致异常误捕获/误跳 finally）
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

    return 1;
}

// 调用闭包函数（跳过参数类型检查快速路径）
// 当编译期已确认所有实参类型与形参精确匹配时使用
// 与 call() 的唯一区别：跳过 param_types 类型转换循环，用 memcpy 直接拷贝参数
static inline int call_no_type_check(ObjClosure* closure, int arg_count, int line) {
    ObjFunction* func = closure->function;

    if (unlikely(vm.frame_cnt >= vm.frame_capacity)) {
        if (!vm_grow_frames(current_exec_vm)) {
            runtime_error("调用栈内存分配失败", line);
            return 0;
        }
    }

    int local_count = func->local_count;
    Value* locals;
    int locals_is_dynamic = 0;

    CallFrame* frame = &vm.frames[vm.frame_cnt];
    if (likely(local_count <= INLINE_LOCALS_MAX)) {
        locals = frame->inline_locals;
    } else {
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
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
