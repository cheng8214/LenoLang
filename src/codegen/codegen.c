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

// ============================================================================
// 槽位预扫描：求"顶层代码用到的最大局部变量槽位 + 1"
// ----------------------------------------------------------------------------
// 局部变量槽位号由语义分析分配（可复用、且可能远大于 0）。寄存器式下变量
// 直接用槽位号当寄存器号，临时寄存器必须从所有槽位之上开始分配 —— 否则
// 临时值会覆盖变量（表现为"变量莫名其妙变成别的值"）。
// AST_FUNC_DEF 不递归：函数体有独立的槽位空间（func->local_count）。
// ============================================================================

static void scan_slots_list(AstList* list, int* max_slot);

static void note_ref(SymRef* r, int* max_slot) {
    if (!r || !r->name) return;
    if ((r->kind == SYM_LOCAL || r->kind == SYM_PARAM) && r->index >= 0 && r->index + 1 > *max_slot) {
        *max_slot = r->index + 1;
    }
}

static void note_slot(int slot, int* max_slot) {
    if (slot >= 0 && slot + 1 > *max_slot) *max_slot = slot + 1;
}

static void scan_slots(Ast* ast, int* max_slot) {
    if (!ast) return;
    switch (ast->kind) {
        case AST_FUNC_DEF: return;   // 函数体独立生成
        case AST_VAR: note_ref(&ast->u.var.ref, max_slot); return;
        case AST_VAR_DECL:
            note_ref(&ast->u.var_decl.ref, max_slot);
            scan_slots(ast->u.var_decl.init, max_slot);
            return;
        case AST_BLOCK: scan_slots_list(&ast->u.block, max_slot); return;
        case AST_EXPR_STMT: scan_slots(ast->u.expr_stmt.expr, max_slot); return;
        case AST_BINOP:
            scan_slots(ast->u.binop.l, max_slot);
            scan_slots(ast->u.binop.r, max_slot);
            return;
        case AST_UNARY: scan_slots(ast->u.unary.operand, max_slot); return;
        case AST_CALL:
            scan_slots(ast->u.call.callee, max_slot);
            scan_slots_list(&ast->u.call.args, max_slot);
            return;
        case AST_INDEX:
            scan_slots(ast->u.index.obj, max_slot);
            scan_slots(ast->u.index.index, max_slot);
            return;
        case AST_SLICE:
            scan_slots(ast->u.slice.obj, max_slot);
            scan_slots(ast->u.slice.start, max_slot);
            scan_slots(ast->u.slice.end, max_slot);
            return;
        case AST_INDEX_ASSIGN:
            scan_slots(ast->u.index_assign.obj, max_slot);
            scan_slots(ast->u.index_assign.index, max_slot);
            scan_slots(ast->u.index_assign.value, max_slot);
            return;
        case AST_ASSIGN:
            for (int i = 0; i < ast->u.assign.name_count; i++) scan_slots(ast->u.assign.targets[i], max_slot);
            if (ast->u.assign.refs) {
                for (int i = 0; i < ast->u.assign.name_count; i++) note_ref(&ast->u.assign.refs[i], max_slot);
            }
            scan_slots(ast->u.assign.value, max_slot);
            return;
        case AST_COMPOUND_ASSIGN:
            note_ref(&ast->u.compound_assign.ref, max_slot);
            scan_slots(ast->u.compound_assign.value, max_slot);
            return;
        case AST_IF:
            scan_slots(ast->u.if_.cond, max_slot);
            scan_slots(ast->u.if_.then, max_slot);
            scan_slots(ast->u.if_.else_, max_slot);
            note_slot(ast->u.if_.guard_bind_index, max_slot);
            scan_slots(ast->u.if_.guard_bind_expr, max_slot);
            return;
        case AST_WHILE:
            scan_slots(ast->u.while_.cond, max_slot);
            scan_slots(ast->u.while_.body, max_slot);
            return;
        case AST_FOR:
            scan_slots(ast->u.for_.start, max_slot);
            scan_slots(ast->u.for_.end, max_slot);
            scan_slots(ast->u.for_.step, max_slot);
            note_slot(ast->u.for_.loop_var_index, max_slot);
            note_slot(ast->u.for_.end_index, max_slot);
            note_slot(ast->u.for_.step_index, max_slot);
            note_slot(ast->u.for_.start_index, max_slot);
            note_slot(ast->u.for_.counter_index, max_slot);
            note_slot(ast->u.for_.index_var_index, max_slot);
            scan_slots(ast->u.for_.body, max_slot);
            return;
        case AST_SWITCH:
            scan_slots(ast->u.switch_.expr, max_slot);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                scan_slots_list(&ast->u.switch_.cases[i].values, max_slot);
                scan_slots(ast->u.switch_.cases[i].body, max_slot);
                note_slot(ast->u.switch_.cases[i].guard_bind_index, max_slot);
                if (ast->u.switch_.cases[i].destructure_indices) {
                    for (int d = 0; d < ast->u.switch_.cases[i].destructure_count; d++) {
                        note_slot(ast->u.switch_.cases[i].destructure_indices[d], max_slot);
                    }
                }
            }
            scan_slots(ast->u.switch_.default_body, max_slot);
            return;
        case AST_RETURN: scan_slots(ast->u.ret, max_slot); return;
        case AST_RETURN_MULTI:
            for (int i = 0; i < ast->u.ret_multi.count; i++) scan_slots(ast->u.ret_multi.exprs[i], max_slot);
            return;
        case AST_THROW: scan_slots(ast->u.throw_.expr, max_slot); return;
        case AST_TRY:
            scan_slots(ast->u.try_.try_body, max_slot);
            scan_slots(ast->u.try_.catch_body, max_slot);
            scan_slots(ast->u.try_.finally_body, max_slot);
            note_ref(&ast->u.try_.catch_var_ref, max_slot);
            return;
        case AST_TYPE_CHECK:
        case AST_AS_CAST:
            scan_slots(ast->u.type_check.expr, max_slot);
            return;
        case AST_AWAIT: scan_slots(ast->u.await.expr, max_slot); return;
        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count; i++) scan_slots(ast->u.interp_string.exprs[i], max_slot);
            return;
        case AST_MODULE_CALL: scan_slots_list(&ast->u.module_call.args, max_slot); return;
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++) scan_slots(ast->u.array.items[i], max_slot);
            return;
        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                scan_slots(ast->u.dict.entries[i].key, max_slot);
                scan_slots(ast->u.dict.entries[i].value, max_slot);
            }
            return;
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++) scan_slots(ast->u.struct_init.field_values[i], max_slot);
            return;
        case AST_FIELD_ACCESS: scan_slots(ast->u.field_access.obj, max_slot); return;
        case AST_ADDRESS_OF: scan_slots(ast->u.address_of.operand, max_slot); return;
        case AST_SAFE_ACCESS:
            scan_slots(ast->u.safe_access.obj, max_slot);
            scan_slots_list(&ast->u.safe_access.args, max_slot);
            return;
        case AST_DESTRUCT_DECL:
            if (ast->u.destruct_decl.refs) {
                for (int i = 0; i < ast->u.destruct_decl.slot_count; i++) {
                    note_ref(&ast->u.destruct_decl.refs[i], max_slot);
                }
            }
            scan_slots(ast->u.destruct_decl.init, max_slot);
            return;
        case AST_EXPORT: scan_slots(ast->u.export.decl, max_slot); return;
        case AST_ALIAS: scan_slots(ast->u.alias.expr, max_slot); return;
        default: return;
    }
}

static void scan_slots_list(AstList* list, int* max_slot) {
    for (int i = 0; i < list->count; i++) scan_slots(list->items[i], max_slot);
}

void codegen(CodeGen* gen, Ast* ast) {
    if (!ast) return;

    const char* current_file = error_get_filename();
    if (current_file && gen->chunk) {
        gen->chunk->filename = strdup(current_file);
    }

    // 临时寄存器必须从所有局部变量槽位之上开始（否则会覆盖变量）
    {
        int max_slot = 0;
        scan_slots(ast, &max_slot);
        if (max_slot > gen->next_reg) gen->next_reg = max_slot;
        if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
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
