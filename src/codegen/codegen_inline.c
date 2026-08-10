// ============================================================================
// 函数内联优化 (codegen_inline.c)
// 在编译期将小函数体直接嵌入调用点，消除 call()/OP_RETURN 帧开销
// ============================================================================

#include "codegen.h"
#include "include/leno_error.h"
#include <string.h>

#define MAX_INLINE_DEPTH  3
#define MAX_INLINE_STMTS  3

// 正在被内联的函数名栈（防止递归内联）
static char* inline_name_stack[MAX_INLINE_DEPTH];
static int inline_name_stack_top = 0;

// ============================================================================
// AST 遍历：为所有 SYM_LOCAL/SYM_PARAM 的 index 添加 offset
// ============================================================================

static void patch_symref(SymRef* ref, int offset) {
    if (ref && ref->name &&
        (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM)) {
        ref->index += offset;
    }
}

static void patch_ast_indices(Ast* ast, int offset) {
    if (!ast) return;

    switch (ast->kind) {
        // 叶子节点
        case AST_NUM:
        case AST_STRING:
        case AST_BOOL:
        case AST_NULL:
            return;

        case AST_VAR:
            patch_symref(&ast->u.var.ref, offset);
            return;

        case AST_BINOP:
            patch_ast_indices(ast->u.binop.l, offset);
            patch_ast_indices(ast->u.binop.r, offset);
            return;

        case AST_UNARY:
            patch_ast_indices(ast->u.unary.operand, offset);
            return;

        case AST_CALL:
            patch_ast_indices(ast->u.call.callee, offset);
            for (int i = 0; i < ast->u.call.args.count; i++)
                patch_ast_indices(ast->u.call.args.items[i], offset);
            return;

        case AST_INDEX:
            patch_ast_indices(ast->u.index.obj, offset);
            patch_ast_indices(ast->u.index.index, offset);
            return;

        case AST_SLICE:
            patch_ast_indices(ast->u.slice.obj, offset);
            patch_ast_indices(ast->u.slice.start, offset);
            patch_ast_indices(ast->u.slice.end, offset);
            return;

        case AST_INDEX_ASSIGN:
            patch_ast_indices(ast->u.index_assign.obj, offset);
            patch_ast_indices(ast->u.index_assign.index, offset);
            patch_ast_indices(ast->u.index_assign.value, offset);
            return;

        case AST_FIELD_ACCESS:
            patch_ast_indices(ast->u.field_access.obj, offset);
            return;

        case AST_SAFE_ACCESS:
            patch_symref(&ast->u.safe_access.ref, offset);
            patch_ast_indices(ast->u.safe_access.obj, offset);
            for (int i = 0; i < ast->u.safe_access.args.count; i++)
                patch_ast_indices(ast->u.safe_access.args.items[i], offset);
            return;

        case AST_INTERP_STRING:
            for (int i = 0; i < ast->u.interp_string.count; i++)
                patch_ast_indices(ast->u.interp_string.exprs[i], offset);
            return;

        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++)
                patch_ast_indices(ast->u.array.items[i], offset);
            return;

        case AST_DICT:
            for (int i = 0; i < ast->u.dict.count; i++) {
                patch_ast_indices(ast->u.dict.entries[i].key, offset);
                patch_ast_indices(ast->u.dict.entries[i].value, offset);
            }
            return;

        case AST_RANGE:
            patch_ast_indices(ast->u.range.start, offset);
            patch_ast_indices(ast->u.range.end, offset);
            return;

        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++)
                patch_ast_indices(ast->u.struct_init.field_values[i], offset);
            return;

        // 语句节点
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.count; i++)
                patch_ast_indices(ast->u.block.items[i], offset);
            return;

        case AST_EXPR_STMT:
            patch_ast_indices(ast->u.expr_stmt.expr, offset);
            return;

        case AST_RETURN:
            patch_ast_indices(ast->u.ret, offset);
            return;

        case AST_VAR_DECL:
            patch_symref(&ast->u.var_decl.ref, offset);
            patch_ast_indices(ast->u.var_decl.init, offset);
            return;

        case AST_ASSIGN:
            for (int i = 0; i < ast->u.assign.name_count; i++) {
                patch_symref(&ast->u.assign.refs[i], offset);
                patch_ast_indices(ast->u.assign.targets[i], offset);
            }
            patch_ast_indices(ast->u.assign.value, offset);
            return;

        case AST_COMPOUND_ASSIGN:
            patch_symref(&ast->u.compound_assign.ref, offset);
            patch_ast_indices(ast->u.compound_assign.value, offset);
            return;

        case AST_IF:
            patch_ast_indices(ast->u.if_.cond, offset);
            patch_ast_indices(ast->u.if_.then, offset);
            patch_ast_indices(ast->u.if_.else_, offset);
            patch_symref(&ast->u.if_.guard_var_ref, offset);
            return;

        case AST_WHILE:
            patch_ast_indices(ast->u.while_.cond, offset);
            patch_ast_indices(ast->u.while_.body, offset);
            return;

        case AST_FOR:
            patch_ast_indices(ast->u.for_.body, offset);
            // for 循环的局部变量索引也需要 patch
            if (ast->u.for_.loop_var_index >= 0)
                ast->u.for_.loop_var_index += offset;
            if (ast->u.for_.end_index >= 0)
                ast->u.for_.end_index += offset;
            if (ast->u.for_.step_index >= 0)
                ast->u.for_.step_index += offset;
            if (ast->u.for_.start_index >= 0)
                ast->u.for_.start_index += offset;
            if (ast->u.for_.counter_index >= 0)
                ast->u.for_.counter_index += offset;
            if (ast->u.for_.index_var_index >= 0)
                ast->u.for_.index_var_index += offset;
            patch_ast_indices(ast->u.for_.start, offset);
            patch_ast_indices(ast->u.for_.end, offset);
            patch_ast_indices(ast->u.for_.step, offset);
            return;

        case AST_TRY:
            patch_symref(&ast->u.try_.catch_var_ref, offset);
            patch_ast_indices(ast->u.try_.try_body, offset);
            patch_ast_indices(ast->u.try_.catch_body, offset);
            patch_ast_indices(ast->u.try_.finally_body, offset);
            return;

        case AST_THROW:
            patch_ast_indices(ast->u.throw_.expr, offset);
            return;

        case AST_SWITCH:
            patch_ast_indices(ast->u.switch_.expr, offset);
            for (int i = 0; i < ast->u.switch_.case_count; i++) {
                patch_symref(&ast->u.switch_.cases[i].guard_var_ref, offset);
                for (int j = 0; j < ast->u.switch_.cases[i].values.count; j++)
                    patch_ast_indices(ast->u.switch_.cases[i].values.items[j], offset);
                patch_ast_indices(ast->u.switch_.cases[i].body, offset);
                // destructure_indices
                for (int j = 0; j < ast->u.switch_.cases[i].destructure_count; j++)
                    if (ast->u.switch_.cases[i].destructure_indices)
                        ast->u.switch_.cases[i].destructure_indices[j] += offset;
            }
            patch_ast_indices(ast->u.switch_.default_body, offset);
            return;

        case AST_MODULE_CALL:
            patch_symref(&ast->u.module_call.lib_ref, offset);
            for (int i = 0; i < ast->u.module_call.args.count; i++)
                patch_ast_indices(ast->u.module_call.args.items[i], offset);
            return;

        case AST_MODULE_ACCESS:
            patch_symref(&ast->u.module_access.ref, offset);
            return;

        case AST_ADDRESS_OF:
            patch_ast_indices(ast->u.address_of.operand, offset);
            return;

        case AST_AWAIT:
            patch_ast_indices(ast->u.await.expr, offset);
            return;

