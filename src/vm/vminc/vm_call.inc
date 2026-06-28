// ============================================================================
// 函数调用实现 - 从 vm_call.c 迁移
// ============================================================================

// 辅助函数：检查值是否匹配期望类型
static inline int value_matches_type(Value value, TypeKind expected) {
    switch (expected) {
        case TYPE_STRING:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING);
        case TYPE_INT:
            return val_is_int(value) || val_is_bigint(value);
        case TYPE_FLOAT:
            return val_is_float(value) || val_is_int(value) || val_is_bigint(value);
        case TYPE_BOOL:
            return val_is_bool(value);
        case TYPE_ARRAY:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_ARRAY);
        case TYPE_DICT:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_DICT);
        case TYPE_FILE:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_FILE);
        case TYPE_RGB:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_RGB);
        case TYPE_SOCKET:
            return (val_is_obj(value) && val_as_obj(value)->type == OBJ_SOCKET);
        case TYPE_PTR:
            return (val_is_obj(value) &&
                   (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                    val_as_obj(value)->type == OBJ_FFI_LIBRARY ||
                    val_as_obj(value)->type == OBJ_FFI_CALLBACK));
        default:
            return 1;
    }
}

// 辅助函数：从运行时值推断泛型类型名（返回线程本地缓冲区指针）
// 返回值如 "int"/"float"/"string"/"bool" 或 "struct FooName"
// 返回 NULL 表示无法推断
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

// 辅助函数：检查值是否匹配泛型类型名（用于运行时泛型类型检查）
// actual_type 格式：基础类型如 "int"/"float"/"string"/"bool"，
// 或带前缀的结构体类型如 "struct Foo"
static inline int value_matches_generic_type(Value value, const char* actual_type) {
    if (strcmp(actual_type, "int") == 0) {
        return val_is_int(value) || val_is_bigint(value);
    }
    if (strcmp(actual_type, "float") == 0) {
        return val_is_float(value) || val_is_int(value) || val_is_bigint(value);
    }
    if (strcmp(actual_type, "string") == 0) {
        return val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING;
    }
    if (strcmp(actual_type, "bool") == 0) {
        return val_is_bool(value);
    }
    if (strncmp(actual_type, "struct ", 7) == 0) {
        // 带 "struct " 前缀的结构体类型：检查是否为 struct，并验证名称匹配
        if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
            ObjStruct* arg_struct = (ObjStruct*)val_as_obj(value);
            return arg_struct->def->name && strcmp(arg_struct->def->name, actual_type + 7) == 0;
        }
        return 0;
    }
    // 无前缀但可能是结构体名称
    if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
        ObjStruct* arg_struct = (ObjStruct*)val_as_obj(value);
        return arg_struct->def->name && strcmp(arg_struct->def->name, actual_type) == 0;
    }
    return 0;
}

