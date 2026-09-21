// ============================================================================
// 寄存器式字节码发射层
// 所有指令定长 4 字节，使用 reg_encode_* 系列函数写入 chunk
// chunk->lines 仍按字节索引：一条指令的 4 个字节写同一 line
// ============================================================================

#include "codegen.h"

// --- 基础：写 4 字节指令的封装 ---

void emit_reg_iABC(CodeGen* gen, OpCode op, int a, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, op, a, b, c, line);
}

void emit_reg_iABx(CodeGen* gen, OpCode op, int a, int bx, int line) {
    reg_encode_iABx(gen->chunk, op, a, bx, line);
}

void emit_reg_iAsBx(CodeGen* gen, OpCode op, int a, int sbx, int line) {
    reg_encode_iAsBx(gen->chunk, op, a, sbx, line);
}

// --- 装载指令 ---

int emit_loadk(CodeGen* gen, int const_idx, int line) {
    // 返回分配的寄存器号（由调用方管理）
    // LOADK iABx: R[A] = K[Bx]
    // 这里只写指令，A 由调用方传入
    // 实际调用方用 emit_loadk_to(gen, dst, const_idx, line)
    return const_idx;  // 占位，实际用 emit_loadk_to
}

void emit_loadk_to(CodeGen* gen, int dst, int const_idx, int line) {
    reg_encode_iABx(gen->chunk, OP_LOADK, dst, const_idx, line);
}

void emit_loadi_to(CodeGen* gen, int dst, int imm, int line) {
    reg_encode_iAsBx(gen->chunk, OP_LOADI, dst, imm, line);
}

void emit_loadf_to(CodeGen* gen, int dst, double val, int line) {
    // 浮点立即数：存入常量表，用 LOADK
    int idx = chunk_add_const(gen->chunk, val_float(val));
    reg_encode_iABx(gen->chunk, OP_LOADK, dst, idx, line);
}

void emit_loadnil_to(CodeGen* gen, int dst, int line) {
    reg_encode_iABC(gen->chunk, OP_LOADNIL, dst, 0, 0, line);
}

void emit_loadtrue_to(CodeGen* gen, int dst, int line) {
    reg_encode_iABC(gen->chunk, OP_LOADTRUE, dst, 0, 0, line);
}

void emit_loadfalse_to(CodeGen* gen, int dst, int line) {
    reg_encode_iABC(gen->chunk, OP_LOADFALSE, dst, 0, 0, line);
}

void emit_mov(CodeGen* gen, int dst, int src, int line) {
    reg_encode_iABC(gen->chunk, OP_MOV, dst, src, 0, line);
}

// --- 全局变量 ---

void emit_getglobal_to(CodeGen* gen, int dst, int slot, int line) {
    reg_encode_iABx(gen->chunk, OP_GETGLOBAL, dst, slot, line);
}

void emit_setglobal(CodeGen* gen, int src, int slot, int line) {
    reg_encode_iABx(gen->chunk, OP_SETGLOBAL, src, slot, line);
}

void emit_defglobal(CodeGen* gen, int src, int slot, int line) {
    reg_encode_iABx(gen->chunk, OP_DEFGLOBAL, src, slot, line);
}

// 全局函数直呼：R[base] = 全局函数[slot](R[base+1..base+nargs])
//   slot / nargs 都必须在 8 位内（调用方保证；否则不发这条指令）
void emit_call_global(CodeGen* gen, int base, int nargs, int slot, int line) {
    reg_encode_iABC(gen->chunk, OP_CALL_GLOBAL_FUNC, base, nargs, slot, line);
}
void emit_getglobalfunc_to(CodeGen* gen, int dst, int slot, int line) {
    reg_encode_iABx(gen->chunk, OP_GETGLOBALFUNC, dst, slot, line);
}

void emit_defglobalfunc(CodeGen* gen, int src, int slot, int line) {
    reg_encode_iABx(gen->chunk, OP_DEFGLOBALFUNC, src, slot, line);
}

// --- upvalue / 闭包 ---

