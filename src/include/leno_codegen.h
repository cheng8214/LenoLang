#ifndef LENO_CODEGEN_H
#define LENO_CODEGEN_H

#include "leno_ast.h"
#include "leno_semantic.h"
#include "leno_vm.h"

// ============================================================================
// 循环上下文（用于 break 和 continue）
// ============================================================================

typedef struct {
    int break_jumps[MAX_BREAK_JUMPS];     // break 跳转位置数组
    int break_count;
    int continue_jumps[MAX_CONTINUE_JUMPS];  // continue 跳转位置数组（需要回填）
    int continue_count;
    int continue_target;      // continue 跳转目标位置（用于简单循环）
} LoopContext;

// 循环上下文链表节点（堆分配，无嵌套深度限制，不占栈空间）
typedef struct LoopContextNode {
    LoopContext ctx;
    struct LoopContextNode* prev;
} LoopContextNode;

// ============================================================================
// 析构追踪条目（用于在作用域结束/return 时调用析构函数）
// ============================================================================

typedef struct {
    int local_slot;    // 局部变量槽位索引（= 寄存器号）
} DtorEntry;

// ============================================================================
// 寄存器式字节码生成器
// ============================================================================
// 寄存器文件 = CallFrame.locals：
//   R0..arity-1     = 函数参数
//   R[arity..n-1]   = 声明的局部变量
//   R[n..max-1]     = 临时寄存器（空闲栈管理）
// local_count = 寄存器高水位（GC 依赖）
// ============================================================================

#define MAX_REG 256  // 单字节寄存器号上限（超过需 EXTEND 前缀）
#define MAX_MOD_ALIASES 64  // import 别名登记上限（区分原生模块与 .leno 模块）

typedef struct {
    Chunk* chunk;
    Semantic* sem;
    int scope_depth;
    LoopContextNode* loop_head;    // 链表头（当前最内层循环），使用链式堆分配避免栈溢出
    int loop_count;                 // 循环嵌套深度
    ObjFunction* current_func;     // 当前正在生成的函数（用于更新 local_count）
    Ast* current_func_ast;         // 当前函数的 **AST**（gen_return 要用它取声明的返回类型做
                                   //   返回值规范化 —— ObjFunction 上的 return_types 在寄存器式
                                   //   里从未被填过，不能依赖）
    // --- 寄存器分配器 ---
    int next_reg;           // 下一个可分配的寄存器号（= 参数+声明变量数量）
    int max_reg;            // 寄存器高水位（临时寄存器峰值，写回 local_count）
    int free_regs[MAX_REG]; // 空闲寄存器栈
    int freetop;            // 空闲栈顶
    int scope_base;         // 当前作用域起始寄存器（作用域退出时回退到此）
    // --- 析构追踪 ---
    DtorEntry* dtor_entries;
    int dtor_count;
    int dtor_capacity;
    int dtor_temp_slot;     // return 时保存返回值的临时寄存器（-1=未分配）
    // --- 多返回值 / 解构 ---
    int suppress_multi_pop;  // 解构声明上下文标记
    // --- import 别名表：区分「原生模块」与「.leno 源码模块」 ---
    //   两者的 AST_MODULE_CALL 长得一模一样（module_name 是别名、lib_ref 无信息、
    //   全局符号都是 SYM_GLOBAL），只能在 import 时登记，调用点回查。
    char* mod_aliases[MAX_MOD_ALIASES];
    char* mod_real_names[MAX_MOD_ALIASES];  // 真实模块名（native 方法表按它注册）
    unsigned char mod_alias_native[MAX_MOD_ALIASES];
    int mod_alias_count;
} CodeGen;

// 登记 import：别名 → 真实模块名 + 是否原生模块
//   别名（如 `import times as ti` 的 ti）与真实模块名（times）都要记住：
//   AST_MODULE_CALL 里只有别名，而 native 方法表是按真实名注册的。
static inline void codegen_record_module_alias(CodeGen* gen, const char* alias,
                                               const char* real_name, int is_native) {
    if (!alias || !alias[0]) return;
    for (int i = 0; i < gen->mod_alias_count; i++) {
        if (gen->mod_aliases[i] && strcmp(gen->mod_aliases[i], alias) == 0) {
            gen->mod_alias_native[i] = (unsigned char)(is_native ? 1 : 0);
            if (real_name) {
                free(gen->mod_real_names[i]);
                gen->mod_real_names[i] = strdup(real_name);
            }
            return;
        }
    }
    if (gen->mod_alias_count >= MAX_MOD_ALIASES) return;
    int i = gen->mod_alias_count++;
    gen->mod_aliases[i] = strdup(alias);
    gen->mod_real_names[i] = strdup(real_name ? real_name : alias);
    gen->mod_alias_native[i] = (unsigned char)(is_native ? 1 : 0);
}

