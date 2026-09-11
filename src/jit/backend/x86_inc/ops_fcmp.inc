/* ============================================================================
 * ops_fcmp.inc - Float comparison cases (EQ/LT/GT/LE/GE_FLOAT, SSE2 unsigned-cmp -> setcc)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 *
 * NaN 语义与 VM 保持一致（IEEE）：与 NaN 的比较除 != 外一律为 false。
 * UCOMISD 在无序（NaN）时置 PF=ZF=CF=1，据此：
 *   GT 用 JA  (CF=0 && ZF=0) → 无序时结果为 0，天然正确
 *   GE 用 JAE (CF=0)         → 无序时结果为 0，天然正确
 *   EQ/LT/LE 的 setcc 在无序时给出 1，必须再 AND「非 PF」把无序压成 false。
 * 旧实现在这里遇到 NaN 直接 bailout，会把整个热循环永久踢回解释器
 * （例如鼠标坐标来自 SDL 时可能为 NaN 的比较）。
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
                /* SETcc based on comparison type.
                 * UCOMISD sets CF/ZF/PF but clears SF/OF, so we MUST use
                 * unsigned condition codes (CF-based), NOT signed (SF/OF-based). */
                uint8_t cc;
                int need_pf_mask = 0;   /* 1 = 无序时 setcc 会误判为真，需屏蔽 */
                switch (op) {
                    case OP_EQ_FLOAT: cc = 0x4; need_pf_mask = 1; break;  /* ZF=1 → equal */
                    case OP_LT_FLOAT: cc = 0x2; need_pf_mask = 1; break;  /* CF=1 → below (unsigned less) */
                    case OP_GT_FLOAT: cc = 0x7; break;  /* CF=0 and ZF=0 → above (unsigned greater) */
                    case OP_LE_FLOAT: cc = 0x6; need_pf_mask = 1; break;  /* CF=1 or ZF=1 → below or equal */
                    case OP_GE_FLOAT: cc = 0x3; break;  /* CF=0 → above or equal */
                    default: cc = 0x4; break;
                }
                emit_setcc_reg(cb, cc, JIT_RAX);
                if (need_pf_mask) {
                    /* SETNP DL (0F 9B /0 — PF=0 → 有序) */
                    emit_setcc_reg(cb, 0x0B, JIT_RDX);
                    /* AND AL, DL (20 /r，8 位，AL/DL 无需 REX) */
                    emit_byte(cb, 0x20);
                    emit_byte(cb, modrm(3, JIT_RDX & 7, JIT_RAX & 7));
                }
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                TOS_PRODUCE();
                vstack--;
                break;
            }
