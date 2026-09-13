/* ============================================================================
 * ops_loop.inc - Loop control cases (FOR_PREP, LOOP, FOR_LOOP)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Inner for-loop init: OP_FOR_PREP ---- */
            case OP_FOR_PREP: {
                /* 8 bytes: op, start_slot, end_slot, step_slot,
                 *         loop_var_slot, inclusive, jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t start_sl = ip[1];
                uint8_t end_sl   = ip[2];
                uint8_t step_sl  = ip[3];
                uint8_t lv_sl    = ip[4];
                uint8_t incl     = ip[5];
                uint16_t joff    = rd_short(ip + 6);

                int si_start = cur_local_map[start_sl];
                int si_end   = cur_local_map[end_sl];
                int si_step  = cur_local_map[step_sl];
                int si_lv    = cur_local_map[lv_sl];
                int d_start = scratch_disp(si_start);
                int d_end   = scratch_disp(si_end);
                int d_step  = scratch_disp(si_step);
                int d_lv    = scratch_disp(si_lv);

                /* step 为 float（类型位图 bit=1）→ JIT 的 FOR_LOOP 走 int48 快路径，
                 * 无法处理 double 位模式的方向/比较 → 交回解释器 */
                if (si_step >= 0) {
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x0F);
                    emit_byte(cb, 0xBA);
                    emit_byte(cb, 0xE3);  /* BT RBX, imm8 */
                    emit_byte(cb, (uint8_t)(si_step & 0xFF));
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int step_float_bail = emit_jcc(cb, 0x82);  /* JC -> bailout */
                    patch_add(ctx, step_float_bail, -1, 0);
                }

                /* Check step != 0 (bailout if step == 0)
                 * §8.45：这四个槽的访问全部走宏 —— 它们可能已被 pin（见
                 * pick_pin_locals 的三元组优先），此时读写都落在寄存器上。 */
                EMIT_LOAD_LOCAL(JIT_RAX, si_step, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                int step_bail = emit_jcc(cb, 0x84);  /* JE (step==0) → bailout */
                patch_add(ctx, step_bail, -1, 0);

                /* step 符号分流：step < 0 走反向初始条件检查（支持倒序循环） */
                EMIT_LOAD_LOCAL(JIT_RAX, si_step, d_step);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                int prep_neg_patch = emit_jcc(cb, 0x88);  /* JS -> negative init check */

                /* Set loop_var = start */
                EMIT_LOAD_LOCAL(JIT_RAX, si_start, d_start);
                EMIT_STORE_LOCAL(JIT_RAX, si_lv, d_lv);

                /* Check initial condition: start vs end (step > 0) */
                EMIT_LOAD_LOCAL(JIT_RDX, si_end, d_end);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* step > 0, exclusive: JGE (start >= end) → skip inner loop
                 * step > 0, inclusive: JG  (start > end)  → skip inner loop */
                uint8_t cc_skip = incl ? 0x8F /*JG*/ : 0x8D /*JGE*/;
                int skip_patch = emit_jcc(cb, cc_skip);
                /* Forward jump: target = bc_off + 8 + joff (past inner body + FOR_LOOP) */
                int skip_target = bc_off + size + (int)joff;
                patch_add(ctx, skip_patch, skip_target, 0);

                int prep_done_patch = emit_jmp(cb);   /* 正向检查结束，跳过反向块 */

                /* ---- 反向初始条件 (step < 0)：跳过条件与正向相反 ----
                 * exclusive: start <= end -> skip；inclusive: start < end -> skip */
                patch_rel32(cb, prep_neg_patch, cb->len);
                EMIT_LOAD_LOCAL(JIT_RAX, si_start, d_start);
                EMIT_STORE_LOCAL(JIT_RAX, si_lv, d_lv);
                EMIT_LOAD_LOCAL(JIT_RDX, si_end, d_end);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                {
                    uint8_t cc_skip_neg = incl ? 0x8C /*JL*/ : 0x8E /*JLE*/;
                    int skip_patch_neg = emit_jcc(cb, cc_skip_neg);
                    patch_add(ctx, skip_patch_neg, skip_target, 0);
                }
                patch_rel32(cb, prep_done_patch, cb->len);
                break;
            }

            /* ---- Back-edge: OP_LOOP ---- */
            case OP_LOOP: {
                /* 5 bytes: op + 4-byte big-endian int32 backward offset */
                TOS_SPILL();
                int32_t loff = rd_int32(ip + 1);
                int target_bc = bc_off + size - loff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) {
                    /* 回边目标必须是循环体内已发射的指令。找不到 = off_map 条目被丢
                     * （容量溢出）或扫描/字节码失配。旧实现静默退化成「跳到外层循环
                     * 起点」，结果是死循环 + RSP 漂移 → 栈溢出（§8.26 同类）。
                     * 按「宁可不编，不要猜」拒绝这次编译。 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] LOOP-FAIL(OP_LOOP): 回边 target_bc=%d "
                                        "不在 off_map（off_count=%d inline_depth=%d）→ 拒绝编译\n",
                                target_bc, ctx->off_count, inline_depth);
                    return 0;
                }
                /* ★ 回边 GC 轮询（§8.37 路线 3）。三个条件缺一不可：
                 * 1) `bc_off + size == sr->body_size` —— **只认 JIT 自己那条回边**
                 *    （编译体的最后一条指令）。嵌套回边与内联 callee 里的回边都
                 *    不能发射：让出时解释器会用它自己的 `frame->ip`（= 外层 OP_LOOP）
                 *    减去 offset 从**外层循环头**继续，而那半轮已经被执行过 ——
                 *    实测「每个 yield 多算一轮迭代」（sum 6.398e+06 → 6.671e+06）。
                 * 2) `body_has_callout` —— 循环体里发射过 callout（可能分配）。
                 *    纯算术热循环零开销（否则给 2.5 cycle/iter 的 i++ 加 2 条指令
                 *    就是百分比级的税）。
                 * 3) 补丁点没超上限（防御性）。
                 * 轮询本身 2 条指令：mov r8, imm64(&jit_gc_yield_flag); cmp [r8], 0
                 * —— 借的是"GC 想让 JIT 停一下"这个全局标志（HotSpot polling page
                 * 的同一形状：轮询一个由 VM 侧置位的标志），命中才付让出的代价。
                 * 成本实测（§8.39，10 亿次 arr[0] 的极紧循环）：无轮询 1906ms →
                 * 2 条指令 2015ms（+0.11ns/轮，≈5.7%）。这是本机制的下界形态：
                 * 只由「循环体里有 callout」的循环支付，而那种循环每轮本来要付
                 * 一次 callout（~1ns 起）⇒ 真实分配循环上远小于 1%。
                 * 位置在 jmp 回边**之前**，语义上正是解释器 OP_LOOP 的执行点，
                 * 所以解释器接手时 `frame->ip -= offset` 恰好从循环头继续、
                 * 不重放本轮（这正是 bailout 做不到的，见 §8.37）。 */
                if (body_has_callout && yield_jmp_cnt < JIT_YIELD_MAX &&
                    bc_off + size == sr->body_size) {
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_gc_yield_flag);
                    emit_cmp_mem8_imm8(cb, JIT_R8, 0, 0);
                    yield_jmp_patches[yield_jmp_cnt++] = emit_jcc(cb, 0x85);  /* JNZ → yield */
                }
                int patch = emit_jmp(cb);
                patch_rel32(cb, patch, target_mc);
                break;
            }

            /* ---- Back-edge: OP_FOR_LOOP ---- */
            case OP_FOR_LOOP: {
                /* 7 bytes: op, lv_slot, step_slot, end_slot, inclusive,
                 *         jump_off_hi, jump_off_lo */
                TOS_SPILL();
                uint8_t lv_sl  = ip[1];
                uint8_t st_sl  = ip[2];
                uint8_t en_sl  = ip[3];
                uint8_t incl   = ip[4];
                uint16_t joff  = rd_short(ip + 5);

                int si_lv = cur_local_map[lv_sl];
                int si_st = cur_local_map[st_sl];
                int si_en = cur_local_map[en_sl];
                int d_lv = scratch_disp(si_lv);
                int d_st = scratch_disp(si_st);
                int d_en = scratch_disp(si_en);
                /* §8.45：三元组（loop_var / step / end）若都驻留寄存器，回边这道
                 * 「增量 + 比较」就能直接在寄存器上做 —— 省掉 4 次访存。整组才有效：
                 * 只 pin 一两个只能把访存换成寄存器搬移，指令数中性（§8.43）。 */
                int lv_reg = pin_reg_of(pin_si, pin_n, si_lv);
                int st_reg = pin_reg_of(pin_si, pin_n, si_st);
                int en_reg = pin_reg_of(pin_si, pin_n, si_en);
                int trio_reg = (lv_reg && st_reg && en_reg) ? lv_reg : 0;

                int target_bc = bc_off + size - (int)joff;
                int target_mc = offmap_lookup(ctx, target_bc);
                if (target_mc < 0) {
                    /* 同 OP_LOOP：回边找不到目标就拒绝编译，绝不退化成外层循环起点 */
                    if (jit_debug_on())
                        fprintf(stderr, "[JIT-DEBUG] LOOP-FAIL(OP_FOR_LOOP): 回边 target_bc=%d "
                                        "不在 off_map（off_count=%d inline_depth=%d）→ 拒绝编译\n",
                                target_bc, ctx->off_count, inline_depth);
                    return 0;
                }

                /* ★ 回边 GC 轮询（§8.37 路线 3，同 OP_LOOP）。位置要求更严：
                 * **必须放在 EMIT_FOR_STEP_AND_CMP 之前** —— 那一段会执行
                 * `loop_var += step`，而解释器接手时会自己跑 OP_FOR_LOOP 的
                 * 增量语义；放在它后面就会多加一次（表现为迭代跳步）。
                 * 只认 JIT 自己那条回边（理由同 OP_LOOP：嵌套/内联回边让出会
                 * 从外层循环头重跑）。 */
                if (body_has_callout && yield_jmp_cnt < JIT_YIELD_MAX &&
                    bc_off + size == sr->body_size) {
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_gc_yield_flag);
                    emit_cmp_mem8_imm8(cb, JIT_R8, 0, 0);
                    yield_jmp_patches[yield_jmp_cnt++] = emit_jcc(cb, 0x85);  /* JNZ → yield */
                }

                /* loop_var += step (+ int48 check)，再与 end 比较；
                 * 继续循环的跳转由方向决定（正/负步长各一份）。 */
                #define EMIT_FOR_STEP_AND_CMP() do { \
                    if (trio_reg) { \
                        /* 全寄存器形态：add lv, step / cmp lv, end */ \
                        emit_add_rr(cb, lv_reg, st_reg); \
                        EMIT_INT48_CHECK_REG(lv_reg, bc_off); \
                        emit_cmp_rr(cb, lv_reg, en_reg); \
                    } else { \
                        EMIT_LOAD_LOCAL(JIT_RAX, si_lv, d_lv); \
                        EMIT_LOAD_LOCAL(JIT_RDX, si_st, d_st); \
                        emit_add_rr(cb, JIT_RAX, JIT_RDX); \
                        EMIT_INT48_CHECK(bc_off); \
                        EMIT_STORE_LOCAL(JIT_RAX, si_lv, d_lv); \
                        EMIT_LOAD_LOCAL(JIT_RDX, si_en, d_en); \
                        emit_cmp_rr(cb, JIT_RAX, JIT_RDX); \
                    } \
                } while(0)

                /* step 符号分流：step == 0 不回溯（落到循环之后）；
                 * step < 0 走反向比较（倒序循环） */
                if (trio_reg) {
                    emit_test_rr(cb, st_reg, st_reg);
                } else {
                    EMIT_LOAD_LOCAL(JIT_RAX, si_st, d_st);
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                }
                int zero_patch = emit_jcc(cb, 0x84);   /* JZ -> after (step == 0) */
                int neg_patch  = emit_jcc(cb, 0x88);   /* JS -> backward direction */

                /* ---- forward (step > 0): continue while lv <(=) end ---- */
                EMIT_FOR_STEP_AND_CMP();
                {
                    uint8_t cc = incl ? 0x8E /*JLE*/ : 0x8C /*JL*/;
                    int patch = emit_jcc(cb, cc);
                    patch_rel32(cb, patch, target_mc);
                }
                int done_patch = emit_jmp(cb);

                /* ---- backward (step < 0): continue while lv >(=) end ---- */
                patch_rel32(cb, neg_patch, cb->len);
                EMIT_FOR_STEP_AND_CMP();
                {
                    uint8_t cc_neg = incl ? 0x8D /*JGE*/ : 0x8F /*JG*/;
                    int patch = emit_jcc(cb, cc_neg);
                    patch_rel32(cb, patch, target_mc);
                }
                patch_rel32(cb, done_patch, cb->len);
                patch_rel32(cb, zero_patch, cb->len);

                #undef EMIT_FOR_STEP_AND_CMP
                break;
            }

