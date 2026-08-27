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
    gen->dtor_entries = NULL;
    gen->dtor_count = 0;
    gen->dtor_capacity = 0;
    gen->dtor_temp_slot = -1;
gen->inline_depth = 0;
gen->inline_result_slot = -1;
gen->inline_return_jump_count = 0;
gen->inline_discard_result = 0;
gen->inline_no_result = 0;
gen->suppress_multi_pop = 0;
// 重置内联函数名栈，防止上次编译残留状态影响本次编译
inline_name_stack_reset();
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

    MainFuncInfo main_info = find_main_function(gen->sem);
    if (main_info.has_main) {
        emit_get_global_func(gen, main_info.main_index, ast->line);
        emit_call(gen, 0, ast->line);
        // main 的返回值作为进程退出码：用 OP_RETURN 结束顶层帧，
        // 返回值会存入 vm.last_return_value，由 vm_get_exit_code() 读取
        emit_byte(gen, OP_RETURN, ast->line);
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
