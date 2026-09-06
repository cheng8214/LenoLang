/*
 * jit.h - JIT compiler public API
 *
 * Design:
 *   - JIT is an optional acceleration layer on top of the existing stack VM.
 *   - VM bytecode format and dispatch loop are NOT modified.
 *   - Only hot loops are JIT-compiled (detected via hit counter in OP_LOOP /
 *     OP_FOR_LOOP handlers).
 *   - JIT functions work with raw int64_t values extracted from NaN-boxed
 *     locals at entry; on exit, values are re-encoded and written back.
 *   - On type-guard failure, JIT returns 1 (bailout) and the interpreter
 *     resumes normal execution.
 *
 * Calling convention (Windows x64):
 *   RCX = Value* locals   (first argument)
 *   Return value (EAX) = 0 (success) or 1 (bailout)
 */
#ifndef LENO_JIT_H
#define LENO_JIT_H

#include <stdint.h>
#include "../include/leno_vm.h"

/* ---- Configuration ---- */
#define JIT_HOT_THRESHOLD   50    /* loop iterations before compilation  */
#define JIT_CACHE_SIZE      256   /* cache hash table size (power of 2)  */
#define JIT_MAX_LOCALS      32    /* max locals to type-guard            */
#define JIT_MAX_LOOP_OPS    256   /* max opcodes in a JIT-able loop body */
#define JIT_MAX_VSTACK      64    /* max virtual stack depth             */
#define JIT_MAX_LABELS      32    /* max jump labels in a loop body      */
#define JIT_MAX_PATCHES     64    /* max jump patches                    */
#define JIT_BAILOUT_LIMIT   3     /* after N bailouts, stop trying      */

/* ---- JIT function type ---- */
/* Windows x64 ABI: RCX = Value* locals, RDX = Value* globals */
typedef int (*JitLoopFn)(Value* locals, Value* globals);

/* ---- Cache entry ---- */
typedef struct {
    const uint8_t* loop_ip;      /* bytecode address of loop body start */
    const uint8_t* after_ip;    /* bytecode address after back-edge    */
    JitLoopFn      fn;          /* compiled function (NULL if none)    */
    int            hit_count;   /* times seen in interpreter           */
    int            is_compiled; /* 1 = compiled                        */
    int            tried;       /* 1 = compilation attempted (don't retry) */
    int            bailout_count;/* times JIT bailed out                */
    int            back_edge;   /* OP_LOOP=1, OP_FOR_LOOP=2            */
} JitCacheEntry;

/* ---- JIT state ---- */
typedef struct {
    JitCacheEntry cache[JIT_CACHE_SIZE];
    int compile_count;
    int execute_count;
    int bailout_count;
    int enabled;
} JitState;

/* Global JIT state (single-threaded VM) */
extern JitState jit_state;

/* ---- API ---- */

/* Initialize JIT state. Call once at startup. */
void jit_init(void);

/* Shut down JIT, free all executable memory. */
void jit_close(void);

/* Enable/disable JIT at runtime. */
void jit_set_enabled(int enabled);

/*
 * Called from OP_LOOP / OP_FOR_LOOP handlers.
 *
 * Parameters:
 *   frame       - current call frame (ip already past the back-edge instruction)
 *   vm_ptr      - VM pointer (for stack access if needed)
 *   loop_offset - the backward jump offset from the back-edge instruction
 *   back_edge   - 1 = OP_LOOP, 2 = OP_FOR_LOOP
 *
 * Returns:
 *   1 = JIT executed the loop to completion; frame->ip is already correct,
 *       caller should just DISPATCH.
 *   0 = JIT did not handle this loop (not hot, not compiled, bailed out,
 *       or unsupported); caller should proceed with normal execution.
 */
int jit_try_hot_loop(CallFrame* frame, VM* vm_ptr, int32_t loop_offset, int back_edge);

/* Print JIT statistics to stdout. */
void jit_print_stats(void);

#endif /* LENO_JIT_H */
