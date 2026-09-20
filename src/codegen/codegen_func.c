// ============================================================================
// 寄存器式 codegen：函数生成
// ============================================================================

#include "codegen.h"

// 当前模块（用于设置函数所属模块）
static ObjModule* g_current_module = NULL;

void codegen_set_module(ObjModule* module) {
    g_current_module = module;
}

// 设置函数字典（兼容旧 API）
static void* g_func_dict = NULL;
void codegen_set_func_dict(void* dict) {
    g_func_dict = dict;
}

// 前向声明
static int ast_has_try(Ast* ast);
static int ast_list_has_try(AstList* list);
static int ast_return_count(Ast* ast, int* found, int* must);
static int ast_list_return_count(AstList* list, int* found, int* must);

static int ast_list_has_try(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        if (ast_has_try(list->items[i])) return 1;
    }
    return 0;
}

static int ast_has_try(Ast* ast) {
    if (!ast) return 0;
    switch (ast->kind) {
        case AST_TRY: return 1;
        case AST_BLOCK: return ast_list_has_try(&ast->u.block);
        case AST_IF: return ast_has_try(ast->u.if_.then) || ast_has_try(ast->u.if_.else_);
        case AST_WHILE: return ast_has_try(ast->u.while_.body);
        case AST_FOR: return ast_has_try(ast->u.for_.body);
        default: return 0;
    }
}

static int ast_list_return_count(AstList* list, int* found, int* must) {
    for (int i = 0; i < list->count; i++) {
        ast_return_count(list->items[i], found, must);
    }
    return 0;
}

static int ast_return_count(Ast* ast, int* found, int* must) {
    if (!ast) return 0;
    switch (ast->kind) {
        case AST_RETURN:
            (*found)++;
            return 1;
        case AST_RETURN_MULTI:
            *must = 1;
            (*found)++;
            return 1;
        case AST_BLOCK:
            return ast_list_return_count(&ast->u.block, found, must);
        case AST_IF:
            ast_return_count(ast->u.if_.then, found, must);
            ast_return_count(ast->u.if_.else_, found, must);
            return 0;
        case AST_WHILE:
            ast_return_count(ast->u.while_.body, found, must);
            return 0;
        case AST_FOR:
            ast_return_count(ast->u.for_.body, found, must);
            return 0;
        default: return 0;
    }
}

// ============================================================================
// 函数原型生成
// ============================================================================

ObjFunction* gen_func_proto(CodeGen* gen, Ast* ast) {
    // 创建函数对象（栈式基线做法：直接 gc_alloc + 单独 malloc Chunk）
    ObjFunction* func = (ObjFunction*)gc_alloc(sizeof(ObjFunction), OBJ_FUNCTION);
    if (!func) return NULL;

    func->arity = ast->u.func.pcnt;
    func->name = strdup(ast->u.func.name);
    func->chunk = (Chunk*)malloc(sizeof(Chunk));
    if (func->chunk) chunk_init(func->chunk);
    func->upvalue_count = ast->u.func.upvalue_count;
    func->local_count = ast->u.func.local_count;
    func->has_try = ast_has_try(ast->u.func.body);
    func->param_types = NULL;
    func->param_generic_names = NULL;
    func->param_generic_count = 0;
    func->module = g_current_module;
    func->type_param_count = ast->u.func.type_param_count;
    func->type_param_names = NULL;
    func->type_param_constraints = NULL;
    func->is_ctor = ast->u.func.is_ctor;
    func->return_count = 0;
    func->return_types = NULL;

    // 设置参数类型
    if (ast->u.func.param_types) {
        func->param_types = (TypeKind*)malloc(sizeof(TypeKind) * ast->u.func.pcnt);
        for (int i = 0; i < ast->u.func.pcnt; i++) {
            func->param_types[i] = ast->u.func.param_types[i]->kind;
        }
    }

    // 检查返回值数量（简化：统计 return 个数，无法静态确定则置 -1）
    // TODO: 完整 ast_return_count 多路径分析（参考栈式 codegen_func.c）
    int found = 0, must = 0;
    ast_return_count(ast->u.func.body, &found, &must);
    if (must) {
        func->return_count = 1;
    } else if (found > 0) {
        func->return_count = 1;
    } else {
        func->return_count = 0;
    }

    // TODO: upvalue_indices / upvalue_is_local 字段在寄存器式 ObjFunction 中不存在
    // 寄存器式闭包捕获改用 OP_CLOSURE + OP_GETUPVAL/SETUPVAL，捕获信息由 codegen 阶段
    // 在 OP_CLOSURE 后续字节编码（参考栈式 OP_CLOSURE 实现）

    return func;
}

