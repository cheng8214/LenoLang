/* ============================================================================
 * ops_imm.inc - Immediate arithmetic cases (ADD/SUB/MUL_INT_IMM)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            case OP_ADD_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* add rax, imm8 (sign-extended) */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* /0 = ADD */
                emit_byte(cb, (uint8_t)imm);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                break;
            }
            case OP_SUB_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* /5 = SUB */
                emit_byte(cb, (uint8_t)imm);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                break;
            }
            case OP_MUL_INT_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_mov_reg_imm64(cb, JIT_RDX, (uint64_t)(int64_t)imm);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK(bc_off);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                break;
            }

