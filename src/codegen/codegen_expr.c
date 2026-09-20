// ============================================================================
// 寄存器式 codegen：表达式生成
// 核心接口：gen_expr_to(gen, ast, dst) → 结果写入 R[dst]
//           gen_expr(gen, ast) → 借临时寄存器，返回寄存器号
// ============================================================================

#include "codegen.h"

// 前向声明（语句相关，定义在 codegen_stmt.c）
extern void gen_default_value(CodeGen* gen, Ast* default_expr);

// ============================================================================
// 表达式入口
// ============================================================================

int gen_expr(CodeGen* gen, Ast* ast) {
    int r = reg_alloc(gen);
    gen_expr_to(gen, ast, r);
    return r;
}

void gen_expr_to(CodeGen* gen, Ast* ast, int dst) {
    if (!ast) {
        emit_loadnil_to(gen, dst, 0);
        return;
    }

    switch (ast->kind) {
        // --- 字面量 ---
        case AST_NUM:
            if (ast->u.num.is_bigint) {
                // BigInt：存常量表
                // TODO: bigint 字面量处理
                ObjBigInt* bi = bigint_from_string(ast->u.num.bigint_str);
                int idx = make_constant(gen, val_obj((Object*)bi));
                emit_loadk_to(gen, dst, idx, ast->line);
            } else if (ast->u.num.is_float) {
                int idx = make_constant(gen, val_float(ast->u.num.value));
                emit_loadk_to(gen, dst, idx, ast->line);
            } else {
                // 整数立即数：小数用 LOADI，大数用 LOADK
                int64_t v = (int64_t)ast->u.num.value;
                if (v >= -32767 && v <= 32767) {
                    emit_loadi_to(gen, dst, (int)v, ast->line);
                } else {
                    int idx = make_constant(gen, val_int(v));
                    emit_loadk_to(gen, dst, idx, ast->line);
                }
            }
            break;

        case AST_STRING:
        {
            ObjString* s = str_copy(ast->u.string.value, ast->u.string.len);
            int idx = make_constant(gen, val_obj((Object*)s));
            emit_loadk_to(gen, dst, idx, ast->line);
            break;
        }

        case AST_BOOL:
            if (ast->u.boolean) {
                emit_loadtrue_to(gen, dst, ast->line);
            } else {
                emit_loadfalse_to(gen, dst, ast->line);
            }
            break;

        case AST_NULL:
            emit_loadnil_to(gen, dst, ast->line);
            break;

        // --- 变量引用 ---
        case AST_VAR:
        {
            Symbol* sym = scope_resolve(gen->sem->current, ast->u.var.name);
            if (!sym) {
                // 不应发生（语义分析已检查）
                emit_loadnil_to(gen, dst, ast->line);
                break;
            }
            switch (sym->kind) {
                case SYM_LOCAL:
                case SYM_PARAM:
                    // 局部变量就是寄存器，直接 MOV
                    if (sym->index != dst) {
                        emit_mov(gen, dst, sym->index, ast->line);
                    }
                    break;
                case SYM_GLOBAL:
                    emit_getglobal_to(gen, dst, sym->index, ast->line);
                    break;
                case SYM_GLOBAL_FUNC:
                    emit_getglobalfunc_to(gen, dst, sym->index, ast->line);
                    break;
                case SYM_NATIVE:
                {
                    // native 函数引用：压入 null 占位（调用时用 CALL_NATIVE）
                    emit_loadnil_to(gen, dst, ast->line);
                    break;
                }
                case SYM_UPVALUE:
                    emit_getupval_to(gen, dst, sym->index, ast->line);
                    break;
                default:
                    emit_loadnil_to(gen, dst, ast->line);
                    break;
            }
            break;
        }

        // --- 二元运算 ---
        case AST_BINOP:
            gen_binop(gen, ast, dst);
            break;

        // --- 一元运算 ---
        case AST_UNARY:
            gen_unary(gen, ast, dst);
            break;

        // --- 函数调用 ---
        case AST_CALL:
            gen_call(gen, ast, dst);
            break;

        // --- 索引访问 ---
        case AST_INDEX:
        {
            int obj_reg = gen_expr(gen, ast->u.index.obj);
            int idx_reg = gen_expr(gen, ast->u.index.index);
            // INDEX: R[A] = R[B][R[C]]
            reg_encode_iABC(gen->chunk, OP_INDEX, dst, obj_reg, idx_reg, ast->line);
            reg_free(gen, idx_reg);
            reg_free(gen, obj_reg);
            break;
        }

        // --- 字段访问 ---
        case AST_FIELD_ACCESS:
        {
            int obj_reg = gen_expr(gen, ast->u.field_access.obj);
            int field_idx = ast->u.field_access.field_index;
            // GET_FIELD: R[A] = R[B].field(C)
            reg_encode_iABC(gen->chunk, OP_GET_FIELD, dst, obj_reg, field_idx, ast->line);
            reg_free(gen, obj_reg);
            break;
        }

        // --- 数组字面量 ---
        case AST_ARRAY:
        {
            int n = ast->u.array.count;
            // 实参从 R[dst+1] 开始
            for (int i = 0; i < n; i++) {
                int r = reg_alloc(gen);
                gen_expr_to(gen, ast->u.array.items[i], r);
            }
            // NEWARRAY: R[A] = new array(R[A+1..A+C-1]), C = count
            reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, n, ast->line);
            // 释放临时寄存器
            for (int i = 0; i < n; i++) {
                gen->next_reg--;  // 回退
            }
            break;
        }

        // --- 字典字面量 ---
        case AST_DICT:
        {
            int n = ast->u.dict.count;
            // 键值对从 R[dst+1] 开始交替
            for (int i = 0; i < n; i++) {
                int r = reg_alloc(gen);
                gen_expr_to(gen, ast->u.dict.entries[i].key, r);
                r = reg_alloc(gen);
                gen_expr_to(gen, ast->u.dict.entries[i].value, r);
            }
            reg_encode_iABC(gen->chunk, OP_NEWDICT, dst, 0, n, ast->line);
            for (int i = 0; i < n * 2; i++) {
                gen->next_reg--;
            }
            break;
        }

        // --- 范围 ---
        case AST_RANGE:
        {
            int start_reg = gen_expr(gen, ast->u.range.start);
            int end_reg = gen_expr(gen, ast->u.range.end);
            // RANGE: R[A] = Range(R[B], R[C])
            reg_encode_iABC(gen->chunk, OP_RANGE, dst, start_reg, end_reg, ast->line);
            reg_free(gen, end_reg);
            reg_free(gen, start_reg);
            break;
        }

        // --- 插值字符串 ---
        case AST_INTERP_STRING:
            gen_interp_string(gen, ast, dst);
            break;

        // --- 模块访问 ---
        case AST_MODULE_ACCESS:
            gen_module_access(gen, ast, dst);
            break;

        // --- 模块方法调用 ---
        case AST_MODULE_CALL:
            gen_module_call(gen, ast, dst);
            break;

        // --- if 表达式 ---
        case AST_IF:
            gen_if_ex(gen, ast, 1);
            // if 表达式结果在 dst，由 gen_if_ex 内部写入
            break;

        // --- 类型检查 ---
        case AST_TYPE_CHECK:
        {
            int src_reg = gen_expr(gen, ast->u.type_check.expr);
            emit_mov(gen, dst, src_reg, ast->line);
            // TYPE_CHECK: R[A] = (R[A] is type(Bx))
            // 简化：Bx = type_info 索引（需从语义分析获取）
            reg_encode_iABx(gen->chunk, OP_TYPE_CHECK, dst, 0, ast->line);
            reg_free(gen, src_reg);
            break;
        }

        // --- 安全类型转换 ---
        case AST_AS_CAST:
        {
            int src_reg = gen_expr(gen, ast->u.type_check.expr);
            // AS_CAST: R[A] = R[A] as type(Bx)
            emit_mov(gen, dst, src_reg, ast->line);
            reg_encode_iABx(gen->chunk, OP_AS_CAST, dst, 0, ast->line);
            reg_free(gen, src_reg);
            break;
        }

        // --- await ---
        case AST_AWAIT:
        {
            int src_reg = gen_expr(gen, ast->u.await.expr);
            // AWAIT: R[A] = await R[B]
            reg_encode_iABC(gen->chunk, OP_AWAIT, dst, src_reg, 0, ast->line);
            reg_free(gen, src_reg);
            break;
        }

        // --- 安全访问 ---
        case AST_SAFE_ACCESS:
            gen_safe_access(gen, ast, dst);
            break;

        // --- 取地址 ---
        case AST_ADDRESS_OF:
        {
            // &obj.field → GET_FIELD_ADDR
            Ast* field_ast = ast->u.address_of.operand;
            int obj_reg = gen_expr(gen, field_ast->u.field_access.obj);
            reg_encode_iABC(gen->chunk, OP_GET_FIELD_ADDR, dst, obj_reg,
                          field_ast->u.field_access.field_index, ast->line);
            reg_free(gen, obj_reg);
            break;
        }

        // --- struct 初始化 ---
        case AST_STRUCT_INIT:
            gen_struct_init(gen, ast, dst);
            break;

        default:
            emit_loadnil_to(gen, dst, ast->line);
            break;
    }
}

