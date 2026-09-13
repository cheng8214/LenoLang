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
            /* ---- Callout: OP_INVOKE_METHOD_TYPED (struct method call, VM re-entry) ---- */
            case OP_INVOKE_METHOD_TYPED: {
                int arg_count = rd_short(ip + 3);

                /* ---- 内联快路径：scan 阶段已解析出唯一的 struct 方法体，
                 * 把 callee 展开在本循环体内（省掉整条 callout + 函数级 JIT
                 * 的逐次序言），与 OP_CALL_GLOBAL_FUNC_TYPED 的内联同构。
                 * 接收者是动态分发 → 必须生成 def 守卫，不匹配即 bailout
                 * 交解释器（方法查找/报错语义完全由解释器负责）。 ---- */
                {
                    int method_inline_idx = FIND_INLINE_SITE(bc_off);
                    if (method_inline_idx >= 0 &&
                        sr->inline_sites[method_inline_idx].is_method) {
                        const InlineSite* mis = &sr->inline_sites[method_inline_idx];
                        TOS_SPILL();
                        /* 1) 倒序弹实参写入 callee locals：
                         *    JIT 栈 [receiver, arg1..argN]，栈顶是 argN，
                         *    于是 i=arg_count-1 → argN → slot arg_count-1，…，
                         *    i=0 → receiver → slot 0（= self）。 */
                        for (int i = arg_count - 1; i >= 0; i--) {
                            emit_byte(cb, 0x58);          /* pop rax */
                            int msi = mis->callee_local_map[i];
                            if (msi < 0) {
                                if (jit_debug_on())
                                    fprintf(stderr, "[JIT-DEBUG] method inline FAIL: unmapped callee slot %d\n", i);
                                return 0;
                            }
                            int md = scratch_disp(msi);
                            if (md >= -128 && md <= 127)
                                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)md, JIT_RAX);
                            else
                                emit_mov_mem32_reg(cb, JIT_RBP, md, JIT_RAX);
                        }
                        vstack -= arg_count;
                        /* 2) 接收者守卫：slot 0 必须是 mis->method_def 的实例 */
                        {
                            int msi0 = mis->callee_local_map[0];
                            int md0 = scratch_disp(msi0);
                            if (md0 >= -128 && md0 <= 127)
                                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)md0);
                            else
                                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, md0);
                            /* 2a) NaN-boxed 对象（top16 == 0xFFFC，用 SHR 比较） */
                            emit_mov_rr(cb, JIT_R8, JIT_RAX);
                            emit_shr_imm(cb, JIT_R8, 48);
                            emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_notobj = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_notobj, -1, 0);
                            emit_and_rr(cb, JIT_RAX, JIT_R10);      /* RAX = Object* */
                            /* 2b) obj->type == OBJ_STRUCT */
                            emit_cmp_mem8_imm8(cb, JIT_RAX, 0, (uint8_t)OBJ_STRUCT);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_notstruct = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_notstruct, -1, 0);
                            /* 2c) obj->def == 编译期解析出的唯一 def */
                            emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, def));
                            emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)mis->method_def);
                            emit_cmp_rr(cb, JIT_RDX, JIT_R8);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_otherdef = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_otherdef, -1, 0);
                        }
                        /* 3) 切到 callee：保存调用方上下文并进入内联帧
                         *    （bc_off 命名空间按**内联实例**编号，见 §8.26） */
                        InlineFrame* mf_frame = &inline_frames[inline_depth];
                        mf_frame->ip = ip + size;
                        mf_frame->end = end;
                        mf_frame->local_map = cur_local_map;
                        mf_frame->chunk = cur_chunk;
                        mf_frame->bc_off = bc_off + size;
                        mf_frame->vstack = vstack + arg_count;
                        mf_frame->tos_live = 0;
                        mf_frame->callee_ret_count = mis->ret_count;
                        mf_frame->callee_arg_count = arg_count;
                        inline_depth++;
                        ip = mis->callee_chunk->code;
                        end = ip + mis->callee_body_size;
                        cur_local_map = mis->callee_local_map;
                        cur_chunk = mis->callee_chunk;
                        bc_off = 0x10000 * (method_inline_idx + 1);
                        vstack = 0;
                        tos_live = 0;
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline(method) ENTER: arg_count=%d ret_count=%d callee_lc=%d\n",
                                    arg_count, mis->ret_count, mis->callee_local_count);
                        continue;
                    }
                }
                /* 返回值个数：编译期按方法名解析（scan 阶段已保证可解析，
                 * 这里再兜一次，绝不允许按错误的个数生成代码）。多返回值
                 * 方法（如 Font.measureString → [float, float]）时，
                 * callout 已把前 (ret_count-1) 个结果写回 vstack 内存，
                 * RAX = 最后一个结果（新 TOS）。 */
                int ret_count = 0;
                /* 静态类型名 → 直接定位 def/方法（确定性解析，不依赖"方法名唯一"） */
                {
                    ObjStructDef* td = NULL;
                    ObjFunction* tf = NULL;
                    (void)jit_resolve_method_typed(cur_chunk, rd_short(ip + 1),
                                                   rd_short(ip + 5), &td, &tf, &ret_count);
                }
                if (ret_count <= 0)
                    ret_count = jit_resolve_method_ret_count(cur_chunk, rd_short(ip + 1));
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

            /* ---- OP_GET_FIELD_FAST (读局部 struct 的字段) ----
             * 内联快路径：obj 是 NaN-boxed struct 且 field_idx < obj->def->field_count
             * 时，直接在机器码里读 field_values[field_idx] —— 这就是
             * struct_get_field 的全部语义（越界分支走慢路径保证一致）。
             * 慢路径保留原 callout（类型不匹配 → 错误文本原样）。
             * 与 OP_INDEX 读取快路径同一模板：所有判定都在产生副作用之前。 */
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
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);   /* 慢路径仍要原始 raw 值 */

                /* 1) NaN-boxed 对象（top16 == TAG_OBJ = 0xFFFC）。
                 *    必须用 SHR：SAR 会把 0xFFFC 符号扩展成 0xFFFF...FFFC，
                 *    比较恒不等 → 快路径变死代码（§8.29 同款坑）。 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int gf_not_obj = emit_jcc(cb, 0x85);          /* JNE → 慢路径 */
                emit_and_rr(cb, JIT_RAX, JIT_R10);            /* RAX = Object* */
                /* 2) obj->type == OBJ_STRUCT（type 是 Object 首字段，偏移 0） */
                emit_cmp_mem8_imm8(cb, JIT_RAX, 0, (uint8_t)OBJ_STRUCT);
                int gf_not_struct = emit_jcc(cb, 0x85);       /* JNE → 慢路径 */
                /* 3) field_idx < obj->def->field_count（32 位零扩展加载，勿用 64 位） */
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, def));
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjStructDef, field_count));
                emit_cmp_reg32_imm32(cb, JIT_R8, (uint32_t)field_idx);
                int gf_oob = emit_jcc(cb, 0x86);              /* JBE（count <= idx）→ 慢路径 */
                /* 4) RAX = field_values[field_idx] */
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, field_values));
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RDX, (int32_t)field_idx * 8);
                EMIT_VALUE_TO_RAW();
                int gf_done = emit_jmp(cb);

                /* ---- 慢路径：原 callout（类型检查 / 报错文本全保留）---- */
                patch_rel32(cb, gf_not_obj, cb->len);
                patch_rel32(cb, gf_not_struct, cb->len);
                patch_rel32(cb, gf_oob, cb->len);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);  /* obj_val (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)field_idx);  /* field_idx (2nd arg) */
                EMIT_CALL(jit_callout_get_field_fast);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                patch_rel32(cb, gf_done, cb->len);
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
                         * 用大偏移把内联体的 bc_off 与调用方隔离开。
                         *
                         * 关键：命名空间基准必须按 **内联实例**（inline_idx）区分，
                         * 不能只按 nesting depth —— off_map / patch 表都以 bc_off
                         * 为键，offmap_lookup 返回**第一个**匹配项。同一深度下多个
                         * 内联点若共用基址，第 2..N 个实例的内部跳转
                         * （OP_JUMP_IF_FALSE / OP_JUMP / OP_FOR_PREP 的 skip /
                         * OP_FOR_LOOP 回边）会解析到**第 1 个实例的机器码**，
                         * 表现为死循环 + RSP 逐轮漂移 → 栈溢出。
                         * 实例：examples/crypto/aes128.leno 的 mix_columns 有 4 个
                         * gf_mul 内联点，全部落在 0x10000 命名空间，1 亿次基准里
                         * JIT 首次执行即 0xC00000FD（见 §8.26）。
                         *
                         * 内联目前只有一层（内联体内 FIND_INLINE_SITE 不会再命中），
                         * 被内联的函数体 ≤ 256 字节（scan 的 cc->len <= 256 限制），
                         * 远小于 0x10000，因此 0x10000 * (inline_idx + 1) 既唯一又
                         * 不会与调用方（基址 0）冲突。将来若支持多层内联，
                         * 需把 depth 并入这个 id。 */
                        ip = is->callee_chunk->code;
                        end = ip + is->callee_body_size;
                        cur_local_map = is->callee_local_map;
                        cur_chunk = is->callee_chunk;
                        bc_off = 0x10000 * (inline_idx + 1);
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

