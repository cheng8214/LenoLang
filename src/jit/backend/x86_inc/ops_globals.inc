/* ============================================================================
 * ops_globals.inc - Global variable cases: OP_GET_GLOBAL / OP_SET_GLOBAL
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Global variable ops ---- */
            /* R9 = globals pointer (saved from RDX at prologue) */
            /* Globals store NaN-boxed Values; JIT virtual stack holds raw int64. */
            /* OP_GET_GLOBAL: load Value, type-guard (int OR float), extract, push raw */
            case OP_GET_GLOBAL: {
                uint16_t slot = rd_short(ip + 1);
                int gd = slot * 8;
                TOS_SPILL();  /* clobbers RAX, R8 */
                /* Load NaN-boxed Value: mov rax, [r9 + gd] */
                if (gd >= -128 && gd <= 127)
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_R9, (int8_t)gd);
                else
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_R9, gd);
                /* Get top 16 bits: mov r8, rax; shr r8, 48 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                {
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));  /* /5 = SHR */
                    emit_byte(cb, 48);
                }
                /* Check int: cmp r8, 0xFFFB */
                {
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));  /* /7 = CMP */
                    emit_uint32(cb, 0x0000FFFB);
                }
                /* je .is_int (rel8 placeholder) */
                emit_byte(cb, 0x74);
                int isint_patch = cb->len;
                emit_byte(cb, 0x00);

                /* Non-int value (float or object).
                 * For floats: RAX already has raw double bits (same as NaN-boxed).
                 * For objects (arrays, dicts, strings): RAX has NaN-boxed
                 * value, pushed as-is for callout consumption (OP_INDEX,
                 * OP_ARRAY_APPEND, etc.). EMIT_RAW_TO_VALUE() in callouts
                 * correctly handles non-int48 values by leaving them as-is. */

                /* Float/object path: push RAX as-is */
                TOS_PRODUCE();
                /* jmp .next (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);

                /* .is_int: patch je to here */
                cb->buf[isint_patch] = (uint8_t)(cb->len - (isint_patch + 1));
                /* Int path: extract int48 */
                emit_and_rr(cb, JIT_RAX, JIT_R10);
                emit_shl_imm(cb, JIT_RAX, 16);
                emit_sar_imm(cb, JIT_RAX, 16);
                TOS_PRODUCE();

                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                vstack++;
                break;
            }
            /* OP_SET_GLOBAL: peek raw int64, runtime type dispatch:
             * - If value fits int48 → re-encode as NaN-boxed int
             * - If not (likely float) → store raw double bits directly */
            case OP_SET_GLOBAL: {
                uint16_t slot = rd_short(ip + 1);
                int gd = slot * 8;
                /* peek TOS → RAX (clobbers RAX, R8) */
                TOS_PEEK_TO(JIT_RAX);
                /* Check if value fits int48: sar r8, 47; inc r8; cmp r8, 1; ja .is_float */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .is_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: re-encode as NaN-boxed int */
                emit_and_rr(cb, JIT_RAX, JIT_R10);
                emit_or_rr(cb, JIT_RAX, JIT_R11);
                if (gd >= -128 && gd <= 127)
                    emit_mov_mem8_reg(cb, JIT_R9, (int8_t)gd, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_R9, gd, JIT_RAX);
                /* jmp .done (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int done_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here — reload TOS and store raw bits */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                TOS_PEEK_TO(JIT_RAX);  /* reload raw value */
                if (gd >= -128 && gd <= 127)
                    emit_mov_mem8_reg(cb, JIT_R9, (int8_t)gd, JIT_RAX);
                else
                    emit_mov_mem32_reg(cb, JIT_R9, gd, JIT_RAX);
                /* .done: patch jmp to here */
                cb->buf[done_patch] = (uint8_t)(cb->len - (done_patch + 1));
                break;
            }

