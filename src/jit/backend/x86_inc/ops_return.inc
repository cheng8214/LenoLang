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

                /* ---- ffi.read_int / ffi.write_int 内联 ----
                 * 这两个方法在像素直写循环里每像素各一次，走通用 callout 每次要付
                 * 「逐个装箱成 Value 数组 + native 调用 + 异常检查」（实测 ~20ns），
                 * 而它们真正做的只是一次 4 字节 memcpy。这里直接生成 load/store；
                 * 任一前置检查不过就写 site 后 bailout 交解释器，保证报错文本
                 * （"空指针引用或指针已释放" / "内存访问越界"）与语义完全一致。 */
                if (mod_meta && strcmp(mod_meta->module_name, "ffi") == 0) {
                    int ffi_is_read  = (arg_count == 2 && strcmp(mod_meta->method_name, "read_int") == 0);
                    int ffi_is_write = (arg_count == 3 && strcmp(mod_meta->method_name, "write_int") == 0);
                    if (ffi_is_read || ffi_is_write) {
                        /* 实参在 JIT 虚拟栈上：read=[ptr, off]，write=[ptr, off, value] */
                        TOS_SPILL();
                        if (ffi_is_write) {
                            emit_pop_reg(cb, JIT_RCX);            /* RCX = value（raw） */
                            /* value 必须是 int48（对应解释器的 val_as_int） */
                            emit_mov_rr(cb, JIT_RDX, JIT_RCX);
                            emit_sar_imm(cb, JIT_RDX, 47);
                            emit_inc_reg(cb, JIT_RDX);
                            emit_cmp_reg_imm8(cb, JIT_RDX, 1);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int fw_val_notint = emit_jcc(cb, 0x87);   /* JA → bailout */
                            patch_add(ctx, fw_val_notint, -1, 0);
                        }
                        emit_pop_reg(cb, JIT_RAX);                /* RAX = offset（raw int48） */
                        emit_pop_reg(cb, JIT_R8);                 /* R8  = ptr（NaN-boxed Value） */

                        /* 1) offset 必须是 int48（解释器 parse_offset 对非 int 按 0 处理） */
                        emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                        emit_sar_imm(cb, JIT_RDX, 47);
                        emit_inc_reg(cb, JIT_RDX);
                        emit_cmp_reg_imm8(cb, JIT_RDX, 1);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_off_notint = emit_jcc(cb, 0x87);        /* JA → bailout */
                        patch_add(ctx, f_off_notint, -1, 0);

                        /* 2) ptr 必须是 NaN-boxed 对象（top16 == 0xFFFC = TAG_OBJ） */
                        emit_mov_rr(cb, JIT_RDX, JIT_R8);
                        {   /* shr rdx, 48 */
                            int _b = (JIT_RDX >> 3) & 1;
                            emit_byte(cb, rex(1, 0, 0, _b));
                            emit_byte(cb, 0xC1);
                            emit_byte(cb, modrm(3, 5, JIT_RDX & 7));
                            emit_byte(cb, 48);
                        }
                        {   /* cmp rdx, 0x0000FFFC */
                            int _b = (JIT_RDX >> 3) & 1;
                            emit_byte(cb, rex(1, 0, 0, _b));
                            emit_byte(cb, 0x81);
                            emit_byte(cb, modrm(3, 7, JIT_RDX & 7));
                            emit_uint32(cb, 0x0000FFFC);
                        }
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_not_obj = emit_jcc(cb, 0x85);           /* JNE → bailout */
                        patch_add(ctx, f_not_obj, -1, 0);
                        emit_and_rr(cb, JIT_R8, JIT_R10);             /* R8 = Object* */

                        /* 3) obj->type == OBJ_FFI_POINTER（type 是 Object 首字段，偏移 0） */
                        {
                            int _b = (JIT_R8 >> 3) & 1;
                            emit_byte(cb, rex(0, 0, 0, _b));
                            emit_byte(cb, 0x80);
                            emit_byte(cb, modrm(1, 7, JIT_R8 & 7));
                            emit_byte(cb, 0x00);
                            emit_byte(cb, (uint8_t)OBJ_FFI_POINTER);
                        }
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_not_ptr = emit_jcc(cb, 0x85);           /* JNE → bailout */
                        patch_add(ctx, f_not_ptr, -1, 0);

                        /* 4) CHECK_NULL_PTR：ptr->ptr != NULL 且 !freed
                         * 用 cmp qword [r8+disp], 0 形式，不额外占寄存器 ——
                         * Win64 下 RSI/RDI 是**非易失**寄存器，而 JIT 序言只保存
                         * rbp/rbx/r12-r14，所以这里只能用易失寄存器 RAX/RCX/RDX/R8。 */
                        {
                            int _b = (JIT_R8 >> 3) & 1;
                            emit_byte(cb, rex(1, 0, 0, _b));          /* REX.W */
                            emit_byte(cb, 0x83);
                            emit_byte(cb, modrm(1, 7, JIT_R8 & 7));
                            emit_byte(cb, (uint8_t)offsetof(ObjFFIPointer, ptr));
                            emit_byte(cb, 0x00);
                        }
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_null = emit_jcc(cb, 0x84);              /* JE → bailout */
                        patch_add(ctx, f_null, -1, 0);
                        {
                            int _b = (JIT_R8 >> 3) & 1;
                            emit_byte(cb, rex(0, 0, 0, _b));
                            emit_byte(cb, 0x83);
                            emit_byte(cb, modrm(1, 7, JIT_R8 & 7));
                            emit_byte(cb, (uint8_t)offsetof(ObjFFIPointer, freed));
                            emit_byte(cb, 0x00);
                        }
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_freed = emit_jcc(cb, 0x85);             /* JNE → bailout */
                        patch_add(ctx, f_freed, -1, 0);

                        /* 5) CHECK_BOUNDS：仅当 owned && size > 0 校验 off+4 <= size
                         *    （offset 为负时无符号比较必然越界，与解释器一致） */
                        {
                            int _b = (JIT_R8 >> 3) & 1;
                            emit_byte(cb, rex(0, 0, 0, _b));
                            emit_byte(cb, 0x83);
                            emit_byte(cb, modrm(1, 7, JIT_R8 & 7));
                            emit_byte(cb, (uint8_t)offsetof(ObjFFIPointer, owned));
                            emit_byte(cb, 0x00);
                        }
                        int f_not_owned = emit_jcc(cb, 0x84);         /* JE → 跳过边界检查 */
                        emit_mov_reg_mem8(cb, JIT_RDX, JIT_R8, (int8_t)offsetof(ObjFFIPointer, size));
                        emit_test_rr(cb, JIT_RDX, JIT_RDX);
                        int f_size0 = emit_jcc(cb, 0x84);             /* JE → 跳过边界检查 */
                        EMIT_STORE_TMP(tmp1_disp, JIT_RAX);           /* 暂存 offset */
                        /* add rax, 4（RAX 仅用于比较，之后从 tmp1 恢复） */
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83);
                        emit_byte(cb, 0xC0); emit_byte(cb, 0x04);
                        emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int f_oob = emit_jcc(cb, 0x87);               /* JA → bailout（无符号） */
                        patch_add(ctx, f_oob, -1, 0);
                        EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);            /* 恢复 offset */
                        patch_rel32(cb, f_not_owned, cb->len);
                        patch_rel32(cb, f_size0, cb->len);

                        /* 6) 访存：RDX = ptr->ptr（基址），RAX = offset（索引） */
                        emit_mov_reg_mem8(cb, JIT_RDX, JIT_R8, (int8_t)offsetof(ObjFFIPointer, ptr));
                        if (ffi_is_read) {
                            /* movsxd rax, dword [rdx + rax]：结果本身就是 raw int48 */
                            emit_byte(cb, 0x48); emit_byte(cb, 0x63);
                            emit_byte(cb, 0x04); emit_byte(cb, 0x02);
                        } else {
                            /* mov dword [rdx + rax], ecx：只写低 32 位（(int32_t)value） */
                            emit_byte(cb, 0x89);
                            emit_byte(cb, 0x0C); emit_byte(cb, 0x02);
                            /* write_int 返回 val_null()：与通用路径一致地压 NULL_VAL */
                            emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)NULL_VAL);
                        }
                        TOS_PRODUCE();
                        vstack -= (arg_count - 1);
                        break;
                    }
                }

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

