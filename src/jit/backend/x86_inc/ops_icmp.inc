/* ============================================================================
 * ops_icmp.inc - Int comparison / immediate comparison / shift cases (EQ..GE_INT, *_INT_IMM, SHL/SHR/USHR_IMM)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Comparisons (push 0/1) ---- */
            case OP_EQ_INT: case OP_LT_INT: case OP_GT_INT:
            case OP_LE_INT: case OP_GE_INT: {
                uint8_t cc;
                switch (op) {
                    case OP_EQ_INT: cc = 0x4; break;  /* JE  */
                    case OP_LT_INT: cc = 0xC; break;  /* JL  */
                    case OP_GT_INT: cc = 0xF; break;  /* JG  */
                    case OP_LE_INT: cc = 0xE; break;  /* JLE */
                    case OP_GE_INT: cc = 0xD; break;  /* JGE */
                    default: cc = 0x4; break;
                }
                TOS_CONSUME_TO(JIT_RDX);  /* b */
                TOS_CONSUME_RAX();         /* a */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                vstack--;
                break;
            }

            /* ---- Immediate comparisons ---- */
            case OP_LT_INT_IMM: case OP_GT_INT_IMM:
            case OP_LE_INT_IMM: case OP_GE_INT_IMM: case OP_EQ_INT_IMM: {
                uint8_t cc;
                switch (op) {
                    case OP_LT_INT_IMM: cc = 0xC; break;
                    case OP_GT_INT_IMM: cc = 0xF; break;
                    case OP_LE_INT_IMM: cc = 0xE; break;
                    case OP_GE_INT_IMM: cc = 0xD; break;
                    case OP_EQ_INT_IMM: cc = 0x4; break;
                    default: cc = 0x4; break;
                }
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* cmp rax, imm8 (sign-extended) */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 7, JIT_RAX & 7));  /* /7 = CMP */
                emit_byte(cb, (uint8_t)imm);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                break;
            }

            /* ---- Shift immediates ---- */
            case OP_SHL_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_shl_imm(cb, JIT_RAX, (uint8_t)imm);
                TOS_PRODUCE();
                break;
            }
            case OP_SHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                emit_sar_imm(cb, JIT_RAX, (uint8_t)imm);
                TOS_PRODUCE();
                break;
            }
            case OP_USHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                /* SHR rax, imm (unsigned right shift) */
                {
                    int b = (JIT_RAX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_RAX & 7));  /* /5 = SHR */
                    emit_byte(cb, (uint8_t)imm);
                }
                TOS_PRODUCE();
                break;
            }

