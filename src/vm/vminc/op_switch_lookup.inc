// ============================================================================
// Switch 查找操作码 (OP_SWITCH_LOOKUP)
// 整数 switch 的二分查找优化
// 编码: const_idx(2) case_count(2) default_offset(4) [body_offset(4)]...
// 常量表存储排序后的 int64 case 值数组
// 栈: [switch_value] -> [] (弹出 switch 值，跳转到匹配的 body 或 default)
// ============================================================================

OPCODE(OP_SWITCH_LOOKUP) {
    uint16_t const_idx = READ_SHORT();
    uint16_t case_count = READ_SHORT();
    int32_t default_offset = READ_INT32();
    // frame->ip 现在指向 offset 表的第一个 entry

    Value switch_val = vm_stack_pop(&vm);

    int matched_index = -1;

    if (likely(val_is_int(switch_val))) {
        int64_t target = val_as_int(switch_val);
        Value arr_val = frame->chunk->constants[const_idx];

        if (val_is_obj(arr_val) && val_as_obj(arr_val)->type == OBJ_ARRAY) {
            ObjArray* arr = (ObjArray*)val_as_obj(arr_val);
            int lo = 0;
            int hi = case_count - 1;

            while (lo <= hi) {
                int mid = lo + (hi - lo) / 2;
                Value cv = arr->elements[mid];
                if (val_is_int(cv)) {
                    int64_t cmp_val = val_as_int(cv);
                    if (cmp_val == target) {
                        matched_index = mid;
                        break;
                    } else if (cmp_val < target) {
                        lo = mid + 1;
                    } else {
                        hi = mid - 1;
                    }
                } else if (val_is_bigint(cv)) {
                    int c = compare_values(switch_val, cv);
                    if (c == 0) { matched_index = mid; break; }
                    if (c < 0) { hi = mid - 1; }
                    else { lo = mid + 1; }
                } else {
                    break;
                }
            }
        }
    }

    // 读取匹配的 offset，或使用 default
    if (matched_index >= 0) {
        // offset 表紧跟在当前 ip 后，每个 4 字节
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