case AST_TYPE_CHECK:
case AST_AS_CAST:
    patch_ast_indices(ast->u.type_check.expr, offset);
    return;

        case AST_FUNC_DEF:
            // 局部函数定义不内联其内部，只 patch ref
            patch_symref(&ast->u.func.ref, offset);
            return;

        default:
            return;
    }
}

// ============================================================================
// 内联条件检查
// ============================================================================

// 检查函数体是否包含不支持的语句（递归）
static int body_has_unsupported(Ast* ast) {
    if (!ast) return 0;
    switch (ast->kind) {
        case AST_BLOCK:
            for (int i = 0; i < ast->u.block.count; i++)
                if (body_has_unsupported(ast->u.block.items[i]))
                    return 1;
            return 0;
        case AST_FUNC_DEF:
            return 1;  // 不内联含局部函数定义的函数
        case AST_TRY:
            return 1;  // 不内联含 try/catch 的函数
        case AST_SWITCH:
            return 1;  // 不内联含 switch 的函数
        case AST_IF:
            if (body_has_unsupported(ast->u.if_.cond)) return 1;
            if (body_has_unsupported(ast->u.if_.then)) return 1;
            if (body_has_unsupported(ast->u.if_.else_)) return 1;
            return 0;
        case AST_WHILE:
        case AST_FOR:
            return 1;  // 不内联含循环的函数（循环变量索引 patch 复杂）
        case AST_RETURN:
            return body_has_unsupported(ast->u.ret);
        case AST_VAR_DECL:
            return body_has_unsupported(ast->u.var_decl.init);
        case AST_EXPR_STMT:
            return body_has_unsupported(ast->u.expr_stmt.expr);
        case AST_ASSIGN:
            if (body_has_unsupported(ast->u.assign.value)) return 1;
            for (int i = 0; i < ast->u.assign.name_count; i++)
                if (body_has_unsupported(ast->u.assign.targets[i])) return 1;
            return 0;
        case AST_COMPOUND_ASSIGN:
            return body_has_unsupported(ast->u.compound_assign.value);
        // 表达式中的匿名函数（闭包）—— upvalue 索引无法简单 remap
        case AST_CALL:
            if (body_has_unsupported(ast->u.call.callee)) return 1;
            for (int i = 0; i < ast->u.call.args.count; i++)
                if (body_has_unsupported(ast->u.call.args.items[i])) return 1;
            return 0;
        case AST_INDEX:
            if (body_has_unsupported(ast->u.index.obj)) return 1;
            return body_has_unsupported(ast->u.index.index);
        case AST_BINOP:
            if (body_has_unsupported(ast->u.binop.l)) return 1;
            return body_has_unsupported(ast->u.binop.r);
        case AST_UNARY:
            return body_has_unsupported(ast->u.unary.operand);
        case AST_FIELD_ACCESS:
            return body_has_unsupported(ast->u.field_access.obj);
        case AST_ARRAY:
            for (int i = 0; i < ast->u.array.count; i++)
                if (body_has_unsupported(ast->u.array.items[i])) return 1;
            return 0;
        case AST_STRUCT_INIT:
            for (int i = 0; i < ast->u.struct_init.field_count; i++)
                if (body_has_unsupported(ast->u.struct_init.field_values[i])) return 1;
            return 0;
        case AST_BREAK:
        case AST_CONTINUE:
        case AST_THROW:
            return 0;
        default:
            return 0;
    }
}