void emit_getupval_to(CodeGen* gen, int dst, int idx, int line) {
    reg_encode_iABx(gen->chunk, OP_GETUPVAL, dst, idx, line);
}

void emit_setupval(CodeGen* gen, int src, int idx, int line) {
    reg_encode_iABx(gen->chunk, OP_SETUPVAL, src, idx, line);
}

void emit_close(CodeGen* gen, int a, int count, int line) {
    reg_encode_iABx(gen->chunk, OP_CLOSE, a, count, line);
}

void emit_closure_to(CodeGen* gen, int dst, int func_const_idx, int line) {
    reg_encode_iABx(gen->chunk, OP_CLOSURE, dst, func_const_idx, line);
}

// --- 算术运算 ---

void emit_add(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_ADD, dst, b, c, line);
}
void emit_sub(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_SUB, dst, b, c, line);
}
void emit_mul(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_MUL, dst, b, c, line);
}
void emit_div(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_DIV, dst, b, c, line);
}
void emit_mod(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_MOD, dst, b, c, line);
}
void emit_neg(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_NEG, dst, b, 0, line);
}
void emit_not(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_NOT, dst, b, 0, line);
}

// int 特化
void emit_add_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_ADD_INT, dst, b, c, line);
}
void emit_sub_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_SUB_INT, dst, b, c, line);
}
void emit_mul_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_MUL_INT, dst, b, c, line);
}
void emit_neg_int(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_NEG_INT, dst, b, 0, line);
}
// 立即数版加减（imm ∈ [-128,127]，编进 C 字段的 8 位有符号）：
//   R[dst] = R[b] + imm / R[b] - imm —— 省掉「求右值 → LOADI → *_INT」两条指令
void emit_add_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_ADD_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
void emit_sub_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_SUB_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
// 立即数乘法（T10-③）：R[dst] = R[b] * imm —— 省掉「求右值 → LOADI → MUL_INT」里的 LOADI
void emit_mul_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_MUL_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
// 立即数比较：R[dst] = R[b] </>/<=/>= imm（imm ∈ [-128,127]）
void emit_lt_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_LT_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
void emit_gt_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_GT_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
void emit_le_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_LE_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}
void emit_ge_int_imm(CodeGen* gen, int dst, int b, int imm, int line) {
    reg_encode_iABC(gen->chunk, OP_GE_INT_IMM, dst, b, (int)((uint8_t)(int8_t)imm), line);
}

// float 特化
void emit_add_f(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_ADD_F, dst, b, c, line);
}
void emit_sub_f(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_SUB_F, dst, b, c, line);
}
void emit_mul_f(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_MUL_F, dst, b, c, line);
}
void emit_div_f(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_DIV_F, dst, b, c, line);
}
void emit_neg_f(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_NEG_F, dst, b, 0, line);
}

// --- 比较运算 ---
void emit_eq(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_EQ, dst, b, c, line);
}
void emit_lt(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_LT, dst, b, c, line);
}
void emit_gt(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_GT, dst, b, c, line);
}
void emit_le(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_LE, dst, b, c, line);
}
void emit_ge(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_GE, dst, b, c, line);
}
void emit_neq(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_NEQ, dst, b, c, line);
}

// int 比较
void emit_lt_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_LT_INT, dst, b, c, line);
}
void emit_gt_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_GT_INT, dst, b, c, line);
}
void emit_le_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_LE_INT, dst, b, c, line);
}
void emit_ge_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_GE_INT, dst, b, c, line);
}
void emit_eq_int(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_EQ_INT, dst, b, c, line);
}

// --- 位运算 ---
void emit_bitand(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_BITAND, dst, b, c, line);
}
void emit_bitor(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_BITOR, dst, b, c, line);
}
void emit_bitxor(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_BITXOR, dst, b, c, line);
}
void emit_bitnot(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_BITNOT, dst, b, 0, line);
}
void emit_shl(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_SHL, dst, b, c, line);
}
void emit_shr(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_SHR, dst, b, c, line);
}
void emit_ushr(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_USHR, dst, b, c, line);
}

