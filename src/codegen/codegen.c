#include "codegen.h"

void codegen_init(CodeGen* gen, Chunk* chunk, Semantic* sem) {
    gen->chunk = chunk;
    gen->sem = sem;
    gen->scope_depth = 0;
    gen->loop_head = NULL;
    gen->loop_count = 0;
    gen->current_func = NULL;
    gen->max_local_slot = -1;
    gen->peak_local_slot = -1;
}

void codegen_cleanup(CodeGen* gen) {
    while (gen->loop_head) {
        LoopContextNode* node = gen->loop_head;
        gen->loop_head = node->prev;
        free(node);
    }
    gen->loop_count = 0;
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

    MainFuncInfo main_info = find_main_function(gen->sem);
    if (main_info.has_main) {
        emit_get_global_func(gen, main_info.main_index, ast->line);
        emit_call(gen, 0, ast->line);
        emit_byte(gen, OP_POP, ast->line);
    }
}

// 模块代码生成
// 模块代码使用模块操作码（OP_GET_MODULE_VAR, OP_SET_MODULE_VAR, OP_GET_MODULE_FUNC, OP_DEFINE_MODULE_FUNC）
void codegen_module(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    // 生成模块代码
    if (ast->kind == AST_BLOCK) {
        gen_block_module(gen, ast);
    } else {
        gen_stmt_module(gen, ast);
    }
}
