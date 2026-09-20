// ============================================================================
// 寄存器式 codegen：初始化 / 清理 / 顶层入口
// ============================================================================

#include "codegen.h"

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem) {
    gen->chunk = chunk;
    gen->sem = sem;
    gen->scope_depth = 0;
    gen->loop_head = NULL;
    gen->loop_count = 0;
    gen->current_func = NULL;
    // 寄存器分配器初始化
    gen->next_reg = 0;
    gen->max_reg = 0;
    gen->freetop = 0;
    gen->scope_base = 0;
    gen->dtor_entries = NULL;
    gen->dtor_count = 0;
    gen->dtor_capacity = 0;
    gen->dtor_temp_slot = -1;
    gen->suppress_multi_pop = 0;
}

void codegen_cleanup(CodeGen* gen) {
    while (gen->loop_head) {
        LoopContextNode* node = gen->loop_head;
        gen->loop_head = node->prev;
        free(node);
    }
    gen->loop_count = 0;
    if (gen->dtor_entries) {
        free(gen->dtor_entries);
        gen->dtor_entries = NULL;
    }
    gen->dtor_count = 0;
    gen->dtor_capacity = 0;
}

void codegen_add_dtor_entry(CodeGen* gen, int local_slot) {
    if (gen->dtor_count >= gen->dtor_capacity) {
        int new_cap = gen->dtor_capacity == 0 ? 8 : gen->dtor_capacity * 2;
        gen->dtor_entries = (DtorEntry*)realloc(gen->dtor_entries, sizeof(DtorEntry) * new_cap);
        gen->dtor_capacity = new_cap;
    }
    gen->dtor_entries[gen->dtor_count].local_slot = local_slot;
    gen->dtor_count++;
}

void codegen(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    if (ast->kind == AST_BLOCK) {
        gen_block(gen, ast);
    } else {
        gen_stmt(gen, ast);
    }

    // 顶层 main 函数调用
    MainFuncInfo main_info = find_main_function(gen->sem);
    if (main_info.has_main) {
        // GETGLOBALFUNC R0, main_index
        // CALL R0, 0 args, 1 result
        // RETURN R0, 1 result
        int r = reg_alloc(gen);
        emit_getglobalfunc_to(gen, r, main_info.main_index, ast->line);
        emit_call(gen, r, 0, 1, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    } else {
        // 无 main：发 RETURN NIL
        int r = reg_alloc(gen);
        emit_loadnil_to(gen, r, ast->line);
        emit_return(gen, r, 1, ast->line);
        reg_free(gen, r);
    }

    // 寄存器高水位写回 chunk->local_count（GC 依赖）
    if (gen->current_func) {
        gen->current_func->local_count = gen->max_reg;
    }
    gen->chunk->local_count = gen->max_reg;
}

// 模块代码生成
void codegen_module(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    if (ast->kind == AST_BLOCK) {
        gen_block_module(gen, ast);
    } else {
        gen_stmt_module(gen, ast);
    }

    gen->chunk->local_count = gen->max_reg;
}