// ============================================================================
// 二元运算
// ============================================================================

void gen_binop(CodeGen* gen, Ast* ast, int dst) {
    Ast* lhs = ast->u.binop.l;
    Ast* rhs = ast->u.binop.r;
    LenoTokenType op = ast->u.binop.op;

    // 短路运算
    if (op == TOK_AND) {
        // R[dst] = R[lhs] && R[rhs]
        gen_expr_to(gen, lhs, dst);
        int jmp_false = emit_jmp_if_false(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_false);
        return;
    }
    if (op == TOK_OR) {
        gen_expr_to(gen, lhs, dst);
        int jmp_true = emit_jmp_if_true(gen, dst, ast->line);
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp_true);
        return;
    }

    // 空合并
    if (op == TOK_NULL_COALESCE) {
        gen_expr_to(gen, lhs, dst);
        // 如果 dst != null 则跳过
        int jmp = emit_jmp_if_true_ex(gen, dst, ast->line);  // 非 null 跳过
        // TODO: IS_NULL + JMP_IF_FALSE
        gen_expr_to(gen, rhs, dst);
        patch_jmp(gen, jmp);
        return;
    }

    // 普通二元运算：求 lhs → dst，求 rhs → 临时寄存器，运算写回 dst
    gen_expr_to(gen, lhs, dst);
    int r = gen_expr(gen, rhs);

    switch (op) {
        case TOK_PLUS:     emit_add(gen, dst, dst, r, ast->line); break;
        case TOK_MINUS:    emit_sub(gen, dst, dst, r, ast->line); break;
        case TOK_STAR:     emit_mul(gen, dst, dst, r, ast->line); break;
        case TOK_SLASH:    emit_div(gen, dst, dst, r, ast->line); break;
        case TOK_MOD:      emit_mod(gen, dst, dst, r, ast->line); break;
        case TOK_EQEQ:     emit_eq(gen, dst, dst, r, ast->line); break;
        case TOK_NEQ:      emit_neq(gen, dst, dst, r, ast->line); break;
        case TOK_LT:       emit_lt(gen, dst, dst, r, ast->line); break;
        case TOK_GT:       emit_gt(gen, dst, dst, r, ast->line); break;
        case TOK_LE:       emit_le(gen, dst, dst, r, ast->line); break;
        case TOK_GE:       emit_ge(gen, dst, dst, r, ast->line); break;
        case TOK_BITAND:   emit_bitand(gen, dst, dst, r, ast->line); break;
        case TOK_BITOR:    emit_bitor(gen, dst, dst, r, ast->line); break;
        case TOK_BITXOR:   emit_bitxor(gen, dst, dst, r, ast->line); break;
        case TOK_SHL:      emit_shl(gen, dst, dst, r, ast->line); break;
        case TOK_SHR:      emit_shr(gen, dst, dst, r, ast->line); break;
        case TOK_USHR:     emit_ushr(gen, dst, dst, r, ast->line); break;
        case TOK_IN:       reg_encode_iABC(gen->chunk, OP_IN, dst, dst, r, ast->line); break;
        default:
            // 字符串拼接用 STRCAT
            if (op == TOK_PLUS) {
                emit_strcat(gen, dst, dst, r, ast->line);
            }
            break;
    }
    reg_free(gen, r);
}