// 调用闭包函数
static inline int call(ObjClosure* closure, int arg_count, int line) {
    if (runtime_type_check && arg_count != closure->function->arity) {
        char msg[BUFFER_MEDIUM];
        snprintf(msg, sizeof(msg), "调用函数 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                closure->function->name, closure->function->arity, arg_count);
        throw_runtime_error(msg, line);
        return 0;
    }

    // 需要扩容
    if (vm.frame_cnt >= vm.frame_capacity) {
        if (!vm_grow_frames(current_exec_vm)) {
            runtime_error("调用栈内存分配失败", line);
            return 0;
        }
    }

    // 从栈中弹出 callee 和参数
    // 栈布局: [...][arg1][arg2][callee]
    
    int local_count = closure->function->local_count;
    Value* locals;
    int locals_is_dynamic = 0;
    
    // 优化：小函数使用内联数组，避免 malloc/free 开销
    // 大部分函数的局部变量数量不超过 INLINE_LOCALS_MAX
    CallFrame* frame = &vm.frames[vm.frame_cnt];  // 先获取帧指针
    if (local_count <= INLINE_LOCALS_MAX) {
        // 使用内联数组，无需内存分配
        locals = frame->inline_locals;
        locals_is_dynamic = 0;
    } else {
        // 大函数使用动态分配
        locals = (Value*)malloc(local_count * sizeof(Value));
        if (!locals) {
            runtime_error("局部变量内存分配失败", line);
            return 0;
        }
        locals_is_dynamic = 1;
    }
    
    // 初始化locals数组
    // slot 0..arg_count-1: 参数（与语义分析中的参数索引对应）
    for (int i = 0; i < arg_count; i++) {
        Value arg = vm.stack[vm.sp - arg_count - 1 + i];
        
        // 根据参数类型进行类型转换
        if (closure->function->param_types && i < closure->function->arity) {
            TypeKind expected_type = closure->function->param_types[i];

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

        // 泛型参数类型检查
#ifndef LENO_RELEASE
        if (runtime_type_check && closure->function->param_generic_names &&
            i < closure->function->arity && closure->function->param_generic_names[i]) {
            const char* generic_name = closure->function->param_generic_names[i];
            // self 是第一个参数（locals[0] 对应参数 0）
            Value self_val = vm.stack[vm.sp - arg_count - 1 + 0];
            if (val_is_obj(self_val) && val_as_obj(self_val)->type == OBJ_STRUCT) {
                ObjStruct* self_obj = (ObjStruct*)val_as_obj(self_val);
                ObjStructDef* self_def = self_obj->def;
                if (self_def->type_param_count > 0 && self_def->type_param_names &&
                    self_obj->generic_type_arg_count > 0 && self_obj->generic_type_args) {
                    for (int pi = 0; pi < self_def->type_param_count && pi < self_obj->generic_type_arg_count; pi++) {
                        if (self_def->type_param_names[pi] && strcmp(self_def->type_param_names[pi], generic_name) == 0) {
                            const char* actual_type = self_obj->generic_type_args[pi];
                            if (!value_matches_generic_type(arg, actual_type)) {
                                char msg[BUFFER_MEDIUM];
                                char* arg_str = value_to_string(arg);
                                snprintf(msg, sizeof(msg),
                                    "泛型参数类型不匹配: 方法 '%s' 参数 %d 期望类型 %s (泛型参数 %s), 实际传入 '%s'",
                                    closure->function->name ? closure->function->name : "unknown", i, actual_type, generic_name, arg_str);
                                free(arg_str);
                                throw_runtime_error(msg, line);
                                return 0;
                            }
                            break;
                        }
                    }
                }
            }
        }
#endif
        
        locals[i] = arg;
    }
    
    // slot arg_count..local_count-1: 局部变量，初始化为null
    for (int i = arg_count; i < local_count; i++) {
        locals[i] = val_null();
    }
    
    // 弹出 callee 和参数
    vm.sp -= (arg_count + 1);
    
    // 为返回值预留一个槽位，避免覆盖调用者的栈
    // 这样函数的栈空间从 stack_base + 1 开始，不会覆盖调用者的值
    vm.sp++;
    
    // 新的 stack_base = 当前的 vm.sp - 1（返回值槽位）
    int new_stack_base = vm.sp - 1;

    vm.frame_cnt++;  // 现在才增加帧计数
    frame->closure = closure;
    frame->chunk = closure->function->chunk;
    frame->ip = closure->function->chunk->code;
    frame->stack_base = new_stack_base;
    frame->slot_count = local_count;
    frame->locals = locals;
    frame->local_count = local_count;
    frame->locals_is_dynamic = locals_is_dynamic;  // 标记分配方式
    frame->catch_ip = NULL;
    frame->finally_ip = NULL;
    frame->prev_catch_ip = NULL;
    frame->prev_finally_ip = NULL;
    frame->in_finally = 0;
    frame->try_return_value = val_null();
    frame->has_try_return = 0;
    // 设置模块信息
    frame->module = closure->function->module;
    // 优化标志：检查是否捕获了 upvalue
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
#ifndef LENO_RELEASE
                // 调试版：参数数量和类型检查
                if (native->arity >= 0 && arg_count != native->arity) {
                    char msg[BUFFER_MEDIUM];
                    snprintf(msg, sizeof(msg), "调用函数 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                            native->name, native->arity, arg_count);
                    throw_runtime_error(msg, line);
                    return 0;
                }
                
                // 运行时参数类型检查
                if (runtime_type_check && native->arity >= 0 && arg_count == native->arity) {
                    for (int i = 0; i < arg_count; i++) {
                        TypeKind expected = native_get_global_function_param_type(native->name, i);
                        if (expected != TYPE_ANY) {
                            Value arg = vm.stack[vm.sp - arg_count - 1 + i];
                            if (!value_matches_type(arg, expected)) {
                                char msg[BUFFER_MEDIUM];
                                snprintf(msg, sizeof(msg), "调用函数 '%s' 时第 %d 个参数类型不匹配: 期望 '%s'",
                                        native->name, i + 1, type_kind_to_string(expected));
                                throw_runtime_error(msg, line);
                                return 0;
                            }
                        }
                    }
                }
#endif
                
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

#ifndef LENO_RELEASE
                // 调试版：运行时类型检查
                if (runtime_type_check) {
                    const char* type_name = NULL;
                    if (val_is_obj(bound->receiver)) {
                        type_name = obj_type_to_name(val_as_obj(bound->receiver)->type);
                    }

                    const InstanceMethodMeta* inst_meta = NULL;
                    if (type_name && native->name) {
                        inst_meta = native_find_instance_method(type_name, native->name);
                    }

                    // 检查参数数量（绑定方法的 arity 包含 receiver）
                    // native->arity >= 0: 固定参数数量
                    if (native->arity >= 0 && arg_count + 1 != native->arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数数量不匹配: 期望 %d, 实际 %d",
                                native->name, native->arity - 1, arg_count);
                        throw_runtime_error(msg, line);
                        return 0;
                    }
                    // native->arity < 0: 可变参数，检查是否少于 min_arity
                    else if (native->arity < 0 && inst_meta && inst_meta->min_arity >= 0 && arg_count < inst_meta->min_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过少: 最少 %d 个, 实际 %d",
                                native->name, inst_meta->min_arity, arg_count);
                        throw_runtime_error(msg, line);
                        return 0;
                    }
                    // native->arity < 0: 可变参数，检查是否超过 max_arity
                    else if (native->arity < 0 && inst_meta && inst_meta->max_arity >= 0 && arg_count > inst_meta->max_arity) {
                        char msg[BUFFER_MEDIUM];
                        snprintf(msg, sizeof(msg), "调用方法 '%s' 时参数过多: 最多 %d 个, 实际 %d",
                                native->name, inst_meta->max_arity, arg_count);
                        throw_runtime_error(msg, line);
                        return 0;
                    }

                    // 检查参数类型（如果知道 receiver 类型）
                    if (type_name && native->name) {
                        for (int i = 0; i < arg_count; i++) {
                            // 参数在栈上，从 vm.sp - arg_count - 1 开始（因为 vm.sp - 1 是 bound_method）
                            Value arg = vm.stack[vm.sp - arg_count - 1 + i];
                            TypeKind expected = native_get_instance_method_param_type(type_name, native->name, i);

                            if (expected != TYPE_ANY && !value_matches_type(arg, expected)) {
                                char msg[BUFFER_MEDIUM];
                                const char* expected_str = type_kind_to_string(expected);
                                snprintf(msg, sizeof(msg), "调用方法 '%s.%s' 时第 %d 个参数类型不匹配: 期望 %s",
                                        type_name, native->name, i + 1, expected_str);
                                throw_runtime_error(msg, line);
                                return 0;
                            }
                        }
                    }
                }
#endif
                
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
