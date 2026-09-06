/*
 * jit_emit.h - x86_64 machine code emission helpers
 *
 * Provides a CodeBuf (dynamic byte buffer) and inline functions to emit
 * x86_64 instructions.  All functions append bytes to the CodeBuf.
 *
 * Register usage convention (Windows x64 calling convention):
 *   RCX  = arg1 (Value* locals)   [fixed, never overwritten]
 *   RAX  = accumulator
 *   RDX  = secondary operand / IDIV implicit
 *   R8   = scratch / secondary
 *   R9   = scratch
 *   R10  = constant register 1 (PAYLOAD_MASK)
 *   R11  = constant register 2 (INT_TAG)
 *   RBP  = frame pointer (scratch area base)
 *   RSP  = stack pointer (virtual stack via PUSH/POP)
 *
 * Condition codes (for Jcc / SETcc):
 *   0x4 = JE/JZ,  0x5 = JNE/JNZ
 *   0xC = JL,     0xD = JGE
 *   0xE = JLE,    0xF = JG
 *   0x2 = JB,     0x3 = JAE
 */
#ifndef LENO_JIT_EMIT_H
#define LENO_JIT_EMIT_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- Register IDs ---- */
#define JIT_RAX  0
#define JIT_RCX  1
#define JIT_RDX  2
#define JIT_RBX  3
#define JIT_RSP  4
#define JIT_RBP  5
#define JIT_RSI  6
#define JIT_RDI  7
#define JIT_R8   8
#define JIT_R9   9
#define JIT_R10  10
#define JIT_R11  11
#define JIT_R12  12
#define JIT_R13  13
#define JIT_R14  14

/* ---- CodeBuf ---- */
typedef struct {
    uint8_t* buf;
    int      len;
    int      cap;
} CodeBuf;

static inline void codebuf_init(CodeBuf* cb, int initial_cap) {
    cb->cap   = initial_cap > 0 ? initial_cap : 256;
    cb->buf   = (uint8_t*)malloc((size_t)cb->cap);
    cb->len   = 0;
}

static inline void codebuf_free(CodeBuf* cb) {
    free(cb->buf);
    cb->buf = NULL;
    cb->len = cb->cap = 0;
}

static inline void codebuf_ensure(CodeBuf* cb, int need) {
    if (cb->len + need > cb->cap) {
        while (cb->len + need > cb->cap) cb->cap *= 2;
        cb->buf = (uint8_t*)realloc(cb->buf, (size_t)cb->cap);
    }
}

static inline void emit_byte(CodeBuf* cb, uint8_t b) {
    codebuf_ensure(cb, 1);
    cb->buf[cb->len++] = b;
}

static inline void emit_uint32(CodeBuf* cb, uint32_t v) {
    codebuf_ensure(cb, 4);
    cb->buf[cb->len++] = (uint8_t)(v);
    cb->buf[cb->len++] = (uint8_t)(v >> 8);
    cb->buf[cb->len++] = (uint8_t)(v >> 16);
    cb->buf[cb->len++] = (uint8_t)(v >> 24);
}

static inline void emit_uint64(CodeBuf* cb, uint64_t v) {
    codebuf_ensure(cb, 8);
    for (int i = 0; i < 8; i++)
        cb->buf[cb->len++] = (uint8_t)(v >> (i * 8));
}

/* ---- Low-level encoding helpers ---- */

/* REX prefix: 0x40 | (W<<3) | (R<<2) | (X<<1) | B */
static inline uint8_t rex(int w, int r, int x, int b) {
    return (uint8_t)(0x40 | (w << 3) | (r << 2) | (x << 1) | b);
}

