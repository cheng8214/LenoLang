/* ============================================================================
 * ops_icmp.inc - Int comparison / immediate comparison / shift cases (EQ..GE_INT, *_INT_IMM, SHL/SHR/USHR_IMM)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Comparisons (push val_bool，与解释器一致：TRUE_VAL/FALSE_VAL) ---- */
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
                /* §8.125：两个操作数都要运行期确认是 int48（bigint 静态也是 int ✗，
                 * 直接按位型比较会给出静默错误的真假值）⇒ 任一不合格即 bailout 交解释器。 */
                EMIT_INT48_CHECK_REG(JIT_RDX, bc_off);
                EMIT_INT48_CHECK(bc_off);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                /* 裸 0/1 → val_bool：解释器这里压的是 val_bool，不是 int48 的 0/1 */
                EMIT_RAW01_TO_BOOLVAL();
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
                /* §8.125：bigint 的静态类型也是 int ✗ ⇒ 必须运行期确认操作数是 int48，
                 * 否则按位型/指针比较会给出**静默错误的真假值**；不合格就 bailout。 */
                EMIT_INT48_CHECK(bc_off);
                /* cmp rax, imm8 (sign-extended) */
                emit_byte(cb, 0x48);
                emit_byte(cb, 0x83);
                emit_byte(cb, modrm(3, 7, JIT_RAX & 7));  /* /7 = CMP */
                emit_byte(cb, (uint8_t)imm);
                /* setcc al; movzx eax, al (don't xor between cmp and setcc — destroys flags!) */
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                /* 裸 0/1 → val_bool（同上：不能把 raw 0/1 当成 int 留下来） */
                EMIT_RAW01_TO_BOOLVAL();
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
                /* §8.125：**必须**运行期确认操作数是 int48。原先依据"静态类型是 int
                 * ⇒ int48"（扫描器保证）省掉了这一步 —— 但 **bigint 的静态类型也是 int**
                 * ⇒ 该推理不成立 ✗：bigint 在虚拟栈里是 NaN-boxed 值，直接 shl/sar 移的是
                 * 它的位型/指针 ⇒ 静默算错（实测 sha512：add64 的 `a >> 32`，a 是 bigint）。
                 * 通用 OP_SHL/OP_SHR/OP_USHR 一直有这步检查（ops_icmp.inc 的通用分支），
                 * 只有 IMM/折叠形态漏了 ⇒ 表现为「只有常量移位量坏、动态移位量走 bailout 正常」。 */
                EMIT_INT48_CHECK(bc_off);
                emit_shl_imm(cb, JIT_RAX, (uint8_t)imm);
                /* VM 语义：int64 左移后 val_int() 截断为 48 位，不 bailout */
                EMIT_INT48_TRUNCATE();
                MARK_TOS_RAW_INT48();   /* §8.41 白名单：截断后必在 int48 内 */
                TOS_PRODUCE();
                break;
            }
            case OP_SHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                EMIT_INT48_CHECK(bc_off);   /* §8.125：同 OP_SHL_IMM —— bigint 也静态是 int */
                emit_sar_imm(cb, JIT_RAX, (uint8_t)imm);
                MARK_TOS_RAW_INT48();   /* §8.41 白名单：算术右移结果必在 int48 内 */
                TOS_PRODUCE();
                break;
            }
            case OP_USHR_IMM: {
                int8_t imm = rd_byte(ip + 1);
                TOS_CONSUME_RAX();
                EMIT_INT48_CHECK(bc_off);   /* §8.125：同上（bigint 走 bailout 交解释器） */
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

            /* ---- 通用相等比较（对齐解释器 OP_EQ / OP_NEQ）----
             * 1) int48 对 int48 → 整数比较（相等性与符号无关，用 JE/JNE）
             * 2) 两边都不是 NaN-boxed → float 比较。IEEE 语义下与 NaN 的比较
             *    恒为「不相等」：unordered 时 == 必须是 false、!= 必须是 true。
             *    UCOMISD 无序时置 PF=ZF=CF=1，于是 SETE 会误判成真（需再 AND
             *    SETNP 压下去），SETNE 会误判成假（需再 OR SETP 抬上来）。
             * 3) 任一是 NaN-boxed → bailout。解释器这条路径上还要处理字符串
             *    按内容相等、数组逐元素、其它对象按指针、BigInt 以及 FFI 指针
             *    的 null 比较，语义复杂又非热点，一律交回解释器。---- */
            case OP_EQ: case OP_NEQ: {
                int is_neq = (op == OP_NEQ);
                uint8_t cc = is_neq ? 0x5 : 0x4;      /* JNE  / JE    */
                uint8_t pf = is_neq ? 0x0A : 0x0B;    /* SETP / SETNP */
                int slow_jcc_b, slow_jcc_a;           /* 非 int48 → 慢路径 */
                int tagged_jcc_a, tagged_jcc_b;       /* NaN-boxed → bailout */
                int end_jmp, skip_bail_jmp;

                /* 必须先把两个操作数都取到寄存器再判定类型（见 8.16 的坑）。
                 * b（右）→ RDX，a（左）→ RAX */
                TOS_CONSUME_TO(JIT_RDX);
                TOS_CONSUME_RAX();

                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_b = emit_jcc(cb, 0x87);      /* ja slow */

                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                slow_jcc_a = emit_jcc(cb, 0x87);      /* ja slow */

                /* int 快路径 */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                end_jmp = emit_jmp(cb);

                /* ---- 慢路径 ---- */
                patch_rel32(cb, slow_jcc_b, cb->len);
                patch_rel32(cb, slow_jcc_a, cb->len);
                /* 类型检查直接跳过来的：RAX/RDX 仍完好，越过下面的「恢复块」 */
                int slow_body_jmp = emit_jmp(cb);

                /* ---- 身份比较快路径（对象 / 指针）----------------------------
                 * 解释器 OP_EQ 的规则（op_compare.inc）：两个 OBJ 值类型相同时，
                 * 只有 STRING 按内容、ARRAY 逐元素、BIGINT 按值比较，
                 * **其余一切对象都是对象身份比较**（含 struct / dict / Ptr）。
                 * 这里补的就是最后这条：值位模式相等 ⟺ 对象身份相等。
                 *
                 * 判定为什么放在**运行时**，而不是让 codegen 按静态类型发新 opcode：
                 * `Ptr[T]` 是 ObjFFIPointer，而 `ffi.nullptr()` / `ffi.ptr_from_int(0)`
                 * 会产生**包装 NULL 的 ObjFFIPointer**，解释器对「null == Ptr」走的是
                 * 「看包装地址」的特殊规则（此时为真）—— 纯身份比较会给假。
                 * 任何「编译期断言这就是身份比较」的谓词都不安全（要么改变语义，
                 * 要么必须把 FFI 的 null 规则也搬进来，那就不是身份比较了）。
                 *
                 * 这里不满足条件一律**落到下面的数值慢路径**（NaN-boxed 交由
                 * callout 计算，见本 case 末尾的桩）。
                 * ★ 跳走前 RAX 已被改写成 type_a，所以数值慢路径入口必须先从
                 *   tmp1/tmp2 恢复 a/b —— 见下方「入口分两处」的说明。
                 *
                 * 动机：matrix_rain 的 `_texCache[i].ren == renderer`（两个 Ptr）
                 * 以前每次都 bailout，整个循环跑不进 JIT（§8.33）。 */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);      /* a */
                EMIT_STORE_TMP(tmp2_disp, JIT_RDX);      /* b */
                int id_no_b, id_no_a, id_neq_type, id_is_str, id_is_arr, id_is_bi, id_done;

                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);  /* b 是 OBJ？（必须 SHR） */
                id_no_b = emit_jcc(cb, 0x85);                   /* JNE → 数值慢路径 */

                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);  /* a 是 OBJ？ */
                id_no_a = emit_jcc(cb, 0x85);

                /* type_b → R8；type_a → RAX（a/b 已存入 tmp，可放心占用） */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_and_rr(cb, JIT_R8, JIT_R10);               /* obj_b */
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_R8, 0);     /* type_b（32 位零扩展） */
                emit_and_rr(cb, JIT_RAX, JIT_R10);              /* obj_a */
                emit_mov_reg32_mem8(cb, JIT_RAX, JIT_RAX, 0);   /* type_a */
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                id_neq_type = emit_jcc(cb, 0x85);               /* 类型不同 → 慢路径 */

                /* 排除「== 不是身份」的三种对象类型 */
                emit_cmp_reg32_imm32(cb, JIT_RAX, (uint32_t)OBJ_STRING);
                id_is_str = emit_jcc(cb, 0x84);                 /* JE → 慢路径 */
                emit_cmp_reg32_imm32(cb, JIT_RAX, (uint32_t)OBJ_ARRAY);
                id_is_arr = emit_jcc(cb, 0x84);
                emit_cmp_reg32_imm32(cb, JIT_RAX, (uint32_t)OBJ_BIGINT);
                id_is_bi = emit_jcc(cb, 0x84);

                /* 身份比较 = 值位模式相等 */
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_LOAD_TMP(JIT_RDX, tmp2_disp);
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                emit_setcc_reg(cb, cc, JIT_RAX);
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                id_done = emit_jmp(cb);

                /* ---- 数值慢路径（int48 已在上方处理；此处只剩 float 或 NaN-boxed）----
                 * 入口分两处：
                 *   ① int48 类型检查直接跳来的 —— RAX/RDX 完好；
                 *   ② 身份判定中途放弃跳来的 —— 那条路上 RAX 已被改写成 type_a，
                 *      必须先从 tmp1/tmp2 恢复原始 a/b，否则后面的数值/NaN-boxed 判定
                 *      拿「类型编号」当操作数（字符串/数组比较会被算成不等；以前这段
                 *      紧跟着就 bailout，被解释器回退掩盖了，移除 bailout 后立刻暴露）。 */
                patch_rel32(cb, id_no_b, cb->len);
                patch_rel32(cb, id_no_a, cb->len);
                patch_rel32(cb, id_neq_type, cb->len);
                patch_rel32(cb, id_is_str, cb->len);
                patch_rel32(cb, id_is_arr, cb->len);
                patch_rel32(cb, id_is_bi, cb->len);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);                   /* a */
                EMIT_LOAD_TMP(JIT_RDX, tmp2_disp);                   /* b */

                patch_rel32(cb, slow_body_jmp, cb->len);
                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, tagged_jcc_a);   /* xmm0 = a */
                EMIT_NUM_TO_XMM(1, JIT_RDX, JIT_R8, tagged_jcc_b);   /* xmm1 = b */
                emit_ucomisd_rr(cb, 0, 1);                           /* flags: a ? b */
                emit_setcc_reg(cb, cc, JIT_RAX);      /* AL = SETE / SETNE */
                emit_setcc_reg(cb, pf, JIT_RDX);      /* DL = SETNP / SETP（无序修正位） */
                if (is_neq) {
                    emit_byte(cb, 0x08);              /* OR  AL, DL：unordered → 真 */
                } else {
                    emit_byte(cb, 0x20);              /* AND AL, DL：unordered → 假 */
                }
                emit_byte(cb, modrm(3, JIT_RDX & 7, JIT_RAX & 7));
                emit_movzx_r32_r8(cb, JIT_RAX, JIT_RAX);
                skip_bail_jmp = emit_jmp(cb);

                /* ---- callout 桩（NaN-boxed：字符串/数组/对象/null/BigInt/FFI 指针）----
                 * 解释器在这条路径上还要处理「字符串按内容相等、数组逐元素、其它对象
                 * 按身份、BigInt 按值、Ptr/cfunc 与 null 看包装地址」等规则。以前一律
                 * bailout，于是 `x != null` / `s != ""` 这种**极常见**的比较会让整个
                 * 循环退回解释器（file_manager 的 _topRects / _colW / Table.render /
                 * _ensure_fitted_range 全部因此打不出 JIT 效果，同 §8.33 的老问题）。
                 * 现在交 jit_callout_value_eq 完成（语义与解释器 OP_EQ 逐条对齐），
                 * 循环继续留在机器码里跑。
                 * 进入本桩时 RAX = a、RDX = b —— EMIT_NUM_TO_XMM 的 tagged 跳转不会
                 * 破坏这两个寄存器（只写 xmm 与 scratch R8）。 */
                patch_rel32(cb, tagged_jcc_a, cb->len);
                patch_rel32(cb, tagged_jcc_b, cb->len);
                EMIT_STORE_TMP(tmp2_disp, JIT_RDX);      /* b（raw） */
                EMIT_RAW_TO_VALUE();                     /* RAX = a 的 NaN-boxed Value */
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);       /* a → RDX */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);       /* b（raw） */
                EMIT_RAW_TO_VALUE();                     /* RAX = b 的 NaN-boxed Value */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);        /* b → R8 */
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);      /* a */
                emit_mov_rr(cb, JIT_ARG2, JIT_R8);       /* b */
                emit_mov_reg_imm64(cb, JIT_ARG3, is_neq ? 1 : 0);   /* 0 = ==, 1 = != */
                EMIT_CALL(jit_callout_value_eq);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                int callout_done = emit_jmp(cb);         /* RAX 已是 NaN-boxed bool */

                patch_rel32(cb, end_jmp, cb->len);
                patch_rel32(cb, skip_bail_jmp, cb->len);
                patch_rel32(cb, id_done, cb->len);
                /* int 快路径 / 身份比较 / float 慢路径在此汇合，RAX 都是裸 0/1 → val_bool */
                EMIT_RAW01_TO_BOOLVAL();
                patch_rel32(cb, callout_done, cb->len);  /* callout 结果跳过 0/1 转换 */
                TOS_PRODUCE();
                vstack--;
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
                /* int 快路径与 float 慢路径在此汇合，RAX 都是裸 0/1 → val_bool */
                EMIT_RAW01_TO_BOOLVAL();
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

