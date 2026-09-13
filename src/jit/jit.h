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
* Calling convention (defined by the backend, e.g. x86_64 Win64):
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
#define JIT_MAX_LOCALS      64    /* max locals to type-guard (incl inline) */
#define JIT_MAX_LOOP_OPS    256   /* max opcodes in a JIT-able loop body */
#define JIT_MAX_VSTACK      64    /* max virtual stack depth             */
#define JIT_MAX_LABELS      32    /* max jump labels in a loop body      */
#define JIT_MAX_PATCHES     512   /* max jump patches                    */
#define JIT_BAILOUT_LIMIT   3     /* after N bailouts, stop trying      */
/* 线性探测窗口：哈希冲突时在同一窗口内先找空槽，避免像老实现那样就地覆盖
 * 另一个热循环（两个别名循环会互相驱逐 → 每次进入都重编译） */
#define JIT_CACHE_PROBES    8

/* ---- JIT function type ---- */
/* Calling convention is target-ABI specific (see backend/x86_64.c);
 * from C's perspective it is a plain function pointer. */
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
    /* ---- 最近一次 bailout 的定位信息（jit_print_stats 输出）----
     * site 编码约定见 x86_64.c 的 EMIT_BAILOUT_SITE_*：
     *   >= 0       : 溢出/截断类检查，值 = 触发指令的 bc_off
     *   -999..-1   : JIT 序言（-1 进入自增溢出、-2 step == 0、-3 step 为 float）
     *   <= -1000   : 其它原因，bc_off = -1000 - site */
    int            last_bailout_site;
    int            last_bailout_bc_off; /* 循环体起始字节码偏移 */
    const char*    last_bailout_fn;     /* 所属函数名（main 为 "<main>"） */
} JitCacheEntry;

/* ---- JIT state ---- */
typedef struct {
    JitCacheEntry cache[JIT_CACHE_SIZE];
    int compile_count;
    int execute_count;
    int bailout_count;
    int cache_evictions;  /* 探测窗口满而被迫驱逐热循环的次数（诊断用） */
    int func_compile_count; /* 函数级 JIT 编译成功次数（含解释器热入口与 callout 急切编译） */
    int func_execute_count; /* 解释器侧函数级 JIT 热入口实际执行的次数 */
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

/*
 * Called from OP_CALL / OP_CALL_GLOBAL_FUNC[_TYPED]——解释器侧的函数级 JIT
 * 热入口。
 *
 * 动机：函数级 JIT（jit_compile_function）此前只有「被某个 JIT 热循环
 * callout 调用到」才会编译，纯递归/解释器调用链（如顶层一次性 fib(30)）
 * 完全进不去，收益恒为 1x。这里在解释器的调用点上打一个独立热点计数，
 * 与「循环回边」解耦。
 *
 * VM 栈约定与 call() 完全一致（调用方负责把 callee 压在栈顶）：
 *   [...][arg1][arg2]...[argN][callee=stack[sp-1]]
 * 执行成功时把 arg_count+1 个槽折叠成 1 个返回值：
 *   sp -= arg_count，返回值写入原 callee 槽（stack[sp-1]）。
 *
 * Parameters:
 *   closure   - 被调闭包（其 function 为编译目标）
 *   arg_count - 实参个数（不含 callee）
 *   typed     - 1 = 调用点已由编译期保证参数类型（对齐 call_no_type_check，
 *               跳过 int/float 提升）；0 = 对齐 call() 的参数类型提升
 *   vm_ptr    - VM 指针
 *
 * Returns:
 *   1 = 已由 JIT 执行完本次调用（返回值已落栈，调用方需重新取 frame 后 DISPATCH）
 *   0 = 未处理（未达热度 / 不可编译 / 执行失败已复位），调用方必须走原解释路径
 */
int jit_try_hot_func_call(ObjClosure* closure, int arg_count, int typed, VM* vm_ptr);

/* Print JIT statistics to stdout. */
void jit_print_stats(void);

#endif /* LENO_JIT_H */
