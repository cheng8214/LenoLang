/* ============================================================================
 * ops_return.inc - Return / module-call cases (RETURN, RETURN_MULTI, MODULE_CALL incl. inline-return handling)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Inline return handling ---- */
            case OP_RETURN: {
                if (inline_depth > 0) {
                    /* In inline mode, OP_RETURN's value is already on vstack.
                     * We just emit a jump to the inline_end label (to be patched
                     * when the callee body ends). The return value stays on the
                     * virtual stack as the call result. */
                    TOS_SPILL();  /* ensure value is on memory stack */
                    /* Emit: jmp <inline_end> (rel32 placeholder) */
                    emit_byte(cb, 0xE9);
                    int patch_loc = cb->len;
                    emit_uint32(cb, 0);
                    inline_ret_patches[inline_ret_patch_cnt++] = patch_loc;
                    /* Match scan phase: vstack-- (return value "consumed").
                     * The value stays on the hardware stack for inline_end,
                     * but vstack tracking must match scan for dead code after
                     * the return to have correct vstack. */
                    vstack--;
                } else {
                    /* Shouldn't happen in loop body, but handle gracefully */
                    TOS_SPILL();
                    vstack--;
                }
                break;
            }
            case OP_RETURN_MULTI: {
                uint8_t rc = ip[1];
                if (inline_depth > 0) {
                    /* Multiple return values are on vstack. Emit jump to inline_end. */
                    TOS_SPILL();
                    emit_byte(cb, 0xE9);
                    int patch_loc = cb->len;
                    emit_uint32(cb, 0);
                    inline_ret_patches[inline_ret_patch_cnt++] = patch_loc;
                    /* Match scan phase: vstack -= rc.
                     * The return values stay on the hardware stack for
                     * inline_end, but vstack tracking must match scan for
                     * dead code after the return. */
                    vstack -= rc;
                } else {
                    TOS_SPILL();
                    vstack -= rc;
                }
                break;
            }

            /* ---- Module call (e.g., maths.sqrt) ---- */
            case OP_MODULE_CALL: {
                uint16_t module_idx = rd_short(ip + 1);
                uint16_t method_idx = rd_short(ip + 3);
                int arg_count = rd_short(ip + 5);
                TOS_SPILL();
                /* Save RSP before callout */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);  /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);    /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)module_idx);   /* module_idx (3rd) */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)method_idx);   /* method_idx (4th) */
                /* 5th param (chunk ptr): Windows x64 passes it as the first
                 * stack argument at [RSP+32]; System V passes it in R8. */
#ifdef _WIN32
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)(uintptr_t)cur_chunk);
                emit_mov_mem8_reg(cb, JIT_RSP, 32, JIT_RAX);
#else
                emit_mov_reg_imm64(cb, JIT_ARG5, (uint64_t)(uintptr_t)cur_chunk);
#endif
                EMIT_CALL(jit_callout_module_call);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop arg_count args, push 1 result */
                int pop_bytes2 = arg_count * 8;
                if (pop_bytes2 <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes2);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes2);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

