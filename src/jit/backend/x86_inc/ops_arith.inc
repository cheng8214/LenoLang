/* ============================================================================
 * ops_arith.inc - Integer arithmetic + bitwise cases (add/sub/mul/mod/neg/cast_int, and/or/xor/not)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Arithmetic ---- */
            case OP_ADD_INT:
                TOS_CONSUME_TO(JIT_RDX);   /* right → RDX */
                TOS_CONSUME_TO(JIT_RAX);   /* left → RAX */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_SUB_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            /* ---- 通用 ADD（对齐解释器 OP_ADD 语义）----
             * 1) int48 + int48 → 整数加（溢出检查；溢出交解释器走 BigInt）
             * 2) 两边都不是 NaN-boxed（即都是裸数值）→ float 加
             *    （int48 操作数用 CVTSI2SD 提升；结果保持 float，同 val_float）
             * 3) 任一是 NaN-boxed（字符串/对象/null）→ concat callout
             *    （任一 ObjString 即拼接；双方都非字符串时返回 NULL_VAL →
             *     bailout 回解释器保持原语义）---- */
            case OP_ADD: {
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_r, tagged_jcc_l;      /* NaN-boxed → concat */
                int end_jmp, skip_cat_jmp;

                /* b（右）→ RAX，a（左）→ RDX */
                TOS_CONSUME_RAX();
                TOS_CONSUME_TO(JIT_RDX);

                /* b（RDX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                /* a（RAX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径 */
                emit_add_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);              /* 跳过慢路径 */

                /* ---- 慢路径：float 加 / concat 分派 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                /* 两个操作数都规范化为 double；任一是 NaN-boxed 则跳去 concat */
                EMIT_NUM_TO_XMM(0, JIT_RDX, JIT_R8, tagged_jcc_r);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RAX, JIT_R8, tagged_jcc_l);   /* xmm1 = b */

                emit_sse2_rr(cb, 0x58, 0, 1);        /* ADDSD xmm0, xmm1 */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                skip_cat_jmp = emit_jmp(cb);         /* 跳过 concat 段 */

                /* ---- concat 段：jit_callout_concat(result = a+b) ----
                 * 栈序：先弹右操作数（RAX），后弹左操作数（RDX）。
                 * concat(左, 右) → arg1=RDX(左)、arg2=RAX(右)。 */
                patch_rel32(cb, tagged_jcc_r, cb->len);
                patch_rel32(cb, tagged_jcc_l, cb->len);

                /* ---- §8.103：歧义区不猜（concat 需要精确类型）----
                 * 裸位型里 float 与 int48 在 `[0, 2^47)` 上**重合**（float 侧 = +0.0 与正次正规数），
                 * 而下面两次 EMIT_RAW_TO_VALUE 的启发式（raw>>47 ∈ {-1,0} ⇒ 当 int48）会把
                 * 浮点 0.0 贴成 int 0 ⇒ `"" + p`（p 是 float 0.0）拼出 "0" 而不是 "0.0" ✗。
                 * 实测：jit_probes/probe_float_param_faithful.leno（JIT "0" vs NO_JIT "0.0"）。
                 * ⇒ 任一操作数落在歧义区就 bailout，交解释器按**真实类型**拼接（正确性优先；
                 * 代价是小非负整数参与拼接的循环会回退，后续用 TOS 静态类型跟踪消除，见 §8.103）。 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_test_rr(cb, JIT_R8, JIT_R8);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _amb_a = emit_jcc(cb, 0x84);    /* JZ（左操作数歧义）→ bailout */
                    patch_add(ctx, _amb_a, -1, 0);
                }
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_test_rr(cb, JIT_R8, JIT_R8);
                /* §8.116：右操作数（TOS）若由**白名单指令**产出（`prev_int48`，见 §8.41 的
                 * EMIT_INT48_CHECK_TOS / MARK_TOS_RAW_INT48）⇒ 它**已被证明是裸 int48** ✓
                 * ⇒ 位型启发式不会被浮点撞码，不必 bail ✓。这一条正是最常见的形态
                 * `"" + (i + 1)`（`i+1` 走 ADD_INT_IMM 白名单 ✓）—— 修前它每次迭代都 bail，
                 * 把 drawHUD 的循环整块拉黑 ✗（PvZ 实测 ✓）。
                 * 注意：`prev_int48` 只对**紧邻的上一条指令**有效（一次性 ✓），而右操作数恰好
                 * 就是它 ✓；left 侧没有这个保证 ⇒ 保持原检查（不猜 ✓）。 */
                if (!prev_int48) {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _amb_b = emit_jcc(cb, 0x84);    /* JZ（右操作数歧义）→ bailout */
                    patch_add(ctx, _amb_b, -1, 0);
                }

                /* RAX=右(raw), RDX=左(raw) → Value（int 转 int48 Value，对象保持） */
                EMIT_RAW_TO_VALUE();                 /* RAX = Value(右) */
                emit_push_reg(cb, JIT_RAX);          /* 暂存 Value(右) */
                emit_mov_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_RAW_TO_VALUE();                 /* RAX = Value(左) */
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);   /* RDX = Value(左) */
                emit_pop_reg(cb, JIT_RAX);           /* RAX = Value(右) */

                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);  /* arg1 = Value(左) */
                emit_mov_rr(cb, JIT_ARG2, JIT_RAX);  /* arg2 = Value(右) */
                EMIT_CALL(jit_callout_concat);
                EMIT_CALLOUT_END();

                /* 结果 top16==0xfff8（NULL_VAL）→ concat 不支持 → bailout */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                {   /* shr r8, 48 */
                    int _b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, _b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                    emit_byte(cb, 48);
                }
                {   /* cmp r8, 0xfff8 */
                    int _b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, _b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFF8);
                }
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int _p = emit_jcc(cb, 0x84);   /* je → bailout（双方均非字符串） */
                    patch_add(ctx, _p, -1, 0);
                }

                /* 成功：结果透传为 TOS（float 为裸 double；concat 结果为字符串 Value，
                 * 不做 int48 转换） */
                patch_rel32(cb, end_jmp, cb->len);
                patch_rel32(cb, skip_cat_jmp, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- 通用 SUB（对齐解释器 OP_SUB 语义）----
             * 1) int48 - int48 → 整数减（溢出交解释器走 BigInt）
             * 2) 两边都不是 NaN-boxed（即都是裸数值）→ float 减
             *    （int48 操作数用 CVTSI2SD 提升；结果保持 float，同 val_float）
             * 3) 任一是 NaN-boxed（对象/null/bool）→ bailout
             *    （BigInt 运算与类型错误交回解释器）---- */
            case OP_SUB: {
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_a, tagged_jcc_b;      /* NaN-boxed → bailout */
                int end_jmp, skip_bail_jmp;

                /* b（右）→ RDX，a（左）→ RAX。
                 * 必须先把两个操作数都取到寄存器再判定类型：慢路径的 float 加
                 * 需要完整的 a/b，若把判定夹在两次消费之间，从 b 的判定跳走时
                 * a 还没被 pop 出来（被跳过的 pop 会让 RAX 停在 b 上）。 */
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_RAX();

                /* b（RDX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                /* a（RAX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径：RAX = a - b */
                emit_sub_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);

                /* ---- 慢路径：float 减 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, tagged_jcc_a);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RDX, JIT_R8, tagged_jcc_b);   /* xmm1 = b */
                emit_sse2_rr(cb, 0x5C, 0, 1);        /* SUBSD xmm0, xmm1 */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                skip_bail_jmp = emit_jmp(cb);

                /* ---- bailout 桩：NaN-boxed 操作数交回解释器 ---- */
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
            /* ---- 通用 MUL（对齐解释器 OP_MUL 语义）----
             * 1) int48 * int48 → 整数乘；int64 溢出或超出 int48 → bailout
             *    （解释器在此升 BigInt，bailout 后由解释器接管）
             * 2) 两边都不是 NaN-boxed（即都是裸数值）→ float 乘
             *    （int48 操作数用 CVTSI2SD 提升；结果保持 float，同 val_float）
             * 3) 任一是 NaN-boxed（null/字符串/对象/BigInt）→ bailout
             *    （null 报错、BigInt 运算、类型错误全交解释器）---- */
            case OP_MUL: {
                int slow_jcc_b, slow_jcc_a;          /* 非 int48 → 慢路径 */
                int tagged_jcc_a, tagged_jcc_b;      /* NaN-boxed → bailout */
                int end_jmp, skip_bail_jmp;

                /* 必须先把两个操作数都取到寄存器再判定类型（见 8.16 的坑）。
                 * b（右）→ RDX，a（左）→ RAX */
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_RAX();

                /* b（RDX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);     /* ja slow */

                /* a（RAX）非 int48 → 慢路径 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);     /* ja slow */

                /* int 快路径：RAX = a * b。
                 * 两个检查缺一不可：INT64_OVF 捕 int64 溢出，INT48 捕
                 * 「不溢出 int64 但超 48 位」（解释器此时已升 BigInt）。 */
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK(bc_off);
                EMIT_INT48_CHECK(bc_off);
                end_jmp = emit_jmp(cb);

                /* ---- 慢路径：float 乘 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);

                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, tagged_jcc_a);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RDX, JIT_R8, tagged_jcc_b);   /* xmm1 = b */
                emit_sse2_rr(cb, 0x59, 0, 1);        /* MULSD xmm0, xmm1 */
                /* movq rax, xmm0 */
                emit_byte(cb, 0x66); emit_byte(cb, 0x48); emit_byte(cb, 0x0F);
                emit_byte(cb, 0x7E); emit_byte(cb, modrm(3, 0, 0));
                skip_bail_jmp = emit_jmp(cb);

                /* ---- bailout 桩：NaN-boxed 操作数交回解释器 ---- */
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
            /* ---- 通用 MOD（对齐解释器 OP_MOD 语义）----
             * 解释器的 OP_MOD **没有 float 路径**：int%int → int（除数为 0 报错）；
             * null → 报错；BigInt → bigint_mod；其余（含 float）→ 类型错误。
             * 所以这里只做 int48 快路径，其余一律 bailout 交解释器，
             * 绝不能照抄 OP_SUB 的 float 中间段。
             * 另外需要自行挡掉除数为 0——OP_MOD_INT 靠编译器保证非零，通用版没有这保证，
             * 直接 idiv 会触发 #DE 硬件异常。挡下后交解释器抛「取模除零错误」。
             * 余数必然落在 int48 内（|a%b| < |b|），无需 INT48 检查。 */
            case OP_MOD: {
                int chk[3];
                int nc = 0;
                int end_jmp;

                TOS_CONSUME_TO(JIT_R8);    /* b（右）= 除数 */
                TOS_CONSUME_TO(JIT_RAX);   /* a（左）= 被除数 */

                /* b 非 int48 → bailout（用 RDX 当 scratch，R8 要留给 idiv） */
                emit_mov_rr(cb, JIT_RDX, JIT_R8);
                emit_sar_imm(cb, JIT_RDX, 47);
                emit_inc_reg(cb, JIT_RDX);
                emit_cmp_reg_imm8(cb, JIT_RDX, 1);
                chk[nc++] = emit_jcc(cb, 0x87);      /* ja → bail */

                /* a 非 int48 → bailout */
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                emit_sar_imm(cb, JIT_RDX, 47);
                emit_inc_reg(cb, JIT_RDX);
                emit_cmp_reg_imm8(cb, JIT_RDX, 1);
                chk[nc++] = emit_jcc(cb, 0x87);      /* ja → bail */

                /* 除数 == 0 → bailout */
                emit_test_rr(cb, JIT_R8, JIT_R8);
                chk[nc++] = emit_jcc(cb, 0x84);      /* je → bail */

                /* int 快路径：RAX = a % b（余数在 RDX） */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                TOS_PRODUCE_FROM_RDX();
                end_jmp = emit_jmp(cb);

                /* ---- bailout 桩 ---- */
                for (int _i = 0; _i < nc; _i++)
                    patch_rel32(cb, chk[_i], cb->len);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                {
                    int _p = emit_jmp(cb);
                    patch_add(ctx, _p, -1, 0);
                }

                patch_rel32(cb, end_jmp, cb->len);
                vstack--;
                break;
            }
            case OP_MUL_INT:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_imul_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT64_OVF_CHECK(bc_off);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_MOD_INT:
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                /* 除数为 0 时解释器抛「取模除零错误」，而 idiv 0 会触发 #DE
                 * 直接把进程干掉 → 先挡下交解释器（与通用 OP_MOD 的策略一致）。 */
                emit_test_rr(cb, JIT_R8, JIT_R8);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jcc(cb, 0x84); patch_add(ctx, _p, -1, 0); }  /* jz → bailout */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                TOS_PRODUCE_FROM_RDX();
                MARK_TOS_RAW_INT48();   /* §8.41 白名单：余数必然落在 int48 内 */
                vstack--;
                break;
            case OP_NEG_INT:
                TOS_CONSUME_RAX();
                emit_neg_reg(cb, JIT_RAX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                break;
            case OP_CAST_INT: {
                /* 解释器语义（vm/vminc/op_unary.inc OP_CAST_INT）：
                 *   BigInt → int48（超范围保持原值）；int/float → 向零截断为 int；
                 *   bool → 1/0；null 保持 null；其余抛「类型转换 int: 操作数必须是数字」。
                 * 旧实现是「no-op（假定已经是 int）」，只在操作数确实是 int48 时成立：
                 * `int i = (a > b)` / `int i = 2.7` 都会补 OP_CAST_INT，操作数是
                 * bool / float。把 NaN-boxed 的 TRUE_VAL 当 int 留在栈上，写回 locals
                 * 时会按 int 重装箱、把位模式搅成垃圾（且布尔语义彻底丢失）。 */
                /* ---- §8.41 窥孔：这个 CAST 是恒等变换吗？----
                 * 编译器为「非字面量赋给 int/float/string 局部量」无条件插 CAST
                 * （assign_cast_needed 对非字面量一律返回 1，见 codegen_stmt.c）。
                 * 但只要**紧邻的前一条指令**已经用 int48 检查证明了结果就是 raw
                 * int48，且那个结果仍作为 TOS 躺在 RAX 里，本 CAST 的全部工作就是
                 * 「已是 int48 → 原样」，可以整段省掉（4 条检查 + 冷路径）。
                 *
                 * 三个前提缺一不可：
                 *  1) prev_int48 —— 前一条指令在白名单里（EMIT_INT48_CHECK_TOS /
                 *     MARK_TOS_RAW_INT48 的调用点）。多路径的 opcode（OP_ADD/SUB/MUL
                 *     通用版有 int/float/concat 三条路、USHR_IMM 的 shr 会越界）不在
                 *     白名单里：它们的非 int 路径不会执行那个检查。
                 *  2) tos_live —— 值确实还在 RAX（不是被 spill 到内存栈了）。
                 *  3) 本 bc_off 不是跳转目标、且不在内联体内 —— 三目/短路的合并点会
                 *     落在 CAST 上，从别的路径跳进来时前一条指令不是那个检查。 */
                if (prev_int48 && tos_live && inline_depth == 0 &&
                    !bc_is_jump_target(ctx, bc_off)) {
                    break;   /* 值已是 raw int48 且是 TOS：语义等价于原样放行 */
                }
                TOS_CONSUME_RAX();
                /* 1) int48 → 原样 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                int ci_int_p = emit_jcc(cb, 0x86);         /* jbe → .done（int48 原样） */
                /* 2) 裸 double → 向零截断（VM: (int64_t)val_as_num(a)）；
                 *    超 int48 范围（含 NaN/Inf 落成 INT64_MIN）交解释器处理 */
                emit_mov_reg_imm64(cb, JIT_R8, JIT_NAN_SIG);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int ci_double_p = emit_jcc(cb, 0x82);      /* jb → .from_double */
                /* 3) NaN-boxed：TRUE_VAL → 1，FALSE_VAL → 0，NULL_VAL → 原样，其余 bailout */
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)TRUE_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int ci_true_p = emit_jcc(cb, 0x84);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)FALSE_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int ci_false_p = emit_jcc(cb, 0x84);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)NULL_VAL);
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int ci_null_p = emit_jcc(cb, 0x84);
                /* 其余 NaN-boxed（BigInt/对象/字符串）→ bailout 交解释器 */
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jmp(cb); patch_add(ctx, _p, -1, 0); }

                /* .from_double */
                patch_rel32(cb, ci_double_p, cb->len);
                emit_cvttsd2si_r64_xmm(cb, JIT_RAX, 0);
                EMIT_INT48_CHECK(bc_off);
                int ci_double_done_p = emit_jmp(cb);
                /* .true */
                patch_rel32(cb, ci_true_p, cb->len);
                emit_mov_reg_imm64(cb, JIT_RAX, 1);
                int ci_true_done_p = emit_jmp(cb);
                /* .false */
                patch_rel32(cb, ci_false_p, cb->len);
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                /* .null / .done：NULL_VAL 原样落回来 */
                patch_rel32(cb, ci_null_p, cb->len);
                patch_rel32(cb, ci_true_done_p, cb->len);
                patch_rel32(cb, ci_double_done_p, cb->len);
                patch_rel32(cb, ci_int_p, cb->len);
                TOS_PRODUCE();
                break;
            }

            case OP_CAST_STRING: {
                /* 解释器语义（vm/vminc/op_unary.inc:100 OP_CAST_STRING）：
                 *   null        → 保持 null
                 *   string      → 原样
                 *   其它类型    → val_to_string 生成新字符串
                 * JIT 只做「已经是 string 对象」的快路径 —— 把字符串赋给 string
                 * 变量时最常见；其余（null/int/float/bool/其它对象）bailout 交解释器，
                 * 这样字符串化结果与报错信息与解释器完全一致，JIT 也无需实现格式化。 */
                TOS_CONSUME_RAX();
                /* 1) NaN-box tag 必须是 TAG_OBJ (top16 == 0xFFFC) */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                {   /* shr r8, 48 */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                    emit_byte(cb, 48);
                }
                {   /* cmp r8, 0x0000FFFC */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFFC);
                }
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                int cast_str_not_obj = emit_jcc(cb, 0x85);   /* JNE → bailout */
                patch_add(ctx, cast_str_not_obj, -1, 0);
                /* 2) obj->type 必须是 OBJ_STRING(0)：r8 = rax & PAYLOAD_MASK → 指针，
                 *    cmp byte ptr [r8], OBJ_STRING（Object.type 在偏移 0） */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_and_rr(cb, JIT_R8, JIT_R10);
                {
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(0, 0, 0, b));
                    emit_byte(cb, 0x80);
                    emit_byte(cb, modrm(1, 7, JIT_R8 & 7));
                    emit_byte(cb, 0x00);                     /* disp8 = 0（type 偏移） */
                    emit_byte(cb, (uint8_t)OBJ_STRING);
                }
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                int cast_str_not_string = emit_jcc(cb, 0x85);  /* JNE → bailout */
                patch_add(ctx, cast_str_not_string, -1, 0);
                TOS_PRODUCE();                               /* 值不变，放回 TOS */
                break;
            }

            /* ---- Extra integer ops ---- */
            case OP_DIV_INT: {
                /* 与解释器（vm/vminc/op_type_specialized.inc:119 的
                 * `val_int(a / b)`）对齐：**int / int → int48**（向零截断的整除）。
                 * 旧实现把商 `CVTSI2SD` 成 double 再压栈，结果类型与解释器不一致：
                 *   ① 紧随其后的 OP_MOD_INT 会拿 double 的位模式做整数 idiv →
                 *      `(arr[i] / exp) % 10` 算出的 digit 全是垃圾
                 *      （examples/排序测试/更多排序.leno 的基数排序因此排错并越界）；
                 *   ② `q is int` / 后续 int48 溢出判定与比较语义同样跟着错。
                 * 注意 float 除法走的是 OP_DIV_FLOAT（任一侧为 float 时由编译器选它）。 */
                TOS_CONSUME_TO(JIT_R8);    /* divisor */
                TOS_CONSUME_TO(JIT_RAX);   /* dividend */
                /* 除数为 0：解释器抛「整数除零错误」，idiv 0 会 #DE 崩进程 → bailout。
                 * （被除数是 int48，故 INT64_MIN / -1 的溢出场景不可能出现。） */
                emit_test_rr(cb, JIT_R8, JIT_R8);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                { int _p = emit_jcc(cb, 0x84); patch_add(ctx, _p, -1, 0); }  /* jz → bailout */
                emit_cqo(cb);
                emit_idiv_reg(cb, JIT_R8);
                /* 商必然落在 int48 值域内（|a/b| <= |a|），唯一例外是 INT48_MIN / -1
                 * = 2^47；超界就 bailout，让解释器按 val_int 的截断语义处理。 */
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            }
            case OP_BITAND:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_and_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_or_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITXOR:
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_TO(JIT_RAX);
                emit_xor_rr(cb, JIT_RAX, JIT_RDX);
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                vstack--;
                break;
            case OP_BITNOT:
                TOS_CONSUME_RAX();
                {
                    int b = 0;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xF7);
                    emit_byte(cb, modrm(3, 2, JIT_RAX & 7));  /* /2 = NOT */
                }
                EMIT_INT48_CHECK_TOS(bc_off);   /* §8.41 白名单 */
                TOS_PRODUCE();
                break;

