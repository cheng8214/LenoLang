#include "codegen.h"

void emit_byte(CodeGen* gen, uint8_t byte, int line) {
    chunk_write(gen->chunk, byte, line);
}

void emit_bytes(CodeGen* gen, uint8_t byte1, uint8_t byte2, int line) {
    emit_byte(gen, byte1, line);
    emit_byte(gen, byte2, line);
}

void emit_bytes_2(CodeGen* gen, uint8_t opcode, int operand, int line) {
    emit_byte(gen, opcode, line);
    emit_byte(gen, (operand >> 8) & 0xff, line);
    emit_byte(gen, operand & 0xff, line);
}

void emit_byte_imm(CodeGen* gen, uint8_t opcode, int8_t imm, int line) {
    emit_byte(gen, opcode, line);
    emit_byte(gen, (uint8_t)imm, line);
}

int emit_jump(CodeGen* gen, uint8_t instruction, int line) {
    emit_byte(gen, instruction, line);
    // 4字节偏移量（大端序）
    emit_byte(gen, 0xff, line);
    emit_byte(gen, 0xff, line);
    emit_byte(gen, 0xff, line);
    emit_byte(gen, 0xff, line);
    return gen->chunk->len - 4;
}

void patch_jump(CodeGen* gen, int offset) {
    int jump = gen->chunk->len - offset - 4;
    // 使用 int32_t 范围检查，避免编译器警告
    if ((int32_t)jump > 2147483647 || (int32_t)jump < -2147483647 - 1) {
        error_add(ERR_RUNTIME, 0, "跳转距离过长");
        return;
    }
    // 4字节偏移量（大端序）
    gen->chunk->code[offset] = (jump >> 24) & 0xff;
    gen->chunk->code[offset + 1] = (jump >> 16) & 0xff;
    gen->chunk->code[offset + 2] = (jump >> 8) & 0xff;
    gen->chunk->code[offset + 3] = jump & 0xff;
}

void patch_jump_to(CodeGen* gen, int offset, int target) {
    int jump = target - offset - 4;
    // 使用 int32_t 范围检查，避免编译器警告
    if ((int32_t)jump > 2147483647 || (int32_t)jump < -2147483647 - 1) {
        error_add(ERR_RUNTIME, 0, "跳转距离过长");
        return;
    }
    // 4字节偏移量（大端序）
    gen->chunk->code[offset] = (jump >> 24) & 0xff;
    gen->chunk->code[offset + 1] = (jump >> 16) & 0xff;
    gen->chunk->code[offset + 2] = (jump >> 8) & 0xff;
    gen->chunk->code[offset + 3] = jump & 0xff;
}

void emit_loop(CodeGen* gen, int loop_start, int line) {
    emit_byte(gen, OP_LOOP, line);
    int offset = gen->chunk->len - loop_start + 4;
    if (offset > 2147483647) {
        error_add(ERR_RUNTIME, line, "循环体过大");
        return;
    }
    // 4字节偏移量（大端序）
    emit_byte(gen, (offset >> 24) & 0xff, line);
    emit_byte(gen, (offset >> 16) & 0xff, line);
    emit_byte(gen, (offset >> 8) & 0xff, line);
    emit_byte(gen, offset & 0xff, line);
}

int make_constant(CodeGen* gen, Value value) {
    int constant = chunk_add_const(gen->chunk, value);
    return constant;
}

void emit_constant(CodeGen* gen, Value value, int line) {
    int constant = make_constant(gen, value);
    emit_byte(gen, OP_CONST, line);
    emit_byte(gen, (constant >> 8) & 0xff, line);
    emit_byte(gen, constant & 0xff, line);
}

void emit_native(CodeGen* gen, int constant, int line) {
    emit_byte(gen, OP_GET_NATIVE, line);
    emit_byte(gen, (constant >> 8) & 0xff, line);
    emit_byte(gen, constant & 0xff, line);
}

void emit_closure(CodeGen* gen, int constant, int line) {
    emit_byte(gen, OP_CLOSURE, line);
    emit_byte(gen, (constant >> 8) & 0xff, line);
    emit_byte(gen, constant & 0xff, line);
}

void emit_get_global(CodeGen* gen, int index, int line) {
    emit_byte(gen, OP_GET_GLOBAL, line);
    emit_byte(gen, (index >> 8) & 0xff, line);
    emit_byte(gen, index & 0xff, line);
}

void emit_set_global(CodeGen* gen, int index, int line) {
    emit_byte(gen, OP_SET_GLOBAL, line);
    emit_byte(gen, (index >> 8) & 0xff, line);
    emit_byte(gen, index & 0xff, line);
}

void emit_define_global(CodeGen* gen, int index, int line) {
    emit_byte(gen, OP_DEFINE_GLOBAL, line);
    emit_byte(gen, (index >> 8) & 0xff, line);
    emit_byte(gen, index & 0xff, line);
}

void emit_get_global_func(CodeGen* gen, int index, int line) {
    emit_byte(gen, OP_GET_GLOBAL_FUNC, line);
    emit_byte(gen, (index >> 8) & 0xff, line);
    emit_byte(gen, index & 0xff, line);
}

void emit_define_global_func(CodeGen* gen, int index, int line) {
    emit_byte(gen, OP_DEFINE_GLOBAL_FUNC, line);
    emit_byte(gen, (index >> 8) & 0xff, line);
    emit_byte(gen, index & 0xff, line);
}

void emit_call(CodeGen* gen, int arg_count, int line) {
    emit_byte(gen, OP_CALL, line);
    emit_byte(gen, (arg_count >> 8) & 0xff, line);
    emit_byte(gen, arg_count & 0xff, line);
}

void emit_tail_call(CodeGen* gen, int arg_count, int line) {
    emit_byte(gen, OP_TAIL_CALL, line);
    emit_byte(gen, (arg_count >> 8) & 0xff, line);
    emit_byte(gen, arg_count & 0xff, line);
}

void emit_call_native(CodeGen* gen, int name_const, int arg_count, int line) {
    emit_byte(gen, OP_CALL_NATIVE, line);
    emit_byte(gen, (name_const >> 8) & 0xff, line);
    emit_byte(gen, name_const & 0xff, line);
    emit_byte(gen, (arg_count >> 8) & 0xff, line);
    emit_byte(gen, arg_count & 0xff, line);
}

void emit_tail_call_native(CodeGen* gen, int name_const, int arg_count, int line) {
    emit_byte(gen, OP_TAIL_CALL_NATIVE, line);
    emit_byte(gen, (name_const >> 8) & 0xff, line);
    emit_byte(gen, name_const & 0xff, line);
    emit_byte(gen, (arg_count >> 8) & 0xff, line);
    emit_byte(gen, arg_count & 0xff, line);
}
