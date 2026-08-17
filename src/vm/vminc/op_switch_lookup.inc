// ============================================================================
// Switch 查找操作码 (OP_SWITCH_LOOKUP)
// 通用二分查找优化：支持 int、float、string case 值
// 编码: const_idx(2) case_count(2) default_offset(4) [body_offset(4)]...
// 常量表存储排序后的 Value 数组（按类型分组排序）
// 栈: [switch_value] -> [] (弹出 switch 值，跳转到匹配的 body 或 default)
// ============================================================================

OPCODE(OP_SWITCH_LOOKUP) {
    uint16_t const_idx = READ_SHORT();
    uint16_t case_count = READ_SHORT();
    int32_t default_offset = READ_INT32();
    // frame->ip 现在指向 offset 表的第一个 entry

    Value switch_val = vm_stack_pop(&vm);

    int matched_index = -1;

    Value arr_val = frame->chunk->constants[const_idx];
    if (val_is_obj(arr_val) && val_as_obj(arr_val)->type == OBJ_ARRAY) {
        ObjArray* arr = (ObjArray*)val_as_obj(arr_val);

        // 根据类型选择比较策略
        if (val_is_int(switch_val)) {
            // 整数二分查找
            int64_t target = val_as_int(switch_val);
            int lo = 0, hi = case_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                Value cv = arr->elements[mid];
                int64_t cmp_val;
                if (val_is_int(cv)) {
                    cmp_val = val_as_int(cv);
                } else if (val_is_bigint(cv)) {
                    int c = compare_values(switch_val, cv);
                    if (c == 0) { matched_index = mid; break; }
                    if (c < 0) { hi = mid - 1; }
                    else { lo = mid + 1; }
                    continue;
                } else {
                    break;  // 类型不匹配，回退
                }
                if (cmp_val == target) { matched_index = mid; break; }
                else if (cmp_val < target) { lo = mid + 1; }
                else { hi = mid - 1; }
            }
        } else if (val_is_bigint(switch_val)) {
            // BigInt 二分查找
            int lo = 0, hi = case_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                Value cv = arr->elements[mid];
                if (val_is_int(cv) || val_is_bigint(cv)) {
                    int c = compare_values(switch_val, cv);
                    if (c == 0) { matched_index = mid; break; }
                    if (c < 0) { hi = mid - 1; }
                    else { lo = mid + 1; }
                } else {
                    break;
                }
            }
        } else if (val_is_float(switch_val)) {
            // 浮点数二分查找
            double target = val_as_double(switch_val);
            int lo = 0, hi = case_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                Value cv = arr->elements[mid];
                if (val_is_float(cv)) {
                    double cmp_val = val_as_double(cv);
                    if (cmp_val == target) { matched_index = mid; break; }
                    else if (cmp_val < target) { lo = mid + 1; }
                    else { hi = mid - 1; }
                } else {
                    break;
                }
            }
        } else if (val_is_string(switch_val)) {
            // 字符串二分查找（按字典序）
            ObjString* target = (ObjString*)val_as_obj(switch_val);
            int lo = 0, hi = case_count - 1;
            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                Value cv = arr->elements[mid];
                if (val_is_string(cv)) {
                    ObjString* cs = (ObjString*)val_as_obj(cv);
                    // 先比 hash（快速路径）
                    if (cs->hash == target->hash && cs->len == target->len) {
                        if (memcmp(cs->chars, target->chars, cs->len) == 0) {
                            matched_index = mid;
                            break;
                        }
                    }
                    // hash 不等，按字典序比较
                    int min_len = cs->len < target->len ? cs->len : target->len;
                    int cmp = memcmp(cs->chars, target->chars, min_len);
                    if (cmp == 0) {
                        cmp = cs->len - target->len;
                    }
                    if (cmp == 0) { matched_index = mid; break; }
                    else if (cmp < 0) { lo = mid + 1; }
                    else { hi = mid - 1; }
                } else {
                    break;
                }
            }
        }
    }

    // 读取匹配的 offset，或使用 default
    if (matched_index >= 0) {
        uint8_t* offset_ptr = frame->ip + matched_index * 4;
        int32_t offset = (int32_t)((offset_ptr[0] << 24) | (offset_ptr[1] << 16) | (offset_ptr[2] << 8) | offset_ptr[3]);
        frame->ip += case_count * 4;  // 跳过 offset 表
        frame->ip += offset;
    } else {
        frame->ip += case_count * 4;  // 跳过 offset 表
        frame->ip += default_offset;
    }

    DISPATCH;
}