// ============================================================================
// 函数闭包生成
// ============================================================================

void gen_func_closure(CodeGen* gen, Ast* ast, ObjFunction* func) {
    // 生成函数体字节码到 func->chunk
    Chunk* saved_chunk = gen->chunk;
    gen->chunk = func->chunk;  // func->chunk 已是 Chunk*（malloc 出的实例）

    // 保存当前函数
    ObjFunction* saved_func = gen->current_func;
    gen->current_func = func;

    // ★ 必须保存/恢复寄存器分配器状态：函数体是独立寄存器空间，
    //   若把内层的 next_reg（往往很小）留在 gen 上，外层后续的临时寄存器
    //   会分配进变量槽位（表现为"嵌套函数定义后，外层变量被闭包覆盖"）。
    int saved_next_reg = gen->next_reg;
    int saved_max_reg = gen->max_reg;
    int saved_freetop = gen->freetop;
    int saved_scope_base = gen->scope_base;

    // 重置寄存器分配器。
    // 关键：局部变量（参数 + 声明变量）的槽位号由语义分析分配，可能远大于 arity；
    // 临时寄存器必须从"所有槽位之上"开始，否则会覆盖变量（静默错值）。
    int base_reg = func->arity;
    if (func->local_count > base_reg) base_reg = func->local_count;
    gen->next_reg = base_reg;
    gen->max_reg = base_reg;
    gen->freetop = 0;
    gen->scope_base = base_reg;

    // 生成函数体
    if (ast->u.func.body) {
        gen_stmt(gen, ast->u.func.body);
    }

    // 如果函数没有显式 return，补一个
    int r = reg_alloc(gen);
    emit_loadnil_to(gen, r, ast->line);
    emit_return(gen, r, 1, ast->line);
    reg_free(gen, r);

    // 寄存器高水位写回 local_count
    func->local_count = gen->max_reg;

    // 恢复（含寄存器分配器状态）
    gen->chunk = saved_chunk;
    gen->current_func = saved_func;
    gen->next_reg = saved_next_reg;
    gen->max_reg = saved_max_reg;
    gen->freetop = saved_freetop;
    gen->scope_base = saved_scope_base;
}

// ============================================================================
// 函数定义语句
// ============================================================================

// 发射 OP_CLOSURE 及其捕获描述（紧随指令的非指令数据，每条 3 字节）：
//   [is_local:u8][index:u8][is_value_capture:u8] × upvalue_count
void emit_closure_upvals(CodeGen* gen, int dst, int const_idx, Ast* ast) {
    reg_encode_iABx(gen->chunk, OP_CLOSURE, dst, const_idx, ast->line);
    int n = ast->u.func.upvalue_count;
    for (int i = 0; i < n; i++) {
        int is_local = (ast->u.func.upvalue_is_local && ast->u.func.upvalue_is_local[i]) ? 1 : 0;
        int index = (ast->u.func.upvalue_indices && ast->u.func.upvalue_indices[i] >= 0)
                        ? ast->u.func.upvalue_indices[i] : 0;
        int is_value_capture = (ast->u.func.upvalue_is_value_capture && ast->u.func.upvalue_is_value_capture[i]) ? 1 : 0;
        chunk_write(gen->chunk, (uint8_t)(is_local & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(index & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(is_value_capture & 0xFF), ast->line);
    }
}

void gen_func(CodeGen* gen, Ast* ast) {
    // 1. 生成函数原型
    ObjFunction* func = gen_func_proto(gen, ast);

    // 2. 生成函数体
    gen_func_closure(gen, ast, func);

    // 3. 绑定到符号槽位
    SymRef* ref = &ast->u.func.ref;
    int const_idx = make_constant(gen, val_obj((Object*)func));

    if (ref->kind == SYM_GLOBAL_FUNC) {
        int r = reg_alloc(gen);
        emit_closure_upvals(gen, r, const_idx, ast);
        emit_defglobalfunc(gen, r, ref->index, ast->line);
        reg_free(gen, r);
    } else if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
        // 局部（嵌套）函数：函数值写到该变量自己的寄存器
        int dst = ref->index;
        if (dst >= gen->next_reg) {
            gen->next_reg = dst + 1;
            if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
        }
        emit_closure_upvals(gen, dst, const_idx, ast);
    } else if (ref->kind == SYM_GLOBAL) {
        int r = reg_alloc(gen);
        emit_closure_upvals(gen, r, const_idx, ast);
        emit_defglobal(gen, r, ref->index, ast->line);
        reg_free(gen, r);
    }
    // 其他（模块函数等）后续阶段处理
}
