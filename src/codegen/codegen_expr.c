// ============================================================================
// 寄存器式 codegen：表达式生成
// 核心接口：gen_expr_to(gen, ast, dst) → 结果写入 R[dst]
//           gen_expr(gen, ast) → 借临时寄存器，返回寄存器号
// ============================================================================

#include "codegen.h"

// 前向声明（语句相关，定义在 codegen_stmt.c）
extern void gen_default_value(CodeGen* gen, Ast* default_expr);

// ============================================================================
// 类型检查 / 安全转换（就地作用于 R[reg]）
//   需要名字的类别（struct/face/enum、struct/face/cstruct）在指令后附 2 字节名字常量
// ============================================================================
static void emit_type_op(CodeGen* gen, OpCode op, int reg, TypeInfo* t, int with_enum, int line) {
    TypeKind kind = t ? t->kind : TYPE_ANY;
    TypeKind elem = (t && t->element_type) ? t->element_type->kind : TYPE_ANY;

    int need_name = 0;
    if (op == OP_TYPE_CHECK) {
        need_name = (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_ENUM);
    } else {
        need_name = (kind == TYPE_STRUCT || kind == TYPE_FACE || kind == TYPE_CSTRUCT);
    }
    (void)with_enum;

    if (need_name) {
        const char* nm = (t && t->struct_name) ? t->struct_name : "";
        int nc = make_constant(gen, val_obj((Object*)str_copy(nm, (int)strlen(nm))));
        reg_encode_iABC(gen->chunk, op, reg, kind, elem, line);
        chunk_write(gen->chunk, (uint8_t)((nc >> 8) & 0xFF), line);
        chunk_write(gen->chunk, (uint8_t)(nc & 0xFF), line);
    } else {
        reg_encode_iABC(gen->chunk, op, reg, kind, elem, line);
    }
}

void emit_type_check_to(CodeGen* gen, int reg, TypeInfo* t, int line) {
    emit_type_op(gen, OP_TYPE_CHECK, reg, t, 1, line);
}

