    LABEL(OP_SWITCH_LOOKUP): DISPATCH();

    // --- for 数值循环（8 字节定长网格；与栈式 op_for_loop.inc 语义一致）---
    // OP_FOR_PREP: op + A(start_slot) + B(end_slot) + C(step_slot)
    //              + [var_slot:u8][inclusive:u8][sBx:u16]           （共 8 字节）
    //   局部变量槽位号即寄存器号（每个 slot 独立，不要求连续）
    LABEL(OP_FOR_PREP): {
        int start_slot = READ_A();
        int end_slot = READ_B();
        int step_slot = READ_C();
        int var_slot = frame->ip[0];
        int inclusive = frame->ip[1];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        if (unlikely(start_slot >= frame->local_count || end_slot >= frame->local_count ||
                     step_slot >= frame->local_count || var_slot >= frame->local_count ||
                     start_slot < 0 || end_slot < 0 || step_slot < 0 || var_slot < 0)) {
            if (!throw_runtime_error("for 循环槽位越界", GET_CURRENT_LINE())) return -1;
            frame = &vm.frames[vm.frame_cnt - 1];
            DISPATCH();
        }

        Value start_val = frame->locals[start_slot];
        Value end_val = frame->locals[end_slot];
        Value step_val = frame->locals[step_slot];
        frame->locals[var_slot] = start_val;

        // ★ 计数制快路径（借鉴 Lua 5.5 的 forprep，lvm.c:214-265）：
        //   三者全是 int48 时，入口就把**迭代次数**算好、写进 **end 槽**。
        //   为什么可以占用 end 槽：它是 codegen 给本循环分配的临时槽，循环体内只有
        //   OP_FOR_LOOP 读它（step 槽同理）⇒ 复用安全（Lua 也正是复用 ra 存 count）。
        //   之后每轮只做「减一 + 判零 + 加步长」，**不再读上界、不再判 step 正负**。
        //   排他式折成含上界：整数序列下 `x < end` ⟺ `x <= end-1`（下行 `x > end`
        //   ⟺ `x >= end+1`），于是两种方向共用同一个计数公式。
        if (likely(val_is_int(start_val) && val_is_int(end_val) && val_is_int(step_val))) {
            int64_t start = val_as_int(start_val);
            int64_t end = val_as_int(end_val);
            int64_t step = val_as_int(step_val);
            // ⚠ 口径与 Lua 完全一致（lvm.c:227-237）：进循环体**那一轮不算**在 count 里，
            //   count = 首轮之外的**额外**迭代次数 ⇒ 循环回边处 `if (count > 0) { count--; ... }`。
            //   count < 0 表示"首轮条件就不成立"⇒ 跳过整段（step == 0 同此）。
            int64_t count = -1;
            if (step > 0) {
                int64_t limit = inclusive ? end : end - 1;   // 排他 ⟺ 整数下 x <= end-1
                if (limit >= start) count = (limit - start) / step;
            } else if (step < 0) {
                int64_t limit = inclusive ? end : end + 1;   // 排他 ⟺ 整数下 x >= end+1
                if (start >= limit) count = (start - limit) / (-step);
            }
            frame->locals[end_slot] = val_int(count);   // 计数制标记：end 槽改存 count
            if (count < 0) frame->ip += sbx;            // 首轮都不成立 ⇒ 跳过循环体
            DISPATCH();
        }

        // 非全 int：保持通用（double / bigint）语义。但必须把 end 槽**规范成非 int**，
        // 否则 FOR_LOOP 会把它误判成计数制的 count。语义等价：通用路径本来就把上界
        // 当 double（或 bigint）用，而 int48 转 double **无精度损失**。
        if (val_is_bigint(start_val) || val_is_bigint(step_val) || val_is_bigint(end_val)) {
            if (val_is_int(end_val))
                frame->locals[end_slot] = val_obj((Object*)bigint_from_int64(val_as_int(end_val)));
        } else if (!val_is_float(end_val)) {
            frame->locals[end_slot] = val_float(value_to_double(end_val));
        }

        double start_num = value_to_double(start_val);
        double end_num = value_to_double(end_val);
        double step_num = value_to_double(step_val);

        int should_run = 0;
        if (step_num > 0) {
            should_run = inclusive ? (start_num <= end_num) : (start_num < end_num);
        } else if (step_num < 0) {
            should_run = inclusive ? (start_num >= end_num) : (start_num > end_num);
        }
        // step == 0 时不执行

        if (!should_run) {
            frame->ip += sbx;
        }
        DISPATCH();
    }
    // OP_FOR_LOOP: op + A(var_slot) + B(step_slot) + C(end_slot)
    //              + [inclusive:u8][pad:u8][sBx:u16]                （共 8 字节）
    LABEL(OP_FOR_LOOP): {
        int var_slot = READ_A();
        int step_slot = READ_B();
        int end_slot = READ_C();
        int inclusive = frame->ip[0];
        int sbx = (((int)frame->ip[2] << 8) | (int)frame->ip[3]) - 32768;
        frame->ip += 4;

        // 槽位越界检查：OP_FOR_PREP 已就同一批槽位号检查过（槽位号全程不变）⇒
        // 循环内不再重复（原先每轮 4 次比较 + 分支）。

        /* 回边是天然 GC 安全点 */
        if (unlikely(vm.gc_force_request)) {
            vm.gc_force_request = 0;
            gc_force_collect();
        }

        Value end_or_count = frame->locals[end_slot];

        // ★ 计数制（见 OP_FOR_PREP）：end 槽是 int ⇒ 它就是**剩余迭代次数**。
        //   每轮只做「减一 + 判零 + 加步长」，不读上界、不判 step 正负、不比边界。
        if (likely(val_is_int(end_or_count))) {
            int64_t count = val_as_int(end_or_count);
            if (count > 0) {
                frame->locals[end_slot] = val_int(count - 1);
                Value lv = frame->locals[var_slot];
                Value sv = frame->locals[step_slot];
                if (likely(val_is_int(lv) && val_is_int(sv))) {
                    int64_t nv = val_as_int(lv) + val_as_int(sv);
                    frame->locals[var_slot] = (nv >= INT48_MIN && nv <= INT48_MAX)
                                                  ? val_int(nv)
                                                  : val_obj((Object*)bigint_from_int64(nv));
                } else if (unlikely(val_is_bigint(lv) || val_is_bigint(sv))) {
                    // 循环变量已提升为 bigint（int48 溢出）⇒ 后续用 bigint 加
                    frame->locals[var_slot] = bigint_add(get_bigint_operand(lv),
                                                         get_bigint_operand(sv));
                } else {
                    // 循环体把变量改成 float ⇒ 值语义与通用路径一致（次数仍由 count 定，
                    // 与 Lua 5.5 同口径：控制变量的赋值不影响迭代次数）
                    frame->locals[var_slot] = val_num(value_to_double(lv) + value_to_double(sv));
                }
                frame->ip += sbx;
            }
            DISPATCH();
        }

        Value loop_var = frame->locals[var_slot];
        Value step_val = frame->locals[step_slot];
        Value end_val = frame->locals[end_slot];

        if (likely(val_is_int(loop_var) && val_is_int(step_val) && val_is_int(end_val))) {
            int64_t loop_num = val_as_int(loop_var) + val_as_int(step_val);
            int64_t step_num = val_as_int(step_val);
            int64_t end_num = val_as_int(end_val);
            frame->locals[var_slot] = (loop_num >= INT48_MIN && loop_num <= INT48_MAX)
                                          ? val_int(loop_num)
                                          : val_obj((Object*)bigint_from_int64(loop_num));
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        } else if (unlikely(val_is_bigint(loop_var) || val_is_bigint(step_val) || val_is_bigint(end_val))) {
            ObjBigInt* lb = get_bigint_operand(loop_var);
            ObjBigInt* sb = get_bigint_operand(step_val);
            ObjBigInt* eb = get_bigint_operand(end_val);
            Value nv = bigint_add(lb, sb);
            frame->locals[var_slot] = nv;
            int cmp = bigint_compare(get_bigint_operand(nv), eb);
            int step_cmp = bigint_compare(sb, bigint_from_int64(0));
            int cont = 0;
            if (step_cmp > 0) cont = inclusive ? (cmp <= 0) : (cmp < 0);
            else if (step_cmp < 0) cont = inclusive ? (cmp >= 0) : (cmp > 0);
            if (cont) frame->ip += sbx;
        } else {
            double loop_num = value_to_double(loop_var) + value_to_double(step_val);
            double step_num = value_to_double(step_val);
            double end_num = value_to_double(end_val);
            frame->locals[var_slot] = val_num(loop_num);
            int cont = 0;
            if (step_num > 0) cont = inclusive ? (loop_num <= end_num) : (loop_num < end_num);
            else if (step_num < 0) cont = inclusive ? (loop_num >= end_num) : (loop_num > end_num);
            if (cont) frame->ip += sbx;
        }
        DISPATCH();
    }

    // --- 扩展指令 ---
    LABEL(OP_EXTRAARG): DISPATCH();
    LABEL(OP_EXTEND): DISPATCH();

    #if !VM_USE_JUMPTABLE
                default:
                    // 未知 opcode
                    goto vm_exit;
            } // switch
        } // for
    #endif

vm_exit:
    #undef R
    #undef READ_OPCODE
    #undef READ_A
    #undef READ_B
    #undef READ_C
    #undef READ_Bx
    #undef READ_sBx
    #undef READ_sJ
    #undef READ_Ax
    #undef GET_CURRENT_LINE
    #undef ADVANCE
    #undef DISPATCH
    #undef LABEL
    #undef REG_FINISH_RETURN
    #undef READ_CONST_IDX
    return 0;
