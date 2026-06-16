// ============================================================================
// 属性获取类操作码 (OP_GET_PROPERTY)
// ============================================================================

OPCODE(OP_GET_PROPERTY) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取属性或实例方法：obj.name
    int name_idx = READ_SHORT();
    Value name_val = frame->chunk->constants[name_idx];

    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("属性名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }

    ObjString* name = (ObjString*)val_as_obj(name_val);
    Value receiver = vm_stack_pop(&vm);

    // 获取 receiver 类型
    ObjType receiver_type = get_receiver_type(receiver);

    // 检查是否支持内联缓存
    // 注意：在子线程中禁用内联缓存，因为多个线程可能执行相同的字节码，
    // 导致缓存索引冲突（不同线程使用相同的 ip 地址）
    #ifdef VM_USE_THREAD_LOCAL
        // 子线程：不使用内联缓存，直接查找方法
        if (ic_type_supported(receiver_type)) {
            ObjNative* method = find_method_by_type(receiver_type, name->chars);

            if (method) {
                // ===== 窥孔优化：检测下一条指令是否是 OP_CALL =====
                if (*frame->ip == OP_CALL) {
                    uint16_t arg_count = ((uint16_t)frame->ip[1] << 8) | frame->ip[2];
                    frame->ip += 3;

                    int arg_start = vm.sp - arg_count;
                    for (int i = arg_count - 1; i >= 0; i--) {
                        vm.stack[arg_start + i + 1] = vm.stack[arg_start + i];
                    }
                    vm.stack[arg_start] = receiver;
                    vm.sp++;

                    Value result = method->function(arg_count + 1, &vm.stack[arg_start]);

                    if (unlikely(vm.has_exception)) {
                        char* msg = NULL;
                        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                        } else {
                            msg = value_to_string(vm.exception);
                        }
                        error_clear();
                        throw_runtime_error(msg, GET_CURRENT_LINE());
                        free(msg);
                        frame = &vm.frames[vm.frame_cnt - 1];
                        if (frame->catch_ip || frame->finally_ip) {
                            DISPATCH;
                        }
                        return -1;
                    }

                    vm.sp -= arg_count + 1;
                    vm_stack_push(&vm, result);
                    DISPATCH;
                }

                // 创建绑定方法（非直接调用场景）
                vm_stack_push(&vm, receiver);
                ObjBoundMethod* bound = bound_method_new(receiver, method);
                vm_stack_pop(&vm);

                vm_stack_push(&vm, val_obj((Object*)bound));
                DISPATCH;
            }

            if (receiver_type == OBJ_DICT) {
                ObjDict* dict = (ObjDict*)val_as_obj(receiver);
                Value value = dict_get(dict, val_obj((Object*)name));
                vm_stack_push(&vm, value);
                DISPATCH;
            }

            if (receiver_type == OBJ_STRUCT) {
                ObjStruct* struct_obj = (ObjStruct*)val_as_obj(receiver);
                ObjStructDef* def = struct_obj->def;

                int field_idx = struct_get_field_index(def, name->chars);
                if (field_idx >= 0) {
                    Value value = struct_get_field(struct_obj, field_idx);
                    vm_stack_push(&vm, value);
                    DISPATCH;
                }
                // 字段不存在，继续执行到错误处理
            }

            if (receiver_type == OBJ_CSTRUCT) {
                ObjCStruct* cstruct_obj = (ObjCStruct*)val_as_obj(receiver);
                ObjCStructDef* def = cstruct_obj->def;

                int field_idx = cstruct_get_field_index(def, name->chars);
                if (field_idx >= 0) {
                    CStructFieldInfo* field = &def->fields[field_idx];

                    // 检查是否是数组字段
                    if (field->array_dim > 0) {
                        // 创建数组视图对象
                        ObjCStructArrayView* view = (ObjCStructArrayView*)gc_alloc(sizeof(ObjCStructArrayView), OBJ_CSTRUCT_ARRAY_VIEW);
                        if (!view) {
                            runtime_error("内存不足，无法创建数组视图", GET_CURRENT_LINE());
                            return -1;
                        }
                        view->cstruct = cstruct_obj;
                        view->field_index = field_idx;
                        view->element_type = field->type;
                        view->element_size = field->size;
                        view->array_dim = field->array_dim;
                        vm_stack_push(&vm, val_obj((Object*)view));
                        DISPATCH;
                    }
                    // 检查是否是嵌套 cstruct 字段
                    else if (field->type == TYPE_CSTRUCT) {
                        // 查找嵌套 cstruct 的定义
                        const char* nested_struct_name = field->struct_name ? field->struct_name : field->name;
                        ObjCStructDef* nested_def = cstruct_def_find(nested_struct_name);
                        if (!nested_def) {
                            runtime_error("找不到嵌套 cstruct 定义", GET_CURRENT_LINE());
                            return -1;
                        }

                        // 创建指向嵌套结构体内存的 cstruct 实例（不拥有内存）
                        ObjCStruct* nested_obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
                        if (!nested_obj) {
                            runtime_error("内存不足，无法创建嵌套 cstruct", GET_CURRENT_LINE());
                            return -1;
                        }
                        nested_obj->def = nested_def;
                        nested_obj->data = cstruct_obj->data + field->offset;
                        nested_obj->owns_memory = 0;
                        vm_stack_push(&vm, val_obj((Object*)nested_obj));
                        DISPATCH;
                    }
                    else {
                        // 普通字段，直接返回值
                        Value value = cstruct_get_field_value(cstruct_obj, field_idx);
                        vm_stack_push(&vm, value);
                        DISPATCH;
                    }
                }
                // 字段不存在，继续执行到错误处理
            }

            runtime_error("对象没有此方法", GET_CURRENT_LINE());
            return -1;
        }
    #else
        // 主线程：使用内联缓存优化
        if (ic_type_supported(receiver_type)) {
            // 计算缓存索引（使用当前指令地址）
            uint8_t* ip_before = frame->ip - 3;  // OP_GET_PROPERTY 是 3 字节指令
            int cache_idx = ic_cache_index(ip_before);
            InlineCacheEntry* cache = &vm.ic_cache[cache_idx];
            uint32_t name_hash = name->hash;  // ObjString 预计算的 FNV-1a 哈希值

            // 检查缓存是否命中（同时校验 receiver 类型 + 方法名哈希）
            if (cache->valid && cache->receiver_type == receiver_type && cache->name_hash == name_hash) {
                // 缓存命中！
                vm.ic_hits++;

                // ===== 窥孔优化：检测下一条指令是否是 OP_CALL =====
                // 如果是，直接执行方法调用，跳过 ObjBoundMethod 创建和 OP_CALL 执行
                if (*frame->ip == OP_CALL) {
                    uint16_t arg_count = ((uint16_t)frame->ip[1] << 8) | frame->ip[2];
                    frame->ip += 3;  // 跳过 OP_CALL 指令

                    // 栈布局: [arg1, ..., argN]（receiver 已被 vm_stack_pop 弹出）
                    // 目标布局: [receiver, arg1, ..., argN]
                    // 将 receiver 放回栈顶并移到参数开头
                    int arg_start = vm.sp - arg_count;
                    for (int i = arg_count - 1; i >= 0; i--) {
                        vm.stack[arg_start + i + 1] = vm.stack[arg_start + i];
                    }
                    vm.stack[arg_start] = receiver;
                    vm.sp++;

                    Value result = cache->method->function(arg_count + 1, &vm.stack[arg_start]);

                    // 检查 native 函数是否抛出异常
                    if (unlikely(vm.has_exception)) {
                        char* msg = NULL;
                        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                        } else {
                            msg = value_to_string(vm.exception);
                        }
                        error_clear();
                        throw_runtime_error(msg, GET_CURRENT_LINE());
                        free(msg);
                        frame = &vm.frames[vm.frame_cnt - 1];
                        if (frame->catch_ip || frame->finally_ip) {
                            DISPATCH;
                        }
                        return -1;
                    }

                    vm.sp -= arg_count + 1;
                    vm_stack_push(&vm, result);
                    DISPATCH;
                }

                // 非直接调用场景（如 var f = obj.method 捕获方法引用）
                // 回退到创建 ObjBoundMethod
                vm_stack_push(&vm, receiver);
                ObjBoundMethod* bound = bound_method_new(receiver, cache->method);
                vm_stack_pop(&vm);

                vm_stack_push(&vm, val_obj((Object*)bound));
                DISPATCH;
            }

            // 缓存未命中，进行查找
            vm.ic_misses++;
            ObjNative* method = find_method_by_type(receiver_type, name->chars);

            if (method) {
                // 更新缓存（存储 receiver 类型 + 方法名哈希 + 方法指针）
                cache->valid = 1;
                cache->receiver_type = receiver_type;
                cache->name_hash = name_hash;
                cache->method = method;

                // ===== 窥孔优化：检测下一条指令是否是 OP_CALL =====
                if (*frame->ip == OP_CALL) {
                    uint16_t arg_count = ((uint16_t)frame->ip[1] << 8) | frame->ip[2];
                    frame->ip += 3;

                    int arg_start = vm.sp - arg_count;
                    for (int i = arg_count - 1; i >= 0; i--) {
                        vm.stack[arg_start + i + 1] = vm.stack[arg_start + i];
                    }
                    vm.stack[arg_start] = receiver;
                    vm.sp++;

                    Value result = method->function(arg_count + 1, &vm.stack[arg_start]);

                    if (unlikely(vm.has_exception)) {
                        char* msg = NULL;
                        if (val_is_obj(vm.exception) && val_as_obj(vm.exception)->type == OBJ_STRING) {
                            msg = strdup(((ObjString*)val_as_obj(vm.exception))->chars);
                        } else {
                            msg = value_to_string(vm.exception);
                        }
                        error_clear();
                        throw_runtime_error(msg, GET_CURRENT_LINE());
                        free(msg);
                        frame = &vm.frames[vm.frame_cnt - 1];
                        if (frame->catch_ip || frame->finally_ip) {
                            DISPATCH;
                        }
                        return -1;
                    }

                    vm.sp -= arg_count + 1;
                    vm_stack_push(&vm, result);
                    DISPATCH;
                }

                // 创建绑定方法（非直接调用场景）
                vm_stack_push(&vm, receiver);
                ObjBoundMethod* bound = bound_method_new(receiver, method);
                vm_stack_pop(&vm);

                vm_stack_push(&vm, val_obj((Object*)bound));
                DISPATCH;
            }

            if (receiver_type == OBJ_DICT) {
                ObjDict* dict = (ObjDict*)val_as_obj(receiver);
                Value value = dict_get(dict, val_obj((Object*)name));
                vm_stack_push(&vm, value);
                DISPATCH;
            }

            if (receiver_type == OBJ_STRUCT) {
                ObjStruct* struct_obj = (ObjStruct*)val_as_obj(receiver);
                ObjStructDef* def = struct_obj->def;

                int field_idx = struct_get_field_index(def, name->chars);
                if (field_idx >= 0) {
                    Value value = struct_get_field(struct_obj, field_idx);
                    vm_stack_push(&vm, value);
                    DISPATCH;
                }
                // 字段不存在，继续执行到错误处理
            }

            if (receiver_type == OBJ_CSTRUCT) {
                ObjCStruct* cstruct_obj = (ObjCStruct*)val_as_obj(receiver);
                ObjCStructDef* def = cstruct_obj->def;

                int field_idx = cstruct_get_field_index(def, name->chars);
                if (field_idx >= 0) {
                    CStructFieldInfo* field = &def->fields[field_idx];

                    // 检查是否是数组字段
                    if (field->array_dim > 0) {
                        // 创建数组视图对象
                        ObjCStructArrayView* view = (ObjCStructArrayView*)gc_alloc(sizeof(ObjCStructArrayView), OBJ_CSTRUCT_ARRAY_VIEW);
                        if (!view) {
                            runtime_error("内存不足，无法创建数组视图", GET_CURRENT_LINE());
                            return -1;
                        }
                        view->cstruct = cstruct_obj;
                        view->field_index = field_idx;
                        view->element_type = field->type;
                        view->element_size = field->size;
                        view->array_dim = field->array_dim;
                        vm_stack_push(&vm, val_obj((Object*)view));
                        DISPATCH;
                    }
                    // 检查是否是嵌套 cstruct 字段
                    else if (field->type == TYPE_CSTRUCT) {
                        // 查找嵌套 cstruct 的定义
                        const char* nested_struct_name = field->struct_name ? field->struct_name : field->name;
                        ObjCStructDef* nested_def = cstruct_def_find(nested_struct_name);
                        if (!nested_def) {
                            runtime_error("找不到嵌套 cstruct 定义", GET_CURRENT_LINE());
                            return -1;
                        }

                        // 创建指向嵌套结构体内存的 cstruct 实例（不拥有内存）
                        ObjCStruct* nested_obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
                        if (!nested_obj) {
                            runtime_error("内存不足，无法创建嵌套 cstruct", GET_CURRENT_LINE());
                            return -1;
                        }
                        nested_obj->def = nested_def;
                        nested_obj->data = cstruct_obj->data + field->offset;
                        nested_obj->owns_memory = 0;
                        vm_stack_push(&vm, val_obj((Object*)nested_obj));
                        DISPATCH;
                    }
                    else {
                        // 普通字段，直接返回值
                        Value value = cstruct_get_field_value(cstruct_obj, field_idx);
                        vm_stack_push(&vm, value);
                        DISPATCH;
                    }
                }
                // 字段不存在，继续执行到错误处理
            }

            runtime_error("对象没有此方法", GET_CURRENT_LINE());
            return -1;
        }
    #endif

    // 不支持缓存的类型（如模块），使用原来的逻辑

    if (val_is_obj(receiver) && val_as_obj(receiver)->type == OBJ_MODULE) {
        ObjModule* module = (ObjModule*)val_as_obj(receiver);
        Value value = dict_get(module->exports, val_obj((Object*)name));
        vm_stack_push(&vm, value);
        DISPATCH;
    }

    // 处理 cstruct 定义对象的方法调用（如 SYSTEM_INFO.malloc()）
    if (val_is_obj(receiver) && val_as_obj(receiver)->type == OBJ_CSTRUCT_DEF) {
        ObjCStructDef* def = (ObjCStructDef*)val_as_obj(receiver);
        // 查找 cstruct 方法（如 malloc, free, size 等）
        ObjNative* native_method = cstruct_find_method(name->chars);
        if (native_method) {
            // 创建绑定方法，将 cstruct 定义作为接收者
            ObjBoundMethod* bound = bound_method_new(receiver, native_method);
            vm_stack_push(&vm, val_obj((Object*)bound));
            DISPATCH;
        }
        // 方法未找到，报错
        char msg[256];
        snprintf(msg, sizeof(msg), "cstruct '%s' 没有方法 '%s'", def->name, name->chars);
        throw_runtime_error(msg, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    throw_runtime_error("只能获取数组、字典或模块的属性", GET_CURRENT_LINE());
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}
