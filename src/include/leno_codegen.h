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

// ============================================================================
// 字节码生成器
// ============================================================================

typedef struct {
    Chunk* chunk;
    Semantic* sem;
    int scope_depth;
    LoopContext loops[MAX_LOOP_NESTING];    // 循环上下文栈
    int loop_count;           // 当前循环嵌套深度
    ObjFunction* current_func; // 当前正在生成的函数（用于更新 local_count）
    int max_local_slot;       // 最大使用的局部变量槽位（包括临时槽位）
    int peak_local_slot;      // 真正的槽位峰值（不受 gen_assign 临时槽位级联影响）
} CodeGen;

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem);
void codegen(CodeGen* gen, Ast* ast);
void codegen_module(CodeGen* gen, Ast* ast);  // 模块代码生成
void codegen_set_func_dict(void* dict);  // 设置全局函数字典
void codegen_set_module(ObjModule* module);  // 设置当前模块

// 函数生成（供 codegen_stmt.c 使用）
ObjFunction* gen_func_proto(CodeGen* gen, Ast* ast);
void gen_func_closure(CodeGen* gen, Ast* ast, ObjFunction* func);

#endif // LENO_CODEGEN_H
