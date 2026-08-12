// ============================================================================
// 索引和切片操作类操作码 (OP_INDEX_SLICE)
// ============================================================================

// 共享的索引赋值逻辑宏：pop 3 个值，执行赋值
// _nopush=1 时不压入 obj_val 到栈顶（语句上下文）
#define DO_INDEX_SET(_nopush, _label) do { \
    Value value = vm_stack_pop(&vm); \
    Value index_val = vm_stack_pop(&vm); \
    Value obj_val = vm_stack_pop(&vm); \
    if (!val_is_obj(obj_val)) { \
        throw_runtime_error("索引赋值需要对象类型", GET_CURRENT_LINE()); \
        goto _label; \
    } \
    Object* obj = val_as_obj(obj_val); \
    if (obj->type == OBJ_ARRAY) { \
        if (!val_is_num(index_val)) { \
            throw_runtime_error("数组索引必须是数字", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        int _idx = (int)value_to_double(index_val); \
        ObjArray* arr = (ObjArray*)obj; \
        if (_idx < 0 || _idx >= arr->capacity) { \
            throw_runtime_error("数组索引越界", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        arr->elements[_idx] = value; \
        gc_write_barrier((Object*)arr, value); \
        if (_idx >= arr->count) arr->count = _idx + 1; \
        if (!(_nopush)) vm_stack_push(&vm, obj_val); \
    } else if (obj->type == OBJ_DICT) { \
        ObjDict* dict = (ObjDict*)obj; \
        dict_set(dict, index_val, value); \
        if (!(_nopush)) vm_stack_push(&vm, obj_val); \
    } else if (obj->type == OBJ_STRUCT) { \
        if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) { \
            throw_runtime_error("struct 字段名必须是字符串", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        ObjStruct* struct_obj = (ObjStruct*)obj; \
        ObjStructDef* def = struct_obj->def; \
        ObjString* field_name = (ObjString*)val_as_obj(index_val); \
        int field_idx = struct_get_field_index(def, field_name->chars); \
        if (field_idx < 0) { \
            throw_runtime_error("struct 字段不存在", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        TypeKind expected_type = def->fields[field_idx].type; \
        if (expected_type == TYPE_FLOAT && val_is_int(value)) { \
            value = val_float((double)val_as_int(value)); \
        } else if (expected_type == TYPE_FLOAT && val_is_bigint(value)) { \
            value = val_float(bigint_to_double(val_as_bigint(value))); \
        } \
        struct_set_field(struct_obj, field_idx, value); \
        if (!(_nopush)) vm_stack_push(&vm, obj_val); \
    } else if (obj->type == OBJ_CSTRUCT) { \
        if (!val_is_obj(index_val) || val_as_obj(index_val)->type != OBJ_STRING) { \
            throw_runtime_error("cstruct 字段名必须是字符串", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        ObjCStruct* cstruct_obj = (ObjCStruct*)obj; \
        ObjCStructDef* def = cstruct_obj->def; \
        ObjString* field_name = (ObjString*)val_as_obj(index_val); \
        int field_idx = cstruct_get_field_index(def, field_name->chars); \
        if (field_idx < 0) { \
            throw_runtime_error("cstruct 字段不存在", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        CStructFieldInfo* field = &def->fields[field_idx]; \
        if (val_is_int(value) && (field->type == TYPE_F32 || field->type == TYPE_F64)) { \
            value = val_float((double)val_as_int(value)); \
        } else if (val_is_float(value) && (field->type >= TYPE_I8 && field->type <= TYPE_U64)) { \
            value = val_num(val_as_num(value)); \
        } \
        cstruct_set_field_value(cstruct_obj, field_idx, value); \
        if (!(_nopush)) vm_stack_push(&vm, obj_val); \
    } else if (obj->type == OBJ_CSTRUCT_ARRAY_VIEW) { \
        if (!val_is_num(index_val)) { \
            throw_runtime_error("cstruct 数组索引必须是数字", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        int _idx = (int)value_to_double(index_val); \
        ObjCStructArrayView* view = (ObjCStructArrayView*)obj; \
        if (_idx < 0 || _idx >= view->array_dim) { \
            throw_runtime_error("cstruct 数组索引越界", GET_CURRENT_LINE()); \
            goto _label; \
        } \
        ObjCStruct* cst = view->cstruct; \
        CStructFieldInfo* field = &cst->def->fields[view->field_index]; \
        uint8_t* element_addr = cst->data + field->offset + (_idx * view->element_size); \
        if (val_is_int(value) && (view->element_type == TYPE_F32 || view->element_type == TYPE_F64)) { \
            value = val_float((double)val_as_int(value)); \
        } else if (val_is_float(value) && (view->element_type >= TYPE_I8 && view->element_type <= TYPE_U64)) { \
            value = val_num(val_as_num(value)); \
        } \
        switch (view->element_type) { \
            case TYPE_I8:   *(int8_t*)element_addr = (int8_t)val_as_num(value); break; \
            case TYPE_U8:   *(uint8_t*)element_addr = (uint8_t)val_as_num(value); break; \
            case TYPE_I16:  *(int16_t*)element_addr = (int16_t)val_as_num(value); break; \
            case TYPE_U16:  *(uint16_t*)element_addr = (uint16_t)val_as_num(value); break; \
            case TYPE_I32:  *(int32_t*)element_addr = (int32_t)val_as_num(value); break; \
            case TYPE_U32:  *(uint32_t*)element_addr = (uint32_t)val_as_num(value); break; \
            case TYPE_I64:  *(int64_t*)element_addr = (int64_t)val_as_num(value); break; \
            case TYPE_U64:  *(uint64_t*)element_addr = (uint64_t)val_as_num(value); break; \
            case TYPE_F32:  *(float*)element_addr = (float)val_as_num(value); break; \
            case TYPE_F64:  *(double*)element_addr = (double)val_as_num(value); break; \
            case TYPE_BOOL: *(uint8_t*)element_addr = val_as_num(value) != 0 ? 1 : 0; break; \
            case TYPE_C_INT:       *(int*)element_addr = (int)val_as_num(value); break; \
            case TYPE_C_UINT:      *(unsigned int*)element_addr = (unsigned int)val_as_num(value); break; \
            case TYPE_C_LONG:      *(long*)element_addr = (long)val_as_num(value); break; \
            case TYPE_C_ULONG:     *(unsigned long*)element_addr = (unsigned long)val_as_num(value); break; \
            case TYPE_C_LONGLONG:  *(long long*)element_addr = (long long)val_as_num(value); break; \
            case TYPE_C_ULONGLONG: *(unsigned long long*)element_addr = (unsigned long long)val_as_num(value); break; \
            case TYPE_C_SIZE:      *(size_t*)element_addr = (size_t)val_as_num(value); break; \
            case TYPE_C_SSIZE:     *(ssize_t*)element_addr = (ssize_t)val_as_num(value); break; \
            default: break; \
        } \
        if (!(_nopush)) vm_stack_push(&vm, obj_val); \
    } else { \
        throw_runtime_error("索引赋值需要数组、字典或 struct", GET_CURRENT_LINE()); \
    } \
    _label:; \
} while(0)

OPCODE(OP_INDEX_SET) {
    frame = &vm.frames[vm.frame_cnt - 1];
    DO_INDEX_SET(0, idx_set_done_1);
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_INDEX_SET_NOPUSH) {
    frame = &vm.frames[vm.frame_cnt - 1];
    DO_INDEX_SET(1, idx_set_done_2);
    frame = &vm.frames[vm.frame_cnt - 1];
    DISPATCH;
}

OPCODE(OP_SLICE) {
    frame = &vm.frames[vm.frame_cnt - 1];
    // 数组切片：arr[start:end]，栈顶是 end（可为null），次栈顶是 start（可为null），次次栈顶是数组
    Value end_val = vm_stack_pop(&vm);
    Value start_val = vm_stack_pop(&vm);
    Value arr_val = vm_stack_pop(&vm);
    
    // 处理普通数组切片
    if (val_is_obj(arr_val) && val_as_obj(arr_val)->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
        int arr_len = arr->count;
        
        // 计算起始索引
        int start_idx = 0;
        if (!val_is_null(start_val)) {
            if (!val_is_num(start_val)) {
                throw_runtime_error("切片起始索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            start_idx = (int)value_to_double(start_val);
            if (start_idx < 0) start_idx = 0;
            if (start_idx > arr_len) start_idx = arr_len;
        }
        
        // 计算结束索引（包含）
        int end_idx = arr_len - 1;
        if (!val_is_null(end_val)) {
            if (!val_is_num(end_val)) {
                throw_runtime_error("切片结束索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            end_idx = (int)value_to_double(end_val);
            if (end_idx < 0) end_idx = 0;
            if (end_idx >= arr_len) end_idx = arr_len - 1;
        }

        // 创建新数组存储切片结果
        int slice_len = end_idx - start_idx + 1;
        if (slice_len < 0) slice_len = 0;

        ObjArray* result = arr_new(slice_len > 0 ? slice_len : 1);
        if (!result) {
            throw_runtime_error("切片数组内存分配失败", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        
        // 复制元素
        for (int i = 0; i < slice_len; i++) {
            arr_write(result, i, arr->elements[start_idx + i]);
        }
        
        vm_stack_push(&vm, val_obj((Object*)result));
        DISPATCH;
    }
    // 处理 cstruct 数组字段视图切片
    else if (val_is_obj(arr_val) && val_as_obj(arr_val)->type == OBJ_CSTRUCT_ARRAY_VIEW) {
        ObjCStructArrayView* view = (ObjCStructArrayView*)val_as_obj(arr_val);
        int arr_len = view->array_dim;
        
        // 计算起始索引
        int start_idx = 0;
        if (!val_is_null(start_val)) {
            if (!val_is_num(start_val)) {
                throw_runtime_error("切片起始索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            start_idx = (int)value_to_double(start_val);
            if (start_idx < 0) start_idx = 0;
            if (start_idx > arr_len) start_idx = arr_len;
        }
        
        // 计算结束索引（包含）
        int end_idx = arr_len - 1;
        if (!val_is_null(end_val)) {
            if (!val_is_num(end_val)) {
                throw_runtime_error("切片结束索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            end_idx = (int)value_to_double(end_val);
            if (end_idx < 0) end_idx = 0;
            if (end_idx >= arr_len) end_idx = arr_len - 1;
        }

        // 创建新数组存储切片结果
        int slice_len = end_idx - start_idx + 1;
        if (slice_len < 0) slice_len = 0;

        ObjArray* result = arr_new(slice_len > 0 ? slice_len : 1);
        if (!result) {
            throw_runtime_error("切片数组内存分配失败", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        
        // 从 cstruct 数组字段中读取元素值
        ObjCStruct* cst = view->cstruct;
        CStructFieldInfo* field = &cst->def->fields[view->field_index];
        
        for (int i = 0; i < slice_len; i++) {
            int element_idx = start_idx + i;
            uint8_t* element_addr = cst->data + field->offset + (element_idx * view->element_size);
            
            Value element_val;
            // 根据元素类型读取值
            switch (view->element_type) {
                case TYPE_I8:
                    element_val = val_num((double)*(int8_t*)element_addr);
                    break;
                case TYPE_U8:
                    element_val = val_num((double)*(uint8_t*)element_addr);
                    break;
                case TYPE_I16:
                    element_val = val_num((double)*(int16_t*)element_addr);
                    break;
                case TYPE_U16:
                    element_val = val_num((double)*(uint16_t*)element_addr);
                    break;
                case TYPE_I32:
                    element_val = val_num((double)*(int32_t*)element_addr);
                    break;
                case TYPE_U32:
                    element_val = val_num((double)*(uint32_t*)element_addr);
                    break;
                case TYPE_I64:
                    element_val = val_num((double)*(int64_t*)element_addr);
                    break;
                case TYPE_U64:
                    element_val = val_num((double)*(uint64_t*)element_addr);
                    break;
                case TYPE_F32:
                    element_val = val_float((double)*(float*)element_addr);
                    break;
                case TYPE_F64:
                    element_val = val_float((double)*(double*)element_addr);
                    break;
                case TYPE_BOOL:
                    element_val = val_bool(*(uint8_t*)element_addr != 0);
                    break;
                case TYPE_C_INT:
                    element_val = val_num((double)*(int*)element_addr);
                    break;
                case TYPE_C_UINT:
                    element_val = val_num((double)*(unsigned int*)element_addr);
                    break;
                case TYPE_C_LONG:
                    element_val = val_num((double)*(long*)element_addr);
                    break;
                case TYPE_C_ULONG:
                    element_val = val_num((double)*(unsigned long*)element_addr);
                    break;
                case TYPE_C_LONGLONG:
                    element_val = val_num((double)*(long long*)element_addr);
                    break;
                case TYPE_C_ULONGLONG:
                    element_val = val_num((double)*(unsigned long long*)element_addr);
                    break;
                case TYPE_C_SIZE:
                    element_val = val_num((double)*(size_t*)element_addr);
                    break;
                case TYPE_C_SSIZE:
                    element_val = val_num((double)*(ssize_t*)element_addr);
                    break;
                default:
                    element_val = val_null();
                    break;
            }
            
            arr_write(result, i, element_val);
        }
        
        vm_stack_push(&vm, val_obj((Object*)result));
        DISPATCH;
    }
    // 处理字符串切片
    else if (val_is_obj(arr_val) && val_as_obj(arr_val)->type == OBJ_STRING) {
        ObjString* str = (ObjString*)val_as_obj(arr_val);
        int char_len = str->char_len;
        
        // 计算起始索引（字符级）
        int start_idx = 0;
        if (!val_is_null(start_val)) {
            if (!val_is_num(start_val)) {
                throw_runtime_error("字符串切片起始索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            start_idx = (int)value_to_double(start_val);
            if (start_idx < 0) start_idx = 0;
            if (start_idx > char_len) start_idx = char_len;
        }
        
        // 计算结束索引（字符级，包含）
        int end_idx = char_len - 1;
        if (!val_is_null(end_val)) {
            if (!val_is_num(end_val)) {
                throw_runtime_error("字符串切片结束索引必须是数字", GET_CURRENT_LINE());
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH;
            }
            end_idx = (int)value_to_double(end_val);
            if (end_idx < 0) end_idx = 0;
            if (end_idx >= char_len) end_idx = char_len - 1;
        }

        // 计算切片长度（包含 end_idx）
        int slice_len = end_idx - start_idx + 1;
        if (slice_len < 0) slice_len = 0;

        if (slice_len == 0) {
            ObjString* result = str_copy("", 0);
            vm_stack_push(&vm, val_obj((Object*)result));
            DISPATCH;
        }

        // 将字符索引转换为字节偏移
        int byte_start = utf8_char_offset(str->chars, str->len, start_idx);
        int byte_end = utf8_char_offset(str->chars, str->len, end_idx + 1);
        int new_byte_len = byte_end - byte_start;
        
        ObjString* result = str_copy(str->chars + byte_start, new_byte_len);
        if (!result) {
            throw_runtime_error("字符串切片内存分配失败", GET_CURRENT_LINE());
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH;
        }
        
        vm_stack_push(&vm, val_obj((Object*)result));
        DISPATCH;
    }
    else {
        throw_runtime_error("切片操作需要数组、字符串或 cstruct 数组字段", GET_CURRENT_LINE());
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH;
    }
}
