/* ============================================================================
 * ops_callout.inc - Callout cases (ACC_FIELDS, INVOKE_METHOD, GET_FIELD_FAST, CALL_GLOBAL_FUNC[_TYPED])
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Callout: OP_ACC_FIELDS (struct field accumulation) ---- */
            case OP_ACC_FIELDS: {
                /* Pop struct obj, convert to NaN-boxed, save to tmp */
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* struct obj */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* obj (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)ip[1]);  /* count (2nd arg) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)(ip + 2));  /* field_indices ptr (3rd) */
                EMIT_CALL(jit_callout_acc_fields);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                break;
            }
            /* ---- 裸 OP_CALL（callee 是运行时值：局部闭包 / 回调表元素 / 字段）—— §8.59 ----
             * 形态与记账见 jit_scan.c 的同名 case：栈 [args(ac)][callee]，返回后留 1 个值
             * （callout 在**调用前**复核 return_count，不符即 bailout 交解释器）。
             * 内存待消费 = callee(1) + args(ac) = ac + 1；内存保留 = rc - 1 = 0
             * ⇒ 弹 (ac + 1) 个槽（正是 §8.57 的 `argc - rc + 2` 在 rc=1 时的取值）。 */
            case OP_CALL: {
                int cv_argc = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);            /* vstack_top = TOS = callee */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                /* add rax, 8 —— 跳过 callee 槽，指向最后一个实参（与 §8.57 同款） */
                emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC0); emit_byte(cb, 0x08);
                emit_mov_rr(cb, JIT_ARG1, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)cv_argc);
                EMIT_CALL(jit_callout_call_value);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int cv_fail = emit_jcc(cb, 0x85);     /* JNZ → bailout */
                    patch_add(ctx, cv_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                {
                    int cv_pop = (cv_argc + 1) * 8;
                    if (cv_pop > 0) {
                        if (cv_pop <= 127) {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                            emit_byte(cb, (uint8_t)cv_pop);
                        } else {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                            emit_uint32(cb, (uint32_t)cv_pop);
                        }
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= cv_argc;
                break;
            }

            /* ---- OP_TAIL_CALL（尾调用，R6-b）—— 只对**函数级 JIT** 有意义 ----
             * VM 语义（op_call.inc:35-134）：关闭本帧 upvalue → 释放本帧 locals →
             * **复用本帧**执行 callee（真 TCO、永不返回本函数）。
             *
             * JIT 侧 = 「调用 + 把结果当作本函数的返回值」：
             *   1) callout 走 vm_call_value 完整路径（callee 可能是闭包 / 原生 / bound
             *      method / 裸函数），并按 VM **实际发布**的个数把结果搬进
             *      jit_fn_results[] + jit_fn_result_count（**不对 rc 做静态假设**，
             *      多返回值同样正确）；
             *   2) 机器码只做失败检查 → epilogue（结果由发布区带走，RAX 不再需要）。
             * 于是 VM 那步"关闭本帧 upvalue"对 JIT 函数**恒为空操作**：按 R5 不变量 I1 +
             * C3 拒绝，JIT 函数不可能有指向自己 locals 的 open upvalue。
             * 空间行为由 callout 的 `jit_func_depth >= JIT_FUNC_MAX_DEPTH` bailout 保住
             * （交解释器做真正的 TCO）。详见 §8.76。
             *
             * 循环模式与内联体一律拒绝：两者都没有"返回调用者"的能力。循环模式本不该走到
             * 这里（scan 已置 has_reachable_return 让 jit_compile 拒绝整循环）—— 这里是
             * 防御性兜底，绝不放行。 */
            case OP_TAIL_CALL: {
                if (inline_depth > 0 || !ctx->func_mode) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] TAIL_CALL 仅函数级 JIT 支持"
                                        "（inline_depth=%d func_mode=%d）→ 拒绝编译\n",
                                inline_depth, ctx->func_mode);
                    return 0;
                }
                int tc_argc = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);          /* vstack_top = TOS = callee */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                /* add rax, 8 —— 跳过 callee 槽，指向最后一个实参（与 OP_CALL 同款） */
                emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC0); emit_byte(cb, 0x08);
                emit_mov_rr(cb, JIT_ARG1, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)tc_argc);
                EMIT_CALL(jit_callout_tail_call);
                EMIT_CALLOUT_END();
                /* 失败检查（结果已在发布区，无需回读 RAX） */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int tc_fail = emit_jcc(cb, 0x85);        /* JNZ → bailout（交解释器） */
                    patch_add(ctx, tc_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);   /* jit_callout_failed = 0 */
                /* RAX 此刻为 0 = jr 成功（返回值已在 jit_fn_results 发布区） */
                EMIT_EPILOGUE();
                vstack -= (tc_argc + 1);
                vstack = VSTACK_UNREACHABLE;
                break;
            }

            /* ---- OP_GET_METHOD + OP_CALL（动态派发方法调用）—— §8.57 ----
             * 形态与记账见 jit_scan.c 的同名 case：调用方在 `[self][args...]` 之上**多压了一个
             * receiver** 供 GET_METHOD 消费，所以
             *   - 传给 jit_callout_invoke_method 的 vstack_top 要 +8（跳过那个多余槽），
             *     这样它看到的正好是「receiver 在 [argc-1]」的实参块；
             *   - 合并后弹 (argc + 1 - rc) 个槽。
             * rc 与 callout 运行时读到的 return_count 同一规则（方法自身 return_count），
             * scan 阶段已保证可解析。动态派发到非法/原生布局时 callout 会置 failed → bailout
             * 交解释器（循环侧有 JIT_BAILOUT_LIMIT 兜底）。 */
            case OP_GET_METHOD: {
                /* ---- 独立取值形态（`obj.m` 不紧跟 OP_CALL）—— R2 批次 4（§8.66）----
                 * 判定条件必须与 jit_scan.c 的同名 case **完全一致**（否则 size 记账错位）：
                 * 只有 `ip[3] == OP_CALL` 才是可合并的动态派发调用形态。 */
                if (!(ip + 6 <= end && ip[3] == OP_CALL)) {
                    uint16_t gm_bare_idx = rd_short(ip + 1);
                    Value gm_name_val = NULL_VAL;
                    if (ctx->chunk && ctx->chunk->constants &&
                        (int)gm_bare_idx < ctx->chunk->const_cnt) {
                        gm_name_val = ctx->chunk->constants[gm_bare_idx];
                    }
                    TOS_CONSUME_RAX();                     /* 弹接收者（raw）→ RAX */
                    EMIT_RAW_TO_VALUE();
                    EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                    EMIT_CALLOUT_BEGIN();
                    EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);    /* obj */
                    emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)gm_name_val);  /* 方法名常量 */
                    EMIT_CALL(jit_callout_get_method);
                    EMIT_CALLOUT_END();
                    EMIT_VALUE_TO_RAW();
                    EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                    /* 找不到方法 / 类型不支持 → bailout 交解释器（报错文本一致） */
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    {
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int gm_bare_fail = emit_jcc(cb, 0x85);   /* JNZ → bailout */
                        patch_add(ctx, gm_bare_fail, -1, 0);
                    }
                    emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                    emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                    EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                    TOS_PRODUCE();
                    break;                                 /* pop1 push1 → net 0 */
                }
                /* ---- 以下为 §8.57 的「GET_METHOD + OP_CALL」动态派发调用形态 ---- */
                uint16_t gm_name_idx = rd_short(ip + 1);
                int gm_argc = rd_short(ip + 4);
                int gm_rc = jit_resolve_method_ret_count(cur_chunk, gm_name_idx);
                if (gm_rc <= 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] GET_METHOD ret_count 不可知 at bc_off=%d\n", bc_off);
                    return 0;
                }
                size = 6;   /* 连同 OP_CALL 一起消费 */
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                /* add rax, 8 —— 跳过 GET_METHOD 要消费的那个 receiver 槽 */
                emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC0); emit_byte(cb, 0x08);
                emit_mov_rr(cb, JIT_ARG1, JIT_RAX);              /* vstack_top（实参块） */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)gm_argc);
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)ip);
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)cur_chunk);
                EMIT_CALL(jit_callout_invoke_method);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int gm_fail = emit_jcc(cb, 0x85);     /* JNZ → bailout */
                    patch_add(ctx, gm_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                {
                    /* 内存里待消费 = 实参块(argc) + 额外 receiver(1)；保留在内存的是
                     * 多返回值的前 (rc-1) 个（callout 已写回实参槽），最后一个在 RAX。
                     * ⇒ 弹 (argc + 1) - (rc - 1) = argc - rc + 2 个槽。
                     * 注意它比 scan 的 vstack 计数（-(argc + 1 - rc)）多 1：TOS 在寄存器里，
                     * 内存弹栈数天然比计数差 1（`INVOKE_METHOD` 的 argc-rc+1 同款）。
                     * 少弹 1 格 = RSP 漂移 ⇒ 后续指令读错槽（本轮就是踩了这个，
                     * 表现为 OP_ADD_INT 的 "int48 溢出/截断" bailout）。 */
                    int gm_pop = (gm_argc - gm_rc + 2) * 8;
                    if (gm_pop > 0) {
                        if (gm_pop <= 127) {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                            emit_byte(cb, (uint8_t)gm_pop);
                        } else {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                            emit_uint32(cb, (uint32_t)gm_pop);
                        }
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (gm_argc + 1 - gm_rc);
                break;
            }

            /* ---- Callout: OP_INVOKE_METHOD_TYPED (struct method call, VM re-entry) ---- */
            case OP_INVOKE_METHOD_TYPED: {
                int arg_count = rd_short(ip + 3);

                /* ---- 内联快路径：scan 阶段已解析出唯一的 struct 方法体，
                 * 把 callee 展开在本循环体内（省掉整条 callout + 函数级 JIT
                 * 的逐次序言），与 OP_CALL_GLOBAL_FUNC_TYPED 的内联同构。
                 * 接收者是动态分发 → 必须生成 def 守卫，不匹配即 bailout
                 * 交解释器（方法查找/报错语义完全由解释器负责）。 ---- */
                {
                    int method_inline_idx = FIND_INLINE_SITE(bc_off);
                    if (method_inline_idx >= 0 &&
                        sr->inline_sites[method_inline_idx].is_method) {
                        const InlineSite* mis = &sr->inline_sites[method_inline_idx];
                        TOS_SPILL();
                        /* 1) 倒序弹实参写入 callee locals：
                         *    JIT 栈 [receiver, arg1..argN]，栈顶是 argN，
                         *    于是 i=arg_count-1 → argN → slot arg_count-1，…，
                         *    i=0 → receiver → slot 0（= self）。 */
                        for (int i = arg_count - 1; i >= 0; i--) {
                            emit_byte(cb, 0x58);          /* pop rax */
                            int msi = mis->callee_local_map[i];
                            if (msi < 0) {
                                if (jit_debug_on())
                                    fprintf(stderr, "[JIT-DEBUG] method inline FAIL: unmapped callee slot %d\n", i);
                                return 0;
                            }
                            int md = scratch_disp(msi);
                            if (md >= -128 && md <= 127)
                                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)md, JIT_RAX);
                            else
                                emit_mov_mem32_reg(cb, JIT_RBP, md, JIT_RAX);
                        }
                        vstack -= arg_count;
                        /* 2) 接收者守卫：slot 0 必须是 mis->method_def 的实例 */
                        {
                            int msi0 = mis->callee_local_map[0];
                            int md0 = scratch_disp(msi0);
                            if (md0 >= -128 && md0 <= 127)
                                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)md0);
                            else
                                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, md0);
                            /* 2a) NaN-boxed 对象（top16 == 0xFFFC，用 SHR 比较） */
                            emit_mov_rr(cb, JIT_R8, JIT_RAX);
                            emit_shr_imm(cb, JIT_R8, 48);
                            emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_notobj = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_notobj, -1, 0);
                            emit_and_rr(cb, JIT_RAX, JIT_R10);      /* RAX = Object* */
                            /* 2b) obj->type == OBJ_STRUCT */
                            emit_cmp_mem8_imm8(cb, JIT_RAX, 0, (uint8_t)OBJ_STRUCT);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_notstruct = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_notstruct, -1, 0);
                            /* 2c) obj->def == 编译期解析出的唯一 def */
                            emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, def));
                            emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)mis->method_def);
                            emit_cmp_rr(cb, JIT_RDX, JIT_R8);
                            EMIT_BAILOUT_SITE_NONOVF(bc_off);
                            int mi_otherdef = emit_jcc(cb, 0x85);
                            patch_add(ctx, mi_otherdef, -1, 0);
                        }
                        /* 3) 切到 callee：保存调用方上下文并进入内联帧
                         *    （bc_off 命名空间按**内联实例**编号，见 §8.26） */
                        InlineFrame* mf_frame = &inline_frames[inline_depth];
                        mf_frame->ip = ip + size;
                        mf_frame->end = end;
                        mf_frame->local_map = cur_local_map;
                        mf_frame->chunk = cur_chunk;
                        mf_frame->bc_off = bc_off + size;
                        mf_frame->vstack = vstack + arg_count;
                        mf_frame->tos_live = 0;
                        mf_frame->callee_ret_count = mis->ret_count;
                        mf_frame->callee_arg_count = arg_count;
                        mf_frame->module = cur_module;   /* §8.95：切模块（callee 的）*/
                        inline_depth++;
                        ip = mis->callee_chunk->code;
                        end = ip + mis->callee_body_size;
                        cur_local_map = mis->callee_local_map;
                        cur_chunk = mis->callee_chunk;
                        cur_module = mis->callee_module;   /* §8.95：切到 callee 模块 */
                        bc_off = 0x10000 * (method_inline_idx + 1);
                        vstack = 0;
                        tos_live = 0;
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline(method) ENTER: arg_count=%d ret_count=%d callee_lc=%d\n",
                                    arg_count, mis->ret_count, mis->callee_local_count);
                        continue;
                    }
                }
                /* 返回值个数：编译期按方法名解析（scan 阶段已保证可解析，
                 * 这里再兜一次，绝不允许按错误的个数生成代码）。多返回值
                 * 方法（如 Font.measureString → [float, float]）时，
                 * callout 已把前 (ret_count-1) 个结果写回 vstack 内存，
                 * RAX = 最后一个结果（新 TOS）。 */
                int ret_count = 0;
                /* 静态类型名 → 直接定位 def/方法（确定性解析，不依赖"方法名唯一"） */
                {
                    ObjStructDef* td = NULL;
                    ObjFunction* tf = NULL;
                    (void)jit_resolve_method_typed(cur_chunk, rd_short(ip + 1),
                                                   rd_short(ip + 5), &td, &tf, &ret_count);
                }
                if (ret_count <= 0)
                    ret_count = jit_resolve_method_ret_count(cur_chunk, rd_short(ip + 1));
                if (ret_count <= 0) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] INVOKE_METHOD ret_count 无法确定 at bc_off=%d\n", bc_off);
                    return 0;
                }
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into arg1 (1st argument register). */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)ip);  /* bytecode ip (3rd) */
                /* 4th arg 用 cur_chunk：内联 callee 时 ip 指向 callee 的字节码，
                 * 方法名常量索引必须到 callee 的常量表里取（其余 callout 同理）。 */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)cur_chunk);  /* chunk (4th) */
                EMIT_CALL(jit_callout_invoke_method);
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
                /* Pop (arg_count - ret_count + 1) 个实参槽：剩下的
                 * (ret_count - 1) 个槽由 callout 写入额外的返回值，
                 * 与 OP_CALL_GLOBAL_FUNC 的多返回值处理完全一致。 */
                int pop_bytes = (arg_count - ret_count + 1) * 8;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - ret_count);
                break;
            }

            /* ---- OP_GET_GLOBAL_FUNC（R6-b 前置）—— push global_funcs[slot] ----
             * 解释器语义（op_variables.inc:206-214）：越界报"全局函数索引越界"，
             * 否则 push vm.global_funcs[slot]（通常是 OBJ_CLOSURE）。
             * 为什么需要它：`return f(x)` 的尾调用形态是
             * `OP_GET_GLOBAL_FUNC + OP_TAIL_CALL`，而它此前没有 case ⇒ 含尾调用的函数
             * 整体被拒（实测拒收原因："unsupported opcode 18 at offset 3"）。
             * 顺带解锁「取函数值再调用」的一般形态（`OP_GET_GLOBAL_FUNC + OP_CALL`）。
             * net +1（压入一个值）；越界/空 VM → failed → bailout 交解释器报错。 */
            case OP_GET_GLOBAL_FUNC: {
                uint16_t ggf_slot = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)ggf_slot);
                EMIT_CALL(jit_callout_get_global_func);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int ggf_fail = emit_jcc(cb, 0x85);       /* JNZ → bailout */
                    patch_add(ctx, ggf_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack++;
                break;
            }

            /* ---- OP_GET_CSTRUCT_DEF（R6-d）—— 按名字查 cstruct 定义注册表并压栈 ----
             * 解释器语义（op_cstruct.inc:103-129）：名字常量（字符串）→ cstruct_def_find
             * → 查到 push `val_obj(def)`，查不到报"找不到 cstruct 定义 'X'"。
             *
             * ⚠ 为什么**不做编译期解析**（对比 §8.56 模块方法、OP_CALL_NATIVE 的 native 表）：
             * cstruct 定义是**运行时注册**的（`cstruct_def_register` 在 `OP_DEFINE_CSTRUCT`
             * 执行时才发生），且同名重声明会**覆盖**条目并把旧 def 的 name/字段表清空
             * （object_cstruct.c:229-241）⇒ 编译期嵌入 def 指针会被"换掉"，与解释器分叉。
             * 所以这里只做「能不能编」的解码检查（常量存在且是字符串），查找每次执行在
             * callout 里做一遍 —— 与解释器逐字一致（含覆盖语义与报错路径）。
             * net +1；查不到 → callout 置 failed → bailout。 */
            case OP_GET_CSTRUCT_DEF: {
                uint16_t csd_ci = rd_short(ip + 1);
                if (!cur_chunk || !cur_chunk->constants || (int)csd_ci >= cur_chunk->const_cnt) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] get_cstruct_def: 名字常量越界 → 拒编\n");
                    return 0;
                }
                Value csd_name = cur_chunk->constants[csd_ci];
                if (!val_is_obj(csd_name) || val_as_obj(csd_name)->type != OBJ_STRING) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] get_cstruct_def: 名字常量非字符串 → 拒编\n");
                    return 0;
                }
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1,
                                   (uint64_t)(uintptr_t)(ObjString*)val_as_obj(csd_name));
                EMIT_CALL(jit_callout_get_cstruct_def);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int csd_fail = emit_jcc(cb, 0x85);       /* JNZ → bailout */
                    patch_add(ctx, csd_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack++;
                break;
            }

            /* ---- OP_GET_MODULE_VAR / OP_GET_MODULE_FUNC / OP_SET_MODULE_VAR（§5 覆盖面 L6）----
             * 解释器语义：vm/vminc/op_module_var.inc（module 取自当前帧 frame->module，
             * 而 frame->module 又来自被调函数 → 见 jit_scan.c 里内联侧的拒绝理由）。
             * 三条全走 callout：
             *   GET_MODULE_VAR / GET_MODULE_FUNC：压入 globals[index]（net +1）
             *   SET_MODULE_VAR：**peek** TOS → 写入 + gc_write_barrier，TOS 原样保留（net 0）
             * 不做原生内存访问的原因：模块指针是「每帧」的（vm.frames[frame_cnt-1].module），
             * 在机器码里硬编码帧/模块的结构体偏移太脆；且 SET 还带写屏障（必须走 VM 的
             * gc_write_barrier）。无模块 / 索引越界 → callout 置 failed → bailout 交解释器报错。 */
            case OP_GET_MODULE_FUNC: {
                uint16_t index = rd_short(ip + 1);
                /* 窥孔：`OP_GET_MODULE_FUNC + OP_CALL` = 模块函数调用（scan 已解析出
                 * ret_count 并允许了这次合并；这里再解析一次作防御性校验，解析不出来
                 * 就拒绝编译整个循环 —— 宁可不编，不猜返回值个数）。 */
                int merged_call = (ip + 6 <= end && ip[3] == OP_CALL);
                if (merged_call) {
                    int argc = rd_short(ip + 4);
                    int rc = jit_resolve_module_func(index);
                    if (rc <= 0) {
                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] 模块函数 ret_count 不可知（globals[%u]）\n",
                                    (unsigned)index);
                        return 0;
                    }
                    size = 6;   /* 连同 OP_CALL 一起消费 */
                    TOS_SPILL();
                    EMIT_STORE_TMP(tmp1_disp, JIT_RSP);            /* vstack_top 参数 */
                    EMIT_CALLOUT_BEGIN();
                    /* 模块指针编译期嵌入（见 jit_priv.h：函数级 JIT 快路径不压帧）。 */
                    emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)(uintptr_t)jit_scan_get_module());
                    EMIT_LOAD_TMP(JIT_ARG2, tmp1_disp);            /* vstack_top */
                    emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)argc);
                    emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)index);
                    /* 5th param (ret_count): Windows x64 走第一个栈参数 [RSP+32]
                     * （EMIT_CALLOUT_ALLOC 已预留 32B shadow + 16B 栈参数区），
                     * System V 用 R8 —— 与 OP_STRUCT_INIT / MODULE_CALL 同款写法。 */