// ============================================================================
// 一元运算
// ============================================================================

void gen_unary(CodeGen* gen, Ast* ast, int dst) {
    LenoTokenType op = ast->u.unary.op;
    int operand = gen_expr(gen, ast->u.unary.operand);

    switch (op) {
        case TOK_MINUS:    emit_neg(gen, dst, operand, ast->line); break;
        case TOK_NOT:       emit_not(gen, dst, operand, ast->line); break;
        case TOK_BITNOT:    emit_bitnot(gen, dst, operand, ast->line); break;
        case TOK_INC:       emit_inc(gen, dst, operand, ast->line); break;
        case TOK_DEC:       emit_dec(gen, dst, operand, ast->line); break;
        default:            emit_mov(gen, dst, operand, ast->line); break;
    }
    reg_free(gen, operand);
}

// ============================================================================
// 函数调用
// ============================================================================

void gen_call(CodeGen* gen, Ast* ast, int dst) {
    // 检查原生函数调用
    if (ast->u.call.callee->kind == AST_VAR) {
        const char* func_name = ast->u.call.callee->u.var.name;
        Symbol* sym = scope_resolve(gen->sem->current, func_name);

        if (sym && sym->kind == SYM_NATIVE) {
            // CALL_NATIVE: R[A] = native(name)(R[A+1..A+C-1])
            ObjString* nameStr = str_copy(sym->name, (int)strlen(sym->name));
            int name_const = make_constant(gen, val_obj((Object*)nameStr));
            int nargs = ast->u.call.args.count;

            // 实参放在 R[dst+1..dst+nargs]
            for (int i = 0; i < nargs; i++) {
                int r = reg_alloc(gen);
                gen_expr_to(gen, ast->u.call.args.items[i], r);
            }
            emit_call_native(gen, dst, name_const, nargs, ast->line);

            // 释放临时寄存器
            for (int i = 0; i < nargs; i++) {
                gen->next_reg--;
            }
            return;
        }

        if (sym && sym->kind == SYM_GLOBAL_FUNC) {
            // GETGLOBALFUNC R[dst], slot
            // 实参放在 R[dst+1..dst+nargs]
            int nargs = ast->u.call.args.count;
            emit_getglobalfunc_to(gen, dst, sym->index, ast->line);
            for (int i = 0; i < nargs; i++) {
                int r = reg_alloc(gen);
                gen_expr_to(gen, ast->u.call.args.items[i], r);
            }
            // CALL: R[A] = callee, R[A+1..A+B-1] = args, B=nargs+1, C=nresults+1
            emit_call(gen, dst, nargs, 1, ast->line);
            for (int i = 0; i < nargs; i++) {
                gen->next_reg--;
            }
            return;
        }
    }

    // 通用调用：求值 callee → dst，参数 → dst+1..
    int nargs = ast->u.call.args.count;
    gen_expr_to(gen, ast->u.call.callee, dst);
    for (int i = 0; i < nargs; i++) {
        int r = reg_alloc(gen);
        gen_expr_to(gen, ast->u.call.args.items[i], r);
    }
    emit_call(gen, dst, nargs, 1, ast->line);
    for (int i = 0; i < nargs; i++) {
        gen->next_reg--;
    }
}

