/*
 * jit_priv.h - 内部共享声明（jit_callout.c / jit_scan.c / jit.c 共用）
 * 公共平台部分：见 jit.h（API）、jit_mem.h（可执行内存，平台分支）。
 */
#ifndef LENO_JIT_PRIV_H
#define LENO_JIT_PRIV_H

#include "jit.h"
#include "../include/native.h"   /* ModuleMethodMeta（模块方法编译期解析用） */
#include <stdint.h>
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

/* ---- NaN-boxing constants (must match leno_value.h) ---- */
/* QNAN | SIGN_BIT | TAG_MASK = 0xFFFF000000000000 */
#define JIT_TYPE_MASK   0xFFFF000000000000ULL
/* QNAN | SIGN_BIT | TAG_INT  = 0xFFFB000000000000 */
#define JIT_INT_TAG     0xFFFB000000000000ULL
/* PAYLOAD_MASK                        = 0x0000FFFFFFFFFFFF */
#define JIT_PAYLOAD_MSK 0x0000FFFFFFFFFFFFULL
/* QNAN | SIGN_BIT                     = 0xFFF8000000000000
 * Any Value whose top 16 bits (after shr 48) >= 0xFFF8 is NaN-boxed
 * (null/bool/int/obj/bigint) and NOT a float. */
#define JIT_NAN_SIG     0xFFF8000000000000ULL