/* ModRM byte */
static inline uint8_t modrm(int mod, int reg, int rm) {
    return (uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

/*
 * Emit REX.W + opcode + ModRM(11, src, dst) for reg-reg operations.
 * The opcode determines direction:
 *   0x01 = ADD r/m64, r64   (dst in rm, src in reg)
 *   0x29 = SUB r/m64, r64
 *   0x89 = MOV r/m64, r64
 *   0x39 = CMP r/m64, r64
 *   0x85 = TEST r/m64, r64
 *   0x31 = XOR r/m64, r64
 *   0x21 = AND r/m64, r64
 *   0x09 = OR  r/m64, r64
 */
static inline void emit_rr(CodeBuf* cb, uint8_t opcode, int dst, int src) {
    int r = (src >> 3) & 1;   /* R extends reg field */
    int b = (dst >> 3) & 1;   /* B extends rm field  */
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, opcode);
    emit_byte(cb, modrm(3, src & 7, dst & 7));
}

/* ---- MOV variants ---- */

/* MOV reg, imm64 */
static inline void emit_mov_reg_imm64(CodeBuf* cb, int reg, uint64_t imm) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, (uint8_t)(0xB8 + (reg & 7)));
    emit_uint64(cb, imm);
}

/* MOV reg, [base + disp8]  (64-bit) */
static inline void emit_mov_reg_mem8(CodeBuf* cb, int reg, int base, int8_t disp) {
    int r = (reg >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x8B);
    emit_byte(cb, modrm(1, reg & 7, base & 7));
    /* SIB byte needed when base is RSP/R12 (rm field = 4 in memory mode) */
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);   /* SIB: scale=0, index=none(4), base=RSP(4) */
    emit_byte(cb, (uint8_t)disp);
}

/* MOV reg, [base + disp32] (64-bit) */
static inline void emit_mov_reg_mem32(CodeBuf* cb, int reg, int base, int32_t disp) {
    int r = (reg >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x8B);
    emit_byte(cb, modrm(2, reg & 7, base & 7));
    /* SIB byte needed when base is RSP/R12 (rm field = 4 in memory mode) */
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_uint32(cb, (uint32_t)disp);
}

/* MOV [base + disp8], reg (64-bit) */
static inline void emit_mov_mem8_reg(CodeBuf* cb, int base, int8_t disp, int reg) {
    int r = (reg >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x89);
    emit_byte(cb, modrm(1, reg & 7, base & 7));
    /* SIB byte needed when base is RSP/R12 (rm field = 4 in memory mode) */
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_byte(cb, (uint8_t)disp);
}

/* MOV [base + disp32], reg (64-bit) */
static inline void emit_mov_mem32_reg(CodeBuf* cb, int base, int32_t disp, int reg) {
    int r = (reg >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x89);
    emit_byte(cb, modrm(2, reg & 7, base & 7));
    /* SIB byte needed when base is RSP/R12 (rm field = 4 in memory mode) */
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_uint32(cb, (uint32_t)disp);
}

/* MOV reg, reg (64-bit)  → MOV dst, src */
static inline void emit_mov_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x89, dst, src);
}

/* ---- Arithmetic (reg, reg) ---- */

/* ADD dst, src */
static inline void emit_add_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x01, dst, src);
}

/* SUB dst, src */
static inline void emit_sub_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x29, dst, src);
}

/* IMUL dst, src (two-operand form) */
static inline void emit_imul_rr(CodeBuf* cb, int dst, int src) {
    int r = (dst >> 3) & 1;
    int b = (src >> 3) & 1;
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0xAF);
    emit_byte(cb, modrm(3, dst & 7, src & 7));
}

/* IDIV r/m64 — divides RDX:RAX by reg; quotient→RAX, remainder→RDX */
static inline void emit_idiv_reg(CodeBuf* cb, int reg) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0xF7);
    emit_byte(cb, modrm(3, 7, reg & 7));   /* /7 = IDIV */
}

/* CQO — sign-extend RAX into RDX:RAX */
static inline void emit_cqo(CodeBuf* cb) {
    emit_byte(cb, 0x48);
    emit_byte(cb, 0x99);
}