// ============================================================================
// 插值字符串
// ============================================================================

void gen_interp_string(CodeGen* gen, Ast* ast, int dst) {
    int count = ast->u.interp_string.count;
    // 交替：字符串片段 + 表达式
    // 先求第一个片段到 dst，然后逐个 STRCAT
    int first = 1;
    int cur = dst;

    for (int i = 0; i < count; i++) {
        // 字符串片段
        if (ast->u.interp_string.parts[i]) {
            ObjString* s = str_copy(ast->u.interp_string.parts[i],
                                    (int)strlen(ast->u.interp_string.parts[i]));
            int idx = make_constant(gen, val_obj((Object*)s));
            if (first) {
                emit_loadk_to(gen, cur, idx, ast->line);
                first = 0;
            } else {
                int tmp = reg_alloc(gen);
                emit_loadk_to(gen, tmp, idx, ast->line);
                emit_strcat(gen, cur, cur, tmp, ast->line);
                reg_free(gen, tmp);
            }
        }
        // 表达式
        if (i < ast->u.interp_string.count && ast->u.interp_string.exprs[i]) {
            if (first) {
                gen_expr_to(gen, ast->u.interp_string.exprs[i], cur);
                first = 0;
            } else {
                int tmp = gen_expr(gen, ast->u.interp_string.exprs[i]);
                emit_strcat(gen, cur, cur, tmp, ast->line);
                reg_free(gen, tmp);
            }
        }
    }
}