// 检查函数是否可以内联
static int can_inline(CodeGen* gen, Ast* func_def, int provided_arg_count) {
    (void)provided_arg_count;
    if (!func_def || func_def->kind != AST_FUNC_DEF) return 0;

    // 闭包函数不能内联（有 upvalue）
    if (func_def->u.func.upvalue_count > 0) return 0;
    // async 函数不能内联
    if (func_def->u.func.is_async) return 0;
    // 构造/析构函数不能内联
    if (func_def->u.func.is_ctor || func_def->u.func.is_dtor) return 0;
    // 泛型函数不能内联
    if (func_def->u.func.type_param_count > 0) return 0;
    // 嵌套深度限制
    if (gen->inline_depth >= MAX_INLINE_DEPTH) return 0;
    // 递归内联检测：如果函数名已在内联栈中，禁止再内联
    if (func_def->u.func.name) {
        for (int i = 0; i < inline_name_stack_top; i++) {
            if (inline_name_stack[i] &&
                strcmp(inline_name_stack[i], func_def->u.func.name) == 0)
                return 0;
        }
    }
    // 暂不支持 any 类型参数（call() 中有类型转换逻辑）
    if (func_def->u.func.param_types) {
        for (int i = 0; i < func_def->u.func.pcnt; i++) {
            if (func_def->u.func.param_types[i] &&
                func_def->u.func.param_types[i]->kind == TYPE_ANY)
                return 0;
        }
    }

    // 默认参数：如果有默认参数，需要更复杂的处理，暂不支持
    if (func_def->u.func.default_count > 0) return 0;

    Ast* body = func_def->u.func.body;
    if (!body || body->kind != AST_BLOCK) return 0;

    // 语句数量限制
    if (body->u.block.count > MAX_INLINE_STMTS) return 0;

    // 检查是否包含不支持的语句
    if (body_has_unsupported(body)) return 0;

    return 1;
}

// ============================================================================
// 内联实现
// ============================================================================

// 前向声明
extern void gen_expr(CodeGen* gen, Ast* ast);
extern void gen_stmt(CodeGen* gen, Ast* ast);

