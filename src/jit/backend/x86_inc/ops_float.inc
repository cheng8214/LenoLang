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
                /* ---- §8.91：callout 失败必须 bailout（**这里原先漏了守卫** ✗）----
                 * `jit_callout_div` 的错误路径（除零 / 操作数非数字）只置
                 * `jit_callout_failed` + 返回 NULL_VAL ⇒ 没有这段检查时 JIT 会把 NULL
                 * 当结果继续跑：实测热循环里 250 次除零**全部静默成功**（`ok=250`），
                 * 而 `LENO_NO_JIT=1` 每次都抛（`caught=300`）⇒ try/catch 捕获次数与退出码
                 * 都对不上（`probe_error_channel_div.leno` 抓到的就是这个）。
                 * 加上守卫：bailout ⇒ 解释器重放本条指令 ⇒ 抛**可被 try/catch 捕获**的原文错误。 */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int dv_fail = emit_jcc(cb, 0x85);     /* JNZ → bailout */
                    patch_add(ctx, dv_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
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
                /* 解释器语义（vm/vminc/op_unary.inc OP_CAST_FLOAT）：
                 *   int/float → double；bool → 1.0/0.0；BigInt → double；
                 *   null 保持 null；其余抛「类型转换 float: 操作数必须是数字」。
                 * JIT 的四态分派：
                 *   1) int48            → CVTSI2SD
                 *   2) 裸 double        → 原样（已达标）
                 *   3) TRUE_VAL/FALSE_VAL → 1.0/0.0（比较结果走这里；缺了这分支，
                 *      比较结果的新表示会以 NaN-boxed 形式原样漏进 float slot）
                 *   4) NULL_VAL         → 原样（解释器也保持 null）
                 * 其余 NaN-boxed（BigInt/对象）bailout 交解释器，语义与报错天然一致。 */
                TOS_CONSUME_RAX();
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .not_int (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: convert int64 to double */
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                /* movq rax, xmm0 */
                EMIT_MOVQ_RAX_XMM0();
                int cf_done_p = emit_jmp(cb);              /* → .done */

                /* .not_int: 裸 double 与 NaN-boxed 分流 */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int cf_double_p = emit_jcc(cb, 0x82);      /* jb → 裸 double，原样 */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int cf_true_p = emit_jcc(cb, 0x84);        /* JE → 1.0 */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int cf_false_p = emit_jcc(cb, 0x84);       /* JE → 0.0 */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int cf_null_p = emit_jcc(cb, 0x84);        /* JE → 原样（null） */
                /* 其余 NaN-boxed（BigInt/对象/字符串）→ bailout 交解释器 */
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jmp(cb); patch_add(ctx, _p, -1, 0); }

                /* .bool_true: 1 → 复用整数转换路径 */
                patch_rel32(cb, cf_true_p, cb->len);
                emit_mov_reg_imm64(cb, JIT_RAX, 1);
                int cf_true_done_p = emit_jmp(cb);         /* → .int_conv */
                /* .bool_false: 0 */
                patch_rel32(cb, cf_false_p, cb->len);
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                /* .int_conv: rax（0/1）→ double */
                patch_rel32(cb, cf_true_done_p, cb->len);
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                EMIT_MOVQ_RAX_XMM0();
                int cf_bool_done_p = emit_jmp(cb);         /* → .done */

                /* .not_boxed（裸 double）与 .null：RAX 已经是对的值，直接落 .done */
                patch_rel32(cb, cf_double_p, cb->len);
                patch_rel32(cb, cf_null_p, cb->len);
                /* .done */
                patch_rel32(cb, cf_done_p, cb->len);
                patch_rel32(cb, cf_bool_done_p, cb->len);
                TOS_PRODUCE();
                break;
            }