#ifdef _WIN32
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)rc);
                    emit_mov_mem8_reg(cb, JIT_RSP, 32, JIT_RAX);
#else
                    emit_mov_reg_imm64(cb, JIT_ARG5, (uint64_t)rc);
#endif
                    EMIT_CALL(jit_callout_call_module_func);
                    EMIT_CALLOUT_END();
                    EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    {
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int mc_fail = emit_jcc(cb, 0x85);          /* JNZ → bailout */
                        patch_add(ctx, mc_fail, -1, 0);
                    }
                    emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                    emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                    EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                    /* 弹 (argc - rc + 1) 个实参槽：剩下 (rc-1) 个槽由 callout 写过
                     * 额外的返回值，与 OP_CALL_GLOBAL_FUNC / INVOKE_METHOD 同构。 */
                    int pop_bytes = (argc - rc + 1) * 8;
                    if (pop_bytes > 0) {
                        if (pop_bytes <= 127) {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                            emit_byte(cb, (uint8_t)pop_bytes);
                        } else {
                            emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                            emit_uint32(cb, (uint32_t)pop_bytes);
                        }
                    }
                    EMIT_VALUE_TO_RAW();
                    TOS_PRODUCE();
                    vstack -= (argc - rc);
                    break;
                }
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)(uintptr_t)cur_module);   /* §8.95 */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)index);
                EMIT_CALL(jit_callout_get_module_var);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int mv_fail = emit_jcc(cb, 0x85);      /* JNZ → bailout */
                    patch_add(ctx, mv_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_GET_MODULE_VAR: {
                uint16_t index = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)(uintptr_t)cur_module);   /* §8.95 */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)index);
                EMIT_CALL(jit_callout_get_module_var);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int mv2_fail = emit_jcc(cb, 0x85);      /* JNZ → bailout */
                    patch_add(ctx, mv2_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                TOS_PRODUCE();
                vstack++;
                break;
            }
            case OP_SET_MODULE_VAR: {
                uint16_t index = rd_short(ip + 1);
                TOS_SPILL();                                   /* TOS 落到 [rsp]，值本身不变 */
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RSP, 0);    /* raw TOS */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)(uintptr_t)cur_module);   /* §8.95 */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)index);
                EMIT_LOAD_TMP(JIT_ARG3, tmp1_disp);            /* TOS 的 Value 形态 */
                EMIT_CALL(jit_callout_set_module_var);
                EMIT_CALLOUT_END();
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int smv_fail = emit_jcc(cb, 0x85);     /* JNZ → bailout */
                    patch_add(ctx, smv_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                /* 解释器是 peek：TOS 保持原样，不 pop 也不改写 → 不做 TOS_PRODUCE、vstack 不变 */
                break;
            }

            /* ---- OP_SET_PTR_ELEM_TYPE / OP_SET_DECLARED_FACE（存值前的类型标记，§8.58）----
             * 解释器语义：**peek TOS**、不弹不推（net 0）、无返回值、类型不匹配时静默不做。
             * codegen 在「var-decl / 赋值」时发射，紧跟值之后、存槽之前（见 codegen_stmt.c）。
             * ⇒ 直接调用 callout：不做失败检查、不动 vstack、也不 TOS_PRODUCE（TOS 原样保留）。 */
            case OP_SET_PTR_ELEM_TYPE: {
                uint8_t elem_type = ip[1];
                TOS_SPILL();                                   /* TOS 落到 [rsp]，值不变 */
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RSP, 0);    /* raw TOS */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)elem_type);
                EMIT_CALL(jit_callout_set_ptr_elem_type);
                EMIT_CALLOUT_END();
                break;
            }
            case OP_SET_DECLARED_FACE: {
                uint16_t face_name_idx = rd_short(ip + 1);
                TOS_SPILL();
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RSP, 0);
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)face_name_idx);
                /* face 名字在**当前编译单元**的常量表里（内联时 cur_chunk 已切到 callee） */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)(uintptr_t)cur_chunk);
                EMIT_CALL(jit_callout_set_declared_face);
                EMIT_CALLOUT_END();
                break;
            }

            /* ---- OP_STRING_ADD（字符串插值 `"${x}"` 编译出的拼接）—— R2 批次 2（§8.64）----
             * 解释器语义（vm/vminc/op_string.inc）：弹 b（TOS）、弹 a → 拼成新串 → 压回，
             * 即 **pop 2 push 1 → net -1**。两侧都是 ObjString 时走 str_concat
             * （正确处理内嵌 NUL），否则两侧各自转字符串（value_to_string）再拼。
             * 拼接逻辑复用 VM 的 string_add（**语义唯一来源**），JIT 只做取值/装箱/回填。
             * 会分配：在 JIT 帧里安全 —— GC 在 JIT 执行期间只置让出标志、不就地回收
             *   （§8.36/§8.37），所以 callout 里分配不会中途搬走 JIT 机器栈里的活值。
             * 不报错（任何值都能转字符串）⇒ 无失败通道、无 bailout 分支。 */
            case OP_STRING_ADD: {
                TOS_CONSUME_TO(JIT_RDX);            /* b（TOS，raw） */
                TOS_CONSUME_RAX();                  /* a（raw） */
                EMIT_RAW_TO_VALUE();                /* 裸 int48 → 装箱；其余原样 */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX); /* a（RAW_TO_VALUE 只动 RAX/R8，RDX 安全） */
                emit_mov_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_RAW_TO_VALUE();                /* b */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp); /* a */
                EMIT_LOAD_TMP(JIT_ARG2, tmp2_disp); /* b */
                EMIT_CALL(jit_callout_string_add);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack--;                           /* 净 -1 */
                break;
            }

            /* ---- OP_TYPE_CHECK（`is` 判定）—— R2 批次 3（§8.65）----
             * 解释器语义（vm/vminc/op_type_check.inc）：弹栈顶 → 判定是否符合期望类型 →
             * 压 val_bool（pop 1 push 1，**net 0**）。
             * 编码：type_kind(1) + [elem_type(1)]（多数类型）
             *   或 + [name_const(2)]（TYPE_STRUCT / TYPE_FACE / **TYPE_ENUM**）。
             * 判定整个交给 callout（它调 VM 的 type_check_value —— 语义唯一来源）；
             * **名字常量在编译期从 chunk->constants 取好**传进去（与 §8.61 的 switch
             * case 数组同一做法：常量表是 GC 根，指针稳定、不会被回收）。
             * 纯判定、不分配、不报错 ⇒ 无失败通道、无 bailout 分支。 */
            case OP_TYPE_CHECK: {
                uint8_t tk = ip[1];
                int elem = (int)TYPE_ANY;
                Value name_val = NULL_VAL;
                if (tk == TYPE_STRUCT || tk == TYPE_FACE || tk == TYPE_ENUM) {
                    uint16_t ni = rd_short(ip + 2);
                    if (ctx->chunk && ctx->chunk->constants &&
                        (int)ni < ctx->chunk->const_cnt) {
                        name_val = ctx->chunk->constants[ni];
                    }
                } else {
                    elem = (int)ip[2];
                }
                TOS_CONSUME_RAX();                     /* 弹操作数（raw）→ RAX */
                /* ---- §8.100：`is float` 在**歧义区**交回解释器（不猜）----
                 * JIT 虚拟栈用"裸位型"表示数值：int48 = 裸 int64、float = 裸 double；
                 * 二者在 `raw ∈ [0, 2^47)` 上位型**重合**（float 侧恰好是 +0.0 与正次正规数）
                 * ⇒ 而下面 EMIT_RAW_TO_VALUE 的启发式（raw>>47 ∈ {-1,0} ⇒ 当 int48 装箱）
                 * 会把浮点 0.0 贴成 int 0 ⇒ `0.0 is float` 在 JIT 判**假**、解释器判**真**
                 * （静默分叉；与 §8.99 那个 SDL `0.0` 是同一根因的另一个出口）。
                 * 实测（值域探针 jit_probes/probe_type_check_float_zero.leno）：
                 * `zero=50` —— 热阈值前 50 次解释执行判真、之后 JIT 判假 —— vs NO_JIT 2000。
                 * ⚠ 这条信息在裸表示里**已丢失**（int 0 与 float 0.0 位型相同，无法用位型恢复），
                 * 所以唯一"不猜"的做法是：期望类型属浮点族且 raw 落在歧义区 ⇒ bailout，
                 * 交解释器按真实类型判定（代价只落在"对 {0, 正次正规} 做 is float"这一形态上）。
                 * 注：`raw>>47 == -1`（负 int48 / 负 NaN）与其它值域都不在歧义区，不受影响；
                 * 反向的 `int 0 is int`（JIT 判真）属同一表示缺口的另一面，见 §8.100 的待决记录。 */
                if (tk == TYPE_INT || tk == TYPE_FLOAT ||
                    (tk >= TYPE_I8 && tk <= TYPE_C_SSIZE)) {
                    /* §8.117：先**证明** TOS 是裸 int48；证明了就不必猜 ✓（判据同族缺陷：
                     * `raw>>47 == 0` 等于"所有非负 int48" ✗ ⇒ `v is int` 每次迭代都 bail ✗
                     * ⇒ 含 `is` 的热循环被拉黑 ✓。实测 probe_is_int_loop。）
                     * 两条证明都取自**既有**判据，不新增假设 ✓：
                     *   a) `prev_int48`：紧邻的上一条指令在白名单里（§8.41）⇒ 结果必是裸 int48 ✓
                     *   b) `tos_from_si` + RBX 位图 bit=0：本 JIT 一贯的"该槽按 int 处理"判据，
                     *      与 `EMIT_RAW_TO_VALUE`（装箱）用的是**同一条** ✓（要么一起信、要么一起不信）
                     * 两条都不成立 ⇒ 保持原来的"歧义 ⇒ bailout"（不猜 ✓）。 */
                    int _proven = prev_int48;
                    if (!_proven && tos_from_si >= 0 && tos_from_si < 64) {
                        /* BT RBX, si；JNC → 跳过下面的歧义检查（bit=0 ⇒ 按 int 处理） */
                        emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xBA);
                        emit_byte(cb, 0xE3);
                        emit_byte(cb, (uint8_t)(tos_from_si & 0xFF));
                        int _slot_int_skip = emit_jcc(cb, 0x83);   /* JNC rel32 */
                        emit_mov_rr(cb, JIT_R8, JIT_RAX);
                        emit_sar_imm(cb, JIT_R8, 47);
                        emit_test_rr(cb, JIT_R8, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int amb_p = emit_jcc(cb, 0x84);     /* JZ（歧义）→ bailout */
                        patch_add(ctx, amb_p, -1, 0);
                        patch_rel32(cb, _slot_int_skip, cb->len);
                    } else if (!_proven) {
                        emit_mov_rr(cb, JIT_R8, JIT_RAX);
                        emit_sar_imm(cb, JIT_R8, 47);       /* 歧义 ⇔ 0 ≤ raw < 2^47 */
                        emit_test_rr(cb, JIT_R8, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int amb_p = emit_jcc(cb, 0x84);     /* JZ（歧义）→ bailout */
                        patch_add(ctx, amb_p, -1, 0);
                    }
                    /* _proven ⇒ 什么都不发：正常装箱 + callout 判定即为精确答案 ✓ */
                }
                EMIT_RAW_TO_VALUE();                   /* 裸 int48 → 装箱；其余原样 */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint32_t)tk);
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint32_t)elem);
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)name_val);
                EMIT_CALL(jit_callout_type_check);
                EMIT_CALLOUT_END();                    /* RAX = 0/1 */
                EMIT_RAW01_TO_BOOLVAL();               /* 与解释器一致：压的是 val_bool */
                TOS_PRODUCE();
                break;                                 /* net 0 */
            }

            /* ---- OP_AS_CAST（`as` 安全转换）—— R6-g ----
             * 解释器语义（vm/vminc/op_as_cast.inc）：弹栈顶 → 按期望类型检查/转换 →
             * 匹配（可能是转换后的值）或 null 压回（pop 1 push 1，**net 0**）。
             * 编码：type_kind(1) + [name_const(2)]（**TYPE_STRUCT / TYPE_FACE / TYPE_CSTRUCT**）
             *   或 + [elem_type(1)]（其余，**含 TYPE_ENUM**）。
             *   ⚠ 与上面 OP_TYPE_CHECK 的名字常量集合**不同**（那边是 STRUCT/FACE/ENUM、
             *     没有 CSTRUCT）—— 长度表因此分成两条（见 jit_scan.c 的 opcode_size）。
             * 转换整个交给 callout（它调 VM 的 vm_as_cast —— 语义唯一来源）；
             * 名字常量在编译期从 chunk->constants 取好（常量表是 GC 根、指针稳定）。
             * 不报错（不匹配 ⇒ null），但可能分配（字符串转换 / 整数转 FFI 指针）——
             * JIT 帧里安全（GC 只置让出标志）⇒ 无失败通道、无 bailout 分支。 */
            case OP_AS_CAST: {
                uint8_t tk = ip[1];
                int elem = (int)TYPE_ANY;
                Value name_val = NULL_VAL;
                if (tk == TYPE_STRUCT || tk == TYPE_FACE || tk == TYPE_CSTRUCT) {
                    uint16_t ni = rd_short(ip + 2);
                    if (ctx->chunk && ctx->chunk->constants &&
                        (int)ni < ctx->chunk->const_cnt) {
                        name_val = ctx->chunk->constants[ni];
                    }
                } else {
                    elem = (int)ip[2];
                }
                TOS_CONSUME_RAX();                     /* 弹操作数（raw）→ RAX */
                EMIT_RAW_TO_VALUE();                   /* 裸 int48 → 装箱；其余原样 */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint32_t)tk);
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint32_t)elem);
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)name_val);
                EMIT_CALL(jit_callout_as_cast);
                EMIT_CALLOUT_END();                    /* RAX = Value（转换结果或 NULL_VAL） */
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                break;                                 /* net 0 */
            }

            /* ---- OP_GET_FIELD_ADDR（`&c.field`）—— R6-h ----
             * 解释器语义（vm/vminc/op_struct.inc:866-925）：弹 obj → **必须是 cstruct** →
             * 索引越界检查 → 构造**非拥有**的 FFI 指针（指向 obj->data + field->offset）→
             * 压回（pop 1 push 1，**net 0**）。编码：field_idx(1)（长度 2，早已登记）。
             * 指针构造调 `cstruct_field_addr_new`（object_cstruct.c）—— 语义唯一来源，
             * 解释器用的是同一个；错误路径（非 cstruct / 越界 / 分配失败）→ failed →
             * bailout → 解释器重放本条指令报原文（"& 取地址仅支持 cstruct 字段…"等）。 */
            case OP_GET_FIELD_ADDR: {
                uint8_t fa_idx = ip[1];
                TOS_CONSUME_RAX();                     /* 弹 obj（raw）→ RAX */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint32_t)fa_idx);
                EMIT_CALL(jit_callout_get_field_addr);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fa_fail = emit_jcc(cb, 0x85);      /* JNZ → bailout */
                    patch_add(ctx, fa_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                break;                                 /* net 0 */
            }

            /* ---- OP_GET_FIELD / OP_SET_FIELD（struct 字段读写）—— §5 覆盖面 L3 ----
             * 解释器语义：vm/vminc/op_struct.inc。
             *   GET_FIELD：弹 obj、压 field（net 0）；struct 走越界检查 + struct_get_field。
             *   SET_FIELD：弹 value + obj，**压回 value**（赋值表达式的值，net -1）；
             *              struct 有 int→float / bigint→float 自动提升 + GC 写屏障。
             * JIT：两条都走 callout —— struct 路径完整实现；cstruct 与「非 struct/cstruct
             *   类型」的报错路径由 callout 置 failed → bailout → 解释器重放本条指令，
             *   报错文本、分配语义与 try/catch 可捕获性全与 NO_JIT 一致。
             *   特别注意：**字段写入必须走 struct_set_field**（leno_value.h 的 static inline
             *   唯一写入入口，内含写屏障）—— 这里不做原生内联写，避免漏屏障破坏 GC 契约。 */
            case OP_GET_FIELD:
            case OP_SET_FIELD: {
                uint8_t field_idx = ip[1];
                int is_set = (op == OP_SET_FIELD);
                TOS_SPILL();
                if (is_set) {
                    emit_pop_reg(cb, JIT_RDX);                 /* value（TOS） */
                    emit_pop_reg(cb, JIT_RAX);                 /* obj */
                    EMIT_STORE_TMP(tmp1_disp, JIT_RAX);        /* obj（raw） */
                    EMIT_STORE_TMP(tmp2_disp, JIT_RDX);        /* value（raw） */
                    /* 两个操作数先就地转成 Value 写回 tmp 槽；实参寄存器留到 callout 序言
                     * 再载入 —— EMIT_RAW_TO_VALUE 以 R8 为 scratch，跨它保存值会被踩
                     * （§8.53 踩坑①）。 */
                    EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                    EMIT_RAW_TO_VALUE();
                    EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                    EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                    EMIT_RAW_TO_VALUE();
                    EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                    EMIT_CALLOUT_BEGIN();
                    EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);        /* obj */
                    emit_mov_reg_imm64(cb, JIT_ARG2, (uint32_t)field_idx);
                    EMIT_LOAD_TMP(JIT_ARG3, tmp2_disp);        /* value */
                    EMIT_CALL(jit_callout_set_field);
                    EMIT_CALLOUT_END();
                } else {
                    emit_pop_reg(cb, JIT_RAX);                 /* obj（TOS） */
                    EMIT_RAW_TO_VALUE();
                    EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                    EMIT_CALLOUT_BEGIN();
                    EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);        /* obj */
                    emit_mov_reg_imm64(cb, JIT_ARG2, (uint32_t)field_idx);
                    EMIT_CALL(jit_callout_get_field);
                    EMIT_CALLOUT_END();
                }
                EMIT_VALUE_TO_RAW();
                /* 错误（非 struct/cstruct、cstruct 读、越界）→ bailout 交解释器重放 */
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fld_fail = emit_jcc(cb, 0x85);         /* JNZ → bailout */
                    patch_add(ctx, fld_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp3_disp);
                TOS_PRODUCE();
                if (is_set) vstack--;   /* SET: pop2 push1（net -1）；GET: pop1 push1（net 0） */
                break;
            }

            /* ---- OP_GET_FIELD_FAST (读局部 struct 的字段) ----
             * 内联快路径：obj 是 NaN-boxed struct 且 field_idx < obj->def->field_count
             * 时，直接在机器码里读 field_values[field_idx] —— 这就是
             * struct_get_field 的全部语义（越界分支走慢路径保证一致）。
             * 慢路径保留原 callout（类型不匹配 → 错误文本原样）。
             * 与 OP_INDEX 读取快路径同一模板：所有判定都在产生副作用之前。 */
            case OP_GET_FIELD_FAST: {
                uint16_t slot = rd_short(ip + 1);
                uint8_t field_idx = ip[3];
                int si = cur_local_map[slot];
                int disp = scratch_disp(si);
                TOS_SPILL();
                /* Load local (struct obj raw) from scratch area */
                if (disp >= -128 && disp <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)disp);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, disp);
                }
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);   /* 慢路径仍要原始 raw 值 */

                /* 1) NaN-boxed 对象（top16 == TAG_OBJ = 0xFFFC）。
                 *    必须用 SHR：SAR 会把 0xFFFC 符号扩展成 0xFFFF...FFFC，
                 *    比较恒不等 → 快路径变死代码（§8.29 同款坑）。 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int gf_not_obj = emit_jcc(cb, 0x85);          /* JNE → 慢路径 */
                emit_and_rr(cb, JIT_RAX, JIT_R10);            /* RAX = Object* */
                /* 2) obj->type == OBJ_STRUCT（type 是 Object 首字段，偏移 0） */
                emit_cmp_mem8_imm8(cb, JIT_RAX, 0, (uint8_t)OBJ_STRUCT);
                int gf_not_struct = emit_jcc(cb, 0x85);       /* JNE → 慢路径 */
                /* 3) field_idx < obj->def->field_count（32 位零扩展加载，勿用 64 位） */
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, def));
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjStructDef, field_count));
                emit_cmp_reg32_imm32(cb, JIT_R8, (uint32_t)field_idx);
                int gf_oob = emit_jcc(cb, 0x86);              /* JBE（count <= idx）→ 慢路径 */
                /* 4) RAX = field_values[field_idx] */
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RAX, (int8_t)offsetof(ObjStruct, field_values));
                emit_mov_reg_mem32(cb, JIT_RAX, JIT_RDX, (int32_t)field_idx * 8);
                EMIT_VALUE_TO_RAW();
                int gf_done = emit_jmp(cb);

                /* ---- 慢路径：原 callout（类型检查 / 报错文本全保留）---- */
                patch_rel32(cb, gf_not_obj, cb->len);
                patch_rel32(cb, gf_not_struct, cb->len);
                patch_rel32(cb, gf_oob, cb->len);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                /* Set args AFTER CALLOUT_BEGIN (saves original RCX=locals to R13) */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);  /* obj_val (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)field_idx);  /* field_idx (2nd arg) */
                EMIT_CALL(jit_callout_get_field_fast);
                EMIT_CALLOUT_END();
                /* ---- §8.91：失败（接收者不是 struct）→ bailout（**原先漏了守卫** ✗）----
                 * 快路径的 `gf_not_obj` / `gf_not_struct` / `gf_oob` 都跳到这条慢路径 ⇒
                 * 非 struct 接收者一定会走进 callout，而它只置 `failed` + 返回 NULL_VAL
                 * ⇒ 没有这段检查就会把 null 当成字段值继续用（**静默算错**）。
                 * 守卫放在 `gf_done` 合并点**之前**：快路径（三项检查已通过、RAX 已是字段值）
                 * 直接跳过守卫；慢路径检查通过后也走同一段收尾 ✓。 */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int gff_fail = emit_jcc(cb, 0x85);    /* JNZ → bailout */
                    patch_add(ctx, gff_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();
                patch_rel32(cb, gf_done, cb->len);
                TOS_PRODUCE();
                vstack++;
                break;
            }

            /* ---- Callout: OP_CALL_GLOBAL_FUNC / OP_CALL_GLOBAL_FUNC_TYPED ---- */
            case OP_CALL_GLOBAL_FUNC:
            case OP_CALL_GLOBAL_FUNC_TYPED: {
                uint16_t func_slot = rd_short(ip + 1);
                int arg_count = rd_short(ip + 3);

                /* ---- Check if this call site is marked for inlining ---- */
                if (op == OP_CALL_GLOBAL_FUNC_TYPED) {
                    int inline_idx = FIND_INLINE_SITE(bc_off);
                    if (inline_idx >= 0) {
                        const InlineSite* is = &sr->inline_sites[inline_idx];
                        /* Spill TOS to memory stack */
                        TOS_SPILL();
                        /* Pop args from vstack and store to callee locals.
                         * Args are on the stack in order: last arg = TOS.
                         * Callee local slot 0 = first arg, slot (arg_count-1) = last arg.
                         * Pop in reverse: last arg first (slot arg_count-1), ..., first arg last (slot 0). */
                        for (int i = arg_count - 1; i >= 0; i--) {
                            /* pop rax */
                            emit_byte(cb, 0x58);  /* pop rax */
                            /* store to callee local slot i */
                            int si = is->callee_local_map[i];
                            if (si < 0) {
                                /* shouldn't happen: scan pre-mapped all locals */
                                if (jit_debug_on())
                                    fprintf(stderr, "[JIT-DEBUG] inline FAIL: unmapped callee slot %d\n", i);
                                return 0;
                            }
                            int disp = scratch_disp(si);
                            if (disp >= -128 && disp <= 127)
                                emit_mov_mem8_reg(cb, JIT_RBP, (int8_t)disp, JIT_RAX);
                            else
                                emit_mov_mem32_reg(cb, JIT_RBP, disp, JIT_RAX);
                        }
                        vstack -= arg_count;

                        /* Save caller context and switch to callee */
                        InlineFrame* f = &inline_frames[inline_depth];
                        f->ip = ip + size;  /* next instruction after the call */
                        f->end = end;
                        f->local_map = cur_local_map;
                        f->chunk = cur_chunk;
                        f->bc_off = bc_off + size;
                        f->vstack = vstack + arg_count;  /* vstack before args were popped */
                        f->tos_live = 0;  /* TOS already spilled */
                        f->callee_ret_count = is->ret_count;
                        f->callee_arg_count = arg_count;
                        f->module = cur_module;          /* §8.95：切模块（callee 的）*/
                        inline_depth++;

                        /* Switch to callee bytecode.
                         * 用大偏移把内联体的 bc_off 与调用方隔离开。
                         *
                         * 关键：命名空间基准必须按 **内联实例**（inline_idx）区分，
                         * 不能只按 nesting depth —— off_map / patch 表都以 bc_off
                         * 为键，offmap_lookup 返回**第一个**匹配项。同一深度下多个
                         * 内联点若共用基址，第 2..N 个实例的内部跳转
                         * （OP_JUMP_IF_FALSE / OP_JUMP / OP_FOR_PREP 的 skip /
                         * OP_FOR_LOOP 回边）会解析到**第 1 个实例的机器码**，
                         * 表现为死循环 + RSP 逐轮漂移 → 栈溢出。
                         * 实例：examples/crypto/aes128.leno 的 mix_columns 有 4 个
                         * gf_mul 内联点，全部落在 0x10000 命名空间，1 亿次基准里
                         * JIT 首次执行即 0xC00000FD（见 §8.26）。
                         *
                         * 内联目前只有一层（内联体内 FIND_INLINE_SITE 不会再命中），
                         * 被内联的函数体 ≤ 256 字节（scan 的 cc->len <= 256 限制），
                         * 远小于 0x10000，因此 0x10000 * (inline_idx + 1) 既唯一又
                         * 不会与调用方（基址 0）冲突。将来若支持多层内联，
                         * 需把 depth 并入这个 id。 */
                        ip = is->callee_chunk->code;
                        end = ip + is->callee_body_size;
                        cur_local_map = is->callee_local_map;
                        cur_chunk = is->callee_chunk;
                        cur_module = is->callee_module;   /* §8.95：切到 callee 模块 */
                        bc_off = 0x10000 * (inline_idx + 1);
                        vstack = 0;
                        tos_live = 0;

                        if (jit_debug_on())
                            fprintf(stderr, "[JIT-DEBUG] inline ENTER: func_slot=%d arg_count=%d ret_count=%d\n",
                                    func_slot, arg_count, is->ret_count);
                        continue;  /* skip normal ip+=size advance */
                    }
                }

                /* ---- Non-inline: callout path (original code) ---- */
                int ret_count = 1;
                if (op == OP_CALL_GLOBAL_FUNC_TYPED && ctx->vm_ptr) {
                    if (func_slot < ctx->vm_ptr->global_func_capacity) {
                        Value callee = ctx->vm_ptr->global_funcs[func_slot];
                        if (val_is_obj(callee)) {
                            Object* obj2 = val_as_obj(callee);
                            ObjFunction* func2 = NULL;
                            if (obj2->type == OBJ_CLOSURE)
                                func2 = ((ObjClosure*)obj2)->function;
                            else if (obj2->type == OBJ_FUNCTION)
                                func2 = (ObjFunction*)obj2;
                            if (func2 && func2->return_count > 1)
                                ret_count = func2->return_count;
                        }
                    }
                }
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) to tmp1
                 * BEFORE CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14.
                 * Load saved vstack_ptr into arg1 (1st argument register). */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)arg_count);  /* arg_count (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)func_slot);   /* func_slot (3rd) */
                EMIT_CALL(jit_callout_global_func);
                EMIT_CALLOUT_END();
                /* Check jit_callout_failed flag — if set, bail out.
                 * RAX holds the callout return value; save it, check flag, restore. */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);  /* RAX = *flag */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);  /* JNZ → bailout */
                    patch_add(ctx, fail_patch, -1, 0);  /* bailout */
                }
                /* Clear flag */
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);  /* *flag = 0 */
                /* Restore callout return value */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop (arg_count - ret_count + 1) args from JIT virtual stack.
                 * The remaining (ret_count - 1) slots hold the extra return
                 * values that the callout wrote to vstack_top. */
                int pop_bytes = (arg_count - ret_count + 1) * 8;
                if (pop_bytes <= 0) {
                    /* ret_count >= arg_count + 1: no pop needed (or even push) */
                } else if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - ret_count);
                break;
            }

            /* ---- OP_DICT（字典字面量 `{k:v, ...}`）—— R6-i ----
             * 解释器语义（vm/vminc/op_dict.inc）：从栈上逆序取 count 组「键、值」→
             * 正序写入新字典 → 压回（弹 2*count 压 1 ⇒ **net 1-2*count**；
             * count=0 是「空字典字面量 `{}`」⇒ net **+1**，与 OP_ARRAY 的 0 元素同形）。
             * 与 OP_ARRAY 完全同构，只是弹两倍槽位；整块交给 callout 收集+插入。
             * 有失败通道（分配失败 / dict_set 抛错）⇒ failed 检查 → bailout 交解释器。 */
            case OP_DICT: {
                uint16_t di_cnt = rd_short(ip + 1);
                TOS_SPILL();
                /* ---- §8.105：与 OP_ARRAY 同款守卫（键与值共 2*count 个槽）----
                 * 元素由 `jit_callout_dict_new` 内部转换、无来源槽信息 ⇒ 歧义区必须交解释器。 */
                for (int _e = 0; _e < (int)(2 * di_cnt); _e++) {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RSP, _e * 8);
                    emit_mov_rr(cb, JIT_R8, JIT_RAX);
                    emit_sar_imm(cb, JIT_R8, 47);
                    emit_test_rr(cb, JIT_R8, JIT_R8);
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _amb_e = emit_jcc(cb, 0x84);    /* JZ（歧义）→ bailout */
                    patch_add(ctx, _amb_e, -1, 0);
                }
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)di_cnt);   /* count (2nd) */
                EMIT_CALL(jit_callout_dict_new);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int di_fail = emit_jcc(cb, 0x85);        /* JNZ → bailout */
                    patch_add(ctx, di_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* 弹掉 2*count 个槽（键值对）；返回的字典成为新 TOS */
                int di_pop = (int)di_cnt * 16;
                if (di_pop > 0) {
                    if (di_pop <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)di_pop);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)di_pop);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (2 * di_cnt - 1);               /* count=0 ⇒ +1（空字典）*/
                break;
            }

            /* ---- Callout: OP_ARRAY (array literal [e1..eN]) ---- */
            case OP_ARRAY: {
                uint16_t count = rd_short(ip + 1);
                TOS_SPILL();
                /* ---- §8.127：给每个元素挂"来源证明"（bit i = 元素 i，按**源码顺序** 0 起）----
                 * bit=1 ⇒ 该元素的 raw 必是 double 位型 ⇒ callout 内**跳过** int48 装箱启发式
                 * （`[0.0]` 的 0.0 位型全 0，启发式会把它贴成 int 0 ✗ ⇒ JIT `g=[0]` vs
                 * NO_JIT `g=[0.0]`）。来源位图由产出该元素的指令置位，见 x86_64.c 的
                 * `slot_float_prov` 记账（单路径浮点产出 + float 常量 ⇒ 已证明）。
                 * 元素按源码顺序入栈 ⇒ 元素 i 占 vstack 槽 `vstack - count + i` ✓；
                 * callout 侧 vstack_top[count-1-i] 与 bit i 一一对应 ✓。 */
                uint32_t elem_float_prov = 0;      /* 静态通道（编译期已证明）*/
                int dyn_bt_si[32];                 /* 动态通道：待 BT 的局部量 scratch 槽 */
                int dyn_bit[32];
                int dyn_cnt = 0;
                {
                    int _base = vstack - (int)count;
                    for (int _e = 0; _e < (int)count && _e < 32; _e++) {
                        int _sl = _base + _e;
                        if (_sl < 0 || _sl >= 64) continue;
                        if (slot_float_prov[_sl]) { elem_float_prov |= (1u << _e); continue; }
                        /* ---- 动态通道：元素由 `OP_GET_LOCAL` 产出（`slot_from_si`）----
                         * RBX 类型位图（bit si = "局部量 si 非 int"）由**序言按所有局部量**
                         * 逐槽 tag 测试建立 ✓ ⇒ 运行期 `BT RBX, si` 就能拿到"该槽是 float"✓
                         * （对 int48 值域的 raw：非 int ⇒ 只能是 float ✓，对象的 raw 是
                         *  NaN-boxed、永不落在 int48 值域 ✓）。
                         * 两个前提（缺一即弃用，退回默认启发式，**不会**产生新错误）：
                         *   · 循环体内**不写**该局部量（`local_written_mask`）—— 否则位图是
                         *     入口快照、会陈旧 ✗（例：循环里 `z = 5` 后仍是 float 位 ⇒ 错 ✗）；
                         *   · 当前不在内联展开中（`inline_depth == 0`）—— callee 的槽号不在
                         *     位图覆盖范围（序言只建 n = 调用方局部量）✗。
                         * 实测收益：`probe_array_literal_float` 的 `[z]`（z 是循环外算出的
                         * float 0.0，循环内不写 ✓）从 `g=[0]` 回到 `g=[0.0]` ✓。 */
                        if (inline_depth != 0) continue;
                        int _si = slot_from_si[_sl];
                        if (_si < 0 || _si >= 64 || _si >= n) continue;
                        if (local_written_mask & (1ull << (unsigned)_si)) continue;
                        dyn_bt_si[dyn_cnt] = _si;
                        dyn_bit[dyn_cnt]   = _e;
                        dyn_cnt++;
                    }
                }
                /* ---- §8.128 第三通道：紧邻的上一条指令是 **callout 产出** ----
                 * `EMIT_VALUE_TO_RAW` 在 tag 转换点上已把"这个 raw 是不是 int48"记进帧标志
                 * （`setne`，多 1 条指令）⇒ 这里只需读标志就能精确判定装箱方式 ✓。
                 * 为什么它能修 Docker 形态（`[t[i%2]]`）：元素是 `OP_INDEX` callout 返回的
                 * VM Value（带 tag ✓）⇒ 位型虽是全 0，但类型在转换那一刻是**已知**的 ✓，
                 * 而 §8.127 的两条通道（产出指令白名单 / 局部量位图）都覆盖不到它 ✗。
                 * 覆盖面：**只有最后一个元素**（标志是单字节、只描述最近一次转换；元素按
                 * 源码顺序入栈 ⇒ 最后入栈者 = 第 count-1 个 ⇒ 与"紧邻"一一对应 ✓）。
                 * 与 §8.116 的"紧邻白名单"同一取舍：不猜、不 bail，只认得出就认 ✓。 */
                int use_ret_dbl = (prev_ret_dbl && count > 0 && (int)count <= 32);

                /* 动态部分发码：mask(静态) 存 tmp3；每个动态元素 `bt rbx, si` + 条件 OR。
                 * 只用 RAX（R8 是 callout 的 ARG3 ✗ 不能借）、不动 RSP ✓（caller 稍后
                 * 才用 RSP 取 vstack_top）。 */
                if (dyn_cnt > 0 || use_ret_dbl) {
                    emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)elem_float_prov);
                    EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                    for (int _d = 0; _d < dyn_cnt; _d++) {
                        /* bt rbx, si（48 0F BA E3 imm8）→ CF */
                        emit_byte(cb, 0x48); emit_byte(cb, 0x0F); emit_byte(cb, 0xBA);
                        emit_byte(cb, 0xE3);
                        emit_byte(cb, (uint8_t)(dyn_bt_si[_d] & 0xFF));
                        int _skip = emit_jcc(cb, 0x83);            /* JNC：CF=0（int）⇒ 跳过 */
                        EMIT_LOAD_TMP(JIT_RAX, tmp3_disp);
                        emit_alu_imm32_reg(cb, 1, JIT_RAX, (uint32_t)(1u << dyn_bit[_d])); /* OR */
                        EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                        patch_add(ctx, _skip, -1, 0);
                    }
                    if (use_ret_dbl) {
                        /* cmp byte [rbp+ret_dbl_disp], 0 ; JE → 跳过（int48 载荷）*/
                        emit_byte(cb, 0x80);                       /* CMP r/m8, imm8 */
                        if (ret_dbl_disp >= -128 && ret_dbl_disp <= 127) {
                            emit_byte(cb, 0x7D);                   /* ModRM(01,111,101) */
                            emit_byte(cb, (uint8_t)(int8_t)ret_dbl_disp);
                        } else {
                            emit_byte(cb, 0xBD);                   /* ModRM(10,111,101) */
                            emit_uint32(cb, (uint32_t)ret_dbl_disp);
                        }
                        emit_byte(cb, 0x00);
                        int _skip_f = emit_jcc(cb, 0x84);          /* JE：标志=0 ⇒ 是 int48 */
                        EMIT_LOAD_TMP(JIT_RAX, tmp3_disp);
                        emit_alu_imm32_reg(cb, 1, JIT_RAX,
                                           (uint32_t)(1u << (unsigned)(count - 1)));  /* OR */
                        EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                        patch_add(ctx, _skip_f, -1, 0);
                    }
                }
                /* ---- §8.105：字面量元素若落在歧义区 ⇒ bailout ----
                 * 元素转换发生在 **callout 内部**（`jit_callout_array_new` 用 `jit_raw_to_value`），
                 * 那里拿不到"来源槽" ⇒ §8.104 的 RBX 位图判据用不上 ✗ ⇒ 浮点 0.0（位型全 0）
                 * 会被贴成 int 0 并**存进数组**；污染值能活过循环（实测
                 * `probe_array_literal_float`：JIT `g=[0]` vs NO_JIT `g=[0.0]`，且 Bailouts=0
                 * ⇒ 循环确实在 JIT 里跑，不是被守卫掩盖）。
                 * 守卫放在交给 callout **之前**（此刻元素就在 RSP 上）⇒ 命中即交解释器
                 * 按真实类型建数组。代价只落在"JIT 循环内建的字面量含歧义区值"这一形态上。 */
                /* ⚠ 上面的守卫**已撤销**（2026-09-18）。原因与实测：
                 * · 判据 `raw>>47 == 0` 等价于 **所有非负 int48**（`emit_mov_reg_mem32` 是
                 *   64 位读，后缀指位移宽度）⇒ 命中"非负"这一半、漏掉 `== -1` 那一半，
                 *   两个方向都错（`_jit_bugs/diag_array_literal_guard.leno` 实测：
                 *   [1,2,3] / [0,0,0] 都 bail，[-1,-2,-3] 不 bail）；
                 * · 值域判据在这件事上**无解**：任何 int48 raw 同时都是 ±0.0/次正规的
                 *   double 位型，要精确只能挂"来源证明"（§8.116 的做法）；
                 * · 代价远大于收益：循环里若先有副作用（如 `arr.add`）再建非负元素字面量，
                 *   守卫会让循环 bail ⇒ 解释器**重做整轮**且 JIT 半轮副作用不回滚 ⇒
                 *   **数据污染**（`_jit_bugs/diag_bailout_double_side_effect.leno`：
                 *   `G.len()=204` 应为 201 ✗✗）。这正是 Bug5（AES 解密偶发污染）的根因，
                 *   也解释了它的 8% 随机率（3 次 bail 后拉黑 ⇒ 窗口只覆盖几轮）。
                 * 处置同 §8.115（对 native 实参守卫）：撤销。
                 * 残余（如实记录）：JIT 区域内建的**多元素**字面量若含"浮点 ±0.0/次正规"
                 * 元素，会被贴成 int ⇒ 类型标签与解释器分叉（数值相同）。
                 * 精确修法（待做）：按 §8.116 为每个元素挂"来源证明" —— 元素产出指令可
                 * 证明为裸 int48 时放行；当前无局部量静态类型表，需要新增来源跟踪。 */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)count);  /* count (2nd) */
                if (dyn_cnt > 0 || use_ret_dbl)
                    EMIT_LOAD_TMP(JIT_ARG3, tmp3_disp);   /* §8.127/§8.128 第3参（含动态位）*/
                else
                    emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)elem_float_prov);
                EMIT_CALL(jit_callout_array_new);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop count elements from JIT virtual stack */
                int pop_bytes = count * 8;
                if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (count - 1);
                break;
            }

            /* ---- Callout: OP_CALL_NATIVE (direct native call) ----
             * The native function pointer is resolved at compile time and
             * embedded as an immediate: the runtime callout then calls it
             * directly, avoiding a strcmp scan of the global native table
             * on every execution (the interpreter's IC cache does the same). */
            case OP_CALL_NATIVE: {
                uint16_t name_idx = rd_short(ip + 1);
                uint16_t arg_count = rd_short(ip + 3);
                /* Compile-time resolution: name constant -> ObjNative* */
                Value name_val = cur_chunk->constants[name_idx];
                ObjNative* native = NULL;
                if (val_is_obj(name_val) && val_as_obj(name_val)->type == OBJ_STRING) {
                    native = native_find_function(((ObjString*)val_as_obj(name_val))->chars);
                }
                if (!native) {
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] call_native: unresolved native at compile time\n");
                    return 0;  /* bail out → interpreter keeps semantics */
                }
                /* ---- 内联快速路径：_int / _float（单参数数值转换）----
                 * 这两个原语在热循环里极其常见（如 _int(x) 取像素下标），而
                 * 通用 callout 每次要付 ~23ns 的装箱/调用开销。这里直接生成
                 * CVTTSD2SI / CVTSI2SD（语义与 types.c 的 native_to_int/_float
                 * 一致：int 原样、float 按 (int) 截断 / 提升为 double）；
                 * 遇到 NaN-boxed 的其它类型（bool/null/string/bigint/ptr）
                 * 则 bailout 交回解释器，保证语义与报错信息不变。 */
                if (arg_count == 1 && native->name &&
                    (strcmp(native->name, "_int") == 0 || strcmp(native->name, "_float") == 0)) {
                    int is_int_cast = (strcmp(native->name, "_int") == 0);
                    TOS_CONSUME_RAX();          /* RAX = 参数原始值（int48 或裸 double） */
                    /* int48 判定：mov r8,rax; sar r8,47; inc r8; cmp r8,1; ja .not_int */
                    emit_mov_rr(cb, JIT_R8, JIT_RAX);
                    emit_sar_imm(cb, JIT_R8, 47);
                    emit_inc_reg(cb, JIT_R8);
                    emit_cmp_reg_imm8(cb, JIT_R8, 1);
                    int not_int_p = emit_jcc(cb, 0x87);
                    if (is_int_cast) {
                        /* _int(int) → 原样 */
                        int done0 = emit_jmp(cb);
                        patch_rel32(cb, not_int_p, cb->len);
                        /* 非 int48：先认 bool —— true→1 / false→0，与 types.c 的
                         * native_to_int 一致；判据用**全值精确比较**，与 VM 的
                         * val_is_bool（v == TRUE_VAL || v == FALSE_VAL）保持一致。 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_true = emit_jcc(cb, 0x84);   /* JE → .bool_true */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_false = emit_jcc(cb, 0x84);  /* JE → .bool_false */
                        /* null → 0（同 types.c native_to_int 的 VAL_NULL 分支 val_int(0)），
                         * 与 false 同路；判据同样是全值精确比较 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int int_is_null = emit_jcc(cb, 0x84);   /* JE → 与 false 同路（0） */
                        /* 其余 NaN-boxed（string/bigint/ptr）→ bailout 交解释器 */
                        emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int bail_p = emit_jcc(cb, 0x83);        /* jae（无符号）→ bailout */
                        patch_add(ctx, bail_p, -1, 0);
                        /* 裸 double → (int) 截断：movq xmm0,rax; cvttsd2si eax,xmm0; movsxd rax,eax */
                        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                        emit_byte(cb, 0x6E); emit_byte(cb, modrm(3, 0, JIT_RAX & 7));
                        emit_byte(cb, 0xF2); emit_byte(cb, 0x0F); emit_byte(cb, 0x2C);
                        emit_byte(cb, modrm(3, 0, JIT_RAX & 7));  /* cvttsd2si eax, xmm0 */
                        emit_byte(cb, 0x48); emit_byte(cb, 0x63); emit_byte(cb, 0xC0); /* movsxd rax,eax */
                        int int_done_jmp = emit_jmp(cb);        /* 跳过下面两个 bool 分支 */
                        /* .bool_true → 1 */
                        patch_rel32(cb, int_is_true, cb->len);
                        emit_mov_reg_imm64(cb, JIT_RAX, 1);
                        int int_true_done = emit_jmp(cb);
                        /* .bool_false / null → 0 */
                        patch_rel32(cb, int_is_false, cb->len);
                        patch_rel32(cb, int_is_null, cb->len);
                        emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                        patch_rel32(cb, int_true_done, cb->len);
                        patch_rel32(cb, int_done_jmp, cb->len);
                        patch_rel32(cb, done0, cb->len);
                    } else {
                        /* _float(int) → cvtsi2sd xmm0,rax; movq rax,xmm0 */
                        emit_cvtsi2sd(cb, 0, JIT_RAX);
                        emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                        emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                        int done0 = emit_jmp(cb);
                        patch_rel32(cb, not_int_p, cb->len);
                        /* 非 int48：先认 bool —— true→1.0 / false→0.0，与 types.c 的
                         * native_to_float 一致；判定同 VM 的 val_is_bool：全值精确比较。 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_true = emit_jcc(cb, 0x84);   /* JE → .bool_true */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_false = emit_jcc(cb, 0x84);  /* JE → .bool_false */
                        /* null → 0.0（同 types.c native_to_float 的 VAL_NULL 分支），
                         * 与 false 同路；判据同样是全值精确比较 */
                        emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        int flt_is_null = emit_jcc(cb, 0x84);   /* JE → 与 false 同路（0.0） */
                        /* 其余 NaN-boxed → bailout；裸 double 原样保留 */
                        emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                        emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                        EMIT_BAILOUT_SITE_NONOVF(bc_off);
                        int bail_p = emit_jcc(cb, 0x83);
                        patch_add(ctx, bail_p, -1, 0);
                        int flt_done_jmp = emit_jmp(cb);         /* 跳过下面两个 bool 分支 */
                        /* .bool_true → 1.0（裸 double 位模式） */
                        patch_rel32(cb, flt_is_true, cb->len);
                        emit_mov_reg_imm64(cb, JIT_RAX, 0x3FF0000000000000ULL);
                        int flt_true_done = emit_jmp(cb);
                        /* .bool_false / null → 0.0（位模式全 0） */
                        patch_rel32(cb, flt_is_false, cb->len);
                        patch_rel32(cb, flt_is_null, cb->len);
                        emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                        patch_rel32(cb, flt_true_done, cb->len);
                        patch_rel32(cb, flt_done_jmp, cb->len);
                        patch_rel32(cb, done0, cb->len);
                    }
                    TOS_PRODUCE();
                    break;                      /* arg_count == 1 → vstack 不变 */
                }
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)(uintptr_t)native);  /* ObjNative* (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)arg_count);  /* arg_count (3rd) */
                EMIT_CALL(jit_callout_call_native);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Pop ALL arg_count args: the callout reads the args from the
                 * vstack and returns its result in RAX, which becomes the new
                 * TOS (tos_live=1). Physical stack must shrink by arg_count
                 * slots to stay consistent with vstack -= (arg_count - 1). */
                int pop_bytes = arg_count * 8;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

            /* ---- OP_CLIB_CALL（FFI 动态库调用，R6-e）----
             * 解释器语义（op_clib_call.inc）：栈上是 [lib_obj, func_name, user_args...]，
             * arg_count = user_arg_count + 2；结果压栈 ⇒ net -(arg_count-1)。
             * 操作数**变长**（`arg_count(2) ret_type(1) user_arg_count(1) arg_types[]`），
             * 所以整条指令的 `ip` 直接交给 callout 现读 —— 只占 2 个寄存器参数，
             * 与 OP_CLOSURE 传捕获描述表指针同一先例。
             * 走 callout 而不是内联机器码的原因：`ffi_clib_call` 要按 ret_type_kind 做
             * 返回值展开、按 arg_types 做参数窄化，且内部可能回调进 VM（自动泵送）。 */
            case OP_CLIB_CALL: {
                uint16_t cc_argc = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)(uintptr_t)ip);  /* 指令指针 (2nd) */
                EMIT_CALL(jit_callout_clib_call);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int cc_fail = emit_jcc(cb, 0x85);       /* JNZ → bailout */
                    patch_add(ctx, cc_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* 弹掉全部 arg_count 个实参；callout 的返回值占其中一个槽成为新 TOS
                 * （与 OP_CALL_NATIVE 同款：物理栈收缩 arg_count 槽 = vstack -= (argc-1)）。 */
                int cc_pop = (int)cc_argc * 8;
                if (cc_pop > 0) {
                    if (cc_pop <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)cc_pop);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)cc_pop);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (cc_argc - 1);
                break;
            }

            /* ---- Callout: OP_GET_PROPERTY (obj.name / obj.name(args) peephole) ---- */
            case OP_GET_PROPERTY: {
                uint16_t name_idx = rd_short(ip + 1);
                /* Peephole: GET_PROPERTY + OP_CALL(arg_count16) merged in scan;
                 * mirror the same condition here (end = current body end). */
                int merged = (ip + 6 <= end && ip[3] == OP_CALL);
                uint16_t merged_argc = merged ? rd_short(ip + 4) : 0xFFFF;
                /* 窥孔合并且已被 scan 确认：codegen 也要连同 OP_CALL 一起消费 */
                if (merged) size = 6;
                TOS_SPILL();
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);   /* vstack_top */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)name_idx);         /* name const (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)merged_argc);      /* call args / 0xFFFF (3rd) */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)cur_chunk);  /* chunk (4th) */
                EMIT_CALL(jit_callout_get_property);
                EMIT_CALLOUT_END();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int fail_patch = emit_jcc(cb, 0x85);
                    patch_add(ctx, fail_patch, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                /* Merged call: pop receiver + args, push result → net -arg_count.
                 * Standalone: receiver 被结果取代 —— 但**结果放在 RAX（TOS 缓存）**，
                 * 内存里那个 receiver 槽必须显式弹掉，否则每执行一次就永久多留一个
                 * 槽（实测 **8 字节/次**：循环里 RSP 单调下漂，跑到 ~2MB 栈界时崩溃，
                 * 即 §8.67 / roadmap R12 的 `0xC0000374`；`probe` 实测
                 * `n=100000 → rsp_delta=-800000`、`n=200000 → -1600000`）。
                 * 判据与合并分支同源：**弹掉本指令消费掉的操作数**（独立访问只消费
                 * receiver 一个）；结果作为 TOS 留在 RAX，内存栈应为 vstack-1 项。 */
                int pop_bytes = merged ? (merged_argc + 1) * 8 : 8;
                if (pop_bytes > 0) {
                    if (pop_bytes <= 127) {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                        emit_byte(cb, (uint8_t)pop_bytes);
                    } else {
                        emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                        emit_uint32(cb, (uint32_t)pop_bytes);
                    }
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                if (merged) vstack -= merged_argc;
                break;
            }

            /* ---- Callout: OP_STRUCT_INIT (struct literal, e.g. Hit{...}) ----
             * ip: op(1) + name_const(2) + arg_count(1) + generic_count(1) +
             *     field_indices(arg_count bytes)。scan 已拒绝泛型
             *     (generic_count>0)，此处仅处理非泛型路径。 */
            case OP_STRUCT_INIT: {
                uint8_t arg_count = ip[3];
                TOS_SPILL();
                /* Save current RSP (virtual stack pointer) BEFORE
                 * CALLOUT_BEGIN aligns/changes RSP. */
                EMIT_STORE_TMP(tmp1_disp, JIT_RSP);
                EMIT_CALLOUT_BEGIN();
                /* After CALLOUT_BEGIN: RSP is aligned, RCX/R9 saved to R13/R14. */
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);   /* vstack_top (1st arg) */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)rd_short(ip + 1)); /* name idx (2nd) */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint64_t)arg_count);        /* arg_count (3rd) */
                emit_mov_reg_imm64(cb, JIT_ARG4, (uint64_t)(uintptr_t)ip);    /* ip (4th) */
                /* 5th param (chunk ptr): Windows x64 passes it as the first
                 * stack argument at [RSP+32]; System V passes it in R8. */
#ifdef _WIN32
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)(uintptr_t)cur_chunk);
                emit_mov_mem8_reg(cb, JIT_RSP, 32, JIT_RAX);
#else
                emit_mov_reg_imm64(cb, JIT_ARG5, (uint64_t)(uintptr_t)cur_chunk);
#endif
                EMIT_CALL(jit_callout_struct_init);
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
                /* Pop arg_count arg slots from JIT virtual stack,
                 * push 1 result → net -(arg_count - 1) */
                int pop_bytes = arg_count * 8;
                if (pop_bytes <= 127) {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x83); emit_byte(cb, 0xC4);
                    emit_byte(cb, (uint8_t)pop_bytes);
                } else {
                    emit_byte(cb, 0x48); emit_byte(cb, 0x81); emit_byte(cb, 0xC4);
                    emit_uint32(cb, (uint32_t)pop_bytes);
                }
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= (arg_count - 1);
                break;
            }

