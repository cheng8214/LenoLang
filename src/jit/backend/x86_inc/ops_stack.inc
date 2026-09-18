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
                /* §8.104：记下 TOS 的来源槽 —— 供本次转换（EMIT_RAW_TO_VALUE）用 RBX
                 * 类型位图判定"该槽已知非 int"⇒ 跳过装箱（避免把 float 0.0 贴成 int 0）。 */
                tos_from_si = si;
                int disp = scratch_disp(si);
                int fold_reg = pin_reg_of(pin_si, pin_n, si);   /* §8.45：本槽自己的寄存器 */
                /* ---- §8.44 语句级折叠：pinned 槽上的 `x = x ⊕ k` 直接落在寄存器上 ---- */
                if (fold_reg && !tos_live) {
                    uint8_t alu = 0;
                    int32_t fold_imm = 0;
                    int flen = peek_local_fold(ctx, ip, cur_chunk, slot, bc_off, &alu, &fold_imm);
                    /* 防御：只对 emit 表里确实有实现的那几个 opcode 折叠（避免
                     * 形态识别与发射表不一致时静默少发一条运算） */
                    if (flen > 0 &&
                        (alu == OP_BITAND || alu == OP_BITOR || alu == OP_BITXOR ||
                         alu == OP_ADD_INT_IMM || alu == OP_SUB_INT_IMM ||
                         alu == OP_SHL_IMM || alu == OP_SHR_IMM)) {
                        /* ---- §8.46 一步回看：上一条语句留下的**值域掩码** ----
                         * 掩码 M 的语义：该槽的值 ∈ [0, M]，且 M 的位都在低 47 位内
                         * （⇒ 值非负、且在 int48 内）。仅当下列条件全部满足才可用：
                         *   · 紧邻的上一条语句是**同槽**折叠（fold_prev_si == si）；
                         *   · 当前语句起始偏移**不是前向跳转目标**（否则那条语句
                         *     可能被跳过 ⇒ M 未必成立；扫描器 jt_fwd / jt_ok）；
                         *   · 体内没有内层回边（do{}while 形态第二轮会从本语句进入
                         *     而跳过上一条；扫描器 has_back_jump）。
                         * 这两条来自**扫描器的静态事实**，不做任何跨迭代猜测。 */
                        int mask_ok = (fold_prev_si == si) && sr->jt_ok && !sr->has_back_jump &&
                                      bc_off < JIT_SCAN_JT_MAX && !sr->jt_fwd[bc_off];
                        int noop_and = 0, skip_check = 0;
                        if (mask_ok) {
                            /* 恒等 AND：值 ⊆ M ⇒ `x & imm` 不改变 x（gcc 删掉 `&1135`
                             * 的同一推理：M=0x467 ⊆ 0x46F）。整条不发。 */
                            if (alu == OP_BITAND && fold_imm >= 0 &&
                                (fold_prev_mask & ~(uint64_t)(uint32_t)fold_imm) == 0)
                                noop_and = 1;
                            /* 值域 + 立即数仍在 int48 内 ⇒ 溢出检查恒真，不发 */
                            if (alu == OP_ADD_INT_IMM || alu == OP_SUB_INT_IMM) {
                                int64_t k  = (int64_t)fold_imm;
                                int64_t lo = (alu == OP_ADD_INT_IMM) ? (k < 0 ? k : 0) : (k > 0 ? -k : 0);
                                int64_t hi = (int64_t)fold_prev_mask +
                                             ((alu == OP_ADD_INT_IMM) ? (k > 0 ? k : 0) : (k < 0 ? -k : 0));
                                if (lo >= -(1LL << 47) && hi <= (1LL << 47) - 1) skip_check = 1;
                            }
                        }
                        /* §8.125：**折叠路径同样不能假定操作数是 int48** ✗ ——
                         * 下面那条"位运算不会溢出 ⇒ 不需要检查"只对"静态类型是 int"成立，
                         * 而 **bigint 的静态类型也是 int** ⇒ 一个 bigint（NaN-boxed 指针）
                         * 落进 pinned 槽时，位运算/移位会作用在它的**位型**上 ⇒ 静默算错
                         * （实测 sha512 的 add64：`a >> 32`，a 是 bigint；动态移位量走通用
                         * 路径有检查 ⇒ 正常，只有 IMM/折叠形态漏了）。
                         * 这里对**没有自带检查**的 alu 补一步运行期确认；ADD/SUB 下面已有检查。 */
                        if (alu == OP_BITAND || alu == OP_BITOR || alu == OP_BITXOR ||
                            alu == OP_SHL_IMM || alu == OP_SHR_IMM) {
                            EMIT_INT48_CHECK_REG(fold_reg, bc_off);
                        }
                        if (!noop_and) {
                            switch (alu) {
                                case OP_BITAND:      emit_alu_imm32_reg(cb, 4, fold_reg, (uint32_t)fold_imm); break;
                                case OP_BITOR:       emit_alu_imm32_reg(cb, 1, fold_reg, (uint32_t)fold_imm); break;
                                case OP_BITXOR:      emit_alu_imm32_reg(cb, 6, fold_reg, (uint32_t)fold_imm); break;
                                case OP_ADD_INT_IMM: emit_alu_imm8_reg(cb, 0, fold_reg, (uint8_t)fold_imm); break;
                                case OP_SUB_INT_IMM: emit_alu_imm8_reg(cb, 5, fold_reg, (uint8_t)fold_imm); break;
                                case OP_SHL_IMM:
                                    emit_shl_imm(cb, fold_reg, (uint8_t)fold_imm);
                                    /* VM 语义：int64 左移后**截断**为 48 位（不 bailout），
                                     * 与 SHL_IMM 原 codegen 的 EMIT_INT48_TRUNCATE 一致 */
                                    emit_shl_imm(cb, fold_reg, 16);
                                    emit_sar_imm(cb, fold_reg, 16);
                                    break;
                                case OP_SHR_IMM:     emit_sar_imm(cb, fold_reg, (uint8_t)fold_imm); break;
                                default: break;
                            }
                            /* §8.44：**只有会溢出 int48 的运算才需要检查**。
                             * AND/OR/XOR 在 int48 操作数下结果必然仍在 int48 内（位运算保持
                             * 符号扩展模式）；算术右移只会变小 ⇒ 两者的检查恒真。
                             * 操作数是 int48 这一点由**扫描器**保证（位运算的操作数静态类型
                             * 必须是 int，浮点/null 入口的循环会被拒绝编译 —— 见 §8.41 的实测），
                             * 立即数是 int 字面量。⇒ 省掉检查语义不变。
                             * ADD/SUB 会溢出 ⇒ 必须保留（溢出 bailout 交解释器走 BigInt），
                             * 但 §8.46 能证明「这一步加不出 int48」时同样可以不发。 */
                            if ((alu == OP_ADD_INT_IMM || alu == OP_SUB_INT_IMM) && !skip_check)
                                EMIT_INT48_CHECK_REG(fold_reg, bc_off);
                        }
                        /* 记录留给下一步回看的掩码：AND ⇒ 收紧到 imm（或更紧的 M）；
                         * XOR/OR ⇒ 并上 imm 的位（只在仍落在低 47 位内时可信）。 */
                        if (alu == OP_BITAND && fold_imm >= 0) {
                            uint64_t nm = (uint64_t)(uint32_t)fold_imm;
                            if (mask_ok && (fold_prev_mask & ~nm) == 0) nm = fold_prev_mask;
                            prev_fold_si = si; prev_fold_mask = nm;
                        } else if (mask_ok && (alu == OP_BITXOR || alu == OP_BITOR)) {
                            uint64_t tm = fold_prev_mask | (uint64_t)(uint32_t)fold_imm;
                            if ((tm >> 47) == 0) { prev_fold_si = si; prev_fold_mask = tm; }
                        }
                        size = flen;   /* 吃掉整条语句的字节码（含可选的 CAST） */
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-CG] FOLD slot=%d alu=%d imm=%d len=%d%s%s\n",
                                    slot, (int)alu, (int)fold_imm, flen,
                                    noop_and ? " [恒等 AND 不发]" : "",
                                    skip_check ? " [检查恒真不发]" : "");
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
                if (pin_n > 0 && !tos_live && val_is_int(cv)) {
                    int fslot = 0;
                    uint8_t falu = 0;
                    int32_t fimm = 0;
                    int flen = peek_local_fold_constfirst(ctx, ip, cur_chunk, bc_off,
                                                          &fslot, &falu, &fimm);
                    int fsi  = (flen > 0) ? cur_local_map[fslot] : -1;
                    int freg = (flen > 0) ? pin_reg_of(pin_si, pin_n, fsi) : 0;
                    if (freg && (falu == OP_BITAND || falu == OP_BITOR || falu == OP_BITXOR)) {
                        /* §8.46 一步回看（条件与形态 A 完全相同，见那里的说明） */
                        int mask_ok = (fold_prev_si == fsi) && sr->jt_ok && !sr->has_back_jump &&
                                      bc_off < JIT_SCAN_JT_MAX && !sr->jt_fwd[bc_off];
                        int noop_and = mask_ok && falu == OP_BITAND && fimm >= 0 &&
                                       (fold_prev_mask & ~(uint64_t)(uint32_t)fimm) == 0;
                        /* §8.125：形态 B（`k ⊕ x`）同样要运行期确认 —— 见形态 A 处的说明
                         * （"静态类型是 int"推不出"值是 int48"，bigint 静态也是 int ✗）。 */
                        EMIT_INT48_CHECK_REG(freg, bc_off);
                        if (!noop_and) {
                            if (falu == OP_BITAND)     emit_alu_imm32_reg(cb, 4, freg, (uint32_t)fimm);
                            else if (falu == OP_BITOR) emit_alu_imm32_reg(cb, 1, freg, (uint32_t)fimm);
                            else                       emit_alu_imm32_reg(cb, 6, freg, (uint32_t)fimm);
                        }
                        if (falu == OP_BITAND && fimm >= 0) {
                            uint64_t nm = (uint64_t)(uint32_t)fimm;
                            if (mask_ok && (fold_prev_mask & ~nm) == 0) nm = fold_prev_mask;
                            prev_fold_si = fsi; prev_fold_mask = nm;
                        } else if (mask_ok && (falu == OP_BITOR || falu == OP_BITXOR)) {
                            uint64_t tm = fold_prev_mask | (uint64_t)(uint32_t)fimm;
                            if ((tm >> 47) == 0) { prev_fold_si = fsi; prev_fold_mask = tm; }
                        }
                        size = flen;   /* 吃掉整条语句的字节码（含可选的 CAST） */
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-CG] FOLDB slot=%d alu=%d imm=%d len=%d%s\n",
                                    fslot, (int)falu, (int)fimm, flen,
                                    noop_and ? " [恒等 AND 不发]" : "");
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
                    tos_float_prov = 1;   /* §8.127：float 常量 ⇒ raw 必是 double 位型 ✓ */
                } else {
                    emit_mov_reg_imm64(cb, JIT_RAX, cv);
                    TOS_PRODUCE();
                }
                vstack++;
                break;
            }

