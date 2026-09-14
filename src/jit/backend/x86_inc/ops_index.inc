/* ============================================================================
 * ops_index.inc - Index / array / dict callout cases (OP_INDEX, ARRAY_APPEND_NOPUSH, DICT_SET, INDEX_SET_NOPUSH)
 *
 * Included INSIDE compile_loop()'s switch(op) in backend/x86_64.c.
 * NOT a translation unit: relies on function-local macros (TOS_*,
 * EMIT_*) and locals (cb, ctx, sr, ip, vstack, tos_live, ...).
 * ========================================================================== */
/* ---- Callout: OP_INDEX (array/dict index access) ----
             * Fast path: Array[int] index → native load (no C callout).
             * Slow path: convert both operands to Values and callout
             * (dict / non-int index / out-of-bounds / error). */
            case OP_INDEX: {
                TOS_SPILL();
                /* Pop idx + obj (raw stack forms): RAX = idx, RDX = obj.
                 * Keep both in scratch for the slow path below. */
                emit_pop_reg(cb, JIT_RAX);
                emit_pop_reg(cb, JIT_RDX);
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                EMIT_STORE_TMP(tmp2_disp, JIT_RDX);

                /* Fast path conditions (violated ⇒ jump to slow):
                 *  1. idx is int48      (raw>>47 +1 <= 1)
                 *  2. obj is object     (NaN-box top16 == 0xFFFC, TAG_OBJ)
                 *  3. obj->type == OBJ_ARRAY  (type at offset 0, OBJ_ARRAY == 1)
                 *  4. (uint64)idx < arr->count (int32 at offset 40,
                 *     ObjArray: Object header 32B + Value* elements @32 + int count @40)
                 * Fast load: rax = arr->elements[idx], then raw-convert. */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                int p_idx_notint = emit_jcc(cb, 0x87);   /* JA → slow */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                {   /* shr r8, 48 */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0xC1);
                    emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                    emit_byte(cb, 48);
                }
                {   /* cmp r8, 0x0000FFFC (top16 == TAG_OBJ) */
                    int b = (JIT_R8 >> 3) & 1;
                    emit_byte(cb, rex(1, 0, 0, b));
                    emit_byte(cb, 0x81);
                    emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                    emit_uint32(cb, 0x0000FFFC);
                }
                int p_obj_notobj = emit_jcc(cb, 0x85);   /* JNE → slow */
                /* Unbox obj: rdx = (rdx & PAYLOAD_MSK) → real pointer.
                 * RDX still holds the NaN-boxed raw (0xFFFC<<48 | ptr). */
                emit_and_rr(cb, JIT_RDX, JIT_R10);
                /* cmp byte ptr [rdx], OBJ_ARRAY */
                emit_byte(cb, 0x80);
                emit_byte(cb, 0x7A);                     /* mod01 reg7 rm=RDX */
                emit_byte(cb, 0x00);                     /* disp8 = type offset 0 */
                emit_byte(cb, (uint8_t)OBJ_ARRAY);
                int p_obj_notarr = emit_jcc(cb, 0x85);   /* JNE → slow */
                /* r8 = (uint64)count（**必须 32 位零扩展加载**）; cmp rax, r8;
                 * jae → slow（负数/超界索引按无符号都是巨值 ⇒ 取慢路径报错）。
                 * 用 64 位加载会把相邻的 capacity 一起读进来
                 * （count@40 / capacity@44 相邻 → r8 = count|capacity<<32），
                 * 越界比较因此恒不成立 → JIT 静默读越界内存（§8.28）。 */
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, count));
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int p_idx_oob = emit_jcc(cb, 0x83);      /* JAE → slow */
                /* rdx = arr->elements (+32); rax = [rdx + rax*8] */
                emit_mov_reg_mem8(cb, JIT_RDX, JIT_RDX, 32);
                emit_byte(cb, 0x48);                     /* mov rax, [rdx+rax*8] */
                emit_byte(cb, 0x8B);
                emit_byte(cb, 0x04);
                emit_byte(cb, 0xC2);
                EMIT_VALUE_TO_RAW();
                /* jmp done (rel32 placeholder) */
                emit_byte(cb, 0xE9);
                int done_loc = cb->len;
                emit_uint32(cb, 0);

                /* ---- Slow path: NaN-box both, callout ---- */
                patch_rel32(cb, p_idx_notint, cb->len);
                patch_rel32(cb, p_obj_notobj, cb->len);
                patch_rel32(cb, p_obj_notarr, cb->len);
                /* 注意：p_idx_oob **不**接到这里（见本 case 末尾的 bailout 桩）。
                 * 越界必须交回解释器：callout 只 error_add_at 记录错误然后返回
                 * NULL_VAL，JIT 会继续跑完循环（错误被静默吞掉），而解释器抛的是
                 * 可被 try/catch 捕获的异常 —— 实测 NO_JIT 捕获 239 次、JIT 捕获 0 次。 */
                /* idx → Value in tmp1 */
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                /* obj → Value in RDX */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                /* Callout: arg1=obj, arg2=index */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);
                emit_mov_rr(cb, JIT_ARG2, JIT_R8);
                EMIT_CALL(jit_callout_index);
                EMIT_CALLOUT_END();
                /* Convert result */
                EMIT_VALUE_TO_RAW();
                /* 慢路径执行完必须**跳过**下面的越界 bailout 桩直达 done：
                 * 否则每次非数组索引（dict 取键 / struct 按字段名 / 非 int 索引）
                 * 都会掉进桩里无条件 bailout —— 整个循环永远跑不进 JIT。
                 * 实测：`obj["field"]` 所在循环 100% bailout（matrix_rain 的
                 * invalidateRenderer 每次调用都 bailout 一次）。 */
                int slow_done_loc = emit_jmp(cb);
                /* 数组越界 → bailout 桩。交回解释器重跑本次迭代，由解释器抛出与
                 * NO_JIT 完全一致的「数组索引越界」异常（可被 try/catch 捕获）。
                 * 位置必须在 `done` 标签**之前**：桩以无条件 jmp 结束、没有
                 * fall-through，而 done 紧接着桩之后 —— 否则快路径的 jmp done
                 * 会直接落进桩里，变成"每次读数组都 bailout"（实测 c[0] 循环
                 * 从 47ms 退化到 453ms）。 */
                patch_rel32(cb, p_idx_oob, cb->len);
                EMIT_BAILOUT_SITE_NONOVF(bc_off);
                {
                    int bp = emit_jmp(cb);
                    patch_add(ctx, bp, -1, 0);
                }

                /* done: result in RAX = live TOS */
                patch_rel32(cb, done_loc, cb->len);
                patch_rel32(cb, slow_done_loc, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- OP_ITER_GET / OP_ITER_GET_VALUE（for-in 迭代）—— §5 覆盖面 L2 ----
             * 解释器语义：ITER_GET 在 vm/vminc/op_utils.inc、ITER_GET_VALUE 在
             * vm/vminc/op_iter.inc；两者都是「弹 obj + index，压 1 个结果」（net -1）。
             *   ITER_GET      ：数字→索引本身 / array→元素 / dict→**键** / enum→成员值 /
             *                   string→单字符 / struct→**字段名**
             *   ITER_GET_VALUE：只支持 dict→值、struct→字段值，其余抛错
             * JIT 分工：
             *   - 原生快路径（仅 ITER_GET）：obj 是数组 + index 是 int48 + 索引在界内
             *     → 直接读 elements[index]（`for x in arr` 是最高频形态）。
             *   - 其余（数字迭代 / dict 键值 / enum / string / struct）全交 callout。
             *   - **所有错误与越界**由 callout 置 jit_callout_failed → bailout →
             *     解释器重放本条指令，抛出与 NO_JIT 一致、可被 try/catch 捕获的异常
             *     （只 error_add_at + 返回 NULL 会被 JIT 静默吞掉，见 §8.28）。 */
            case OP_ITER_GET:
            case OP_ITER_GET_VALUE: {
                int it_want_value = (op == OP_ITER_GET_VALUE);
                TOS_SPILL();
                /* 弹出顺序与 OP_INDEX 一致：RAX = index（TOS），RDX = obj */
                emit_pop_reg(cb, JIT_RAX);
                emit_pop_reg(cb, JIT_RDX);
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);      /* index（raw） */
                EMIT_STORE_TMP(tmp2_disp, JIT_RDX);      /* obj（raw） */

                int it_done_loc = -1;
                if (!it_want_value) {
                    /* 快路径守卫（与 OP_INDEX 同构）：index 是 int48、obj 是 OBJ_ARRAY、
                     * (uint64)index < count。任一条不成立 → callout（含越界报错）。 */
                    emit_mov_rr(cb, JIT_R8, JIT_RAX);
                    emit_sar_imm(cb, JIT_R8, 47);
                    emit_inc_reg(cb, JIT_R8);
                    emit_cmp_reg_imm8(cb, JIT_R8, 1);
                    int it_idx_notint = emit_jcc(cb, 0x87);      /* JA → callout */
                    emit_mov_rr(cb, JIT_R8, JIT_RDX);
                    {   /* shr r8, 48 */
                        int b = (JIT_R8 >> 3) & 1;
                        emit_byte(cb, rex(1, 0, 0, b));
                        emit_byte(cb, 0xC1);
                        emit_byte(cb, modrm(3, 5, JIT_R8 & 7));
                        emit_byte(cb, 48);
                    }
                    {   /* cmp r8, 0x0000FFFC（top16 == TAG_OBJ） */
                        int b = (JIT_R8 >> 3) & 1;
                        emit_byte(cb, rex(1, 0, 0, b));
                        emit_byte(cb, 0x81);
                        emit_byte(cb, modrm(3, 7, JIT_R8 & 7));
                        emit_uint32(cb, 0x0000FFFC);
                    }
                    int it_notobj = emit_jcc(cb, 0x85);          /* JNE → callout */
                    emit_and_rr(cb, JIT_RDX, JIT_R10);           /* RDX = Object* */
                    emit_byte(cb, 0x80);
                    emit_byte(cb, 0x7A);                         /* cmp byte [rdx+0] */
                    emit_byte(cb, 0x00);
                    emit_byte(cb, (uint8_t)OBJ_ARRAY);
                    int it_notarr = emit_jcc(cb, 0x85);          /* JNE → callout */
                    /* count 必须 32 位零扩展加载（§8.28：64 位会把相邻 capacity 读进来） */
                    emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, count));
                    emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                    int it_oob = emit_jcc(cb, 0x83);             /* JAE → callout（越界报错） */
                    emit_mov_reg_mem8(cb, JIT_RDX, JIT_RDX, 32); /* rdx = arr->elements */
                    emit_byte(cb, 0x48);
                    emit_byte(cb, 0x8B);
                    emit_byte(cb, 0x04);
                    emit_byte(cb, 0xC2);                         /* mov rax, [rdx + rax*8] */
                    EMIT_VALUE_TO_RAW();
                    it_done_loc = emit_jmp(cb);

                    /* 快路径的 4 个失败出口汇总到 callout 桩入口 */
                    patch_rel32(cb, it_idx_notint, cb->len);
                    patch_rel32(cb, it_notobj, cb->len);
                    patch_rel32(cb, it_notarr, cb->len);
                    patch_rel32(cb, it_oob, cb->len);
                }

                /* ---- callout：obj → ARG1，index → ARG2 ----
                 * ⚠ EMIT_RAW_TO_VALUE() **以 R8 为 scratch**（见其定义）。所以两次转换
                 * 必须都在 RAX 上做完、结果就地写回 tmp 槽，实参寄存器一律留到 callout
                 * 序言里再载入 —— 否则「先把 index 放进 R8，再转换 obj」会把 index 踩掉
                 * （实测：callout 收到的 index 是 obj 转换残留的裸位模式，恒判「index 不是
                 * 数字」→ 每次迭代 bailout）。 */
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);           /* index（raw） */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);          /* 就地写回：index（Value） */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);           /* obj（raw） */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);          /* 就地写回：obj（Value） */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp2_disp);          /* obj */
                EMIT_LOAD_TMP(JIT_ARG2, tmp1_disp);          /* index */
                emit_mov_reg_imm64(cb, JIT_ARG3, it_want_value ? 1 : 0);
                EMIT_CALL(jit_callout_iter_get);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                /* 错误/越界 → bailout 交解释器重放（报错文本与 try/catch 都在那边） */
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                emit_mov_reg_imm64(cb, JIT_R8, (uint64_t)(uintptr_t)&jit_callout_failed);
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_R8, 0);
                emit_test_rr(cb, JIT_RAX, JIT_RAX);
                {
                    EMIT_BAILOUT_SITE_NONOVF(bc_off);
                    int it_fail = emit_jcc(cb, 0x85);            /* JNZ → bailout */
                    patch_add(ctx, it_fail, -1, 0);
                }
                emit_xor_rr(cb, JIT_RAX, JIT_RAX);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                EMIT_LOAD_TMP(JIT_RAX, tmp3_disp);

                if (it_done_loc >= 0) patch_rel32(cb, it_done_loc, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }

            /* ---- OP_ARRAY_APPEND_NOPUSH（arr.add(v) 语句形式）----
             * 内联快路径：目标确为数组、有剩余容量、且不需要写屏障时，直接在机器码
             * 里做 elements[count] = value; count++，不走 C callout。
             * 慢路径保留原 callout（类型检查 / 扩容 / 写屏障 / 报错文本原样）。
             *
             * 顺序约束（重要）：**所有慢路径判定都发生在任何变异之前**，
             * 因此跳到慢路径是安全的 —— callout 会从原状态完整重做，不会重复 append。
             *
             * 寄存器预算：可用的暂存只有 RAX/RCX/RDX/R8（RCX=locals、R9=globals、
             * R10/R11=常量、RBX=类型位图都不能动）；地址一时用不上临时槽时，
             * 用 push/pop 借一下硬件栈（与 OP_ADD 的 concat 路径同一手法）。 */
            case OP_ARRAY_APPEND_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);           /* value（TOS）*/
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RDX);           /* array */
                EMIT_STORE_TMP(tmp2_disp, JIT_RDX);

                /* 1) array 是 NaN-boxed 对象（top16 == TAG_OBJ = 0xFFFC）
                 *    必须用 SHR（逻辑右移）读 top16，SAR 会符号扩展成
                 *    0xFFFF...FFFC 让比较恒不相等（快路径变死代码）。 */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int ap_notobj = emit_jcc(cb, 0x85);              /* JNE → slow */
                /* 2) obj->type == OBJ_ARRAY（type 是 Object 首字段，偏移 0） */
                emit_and_rr(cb, JIT_RDX, JIT_R10);               /* RDX = Object* */
                emit_cmp_mem8_imm8(cb, JIT_RDX, 0, (uint8_t)OBJ_ARRAY);
                int ap_notarr = emit_jcc(cb, 0x85);              /* JNE → slow */

                /* 3) 写屏障判定（必须在变异前）：value 是对象 且 arr 已是老年代
                 *    且 value 是年轻代 → 需要写 remembered set，交 callout。
                 *    热路径上通常第一条就不成立（arr.add(1) 这类非对象值）。 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int ap_val_notobj = emit_jcc(cb, 0x85);          /* JNE → 无需屏障 */
                emit_cmp_mem8_imm8(cb, JIT_RDX, (int8_t)offsetof(Object, generation), GEN_OLD);
                int ap_arr_notold = emit_jcc(cb, 0x85);          /* JNE → 无需屏障 */
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_and_rr(cb, JIT_R8, JIT_R10);                /* R8 = value_obj */
                emit_cmp_mem8_imm8(cb, JIT_R8, (int8_t)offsetof(Object, generation), GEN_YOUNG);
                int ap_barrier = emit_jcc(cb, 0x84);             /* JE → 需要屏障 → slow */
                patch_rel32(cb, ap_val_notobj, cb->len);
                patch_rel32(cb, ap_arr_notold, cb->len);

                /* 4) count < capacity（32 位比较；两者都是非负 int） */
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, count));
                emit_cmp_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, capacity));
                int ap_full = emit_jcc(cb, 0x83);                /* JAE → slow（容量满）*/

                /* 5) &elements[count] */
                emit_shl_imm(cb, JIT_R8, 3);                     /* count*8 */
                emit_mov_reg_mem8(cb, JIT_RAX, JIT_RDX, (int8_t)offsetof(ObjArray, elements));
                emit_add_rr(cb, JIT_RAX, JIT_R8);                /* RAX = &elements[count] */
                emit_push_reg(cb, JIT_RAX);                      /* 暂存地址（下面 pop）*/
                /* 6) value raw → Value（RAW_TO_VALUE 只作用于 RAX）*/
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                emit_pop_reg(cb, JIT_R8);                        /* R8 = 地址 */
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);       /* *addr = value */
                /* 7) count++ */
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, count));
                emit_inc_reg(cb, JIT_R8);
                emit_mov_mem32_reg32(cb, JIT_RDX, (int8_t)offsetof(ObjArray, count), JIT_R8);
                int ap_done = emit_jmp(cb);

                /* ---- 慢路径：原 callout（含类型检查 / 扩容 / 写屏障 / 报错）---- */
                patch_rel32(cb, ap_notobj, cb->len);
                patch_rel32(cb, ap_notarr, cb->len);
                patch_rel32(cb, ap_barrier, cb->len);
                patch_rel32(cb, ap_full, cb->len);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);              /* tmp1 ← value(Value) */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);               /* RDX = array(Value) */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);                /* R8 = value(Value) */
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);
                emit_mov_rr(cb, JIT_ARG2, JIT_R8);
                EMIT_CALL(jit_callout_array_append);
                EMIT_CALLOUT_END();
                patch_rel32(cb, ap_done, cb->len);
                vstack -= 2;
                break;
            }
            /* ---- Callout: OP_DICT_SET (dict[key]=val, returns dict) ---- */
            case OP_DICT_SET: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp3_disp);
                EMIT_LOAD_TMP(JIT_ARG2, tmp2_disp);
                EMIT_LOAD_TMP(JIT_ARG3, tmp1_disp);
                EMIT_CALL(jit_callout_dict_set);
                EMIT_CALLOUT_END();
                EMIT_VALUE_TO_RAW();
                TOS_PRODUCE();
                vstack -= 2;
                break;
            }
            /* ---- OP_INDEX_SET_NOPUSH（arr[idx]=val 语句形式）----
             * 内联快路径：array 是数组、idx 是 int48、0 <= idx < capacity、
             * 且不需要写屏障 → elements[idx] = value; if (idx >= count) count = idx+1。
             * 语义完全对齐 jit_callout_index_set 的数组分支：**越界判据是 capacity
             * 而不是 count**，idx >= count 时把 count 抬到 idx+1。
             * 慢路径：非数组 / 浮点或越界索引 / 需要写屏障 → 原 callout
             * （dict / struct / cstruct 分支与全部报错文本都保留）。
             * 同样所有判定都在变异之前，慢路径不会看到"写了一半"的状态。 */
            case OP_INDEX_SET_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* value (TOS) */
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* index */
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RDX);   /* array */
                EMIT_STORE_TMP(tmp3_disp, JIT_RDX);

                /* 1) array 是 NaN-boxed 对象 且 type == OBJ_ARRAY（top16 用 SHR 读） */
                emit_mov_rr(cb, JIT_R8, JIT_RDX);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int is_notobj = emit_jcc(cb, 0x85);              /* JNE → slow */
                emit_and_rr(cb, JIT_RDX, JIT_R10);               /* RDX = Object* */
                emit_cmp_mem8_imm8(cb, JIT_RDX, 0, (uint8_t)OBJ_ARRAY);
                int is_notarr = emit_jcc(cb, 0x85);              /* JNE → slow */

                /* 2) index 是 int48（放 RAX 备用） */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                emit_mov_rr(cb, JIT_R8, JIT_RAX);
                emit_sar_imm(cb, JIT_R8, 47);
                emit_inc_reg(cb, JIT_R8);
                emit_cmp_reg_imm8(cb, JIT_R8, 1);
                int is_idx_notint = emit_jcc(cb, 0x87);          /* JA → slow */

                /* 3) 写屏障判定（变异前） */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                emit_shr_imm(cb, JIT_R8, 48);
                emit_cmp_reg32_imm32(cb, JIT_R8, 0x0000FFFcu);
                int is_val_notobj = emit_jcc(cb, 0x85);          /* JNE → 无需屏障 */
                emit_cmp_mem8_imm8(cb, JIT_RDX, (int8_t)offsetof(Object, generation), GEN_OLD);
                int is_arr_notold = emit_jcc(cb, 0x85);          /* JNE → 无需屏障 */
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                emit_and_rr(cb, JIT_R8, JIT_R10);                /* R8 = value_obj */
                emit_cmp_mem8_imm8(cb, JIT_R8, (int8_t)offsetof(Object, generation), GEN_YOUNG);
                int is_barrier = emit_jcc(cb, 0x84);             /* JE → 需要屏障 → slow */
                patch_rel32(cb, is_val_notobj, cb->len);
                patch_rel32(cb, is_arr_notold, cb->len);

                /* 4) 0 <= idx < capacity（无符号比较：负数/超界 → slow → 解释器报错） */
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, capacity));
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int is_oob = emit_jcc(cb, 0x83);                 /* JAE → slow */

                /* 5) &elements[idx]（压栈暂存，RAW_TO_VALUE 只能作用于 RAX） */
                emit_shl_imm(cb, JIT_RAX, 3);
                emit_mov_reg_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, elements));
                emit_add_rr(cb, JIT_RAX, JIT_R8);
                emit_push_reg(cb, JIT_RAX);
                /* 6) value raw → Value，写入槽位 */
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                emit_pop_reg(cb, JIT_R8);
                emit_mov_mem8_reg(cb, JIT_R8, 0, JIT_RAX);
                /* 7) if (idx >= count) count = idx + 1 */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);               /* idx（int48 raw 即整数值）*/
                emit_mov_reg32_mem8(cb, JIT_R8, JIT_RDX, (int8_t)offsetof(ObjArray, count));
                emit_cmp_rr(cb, JIT_RAX, JIT_R8);
                int is_within = emit_jcc(cb, 0x82);              /* JB (idx<count) → 跳过 */
                emit_inc_reg(cb, JIT_RAX);
                emit_mov_mem32_reg32(cb, JIT_RDX, (int8_t)offsetof(ObjArray, count), JIT_RAX);
                patch_rel32(cb, is_within, cb->len);
                int is_done = emit_jmp(cb);

                /* ---- 慢路径：原 callout ---- */
                patch_rel32(cb, is_notobj, cb->len);
                patch_rel32(cb, is_notarr, cb->len);
                patch_rel32(cb, is_idx_notint, cb->len);
                patch_rel32(cb, is_barrier, cb->len);
                patch_rel32(cb, is_oob, cb->len);
                EMIT_LOAD_TMP(JIT_RAX, tmp1_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);              /* value(Value) */
                EMIT_LOAD_TMP(JIT_RAX, tmp2_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);              /* index(Value) */
                EMIT_LOAD_TMP(JIT_RAX, tmp3_disp);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);              /* array(Value) */
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp3_disp);  /* array (1st arg) */
                EMIT_LOAD_TMP(JIT_ARG2, tmp2_disp);  /* index (2nd arg) */
                EMIT_LOAD_TMP(JIT_ARG3, tmp1_disp);  /* value (3rd arg) */
                EMIT_CALL(jit_callout_index_set);
                EMIT_CALLOUT_END();
                patch_rel32(cb, is_done, cb->len);
                vstack -= 3;
                break;
            }
