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

            /* ---- OP_IS_NULL（`?.` / `??` 编译出）—— R2 批次 1 ----
             * 解释器语义（vm/vminc/op_compare.inc）：弹栈顶 → push val_bool(val_is_null(v))，
             * net 0。解释器注释写明它存在就是为了**避开 OP_EQ 的 `0.0 == null` 陷阱**，
             * 所以判据必须是 val_is_null（位比较 NULL_VAL），不能用"等于零"。
             * 纯判断、不分配、不报错 ⇒ 无失败通道、无 bailout 分支。
             * 不做原生位比较的原因：JIT 虚拟栈有"裸 int48 / 裸 double / NaN-boxed"三态，
             * 而 null 只对应 NULL_VAL 一种位模式 —— 交给 callout 与解释器共用同一判据最稳。 */
            case OP_IS_NULL: {
                TOS_CONSUME_RAX();                     /* 弹操作数（raw）→ RAX，tos_live=0 */
                EMIT_RAW_TO_VALUE();                   /* 裸 int48 → 装箱；其余原样 */
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RAX);
                EMIT_CALL(jit_callout_is_null);
                EMIT_CALLOUT_END();                    /* RAX = bool（NaN-boxed） */
                EMIT_VALUE_TO_RAW();                   /* bool 非 int ⇒ 恒等，统一收尾 */
                TOS_PRODUCE();
                break;                                 /* net 0 */
            }

            /* ---- R5-P1：OP_CLOSURE（**仅零捕获** C0）----
             * 语义与 vm/vminc/op_call.inc 的 OP_CLOSURE 在 upvalue_count==0 时一致：
             * 每次执行都新建一个闭包对象（身份语义与解释器一致，设计文档 I4），
             * gc_alloc 全块清零 ⇒ upvalues[] 全 NULL、type_param_* 为 0（§2.1 V9）。
             * 捕获表非空的形态（C1/C2/C3）在 scan 阶段已拒收，callout 再复核一次
             * （成本一次比较）：不符即 bailout 交解释器，绝不建"空壳闭包"。
             * 记账：弹 0 压 1 ⇒ vstack++。先 TOS_SPILL 把上一个 TOS 落到内存栈，
             * 保证 callout 期间（RAX 会被覆盖）栈形态一致 —— 与 OP_CALL 同款收尾。 */
            case OP_CLOSURE: {
                uint16_t clo_ci = rd_short(ip + 1);
                Value clo_fn = NULL_VAL;
                if (ctx->chunk && ctx->chunk->constants && (int)clo_ci < ctx->chunk->const_cnt)
                    clo_fn = ctx->chunk->constants[clo_ci];   /* 常量表是 GC 根 ⇒ 无需额外保活 */
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                emit_mov_reg_imm64(cb, JIT_ARG1, (uint64_t)clo_fn);
                EMIT_CALL(jit_callout_make_closure);
                EMIT_CALLOUT_END();                       /* RAX = 闭包（NaN-boxed） */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int clo_fail = emit_jcc(cb, 0x85);    /* JNZ → bailout */
                    patch_add(ctx, clo_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();                      /* 对象是 NaN-boxed ⇒ 恒等 */
                TOS_PRODUCE();
                vstack++;
                break;                                    /* pop0 push1 → net +1 */
            }

            /* ---- R5-P2：OP_GET_UPVALUE（读捕获变量）----
             * 弹 0 压 1。闭包指针取自**本帧固定槽**（序言存入的第三参），slot 是 2 字节操作数。
             * 三级守卫（闭包 NULL / slot 越界 / upvalue NULL）都在 callout 里做，失败即
             * bailout 交解释器 —— 机器码绝不 NULL 解引用。
             * ⚠ 每次都走 callout 重新取 `upvalue->location`（设计文档不变量 I2）：
             * open→closed 转换会改写 location，任何形式的缓存都会读到过期地址。 */
            case OP_GET_UPVALUE: {
                uint16_t uv_get_slot = rd_short(ip + 1);
                TOS_SPILL();
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, closure_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)uv_get_slot);
                EMIT_CALL(jit_callout_upvalue_get);
                EMIT_CALLOUT_END();                       /* RAX = 捕获变量的 Value */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int uv_get_fail = emit_jcc(cb, 0x85);   /* JNZ → bailout */
                    patch_add(ctx, uv_get_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_VALUE_TO_RAW();                      /* tagged int → 裸 int48（统一收尾） */
                TOS_PRODUCE();
                vstack++;
                break;                                    /* pop0 push1 → net +1 */
            }

            /* ---- R5-P2：OP_SET_UPVALUE（写捕获变量）----
             * **peek TOS**（不弹栈），net 0 —— 与 VM 的 `*location = peek(0)` 一致。
             * 顺序很关键：先 TOS_SPILL 把 TOS 的**原始表示**（可能是裸 int48 / 裸 double）
             * 原样落进内存栈，再读出来装箱；反过来做会让 RAW→VALUE 的转换污染内存栈里的表示，
             * 后续按"裸"解释这条槽的指令就会算错。 */
            case OP_SET_UPVALUE: {
                uint16_t uv_set_slot = rd_short(ip + 1);
                TOS_SPILL();                              /* 保住 TOS 原始表示，tos_live=0 */
                TOS_PEEK_TO(JIT_RAX);                     /* 读出 TOS（内存栈顶，不消费） */
                EMIT_RAW_TO_VALUE();                      /* 裸 int48 → 装箱（只影响这次副本） */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, closure_disp);
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)uv_set_slot);
                EMIT_LOAD_TMP(JIT_ARG3, tmp1_disp);
                EMIT_CALL(jit_callout_upvalue_set);
                EMIT_CALLOUT_END();
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int uv_set_fail = emit_jcc(cb, 0x85);   /* JNZ → bailout */
                    patch_add(ctx, uv_set_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                break;                                    /* net 0：TOS 保持不动 */
            }

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
