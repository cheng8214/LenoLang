/* ============================================================================
 * ops_stack.inc - Stack / local-variable / OP_CONST cases (get_local, set_local, move_local, ...)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Stack ops ---- */
            case OP_ZERO:
                TOS_SPILL();
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);  /* xor rax, rax → 0 */
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_ONE:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, 1);
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_POP:
                TOS_DISCARD();
                vstack--;
                break;
            case OP_DUP:
                if (tos_live) {
                    /* RAX has TOS, push a copy to stack; RAX stays as TOS */
                    emit_push_reg(cb, JIT_RAX);
                } else {
                    /* Peek TOS from stack into RAX, mark as live */
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RSP, 0);
                    TOS_PRODUCE();
                }
                vstack++;
                break;

            /* ---- Local variable ops ---- */
            case OP_GET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                EMIT_LOAD_LOCAL(JIT_RAX, si, disp);   /* §8.42：pinned 槽走寄存器 */
                TOS_PRODUCE();
                vstack++;
                if (jit_debug_on()) fprintf(stderr, "[JIT-CG] GET_LOCAL slot=%d -> scratch[%d] disp=%d vstack=%d\n", slot, si, disp, vstack);
                break;
            }
            case OP_SET_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                /* peek TOS → store to local */
                TOS_PEEK_TO(JIT_RAX);
                EMIT_STORE_LOCAL(JIT_RAX, si, disp);   /* §8.42：pinned 槽走寄存器 */
                break;
            }
            case OP_SET_LOCAL_POP: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                /* consume TOS → store to local */
                TOS_CONSUME_RAX();
                EMIT_STORE_LOCAL(JIT_RAX, si, disp);   /* §8.42：pinned 槽走寄存器 */
                vstack--;
                if (jit_debug_on()) fprintf(stderr, "[JIT-CG] SET_LOCAL_POP slot=%d -> scratch[%d] disp=%d vstack=%d\n", slot, si, disp, vstack);
                break;
            }
            case OP_MOVE_LOCAL: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                int si_src = cur_local_map[src];
                int si_dst = cur_local_map[dst];
                int d_src = scratch_disp(si_src);
                int d_dst = scratch_disp(si_dst);
                TOS_SPILL();
                if (d_src >= -128 && d_src <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_src);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_src);
                }
                if (d_dst >= -128 && d_dst <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_dst, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, d_dst, JIT_RAX);
                }
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_MOVE_LOCAL_POP: {
                uint16_t src = rd_short(ip + 1);
                uint16_t dst = rd_short(ip + 3);
                int si_src = cur_local_map[src];
                int si_dst = cur_local_map[dst];
                int d_src = scratch_disp(si_src);
                int d_dst = scratch_disp(si_dst);
                /* No stack effect (pop + local op), but clobbers RAX */
                TOS_SPILL();
                if (d_src >= -128 && d_src <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)d_src);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, d_src);
                }
                if (d_dst >= -128 && d_dst <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)d_dst, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, d_dst, JIT_RAX);
                }
                break;
            }
            case OP_SET_LOCAL_CONST: {
                uint16_t ci = rd_short(ip + 1);
                uint16_t slot = rd_short(ip + 3);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                Value cv = cur_chunk->constants[ci];
                TOS_SPILL();  /* clobbers RAX */
                if (val_is_int(cv)) {
                    int64_t iv = val_as_int(cv);
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)iv);
                } else if (val_is_float(cv)) {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                } else {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x0F);
                    emit_byte(cb, 0xBA);
                    emit_byte(cb, 0xEB);
                    emit_byte(cb, (uint8_t)si);
                }
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_CONST: {
                uint16_t ci = rd_short(ip + 1);
                Value cv = cur_chunk->constants[ci];
                TOS_SPILL();
                if (val_is_int(cv)) {
                    int64_t iv = val_as_int(cv);
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)iv);
                    TOS_PRODUCE();
                } else if (val_is_float(cv)) {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    TOS_PRODUCE();
                } else {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    TOS_PRODUCE();
                }
                vstack++;
                break;
            }

