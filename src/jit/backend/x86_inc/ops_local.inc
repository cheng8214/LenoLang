/* ============================================================================
 * ops_local.inc - Local-variable increment/decrement cases (INC/DEC_LOCAL[_NOPUSH], PRE_INC/DEC_LOCAL)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Inc/Dec locals ---- */
            case OP_INC_LOCAL_NOPUSH: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                /* mov rax, [rbp+disp]; add rax, 1; mov [rbp+disp], rax */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_DEC_LOCAL_NOPUSH: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                break;
            }
            case OP_INC_LOCAL: {
                /* push old value, then inc local */
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                /* Save old value in R8 before incrementing */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                /* RAX = old value (from R8) */
                emit_mov_rr(cb, JIT_RAX, JIT_R8);
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_DEC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                /* Save old value in R8 before decrementing */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                emit_mov_rr(cb, JIT_RAX, JIT_R8);  /* old value */
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_PRE_INC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* ADD rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                TOS_PRODUCE();  /* RAX = new value */
                vstack++;
                break;
            }
            case OP_PRE_DEC_LOCAL: {
                uint16_t slot = rd_short(ip + 1);
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();  /* clobbers RAX */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* SUB rax, 1 */
                emit_byte(cb, 1);
                EMIT_INT48_CHECK();
                if (disp >= -128 && disp <= 127) {
                    emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                } else {
                    emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                }
                TOS_PRODUCE();  /* RAX = new value */
                vstack++;
                break;
            }

