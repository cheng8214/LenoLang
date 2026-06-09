// ============================================================================
// 工具操作类操作码 (OP_UTILS)
// ============================================================================
// 通用索引访问、长度获取、迭代获取

OPCODE(OP_INDEX) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 通用索引访问：支持数组和字典
    Value index_val = vm_stack_pop(&vm);
    Value obj_val = vm_stack_pop(&vm);

    if (!val_is_obj(obj_val)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "索引操作需要对象类型，但实际类型为 '%s'", value_type_desc(obj_val));
        throw_runtime_error(msg, GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    switch (val_as_obj(obj_val)->type) {
        case OBJ_ARRAY: {
            if (!val_is_num(index_val)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "数组索引必须是数字，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            int index = (int)value_to_double(index_val);
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            if (index < 0 || index >= arr->count) {
                char msg[256];
                snprintf(msg, sizeof(msg), "数组索引越界: 索引=%d, 数组长度=%d", index, arr->count);
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            vm_stack_push(&vm, arr->elements[index]);
            break;
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            Value result = dict_get(dict, index_val);
            vm_stack_push(&vm, result);
            break;
        }
        case OBJ_STRING: {
            if (!val_is_num(index_val)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "字符串索引必须是数字，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            int index = (int)value_to_double(index_val);
            ObjString* str = (ObjString*)val_as_obj(obj_val);
            if (index < 0 || index >= str->len) {
                char msg[256];
                snprintf(msg, sizeof(msg), "字符串索引越界: 索引=%d, 字符串长度=%d", index, str->len);
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjString* result = str_new(&str->chars[index], 1);
            vm_stack_push(&vm, val_obj((Object*)result));
            break;
        }
        case OBJ_MODULE: {
            if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) {
                throw_runtime_error("模块键必须是字符串", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjModule* module = (ObjModule*)val_as_obj(obj_val);
            Value result = dict_get(module->exports, index_val);
            vm_stack_push(&vm, result);
            break;
        }
        case OBJ_STRUCT: {
            if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) {
                char msg[256];
                snprintf(msg, sizeof(msg), "struct 字段名必须是字符串，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjStruct* st = (ObjStruct*)val_as_obj(obj_val);
            ObjString* key = (ObjString*)val_as_obj(index_val);

            // 首先尝试查找字段
            int field_idx = struct_get_field_index(st->def, key->chars);
            if (field_idx >= 0) {
                Value result = struct_get_field(st, field_idx);
                vm_stack_push(&vm, result);
                break;
            }

            // 字段未找到，尝试查找用户定义的方法
            ObjStructDef* def = st->def;
            ObjClosure* closure = NULL;
            for (int i = 0; i < def->method_count; i++) {
                if (strcmp(def->methods[i].name, key->chars) == 0) {
                    closure = def->methods[i].closure;
                    break;
                }
            }

            if (closure) {
                // 找到用户定义的方法，压入预创建的闭包
                vm_stack_push(&vm, val_obj((Object*)closure));
                break;
            }

            // 尝试查找原生方法（如 copy）
            ObjNative* native_method = struct_find_method(key->chars);
            if (native_method) {
                // 创建绑定方法（receiver + native_method）
                ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
                vm_stack_push(&vm, val_obj((Object*)bound));
                break;
            }

            // 未找到字段或方法，报错
            char msg[256];
            snprintf(msg, sizeof(msg), "struct '%s' 没有字段或方法 '%s'", st->def->name, key->chars);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            break;
        }
        case OBJ_CSTRUCT: {
            // cstruct 索引访问：从 cstruct 的字段值数组中获取，或查找方法
            if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 字段名必须是字符串，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjCStruct* cst = (ObjCStruct*)val_as_obj(obj_val);
            ObjString* key = (ObjString*)val_as_obj(index_val);

            // 首先尝试查找字段
            int field_idx = cstruct_get_field_index(cst->def, key->chars);
            if (field_idx >= 0) {
                Value result = cstruct_get_field_value(cst, field_idx);
                vm_stack_push(&vm, result);
                break;
            }

            // 字段未找到，尝试查找原生方法
            ObjNative* native_method = cstruct_find_method(key->chars);
            if (native_method) {
                // 创建绑定方法（receiver + native_method）
                ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
                vm_stack_push(&vm, val_obj((Object*)bound));
                break;
            }

            // 未找到字段或方法，报错
            char msg[256];
            snprintf(msg, sizeof(msg), "cstruct '%s' 没有字段或方法 '%s'", cst->def->name, key->chars);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            break;
        }
        case OBJ_ENUM_DEF: {
            // enum 定义索引访问：通过成员名获取成员值
            if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) {
                char msg[256];
                snprintf(msg, sizeof(msg), "enum 成员名必须是字符串，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjEnumDef* def = (ObjEnumDef*)val_as_obj(obj_val);
            ObjString* key = (ObjString*)val_as_obj(index_val);

            // 查找成员值
            int64_t value = enum_def_get_member_value(def, key->chars);
            if (value < 0) {
                char msg[256];
                snprintf(msg, sizeof(msg), "enum '%s' 没有成员 '%s'", def->name, key->chars);
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }

            // 使用val_int_safe支持大整数
            vm_stack_push(&vm, val_int_safe(value));
            break;
        }
        case OBJ_CSTRUCT_DEF: {
            // cstruct 定义索引访问：获取静态方法（如 alloc, from_ptr）
            if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 方法名必须是字符串，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjCStructDef* def = (ObjCStructDef*)val_as_obj(obj_val);
            ObjString* key = (ObjString*)val_as_obj(index_val);

            // 查找 cstruct 静态方法
            ObjNative* native_method = cstruct_find_method(key->chars);
            if (native_method) {
                // 创建绑定方法（receiver + native_method）
                ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
                vm_stack_push(&vm, val_obj((Object*)bound));
                break;
            }

            // 未找到方法，报错
            char msg[256];
            snprintf(msg, sizeof(msg), "cstruct '%s' 没有方法 '%s'", def->name, key->chars);
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            break;
        }
        case OBJ_CSTRUCT_ARRAY_VIEW: {
            // cstruct 数组字段视图索引访问
            if (!val_is_num(index_val)) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 数组索引必须是数字，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            int index = (int)value_to_double(index_val);
            ObjCStructArrayView* view = (ObjCStructArrayView*)val_as_obj(obj_val);

            // 边界检查
            if (index < 0 || index >= view->array_dim) {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 数组索引越界: 索引=%d, 数组长度=%d", index, view->array_dim);
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }

            // 计算元素地址
            ObjCStruct* cst = view->cstruct;
            CStructFieldInfo* field = &cst->def->fields[view->field_index];
            uint8_t* element_addr = cst->data + field->offset + (index * view->element_size);

            // 如果元素类型是嵌套 cstruct，创建 cstruct 实例
            if (view->element_type == TYPE_CSTRUCT && field->struct_name) {
                ObjCStructDef* nested_def = cstruct_def_find(field->struct_name);
                if (nested_def) {
                    ObjCStruct* nested_obj = (ObjCStruct*)gc_alloc(sizeof(ObjCStruct), OBJ_CSTRUCT);
                    if (nested_obj) {
                        nested_obj->def = nested_def;
                        nested_obj->data = element_addr;
                        nested_obj->owns_memory = 0;
                        vm_stack_push(&vm, val_obj((Object*)nested_obj));
                        break;
                    }
                }
            }

            // 读取基本类型元素值
            Value result = val_null();
            switch (view->element_type) {
                case TYPE_I8:
                    result = val_num((double)(*(int8_t*)element_addr));
                    break;
                case TYPE_U8:
                    result = val_num((double)(*(uint8_t*)element_addr));
                    break;
                case TYPE_I16:
                    result = val_num((double)(*(int16_t*)element_addr));
                    break;
                case TYPE_U16:
                    result = val_num((double)(*(uint16_t*)element_addr));
                    break;
                case TYPE_I32:
                    result = val_num((double)(*(int32_t*)element_addr));
                    break;
                case TYPE_U32:
                    result = val_num((double)(*(uint32_t*)element_addr));
                    break;
                case TYPE_I64:
                    result = val_num((double)(*(int64_t*)element_addr));
                    break;
                case TYPE_U64:
                    result = val_num((double)(*(uint64_t*)element_addr));
                    break;
                case TYPE_F32:
                    result = val_num((double)(*(float*)element_addr));
                    break;
                case TYPE_F64:
                    result = val_num((*(double*)element_addr));
                    break;
                case TYPE_BOOL:
                    result = val_bool(*(uint8_t*)element_addr);
                    break;
                case TYPE_C_INT:
                    result = val_num((double)(*(int*)element_addr));
                    break;
                case TYPE_C_UINT:
                    result = val_num((double)(*(unsigned int*)element_addr));
                    break;
                case TYPE_C_LONG:
                    result = val_num((double)(*(long*)element_addr));
                    break;
                case TYPE_C_ULONG:
                    result = val_num((double)(*(unsigned long*)element_addr));
                    break;
                case TYPE_C_LONGLONG:
                    result = val_num((double)(*(long long*)element_addr));
                    break;
                case TYPE_C_ULONGLONG:
                    result = val_num((double)(*(unsigned long long*)element_addr));
                    break;
                case TYPE_C_SIZE:
                    result = val_num((double)(*(size_t*)element_addr));
                    break;
                case TYPE_C_SSIZE:
                    result = val_num((double)(*(ssize_t*)element_addr));
                    break;
                default:
                    break;
            }
            vm_stack_push(&vm, result);
            break;
        }
        case OBJ_CSTRUCT_ARRAY: {
            // cstruct 数组索引访问（批量操作）或方法访问
            if (val_is_num(index_val)) {
                // 数字索引：访问数组元素
                int index = (int)value_to_double(index_val);
                ObjCStructArray* array = (ObjCStructArray*)val_as_obj(obj_val);

                // 边界检查
                if (index < 0 || index >= array->count) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cstruct 数组索引越界: 索引=%d, 数组长度=%d", index, array->count);
                    throw_runtime_error(msg, GET_CURRENT_LINE());
                    frame = &vm.frames[vm.frame_cnt - 1];
                    break;
                }

                // 获取指定索引的元素（创建临时 cstruct 实例，不拥有内存）
                ObjCStruct* element = cstruct_array_get(array, index);
                if (element) {
                    vm_stack_push(&vm, val_obj((Object*)element));
                } else {
                    vm_stack_push(&vm, val_null());
                }
            } else if (val_is_obj(index_val) && val_as_obj(index_val)->type == OBJ_STRING) {
                // 字符串索引：方法访问
                ObjString* key = (ObjString*)val_as_obj(index_val);
                
                // 查找 cstruct 方法（cstruct 数组使用 cstruct 的方法表）
                ObjNative* native_method = cstruct_find_method(key->chars);
                if (native_method) {
                    // 创建绑定方法（receiver + native_method）
                    ObjBoundMethod* bound = bound_method_new(obj_val, native_method);
                    vm_stack_push(&vm, val_obj((Object*)bound));
                } else {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "cstruct 数组没有方法 '%s'", key->chars);
                    throw_runtime_error(msg, GET_CURRENT_LINE());
                    frame = &vm.frames[vm.frame_cnt - 1];
                }
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "cstruct 数组索引必须是数字或方法名，但实际类型为 '%s'", value_type_desc(index_val));
                throw_runtime_error(msg, GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
            }
            break;
        }
        default:
            char msg[256];
            snprintf(msg, sizeof(msg), "不支持的索引类型: '%s' 不支持索引访问", value_type_desc(obj_val));
            throw_runtime_error(msg, GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            break;
    }
    DISPATCH;
}

OPCODE(OP_LENGTH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 获取长度：支持字符串、数组、字典、数字
    Value obj_val = vm_stack_pop(&vm);

    // 数字类型：返回数字本身作为长度（支持 for n to var i 语法）
    if (val_is_num(obj_val)) {
        int len = (int)value_to_double(obj_val);
        if (len < 0) len = 0;
        vm_stack_push(&vm, val_int(len));
        DISPATCH;
    }

    if (!val_is_obj(obj_val)) {
        throw_runtime_error("LENGTH操作需要对象或数字类型", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    switch (val_as_obj(obj_val)->type) {
        case OBJ_STRING: {
            ObjString* str = (ObjString*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(str->len));
            break;
        }
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(arr->count));
            break;
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(dict->order_count));
            break;
        }
        case OBJ_ENUM_DEF: {
            ObjEnumDef* enum_def = (ObjEnumDef*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(enum_def->member_count));
            break;
        }
        case OBJ_CSTRUCT_ARRAY: {
            ObjCStructArray* array = (ObjCStructArray*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(array->count));
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)val_as_obj(obj_val);
            vm_stack_push(&vm, val_int(struct_obj->def->field_count));
            break;
        }
        default:
            throw_runtime_error("不支持的LENGTH类型", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            break;
    }
    DISPATCH;
}

OPCODE(OP_ITER_GET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 迭代获取元素：运行时检测类型
    // 栈顶是索引，次栈顶是对象
    Value index_val = vm_stack_pop(&vm);
    Value obj_val = vm_stack_pop(&vm);

    if (!val_is_num(index_val)) {
        throw_runtime_error("ITER_GET索引必须是数字", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    int index = (int)value_to_double(index_val);

    // 支持数字类型：for n to var i 时，返回索引本身
    if (val_is_num(obj_val)) {
        int max = (int)value_to_double(obj_val);
        if (index < 0 || index >= max) {
            throw_runtime_error("数字迭代索引越界", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        vm_stack_push(&vm, val_int(index));
        DISPATCH;
    }

    if (!val_is_obj(obj_val)) {
        throw_runtime_error("ITER_GET需要对象或数字类型", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }

    switch (val_as_obj(obj_val)->type) {
        case OBJ_ARRAY: {
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            if (index < 0 || index >= arr->count) {
                throw_runtime_error("数组索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            vm_stack_push(&vm, arr->elements[index]);
            break;
        }
        case OBJ_STRING: {
            ObjString* str = (ObjString*)val_as_obj(obj_val);
            if (index < 0 || index >= str->len) {
                throw_runtime_error("字符串索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            ObjString* result = str_new(&str->chars[index], 1);
            vm_stack_push(&vm, val_obj((Object*)result));
            break;
        }
        case OBJ_DICT: {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            if (index < 0 || index >= dict->order_count) {
                throw_runtime_error("字典键索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            Value key = dict_get_key_by_index(dict, index);
            if (!val_is_null(key)) {
                vm_stack_push(&vm, key);
            } else {
                vm_stack_push(&vm, val_null());
            }
            break;
        }
        case OBJ_ENUM_DEF: {
            ObjEnumDef* enum_def = (ObjEnumDef*)val_as_obj(obj_val);
            if (index < 0 || index >= enum_def->member_count) {
                throw_runtime_error("枚举索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            // 返回枚举成员的值
            vm_stack_push(&vm, val_int((int)enum_def->members[index].value));
            break;
        }
        case OBJ_STRUCT: {
            ObjStruct* struct_obj = (ObjStruct*)val_as_obj(obj_val);
            if (index < 0 || index >= struct_obj->def->field_count) {
                throw_runtime_error("结构体字段索引越界", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                break;
            }
            // 返回字段名（与字典迭代一致：ITER_GET 返回 key）
            ObjString* field_name = str_copy(struct_obj->def->fields[index].name,
                                              strlen(struct_obj->def->fields[index].name));
            vm_stack_push(&vm, val_obj((Object*)field_name));
            break;
        }
        default:
            runtime_error("ITER_GET不支持的类型", GET_CURRENT_LINE());
            return -1;
    }
    DISPATCH;
}
