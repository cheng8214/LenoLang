/* ============================================================================
 * ops_fcmp.inc - Float comparison cases (EQ/LT/GT/LE/GE_FLOAT, SSE2 unsigned-cmp -> setcc)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            case OP_EQ_FLOAT: case OP_LT_FLOAT: case OP_GT_FLOAT:
            case OP_LE_FLOAT: case OP_GE_FLOAT: {
                TOS_CONSUME_TO(JIT_RAX);  /* b */
                TOS_CONSUME_TO(JIT_RDX);  /* a */
                /* movq xmm0, rdx; movq xmm1, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RDX & 7));
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RAX & 7));
                /* UCOMISD xmm0, xmm1 → sets EFLAGS for xmm0 OP xmm1 */
                emit_ucomisd_rr(cb, 0, 1);
                /* Need to handle unordered (NaN) case: PF=1 means unordered */
                /* For simplicity, if NaN, bail out (jp bailout) */
                {
                    int jp_patch = emit_jcc(cb, 0x8A);  /* JP (unordered/NaN) → bailout */
                    patch_add(ctx, jp_patch, -1, 0);
                }
                /* SETcc based on comparison type.
                 * UCOMISD sets CF/ZF/PF but clears SF/OF, so we MUST use
                 * unsigned condition codes (CF-based), NOT signed (SF/OF-based). */
                uint8_t cc;
                switch (op) {
                    case OP_EQ_FLOAT: cc = 0x4; break;  /* ZF=1 → equal */
                    case OP_LT_FLOAT: cc = 0x2; break;  /* CF=1 → below (unsigned less) */
                    case OP_GT_FLOAT: cc = 0x7; break;  /* CF=0 and ZF=0 → above (unsigned greater) */
                    case OP_LE_FLOAT: cc = 0x6; break;  /* CF=1 or ZF=1 → below or equal */
                    case OP_GE_FLOAT: cc = 0x3; break;  /* CF=0 → above or equal */
                    default: cc = 0x4; break;
                }
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                vstack--;
                break;
            }
