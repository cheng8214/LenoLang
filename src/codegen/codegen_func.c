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

// 统计函数体内所有 return 语句的返回值个数（含"必返"分析）。
// 返回值：
//   -2  函数体没有 return 语句（隐式返回 null，按 1 个返回值处理见调用处）
//   -1  各 return 的返回值个数不一致，无法静态确定
//   >=0 所有 return 一致的返回值个数
// *must（出参，可 NULL）：
//   1 = 该子树所有执行路径都终止于 return（不存在 fall-through 到函数尾的路径）
//   0 = 存在 fall-through 路径（if 无 else / 循环 0 次迭代 / switch 无匹配
//       case / try 异常路径等），函数可能隐式返回 1 个 null
//
// 必返分析的必要性：
//   func f(arr) { for a in arr { return 1, 2 } }
//   若只统计显式 return 会得 2，但循环 0 次迭代时实际隐式返回 1 个 null，
//   运行时存在两种返回个数。调用处必须结合 must 判定：
//   存在 fall-through 且显式个数 != 1 时，return_count 只能是 -1。
static int ast_return_count_in_block(AstList* list, int* found, int* must);
static int ast_return_count(Ast* ast, int* found, int* must);

static int ast_return_count_in_block(AstList* list, int* found, int* must) {
    int result = -2;  // -2 = 尚未遇到 return
    int all_must = 0; // 是否已遇到必返语句（其后语句不可达）
    for (int i = 0; i < list->count; i++) {
        int m = 0;
        int r = ast_return_count(list->items[i], found, &m);
        if (r == -1) {
            if (must) *must = all_must;  // 已不一致，must 已无意义，保持已扫描状态
            return -1;  // 内部已不一致，直接传播
        }
        if (r >= 0) {
            if (result == -2) result = r;
            else if (result != r) {
                if (must) *must = all_must;
                return -1;
            }
        }
        if (m) {
            // 该语句必然以 return 终止，其后语句运行时不可达，
            // 停止扫描（不可达代码里的 return 不影响运行时返回个数）
            all_must = 1;
            break;
        }
    }
    if (must) *must = all_must;
    return result;
}