static inline uint16_t rd_short(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline int32_t rd_int32(const uint8_t* p) {
    return (int32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
}
static inline int8_t rd_byte(const uint8_t* p) {
    return (int8_t)p[0];
}

/* ---- Inline site ----
 * 两种来源：
 *   1) OP_CALL_GLOBAL_FUNC_TYPED —— 全局函数（func_slot 定位校准，静态唯一）；
 *   2) OP_INVOKE_METHOD_TYPED —— struct 方法（is_method=1，method_def 为编译期
 *      按静态类型名解析出的定义；codegen 仍为该接收者生成运行时 def 守卫，因为
 *      VM 侧是动态分发，同一条字节码理论上可落到别的 struct 上）。 */
typedef struct {
    int bc_off;              /* bytecode offset of the call in caller */
    uint16_t func_slot;      /* global func slot of callee（方法内联时为 0xFFFF） */
    int arg_count;           /* number of args（方法含 self） */
    int ret_count;           /* number of return values (1 or 2) */
    Chunk* callee_chunk;     /* callee's bytecode chunk */
    int callee_local_count;  /* callee's local_count */
    int callee_local_base;   /* base scratch index for callee locals */
    int callee_body_size;    /* bytecode size of callee body */
    int callee_local_map[256]; /* callee local slot → scratch index */
    int inline_end_mc;       /* mc offset of inline-end label (set during codegen) */
    /* ---- 仅方法内联（is_method=1）使用 ---- */
    int is_method;           /* 1 = OP_INVOKE_METHOD_TYPED 内联点 */
    ObjStructDef* method_def;/* 接收者必须是的 struct 定义（运行时守卫） */
} InlineSite;

/* ---- Scan result ---- */
typedef struct {
    int capable;
    int num_locals;
    uint8_t local_slots[JIT_MAX_LOCALS];
    int local_map[256];         /* slot → scratch index, -1 if unused */
    int max_vstack;
    int body_size;              /* bytecode bytes (including back-edge) */
    int back_edge_type;         /* 1=OP_LOOP, 2=OP_FOR_LOOP */
    /* FOR_LOOP info */
    uint8_t for_loop_var_slot;
    uint8_t for_step_slot;
    uint8_t for_end_slot;
    uint8_t for_inclusive;
    /* Inline sites */
    InlineSite inline_sites[4]; /* max 4 inline calls per loop */
    int inline_count;
    int inline_extra_locals;   /* total extra locals from inlining */
    /* 循环体内存在可达的 OP_RETURN / OP_RETURN_MULTI（提前返回）。
     * 循环 JIT（loop mode）没有能力从机器码中返回函数——codegen 对
     * OP_RETURN 只能「spill 后继续往下跑」，于是这个 return 被静默丢弃
     * （五子棋 nearStone 的 `if 有子 { return true }` 因此永远返回 false，
     * AI 每步都走天元）。消费方 jit_compile() 据此拒绝该循环；
     * 函数级 JIT（func_mode）与内联 callee 都能正确处理 return，不受影响。 */
    int has_reachable_return;

    /* ---- §8.46 语句级折叠「一步回看」的健全信息（见 ops_stack.inc 的用法）----
     * 回看的前提是：**当前语句只可能由紧邻的上一条语句直落到达**。两条信息合起来
     * 保证这一点（任一不满足就放弃回看，不发恒等运算/不省检查）：
     *   jt_fwd[off]  —— off 是某个**前向**跳转的目标 ⇒ 上一条语句可能被跳过；
     *   has_back_jump—— 体内存在**非本条回边**的反向跳转（嵌套循环的 OP_LOOP /
     *                   内层 OP_FOR_LOOP）。这种目标可能正好落在两条相邻语句之间
     *                   （do{}while 形态：循环头就是第二条语句），第二轮的到达
     *                   会跳过第一条 ⇒ 回看状态失效。
     * jt_ok = 0 表示 body 超过位图上限（或处于内联体内、bc_off 被重定位）⇒ 不可信。 */
    #define JIT_SCAN_JT_MAX 512
    uint8_t jt_fwd[JIT_SCAN_JT_MAX];
    int jt_ok;
    int has_back_jump;
} ScanResult;

/* ---- Codegen: offset map and patch list ---- */
typedef struct {
    int bc_off;  /* bytecode offset (relative to body_start) */
    int mc_off;  /* machine code offset (relative to codebuf) */
} OffMap;

typedef struct {
    int patch_mc;     /* offset in codebuf where rel32 lives */
    int target_bc;    /* bytecode offset of jump target */
    int vstack;       /* virtual stack depth at this point (for cleanup) */
} Patch;

typedef struct {
    CodeBuf cb;
    OffMap off_map[JIT_MAX_LOOP_OPS * 10];
    int off_count;
    Patch patches[JIT_MAX_PATCHES];
    int patch_count;
    int loop_start_mc;   /* machine code offset of loop body start */
    int exit_mc;         /* machine code offset of exit code */
    /* off_map / patches 溢出标志（见 offmap_add / patch_add 的说明）。
     * 置位则 compile_loop 收尾时拒绝这次编译，循环退回解释器执行。 */
    int off_overflow;
    int patch_overflow;
    /* ---- bailout 站点桩（§8.40，见下面 patch_add 的说明）---- */
    int bail_stub_mc[JIT_MAX_BAILOUT_STUBS];    /* 该分支的 jcc/jmp rel32 位置 */
    int bail_stub_site[JIT_MAX_BAILOUT_STUBS];  /* 写进 jit_bailout_site 的值 */
    int bail_stub_count;
    int pending_site;        /* EMIT_BAILOUT_SITE_WRITE 记下、等 bailout 分支消费 */
    int pending_site_valid;
    int bailout_mc;      /* machine code offset of bailout code */
    int framedead_mc;    /* machine code offset of frame-dead exit (write back, ret 2) */
    int framedead_nowb_mc; /* machine code offset of frame-dead exit (no write back, ret 3) */
    const ScanResult* sr;
    const uint8_t* body_start;
    Chunk* chunk;        /* for constant table access */
    VM* vm_ptr;          /* for looking up global_funcs at codegen time */
    int func_mode;       /* 1 = 函数级 JIT：整个函数编译为 fn(locals, globals)，
                          * OP_RETURN 真正返回（结果写 jit_fn_result），
                          * exit 不写回 locals（调用方用临时数组） */
} CodegenCtx;

/* ---- Platform-independent codegen helpers (shared by all backends) ----
 * Used by every backend's compile_loop (off_map / patches are generic
 * bytecode-offset <-> machine-code-offset bookkeeping, no ISA specifics). */
/* off_map / patches 溢出都**不能**静默丢弃条目：
 *   - 丢了 off_map 条目 → offmap_lookup 找不到目标 → OP_LOOP/OP_FOR_LOOP 的
 *     回边会退化成「跳到外层循环起点」→ 死循环 + RSP 漂移 → 栈溢出（§8.26 同类）；
 *     前向跳转则被重定向到 exit → 静默算错。
 *   - 丢了 patch 条目 → 那条 rel32 永远保持 0 → 跳到代码段开头。
 * 因此这里只记录标志，由 compile_loop 在收尾时统一拒绝编译（宁可不编，不要猜）。
 * 注意 off_map 的实际容量是 JIT_MAX_LOOP_OPS * 10，旧实现只用到 * 2，
 * 4096 字节的循环体 + 4 个内联体就可能触到，属于可复现的隐患。 */
static inline void offmap_add(CodegenCtx* ctx, int bc_off, int mc_off) {
    if (ctx->off_count < JIT_MAX_LOOP_OPS * 10) {
        ctx->off_map[ctx->off_count].bc_off = bc_off;
        ctx->off_map[ctx->off_count].mc_off = mc_off;
        ctx->off_count++;
    } else {
        ctx->off_overflow = 1;
    }
}

static inline int offmap_lookup(CodegenCtx* ctx, int bc_off) {
    for (int i = 0; i < ctx->off_count; i++) {
        if (ctx->off_map[i].bc_off == bc_off)
            return ctx->off_map[i].mc_off;
    }
    return -1;
}

/* bailout 分支的「站点桩」（§8.40）
 *
 * 原来：每个 int48/溢出检查在**热路径上**就地写一次 jit_bailout_site
 *       （push r9; movabs r9,&site; mov [r9],imm32; pop r9 —— 4 条指令、15 字节，
 *        其中 movabs 占 10 字节），只为「万一 bailout 时能把失败位置报出来」。
 * 现在：分支改成先跳到**自己的桩**，桩里才写 site 再 jmp 到共享 bailout 块
 *       （诊断信息一字不差，但只有真的 bailout 才执行）。
 *
 * 记录时序：EMIT_BAILOUT_SITE_WRITE(site) 只置 pending_*；紧随其后的
 * patch_add(..., -1, ...)（= bailout 分支）把它消费成一条桩记录。 */
static inline void patch_add(CodegenCtx* ctx, int patch_mc, int target_bc, int vstack) {
    if (target_bc == -1 && ctx->pending_site_valid) {
        ctx->pending_site_valid = 0;
        if (ctx->bail_stub_count < JIT_MAX_BAILOUT_STUBS) {
            ctx->bail_stub_mc[ctx->bail_stub_count] = patch_mc;
            ctx->bail_stub_site[ctx->bail_stub_count] = ctx->pending_site;
            ctx->bail_stub_count++;
        } else {
            /* 桩表满（单个循环体里有 >512 个 bailout 检查，实际不可达）：按项目
             * 原则「宁可不编，不要猜」拒绝这次编译 —— 绝不静默丢站点（那会让
             * stats 报出错误的失败位置）。 */
            ctx->patch_overflow = 1;
        }
        return;
    }
    /* 站点没被 bailout 分支消费（例如刚写完 site 就改了主意）→ 丢弃，
     * 避免串到下一个不相干的分支上。 */
    ctx->pending_site_valid = 0;
    if (ctx->patch_count < JIT_MAX_PATCHES) {
        ctx->patches[ctx->patch_count].patch_mc = patch_mc;
        ctx->patches[ctx->patch_count].target_bc = target_bc;
        ctx->patches[ctx->patch_count].vstack = vstack;
        ctx->patch_count++;
    } else {
        ctx->patch_overflow = 1;
    }
}

/* ---- Global state (defined in jit_callout.c) ---- */
extern VM* jit_callout_vm;
extern volatile int jit_callout_failed;
extern Value* jit_reloaded_locals;
extern int64_t jit_bailout_rax;
extern int32_t jit_bailout_site;

/* ---- Function-level JIT (defined in jit_callout.c) ----
 * jit_fn_result:  函数级 JIT 机器码通过 OP_RETURN 写入的返回值（单返回）。
 * jit_func_depth: 当前 JIT 函数嵌套深度（递归保护，超过上限回退解释路径）。
 * jit_func_locals_pool: 按 depth 索引的固定 locals 池（零分配快路径）。
 * 池行宽 = JIT_MAX_LOCALS(64) 槽；depth 上限受 jit_func_depth < 64 约束。 */
extern Value jit_fn_result;
extern int jit_func_depth;
/* ---- 「当前在 JIT 机器码里吗」（2026-09-13，§8.36 / §8.37）----
 * jit_loop_depth: 正在执行**循环 JIT** 机器码的层数（jit_try_hot_loop 维护）
 * jit_func_depth: 正在执行**函数级 JIT** 机器码的层数（原义，递归保护）
 * jit_in_frame() : 两者之一 > 0。
 * 用途：gc_alloc 的 malloc 失败路径据此决定能不能**同步**回收 ——
 * JIT 的活值在它自己的机器栈帧里（locals scratch + vstack），mark_roots 看不见，
 * 就地回收等于把还在用的对象当垃圾（use-after-free）。 */
extern int jit_loop_depth;
int jit_in_frame(void);
/* ---- 「GC 想让 JIT 让出」标志（2026-09-13，§8.37 路线 3）----
 * gc_alloc 在年轻代跨过阈值的**那一瞬间**（deferred_gc 的 0→1）置位；
 * JIT 在每个回边轮询它（2 条指令：mov r8, imm64(&flag); cmp [r8], 0; jne），
 * 命中就走**出口路径**让出到解释器（写回 locals → 解释器回收 → 重进 JIT）。
 * 解释器在消费让出时清零。
 * 用普通全局（而非 per-VM 字段）是为了让轮询保持 2 条指令；多线程下只会
 * 「别人的请求让本线程多让出一次」，让出本身无害（解释器发现无需回收就直接继续）。 */
extern int jit_gc_yield_flag;
#define JIT_FUNC_MAX_DEPTH 64
extern Value jit_func_locals_pool[JIT_FUNC_MAX_DEPTH][JIT_MAX_LOCALS];

/* ---- Function-level JIT cache (defined in jit.c) ---- */
/* direct-mapped cache, power of 2。
 * 32 → 256（2026-09-13）：函数级 JIT 现在也从解释器调用点进入，
 * 同一进程里被编译的函数个数远多于「只有热循环 callout 调用」的年代；
 * 旧的 32 槽在中等规模程序里会频繁哈希冲突，而冲突的代价不只是重编译
 * —— jit_func_entry_claim 会 jit_mem_free 掉被驱逐函数的机器码，
 * 若那个函数正在 C 栈上执行（A 调 B，B 的 callout 又编译了撞槽的 C），
 * 就是 use-after-free。256 槽 ≈ 8KB 元数据，把冲突概率压到可忽略。 */
#define JIT_FUNC_CACHE_SIZE 256

/* 解释器侧函数调用热度阈值。与 JIT_HOT_THRESHOLD（循环回边 50 次）分开：
 * 函数编译比循环编译贵，且编译前的调用走的是完整解释器路径，
 * 阈值过低会让「只调用几次的冷函数」白付编译成本。 */
#define JIT_FUNC_HOT_THRESHOLD 50

typedef struct {
    ObjFunction* func;   /* cache key: ObjFunction pointer */
    int tried;           /* 1 = compilation attempted */
    JitLoopFn fn;        /* compiled machine code (NULL if compilation failed) */
    /* ---- 解释器侧热入口专用（jit_try_hot_func_call）----
     * 只由解释器调用点自增；JIT callout 路径的急切编译不计入，
     * 以保持「JIT 循环调用函数时立刻编译」的既有行为不变。 */
    int hit_count;
    /* 解释器侧入口停用：编译失败，或执行时返回 bailout / 失败。
     * 停用后该函数的解释器调用点永远走原解释路径（不再反复试探）。
     * 只影响解释器侧入口，callout 侧的急切编译/回退行为不受影响。 */
    int hot_disabled;
} JitFuncCacheEntry;
extern JitFuncCacheEntry jit_func_cache[JIT_FUNC_CACHE_SIZE];

/* Look up (or compile on first use) a function-level JIT entry.
 * Returns NULL if JIT disabled / unsupported / compilation failed. */
JitLoopFn jit_func_lookup_or_compile(ObjFunction* func, VM* vm_ptr);

/* ---- Runtime helpers (jit_callout.c) ---- */
int jit_debug_on(void);
void jit_ft_profile_dump(void);
void jit_bailout_debug(int64_t rsp_val);
Value jit_callout_index(Value obj_val, Value idx_val);
Value jit_callout_concat(Value a, Value b);
int jit_callout_array_append(Value arr_val, Value value);
Value jit_callout_dict_set(Value dict_val, Value key_val, Value value);
int jit_callout_index_set(Value obj_val, Value idx_val, Value value);
Value jit_callout_div(Value a, Value b);
Value jit_callout_acc_fields(Value obj_val, uint8_t count, const uint8_t* field_indices);
Value jit_callout_struct_init(int64_t* vstack_top, uint16_t name_const_idx,
                              uint8_t arg_count, const uint8_t* ip, Chunk* chunk);
Value jit_callout_invoke_method(int64_t* vstack_top, int arg_count, const uint8_t* ip, Chunk* chunk);
Value jit_callout_global_func(int64_t* vstack_top, int arg_count, uint16_t func_slot);
Value jit_callout_get_field_fast(Value obj_val, uint8_t field_idx);
Value jit_callout_module_call(int64_t* vstack_top, int arg_count, uint16_t module_idx, uint16_t method_idx, Chunk* chunk);

/* ---- 模块方法编译期解析（codegen 用）----
 * 旧实现让 callout 每次调用都做「字符串哈希 + strcmp」查模块方法表（解释器侧
 * 有 inline cache 规避，JIT 没有）。现在在 JIT 编译期解析一次，把
 * ModuleMethodMeta* 直接嵌进机器码。模块方法表在启动注册后不再变更
 * （native_reset_registry 未被调用），指针长期有效。 */
ModuleMethodMeta* jit_resolve_module_method(Chunk* chunk, uint16_t module_idx, uint16_t method_idx);

/* Callout：已解析 meta 的模块方法调用（无运行时查找） */
Value jit_callout_module_call_meta(int64_t* vstack_top, int arg_count, ModuleMethodMeta* meta);

/* ---- struct 方法返回值个数（按方法名推断，jit_scan.c）----
 * OP_INVOKE_METHOD_TYPED 正常路径用字节码里的静态类型名直接定位方法，解析失败时
 * 退回本函数（要求方法名在全部 def 中唯一）。JIT 的栈记账必须知道调用后留下几个
 * 返回值，多返回值方法（如 Font.measureString → [float, float]）按 1 个记账会让
 * 第一个返回值读到实参槽的残留值。返回 0 表示「无法确定」，调用方必须拒绝 JIT。 */
int jit_resolve_method_ret_count(Chunk* chunk, uint16_t name_const_idx);

/* ---- struct 方法**定义**解析（编译期，供方法内联用）----
 * 只有「方法名在全部已注册 struct 定义中唯一」且能取到 ObjFunction 时才返回
 * 非 NULL，并通过 out_def 回传该定义（codegen 用它生成接收者 def 守卫）。
 * 同名方法存在于多个 def（动态分发可能落到别的实现）/ 定义不完整 / 命中
 * 构造析构 → 一律返回 NULL（不猜，退回 callout）。 */
ObjFunction* jit_resolve_method_func(Chunk* chunk, uint16_t name_const_idx,
                                     ObjStructDef** out_def);

/* ---- 带静态类型的方法解析（OP_INVOKE_METHOD_TYPED，字节码带了类型名常量）----
 * 直接按类型名定位 def 再找方法，不需要「方法名在所有 def 中唯一」的推断，
 * 因此同名方法（init/update/clone…）也能确定性解析 / 内联。
 * 成功返回 1，并回传 def、方法体、返回值个数；失败返回 0（调用方应退回
 * jit_resolve_method_ret_count / jit_resolve_method_func，再不行拒绝 JIT）。 */
int jit_resolve_method_typed(Chunk* chunk, uint16_t name_const_idx, uint16_t type_const_idx,
                             ObjStructDef** out_def, ObjFunction** out_fn, int* out_ret_count);

/* ---- 通用「数值薄调用」桥（jit_callout.c）----
 * 模块方法若注册为「全部参数 + 返回值都是 float」（param_types/return_type），
 * codegen 可以把实参当 double 直接放进 xmm0..3 并调用下面的薄桥：
 *   - 桥内部仍调用该模块原本的 NativeFn（数学实现只有模块里那一份）
 *   - 省掉 callout 的 vstack 数组、逐参数装箱、模块方法查表
 *   - native 内部抛错 → 置 jit_callout_failed，codegen 检查后 bailout
 * 支持 1..4 个 double 参数（xmm0..3；首参寄存器位放桥函数指针）。 */
void* jit_thin_bridge_for(int arity);
Value jit_callout_array_new(int64_t* vstack_top, uint16_t count);
Value jit_callout_call_native(int64_t* vstack_top, ObjNative* native, uint16_t arg_count);
Value jit_callout_get_property(int64_t* vstack_top, uint16_t name_const_idx, uint16_t call_or_args, Chunk* chunk);

/* ---- Scanning (jit_scan.c) ---- */
int cache_hash(const uint8_t* ip);
int opcode_size(const uint8_t* ip);
void scan_loop_body(const uint8_t* body_start, int body_size, int back_edge, ScanResult* r,
                    VM* vm_ptr, Chunk* chunk);

/* ---- Codegen (backend/x86_64.c) ---- */
int compile_loop(CodegenCtx* ctx);

#endif /* LENO_JIT_PRIV_H */