// --- 类型转换 ---
void emit_cast_float(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_CAST_FLOAT, dst, b, 0, line);
}
void emit_cast_int(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_CAST_INT, dst, b, 0, line);
}
void emit_cast_string(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_CAST_STRING, dst, b, 0, line);
}
void emit_is_null(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_IS_NULL, dst, b, 0, line);
}

// --- 字符串拼接 ---
void emit_strcat(CodeGen* gen, int dst, int b, int c, int line) {
    reg_encode_iABC(gen->chunk, OP_STRCAT, dst, b, c, line);
}

// --- 跳转 ---
// JMP (iAsJ): 返回跳转偏移写入位置（字节索引），用于后续 patch
int emit_jmp(CodeGen* gen, int line) {
    int pos = gen->chunk->len;
    reg_encode_iAsJ(gen->chunk, OP_JMP, 0, line);  // 占位 sJ=0
    return pos;
}

// JMP_IF_FALSE (iAsBx): 如果 !R[A] 则跳转 sBx 字节
int emit_jmp_if_false(CodeGen* gen, int a, int line) {
    int pos = gen->chunk->len;
    reg_encode_iAsBx(gen->chunk, OP_JMP_IF_FALSE, a, 0, line);  // 占位
    return pos;
}

// JMP_IF_TRUE (iAsBx)
int emit_jmp_if_true(CodeGen* gen, int a, int line) {
    int pos = gen->chunk->len;
    reg_encode_iAsBx(gen->chunk, OP_JMP_IF_TRUE, a, 0, line);
    return pos;
}

// 「比较 + 条件跳转」融合（T10-①）：返回跳转偏移的写入位置（用 patch_jmp 回填）。
//   语义 = 「比较为假则跳」，与 `emit_*_int[_imm] + emit_jmp_if_false` 逐条等价。
//   is_imm=1 时 b 是 int8 立即数，否则 b 是右操作数寄存器号。
//   ⚠ 这条指令占 **8 字节**（第二个 4 字节字携带偏移）；patch_jmp / patch_jmp_to
//     会按 opcode 自动选对尺寸（见 instr_bytes_at）。
int emit_cmpjmp(CodeGen* gen, OpCode op, int a, int b, int is_imm, int line) {
    int pos = gen->chunk->len;
    reg_encode_iABC(gen->chunk, op, a, b, is_imm ? 0x80 : 0, line);
    chunk_write(gen->chunk, 0, line);   // 第二个字：16 位偏移占位
    chunk_write(gen->chunk, 0, line);
    chunk_write(gen->chunk, 0, line);
    chunk_write(gen->chunk, 0, line);
    return pos;
}

// patch 跳转偏移：从 pos 位置的指令开始，计算跳转到当前 chunk->len
// 按指令自身的编码形式写回：
//   OP_JMP 是 iAsJ （24 位有符号偏移，占 byte1..3）
//   OP_JMP_IF_FALSE / OP_JMP_IF_TRUE 是 iAsBx（16 位无符号 Bx，byte1 是寄存器 A）
// 这里自动识别，调用方无需区分 —— 混用会写出完全错误的跳距。
// 指令占几个字节：`OP_CMPJMP_*`（比较+跳转融合）带一个 4 字节的偏移字 ⇒ 8 字节，其余 4 字节。
// 跳转偏移的算法必须按**指令自身长度**扣（`len - pos - size`），用错尺寸会写出错误的跳距。
static int instr_bytes_at(Chunk* chunk, int pos) {
    uint8_t op = chunk->code[pos];
    return (op >= (uint8_t)OP_CMPJMP_LT && op <= (uint8_t)OP_CMPJMP_GE) ? 8 : 4;
}

