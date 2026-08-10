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
    int local_slot;    // 局部变量槽位索引
} DtorEntry;

// ============================================================================
// 字节码生成器
// ============================================================================

typedef struct {
    Chunk* chunk;
    Semantic* sem;
    int scope_depth;
    LoopContextNode* loop_head;    // 链表头（当前最内层循环），使用链式堆分配避免栈溢出
    int loop_count;                 // 循环嵌套深度
    ObjFunction* current_func; // 当前正在生成的函数（用于更新 local_count）
    int max_local_slot;       // 最大使用的局部变量槽位（包括临时槽位）
    int peak_local_slot;      // 真正的槽位峰值（不受 gen_assign 临时槽位级联影响）
    DtorEntry* dtor_entries;  // 需要析构的局部变量数组
    int dtor_count;           // 当前条目数
    int dtor_capacity;        // 数组容量
    int dtor_temp_slot;       // return 时保存返回值的临时槽位（-1=未分配）
    // 函数内联状态
    int inline_depth;                 // 当前内联嵌套深度（0=不在内联中）
    int inline_result_slot;           // 内联函数返回值存放槽位
    int inline_return_jumps[256];     // 内联函数中 return 语句的跳转位置
    int inline_return_jump_count;     // 待回填的 return 跳转数量
    int inline_discard_result;        // 1=当前调用结果将被丢弃（表达式语句）
    int inline_no_result;             // 1=内联未在栈上留下值（void函数+discard）
} CodeGen;

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem);
void codegen_cleanup(CodeGen* gen);  // 释放循环上下文链表
void codegen(CodeGen* gen, Ast* ast);
void codegen_module(CodeGen* gen, Ast* ast);  // 模块代码生成
void codegen_set_func_dict(void* dict);  // 设置全局函数字典
void codegen_set_module(ObjModule* module);  // 设置当前模块
void codegen_add_dtor_entry(CodeGen* gen, int local_slot);  // 添加析构追踪条目

// 函数生成（供 codegen_stmt.c 使用）
ObjFunction* gen_func_proto(CodeGen* gen, Ast* ast);
void gen_func_closure(CodeGen* gen, Ast* ast, ObjFunction* func);

#endif // LENO_CODEGEN_H
