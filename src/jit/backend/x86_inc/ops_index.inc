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
                /* r8 = (uint64)(int32)count; cmp rax, r8; jae → slow
                 * (negative idx becomes huge unsigned ⇒ jae taken) */
                emit_mov_reg_mem32(cb, JIT_R8, JIT_RDX, 40);
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
                patch_rel32(cb, p_idx_oob, cb->len);
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
                /* done: result in RAX = live TOS */
                patch_rel32(cb, done_loc, cb->len);
                TOS_PRODUCE();
                vstack--;
                break;
            }
            /* ---- Callout: OP_ARRAY_APPEND_NOPUSH ---- */
            case OP_ARRAY_APPEND_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);
                EMIT_RAW_TO_VALUE();
                emit_mov_rr(cb, JIT_RDX, JIT_RAX);
                EMIT_LOAD_TMP(JIT_R8, tmp1_disp);
                EMIT_CALLOUT_BEGIN();
                emit_mov_rr(cb, JIT_ARG1, JIT_RDX);
                emit_mov_rr(cb, JIT_ARG2, JIT_R8);
                EMIT_CALL(jit_callout_array_append);
                EMIT_CALLOUT_END();
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
            /* ---- Callout: OP_INDEX_SET_NOPUSH (arr[idx]=val, no push) ---- */
            case OP_INDEX_SET_NOPUSH: {
                TOS_SPILL();
                emit_pop_reg(cb, JIT_RAX);   /* value (top) */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp1_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* index */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp2_disp, JIT_RAX);
                emit_pop_reg(cb, JIT_RAX);   /* array */
                EMIT_RAW_TO_VALUE();
                EMIT_STORE_TMP(tmp3_disp, JIT_RAX);
                EMIT_CALLOUT_BEGIN();
                EMIT_LOAD_TMP(JIT_ARG1, tmp3_disp);  /* array (1st arg) */
                EMIT_LOAD_TMP(JIT_ARG2, tmp2_disp);  /* index (2nd arg) */
                EMIT_LOAD_TMP(JIT_ARG3, tmp1_disp);   /* value (3rd arg) */
                EMIT_CALL(jit_callout_index_set);
                EMIT_CALLOUT_END();
                vstack -= 3;
                break;
            }