// 尝试内联函数调用
// 返回 1=已内联, 0=未内联（回退到普通调用）
int try_inline_call(CodeGen* gen, Ast* ast, Ast* func_def) {
    if (!can_inline(gen, func_def, ast->u.call.args.count))
        return 0;

    Ast* body = func_def->u.func.body;
    int local_count = func_def->u.func.local_count;
    int arg_count = ast->u.call.args.count;
    int body_count = body->u.block.count;

    // 分配槽位：从当前 max_local_slot+1 开始
    int base = gen->max_local_slot + 1;

    // 检测快速路径：空函数体或单条 return 语句（无条件 return）
    // 这些情况不需要 result_slot 和 return 跳转机制
    int fast_path = 0;
    Ast* fast_return_expr = NULL;  // NULL 表示 void return

    if (body_count == 0) {
        // func f() {} → 返回 null
        fast_path = 1;
        fast_return_expr = NULL;
    } else if (body_count == 1 &&
               body->u.block.items[0]->kind == AST_RETURN) {
        // func f() { return expr } 或 func f() { return }
        fast_path = 1;
        fast_return_expr = body->u.block.items[0]->u.ret;
    }

    // 1. 参数已在 gen_call 中求值并压栈，这里逆序弹出存入参数槽位
    for (int i = arg_count - 1; i >= 0; i--) {
        emit_bytes_2(gen, OP_SET_LOCAL_POP, base + i, ast->line);
    }
    // 初始化其余局部变量为 null
    for (int i = arg_count; i < local_count; i++) {
        emit_byte(gen, OP_NULL, ast->line);
        emit_bytes_2(gen, OP_SET_LOCAL_POP, base + i, ast->line);
    }

    // 更新槽位计数
    int needed = base + local_count;
    if (fast_path) {
        // 快速路径不需要 result_slot
    } else {
        needed = base + local_count + 1;  // +1 for result_slot
    }
    if (needed > gen->max_local_slot) gen->max_local_slot = needed;
    if (needed > gen->peak_local_slot) gen->peak_local_slot = needed;
    if (gen->current_func && needed > gen->current_func->local_count)
        gen->current_func->local_count = needed;

    // 2. Patch AST 中所有局部变量索引
    patch_ast_indices(body, base);

    // 压入函数名到递归检测栈
    inline_name_stack[inline_name_stack_top++] = func_def->u.func.name;

    if (fast_path) {
        // ===== 快速路径：直接生成返回表达式，无需 result_slot =====
        int saved_depth = gen->inline_depth;
        gen->inline_depth++;  // 仅为递归检测和深度限制

        if (fast_return_expr) {
            gen_expr(gen, fast_return_expr);
            gen->inline_no_result = 0;
        } else {
            // 空函数体：如果结果将被丢弃，什么都不生成
            if (gen->inline_discard_result) {
                gen->inline_no_result = 1;  // 告知 gen_expr_stmt 跳过 OP_POP
            } else {
                emit_byte(gen, OP_NULL, ast->line);
                gen->inline_no_result = 0;
            }
        }

        gen->inline_depth = saved_depth;
        inline_name_stack_top--;

        // 恢复 AST 索引
        patch_ast_indices(body, -base);
        return 1;
    }

    // ===== 通用路径：使用 result_slot 和 return 跳转 =====
    int result_slot = base + local_count;

    // 3. 保存内联上下文
    int saved_depth = gen->inline_depth;
    int saved_result_slot = gen->inline_result_slot;
    int saved_jump_count = gen->inline_return_jump_count;

    gen->inline_depth++;
    gen->inline_result_slot = result_slot;
    gen->inline_return_jump_count = 0;

    // 4. 生成函数体（逐条语句，不用 gen_block 避免预处理逻辑）
    for (int i = 0; i < body_count; i++) {
        gen_stmt(gen, body->u.block.items[i]);
    }

    // 5. 函数体末尾没有 return 的情况（void 函数）：push null
    emit_byte(gen, OP_NULL, ast->line);
    emit_bytes_2(gen, OP_SET_LOCAL, gen->inline_result_slot, ast->line);

    // 6. 回填所有 return 跳转到这里
    for (int i = 0; i < gen->inline_return_jump_count; i++) {
        patch_jump(gen, gen->inline_return_jumps[i]);
    }

    // 7. 加载返回值到栈顶
    emit_bytes_2(gen, OP_GET_LOCAL, gen->inline_result_slot, ast->line);

    // 8. 恢复上下文
    gen->inline_depth = saved_depth;
    gen->inline_result_slot = saved_result_slot;
    gen->inline_return_jump_count = saved_jump_count;
    inline_name_stack_top--;

    // 9. 恢复 AST 索引
    patch_ast_indices(body, -base);

    return 1;
}