void emit_as_cast_to(CodeGen* gen, int reg, TypeInfo* t, int line) {
    emit_type_op(gen, OP_AS_CAST, reg, t, 0, line);
}

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
        // 符号解析结果由语义分析阶段写入 AST（u.var.ref），codegen 直接使用。
        // 不再用 scope_resolve(sem->current, name)：codegen 阶段 current 作用域
        // 已不对应声明点，而且语义阶段早有精确结果。
        case AST_VAR:
        {
            SymRef* ref = &ast->u.var.ref;
            if (!ref->name) {
                emit_loadnil_to(gen, dst, ast->line);
                break;
            }
            switch (ref->kind) {
                case SYM_LOCAL:
                case SYM_PARAM:
                    // 局部变量就是寄存器，直接 MOV
                    if (ref->index != dst) {
                        emit_mov(gen, dst, ref->index, ast->line);
                    }
                    break;
                case SYM_GLOBAL:
                    emit_getglobal_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_GLOBAL_FUNC:
                    emit_getglobalfunc_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_UPVALUE:
                    emit_getupval_to(gen, dst, ref->index, ast->line);
                    break;
                case SYM_MODULE:
                    // 模块变量：由 OP_GET_MODULE_VAR 处理（索引在 ref->index）
                    reg_encode_iABC(gen->chunk, OP_GET_MODULE_VAR, dst, ref->index, 0, ast->line);
                    break;
                case SYM_NATIVE:
                case SYM_TYPE:
                case SYM_STRUCT:
                case SYM_CSTRUCT:
                case SYM_CLIB:
                case SYM_CFUNC:
                case SYM_ENUM:
                case SYM_FUNC_ALIAS:
                default:
                    // 函数引用 / 类型名等：作为值使用时是 null 占位
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

        // --- 匿名函数表达式（闭包工厂返回值等）：创建闭包写到 dst ---
        case AST_FUNC_DEF: {
            ObjFunction* fn = gen_func_proto(gen, ast);
            if (fn) {
                gen_func_closure(gen, ast, fn);
                int cidx = make_constant(gen, val_obj((Object*)fn));
                emit_closure_upvals(gen, dst, cidx, ast);
            } else {
                emit_loadnil_to(gen, dst, ast->line);
            }
            break;
        }

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
        // 元素必须落在 R[dst+1 .. dst+n]（NEWARRAY 按 A+i 读取），
        // 所以先把高水位抬到 dst+n+1，元素间才不会夹入临时寄存器。
        case AST_ARRAY:
        {
            int n = ast->u.array.count;
            if (dst + 1 + n > gen->next_reg) {
                gen->next_reg = dst + 1 + n;
                if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
            }
            for (int i = 0; i < n; i++) {
                gen_expr_to(gen, ast->u.array.items[i], dst + 1 + i);
            }
            // NEWARRAY: R[A] = new array(R[A+1..A+C-1]), C = count
            reg_encode_iABC(gen->chunk, OP_NEWARRAY, dst, 0, n, ast->line);
            break;
        }

        // --- 字典字面量 ---
        // 键值对交替落在 R[dst+1 .. dst+2n]（同 NEWARRAY 的连号约定）
        case AST_DICT:
        {
            int n = ast->u.dict.count;
            if (dst + 1 + n * 2 > gen->next_reg) {
                gen->next_reg = dst + 1 + n * 2;
                if (gen->next_reg > gen->max_reg) gen->max_reg = gen->next_reg;
            }
            for (int i = 0; i < n; i++) {
                gen_expr_to(gen, ast->u.dict.entries[i].key, dst + 1 + i * 2);
                gen_expr_to(gen, ast->u.dict.entries[i].value, dst + 2 + i * 2);
            }
            reg_encode_iABC(gen->chunk, OP_NEWDICT, dst, 0, n, ast->line);
            break;
        }

        // --- 切片：arr[start:end] ---
        // OP_SLICE 约定：A = 结果，B = 对象，start/end 固定在 R[A+1] / R[A+2]
        case AST_SLICE: {
            int base = reg_alloc_block(gen, 3);
            gen_expr_to(gen, ast->u.slice.obj, base);
            if (ast->u.slice.start) gen_expr_to(gen, ast->u.slice.start, base + 1);
            else emit_loadnil_to(gen, base + 1, ast->line);
            if (ast->u.slice.end) gen_expr_to(gen, ast->u.slice.end, base + 2);
            else emit_loadnil_to(gen, base + 2, ast->line);
            reg_encode_iABC(gen->chunk, OP_SLICE, base, base, 0, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
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

        // --- 类型检查：x is T ---
        case AST_TYPE_CHECK:
        {
            gen_expr_to(gen, ast->u.type_check.expr, dst);
            emit_type_check_to(gen, dst, ast->u.type_check.type, ast->line);
            break;
        }

        // --- 安全类型转换：x as T ---
        case AST_AS_CAST:
        {
            gen_expr_to(gen, ast->u.type_check.expr, dst);
            emit_as_cast_to(gen, dst, ast->u.type_check.type, ast->line);
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

        // --- 赋值作为表达式（parser 会把赋值语句包成表达式语句）---
        case AST_ASSIGN: {
            gen_assign(gen, ast);
            // 表达式结果 = 被赋的值
            if (ast->u.assign.name_count >= 1 && ast->u.assign.targets) {
                Ast* t = ast->u.assign.targets[0];
                SymRef* ref = (ast->u.assign.refs && ast->u.assign.refs[0].name)
                                  ? &ast->u.assign.refs[0] : &t->u.var.ref;
                if (t->kind == AST_VAR) {
                    if (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM) {
                        if (ref->index != dst) emit_mov(gen, dst, ref->index, ast->line);
                    } else if (ref->kind == SYM_GLOBAL) {
                        emit_getglobal_to(gen, dst, ref->index, ast->line);
                    } else if (ref->kind == SYM_UPVALUE) {
                        emit_getupval_to(gen, dst, ref->index, ast->line);
                    } else {
                        emit_loadnil_to(gen, dst, ast->line);
                    }
                } else {
                    emit_loadnil_to(gen, dst, ast->line);
                }
            } else {
                emit_loadnil_to(gen, dst, ast->line);
            }
            break;
        }

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
    Ast* operand_ast = ast->u.unary.operand;

    // ++ / -- 必须就地作用于变量的寄存器 —— 否则只改了副本，变量本身不变。
    if (op == TOK_INC || op == TOK_DEC) {
        if (operand_ast && operand_ast->kind == AST_VAR) {
            SymRef* ref = &operand_ast->u.var.ref;
            int is_local = (ref->kind == SYM_LOCAL || ref->kind == SYM_PARAM);
            int slot;

            if (is_local) {
                slot = ref->index;
            } else {
                slot = reg_alloc(gen);
                if (ref->kind == SYM_GLOBAL) {
                    emit_getglobal_to(gen, slot, ref->index, ast->line);
                } else if (ref->kind == SYM_UPVALUE) {
                    emit_getupval_to(gen, slot, ref->index, ast->line);
                } else {
                    emit_loadnil_to(gen, slot, ast->line);
                }
            }

            if (ast->u.unary.is_postfix) {
                // 后缀：表达式取旧值，变量随后自增/自减
                if (dst != slot) emit_mov(gen, dst, slot, ast->line);
                if (op == TOK_INC) emit_inc(gen, slot, slot, ast->line);
                else emit_dec(gen, slot, slot, ast->line);
            } else {
                // 前缀：先自增/自减，表达式取新值
                if (op == TOK_INC) emit_inc(gen, slot, slot, ast->line);
                else emit_dec(gen, slot, slot, ast->line);
                if (dst != slot) emit_mov(gen, dst, slot, ast->line);
            }

            if (!is_local) {
                if (ref->kind == SYM_GLOBAL) {
                    emit_setglobal(gen, slot, ref->index, ast->line);
                } else if (ref->kind == SYM_UPVALUE) {
                    emit_setupval(gen, slot, ref->index, ast->line);
                }
                reg_free(gen, slot);
            }
            return;
        }
    }

    int operand = gen_expr(gen, operand_ast);

    switch (op) {
        case TOK_MINUS:    emit_neg(gen, dst, operand, ast->line); break;
        case TOK_NOT:       emit_not(gen, dst, operand, ast->line); break;
        case TOK_BITNOT:    emit_bitnot(gen, dst, operand, ast->line); break;
        default:            emit_mov(gen, dst, operand, ast->line); break;
    }
    reg_free(gen, operand);
}

// ============================================================================
// 函数调用
// ============================================================================

// 生成 obj.name(实参...) 的方法调用：
//   R[base] = R[base].name    （OP_GET_METHOD，产出绑定方法 / native）
//   R[base+1..] = 实参
//   CALL R[base], nargs, 1
static void gen_method_call(CodeGen* gen, Ast* obj_ast, const char* mname,
                            AstList* args, int nargs, int dst, int line) {
    int base = reg_alloc_block(gen, nargs + 1);
    gen_expr_to(gen, obj_ast, base);

    int mlen = (int)strlen(mname);
    ObjString* nameStr = str_copy(mname, mlen);
    int name_const = make_constant(gen, val_obj((Object*)nameStr));

    // C 为 0 或超 8 位 → 紧随一条 EXTRAARG 携带 24 位常量索引
    if (name_const == 0 || name_const > 255) {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, 0, line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, name_const, line);
    } else {
        reg_encode_iABC(gen->chunk, OP_GET_METHOD, base, base, name_const, line);
    }

    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, args->items[i], base + 1 + i);
    }
    emit_call(gen, base, nargs, 1, line);
    if (base != dst) emit_mov(gen, dst, base, line);
    reg_free_block(gen, base);
}

void gen_call(CodeGen* gen, Ast* ast, int dst) {
    Ast* callee = ast->u.call.callee;
    int nargs = ast->u.call.args.count;

    // --- 方法调用：obj.name(args) ---
    //   语义分析对 `s.len()` 这类会给出 AST_FIELD_ACCESS 或 AST_INDEX("len")，
    //   两种形态都走同一套：GET_METHOD 得到绑定方法后再 CALL。
    if (callee && callee->kind == AST_FIELD_ACCESS) {
        gen_method_call(gen, callee->u.field_access.obj, callee->u.field_access.field_name,
                        &ast->u.call.args, nargs, dst, ast->line);
        return;
    }
    if (callee && callee->kind == AST_INDEX && callee->u.index.index &&
        callee->u.index.index->kind == AST_STRING) {
        gen_method_call(gen, callee->u.index.obj, callee->u.index.index->u.string.value,
                        &ast->u.call.args, nargs, dst, ast->line);
        return;
    }

    // 调用约定：R[base] = callee，R[base+1 .. base+nargs] = 实参。
    // 必须整块连续分配，否则实参之间会被临时寄存器隔开（VM 按 A+i 取参）。
    if (callee && callee->kind == AST_VAR) {
        SymRef* ref = &callee->u.var.ref;

        // --- native 直接调用（CALL_NATIVE）：省掉"取函数值 + CALL" ---
        if (ref->kind == SYM_NATIVE) {
            int base = reg_alloc_block(gen, nargs + 1);
            for (int i = 0; i < nargs; i++) {
                gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
            }
            ObjString* nameStr = str_copy(ref->name, (int)strlen(ref->name));
            int name_const = make_constant(gen, val_obj((Object*)nameStr));
            emit_call_native(gen, base, name_const, nargs, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            return;
        }

        // --- 全局函数直接调用 ---
        if (ref->kind == SYM_GLOBAL_FUNC) {
            int base = reg_alloc_block(gen, nargs + 1);
            emit_getglobalfunc_to(gen, base, ref->index, ast->line);
            for (int i = 0; i < nargs; i++) {
                gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
            }
            emit_call(gen, base, nargs, 1, ast->line);
            if (base != dst) emit_mov(gen, dst, base, ast->line);
            reg_free_block(gen, base);
            return;
        }
    }

    // --- 通用调用 ---
    int base = reg_alloc_block(gen, nargs + 1);
    gen_expr_to(gen, callee, base);
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, ast->u.call.args.items[i], base + 1 + i);
    }
    emit_call(gen, base, nargs, 1, ast->line);
    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
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

// 取「别名变量持有的模块对象」到 dst（mod.member 的 mod 部分）。
//   语义分析把别名符号写在 ast->u.module_access.ref：
//     SYM_GLOBAL  （主文件里的 import 别名 → 全局变量）
//     SYM_MODULE  （模块内 import 别的模块 → 本模块的全局槽）
//     SYM_LOCAL / SYM_UPVALUE
static void emit_module_object(CodeGen* gen, Ast* ast, int dst) {
    SymRef* ref = &ast->u.module_access.ref;

    if (ref->name) {
        switch (ref->kind) {
            case SYM_LOCAL:
            case SYM_PARAM:
                if (ref->index != dst) emit_mov(gen, dst, ref->index, ast->line);
                return;
            case SYM_GLOBAL:
                emit_getglobal_to(gen, dst, ref->index, ast->line);
                return;
            case SYM_UPVALUE:
                emit_getupval_to(gen, dst, ref->index, ast->line);
                return;
            case SYM_MODULE:
                reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, ref->index, ast->line);
                return;
            default:
                break;
        }
    }

    // 兜底：按别名在全局作用域查
    const char* alias = ast->u.module_access.module_name;
    Symbol* sym = alias ? scope_resolve(gen->sem->root_scope, alias) : NULL;
    if (sym) {
        if (sym->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, dst, sym->index, ast->line);
            return;
        }
        if (sym->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, dst, sym->index, ast->line);
            return;
        }
        if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM) {
            if (sym->index != dst) emit_mov(gen, dst, sym->index, ast->line);
            return;
        }
    }
    emit_loadnil_to(gen, dst, ast->line);
}

