// ============================================================================
// C 布局结构体相关操作码
// ============================================================================

OPCODE(OP_CSTRUCT_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 定义 C 布局结构体
    // 操作数：2字节名称常量索引 + 1字节字段数量 + 2字节总大小 + 1字节对齐要求
    // 每个字段：2字节字段名常量索引 + 1字节类型 + 2字节偏移量 + 2字节数组维度 + 2字节结构体类型名常量索引
    //          如果类型是 TYPE_PTR_GENERIC：+ 1字节元素类型
    uint16_t name_idx = READ_SHORT();
    uint8_t field_count = READ_BYTE();
    uint16_t total_size = READ_SHORT();
    uint8_t alignment = READ_BYTE();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("cstruct 名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    // 创建 C 布局结构体定义
    ObjCStructDef* def = cstruct_def_new(name->chars, field_count, total_size, alignment);

    // 读取字段信息
    for (int i = 0; i < field_count; i++) {
        uint16_t field_name_idx = READ_SHORT();
        uint8_t field_type = READ_BYTE();
        uint16_t field_offset = READ_SHORT();
        uint16_t array_dim = READ_SHORT();
        uint16_t struct_name_idx = READ_SHORT();

        Value field_name_val = frame->chunk->constants[field_name_idx];
        if (!val_is_obj(field_name_val) || val_as_obj(field_name_val)->type != OBJ_STRING) {
            runtime_error("cstruct 字段名必须是字符串", GET_CURRENT_LINE());
            return -1;
        }
        ObjString* field_name = (ObjString*)val_as_obj(field_name_val);

        // 获取结构体类型名（仅当 type == TYPE_CSTRUCT 时有效）
        char* struct_name = NULL;
        if (field_type == TYPE_CSTRUCT && struct_name_idx != 0xFFFF) {
            Value struct_name_val = frame->chunk->constants[struct_name_idx];
            if (val_is_obj(struct_name_val) && val_as_obj(struct_name_val)->type == OBJ_STRING) {
                struct_name = ((ObjString*)val_as_obj(struct_name_val))->chars;
            }
        }

        // 读取 Ptr[T] 元素类型（仅当 type == TYPE_PTR_GENERIC 时有效）
        TypeKind element_type = TYPE_PTR;
        if (field_type == TYPE_PTR_GENERIC) {
            element_type = (TypeKind)READ_BYTE();
        }

        // 计算字段大小（单个元素的大小）
        int field_size = c_layout_type_size((TypeKind)field_type);

        // 设置字段信息
        cstruct_def_set_field(def, i, field_name->chars, (TypeKind)field_type, field_offset, field_size, array_dim, struct_name, element_type);
    }

    // 注册 C 布局结构体定义
    cstruct_def_register(def);

    // 将 cstruct 定义对象保存到当前模块的 globals 中（如果有模块上下文）
    // 这样序列化时可以包含 cstruct 定义
    if (vm.current_module_frame && vm.current_module_frame->module) {
        ObjModule* mod = vm.current_module_frame->module;
        // 查找是否已有该名称的 global 变量，或者分配新的
        // 简单处理：扩展 globals 数组并添加
        int new_index = mod->global_count;
        Value* new_globals = realloc(mod->globals, (new_index + 1) * sizeof(Value));
        if (new_globals) {
            mod->globals = new_globals;
            mod->globals[new_index] = val_obj((Object*)def);
            mod->global_count = new_index + 1;
            if (mod->global_count > mod->global_capacity) {
                mod->global_capacity = mod->global_count;
            }
            gc_write_barrier((Object*)mod, val_obj((Object*)def));
        }
    }

    // 将 cstruct 定义对象压入栈
    vm_stack_push(&vm, val_obj((Object*)def));

    DISPATCH;
}

OPCODE(OP_GET_CSTRUCT_DEF) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取 C 布局结构体定义
    // 操作数：2字节名称常量索引
    uint16_t name_idx = READ_SHORT();

    Value name_val = frame->chunk->constants[name_idx];
    if (!val_is_obj(name_val) || val_as_obj(name_val)->type != OBJ_STRING) {
        runtime_error("cstruct 名称必须是字符串", GET_CURRENT_LINE());
        return -1;
    }
    ObjString* name = (ObjString*)val_as_obj(name_val);

    // 查找 cstruct 定义
    ObjCStructDef* def = cstruct_def_find(name->chars);
    if (!def) {
        char msg[256];
        snprintf(msg, sizeof(msg), "找不到 cstruct 定义 '%s'", name->chars);
        runtime_error(msg, GET_CURRENT_LINE());
        return -1;
    }

    // 将 cstruct 定义对象压入栈
    vm_stack_push(&vm, val_obj((Object*)def));

    DISPATCH;
}
