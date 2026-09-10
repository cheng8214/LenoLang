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
            /* ---- 通用 ADD：int fast path；任一操作数非 int48 → 字符串拼接
             * callout（对齐解释器 OP_ADD：任一 ObjString 即拼接）。concat 返回
             * NULL_VAL（双方均非字符串）→ bailout 回解释器保持原语义。---- */
            case OP_ADD: {
                int concat_jcc_r, concat_jcc_l, end_jmp;

                /* a → RAX，b → RDX */
                TOS_CONSUME_RAX();
                TOS_CONSUME_TO(JIT_RDX);

                /* b（RDX）非 int48 → concat 分支 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                concat_jcc_r = emit_jcc(cb, 0x85);   /* jne concat */

                /* a（RAX）非 int48 → concat 分支 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                concat_jcc_l = emit_jcc(cb, 0x85);   /* jne concat */

                /* int 快路径 */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);              /* 跳过 concat 段 */

                /* ---- concat 段：jit_callout_concat(result = a+b) ----
                 * 栈序：先弹右操作数（RAX），后弹左操作数（RDX）。
                 * concat(左, 右) → arg1=RDX(左)、arg2=RAX(右)。 */
                patch_rel32(cb, concat_jcc_r, cb->len);
                patch_rel32(cb, concat_jcc_l, cb->len);

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
                { int _p = emit_jcc(cb, 0x84); patch_add(ctx, _p, -1, 0); }  /* je bailout */

                /* 成功：结果（字符串 Value）透传为 TOS，不做 int48 转换 */
                patch_rel32(cb, end_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- 通用 SUB：int only，输入或结果非 int48 → bailout ---- */
            case OP_SUB: {
                /* 检查 b（RDX）是 int48 */
                TOS_CONSUME_TO(JIT_RDX);
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                { int _p = emit_jcc(cb, 0x87); patch_add(ctx, _p, -1, 0); }
                /* 检查 a（RAX）是 int48 */
                TOS_CONSUME_RAX();
                EMIT_INT48_CHECK(bc_off);
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
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