/* NEG reg */
static inline void emit_neg_reg(CodeBuf* cb, int reg) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0xF7);
    emit_byte(cb, modrm(3, 3, reg & 7));   /* /3 = NEG */
}

/* INC reg (64-bit) */
static inline void emit_inc_reg(CodeBuf* cb, int reg) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0xFF);
    emit_byte(cb, modrm(3, 0, reg & 7));   /* /0 = INC */
}

/* CMP reg, imm8 (sign-extended to 64-bit) */
static inline void emit_cmp_reg_imm8(CodeBuf* cb, int reg, int8_t imm) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0x83);
    emit_byte(cb, modrm(3, 7, reg & 7));  /* /7 = CMP */
    emit_byte(cb, (uint8_t)imm);
}

/* CMP r1, r2 */
static inline void emit_cmp_rr(CodeBuf* cb, int r1, int r2) {
    emit_rr(cb, 0x39, r1, r2);
}

/* TEST r1, r2 */
static inline void emit_test_rr(CodeBuf* cb, int r1, int r2) {
    emit_rr(cb, 0x85, r1, r2);
}

/* AND dst, src */
static inline void emit_and_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x21, dst, src);
}

/* OR dst, src */
static inline void emit_or_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x09, dst, src);
}

/* XOR dst, src (use same reg to zero) */
static inline void emit_xor_rr(CodeBuf* cb, int dst, int src) {
    emit_rr(cb, 0x31, dst, src);
}

/* SHL reg, imm8 */
static inline void emit_shl_imm(CodeBuf* cb, int reg, uint8_t count) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0xC1);
    emit_byte(cb, modrm(3, 4, reg & 7));    /* /4 = SHL */
    emit_byte(cb, count);
}

/* SAR reg, imm8 */
static inline void emit_sar_imm(CodeBuf* cb, int reg, uint8_t count) {
    int b = (reg >> 3) & 1;
    emit_byte(cb, rex(1, 0, 0, b));
    emit_byte(cb, 0xC1);
    emit_byte(cb, modrm(3, 7, reg & 7));    /* /7 = SAR */
    emit_byte(cb, count);
}

/* ---- Stack operations ---- */

/* PUSH reg64 */
static inline void emit_push_reg(CodeBuf* cb, int reg) {
    if (reg >= 8) emit_byte(cb, 0x41);      /* REX.B */
    emit_byte(cb, (uint8_t)(0x50 + (reg & 7)));
}

/* POP reg64 */
static inline void emit_pop_reg(CodeBuf* cb, int reg) {
    if (reg >= 8) emit_byte(cb, 0x41);
    emit_byte(cb, (uint8_t)(0x58 + (reg & 7)));
}

/* PUSH RBP */
static inline void emit_push_rbp(CodeBuf* cb) {
    emit_byte(cb, 0x55);
}

/* POP RBP */
static inline void emit_pop_rbp(CodeBuf* cb) {
    emit_byte(cb, 0x5D);
}

/* MOV RBP, RSP */
static inline void emit_mov_rbp_rsp(CodeBuf* cb) {
    emit_byte(cb, 0x48);
    emit_byte(cb, 0x89);
    emit_byte(cb, 0xE5);   /* ModRM(11, 4, 5) */
}

/* SUB RSP, imm8 */
static inline void emit_sub_rsp_imm8(CodeBuf* cb, uint8_t imm) {
    emit_byte(cb, 0x48);
    emit_byte(cb, 0x83);
    emit_byte(cb, 0xEC);   /* ModRM(11, 5, 4) */
    emit_byte(cb, imm);
}

/* SUB RSP, imm32 */
static inline void emit_sub_rsp_imm32(CodeBuf* cb, int32_t imm) {
    emit_byte(cb, 0x48);
    emit_byte(cb, 0x81);
    emit_byte(cb, 0xEC);
    emit_uint32(cb, (uint32_t)imm);
}

/* LEAVE (MOV RSP, RBP; POP RBP) */
static inline void emit_leave(CodeBuf* cb) {
    emit_byte(cb, 0xC9);
}

