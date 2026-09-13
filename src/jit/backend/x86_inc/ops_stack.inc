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
                /* ---- §8.44 语句级折叠：pinned 槽上的 `x = x ⊕ k` 直接落在寄存器上 ---- */
                if (pin_reg && si == pin_si && !tos_live) {
                    uint8_t alu = 0;
                    int32_t fold_imm = 0;
                    int flen = peek_local_fold(ctx, ip, cur_chunk, slot, bc_off, &alu, &fold_imm);
                    /* 防御：只对 emit 表里确实有实现的那几个 opcode 折叠（避免
                     * 形态识别与发射表不一致时静默少发一条运算） */
                    if (flen > 0 &&
                        (alu == OP_BITAND || alu == OP_BITOR || alu == OP_BITXOR ||
                         alu == OP_ADD_INT_IMM || alu == OP_SUB_INT_IMM ||
                         alu == OP_SHL_IMM || alu == OP_SHR_IMM)) {
                        switch (alu) {
                            case OP_BITAND:      emit_alu_imm32_reg(cb, 4, pin_reg, (uint32_t)fold_imm); break;
                            case OP_BITOR:       emit_alu_imm32_reg(cb, 1, pin_reg, (uint32_t)fold_imm); break;
                            case OP_BITXOR:      emit_alu_imm32_reg(cb, 6, pin_reg, (uint32_t)fold_imm); break;
                            case OP_ADD_INT_IMM: emit_alu_imm8_reg(cb, 0, pin_reg, (uint8_t)fold_imm); break;
                            case OP_SUB_INT_IMM: emit_alu_imm8_reg(cb, 5, pin_reg, (uint8_t)fold_imm); break;
                            case OP_SHL_IMM:
                                emit_shl_imm(cb, pin_reg, (uint8_t)fold_imm);
                                /* VM 语义：int64 左移后**截断**为 48 位（不 bailout），
                                 * 与 SHL_IMM 原 codegen 的 EMIT_INT48_TRUNCATE 一致 */
                                emit_shl_imm(cb, pin_reg, 16);
                                emit_sar_imm(cb, pin_reg, 16);
                                break;
                            case OP_SHR_IMM:     emit_sar_imm(cb, pin_reg, (uint8_t)fold_imm); break;
                            default: break;
                        }
                        /* §8.44：**只有会溢出 int48 的运算才需要检查**。
                         * AND/OR/XOR 在 int48 操作数下结果必然仍在 int48 内（位运算保持
                         * 符号扩展模式）；算术右移只会变小 ⇒ 两者的检查恒真。
                         * 操作数是 int48 这一点由**扫描器**保证（位运算的操作数静态类型
                         * 必须是 int，浮点/null 入口的循环会被拒绝编译 —— 见 §8.41 的实测），
                         * 立即数是 int 字面量。⇒ 省掉检查语义不变。
                         * ADD/SUB 会溢出 ⇒ 必须保留（溢出 bailout 交解释器走 BigInt）。
                         * 实测这一步是本次最大的单项：422ms → 313ms（1.35x）。 */
                        if (alu == OP_ADD_INT_IMM || alu == OP_SUB_INT_IMM)
                            EMIT_INT48_CHECK_REG(pin_reg, bc_off);
                        size = flen;   /* 吃掉整条语句的字节码（含可选的 CAST） */
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-CG] FOLD slot=%d alu=%d imm=%d len=%d\n",
                                    slot, (int)alu, (int)fold_imm, flen);
                        break;
                    }
                }
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
                /* ---- §8.44 形态 B：`x = k ⊕ x`（常量在左；只有可交换的位运算能折，
                 * 不能溢出 ⇒ 与形态 A 一样不需要 int48 检查）---- */
                if (pin_reg && !tos_live && val_is_int(cv)) {
                    int fslot = 0;
                    uint8_t falu = 0;
                    int32_t fimm = 0;
                    int flen = peek_local_fold_constfirst(ctx, ip, cur_chunk, bc_off,
                                                          &fslot, &falu, &fimm);
                    if (flen > 0 && cur_local_map[fslot] == pin_si &&
                        (falu == OP_BITAND || falu == OP_BITOR || falu == OP_BITXOR)) {
                        if (falu == OP_BITAND)     emit_alu_imm32_reg(cb, 4, pin_reg, (uint32_t)fimm);
                        else if (falu == OP_BITOR) emit_alu_imm32_reg(cb, 1, pin_reg, (uint32_t)fimm);
                        else                       emit_alu_imm32_reg(cb, 6, pin_reg, (uint32_t)fimm);
                        size = flen;   /* 吃掉整条语句的字节码（含可选的 CAST） */
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-CG] FOLDB slot=%d alu=%d imm=%d len=%d\n",
                                    fslot, (int)falu, (int)fimm, flen);
                        break;
                    }
                }
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

