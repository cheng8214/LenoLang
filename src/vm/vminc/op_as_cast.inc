// ============================================================================
// 安全类型转换操作码 (OP_AS_CAST)
//
// 字节码格式（非 face 类型）:
//   [OP_AS_CAST] [type_kind:1] [element_type_kind:1]
//   - type_kind: 主类型 (TypeKind 枚举值)
//   - element_type_kind: 元素/子类型，无元素类型时为 TYPE_ANY
//
// 字节码格式（face 类型）:
//   [OP_AS_CAST] [TYPE_FACE:1] [face_name_const_idx:2]
//   - face_name_const_idx: 2 字节大端序，指向常量池中的 face 名称字符串
//
// 执行逻辑:
//   弹出栈顶值，检查是否符合期望类型:
//   - 匹配: 将原值压回栈
//   - 不匹配: 将 null 压入栈
// ============================================================================

OPCODE(OP_AS_CAST) {
    frame = &vm.frames[vm.frame_cnt - 1];
    uint8_t type_kind_byte = READ_BYTE();
    TypeKind expected_type = (TypeKind)type_kind_byte;

    Value value = vm_stack_pop(&vm);
    int matches = 0;

    switch (expected_type) {
        case TYPE_INT:
            (void)READ_BYTE();
            matches = val_is_int(value);
            break;
        case TYPE_FLOAT:
            (void)READ_BYTE();
            matches = val_is_float(value);
            break;
        case TYPE_STRING:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRING);
            break;
        case TYPE_BOOL:
            (void)READ_BYTE();
            matches = val_is_bool(value);
            break;

        case TYPE_ARRAY: {
            uint8_t elem_type_byte = READ_BYTE();
            TypeKind expected_elem_type = (TypeKind)elem_type_byte;
            if (!val_is_obj(value) || val_as_obj(value)->type != OBJ_ARRAY) {
                matches = 0;
                break;
            }
            if (expected_elem_type != TYPE_ANY) {
                ObjArray* arr = (ObjArray*)val_as_obj(value);
                matches = 1;
                for (int i = 0; i < arr->count; i++) {
                    Value elem = arr->elements[i];
                    int elem_matches = 0;
                    switch (expected_elem_type) {
                        case TYPE_INT:
                            elem_matches = val_is_int(elem);
                            break;
                        case TYPE_FLOAT:
                            elem_matches = val_is_float(elem);
                            break;
                        case TYPE_STRING:
                            elem_matches = (val_is_obj(elem) && val_as_obj(elem)->type == OBJ_STRING);
                            break;
                        case TYPE_BOOL:
                            elem_matches = val_is_bool(elem);
                            break;
                        default:
                            elem_matches = 1;
                            break;
                    }
                    if (!elem_matches) {
                        matches = 0;
                        break;
                    }
                }
            } else {
                matches = 1;
            }
            break;
        }

        case TYPE_DICT:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_DICT);
            break;
        case TYPE_STRUCT: {
            uint16_t struct_name_idx = READ_SHORT();
            Value struct_name_val = frame->chunk->constants[struct_name_idx];
            if (!val_is_obj(struct_name_val) || val_as_obj(struct_name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* struct_name = ((ObjString*)val_as_obj(struct_name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (obj->def && obj->def->name) {
                    matches = (strcmp(obj->def->name, struct_name) == 0);
                } else {
                    matches = 0;
                }
            } else {
                matches = 0;
            }
            break;
        }

        case TYPE_FACE: {
            uint16_t face_name_idx = READ_SHORT();
            Value face_name_val = frame->chunk->constants[face_name_idx];
            if (!val_is_obj(face_name_val) || val_as_obj(face_name_val)->type != OBJ_STRING) {
                matches = 0;
                break;
            }
            const char* face_name = ((ObjString*)val_as_obj(face_name_val))->chars;
            if (val_is_obj(value) && val_as_obj(value)->type == OBJ_STRUCT) {
                ObjStruct* obj = (ObjStruct*)val_as_obj(value);
                if (!obj->def) {
                    matches = 0;
                    break;
                }
                ObjFaceDef* fdef = face_def_find(face_name);
                if (fdef) {
                    matches = struct_implements_face(obj->def, fdef);
                } else {
                    matches = 0;
                }
            }
            break;
        }

        case TYPE_FILE:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_FILE);
            break;
        case TYPE_WIN:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_GUI_WINDOW);
            break;
        case TYPE_DRAW:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_GUI_RENDERER);
            break;
        case TYPE_EVENT:
            (void)READ_BYTE();
            matches = (val_is_obj(value) && val_as_obj(value)->type == OBJ_DICT);
            break;
        case TYPE_PTR:
            (void)READ_BYTE();
            matches = (val_is_obj(value) &&
                      (val_as_obj(value)->type == OBJ_FFI_POINTER ||
                       val_as_obj(value)->type == OBJ_FFI_LIBRARY ||
                       val_as_obj(value)->type == OBJ_FFI_CALLBACK));
            break;
        case TYPE_NULL:
            (void)READ_BYTE();
            matches = val_is_null(value);
            break;
        case TYPE_ANY:
            (void)READ_BYTE();
            matches = 1;
            break;
        default:
            (void)READ_BYTE();
            matches = 0;
            break;
    }

    if (matches) {
        vm_stack_push(&vm, value);
    } else {
        vm_stack_push(&vm, val_null());
    }
    DISPATCH;
}