/* RET */
static inline void emit_ret(CodeBuf* cb) {
    emit_byte(cb, 0xC3);
}

/* MOV EAX, imm32 (sets return value) */
static inline void emit_mov_eax_imm32(CodeBuf* cb, uint32_t imm) {
    emit_byte(cb, 0xB8);
    emit_uint32(cb, imm);
}

/* XOR EAX, EAX (return 0) */
static inline void emit_xor_eax_eax(CodeBuf* cb) {
    emit_byte(cb, 0x31);
    emit_byte(cb, 0xC0);
}

/* ---- Jumps (rel32, patch later) ---- */

/* JMP rel32 — returns offset of the rel32 placeholder for patching */
static inline int emit_jmp(CodeBuf* cb) {
    emit_byte(cb, 0xE9);
    int off = cb->len;
    emit_uint32(cb, 0);
    return off;
}

/* Jcc rel32 — returns offset of the rel32 placeholder */
static inline int emit_jcc(CodeBuf* cb, uint8_t cc) {
    emit_byte(cb, 0x0F);
    emit_byte(cb, cc);
    int off = cb->len;
    emit_uint32(cb, 0);
    return off;
}

/* SETcc reg8 — set byte to 1 if condition, 0 otherwise */
static inline void emit_setcc_reg(CodeBuf* cb, uint8_t cc, int reg) {
    int b = (reg >> 3) & 1;
    if (b) emit_byte(cb, 0x41);    /* REX.B for R8B-R15B */
    emit_byte(cb, 0x0F);
    emit_byte(cb, (uint8_t)(0x90 + cc));
    emit_byte(cb, modrm(3, 0, reg & 7));
}

/* MOVZX r32, r8  (zero-extend 8-bit result to 32/64-bit) */
static inline void emit_movzx_r32_r8(CodeBuf* cb, int dst, int src) {
    int r = (dst >> 3) & 1;
    int b = (src >> 3) & 1;
    emit_byte(cb, rex(0, r, 0, b));   /* no W, but need R/B for ext regs */
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0xB6);
    emit_byte(cb, modrm(3, dst & 7, src & 7));
}

/* ---- Patch rel32 at a given offset ---- */
static inline void patch_rel32(CodeBuf* cb, int patch_off, int target_off) {
    int32_t rel = target_off - (patch_off + 4);
    cb->buf[patch_off + 0] = (uint8_t)(rel);
    cb->buf[patch_off + 1] = (uint8_t)(rel >> 8);
    cb->buf[patch_off + 2] = (uint8_t)(rel >> 16);
    cb->buf[patch_off + 3] = (uint8_t)(rel >> 24);
}

/* ---- XMM (SSE2) emit helpers for float operations ---- */

/*
 * XMM register encoding: register 0-7 map directly, 8-15 need REX.R.
 * We use XMM0-XMM3 to avoid conflicts.
 *   XMM0 = 0, XMM1 = 1, XMM2 = 2, XMM3 = 3
 *
 * SSE2 opcodes (with 0x66 prefix, REX.W):
 *   ADDSD  = 0x0F 0x58 /r  (xmm1 = xmm2 + xmm1, we use dst, src form)
 *   SUBSD  = 0x0F 0x5C /r
 *   MULSD  = 0x0F 0x59 /r
 *   DIVSD  = 0x0F 0x5E /r
 *   UCOMISD= 0x0F 0x2E /r  (compare, sets EFLAGS)
 *   SQRTSD = 0x0F 0x51 /r  (not needed yet)
 *
 * MOVSD reg, [base+disp8]   = 0x66 0x0F 0x10 /r
 * MOVSD [base+disp8], reg   = 0x66 0x0F 0x11 /r
 * MOVSD reg, [base+disp32]  = 0x66 0x0F 0x10 /r with disp32
 * MOVSD reg, reg             = 0x66 0x0F 0x10 /r (load form, modrm(3,...))
 *
 * CVTSI2SD xmm, r/m64       = 0x66 0x0F 0x2A /r (int64 → double)
 */

