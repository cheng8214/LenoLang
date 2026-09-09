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
            /* ---- Logical NOT (int or float dispatch) ---- */
            case OP_NOT: {
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
                /* Int path: test rax, rax; sete al; movzx eax, al */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                emit_byte(cb, 0x0F); emit_byte(cb, 0x94); emit_byte(cb, modrm(3, 0, JIT_RAX & 7)); /* sete al */
                emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xB6); emit_byte(cb, modrm(3, JIT_RAX & 7, JIT_RAX & 7)); /* movzx rax, al */
                TOS_PRODUCE();
                /* jmp .next */
                emit_byte(cb, 0xEB);
                int next_patch = cb->len;
                emit_byte(cb, 0x00);
                /* .is_float: patch ja to here */
                cb->buf[flt_patch] = (uint8_t)(cb->len - (flt_patch + 1));
                /* Float path: movq xmm0, rax */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                /* xorpd xmm1, xmm1 (zero) */
                emit_byte(cb, 0x66); emit_byte(cb, 0x0F); emit_byte(cb, 0x57); emit_byte(cb, modrm(3, 1, 1));
                /* ucomisd xmm0, xmm1 → ZF=1 if equal */
                emit_ucomisd_rr(cb, 0, 1);
                /* sete al; movzx rax, al */
                emit_byte(cb, 0x0F); emit_byte(cb, 0x94); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xB6); emit_byte(cb, modrm(3, JIT_RAX & 7, JIT_RAX & 7));
                TOS_PRODUCE();
                /* .next: patch jmp to here */
                cb->buf[next_patch] = (uint8_t)(cb->len - (next_patch + 1));
                break;
            }
