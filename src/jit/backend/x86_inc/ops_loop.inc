/* ============================================================================
 * ops_loop.inc - Loop control cases (FOR_PREP, LOOP, FOR_LOOP)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Inner for-loop init: OP_FOR_PREP ---- */
            case OP_FOR_PREP: {
                /* 8 bytes: op, start_slot, end_slot, step_slot,
                 *         loop_var_slot, inclusive, jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t start_sl = ip[1];
                uint8_t end_sl   = ip[2];
                uint8_t step_sl  = ip[3];
                uint8_t lv_sl    = ip[4];
                uint8_t incl     = ip[5];
                uint16_t joff    = rd_short(ip + 6);

                int si_start = cur_local_map[start_sl];
                int si_end   = cur_local_map[end_sl];
                int si_step  = cur_local_map[step_sl];
                int si_lv    = cur_local_map[lv_sl];
                int d_start = scratch_disp(si_start);
                int d_end   = scratch_disp(si_end);
                int d_step  = scratch_disp(si_step);
                int d_lv    = scratch_disp(si_lv);

                /* step 为 float（类型位图 bit=1）→ JIT 的 FOR_LOOP 走 int48 快路径，
                 * 无法处理 double 位模式的方向/比较 → 交回解释器 */
                if (si_step >= 0) {
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x0F);
                    emit_byte(cb, 0xBA);
                    emit_byte(cb, 0xE3);  /* BT RBX, imm8 */
                    emit_byte(cb, (uint8_t)(si_step & 0xFF));
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int step_float_bail = emit_jcc(cb, 0x82);  /* JC -> bailout */
                    patch_add(ctx, step_float_bail, -1, 0);
                }

                /* Check step != 0 (bailout if step == 0) */
                if (d_step >= -128 && d_step <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_step);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                int step_bail = emit_jcc(cb, 0x84);  /* JE (step==0) → bailout */
                patch_add(ctx, step_bail, -1, 0);

                /* step 符号分流：step < 0 走反向初始条件检查（支持倒序循环） */
                if (d_step >= -128 && d_step <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_step);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                int prep_neg_patch = emit_jcc(cb, 0x88);  /* JS -> negative init check */

                /* Set loop_var = start */
                if (d_start >= -128 && d_start <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_start);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_start);
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);

                /* Check initial condition: start vs end (step > 0) */
                if (d_end >= -128 && d_end <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_end);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_end);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* step > 0, exclusive: JGE (start >= end) → skip inner loop
                 * step > 0, inclusive: JG  (start > end)  → skip inner loop */
                uint8_t cc_skip = incl ? 0x8F /*JG*/ : 0x8D /*JGE*/;
                int skip_patch = emit_jcc(cb, cc_skip);
                /* Forward jump: target = bc_off + 8 + joff (past inner body + FOR_LOOP) */
                int skip_target = bc_off + size + (int)joff;
                patch_add(ctx, skip_patch, skip_target, 0);

                int prep_done_patch = emit_jmp(cb);   /* 正向检查结束，跳过反向块 */

                /* ---- 反向初始条件 (step < 0)：跳过条件与正向相反 ----
                 * exclusive: start <= end -> skip；inclusive: start < end -> skip */
                patch_rel32(cb, prep_neg_patch, cb->len);
                if (d_start >= -128 && d_start <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_start);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_start);
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);
                if (d_end >= -128 && d_end <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_end);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_end);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                {
                    uint8_t cc_skip_neg = incl ? 0x8C /*JL*/ : 0x8E /*JLE*/;
                    int skip_patch_neg = emit_jcc(cb, cc_skip_neg);
                    patch_add(ctx, skip_patch_neg, skip_target, 0);
                }
                patch_rel32(cb, prep_done_patch, cb->len);
                break;
            }

            /* ---- Back-edge: OP_LOOP ---- */
            case OP_LOOP: {
                /* 5 bytes: op + 4-byte big-endian int32 backward offset */
                TOS_SPILL();
                int32_t loff = rd_int32(ip + 1);
                int target_bc = bc_off + size - loff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) target_mc = ctx->loop_start_mc;
                int patch = emit_jmp(cb);
                patch_rel32(cb, patch, target_mc);
                break;
            }

            /* ---- Back-edge: OP_FOR_LOOP ---- */
            case OP_FOR_LOOP: {
                /* 7 bytes: op, lv_slot, step_slot, end_slot, inclusive,
                 *         jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t lv_sl  = ip[1];
                uint8_t st_sl  = ip[2];
                uint8_t en_sl  = ip[3];
                uint8_t incl   = ip[4];
                uint16_t joff  = rd_short(ip + 5);

                int si_lv = cur_local_map[lv_sl];
                int si_st = cur_local_map[st_sl];
                int si_en = cur_local_map[en_sl];
                int d_lv = scratch_disp(si_lv);
                int d_st = scratch_disp(si_st);
                int d_en = scratch_disp(si_en);

                int target_bc = bc_off + size - (int)joff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) target_mc = ctx->loop_start_mc;

                /* loop_var += step (+ int48 check)，再与 end 比较；
                 * 继续循环的跳转由方向决定（正/负步长各一份）。 */
                #define EMIT_FOR_STEP_AND_CMP() do { \
                    if (d_lv >= -128 && d_lv <= 127) \
                        emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv); \
                    else \
                        emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv); \
                    if (d_st >= -128 && d_st <= 127) \
                        emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st); \
                    else \
                        emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st); \
                    emit_add_rr(cb, JIT_RAX, JIT_RDX); \
                    EMIT_INT48_CHECK(bc_off); \
                    if (d_lv >= -128 && d_lv <= 127) \
                        emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX); \
                    else \
                        emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX); \
                    if (d_en >= -128 && d_en <= 127) \
                        emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en); \
                    else \
                        emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en); \
                    emit_cmp_rr(cb, JIT_RAX, JIT_RDX); \
                } while(0)

                /* step 符号分流：step == 0 不回溯（落到循环之后）；
                 * step < 0 走反向比较（倒序循环） */
                if (d_st >= -128 && d_st <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_st);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_st);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                int zero_patch = emit_jcc(cb, 0x84);   /* JZ -> after (step == 0) */
                int neg_patch  = emit_jcc(cb, 0x88);   /* JS -> backward direction */

                /* ---- forward (step > 0): continue while lv <(=) end ---- */
                EMIT_FOR_STEP_AND_CMP();
                {
                    uint8_t cc = incl ? 0x8E /*JLE*/ : 0x8C /*JL*/;
                    int patch = emit_jcc(cb, cc);
                    patch_rel32(cb, patch, target_mc);
                }
                int done_patch = emit_jmp(cb);

                /* ---- backward (step < 0): continue while lv >(=) end ---- */
                patch_rel32(cb, neg_patch, cb->len);
                EMIT_FOR_STEP_AND_CMP();
                {
                    uint8_t cc_neg = incl ? 0x8D /*JGE*/ : 0x8F /*JG*/;
                    int patch = emit_jcc(cb, cc_neg);
                    patch_rel32(cb, patch, target_mc);
                }
                patch_rel32(cb, done_patch, cb->len);
                patch_rel32(cb, zero_patch, cb->len);

                #undef EMIT_FOR_STEP_AND_CMP
                break;
            }

