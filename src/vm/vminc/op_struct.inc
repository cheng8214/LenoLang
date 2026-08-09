// ============================================================================
// struct 相关操作码
// ============================================================================

OPCODE(OP_STRUCT_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 定义结构体
    // 操作数：2字节名称常量索引 + 1字节字段数量 + 1字节方法数量
    // 每个字段：2字节字段名常量索引 + 1字节字段类型
    //          如果字段类型是 TYPE_STRUCT：1字节是否有 struct 类型名 + [2字节类型名常量索引]
    //          1字节是否有默认值 + [2字节默认值常量索引]
    uint16_t name_idx = READ_SHORT();
    uint8_t field_count = READ_BYTE();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("结构体名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    // 读取方法数量
    uint8_t method_count = READ_BYTE();

    // 创建结构体定义
    ObjStructDef* def = struct_def_new(name->chars, field_count, method_count);

    // 读取 impl 声明的 face 名称
    uint8_t impl_count = READ_BYTE();
    if (impl_count > 0) {
        def->impl_names = (char**)calloc(impl_count, sizeof(char*));
        def->impl_count = impl_count;
        for (int i = 0; i < impl_count; i++) {
            uint16_t impl_name_idx = READ_SHORT();
            Value impl_name_val = frame->chunk->constants[impl_name_idx];
            if (val_is_obj(impl_name_val) && val_as_obj(impl_name_val)->type == OBJ_STRING) {
                def->impl_names[i] = strdup(((ObjString*)val_as_obj(impl_name_val))->chars);
            }
        }
    }

    // 读取泛型类型参数信息
    uint8_t type_param_count = READ_BYTE();
    def->type_param_count = type_param_count;
    if (type_param_count > 0) {
        def->type_param_names = (char**)malloc(sizeof(char*) * type_param_count);
        def->type_param_constraints = (char**)calloc(type_param_count, sizeof(char*));
        for (int i = 0; i < type_param_count; i++) {
            uint16_t param_name_idx = READ_SHORT();
            Value param_name_val = frame->chunk->constants[param_name_idx];
            if (val_is_obj(param_name_val) && val_as_obj(param_name_val)->type == OBJ_STRING) {
                def->type_param_names[i] = strdup(((ObjString*)val_as_obj(param_name_val))->chars);
            } else {
                def->type_param_names[i] = strdup("T");
            }
        }
    }

    // 读取字段信息
    for (int i = 0; i < field_count; i++) {
        uint16_t field_name_idx = READ_SHORT();
        uint8_t field_type = READ_BYTE();
        uint8_t is_nullable = READ_BYTE();  // nullable 标记

        // 读取 struct 类型名（如果字段是 struct 类型）
        char* struct_type_name = NULL;
        if (field_type == TYPE_STRUCT) {
            uint8_t has_struct_type_name = READ_BYTE();
            if (has_struct_type_name) {
                uint16_t type_name_idx = READ_SHORT();
                Value type_name_val = frame->chunk->constants[type_name_idx];
                if (val_is_obj(type_name_val) && val_as_obj(type_name_val)->type == OBJ_STRING) {
                    struct_type_name = ((ObjString*)val_as_obj(type_name_val))->chars;
                }
            }
        }

        // 读取 Ptr[T] 元素类型（如果字段是 TYPE_PTR_GENERIC 类型）
        TypeKind element_type = TYPE_PTR;
        if (field_type == TYPE_PTR_GENERIC) {
            element_type = (TypeKind)READ_BYTE();
        }

        uint8_t has_default = READ_BYTE();

        Value field_name_val = frame->chunk->constants[field_name_idx];
        if (!val_is_obj(field_name_val) || val_as_obj(field_name_val)->type != OBJ_STRING) {
            runtime_error("字段名必须是字符串", GET_CURRENT_LINE());
            return -1;
        }
        ObjString* field_name = (ObjString*)val_as_obj(field_name_val);

        Value default_value = val_null();
        if (has_default) {
            // 从常量池读取默认值
            uint16_t default_idx = READ_SHORT();
            default_value = frame->chunk->constants[default_idx];
        }

        struct_def_set_field(def, i, field_name->chars, (TypeKind)field_type,
                            struct_type_name, default_value, has_default, element_type, is_nullable);
    }
    
    // 读取方法信息
    for (int i = 0; i < method_count; i++) {
        uint16_t method_name_idx = READ_SHORT();
        Value method_name_val = frame->chunk->constants[method_name_idx];
        if (!val_is_obj(method_name_val) || val_as_obj(method_name_val)->type != OBJ_STRING) {
            runtime_error("方法名必须是字符串", GET_CURRENT_LINE());
            return -1;
        }
        ObjString* method_name = (ObjString*)val_as_obj(method_name_val);
        def->methods[i].name = strdup(method_name->chars);
        
        uint16_t func_idx = READ_SHORT();
        Value func_val = frame->chunk->constants[func_idx];
        if (!val_is_obj(func_val) || val_as_obj(func_val)->type != OBJ_FUNCTION) {
            runtime_error("方法必须是函数对象", GET_CURRENT_LINE());
            return -1;
        }
        def->methods[i].func = (ObjFunction*)val_as_obj(func_val);

        // 预创建闭包（优化：避免每次调用都创建）
        // 注意：只有不需要 upvalue 的方法才能预创建闭包
        // 需要 upvalue 的方法将在 OP_GET_METHOD 中动态创建
        ObjFunction* func = def->methods[i].func;
        if (func->upvalue_count == 0) {
            ObjClosure* closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
            if (!closure) {
                runtime_error("内存不足，无法创建方法闭包", GET_CURRENT_LINE());
                return -1;
            }
            closure->function = func;
            closure->upvalue_count = 0;
            def->methods[i].closure = closure;
        } else {
            // 需要 upvalue 的方法，标记为 NULL，在运行时动态创建
            def->methods[i].closure = NULL;
        }
    }

    // 读取构造/析构函数标志和索引
    uint8_t ctor_dtor_flags = READ_BYTE();
    def->has_ctor = (ctor_dtor_flags & 1) != 0;
    def->has_dtor = (ctor_dtor_flags & 2) != 0;
    if (def->has_ctor) {
        def->ctor_index = READ_BYTE();
    }
    if (def->has_dtor) {
        def->dtor_index = READ_BYTE();
    }

    // 读取关联常量信息
    uint8_t const_count = READ_BYTE();
    def->const_count = const_count;
    if (const_count > 0) {
        def->const_names = (char**)malloc(sizeof(char*) * const_count);
        def->const_values = (Value*)calloc(const_count, sizeof(Value));
        for (int i = 0; i < const_count; i++) {
            uint16_t cname_idx = READ_SHORT();
            Value cname_val = frame->chunk->constants[cname_idx];
            if (val_is_obj(cname_val) && val_as_obj(cname_val)->type == OBJ_STRING) {
                def->const_names[i] = strdup(((ObjString*)val_as_obj(cname_val))->chars);
            } else {
                def->const_names[i] = strdup("");
            }
            uint16_t cval_idx = READ_SHORT();
            def->const_values[i] = frame->chunk->constants[cval_idx];
        }
    }
    
    // 注册结构体定义
    struct_def_register(def);
    
    DISPATCH;
}

OPCODE(OP_FACE_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_idx = READ_SHORT();
    uint8_t method_count = READ_BYTE();
    uint8_t type_param_count = READ_BYTE();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("接口名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    ObjFaceDef* def = face_def_new(name->chars, method_count);

    // 读取泛型类型参数
    if (type_param_count > 0) {
        char** tp_names = (char**)malloc(sizeof(char*) * type_param_count);
        for (int tp = 0; tp < type_param_count; tp++) {
            uint16_t tp_name_idx = READ_SHORT();
            Value tp_name_val = frame->chunk->constants[tp_name_idx];
            if (val_is_obj(tp_name_val) && val_as_obj(tp_name_val)->type == OBJ_STRING) {
                tp_names[tp] = strdup(((ObjString*)val_as_obj(tp_name_val))->chars);
            } else {
                tp_names[tp] = strdup("T");
            }
        }
        face_def_set_type_params(def, type_param_count, tp_names, NULL);
        for (int tp = 0; tp < type_param_count; tp++) free(tp_names[tp]);
        free(tp_names);
    }

    for (int i = 0; i < method_count; i++) {
        uint16_t mname_idx = READ_SHORT();
        uint8_t param_count = READ_BYTE();
        uint8_t return_type_kind = READ_BYTE();

        Value mname_val = frame->chunk->constants[mname_idx];
        if (!val_is_obj(mname_val) || val_as_obj(mname_val)->type != OBJ_STRING) {
            runtime_error("接口方法名必须是字符串", GET_CURRENT_LINE());
            return -1;
        }
        def->methods[i].name = strdup(((ObjString*)val_as_obj(mname_val))->chars);
        def->methods[i].param_count = param_count;
        if (return_type_kind != TYPE_INFER) {
            def->methods[i].return_type = type_new((TypeKind)return_type_kind);
        } else {
            def->methods[i].return_type = NULL;
        }
        def->methods[i].param_types = NULL;
        if (param_count > 0) {
            def->methods[i].param_types = (TypeInfo**)malloc(sizeof(TypeInfo*) * param_count);
            for (int j = 0; j < param_count; j++) {
                uint8_t ptk = READ_BYTE();
                if (ptk != TYPE_INFER) {
                    def->methods[i].param_types[j] = type_new((TypeKind)ptk);
                } else {
                    def->methods[i].param_types[j] = NULL;
                }
            }
        }
    }

    face_def_register(def);

    DISPATCH;
}

OPCODE(OP_STRUCT_INIT) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 创建结构体实例
    // 操作数：2字节结构体名称常量索引 + 1字节参数字段数量 + 1字节泛型参数数量 + 泛型参数常量索引
    // 每个参数：1字节字段索引（编译期确定，避免运行时线性搜索）
    uint16_t name_idx = READ_SHORT();
    uint8_t arg_count = READ_BYTE();

    // 读取泛型参数信息
    uint8_t generic_type_count = READ_BYTE();
    char** generic_type_args = NULL;
    if (generic_type_count > 0) {
        generic_type_args = (char**)malloc(sizeof(char*) * generic_type_count);
        for (int gi = 0; gi < generic_type_count; gi++) {
            uint16_t type_name_idx = READ_SHORT();
            Value type_name_val = frame->chunk->constants[type_name_idx];
            if (val_is_obj(type_name_val) && val_as_obj(type_name_val)->type == OBJ_STRING) {
                ObjString* type_name = (ObjString*)val_as_obj(type_name_val);
                generic_type_args[gi] = strdup(type_name->chars);
            } else {
                generic_type_args[gi] = strdup("unknown");
            }
        }
    }

    // 泛型参数替换：如果泛型参数名是类型参数（如 K, V, T），
    // 1) 从调用栈中查找 self 实例的实际泛型参数进行替换
    // 2) 若仍未解析，从闭包函数参数运行时值推断具体类型
    if (generic_type_count > 0 && generic_type_args) {
        int resolved = 0;
        // 遍历调用栈帧，查找 self（struct 实例）
        for (int fi = vm.frame_cnt - 1; fi >= 1 && generic_type_count > 0; fi--) {
            CallFrame* check_frame = &vm.frames[fi];
            if (check_frame->locals && check_frame->local_count > 0) {
                Value self_val = check_frame->locals[0];
                if (val_is_obj(self_val) && val_as_obj(self_val)->type == OBJ_STRUCT) {
                    ObjStruct* self_obj = (ObjStruct*)val_as_obj(self_val);
                    ObjStructDef* self_def = self_obj->def;
                    if (self_def->type_param_count > 0 && self_def->type_param_names &&
                        self_obj->generic_type_arg_count > 0 && self_obj->generic_type_args) {
                        for (int gi = 0; gi < generic_type_count; gi++) {
                            const char* arg_name = generic_type_args[gi];
                            const char* pure_name = arg_name;
                            if (strncmp(arg_name, "struct ", 7) == 0) {
                                pure_name = arg_name + 7;
                            }
                            for (int pi = 0; pi < self_def->type_param_count && pi < self_obj->generic_type_arg_count; pi++) {
                                if (self_def->type_param_names[pi] && pure_name &&
                                    strcmp(self_def->type_param_names[pi], pure_name) == 0) {
                                    free(generic_type_args[gi]);
                                    generic_type_args[gi] = strdup(self_obj->generic_type_args[pi]);
                                    resolved = 1;
                                    break;
                                }
                            }
                        }
                    }
                    if (resolved) break;
                }
            }
        }
        
        // 2) struct self 未解析时，依次尝试：
        //    a. closure type_param_args（泛型函数调用时传入的精准类型参数）
        //    b. 从闭包函数参数运行时值推断
        if (!resolved) {
            for (int fi = vm.frame_cnt - 1; fi >= 1; fi--) {
                CallFrame* check_frame = &vm.frames[fi];
                if (!check_frame->closure || !check_frame->closure->function) continue;
                ObjFunction* func = check_frame->closure->function;
                ObjClosure* clo = check_frame->closure;
                
                // a: 优先使用 closure type_param_args（精准，来自调用方）
                if (clo->type_param_count > 0 && clo->type_param_args &&
                    func->type_param_count > 0 && func->type_param_names) {
                    int any_resolved = 0;
                    for (int gi = 0; gi < generic_type_count; gi++) {
                        const char* arg_name = generic_type_args[gi];
                        const char* pure_name = arg_name;
                        if (strncmp(arg_name, "struct ", 7) == 0) pure_name = arg_name + 7;
                        for (int pi = 0; pi < func->type_param_count && pi < clo->type_param_count; pi++) {
                            if (func->type_param_names[pi] && pure_name &&
                                strcmp(func->type_param_names[pi], pure_name) == 0) {
                                free(generic_type_args[gi]);
                                generic_type_args[gi] = strdup(clo->type_param_args[pi]);
                                any_resolved = 1;
                                break;
                            }
                        }
                    }
                    if (any_resolved) { resolved = 1; break; }
                }
                
                // b: 回退到运行时值推断
                if (func->type_param_count > 0 && func->type_param_names &&
                    check_frame->locals && check_frame->local_count > 0) {
                    int any_resolved = 0;
                    for (int gi = 0; gi < generic_type_count; gi++) {
                        const char* arg_name = generic_type_args[gi];
                        const char* pure_name = arg_name;
                        if (strncmp(arg_name, "struct ", 7) == 0) {
                            pure_name = arg_name + 7;
                        }
                        // 检查是否匹配函数级类型参数名
                        int matched_param_idx = -1;
                        for (int pi = 0; pi < func->type_param_count; pi++) {
                            if (func->type_param_names[pi] && pure_name &&
                                strcmp(func->type_param_names[pi], pure_name) == 0) {
                                matched_param_idx = pi;
                                break;
                            }
                        }
                        if (matched_param_idx < 0) continue;
                        // 从函数参数推断：查找哪个参数使用了这个类型参数
                        for (int ai = 0; ai < func->arity && ai < check_frame->local_count; ai++) {
                            Value arg_val = check_frame->locals[ai];
                            if (val_is_null(arg_val)) continue;
                            // 参数直接是泛型类型（如 Ok[T](T val) 中的 val）
                            if (func->param_generic_names && func->param_generic_names[ai] &&
                                pure_name && strcmp(func->param_generic_names[ai], pure_name) == 0) {
                                const char* inferred = value_to_generic_type_name(arg_val);
                                if (inferred) {
                                    free(generic_type_args[gi]);
                                    generic_type_args[gi] = strdup(inferred);
                                    any_resolved = 1;
                                    break;
                                }
                            }
                            // 参数是容器类型（如 reverse[T](Array[T] arr)），从 type_info 推断
                            if (val_is_obj(arg_val)) {
                                ObjType ot = val_as_obj(arg_val)->type;
                                if (ot == OBJ_ARRAY) {
                                    ObjArray* arr = (ObjArray*)val_as_obj(arg_val);
                                    if (arr->type_info && arr->type_info->element_type) {
                                        const char* inferred = typeinfo_to_generic_type_name(arr->type_info->element_type);
                                        if (inferred) {
                                            free(generic_type_args[gi]);
                                            generic_type_args[gi] = strdup(inferred);
                                            any_resolved = 1;
                                            break;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    if (any_resolved) resolved = 1;
                    if (resolved) break;
                }
            }
        }
    }

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("结构体名称必须是字符串", GET_CURRENT_LINE());
        if (generic_type_args) {
            for (int gi = 0; gi < generic_type_count; gi++) free(generic_type_args[gi]);
            free(generic_type_args);
        }
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    ObjStructDef* def = struct_def_find(name->chars);
    if (!def) {
        char _undef_msg[256];
        snprintf(_undef_msg, sizeof(_undef_msg), "未定义的结构体 '%s'", name->chars);
        runtime_error(_undef_msg, GET_CURRENT_LINE());
        if (generic_type_args) {
            for (int gi = 0; gi < generic_type_count; gi++) free(generic_type_args[gi]);
            free(generic_type_args);
        }
        return -1;
    }

    // 创建实例
    ObjStruct* obj = struct_instance_new(def);

    // 设置泛型参数信息
    obj->generic_type_args = generic_type_args;
    obj->generic_type_arg_count = generic_type_count;
    
    // 设置参数字段
    for (int i = 0; i < arg_count; i++) {
        // 从字节码读取字段索引（1字节）
        uint8_t field_idx = READ_BYTE();
        
        // 从栈中弹出值（注意：栈中是反序的）
        Value field_value = vm_stack_pop(&vm);
        
        // 边界检查
        if (field_idx >= def->field_count) {
            char _idx_msg[256];
            snprintf(_idx_msg, sizeof(_idx_msg), "struct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", field_idx, def->field_count);
            runtime_error(_idx_msg, GET_CURRENT_LINE());
            return -1;
        }
        
        // 检查类型并进行自动类型提升
        TypeKind expected_type = def->fields[field_idx].type;

        // int -> float 自动提升
        if (expected_type == TYPE_FLOAT && val_is_int(field_value)) {
            field_value = val_float((double)val_as_int(field_value));
        }
        // bigint -> float 自动提升
        else if (expected_type == TYPE_FLOAT && val_is_bigint(field_value)) {
            field_value = val_float(bigint_to_double(val_as_bigint(field_value)));
        }

        struct_set_field(obj, field_idx, field_value);
    }
    
    // 将实例压入栈
    vm_stack_push(&vm, val_obj((Object*)obj));

    // 如果有构造函数，自动调用
    if (def->has_ctor && def->ctor_index >= 0 && def->ctor_index < def->method_count) {
        StructMethodInfo* mi = &def->methods[def->ctor_index];
        ObjClosure* ctor_closure = mi->closure;
        if (!ctor_closure && mi->func) {
            // 动态创建闭包（需要 upvalue 的情况）
            ObjFunction* ctor_func = mi->func;
            ctor_closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
            if (!ctor_closure) {
                runtime_error("内存不足，无法创建构造函数闭包", GET_CURRENT_LINE());
                return -1;
            }
            ctor_closure->function = ctor_func;
            ctor_closure->upvalue_count = ctor_func->upvalue_count;
            for (int u = 0; u < ctor_func->upvalue_count; u++) {
                ctor_closure->upvalues[u] = NULL;
            }
            mi->closure = ctor_closure;  // 缓存
        }
        if (ctor_closure) {
            // 栈布局: [instance(self), closure(callee)]
            // call() 读取: arg[0]=instance(self), callee=closure
            vm_stack_push(&vm, val_obj((Object*)ctor_closure));
            call(ctor_closure, 1, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;  // 进入构造函数执行
        }
    }
    
    DISPATCH;
}

OPCODE(OP_DTOR_LOCAL) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 对局部变量调用析构函数
    // 操作数：2字节 local_slot
    uint16_t slot = READ_SHORT();

    if (slot >= (uint16_t)frame->local_count) {
        vm_stack_push(&vm, val_null());
        DISPATCH;
    }

    Value val = frame->locals[slot];

    // 检查是否是 struct 实例且有析构函数
    if (val_is_obj(val) && val_as_obj(val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(val);
        ObjStructDef* def = obj->def;

        if (def->has_dtor && def->dtor_index >= 0 && def->dtor_index < def->method_count) {
            StructMethodInfo* mi = &def->methods[def->dtor_index];
            ObjClosure* dtor_closure = mi->closure;
            if (!dtor_closure && mi->func) {
                ObjFunction* dtor_func = mi->func;
                dtor_closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                if (!dtor_closure) {
                    runtime_error("内存不足，无法创建析构函数闭包", GET_CURRENT_LINE());
                    return -1;
                }
                dtor_closure->function = dtor_func;
                dtor_closure->upvalue_count = dtor_func->upvalue_count;
                for (int u = 0; u < dtor_func->upvalue_count; u++) {
                    dtor_closure->upvalues[u] = NULL;
                }
                mi->closure = dtor_closure;  // 缓存
            }
            if (dtor_closure) {
                // 栈布局: [instance(self), closure(callee)]
                vm_stack_push(&vm, val);                              // self
                vm_stack_push(&vm, val_obj((Object*)dtor_closure));   // callee
                call(dtor_closure, 1, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;  // 进入析构函数执行
            }
        }
    }

    // 不是 struct 或没有析构函数 → 压入 null 作为"返回值"
    vm_stack_push(&vm, val_null());
    DISPATCH;
}

OPCODE(OP_GET_FIELD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取结构体/cstruct字段
    // 操作数：1字节字段索引（编译期确定，避免运行时线性搜索）
    uint8_t field_idx = READ_BYTE();
    
    // 从栈中弹出对象
    Value obj_val = vm_stack_pop(&vm);
    
    if (!val_is_obj(obj_val)) {
        char _type_buf[64];
        if (val_is_null(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "null");
        else if (val_is_int(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "int(%lld)", (long long)val_as_int(obj_val));
        else if (val_is_float(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "float(%g)", val_as_double(obj_val));
        else if (val_is_bool(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "bool");
        else snprintf(_type_buf, sizeof(_type_buf), "unknown");
        char _gf_msg[256];
        snprintf(_gf_msg, sizeof(_gf_msg), "尝试在非对象类型上获取字段（实际类型: %s）", _type_buf);
        runtime_error(_gf_msg, GET_CURRENT_LINE());
        return -1;
    }
    
    // 处理普通 struct
    if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
        ObjStructDef* def = obj->def;
        
        // 边界检查
        if (field_idx >= def->field_count) {
            char _idx_msg[256];
            snprintf(_idx_msg, sizeof(_idx_msg), "struct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", field_idx, def->field_count);
            runtime_error(_idx_msg, GET_CURRENT_LINE());
            return -1;
        }
        
        Value field_value = struct_get_field(obj, field_idx);
        vm_stack_push(&vm, field_value);
    }
    else if (val_as_obj(obj_val)->type == OBJ_CSTRUCT) {
        ObjCStruct* obj = (ObjCStruct*)val_as_obj(obj_val);
        ObjCStructDef* def = obj->def;
        
        if (field_idx >= def->field_count) {
            char _idx_msg[256];
            snprintf(_idx_msg, sizeof(_idx_msg), "cstruct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", field_idx, def->field_count);
            runtime_error(_idx_msg, GET_CURRENT_LINE());
            return -1;
        }
        
        CStructFieldInfo* field = &def->fields[field_idx];
        
        // 检查是否是 str16 类型（需要特殊处理，自动转换 UTF-16 到 UTF-8）
        if (field->type == TYPE_STR16 && field->array_dim > 0) {
            // str16 类型：调用 getter 进行 UTF-16 到 UTF-8 转换
            Value field_value = cstruct_get_field_value(obj, field_idx);
            vm_stack_push(&vm, field_value);
        }
        // 检查是否是数组字段
        else if (field->array_dim > 0) {
            // 创建数组视图对象
            ObjCStructArrayView* view = (ObjCStructArrayView*)gc_alloc(sizeof(ObjCStructArrayView), OBJ_CSTRUCT_ARRAY_VIEW);
            if (!view) {
                runtime_error("内存不足，无法创建数组视图", GET_CURRENT_LINE());
                return -1;
            }
            view->cstruct = obj;
            view->field_index = field_idx;
            view->element_type = field->type;
            view->element_size = field->size;
            view->array_dim = field->array_dim;
            vm_stack_push(&vm, val_obj((Object*)view));
        }
        // 检查是否是嵌套 cstruct 字段
        else if (field->type == TYPE_CSTRUCT) {
            // 查找嵌套 cstruct 的定义
            const char* nested_struct_name = field->struct_name ? field->struct_name : field->name;
            ObjCStructDef* nested_def = cstruct_def_find(nested_struct_name);
            if (!nested_def) {
                char _nest_msg[256];
                snprintf(_nest_msg, sizeof(_nest_msg), "找不到嵌套 cstruct 定义 '%s'",
                         nested_struct_name ? nested_struct_name : "?");
                runtime_error(_nest_msg, GET_CURRENT_LINE());
                return -1;
            }
            
            // 创建指向嵌套结构体内存的 cstruct 实例（不拥有内存）
            ObjCStruct* nested_obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
            if (!nested_obj) {
                runtime_error("内存不足，无法创建嵌套 cstruct", GET_CURRENT_LINE());
                return -1;
            }
            nested_obj->def = nested_def;
            nested_obj->data = obj->data + field->offset;
            nested_obj->owns_memory = 0;  // 不拥有内存，由父结构体管理
            vm_stack_push(&vm, val_obj((Object*)nested_obj));
        }
        else {
            Value field_value = cstruct_get_field_value(obj, field_idx);
            vm_stack_push(&vm, field_value);
        }
    }
    else {
        Object* actual_obj = val_as_obj(obj_val);
        ObjType actual_type = actual_obj->type;
        char dbg[256];
        snprintf(dbg, sizeof(dbg), "尝试在非 struct/cstruct 类型上获取字段 (类型=%d, line=%d, val_bits=0x%llx)",
            actual_type, GET_CURRENT_LINE(), (unsigned long long)obj_val);
        runtime_error(dbg, GET_CURRENT_LINE());
        return -1;
    }
    
    DISPATCH;
}

OPCODE(OP_SET_FIELD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 设置结构体/cstruct字段
    // 操作数：1字节字段索引（编译期确定，避免运行时线性搜索）
    uint8_t field_idx = READ_BYTE();
    
    // 获取值和对象
    Value value = vm_stack_pop(&vm);
    Value obj_val = vm_stack_pop(&vm);
    
    if (!val_is_obj(obj_val)) {
        char _type_buf[64];
        if (val_is_null(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "null");
        else if (val_is_int(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "int(%lld)", (long long)val_as_int(obj_val));
        else if (val_is_float(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "float(%g)", val_as_double(obj_val));
        else if (val_is_bool(obj_val)) snprintf(_type_buf, sizeof(_type_buf), "bool");
        else snprintf(_type_buf, sizeof(_type_buf), "unknown");
        char _sf_msg[256];
        snprintf(_sf_msg, sizeof(_sf_msg), "尝试在非对象类型上设置字段（实际类型: %s）", _type_buf);
        runtime_error(_sf_msg, GET_CURRENT_LINE());
        return -1;
    }
    
    if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
        ObjStructDef* def = obj->def;
        
        if (field_idx >= def->field_count) {
            char _idx_msg[256];
            snprintf(_idx_msg, sizeof(_idx_msg), "struct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", field_idx, def->field_count);
            runtime_error(_idx_msg, GET_CURRENT_LINE());
            return -1;
        }

        // 检查类型并进行自动类型提升
        TypeKind expected_type = def->fields[field_idx].type;

        // int -> float 自动提升
        if (expected_type == TYPE_FLOAT && val_is_int(value)) {
            value = val_float((double)val_as_int(value));
        }
        // bigint -> float 自动提升
        else if (expected_type == TYPE_FLOAT && val_is_bigint(value)) {
            value = val_float(bigint_to_double(val_as_bigint(value)));
        }

        struct_set_field(obj, field_idx, value);
        
        // 将值重新压入栈（赋值表达式的值）
        vm_stack_push(&vm, value);
    }
    else if (val_as_obj(obj_val)->type == OBJ_CSTRUCT) {
        ObjCStruct* obj = (ObjCStruct*)val_as_obj(obj_val);
        ObjCStructDef* def = obj->def;
        
        if (field_idx >= def->field_count) {
            char _idx_msg[256];
            snprintf(_idx_msg, sizeof(_idx_msg), "cstruct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                     def->name ? def->name : "?", field_idx, def->field_count);
            runtime_error(_idx_msg, GET_CURRENT_LINE());
            return -1;
        }

        // cstruct 字段设置（数值类型自动转换）
        CStructFieldInfo* field = &def->fields[field_idx];
        
        // 数值类型自动转换
        if (val_is_int(value) && (field->type == TYPE_F32 || field->type == TYPE_F64)) {
            value = val_float((double)val_as_int(value));
        }
        else if (val_is_float(value) && (field->type >= TYPE_I8 && field->type <= TYPE_U64)) {
            value = val_num(val_as_num(value));
        }
        
        cstruct_set_field_value(obj, field_idx, value);
        
        vm_stack_push(&vm, value);
    }
    else {
        Object* _actual = val_as_obj(obj_val);
        const char* _tn = "unknown";
        switch (_actual->type) {
            case OBJ_STRING: _tn = "string"; break;
            case OBJ_ARRAY:  _tn = "array"; break;
            case OBJ_DICT:   _tn = "dict"; break;
            case OBJ_CLOSURE: _tn = "closure"; break;
            case OBJ_MODULE: _tn = "module"; break;
            case OBJ_FILE:   _tn = "File"; break;
            case OBJ_SOCKET: _tn = "Socket"; break;
            default: break;
        }
        char _sf2_msg[256];
        snprintf(_sf2_msg, sizeof(_sf2_msg), "尝试在非 struct/cstruct 类型上设置字段（实际类型: %s）", _tn);
        runtime_error(_sf2_msg, GET_CURRENT_LINE());
        return -1;
    }
    
    DISPATCH;
}

OPCODE(OP_GET_FIELD_ADDR) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取 cstruct 字段地址（返回 ObjFFIPointer）
    // 操作数：1字节字段索引
    uint8_t field_idx = READ_BYTE();
    
    Value obj_val = vm_stack_pop(&vm);
    
    if (!val_is_obj(obj_val) || val_as_obj(obj_val)->type != OBJ_CSTRUCT) {
        char _addr_type_buf[64];
        if (val_is_null(obj_val)) snprintf(_addr_type_buf, sizeof(_addr_type_buf), "null");
        else if (val_is_int(obj_val)) snprintf(_addr_type_buf, sizeof(_addr_type_buf), "int");
        else if (val_is_float(obj_val)) snprintf(_addr_type_buf, sizeof(_addr_type_buf), "float");
        else if (val_is_obj(obj_val)) {
            switch (val_as_obj(obj_val)->type) {
                case OBJ_STRUCT: snprintf(_addr_type_buf, sizeof(_addr_type_buf), "struct"); break;
                case OBJ_STRING: snprintf(_addr_type_buf, sizeof(_addr_type_buf), "string"); break;
                case OBJ_ARRAY:  snprintf(_addr_type_buf, sizeof(_addr_type_buf), "array"); break;
                case OBJ_DICT:   snprintf(_addr_type_buf, sizeof(_addr_type_buf), "dict"); break;
                default:         snprintf(_addr_type_buf, sizeof(_addr_type_buf), "other"); break;
            }
        } else {
            snprintf(_addr_type_buf, sizeof(_addr_type_buf), "unknown");
        }
        char _addr_msg[256];
        snprintf(_addr_msg, sizeof(_addr_msg), "& 取地址仅支持 cstruct 字段（实际类型: %s）", _addr_type_buf);
        runtime_error(_addr_msg, GET_CURRENT_LINE());
        return -1;
    }
    
    ObjCStruct* obj = (ObjCStruct*)val_as_obj(obj_val);
    ObjCStructDef* def = obj->def;
    
    if (field_idx >= def->field_count) {
        char _idx_msg[256];
        snprintf(_idx_msg, sizeof(_idx_msg), "cstruct '%s' 字段索引越界（索引 %d，共 %d 个字段）",
                 def->name ? def->name : "?", field_idx, def->field_count);
        runtime_error(_idx_msg, GET_CURRENT_LINE());
        return -1;
    }
    
    CStructFieldInfo* field = &def->fields[field_idx];
    void* addr = obj->data + field->offset;
    
    // 创建 ObjFFIPointer（非拥有，指向 cstruct 字段内存）
    ObjFFIPointer* ffi_ptr = (ObjFFIPointer*)gc_alloc(sizeof(ObjFFIPointer), OBJ_FFI_POINTER);
    if (!ffi_ptr) {
        runtime_error("内存不足", GET_CURRENT_LINE());
        return -1;
    }
    ffi_ptr->ptr = addr;
    ffi_ptr->size = (size_t)field->size;
    ffi_ptr->owned = 0;   // 不拥有内存（cstruct 拥有）
    ffi_ptr->freed = 0;
    ffi_ptr->element_type = field->type;
    
    vm_stack_push(&vm, val_obj((Object*)ffi_ptr));
    
    DISPATCH;
}

OPCODE(OP_GET_METHOD) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取结构体方法或原生对象方法
    // 操作数：2字节方法名常量索引
    // 栈：obj -> func
    uint16_t method_name_idx = READ_SHORT();
    
    Value method_name_val = frame->chunk->constants[method_name_idx];
    if (!val_is_obj(method_name_val) || val_as_obj(method_name_val)->type != OBJ_STRING) {
        runtime_error("方法名必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* method_name = (ObjString*)val_as_obj(method_name_val);
    
    // 从栈中弹出对象
    Value obj_val = vm_stack_pop(&vm);
    
    if (!val_is_obj(obj_val)) {
        // 增强错误信息：显示实际类型和值
        char type_buf[64];
        if (val_is_null(obj_val)) snprintf(type_buf, sizeof(type_buf), "null");
        else if (val_is_int(obj_val)) snprintf(type_buf, sizeof(type_buf), "int(%lld)", (long long)val_as_int(obj_val));
        else if (val_is_float(obj_val)) snprintf(type_buf, sizeof(type_buf), "float(%g)", val_as_double(obj_val));
        else if (val_is_bool(obj_val)) snprintf(type_buf, sizeof(type_buf), "bool(%s)", val_as_bool(obj_val) ? "true" : "false");
        else snprintf(type_buf, sizeof(type_buf), "unknown");
        char msg[256];
        ObjString* mn = (ObjString*)val_as_obj(method_name_val);
        snprintf(msg, sizeof(msg), "尝试在非对象类型上获取方法 '%s'（实际类型: %s）",
                 mn ? mn->chars : "?", type_buf);
        runtime_error(msg, GET_CURRENT_LINE());
        return -1;
    }
    
    Object* obj = val_as_obj(obj_val);
    
    // 处理 struct 类型
    if (obj->type == OBJ_STRUCT) {
        ObjStruct* s = (ObjStruct*)obj;
        ObjStructDef* def = s->def;
        
        // 在方法表中查找
        ObjClosure* closure = NULL;
        ObjFunction* func = NULL;
        for (int i = 0; i < def->method_count; i++) {
            if (strcmp(def->methods[i].name, method_name->chars) == 0) {
                // 跳过构造/析构函数（不允许显式调用）
                if (def->has_ctor && i == def->ctor_index) continue;
                if (def->has_dtor && i == def->dtor_index) continue;
                closure = def->methods[i].closure;
                func = def->methods[i].func;
                break;
            }
        }

        if (func) {
            if (closure) {
                vm_stack_push(&vm, val_obj((Object*)closure));
                DISPATCH;
            } else {
                vm_stack_push(&vm, obj_val);
                ObjClosure* new_closure = (ObjClosure*)gc_alloc(sizeof(ObjClosure), OBJ_CLOSURE);
                vm_stack_pop(&vm);
                if (!new_closure) {
                    runtime_error("内存不足，无法创建方法闭包", GET_CURRENT_LINE());
                    return -1;
                }
                new_closure->function = func;
                new_closure->upvalue_count = func->upvalue_count;
                for (int i = 0; i < func->upvalue_count; i++) {
                    new_closure->upvalues[i] = NULL;
                }
                vm_stack_push(&vm, val_obj((Object*)new_closure));
                DISPATCH;
            }
        }
        
        // 未找到用户定义的方法，尝试查找原生方法（如 copy）
        ObjNative* native_method = struct_find_method(method_name->chars);
        if (native_method) {
            vm_stack_push(&vm, obj_val);
            ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
            vm_stack_pop(&vm);
            vm_stack_push(&vm, val_obj((Object*)bound));
            DISPATCH;
        }
        
        char _msg1[256];
        snprintf(_msg1, sizeof(_msg1), "类型 '%s' 没有方法 '%s'",
                 def->name ? def->name : "?", method_name->chars);
        runtime_error(_msg1, GET_CURRENT_LINE());
        return -1;
    }

    // 处理原生对象类型（File, Socket 等）
    ObjNative* native_method = NULL;
    if (obj->type == OBJ_FILE) {
        native_method = file_find_method(method_name->chars);
    } else if (obj->type == OBJ_SOCKET) {
        native_method = socket_find_method(method_name->chars);
    }

    if (native_method) {
        vm_stack_push(&vm, obj_val);
        ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
        vm_stack_pop(&vm);
        vm_stack_push(&vm, val_obj((Object*)bound));
        DISPATCH;
    }

    if (obj->type == OBJ_FILE || obj->type == OBJ_SOCKET) {
        char _obj_type_name[32];
        if (obj->type == OBJ_FILE) snprintf(_obj_type_name, sizeof(_obj_type_name), "File");
        else snprintf(_obj_type_name, sizeof(_obj_type_name), "Socket");
        char _msg2[256];
        snprintf(_msg2, sizeof(_msg2), "类型 '%s' 没有方法 '%s'",
                 _obj_type_name, method_name->chars);
        runtime_error(_msg2, GET_CURRENT_LINE());
        return -1;
    }
    
    {
        // 其他对象类型：显示实际类型名
        const char* _tn = "unknown";
        switch (obj->type) {
            case OBJ_STRING: _tn = "string"; break;
            case OBJ_ARRAY:  _tn = "array"; break;
            case OBJ_DICT:   _tn = "dict"; break;
            case OBJ_CLOSURE: _tn = "closure"; break;
            case OBJ_MODULE: _tn = "module"; break;
            case OBJ_CSTRUCT: _tn = "cstruct"; break;
            case OBJ_FFI_LIBRARY: _tn = "ffi.Library"; break;
            case OBJ_FFI_POINTER: _tn = "ffi.Pointer"; break;
            case OBJ_FFI_CALLBACK: _tn = "ffi.Callback"; break;
            case OBJ_THREAD: _tn = "Thread"; break;
            case OBJ_CHANNEL: _tn = "Channel"; break;
            case OBJ_COROUTINE: _tn = "Coroutine"; break;
            default: break;
        }
        char _msg3[256];
        snprintf(_msg3, sizeof(_msg3), "尝试在非 struct 类型上获取方法 '%s'（实际类型: %s）",
                 method_name->chars, _tn);
        runtime_error(_msg3, GET_CURRENT_LINE());
    }
    return -1;
}
