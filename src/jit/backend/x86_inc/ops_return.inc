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
                    /* After return, code is unreachable */
                    vstack = VSTACK_UNREACHABLE;
                } else if (ctx->func_mode) {
                    /* ---- 函数级 JIT：OP_RETURN 真正返回 ----
                     * 返回值在 RAX（TOS），转为 NaN-boxed Value 写入
                     * jit_fn_result，然后 epilogue 返回调用方。
                     * 不写回 locals（调用方使用临时 locals 数组）。 */
                    TOS_CONSUME_RAX();          /* result into RAX */
                    EMIT_RAW_TO_VALUE();        /* raw -> NaN-boxed Value */
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_fn_result);
                    emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                    emit_xor_eax_eax(cb);       /* jr=0: success (jit_fn_result holds the value) */
                    EMIT_EPILOGUE();
                    vstack--;
                    /* After return, code is unreachable */
                    vstack = VSTACK_UNREACHABLE;
                } else {
                    /* Shouldn't happen in loop body, but handle gracefully */
                    TOS_SPILL();
                    vstack--;
                    vstack = VSTACK_UNREACHABLE;
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
                    vstack = VSTACK_UNREACHABLE;
                } else if (ctx->func_mode) {
                    if (rc == 1) {
                        /* 单返回值：与 OP_RETURN 相同，真正返回 */
                        TOS_CONSUME_RAX();
                        EMIT_RAW_TO_VALUE();
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_fn_result);
                        emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                        emit_xor_eax_eax(cb);   /* jr=0: success */
                        EMIT_EPILOGUE();
                    } else {
                        /* 多返回值：函数级 JIT 不支持，bailout 回解释路径 */
                        emit_byte(cb, 0xE9);
                        int bail_patch = cb->len;
                        emit_uint32(cb, 0);
                        patch_add(ctx, bail_patch, -1, 0);
                    }
                    vstack -= rc;
                    vstack = VSTACK_UNREACHABLE;
                } else {
                    TOS_SPILL();
                    vstack -= rc;
                    vstack = VSTACK_UNREACHABLE;
                }
                break;
            }

            /* ---- Module call (e.g., maths.sqrt) ---- */
            case OP_MODULE_CALL: {
                uint16_t module_idx = rd_short(ip + 1);
                uint16_t method_idx = rd_short(ip + 3);
                int arg_count = rd_short(ip + 5);
                /* 编译期解析模块方法：成功则内嵌 meta 指针直接调用，
                 * 省掉每次调用一次的字符串哈希 + strcmp 查表
                 * （解释器侧用 inline cache 规避，JIT 之前没做）。 */
                ModuleMethodMeta* mod_meta =
                    jit_resolve_module_method(cur_chunk, module_idx, method_idx);

                /* ---- 通用「数值薄调用」快速路径（不含任何模块/方法名特判）----
                 * 判据完全来自模块方法自身注册的元信息：
                 *   - 参数个数 1..3，且 param_types[0..n-1] 全是 TYPE_FLOAT
                 *     （double 按位置走 xmm0..2，两个 ABI 一致）
                 *   - return_type == TYPE_FLOAT（double 结果在 xmm0）
                 * 满足则把实参按 raw→double 送进 xmm，直接调用通用薄桥
                 * jit_thin_fN：它内部仍调用该模块原本的 NativeFn，因此
                 * **数学实现只有模块里那一份**，JIT 不复制任何公式。
                 * 相比通用 callout，省掉了 vstack 数组、逐参数装箱、模块
                 * 方法查表与失败标志的完整往返（实测 ~20ns → ~8ns/次）。
                 * 实参若为 NaN-boxed 非数值（对应 get_number 的宽松语义），
                 * 则 bailout 交回解释器，保证结果与报错完全一致。 */
                int thin_ar = 0;
                if (mod_meta && arg_count >= 1 && arg_count <= 3 &&
                    mod_meta->return_type == TYPE_FLOAT && mod_meta->function) {
                    int all_f = 1;
                    for (int ai = 0; ai < arg_count; ai++) {
                        if (mod_meta->param_types[ai] != TYPE_FLOAT) { all_f = 0; break; }
                    }
                    if (all_f) thin_ar = arg_count;
                }
                if (thin_ar) {
                    TOS_SPILL();
                    int thin_tagged[4]; int thin_ntag = 0;
                    /* 实参在栈上按调用顺序排列，最后压入的在栈顶 → 倒序弹出 */
                    for (int ai = thin_ar - 1; ai >= 0; ai--) {
                        emit_pop_reg(cb, JIT_RAX);
                        EMIT_NUM_TO_XMM(ai, JIT_RAX, JIT_R8, thin_tagged[thin_ntag++]);
                    }
                    EMIT_CALLOUT_BEGIN();
                    /* 数据实参已在 xmm0..N-1；桥的最后一个参数是该模块原本的
                     * NativeFn（数学实现只有那一份）。它的寄存器位置两个 ABI
                     * 不同：Win64 按参数位置（第 N+1 位 → RDX/R8/R9），
                     * SysV 按第 1 个整型参数（→ RDI）。 */
#ifdef _WIN32
                    int thin_fn_reg = (thin_ar == 1) ? JIT_ARG2
                                    : (thin_ar == 2) ? JIT_ARG3 : JIT_ARG4;
#else
                    int thin_fn_reg = JIT_ARG1;
#endif
                    emit_mov_reg_imm64(cb, thin_fn_reg, (uint64_t)(uintptr_t)mod_meta->function);
                    EMIT_CALL(jit_thin_bridge_for(thin_ar));
                    EMIT_CALLOUT_END();
                    /* double 结果（xmm0）→ 裸 double 位模式存 RAX */
                    emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                    emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));   /* movq rax, xmm0 */
                    /* 桥内 native 抛错 → jit_callout_failed → bailout */
                    EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    {
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int fail_patch = emit_jcc(cb, 0x85);   /* JNZ → bailout */
                        patch_add(ctx, fail_patch, -1, 0);
                    }
                    emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                    emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                    EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                    int thin_jump = emit_jmp(cb);
                    /* 非数值实参的 bailout 桩 */
                    for (int ti = 0; ti < thin_ntag; ti++)
                        patch_rel32(cb, thin_tagged[ti], cb->len);
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    {
                        int bp = emit_jmp(cb);
                        patch_add(ctx, bp, -1, 0);
                    }
                    patch_rel32(cb, thin_jump, cb->len);
                    TOS_PRODUCE();
                    vstack -= (arg_count - 1);
                    break;
                }

                TOS_SPILL();
                /* Save RSP before callout */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);  /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);    /* arg_count (2nd) */
                if (mod_meta) {
                    emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)mod_meta); /* meta (3rd) */
                    EMIT_CALL(jit_callout_module_call_meta);
                } else {
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
                }
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
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