// ============================================================================
// 模块访问 / 模块调用
// ============================================================================

void gen_module_access(CodeGen* gen, Ast* ast, int dst) {
    // GET_MODULE_VAR: R[A] = module[B].var[C]
    // 简化：用常量索引
    int mod_idx = 0;  // TODO: 从语义分析获取模块索引
    int var_idx = 0;  // TODO: 从语义分析获取变量索引
    reg_encode_iABC(gen->chunk, OP_GET_MODULE_VAR, dst, mod_idx, var_idx, ast->line);
}

void gen_module_call(CodeGen* gen, Ast* ast, int dst) {
    // MODULE_CALL: R[A] = module.method(R[A+1..])
    int nargs = ast->u.module_call.args.count;
    for (int i = 0; i < nargs; i++) {
        int r = reg_alloc(gen);
        gen_expr_to(gen, ast->u.module_call.args.items[i], r);
    }
    // 简化：mod_idx 和 method_idx 从语义分析获取
    reg_encode_iABC(gen->chunk, OP_MODULE_CALL, dst, 0, nargs, ast->line);
    for (int i = 0; i < nargs; i++) {
        gen->next_reg--;
    }
}

// ============================================================================
// struct 初始化
// ============================================================================

void gen_struct_init(CodeGen* gen, Ast* ast, int dst) {
    int n = ast->u.struct_init.field_count;
    // 实参从 R[dst+1] 开始
    for (int i = 0; i < n; i++) {
        int r = reg_alloc(gen);
        gen_expr_to(gen, ast->u.struct_init.field_values[i], r);
    }
    // STRUCT_INIT: R[A] = new struct(R[A+1..A+C-1])
    reg_encode_iABC(gen->chunk, OP_STRUCT_INIT, dst, 0, n, ast->line);
    for (int i = 0; i < n; i++) {
        gen->next_reg--;
    }
}

// ============================================================================
// 安全访问（?.）
// ============================================================================

void gen_safe_access(CodeGen* gen, Ast* ast, int dst) {
    // 简化：先求 obj，检查 null，null 则 dst=null，否则正常访问
    // TODO: 完整实现
    emit_loadnil_to(gen, dst, ast->line);
}

// ============================================================================
// 辅助：非 null 跳转（用于空合并）
// ============================================================================

// 临时实现：JMP_IF_TRUE
int emit_jmp_if_true_ex(CodeGen* gen, int a, int line) {
    return emit_jmp_if_true(gen, a, line);
}
