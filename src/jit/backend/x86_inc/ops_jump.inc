/* ============================================================================
 * ops_jump.inc - Jumps and fused compare+jump cases (JUMP, JUMP_IF_FALSE/TRUE, CMPJMP_LL/LG_INT)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
            /* ---- Jumps ---- */
            case OP_JUMP: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                TOS_SPILL();  /* jump targets expect tos_live=0 */
                int patch = emit_jmp(cb);
                patch_add(ctx, patch, target_bc, vstack);
                /* After unconditional jump, fall-through is unreachable.
                 * Mark vstack so the restore block knows to pick up
                 * the correct vstack from a patch record at the next
                 * reachable bc_off. */
                vstack = VSTACK_UNREACHABLE;
                break;
            }
            case OP_SWITCH_LOOKUP: {
                /* switch 跳转表（§8.61 覆盖面 R1）。解释器语义见 op_switch_lookup.inc：
                 * 弹 switch 值 → 在 case 值数组里二分查找 → 命中跳对应 case 体，否则跳 default 体。
                 * 编码：const_idx(2) case_count(2) default_off(4) [body_off(4)]...，
                 * **偏移基准 = 指令起点 + size**（= 偏移表之后）。
                 *
                 * JIT 把"查找"整个交给 callout（它调 VM 的 switch_lookup_index —— 语义唯一
                 * 来源），只把返回的**下标**编译成线性比较链：
                 *     cmp rax,0 / je case0 …  / jmp default
                 * case 少时足够（本项目 switch 一般 ≤ 十几个 case），且不需要间接跳转表、
                 * 不需要在代码段里放地址表。
                 *
                 * 这条指令永不失败（不分配、不报错、无类型错误语义）⇒ **没有 bailout 分支**：
                 * 任何值都能得到一个确定的目标（命中或 default），与解释器逐字节等价。 */
                uint16_t const_idx = rd_short(ip + 1);
                uint16_t case_count = rd_short(ip + 3);
                int32_t default_off = rd_int32(ip + 5);
                int base_bc = bc_off + size;

                /* case 值数组：编译期从常量表取（与解释器同一来源，常量表是 GC 根）。
                 * 非数组 / 越界 → 传 NULL_VAL → callout 返回 -1 → 走 default，
                 * 与解释器「不是 OBJ_ARRAY 就 matched_index 保持 -1」一致。 */
                Value cases_val = NULL_VAL;
                if (ctx->chunk && ctx->chunk->constants &&
                    (int)const_idx < ctx->chunk->const_cnt) {
                    cases_val = ctx->chunk->constants[const_idx];
                }

                TOS_CONSUME_RAX();                     /* 弹 switch 值（raw）→ RAX，tos_live=0 */
                EMIT_RAW_TO_VALUE();                   /* raw → NaN-boxed Value（R8 是 scratch） */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);    /* 先落 tmp 槽，跨 callout 序言取用 */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp1_disp);    /* switch 值 */
                emit_mov_reg_imm64(cb, JIT_ARG2, (uint64_t)cases_val);   /* case 值数组 */
                emit_mov_reg_imm64(cb, JIT_ARG3, (uint32_t)case_count);
                EMIT_CALL(jit_callout_switch_lookup);
                EMIT_CALLOUT_END();                    /* RAX = 匹配下标（-1 = default） */
                for (int k = 0; k < (int)case_count; k++) {
                    emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uint32_t)k);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int case_p = emit_jcc(cb, 0x84);   /* JE → case 体 */
                    patch_add(ctx, case_p, base_bc + rd_int32(ip + 9 + k * 4), vstack);
                }
                {
                    int def_p = emit_jmp(cb);
                    patch_add(ctx, def_p, base_bc + default_off, vstack);
                }
                vstack = VSTACK_UNREACHABLE;   /* 跳转表之后没有 fall-through */
                break;
            }
            case OP_JUMP_IF_FALSE: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                if (tos_live) {
                    /* RAX already holds the condition value.
                     * Proper falsey check: NaN-boxed FALSE_VAL (0xFFF9..)
                     * and NULL_VAL (0xFFF8..) are non-zero, so a simple
                     * test rax,rax is insufficient. Must also compare
                     * against FALSE_VAL and NULL_VAL. */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);   /* JZ = falsey (zero: int 0 / float +0.0) */
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);  /* JE = falsey (FALSE_VAL) */
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);  /* JE = falsey (NULL_VAL) */
/* Truthy: jump over falsey code */
                    int truthy_jmp = emit_jmp(cb);
                    /* Falsey path: JUMP_IF_FALSE is peek-only in the VM —
                     * the condition value stays on the stack for the target
                     * instruction (normally an OP_POP) to consume.  The
                     * target was compiled in the linear fall-through state
                     * (tos_live=0, vstack as recorded), so its OP_POP is
                     * "add rsp,8" popping exactly one memory entry.  We must
                     * push RAX here so that pop consumes OUR value; otherwise
                     * the add rsp,8 would pop garbage and desync the
                     * runtime stack (e.g. OP_INDEX pops a wrong obj operand). */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    emit_push_reg(cb, JIT_RAX);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, vstack);
                    /* Patch truthy jump to here */
                    patch_rel32(cb, truthy_jmp, cb->len);
                    /* Fall-through (truthy): RAX still holds condition,
                     * next OP_POP will TOS_DISCARD */
                    tos_live = 1;
                } else {
                    /* Value on memory stack: peek + conditional jump (no pop) */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);  /* mov rax, [rsp] */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);
                    /* Truthy: jump over falsey code */
                    int truthy_jmp = emit_jmp(cb);
                    /* Falsey path: value stays on stack for target POP */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, vstack);
                    /* Patch truthy jump to here */
                    patch_rel32(cb, truthy_jmp, cb->len);
                    /* Fall-through: value still on stack, next OP_POP pops it */
                }
                break;
            }
            case OP_JUMP_IF_TRUE: {
                int32_t off = rd_int32(ip + 1);
                int target_bc = bc_off + size + off;
                if (tos_live) {
                    /* RAX already holds the condition value.
                     * If falsey (zero, FALSE_VAL, NULL_VAL): skip jump.
                     * Otherwise (truthy): push and jump to target. */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);   /* JZ = falsey (zero) */
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);  /* JE = falsey (FALSE_VAL) */
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);  /* JE = falsey (NULL_VAL) */
                    /* Truthy: push condition to stack and jump to target */
                    emit_push_reg(cb, JIT_RAX);
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, vstack);
                    /* Patch all falsey jumps to here (skip) */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    /* Fall-through: tos_live stays 1 */
                } else {
                    /* Value on memory stack: peek + conditional jump (no pop) */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0x24);  /* mov rax, [rsp] */
                    emit_test_rr(cb, JIT_RAX, JIT_RAX);
                    int jz_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, FALSE_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je1_patch = emit_jcc(cb, 0x84);
                    emit_mov_reg_imm64(cb, JIT_R8, NULL_VAL);
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int je2_patch = emit_jcc(cb, 0x84);
                    /* Truthy: value stays on stack, jump to target */
                    int jmp_patch = emit_jmp(cb);
                    patch_add(ctx, jmp_patch, target_bc, vstack);
                    /* Patch all falsey jumps to here (skip) */
                    patch_rel32(cb, jz_patch, cb->len);
                    patch_rel32(cb, je1_patch, cb->len);
                    patch_rel32(cb, je2_patch, cb->len);
                    /* Fall-through: value still on stack, next OP_POP pops it */
                }
                break;
            }

            /* ---- Fused compare + jump ---- */
            case OP_CMPJMP_LL_INT: {
                uint8_t cmp_op = ip[1];
                uint16_t sa = rd_short(ip + 2);
                uint16_t sb = rd_short(ip + 4);
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                int si_a = cur_local_map[sa];
                int si_b = cur_local_map[sb];
                int da = scratch_disp(si_a);
                int db = scratch_disp(si_b);
                TOS_SPILL();  /* clobbers RAX/RDX */
                /* mov rax, [rbp+da]; mov rdx, [rbp+db]; cmp rax, rdx */
                if (da >= -128 && da <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)da);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, da);
                }
                if (db >= -128 && db <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RBP, (int8_t)db);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_RBP, db);
                }
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Jump if comparison is FALSE (i.e., NOT the condition) */
                /* cmp_op: 0=EQ,1=NE,2=LT,3=GT,4=LE,5=GE */
                /* We need: jump if NOT condition → invert the condition code */
                uint8_t cc;
                switch (cmp_op) {
                    case 0: cc = 0x85; break;  /* EQ → JNE (jump if not eq) */
                    case 1: cc = 0x84; break;  /* NE → JE  (jump if not ne = eq) */
                    case 2: cc = 0x8D; break;  /* LT → JGE (jump if not lt) */
                    case 3: cc = 0x8E; break;  /* GT → JLE (jump if not gt) */
                    case 4: cc = 0x8F; break;  /* LE → JG  (jump if not le) */
                    case 5: cc = 0x8C; break;  /* GE → JL  (jump if not ge) */
                    default: cc = 0x85; break;
                }
                int patch = emit_jcc(cb, cc);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }

            /* ---- CMPJMP: local int vs 立即数 + conditional jump ---- */
            /* Operands: cmp_op(1) slot(2) imm32(4) offset(4) = 11 bytes + opcode = 12 */
            case OP_CMPJMP_LI_INT: {
                uint8_t cmp_op = ip[1];
                uint16_t sa = rd_short(ip + 2);
                int32_t imm = rd_int32(ip + 4);
                int32_t off = rd_int32(ip + 8);
                int target_bc = bc_off + size + off;
                int si_a = cur_local_map[sa];
                int da = scratch_disp(si_a);
                TOS_SPILL();  /* clobbers RAX */
                /* Load local (raw int48) from scratch area: mov rax, [rbp+da]
                 * （该槽已被 pin_excl 排除，内存副本权威，与 LL/LG 同一约定） */
                if (da >= -128 && da <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)da);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, da);
                }
                /* cmp rax, imm32（符号扩展） */
                emit_cmp_reg_imm32(cb, JIT_RAX, (uint32_t)imm);
                /* Jump if comparison is FALSE（与 LL/LG 同一套反转） */
                uint8_t cc;
                switch (cmp_op) {
                    case 0: cc = 0x85; break;  /* EQ → JNE (jump if not eq) */
                    case 1: cc = 0x84; break;  /* NE → JE  (jump if not ne = eq) */
                    case 2: cc = 0x8D; break;  /* LT → JGE (jump if not lt) */
                    case 3: cc = 0x8E; break;  /* GT → JLE (jump if not gt) */
                    case 4: cc = 0x8F; break;  /* LE → JG  (jump if not le) */
                    case 5: cc = 0x8C; break;  /* GE → JL  (jump if not ge) */
                    default: cc = 0x85; break;
                }
                int patch = emit_jcc(cb, cc);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }

            /* ---- CMPJMP: local int vs global int + conditional jump ---- */
            /* Operands: cmp_op(1) slot(2) global_idx(2) offset(4) = 9 bytes + opcode = 10 */
            case OP_CMPJMP_LG_INT: {
                uint8_t cmp_op = ip[1];
                uint16_t sa = rd_short(ip + 2);
                uint16_t gi = rd_short(ip + 4);
                int32_t off = rd_int32(ip + 6);
                int target_bc = bc_off + size + off;
                int si_a = cur_local_map[sa];
                int da = scratch_disp(si_a);
                int gd = gi * 8;
                TOS_SPILL();  /* clobbers RAX */
                /* Load local (raw int48) from scratch area: mov rax, [rbp+da] */
                if (da >= -128 && da <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RAX, JIT_RBP, (int8_t)da);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RAX, JIT_RBP, da);
                }
                /* Load global (NaN-boxed int) from globals: mov rdx, [r9+gd] */
                if (gd >= -128 && gd <= 127) {
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_R9, (int8_t)gd);
                } else {
                    emit_mov_reg_mem32(cb, JIT_RDX, JIT_R9, gd);
                }
                /* Extract int48 payload from NaN-boxed value: shl rdx,16; sar rdx,16 */
                {
                    int b = (JIT_RDX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 4, JIT_RDX & 7));  /* /4 = SHL */
                    emit_byte(cb, 16);
                }
                {
                    int b = (JIT_RDX >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 7, JIT_RDX & 7));  /* /7 = SAR */
                    emit_byte(cb, 16);
                }
                /* cmp rax, rdx */
                emit_cmp_rr(cb, JIT_RAX, JIT_RDX);
                /* Jump if comparison is FALSE (same inversion as CMPJMP_LL_INT) */
                uint8_t cc;
                switch (cmp_op) {
                    case 0: cc = 0x85; break;  /* EQ → JNE */
                    case 1: cc = 0x84; break;  /* NE → JE  */
                    case 2: cc = 0x8D; break;  /* LT → JGE */
                    case 3: cc = 0x8E; break;  /* GT → JLE */
                    case 4: cc = 0x8F; break;  /* LE → JG  */
                    case 5: cc = 0x8C; break;  /* GE → JL  */
                    default: cc = 0x85; break;
                }
                int patch = emit_jcc(cb, cc);
                patch_add(ctx, patch, target_bc, vstack);
                break;
            }

