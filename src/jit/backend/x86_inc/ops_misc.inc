/* ============================================================================
 * ops_misc.inc - Misc cases (OP_NULL/TRUE/FALSE, try/catch/finally no-ops, default: unsupported -> bailout)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Simple constant pushes ---- */
            case OP_NULL:
                TOS_SPILL();
                /* 必须压真正的 NULL_VAL（QNAN|SIGN_BIT|TAG_NULL），不能压字面量 0：
                 * val_is_null(v) 是「v == NULL_VAL」的精确比较，而 0 是 int48 的 0
                 * —— 压 0 会让 JIT 里的 null 与整数 0 无法区分：`x == null` 在 x 为
                 * 0 时误判为真、`null == null` 误判为假（while cur != null 直接死循环）。
                 * OP_TRUE/OP_FALSE 同样必须与 VM 的 val_bool 表示一致。
                 * 历史：以前 OP_EQ/OP_NEQ 遇到 NaN-boxed 就 bailout，比较全交解释器，
                 * 这个错误表示被掩盖；EQ/NEQ 改走 callout 后立刻暴露成死循环。 */
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)NULL_VAL);
                TOS_PRODUCE();
                vstack++;
                break;
case OP_TRUE:
                TOS_SPILL();
                /* Leno true value = NaN-boxed bool true (must match VM TRUE_VAL,
                 * otherwise is_falsey/val_is_bool misclassify JIT-sunk bools) */
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)TRUE_VAL);
                TOS_PRODUCE();
                vstack++;
                break;
            case OP_FALSE:
                TOS_SPILL();
                emit_mov_reg_imm64(cb, JIT_RAX, (uint64_t)FALSE_VAL);
                TOS_PRODUCE();
                vstack++;
                break;

            /* ---- OP_LENGTH（`.len()`）—— 第 5 节 L1 ----
             * 解释器语义（vm/vminc/op_utils.inc OP_LENGTH）：数字 → `(int)value_to_double`
             *   向零截断、负数 clamp 到 0（`for n to i` 依赖后者）；string→char_len、
             *   array→count、dict→order_count、enum_def→member_count、cstruct_array→count、
             *   struct→def->field_count；其余抛「LENGTH操作需要对象或数字类型」。
             * 分工：数字走原生（`for i < arr.len()` 是热路径，不值当为它付 callout），
             *   对象/非法类型走 tagged 出口 → callout（与解释器同一份语义）。
             * 错误路径**不在 JIT 里造**：callout 只置 jit_callout_failed → bailout →
             *   解释器重放本条指令，报错文本/行号与 NO_JIT 完全一致。
             * 两条路径的结果都是 int（callout 侧经 EMIT_VALUE_TO_RAW）⇒ 可标 raw int48。 */
            case OP_LENGTH: {
                TOS_CONSUME_RAX();                                   /* RAX = 操作数（raw） */
                int len_tagged;
                EMIT_NUM_TO_XMM(0, JIT_RAX, JIT_R8, len_tagged);     /* xmm0 = 数值 */
                /* 与解释器的 `(int)double` 对齐：**32 位** CVTTSD2SI + movsxd。
                 * 不能用 64 位版本 —— 超出 int32 的输入在 x64 硬件上得到 INT32_MIN，
                 * 再被下面的负值 clamp 成 0（解释器侧就是这条路径）；
                 * 64 位版本会保留 3e9 这类值，造成 JIT/解释器结果不一致。 */
                emit_byte(cb, 0xF2); emit_byte(cb, 0x0F); emit_byte(cb, 0x2C);
                emit_byte(cb, modrm(3, 0, 0));                       /* cvttsd2si eax, xmm0 */
                emit_byte(cb, 0x48); emit_byte(cb, 0x63); emit_byte(cb, 0xC0);  /* movsxd rax, eax */
                emit_test_rr(cb, JIT_RAX, JIT_RAX);                  /* CVTTSD2SI 不动 EFLAGS */
                int len_nonneg = emit_jcc(cb, 0x89);                 /* JNS → 非负 */
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);                   /* 负数 → 0 */
                patch_rel32(cb, len_nonneg, cb->len);
                MARK_TOS_RAW_INT48();                                /* §8.41：结果必在 int48 */
                int len_done = emit_jmp(cb);

                /* ---- callout 桩：NaN-boxed 操作数（string/array/dict/enum/…）---- */
                patch_rel32(cb, len_tagged, cb->len);
                EMIT_RAW_TO_VALUE();                                 /* tagged ⇒ 已是 Value，恒等 */
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RAX);
                EMIT_CALL(jit_callout_length);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                /* callout 对非法类型只置 failed（不自己造错误）→ bailout 交解释器 */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int len_fail = emit_jcc(cb, 0x85);               /* JNZ → bailout */
                    patch_add(ctx, len_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);

                patch_rel32(cb, len_done, cb->len);
                TOS_PRODUCE();
                break;      /* 栈净效应 0：pop 1 push 1，vstack 不变 */
            }

            /* ---- Exception handling opcodes (no-op in JIT) ---- */
            /* The JIT skips try/catch/finally setup entirely.
             * In normal execution (no exception), these are effectively
             * no-ops: OP_TRY sets up catch_ip (skipped), OP_JUMP after
             * try body skips the catch body, OP_END_TRY restores state.
             * If an exception occurs from a callout, the JIT bails out
             * and the interpreter re-executes with full try/catch. */
            case OP_TRY:
            case OP_CATCH:
            case OP_FINALLY:
            case OP_END_TRY:
                break;

            default:
                /* Should not reach here if scan passed */
                if (jit_debug_on())
                    fprintf(stderr, "[JIT-DEBUG] codegen FAIL: unsupported opcode %d at bc_off=%d\n", op, bc_off);
                return 0;
