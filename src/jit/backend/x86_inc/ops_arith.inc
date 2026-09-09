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
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MUL_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK();
                EMIT_INT48_CHECK();
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
                EMIT_INT48_CHECK();
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
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_or_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITXOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_xor_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK();
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
                EMIT_INT48_CHECK();
                TOS_PRODUCE();
                break;

