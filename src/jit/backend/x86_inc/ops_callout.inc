/* ============================================================================
 * ops_callout.inc - Callout cases (ACC_FIELDS, INVOKE_METHOD, GET_FIELD_FAST, CALL_GLOBAL_FUNC[_TYPED])
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Callout: OP_ACC_FIELDS (struct field accumulation) ---- */
            case OP_ACC_FIELDS: {
                /* Pop struct obj, convert to NaN-boxed, save to tmp */
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* struct obj */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* obj (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)ip[1]);  /* count (2nd arg) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)(ip + 2));  /* field_indices ptr (3rd) */
                EMIT_CALL(jit_callout_acc_fields);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                break;
            }
            /* ---- Callout: OP_INVOKE_METHOD (struct method call, VM re-entry) ---- */
            case OP_INVOKE_METHOD: {
                int arg_count = rd_short(ip + 3);
                /* 返回值个数：编译期按方法名解析（scan 阶段已保证可解析，
                 * 这里再兜一次，绝不允许按错误的个数生成代码）。多返回值
                 * 方法（如 Font.measureString → [float, float]）时，
                 * callout 已把前 (ret_count-1) 个结果写回 vstack 内存，
                 * RAX = 最后一个结果（新 TOS）。 */
                int ret_count = jit_resolve_method_ret_count(cur_chunk, rd_short(ip + 1));
                if (ret_count <= 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] INVOKE_METHOD ret_count 无法确定 at bc_off=%d\n", bc_off);
                    return 0;
                }
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into arg1 (1st argument register). */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)ip);  /* bytecode ip (3rd) */
                /* 4th arg 用 cur_chunk：内联 callee 时 ip 指向 callee 的字节码，
                 * 方法名常量索引必须到 callee 的常量表里取（其余 callout 同理）。 */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)cur_chunk);  /* chunk (4th) */
                EMIT_CALL(jit_callout_invoke_method);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop (arg_count - ret_count + 1) 个实参槽：剩下的
                 * (ret_count - 1) 个槽由 callout 写入额外的返回值，
                 * 与 OP_CALL_GLOBAL_FUNC 的多返回值处理完全一致。 */
                int pop_bytes = (arg_count - ret_count + 1) * 8;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - ret_count);
                break;
            }

            /* ---- Callout: OP_GET_FIELD_FAST (read local struct + get field) ---- */
            case OP_GET_FIELD_FAST: {
                uint16_t slot = rd_short(ip + 1);
                uint8_t field_idx = ip[3];
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                /* Load local (struct obj raw) from scratch area */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);  /* obj_val (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)field_idx);  /* field_idx (2nd arg) */
                EMIT_CALL(jit_callout_get_field_fast);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack++;
                break;
            }

            /* ---- Callout: OP_CALL_GLOBAL_FUNC / OP_CALL_GLOBAL_FUNC_TYPED ---- */
            case OP_CALL_GLOBAL_FUNC:
            case OP_CALL_GLOBAL_FUNC_TYPED: {
                uint16_t func_slot = rd_short(ip + 1);
                int arg_count = rd_short(ip + 3);

                /* ---- Check if this call site is marked for inlining ---- */
                if (op == OP_CALL_GLOBAL_FUNC_TYPED) {
                    int inline_idx = FIND_INLINE_SITE(bc_off);
                    if (inline_idx >= 0) {
                        const InlineSite* is = &sr->inline_sites[inline_idx];
                        /* Spill TOS to memory stack */
                        TOS_SPILL();
                        /* Pop args from vstack and store to callee locals.
                         * Args are on the stack in order: last arg = TOS.
                         * Callee local slot 0 = first arg, slot (arg_count-1) = last arg.
                         * Pop in reverse: last arg first (slot arg_count-1), ..., first arg last (slot 0). */
                        for (int i = arg_count - 1; i >= 0; i--) {
                            /* pop rax */
                            emit_byte(cb, 0x58);  /* pop rax */
                            /* store to callee local slot i */
                            int si = is->callee_local_map[i];
                            if (si < 0) {
                                /* shouldn't happen: scan pre-mapped all locals */
                                if (jit_debug_on())
                                    fprintf(stderr, "[JIT-DEBUG] inline FAIL: unmapped callee slot %d\n", i);
                                return 0;
                            }
                            int disp = scratch_disp(si);
                            if (disp >= -128 && disp <= 127)
                                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                            else
                                emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                        }
                        vstack -= arg_count;

                        /* Save caller context and switch to callee */
                        InlineFrame* f = &inline_frames[inline_depth];
                        f->ip = ip + size;  /* next instruction after the call */
                        f->end = end;
                        f->local_map = cur_local_map;
                        f->chunk = cur_chunk;
                        f->bc_off = bc_off + size;
                        f->vstack = vstack + arg_count;  /* vstack before args were popped */
                        f->tos_live = 0;  /* TOS already spilled */
                        f->callee_ret_count = is->ret_count;
                        f->callee_arg_count = arg_count;
                        inline_depth++;

                        /* Switch to callee bytecode.
                         * Use a large bc_off base to avoid offmap conflicts
                         * with the caller's bytecode offsets. */
                        ip = is->callee_chunk->code;
                        end = ip + is->callee_body_size;
                        cur_local_map = is->callee_local_map;
                        cur_chunk = is->callee_chunk;
                        bc_off = 0x10000 * inline_depth;
                        vstack = 0;
                        tos_live = 0;

                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline ENTER: func_slot=%d arg_count=%d ret_count=%d\n",
                                    func_slot, arg_count, is->ret_count);
                        continue;  /* skip normal ip+=size advance */
                    }
                }

                /* ---- Non-inline: callout path (original code) ---- */
                int ret_count = 1;
                if (op == OP_CALL_GLOBAL_FUNC_TYPED && ctx->vm_ptr) {
                    if (func_slot < ctx->vm_ptr->global_func_capacity) {
                        Value callee = ctx->vm_ptr->global_funcs[func_slot];
                        if (val_is_obj(callee)) {
                            Object* obj2 = val_as_obj(callee);
                            ObjFunction* func2 = NULL;
                            if (obj2->type == OBJ_CLOSURE)
                                func2 = ((ObjClosure*)obj2)->function;
                            else if (obj2->type == OBJ_FUNCTION)
                                func2 = (ObjFunction*)obj2;
                            if (func2 && func2->return_count > 1)
                                ret_count = func2->return_count;
                        }
                    }
                }
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into arg1 (1st argument register). */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)func_slot);   /* func_slot (3rd) */
                EMIT_CALL(jit_callout_global_func);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out.
                 * RAX holds the callout return value; save it, check flag, restore. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);  /* RAX = *flag */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);  /* bailout */
                }
                /* Clear flag */
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);  /* *flag = 0 */
                /* Restore callout return value */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop (arg_count - ret_count + 1) args from JIT virtual stack.
                 * The remaining (ret_count - 1) slots hold the extra return
                 * values that the callout wrote to vstack_top. */
                int pop_bytes = (arg_count - ret_count + 1) * 8;
                if (pop_bytes <= 0) {
                    /* ret_count >= arg_count + 1: no pop needed (or even push) */
                } else if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - ret_count);
                break;
            }

            /* ---- Callout: OP_ARRAY (array literal [e1..eN]) ---- */
            case OP_ARRAY: {
                uint16_t count = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)count);  /* count (2nd) */
                EMIT_CALL(jit_callout_array_new);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop count elements from JIT virtual stack */
                int pop_bytes = count * 8;
                if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (count - 1);
                break;
            }

            /* ---- Callout: OP_CALL_NATIVE (direct native call) ----
             * The native function pointer is resolved at compile time and
             * embedded as an immediate: the runtime callout then calls it
             * directly, avoiding a strcmp scan of the global native table
             * on every execution (the interpreter's IC cache does the same). */
            case OP_CALL_NATIVE: {
                uint16_t name_idx = rd_short(ip + 1);
                uint16_t arg_count = rd_short(ip + 3);
                /* Compile-time resolution: name constant -> ObjNative* */
                Value name_val = cur_chunk->constants[name_idx];
                ObjNative* native = NULL;
                if (val_is_obj(name_val) && val_as_obj(name_val)->type == OBJ_STRING) {
                    native = native_find_function(((ObjString*)val_as_obj(name_val))->chars);
                }
                if (!native) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] call_native: unresolved native at compile time\n");
                    return 0;  /* bail out → interpreter keeps semantics */
                }
                /* ---- 内联快速路径：_int / _float（单参数数值转换）----
                 * 这两个原语在热循环里极其常见（如 _int(x) 取像素下标），而
                 * 通用 callout 每次要付 ~23ns 的装箱/调用开销。这里直接生成
                 * CVTTSD2SI / CVTSI2SD（语义与 types.c 的 native_to_int/_float
                 * 一致：int 原样、float 按 (int) 截断 / 提升为 double）；
                 * 遇到 NaN-boxed 的其它类型（bool/null/string/bigint/ptr）
                 * 则 bailout 交回解释器，保证语义与报错信息不变。 */
                if (arg_count == 1 && native->name &&
                    (strcmp(native->name, "_int") == 0 || strcmp(native->name, "_float") == 0)) {
                    int is_int_cast = (strcmp(native->name, "_int") == 0);
                    TOS_CONSUME_RAX();          /* RAX = 参数原始值（int48 或裸 double） */
                    /* int48 判定：mov r8,rax; sar r8,47; inc r8; cmp r8,1; ja .not_int */
                    emit_mov_rr(cb, JIT_R8, JIT_RAX);
                    emit_sar_imm(cb, JIT_R8, 47);
                    emit_inc_reg(cb, JIT_R8);
                    emit_cmp_reg_imm8(cb, JIT_R8, 1);
                    int not_int_p = emit_jcc(cb, 0x87);
                    if (is_int_cast) {
                        /* _int(int) → 原样 */
                        int done0 = emit_jmp(cb);
                        patch_rel32(cb, not_int_p, cb->len);
                        /* 非 int48：先认 bool —— true→1 / false→0，与 types.c 的
                         * native_to_int 一致；判据用**全值精确比较**，与 VM 的
                         * val_is_bool（v == TRUE_VAL || v == FALSE_VAL）保持一致。 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_true = emit_jcc(cb, 0x84);   /* JE → .bool_true */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_false = emit_jcc(cb, 0x84);  /* JE → .bool_false */
                        /* null → 0（同 types.c native_to_int 的 VAL_NULL 分支 val_int(0)），
                         * 与 false 同路；判据同样是全值精确比较 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_null = emit_jcc(cb, 0x84);   /* JE → 与 false 同路（0） */
                        /* 其余 NaN-boxed（string/bigint/ptr）→ bailout 交解释器 */
                        emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int bail_p = emit_jcc(cb, 0x83);        /* jae（无符号）→ bailout */
                        patch_add(ctx, bail_p, -1, 0);
                        /* 裸 double → (int) 截断：movq xmm0,rax; cvttsd2si eax,xmm0; movsxd rax,eax */
                        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                        emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                        emit_byte(cb, 0xF2); emit_byte(cb, 0x0F); emit_byte(cb, 0x2C);
                        emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* cvttsd2si eax, xmm0 */
                        emit_byte(cb, 0x48); emit_byte(cb, 0x63); emit_byte(cb, 0xC0); /* movsxd rax,eax */
                        int int_done_jmp = emit_jmp(cb);        /* 跳过下面两个 bool 分支 */
                        /* .bool_true → 1 */
                        patch_rel32(cb, int_is_true, cb->len);
                        emit_mov_reg_imm64(cb, JIT_RAX, 1);
                        int int_true_done = emit_jmp(cb);
                        /* .bool_false / null → 0 */
                        patch_rel32(cb, int_is_false, cb->len);
                        patch_rel32(cb, int_is_null, cb->len);
                        emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                        patch_rel32(cb, int_true_done, cb->len);
                        patch_rel32(cb, int_done_jmp, cb->len);
                        patch_rel32(cb, done0, cb->len);
                    } else {
                        /* _float(int) → cvtsi2sd xmm0,rax; movq rax,xmm0 */
                        emit_cvtsi2sd(cb, 0, JIT_RAX);
                        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                        emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                        int done0 = emit_jmp(cb);
                        patch_rel32(cb, not_int_p, cb->len);
                        /* 非 int48：先认 bool —— true→1.0 / false→0.0，与 types.c 的
                         * native_to_float 一致；判定同 VM 的 val_is_bool：全值精确比较。 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_true = emit_jcc(cb, 0x84);   /* JE → .bool_true */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_false = emit_jcc(cb, 0x84);  /* JE → .bool_false */
                        /* null → 0.0（同 types.c native_to_float 的 VAL_NULL 分支），
                         * 与 false 同路；判据同样是全值精确比较 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_null = emit_jcc(cb, 0x84);   /* JE → 与 false 同路（0.0） */
                        /* 其余 NaN-boxed → bailout；裸 double 原样保留 */
                        emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int bail_p = emit_jcc(cb, 0x83);
                        patch_add(ctx, bail_p, -1, 0);
                        int flt_done_jmp = emit_jmp(cb);         /* 跳过下面两个 bool 分支 */
                        /* .bool_true → 1.0（裸 double 位模式） */
                        patch_rel32(cb, flt_is_true, cb->len);
                        emit_mov_reg_imm64(cb, JIT_RAX, 0x3FF0000000000000ULL);
                        int flt_true_done = emit_jmp(cb);
                        /* .bool_false / null → 0.0（位模式全 0） */
                        patch_rel32(cb, flt_is_false, cb->len);
                        patch_rel32(cb, flt_is_null, cb->len);
                        emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                        patch_rel32(cb, flt_true_done, cb->len);
                        patch_rel32(cb, flt_done_jmp, cb->len);
                        patch_rel32(cb, done0, cb->len);
                    }
                    TOS_PRODUCE();
                    break;                      /* arg_count == 1 → vstack 不变 */
                }
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)(uintptr_t)native);  /* ObjNative* (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)arg_count);  /* arg_count (3rd) */
                EMIT_CALL(jit_callout_call_native);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop ALL arg_count args: the callout reads the args from the
                 * vstack and returns its result in RAX, which becomes the new
                 * TOS (tos_live=1). Physical stack must shrink by arg_count
                 * slots to stay consistent with vstack -= (arg_count - 1). */
                int pop_bytes = arg_count * 8;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

            /* ---- Callout: OP_GET_PROPERTY (obj.name / obj.name(args) peephole) ---- */
            case OP_GET_PROPERTY: {
                uint16_t name_idx = rd_short(ip + 1);
                /* Peephole: GET_PROPERTY + OP_CALL(arg_count16) merged in scan;
                 * mirror the same condition here (end = current body end). */
                int merged = (ip + 6 <= end && ip[3] == OP_CALL);
                uint16_t merged_argc = merged ? rd_short(ip + 4) : 0xFFFF;
                /* 窥孔合并且已被 scan 确认：codegen 也要连同 OP_CALL 一起消费 */
                if (merged) size = 6;
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)name_idx);         /* name const (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)merged_argc);      /* call args / 0xFFFF (3rd) */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)cur_chunk);  /* chunk (4th) */
                EMIT_CALL(jit_callout_get_property);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Merged call: pop receiver + args, push result → net -arg_count.
                 * Standalone: receiver replaced by value → no pop. */
                int pop_bytes = merged ? (merged_argc + 1) * 8 : 0;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                if (merged) vstack -= merged_argc;
                break;
            }

            /* ---- Callout: OP_STRUCT_INIT (struct literal, e.g. Hit{...}) ----
             * ip: op(1) + name_const(2) + arg_count(1) + generic_count(1) +
             *     field_indices(arg_count bytes)。scan 已拒绝泛型
             *     (generic_count>0)，此处仅处理非泛型路径。 */
            case OP_STRUCT_INIT: {
                uint8_t arg_count = ip[3];
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) BEFORE
                 * CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14. */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)rd_short(ip + 1)); /* name idx (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)arg_count);        /* arg_count (3rd) */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)ip);    /* ip (4th) */
                /* 5th param (chunk ptr): Windows x64 passes it as the first
                 * stack argument at [RSP+32]; System V passes it in R8. */
#ifdef _WIN32
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)(uintptr_t)cur_chunk);
                emit_mov_mem8_reg(cb, JIT_RSP, 32, JIT_RAX);
#else
                emit_mov_reg_imm64(cb, JIT_ARG5, (uint64_t)(uintptr_t)cur_chunk);
#endif
                EMIT_CALL(jit_callout_struct_init);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop arg_count arg slots from JIT virtual stack,
                 * push 1 result → net -(arg_count - 1) */
                int pop_bytes = arg_count * 8;
                if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