static void patch_common(CodeGen* gen, int pos, int offset) {
    uint8_t op = gen->chunk->code[pos];
    if (op == (uint8_t)OP_JMP) {
        gen->chunk->code[pos + 1] = (uint8_t)(((uint32_t)offset >> 16) & 0xFF);
        gen->chunk->code[pos + 2] = (uint8_t)(((uint32_t)offset >> 8) & 0xFF);
        gen->chunk->code[pos + 3] = (uint8_t)((uint32_t)offset & 0xFF);
    } else if (op >= (uint8_t)OP_CMPJMP_LT && op <= (uint8_t)OP_CMPJMP_GE) {
        // 偏移在**第二个字**里（byte0/1），与 iAsBx 同约定（offset + 32768）
        int bx = offset + 32768;
        if (bx < 0) bx = 0;
        if (bx > 0xFFFF) bx = 0xFFFF;
        gen->chunk->code[pos + 4] = (uint8_t)((bx >> 8) & 0xFF);
        gen->chunk->code[pos + 5] = (uint8_t)(bx & 0xFF);
    } else {
        int bx = offset + 32768;
        if (bx < 0) bx = 0;
        if (bx > 0xFFFF) bx = 0xFFFF;
        gen->chunk->code[pos + 2] = (uint8_t)((bx >> 8) & 0xFF);
        gen->chunk->code[pos + 3] = (uint8_t)(bx & 0xFF);
    }
}

void patch_jmp(CodeGen* gen, int pos) {
    patch_common(gen, pos, gen->chunk->len - pos - instr_bytes_at(gen->chunk, pos));
}

// patch 跳转到指定目标
void patch_jmp_to(CodeGen* gen, int pos, int target) {
    patch_common(gen, pos, target - pos - instr_bytes_at(gen->chunk, pos));
}

// 写入 2 字节 sBx（OP_FOR_PREP / OP_FOR_LOOP 紧随指令的偏移数据，编码为 sbx+32768）
void patch_sbx_at(CodeGen* gen, int pos, int sbx) {
    int raw = sbx + 32768;
    if (raw < 0) raw = 0;
    if (raw > 0xFFFF) raw = 0xFFFF;
    gen->chunk->code[pos] = (uint8_t)((raw >> 8) & 0xFF);
    gen->chunk->code[pos + 1] = (uint8_t)(raw & 0xFF);
}

// 回跳（循环）：从 pos 回跳到 target
void emit_loop(CodeGen* gen, int target, int line) {
    int pos = gen->chunk->len;
    int offset = target - pos - 4;  // 负数 = 向后跳
    reg_encode_iAsJ(gen->chunk, OP_JMP, offset, line);
}

// --- 自增自减 ---
void emit_inc(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_INC, dst, b, 0, line);
}
void emit_dec(CodeGen* gen, int dst, int b, int line) {
    reg_encode_iABC(gen->chunk, OP_DEC, dst, b, 0, line);
}

// --- 调用 / 返回 ---
// CALL: R[A] = callee, R[A+1..A+B-1] = args, B = nargs+1, C = nresults+1
void emit_call(CodeGen* gen, int a, int nargs, int nresults, int line) {
    reg_encode_iABC(gen->chunk, OP_CALL, a, nargs + 1, nresults + 1, line);
}

// CALL_NATIVE: R[A] = result, B = name_const_idx（0 或 >255 时改由**紧随**的 EXTRAARG
// 携带 24 位索引；EXTRAARG 必须在指令**之后** —— VM 执行到 call 时 ip 正指向它），C = nargs
// 实参在 R[A+1..A+C]
void emit_call_native(CodeGen* gen, int dst, int name_const_idx, int nargs, int line) {
    if (name_const_idx == 0 || name_const_idx > 255) {
        reg_encode_iABC(gen->chunk, OP_CALL_NATIVE, dst, 0, nargs, line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, name_const_idx, line);
    } else {
        reg_encode_iABC(gen->chunk, OP_CALL_NATIVE, dst, name_const_idx, nargs, line);
    }
}

// RETURN: 返回 R[A..A+B-2], B = nresults+1
void emit_return(CodeGen* gen, int a, int nresults, int line) {
    reg_encode_iABC(gen->chunk, OP_RETURN, a, nresults + 1, 0, line);
}

