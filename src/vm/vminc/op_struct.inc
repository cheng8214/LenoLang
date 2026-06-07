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

    // 读取字段信息
    for (int i = 0; i < field_count; i++) {
        uint16_t field_name_idx = READ_SHORT();
        uint8_t field_type = READ_BYTE();

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
                            struct_type_name, default_value, has_default, element_type);
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
    
    // 注册结构体定义
    struct_def_register(def);
    
    DISPATCH;
}

OPCODE(OP_FACE_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint16_t name_idx = READ_SHORT();
    uint8_t method_count = READ_BYTE();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("接口名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    ObjFaceDef* def = face_def_new(name->chars, method_count);

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
    // 操作数：2字节结构体名称常量索引 + 1字节参数字段数量
    // 每个参数：1字节字段索引（编译期确定，避免运行时线性搜索）
    uint16_t name_idx = READ_SHORT();
    uint8_t arg_count = READ_BYTE();
    
    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("结构体名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);
    
    ObjStructDef* def = struct_def_find(name->chars);
    if (!def) {
        runtime_error("未定义的结构体", GET_CURRENT_LINE());
        return -1;
    }
    
    // 创建实例
    ObjStruct* obj = struct_instance_new(def);
    
    // 设置参数字段
    for (int i = 0; i < arg_count; i++) {
        // 从字节码读取字段索引（1字节）
        uint8_t field_idx = READ_BYTE();
        
        // 从栈中弹出值（注意：栈中是反序的）
        Value field_value = vm_stack_pop(&vm);
        
        // 边界检查
        if (field_idx >= def->field_count) {
            runtime_error("struct 字段索引越界", GET_CURRENT_LINE());
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
        runtime_error("尝试在非对象类型上获取字段", GET_CURRENT_LINE());
        return -1;
    }
    
    // 处理普通 struct
    if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
        ObjStructDef* def = obj->def;
        
        // 边界检查
        if (field_idx >= def->field_count) {
            runtime_error("struct 字段索引越界", GET_CURRENT_LINE());
            return -1;
        }
        
        Value field_value = struct_get_field(obj, field_idx);
        vm_stack_push(&vm, field_value);
    }
    else if (val_as_obj(obj_val)->type == OBJ_CSTRUCT) {
        ObjCStruct* obj = (ObjCStruct*)val_as_obj(obj_val);
        ObjCStructDef* def = obj->def;
        
        if (field_idx >= def->field_count) {
            runtime_error("cstruct 字段索引越界", GET_CURRENT_LINE());
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
        runtime_error("尝试在非对象类型上设置字段", GET_CURRENT_LINE());
        return -1;
    }
    
    if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
        ObjStruct* obj = (ObjStruct*)val_as_obj(obj_val);
        ObjStructDef* def = obj->def;
        
        if (field_idx >= def->field_count) {
            runtime_error("struct 字段索引越界", GET_CURRENT_LINE());
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
            runtime_error("cstruct 字段索引越界", GET_CURRENT_LINE());
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
        runtime_error("尝试在非 struct/cstruct 类型上设置字段", GET_CURRENT_LINE());
        return -1;
    }
    
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
        runtime_error("尝试在非对象类型上获取方法", GET_CURRENT_LINE());
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
        
        runtime_error("对象没有此方法", GET_CURRENT_LINE());
        return -1;
    }
    
    // 处理原生对象类型（Event, Draw, Win 等）
    ObjNative* native_method = NULL;
    if (obj->type == OBJ_GUI_EVENT) {
        native_method = event_find_method(method_name->chars);
    } else if (obj->type == OBJ_GUI_RENDERER) {
        native_method = draw_find_method(method_name->chars);
    } else if (obj->type == OBJ_GUI_WINDOW) {
        native_method = window_find_method(method_name->chars);
    } else if (obj->type == OBJ_GUI_IMAGE) {
        native_method = image_find_method(method_name->chars);
    } else if (obj->type == OBJ_GUI_FONT) {
        native_method = font_find_method(method_name->chars);
    } else if (obj->type == OBJ_FILE) {
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

    if (obj->type == OBJ_GUI_EVENT || obj->type == OBJ_GUI_RENDERER || obj->type == OBJ_GUI_WINDOW ||
         obj->type == OBJ_GUI_IMAGE || obj->type == OBJ_GUI_FONT || obj->type == OBJ_FILE || obj->type == OBJ_SOCKET) {
        runtime_error("对象没有此方法", GET_CURRENT_LINE());
        return -1;
    }
    
    runtime_error("尝试在非 struct 类型上获取方法", GET_CURRENT_LINE());
    return -1;
}
