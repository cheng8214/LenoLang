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

    // 重置寄存器分配器：参数占用 R0..arity-1
    gen->next_reg = func->arity;
    gen->max_reg = func->arity;
    gen->freetop = 0;

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

    // 恢复
    gen->chunk = saved_chunk;
    gen->current_func = saved_func;
}

// ============================================================================
// 函数定义语句
// ============================================================================

void gen_func(CodeGen* gen, Ast* ast) {
    // 1. 生成函数原型
    ObjFunction* func = gen_func_proto(gen, ast);

    // 2. 生成函数体
    gen_func_closure(gen, ast, func);

    // 3. 定义到全局函数表
    Symbol* sym = scope_resolve(gen->sem->current, ast->u.func.name);
    if (sym && sym->kind == SYM_GLOBAL_FUNC) {
        // GETGLOBALFUNC → 定义
        int r = reg_alloc(gen);
        // 把函数对象存入常量表，然后 DEFINERLOBALFUNC
        int const_idx = make_constant(gen, val_obj((Object*)func));
        emit_loadk_to(gen, r, const_idx, ast->line);
        emit_defglobalfunc(gen, r, sym->index, ast->line);
        reg_free(gen, r);
    }
}