// 查别名：返回是否原生模块，*real_name 回填真实模块名（未登记则回退为别名本身）
static inline int codegen_module_lookup(CodeGen* gen, const char* alias,
                                        const char** real_name, int* found) {
    if (found) *found = 0;
    if (real_name) *real_name = alias;
    if (!alias) return 0;
    for (int i = 0; i < gen->mod_alias_count; i++) {
        if (gen->mod_aliases[i] && strcmp(gen->mod_aliases[i], alias) == 0) {
            if (found) *found = 1;
            if (real_name) *real_name = gen->mod_real_names[i];
            return gen->mod_alias_native[i];
        }
    }
    return 0;
}

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem);
void codegen_cleanup(CodeGen* gen);
void codegen(CodeGen* gen, Ast* ast);
void codegen_module(CodeGen* gen, Ast* ast);
void codegen_set_func_dict(void* dict);
void codegen_set_module(ObjModule* module);
void codegen_add_dtor_entry(CodeGen* gen, int local_slot);

// 函数生成（供 codegen_stmt.c 使用）
ObjFunction* gen_func_proto(CodeGen* gen, Ast* ast);
void gen_func_closure(CodeGen* gen, Ast* ast, ObjFunction* func);
// 发射 OP_CLOSURE + upvalue 捕获描述（定义在 codegen_func.c）
void emit_closure_upvals(CodeGen* gen, int dst, int const_idx, Ast* ast);

// ============================================================================
// 寄存器分配 API
// ============================================================================

// 借一个临时寄存器
// 注意：free 栈里可能出现 >= next_reg 的号（调用点整块回退 next_reg 后，
// 块内被 free 的号就"悬空"了）——这类号必须丢弃，否则会与块内已占用的寄存器撞车。
static inline int reg_alloc(CodeGen* gen) {
    while (gen->freetop > 0) {
        int r = gen->free_regs[--gen->freetop];
        if (r < gen->next_reg) return r;
    }
    int r = gen->next_reg++;
    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
    return r;
}

// 归还一个临时寄存器
// 只接受"确实是本函数临时区内的号"（r < next_reg），否则会把变量槽位（0/1 等）
// 混进 free 栈，后续临时寄存器分配就会覆盖变量。
static inline void reg_free(CodeGen* gen, int r) {
    if (r >= 0 && r < MAX_REG && r < gen->next_reg) {
        gen->free_regs[gen->freetop++] = r;
    }
}

// 连续分配 n 个寄存器（调用点 / 多返回值等需要"实参紧随 callee"的场景）
// 会把 free 栈清空：块内号码不能被复用，否则实参会被后续分配覆盖。
static inline int reg_alloc_block(CodeGen* gen, int n) {
    gen->freetop = 0;
    int base = gen->next_reg;
    gen->next_reg += n;
    if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
    return base;
}

// 整块归还：回退 next_reg 到 base
static inline void reg_free_block(CodeGen* gen, int base) {
    if (base >= 0 && base < gen->next_reg) {
        gen->next_reg = base;
        if (gen->freetop > 0) {
            int keep = 0;
            while (keep < gen->freetop && gen->free_regs[keep] < base) keep++;
            gen->freetop = keep;
        }
    }
}

// 进入作用域：记录当前 next_reg 为 scope_base
static inline void reg_scope_enter(CodeGen* gen) {
    // 保存当前 scope_base，push 到 next_reg 之前的位置
    // 简化版：scope_base = next_reg，退出时 next_reg 回退到 scope_base
    gen->scope_base = gen->next_reg;
    gen->scope_depth++;
}

// 退出作用域：临时寄存器回退到 scope_base
static inline void reg_scope_exit(CodeGen* gen) {
    gen->next_reg = gen->scope_base;
    gen->freetop = 0;  // 空闲栈清空（所有临时寄存器失效）
    gen->scope_depth--;
}

#endif // LENO_CODEGEN_H
