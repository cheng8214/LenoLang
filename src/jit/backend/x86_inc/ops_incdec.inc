/* ============================================================================
 * ops_incdec.inc - Stack-top increment/decrement and logical NOT cases
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Stack-top increment/decrement (int or float dispatch) ---- */
            case OP_INC: case OP_DEC: {
                int is_inc = (op == OP_INC);
                TOS_CONSUME_RAX();
                /* Type dispatch: check if int48 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                /* ja .is_float (rel8 placeholder) */
                emit_byte(cb, 0x77);
                int flt_patch = cb->len;
                emit_byte(cb, 0x00);
                /* Int path: add/sub 1 */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, is_inc ? 0xC0 : 0xE8);
                emit_byte(cb, 0x01);
                EMIT_INT48_CHECK(bc_off);
                TOS_PRODUCE();
                /* jmp .next (rel8 placeholder) */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                /* Float path: movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* Load 1.0 or -1.0 into xmm1 */
                emit_mov_reg_imm64(cb, JIT_RDX, is_inc ? 0x3FF0000000000000ULL : 0xBFF0000000000000ULL);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 1, JIT_RDX & 7));
                /* addsd xmm0, xmm1 (0x58) or subsd (0x5C) */
                emit_sse2_rr(cb, is_inc ? 0x58 : 0x5C, 0, 1);
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                TOS_PRODUCE();
                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                break;
            }
            /* ---- Logical NOT ----
             * 语义对齐解释器 OP_NOT（val_bool(is_falsey(v))），结果固定产出
             * **NaN-boxed bool**（与 OP_TRUE/OP_FALSE、解释器产出一致）。
             * 假值集合（val_is_truthy 的否定）：
             *   int48 0 / 裸 double ±0.0 / FALSE_VAL / NULL_VAL
             * 其余（非 0 数、NaN、对象、TRUE_VAL）→ 真。
             *
             * 旧实现只区分「int48 / 其它」，把 NaN-boxed 的 TRUE_VAL / FALSE_VAL /
             * null 当裸 double 走 ucomisd：NaN 比较是 unordered（ZF=1），`sete`
             * 恒得 1 —— 于是 `not <任何 bool>` 恒为真。五子棋 bestMove 里
             * `if not nearStone(...) { continue }` 因此永远 continue，候选点恒为空，
             * found 恒 false → 每步都落天元（AI 对 AI 全部下在一个点）。 */
            case OP_NOT: {
                TOS_CONSUME_RAX();
                /* 1) int48 判定 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                int not_int_p = emit_jcc(cb, 0x87);        /* ja → 非 int48 */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                int int_zero_p = emit_jcc(cb, 0x84);       /* int48 == 0 → .true */
                int int_done_p = emit_jmp(cb);             /* → .false */

                /* 2) 非 int48：NaN-boxed（无符号 >= JIT_NAN_SIG）还是裸 double？ */
                patch_rel32(cb, not_int_p, cb->len);
                emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int not_boxed_p = emit_jcc(cb, 0x82);      /* jb → 裸 double */

                /* NaN-boxed：只有 FALSE_VAL / NULL_VAL 是假值，其余（对象/true）为真 */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int boxed_false_p = emit_jcc(cb, 0x84);    /* FALSE_VAL → .true */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int boxed_null_p = emit_jcc(cb, 0x84);     /* NULL_VAL → .true */
                int boxed_done_p = emit_jmp(cb);           /* → .false */

                /* 3) 裸 double：== 0.0（含 -0.0）为假；NaN 为真（val_is_truthy 语义） */
                patch_rel32(cb, not_boxed_p, cb->len);
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x0F); emit_byte(cb, 0x57);
                emit_byte(cb, modrm(3, 1, 1));                                 /* xorpd xmm1, xmm1 */
                emit_ucomisd_rr(cb, 0, 1);
                int flt_nan_p  = emit_jcc(cb, 0x8A);       /* jp（unordered = NaN）→ .false */
                int flt_zero_p = emit_jcc(cb, 0x84);       /* == 0.0 → .true */
                int flt_done_p = emit_jmp(cb);             /* → .false */

                /* 4) 结果出口：.true → TRUE_VAL，.false → FALSE_VAL */
                patch_rel32(cb, int_zero_p, cb->len);
                patch_rel32(cb, boxed_false_p, cb->len);
                patch_rel32(cb, boxed_null_p, cb->len);
                patch_rel32(cb, flt_zero_p, cb->len);
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)TRUE_VAL);
                int true_done_p = emit_jmp(cb);
                patch_rel32(cb, int_done_p, cb->len);
                patch_rel32(cb, boxed_done_p, cb->len);
                patch_rel32(cb, flt_nan_p, cb->len);
                patch_rel32(cb, flt_done_p, cb->len);
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)FALSE_VAL);
                patch_rel32(cb, true_done_p, cb->len);
                TOS_PRODUCE();
                break;
            }
