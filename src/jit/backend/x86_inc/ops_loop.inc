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

                /* Check step > 0 (bailout if step <= 0) */
                if (d_step >= -128 && d_step <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_step);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                int step_bail = emit_jcc(cb, 0x8E);  /* JLE → bailout */
                patch_add(ctx, step_bail, -1, 0);

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

                /* Increment loop_var by step */
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_lv);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_lv);
                if (d_st >= -128 && d_st <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_st);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_st);
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                if (d_lv >= -128 && d_lv <= 127)
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_lv, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_RBP, d_lv, JIT_RAX);
                /* Compare with end */
                if (d_en >= -128 && d_en <= 127)
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)d_en);
                else
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, d_en);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Conditional jump to inner loop start */
                uint8_t cc = incl ? 0x8E /*JLE*/ : 0x8C /*JL*/;
                int target_bc = bc_off + size - (int)joff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) target_mc = ctx->loop_start_mc;
                int patch = emit_jcc(cb, cc);
                patch_rel32(cb, patch, target_mc);
                break;
            }