// mod.member：取模块对象后按成员名查 exports。
//   ⚠ AST_MODULE_ACCESS 表示的是**成员访问**（不是"模块对象表达式"）——
//     只取模块对象会把整个模块对象当成结果（曾表现为 assert_eq(mod.value, 100)
//     实际得到 [object]）。模块成员（变量/函数/类型）都统一放在 exports 字典里。
void gen_module_access(CodeGen* gen, Ast* ast, int dst) {
    const char* member = ast->u.module_access.member_name;
    if (!member || !member[0]) {
        emit_module_object(gen, ast, dst);
        return;
    }

    int mreg = reg_alloc(gen);
    emit_module_object(gen, ast, mreg);

    int ireg = reg_alloc(gen);
    int cidx = make_constant(gen, val_obj((Object*)str_copy(member, (int)strlen(member))));
    emit_loadk_to(gen, ireg, cidx, ast->line);

    reg_encode_iABC(gen->chunk, OP_INDEX, dst, mreg, ireg, ast->line);

    reg_free(gen, ireg);
    reg_free(gen, mreg);
}

void gen_module_call(CodeGen* gen, Ast* ast, int dst) {
    // 两类模块调用：
    //   原生模块（io / jsons / strings ...）→ OP_MODULE_CALL（按名字查 native 方法表）
    //   .leno 源码模块（别名）→ 取模块对象 → exports[方法名] → 普通 CALL
    // 用「该名字能否被原生模块机制识别」区分；lib_ref 对 .leno 模块没有有效信息。
    extern int native_init_module(const char* name);

    int nargs = ast->u.module_call.args.count;
    const char* modname = ast->u.module_call.module_name ? ast->u.module_call.module_name : "";
    const char* methname = ast->u.module_call.method_name ? ast->u.module_call.method_name : "";

    // 先查 import 登记表（编译期 gen_import 记下的"别名 → 真实名 + 是否原生"）；
    // 没有登记（例如模块内 use 来的）再探测原生模块注册表。
    int alias_found = 0;
    const char* real_name = modname;
    int is_native_mod = codegen_module_lookup(gen, modname, &real_name, &alias_found);
    if (!alias_found) {
        is_native_mod = (native_init_module(modname) == 0);
        real_name = modname;
    }

    if (!is_native_mod) {
        // --- .leno 模块成员调用 ---
        int base = reg_alloc_block(gen, nargs + 1);

        SymRef* lib = &ast->u.module_call.lib_ref;
        if (lib->name && (lib->kind == SYM_LOCAL || lib->kind == SYM_PARAM)) {
            emit_mov(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_GLOBAL) {
            emit_getglobal_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_UPVALUE) {
            emit_getupval_to(gen, base, lib->index, ast->line);
        } else if (lib->name && lib->kind == SYM_MODULE) {
            reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, base, lib->index, ast->line);
        } else {
            // 兜底：按模块名（别名）在全局作用域查
            Symbol* sym = scope_resolve(gen->sem->root_scope, modname);
            if (sym && sym->kind == SYM_GLOBAL) {
                emit_getglobal_to(gen, base, sym->index, ast->line);
            } else if (sym && sym->kind == SYM_MODULE) {
                reg_encode_iABx(gen->chunk, OP_GET_MODULE_VAR, base, sym->index, ast->line);
            } else if (sym && (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM)) {
                emit_mov(gen, base, sym->index, ast->line);
            } else {
                emit_loadnil_to(gen, base, ast->line);
            }
        }

        // callee = 模块对象[方法名]
        int ireg = reg_alloc(gen);
        int cidx = make_constant(gen, val_obj((Object*)str_copy(methname, (int)strlen(methname))));
        emit_loadk_to(gen, ireg, cidx, ast->line);
        reg_encode_iABC(gen->chunk, OP_INDEX, base, base, ireg, ast->line);
        reg_free(gen, ireg);

        for (int i = 0; i < nargs; i++) {
            gen_expr_to(gen, ast->u.module_call.args.items[i], base + 1 + i);
        }
        emit_call(gen, base, nargs, 1, ast->line);
        if (base != dst) emit_mov(gen, dst, base, ast->line);
        reg_free_block(gen, base);
        return;
    }

    // --- 原生模块调用 ---
    int base = reg_alloc_block(gen, nargs + 1);

    // ★ 必须先求值实参，再发射 OP_MODULE_CALL —— 指令在执行期直接从
    //   R[base+1..] 取参，若指令先发射、实参后写入，读到的是上一轮的旧值。
    for (int i = 0; i < nargs; i++) {
        gen_expr_to(gen, ast->u.module_call.args.items[i], base + 1 + i);
    }

    // ★ 用**真实模块名**（别名可能被 as 改过，而 native 方法表按注册名查找）
    const char* mod = real_name;
    const char* meth = methname;
    int mlen = (int)strlen(mod);
    int flen = (int)strlen(meth);

    ObjString* combo = str_alloc(mlen + 1 + flen);
    if (combo) {
        memcpy(combo->chars, mod, (size_t)mlen);
        combo->chars[mlen] = '\0';
        memcpy(combo->chars + mlen + 1, meth, (size_t)flen);
        combo->chars[mlen + 1 + flen] = '\0';
        combo->len = mlen + 1 + flen;
        combo->hash = hash_string(combo->chars, combo->len);
    }
    int cidx = make_constant(gen, val_obj((Object*)combo));

    if (cidx == 0 || cidx > 255) {
        reg_encode_iABC(gen->chunk, OP_MODULE_CALL, base, 0, nargs, ast->line);
        reg_encode_iAx(gen->chunk, OP_EXTRAARG, cidx, ast->line);
    } else {
        reg_encode_iABC(gen->chunk, OP_MODULE_CALL, base, cidx, nargs, ast->line);
    }

    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
}

