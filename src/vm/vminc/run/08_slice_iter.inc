    LABEL(OP_SLICE): {
        int a = READ_A(), b = READ_B();
        Value obj_val = R(b);
        Value start_val = R(a + 1);
        Value end_val = R(a + 2);
        int line = GET_CURRENT_LINE();

        if (likely(val_is_obj(obj_val) && val_as_obj(obj_val)->type == OBJ_ARRAY)) {
            ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
            int arr_len = arr->count;
            int start_idx = 0, end_idx = arr_len - 1;

            if (!val_is_null(start_val)) {
                if (unlikely(!val_is_num(start_val))) {
                    if (!throw_runtime_error("切片起始索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                start_idx = (int)value_to_double(start_val);
                if (start_idx < 0) start_idx = 0;
                if (start_idx > arr_len) start_idx = arr_len;
            }
            if (!val_is_null(end_val)) {
                if (unlikely(!val_is_num(end_val))) {
                    if (!throw_runtime_error("切片结束索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                end_idx = (int)value_to_double(end_val);
                if (end_idx < 0) end_idx = 0;
                if (end_idx >= arr_len) end_idx = arr_len - 1;
            }

            int slice_len = end_idx - start_idx + 1;
            if (slice_len < 0) slice_len = 0;
            ObjArray* result = arr_new(slice_len > 0 ? slice_len : 1);
            if (unlikely(!result)) {
                runtime_error("切片数组内存分配失败", line);
                return -1;
            }
            for (int i = 0; i < slice_len; i++) {
                arr_write(result, i, arr->elements[start_idx + i]);
            }
            R(a) = val_obj((Object*)result);
            DISPATCH();
        }

        if (val_is_obj(obj_val) && val_as_obj(obj_val)->type == OBJ_STRING) {
            ObjString* s = (ObjString*)val_as_obj(obj_val);
            int arr_len = s->char_len;
            int start_idx = 0, end_idx = arr_len - 1;

            if (!val_is_null(start_val)) {
                if (unlikely(!val_is_num(start_val))) {
                    if (!throw_runtime_error("切片起始索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                start_idx = (int)value_to_double(start_val);
                if (start_idx < 0) start_idx = 0;
                if (start_idx > arr_len) start_idx = arr_len;
            }
            if (!val_is_null(end_val)) {
                if (unlikely(!val_is_num(end_val))) {
                    if (!throw_runtime_error("切片结束索引必须是数字", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                end_idx = (int)value_to_double(end_val);
                if (end_idx < 0) end_idx = 0;
                if (end_idx >= arr_len) end_idx = arr_len - 1;
            }

            int nchars = end_idx - start_idx + 1;
            if (nchars < 0) nchars = 0;
            if (nchars == 0) {
                R(a) = val_obj((Object*)str_new("", 0));
                DISPATCH();
            }
            int byte_start = utf8_char_offset(s->chars, s->len, start_idx);
            int byte_end = (start_idx + nchars >= s->char_len)
                               ? s->len
                               : utf8_char_offset(s->chars, s->len, start_idx + nchars);
            R(a) = val_obj((Object*)str_new(&s->chars[byte_start], byte_end - byte_start));
            DISPATCH();
        }

        if (!throw_runtime_error("切片: 对象必须是数组或字符串", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 迭代 ---
    // OP_ITER_GET A B C: R[A] = iterate(R[B], R[C])
    //   数组 → 元素；字符串 → 第 N 个字符（按码点）；字典/struct → 第 N 个键/字段名；
    //   数字 → 序号本身（for n to i 的遍历语义）
    LABEL(OP_ITER_GET): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj_val = R(b);
        Value idx_val = R(c);
        int line = GET_CURRENT_LINE();

        if (unlikely(!val_is_num(idx_val))) {
            if (!throw_runtime_error("ITER_GET 索引必须是数字", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        int index = (int)value_to_double(idx_val);

        if (val_is_num(obj_val)) {
            R(a) = val_int(index);
            DISPATCH();
        }
        if (unlikely(!val_is_obj(obj_val))) {
            if (!throw_runtime_error("ITER_GET 需要对象或数字类型", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        switch (val_as_obj(obj_val)->type) {
            case OBJ_ARRAY: {
                ObjArray* arr = (ObjArray*)val_as_obj(obj_val);
                if (likely((uint64_t)index < (uint64_t)arr->count)) {
                    R(a) = arr->elements[index];
                    DISPATCH();
                }
                if (!throw_runtime_error("数组迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            case OBJ_STRING: {
                ObjString* str = (ObjString*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= str->char_len)) {
                    if (!throw_runtime_error("字符串迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                int byte_offset = utf8_char_offset(str->chars, str->len, index);
                int char_bytes = utf8_char_byte_len(str->chars, str->len, byte_offset);
                ObjString* result = str_new(&str->chars[byte_offset], char_bytes);
                R(a) = val_obj((Object*)result);
                DISPATCH();
            }
            case OBJ_DICT: {
                ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= dict->order_count)) {
                    if (!throw_runtime_error("字典键迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = dict_get_key_by_index(dict, index);
                DISPATCH();
            }
            case OBJ_ENUM_DEF: {
                ObjEnumDef* edef = (ObjEnumDef*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= edef->member_count)) {
                    if (!throw_runtime_error("枚举迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = val_int_safe(edef->members[index].value);
                DISPATCH();
            }
            case OBJ_STRUCT: {
                ObjStruct* so = (ObjStruct*)val_as_obj(obj_val);
                if (unlikely(index < 0 || index >= so->def->field_count)) {
                    if (!throw_runtime_error("结构体字段迭代索引越界", line)) return -1;
                    frame = &vm.frames[vm.frame_cnt - 1];
                    DISPATCH();
                }
                R(a) = val_obj((Object*)str_copy(so->def->fields[index].name,
                                                 (int)strlen(so->def->fields[index].name)));
                DISPATCH();
            }
            default:
                if (!throw_runtime_error("ITER_GET 不支持的类型", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
        }
    }
    // OP_ITER_GET_VALUE A B C: R[A] = 第 R[C] 个键对应的值（字典 / struct 字段值）
    LABEL(OP_ITER_GET_VALUE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value obj_val = R(b);
        Value idx_val = R(c);
        int line = GET_CURRENT_LINE();

        if (unlikely(!val_is_num(idx_val))) {
            if (!throw_runtime_error("ITER_GET_VALUE 索引必须是数字", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        int index = (int)value_to_double(idx_val);

        if (unlikely(!val_is_obj(obj_val))) {
            if (!throw_runtime_error("ITER_GET_VALUE 需要对象类型", line)) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        if (val_as_obj(obj_val)->type == OBJ_DICT) {
            ObjDict* dict = (ObjDict*)val_as_obj(obj_val);
            if (unlikely(index < 0 || index >= dict->order_count)) {
                if (!throw_runtime_error("字典值迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = dict_get_value_by_index(dict, index);
            DISPATCH();
        }
        if (val_as_obj(obj_val)->type == OBJ_STRUCT) {
            ObjStruct* so = (ObjStruct*)val_as_obj(obj_val);
            if (unlikely(index < 0 || index >= so->def->field_count)) {
                if (!throw_runtime_error("结构体字段值迭代索引越界", line)) return -1;
                frame = &vm.frames[vm.frame_cnt - 1];
                DISPATCH();
            }
            R(a) = struct_get_field(so, index);
            DISPATCH();
        }
        if (!throw_runtime_error("ITER_GET_VALUE 只能用于字典或结构体", line)) return -1;
        frame = &vm.frames[vm.frame_cnt - 1];
        DISPATCH();
    }

    // --- 范围 ---
    // OP_RANGE A B C: R[A] = Range(R[B], R[C])（左闭右闭：parser 只产出 inclusive=1）
    LABEL(OP_RANGE): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value start = R(b), end = R(c);
        int64_t start_val = 0, end_val = 0;
        if (val_is_int(start)) start_val = val_as_int(start);
        else if (val_is_float(start)) start_val = (int64_t)value_to_double(start);
        if (val_is_int(end)) end_val = val_as_int(end);
        else if (val_is_float(end)) end_val = (int64_t)value_to_double(end);
        ObjRange* range = range_new(start_val, end_val, 1);
        if (unlikely(!range)) {
            if (!throw_runtime_error("range 创建失败", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }
        R(a) = val_obj((Object*)range);
        DISPATCH();
    }

    // --- in ---
    // OP_IN A B C: R[A] = (R[B] in R[C])
    LABEL(OP_IN): {
        int a = READ_A(), b = READ_B(), c = READ_C();
        Value item = R(b);
        Value collection = R(c);
        int found = 0;

        if (val_is_obj(collection)) {
            switch (val_as_obj(collection)->type) {
                case OBJ_ARRAY: {
                    ObjArray* arr = (ObjArray*)val_as_obj(collection);
                    for (int i = 0; i < arr->count; i++) {
                        Value elem = arr->elements[i];
                        if (val_get_type(item) != val_get_type(elem)) continue;
                        if (val_is_num(item) && val_is_num(elem)) {
                            if (value_to_double(item) == value_to_double(elem)) { found = 1; break; }
                        } else if (val_is_bool(item) && val_as_bool(item) == val_as_bool(elem)) {
                            found = 1; break;
                        } else if (val_is_string(item) && val_is_string(elem)) {
                            ObjString* istr = (ObjString*)val_as_obj(item);
                            ObjString* estr = (ObjString*)val_as_obj(elem);
                            if (istr->len == estr->len && strcmp(istr->chars, estr->chars) == 0) { found = 1; break; }
                        } else if (val_is_obj(item) && val_as_obj(item) == val_as_obj(elem)) {
                            found = 1; break;
                        } else if (val_is_null(item)) {
                            found = 1; break;
                        }
                    }
                    break;
                }
                case OBJ_DICT:
                    found = dict_has((ObjDict*)val_as_obj(collection), item);
                    break;
                case OBJ_STRING: {
                    ObjString* str = (ObjString*)val_as_obj(collection);
                    if (val_is_obj(item) && val_as_obj(item)->type == OBJ_STRING) {
                        ObjString* substr = (ObjString*)val_as_obj(item);
                        found = (strstr(str->chars, substr->chars) != NULL);
                    }
                    break;
                }
                case OBJ_RANGE: {
                    ObjRange* range = (ObjRange*)val_as_obj(collection);
                    if (val_is_num(item)) found = range_contains(range, (int64_t)value_to_double(item));
                    break;
                }
                default:
                    break;
            }
        }

        R(a) = val_bool(found);
        DISPATCH();
    }

    // --- 异常处理 ---
    // OP_TRY Bx: Bx = catch 块相对本指令起始的字节偏移