// RETURN_MULTI: B = 存放个数的寄存器
void emit_return_multi(CodeGen* gen, int a, int count_reg, int line) {
    reg_encode_iABC(gen->chunk, OP_RETURN_MULTI, a, count_reg, 0, line);
}

// TAIL_CALL: R[A] = callee, B = nargs+1
void emit_tail_call(CodeGen* gen, int a, int nargs, int line) {
    reg_encode_iABC(gen->chunk, OP_TAIL_CALL, a, nargs + 1, 0, line);
}

// --- 常量表辅助 ---
int make_constant(CodeGen* gen, Value value) {
    return chunk_add_const(gen->chunk, value);
}

// --- 旧 API 兼容存根（后续 codegen 重写时删除） ---
// 这些在 codegen_expr/stmt 重写过程中逐步替换
void emit_byte(CodeGen* gen, uint8_t byte, int line) {
    (void)gen; (void)byte; (void)line;
    // 寄存器式不使用单字节 emit
}

void emit_bytes(CodeGen* gen, uint8_t b1, uint8_t b2, int line) {
    (void)gen; (void)b1; (void)b2; (void)line;
}

void emit_bytes_2(CodeGen* gen, uint8_t op, int operand, int line) {
    (void)gen; (void)op; (void)operand; (void)line;
}

void emit_byte_imm(CodeGen* gen, uint8_t op, int8_t imm, int line) {
    (void)gen; (void)op; (void)imm; (void)line;
}

int emit_jump(CodeGen* gen, uint8_t instruction, int line) {
    (void)instruction;
    return emit_jmp(gen, line);
}

void patch_jump(CodeGen* gen, int offset) {
    patch_jmp(gen, offset);
}

void patch_jump_to(CodeGen* gen, int offset, int target) {
    patch_jmp_to(gen, offset, target);
}

void emit_constant(CodeGen* gen, Value value, int line) {
    (void)value; (void)line;
    // 寄存器式由调用方分配寄存器后调 emit_loadk_to
}

void emit_native(CodeGen* gen, int constant, int line) {
    (void)gen; (void)constant; (void)line;
}

void emit_closure(CodeGen* gen, int constant, int line) {
    (void)gen; (void)constant; (void)line;
}

void emit_get_global(CodeGen* gen, int index, int line) {
    (void)gen; (void)index; (void)line;
}

void emit_set_global(CodeGen* gen, int index, int line) {
    (void)gen; (void)index; (void)line;
}

void emit_define_global(CodeGen* gen, int index, int line) {
    (void)gen; (void)index; (void)line;
}

void emit_get_global_func(CodeGen* gen, int index, int line) {
    (void)gen; (void)index; (void)line;
}

void emit_define_global_func(CodeGen* gen, int index, int line) {
    (void)gen; (void)index; (void)line;
}

void emit_call_old_stub_unused(CodeGen* gen, int arg_count, int line) {
    (void)gen; (void)arg_count; (void)line;
}

void emit_tail_call_old_stub_unused(CodeGen* gen, int arg_count, int line) {
    (void)gen; (void)arg_count; (void)line;
}

void emit_tail_call_native(CodeGen* gen, int name_const, int arg_count, int line) {
    (void)gen; (void)name_const; (void)arg_count; (void)line;
}

void emit_set_local_const(CodeGen* gen, int const_idx, int slot, int line) {
    (void)gen; (void)const_idx; (void)slot; (void)line;
}

int emit_cmpjmp_ll_int(CodeGen* gen, int cmp_op, int sa, int sb, int line) {
    (void)gen; (void)cmp_op; (void)sa; (void)sb; (void)line;
    return 0;
}

int emit_cmpjmp_li_int(CodeGen* gen, int cmp_op, int slot, int imm, int line) {
    (void)gen; (void)cmp_op; (void)slot; (void)imm; (void)line;
    return 0;
}

int emit_cmpjmp_lg_int(CodeGen* gen, int cmp_op, int slot, int gi, int line) {
    (void)gen; (void)cmp_op; (void)slot; (void)gi; (void)line;
    return 0;
}
