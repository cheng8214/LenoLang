/* ============================================================================
 * ops_float.inc - Float arithmetic cases (add/sub/mul/div_float, OP_DIV, neg/cast_float)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Float arithmetic ----
             * 虚拟栈上的浮点值是裸 double 位模式，但操作数**未必已是 float**
             * （见 EMIT_FLOAT_ARGS2 的说明：float 局部量 + 动态类型调用结果 /
             *  数组元素 / 字典值时会直接把 int48 喂进来）。解释器一律经
             * val_as_num_ex 提升，所以这里统一用 EMIT_FLOAT_ARGS2 取操作数，
             * NaN-boxed（含 BigInt）一律 bailout 交解释器。 */
            case OP_ADD_FLOAT: {
                int ta, tb, end_jmp;
                EMIT_FLOAT_ARGS2(ta, tb);
                emit_sse2_rr(cb, 0x58, 0, 1);  /* ADDSD xmm0, xmm1 → xmm0 = a + b */
                EMIT_MOVQ_RAX_XMM0();
                end_jmp = emit_jmp(cb);
                EMIT_FLOAT_TAGGED_BAILOUT(ta, tb);
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_SUB_FLOAT: {
                int ta, tb, end_jmp;
                EMIT_FLOAT_ARGS2(ta, tb);
                emit_sse2_rr(cb, 0x5C, 0, 1);  /* SUBSD xmm0, xmm1 */
                EMIT_MOVQ_RAX_XMM0();
                end_jmp = emit_jmp(cb);
                EMIT_FLOAT_TAGGED_BAILOUT(ta, tb);
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_MUL_FLOAT: {
                int ta, tb, end_jmp;
                EMIT_FLOAT_ARGS2(ta, tb);
                emit_sse2_rr(cb, 0x59, 0, 1);  /* MULSD xmm0, xmm1 */
                EMIT_MOVQ_RAX_XMM0();
                end_jmp = emit_jmp(cb);
                EMIT_FLOAT_TAGGED_BAILOUT(ta, tb);
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* DIV 额外要求与解释器一致：除数为 ±0.0 要抛「浮点除零错误」，
             * JIT 不能默默算成 inf/NaN。
             * 注意必须与 **0.0** 比较：UCOMISD xmm1, xmm1（自己比自己）永远
             * 相等 → ZF 恒为 1，会变成「每次除法都 bailout」。
             * 无序（NaN 除数，PF=1）不走 bailout：DIVSD 得 NaN，与解释器
             * 结果一致，避免把含 NaN 的热循环整片踢回解释器。 */
            case OP_DIV_FLOAT: {
                int ta, tb, izero, jnan, end_jmp;
                EMIT_FLOAT_ARGS2(ta, tb);
                emit_xorpd_xmm_xmm(cb, 2, 2);        /* xmm2 = 0.0 */
                emit_ucomisd_rr(cb, 1, 2);           /* xmm1 ? 0.0 */
                jnan = emit_jcc(cb, 0x8A);           /* JP：无序(NaN) → 不回退 */
                izero = emit_jcc(cb, 0x84);          /* JE：等于 ±0 → bailout */
                patch_rel32(cb, jnan, cb->len);      /* .nan: 直接进 DIVSD */
                emit_sse2_rr(cb, 0x5E, 0, 1);        /* DIVSD xmm0, xmm1 */
                EMIT_MOVQ_RAX_XMM0();
                end_jmp = emit_jmp(cb);
                patch_rel32(cb, ta, cb->len);
                patch_rel32(cb, tb, cb->len);
                patch_rel32(cb, izero, cb->len);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jmp(cb); patch_add(ctx, _p, -1, 0); }
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
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
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);   /* a */
                emit_mov_rr(cb, JIT_ARG2, JIT_R8);    /* b */
                EMIT_CALL(jit_callout_div);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* 一元负号：解释器用 val_as_num（int 提升为 double）→ 同样三态取值 */
            case OP_NEG_FLOAT: {
                int ta, end_jmp;
                TOS_CONSUME_RAX();
                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, ta);   /* xmm0 = 操作数 */
                /* Load sign mask into xmm1: mov rdx, 0x8000000000000000; movq xmm1, rdx */
                emit_mov_reg_imm64(cb, JIT_RDX, 0x8000000000000000ULL);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RDX & 7));
                emit_xorpd_xmm_xmm(cb, 0, 1);
                EMIT_MOVQ_RAX_XMM0();
                end_jmp = emit_jmp(cb);
                patch_rel32(cb, ta, cb->len);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jmp(cb); patch_add(ctx, _p, -1, 0); }
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                break;
            }
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

