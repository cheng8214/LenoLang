/* ============================================================================
 * ops_float.inc - Float arithmetic cases (add/sub/mul/div_float, OP_DIV, neg/cast_float)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Float arithmetic ---- */
            /* Float values on the virtual stack are stored as raw double bits in int64 slots. */
            /* We load them into XMM registers, do SSE2 arithmetic, store back. */
            case OP_ADD_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);   /* b (raw double bits) */
                TOS_CONSUME_TO(JIT_RDX);   /* a */
                /* movq xmm0, rdx; movq xmm1, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x58, 0, 1);  /* ADDSD xmm0, xmm1 → xmm0 = a + b */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x5C, 0, 1);  /* SUBSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MUL_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x59, 0, 1);  /* MULSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_DIV_FLOAT:
                TOS_CONSUME_TO(JIT_RAX);
                TOS_CONSUME_TO(JIT_RDX);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                emit_sse2_rr(cb, 0x5E, 0, 1);  /* DIVSD xmm0, xmm1 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_DIV: {
                /* 通用除法 callout: 运行时类型分发 (int/int, float 混合, BigInt) */
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* b (divisor) */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* a (dividend) */
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_RCX, JIT_RDX);   /* a */
                emit_mov_rr(cb, JIT_RDX, JIT_R8);    /* b */
                EMIT_CALL(jit_callout_div);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_NEG_FLOAT:
                TOS_CONSUME_RAX();
                /* movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* Load sign mask into xmm1: mov rdx, 0x8000000000000000; movq xmm1, rdx */
                emit_mov_reg_imm64(cb, JIT_RDX, 0x8000000000000000ULL);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RDX & 7));
                emit_xorpd_xmm_xmm(cb, 0, 1);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                break;
            case OP_CAST_FLOAT: {
                /* Runtime type dispatch:
                 * - If value fits int48 → it's an int, convert via CVTSI2SD
                 * - If not (likely float) → already float, pass through as-is */
                TOS_CONSUME_RAX();
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .already_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: convert int64 to double */
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                /* .already_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                TOS_PRODUCE();
                break;
            }