static int ast_return_count(Ast* ast, int* found, int* must) {
    if (must) *must = 0;
    if (!ast) return -2;

    switch (ast->kind) {
        case AST_RETURN:
            *found = 1;
            if (must) *must = 1;
            return 1;
        case AST_RETURN_MULTI:
            *found = 1;
            if (must) *must = 1;
            return ast->u.ret_multi.count;
        case AST_BLOCK:
            return ast_return_count_in_block(&ast->u.block, found, must);
        case AST_IF: {
            int m1 = 0, m2 = 0;
            int r1 = ast_return_count(ast->u.if_.then, found, &m1);
            if (r1 == -1) return -1;
            int r2 = ast_return_count(ast->u.if_.else_, found, &m2);
            if (r2 == -1) return -1;
            if (r1 >= 0 && r2 >= 0 && r1 != r2) return -1;
            if (must) *must = m1 && m2;  // then/else 都必返才必返（无 else 时 m2=0）
            return (r1 >= 0) ? r1 : r2;
        }
        case AST_WHILE: {
            // 循环可能 0 次迭代，恒非必返；体内 return 个数仍参与统计
            int m = 0;
            int r = ast_return_count(ast->u.while_.body, found, &m);
            if (must) *must = 0;
            return r;
        }
        case AST_FOR: {
            // 同 AST_WHILE：0 次迭代路径存在，恒非必返
            int m = 0;
            int r = ast_return_count(ast->u.for_.body, found, &m);
            if (must) *must = 0;
            return r;
        }
        case AST_SWITCH: {
            // 所有 case 分支 + default 的 return 都参与统计；
            // 仅当有 default 且全部分支都必返时才必返（无 default 时
            // 可能无匹配 case 直接跳出 → fall-through）
            int result = -2;
            int all_must = (ast->u.switch_.default_body != NULL);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                int m = 0;
                int r = ast_return_count(ast->u.switch_.cases[i].body, found, &m);
                if (r == -1) {
                    if (must) *must = 0;
                    return -1;
                }
                if (r >= 0) {
                    if (result == -2) result = r;
                    else if (result != r) {
                        if (must) *must = 0;
                        return -1;
                    }
                }
                if (!m) all_must = 0;  // 任一分支非必返则整体非必返
            }
            if (ast->u.switch_.default_body != NULL) {
                int m = 0;
                int r = ast_return_count(ast->u.switch_.default_body, found, &m);
                if (r == -1) {
                    if (must) *must = 0;
                    return -1;
                }
                if (r >= 0) {
                    if (result == -2) result = r;
                    else if (result != r) {
                        if (must) *must = 0;
                        return -1;
                    }
                }
                if (!m) all_must = 0;
            }
            if (must) *must = all_must;
            return result;
        }
        case AST_TRY: {
            // try/catch/finally 三个块的 return 都参与统计；
            // 异常路径与 finally 覆盖语义复杂（finally 中 throw/return
            // 可覆盖 try 的返回值），保守视为非必返
            int result = -2;
            int m = 0;
            int r = ast_return_count(ast->u.try_.try_body, found, &m);
            if (r == -1) return -1;
            if (r >= 0) result = r;
            if (ast->u.try_.catch_body != NULL) {
                m = 0;
                r = ast_return_count(ast->u.try_.catch_body, found, &m);
                if (r == -1) return -1;
                if (r >= 0) {
                    if (result == -2) result = r;
                    else if (result != r) return -1;
                }
            }
            if (ast->u.try_.finally_body != NULL) {
                m = 0;
                r = ast_return_count(ast->u.try_.finally_body, found, &m);
                if (r == -1) return -1;
                if (r >= 0) {
                    if (result == -2) result = r;
                    else if (result != r) return -1;
                }
            }
            if (must) *must = 0;
            return result;
        }
        case AST_FUNC_DEF:
            // 嵌套函数的 return 不属于本函数
            return -2;
        default:
            return -2;
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
    func->param_generic_names = NULL;
    func->param_generic_count = 0;
    func->module = g_current_module;  // 设置函数所属模块
    func->type_param_count = 0;
    func->type_param_names = NULL;
    func->type_param_constraints = NULL;
    func->is_ctor = ast->u.func.is_ctor;
    func->return_types = NULL;

    // 统计返回值个数（编译期确定，供 JIT 等消费方使用）
    {
        int found = 0;
        int must = 0;
        int rc = ast_return_count(ast->u.func.body, &found, &must);
        if (rc == -1) {
            func->return_count = -1;   // 各 return 个数不一致，无法静态确定
        } else if (!found) {
            func->return_count = 1;    // 无 return 语句，隐式返回 null（1 个值）
        } else if (!must && rc != 1) {
            // 存在 fall-through 路径（if 无 else / 循环 0 次迭代 / switch
            // 无匹配 case / try 异常路径等），隐式返回 1 个 null 与显式
            // rc 个返回值在运行时并存，无法静态确定
            func->return_count = -1;
        } else {
            func->return_count = rc;   // 所有执行路径的返回值个数一致
        }
    } // end return_count

    // 从声明中提取返回值类型（供 JIT 内联使用）
    if (func->return_count > 0 && ast->u.func.return_type) {
        TypeInfo* rt = ast->u.func.return_type;
        func->return_types = (TypeKind*)malloc(sizeof(TypeKind) * func->return_count);
        if (func->return_types) {
            if (rt->kind == TYPE_MULTI_RET && rt->param_types && rt->param_count == func->return_count) {
                for (int i = 0; i < func->return_count; i++) {
                    func->return_types[i] = rt->param_types[i]->kind;
                }
            } else {
                // 单返回值：直接取 return_type 的 kind
                func->return_types[0] = rt->kind;
            }
        }
    }
    if (ast->u.func.type_param_count > 0 && ast->u.func.type_params) {
        func->type_param_count = ast->u.func.type_param_count;
        func->type_param_names = (char**)malloc(sizeof(char*) * ast->u.func.type_param_count);
        for (int i = 0; i < ast->u.func.type_param_count; i++) {
            func->type_param_names[i] = strdup(ast->u.func.type_params[i]);
        }
        // 存储约束信息
        if (ast->u.func.type_param_constraints) {
            func->type_param_constraints = (char**)malloc(sizeof(char*) * ast->u.func.type_param_count);
            for (int i = 0; i < ast->u.func.type_param_count; i++) {
                func->type_param_constraints[i] = ast->u.func.type_param_constraints[i]
                    ? strdup(ast->u.func.type_param_constraints[i]) : NULL;
            }
        }
    }
    
    // 存储参数类型用于运行时类型检查
    if (func->arity > 0 && ast->u.func.param_types) {
        func->param_types = (TypeKind*)malloc(sizeof(TypeKind) * func->arity);
        if (func->param_types) {
            for (int i = 0; i < func->arity; i++) {
                func->param_types[i] = ast->u.func.param_types[i]->kind;
            }
        }
        // 存储泛型参数名（如 T, K, V），用于运行时泛型方法参数类型检查
        int generic_count = 0;
        for (int i = 0; i < func->arity; i++) {
            if (ast->u.func.param_types[i]->kind == TYPE_GENERIC_PARAM && ast->u.func.param_types[i]->type_param_name) {
                generic_count++;
            }
        }
        if (generic_count > 0) {
            func->param_generic_names = (char**)malloc(sizeof(char*) * func->arity);
            func->param_generic_count = generic_count;
            for (int i = 0; i < func->arity; i++) {
                if (ast->u.func.param_types[i]->kind == TYPE_GENERIC_PARAM && ast->u.func.param_types[i]->type_param_name) {
                    func->param_generic_names[i] = strdup(ast->u.func.param_types[i]->type_param_name);
                } else {
                    func->param_generic_names[i] = NULL;
                }
            }
        }
    }
    
    chunk_init(func->chunk);

    Chunk* prev_chunk = gen->chunk;
    ObjFunction* prev_func = gen->current_func;
    int prev_max_slot = gen->max_local_slot;
    int prev_peak_slot = gen->peak_local_slot;
    int prev_dtor_count = gen->dtor_count;
    int prev_dtor_temp_slot = gen->dtor_temp_slot;

    if (prev_chunk && prev_chunk->filename) {
        func->chunk->filename = strdup(prev_chunk->filename);
    }

    gen->chunk = func->chunk;
    gen->current_func = func;
    gen->max_local_slot = func->local_count - 1;  // 初始最大槽位是当前局部变量数-1
    gen->peak_local_slot = func->local_count - 1; // 峰值初始为声明变量数-1
    gen->dtor_count = 0;          // 每个函数独立的析构追踪
    gen->dtor_temp_slot = -1;     // 重置临时槽位

    gen->scope_depth++;

    // 将函数名压入内联检测栈，防止函数体内的递归调用被内联
    inline_name_stack_push(ast->u.func.name);

    // 局部变量由 var_decl 负责初始化，不需要预先生成 null
    // 这样可以避免冗余的 null 赋值（如 var n = arr.len() 时）

    gen_block(gen, ast->u.func.body);
    gen->scope_depth--;

    inline_name_stack_pop();

    // 更新函数的 local_count：使用峰值为准（覆盖 gen_assign 临时槽位需求）
    // peak_local_slot 避免了 gen_assign 级联膨胀问题，只记录真实需要的最大槽位
    int effective_max = gen->max_local_slot;
    if (gen->peak_local_slot > effective_max) {
        effective_max = gen->peak_local_slot;
    }
    if (effective_max >= func->local_count) {
        func->local_count = effective_max + 1;
    }

    emit_byte(gen, OP_NULL, ast->line);
    emit_byte(gen, OP_RETURN, ast->line);

    gen->chunk = prev_chunk;
    gen->current_func = prev_func;
    gen->max_local_slot = prev_max_slot;
    gen->peak_local_slot = prev_peak_slot;
    gen->dtor_count = prev_dtor_count;
    gen->dtor_temp_slot = prev_dtor_temp_slot;

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
            emit_bytes_2(gen, OP_SET_LOCAL_POP, ast->u.func.ref.index, ast->line);
        }
    }
}

void gen_func(CodeGen* gen, Ast* ast) {
    ObjFunction* func = gen_func_proto(gen, ast);
    gen_func_closure(gen, ast, func);
}
