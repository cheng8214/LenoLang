#include "codegen.h"

// 当前模块（用于设置函数所属模块）
static ObjModule* g_current_module = NULL;

// 设置当前模块
void codegen_set_module(ObjModule* module) {
    g_current_module = module;
}

// 前向声明
static int ast_has_try(Ast* ast);

// 检查 AST 列表中是否包含 try 语句
static int ast_list_has_try(AstList* list) {
    for (int i = 0; i < list->count; i++) {
        if (ast_has_try(list->items[i])) {
            return 1;
        }
    }
    return 0;
}

// 递归检查 AST 中是否包含 try 语句
static int ast_has_try(Ast* ast) {
    if (!ast) return 0;
    
    switch (ast->kind) {
        case AST_TRY:
            return 1;
        case AST_BLOCK:
            return ast_list_has_try(&ast->u.block);
        case AST_IF:
            return ast_has_try(ast->u.if_.then) || 
                   ast_has_try(ast->u.if_.else_);
        case AST_WHILE:
            return ast_has_try(ast->u.while_.body);
        case AST_FOR:
            return ast_has_try(ast->u.for_.body);
        case AST_FUNC_DEF:
            // 不检查嵌套函数，因为每个函数有自己的 has_try 标记
            return 0;
        case AST_CLIB_DEF:
        default:
            return 0;
    }
}

// 生成函数原型（函数对象），但不创建闭包
// 返回函数对象，供后续创建闭包使用
ObjFunction* gen_func_proto(CodeGen* gen, Ast* ast) {
    ObjFunction* func = (ObjFunction*)gc_alloc(sizeof(ObjFunction), OBJ_FUNCTION);
    if (!func) return NULL;

    func->arity = ast->u.func.pcnt;
    func->name = strdup(ast->u.func.name);
    func->chunk = (Chunk*)malloc(sizeof(Chunk));
    func->upvalue_count = ast->u.func.upvalue_count;
    func->local_count = ast->u.func.local_count;
    func->has_try = ast_has_try(ast->u.func.body);  // 检测函数体是否包含 try
    func->param_types = NULL;
    func->module = g_current_module;  // 设置函数所属模块
    
    // 存储参数类型用于运行时类型检查
    if (func->arity > 0 && ast->u.func.param_types) {
        func->param_types = (TypeKind*)malloc(sizeof(TypeKind) * func->arity);
        if (func->param_types) {
            for (int i = 0; i < func->arity; i++) {
                func->param_types[i] = ast->u.func.param_types[i]->kind;
            }
        }
    }
    
    chunk_init(func->chunk);

    Chunk* prev_chunk = gen->chunk;
    ObjFunction* prev_func = gen->current_func;
    int prev_max_slot = gen->max_local_slot;

    if (prev_chunk && prev_chunk->filename) {
        func->chunk->filename = strdup(prev_chunk->filename);
    }

    gen->chunk = func->chunk;
    gen->current_func = func;
    gen->max_local_slot = func->local_count - 1;  // 初始最大槽位是当前局部变量数-1

    gen->scope_depth++;

    // 局部变量由 var_decl 负责初始化，不需要预先生成 null
    // 这样可以避免冗余的 null 赋值（如 var n = arr.len() 时）

    gen_block(gen, ast->u.func.body);
    gen->scope_depth--;

    // 更新函数的 local_count 以包含临时槽位
    if (gen->max_local_slot >= func->local_count) {
        func->local_count = gen->max_local_slot + 1;
    }

    emit_byte(gen, OP_NULL, ast->line);
    emit_byte(gen, OP_RETURN, ast->line);

    gen->chunk = prev_chunk;
    gen->current_func = prev_func;
    gen->max_local_slot = prev_max_slot;

    return func;
}

// 创建闭包并存储到函数槽位
void gen_func_closure(CodeGen* gen, Ast* ast, ObjFunction* func) {
    if (!func) return;

    emit_closure(gen, make_constant(gen, val_obj((Object*)func)), ast->line);

    for (int i = 0; i < ast->u.func.upvalue_count; i++) {
        uint16_t is_local = (uint16_t)ast->u.func.upvalue_is_local[i];
        uint16_t index = (uint16_t)ast->u.func.upvalue_indices[i];
        uint16_t is_value_capture = (uint16_t)ast->u.func.upvalue_is_value_capture[i];
        emit_byte(gen, (is_local >> 8) & 0xff, ast->line);
        emit_byte(gen, is_local & 0xff, ast->line);
        emit_byte(gen, (index >> 8) & 0xff, ast->line);
        emit_byte(gen, index & 0xff, ast->line);
        emit_byte(gen, (is_value_capture >> 8) & 0xff, ast->line);
        emit_byte(gen, is_value_capture & 0xff, ast->line);
    }

    if (ast->u.func.ref.name) {
        if (ast->u.func.ref.kind == SYM_GLOBAL_FUNC) {
            emit_define_global_func(gen, ast->u.func.ref.index, ast->line);
        } else if (ast->u.func.ref.kind == SYM_LOCAL) {
            emit_bytes_2(gen, OP_SET_LOCAL, ast->u.func.ref.index, ast->line);
            emit_byte(gen, OP_POP, ast->line);
        }
    }
}

void gen_func(CodeGen* gen, Ast* ast) {
    ObjFunction* func = gen_func_proto(gen, ast);
    gen_func_closure(gen, ast, func);
}
