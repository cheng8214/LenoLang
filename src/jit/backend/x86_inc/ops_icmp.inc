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
                /* VM 快路径仅 imm < 32；>= 32 走 BigInt 路径 → 无条件 bailout */
                if (imm >= 32) {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _p = emit_jmp(cb);
                    patch_add(ctx, _p, -1, 0);
                    TOS_PRODUCE();
                    break;
                }
                emit_shl_imm(cb, JIT_RAX, (uint8_t)imm);
                /* VM 语义：int64 左移后 val_int() 截断为 48 位，不 bailout */
                EMIT_INT48_TRUNCATE();
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

            /* ---- 通用比较（对齐解释器 OP_LT..OP_GE）----
             * 1) int48 对 int48 → 有符号整数比较
             * 2) 两边都不是 NaN-boxed → float 比较（UCOMISD + 无符号 CC）；
             *    无序（NaN）按 IEEE 语义：< > <= >= 一律 false（不再 bailout）
             * 3) 任一是 NaN-boxed（字符串/bigint/类型错误）→ bailout ---- */
            case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
                uint8_t cc;    /* 整数比较用有符号 CC（CMP 后 SF/OF 有效） */
                uint8_t fcc;   /* float 比较用无符号 CC（UCOMISD 只置 CF/ZF/PF） */
                switch (op) {
                    case OP_LT: cc = 0xC; fcc = 0x2; break;  /* JL  / JB  */
                    case OP_GT: cc = 0xF; fcc = 0x7; break;  /* JG  / JA  */
                    case OP_LE: cc = 0xE; fcc = 0x6; break;  /* JLE / JBE */
                    case OP_GE: cc = 0xD; fcc = 0x3; break;  /* JGE / JAE */
                    default:    cc = 0xC; fcc = 0x2; break;
                }
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_a, tagged_jcc_b;      /* NaN-boxed → bailout */
                int end_jmp, skip_bail_jmp;

                /* b（右）→ RDX，a（左）→ RAX。
                 * 必须先把两个操作数都取到寄存器再判定类型：慢路径的 float 比较
                 * 需要完整的 a/b，若把判定夹在两次消费之间，从 b 的判定跳走时
                 * a 还没被 pop 出来（被跳过的 pop 会让 RAX 停在 b 上）。 */
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_RAX();

                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径 */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                end_jmp = emit_jmp(cb);

                /* ---- 慢路径：float 比较 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, tagged_jcc_a);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RDX, JIT_R8, tagged_jcc_b);   /* xmm1 = b */
                emit_ucomisd_rr(cb, 0, 1);                           /* flags: a ? b */
                /* NaN（无序）按 IEEE 语义处理，不再 bailout：
                 * JA/JAE 在无序时结果为 0；JB/JBE 的 setcc 会给出 1，
                 * 需再 AND「非 PF」把无序压成 false */
                emit_setcc_reg(cb, fcc, JIT_RAX);
                if (op == OP_LT || op == OP_LE) {
                    emit_setcc_reg(cb, 0x0B, JIT_RDX);   /* SETNP DL */
                    emit_byte(cb, 0x20);                 /* AND AL, DL */
                    emit_byte(cb, modrm(3, JIT_RDX & 7, JIT_RAX & 7));
                }
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                skip_bail_jmp = emit_jmp(cb);

                /* ---- bailout 桩（仅 NaN-boxed 操作数：字符串/bigint 等）---- */
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

            /* ---- 通用移位（int fast path）：计数与值均须 int48，计数须 0..31，
             * 移位计数放 CL（RCX 承载 locals 指针，用 push/pop 保护） ---- */
            case OP_SHL: case OP_SHR: case OP_USHR: {
                TOS_CONSUME_TO(JIT_RDX);   /* count */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _p = emit_jcc(cb, 0x87);   /* jne → 计数非 int48 */
                    patch_add(ctx, _p, -1, 0);
                }
                /* count >= 32 → bailout（交还 VM 处理 bigint 语义） */
                emit_cmp_reg_imm8(cb, JIT_RDX, 32);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _p = emit_jcc(cb, 0x83);   /* JAE */
                    patch_add(ctx, _p, -1, 0);
                }
                TOS_CONSUME_RAX();         /* value */
                EMIT_INT48_CHECK(bc_off);
                /* RCX 是 locals 指针，必须保护；bailout 时会 mov rsp,rbp 重设，无需恢复 */
                emit_push_reg(cb, JIT_RCX);
                emit_mov_rr(cb, JIT_RCX, JIT_RDX);
                {
                    int b = (JIT_RAX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xD3);
                    emit_byte(cb, modrm(3, (op == OP_SHL) ? 4 : (op == OP_SHR) ? 7 : 5, JIT_RAX & 7));
                }
                emit_pop_reg(cb, JIT_RCX);
                /* VM 语义：OP_SHL 快路径(shift<32)是 int64 左移后 val_int() 截断为
                 * 48 位有符号，不提升 BigInt → JIT 也截断而不是 bailout。
                 * OP_SHR 为算术右移，结果必在原 int48 范围内；OP_USHR 结果可能
                 * 落回 int48 内也可能超出（> 2^47-1 时 VM 转 BigInt）→ 保留检查。 */
                if (op == OP_SHL) {
                    EMIT_INT48_TRUNCATE();
                } else {
                    EMIT_INT48_CHECK(bc_off);
                }
                TOS_PRODUCE();
                vstack--;
                break;
            }

