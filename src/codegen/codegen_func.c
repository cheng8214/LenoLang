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
    // ★ async 标记必须落到**函数对象**上（运行期要用，见 ObjFunction.is_async 的注释）：
    //   `var f = some_async; f()` / 把 async 函数当参数传 / 绑定方法 这些写法
    //   静态判不出 async ⇒ 由 VM 在 OP_CALL 里按这个标记建协程。
    func->is_async = ast->u.func.is_async;
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
    // ★ 函数 chunk 继承**所属文件**名（照栈式 gen_func 的实现）：
    //   运行期异常 dict 的 `file`、调用栈里的 `func (file:line)` 都取
    //   `frame->chunk->filename` —— 不继承的话每个函数 chunk 的 filename 都是 NULL，
    //   报错与栈追溯只剩一个光秃秃的 `main (:16)`（examples/try catch/* 一整批差异）。
    if (saved_chunk && saved_chunk->filename && !func->chunk->filename) {
        func->chunk->filename = strdup(saved_chunk->filename);
    }
    gen->chunk = func->chunk;  // func->chunk 已是 Chunk*（malloc 出的实例）

    // 保存当前函数
    ObjFunction* saved_func = gen->current_func;
    gen->current_func = func;
    // ★ 还要记住当前函数的 **AST**：gen_return 靠它取声明返回类型做规范化（C1）。
    //   嵌套函数（局部函数 / 方法）会层层覆盖，所以必须保存/恢复。
    Ast* saved_func_ast = gen->current_func_ast;
    gen->current_func_ast = ast;

    // ★ 必须保存/恢复寄存器分配器状态：函数体是独立寄存器空间。
    //   注意 free 栈要连**内容**一起保存 —— 只恢复 freetop 计数是不够的：
    //   内层函数会把 free_regs[0..] 覆盖成自己的小号码，外层恢复计数后就会
    //   拿到那些号码（常常正是 0/1 之类的变量槽位），把变量静默覆盖掉。
    int saved_next_reg = gen->next_reg;
    int saved_max_reg = gen->max_reg;
    int saved_freetop = gen->freetop;
    int saved_scope_base = gen->scope_base;
    // 析构条目也是**每函数独立**的：外层函数的条目不能泄漏进本函数
    // （否则 return 时会对外层变量发析构指令，槽位号还会撞车）
    int saved_dtor_count = gen->dtor_count;
    gen->dtor_count = 0;
    int saved_free_regs[MAX_REG];
    if (saved_freetop > 0) {
        memcpy(saved_free_regs, gen->free_regs, sizeof(int) * (size_t)saved_freetop);
    }

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
    //   ★ 不再发 LOADNIL：OP_RETURN 在 nresults == 0（B 字段 = 1）时就返回 null，
    //     与原「LOADNIL + RETURN(nresults=1)」**结果完全相同**（两者都让调用方拿到 null），
    //     但空体函数从 2 条指令降到 1 条 —— 空函数调用基准里 callee 体就是这一条。
    //   ⚠ 仍保留 reg_alloc/reg_free：max_reg（→ local_count → locals 数组大小、GC 扫描范围）
    //     的记账必须与改动前逐位一致，不能顺手把槽位也省掉。
    //   ⚠ A 字段此时指向一个**未初始化**的寄存器，但 nresults==0 ⇒ VM 根本不读它
    //     （OP_RETURN: `Value result = (nresults >= 1) ? R(a) : val_null();`）。
    int r = reg_alloc(gen);
    emit_return(gen, r, 0, ast->line);
    reg_free(gen, r);

    // 寄存器高水位写回 local_count
    func->local_count = gen->max_reg;

    // 恢复（含寄存器分配器状态与 free 栈内容）
    gen->chunk = saved_chunk;
    gen->current_func = saved_func;
    gen->current_func_ast = saved_func_ast;
    gen->next_reg = saved_next_reg;
    gen->max_reg = saved_max_reg;
    gen->freetop = saved_freetop;
    gen->scope_base = saved_scope_base;
    gen->dtor_count = saved_dtor_count;
    if (saved_freetop > 0) {
        memcpy(gen->free_regs, saved_free_regs, sizeof(int) * (size_t)saved_freetop);
    }
}

// ============================================================================
// 函数定义语句
// ============================================================================

// 发射 OP_CLOSURE 及其捕获描述（紧随指令的非指令数据，每条 **4 字节**，保持
// 4 字节指令网格不变形 —— 否则后续指令偏移全错，反汇编与跳转调试都会失真）：
//   [is_local:u8][index:u8][is_value_capture:u8][pad:u8] × upvalue_count
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
        chunk_write(gen->chunk, 0, ast->line);
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
    } else if (ref->kind == SYM_MODULE) {
        // 模块内的函数：写进 module->globals[ref->index]
        int r = reg_alloc(gen);
        emit_closure_upvals(gen, r, const_idx, ast);
        // 编码必须与 VM 的 READ_Bx() 一致（模块槽位是 16 位 Bx，不是 8 位 B）
        reg_encode_iABx(gen->chunk, OP_DEFINE_MODULE_FUNC, r, ref->index, ast->line);
        reg_free(gen, r);
    }
}