/* MOVSD xmm, [base + disp8] */
static inline void emit_movsd_xmm_mem8(CodeBuf* cb, int xmm, int base, int8_t disp) {
    int r = (xmm >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x10);
    emit_byte(cb, modrm(1, xmm & 7, base & 7));
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_byte(cb, (uint8_t)disp);
}

/* MOVSD xmm, [base + disp32] */
static inline void emit_movsd_xmm_mem32(CodeBuf* cb, int xmm, int base, int32_t disp) {
    int r = (xmm >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x10);
    emit_byte(cb, modrm(2, xmm & 7, base & 7));
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_uint32(cb, (uint32_t)disp);
}

/* MOVSD [base + disp8], xmm */
static inline void emit_movsd_mem8_xmm(CodeBuf* cb, int base, int8_t disp, int xmm) {
    int r = (xmm >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x11);
    emit_byte(cb, modrm(1, xmm & 7, base & 7));
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_byte(cb, (uint8_t)disp);
}

/* MOVSD [base + disp32], xmm */
static inline void emit_movsd_mem32_xmm(CodeBuf* cb, int base, int32_t disp, int xmm) {
    int r = (xmm >> 3) & 1;
    int b = (base >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x11);
    emit_byte(cb, modrm(2, xmm & 7, base & 7));
    if ((base & 7) == 4)
        emit_byte(cb, 0x24);
    emit_uint32(cb, (uint32_t)disp);
}

/* SSE2 scalar arithmetic: opcode is 0x58=ADDSD, 0x5C=SUBSD, 0x59=MULSD, 0x5E=DIVSD */
/* Operation: xmm_dst = xmm_dst OP xmm_src (ModRM(11, dst, src) for these) */
/* Scalar SSE2 uses 0xF2 prefix (not 0x66 which is packed double) */
static inline void emit_sse2_rr(CodeBuf* cb, uint8_t opcode, int dst, int src) {
    int r = (src >> 3) & 1;
    int b = (dst >> 3) & 1;
    emit_byte(cb, 0xF2);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, opcode);
    emit_byte(cb, modrm(3, dst & 7, src & 7));
}

/* UCOMISD xmm1, xmm2 — compare, sets EFLAGS */
static inline void emit_ucomisd_rr(CodeBuf* cb, int xmm1, int xmm2) {
    int r = (xmm2 >> 3) & 1;
    int b = (xmm1 >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x2E);
    emit_byte(cb, modrm(3, xmm1 & 7, xmm2 & 7));
}

/* CVTSI2SD xmm, r/m64 — convert int64 to double */
static inline void emit_cvtsi2sd(CodeBuf* cb, int xmm, int reg) {
    int r = (xmm >> 3) & 1;
    int b = (reg >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x2A);
    emit_byte(cb, modrm(3, xmm & 7, reg & 7));
}

/* MOVSD xmm, xmm (load form: 0x66 0x0F 0x10 /r with mod=11) */
static inline void emit_movsd_xmm_xmm(CodeBuf* cb, int dst, int src) {
    int r = (src >> 3) & 1;
    int b = (dst >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x10);
    emit_byte(cb, modrm(3, dst & 7, src & 7));
}

/* XORPD xmm, xmm — XOR for sign-flipping (negate double via XORPD with sign mask) */
static inline void emit_xorpd_xmm_xmm(CodeBuf* cb, int dst, int src) {
    int r = (src >> 3) & 1;
    int b = (dst >> 3) & 1;
    emit_byte(cb, 0x66);
    emit_byte(cb, rex(1, r, 0, b));
    emit_byte(cb, 0x0F);
    emit_byte(cb, 0x57);
    emit_byte(cb, modrm(3, dst & 7, src & 7));
}

#endif /* LENO_JIT_EMIT_H */