// ============================================================================
// struct 初始化
// ============================================================================

// TypeInfo → 泛型实参名（与栈式 value_to_generic_type_name 的口径一致）
static const char* typeinfo_to_name(TypeInfo* t) {
    if (!t) return "unknown";
    switch (t->kind) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_BOOL:   return "bool";
        case TYPE_ANY:    return "any";
        case TYPE_STRUCT: return t->struct_name ? t->struct_name : "struct";
        default:          return "unknown";
    }
}

void gen_struct_init(CodeGen* gen, Ast* ast, int dst) {
    int n = ast->u.struct_init.field_count;
    int base = reg_alloc_block(gen, n + 1);

    // 构造实参 → R[base+1 .. base+n]
    for (int i = 0; i < n; i++) {
        gen_expr_to(gen, ast->u.struct_init.field_values[i], base + 1 + i);
    }

    ObjString* sname = str_copy(ast->u.struct_init.struct_name,
                                (int)strlen(ast->u.struct_init.struct_name));
    int name_const = make_constant(gen, val_obj((Object*)sname));

    reg_encode_iABC(gen->chunk, OP_STRUCT_INIT, base, n, 0, ast->line);

    // --- 紧随数据 ---
    // 名字常量
    chunk_write(gen->chunk, (uint8_t)((name_const >> 8) & 0xFF), ast->line);
    chunk_write(gen->chunk, (uint8_t)(name_const & 0xFF), ast->line);

    // 泛型实参
    int gc = ast->u.struct_init.generic_type_count;
    chunk_write(gen->chunk, (uint8_t)(gc & 0xFF), ast->line);
    for (int i = 0; i < gc; i++) {
        const char* tn = typeinfo_to_name(ast->u.struct_init.generic_type_args
                                              ? ast->u.struct_init.generic_type_args[i] : NULL);
        int tc = make_constant(gen, val_obj((Object*)str_copy(tn, (int)strlen(tn))));
        chunk_write(gen->chunk, (uint8_t)((tc >> 8) & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(tc & 0xFF), ast->line);
    }

    // 每个实参对应字段名（0 = 按位置 i 赋值）
    for (int i = 0; i < n; i++) {
        const char* fname = ast->u.struct_init.field_names ? ast->u.struct_init.field_names[i] : NULL;
        int fc = 0;
        if (fname && fname[0]) {
            fc = make_constant(gen, val_obj((Object*)str_copy(fname, (int)strlen(fname))));
        }
        chunk_write(gen->chunk, (uint8_t)((fc >> 8) & 0xFF), ast->line);
        chunk_write(gen->chunk, (uint8_t)(fc & 0xFF), ast->line);
    }

    if (base != dst) emit_mov(gen, dst, base, ast->line);
    reg_free_block(gen, base);
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
