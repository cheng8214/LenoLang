/* ============================================================================
 * ops_arith.inc - Integer arithmetic + bitwise cases (add/sub/mul/mod/neg/cast_int, and/or/xor/not)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Arithmetic ---- */
            case OP_ADD_INT:
                TOS_CONSUME_TO(JIT_RDX);   /* right → RDX */
                TOS_CONSUME_TO(JIT_RAX);   /* left → RAX */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            /* ---- 通用 ADD（对齐解释器 OP_ADD 语义）----
             * 1) int48 + int48 → 整数加（溢出检查；溢出交解释器走 BigInt）
             * 2) 两边都不是 NaN-boxed（即都是裸数值）→ float 加
             *    （int48 操作数用 CVTSI2SD 提升；结果保持 float，同 val_float）
             * 3) 任一是 NaN-boxed（字符串/对象/null）→ concat callout
             *    （任一 ObjString 即拼接；双方都非字符串时返回 NULL_VAL →
             *     bailout 回解释器保持原语义）---- */
            case OP_ADD: {
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_r, tagged_jcc_l;      /* NaN-boxed → concat */
                int end_jmp, skip_cat_jmp;

                /* b（右）→ RAX，a（左）→ RDX */
                TOS_CONSUME_RAX();
                TOS_CONSUME_TO(JIT_RDX);

                /* b（RDX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                /* a（RAX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径 */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);              /* 跳过慢路径 */

                /* ---- 慢路径：float 加 / concat 分派 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                /* 两个操作数都规范化为 double；任一是 NaN-boxed 则跳去 concat */
                EMIT_NUM_TO_XMM(0, JIT_RDX, JIT_R8, tagged_jcc_r);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RAX, JIT_R8, tagged_jcc_l);   /* xmm1 = b */

                emit_sse2_rr(cb, 0x58, 0, 1);        /* ADDSD xmm0, xmm1 */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                skip_cat_jmp = emit_jmp(cb);         /* 跳过 concat 段 */

                /* ---- concat 段：jit_callout_concat(result = a+b) ----
                 * 栈序：先弹右操作数（RAX），后弹左操作数（RDX）。
                 * concat(左, 右) → arg1=RDX(左)、arg2=RAX(右)。 */
                patch_rel32(cb, tagged_jcc_r, cb->len);
                patch_rel32(cb, tagged_jcc_l, cb->len);

                /* RAX=右(raw), RDX=左(raw) → Value（int 转 int48 Value，对象保持） */
                EMIT_RAW_TO_VALUE();                 /* RAX = Value(右) */
                emit_push_reg(cb, JIT_RAX);          /* 暂存 Value(右) */
                emit_mov_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_RAW_TO_VALUE();                 /* RAX = Value(左) */
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);   /* RDX = Value(左) */
                emit_pop_reg(cb, JIT_RAX);           /* RAX = Value(右) */

                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);  /* arg1 = Value(左) */
                emit_mov_rr(cb, JIT_ARG2, JIT_RAX);  /* arg2 = Value(右) */
                EMIT_CALL(jit_callout_concat);
                EMIT_CALLOUT_END();

                /* 结果 top16==0xfff8（NULL_VAL）→ concat 不支持 → bailout */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                {   /* shr r8, 48 */
                    int _b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, _b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                    emit_byte(cb, 48);
                }
                {   /* cmp r8, 0xfff8 */
                    int _b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, _b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFF8);
                }
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _p = emit_jcc(cb, 0x84);   /* je → bailout（双方均非字符串） */
                    patch_add(ctx, _p, -1, 0);
                }

                /* 成功：结果透传为 TOS（float 为裸 double；concat 结果为字符串 Value，
                 * 不做 int48 转换） */
                patch_rel32(cb, end_jmp, cb->len);
                patch_rel32(cb, skip_cat_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- 通用 SUB（对齐解释器 OP_SUB 语义）----
             * 1) int48 - int48 → 整数减（溢出交解释器走 BigInt）
             * 2) 两边都不是 NaN-boxed（即都是裸数值）→ float 减
             *    （int48 操作数用 CVTSI2SD 提升；结果保持 float，同 val_float）
             * 3) 任一是 NaN-boxed（对象/null/bool）→ bailout
             *    （BigInt 运算与类型错误交回解释器）---- */
            case OP_SUB: {
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_a, tagged_jcc_b;      /* NaN-boxed → bailout */
                int end_jmp, skip_bail_jmp;

                /* b（右）→ RDX，a（左）→ RAX。
                 * 必须先把两个操作数都取到寄存器再判定类型：慢路径的 float 加
                 * 需要完整的 a/b，若把判定夹在两次消费之间，从 b 的判定跳走时
                 * a 还没被 pop 出来（被跳过的 pop 会让 RAX 停在 b 上）。 */
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_RAX();

                /* b（RDX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                /* a（RAX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径：RAX = a - b */
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);

                /* ---- 慢路径：float 减 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, tagged_jcc_a);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RDX, JIT_R8, tagged_jcc_b);   /* xmm1 = b */
                emit_sse2_rr(cb, 0x5C, 0, 1);        /* SUBSD xmm0, xmm1 */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                skip_bail_jmp = emit_jmp(cb);

                /* ---- bailout 桩：NaN-boxed 操作数交回解释器 ---- */
                patch_rel32(cb, tagged_jcc_a, cb->len);
                patch_rel32(cb, tagged_jcc_b, cb->len);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                {
                    int _p = emit_jmp(cb);
                    patch_add(ctx, _p, -1, 0);
                }

                patch_rel32(cb, end_jmp, cb->len);
                patch_rel32(cb, skip_bail_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_MUL_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK(bc_off);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MOD_INT:
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                TOS_PRODUCE_FROM_RDX();
                vstack--;
                break;
            case OP_NEG_INT:
                TOS_CONSUME_RAX();
                emit_neg_reg(cb, JIT_RAX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                break;
            case OP_CAST_INT:
                /* no-op: value is already int in JIT context */
                break;

            /* ---- Extra integer ops ---- */
            case OP_DIV_INT: {
                /* int / int -> float result (pushed as raw double bits) */
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                /* Convert quotient (int64 in RAX) to double in XMM0 */
                emit_cvtsi2sd(cb, 0 /* XMM0 */, JIT_RAX);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66);
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E);
                emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_BITAND:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_and_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_or_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITXOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_xor_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITNOT:
                TOS_CONSUME_RAX();
                {
                    int b = 0;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xF7);
                    emit_byte(cb, modrm(3, 2, JIT_RAX & 7));  /* /2 = NOT */
                }
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                break;

